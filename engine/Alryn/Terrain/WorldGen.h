#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Noise.h>
#include <Alryn/Core/Types.h>

// Deterministic world description shared by the server and every client. Terrain
// is a biome-blended height field; clients and the server sample the same
// functions, so only runtime edits travel over the network.
namespace alryn::worldgen {

inline constexpr f32 voxel_size = 0.5f;
inline constexpr f32 water_level = -1.0f;

// Smooth surface height at (x, z): blends ocean / plains / mountains from a
// low-frequency "region" field, with finer detail layered on land.
inline f32 height(f32 x, f32 z, u32 seed) {
    const f32 region = noise::fbm2d(x * 0.006f, z * 0.006f, 3, 2.0f, 0.5f, seed + 101u);
    const f32 continental = glm::smoothstep(-0.30f, 0.10f, region); // 0 ocean .. 1 land
    const f32 mountainous = glm::smoothstep(0.30f, 0.85f, region);  // 0 .. 1 mountains

    const f32 base = glm::mix(-8.0f, 1.5f, continental) + mountainous * 6.0f;
    const f32 amp = glm::mix(0.6f, 8.0f, mountainous) * continental + 0.4f;
    const f32 detail = noise::fbm2d(x * 0.035f, z * 0.035f, 5, 2.0f, 0.5f, seed) +
                       0.25f * noise::fbm2d(x * 0.13f, z * 0.13f, 3, 2.0f, 0.5f, seed + 9u);
    return base + amp * detail;
}

inline f32 density(const Vec3& p, u32 seed) {
    return p.y - height(p.x, p.z, seed);
}

// Wetness field: drives grass (wet) vs desert sand (dry).
inline f32 moisture(f32 x, f32 z, u32 seed) {
    return noise::fbm2d(x * 0.018f, z * 0.018f, 3, 2.0f, 0.5f, seed + 202u);
}

// Surface colour from height, slope, and moisture -> beaches, grass/desert,
// rocky slopes, and snow peaks.
inline Vec3 surface_color(const Vec3& p, const Vec3& normal, u32 seed) {
    const f32 up = glm::clamp(normal.y, 0.0f, 1.0f);
    const f32 m = moisture(p.x, p.z, seed);
    const f32 h = p.y;

    const Vec3 grass{0.30f, 0.50f, 0.26f};
    const Vec3 sand{0.78f, 0.71f, 0.47f};
    const Vec3 wet_sand{0.42f, 0.40f, 0.32f};
    const Vec3 rock{0.42f, 0.42f, 0.46f};
    const Vec3 snow{0.92f, 0.93f, 0.97f};

    // Flat ground: desert sand <-> grass by moisture.
    Vec3 flat = glm::mix(sand, grass, glm::smoothstep(-0.15f, 0.2f, m));
    // Steep slopes turn rocky.
    Vec3 color = glm::mix(rock, flat, glm::smoothstep(0.5f, 0.75f, up));

    // Sandy beach band just above the water on gentle ground.
    const f32 beach = glm::smoothstep(water_level + 2.2f, water_level + 0.3f, h) *
                      glm::smoothstep(0.6f, 0.85f, up);
    color = glm::mix(color, sand, beach);
    // Darker silt below the waterline.
    color = glm::mix(color, wet_sand, glm::smoothstep(water_level + 0.2f, water_level - 2.5f, h));
    // Snow on high, flatter ground.
    const f32 snow_amt = glm::smoothstep(8.5f, 12.5f, h) * glm::smoothstep(0.5f, 0.8f, up);
    color = glm::mix(color, snow, snow_amt);

    // Gentle height shading.
    return color * (0.85f + 0.15f * glm::clamp(h * 0.04f + 0.5f, 0.0f, 1.0f));
}

} // namespace alryn::worldgen
