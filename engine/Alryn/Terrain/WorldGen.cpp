#include <Alryn/Terrain/WorldGen.h>

#include <Alryn/World/Prop.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// The levelled GROUND height: the natural land (base_height) with a flat building pad under every
// town house. It lives out of line because it needs the town layout (World/Village.h) - which is
// itself planned on base_height, so the pads never feed back into the layout they come from.

namespace alryn::worldgen {
namespace {

// ---- Settlement placement ------------------------------------------------------------------------

// The settlement cell (vcx,vcz) would grow on its own, before its neighbours have their say: whether
// one grows at all (rarer out in the wilderness bands), its tier + size, its site (jittered in the
// cell, on flat dry ground clear of rivers) and its street style. A would-be city on ground too broken
// for one settles for a town. Hardy folk seek out the high country: of the few spots in the cell a
// settlement could take, one up on an alpine plateau wins (a snowbound town).
std::optional<Village> raw_site(int vcx, int vcz, u32 seed) {
    const u32 salt = seed + 313u;
    // Settlement density varies across the world: a low-frequency field carves out WILDERNESS bands
    // where towns are far rarer, so some sit way out on their own (longer, lonelier hauls) instead of
    // every region being evenly dotted. `keep` is ~1 in settled land, ~0 in the wild.
    const f32 settle = noise::fbm2d(static_cast<f32>(vcx) * 0.16f, static_cast<f32>(vcz) * 0.16f, 2, 2.0f,
                                    0.5f, seed + 777u);
    const f32 keep = glm::smoothstep(-0.40f, 0.18f, settle);
    if (detail::hash01(detail::tree_hash(vcx, vcz, salt)) > 0.56f * (0.4f + 0.6f * keep)) {
        return std::nullopt; // only some cells grow a settlement (sparser out in the wild)
    }
    const f32 sz = detail::hash01(detail::tree_hash(vcx, vcz, salt + 8u));
    const f32 sv = detail::hash01(detail::tree_hash(vcx, vcz, salt + 9u));
    const TownTier wish = sz < 0.24f ? TownTier::Hamlet : sz < 0.56f ? TownTier::Village : sz < 0.85f ? TownTier::Town
                                                                                                     : TownTier::City;
    auto half_of = [&](TownTier t) {
        switch (t) {
            case TownTier::Hamlet: return 22.0f + 6.0f * sv;
            case TownTier::Village: return 30.0f + 7.0f * sv;
            case TownTier::Town: return 39.0f + 10.0f * sv;
            case TownTier::City: return 86.0f + 14.0f * sv;
        }
        return 39.0f;
    };

    // Can a settlement of this tier stand at (cx,cz)? Fills its ground + (a would-be city may come down
    // to a town) its tier.
    auto fits = [&](f32 cx, f32 cz, TownTier& tier, f32& gh) {
        tier = wish;
        gh = base_height(cx, cz, seed);
        if (gh < water_level + 2.0f || gh > 18.5f) {
            return false; // not on buildable ground (above water; valleys or a high alpine shelf)
        }
        // A city sprawls over gentle country (its houses each stand on their own levelled pad, so it can
        // climb a rise) - but not across a lake or up a crag: nearly all of its ground must be dry and
        // within a few metres of the centre. Failing that it's only a town.
        if (tier == TownTier::City) {
            int ok = 0, n = 0;
            for (const f32 ring : {0.45f, 0.8f}) {
                for (int i = 0; i < 16; ++i) {
                    const f32 a = TwoPi * (static_cast<f32>(i) + ring) / 16.0f;
                    const f32 r = ring * half_of(tier);
                    const f32 h2 = base_height(cx + std::cos(a) * r, cz + std::sin(a) * r, seed);
                    ++n;
                    if (h2 > water_level + 1.2f && std::abs(h2 - gh) < 6.5f) {
                        ++ok;
                    }
                }
            }
            if (static_cast<f32>(ok) < 0.88f * static_cast<f32>(n)) {
                tier = TownTier::Town;
            }
        }
        const f32 half = half_of(tier);
        if (tier != TownTier::City) {
            for (f32 ox : {-half, 0.0f, half}) {
                for (f32 oz : {-half, 0.0f, half}) {
                    if (std::abs(base_height(cx + ox, cz + oz, seed) - gh) > 3.8f) {
                        return false; // not flat enough for a settlement of this size
                    }
                }
            }
        }
        // Reject a site a river threads through: a town straddling the carved channel floats its market
        // stalls + drops its wagons in the water. The flat check above already rejects a river running
        // near the footprint's edge/corner samples (height() carves the channel); this covers the gap
        // across the core (market + wagon depot + inner houses). The river is the zero-contour of
        // river_field, so a sign flip between the centre and a core-ring sample means a river runs between
        // them; an in-channel sample (river_amount) catches one tangent to the ring.
        const f32 rc = river_field(cx, cz, seed);
        if (river_amount(cx, cz, seed) > 0.2f) {
            return false; // the centre itself is in a channel
        }
        const f32 rr = std::min(half * 0.55f, 30.0f); // the market, wagon spots + the inner houses
        for (int i = 0; i < 12; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / 12.0f;
            const f32 rf = river_field(cx + std::cos(a) * rr, cz + std::sin(a) * rr, seed);
            if (std::abs(rf) < 0.024f || (rf < 0.0f) != (rc < 0.0f)) {
                return false; // a river runs through the town's core
            }
        }
        return true;
    };

    // A city keeps nearer its cell's middle (it needs the room); the rest wander more. The first spot
    // that fits is taken - unless a later one up in the snow fits too.
    const f32 jit = wish == TownTier::City ? 0.16f : 0.3f;
    std::optional<Village> best;
    for (u32 k = 0; k < 4; ++k) {
        const f32 jx = (detail::hash01(detail::tree_hash(vcx, vcz, salt + 1u + 20u * k)) - 0.5f) * village_cell * jit;
        const f32 jz = (detail::hash01(detail::tree_hash(vcx, vcz, salt + 2u + 20u * k)) - 0.5f) * village_cell * jit;
        const f32 cx = (static_cast<f32>(vcx) + 0.5f) * village_cell + jx;
        const f32 cz = (static_cast<f32>(vcz) + 0.5f) * village_cell + jz;
        TownTier tier;
        f32 gh;
        if (!fits(cx, cz, tier, gh)) {
            continue;
        }
        Village v;
        v.center = Vec2{cx, cz};
        v.ground = gh;
        v.half = half_of(tier);
        v.vseed = detail::tree_hash(vcx, vcz, salt + 7u);
        v.tier = tier;
        const f32 style = detail::hash01(detail::tree_hash(vcx, vcz, salt + 10u));
        switch (tier) {
            case TownTier::Hamlet: v.layout = TownLayout::Lane; break;
            case TownTier::Village: v.layout = style < 0.6f ? TownLayout::Lane : TownLayout::Radial; break;
            case TownTier::Town: v.layout = style < 0.55f ? TownLayout::Radial : TownLayout::Grid; break;
            case TownTier::City: v.layout = TownLayout::Rings; break;
        }
        v.snowy = gh >= snow_town_ground;
        if (!best || (v.snowy && !best->snowy)) {
            best = v;
        }
        if (best->snowy) {
            break;
        }
    }
    return best;
}

// A (cx, cz, seed) keyed cache of optional settlements, shared across threads, with a small
// thread-local direct-mapped front (terrain meshing asks the same few cells over and over).
struct SiteKey {
    int x = INT_MIN;
    int z = INT_MIN;
    u32 seed = 0;
    bool operator==(const SiteKey&) const = default;
};
struct SiteKeyHash {
    usize operator()(const SiteKey& k) const {
        return (static_cast<usize>(static_cast<u32>(k.x)) * 73856093u) ^
               (static_cast<usize>(static_cast<u32>(k.z)) * 19349663u) ^ (static_cast<usize>(k.seed) * 83492791u);
    }
};
template <typename Compute>
std::optional<Village> cached_site(std::mutex& mtx, std::unordered_map<SiteKey, std::optional<Village>, SiteKeyHash>& map,
                                   const SiteKey& key, Compute&& compute) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = map.find(key);
        if (it != map.end()) {
            return it->second;
        }
    }
    std::optional<Village> v = compute();
    std::lock_guard<std::mutex> lock(mtx);
    return map.emplace(key, v).first->second;
}

std::optional<Village> raw_site_cached(int vcx, int vcz, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<SiteKey, std::optional<Village>, SiteKeyHash> map;
    return cached_site(mtx, map, SiteKey{vcx, vcz, seed}, [&] { return raw_site(vcx, vcz, seed); });
}

// A great city needs its neighbouring cells clear: it SURVIVES unless a neighbouring would-be city has
// a stronger claim (priority by hash), and every other settlement in the 8 cells round a surviving city
// gives way to it.
bool city_survives(int vcx, int vcz, u32 seed) {
    const auto self = raw_site_cached(vcx, vcz, seed);
    if (!self || self->tier != TownTier::City) {
        return false;
    }
    auto prio = [&](int x, int z) { return detail::hash01(detail::tree_hash(x, z, seed + 324u)); };
    const f32 mine = prio(vcx, vcz);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const auto nb = raw_site_cached(vcx + dx, vcz + dz, seed);
            if (nb && nb->tier == TownTier::City && prio(vcx + dx, vcz + dz) > mine) {
                return false;
            }
        }
    }
    return true;
}

std::optional<Village> resolve_site(int vcx, int vcz, u32 seed) {
    const auto self = raw_site_cached(vcx, vcz, seed);
    if (!self) {
        return std::nullopt;
    }
    if (self->tier == TownTier::City) {
        return city_survives(vcx, vcz, seed) ? self : std::nullopt;
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if ((dx != 0 || dz != 0) && city_survives(vcx + dx, vcz + dz, seed)) {
                return std::nullopt; // the great city next door has claimed this land
            }
        }
    }
    return self;
}

} // namespace

std::optional<Village> village_at(int vcx, int vcz, u32 seed) {
    struct Entry {
        SiteKey key;
        std::optional<Village> v;
    };
    thread_local std::array<Entry, 256> t_front;
    const SiteKey key{vcx, vcz, seed};
    Entry& e = t_front[(static_cast<u32>(vcx) * 73856093u ^ static_cast<u32>(vcz) * 19349663u ^ seed) & 255u];
    if (e.key == key) {
        return e.v;
    }
    static std::mutex mtx;
    static std::unordered_map<SiteKey, std::optional<Village>, SiteKeyHash> map;
    e.v = cached_site(mtx, map, key, [&] { return resolve_site(vcx, vcz, seed); });
    e.key = key;
    return e.v;
}

namespace {

constexpr f32 kYard = 1.2f;  // flat margin around a house's walls: plinth, doorstep, yard props
constexpr f32 kSkirt = 2.4f; // ...then ease back to the natural ground over this distance
constexpr f32 kCell = 32.0f; // lookup grid: each cell lists the pads that reach into it

// One levelled building pad, in its house's local frame. The house prop is drawn with
// translate(c) * rotateY(yaw), so a world point maps back to local xz by rotateY(-yaw).
struct Pad {
    Vec2 c{0.0f};
    f32 cs = 1.0f, sn = 0.0f; // cos / sin of the house yaw
    Vec2 lo{0.0f}, hi{0.0f};  // the flat area (footprint + yard), local xz
    Vec2 walls{0.0f};         // the footprint half-extents (indoors)
    f32 reach = 0.0f;         // no influence beyond this distance from c (a cheap reject)
    f32 base = 0.0f;          // the pad height: the natural ground at the house centre

    Vec2 local(f32 x, f32 z) const {
        const f32 dx = x - c.x;
        const f32 dz = z - c.y;
        return Vec2{dx * cs - dz * sn, dx * sn + dz * cs};
    }
};
using Pads = std::vector<Pad>;
using PadsPtr = std::shared_ptr<const Pads>;

// Set while a cell's pads are being built. The layout must only ever read base_height; this keeps
// a stray height() call inside it from recursing into the half-built cache.
thread_local bool t_building = false;

// Every house pad of town `v`: the same plots village_props builds its houses on, each house
// standing at the natural height of its centre.
Pads town_pads(const Village& v, u32 seed) {
    Pads out;
    for (const detail::HousePlot& h : detail::cached_town_plan(v, seed).houses) {
        const Vec2 e = PropLibrary::house_half_extents(h.variant);
        Pad p;
        p.c = h.pos;
        p.cs = std::cos(h.yaw);
        p.sn = std::sin(h.yaw);
        p.walls = e;
        p.lo = -e - Vec2{kYard};
        p.hi = e + Vec2{kYard};
        if (h.variant == kHousePub) {
            p.hi += Vec2{2.0f, 3.6f}; // the beer garden, off the front-right
        }
        p.reach = glm::length(glm::max(-p.lo, p.hi)) + kSkirt;
        p.base = base_height(h.pos.x, h.pos.y, seed);
        out.push_back(p);
    }
    return out;
}

// Cached per town (keyed like village_gate_points): the layout is far too slow to redo per sample.
PadsPtr cached_town_pads(const Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, PadsPtr> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }
    PadsPtr pads = std::make_shared<const Pads>(town_pads(v, seed));
    std::lock_guard<std::mutex> lock(mtx);
    return cache.emplace(key, std::move(pads)).first->second;
}

// The pads reaching into lookup cell (qx,qz), gathered from every town near enough to own them.
PadsPtr build_cell(int qx, int qz, u32 seed) {
    const Vec2 lo{static_cast<f32>(qx) * kCell, static_cast<f32>(qz) * kCell};
    const Vec2 hi = lo + Vec2{kCell};
    // A town's houses stand inside its organic wall (at most 1.35x its half-width out), so only
    // towns centred within that + a pad's reach of the cell can touch it.
    constexpr f32 town_reach = village_half_max * 1.35f + 12.0f;
    const int vx0 = static_cast<int>(std::floor((lo.x - town_reach) / village_cell));
    const int vx1 = static_cast<int>(std::floor((hi.x + town_reach) / village_cell));
    const int vz0 = static_cast<int>(std::floor((lo.y - town_reach) / village_cell));
    const int vz1 = static_cast<int>(std::floor((hi.y + town_reach) / village_cell));
    auto pads = std::make_shared<Pads>();
    for (int vz = vz0; vz <= vz1; ++vz) {
        for (int vx = vx0; vx <= vx1; ++vx) {
            const auto v = village_at(vx, vz, seed);
            if (!v) {
                continue;
            }
            for (const Pad& p : *cached_town_pads(*v, seed)) {
                if (glm::length(glm::clamp(p.c, lo, hi) - p.c) < p.reach) {
                    pads->push_back(p);
                }
            }
        }
    }
    return pads;
}

struct CellKey {
    int x = INT_MIN;
    int z = INT_MIN;
    u32 seed = 0;
    bool operator==(const CellKey&) const = default;
};
struct CellKeyHash {
    usize operator()(const CellKey& k) const {
        return (static_cast<usize>(static_cast<u32>(k.x)) * 73856093u) ^
               (static_cast<usize>(static_cast<u32>(k.z)) * 19349663u) ^
               (static_cast<usize>(k.seed) * 83492791u);
    }
};

// The pads that can affect (x,z). Queries come in long runs from the same cell (a chunk's terrain
// columns, a raycast, a vegetation sweep), so each thread remembers its last cell and only takes
// the shared cache's lock when it moves to another one.
const Pads& cell_pads(f32 x, f32 z, u32 seed) {
    thread_local CellKey t_key;
    thread_local PadsPtr t_pads;
    const CellKey key{static_cast<int>(std::floor(x / kCell)), static_cast<int>(std::floor(z / kCell)),
                      seed};
    if (t_pads && key == t_key) {
        return *t_pads;
    }
    static std::mutex mtx;
    static std::unordered_map<CellKey, PadsPtr, CellKeyHash> cache;
    PadsPtr pads;
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            pads = it->second;
        }
    }
    if (!pads) {
        t_building = true;
        PadsPtr built = build_cell(key.x, key.z, seed);
        t_building = false;
        std::lock_guard<std::mutex> lock(mtx);
        pads = cache.emplace(key, std::move(built)).first->second;
    }
    t_key = key;
    t_pads = std::move(pads);
    return *t_pads;
}

} // namespace

f32 height(f32 x, f32 z, u32 seed) {
    const f32 h = base_height(x, z, seed);
    if (t_building) {
        return h;
    }
    const Pads& pads = cell_pads(x, z, seed);
    // Blend toward the pads this point is on or near: full weight on a pad's flat area, easing to
    // none across its skirt. Where neighbouring pads overlap (two houses a short alley apart), their
    // heights average - weighted toward the house whose walls you're nearer the inside of, so every
    // floor stays flat at its own height and the alley between them ramps from one to the other.
    f32 w_max = 0.0f;
    f32 w_sum = 0.0f;
    f32 b_sum = 0.0f;
    for (const Pad& p : pads) {
        const Vec2 d{x - p.c.x, z - p.c.y};
        if (glm::dot(d, d) >= p.reach * p.reach) {
            continue;
        }
        const Vec2 l = p.local(x, z);
        const Vec2 out = glm::max(glm::max(p.lo - l, l - p.hi), Vec2{0.0f}); // past the flat area
        const f32 w = 1.0f - glm::smoothstep(0.0f, kSkirt, glm::length(out));
        if (w <= 0.0f) {
            continue;
        }
        const Vec2 q = glm::abs(l) - p.walls; // signed distance outside the walls (< 0 indoors)
        const f32 s = glm::length(glm::max(q, Vec2{0.0f})) + std::min(std::max(q.x, q.y), 0.0f);
        const f32 f = (w * w) * (w * w) * std::exp(-6.0f * glm::clamp(s, -1.0f, 4.0f));
        w_max = std::max(w_max, w);
        w_sum += f;
        b_sum += f * p.base;
    }
    if (w_max <= 0.0f) {
        return h;
    }
    // A carved river channel keeps its bed: a skirt never fills the water in.
    const f32 keep = 1.0f - river_amount(x, z, seed);
    return glm::mix(h, b_sum / w_sum, w_max * keep);
}

bool under_building(f32 x, f32 z, u32 seed, f32 margin) {
    for (const Pad& p : cell_pads(x, z, seed)) {
        const Vec2 l = p.local(x, z);
        if (std::abs(l.x) < p.walls.x + margin && std::abs(l.y) < p.walls.y + margin) {
            return true;
        }
    }
    return false;
}

} // namespace alryn::worldgen
