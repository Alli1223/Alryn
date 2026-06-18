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
// Connect towns whose village cells are within this Chebyshev distance (~340 m).
inline constexpr int road_max_cells = 2;
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
std::vector<Vec2> route_polyline(const Vec2& a, const Vec2& b, u32 seed);

// Direction from a town toward its nearest connected town (where its single gate
// faces), or nullopt if the town has no road (isolated by water).
std::optional<Vec2> primary_direction(const worldgen::Village& v, u32 seed);

} // namespace alryn::roads
