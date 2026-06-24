#include <doctest/doctest.h>

#include "support/OffscreenRenderer.h"

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
                  bool with_wagon = false, f32 wagon_yaw = 0.0f) {
    constexpr f32 voxel = 1.0f;
    constexpr int cv = 16;                 // voxels per chunk
    constexpr f32 cw = static_cast<f32>(cv) * voxel; // chunk world size (16 m)
    constexpr f32 y_min = -14.0f, y_max = 16.0f;
    const int yv = static_cast<int>((y_max - y_min) / voxel);

    WorldSampler sampler(seed);
    const DensitySampler density = sampler.snapshot();
    PropLibrary lib;

    std::vector<test::OffscreenRenderer::Draw> draws;
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
                field, IVec3{0}, field.cell_count(), 0.0f, [&](const Vec3& p, const Vec3& n) {
                    const f32 up = glm::clamp(n.y, 0.0f, 1.0f);
                    Vec3 c = roads::tint_surface(worldgen::surface_color(p, n, seed), p, up, seed);
                    return town_path_tint(c, p, up, seed);
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
    // Plank bridges where the roads cross rivers (deck stretched to the span, level with the banks).
    {
        const MeshData bridge = PropLibrary::build_plank_bridge().parts[0].mesh;
        for (const roads::Bridge& b : roads::bridges(focus, radius + 24.0f, seed)) {
            const Vec2 bdir{std::cos(b.yaw), std::sin(b.yaw)};
            const Vec2 e0 = b.center - bdir * (b.length * 0.5f);
            const Vec2 e1 = b.center + bdir * (b.length * 0.5f);
            const f32 deck_y = std::max(worldgen::height(e0.x, e0.y, seed),
                                        worldgen::height(e1.x, e1.y, seed));
            add(bridge, at(Vec3{b.center.x, deck_y, b.center.y}) *
                            glm::rotate(Mat4{1.0f}, -b.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                            glm::scale(Mat4{1.0f}, Vec3{b.length, 1.0f, 1.0f}));
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
    r.render(draws, view, proj, sky, glm::normalize(Vec3{0.45f, 0.85f, 0.4f}), out);
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
