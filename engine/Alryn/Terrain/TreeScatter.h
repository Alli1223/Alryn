#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
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

// Deterministically scatters trees over the chunk covering world
// [cx*cw, (cx+1)*cw] x [cz*cw, (cz+1)*cw]. Trees only appear on gentle, above-water,
// non-desert, non-peak ground; forests are denser where the world is wetter.
// Placement is on a global cell grid, so neighbouring chunks never double-place.
inline std::vector<TreeInstance> scatter_trees(int cx, int cz, f32 chunk_world, u32 seed) {
    std::vector<TreeInstance> trees;
    constexpr f32 cell = 2.9f; // very dense woodland (room to weave between trunks)
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;
    const int gx0 = static_cast<int>(std::floor(x0 / cell));
    const int gz0 = static_cast<int>(std::floor(z0 / cell));
    const int n = std::max(1, static_cast<int>(std::lround(chunk_world / cell)));

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int gx = gx0 + i;
            const int gz = gz0 + j;
            const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, seed + 1u)) - 0.5f) * cell * 0.85f;
            const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, seed + 2u)) - 0.5f) * cell * 0.85f;
            const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
            const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;

            const f32 gh = worldgen::height(wx, wz, seed);
            if (gh < worldgen::water_level + 1.2f || gh > 9.0f) {
                continue; // in/near water, or up on the peaks
            }
            const f32 moist = worldgen::moisture(wx, wz, seed);
            if (moist < -0.1f) {
                continue; // dry desert stays open
            }
            const u32 h = detail::tree_hash(gx, gz, seed + 777u);
            // Thick canopy where it's wet; still well-wooded toward dry/edge ground.
            const f32 density = 0.45f + glm::clamp(moist, 0.0f, 0.7f) * 0.5f;
            if (detail::hash01(h) > density) {
                continue;
            }
            const f32 slope = std::abs(worldgen::height(wx + 1.0f, wz, seed) - gh) +
                              std::abs(worldgen::height(wx, wz + 1.0f, seed) - gh);
            if (slope > 2.6f) {
                continue; // too steep
            }
            if (roads::distance(wx, wz, seed) < roads::road_half_width + 10.0f) {
                continue; // a broad cleared verge - open sightlines along the road, no canopy roof
            }
            // Towns sit in a clearing: bare for a ring outside the wall, then the forest
            // thickens back over a fade band - so you emerge from woods into open ground
            // around a town instead of it being walled in by giant canopies.
            if (const auto tv = worldgen::village_containing(wx, wz, seed, 22.0f)) {
                const Vec2 dv{wx - tv->center.x, wz - tv->center.y};
                const f32 edge = glm::length(dv) - worldgen::town_radius(*tv, std::atan2(dv.y, dv.x), seed);
                if (edge < 10.0f) {
                    continue; // open clearing hugging the town
                }
                const f32 thicken = glm::clamp((edge - 10.0f) / 12.0f, 0.0f, 1.0f);
                if (detail::hash01(h ^ 0x51u) > thicken) {
                    continue; // forest thins as it nears the clearing
                }
            }

            TreeInstance t;
            t.position = Vec3{wx, gh, wz};
            // Big trees for a dense, canopied forest (half the previous towering size).
            t.scale = 2.3f + detail::hash01(detail::tree_hash(gx, gz, seed + 3u)) * 1.5f;
            t.yaw = detail::hash01(detail::tree_hash(gx, gz, seed + 4u)) * TwoPi;
            t.variant = static_cast<int>(h % 5u); // pine / oak / birch / broad oak / dead
            const f32 cv = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5u)) * 0.3f;
            t.tint = Vec3{cv * 0.95f, cv, cv * 0.9f};
            trees.push_back(t);
        }
    }
    return trees;
}

} // namespace alryn
