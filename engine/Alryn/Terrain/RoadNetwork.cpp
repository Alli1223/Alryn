#include <Alryn/Terrain/RoadNetwork.h>

#include <Alryn/Core/Noise.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <queue>
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

// A shallow RIVER channel (a winding river carved just below the waterline), as opposed to deep
// ocean/lake. Roads cross rivers (a bridge spans them) rather than detouring around them, so the
// road's water-avoidance ignores these. The threshold is LOW so the whole carved band counts -
// including its shallow edges, which dip just below the waterline but aren't the river's deep middle
// (otherwise a road would be dropped at the river's banks).
bool crossable_river(const Vec2& p, u32 seed) {
    return worldgen::river_amount(p.x, p.y, seed) > 0.04f &&
           worldgen::height(p.x, p.y, seed) > worldgen::water_level - 2.5f;
}

// Cost of routing a road through (x,z): a strong penalty for being at/under water, so the refinement
// bends the road onto dry land - but RIVERS are crossed (bridged), not avoided.
f32 water_cost(const Vec2& p, u32 seed) {
    if (crossable_river(p, seed)) {
        return 0.0f;
    }
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
        // A river is crossable (bridged), so it doesn't count as water the road must avoid/drop.
        return !crossable_river(p, seed) &&
               worldgen::height(p.x, p.y, seed) < worldgen::water_level + 0.4f;
    };

    const f32 max_off = std::min(span * 0.6f, 120.0f); // bend further to snake around water bodies
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

// ---- Routed-polyline cache (canonical cell pair) --------------------------
// route() is the expensive bit (iterative water-avoidance + meander). Cache it per unordered town
// pair so desired_links / owned_segments / town_links don't recompute the same road.
struct EdgeKey {
    u32 seed;
    int acx, acz, bcx, bcz;
    bool operator==(const EdgeKey& o) const {
        return seed == o.seed && acx == o.acx && acz == o.acz && bcx == o.bcx && bcz == o.bcz;
    }
};
struct EdgeKeyHash {
    usize operator()(const EdgeKey& k) const {
        usize h = std::hash<u32>{}(k.seed);
        for (int v : {k.acx, k.acz, k.bcx, k.bcz}) {
            h ^= std::hash<int>{}(v) + 0x9E3779B9u + (h << 6) + (h >> 2);
        }
        return h;
    }
};
std::mutex g_route_mutex;
std::unordered_map<EdgeKey, std::shared_ptr<const std::vector<Vec2>>, EdgeKeyHash> g_route;

bool cell_less(int acx, int acz, int bcx, int bcz) {
    return acz != bcz ? acz < bcz : acx < bcx;
}

// The routed polyline between the towns in two cells (canonical order; empty if either has no town
// or they can't be linked on land). Cached.
std::shared_ptr<const std::vector<Vec2>> routed(u32 seed, int acx, int acz, int bcx, int bcz) {
    if (!cell_less(acx, acz, bcx, bcz)) {
        std::swap(acx, bcx);
        std::swap(acz, bcz);
    }
    const EdgeKey key{seed, acx, acz, bcx, bcz};
    {
        std::lock_guard<std::mutex> lock(g_route_mutex);
        const auto it = g_route.find(key);
        if (it != g_route.end()) {
            return it->second;
        }
    }
    auto poly = std::make_shared<std::vector<Vec2>>();
    const auto a = town_at(acx, acz, seed);
    const auto b = town_at(bcx, bcz, seed);
    if (a && b) {
        *poly = route(a->center, b->center, seed);
    }
    std::lock_guard<std::mutex> lock(g_route_mutex);
    return g_route.emplace(key, std::move(poly)).first->second;
}

// ---- Desired links: each town wants its nearest few routable neighbours ----
std::mutex g_desired_mutex;
std::unordered_map<CellKey, std::shared_ptr<const std::vector<CellKey>>, CellKeyHash> g_desired;

std::shared_ptr<const std::vector<CellKey>> desired_links(u32 seed, int cx, int cz) {
    const CellKey key{seed, cx, cz};
    {
        std::lock_guard<std::mutex> lock(g_desired_mutex);
        const auto it = g_desired.find(key);
        if (it != g_desired.end()) {
            return it->second;
        }
    }
    auto out = std::make_shared<std::vector<CellKey>>();
    if (const auto a = town_at(cx, cz, seed)) {
        // All neighbouring towns within reach, by distance (cheap - no routing yet).
        std::vector<std::pair<f32, CellKey>> cand;
        for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
            for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                if (const auto b = town_at(cx + dx, cz + dz, seed)) {
                    cand.push_back({glm::length(b->center - a->center), CellKey{seed, cx + dx, cz + dz}});
                }
            }
        }
        std::sort(cand.begin(), cand.end(), [](const auto& l, const auto& r) {
            if (l.first != r.first) {
                return l.first < r.first;
            }
            return l.second.cz != r.second.cz ? l.second.cz < r.second.cz : l.second.cx < r.second.cx;
        });
        // Route the nearest candidates (only) in order until we have road_links_per_town that are
        // reachable on land - so we pay route() for just a handful per town. Capped attempts bound
        // the cost: if the nearest few aren't routable (a coastal town), we don't route the whole
        // neighbourhood. (With the land-dominated terrain the nearest are almost always routable.)
        constexpr int kMaxRouteAttempts = 8;
        int attempts = 0;
        for (const auto& [d, cell] : cand) {
            if (static_cast<int>(out->size()) >= road_links_per_town || attempts >= kMaxRouteAttempts) {
                break;
            }
            ++attempts;
            if (!routed(seed, cx, cz, cell.cx, cell.cz)->empty()) {
                out->push_back(cell);
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_desired_mutex);
    return g_desired.emplace(key, std::move(out)).first->second;
}

// An edge between two town cells is built iff EITHER endpoint wants it (it's among that town's
// nearest few). This guarantees every town links to its nearest neighbours - no town is stranded.
bool edge_wanted(u32 seed, int acx, int acz, int bcx, int bcz) {
    for (const CellKey& k : *desired_links(seed, acx, acz)) {
        if (k.cx == bcx && k.cz == bcz) {
            return true;
        }
    }
    for (const CellKey& k : *desired_links(seed, bcx, bcz)) {
        if (k.cx == acx && k.cz == acz) {
            return true;
        }
    }
    return false;
}

// Segments of every road owned by the town (if any) in village cell (cx,cz): the wanted edges to
// nearer-ordered cells (the precedes owner builds each edge exactly once).
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
    if (town_at(cx, cz, seed)) {
        for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
            for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
                const int bcx = cx + dx, bcz = cz + dz;
                if ((dx == 0 && dz == 0) || !precedes(cx, cz, bcx, bcz)) {
                    continue; // self, or the other town owns this edge
                }
                if (town_at(bcx, bcz, seed) && edge_wanted(seed, cx, cz, bcx, bcz)) {
                    polyline_to_segments(*routed(seed, cx, cz, bcx, bcz), *segs);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_owned_mutex);
    return g_owned.emplace(key, std::move(segs)).first->second;
}

// ---- Bridges (road-over-river crossings) ----------------------------------
std::mutex g_bridges_mutex;
std::unordered_map<CellKey, std::shared_ptr<const std::vector<Bridge>>, CellKeyHash> g_bridges;

// Append a Bridge for each contiguous span where the road polyline crosses a river - sampled
// finely along the polyline (river bands are narrower than the road-point spacing).
void poly_bridges(const std::vector<Vec2>& poly, u32 seed, std::vector<Bridge>& out) {
    if (poly.size() < 2) {
        return;
    }
    constexpr f32 step = 1.5f;
    constexpr f32 thresh = 0.5f; // the river channel proper (not the shallow banks)
    bool inriver = false;
    Vec2 enter{0.0f};
    Vec2 enter_dir{1.0f, 0.0f};
    for (usize i = 0; i + 1 < poly.size(); ++i) {
        const Vec2 a = poly[i], b = poly[i + 1];
        const f32 segl = glm::length(b - a);
        if (segl < 1e-3f) {
            continue;
        }
        const Vec2 dir = (b - a) / segl;
        for (f32 t = 0.0f; t < segl; t += step) {
            const Vec2 p = a + dir * t;
            const bool r = worldgen::river_amount(p.x, p.y, seed) > thresh;
            if (r && !inriver) {
                inriver = true;
                enter = p;
                enter_dir = dir;
            } else if (!r && inriver) {
                inriver = false;
                const f32 len = glm::length(p - enter);
                if (len > 0.5f) {
                    out.push_back(Bridge{(enter + p) * 0.5f, std::atan2(enter_dir.y, enter_dir.x),
                                         len + 3.5f});
                }
            }
        }
    }
}

// Bridges on the roads owned by the town in cell (cx,cz). Cached (like owned_segments).
std::shared_ptr<const std::vector<Bridge>> owned_bridges(u32 seed, int cx, int cz) {
    const CellKey key{seed, cx, cz};
    {
        std::lock_guard<std::mutex> lock(g_bridges_mutex);
        const auto it = g_bridges.find(key);
        if (it != g_bridges.end()) {
            return it->second;
        }
    }
    auto out = std::make_shared<std::vector<Bridge>>();
    if (town_at(cx, cz, seed)) {
        for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
            for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
                const int bcx = cx + dx, bcz = cz + dz;
                if ((dx == 0 && dz == 0) || !precedes(cx, cz, bcx, bcz)) {
                    continue;
                }
                if (town_at(bcx, bcz, seed) && edge_wanted(seed, cx, cz, bcx, bcz)) {
                    poly_bridges(*routed(seed, cx, cz, bcx, bcz), seed, *out);
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_bridges_mutex);
    return g_bridges.emplace(key, std::move(out)).first->second;
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

// ---- Town graph (for multi-hop routing through intermediate towns) --------

// Neighbour town cells of (cx,cz) over the road graph, cached. An edge is a real built road (the
// `edge_wanted` rule - either town wants the other among its nearest few), so the graph + the drawn
// roads agree exactly, and the graph is connected (every town reaches its nearest neighbours).
std::mutex g_links_mutex;
std::unordered_map<CellKey, std::shared_ptr<const std::vector<CellKey>>, CellKeyHash> g_links;

std::shared_ptr<const std::vector<CellKey>> town_links(u32 seed, int cx, int cz) {
    const CellKey key{seed, cx, cz};
    {
        std::lock_guard<std::mutex> lock(g_links_mutex);
        const auto it = g_links.find(key);
        if (it != g_links.end()) {
            return it->second;
        }
    }
    auto links = std::make_shared<std::vector<CellKey>>();
    if (town_at(cx, cz, seed)) {
        for (int dz = -road_max_cells; dz <= road_max_cells; ++dz) {
            for (int dx = -road_max_cells; dx <= road_max_cells; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                if (town_at(cx + dx, cz + dz, seed) && edge_wanted(seed, cx, cz, cx + dx, cz + dz)) {
                    links->push_back(CellKey{seed, cx + dx, cz + dz});
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_links_mutex);
    return g_links.emplace(key, std::move(links)).first->second;
}

// The town cell whose centre is (very near) world point `c`, if it's a town centre.
std::optional<CellKey> owning_cell(u32 seed, const Vec2& c) {
    const int bx = static_cast<int>(std::floor(c.x / worldgen::village_cell));
    const int bz = static_cast<int>(std::floor(c.y / worldgen::village_cell));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (const auto t = town_at(bx + dx, bz + dz, seed)) {
                if (glm::length(t->center - c) < 2.5f) {
                    return CellKey{seed, bx + dx, bz + dz};
                }
            }
        }
    }
    return std::nullopt;
}

Vec2 cell_center(u32 seed, const CellKey& k) {
    const auto t = town_at(k.cx, k.cz, seed);
    return t ? t->center : Vec2{0.0f};
}

// Dijkstra over the town graph from `src` toward `dst`. If `dst` is set, stops when it's settled
// and returns the cell path; otherwise explores the whole reachable component and returns every
// settled cell (with its graph distance in `out_dist`) for reachable_towns. Bounded for safety.
std::vector<CellKey> dijkstra(u32 seed, const CellKey& src, const std::optional<CellKey>& dst,
                              std::vector<std::pair<CellKey, f32>>* settled_out) {
    std::unordered_map<CellKey, f32, CellKeyHash> dist;
    std::unordered_map<CellKey, CellKey, CellKeyHash> prev;
    using QE = std::pair<f32, CellKey>;
    const auto cmp = [](const QE& a, const QE& b) { return a.first > b.first; };
    std::priority_queue<QE, std::vector<QE>, decltype(cmp)> pq(cmp);
    dist[src] = 0.0f;
    pq.push({0.0f, src});
    int settled = 0;
    while (!pq.empty()) {
        const auto [d, c] = pq.top();
        pq.pop();
        if (d > dist[c]) {
            continue; // stale queue entry
        }
        if (settled_out) {
            settled_out->push_back({c, d});
        }
        if (dst && c == *dst) {
            break;
        }
        if (++settled > 800) {
            break; // hard bound (towns are sparse; this is never hit in practice)
        }
        const Vec2 cc = cell_center(seed, c);
        for (const CellKey& n : *town_links(seed, c.cx, c.cz)) {
            const f32 nd = d + glm::length(cell_center(seed, n) - cc);
            const auto it = dist.find(n);
            if (it == dist.end() || nd < it->second) {
                dist[n] = nd;
                prev[n] = c;
                pq.push({nd, n});
            }
        }
    }
    if (!dst || !dist.count(*dst)) {
        return {};
    }
    std::vector<CellKey> path;
    for (CellKey c = *dst;;) {
        path.push_back(c);
        if (c == src) {
            break;
        }
        const auto it = prev.find(c);
        if (it == prev.end()) {
            return {};
        }
        c = it->second;
    }
    std::reverse(path.begin(), path.end());
    return path;
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

std::vector<Bridge> bridges(const Vec2& center, f32 radius, u32 seed) {
    const f32 cell = worldgen::village_cell;
    const int lo_x = static_cast<int>(std::floor((center.x - radius) / cell)) - road_max_cells;
    const int hi_x = static_cast<int>(std::floor((center.x + radius) / cell)) + road_max_cells;
    const int lo_z = static_cast<int>(std::floor((center.y - radius) / cell)) - road_max_cells;
    const int hi_z = static_cast<int>(std::floor((center.y + radius) / cell)) + road_max_cells;
    std::vector<Bridge> out;
    for (int cz = lo_z; cz <= hi_z; ++cz) {
        for (int cx = lo_x; cx <= hi_x; ++cx) {
            const auto owned = owned_bridges(seed, cx, cz);
            out.insert(out.end(), owned->begin(), owned->end());
        }
    }
    return out;
}

std::vector<Vec2> route_through_towns(const Vec2& a, const Vec2& b, u32 seed) {
    const auto sa = owning_cell(seed, a);
    const auto sb = owning_cell(seed, b);
    if (!sa || !sb) {
        return route(a, b, seed); // not town centres: a plain direct route
    }
    if (*sa == *sb) {
        return {};
    }
    const std::vector<CellKey> cells = dijkstra(seed, *sa, sb, nullptr);
    if (cells.size() < 2) {
        return {}; // unreachable on the road graph
    }
    // Chain the per-edge roads, dropping the duplicated shared endpoint between legs.
    std::vector<Vec2> out;
    for (usize i = 0; i + 1 < cells.size(); ++i) {
        const std::vector<Vec2> seg =
            route(cell_center(seed, cells[i]), cell_center(seed, cells[i + 1]), seed);
        if (seg.empty()) {
            return {};
        }
        for (usize k = (out.empty() ? 0 : 1); k < seg.size(); ++k) {
            out.push_back(seg[k]);
        }
    }
    return out;
}

std::vector<Vec2> route_polyline(const Vec2& a, const Vec2& b, u32 seed) {
    return route_through_towns(a, b, seed);
}

f32 route_length(const std::vector<Vec2>& route) {
    f32 len = 0.0f;
    for (usize i = 1; i < route.size(); ++i) {
        len += glm::length(route[i] - route[i - 1]);
    }
    return len;
}

namespace {
// How tough it is to haul a cart across a biome, 0 (easy) .. 1 (hard). Mountains/snow are steep +
// exposed, bog is sticky + ambush-friendly, desert is open + hot; lowland forest/plains are easy.
f32 biome_hazard(worldgen::Biome b) {
    switch (b) {
        case worldgen::Biome::Mountains: return 1.0f;
        case worldgen::Biome::Snow: return 1.0f;
        case worldgen::Biome::Bog: return 0.8f;
        case worldgen::Biome::Desert: return 0.7f;
        case worldgen::Biome::Beach: return 0.2f;
        default: return 0.0f; // Forest / Plains / Ocean(roads don't cross water)
    }
}
} // namespace

f32 route_hazard(const std::vector<Vec2>& route, u32 seed) {
    if (route.size() < 2) {
        return 0.0f;
    }
    f32 sum = 0.0f;
    for (const Vec2& p : route) {
        sum += biome_hazard(worldgen::biome_at(p.x, p.y, seed));
    }
    return sum / static_cast<f32>(route.size());
}

u8 route_difficulty(const std::vector<Vec2>& route, u32 seed) {
    const f32 h = route_hazard(route, seed);
    return h < 0.18f ? 1u : (h < 0.45f ? 2u : 3u);
}

std::vector<worldgen::Village> reachable_towns(const Vec2& center, u32 seed, int max_results) {
    const auto src = owning_cell(seed, center);
    if (!src) {
        return {};
    }
    std::vector<std::pair<CellKey, f32>> settled;
    dijkstra(seed, *src, std::nullopt, &settled);
    // Nearest graph-distance first; break ties on cell so the order is deterministic.
    std::sort(settled.begin(), settled.end(), [](const auto& l, const auto& r) {
        if (l.second != r.second) {
            return l.second < r.second;
        }
        return l.first.cz != r.first.cz ? l.first.cz < r.first.cz : l.first.cx < r.first.cx;
    });
    std::vector<worldgen::Village> out;
    for (const auto& [cell, d] : settled) {
        if (cell == *src) {
            continue; // skip the origin town itself
        }
        if (const auto v = town_at(cell.cx, cell.cz, seed)) {
            out.push_back(*v);
            if (static_cast<int>(out.size()) >= max_results) {
                break;
            }
        }
    }
    return out;
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
