#include <Alryn/Terrain/WorldGen.h>

#include <Alryn/World/Prop.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/Village.h>

#include <algorithm>
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
    detail::for_each_house(v, seed, detail::village_gate_points(v, seed),
                           [&](const detail::HousePlot& h) {
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
                           });
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
