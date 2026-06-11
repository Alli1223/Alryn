#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/TreeScatter.h> // detail::tree_hash / hash01
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Prop.h>

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

    // Bushes: gentle, above-water, non-desert ground.
    place(3.0f, seed + 6100u, PropCategory::Bush, 3, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.8f || gh > 9.0f) return false;
        if (worldgen::moisture(x, z, seed) < -0.05f) return false;
        if (detail::ground_slope(x, z, seed) > 2.0f) return false;
        return detail::hash01(h) < 0.4f;
    });

    // Rocks: sparse, tolerate slopes, above water.
    place(6.0f, seed + 6200u, PropCategory::Rock, 3, [&](f32 /*x*/, f32 /*z*/, f32 gh, u32 h) {
        if (gh < worldgen::water_level + 0.4f) return false;
        return detail::hash01(h) < 0.30f;
    });

    // Houses: occasional, on a reasonably flat, above-water clearing (villages
    // cluster in flatter terrain; steep hills stay empty).
    place(chunk_world, seed + 6300u, PropCategory::House, 4, [&](f32 x, f32 z, f32 gh, u32 h) {
        if (detail::hash01(h) > 0.22f) return false;
        if (gh < worldgen::water_level + 1.2f || gh > 7.5f) return false;
        // Need fairly flat ground over the footprint (villages favour flat land).
        for (f32 ox : {-2.5f, 2.5f}) {
            for (f32 oz : {-2.5f, 2.5f}) {
                if (std::abs(worldgen::height(x + ox, z + oz, seed) - gh) > 1.4f) return false;
            }
        }
        return true;
    });

    return out;
}

} // namespace alryn
