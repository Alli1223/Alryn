#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/TreeScatter.h> // detail::tree_hash / hash01
#include <Alryn/Terrain/WorldGen.h>

#include <algorithm>
#include <cmath>

namespace alryn {

namespace detail {

// Appends `src` to `dst` transformed by `xf` (positions + normals) and tinted.
// Assumes near-uniform scale (grass/flowers), so mat3(xf) suffices for normals.
inline void append_transformed(MeshData& dst, const MeshData& src, const Mat4& xf,
                               const Vec3& tint) {
    const Mat3 nm{xf};
    const u32 base = static_cast<u32>(dst.vertices.size());
    for (const Vertex& v : src.vertices) {
        Vertex o;
        o.position = Vec3{xf * Vec4{v.position, 1.0f}};
        o.normal = glm::normalize(nm * v.normal);
        o.color = v.color * tint;
        dst.vertices.push_back(o);
    }
    for (u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
}

} // namespace detail

// Builds a single baked mesh of grass tufts + occasional flowers for the chunk
// covering world [cx*cw,(cx+1)*cw] x [cz*cw,(cz+1)*cw]. Vegetation grows on gentle,
// above-water, non-desert ground; denser and lusher where the world is wetter.
// Placement is on a global cell grid, so neighbouring chunks tile seamlessly.
inline MeshData build_vegetation(int cx, int cz, f32 chunk_world, u32 seed) {
    MeshData m;
    constexpr f32 cell = 0.7f;
    const f32 x0 = static_cast<f32>(cx) * chunk_world;
    const f32 z0 = static_cast<f32>(cz) * chunk_world;
    const int gx0 = static_cast<int>(std::floor(x0 / cell));
    const int gz0 = static_cast<int>(std::floor(z0 / cell));
    const int n = std::max(1, static_cast<int>(std::lround(chunk_world / cell)));

    static const Vec3 blossoms[] = {{0.92f, 0.28f, 0.32f}, {0.95f, 0.82f, 0.28f},
                                    {0.97f, 0.97f, 0.99f}, {0.72f, 0.45f, 0.90f},
                                    {0.96f, 0.60f, 0.78f}};

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int gx = gx0 + i;
            const int gz = gz0 + j;
            const f32 jx = (detail::hash01(detail::tree_hash(gx, gz, seed + 5001u)) - 0.5f) * cell * 0.9f;
            const f32 jz = (detail::hash01(detail::tree_hash(gx, gz, seed + 5002u)) - 0.5f) * cell * 0.9f;
            const f32 wx = (static_cast<f32>(gx) + 0.5f) * cell + jx;
            const f32 wz = (static_cast<f32>(gz) + 0.5f) * cell + jz;

            const f32 gh = worldgen::height(wx, wz, seed);
            if (gh < worldgen::water_level + 0.6f) {
                continue; // underwater or on the waterline
            }
            const f32 moist = worldgen::moisture(wx, wz, seed);
            if (moist < -0.08f) {
                continue; // dry desert: bare sand
            }
            const f32 slope = std::abs(worldgen::height(wx + 1.0f, wz, seed) - gh) +
                              std::abs(worldgen::height(wx, wz + 1.0f, seed) - gh);
            if (slope > 2.4f) {
                continue; // too steep
            }
            const f32 density = 0.32f + glm::clamp(moist, 0.0f, 0.7f) * 0.5f;
            if (detail::hash01(detail::tree_hash(gx, gz, seed + 5003u)) > density) {
                continue;
            }

            // Lush green where wet, dry tan where arid.
            const Vec3 dry{0.55f, 0.50f, 0.26f};
            const Vec3 lush{0.27f, 0.55f, 0.25f};
            const Vec3 g = glm::mix(dry, lush, glm::smoothstep(-0.05f, 0.32f, moist));
            const f32 shade = 0.88f + detail::hash01(detail::tree_hash(gx, gz, seed + 5004u)) * 0.24f;
            const f32 sc = 0.75f + detail::hash01(detail::tree_hash(gx, gz, seed + 5005u)) * 0.7f;
            const f32 sy = 0.8f + detail::hash01(detail::tree_hash(gx, gz, seed + 5006u)) * 0.5f;
            const f32 yaw = detail::hash01(detail::tree_hash(gx, gz, seed + 5007u)) * TwoPi;
            const Mat4 xf = glm::translate(Mat4{1.0f}, Vec3{wx, gh, wz}) *
                            glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                            glm::scale(Mat4{1.0f}, Vec3{sc, sc * sy, sc});
            detail::append_transformed(m, primitives::grass_tuft(4, g), xf, Vec3{shade});

            // Occasional wildflower on wetter ground.
            if (moist > 0.04f &&
                detail::hash01(detail::tree_hash(gx, gz, seed + 5008u)) < 0.08f) {
                const u32 bi = detail::tree_hash(gx, gz, seed + 5009u) % 5u;
                const f32 fsc = 0.9f + detail::hash01(detail::tree_hash(gx, gz, seed + 5010u)) * 0.5f;
                const f32 fyaw = detail::hash01(detail::tree_hash(gx, gz, seed + 5011u)) * TwoPi;
                const Mat4 fxf = glm::translate(Mat4{1.0f}, Vec3{wx, gh, wz}) *
                                 glm::rotate(Mat4{1.0f}, fyaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{fsc});
                detail::append_transformed(m, primitives::flower(blossoms[bi]), fxf, Vec3{1.0f});
            }
        }
    }
    return m;
}

} // namespace alryn
