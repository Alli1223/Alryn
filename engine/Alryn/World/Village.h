#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Prop.h>
#include <Alryn/World/PropLibrary.h>

#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace alryn {

namespace detail {

inline constexpr f32 kGateHalf = 2.6f;        // minimum gate opening half-width (one road)
inline constexpr f32 kGateMaxHalf = 9.0f;     // cap on a widened opening (several roads at once)
inline constexpr f32 kGatehouseMargin = 1.4f; // gatehouse offset just outside the opening edge
inline constexpr f32 kGateClusterDist = 12.0f; // roads crossing within this share one wide gate
inline constexpr f32 kMarketHalf = 9.0f;     // central market square footprint (stalls + slack)

// A gate opening: its angle + world position on the wall, and its half-width along the wall.
// The half-width WIDENS when several roads leave town near the same spot, so the opening (and
// its flanking gatehouses) spans them all - one narrow gate could leave divergent roads butting
// into a wall.
struct VillageGate {
    f32 ang = 0.0f;
    Vec2 pos{0.0f};
    f32 half = kGateHalf;
};

// The boundary point of town v at world-angle `ang` (on its organic outline).
inline Vec2 town_boundary(const worldgen::Village& v, f32 ang, u32 seed) {
    const f32 r = worldgen::town_radius(v, ang, seed);
    return Vec2{v.center.x + std::cos(ang) * r, v.center.y + std::sin(ang) * r};
}

// Smallest signed difference a-b wrapped to [-Pi, Pi].
inline f32 ang_diff(f32 a, f32 b) {
    f32 d = a - b;
    while (d > Pi) d -= TwoPi;
    while (d < -Pi) d += TwoPi;
    return d;
}

// Computes the town's gates: one where each incident road actually CROSSES the (organic) wall.
// A road meanders on its way out of town, so where it crosses the wall can be well off its
// initial heading - cutting the gate at the heading (the old bug) left the road butting into a
// wall. Here we take the road to each neighbouring town, walk it from this centre outward, and
// open the wall at the first boundary crossing, so the road always runs through the gap. Only
// incident roads count, so the gate total stays bounded. Isolated town -> one default gate (+Z).
inline std::vector<VillageGate> compute_gates(const worldgen::Village& v, u32 seed) {
    // Signed "outside-ness" of a point: <0 inside the organic wall, >0 outside.
    auto signed_out = [&](const Vec2& p) {
        const Vec2 d = p - v.center;
        const f32 r = glm::length(d);
        if (r < 1e-3f) {
            return -worldgen::town_radius(v, 0.0f, seed);
        }
        return r - worldgen::town_radius(v, std::atan2(d.y, d.x), seed);
    };

    // 1. Collect every incident road's first wall crossing point.
    std::vector<Vec2> crossings;
    const int vcx = static_cast<int>(std::floor(v.center.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(v.center.y / worldgen::village_cell));
    for (int dz = -roads::road_max_cells; dz <= roads::road_max_cells; ++dz) {
        for (int dx = -roads::road_max_cells; dx <= roads::road_max_cells; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            const auto nb = worldgen::village_at(vcx + dx, vcz + dz, seed);
            if (!nb) {
                continue;
            }
            const std::vector<Vec2> poly = roads::route_polyline(v.center, nb->center, seed);
            for (usize i = 1; i < poly.size(); ++i) {
                const f32 fa = signed_out(poly[i - 1]);
                const f32 fb = signed_out(poly[i]);
                if ((fa < 0.0f) == (fb < 0.0f)) {
                    continue; // both ends the same side of the wall - no crossing here
                }
                const f32 t = glm::clamp(fa / (fa - fb), 0.0f, 1.0f);
                const Vec2 cross = glm::mix(poly[i - 1], poly[i], t);
                if (glm::length(cross - v.center) > 1e-3f) {
                    crossings.push_back(cross);
                }
                break; // the first crossing leaving town is the gate (ignore any re-entries)
            }
        }
    }

    // 2. Cluster crossings that leave near the same spot (within kGateClusterDist). Each cluster
    //    becomes one gate, its opening WIDENED to span every road in it - so two or three roads
    //    that exit together all run through a single wide gate instead of butting into a wall.
    std::vector<std::vector<Vec2>> clusters;
    for (const Vec2& c : crossings) {
        int found = -1;
        for (usize k = 0; k < clusters.size() && found < 0; ++k) {
            for (const Vec2& p : clusters[k]) {
                if (glm::length(c - p) < kGateClusterDist) {
                    found = static_cast<int>(k);
                    break;
                }
            }
        }
        if (found < 0) {
            clusters.push_back({c});
        } else {
            clusters[static_cast<usize>(found)].push_back(c);
        }
    }

    std::vector<VillageGate> gates;
    for (const std::vector<Vec2>& pts : clusters) {
        Vec2 mean{0.0f};
        for (const Vec2& p : pts) {
            mean += p;
        }
        mean /= static_cast<f32>(pts.size());
        const Vec2 dm = mean - v.center;
        const f32 ang = glm::length(dm) > 1e-3f ? std::atan2(dm.y, dm.x) : HalfPi;
        const Vec2 gpos = town_boundary(v, ang, seed);
        // Half-width = how far the cluster's roads spread along the wall + each road's own
        // half-width + a little margin (clamped to a sane range), so the gap covers them all.
        Vec2 radial = gpos - v.center;
        radial = glm::length(radial) > 1e-3f ? glm::normalize(radial) : Vec2{0.0f, 1.0f};
        const Vec2 tangent{-radial.y, radial.x};
        f32 spread = 0.0f;
        for (const Vec2& p : pts) {
            spread = std::max(spread, std::abs(glm::dot(p - gpos, tangent)));
        }
        const f32 half = glm::clamp(spread + roads::road_half_width + 0.4f, kGateHalf, kGateMaxHalf);
        gates.push_back(VillageGate{ang, gpos, half});
    }
    if (gates.empty()) {
        gates.push_back(VillageGate{HalfPi, town_boundary(v, HalfPi, seed), kGateHalf});
    }
    return gates;
}

// Cached per town (keyed by its layout seed + world seed), because this runs per terrain
// vertex while colouring the town paths - computing the road crossings every time would be
// far too slow. Thread-safe (the terrain mesher runs on a worker thread).
inline std::vector<VillageGate> village_gate_points(const worldgen::Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, std::vector<VillageGate>> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }
    std::vector<VillageGate> gates = compute_gates(v, seed);
    std::lock_guard<std::mutex> lock(mtx);
    return cache.emplace(key, std::move(gates)).first->second;
}

// One placed house: its ground-plane position, the yaw facing the town centre, and its
// PropLibrary variant.
struct HousePlot {
    Vec2 pos;
    f32 yaw;
    u8 variant;
};

// A two-storey house pair joined by a covered bridge (some towns have one).
struct BridgeInfo {
    bool present = false;
    Vec2 center{0.0f};
    f32 yaw = 0.0f;
};

inline f32 point_seg_dist(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    const f32 t = len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

// Half the footprint of a house variant (its largest extent + roof overhang), used as a
// collision radius when laying the town out.
inline f32 house_reach(u8 variant) {
    const Vec2 e = PropLibrary::house_half_extents(variant);
    return std::max(e.x, e.y) + 0.6f;
}

// A river running across some towns: a straight stone-lined channel offset to one side of the
// market so it splits the town and is crossed by stone bridges. Only the bigger towns are
// eligible (a small one has no room), and ~half of those get one - the rest sit on dry land.
struct RiverInfo {
    bool present = false;
    Vec2 dir{1.0f, 0.0f};    // unit direction the river flows
    Vec2 normal{0.0f, 1.0f}; // unit perpendicular
    f32 offset = 0.0f;       // signed distance of the centreline from the town centre, along normal
    f32 half_width = 3.7f;   // channel half-width (water + banks), matches build_river
};

inline RiverInfo town_river(const worldgen::Village& v, u32 seed) {
    RiverInfo r;
    const int vid = static_cast<int>(v.vseed);
    if (v.half < 36.0f || detail::hash01(detail::tree_hash(vid, 0, 6000u + seed)) > 0.5f) {
        return r; // small towns + half of the rest stay on dry land
    }
    r.present = true;
    const f32 ang = detail::hash01(detail::tree_hash(vid, 1, 6001u + seed)) * TwoPi;
    r.dir = Vec2{std::cos(ang), std::sin(ang)};
    r.normal = Vec2{-r.dir.y, r.dir.x};
    const f32 side = detail::hash01(detail::tree_hash(vid, 2, 6002u + seed)) < 0.5f ? -1.0f : 1.0f;
    r.offset = side * (kMarketHalf + 4.5f); // runs just past the market plaza on one side
    return r;
}

// Signed perpendicular distance of world point p from the river centreline; |.| < half_width
// means p is in the water.
inline f32 river_dist(const RiverInfo& r, const Vec2& center, const Vec2& p) {
    return glm::dot(p - center, r.normal) - r.offset;
}

// Deterministically lays out a town's houses: rings of cottages facing the centre, plus
// (in some towns) a two-storey bridge pair. A candidate is rejected if it sits on uneven
// ground, would clip the **central market**, cross the **perimeter wall**, or **overlap
// another house** - so houses never intersect anything. Calls `fn(HousePlot)` for each
// accepted house; fills `*bridge` if a bridge pair was placed. Allocation-free, so the
// per-vertex town-path colouring can call it cheaply. Shared by village_props +
// town_path_amount, so streets always lead to the actual houses.
template <typename Fn>
inline void for_each_house(const worldgen::Village& v, u32 seed,
                           const std::vector<VillageGate>& gates, Fn&& fn,
                           BridgeInfo* bridge = nullptr) {
    const f32 cx = v.center.x;
    const f32 cz = v.center.y;
    const f32 half = v.half;
    const int vid = static_cast<int>(v.vseed);
    constexpr f32 market_reach = kMarketHalf; // the central market square footprint + slack
    constexpr f32 wall_margin = 1.2f;  // gap kept between a house and the palisade
    constexpr f32 avenue_half = 2.8f;  // keep houses off the gate->market avenues

    const RiverInfo river = town_river(v, seed);

    std::array<Vec2, 160> placed;
    std::array<f32, 160> reach;
    int np = 0;
    auto try_place = [&](f32 x, f32 z, f32 yaw, u8 variant) -> bool {
        const f32 r = house_reach(variant);
        const f32 gh = worldgen::height(x, z, seed);
        if (std::abs(gh - v.ground) > 2.4f || gh < worldgen::water_level + 1.0f) {
            return false; // too uneven to build on, or it would sit in the water
        }
        if (river.present &&
            std::abs(river_dist(river, v.center, Vec2{x, z})) < river.half_width + r + 1.0f) {
            return false; // keep houses off the river channel + its banks
        }
        const Vec2 d{x - cx, z - cz};
        const f32 dist = glm::length(d);
        if (dist < r + market_reach) {
            return false; // would clip the central marketplace
        }
        const f32 ang = std::atan2(d.y, d.x);
        if (dist + r > worldgen::town_radius(v, ang, seed) - wall_margin) {
            return false; // would cross / poke through the (organic) perimeter wall
        }
        for (const VillageGate& g : gates) {
            if (point_seg_dist(Vec2{x, z}, v.center, g.pos) < r + avenue_half) {
                return false; // keep the gate->market avenue clear for the cart
            }
        }
        for (int k = 0; k < np; ++k) {
            if (glm::length(placed[k] - Vec2{x, z}) < r + reach[k]) {
                return false; // would overlap an already-placed house
            }
        }
        if (np < static_cast<int>(placed.size())) {
            placed[np] = Vec2{x, z};
            reach[np] = r;
            ++np;
        }
        fn(HousePlot{Vec2{x, z}, yaw, variant});
        return true;
    };

    // Houses fill the buildable annulus (outside the market, inside the wall) as a dense,
    // organic sprawl. We walk a sunflower (phyllotaxis) spiral of candidate sites from the
    // plaza outward - which fills area evenly without a visible grid - and keep each that
    // doesn't clip the market, cross the wall, block a gate->market avenue or overlap another
    // house. So a big town packs in ~40-50 homes and a small one a cosy dozen, never on a
    // neat ring. The candidate budget scales with the town's area.
    const f32 inner = kMarketHalf + 6.0f;        // first homes just off the market square
    const f32 outer = half * 0.94f;              // out to just inside the wall
    const int budget = glm::clamp(static_cast<int>(std::round(half * 1.1f)), 14, 64);
    constexpr f32 golden = 2.39996323f;          // golden angle (radians)
    for (int i = 0; i < budget; ++i) {
        const f32 frac = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(budget);
        const f32 jr = (detail::hash01(detail::tree_hash(vid, i, 510u)) - 0.5f) * 5.5f;
        const f32 radius = glm::clamp(glm::mix(inner, outer, std::sqrt(frac)) + jr, inner, outer);
        const f32 a = static_cast<f32>(i) * golden + detail::hash01(detail::tree_hash(vid, i, 511u)) * 0.5f;
        const f32 x = cx + std::cos(a) * radius;
        const f32 z = cz + std::sin(a) * radius;
        const f32 yaw = std::atan2(cx - x, cz - z); // front (+Z) faces the centre
        // Most plots are ordinary homes; sprinkle in a few landmark buildings - one pub + one
        // blacksmith near the heart of town, and the odd tall townhouse - for variety.
        const u32 hh = detail::tree_hash(vid, i, 512u);
        u8 var;
        if (i == budget / 2) {
            var = static_cast<u8>(kHousePub);
        } else if (i == budget / 3) {
            var = static_cast<u8>(kHouseBlacksmith);
        } else if (hh % 6u == 0u) {
            var = static_cast<u8>(kHouseTownhouse);
        } else {
            var = static_cast<u8>(hh % kHouseVariants);
        }
        try_place(x, z, yaw, var);
    }

    // Some towns have a pair of two-storey houses joined by a raised covered bridge.
    if (detail::hash01(detail::tree_hash(vid, 0, 900u)) < 0.55f) {
        const f32 a = detail::hash01(detail::tree_hash(vid, 1, 901u)) * TwoPi;
        const f32 r = half * 0.6f;
        const Vec2 c{cx + std::cos(a) * r, cz + std::sin(a) * r};
        const Vec2 tan{-std::sin(a), std::cos(a)}; // tangent to the ring
        const f32 yaw_house = std::atan2(cx - c.x, cz - c.y);
        const bool a_ok = try_place(c.x + tan.x * 4.6f, c.y + tan.y * 4.6f, yaw_house, 3);
        const bool b_ok = try_place(c.x - tan.x * 4.6f, c.y - tan.y * 4.6f, yaw_house, 3);
        if (a_ok && b_ok && bridge != nullptr) {
            bridge->present = true;
            bridge->center = c;
            bridge->yaw = std::atan2(-tan.y, tan.x);
        }
    }
}
} // namespace detail

// A few reserved wagon-depot spots near the main gate, clear of houses + market, where the
// transport offers park. Server-side `generate_offers` uses these so a wagon never spawns
// inside a building (gathers the house layout to avoid it).
inline std::vector<Vec3> village_wagon_spots(const worldgen::Village& v, u32 seed,
                                             int count = 4) {
    const auto gates = detail::village_gate_points(v, seed);
    std::vector<std::pair<Vec2, f32>> occ;
    detail::for_each_house(v, seed, gates, [&](const detail::HousePlot& h) {
        occ.emplace_back(h.pos, detail::house_reach(h.variant));
    });
    occ.emplace_back(v.center, detail::kMarketHalf); // the market square
    const Vec2 dir = gates.empty() ? Vec2{0.0f, 1.0f} : glm::normalize(gates[0].pos - v.center);
    const Vec2 perp{-dir.y, dir.x};
    constexpr f32 kSpotSpacing = 3.4f; // distance between adjacent depot spots
    std::vector<Vec3> spots;
    // March outward from the plaza toward the main gate in rows, sweeping laterally in each,
    // and accept positions clear of buildings/market *and* of spots already placed - so every
    // offer gets its own distinct spot and wagons never stack.
    for (int row = 0; row < 6 && static_cast<int>(spots.size()) < count; ++row) {
        const Vec2 base = v.center + dir * (v.half * 0.34f + static_cast<f32>(row) * kSpotSpacing);
        for (int i = 0; i < 6 && static_cast<int>(spots.size()) < count; ++i) {
            const Vec2 c = base + perp * ((static_cast<f32>(i) - 2.5f) * kSpotSpacing);
            bool ok = true;
            for (const auto& o : occ) {
                if (glm::length(o.first - c) < 2.4f + o.second) {
                    ok = false;
                    break;
                }
            }
            for (const Vec3& s : spots) {
                if (glm::length(Vec2{s.x, s.z} - c) < kSpotSpacing - 0.5f) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                spots.push_back(Vec3{c.x, worldgen::height(c.x, c.y, seed), c.y});
            }
        }
    }
    if (spots.empty()) {
        const Vec2 base = v.center + dir * (v.half * 0.34f);
        spots.push_back(Vec3{base.x, worldgen::height(base.x, base.y, seed), base.y});
    }
    return spots;
}

// Lays out a medieval town deterministically: a central market, cottages in loose rings, a
// stone palisade following the town's organic outline (with gates where roads cross it),
// raised cobblestone streets, a fountain, planters, bushes and street lanterns. Every
// object is placed against a shared occupancy list so nothing intersects. Returned as
// world-space PropInstances (the scatter filters them per chunk).
inline std::vector<PropInstance> village_props(const worldgen::Village& v, u32 seed) {
    std::vector<PropInstance> out;
    const f32 cx = v.center.x;
    const f32 cz = v.center.y;
    const f32 half = v.half;
    const int vid = static_cast<int>(v.vseed);
    const auto gates = detail::village_gate_points(v, seed);

    auto push = [&](PropCategory cat, u8 var, f32 x, f32 z, f32 yaw) {
        const f32 gh = worldgen::height(x, z, seed);
        if (gh < worldgen::water_level + 0.5f) {
            return; // never place a town prop down in the water
        }
        PropInstance p;
        p.category = cat;
        p.variant = var;
        p.position = Vec3{x, gh, z};
        p.yaw = yaw;
        p.scale = 1.0f;
        out.push_back(p);
    };

    // Some towns have a river running through them (others are on dry land). Streets, decor and
    // greenery keep out of its channel; stone bridges carry the avenues across it.
    const detail::RiverInfo river = detail::town_river(v, seed);
    auto in_river = [&](f32 x, f32 z) {
        return river.present &&
               std::abs(detail::river_dist(river, v.center, Vec2{x, z})) < river.half_width + 0.4f;
    };

    // Shared occupancy so decor never overlaps a building/market/fountain/wagon spot.
    std::vector<std::pair<Vec2, f32>> occ;
    auto occupied = [&](Vec2 p, f32 r) {
        for (const auto& o : occ) {
            if (glm::length(o.first - p) < r + o.second) {
                return true;
            }
        }
        return false;
    };

    // Market centre.
    push(PropCategory::Market, 0, cx, cz, 0.0f);
    occ.emplace_back(v.center, detail::kMarketHalf);

    // Houses (+ optional bridge pair), collision-rejected against walls/market/avenues.
    std::vector<detail::HousePlot> plots;
    detail::BridgeInfo bridge;
    detail::for_each_house(
        v, seed, gates, [&](const detail::HousePlot& h) { plots.push_back(h); }, &bridge);
    for (const detail::HousePlot& h : plots) {
        push(PropCategory::House, h.variant, h.pos.x, h.pos.y, h.yaw);
        occ.emplace_back(h.pos, detail::house_reach(h.variant));
    }
    if (bridge.present) {
        push(PropCategory::Bridge, 0, bridge.center.x, bridge.center.y, bridge.yaw);
    }

    // The river: stone-lined channel tiles laid along its course (within the town outline),
    // and arched stone bridges where the gate->market avenues cross it.
    if (river.present) {
        const f32 river_yaw = std::atan2(-river.dir.y, river.dir.x); // prop local +X -> river.dir
        constexpr f32 tile = 4.0f;                                   // build_river tile length
        for (f32 t = -half * 1.45f; t <= half * 1.45f; t += tile) {
            const Vec2 c = v.center + river.normal * river.offset + river.dir * t;
            const Vec2 d = c - v.center;
            if (glm::length(d) > worldgen::town_radius(v, std::atan2(d.y, d.x), seed) - 0.5f) {
                continue; // the channel ends at the wall
            }
            push(PropCategory::River, 0, c.x, c.y, river_yaw);
        }
        bool any_bridge = false;
        for (const detail::VillageGate& g : gates) {
            const Vec2 av = g.pos - v.center; // avenue: centre -> gate
            const f32 denom = glm::dot(av, river.normal);
            if (std::abs(denom) < 1e-3f) {
                continue; // avenue runs parallel to the river
            }
            const f32 s = river.offset / denom; // where the avenue meets the centreline
            if (s < 0.05f || s > 1.05f) {
                continue;
            }
            const Vec2 bp = v.center + av * s;
            const Vec2 bdir = glm::normalize(av);
            push(PropCategory::Bridge, 1, bp.x, bp.y, std::atan2(-bdir.y, bdir.x));
            any_bridge = true;
        }
        if (!any_bridge) {
            // No avenue crosses it - bridge the channel from the plaza so the banks still connect.
            const Vec2 bp = v.center + river.normal * river.offset;
            const Vec2 bdir = river.normal * (river.offset < 0.0f ? -1.0f : 1.0f);
            push(PropCategory::Bridge, 1, bp.x, bp.y, std::atan2(-bdir.y, bdir.x));
        }
    }

    // Reserve the wagon depot spots so nothing decorative lands on them.
    for (const Vec3& s : village_wagon_spots(v, seed)) {
        occ.emplace_back(Vec2{s.x, s.z}, 2.0f);
    }

    // A fountain set as a little plaza garden, just off the market on the cross-axis, ringed
    // with planters so it reads as a deliberate feature rather than a lone basin.
    const Vec2 gate_dir = gates.empty() ? Vec2{0.0f, 1.0f} : glm::normalize(gates[0].pos - v.center);
    const Vec2 gate_perp{-gate_dir.y, gate_dir.x};
    for (f32 off : {12.0f, -12.0f, 14.0f}) {
        const Vec2 fp = v.center + gate_perp * off;
        if (!occupied(fp, 2.6f)) {
            push(PropCategory::Fountain, 0, fp.x, fp.y, 0.0f);
            occ.emplace_back(fp, 2.6f);
            for (int i = 0; i < 4; ++i) { // a ring of planters around the basin
                const f32 a = TwoPi * (static_cast<f32>(i) + 0.5f) / 4.0f;
                const Vec2 pp = fp + Vec2{std::cos(a), std::sin(a)} * 2.4f;
                if (!occupied(pp, 0.6f)) {
                    push(PropCategory::Planter, static_cast<u8>(i % 3), pp.x, pp.y, 0.0f);
                    occ.emplace_back(pp, 0.6f);
                }
            }
            break;
        }
    }

    // Street lanterns in front of each house, plus a plaza ring.
    for (const detail::HousePlot& h : plots) {
        Vec2 toward = v.center - h.pos;
        const f32 len = glm::length(toward);
        if (len < 0.01f) {
            continue;
        }
        toward /= len;
        const Vec2 lp = h.pos + toward * (detail::house_reach(h.variant) + 0.6f);
        push(PropCategory::Lantern, 0, lp.x, lp.y, 0.0f);
    }
    for (int i = 0; i < 6; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 6.0f + 0.4f;
        push(PropCategory::Lantern, 0, cx + std::cos(a) * 11.5f, cz + std::sin(a) * 11.5f, 0.0f);
    }

    // Raised cobblestone streets: tiles along each gate->market avenue and market->house
    // spoke, plus a ring around the plaza. Tiles overlap slightly for a continuous path.
    // `rows` lateral lanes (offset by ~1.6 m) make a wide avenue a cart can roll down; the
    // tiles overlap (spacing 1.7 m, tiles are 2.4 m) so they butt into one continuous street.
    // Path tiles are 2.3 m square; lay them ~2.3 m apart (and rows 2.3 m apart) so they ABUT
    // edge-to-edge into one continuous cobbled street with no overlap (overlap = z-fighting).
    constexpr f32 tile = 2.3f;
    auto lay_path = [&](Vec2 a, Vec2 b, int rows) {
        const Vec2 d = b - a;
        const f32 L = glm::length(d);
        if (L < 1.5f) {
            return;
        }
        const Vec2 u = d / L;
        const Vec2 perp{-u.y, u.x};
        const f32 yaw = std::atan2(-u.y, u.x);
        const int n = std::max(1, static_cast<int>(std::round(L / tile)));
        for (int r = 0; r < rows; ++r) {
            const f32 lat = (static_cast<f32>(r) - static_cast<f32>(rows - 1) * 0.5f) * tile;
            for (int k = 1; k <= n; ++k) {
                const Vec2 p =
                    a + u * (static_cast<f32>(k) * L / static_cast<f32>(n)) + perp * lat;
                if (in_river(p.x, p.y)) {
                    continue; // the river crosses here - the bridge carries the street over it
                }
                push(PropCategory::Path, 0, p.x, p.y, yaw);
            }
        }
    };
    for (const detail::VillageGate& g : gates) {
        lay_path(v.center, g.pos, 2); // wide avenue from each gate to the market (a cart fits)
    }
    for (const detail::HousePlot& h : plots) {
        lay_path(v.center, h.pos, 1); // a footpath spoke to each house
    }
    // A paved plaza ring (two concentric rings, spaced a tile apart) around the market square.
    for (f32 rad : {11.0f, 13.3f}) {
        const int ring_n = std::max(10, static_cast<int>(TwoPi * rad / tile));
        for (int i = 0; i < ring_n; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(ring_n);
            const f32 rx = cx + std::cos(a) * rad, rz = cz + std::sin(a) * rad;
            if (in_river(rx, rz)) {
                continue;
            }
            push(PropCategory::Path, 0, rx, rz, a);
        }
    }

    // Greenery: planters ringing the plaza + bushes scattered on open interior ground.
    for (int i = 0; i < 5; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 5.0f + 1.1f;
        const Vec2 pp = v.center + Vec2{std::cos(a), std::sin(a)} * 11.0f;
        if (!occupied(pp, 0.7f) && !in_river(pp.x, pp.y)) {
            push(PropCategory::Planter, 0, pp.x, pp.y, 0.0f);
            occ.emplace_back(pp, 0.7f);
        }
    }
    // Scatter bushes + planters to green up the open ground, biased toward the bare band
    // between the outer house ring and the wall so the town doesn't read as empty there.
    const int green_n = static_cast<int>(half * 1.6f);
    for (int i = 0; i < green_n; ++i) {
        const f32 a = detail::hash01(detail::tree_hash(vid, i, 7700u)) * TwoPi;
        const f32 rr = (0.45f + 0.5f * detail::hash01(detail::tree_hash(vid, i, 7701u))) * half;
        if (rr > worldgen::town_radius(v, a, seed) - 1.6f) {
            continue; // stay inside the wall
        }
        const Vec2 bp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(bp, 1.0f) || in_river(bp.x, bp.y)) {
            continue;
        }
        const bool planter = detail::hash01(detail::tree_hash(vid, i, 7702u)) < 0.22f;
        push(planter ? PropCategory::Planter : PropCategory::Bush,
             static_cast<u8>(detail::tree_hash(vid, i, 7703u) % 3u), bp.x, bp.y,
             detail::hash01(detail::tree_hash(vid, i, 7704u)) * TwoPi);
        occ.emplace_back(bp, 1.0f);
    }

    // Medieval clutter to fill the town out: stored goods (barrels/crates/woodpiles/sacks)
    // tucked against house fronts, plus stalls, hay, signposts and troughs scattered along the
    // streets and plaza. Everything avoids the occupancy list and the gate->market avenues, so
    // nothing blocks a building or the cart's route.
    auto on_avenue = [&](const Vec2& p) {
        for (const detail::VillageGate& g : gates) {
            if (detail::point_seg_dist(p, v.center, g.pos) < 2.6f) {
                return true;
            }
        }
        return false;
    };
    for (usize hi = 0; hi < plots.size(); ++hi) {
        if (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8100u)) > 0.5f) {
            continue; // only ~half the houses get a goods pile out front
        }
        const detail::HousePlot& h = plots[hi];
        Vec2 toward = v.center - h.pos;
        const f32 len = glm::length(toward);
        if (len < 0.01f) {
            continue;
        }
        toward /= len;
        const Vec2 side{-toward.y, toward.x};
        const f32 soff = (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8101u)) - 0.5f) * 2.6f;
        const Vec2 dp = h.pos + toward * (detail::house_reach(h.variant) + 0.9f) + side * soff;
        if (occupied(dp, 0.8f) || on_avenue(dp) || in_river(dp.x, dp.y)) {
            continue;
        }
        const u8 stored[5] = {0, 1, 6, 7, 2}; // barrel, crates, woodpile, sacks, hay
        push(PropCategory::Decor, stored[detail::tree_hash(vid, static_cast<int>(hi), 8102u) % 5u],
             dp.x, dp.y, detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8103u)) * TwoPi);
        occ.emplace_back(dp, 0.8f);
    }
    const int decor_n = static_cast<int>(half * 1.5f);
    for (int i = 0; i < decor_n; ++i) {
        const f32 a = detail::hash01(detail::tree_hash(vid, i, 8200u)) * TwoPi;
        const f32 rr = (0.3f + 0.64f * detail::hash01(detail::tree_hash(vid, i, 8201u))) * half;
        if (rr > worldgen::town_radius(v, a, seed) - 1.8f) {
            continue; // stay inside the wall
        }
        const Vec2 dp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(dp, 1.1f) || on_avenue(dp) || in_river(dp.x, dp.y)) {
            continue;
        }
        u8 var;
        if (rr < half * 0.45f && detail::hash01(detail::tree_hash(vid, i, 8202u)) < 0.4f) {
            const u8 feature[3] = {3, 4, 5}; // stall, signpost, trough nearer the plaza
            var = feature[detail::tree_hash(vid, i, 8203u) % 3u];
        } else {
            const u8 common[5] = {0, 1, 2, 6, 7}; // barrel, crates, hay, woodpile, sacks
            var = common[detail::tree_hash(vid, i, 8204u) % 5u];
        }
        push(PropCategory::Decor, var, dp.x, dp.y,
             detail::hash01(detail::tree_hash(vid, i, 8205u)) * TwoPi);
        occ.emplace_back(dp, 1.1f);
    }

    // Palisade following the organic outline: walls marched around the boundary (skipping
    // gate gaps), towers flanking each gate, and periodic towers around the wall.
    constexpr f32 seg = 3.2f;
    const int N = glm::clamp(static_cast<int>(std::round(TwoPi * half / seg)), 16, 240);
    const int tower_every = std::max(3, N / 10);
    // True when angle `ang` is within (gate arc + `extra`) of any gate - so we can keep both
    // wall segments AND periodic towers out of the opening (a tower in the gap blocked the road).
    auto near_gate = [&](f32 ang, f32 extra) {
        for (const detail::VillageGate& g : gates) {
            const f32 r = worldgen::town_radius(v, g.ang, seed);
            const f32 arc = g.half / r + 0.06f + extra; // a wider gate cuts a wider gap
            if (std::abs(detail::ang_diff(ang, g.ang)) < arc) {
                return true;
            }
        }
        return false;
    };
    for (int i = 0; i < N; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(N);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(N);
        const f32 amid = TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(N);
        const Vec2 p0 = detail::town_boundary(v, a0, seed);
        const Vec2 p1 = detail::town_boundary(v, a1, seed);
        if (!near_gate(amid, 0.0f)) {
            const Vec2 mid = (p0 + p1) * 0.5f;
            const Vec2 chord = p1 - p0;
            push(PropCategory::Wall, static_cast<u8>(i % 2), mid.x, mid.y,
                 std::atan2(-chord.y, chord.x));
        }
        // Periodic boundary tower - kept a little further from the gate so it never stands in
        // the opening (the flanking gate towers below mark the gate itself).
        if (i % tower_every == 0 && !near_gate(a0, 0.05f)) {
            push(PropCategory::Gate, 1, p0.x, p0.y, 0.0f); // plain (unlit) periodic tower
        }
    }
    // A gatehouse on either side of each gate, facing the town centre (the lit towers + the only
    // braziers, so the night stays even). Placed just outside the opening edge, so for a WIDE
    // gate (several roads) they sit further apart and flank the whole span.
    for (const detail::VillageGate& g : gates) {
        Vec2 radial = g.pos - v.center;
        radial = glm::length(radial) > 1e-3f ? glm::normalize(radial) : Vec2{0.0f, 1.0f};
        const Vec2 tangent{-radial.y, radial.x};
        const f32 yaw = std::atan2(v.center.x - g.pos.x, v.center.y - g.pos.y); // front (+z) -> town
        for (f32 e : {-1.0f, 1.0f}) {
            const Vec2 tp = g.pos + tangent * (e * (g.half + detail::kGatehouseMargin));
            push(PropCategory::Gate, 0, tp.x, tp.y, yaw);
        }
    }
    return out;
}

// The town's gate-opening world positions (one where each road crosses the wall).
inline std::vector<Vec3> village_gates(const worldgen::Village& v, u32 seed) {
    std::vector<Vec3> gates;
    for (const detail::VillageGate& g : detail::village_gate_points(v, seed)) {
        const f32 gx = g.pos.x;
        const f32 gz = g.pos.y;
        gates.push_back(Vec3{gx, worldgen::height(gx, gz, seed), gz});
    }
    return gates;
}

// Intra-town dirt paths: spokes from the central market out to each house, plus a main
// avenue from the gate to the market. Returns the on-path amount (0..1) at (x,z), used
// to colour the town ground. Cheap per vertex (allocation-free) and only does real work
// inside a town.
inline f32 town_path_amount(const Vec3& p, f32 up, u32 seed) {
    if (p.y < worldgen::water_level + 0.5f) {
        return 0.0f;
    }
    const auto v = worldgen::village_containing(p.x, p.z, seed);
    if (!v) {
        return 0.0f;
    }
    const Vec2 q{p.x, p.z};
    f32 best = 1e30f;
    const auto gates = detail::village_gate_points(*v, seed);
    detail::for_each_house(*v, seed, gates, [&](const detail::HousePlot& h) {
        best = std::min(best, detail::point_seg_dist(q, v->center, h.pos));
    });
    // Main avenues: a gate -> market spine for each gate (continues the inter-town roads).
    for (const detail::VillageGate& g : gates) {
        best = std::min(best, detail::point_seg_dist(q, v->center, g.pos));
    }

    constexpr f32 hw = 1.2f; // town path half-width
    const f32 band = glm::smoothstep(hw + 0.4f, hw - 0.4f, best);
    const f32 gentle = glm::smoothstep(0.55f, 0.8f, up);
    return band * gentle;
}

// Overlays the town's dirt paths onto a base terrain colour (applied while meshing,
// like roads::tint_surface, to avoid pulling the village layout into surface_color).
inline Vec3 town_path_tint(Vec3 color, const Vec3& p, f32 up, u32 seed) {
    const f32 on = town_path_amount(p, up, seed);
    if (on > 0.0f) {
        color = glm::mix(color, Vec3{0.40f, 0.33f, 0.24f}, on * 0.85f);
    }
    return color;
}

} // namespace alryn
