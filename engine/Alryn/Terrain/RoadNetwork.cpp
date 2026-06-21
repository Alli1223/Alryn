#include <Alryn/Terrain/RoadNetwork.h>

#include <Alryn/Core/Noise.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace alryn::roads {
namespace {

// ---- Town-graph helpers ---------------------------------------------------

// A town candidate at a village cell, if the ground there grows one.
std::optional<worldgen::Village> town_at(int cx, int cz, u32 seed) {
    return worldgen::village_at(cx, cz, seed);
}

// Deterministic, order-independent owner: the edge between two towns is built by the
// town whose cell comes first in (cz, cx) order, so each road is computed exactly once.
bool precedes(int acx, int acz, int bcx, int bcz) {
    return acz != bcz ? acz < bcz : acx < bcx;
}

// Cost of routing a road through (x,z): a strong penalty for being at/under water, so
// the refinement bends the road onto dry land.
f32 water_cost(const Vec2& p, u32 seed) {
    const f32 h = worldgen::height(p.x, p.y, seed);
    const f32 below = (worldgen::water_level + 1.2f) - h; // >0 when too low/wet
    if (below <= 0.0f) {
        return 0.0f;
    }
    return below * below;
}

// Order-independent hash of an edge (so route(A,B) and route(B,A) meander identically).
u32 edge_hash(const Vec2& a, const Vec2& b, u32 seed) {
    const Vec2& lo = (a.x != b.x ? a.x < b.x : a.y < b.y) ? a : b;
    const Vec2& hi = (a.x != b.x ? a.x < b.x : a.y < b.y) ? b : a;
    auto q = [](f32 v) { return static_cast<u32>(static_cast<i32>(std::lround(v * 0.25f))); };
    u32 h = seed * 2654435761u;
    h ^= q(lo.x) * 73856093u;
    h ^= q(lo.y) * 19349663u;
    h ^= q(hi.x) * 83492791u;
    h ^= q(hi.y) * 50331653u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
f32 hash01u(u32 h) { return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu); }

// Routes a road from town centre A to town centre B as a polyline. The road first bends
// away from water (greedy gradient descent perpendicular to the line), then gets a
// deterministic, winding meander (a couple of sine sweeps) added on so trails curve and
// wander rather than running straight. Any meandered point that lands in water snaps back
// to the validated dry path, so the meander never costs us a road. Returns empty if the
// road can't be kept on land. The geometry is canonicalised on the endpoints, so A->B and
// B->A produce the same curve (just reversed) - keeping the wagon's path on the drawn road.
std::vector<Vec2> route_impl(const Vec2& pa, const Vec2& pb, u32 seed) {
    const Vec2 along = pb - pa;
    const f32 span = glm::length(along);
    if (span < 1e-3f) {
        return {};
    }
    const Vec2 perp = Vec2{-along.y, along.x} / span;

    std::vector<f32> off(road_points + 1, 0.0f); // lateral offset along `perp` per point
    auto point_at = [&](int i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(road_points);
        return pa + along * t + perp * off[i];
    };
    auto in_water = [&](const Vec2& p) {
        return worldgen::height(p.x, p.y, seed) < worldgen::water_level + 0.4f;
    };

    const f32 max_off = std::min(span * 0.5f, 60.0f);
    constexpr f32 eps = 1.5f;
    constexpr f32 step = 8.0f;
    for (int it = 0; it < 24; ++it) {
        for (int i = 1; i < road_points; ++i) {
            const Vec2 base = pa + along * (static_cast<f32>(i) / static_cast<f32>(road_points));
            const Vec2 here = base + perp * off[i];
            const f32 c0 = water_cost(here - perp * eps, seed);
            const f32 c1 = water_cost(here + perp * eps, seed);
            off[i] -= step * (c1 - c0) / (2.0f * eps);
            off[i] = glm::clamp(off[i], -max_off, max_off);
        }
        std::vector<f32> sm = off; // smooth so the road curves rather than zig-zags
        for (int i = 1; i < road_points; ++i) {
            sm[i] = 0.5f * off[i] + 0.25f * (off[i - 1] + off[i + 1]);
        }
        off.swap(sm);
    }
    const std::vector<f32> dry_off = off; // the validated water-avoiding path

    // Add a deterministic winding meander on top (zero at the town ends).
    const u32 eh = edge_hash(pa, pb, seed);
    const f32 mamp = std::min(span * 0.22f, 30.0f);
    const int waves1 = 1 + static_cast<int>(eh % 3u);          // 1..3 broad sweeps
    const int waves2 = 3 + static_cast<int>((eh >> 4) % 4u);   // 3..6 finer wiggles
    const f32 ph1 = hash01u(eh ^ 0x9E3779B9u) * TwoPi;
    const f32 ph2 = hash01u(eh ^ 0x12345679u) * TwoPi;
    constexpr f32 a2 = 0.35f;
    for (int i = 1; i < road_points; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(road_points);
        const f32 env = std::sin(Pi * t);
        const f32 wig = std::sin(t * static_cast<f32>(waves1) * Pi + ph1) +
                        a2 * std::sin(t * static_cast<f32>(waves2) * Pi + ph2);
        off[i] += env * mamp * wig / (1.0f + a2);
        off[i] = glm::clamp(off[i], -max_off, max_off);
    }
    // Nudge meandered points back off water (gradient only; no smoothing, to keep curves).
    for (int it = 0; it < 8; ++it) {
        for (int i = 1; i < road_points; ++i) {
            const Vec2 base = pa + along * (static_cast<f32>(i) / static_cast<f32>(road_points));
            const Vec2 here = base + perp * off[i];
            const f32 c0 = water_cost(here - perp * eps, seed);
            const f32 c1 = water_cost(here + perp * eps, seed);
            off[i] -= step * (c1 - c0) / (2.0f * eps);
            off[i] = glm::clamp(off[i], -max_off, max_off);
        }
    }
    // Smooth the offset profile so the road flows in gentle curves rather than sharp kinks
    // (the meander + water-avoidance descent can leave angular corners). A low-pass on the
    // lateral offset, town ends pinned at 0, keeps the shape but rounds the bends.
    for (int pass = 0; pass < 5; ++pass) {
        std::vector<f32> sm = off;
        for (int i = 1; i < road_points; ++i) {
            sm[i] = 0.5f * off[i] + 0.25f * (off[i - 1] + off[i + 1]);
        }
        off.swap(sm);
    }

    // Any point now in water snaps back to the validated dry path, so smoothing/meander can
    // never drown a road; if the underlying dry route itself crosses water, drop the edge.
    for (int i = 1; i < road_points; ++i) {
        if (in_water(point_at(i))) {
            off[i] = dry_off[i];
        }
    }
    for (int i = 0; i <= road_points; ++i) {
        if (in_water(point_at(i))) {
            return {};
        }
    }

    std::vector<Vec2> pts(road_points + 1);
    for (int i = 0; i <= road_points; ++i) {
        pts[i] = point_at(i);
    }
    return pts;
}

// Canonicalises the endpoint order so the curve is identical regardless of direction
// (the wagon route + the rendered road must agree), returning it oriented pa -> pb.
std::vector<Vec2> route(const Vec2& pa, const Vec2& pb, u32 seed) {
    const bool canonical = pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y;
    if (canonical) {
        return route_impl(pa, pb, seed);
    }
    std::vector<Vec2> r = route_impl(pb, pa, seed);
    std::reverse(r.begin(), r.end());
    return r;
}

void polyline_to_segments(const std::vector<Vec2>& pts, std::vector<Segment>& out) {
    for (usize i = 1; i < pts.size(); ++i) {
        out.push_back(Segment{pts[i - 1], pts[i]});
    }
}

// ---- Per-cell cache -------------------------------------------------------

struct CellKey {
    u32 seed;
    int cx;
    int cz;
    bool operator==(const CellKey& o) const { return seed == o.seed && cx == o.cx && cz == o.cz; }
};
struct CellKeyHash {
    usize operator()(const CellKey& k) const {
        usize h = std::hash<u32>{}(k.seed);
        h ^= std::hash<int>{}(k.cx) + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.cz) + 0x9E3779B9u + (h << 6) + (h >> 2);
        return h;
    }
};

using SegList = std::shared_ptr<const std::vector<Segment>>;

std::mutex g_owned_mutex;
std::unordered_map<CellKey, SegList, CellKeyHash> g_owned; // edges owned by a town in this cell
std::mutex g_window_mutex;
std::unordered_map<CellKey, SegList, CellKeyHash> g_window; // merged segs near a query cell

// Segments of every road owned by the town (if any) in village cell (cx,cz).
SegList owned_segments(u32 seed, int cx, int cz) {
    const CellKey key{seed, cx, cz};
    {
        std::lock_guard<std::mutex> lock(g_owned_mutex);
        const auto it = g_owned.find(key);
        if (it != g_owned.end()) {
            return it->second;
        }
    }

    auto segs = std::make_shared<std::vector<Segment>>();
    if (const auto a = town_at(cx, cz, seed)) {
        for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
            for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const int bcx = cx + dx;
                const int bcz = cz + dz;
                if (!precedes(cx, cz, bcx, bcz)) {
                    continue; // the other town owns this edge
                }
                if (const auto b = town_at(bcx, bcz, seed)) {
                    polyline_to_segments(route(a->center, b->center, seed), *segs);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_owned_mutex);
    return g_owned.emplace(key, std::move(segs)).first->second;
}

// All road segments near village cell (qcx,qcz): the union of roads owned by towns in
// the surrounding window (a road can reach up to road_max_cells away from its owner).
SegList window_segments(u32 seed, int qcx, int qcz) {
    const CellKey key{seed, qcx, qcz};
    {
        std::lock_guard<std::mutex> lock(g_window_mutex);
        const auto it = g_window.find(key);
        if (it != g_window.end()) {
            return it->second;
        }
    }

    auto merged = std::make_shared<std::vector<Segment>>();
    for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
        for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
            const SegList owned = owned_segments(seed, qcx + dx, qcz + dz);
            merged->insert(merged->end(), owned->begin(), owned->end());
        }
    }

    std::lock_guard<std::mutex> lock(g_window_mutex);
    return g_window.emplace(key, std::move(merged)).first->second;
}

f32 point_segment_distance(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    const f32 t = len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return glm::length(p - (a + ab * t));
}

// Nearest road segment to (x,z), with its distance. Returns false if no road is near.
bool nearest(f32 x, f32 z, u32 seed, Segment& out, f32& out_dist) {
    const int qcx = static_cast<int>(std::floor(x / worldgen::village_cell));
    const int qcz = static_cast<int>(std::floor(z / worldgen::village_cell));
    const SegList segs = window_segments(seed, qcx, qcz);
    const Vec2 p{x, z};
    bool found = false;
    out_dist = 1e30f;
    for (const Segment& s : *segs) {
        const f32 d = point_segment_distance(p, s.a, s.b);
        if (d < out_dist) {
            out_dist = d;
            out = s;
            found = true;
        }
    }
    return found;
}

} // namespace

f32 distance(f32 x, f32 z, u32 seed) {
    Segment s;
    f32 d;
    return nearest(x, z, seed, s, d) ? d : 1e30f;
}

Vec2 tangent(f32 x, f32 z, u32 seed) {
    Segment s;
    f32 d;
    if (!nearest(x, z, seed, s, d)) {
        return Vec2{1.0f, 0.0f};
    }
    const Vec2 dir = s.b - s.a;
    const f32 len = glm::length(dir);
    return len > 1e-6f ? dir / len : Vec2{1.0f, 0.0f};
}

f32 amount(const Vec3& p, f32 up, u32 seed) {
    if (p.y < worldgen::water_level + 0.5f) {
        return 0.0f;
    }
    const f32 d = distance(p.x, p.z, seed);
    const f32 band = glm::smoothstep(road_half_width + 0.5f, road_half_width - 0.7f, d);
    const f32 gentle = glm::smoothstep(0.55f, 0.8f, up); // only on flattish ground
    return band * gentle;
}

Vec3 tint_surface(Vec3 color, const Vec3& p, f32 up, u32 seed) {
    const f32 on = amount(p, up, seed);
    if (on > 0.0f) {
        const f32 speckle = noise::fbm2d(p.x * 0.9f, p.z * 0.9f, 1, 2.0f, 0.5f, seed + 717u);
        const Vec3 road_col = glm::mix(Vec3{0.40f, 0.32f, 0.23f}, Vec3{0.52f, 0.46f, 0.38f},
                                       glm::smoothstep(0.1f, 0.4f, speckle));
        color = glm::mix(color, road_col, on);
    }
    return color;
}

std::vector<Segment> gather(const Vec2& center, f32 radius, u32 seed) {
    const f32 cell = worldgen::village_cell;
    const int lo_x = static_cast<int>(std::floor((center.x - radius) / cell)) - road_max_cells;
    const int hi_x = static_cast<int>(std::floor((center.x + radius) / cell)) + road_max_cells;
    const int lo_z = static_cast<int>(std::floor((center.y - radius) / cell)) - road_max_cells;
    const int hi_z = static_cast<int>(std::floor((center.y + radius) / cell)) + road_max_cells;
    std::vector<Segment> out;
    for (int cz = lo_z; cz <= hi_z; ++cz) {
        for (int cx = lo_x; cx <= hi_x; ++cx) {
            const SegList owned = owned_segments(seed, cx, cz);
            out.insert(out.end(), owned->begin(), owned->end());
        }
    }
    return out;
}

std::vector<Vec2> route_polyline(const Vec2& a, const Vec2& b, u32 seed) {
    return route(a, b, seed);
}

std::optional<Vec2> primary_direction(const worldgen::Village& v, u32 seed) {
    // Reuse the cached road segments: find a road whose endpoint sits at this town and
    // return the direction it leaves in (so the gate faces the actual road). The roads
    // already route around water, so an isolated town simply has no incident segment.
    const std::vector<Segment> segs = gather(v.center, worldgen::village_cell * road_max_cells, seed);
    constexpr f32 tol = 1.5f;
    for (const Segment& s : segs) {
        if (glm::length(s.a - v.center) < tol) {
            const Vec2 d = s.b - s.a;
            if (glm::length(d) > 1e-3f) {
                return glm::normalize(d);
            }
        }
        if (glm::length(s.b - v.center) < tol) {
            const Vec2 d = s.a - s.b;
            if (glm::length(d) > 1e-3f) {
                return glm::normalize(d);
            }
        }
    }
    return std::nullopt;
}

} // namespace alryn::roads
