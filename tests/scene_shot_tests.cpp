#include <doctest/doctest.h>

#include "support/OffscreenRenderer.h"

#include <Alryn/Core/Paths.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/Village.h>

#include <optional>
#include <string>
#include <vector>

using namespace alryn;

// Visual-confirmation "screenshots" rendered headlessly. They double as smoke
// tests (assert the scene actually drew geometry) and leave PPM artifacts next to
// the test binary for eyeballing / CI upload. They skip cleanly with no GPU/shaders
// (e.g. on CI), so they never fail the build for environmental reasons.

namespace {
Vec3 pixel(const std::vector<u8>& px, u32 w, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * w + x) * 4;
    return Vec3{px[i] / 255.0f, px[i + 1] / 255.0f, px[i + 2] / 255.0f};
}
} // namespace

TEST_CASE("Scene shot: forest sampler (trees, logs, ferns, mushrooms) renders") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(640, 400)) {
        MESSAGE("No Vulkan device/shaders - skipping forest scene shot");
        return;
    }

    PropLibrary lib;
    std::vector<test::OffscreenRenderer::Draw> draws;
    auto add = [&](const MeshData& data, const Vec3& pos, f32 scale = 1.0f, f32 yaw = 0.0f) {
        if (Mesh* m = renderer.upload(data)) {
            const Mat4 model = glm::translate(Mat4{1.0f}, pos) *
                               glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                               glm::scale(Mat4{1.0f}, Vec3{scale});
            draws.push_back({m, model, Vec4{1.0f}});
        }
    };

    add(primitives::grid(20, 1.6f, Vec3{0.22f, 0.40f, 0.20f}), Vec3{0.0f}); // forest floor

    // Five tree variants in a row (trunk + foliage drawn opaque here).
    for (int v = 0; v < 5; ++v) {
        const primitives::TreeMeshData td = primitives::tree(v);
        const Vec3 pos{static_cast<f32>(v - 2) * 3.6f, 0.0f, -4.0f};
        add(td.trunk, pos, 1.4f);
        add(td.foliage, pos, 1.4f);
    }

    // Forest-floor sampler across the foreground.
    add(lib.logs()[0].parts[0].mesh, Vec3{-5.0f, 0.0f, 1.0f}, 1.0f, 0.5f);
    add(lib.bushes()[0].parts[0].mesh, Vec3{-2.8f, 0.0f, 1.2f}, 1.2f);
    add(lib.rocks()[0].parts[0].mesh, Vec3{-1.0f, 0.0f, 1.4f}, 1.0f);
    add(primitives::fern(0), Vec3{0.6f, 0.0f, 1.4f}, 1.6f);
    add(primitives::mushroom(Vec3{0.74f, 0.16f, 0.13f}, 1.6f, true), Vec3{1.8f, 0.0f, 1.6f});
    add(primitives::tall_grass(7, Vec3{0.32f, 0.52f, 0.26f}), Vec3{2.8f, 0.0f, 1.5f}, 1.4f);
    add(primitives::ground_leaf(0), Vec3{3.8f, 0.0f, 1.4f}, 1.6f);
    REQUIRE_FALSE(draws.empty());

    const Vec3 target{0.0f, 1.5f, -1.0f};
    const Vec3 eye{0.0f, 4.0f, 9.0f};
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = perspective(radians(48.0f),
                                  static_cast<f32>(renderer.width()) / renderer.height(), 0.1f, 100.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const std::string path = (executable_dir() / "forest.ppm").string();
    const std::vector<u8> px =
        renderer.render(draws, view, proj, sky, glm::normalize(Vec3{0.4f, 0.9f, 0.5f}), path);

    // Smoke check: the lower-centre of the frame is forest geometry, not sky.
    const Vec3 ground = pixel(px, renderer.width(), renderer.width() / 2, renderer.height() * 3 / 4);
    CHECK(glm::length(ground - sky) > 0.05f);
    const std::string wrote = "Wrote " + path;
    MESSAGE(wrote);
}

TEST_CASE("Scene shot: medieval village houses render") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(900, 420)) {
        MESSAGE("No Vulkan device/shaders - skipping village scene shot");
        return;
    }

    PropLibrary lib;
    std::vector<test::OffscreenRenderer::Draw> draws;
    auto add = [&](const MeshData& data, const Vec3& pos) {
        if (Mesh* m = renderer.upload(data)) {
            draws.push_back({m, glm::translate(Mat4{1.0f}, pos), Vec4{1.0f}});
        }
    };

    add(primitives::grid(24, 2.0f, Vec3{0.40f, 0.33f, 0.24f}), Vec3{0.0f}); // trampled town ground

    // A row of representative house styles: thatched daub cottage, stone+terracotta
    // townhouse, timber two-storey with shingles, and a big thatched manor.
    const int variants[] = {0, 2, 3, 4};
    for (int i = 0; i < 4; ++i) {
        const PropDef& house = lib.houses()[variants[i]];
        const Vec3 pos{static_cast<f32>(i) * 8.0f - 12.0f, 0.0f, 0.0f};
        for (const PropPart& part : house.parts) {
            if (part.layer == PropLayer::Emissive) {
                continue; // skip the interior glow for an exterior shot
            }
            add(part.mesh, pos);
        }
    }
    REQUIRE_FALSE(draws.empty());

    const Vec3 target{0.0f, 2.2f, 0.0f};
    const Vec3 eye{2.0f, 6.5f, 16.0f};
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = perspective(radians(46.0f),
                                  static_cast<f32>(renderer.width()) / renderer.height(), 0.1f, 200.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const std::string path = (executable_dir() / "village.ppm").string();
    const std::vector<u8> px =
        renderer.render(draws, view, proj, sky, glm::normalize(Vec3{0.4f, 0.9f, 0.5f}), path);

    const Vec3 mid = pixel(px, renderer.width(), renderer.width() / 2, renderer.height() / 2);
    CHECK(glm::length(mid - sky) > 0.05f); // a house, not empty sky
    const std::string wrote = "Wrote " + path;
    MESSAGE(wrote);
}

TEST_CASE("Scene shot: a medieval town overview (walls, houses, market, lanterns)") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 720)) {
        MESSAGE("No Vulkan device/shaders - skipping town overview shot");
        return;
    }

    const u32 seed = 4242u;
    // Pick the best-connected town within reach (most road gates, then largest) so the
    // overview shows the multi-gate layout + a populated plaza.
    std::optional<worldgen::Village> town;
    int town_gates = 0;
    for (int vz = -6; vz <= 6; ++vz) {
        for (int vx = -6; vx <= 6; ++vx) {
            const auto v = worldgen::village_at(vx, vz, seed);
            if (!v) {
                continue;
            }
            const int g = static_cast<int>(village_gates(*v, seed).size());
            if (!town || g > town_gates || (g == town_gates && v->half > town->half)) {
                town = v;
                town_gates = g;
            }
        }
    }
    REQUIRE(town.has_value());
    const std::string gates_msg = std::to_string(town_gates) + " road gates on this town";
    MESSAGE(gates_msg);

    PropLibrary lib;
    std::vector<test::OffscreenRenderer::Draw> draws;
    const Vec2 ctr = town->center;
    auto add = [&](const MeshData& data, const Mat4& model) {
        if (Mesh* m = renderer.upload(data)) {
            draws.push_back({m, model, Vec4{1.0f}});
        }
    };

    add(primitives::grid(40, town->half * 0.06f, Vec3{0.42f, 0.35f, 0.26f}), Mat4{1.0f}); // town ground

    for (const PropInstance& p : village_props(*town, seed)) {
        const PropDef& def = lib.resolve(p);
        const Mat4 model =
            glm::translate(Mat4{1.0f},
                           Vec3{p.position.x - ctr.x, p.position.y - town->ground, p.position.z - ctr.y}) *
            glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
            glm::scale(Mat4{1.0f}, Vec3{p.scale});
        for (const PropPart& part : def.parts) {
            if (part.layer == PropLayer::Glow) {
                continue;
            }
            add(part.mesh, model);
        }
    }
    // A goods wagon parked by the market (the transport entity): body + four wheels.
    {
        const Mat4 wm = glm::translate(Mat4{1.0f}, Vec3{6.0f, 0.0f, 4.0f}) *
                        glm::rotate(Mat4{1.0f}, 0.6f, Vec3{0.0f, 1.0f, 0.0f});
        add(PropLibrary::build_wagon().parts[0].mesh, wm);
        const MeshData wheel = PropLibrary::build_wagon_wheel().parts[0].mesh;
        for (const f32 sx : {-kWagonWheelX, kWagonWheelX}) {
            for (const f32 sz : {-kWagonWheelZ, kWagonWheelZ}) {
                add(wheel, wm * glm::translate(Mat4{1.0f}, Vec3{sx, kWagonWheelRadius, sz}));
            }
        }
    }
    REQUIRE_FALSE(draws.empty());

    const f32 hh = town->half;
    const Vec3 target{0.0f, 1.0f, 0.0f};
    const Vec3 eye{hh * 0.2f, hh * 1.5f, hh * 1.7f};
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = perspective(radians(50.0f),
                                  static_cast<f32>(renderer.width()) / renderer.height(), 0.2f, 600.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const std::string path = (executable_dir() / "town.ppm").string();
    const std::vector<u8> px =
        renderer.render(draws, view, proj, sky, glm::normalize(Vec3{0.4f, 0.9f, 0.5f}), path);

    const Vec3 mid = pixel(px, renderer.width(), renderer.width() / 2, renderer.height() * 3 / 5);
    CHECK(glm::length(mid - sky) > 0.05f); // town geometry, not empty sky
    const std::string wrote = "Wrote " + path;
    MESSAGE(wrote);
}
