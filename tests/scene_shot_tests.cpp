#include <doctest/doctest.h>

#include "support/OffscreenRenderer.h"

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/ClothRig.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/OutfitMesh.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Character/Weapon.h>
#include <Alryn/Core/Paths.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/Terrain/VegetationScatter.h>
#include <Alryn/Terrain/VoxelField.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/Terrain/WorldSampler.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/VehicleTypes.h>
#include <Alryn/World/Village.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
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

// Builds a faithful slice of the LIVE world (the real terrain meshed from the density
// function with the in-game road/town colouring, plus the same scattered props, trees and
// ground vegetation) over a square region and renders it from an aerial 3/4 view to `out`.
// Coarser voxels than the game (1 m) keep the headless mesh cheap; the look is the same.
// Everything is translated by -focus so coordinates stay small and the camera frames it.
void render_world(test::OffscreenRenderer& r, u32 seed, const Vec2& focus, f32 radius,
                  const std::string& out, f32 cam_height_mul = 1.95f, f32 cam_back_mul = 0.85f,
                  const Vec3* eye_rel = nullptr, const Vec3* target_rel = nullptr,
                  bool with_wagon = false, f32 wagon_yaw = 0.0f, bool with_surf = false,
                  bool dollhouse = false) {
    constexpr f32 voxel = 1.0f;
    constexpr int cv = 16;                 // voxels per chunk
    constexpr f32 cw = static_cast<f32>(cv) * voxel; // chunk world size (16 m)
    constexpr f32 y_min = -14.0f, y_max = 50.0f; // covers the tall mountains (matches the game band)
    const int yv = static_cast<int>((y_max - y_min) / voxel);

    WorldSampler sampler(seed);
    const DensitySampler density = sampler.snapshot();
    PropLibrary lib;

    std::vector<test::OffscreenRenderer::Draw> draws;
    std::vector<test::OffscreenRenderer::Draw> water_draws;
    std::vector<test::OffscreenRenderer::Draw> trans_draws;
    auto add = [&](const MeshData& d, const Mat4& model, const Vec4& tint = Vec4{1.0f}) {
        if (!d.indices.empty()) {
            if (Mesh* m = r.upload(d)) {
                draws.push_back({m, model, tint});
            }
        }
    };
    const Vec3 shift{-focus.x, 0.0f, -focus.y};
    auto at = [&](const Vec3& world) { return glm::translate(Mat4{1.0f}, world + shift); };

    const int c0x = static_cast<int>(std::floor((focus.x - radius) / cw));
    const int c1x = static_cast<int>(std::floor((focus.x + radius) / cw));
    const int c0z = static_cast<int>(std::floor((focus.y - radius) / cw));
    const int c1z = static_cast<int>(std::floor((focus.y + radius) / cw));

    for (int cz = c0z; cz <= c1z; ++cz) {
        for (int cx = c0x; cx <= c1x; ++cx) {
            const Vec3 origin{static_cast<f32>(cx) * cw, y_min, static_cast<f32>(cz) * cw};
            VoxelField field(IVec3{cv + 1, yv + 1, cv + 1}, voxel, origin);
            field.fill([&](const Vec3& wp) { return density(wp); });
            const MeshData terrain = mc::polygonize(
                field, IVec3{0}, field.cell_count(), 0.0f,
                [&](const Vec3& p, const Vec3& n) {
                    const f32 up = glm::clamp(n.y, 0.0f, 1.0f);
                    Vec3 c = roads::tint_surface(worldgen::surface_color(p, n, seed), p, up, seed);
                    return town_path_tint(c, p, up, seed);
                },
                [&](const Vec3& p, const Vec3& n) {
                    return town_pave_amount(p, glm::clamp(n.y, 0.0f, 1.0f), seed);
                });
            add(terrain, at(Vec3{0.0f}));
            add(build_vegetation(cx, cz, cw, seed), at(Vec3{0.0f}));

            for (const TreeInstance& t : scatter_trees(cx, cz, cw, seed)) {
                const primitives::TreeMeshData tm = primitives::tree(t.variant);
                const Mat4 model = at(t.position) *
                                   glm::rotate(Mat4{1.0f}, t.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                   glm::scale(Mat4{1.0f}, Vec3{t.scale});
                add(tm.trunk, model);
                add(tm.foliage, model, Vec4{t.tint, 1.0f});
            }
            for (const PropInstance& p : scatter_props(cx, cz, cw, seed)) {
                const PropDef& def = lib.resolve(p);
                const Mat4 model = at(p.position) *
                                   glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                   glm::scale(Mat4{1.0f}, Vec3{p.scale * p.length, p.scale, p.scale});
                for (const PropPart& part : def.parts) {
                    if (part.layer == PropLayer::Glow) {
                        continue;
                    }
                    // The dollhouse view: the client fades a house's roof shell (walls + roof +
                    // upper floors) to a ghost while you stand inside, so the interior shows.
                    if (dollhouse && part.layer == PropLayer::Roof) {
                        if (Mesh* m = r.upload(part.mesh)) {
                            trans_draws.push_back({m, model, Vec4{1.0f, 1.0f, 1.0f, 0.18f}});
                        }
                        continue;
                    }
                    const Vec4 tint = part.layer == PropLayer::Emissive ? Vec4{1.6f, 1.5f, 1.2f, 1.0f}
                                                                        : Vec4{1.0f};
                    add(part.mesh, model, tint);
                }
            }
        }
    }
    // The cargo wagon sitting on the road at the focus (the transport entity in its landscape).
    if (with_wagon) {
        const f32 gy = worldgen::height(focus.x, focus.y, seed);
        // The wagon mesh faces local +X; the client renders it with rotate(-yaw) to face travel.
        const Mat4 wm = glm::translate(Mat4{1.0f}, Vec3{0.0f, gy, 0.0f}) *
                        glm::rotate(Mat4{1.0f}, -wagon_yaw, Vec3{0.0f, 1.0f, 0.0f});
        add(PropLibrary::build_wagon().parts[0].mesh, wm);
        const MeshData wheel = PropLibrary::build_wagon_wheel().parts[0].mesh;
        for (const f32 sx : {-kWagonWheelX, kWagonWheelX}) {
            for (const f32 sz : {-kWagonWheelZ, kWagonWheelZ}) {
                add(wheel, wm * glm::translate(Mat4{1.0f}, Vec3{sx, kWagonWheelRadius, sz}));
            }
        }
    }
    // Stone / wooden bridges where the roads cross rivers (deck stretched to the span, pitched to
    // meet each bank flush; kind picks the style).
    {
        const MeshData stone = PropLibrary::build_arch_bridge().parts[0].mesh;
        const MeshData wood = PropLibrary::build_plank_bridge().parts[0].mesh;
        for (const roads::Bridge& b : roads::bridges(focus, radius + 24.0f, seed)) {
            const f32 base_y = (b.bank_a + b.bank_b) * 0.5f;
            const f32 pitch = std::asin(glm::clamp((b.bank_b - b.bank_a) / b.length, -0.6f, 0.6f));
            add(b.kind == 1 ? wood : stone,
                at(Vec3{b.center.x, base_y, b.center.y}) *
                    glm::rotate(Mat4{1.0f}, -b.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                    glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
                    glm::scale(Mat4{1.0f}, Vec3{b.length, 1.0f, 1.0f}));
        }
    }
    // Water + shore surf. For water scenes we lay the REAL reflective water plane (a wave grid at
    // the waterline, drawn with the game's water.* shaders - depth-tested so land above the line
    // hides it) and the client's draw_surf foam (thin alpha streaks ALONG the waterline) so the
    // shot shows water + foam exactly as the game does.
    if (with_surf) {
        const f32 wext = radius * 2.0f + 16.0f;
        if (Mesh* wmesh = r.upload(primitives::grid(72, wext / 72.0f, Vec3{0.1f, 0.3f, 0.4f}))) {
            water_draws.push_back(
                {wmesh, glm::translate(Mat4{1.0f}, Vec3{0.0f, worldgen::water_level, 0.0f}),
                 Vec4{1.0f}});
        }
        Mesh* foam = r.upload(primitives::grid(1, 1.0f, Vec3{0.96f, 0.99f, 1.0f})); // flat up-quad
        const f32 t = 2.0f; // a fixed wave phase
        constexpr f32 scell = 1.5f;
        const int sx0 = static_cast<int>(std::floor((focus.x - radius) / scell));
        const int sx1 = static_cast<int>(std::floor((focus.x + radius) / scell));
        const int sz0 = static_cast<int>(std::floor((focus.y - radius) / scell));
        const int sz1 = static_cast<int>(std::floor((focus.y + radius) / scell));
        for (int gz = sz0; gz <= sz1; ++gz) {
            for (int gx = sx0; gx <= sx1; ++gx) {
                const f32 wx = static_cast<f32>(gx) * scell + scell * 0.5f;
                const f32 wz = static_cast<f32>(gz) * scell + scell * 0.5f;
                const f32 gh = worldgen::height(wx, wz, seed);
                if (gh < worldgen::water_level - 0.22f || gh > worldgen::water_level + 0.2f) continue;
                const f32 hl = worldgen::height(wx - scell, wz, seed);
                const f32 hr = worldgen::height(wx + scell, wz, seed);
                const f32 hu = worldgen::height(wx, wz - scell, seed);
                const f32 hd = worldgen::height(wx, wz + scell, seed);
                const f32 lo = std::min(std::min(hl, hr), std::min(hu, hd));
                const f32 hi = std::max(std::max(hl, hr), std::max(hu, hd));
                if (lo > worldgen::water_level - 0.18f || hi < worldgen::water_level + 0.04f) continue;
                const Vec2 grad{hr - hl, hd - hu};
                if (glm::length(grad) < 1e-3f) continue;
                const Vec2 up_slope = glm::normalize(grad);
                const Vec2 tangent{up_slope.y, -up_slope.x};
                const f32 along = glm::dot(Vec2{wx, wz}, tangent);
                const f32 wave = 0.5f + 0.5f * std::sin(t * 1.4f + along * 0.5f);
                const f32 crest = glm::smoothstep(0.2f, 1.0f, wave);
                const Vec2 p = Vec2{wx, wz} + up_slope * (crest * 0.4f - 0.1f);
                const f32 ang = std::atan2(-tangent.y, tangent.x);
                const f32 len = 1.5f, wid = 0.26f + crest * 0.18f;
                const f32 a = 0.4f + 0.45f * crest;
                if (foam != nullptr) {
                    trans_draws.push_back({foam,
                                           at(Vec3{p.x, worldgen::water_level + 0.05f, p.y}) *
                                               glm::rotate(Mat4{1.0f}, ang, Vec3{0.0f, 1.0f, 0.0f}) *
                                               glm::scale(Mat4{1.0f}, Vec3{len, 1.0f, wid}),
                                           Vec4{0.96f, 0.99f, 1.0f, a}});
                }
            }
        }
    }
    REQUIRE_FALSE(draws.empty());

    const Vec3 target = target_rel ? *target_rel : Vec3{0.0f, 0.0f, 0.0f};
    const Vec3 eye =
        eye_rel ? *eye_rel : Vec3{radius * 0.28f, radius * cam_height_mul, radius * cam_back_mul};
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj =
        perspective(radians(48.0f), static_cast<f32>(r.width()) / r.height(), 0.5f, 2000.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    r.render(draws, view, proj, sky, glm::normalize(Vec3{0.45f, 0.85f, 0.4f}), out, Vec4{1.0f},
             water_draws, trans_draws);
    const std::string wrote = "Wrote " + out;
    MESSAGE(wrote);
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
    // Long meadow grass: a fresh-green clump and a dried straw-yellow one.
    add(primitives::meadow_grass(11, Vec3{0.30f, 0.55f, 0.24f}), Vec3{-3.6f, 0.0f, 2.4f}, 1.6f);
    add(primitives::meadow_grass(11, Vec3{0.76f, 0.67f, 0.31f}), Vec3{4.3f, 0.0f, 2.4f}, 1.6f, 1.0f);
    // A dense THICKET: a tight cluster of overlapping clumps that reads as thick grass.
    const Vec3 thicket_off[] = {{0, 0, 0}, {0.3f, 0, 0.2f}, {-0.3f, 0, 0.15f}, {0.1f, 0, -0.25f},
                                {-0.18f, 0, -0.18f}, {0.28f, 0, -0.05f}};
    for (int k = 0; k < 6; ++k) {
        add(primitives::meadow_grass(11, Vec3{0.31f, 0.54f, 0.23f}),
            Vec3{0.2f, 0.0f, 2.7f} + thicket_off[k], 1.5f, static_cast<f32>(k) * 1.05f);
    }
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

TEST_CASE("Scene shot: the vehicle line-up (cart, wagon, carriage + horse) renders") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 420)) {
        MESSAGE("No Vulkan device/shaders - skipping vehicle line-up shot");
        return;
    }

    std::vector<test::OffscreenRenderer::Draw> draws;
    auto add = [&](const MeshData& data, const Mat4& model) {
        if (Mesh* m = renderer.upload(data)) {
            draws.push_back({m, model, Vec4{1.0f}});
        }
    };

    add(primitives::grid(28, 1.6f, Vec3{0.40f, 0.34f, 0.25f}), Mat4{1.0f}); // depot ground

    const MeshData wheel = PropLibrary::build_wagon_wheel().parts[0].mesh;
    // A thin link mesh (y = 0..1) for the harness rope, oriented between two points like the
    // client's verlet trace (here drawn as a static sag for the still shot).
    const MeshData link = primitives::box(Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 1.0f, 0.5f},
                                          Vec3{0.16f, 0.11f, 0.06f});
    auto add_link = [&](const Vec3& a, const Vec3& b) {
        const Vec3 d = b - a;
        const f32 L = glm::length(d);
        if (L < 1e-4f) {
            return;
        }
        const Vec3 up = d / L;
        const Vec3 ref = std::abs(up.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 bx = glm::normalize(glm::cross(ref, up));
        const Vec3 bz = glm::cross(up, bx);
        Mat4 m{1.0f};
        m[0] = Vec4{bx * 0.04f, 0.0f};
        m[1] = Vec4{up * L, 0.0f};
        m[2] = Vec4{bz * 0.04f, 0.0f};
        m[3] = Vec4{a, 1.0f};
        add(link, m);
    };

    // Each vehicle type in a row, facing local +X (toward the right of frame), with its own
    // wheel count/size + lamp baked into the body mesh.
    for (u8 t = 0; t < vehicle_type_count(); ++t) {
        const VehicleType& vt = vehicle_type(t);
        const Mat4 base = glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, static_cast<f32>(t) * 4.5f - 4.5f});
        add(vt.body(), base);
        const f32 wscale = vt.wheel_radius() / kWagonWheelRadius;
        for (const Vec3& off : vt.wheels()) {
            add(wheel, base * glm::translate(Mat4{1.0f}, off) * glm::scale(Mat4{1.0f}, Vec3{wscale}));
        }
        // A few physical cargo crates resting in the bed (local bed coords -> world via base).
        const CargoBed bd = vt.bed();
        const MeshData crate = primitives::box(Vec3{-0.2f, 0.0f, -0.2f}, Vec3{0.2f, 0.4f, 0.2f},
                                               Vec3{0.55f, 0.40f, 0.22f});
        for (const f32 fx : {0.25f, 0.55f, 0.78f}) {
            for (const f32 fz : {0.32f, 0.68f}) {
                const Vec3 local{glm::mix(bd.lo.x + 0.2f, bd.hi.x - 0.2f, fx), bd.lo.y,
                                 glm::mix(bd.lo.z + 0.2f, bd.hi.z - 0.2f, fz)};
                add(crate, base * glm::translate(Mat4{1.0f}, local));
            }
        }
        // The carriage's horse stands out in front along its heading, joined by two harness
        // ropes from the shaft tips to the horse's collar (the same endpoints update_ropes uses).
        if (vt.horse_drawn()) {
            const Vec3 cpos = Vec3{base[3]};
            const Mat4 hb = base * glm::translate(Mat4{1.0f}, Vec3{vt.reach() + 1.8f, 0.0f, 0.0f});
            const Vec3 hpos = Vec3{hb[3]};
            add(build_horse_body(), hb);
            for (const Vec3& leg : kHorseLegs) {
                add(build_horse_leg(), hb * glm::translate(Mat4{1.0f}, leg));
            }
            for (const f32 s : {1.0f, -1.0f}) {
                const Vec3 A = cpos + Vec3{2.4f, 0.75f, 0.12f * s};   // shaft tip
                const Vec3 B = hpos + Vec3{0.45f, 1.05f, 0.18f * s};  // horse collar
                constexpr int n = 6;
                Vec3 prev = A;
                for (int i = 1; i <= n; ++i) {
                    const f32 u = static_cast<f32>(i) / static_cast<f32>(n);
                    Vec3 p = glm::mix(A, B, u);
                    p.y -= 0.18f * 4.0f * u * (1.0f - u); // gravity sag (catenary-ish)
                    add_link(prev, p);
                    prev = p;
                }
            }
        }
    }
    REQUIRE_FALSE(draws.empty());

    const Vec3 target{1.0f, 1.2f, 0.0f};
    const Vec3 eye{-6.0f, 5.0f, 10.0f};
    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = perspective(radians(50.0f),
                                  static_cast<f32>(renderer.width()) / renderer.height(), 0.1f, 200.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const std::string path = (executable_dir() / "vehicles.ppm").string();
    const std::vector<u8> px =
        renderer.render(draws, view, proj, sky, glm::normalize(Vec3{0.4f, 0.9f, 0.5f}), path);

    const Vec3 mid = pixel(px, renderer.width(), renderer.width() / 2, renderer.height() / 2);
    CHECK(glm::length(mid - sky) > 0.05f); // a vehicle, not empty sky
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

TEST_CASE("Scene shot: real-world towns + the roads between them") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(1100, 760)) {
        MESSAGE("No Vulkan device/shaders - skipping world town shots");
        return;
    }
    const u32 seed = 4242u;

    // Collect a few real towns and a road-connected pair (to frame the road between them).
    std::vector<worldgen::Village> towns;
    std::optional<worldgen::Village> pa, pb;
    for (int vz = -7; vz <= 7; ++vz) {
        for (int vx = -7; vx <= 7; ++vx) {
            const auto v = worldgen::village_at(vx, vz, seed);
            if (!v) {
                continue;
            }
            towns.push_back(*v);
            for (int dz = -roads::road_max_cells; dz <= roads::road_max_cells && !pb; ++dz) {
                for (int dx = -roads::road_max_cells; dx <= roads::road_max_cells && !pb; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const auto v2 = worldgen::village_at(vx + dx, vz + dz, seed);
                    if (v2 && !roads::route_polyline(v->center, v2->center, seed).empty()) {
                        pa = v;
                        pb = v2;
                    }
                }
            }
        }
    }
    REQUIRE_FALSE(towns.empty());

    // A close overview of three individual towns (varied sizes/shapes).
    std::sort(towns.begin(), towns.end(),
              [](const worldgen::Village& a, const worldgen::Village& b) { return a.half > b.half; });
    const int shots = std::min<int>(3, static_cast<int>(towns.size()));
    for (int i = 0; i < shots; ++i) {
        const worldgen::Village& t = towns[static_cast<usize>(i)];
        render_world(renderer, seed, t.center, t.half + 16.0f,
                     (executable_dir() / ("world_town" + std::to_string(i) + ".ppm")).string());
    }

    // A wide shot framing two connected towns with the road running between them.
    if (pa && pb) {
        // Frame the road as it leaves town A's gate toward town B: focus a little way out on
        // the road, camera low and behind looking along it, so the cobble->dirt road and its
        // cleared forest verge read clearly.
        const Vec2 d2 = glm::normalize(pb->center - pa->center);
        const Vec2 fc = pa->center + d2 * (pa->half + 22.0f);
        const f32 span = 46.0f;
        const Vec3 dir{d2.x, 0.0f, d2.y};
        const Vec3 eye = -dir * (span * 0.95f) + Vec3{span * 0.15f, span * 0.85f, 0.0f};
        const Vec3 tgt = dir * (span * 0.35f) + Vec3{0.0f, 2.0f, 0.0f};
        render_world(renderer, seed, fc, span, (executable_dir() / "world_road.ppm").string(),
                     1.0f, 1.0f, &eye, &tgt);
    }
}

// House interiors in a real town, seen the way the player does on stepping inside: the roof shell
// faded to a ghost (the client's dollhouse view) from the iso camera, so the floor, hearth, bed and
// table read - and anything poking up through the floor (terrain, grass) would show.
TEST_CASE("Scene shot: house interiors in a real town (dollhouse view)") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 600)) {
        MESSAGE("No Vulkan device/shaders - skipping house interior shots");
        return;
    }
    const u32 seed = 4242u;
    std::optional<worldgen::Village> town;
    for (int vz = -6; vz <= 6 && !town; ++vz) {
        for (int vx = -6; vx <= 6 && !town; ++vx) {
            town = worldgen::village_at(vx, vz, seed);
        }
    }
    REQUIRE(town.has_value());
    int shots = 0;
    for (const PropInstance& p : village_props(*town, seed)) {
        if (p.category != PropCategory::House || p.variant >= kHouseVariants || shots >= 3) {
            continue;
        }
        // Iso camera in FRONT of the house (its local +z), ~50 deg down onto the floor.
        const Vec3 front{std::sin(p.yaw), 0.0f, std::cos(p.yaw)};
        const Vec3 side{front.z, 0.0f, -front.x};
        const f32 gy = p.position.y;
        const Vec3 eye = front * 8.5f + side * 3.0f + Vec3{0.0f, gy + 11.0f, 0.0f};
        const Vec3 tgt{0.0f, gy + 0.3f, 0.0f};
        render_world(renderer, seed, Vec2{p.position.x, p.position.z}, 12.0f,
                     (executable_dir() / ("house_interior" + std::to_string(shots) + ".ppm")).string(),
                     1.0f, 1.0f, &eye, &tgt, false, 0.0f, false, /*dollhouse=*/true);
        ++shots;
    }
    CHECK(shots > 0);
}

// Wagon-on-road vistas across the biomes the roads cross (forest / plains / desert / bog /
// mountains), framed low along the road so the cart sits in its landscape. These are the baseline
// aesthetic shots to compare against reference art (`make shots` -> wagon_<biome>.png).
TEST_CASE("Scene shot: the wagon on roads across biomes") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 600)) {
        MESSAGE("No Vulkan device/shaders - skipping wagon vista shots");
        return;
    }
    const u32 seed = 1337u;

    // Collect one OPEN-ROAD point per biome (out in the wilderness between towns, so the cart sits
    // in the biome's landscape, not on town ground) by walking the road segments over a wide area.
    std::map<worldgen::Biome, Vec2> spots;
    for (const roads::Segment& s : roads::gather(Vec2{0.0f, 0.0f}, 1600.0f, seed)) {
        const Vec2 mid = (s.a + s.b) * 0.5f;
        if (worldgen::height(mid.x, mid.y, seed) < worldgen::water_level + 0.6f) {
            continue;
        }
        if (worldgen::inside_village(mid.x, mid.y, seed, 12.0f)) {
            continue; // keep clear of towns - we want the open road in each biome
        }
        const worldgen::Biome b = worldgen::biome_at(mid.x, mid.y, seed);
        spots.emplace(b, mid); // first open-road point found in each biome
    }
    REQUIRE_FALSE(spots.empty());

    int shot = 0;
    for (const auto& [biome, p] : spots) {
        const Vec2 tan = roads::tangent(p.x, p.y, seed);
        const f32 wagon_yaw = std::atan2(tan.y, tan.x);
        const Vec3 dir{tan.x, 0.0f, tan.y};
        const f32 span = 17.0f;
        // The wagon sits at the terrain height (render_world only shifts in xz), so anchor the
        // camera to that ground height too - otherwise on a mountain the camera ends up under it.
        const f32 gy = worldgen::height(p.x, p.y, seed);
        // A low, close 3/4 camera behind + beside the cart, looking along the road so the wagon is
        // prominent in the foreground with the biome stretching out behind it.
        const Vec3 eye = -dir * (span * 0.55f) + Vec3{span * 0.34f, gy + span * 0.46f, span * 0.18f};
        const Vec3 tgt = dir * (span * 0.25f) + Vec3{0.0f, gy + 1.1f, 0.0f};
        const std::string name =
            std::string("wagon_") + worldgen::biome_name(biome) + ".ppm";
        std::string low = name;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        render_world(renderer, seed, p, span, (executable_dir() / low).string(), 1.0f, 1.0f, &eye,
                     &tgt, /*with_wagon=*/true, wagon_yaw);
        ++shot;
    }
    CHECK(shot > 0);
}

// The water's edge: a warm shoreline where the new shore stones, fringing reeds, floating lily
// pads and submerged coral cluster around the waterline. (The OffscreenRenderer draws no water
// plane, so the lily pads + reef sit "in the open" - which makes them easy to eyeball.)
TEST_CASE("Scene shot: a shoreline (stones, reeds, lily pads, coral) renders") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 600)) {
        MESSAGE("No Vulkan device/shaders - skipping shoreline shot");
        return;
    }
    // Hunt a warm coastal spot: a waterline foot with both shallow water and dry land nearby (so a
    // mix of stones/reeds on the bank and lily pads/coral out in the shallows can scatter), warmest
    // first so the tropical coral shows. Scan a few seeds.
    auto score_at = [](u32 seed, const Vec2& p) -> f32 {
        const f32 h = worldgen::height(p.x, p.y, seed);
        if (h < worldgen::water_level - 0.3f || h > worldgen::water_level + 1.0f) return -1.0f;
        int water = 0, land = 0;
        for (int a = 0; a < 8; ++a) {
            const f32 ang = TwoPi * static_cast<f32>(a) / 8.0f;
            const Vec2 q = p + Vec2{std::cos(ang), std::sin(ang)} * 9.0f;
            const f32 hh = worldgen::height(q.x, q.y, seed);
            if (hh < worldgen::water_level - 1.0f) ++water;
            if (hh > worldgen::water_level + 0.5f) ++land;
        }
        if (water == 0 || land == 0) return -1.0f; // want a real shoreline, not open sea or dry
        return worldgen::temperature(p.x, p.y, seed) * 3.0f +
               static_cast<f32>(std::min(water, 3) + std::min(land, 3));
    };
    u32 best_seed = 1337u;
    Vec2 best{0.0f};
    f32 best_score = -1.0f;
    for (const u32 seed : {1337u, 4242u, 99u, 777u, 51u}) {
        for (int j = -70; j <= 70; ++j) {
            for (int i = -70; i <= 70; ++i) {
                const Vec2 p{static_cast<f32>(i) * 8.0f, static_cast<f32>(j) * 8.0f};
                const f32 s = score_at(seed, p);
                if (s > best_score) {
                    best_score = s;
                    best = p;
                    best_seed = seed;
                }
            }
        }
        if (best_score > 6.0f) break; // good enough - a warm, well-mixed shore
    }
    REQUIRE(best_score > 0.0f);

    // Look from the land down across the waterline (downhill = toward the water).
    const f32 gy = worldgen::height(best.x, best.y, best_seed);
    const Vec2 grad{worldgen::height(best.x + 2.0f, best.y, best_seed) -
                        worldgen::height(best.x - 2.0f, best.y, best_seed),
                    worldgen::height(best.x, best.y + 2.0f, best_seed) -
                        worldgen::height(best.x, best.y - 2.0f, best_seed)};
    const Vec2 down = glm::length(grad) > 1e-4f ? -glm::normalize(grad) : Vec2{1.0f, 0.0f};
    const Vec3 d{down.x, 0.0f, down.y};
    constexpr f32 span = 22.0f;
    (void)gy;
    // Frame it from the game's ISO camera angle (~50 deg down), sitting back over the land and
    // looking out across the waterline - so the flat foam streaks show their bright tops + the
    // water reflects, the way they do in-game (a low grazing angle hid both).
    const f32 pitch = radians(50.0f);
    const Vec3 eye = Vec3{0.0f, worldgen::water_level, 0.0f} +
                     span * 1.7f * (-d * std::cos(pitch) + Vec3{0.0f, std::sin(pitch), 0.0f});
    const Vec3 tgt = d * (span * 0.35f) + Vec3{0.0f, worldgen::water_level + 0.2f, 0.0f};
    render_world(renderer, best_seed, best, span, (executable_dir() / "shore.ppm").string(), 1.0f,
                 1.0f, &eye, &tgt, /*with_wagon=*/false, /*wagon_yaw=*/0.0f, /*with_surf=*/true);
}

// Environment showcase: a curated set of scene shots framed from the GAME's iso camera angle
// (~50 deg down), each at a representative spot along the road network - open road in the wilds,
// a town, and a shoreline (water + foam). These are the reference shots to eyeball the world's
// look + feel and catch regressions (`make shots` -> env_*.png).
TEST_CASE("Scene shot: environment showcase (road / town / shore, iso framed)") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 600)) {
        MESSAGE("No Vulkan device/shaders - skipping environment showcase");
        return;
    }
    const u32 seed = 1337u;

    // The game's iso camera: ~50 deg downward, a fixed yaw, pulled back to frame `span` metres.
    auto iso = [](f32 ground_y, f32 span, f32 yaw_deg, Vec3& eye, Vec3& tgt) {
        const f32 pitch = radians(50.0f);
        const f32 yaw = radians(yaw_deg);
        const f32 dist = span * 1.8f;
        eye = Vec3{0.0f, ground_y, 0.0f} +
              dist * Vec3{std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
        tgt = Vec3{0.0f, ground_y + 1.0f, 0.0f};
    };

    // ---- Open road in the wilderness: first wilderness road midpoint above water. ----
    {
        Vec2 spot{0.0f};
        bool found = false;
        for (const roads::Segment& s : roads::gather(Vec2{0.0f, 0.0f}, 1400.0f, seed)) {
            const Vec2 mid = (s.a + s.b) * 0.5f;
            if (worldgen::height(mid.x, mid.y, seed) < worldgen::water_level + 0.8f) continue;
            if (worldgen::inside_village(mid.x, mid.y, seed, 14.0f)) continue;
            spot = mid;
            found = true;
            break;
        }
        if (found) {
            const f32 gy = worldgen::height(spot.x, spot.y, seed);
            const Vec2 tan = roads::tangent(spot.x, spot.y, seed);
            const f32 yaw = degrees(std::atan2(tan.y, tan.x)) + 28.0f; // look along the road, offset for depth
            Vec3 eye, tgt;
            iso(gy, 20.0f, yaw, eye, tgt);
            render_world(renderer, seed, spot, 20.0f, (executable_dir() / "env_road.ppm").string(),
                         1.0f, 1.0f, &eye, &tgt);
        }
    }

    // ---- A town: the best-connected town within reach, framed from above. ----
    {
        std::optional<worldgen::Village> town;
        int best_gates = -1;
        for (int cz = -3; cz <= 3; ++cz) {
            for (int cx = -3; cx <= 3; ++cx) {
                if (const auto v = worldgen::village_at(cx, cz, seed)) {
                    const int g = static_cast<int>(village_gates(*v, seed).size());
                    if (g > best_gates) {
                        best_gates = g;
                        town = v;
                    }
                }
            }
        }
        if (town) {
            Vec3 eye, tgt;
            iso(town->ground, town->half * 1.15f, 35.0f, eye, tgt);
            render_world(renderer, seed, town->center, town->half * 1.15f,
                         (executable_dir() / "env_town.ppm").string(), 1.0f, 1.0f, &eye, &tgt);
        }
    }

    // ---- A shoreline: a road point near the water's edge (so it reads as a coastal road). ----
    {
        Vec2 spot{0.0f};
        f32 best = 1e9f;
        for (const roads::Segment& s : roads::gather(Vec2{0.0f, 0.0f}, 1400.0f, seed)) {
            const Vec2 mid = (s.a + s.b) * 0.5f;
            // look for a road point with water nearby (a coastal stretch)
            f32 near_water = 1e9f;
            for (int a = 0; a < 8; ++a) {
                const f32 ang = TwoPi * static_cast<f32>(a) / 8.0f;
                const Vec2 q = mid + Vec2{std::cos(ang), std::sin(ang)} * 12.0f;
                if (worldgen::height(q.x, q.y, seed) < worldgen::water_level)
                    near_water = std::min(near_water, glm::length(q - mid));
            }
            if (near_water < best && worldgen::height(mid.x, mid.y, seed) > worldgen::water_level) {
                best = near_water;
                spot = mid;
            }
        }
        if (best < 1e8f) {
            const f32 gy = worldgen::height(spot.x, spot.y, seed);
            Vec3 eye, tgt;
            iso(gy, 22.0f, 35.0f, eye, tgt);
            render_world(renderer, seed, spot, 22.0f, (executable_dir() / "env_shore.ppm").string(),
                         1.0f, 1.0f, &eye, &tgt, false, 0.0f, /*with_surf=*/true);
        }
    }
    CHECK(true); // the shots double as smoke tests; render_world REQUIREs non-empty geometry
}

// A wagon crossing a plank bridge where a road spans a river - the new river + bridge feature.
TEST_CASE("Scene shot: a wagon crossing a river bridge") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(960, 600)) {
        MESSAGE("No Vulkan device/shaders - skipping bridge vista");
        return;
    }
    // Find a bridge (a road-over-river crossing) somewhere near the origin, across a few seeds.
    for (const u32 seed : {1337u, 4242u, 99u, 777u}) {
        const auto bs = roads::bridges(Vec2{0.0f, 0.0f}, 1500.0f, seed);
        if (bs.empty()) {
            continue;
        }
        const roads::Bridge& b = bs.front();
        const Vec2 bd{std::cos(b.yaw), std::sin(b.yaw)};
        const Vec2 e0 = b.center - bd * (b.length * 0.5f);
        const Vec2 e1 = b.center + bd * (b.length * 0.5f);
        // Frame on the BANK/deck height (not the carved river bottom), from a 3/4 aerial.
        const f32 deck_y = std::max(worldgen::height(e0.x, e0.y, seed), worldgen::height(e1.x, e1.y, seed));
        const f32 span = 24.0f;
        const Vec3 eye{span * 0.4f, deck_y + span * 0.85f, span * 0.55f};
        const Vec3 tgt{0.0f, deck_y, 0.0f};
        render_world(renderer, seed, b.center, span, (executable_dir() / "bridge.ppm").string(), 1.0f,
                     1.0f, &eye, &tgt);
        return;
    }
    MESSAGE("no bridge found near origin in the scanned seeds - skipping");
}

namespace {
// One character for a line-up shot: who they are, what they wear, where they stand + face, and
// whether they're caught mid-stride (the walk cycle + its cloth sim) or standing at rest.
struct Sitter {
    u32 seed;
    CharacterAppearance app;
    OutfitKind kind;
    Equipment eq;
    Vec3 pos;
    int role = -1;   // PlayerRole for the held weapons, -1 = none (townsfolk / bandits)
    f32 yaw = 0.0f;  // radians about +Y; 0 faces the camera (+Z)
    bool walk = false;
};

// Renders `cast` the way the client draws a character - skinned body + skinned outfit + attachment
// primitives + the role's modular weapons hung from the hand joints + the SIMULATED cloth (capes,
// robe / surcoat skirts, stoles), settled against the posed body colliders - to `file` next to the
// test binary. Returns the pixels (empty if there's no GPU).
std::vector<u8> render_cast(const std::vector<Sitter>& cast, const Vec3& eye, const Vec3& target,
                            const std::string& file, u32 w = 1200, u32 h = 700, f32 fov = 40.0f) {
    test::OffscreenRenderer renderer;
    if (!renderer.init(w, h)) {
        return {};
    }
    std::vector<test::OffscreenRenderer::Draw> draws;
    // The client's five unit shapes, tinted per bone.
    Mesh* shape_box = renderer.upload(primitives::cube(1.0f, Vec3{1.0f}));
    Mesh* shape_sphere = renderer.upload(primitives::sphere(18, 12, Vec3{1.0f}));
    Mesh* shape_cyl = renderer.upload(primitives::cylinder(16, Vec3{1.0f}));
    Mesh* shape_capsule = renderer.upload(primitives::capsule(18, 6, Vec3{1.0f}));
    Mesh* shape_rounded = renderer.upload(primitives::rounded_box(0.32f, Vec3{1.0f}));
    auto shape_of = [&](BoneShape s) -> Mesh* {
        switch (s) {
            case BoneShape::Sphere: return shape_sphere;
            case BoneShape::Cylinder: return shape_cyl;
            case BoneShape::Capsule: return shape_capsule;
            case BoneShape::RoundedBox: return shape_rounded;
            case BoneShape::Box: break;
        }
        return shape_box;
    };
    if (Mesh* ground = renderer.upload(primitives::grid(30, 1.2f, Vec3{0.38f, 0.33f, 0.26f}))) {
        draws.push_back({ground, Mat4{1.0f}, Vec4{1.0f}});
    }
    auto add_mesh = [&](const MeshData& md, const Mat4& model) {
        if (!md.indices.empty()) {
            if (Mesh* m = renderer.upload(md)) {
                draws.push_back({m, model, Vec4{1.0f}});
            }
        }
    };

    for (const Sitter& s : cast) {
        CharacterModel model = CharacterModel::create(s.seed, s.app);
        apply_outfit(model, s.kind, s.eq);
        const SkinnedMesh body = build_body_mesh(model);
        const SkinnedMesh outfit = build_outfit_mesh(model, s.kind, s.eq);
        const BodyColliders fit = fit_body_colliders(model, body, outfit);
        std::vector<ClothPiece> cloth = outfit_cloth(model, s.kind, s.eq, fit);

        // Walk (or stand) for a couple of seconds so the cloth swings + settles against the body.
        CharacterAnimator anim;
        const f32 dt = 1.0f / 60.0f;
        Vec3 at = s.pos - Vec3{std::sin(s.yaw), 0.0f, std::cos(s.yaw)} * (s.walk ? 3.2f * 2.0f : 0.0f);
        Mat4 root{1.0f};
        std::vector<Quat> pose;
        std::vector<ClothCollider> colliders;
        for (int f = 0; f < 120; ++f) {
            anim.update(s.walk ? 3.2f : 0.0f, Timestep{dt});
            if (s.walk) {
                at += Vec3{std::sin(s.yaw), 0.0f, std::cos(s.yaw)} * (3.2f * dt);
            }
            root = glm::translate(Mat4{1.0f}, at) * glm::rotate(Mat4{1.0f}, s.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                   (s.walk ? anim.body_offset() : Mat4{1.0f});
            pose = s.walk ? anim.pose(model) : std::vector<Quat>{};
            const std::vector<Mat4> jm = model.joint_matrices(root, pose);
            pose_body_colliders(fit, jm, colliders);
            ClothEnv env;
            env.wind = Vec3{0.6f, 0.0f, -0.4f};
            env.dt = dt;
            env.body = colliders;
            env.ground = 0.0f;
            for (ClothPiece& c : cloth) {
                step_cloth(c, model, jm, root, env);
            }
        }

        const CharacterPalette& pal = model.palette();
        auto palette = [&pal](u8 m) { return body_material_color(pal, static_cast<BodyMaterial>(m)); };
        std::vector<Vertex> verts;
        for (const SkinnedMesh* sm : {&body, &outfit}) {
            if (sm->vertices.empty()) {
                continue;
            }
            skin(*sm, model.joint_matrices(Mat4{1.0f}, pose), verts, palette);
            MeshData md;
            md.vertices = verts;
            md.indices = sm->indices;
            add_mesh(md, root);
        }
        // The attachment primitives riding on the skinned body (face, hair, helm, pauldrons, ...).
        const std::vector<Mat4> mats = model.bone_matrices(root, pose);
        for (usize i = 0; i < model.bones().size(); ++i) {
            const Bone& b = model.bones()[i];
            if (b.attachment) {
                const Vec3 c = body_material_color(pal, static_cast<BodyMaterial>(b.color));
                draws.push_back({shape_of(b.shape), mats[i], Vec4{c, 1.0f}});
            }
        }
        // The simulated cloth, built in world space.
        for (const ClothPiece& c : cloth) {
            MeshData md;
            if (c.ring) {
                // The device's front panel is found in the character's local frame (as the client does).
                std::vector<ClothChain> local = c.chains;
                const Mat4 inv = glm::inverse(root);
                for (ClothChain& ch : local) {
                    for (Vec3& p : ch.pos) {
                        p = Vec3{inv * Vec4{p, 1.0f}};
                    }
                }
                const Vec3 body_axis{0.0f};
                build_cloth_tube(local, c.closed, c.color, md, c.device, c.device_color, &body_axis);
                add_mesh(md, root);
            } else {
                build_cloth_mesh(c.chains[0], Mat3{root} * c.side_local, c.color, md, Mat3{root} * c.drape_local);
                add_mesh(md, Mat4{1.0f});
            }
        }
        // The held weapons, hung from the hand joints exactly as the client attaches them.
        if (s.role >= 0) {
            const std::vector<Mat4> jmats = model.joint_matrices(root, pose);
            auto hand_frame = [&](BonePart arm) -> Mat4 {
                for (usize i = 0; i < model.bones().size(); ++i) {
                    if (model.bones()[i].part == arm) {
                        const f32 wrist = model.bones()[i].box_center.y * 2.0f;
                        return jmats[i] * glm::translate(Mat4{1.0f}, Vec3{0.0f, wrist, 0.0f});
                    }
                }
                return Mat4{1.0f};
            };
            auto add_weapon = [&](WeaponType t, BonePart arm) {
                if (t == WeaponType::None) {
                    return;
                }
                const Mat4 hand = hand_frame(arm);
                for (const WeaponPiece& wp : weapon_pieces(t, s.eq.weapon(), pal)) {
                    draws.push_back({shape_of(wp.shape), hand * wp.local, Vec4{wp.color, 1.0f}});
                }
            };
            add_weapon(role_weapon(static_cast<u8>(s.role), 0), BonePart::LowerArmL);
            add_weapon(role_offhand(static_cast<u8>(s.role)), BonePart::LowerArmR);
        }
    }

    const Mat4 view = look_at(eye, target, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj = perspective(radians(fov), static_cast<f32>(renderer.width()) / renderer.height(), 0.1f, 100.0f);
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const std::string path = (executable_dir() / file).string();
    // A slightly warm, dimmed key so materials don't blow out near-white (the in-game look).
    std::vector<u8> px = renderer.render(draws, view, proj, sky, glm::normalize(Vec3{0.35f, 0.8f, 0.55f}), path,
                                         Vec4{1.0f, 0.95f, 0.85f, 0.95f});
    const std::string wrote = "Wrote " + path;
    MESSAGE(wrote);
    return px;
}
} // namespace

// A line-up of characters (all races, every hero role at every gear tier, townsfolk + bandits) drawn the
// way the client draws them, so the aesthetic can be eyeballed from a few stills: the overview, close
// front + back views of each role's three looks (capes, hoods, helms), and a mid-stride shot of the
// cloth swinging against the walking body.
TEST_CASE("Scene shot: character line-up (races, roles, tiers, NPCs)") {
    std::vector<Sitter> cast;
    // Back row: the four hero roles in MASTER gear, mixing races/skins/tints.
    const Equipment master{3, 3, 0, 0};
    cast.push_back({11u, {1, 1, EyeStyle::Round, EarStyle::Round, HairStyle::Short, Race::Human},
                    OutfitKind::Plate, master, Vec3{-4.2f, 0.0f, -4.5f}, 0});
    cast.push_back({12u, {3, 2, EyeStyle::Sharp, EarStyle::Pointed, HairStyle::Ponytail, Race::Elf},
                    OutfitKind::Leather, Equipment{3, 3, 2, 0}, Vec3{-1.4f, 0.0f, -4.5f}, 1});
    cast.push_back({13u, {2, 3, EyeStyle::Round, EarStyle::Round, HairStyle::Bald, Race::Dwarf},
                    OutfitKind::Holy, Equipment{3, 3, 5, 0}, Vec3{1.4f, 0.0f, -4.5f}, 2});
    cast.push_back({14u, {4, 0, EyeStyle::Wide, EarStyle::Small, HairStyle::Spiky, Race::Human},
                    OutfitKind::Robe, Equipment{3, 3, 3, 0}, Vec3{4.2f, 0.0f, -4.5f}, 3});
    // Middle row: the Knight's plate at each tier (rags -> master), one race, same seed - so the tier
    // progression is the only variable.
    for (u8 t = 0; t < kTierCount; ++t) {
        cast.push_back({21u, {1, 0, EyeStyle::Round, EarStyle::Round, HairStyle::Short, Race::Human},
                        OutfitKind::Plate, Equipment{t, t, 1, 0}, Vec3{-4.2f + 2.8f * static_cast<f32>(t), 0.0f, 0.0f},
                        0});
    }
    // Front row: the three races side by side in townsfolk garb (a hood, a coif, a straw hat), then the
    // two bandit kinds.
    cast.push_back({31u, {2, 1, EyeStyle::Round, EarStyle::Round, HairStyle::Short, Race::Human},
                    OutfitKind::Peasant, Equipment{0, 0, 0 | (0 << 2) | 16, 0}, Vec3{-5.6f, 0.0f, 4.5f}});
    cast.push_back({32u, {1, 3, EyeStyle::Sleepy, EarStyle::Round, HairStyle::Mohawk, Race::Dwarf},
                    OutfitKind::Peasant, Equipment{0, 0, 1 | (1 << 2), 0}, Vec3{-2.8f, 0.0f, 4.5f}});
    cast.push_back({33u, {0, 2, EyeStyle::Sharp, EarStyle::Pointed, HairStyle::Ponytail, Race::Elf},
                    OutfitKind::Peasant, Equipment{0, 0, 3 | (2 << 2) | 16, 0}, Vec3{0.0f, 0.0f, 4.5f}});
    cast.push_back({34u, {3, 0, EyeStyle::Sharp, EarStyle::Round, HairStyle::Bald, Race::Human},
                    OutfitKind::Brigand, Equipment{}, Vec3{2.8f, 0.0f, 4.5f}});
    cast.push_back({35u, {2, 1, EyeStyle::Round, EarStyle::Round, HairStyle::Short, Race::Human},
                    OutfitKind::Outlaw, Equipment{}, Vec3{5.6f, 0.0f, 4.5f}});

    const std::vector<u8> px = render_cast(cast, Vec3{0.0f, 5.2f, 13.5f}, Vec3{0.0f, 1.0f, 0.0f}, "characters.ppm");
    if (px.empty()) {
        MESSAGE("No Vulkan device/shaders - skipping character line-up shot");
        return;
    }
    const Vec3 sky{0.46f, 0.62f, 0.82f};
    const Vec3 mid = pixel(px, 1200, 600, 350);
    CHECK(glm::length(mid - sky) > 0.05f); // characters, not empty sky

    // Close-ups, one still per design look (worn / fine / master): the four hero roles facing the
    // camera, then the same four turned round (capes, quivers, liripipes).
    const OutfitKind kinds[4] = {OutfitKind::Plate, OutfitKind::Leather, OutfitKind::Holy, OutfitKind::Robe};
    const u8 tints[4] = {0, 2, 5, 3};
    for (int t = 0; t < 3; ++t) {
        const u8 tier = static_cast<u8>(t == 0 ? 1 : t + 1);
        std::vector<Sitter> roles;
        for (int side = 0; side < 2; ++side) {
            for (int k = 0; k < 4; ++k) {
                Sitter s{40u + static_cast<u32>(k), {1, static_cast<u8>(k), EyeStyle::Round, EarStyle::Round,
                                                    HairStyle::Short, Race::Human},
                         kinds[k], Equipment{tier, tier, tints[k], 0},
                         Vec3{-4.55f + 1.3f * static_cast<f32>(side * 4 + k), 0.0f, 0.0f}, k};
                s.yaw = side == 0 ? 0.0f : Pi;
                roles.push_back(s);
            }
        }
        const std::string file = "characters_tier" + std::to_string(t) + ".ppm";
        CHECK_FALSE(render_cast(roles, Vec3{0.0f, 2.1f, 7.8f}, Vec3{0.0f, 0.85f, 0.0f}, file, 1600, 600, 30.0f).empty());
    }

    // Mid-stride, side-on: the cloth (capes, skirts) swings with the legs instead of passing through.
    std::vector<Sitter> walkers;
    const std::pair<OutfitKind, int> walk_kinds[4] = {{OutfitKind::Plate, 0}, {OutfitKind::Holy, 2},
                                                      {OutfitKind::Robe, 3}, {OutfitKind::Plate, 0}};
    for (int k = 0; k < 4; ++k) {
        const u8 tier = static_cast<u8>(k == 3 ? 2 : 3);
        walkers.push_back({50u + static_cast<u32>(k), CharacterAppearance{}, walk_kinds[k].first,
                           Equipment{tier, tier, tints[walk_kinds[k].second], 0},
                           Vec3{-3.3f + 2.2f * static_cast<f32>(k), 0.0f, 0.0f}, walk_kinds[k].second, HalfPi, true});
    }
    CHECK_FALSE(render_cast(walkers, Vec3{0.0f, 2.2f, 7.5f}, Vec3{0.0f, 0.85f, 0.0f}, "characters_walk.ppm", 1200, 600, 40.0f).empty());
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
            glm::scale(Mat4{1.0f}, Vec3{p.scale * p.length, p.scale, p.scale});
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

// The world map's terrain raster draws through the renderer's instanced UI-tile path
// (ui_tile.vert/frag + per-instance rect/colour vertex input). Verify that path down to the
// pixels: tiles land exactly on their rects, the batch pan/zoom transform (scale about a
// pivot + offset) moves them where the map's math says, and the scissor clip is honoured.
TEST_CASE("Shot: instanced UI-tile raster draws, transforms and clips (world-map path)") {
    test::OffscreenRenderer renderer;
    if (!renderer.init(320, 240)) {
        MESSAGE("No Vulkan device/shaders - skipping UI tile raster shot");
        return;
    }
    const Vec3 bg{0.0f, 0.0f, 0.0f};
    const Mat4 identity{1.0f};
    const Vec3 sun{0.0f, 1.0f, 0.0f};
    auto near3 = [](const Vec3& a, const Vec3& b) { return glm::length(a - b) < 0.02f; };
    const Vec3 red{1.0f, 0.0f, 0.0f}, green{0.0f, 1.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};

    test::OffscreenRenderer::UITileBatch batch;
    batch.tiles.push_back({Vec4{10.0f, 10.0f, 20.0f, 20.0f}, Vec4{red, 1.0f}});
    batch.tiles.push_back({Vec4{40.0f, 10.0f, 20.0f, 20.0f}, Vec4{green, 1.0f}});
    batch.tiles.push_back({Vec4{10.0f, 40.0f, 20.0f, 20.0f}, Vec4{blue, 1.0f}});

    // 1. Identity transform: every tile sits exactly on its rect (flat colour, no AA).
    {
        const std::vector<u8> px = renderer.render({}, identity, identity, bg, sun, "",
                                                   Vec4{1.0f}, {}, {}, &batch);
        const u32 w = renderer.width();
        CHECK(near3(pixel(px, w, 20, 20), red));
        CHECK(near3(pixel(px, w, 50, 20), green));
        CHECK(near3(pixel(px, w, 20, 50), blue));
        CHECK(near3(pixel(px, w, 150, 150), bg)); // untouched background
    }

    // 2. The map's pan/zoom transform: p' = pivot + (p - pivot) * scale + offset. With
    //    pivot (20,20), scale 2, offset (30,10) the red tile {10,10,20,20} must cover
    //    30..70 x 10..50 and its original spot must be background again.
    {
        batch.pivot = Vec2{20.0f, 20.0f};
        batch.offset = Vec2{30.0f, 10.0f};
        batch.scale = 2.0f;
        const std::vector<u8> px = renderer.render({}, identity, identity, bg, sun, "",
                                                   Vec4{1.0f}, {}, {}, &batch);
        const u32 w = renderer.width();
        CHECK(near3(pixel(px, w, 50, 30), red));    // scaled + shifted red tile centre
        CHECK(near3(pixel(px, w, 110, 30), green)); // green: 90..130 x 10..50
        CHECK(near3(pixel(px, w, 50, 90), blue));   // blue: 30..70 x 70..110
        CHECK(near3(pixel(px, w, 15, 15), bg));     // the un-transformed spot is empty now
    }

    // 3. Scissor clip: with the identity transform back and a clip that ends at x = 45,
    //    the green tile (40..60) is cut off past the clip edge while red still draws.
    {
        batch.pivot = Vec2{0.0f};
        batch.offset = Vec2{0.0f};
        batch.scale = 1.0f;
        batch.scissor = Vec4{0.0f, 0.0f, 45.0f, 240.0f};
        const std::vector<u8> px = renderer.render({}, identity, identity, bg, sun, "",
                                                   Vec4{1.0f}, {}, {}, &batch);
        const u32 w = renderer.width();
        CHECK(near3(pixel(px, w, 20, 20), red));  // inside the clip
        CHECK(near3(pixel(px, w, 42, 20), green)); // green's left edge is inside the clip
        CHECK(near3(pixel(px, w, 50, 20), bg));   // past the clip edge: no tile drawn
    }
}
