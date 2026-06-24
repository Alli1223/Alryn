#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/ScatterHash.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Village.h> // a few decorative trees inside towns, clear of streets/buildings

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
                continue; // very dry ground stays open
            }
            const worldgen::Biome biome = worldgen::biome_at(wx, wz, seed);
            if (biome == worldgen::Biome::Desert || biome == worldgen::Biome::Snow ||
                biome == worldgen::Biome::Beach || biome == worldgen::Biome::Ocean) {
                continue; // deserts grow cacti, peaks/coasts grow nothing
            }
            const u32 h = detail::tree_hash(gx, gz, seed + 777u);
            // Canopy density per biome: thick wet forest, sparse alpine conifers, a few gnarled
            // trees in the bog, scattered lone trees on the open plains.
            f32 density;
            switch (biome) {
                case worldgen::Biome::Mountains: density = 0.24f; break;
                case worldgen::Biome::Bog: density = 0.18f; break;
                case worldgen::Biome::Plains: density = 0.14f; break;
                default: density = 0.45f + glm::clamp(moist, 0.0f, 0.7f) * 0.5f; break;
            }
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
                const f32 distc = glm::length(dv);
                const f32 edge = distc - worldgen::town_radius(*tv, std::atan2(dv.y, dv.x), seed);
                if (edge < 0.0f) {
                    // INSIDE the town: a few decorative trees in the open green spots - off the
                    // plaza, off the dirt streets/flagstones, and clear of any building.
                    if (distc < detail::kMarketHalf + 4.0f) {
                        continue; // keep the market plaza open
                    }
                    if (town_path_amount(Vec3{wx, gh, wz}, 1.0f, seed) > 0.05f) {
                        continue; // off the streets
                    }
                    if (detail::hash01(h ^ 0x99u) > 0.14f) {
                        continue; // sparse - just a scattered few
                    }
                    bool near_building = false;
                    const auto gates = detail::village_gate_points(*tv, seed);
                    detail::for_each_house(*tv, seed, gates, [&](const detail::HousePlot& hp) {
                        if (glm::length(hp.pos - Vec2{wx, wz}) < detail::house_reach(hp.variant) + 2.0f) {
                            near_building = true;
                        }
                    });
                    if (near_building) {
                        continue; // don't grow a tree through a house
                    }
                    // else: fall through and place a tree here
                } else if (edge < 4.0f) {
                    continue; // a thin clearing right at the wall, so trees hug the town edge
                } else {
                    const f32 thicken = glm::clamp((edge - 4.0f) / 11.0f, 0.0f, 1.0f);
                    if (detail::hash01(h ^ 0x51u) > thicken) {
                        continue; // forest thickens back over a fade band beyond the wall
                    }
                }
            }

            TreeInstance t;
            t.position = Vec3{wx, gh, wz};
            // Big trees for a dense, canopied forest (half the previous towering size).
            t.scale = 2.3f + detail::hash01(detail::tree_hash(gx, gz, seed + 3u)) * 1.5f;
            t.yaw = detail::hash01(detail::tree_hash(gx, gz, seed + 4u)) * TwoPi;
            const f32 cv = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5u)) * 0.3f;
            const Vec3 leaf_base{0.16f, 0.40f, 0.19f};
            if (biome == worldgen::Biome::Mountains) {
                t.variant = 0;        // alpine conifers
                t.scale *= 0.82f;     // a touch smaller up high
                t.tint = Vec3{cv * 0.64f, cv * 0.86f, cv * 0.66f}; // deep, cool green
            } else if (biome == worldgen::Biome::Bog) {
                t.variant = 4;        // weathered, gnarled dead trees
                t.scale *= 0.88f;
                t.tint = Vec3{0.42f, 0.40f, 0.33f} * (cv + 0.08f); // dark + lifeless
            } else {
                // Forest / plains: the broadleaf mix with the autumn-patchwork tint. The foliage
                // mesh is baked deep forest green (primitives::tree()); a tint of target/leaf_base
                // re-colours the whole canopy. Conifers + dead trees stay green/muted; deciduous
                // trees turn an AUTUMN patchwork - green / gold / orange / russet by a per-tree hash.
                t.variant = static_cast<int>(h % 5u); // pine / oak / birch / broad oak / dead
                if (biome == worldgen::Biome::Plains) {
                    t.scale *= 0.9f;
                }
                if (t.variant == 0 || t.variant == 4) {
                    t.tint = Vec3{cv * 0.92f, cv, cv * 0.88f}; // pine / dead: stay green-ish
                } else {
                    const f32 a = detail::hash01(detail::tree_hash(gx, gz, seed + 6u));
                    const Vec3 target = a < 0.30f   ? Vec3{0.34f, 0.52f, 0.22f}  // still green
                                        : a < 0.60f ? Vec3{0.84f, 0.66f, 0.22f}  // golden yellow
                                        : a < 0.85f ? Vec3{0.88f, 0.46f, 0.16f}  // orange
                                                    : Vec3{0.74f, 0.30f, 0.16f}; // russet red
                    t.tint = (target / leaf_base) * cv;
                }
            }
            trees.push_back(t);
        }
    }
    return trees;
}

} // namespace alryn
