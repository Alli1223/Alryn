#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/WorldGen.h>

#include <cmath>
#include <vector>

namespace alryn {

struct TreeInstance {
    Vec3 position{0.0f}; // trunk base, on the ground
    f32 scale = 1.0f;
    f32 yaw = 0.0f;
    int variant = 0;
    Vec3 tint{1.0f};
};

namespace detail {

inline u32 tree_hash(int x, int z, u32 salt) {
    u32 h = salt + 0x9E3779B9u + static_cast<u32>(x) * 0x85EBCA77u + static_cast<u32>(z) * 0xC2B2AE3Du;
    h = (h ^ (h >> 15)) * 0x2545F491u;
    h = (h ^ (h >> 13)) * 0x27D4EB2Fu;
    return h ^ (h >> 16);
}

inline f32 hash01(u32 h) {
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}

} // namespace detail

// Deterministically scatters trees over the chunk covering world
// [cx*cw, (cx+1)*cw] x [cz*cw, (cz+1)*cw]. Trees only appear on gentle, above-water,
// non-desert, non-peak ground; forests are denser where the world is wetter.
// Placement is on a global cell grid, so neighbouring chunks never double-place.
inline std::vector<TreeInstance> scatter_trees(int cx, int cz, f32 chunk_world, u32 seed) {
    std::vector<TreeInstance> trees;
    constexpr f32 cell = 4.0f;
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;
    const int gx0 = static_cast<int>(std::floor(x0 / cell));
    const int gz0 = static_cast<int>(std::floor(z0 / cell));
    const int n = static_cast<int>(chunk_world / cell);

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int gx = gx0 + i;
            const int gz = gz0 + j;
            const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, seed + 1u)) - 0.5f) * cell * 0.8f;
            const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, seed + 2u)) - 0.5f) * cell * 0.8f;
            const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
            const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;

            const f32 gh = worldgen::height(wx, wz, seed);
            if (gh < worldgen::water_level + 1.2f || gh > 8.0f) {
                continue; // in/near water, or up on the peaks
            }
            const f32 moist = worldgen::moisture(wx, wz, seed);
            if (moist < -0.1f) {
                continue; // dry desert
            }
            const u32 h = detail::tree_hash(gx, gz, seed + 777u);
            const f32 density = 0.22f + glm::clamp(moist, 0.0f, 0.7f) * 0.6f;
            if (detail::hash01(h) > density) {
                continue;
            }
            const f32 slope = std::abs(worldgen::height(wx + 1.0f, wz, seed) - gh) +
                              std::abs(worldgen::height(wx, wz + 1.0f, seed) - gh);
            if (slope > 2.2f) {
                continue; // too steep
            }

            TreeInstance t;
            t.position = Vec3{wx, gh, wz};
            t.scale = 1.0f + detail::hash01(detail::tree_hash(gx, gz, seed + 3u)) * 0.8f;
            t.yaw = detail::hash01(detail::tree_hash(gx, gz, seed + 4u)) * TwoPi;
            t.variant = static_cast<int>(h % 2u);
            const f32 cv = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5u)) * 0.3f;
            t.tint = Vec3{cv * 0.95f, cv, cv * 0.9f};
            trees.push_back(t);
        }
    }
    return trees;
}

} // namespace alryn
