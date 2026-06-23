#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/WorldGen.h>

#include <optional>
#include <vector>

// Deterministic road network connecting nearby towns. Replaces the old noise-contour
// "paths": for each town (see worldgen::village_at) we draw a road to nearby towns,
// routed to avoid water. The whole thing is a pure function of the world seed, so the
// server, the streaming client and the map all agree without any networking.
//
// Roads are queried per marching-tetra vertex while meshing terrain (on the streaming
// worker thread), so the heavy routing is computed once per village cell and cached
// (thread-safe). `distance`/`tangent`/`amount`/`tint_surface` are the hot queries;
// `gather` feeds the in-game map.
namespace alryn::roads {

// Half-width of the dirt road (fences/lanterns line its edges, like the old paths).
inline constexpr f32 road_half_width = 2.2f;
// How far (in village cells, Chebyshev) a town looks for neighbours to link by road (~850 m). Each
// town connects to its NEAREST few routable neighbours (road_links_per_town), so no town is left
// stranded even on sparse, mountainous, river-cut terrain - and the graph stays well-connected for
// long multi-hop hauls.
inline constexpr int road_max_cells = 3;
// Each town wants a road to its nearest this-many routable towns (an edge is built if EITHER side
// wants it, so towns reliably get one and usually two-or-more connections).
inline constexpr int road_links_per_town = 3;
// Polyline resolution of a routed road (segments = road_points). Higher now that roads
// meander, so the curves render + collide smoothly.
inline constexpr int road_points = 28;

// A straight piece of a road centreline in world xz.
struct Segment {
    Vec2 a{0.0f};
    Vec2 b{0.0f};
};

// Distance (world units) from (x,z) to the nearest road centreline. Huge if no road
// is near. Replaces worldgen::path_distance.
f32 distance(f32 x, f32 z, u32 seed);

// Unit direction ALONG the nearest road at (x,z). Replaces worldgen::path_tangent.
Vec2 tangent(f32 x, f32 z, u32 seed);

// 1 on the road (on gentle, above-water ground), fading to 0 at the edges. Replaces
// worldgen::path_amount.
f32 amount(const Vec3& p, f32 up, u32 seed);

// Overlays the worn dirt/gravel road colour onto a base terrain colour. Applied while
// meshing terrain (kept out of worldgen::surface_color to avoid an include cycle).
Vec3 tint_surface(Vec3 color, const Vec3& p, f32 up, u32 seed);

// All road segments within `radius` of `center` (world xz) - for drawing the map.
std::vector<Segment> gather(const Vec2& center, f32 radius, u32 seed);

// The ordered, water-avoiding road polyline between two town centres (a -> b), or empty
// if they can't be linked on land. For the wagon driver's path + the map route overlay.
// This now follows the TOWN GRAPH: for far towns it chains the per-edge roads through any
// intermediate towns on the way (a multi-hop haul), so the wagon passes through them.
std::vector<Vec2> route_polyline(const Vec2& a, const Vec2& b, u32 seed);

// The multi-hop road polyline from town centre `a` to town centre `b`, routed over the town
// graph so it passes THROUGH intermediate towns that lie on the way. Empty if `b`'s town isn't
// reachable from `a`'s on the road graph. (If a/b aren't town centres it falls back to a direct
// route.) `route_polyline` delegates to this.
std::vector<Vec2> route_through_towns(const Vec2& a, const Vec2& b, u32 seed);

// Total world-length of a road polyline (sum of segment lengths) - the true haul distance
// (longer than the straight line for a winding multi-hop route), for reward scaling.
f32 route_length(const std::vector<Vec2>& route);

// How hazardous the BIOMES a route crosses are, 0 (easy lowland forest/plains) .. 1 (a hard slog
// over mountain passes / through bogs + deserts). The mean per-point biome hazard along the
// polyline - so a haul that threads tough country reads as more dangerous.
f32 route_hazard(const std::vector<Vec2>& route, u32 seed);

// The route's difficulty TIER (1 easy .. 3 hard) from its biome hazard - drives the ambush size on
// a contract and the danger shown on the map.
u8 route_difficulty(const std::vector<Vec2>& route, u32 seed);

// Every town reachable from `center`'s town over the road graph (including far, multi-hop ones),
// nearest graph-distance first, capped at `max_results`. Lets the game offer long-haul contracts
// to distant towns, not just immediate neighbours.
std::vector<worldgen::Village> reachable_towns(const Vec2& center, u32 seed, int max_results = 16);

// Direction from a town toward its nearest connected town (where its single gate
// faces), or nullopt if the town has no road (isolated by water).
std::optional<Vec2> primary_direction(const worldgen::Village& v, u32 seed);

} // namespace alryn::roads
