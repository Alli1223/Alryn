#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Noise.h> // meadow-field noise that clusters wildflowers into beds
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/TreeScatter.h> // detail::tree_hash / hash01
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/Village.h> // town_path_amount (grass grows in the town's green areas, off streets)

#include <algorithm>
#include <cmath>

namespace alryn {

namespace detail {

// Appends `src` to `dst` transformed by `xf` (positions + normals) and tinted.
// Assumes near-uniform scale (grass/flowers), so mat3(xf) suffices for normals.
// `base_y` is the plant's ground height: each vertex's height above it becomes the
// `sway` weight so the wind shader bends tips more than roots.
inline void append_transformed(MeshData& dst, const MeshData& src, const Mat4& xf,
                               const Vec3& tint, f32 base_y, f32 sway_scale = 1.0f) {
    const Mat3 nm{xf};
    const u32 base = static_cast<u32>(dst.vertices.size());
    for (const Vertex& v : src.vertices) {
        Vertex o;
        o.position = Vec3{xf * Vec4{v.position, 1.0f}};
        o.normal = glm::normalize(nm * v.normal);
        o.color = v.color * tint;
        // Height above the plant base drives the wind bend; `sway_scale` 0 makes a plant rigid
        // (e.g. a cactus shouldn't wave around like grass).
        o.sway = glm::max(o.position.y - base_y, 0.0f) * sway_scale;
        dst.vertices.push_back(o);
    }
    for (u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
}

} // namespace detail

namespace detail {

// Common gate for ground vegetation: above water, off roads/streets, not too steep, moist enough.
// `biome_filter` (on for the lush forest-floor plants) also rejects the desert / bog / snow biomes,
// which grow their OWN plants instead (cacti, reeds) - those placers pass biome_filter=false and do
// their own biome check. Returns the local moisture via `out_moist`.
inline bool veg_ground(f32 wx, f32 wz, u32 seed, f32 gh, f32 max_slope, f32 min_moist,
                       f32& out_moist, bool town_ok = false, bool biome_filter = true) {
    if (gh < worldgen::water_level + 0.6f) {
        return false;
    }
    if (roads::distance(wx, wz, seed) < roads::road_half_width - 0.2f) {
        return false; // bare dirt on the road itself (grass grows up to the edge)
    }
    if (worldgen::inside_village(wx, wz, seed)) {
        if (!town_ok) {
            return false; // most plants don't grow on the trampled town ground...
        }
        if (town_path_amount(Vec3{wx, gh, wz}, 1.0f, seed) > 0.1f) {
            return false; // ...but grass does, in the green areas - just keep it off the streets
        }
    }
    out_moist = worldgen::moisture(wx, wz, seed);
    if (biome_filter) {
        // Cheap biome proxy reusing gh + out_moist: keep the lush forest-floor plants out of the
        // snow line, the wet bog hollows, and the hot/dry desert (which get reeds / cacti instead).
        if (gh > 9.5f) {
            return false;
        }
        const bool bog = out_moist > 0.42f && gh < worldgen::water_level + 4.5f;
        const bool desert = out_moist < 0.08f && worldgen::temperature(wx, wz, seed) > 0.58f;
        if (bog || desert) {
            return false;
        }
    }
    if (out_moist < min_moist) {
        return false;
    }
    const f32 slope = std::abs(worldgen::height(wx + 1.0f, wz, seed) - gh) +
                      std::abs(worldgen::height(wx, wz + 1.0f, seed) - gh);
    return slope <= max_slope;
}

} // namespace detail

// Builds one baked mesh of ground vegetation for the chunk covering world
// [cx*cw,(cx+1)*cw] x [cz*cw,(cz+1)*cw]: a dense, varied forest floor of grass,
// tall grass, ferns, mushrooms, leafy ground cover and wildflowers. Each kind is
// scattered on its own global cell grid (so chunks tile) and chosen by moisture -
// wetter ground reads as lush forest floor, drier as open meadow. It's all one
// opaque mesh, so a whole chunk of undergrowth is a single draw call.
inline MeshData build_vegetation(int cx, int cz, f32 chunk_world, u32 seed) {
    MeshData m;
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;

    // Runs `place` for each jittered cell of size `cell` overlapping the chunk.
    auto for_cells = [&](f32 cell, u32 salt, auto&& place) {
        const int gx0 = static_cast<int>(std::floor(x0 / cell));
        const int gz0 = static_cast<int>(std::floor(z0 / cell));
        const int n = std::max(1, static_cast<int>(std::lround(chunk_world / cell)));
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const int gx = gx0 + i;
                const int gz = gz0 + j;
                const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, salt + 1u)) - 0.5f) * cell * 0.9f;
                const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, salt + 2u)) - 0.5f) * cell * 0.9f;
                const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
                const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;
                place(gx, gz, wx, wz);
            }
        }
    };
    auto place_at = [&](const MeshData& src, f32 wx, f32 wz, f32 gh, f32 yaw, f32 sc, f32 sy,
                        const Vec3& tint, bool rigid = false) {
        const Mat4 xf = glm::translate(Mat4{1.0f}, Vec3{wx, gh, wz}) *
                        glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                        glm::scale(Mat4{1.0f}, Vec3{sc, sc * sy, sc});
        detail::append_transformed(m, src, xf, tint, gh, rigid ? 0.0f : 1.0f);
    };

    // ---- Grass: dense, everywhere it can grow; lush green when wet, tan when dry.
    for_cells(0.6f, seed + 5000u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.4f, -0.08f, moist, true)) return; // grows in town too
        const f32 density = 0.4f + glm::clamp(moist, 0.0f, 0.7f) * 0.5f;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5003u)) > density) return;
        const Vec3 dry{0.55f, 0.50f, 0.26f};
        const Vec3 lush{0.24f, 0.52f, 0.23f};
        const Vec3 g = glm::mix(dry, lush, glm::smoothstep(-0.05f, 0.32f, moist));
        const f32 shade = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5004u)) * 0.28f;
        const f32 sc = 0.8f + detail::hash01(detail::tree_hash(gx, gz, seed + 5005u)) * 0.8f;
        const f32 sy = 0.8f + detail::hash01(detail::tree_hash(gx, gz, seed + 5006u)) * 0.6f;
        place_at(primitives::grass_tuft(4, g), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5007u)) * TwoPi, sc, sy, Vec3{shade});
    });

    // ---- Ferns: lush, shady forest floor (needs moisture).
    for_cells(1.05f, seed + 5100u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.0f, 0.04f, moist)) return;
        const f32 density = glm::clamp(moist, 0.0f, 0.5f) * 0.75f;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5103u)) > density) return;
        const int var = static_cast<int>(detail::tree_hash(gx, gz, seed + 5104u) % 3u);
        const f32 sc = 0.9f + detail::hash01(detail::tree_hash(gx, gz, seed + 5105u)) * 0.9f;
        const f32 shade = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5106u)) * 0.3f;
        place_at(primitives::fern(var), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5107u)) * TwoPi, sc, 1.0f, Vec3{shade});
    });

    // ---- Tall grass tufts: scattered taller clumps.
    for_cells(1.4f, seed + 5200u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.2f, -0.02f, moist, true)) return; // grows in town too
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5203u)) > 0.3f) return;
        const Vec3 g = glm::mix(Vec3{0.45f, 0.46f, 0.24f}, Vec3{0.30f, 0.54f, 0.26f},
                                glm::smoothstep(-0.05f, 0.3f, moist));
        const f32 sc = 0.9f + detail::hash01(detail::tree_hash(gx, gz, seed + 5205u)) * 0.6f;
        place_at(primitives::tall_grass(6, g), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5207u)) * TwoPi, sc, 1.0f, Vec3{1.0f});
    });

    // ---- Long meadow grass: tall clumps that wave in the wind, with the odd REALLY
    // long stalk you wade through. Two low-frequency fields shape it: a "dryness"
    // field paints whole swathes either fresh green or sun-bleached straw yellow, and
    // a "thicket" field concentrates the grass into properly THICK, dense clumps (each
    // cell drops a tight cluster of overlapping tufts) in places, with sparse single
    // tufts between - so meadows have real thickets rather than an even sprinkle.
    // Grows in the town's green areas too.
    for_cells(1.15f, seed + 5900u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.0f, -0.04f, moist, true)) return; // grows in town too
        const f32 thicket = glm::smoothstep(0.05f, 0.6f,
            noise::fbm2d(wx * 0.018f, wz * 0.018f, 2, 2.0f, 0.5f, seed + 5980u));
        const f32 density = 0.3f + thicket * 0.65f; // sparse in the open, near-solid in a thicket
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5903u)) > density) return;
        const f32 dryness = glm::smoothstep(-0.22f, 0.42f,
            noise::fbm2d(wx * 0.025f, wz * 0.025f, 2, 2.0f, 0.5f, seed + 5999u));
        const Vec3 green{0.30f, 0.55f, 0.24f};
        const Vec3 straw{0.76f, 0.67f, 0.31f}; // dried-up yellow
        const Vec3 g = glm::mix(green, straw, dryness);
        // In a thicket, pack a tight CLUSTER of clumps that overlap into a thick mass;
        // in the open, just one tuft.
        const int count = 1 + static_cast<int>(thicket * 3.0f); // up to 4 clumps per cell
        for (int k = 0; k < count; ++k) {
            const f32 ox = (detail::hash01(detail::tree_hash(gx * 11 + k, gz, seed + 5911u)) - 0.5f) * 0.6f;
            const f32 oz = (detail::hash01(detail::tree_hash(gx, gz * 11 + k, seed + 5912u)) - 0.5f) * 0.6f;
            const int blades = 8 + static_cast<int>(detail::hash01(detail::tree_hash(gx + k, gz, seed + 5904u)) * 5.0f);
            const f32 sc = 0.8f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 5905u)) * 0.35f;
            // ~1 in 6 clumps shoots up really long; the rest are ordinary tall grass.
            const bool very_long = detail::hash01(detail::tree_hash(gx + k, gz, seed + 5906u)) < 0.16f;
            const f32 sy = very_long ? (1.5f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 5907u)) * 0.45f)
                                     : (0.95f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 5908u)) * 0.35f);
            const f32 gh2 = worldgen::height(wx + ox, wz + oz, seed);
            place_at(primitives::meadow_grass(blades, g), wx + ox, wz + oz, gh2,
                     detail::hash01(detail::tree_hash(gx + k, gz, seed + 5909u)) * TwoPi, sc, sy, Vec3{1.0f});
        }
    });

    // ---- Mushrooms: damp forest floor, various caps; the odd cluster.
    static const Vec3 caps[] = {{0.74f, 0.16f, 0.13f}, {0.55f, 0.34f, 0.20f}, {0.80f, 0.66f, 0.40f},
                                {0.86f, 0.74f, 0.30f}, {0.55f, 0.40f, 0.55f}};
    for_cells(1.7f, seed + 5300u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 1.8f, 0.06f, moist)) return;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5303u)) > 0.22f) return;
        const u32 ci = detail::tree_hash(gx, gz, seed + 5304u) % 5u;
        const bool red = ci == 0;
        const int count = 1 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 5305u)) * 3.0f);
        for (int k = 0; k < count; ++k) {
            const f32 ox = (detail::hash01(detail::tree_hash(gx * 7 + k, gz, seed + 5306u)) - 0.5f) * 0.35f;
            const f32 oz = (detail::hash01(detail::tree_hash(gx, gz * 7 + k, seed + 5307u)) - 0.5f) * 0.35f;
            const f32 sc = 0.6f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 5308u)) * 0.9f;
            const f32 gh2 = worldgen::height(wx + ox, wz + oz, seed);
            place_at(primitives::mushroom(caps[ci], 1.0f, red), wx + ox, wz + oz, gh2, 0.0f, sc, 1.0f,
                     Vec3{1.0f});
        }
    });

    // ---- Leafy ground cover (clover / herbs): low, common, fills gaps.
    for_cells(0.85f, seed + 5400u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.2f, 0.0f, moist)) return;
        const f32 density = 0.18f + glm::clamp(moist, 0.0f, 0.5f) * 0.4f;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5403u)) > density) return;
        const int var = static_cast<int>(detail::tree_hash(gx, gz, seed + 5404u) % 4u);
        const Vec3 g = glm::mix(Vec3{0.30f, 0.42f, 0.22f}, Vec3{0.22f, 0.48f, 0.24f},
                                glm::smoothstep(0.0f, 0.3f, moist));
        const f32 sc = 0.8f + detail::hash01(detail::tree_hash(gx, gz, seed + 5405u)) * 0.7f;
        place_at(primitives::ground_leaf(var, g), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5407u)) * TwoPi, sc, 1.0f, Vec3{1.0f});
    });

    // ---- Wildflowers: vivid blooms clustered into MEADOW BEDS (denser + more colourful, like the
    // reference). A low-frequency "meadow" field concentrates them into flowery patches rather than
    // an even sprinkle, and each chosen cell drops a small CLUSTER of blooms. Grows in town green
    // areas too (off the streets), for the town flower beds.
    static const Vec3 blossoms[] = {{0.93f, 0.22f, 0.26f}, {0.97f, 0.83f, 0.22f},
                                    {0.98f, 0.98f, 0.99f}, {0.72f, 0.40f, 0.93f},
                                    {0.97f, 0.57f, 0.78f}, {0.98f, 0.54f, 0.16f}};
    for_cells(1.0f, seed + 5500u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.2f, -0.02f, moist, true)) return;
        const f32 meadow = noise::fbm2d(wx * 0.05f, wz * 0.05f, 2, 2.0f, 0.5f, seed + 5599u);
        const f32 density = 0.1f + glm::smoothstep(-0.1f, 0.55f, meadow) * 0.55f; // dense in flowery patches
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5503u)) > density) return;
        const int count = 1 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 5510u)) * 3.0f);
        for (int k = 0; k < count; ++k) {
            const f32 ox = (detail::hash01(detail::tree_hash(gx * 9 + k, gz, seed + 5511u)) - 0.5f) * 0.75f;
            const f32 oz = (detail::hash01(detail::tree_hash(gx, gz * 9 + k, seed + 5512u)) - 0.5f) * 0.75f;
            const u32 bi = detail::tree_hash(gx + k * 13, gz, seed + 5504u) % 6u;
            const f32 sc = 0.85f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 5505u)) * 0.7f;
            const f32 gh2 = worldgen::height(wx + ox, wz + oz, seed);
            place_at(primitives::flower(blossoms[bi]), wx + ox, wz + oz, gh2,
                     detail::hash01(detail::tree_hash(gx + k, gz, seed + 5507u)) * TwoPi, sc, 1.0f, Vec3{1.0f});
        }
    });

    // ---- Bog reeds: tall marsh reeds + cattails in the wet, low-lying swamp hollows. They sway.
    for_cells(0.8f, seed + 5600u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 2.0f, 0.40f, moist, false, false)) return;
        if (worldgen::biome_at(wx, wz, seed) != worldgen::Biome::Bog) return;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5603u)) > 0.55f) return;
        const Vec3 reedc = glm::mix(Vec3{0.30f, 0.40f, 0.24f}, Vec3{0.42f, 0.50f, 0.28f},
                                    detail::hash01(detail::tree_hash(gx, gz, seed + 5604u)));
        const int n = 3 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 5605u)) * 3.0f);
        const f32 sc = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5606u)) * 0.6f;
        place_at(primitives::reed(n, reedc), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5607u)) * TwoPi, sc, 1.0f, Vec3{1.0f});
    });

    // ---- Desert cacti: sparse saguaros standing in the dunes. Rigid (no wind sway).
    for_cells(3.4f, seed + 5700u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 1.1f, -1.0f, moist, false, false)) return;
        if (worldgen::biome_at(wx, wz, seed) != worldgen::Biome::Desert) return;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5703u)) > 0.28f) return;
        const int var = static_cast<int>(detail::tree_hash(gx, gz, seed + 5704u) % 2u);
        const f32 sc = 0.85f + detail::hash01(detail::tree_hash(gx, gz, seed + 5705u)) * 0.7f;
        const Vec3 green = glm::mix(Vec3{0.27f, 0.42f, 0.24f}, Vec3{0.34f, 0.48f, 0.28f},
                                    detail::hash01(detail::tree_hash(gx, gz, seed + 5706u)));
        place_at(primitives::cactus(var, green), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5707u)) * TwoPi, sc, 1.0f, Vec3{1.0f},
                 /*rigid=*/true);
    });

    // ===== Water's edge & underwater life ==================================================
    // The shoreline + shallows get their own life, gated by biome so each body of water reads
    // right: pebbly/sandy stones along every shore, cattail reeds fringing temperate freshwater,
    // floating lily pads on calm ponds & slow rivers, and colourful coral on warm shallow seabeds.
    //
    // These passes are confined to chunks that actually touch the waterline. The vast majority of
    // chunks are dry inland ground, so a cheap 3x3 height probe lets the worker skip the lot for
    // them (keeping the streaming meshing path fast) - only coastal / lakeside / river chunks pay
    // for the per-cell scatter below.
    bool near_water = false;
    for (int sj = 0; sj <= 2 && !near_water; ++sj) {
        for (int si = 0; si <= 2 && !near_water; ++si) {
            const f32 hh = worldgen::height(x0 + chunk_world * 0.5f * static_cast<f32>(si),
                                            z0 + chunk_world * 0.5f * static_cast<f32>(sj), seed);
            // A water-touching chunk dips toward the waterline yet isn't below the reef seabed.
            if (hh < worldgen::water_level + 1.6f && hh > worldgen::water_level - 5.0f) {
                near_water = true;
            }
        }
    }
    if (near_water) {
    // ---- Shore stones: pebbles + small boulders strewn along the waterline of every shore.
    for_cells(2.0f, seed + 6100u, [&](int gx, int gz, f32 wx, f32 wz) {
        const f32 gh = worldgen::height(wx, wz, seed);
        if (gh < worldgen::water_level - 0.5f || gh > worldgen::water_level + 1.4f) return; // wet band
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 6103u)) > 0.5f) return;
        const auto biome = worldgen::biome_at(wx, wz, seed);
        Vec3 stone{0.46f, 0.47f, 0.50f}; // cool grey by default
        if (biome == worldgen::Biome::Desert || biome == worldgen::Biome::Beach) {
            stone = Vec3{0.72f, 0.64f, 0.48f}; // warm sandstone on sandy coasts
        } else if (biome == worldgen::Biome::Bog) {
            stone = Vec3{0.34f, 0.36f, 0.33f}; // dark wet stone in the swamp
        }
        const int var = static_cast<int>(detail::tree_hash(gx, gz, seed + 6104u) % 3u);
        const int count = 1 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 6106u)) * 2.0f);
        for (int k = 0; k < count; ++k) {
            const f32 ox = (detail::hash01(detail::tree_hash(gx * 5 + k, gz, seed + 6107u)) - 0.5f) * 1.4f;
            const f32 oz = (detail::hash01(detail::tree_hash(gx, gz * 5 + k, seed + 6108u)) - 0.5f) * 1.4f;
            const f32 gh2 = worldgen::height(wx + ox, wz + oz, seed);
            if (gh2 < worldgen::water_level - 0.8f) continue; // not out in deep water
            const f32 shade = 0.85f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 6105u)) * 0.3f;
            const f32 sc = 0.16f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 6109u)) * 0.5f;
            place_at(primitives::rock(var, stone), wx + ox, wz + oz, gh2,
                     detail::hash01(detail::tree_hash(gx + k, gz, seed + 6110u)) * TwoPi, sc, 0.7f,
                     Vec3{shade}, /*rigid=*/true);
        }
    });

    // ---- Shoreline reeds: cattails fringing FRESHWATER edges (ponds / rivers / lake shores),
    // beyond the bog where the lush forest-floor reeds already grow. Sparse, kept right at the
    // wet edge (not standing out in deep water), and only where the land just inland reads as
    // forest/plains/bog - so they don't carpet salty sand beaches.
    auto inland_biome = [&](f32 x, f32 z) {
        f32 bh = -1e9f;
        Vec2 best{x, z};
        for (const Vec2& d : {Vec2{8.0f, 0.0f}, Vec2{-8.0f, 0.0f}, Vec2{0.0f, 8.0f}, Vec2{0.0f, -8.0f}}) {
            const f32 h = worldgen::height(x + d.x, z + d.y, seed);
            if (h > bh) {
                bh = h;
                best = Vec2{x + d.x, z + d.y};
            }
        }
        return worldgen::biome_at(best.x, best.y, seed);
    };
    for_cells(1.6f, seed + 6400u, [&](int gx, int gz, f32 wx, f32 wz) {
        const f32 gh = worldgen::height(wx, wz, seed);
        if (gh < worldgen::water_level - 0.12f || gh > worldgen::water_level + 0.5f) return; // wet edge only
        if (worldgen::temperature(wx, wz, seed) > 0.62f) return; // not the hot desert shore
        if (worldgen::slope(wx, wz, seed) > 1.0f) return;         // not on a steep bank
        const auto ib = inland_biome(wx, wz);
        if (ib != worldgen::Biome::Forest && ib != worldgen::Biome::Plains &&
            ib != worldgen::Biome::Bog) {
            return; // freshwater only - skip sandy salt coasts / rock / snow
        }
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 6403u)) > 0.3f) return; // sparse fringe
        const Vec3 reedc = glm::mix(Vec3{0.32f, 0.42f, 0.24f}, Vec3{0.46f, 0.52f, 0.30f},
                                    detail::hash01(detail::tree_hash(gx, gz, seed + 6404u)));
        const int n = 2 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 6405u)) * 2.0f);
        const f32 sc = 0.8f + detail::hash01(detail::tree_hash(gx, gz, seed + 6406u)) * 0.6f;
        place_at(primitives::reed(n, reedc), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 6407u)) * TwoPi, sc, 1.0f, Vec3{1.0f});
    });

    // ---- Lily pads: floating leaves + blooms on calm, shallow FRESH water. A nearby-land check
    // keeps them on enclosed ponds / slow rivers / bog pools, off the open (salt) sea.
    auto near_land = [&](f32 x, f32 z) {
        for (const Vec2& d : {Vec2{5.0f, 0.0f}, Vec2{-5.0f, 0.0f}, Vec2{0.0f, 5.0f}, Vec2{0.0f, -5.0f}}) {
            if (worldgen::height(x + d.x, z + d.y, seed) > worldgen::water_level + 0.2f) return true;
        }
        return false;
    };
    for_cells(1.5f, seed + 6300u, [&](int gx, int gz, f32 wx, f32 wz) {
        const f32 gh = worldgen::height(wx, wz, seed);
        if (gh > worldgen::water_level - 0.04f || gh < worldgen::water_level - 1.8f) return; // shallow water
        const f32 temp = worldgen::temperature(wx, wz, seed);
        if (temp < 0.18f || temp > 0.62f) return;        // not frozen, not desert
        if (worldgen::slope(wx, wz, seed) > 0.7f) return; // calm water only
        if (!near_land(wx, wz)) return;                   // a pond / river, not the open sea
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 6303u)) > 0.5f) return;
        const Vec3 pad = glm::mix(Vec3{0.18f, 0.44f, 0.22f}, Vec3{0.24f, 0.50f, 0.26f},
                                  detail::hash01(detail::tree_hash(gx, gz, seed + 6304u)));
        const int count = 1 + static_cast<int>(detail::hash01(detail::tree_hash(gx, gz, seed + 6305u)) * 3.0f);
        for (int k = 0; k < count; ++k) {
            const f32 ox = (detail::hash01(detail::tree_hash(gx * 7 + k, gz, seed + 6306u)) - 0.5f) * 1.1f;
            const f32 oz = (detail::hash01(detail::tree_hash(gx, gz * 7 + k, seed + 6307u)) - 0.5f) * 1.1f;
            if (worldgen::height(wx + ox, wz + oz, seed) > worldgen::water_level - 0.04f) continue; // stay wet
            const int var = static_cast<int>(detail::tree_hash(gx + k, gz, seed + 6308u) % 3u);
            const f32 sc = 0.8f + detail::hash01(detail::tree_hash(gx + k, gz, seed + 6309u)) * 0.7f;
            place_at(primitives::lily_pad(var, pad), wx + ox, wz + oz, worldgen::water_level + 0.04f,
                     detail::hash01(detail::tree_hash(gx + k, gz, seed + 6310u)) * TwoPi, sc, 1.0f,
                     Vec3{1.0f}, /*rigid=*/true);
        }
    });

    // ---- Coral: colourful reefs on warm, shallow seabeds (underwater, tropical only).
    static const Vec3 reefcol[] = {{0.94f, 0.40f, 0.45f}, {0.96f, 0.62f, 0.28f}, {0.62f, 0.42f, 0.86f},
                                   {0.95f, 0.45f, 0.70f}, {0.38f, 0.74f, 0.66f}};
    for_cells(2.4f, seed + 6200u, [&](int gx, int gz, f32 wx, f32 wz) {
        const f32 gh = worldgen::height(wx, wz, seed);
        if (gh > worldgen::water_level - 0.6f || gh < worldgen::water_level - 4.5f) return; // submerged shallows
        if (worldgen::temperature(wx, wz, seed) < 0.46f) return; // cold seas grow no coral
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 6203u)) > 0.45f) return;
        const u32 ci = detail::tree_hash(gx, gz, seed + 6204u) % 5u;
        const int var = static_cast<int>(detail::tree_hash(gx, gz, seed + 6205u) % 3u);
        const f32 sc = 0.7f + detail::hash01(detail::tree_hash(gx, gz, seed + 6206u)) * 0.9f;
        place_at(primitives::coral(var, reefcol[ci]), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 6207u)) * TwoPi, sc, 1.0f,
                 Vec3{1.0f}, /*rigid=*/true);
    });
    } // near_water

    // ---- Desert dry tufts: sparse bleached grass clumps scattered between the dunes.
    for_cells(1.2f, seed + 5800u, [&](int gx, int gz, f32 wx, f32 wz) {
        f32 moist;
        const f32 gh = worldgen::height(wx, wz, seed);
        if (!detail::veg_ground(wx, wz, seed, gh, 1.4f, -1.0f, moist, false, false)) return;
        if (worldgen::biome_at(wx, wz, seed) != worldgen::Biome::Desert) return;
        if (detail::hash01(detail::tree_hash(gx, gz, seed + 5803u)) > 0.3f) return;
        const Vec3 dry{0.72f, 0.63f, 0.34f};
        const f32 sc = 0.7f + detail::hash01(detail::tree_hash(gx, gz, seed + 5805u)) * 0.6f;
        place_at(primitives::grass_tuft(3, dry), wx, wz, gh,
                 detail::hash01(detail::tree_hash(gx, gz, seed + 5807u)) * TwoPi, sc, 0.8f, Vec3{1.0f});
    });

    return m;
}

} // namespace alryn
