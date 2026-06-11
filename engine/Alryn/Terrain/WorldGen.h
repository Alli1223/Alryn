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

// Smooth surface height at (x, z): blends ocean / lowland / mountains from a
// low-frequency "region" field, then layers rolling hills + finer detail on land
// for a wild, undulating forested landscape.
inline f32 height(f32 x, f32 z, u32 seed) {
    const f32 region = noise::fbm2d(x * 0.006f, z * 0.006f, 3, 2.0f, 0.5f, seed + 101u);
    const f32 continental = glm::smoothstep(-0.30f, 0.10f, region); // 0 ocean .. 1 land
    const f32 mountainous = glm::smoothstep(0.35f, 0.90f, region);  // 0 .. 1 mountains

    const f32 base = glm::mix(-8.0f, 1.0f, continental) + mountainous * 7.0f;

    // Big rolling hills over all land (gentle, walkable, immersive undulation).
    const f32 hills = noise::fbm2d(x * 0.017f, z * 0.017f, 4, 2.0f, 0.5f, seed + 7u);
    // Mid-frequency terrain shape (bumps, dells) + fine surface roughness.
    const f32 detail = noise::fbm2d(x * 0.045f, z * 0.045f, 5, 2.0f, 0.5f, seed);
    const f32 fine = noise::fbm2d(x * 0.14f, z * 0.14f, 3, 2.0f, 0.5f, seed + 9u);

    const f32 land_amp = glm::mix(2.2f, 9.0f, mountainous) * continental + 0.4f;
    return base + continental * hills * 2.6f + land_amp * (detail * 0.7f + fine * 0.22f);
}

inline f32 density(const Vec3& p, u32 seed) {
    return p.y - height(p.x, p.z, seed);
}

// Wetness field: drives lush forest (wet) vs open meadow / dry sand (dry). Biased
// positive so most of the land reads as green woodland, with drier clearings.
inline f32 moisture(f32 x, f32 z, u32 seed) {
    return noise::fbm2d(x * 0.018f, z * 0.018f, 3, 2.0f, 0.5f, seed + 202u) + 0.14f;
}

// Surface colour from height, slope, and moisture -> beaches, grass/desert,
// rocky slopes, and snow peaks.
inline Vec3 surface_color(const Vec3& p, const Vec3& normal, u32 seed) {
    const f32 up = glm::clamp(normal.y, 0.0f, 1.0f);
    const f32 m = moisture(p.x, p.z, seed);
    const f32 h = p.y;

    const Vec3 grass{0.22f, 0.40f, 0.19f};  // deep forest green
    const Vec3 grass2{0.31f, 0.47f, 0.23f}; // lighter clearing green
    const Vec3 dirt{0.33f, 0.26f, 0.17f};   // leaf litter / bare soil
    const Vec3 sand{0.78f, 0.71f, 0.47f};
    const Vec3 wet_sand{0.42f, 0.40f, 0.32f};
    const Vec3 rock{0.42f, 0.42f, 0.46f};
    const Vec3 snow{0.92f, 0.93f, 0.97f};

    // Forest floor: blend two greens by a mid-frequency field, with patches of
    // bare earth / leaf-litter, so the ground reads varied rather than flat green.
    const f32 tone = noise::fbm2d(p.x * 0.06f, p.z * 0.06f, 2, 2.0f, 0.5f, seed + 303u);
    const f32 litter = noise::fbm2d(p.x * 0.11f, p.z * 0.11f, 2, 2.0f, 0.5f, seed + 404u);
    Vec3 ground = glm::mix(grass, grass2, glm::smoothstep(-0.25f, 0.25f, tone));
    ground = glm::mix(ground, dirt, glm::smoothstep(0.22f, 0.5f, litter));
    // Flat ground: desert sand <-> forest floor by moisture.
    Vec3 flat = glm::mix(sand, ground, glm::smoothstep(-0.15f, 0.2f, m));
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
