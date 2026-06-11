#include <doctest/doctest.h>

#include "support/OffscreenRenderer.h"

#include <Alryn/Core/Paths.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/World/PropLibrary.h>

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
