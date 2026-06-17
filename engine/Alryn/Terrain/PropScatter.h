#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h> // detail::tree_hash / hash01
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Prop.h>
#include <Alryn/World/Village.h>

#include <cmath>
#include <vector>

namespace alryn {

namespace detail {

// Local terrain slope at (x,z): how much the ground tilts over ~1 unit.
inline f32 ground_slope(f32 x, f32 z, u32 seed) {
    const f32 g = worldgen::height(x, z, seed);
    return std::abs(worldgen::height(x + 1.0f, z, seed) - g) +
           std::abs(worldgen::height(x, z + 1.0f, seed) - g);
}

} // namespace detail

// Deterministically places discrete props (bushes, rocks, rare houses) over the
// chunk. Bushes like gentle green ground, rocks tolerate slopes, houses need a
// flat, dry-ish, above-water clearing. Cell grids are global so chunks tile.
inline std::vector<PropInstance> scatter_props(int cx, int cz, f32 chunk_world, u32 seed) {
    std::vector<PropInstance> out;
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;

    auto place = [&](f32 cell, u32 salt, PropCategory cat, int variants, auto accept) {
        const int gx0 = static_cast<int>(std::floor(x0 / cell));
        const int gz0 = static_cast<int>(std::floor(z0 / cell));
        const int n = std::max(1, static_cast<int>(chunk_world / cell));
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int gx = gx0 + i;
                const int gz = gz0 + j;
                const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, salt + 1u)) - 0.5f) * cell * 0.7f;
                const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, salt + 2u)) - 0.5f) * cell * 0.7f;
                const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
                const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;
                const f32 gh = worldgen::height(wx, wz, seed);
                const u32 h = detail::tree_hash(gx, gz, salt + 3u);
                if (!accept(wx, wz, gh, h)) {
                    continue;
                }
                PropInstance p;
                p.category = cat;
                p.variant = static_cast<u8>(detail::tree_hash(gx, gz, salt + 4u) % static_cast<u32>(variants));
                p.position = Vec3{wx, gh, wz};
                p.yaw = detail::hash01(detail::tree_hash(gx, gz, salt + 5u)) * TwoPi;
                p.scale = 1.0f;
                out.push_back(p);
            }
        }
    };

    // Keep loose forest props off the roads and out of the towns.
    auto off_path = [&](f32 x, f32 z) {
        return roads::distance(x, z, seed) > roads::road_half_width + 0.8f &&
               !worldgen::inside_village(x, z, seed, 2.0f);
    };

    // Bushes: dense forest undergrowth on gentle, above-water, non-desert ground.
    place(2.2f, seed + 6100u, PropCategory::Bush, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.8f || gh > 9.0f) return false;
        const f32 moist = worldgen::moisture(x, z, seed);
        if (moist < -0.05f) return false;
        if (detail::ground_slope(x, z, seed) > 2.2f || !off_path(x, z)) return false;
        return detail::hash01(h) < 0.30f + glm::clamp(moist, 0.0f, 0.5f) * 0.5f; // lusher when wet
    });

    // Rocks / mossy boulders: tolerate slopes, above water.
    place(5.0f, seed + 6200u, PropCategory::Rock, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.4f || !off_path(x, z)) return false;
        return detail::hash01(h) < 0.32f;
    });

    // Fallen logs: occasional forest-floor debris (you bump into them) on flattish,
    // moist, wooded ground.
    place(5.5f, seed + 6400u, PropCategory::Log, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 1.0f || !off_path(x, z)) return false;
        if (worldgen::moisture(x, z, seed) < 0.0f) return false;
        if (detail::ground_slope(x, z, seed) > 1.6f) return false;
        return detail::hash01(h) < 0.18f;
    });

    // Standalone lanterns dotted across the world - glowing waypoints in the dark
    // forest that pool warm light on the ground and cast shadows at night. A coarse
    // grid on fairly flat, dry ground, kept off the paths (which have their own) and
    // out of the towns.
    place(23.0f, seed + 6800u, PropCategory::Lantern, 1, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.9f) return false;
        if (detail::ground_slope(x, z, seed) > 1.3f) return false;
        if (!off_path(x, z)) return false;
        return detail::hash01(h) < 0.5f;
    });

    // Fences + lanterns lining the path edges. A fine grid; accept cells that fall
    // in the narrow band at the trail's edge, oriented along the path tangent so
    // segments line up. Lanterns replace a fence post here and there.
    {
        constexpr f32 cell = 1.3f;
        const u32 salt = seed + 6700u;
        const int gx0 = static_cast<int>(std::floor(x0 / cell));
        const int gz0 = static_cast<int>(std::floor(z0 / cell));
        const int n = std::max(1, static_cast<int>(chunk_world / cell));
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int gx = gx0 + i;
                const int gz = gz0 + j;
                const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, salt + 1u)) - 0.5f) * cell * 0.3f;
                const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, salt + 2u)) - 0.5f) * cell * 0.3f;
                const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
                const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;
                const f32 gh = worldgen::height(wx, wz, seed);
                if (gh < worldgen::water_level + 0.6f) continue;
                const f32 pd = roads::distance(wx, wz, seed);
                if (pd < roads::road_half_width - 0.05f || pd > roads::road_half_width + 0.55f) {
                    continue; // only the road's edge
                }
                if (detail::ground_slope(wx, wz, seed) > 1.5f) continue; // skip steep road bits
                if (worldgen::inside_village(wx, wz, seed, 2.0f)) continue; // towns have their own walls
                const Vec2 tan = roads::tangent(wx, wz, seed);
                PropInstance p;
                p.position = Vec3{wx, gh, wz};
                p.yaw = std::atan2(-tan.y, tan.x); // align local +X with the path tangent
                p.scale = 1.0f;
                if (detail::hash01(detail::tree_hash(gx, gz, salt + 3u)) < 0.085f) {
                    p.category = PropCategory::Lantern;
                    p.variant = 0;
                } else {
                    p.category = PropCategory::Fence;
                    p.variant = static_cast<u8>(detail::tree_hash(gx, gz, salt + 4u) % 2u);
                }
                out.push_back(p);
            }
        }
    }

    // Village structures (cottages, walls, gate towers) whose pieces fall in this
    // chunk. Towns are large but sparse, so checking the 3x3 village cells around
    // the chunk and filtering to its bounds is cheap (and the chunk result caches).
    {
        const int vcx = static_cast<int>(std::floor((x0 + chunk_world * 0.5f) / worldgen::village_cell));
        const int vcz = static_cast<int>(std::floor((z0 + chunk_world * 0.5f) / worldgen::village_cell));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto village = worldgen::village_at(vcx + dx, vcz + dz, seed);
                if (!village) {
                    continue;
                }
                for (const PropInstance& p : village_props(*village, seed)) {
                    if (p.position.x >= x0 && p.position.x < x0 + chunk_world &&
                        p.position.z >= z0 && p.position.z < z0 + chunk_world) {
                        out.push_back(p);
                    }
                }
            }
        }
    }

    return out;
}

} // namespace alryn
