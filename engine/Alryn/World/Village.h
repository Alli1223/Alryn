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
#include <memory>
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
inline constexpr f32 kStreetHalf = 2.3f;     // clear street half-width (a cart fits down a street)

// The open square at a settlement's heart: the market (ringed by its road) in a village, town or city;
// just a little green round the well in a hamlet.
inline f32 plaza_half(const worldgen::Village& v) { return worldgen::has_market(v) ? kMarketHalf : 4.5f; }
// The ring road round the market (none in a hamlet - its lane runs past the well).
inline f32 ring_road_radius(const worldgen::Village& v) { return plaza_half(v) + 2.5f; }

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

// The roads into a settlement: its gates (where they cross the wall) and each road's actual course
// from the centre out through its gate (a few metres past it) - the town's main streets follow these, so
// the houses line the real road rather than an idealised straight avenue beside it.
struct TownRoads {
    std::vector<VillageGate> gates;
    std::vector<std::vector<Vec2>> paths;
};

// Computes the town's gates: one where each incident road actually CROSSES the (organic) wall.
// A road meanders on its way out of town, so where it crosses the wall can be well off its
// initial heading - cutting the gate at the heading (the old bug) left the road butting into a
// wall. Here we take the road to each neighbouring town, walk it from this centre outward, and
// open the wall at the first boundary crossing, so the road always runs through the gap. Only
// incident roads count, so the gate total stays bounded. Isolated town -> one default gate (+Z).
// (A hamlet has no wall, but its "gates" still mark where the roads come in.)
inline TownRoads compute_town_roads(const worldgen::Village& v, u32 seed) {
    TownRoads out;
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
                    // Keep the road's course out to here (+ a few metres past the gate), once per gate -
                    // roads to several towns often leave along the same first leg.
                    bool dup = false;
                    for (const Vec2& c : crossings) {
                        dup = dup || glm::length(c - cross) < 3.0f;
                    }
                    if (!dup) {
                        std::vector<Vec2> path(poly.begin(), poly.begin() + static_cast<std::ptrdiff_t>(i));
                        const Vec2 seg = poly[i] - poly[i - 1];
                        const f32 sl = glm::length(seg);
                        path.push_back(sl > 1e-3f ? cross + seg / sl * 4.0f : cross);
                        out.paths.push_back(std::move(path));
                    }
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
        const f32 half = glm::clamp(spread + roads::road_half_width + 0.9f, kGateHalf, kGateMaxHalf);
        out.gates.push_back(VillageGate{ang, gpos, half});
    }
    if (out.gates.empty()) {
        out.gates.push_back(VillageGate{HalfPi, town_boundary(v, HalfPi, seed), kGateHalf});
    }
    return out;
}

// Cached per town (keyed by its layout seed + world seed), because this runs per terrain
// vertex while colouring the town paths - computing the road crossings every time would be
// far too slow. Thread-safe (the terrain mesher runs on a worker thread).
inline const TownRoads& village_roads(const worldgen::Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, std::shared_ptr<const TownRoads>> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return *it->second;
        }
    }
    auto roads = std::make_shared<const TownRoads>(compute_town_roads(v, seed));
    std::lock_guard<std::mutex> lock(mtx);
    return *cache.emplace(key, std::move(roads)).first->second;
}
inline std::vector<VillageGate> compute_gates(const worldgen::Village& v, u32 seed) {
    return compute_town_roads(v, seed).gates;
}
inline std::vector<VillageGate> village_gate_points(const worldgen::Village& v, u32 seed) {
    return village_roads(v, seed).gates;
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

inline RiverInfo town_river(const worldgen::Village& /*v*/, u32 /*seed*/) {
    // Town rivers are DISABLED: the in-town stone canal + its crossing bridges read badly, so every
    // town now sits on dry land. The channel/bridge placement in village_props (gated on
    // RiverInfo::present) plus build_river()/build_stone_bridge() stay dormant - re-enable by
    // restoring the old eligibility test (big towns, ~half of them, offset to one side of the market).
    return RiverInfo{}; // present = false
}

// Signed perpendicular distance of world point p from the river centreline; |.| < half_width
// means p is in the water.
inline f32 river_dist(const RiverInfo& r, const Vec2& center, const Vec2& p) {
    return glm::dot(p - center, r.normal) - r.offset;
}

// A street segment (centreline).
struct Street {
    Vec2 a, b;
};

// The settlement's STREET NETWORK, in its own style (worldgen::TownLayout):
//   LANE   - a hamlet / village HIGH STREET right across along its main road, with short lanes off it
//            (a village's little market sits on a ring in the middle; a hamlet's lane passes its well).
//   RADIAL - a ring road circling the market (the centre stays a clear plaza), an avenue from the ring
//            out to each gate (the cart's way out of town) and a few radial spokes into the outer town.
//   GRID   - a planned town: a grid of streets turned to its main gate's heading, round the market ring.
//   RINGS  - a great city: concentric ring roads every ~20 m out to the wall, crossed by radial avenues.
// Rings + spokes follow the town's organic outline (pulled in clear of the wall wherever it bulges in).
// Houses line these streets facing them and the ground is tinted + cobbled along them - so placement
// (`for_each_house`) and colouring (`town_path_amount`) always agree.
inline std::vector<Street> town_streets(const worldgen::Village& v, u32 seed, const std::vector<VillageGate>& gates) {
    std::vector<Street> out;
    auto add = [&](Vec2 a, Vec2 b) {
        if (glm::length(b - a) > 1.5f) {
            out.push_back({a, b});
        }
    };
    const Vec2 c = v.center;
    const int vid = static_cast<int>(v.vseed);
    const bool market = worldgen::has_market(v);
    const f32 ring_r = ring_road_radius(v);
    const f32 base_ang = gates.empty() ? hash01(tree_hash(vid, 0, 720u + seed)) * TwoPi
                                       : std::atan2(gates[0].pos.y - c.y, gates[0].pos.x - c.x);
    // How far out from the centre along `dir` a street may run: `r`, or less where the wall comes in.
    auto reach = [&](const Vec2& dir, f32 r, f32 margin) {
        return std::max(0.0f, std::min(r, worldgen::town_radius(v, std::atan2(dir.y, dir.x), seed) - margin));
    };
    // A ring road at radius r (a polygon of `sides`), conforming to the outline.
    auto ring = [&](f32 r, int sides, f32 phase) {
        Vec2 prev{};
        for (int i = 0; i <= sides; ++i) {
            const f32 a = base_ang + phase + TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
            const Vec2 dir{std::cos(a), std::sin(a)};
            const Vec2 p = c + dir * reach(dir, r, 7.0f);
            if (i > 0) {
                add(prev, p);
            }
            prev = p;
        }
    };
    // The main streets out to each gate, from the ring (or from the centre, in a ring-less hamlet):
    // the REAL road's course through the town, so the homes that line them line the road itself. (A town
    // no road reaches gets a straight avenue to its one gate.)
    const TownRoads& tr = village_roads(v, seed);
    auto gate_avenues = [&](f32 from) {
        for (const std::vector<Vec2>& path : tr.paths) {
            for (usize i = 1; i < path.size(); ++i) {
                Vec2 a = path[i - 1];
                const Vec2 b = path[i];
                if (glm::length(b - c) <= from) {
                    continue; // still inside the plaza ring
                }
                if (glm::length(a - c) < from) {
                    // Start this leg where it leaves the ring: solve |a + t(b-a) - c| = from.
                    const Vec2 d = b - a, f = a - c;
                    const f32 A = glm::dot(d, d), B = 2.0f * glm::dot(f, d), C = glm::dot(f, f) - from * from;
                    const f32 disc = std::max(0.0f, B * B - 4.0f * A * C);
                    const f32 t = A > 1e-6f ? glm::clamp((-B + std::sqrt(disc)) / (2.0f * A), 0.0f, 1.0f) : 0.0f;
                    a += d * t;
                }
                add(a, b);
            }
        }
        if (tr.paths.empty()) {
            for (const VillageGate& g : gates) {
                Vec2 dir = g.pos - c;
                const f32 len = glm::length(dir);
                if (len < 1e-3f) {
                    continue;
                }
                dir /= len;
                add(c + dir * from, g.pos);
            }
        }
    };

    switch (v.layout) {
        case worldgen::TownLayout::Lane: {
            if (market) {
                ring(ring_r, 8, 0.0f);
            }
            const f32 from = market ? ring_r : 0.0f;
            gate_avenues(from);
            const Vec2 axis{std::cos(base_ang), std::sin(base_ang)};
            // The high street runs on through to the far side even where no road leaves that way.
            bool far_gate = false;
            for (const VillageGate& g : gates) {
                const Vec2 d = g.pos - c;
                if (glm::length(d) > 1e-3f && glm::dot(glm::normalize(d), axis) < -0.7f) {
                    far_gate = true;
                }
            }
            if (!far_gate) {
                add(c - axis * std::max(from, 0.01f), c - axis * reach(-axis, v.half * 0.9f, 4.0f));
            }
            // Short lanes off the high street, to either side.
            const Vec2 side{-axis.y, axis.x};
            const int lanes = v.tier == worldgen::TownTier::Hamlet ? 2 : 4 + static_cast<int>(v.vseed % 3u);
            for (int i = 0; i < lanes; ++i) {
                const f32 dir = (i % 2 == 0) ? 1.0f : -1.0f;
                // Spread along the high street, a couple each way from the middle.
                const int per_side = (lanes + 1) / 2;
                const f32 slot = (static_cast<f32>(i / 2) + 0.25f + 0.5f * hash01(tree_hash(vid, i, 721u))) /
                                 static_cast<f32>(per_side);
                const f32 along = dir * (from + 6.0f + std::max(0.0f, v.half * 0.75f - from - 6.0f) * slot);
                const Vec2 root = c + axis * along;
                const f32 s = hash01(tree_hash(vid, i, 722u)) < 0.5f ? 1.0f : -1.0f;
                const bool both = hash01(tree_hash(vid, i, 723u)) < 0.45f;
                for (const f32 sg : {s, -s}) {
                    if (sg != s && !both) {
                        continue;
                    }
                    const Vec2 d = side * sg;
                    const Vec2 tip = root + d * (v.half * 0.8f);
                    const Vec2 td = tip - c;
                    const f32 tl = glm::length(td);
                    const f32 lim = tl > 1e-3f ? reach(td / tl, tl, 5.0f) : 0.0f;
                    add(root, c + (tl > 1e-3f ? td / tl : d) * lim);
                }
            }
            break;
        }
        case worldgen::TownLayout::Radial: {
            ring(ring_r, 8, 0.0f);
            gate_avenues(ring_r);
            constexpr int spokes = 4;
            for (int i = 0; i < spokes; ++i) {
                const f32 a = base_ang + TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(spokes);
                const Vec2 dir{std::cos(a), std::sin(a)};
                add(c + dir * ring_r, c + dir * reach(dir, v.half * 0.9f, 4.0f));
            }
            break;
        }
        case worldgen::TownLayout::Grid: {
            ring(ring_r, 8, 0.0f);
            gate_avenues(ring_r);
            constexpr f32 pitch = 19.0f; // the block size
            const f32 R = v.half * 0.95f;
            const Vec2 ax{std::cos(base_ang), std::sin(base_ang)};
            const Vec2 ay{-ax.y, ax.x};
            // Pull a grid line's end in until it's clear of the wall.
            auto clip = [&](Vec2 p, const Vec2& toward) {
                for (int k = 0; k < 24; ++k) {
                    const Vec2 d = p - c;
                    const f32 l = glm::length(d);
                    if (l < 1e-3f || l < worldgen::town_radius(v, std::atan2(d.y, d.x), seed) - 5.0f) {
                        break;
                    }
                    p += (toward - p) * 0.12f;
                }
                return p;
            };
            const f32 hole = ring_r + 2.0f; // the market + its ring road stay clear
            for (int axis = 0; axis < 2; ++axis) {
                const Vec2 along = axis == 0 ? ax : ay;
                const Vec2 across = axis == 0 ? ay : ax;
                const int n = static_cast<int>(R / pitch);
                for (int k = -n - 1; k <= n; ++k) {
                    const f32 off = (static_cast<f32>(k) + 0.5f) * pitch;
                    if (std::abs(off) >= R - 6.0f) {
                        continue;
                    }
                    const f32 hl = std::sqrt(R * R - off * off);
                    const Vec2 mid = c + across * off;
                    const Vec2 a = clip(mid - along * hl, mid);
                    const Vec2 b = clip(mid + along * hl, mid);
                    if (std::abs(off) < hole) {
                        const f32 cut = std::sqrt(hole * hole - off * off);
                        if (glm::dot(a - mid, along) < -cut) {
                            add(a, mid - along * cut);
                        }
                        if (glm::dot(b - mid, along) > cut) {
                            add(mid + along * cut, b);
                        }
                    } else {
                        add(a, b);
                    }
                }
            }
            break;
        }
        case worldgen::TownLayout::Rings: {
            constexpr f32 pitch = 20.5f; // ring spacing: a row of houses either side of each ring
            f32 outer = ring_r;
            int k = 0;
            for (f32 r = ring_r; r < v.half * 0.92f; r += pitch, ++k) {
                const int sides = std::max(8, static_cast<int>(std::round(TwoPi * r / 15.0f)));
                ring(r, sides, k % 2 == 0 ? 0.0f : Pi / static_cast<f32>(sides));
                outer = r;
            }
            gate_avenues(ring_r);
            // A few long radial avenues from the first ring out (each junction costs a row of homes, so
            // the inner rings are crossed only by the roads in).
            const int spokes = v.half > 92.0f ? 7 : 6;
            for (int i = 0; i < spokes; ++i) {
                const f32 a = base_ang + TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(spokes);
                const Vec2 dir{std::cos(a), std::sin(a)};
                add(c + dir * (ring_r + pitch), c + dir * reach(dir, outer, 6.0f));
            }
            break;
        }
    }
    return out;
}

// town_streets, cached per settlement (the terrain colours + cobbles ask per vertex).
inline const std::vector<Street>& village_streets(const worldgen::Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, std::shared_ptr<const std::vector<Street>>> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return *it->second;
        }
    }
    auto streets = std::make_shared<const std::vector<Street>>(town_streets(v, seed, village_gate_points(v, seed)));
    std::lock_guard<std::mutex> lock(mtx);
    return *cache.emplace(key, std::move(streets)).first->second;
}

// A building's footprint as an oriented rectangle in world xz: its local +X / +Z axes (the house is
// drawn rotateY(yaw), so local +Z - its front - points along (sin yaw, cos yaw)) and half-extents.
struct Footprint {
    Vec2 c{0.0f};
    Vec2 ax{1.0f, 0.0f};
    Vec2 az{0.0f, 1.0f};
    Vec2 e{1.0f};
    Footprint() = default;
    Footprint(const Vec2& center, f32 yaw, const Vec2& half)
        : c(center), ax{std::cos(yaw), -std::sin(yaw)}, az{std::sin(yaw), std::cos(yaw)}, e(half) {}
    Vec2 corner(f32 sx, f32 sz) const { return c + ax * (e.x * sx) + az * (e.y * sz); }
};
// Do two footprints (each grown by `pad`) overlap? (Separating-axis test over the four edge normals.)
inline bool footprints_overlap(const Footprint& a, const Footprint& b, f32 pad) {
    const Vec2 d = b.c - a.c;
    for (const Vec2& n : {a.ax, a.az, b.ax, b.az}) {
        const f32 ra = (a.e.x + pad) * std::abs(glm::dot(a.ax, n)) + (a.e.y + pad) * std::abs(glm::dot(a.az, n));
        const f32 rb = (b.e.x + pad) * std::abs(glm::dot(b.ax, n)) + (b.e.y + pad) * std::abs(glm::dot(b.az, n));
        if (std::abs(glm::dot(d, n)) > ra + rb) {
            return false;
        }
    }
    return true;
}

// Deterministically lays out a settlement's buildings: rows of homes lining every street (facing
// it), its landmarks (a city's KEEP, a town's or city's CHAPEL), and (in some towns) a two-storey
// pair joined by a covered bridge. A candidate is rejected if it sits on uneven ground, would clip
// the plaza, cross the perimeter wall, sit on a street or the real road, or overlap another
// building - so buildings never intersect anything. Calls `fn(HousePlot)` for each accepted one;
// fills `*bridge` if a bridge pair was placed. Shared (via the cached town plan) by village_props,
// the levelled building pads, the townsfolk + the decorative trees, so they always agree.
template <typename Fn>
inline void for_each_house(const worldgen::Village& v, u32 seed, const std::vector<VillageGate>& gates, Fn&& fn,
                           BridgeInfo* bridge = nullptr) {
    const f32 cx = v.center.x;
    const f32 cz = v.center.y;
    const f32 half = v.half;
    const int vid = static_cast<int>(v.vseed);
    const bool city = v.tier == worldgen::TownTier::City;
    const bool hamlet = v.tier == worldgen::TownTier::Hamlet;
    const f32 market_reach = plaza_half(v); // the central plaza footprint + slack
    constexpr f32 wall_margin = 1.2f;       // gap kept between a house and the palisade
    constexpr f32 road_half = kStreetHalf;  // clear road-corridor half-width (a cart fits down a street)
    constexpr f32 eave = 0.35f;             // roof overhang + a sliver of alley between neighbours

    const RiverInfo river = town_river(v, seed);
    const std::vector<Street> streets = town_streets(v, seed, gates);

    std::vector<Footprint> placed;
    placed.reserve(city ? 640 : 128);
    // The house's own plot must be buildable: dry, and level enough across its footprint (each house
    // stands on its own levelled pad, so a settlement can climb a gentle rise - but not a crag).
    auto ground_ok = [&](const Footprint& f) {
        const f32 gh = worldgen::base_height(f.c.x, f.c.y, seed);
        if (gh < worldgen::water_level + 1.0f || std::abs(gh - v.ground) > (city ? 9.0f : 5.0f)) {
            return false;
        }
        for (const f32 sx : {-1.0f, 1.0f}) {
            for (const f32 sz : {-1.0f, 1.0f}) {
                const Vec2 p = f.corner(sx, sz);
                const f32 h = worldgen::base_height(p.x, p.y, seed);
                if (h < worldgen::water_level + 0.8f || std::abs(h - gh) > 1.5f) {
                    return false;
                }
            }
        }
        return true;
    };
    auto try_place = [&](f32 x, f32 z, f32 yaw, u8 variant) -> bool {
        const Vec2 e = PropLibrary::house_half_extents(variant);
        const Footprint f{Vec2{x, z}, yaw, e};
        const f32 r = std::max(e.x, e.y) + eave;
        const Vec2 d{x - cx, z - cz};
        const f32 dist = glm::length(d);
        if (dist < r + market_reach) {
            return false; // would clip the central marketplace / green
        }
        if (river.present && std::abs(river_dist(river, v.center, Vec2{x, z})) < river.half_width + r + 1.0f) {
            return false; // keep houses off the river channel + its banks
        }
        // Every corner stays inside the (organic) perimeter wall.
        for (const f32 sx : {-1.0f, 1.0f}) {
            for (const f32 sz : {-1.0f, 1.0f}) {
                const Vec2 q = f.corner(sx, sz) - v.center;
                if (glm::length(q) > worldgen::town_radius(v, std::atan2(q.y, q.x), seed) - wall_margin) {
                    return false;
                }
            }
        }
        // Keep every street corridor clear: a house sits BESIDE its own street (its front a step back
        // from the kerb), and no corner may jut out into a crossing one at a junction.
        const Vec2 pts[5] = {f.c, f.corner(-1.0f, -1.0f), f.corner(1.0f, -1.0f), f.corner(-1.0f, 1.0f),
                             f.corner(1.0f, 1.0f)};
        for (const Street& s : streets) {
            if (point_seg_dist(pts[0], s.a, s.b) < road_half + std::min(e.x, e.y) * 0.9f) {
                return false;
            }
            for (int k = 1; k < 5; ++k) {
                if (point_seg_dist(pts[k], s.a, s.b) < road_half + 0.15f) {
                    return false;
                }
            }
        }
        // Also keep clear of the actual (meandering) inter-town ROAD where it strays from the streets.
        if (roads::distance(x, z, seed) < roads::road_half_width + std::min(e.x, e.y) * 0.9f) {
            return false;
        }
        for (int k = 1; k < 5; ++k) {
            if (roads::distance(pts[k].x, pts[k].y, seed) < roads::road_half_width + 0.1f) {
                return false;
            }
        }
        for (const Footprint& o : placed) {
            if (footprints_overlap(f, o, eave)) {
                return false; // would overlap an already-placed house
            }
        }
        if (!ground_ok(f)) {
            return false; // too uneven to build on, or it would sit in the water
        }
        placed.push_back(f);
        fn(HousePlot{Vec2{x, z}, yaw, variant});
        return true;
    };

    // LANDMARKS first (they need the room): a city's great keep and a town's / city's chapel, each on
    // the first open block found walking out from the plaza - turned to face the town centre.
    auto place_landmark = [&](u8 variant, u32 salt, f32 r0, f32 r1) {
        const f32 a0 = hash01(tree_hash(vid, 0, salt)) * TwoPi;
        for (int ri = 0; ri < 8; ++ri) {
            const f32 rr = glm::mix(r0, r1, static_cast<f32>(ri) / 7.0f);
            for (int ai = 0; ai < 32; ++ai) {
                const f32 a = a0 + TwoPi * static_cast<f32>(ai) / 32.0f;
                const Vec2 p = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
                const f32 yaw = std::atan2(cx - p.x, cz - p.y); // front (+Z) toward the centre
                if (try_place(p.x, p.y, yaw, variant)) {
                    return;
                }
            }
        }
    };
    if (city) {
        place_landmark(static_cast<u8>(kHouseKeep), 940u, ring_road_radius(v) + 12.0f, half * 0.6f);
    }
    if (v.tier == worldgen::TownTier::Town || city) {
        place_landmark(static_cast<u8>(kHouseChapel), 941u, ring_road_radius(v) + 9.0f, half * 0.7f);
    }

    // Houses LINE the streets, facing them: walk each side of each street dropping homes shoulder to
    // shoulder - a hamlet's cottages spaced out among their gardens, a town's houses a short alley apart,
    // a city's packed into terraces - each set back clear of the street and turned so its front (+Z)
    // faces it. A pub + a smithy turn up near the start (every ~50 homes again in a city), and tall
    // townhouses crowd a city's inner rings. `try_place` rejects any that won't fit, so the rows break
    // naturally round the plaza, the gates and the junctions.
    const f32 gap_lo = hamlet ? 2.4f : v.tier == worldgen::TownTier::Village ? 1.2f : city ? 0.3f : 0.8f;
    const f32 gap_hi = hamlet ? 5.5f : v.tier == worldgen::TownTier::Village ? 3.0f : city ? 1.0f : 2.2f;
    const bool has_pub = !hamlet || hash01(tree_hash(vid, 1, 732u)) < 0.4f;
    int idx = 0;
    for (const Street& st : streets) {
        Vec2 dir = st.b - st.a;
        const f32 len = glm::length(dir);
        if (len < 4.0f) {
            continue;
        }
        dir /= len;
        const Vec2 nrm{-dir.y, dir.x};
        for (const f32 side : {-1.0f, 1.0f}) {
            f32 cursor = 0.6f + hash01(tree_hash(vid, idx, 730u)) * gap_hi;
            while (true) {
                const u32 hh = tree_hash(vid, idx, 731u);
                const Vec2 probe = st.a + dir * std::min(cursor, len);
                const bool inner = city && glm::length(probe - v.center) < half * 0.5f;
                u8 var;
                if (has_pub && (idx == 4 || (city && idx % 47 == 30))) {
                    var = static_cast<u8>(kHousePub);
                } else if (!hamlet && (idx == 9 || (city && idx % 59 == 40))) {
                    var = static_cast<u8>(kHouseBlacksmith);
                } else if (hh % (inner ? 3u : 7u) == 0u) {
                    var = static_cast<u8>(kHouseTownhouse);
                } else {
                    var = static_cast<u8>(hh % kHouseVariants);
                }
                const Vec2 e = PropLibrary::house_half_extents(var);
                if (cursor + 2.0f * e.x > len - 0.4f) {
                    break;
                }
                const Vec2 hc = st.a + dir * (cursor + e.x) + nrm * side * (road_half + e.y + 0.45f);
                const Vec2 face = nrm * (-side); // front (+Z) -> the street
                const bool ok = try_place(hc.x, hc.y, std::atan2(face.x, face.y), var);
                ++idx;
                cursor += ok ? 2.0f * e.x + glm::mix(gap_lo, gap_hi, hash01(tree_hash(vid, idx, 733u))) : 1.7f;
            }
        }
    }

    // Some towns + cities have a pair of two-storey houses joined by a raised covered bridge.
    if ((v.tier == worldgen::TownTier::Town || city) && hash01(tree_hash(vid, 0, 900u)) < 0.55f) {
        const f32 a = hash01(tree_hash(vid, 1, 901u)) * TwoPi;
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

// The settlement's buildings, laid out once (for_each_house) and cached: a city's few hundred homes
// are far too many to lay out again for every chunk / tree / townsperson that asks.
struct TownPlan {
    std::vector<HousePlot> houses;
    BridgeInfo bridge;
};
inline const TownPlan& cached_town_plan(const worldgen::Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, std::shared_ptr<const TownPlan>> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return *it->second;
        }
    }
    auto plan = std::make_shared<TownPlan>();
    for_each_house(
        v, seed, village_gate_points(v, seed), [&](const HousePlot& h) { plan->houses.push_back(h); },
        &plan->bridge);
    std::lock_guard<std::mutex> lock(mtx);
    return *cache.emplace(key, std::move(plan)).first->second;
}
} // namespace detail

// A few reserved wagon-depot spots near the main gate side of the plaza, clear of houses + market,
// where the transport offers park. Server-side `generate_offers` uses these so a wagon never spawns
// inside a building (gathers the house layout to avoid it).
inline std::vector<Vec3> village_wagon_spots(const worldgen::Village& v, u32 seed, int count = 4) {
    const auto gates = detail::village_gate_points(v, seed);
    std::vector<std::pair<Vec2, f32>> occ;
    for (const detail::HousePlot& h : detail::cached_town_plan(v, seed).houses) {
        occ.emplace_back(h.pos, detail::house_reach(h.variant));
    }
    occ.emplace_back(v.center, detail::plaza_half(v)); // the market square / the green
    const Vec2 dir = gates.empty() ? Vec2{0.0f, 1.0f} : glm::normalize(gates[0].pos - v.center);
    const Vec2 perp{-dir.y, dir.x};
    constexpr f32 kSpotSpacing = 3.4f; // distance between adjacent depot spots
    std::vector<Vec3> spots;
    // March outward from the plaza toward the main gate in rows, sweeping laterally in each,
    // and accept positions clear of buildings/market *and* of spots already placed - so every
    // offer gets its own distinct spot and wagons never stack.
    const f32 start = std::max(v.half * 0.34f, detail::plaza_half(v) + 4.5f);
    for (int row = 0; row < 8 && static_cast<int>(spots.size()) < count; ++row) {
        const Vec2 base = v.center + dir * (start + static_cast<f32>(row) * kSpotSpacing);
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
        const Vec2 base = v.center + dir * start;
        spots.push_back(Vec3{base.x, worldgen::height(base.x, base.y, seed), base.y});
    }
    return spots;
}

// Lays out a settlement deterministically: a central market (a well on the green in a hamlet), homes
// lining its streets, a stone palisade following its organic outline (with gates where roads cross
// it - none round a hamlet), raised cobblestone streets, fountains, wells, planters, bushes and street
// lanterns - plus, in a snowbound town, snow on every roof and wall-top, fire braziers along the
// streets and the odd snowman. Every object is placed against a shared occupancy list so nothing
// intersects. Returned as world-space PropInstances (the scatter filters them per chunk).
inline std::vector<PropInstance> village_props(const worldgen::Village& v, u32 seed) {
    std::vector<PropInstance> out;
    const f32 cx = v.center.x;
    const f32 cz = v.center.y;
    const f32 half = v.half;
    const int vid = static_cast<int>(v.vseed);
    const auto gates = detail::village_gate_points(v, seed);
    const bool market = worldgen::has_market(v);
    const bool walled = worldgen::has_wall(v);
    const bool city = v.tier == worldgen::TownTier::City;
    const bool snowy = v.snowy;

    auto push = [&](PropCategory cat, u8 var, f32 x, f32 z, f32 yaw, f32 length = 1.0f) {
        // A house stands on its own levelled pad, whose height is the natural ground at its centre.
        const f32 gh = cat == PropCategory::House ? worldgen::base_height(x, z, seed)
                                                  : worldgen::height(x, z, seed);
        if (gh < worldgen::water_level + 0.5f) {
            return; // never place a town prop down in the water
        }
        PropInstance p;
        p.category = cat;
        p.variant = var;
        p.position = Vec3{x, gh, z};
        p.yaw = yaw;
        p.scale = 1.0f;
        p.length = length; // stretch along local +X (walls span their chord exactly)
        out.push_back(p);
    };

    // Some towns have a river running through them (others are on dry land). Streets, decor and
    // greenery keep out of its channel; stone bridges carry the avenues across it.
    const detail::RiverInfo river = detail::town_river(v, seed);
    auto in_river = [&](f32 x, f32 z) {
        return river.present &&
               std::abs(detail::river_dist(river, v.center, Vec2{x, z})) < river.half_width + 0.4f;
    };
    // True when (x,z) sits on the ACTUAL (meandering) road that winds into the town - distinct from the
    // idealised town_streets, so this is what truly keeps decorative props off the cart's road, not
    // just off the straight street lines. Two margins: `on_road` is the road surface + shoulder (for
    // props that LINE the road, like lanterns - keep them beside it, not on it); `off_road` is a wider
    // clear that also leaves room for a prop's own footprint (for open-ground props - fountain, decor,
    // bushes, planters - so they sit clearly back from the road rather than overhanging its edge).
    auto on_road = [&](f32 x, f32 z) {
        return roads::distance(x, z, seed) < roads::road_half_width + 0.6f;
    };
    auto off_road = [&](f32 x, f32 z) {
        return roads::distance(x, z, seed) < roads::road_half_width + 1.5f;
    };
    // The idealised street network. Decorative props avoid both these lines AND the real meandering
    // road above. Shared by the fountain, lanterns and clutter below.
    const std::vector<detail::Street>& streets = detail::village_streets(v, seed);
    auto on_street = [&](f32 x, f32 z, f32 margin) {
        for (const detail::Street& s : streets) {
            if (detail::point_seg_dist(Vec2{x, z}, s.a, s.b) < margin) {
                return true;
            }
        }
        return false;
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

    // The heart of the place: a market in a village / town / city; a hamlet's green has just its well
    // (placed below) and maybe a single stall.
    if (market) {
        push(PropCategory::Market, snowy ? kSnowMarket : 0, cx, cz, 0.0f);
        occ.emplace_back(v.center, detail::plaza_half(v));
    }

    // Houses (+ optional bridge pair), collision-rejected against walls/market/avenues. A snowbound
    // town builds each one's snow-capped twin.
    const detail::TownPlan& plan = detail::cached_town_plan(v, seed);
    const std::vector<detail::HousePlot>& plots = plan.houses;
    for (const detail::HousePlot& h : plots) {
        push(PropCategory::House, static_cast<u8>(h.variant + (snowy ? kSnowHouses : 0u)), h.pos.x, h.pos.y, h.yaw);
        occ.emplace_back(h.pos, detail::house_reach(h.variant));
    }
    if (plan.bridge.present) {
        push(PropCategory::Bridge, 0, plan.bridge.center.x, plan.bridge.center.y, plan.bridge.yaw);
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

    // The fountain(s): a little garden in the mid-town green (between the ring road and the wall),
    // ringed with planters so it reads as a deliberate feature rather than a lone basin. The basin is
    // wide (~2.6 m), so its CENTRE must clear every street by the dirt band (~2.9 m) plus that whole
    // footprint, or it would overhang a street and snag the wagon. A city has several, one per quarter.
    const Vec2 gate_dir = gates.empty() ? Vec2{0.0f, 1.0f} : glm::normalize(gates[0].pos - v.center);
    const Vec2 gate_perp{-gate_dir.y, gate_dir.x};
    auto try_fountain = [&](const Vec2& fp) {
        if (occupied(fp, 2.6f) || in_river(fp.x, fp.y) || roads::distance(fp.x, fp.y, seed) <= roads::road_half_width + 3.0f ||
            on_street(fp.x, fp.y, 5.5f)) {
            return false;
        }
        push(PropCategory::Fountain, 0, fp.x, fp.y, 0.0f);
        occ.emplace_back(fp, 2.6f);
        for (int i = 0; i < 4; ++i) { // a ring of planters around the basin
            const f32 a = TwoPi * (static_cast<f32>(i) + 0.5f) / 4.0f;
            const Vec2 pp = fp + Vec2{std::cos(a), std::sin(a)} * 2.4f;
            if (!occupied(pp, 0.6f) && !off_road(pp.x, pp.y) && !on_street(pp.x, pp.y, 3.5f)) {
                push(snowy ? PropCategory::Decor : PropCategory::Planter, snowy ? u8{6} : static_cast<u8>(i % 3), pp.x,
                     pp.y, 0.0f);
                occ.emplace_back(pp, 0.6f);
            }
        }
        return true;
    };
    if (v.tier == worldgen::TownTier::Town || v.tier == worldgen::TownTier::Village) {
        for (f32 off : {17.0f, -17.0f, 19.5f, -19.5f}) {
            if (try_fountain(v.center + gate_perp * off)) {
                break;
            }
        }
    } else if (city) {
        for (int q = 0; q < 4; ++q) {
            const f32 a0 = std::atan2(gate_dir.y, gate_dir.x) + TwoPi * (static_cast<f32>(q) + 0.5f) / 4.0f;
            bool done = false;
            for (int k = 0; k < 18 && !done; ++k) {
                const f32 a = a0 + (static_cast<f32>(k % 6) - 2.5f) * 0.12f;
                const f32 rr = half * (0.3f + 0.08f * static_cast<f32>(k / 6));
                done = try_fountain(v.center + Vec2{std::cos(a), std::sin(a)} * rr);
            }
        }
    }

    // Wells - every settlement draws its water somewhere: a hamlet's sits on its green beside the lane,
    // a town's near the plaza, and a city has one in each quarter. Each site is the first candidate
    // clear of the market, the streets, the real road and every building (the fountain's rules).
    auto try_well = [&](const Vec2& wp) {
        // The well is ~1.4 m across, so its centre clears the street band (~2.9 m) by that footprint too.
        if (occupied(wp, 1.4f) || in_river(wp.x, wp.y) || roads::distance(wp.x, wp.y, seed) <= roads::road_half_width + 1.8f ||
            on_street(wp.x, wp.y, 4.3f)) {
            return false;
        }
        push(PropCategory::Well, 0, wp.x, wp.y, detail::hash01(detail::tree_hash(vid, static_cast<int>(wp.x), 7601u)) * TwoPi);
        occ.emplace_back(wp, 1.4f);
        return true;
    };
    {
        const int wells = city ? 4 : 1;
        const f32 a_base = detail::hash01(detail::tree_hash(vid, 3, 7600u)) * TwoPi;
        for (int w = 0; w < wells; ++w) {
            for (int i = 0; i < 16; ++i) {
                const f32 a = a_base + TwoPi * (static_cast<f32>(w) + static_cast<f32>(i % 8) / 8.0f) /
                                           static_cast<f32>(wells);
                const f32 rr = !market ? 4.5f + 1.5f * static_cast<f32>(i / 8)
                               : city  ? half * (0.38f + 0.12f * static_cast<f32>(i / 8))
                                       : 16.0f + 3.0f * static_cast<f32>(i / 8);
                if (try_well(v.center + Vec2{std::cos(a), std::sin(a)} * rr)) {
                    break;
                }
            }
        }
    }

    // Street lanterns line the high street + branches (lit dirt roads at night), plus a few by the
    // plaza. The visible dirt band reaches ~2.9 m from a street's centreline (town_path_amount's outer
    // smoothstep edge), so a post must clear EVERY street by more than that - not just its own: at
    // junctions a post lining one street used to land squarely on the crossing one. kLanternOff keeps a
    // strip of verge between band and post. A snowbound town alternates them with fire braziers, so
    // its streets glow warm against the snow.
    constexpr f32 kStreetClear = 3.4f; // min distance from any street centreline (band edge + verge)
    constexpr f32 kLanternOff = 3.9f;  // side offset from the lined street (> kStreetClear)
    int lamp_i = 0;
    for (const detail::Street& st : streets) {
        Vec2 dir = st.b - st.a;
        const f32 len = glm::length(dir);
        if (len < 7.0f) {
            continue;
        }
        dir /= len;
        const Vec2 nrm{-dir.y, dir.x};
        const int n = static_cast<int>(len / 8.5f);
        for (int k = 1; k <= n; ++k) {
            const f32 side = (k % 2 == 0) ? 1.0f : -1.0f;
            const Vec2 lp = st.a + dir * (static_cast<f32>(k) * 8.5f) + nrm * side * kLanternOff;
            if (glm::length(lp - v.center) < 9.0f || in_river(lp.x, lp.y) || on_road(lp.x, lp.y) ||
                on_street(lp.x, lp.y, kStreetClear) || occupied(lp, 0.5f)) {
                continue; // keep the plaza, river, the road + EVERY street clear (posts sit BESIDE them)
            }
            if (snowy && (lamp_i++ % 3) == 1) {
                push(PropCategory::Decor, kDecorBrazier, lp.x, lp.y, 0.0f);
            } else {
                push(PropCategory::Lantern, 0, lp.x, lp.y, 0.0f);
            }
            occ.emplace_back(lp, 0.5f);
        }
    }
    // A ring of lanterns around the plaza (braziers, in the snow). They must sit OUTSIDE the ring
    // road: the market's footprint meets the ring road's inner dirt edge, so there is no clear ground
    // between them.
    const f32 plaza_ring_r = detail::plaza_half(v) + (market ? 6.2f : 2.6f);
    for (int i = 0; i < (market ? 6 : 3); ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / (market ? 6.0f : 3.0f) + 0.4f;
        const Vec2 lp{cx + std::cos(a) * plaza_ring_r, cz + std::sin(a) * plaza_ring_r};
        if (occupied(lp, 0.6f) || in_river(lp.x, lp.y) || on_road(lp.x, lp.y) ||
            on_street(lp.x, lp.y, kStreetClear)) {
            continue; // a plaza-ring lantern must not land on a street/road or a building
        }
        push(snowy && i % 2 == 0 ? PropCategory::Decor : PropCategory::Lantern, snowy && i % 2 == 0 ? kDecorBrazier : u8{0},
             lp.x, lp.y, 0.0f);
        occ.emplace_back(lp, 0.6f);
    }

    // Streets are cobbled (mesh.frag lays procedural setts over town_pave_amount) on top of the
    // worn-earth street tint (town_path_tint), which shows through as the joints.

    // ---- Civic dressing: the touches that make a town feel lived in --------------------------
    // Heraldic BANNERS ring the plaza between its lanterns, strung together with pennant BUNTING;
    // a NOTICE BOARD of contracts stands at the plaza's edge; flower-sellers' CARTS and BENCHES line
    // the streets; FLOWER BARRELS sit by the doors; and every gate is flanked by a pair of banners.
    // Everything is checked against the same occupancy / street / road rules as the rest of the
    // town, so nothing lands in the wagon's way.
    const u8 town_banner = detail::hash01(detail::tree_hash(vid, 5, 9100u)) < 0.5f ? kDecorBannerRed : kDecorBannerBlue;
    if (market) {
        constexpr int kPlazaPoles = 6;
        const f32 pr = detail::kMarketHalf + 6.2f; // the plaza lantern ring (banners sit between lanterns)
        std::array<Vec2, kPlazaPoles> pole{};
        std::array<bool, kPlazaPoles> placed{};
        for (int i = 0; i < kPlazaPoles; ++i) {
            const f32 a = TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kPlazaPoles) + 0.4f;
            const Vec2 bp = v.center + Vec2{std::cos(a), std::sin(a)} * pr;
            if (occupied(bp, 0.6f) || in_river(bp.x, bp.y) || on_road(bp.x, bp.y) ||
                on_street(bp.x, bp.y, kStreetClear)) {
                continue;
            }
            const Vec2 face = glm::normalize(v.center - bp); // the banner faces into the plaza
            push(PropCategory::Decor, town_banner, bp.x, bp.y, std::atan2(face.x, face.y));
            occ.emplace_back(bp, 0.6f);
            pole[i] = bp;
            placed[i] = true;
        }
        // Bunting strung pole to pole round the plaza (overhead - it never blocks anything).
        for (int i = 0; i < kPlazaPoles; ++i) {
            const int j = (i + 1) % kPlazaPoles;
            if (!placed[i] || !placed[j]) {
                continue;
            }
            const Vec2 d = pole[j] - pole[i];
            const f32 len = glm::length(d);
            if (len < 3.0f || len > 22.0f) {
                continue;
            }
            const Vec2 mid = (pole[i] + pole[j]) * 0.5f;
            push(PropCategory::Decor, kDecorBunting, mid.x, mid.y, std::atan2(-d.y, d.x), len);
        }
    }
    // The notice board: at the plaza's edge, facing the market (where the contracts are posted) - on a
    // hamlet's green, beside the well.
    for (int i = 0; i < 48; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i % 12) / 12.0f + detail::hash01(detail::tree_hash(vid, 6, 9101u)) * TwoPi;
        const f32 rr = detail::plaza_half(v) + (market ? 7.6f : 1.6f) + 2.2f * static_cast<f32>(i / 12); // widen the search
        const Vec2 np = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(np, 1.1f) || in_river(np.x, np.y) || off_road(np.x, np.y) || on_street(np.x, np.y, 3.9f)) {
            continue;
        }
        const Vec2 face = glm::normalize(v.center - np);
        push(PropCategory::Decor, kDecorNoticeBoard, np.x, np.y, std::atan2(face.x, face.y));
        occ.emplace_back(np, 1.1f);
        break;
    }
    // A flower-seller's cart or two near the plaza (not in the snow - a woodpile instead); a hamlet's
    // green gets a single market stall.
    for (int i = 0, carts = 0; i < 24 && carts < (market ? 2 : 1); ++i) {
        const f32 a = TwoPi * detail::hash01(detail::tree_hash(vid, i, 9102u));
        const f32 rr = detail::plaza_half(v) + (market ? 7.0f : 2.0f) + 4.0f * detail::hash01(detail::tree_hash(vid, i, 9103u));
        const Vec2 cp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(cp, 1.3f) || in_river(cp.x, cp.y) || off_road(cp.x, cp.y) || on_street(cp.x, cp.y, 4.2f)) {
            continue;
        }
        const u8 what = !market ? u8{3} : snowy ? u8{6} : kDecorFlowerCart; // stall / woodpile / flower cart
        push(PropCategory::Decor, what, cp.x, cp.y, detail::hash01(detail::tree_hash(vid, i, 9104u)) * TwoPi);
        occ.emplace_back(cp, 1.3f);
        ++carts;
    }
    // Each gate is flanked (just inside the wall) by a pair of the town's banners.
    if (walled) {
        for (const detail::VillageGate& g : gates) {
            Vec2 radial = g.pos - v.center;
            radial = glm::length(radial) > 1e-3f ? glm::normalize(radial) : Vec2{0.0f, 1.0f};
            const Vec2 tangent{-radial.y, radial.x};
            for (const f32 side : {-1.0f, 1.0f}) {
                const Vec2 bp = g.pos - radial * 4.5f + tangent * (side * std::max(g.half + 0.9f, kStreetClear + 0.4f));
                if (occupied(bp, 0.6f) || in_river(bp.x, bp.y) || on_road(bp.x, bp.y) ||
                    on_street(bp.x, bp.y, kStreetClear)) {
                    continue;
                }
                push(PropCategory::Decor, town_banner, bp.x, bp.y, std::atan2(-radial.x, -radial.y)); // face into town
                occ.emplace_back(bp, 0.6f);
            }
        }
    }
    // Benches along the streets, set between the lanterns and facing the street.
    for (usize s = 0; s < streets.size(); ++s) {
        Vec2 dir = streets[s].b - streets[s].a;
        const f32 len = glm::length(dir);
        if (len < 9.0f) {
            continue;
        }
        dir /= len;
        const Vec2 nrm{-dir.y, dir.x};
        const int n = static_cast<int>(len / 8.5f);
        for (int k = 0; k < n; ++k) {
            if (detail::hash01(detail::tree_hash(vid, static_cast<int>(s) * 31 + k, 9105u)) > (city ? 0.3f : 0.55f)) {
                continue; // not every gap gets one
            }
            const f32 side = (k % 2 == 0) ? -1.0f : 1.0f; // opposite the lantern of this stretch
            const Vec2 bp = streets[s].a + dir * ((static_cast<f32>(k) + 0.5f) * 8.5f) + nrm * side * 4.3f;
            if (glm::length(bp - v.center) < detail::plaza_half(v) + 3.0f || occupied(bp, 1.0f) ||
                in_river(bp.x, bp.y) || off_road(bp.x, bp.y) || on_street(bp.x, bp.y, 3.8f)) {
                continue;
            }
            const Vec2 face = nrm * (-side); // the seat looks out over the street
            push(PropCategory::Decor, kDecorBench, bp.x, bp.y, std::atan2(face.x, face.y));
            occ.emplace_back(bp, 1.0f);
        }
    }

    // Greenery: planters ringing the plaza + bushes scattered on open interior ground (in the snow:
    // stacked firewood, and the townsfolk's snowmen). Like the plaza lanterns, the planter ring sits
    // OUTSIDE the ring road.
    if (market) {
        for (int i = 0; i < 5; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / 5.0f + 1.1f;
            const Vec2 pp = v.center + Vec2{std::cos(a), std::sin(a)} * 15.2f;
            if (!occupied(pp, 0.7f) && !in_river(pp.x, pp.y) && !off_road(pp.x, pp.y) &&
                !on_street(pp.x, pp.y, 3.6f)) {
                if (snowy) {
                    push(PropCategory::Decor, i % 2 == 0 ? kDecorSnowman : u8{6}, pp.x, pp.y, a + Pi);
                } else {
                    push(PropCategory::Planter, 0, pp.x, pp.y, 0.0f);
                }
                occ.emplace_back(pp, 0.7f);
            }
        }
    }
    // Scatter bushes + planters to green up the open ground, biased toward the bare band between the
    // outer house ring and the wall so the town doesn't read as empty there.
    const int green_n = static_cast<int>(half * 2.4f);
    for (int i = 0; i < green_n; ++i) {
        const f32 a = detail::hash01(detail::tree_hash(vid, i, 7700u)) * TwoPi;
        const f32 rr = (0.45f + 0.5f * detail::hash01(detail::tree_hash(vid, i, 7701u))) * half;
        if (rr > worldgen::town_radius(v, a, seed) - 1.6f) {
            continue; // stay inside the wall
        }
        const Vec2 bp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        // Clear every street by the dirt band + this prop's ~1 m footprint (off_road only covers the
        // meandering cart road - scattered pots ON the town streets were what the wagon kept snagging on).
        if (occupied(bp, 1.0f) || in_river(bp.x, bp.y) || off_road(bp.x, bp.y) ||
            on_street(bp.x, bp.y, 3.9f)) {
            continue;
        }
        const f32 pick = detail::hash01(detail::tree_hash(vid, i, 7702u));
        if (snowy) {
            if (pick < 0.12f) {
                push(PropCategory::Decor, kDecorSnowman, bp.x, bp.y, detail::hash01(detail::tree_hash(vid, i, 7704u)) * TwoPi);
            } else if (pick < 0.4f) {
                push(PropCategory::Decor, 6, bp.x, bp.y, detail::hash01(detail::tree_hash(vid, i, 7704u)) * TwoPi); // woodpile
            } else {
                continue; // the rest of the commons lies under clean snow
            }
        } else {
            push(pick < 0.22f ? PropCategory::Planter : PropCategory::Bush,
                 static_cast<u8>(detail::tree_hash(vid, i, 7703u) % 3u), bp.x, bp.y,
                 detail::hash01(detail::tree_hash(vid, i, 7704u)) * TwoPi);
        }
        occ.emplace_back(bp, 1.0f);
    }

    // Medieval clutter to fill the town out: stored goods (barrels/crates/woodpiles/sacks)
    // tucked against house fronts, plus stalls, hay, signposts and troughs scattered along the
    // streets and plaza. Everything avoids the occupancy list and the streets, so nothing blocks a
    // building or the cart's route.
    // True when a prop of footprint radius `foot` at `p` would encroach on a street: its centre must
    // clear every centreline by the visible dirt band (~2.9 m) plus that footprint, so no clutter edge
    // pokes onto the road the wagon drives.
    auto on_avenue = [&](const Vec2& p, f32 foot) {
        for (const detail::Street& s : streets) {
            if (detail::point_seg_dist(p, s.a, s.b) < 2.9f + foot) {
                return true; // keep clutter off the streets / cart route
            }
        }
        return false;
    };
    for (usize hi = 0; hi < plots.size(); ++hi) {
        if (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8100u)) > 0.5f) {
            continue; // only ~half the houses get a goods pile out front
        }
        const detail::HousePlot& h = plots[hi];
        if (h.variant >= kHouseChapel) {
            continue; // the chapel + keep keep their fronts clear
        }
        const Vec2 front{std::sin(h.yaw), std::cos(h.yaw)}; // the way the house faces (toward the street)
        const Vec2 along{front.y, -front.x};                // along the street, beside the house front
        const f32 soff = (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8101u)) - 0.5f) * 1.2f;
        const Vec2 dp = h.pos + along * (detail::house_reach(h.variant) * 0.82f + 0.5f) + front * soff;
        if (occupied(dp, 0.8f) || on_avenue(dp, 0.8f) || in_river(dp.x, dp.y) || off_road(dp.x, dp.y)) {
            continue;
        }
        // Barrel, crates, woodpile, sacks, hay - a snowbound house stacks firewood for the winter.
        const u8 stored[5] = {0, 1, 6, 7, 2};
        const u8 what = snowy && detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8104u)) < 0.6f
                            ? u8{6}
                            : stored[detail::tree_hash(vid, static_cast<int>(hi), 8102u) % 5u];
        push(PropCategory::Decor, what, dp.x, dp.y, detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8103u)) * TwoPi);
        occ.emplace_back(dp, 0.8f);
    }

    // Flower barrels by ~40% of the front doors (the goods piles took the other side of the door) -
    // nothing blooms in the snow.
    for (usize hi = 0; hi < plots.size() && !snowy; ++hi) {
        if (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8150u)) > 0.4f) {
            continue;
        }
        const detail::HousePlot& h = plots[hi];
        if (h.variant >= kHouseChapel) {
            continue;
        }
        const Vec2 front{std::sin(h.yaw), std::cos(h.yaw)};
        const Vec2 along{front.y, -front.x};
        const Vec2 e = PropLibrary::house_half_extents(h.variant);
        const Vec2 fp = h.pos + front * (e.y + 0.6f) - along * (e.x * 0.62f);
        if (occupied(fp, 0.45f) || on_avenue(fp, 0.45f) || in_river(fp.x, fp.y) || off_road(fp.x, fp.y)) {
            continue;
        }
        push(PropCategory::Decor, kDecorFlowerBarrel, fp.x, fp.y,
             detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8151u)) * TwoPi);
        occ.emplace_back(fp, 0.45f);
    }

    // Fenced cottage gardens: a little post-and-rail plot beside ~a third of the houses (on the
    // opposite side from the goods pile), with planters + a bush growing inside - the tended
    // garden patches that sell the "lived-in storybook village" look (a hamlet fences more of them;
    // a snowbound garden lies fallow under the drifts). The rail on the house side is left out as the
    // garden's entrance. Corners are all checked against the occupancy list, streets, the real road and
    // the wall, so a garden that doesn't fit is simply skipped.
    const f32 garden_odds = snowy ? 0.0f : !walled ? 0.6f : 0.38f;
    for (usize hi = 0; hi < plots.size(); ++hi) {
        if (detail::hash01(detail::tree_hash(vid, static_cast<int>(hi), 8300u)) > garden_odds) {
            continue;
        }
        const detail::HousePlot& h = plots[hi];
        if (h.variant >= kHouseChapel) {
            continue;
        }
        const Vec2 front{std::sin(h.yaw), std::cos(h.yaw)}; // the way the house faces (the street)
        const Vec2 along{front.y, -front.x};                // along the street
        constexpr f32 gu = 1.7f; // garden half-extent along the street
        constexpr f32 gw = 1.3f; // garden half-extent toward/away from the street
        const Vec2 gc = h.pos - along * (detail::house_reach(h.variant) + gu + 0.6f);
        const Vec2 corners[4] = {gc + along * gu + front * gw, gc + along * gu - front * gw,
                                 gc - along * gu - front * gw, gc - along * gu + front * gw};
        bool fits = !occupied(gc, gu + 0.9f) && !in_river(gc.x, gc.y);
        for (const Vec2& c : corners) {
            const Vec2 d = c - v.center;
            fits = fits && !on_avenue(c, 0.5f) && !off_road(c.x, c.y) &&
                   glm::length(d) < worldgen::town_radius(v, std::atan2(d.y, d.x), seed) - 1.4f;
        }
        if (!fits) {
            continue;
        }
        const u32 gh = detail::tree_hash(vid, static_cast<int>(hi), 8301u);
        for (int c = 0; c < 4; ++c) {
            push(PropCategory::Fence, static_cast<u8>(gh % 2u), corners[c].x, corners[c].y,
                 std::atan2(-along.y, along.x));
            if (c == 3) {
                continue; // the house-side rail (corner 3 -> 0) stays open as the entrance
            }
            const Vec2 rd = corners[c + 1] - corners[c];
            const f32 gap = glm::length(rd);
            const Vec2 mid = (corners[c] + corners[c + 1]) * 0.5f;
            push(PropCategory::FenceRail, static_cast<u8>(gh % 2u), mid.x, mid.y, std::atan2(-rd.y, rd.x), gap);
        }
        for (int pi = 0; pi < 2; ++pi) { // the planted rows inside
            const Vec2 pp = gc + along * (pi == 0 ? -0.8f : 0.8f);
            push(PropCategory::Planter, static_cast<u8>(detail::tree_hash(vid, static_cast<int>(hi) * 2 + pi, 8302u) % 3u),
                 pp.x, pp.y, 0.0f);
        }
        push(PropCategory::Bush, static_cast<u8>(gh % 3u), gc.x + front.x * 0.7f, gc.y + front.y * 0.7f,
             detail::hash01(gh) * TwoPi);
        occ.emplace_back(gc, gu + 0.9f);
    }
    const int decor_n = static_cast<int>(half * 2.2f);
    for (int i = 0; i < decor_n; ++i) {
        const f32 a = detail::hash01(detail::tree_hash(vid, i, 8200u)) * TwoPi;
        const f32 rr = (0.3f + 0.64f * detail::hash01(detail::tree_hash(vid, i, 8201u))) * half;
        if (rr > worldgen::town_radius(v, a, seed) - 1.8f) {
            continue; // stay inside the wall
        }
        const Vec2 dp = v.center + Vec2{std::cos(a), std::sin(a)} * rr;
        if (occupied(dp, 1.1f) || on_avenue(dp, 1.1f) || in_river(dp.x, dp.y) || off_road(dp.x, dp.y)) {
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
        push(PropCategory::Decor, var, dp.x, dp.y, detail::hash01(detail::tree_hash(vid, i, 8205u)) * TwoPi);
        occ.emplace_back(dp, 1.1f);
    }

    // Palisade following the organic outline (none round a hamlet - it just thins out into the
    // fields): walls marched around the boundary (skipping gate gaps), towers flanking each gate, and
    // periodic towers around the wall.
    if (!walled) {
        return out;
    }
    constexpr f32 seg = 3.2f;
    const int N = glm::clamp(static_cast<int>(std::round(TwoPi * half / seg)), 16, 480);
    const int tower_every = std::max(3, N / (city ? 18 : 10));
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
    const u8 wall_snow = snowy ? kSnowWalls : u8{0};
    const u8 gate_snow = snowy ? kSnowGates : u8{0};
    for (int i = 0; i < N; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(N);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(N);
        const f32 amid = TwoPi * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(N);
        const Vec2 p0 = detail::town_boundary(v, a0, seed);
        const Vec2 p1 = detail::town_boundary(v, a1, seed);
        if (!near_gate(amid, 0.0f)) {
            const Vec2 mid = (p0 + p1) * 0.5f;
            const Vec2 chord = p1 - p0;
            // Stretch the (unit-3.2m) wall to span its actual chord, with a slight overlap, so the
            // rampart is continuous on the organic boundary instead of gapping on bulges / doubling up
            // on tight curves (the wall mesh is half_len 1.6 -> 3.2 long).
            const f32 clen = glm::length(chord);
            push(PropCategory::Wall, static_cast<u8>(i % 2 + wall_snow), mid.x, mid.y, std::atan2(-chord.y, chord.x),
                 (clen / 3.2f) * 1.08f);
        }
        // Periodic boundary tower - kept a little further from the gate so it never stands in
        // the opening (the flanking gate towers below mark the gate itself).
        if (i % tower_every == 0 && !near_gate(a0, 0.05f)) {
            push(PropCategory::Gate, static_cast<u8>(1 + gate_snow), p0.x, p0.y, 0.0f); // plain (unlit) tower
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
            push(PropCategory::Gate, gate_snow, tp.x, tp.y, yaw);
        }
    }
    return out;
}

// village_props, cached per town (keyed like village_gate_points). The terrain streamer asks for a
// town's layout once for every chunk it builds near it, and laying out a whole town each time made
// towns slow to stream in. Thread-safe (chunks build on a worker thread); entries are never evicted,
// so the returned reference stays valid.
inline const std::vector<PropInstance>& cached_village_props(const worldgen::Village& v, u32 seed) {
    static std::mutex mtx;
    static std::unordered_map<u64, std::shared_ptr<const std::vector<PropInstance>>> cache;
    const u64 key = (static_cast<u64>(v.vseed) << 32) | static_cast<u64>(seed);
    {
        std::lock_guard<std::mutex> lock(mtx);
        const auto it = cache.find(key);
        if (it != cache.end()) {
            return *it->second;
        }
    }
    auto props = std::make_shared<const std::vector<PropInstance>>(village_props(v, seed));
    std::lock_guard<std::mutex> lock(mtx);
    return *cache.emplace(key, std::move(props)).first->second;
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

// Distance from (x,z) to the nearest of the settlement's own streets.
inline f32 town_street_distance(const worldgen::Village& v, const Vec2& q, u32 seed) {
    f32 best = 1e30f;
    for (const detail::Street& s : detail::village_streets(v, seed)) {
        best = std::min(best, detail::point_seg_dist(q, s.a, s.b));
    }
    return best;
}

// Intra-town dirt streets: the worn-dirt streets the houses line (the cached town_streets network).
// Returns the on-street amount (0..1) at (x,z), used to colour the town ground as a wide earth road.
// Cheap per vertex (allocation-free) and only does real work inside a town.
inline f32 town_path_amount(const Vec3& p, f32 up, u32 seed) {
    if (p.y < worldgen::water_level + 0.5f) {
        return 0.0f;
    }
    const auto v = worldgen::village_containing(p.x, p.z, seed);
    if (!v) {
        return 0.0f;
    }
    const f32 best = town_street_distance(*v, Vec2{p.x, p.z}, seed);
    constexpr f32 hw = 2.2f; // wide dirt road half-width (matches road_half in for_each_house)
    const f32 band = glm::smoothstep(hw + 0.7f, hw - 0.5f, best);
    const f32 gentle = glm::smoothstep(0.55f, 0.8f, up);
    return band * gentle;
}

// COBBLED ground in and around a town (0..1): the market plaza, the street network and the inter-town
// road where it runs through the town - fading out a few metres past the walls, so the cobbles spill
// out of each gate and peter out into the dirt road. The terrain mesher stores this per vertex
// (mc::PaveFn) and mesh.frag lays procedural cobblestones over it, on top of the packed-earth street
// tint below (which shows through as the joints between the stones). A hamlet's lane stays earth.
inline f32 town_pave_amount(const Vec3& p, f32 up, u32 seed) {
    if (p.y < worldgen::water_level + 0.4f) {
        return 0.0f;
    }
    const f32 gentle = glm::smoothstep(0.55f, 0.8f, up); // flat-ish ground only (no cobbled cliffs)
    if (gentle <= 0.0f) {
        return 0.0f;
    }
    const auto v = worldgen::village_containing(p.x, p.z, seed, 10.0f);
    if (!v || !worldgen::has_market(*v)) {
        return 0.0f;
    }
    const Vec2 q{p.x, p.z};
    const Vec2 d = q - v->center;
    const f32 dist = glm::length(d);
    const f32 r = worldgen::town_radius(*v, std::atan2(d.y, d.x), seed);
    const f32 town = glm::smoothstep(r + 9.0f, r - 1.0f, dist); // runs out past the gates
    if (town <= 0.0f) {
        return 0.0f;
    }
    // The market plaza: a broad cobbled square under the stalls.
    f32 pave = glm::smoothstep(detail::kMarketHalf + 2.0f, detail::kMarketHalf, dist);
    // The street network (the same lines the houses face + the dirt tint follows).
    if (dist < r + 1.0f) {
        constexpr f32 hw = 2.2f; // the street half-width (matches town_path_amount)
        pave = std::max(pave, glm::smoothstep(hw + 0.5f, hw - 0.5f, town_street_distance(*v, q, seed)));
    }
    // The real inter-town road, through the town and out of its gates.
    const f32 rd = roads::distance(p.x, p.z, seed);
    pave = std::max(pave, glm::smoothstep(roads::road_half_width + 0.45f, roads::road_half_width - 0.55f, rd));
    return pave * town * gentle;
}

// Overlays the town's dirt streets onto a base terrain colour (applied while meshing, like
// roads::tint_surface, to avoid pulling the village layout into surface_color). Warm packed earth -
// trodden slush in a snowbound town.
inline Vec3 town_path_tint(Vec3 color, const Vec3& p, f32 up, u32 seed) {
    const f32 on = town_path_amount(p, up, seed);
    if (on > 0.0f) {
        const auto v = worldgen::village_containing(p.x, p.z, seed);
        const bool snowy = v && v->snowy;
        color = glm::mix(color, snowy ? Vec3{0.44f, 0.42f, 0.44f} : Vec3{0.46f, 0.34f, 0.22f}, on * 0.88f);
    }
    return color;
}

} // namespace alryn
