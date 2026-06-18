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
#include <vector>

namespace alryn {

namespace detail {

inline constexpr f32 kGateHalf = 2.6f; // gate opening half-width

// A gate opening: its angle from the town centre and its world position on the wall.
struct VillageGate {
    f32 ang = 0.0f;
    Vec2 pos{0.0f};
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

// The town's gates - one where each incident road meets the (organic) wall, so roads run
// straight through the opening. An isolated town gets one default gate (+Z). Shared by
// village_props + village_gates so gates line up with the roads.
inline std::vector<VillageGate> village_gate_points(const worldgen::Village& v, u32 seed) {
    std::vector<VillageGate> gates;
    const std::vector<roads::Segment> segs =
        roads::gather(v.center, worldgen::village_cell * roads::road_max_cells, seed);
    constexpr f32 tol = 1.5f;
    auto add_gate = [&](Vec2 d) {
        if (glm::length(d) < 1e-3f) {
            return;
        }
        d = glm::normalize(d);
        const f32 ang = std::atan2(d.y, d.x);
        for (const VillageGate& e : gates) {
            if (std::abs(ang_diff(ang, e.ang)) < 0.4f) {
                return; // merge near-duplicate gates
            }
        }
        gates.push_back(VillageGate{ang, town_boundary(v, ang, seed)});
    };
    for (const roads::Segment& s : segs) {
        if (glm::length(s.a - v.center) < tol) {
            add_gate(s.b - s.a);
        } else if (glm::length(s.b - v.center) < tol) {
            add_gate(s.a - s.b);
        }
    }
    if (gates.empty()) {
        add_gate(Vec2{0.0f, 1.0f}); // isolated town: one default gate (+Z)
    }
    return gates;
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
    constexpr f32 market_reach = 4.8f; // the central market's footprint (stalls) + slack
    constexpr f32 wall_margin = 1.2f;  // gap kept between a house and the palisade
    constexpr f32 avenue_half = 2.8f;  // keep houses off the gate->market avenues

    std::array<Vec2, 64> placed;
    std::array<f32, 64> reach;
    int np = 0;
    auto try_place = [&](f32 x, f32 z, f32 yaw, u8 variant) -> bool {
        const f32 r = house_reach(variant);
        if (std::abs(worldgen::height(x, z, seed) - v.ground) > 1.6f) {
            return false; // uneven ground
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

    auto ring = [&](f32 radius, int count, u32 rsalt) {
        for (int i = 0; i < count; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(count) +
                          detail::hash01(detail::tree_hash(vid, i, rsalt)) * 0.3f;
            const f32 x = cx + std::cos(a) * radius;
            const f32 z = cz + std::sin(a) * radius;
            const f32 yaw = std::atan2(cx - x, cz - z); // front (+Z) faces the centre
            try_place(x, z, yaw,
                      static_cast<u8>(detail::tree_hash(vid, i, rsalt + 1u) % kHouseVariants));
        }
    };
    auto ring_for = [&](f32 r, f32 spacing, int min_n, u32 rsalt) {
        ring(r, std::max(min_n, static_cast<int>(std::round(TwoPi * r / spacing))), rsalt);
    };
    // Three loose rings (bigger towns fill all three; small towns drop the outer one to
    // rejection) so a town reads as a populated, lived-in cluster of homes.
    ring_for(half * 0.48f, 20.0f, 3, 100u); // inner ring around the market
    ring_for(half * 0.66f, 22.0f, 4, 200u); // middle ring
    ring_for(half * 0.83f, 24.0f, 5, 300u); // outer ring near the walls

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
    occ.emplace_back(v.center, 4.8f); // the market
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
        PropInstance p;
        p.category = cat;
        p.variant = var;
        p.position = Vec3{x, worldgen::height(x, z, seed), z};
        p.yaw = yaw;
        p.scale = 1.0f;
        out.push_back(p);
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
    occ.emplace_back(v.center, 4.8f);

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

    // Reserve the wagon depot spots so nothing decorative lands on them.
    for (const Vec3& s : village_wagon_spots(v, seed)) {
        occ.emplace_back(Vec2{s.x, s.z}, 2.0f);
    }

    // A fountain on the plaza, just off the market (try a few offsets for clear ground).
    const Vec2 gate_dir = gates.empty() ? Vec2{0.0f, 1.0f} : glm::normalize(gates[0].pos - v.center);
    const Vec2 gate_perp{-gate_dir.y, gate_dir.x};
    for (f32 off : {7.0f, -7.0f, 9.0f}) {
        const Vec2 fp = v.center + gate_perp * off;
        if (!occupied(fp, 2.4f)) {
            push(PropCategory::Fountain, 0, fp.x, fp.y, 0.0f);
            occ.emplace_back(fp, 2.4f);
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
        push(PropCategory::Lantern, 0, cx + std::cos(a) * 7.0f, cz + std::sin(a) * 7.0f, 0.0f);
    }

    // Raised cobblestone streets: tiles along each gate->market avenue and market->house
    // spoke, plus a ring around the plaza. Tiles overlap slightly for a continuous path.
    auto lay_path = [&](Vec2 a, Vec2 b) {
        const Vec2 d = b - a;
        const f32 L = glm::length(d);
        if (L < 1.5f) {
            return;
        }
        const Vec2 u = d / L;
        const f32 yaw = std::atan2(-u.y, u.x);
        const int n = std::max(1, static_cast<int>(L / 1.9f));
        for (int k = 1; k <= n; ++k) {
            const Vec2 p = a + u * (static_cast<f32>(k) * L / static_cast<f32>(n));
            push(PropCategory::Path, 0, p.x, p.y, yaw);
        }
    };
    for (const detail::VillageGate& g : gates) {
        lay_path(v.center, g.pos);
    }
    for (const detail::HousePlot& h : plots) {
        lay_path(v.center, h.pos);
    }
    const int ring_n = std::max(8, static_cast<int>(TwoPi * 5.5f / 1.9f));
    for (int i = 0; i < ring_n; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(ring_n);
        push(PropCategory::Path, 0, cx + std::cos(a) * 5.5f, cz + std::sin(a) * 5.5f, a);
    }

    // Greenery: planters ringing the plaza + bushes scattered on open interior ground.
    for (int i = 0; i < 5; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 5.0f + 1.1f;
        const Vec2 pp = v.center + Vec2{std::cos(a), std::sin(a)} * 6.2f;
        if (!occupied(pp, 0.7f)) {
            push(PropCategory::Planter, 0, pp.x, pp.y, 0.0f);
            occ.emplace_back(pp, 0.7f);
        }
    }
    for (int i = 0; i < 28; ++i) {
        const f32 a = detail::hash01(detail::tree_hash(vid, i, 7700u)) * TwoPi;
        const f32 rr = (0.3f + 0.6f * detail::hash01(detail::tree_hash(vid, i, 7701u))) * half;
        if (rr > worldgen::town_radius(v, a, seed) - 2.0f) {
            continue; // stay inside the wall
        }
        const Vec2 bp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(bp, 1.0f)) {
            continue;
        }
        const bool planter = detail::hash01(detail::tree_hash(vid, i, 7702u)) < 0.25f;
        push(planter ? PropCategory::Planter : PropCategory::Bush,
             static_cast<u8>(detail::tree_hash(vid, i, 7703u) % 3u), bp.x, bp.y,
             detail::hash01(detail::tree_hash(vid, i, 7704u)) * TwoPi);
        occ.emplace_back(bp, 1.0f);
    }

    // Palisade following the organic outline: walls marched around the boundary (skipping
    // gate gaps), towers flanking each gate, and periodic towers around the wall.
    constexpr f32 seg = 3.2f;
    const int N = glm::clamp(static_cast<int>(std::round(TwoPi * half / seg)), 16, 240);
    const int tower_every = std::max(3, N / 10);
    for (int i = 0; i < N; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(N);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(N);
        const f32 amid = TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(N);
        bool in_gate = false;
        for (const detail::VillageGate& g : gates) {
            const f32 r = worldgen::town_radius(v, g.ang, seed);
            const f32 arc = detail::kGateHalf / r + 0.06f;
            if (std::abs(detail::ang_diff(amid, g.ang)) < arc) {
                in_gate = true;
                break;
            }
        }
        const Vec2 p0 = detail::town_boundary(v, a0, seed);
        const Vec2 p1 = detail::town_boundary(v, a1, seed);
        if (!in_gate) {
            const Vec2 mid = (p0 + p1) * 0.5f;
            const Vec2 chord = p1 - p0;
            push(PropCategory::Wall, static_cast<u8>(i % 2), mid.x, mid.y,
                 std::atan2(-chord.y, chord.x));
        }
        if (i % tower_every == 0) {
            push(PropCategory::Gate, 1, p0.x, p0.y, 0.0f); // plain (unlit) periodic boundary tower
        }
    }
    // Lit gate towers flanking each gate gap (the only braziers, so the night stays even).
    for (const detail::VillageGate& g : gates) {
        const f32 r = worldgen::town_radius(v, g.ang, seed);
        const f32 arc = detail::kGateHalf / r + 0.06f;
        for (f32 e : {-arc, arc}) {
            const Vec2 tp = detail::town_boundary(v, g.ang + e, seed);
            push(PropCategory::Gate, 0, tp.x, tp.y, 0.0f);
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
