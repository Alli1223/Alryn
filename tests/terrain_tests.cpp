#include <doctest/doctest.h>

#include <Alryn/Core/Noise.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/StreamingTerrain.h>
#include <Alryn/Terrain/Terrain.h>
#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/Terrain/VegetationScatter.h>
#include <Alryn/Terrain/VoxelField.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

using namespace alryn;

namespace {
Vec3 white(const Vec3&, const Vec3&) {
    return Vec3{1.0f};
}
} // namespace

TEST_CASE("Noise: deterministic and bounded") {
    CHECK(noise::value2d(1.5f, 2.5f) == doctest::Approx(noise::value2d(1.5f, 2.5f)));
    for (int i = 0; i < 200; ++i) {
        const f32 n = noise::fbm2d(static_cast<f32>(i) * 0.3f, static_cast<f32>(i) * 0.7f);
        CHECK(n >= -1.01f);
        CHECK(n <= 1.01f);
    }
}

TEST_CASE("VoxelField: set/value/sample and out-of-range clamping") {
    VoxelField field(IVec3{10, 10, 10}, 1.0f, Vec3{0.0f});
    field.fill([](const Vec3&) { return 1.0f; }); // all air
    CHECK(field.value(5, 5, 5) == doctest::Approx(1.0f));

    field.set(5, 5, 5, -0.5f);
    CHECK(field.value(5, 5, 5) == doctest::Approx(-0.5f));
    CHECK(field.value(-3, 0, 0) == field.value(0, 0, 0)); // clamps coords

    field.fill([](const Vec3& p) { return p.x - 5.0f; }); // surface at x = 5
    CHECK(field.sample(Vec3{5.0f, 5.0f, 5.0f}) == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(field.sample(Vec3{6.0f, 5.0f, 5.0f}) > 0.0f);
}

TEST_CASE("VoxelField: apply_sphere carves and reports a region") {
    VoxelField field(IVec3{16, 16, 16}, 1.0f, Vec3{0.0f});
    field.fill([](const Vec3&) { return -1.0f; }); // all solid
    const VoxelField::Region region = field.apply_sphere(Vec3{8.0f, 8.0f, 8.0f}, 3.0f, 2.0f);
    CHECK(region.valid);
    CHECK(field.value(8, 8, 8) > -1.0f); // centre carved toward air
    CHECK(field.value(0, 0, 0) == doctest::Approx(-1.0f)); // far away untouched
}

TEST_CASE("VoxelField: raycast finds a flat surface") {
    VoxelField field(IVec3{20, 20, 20}, 0.5f, Vec3{0.0f});
    field.fill([](const Vec3& p) { return p.y - 3.0f; }); // solid below y = 3
    const auto hit = field.raycast(Vec3{2.0f, 8.0f, 2.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->y == doctest::Approx(3.0f).epsilon(0.1));
}

TEST_CASE("MarchingTetra: uniform fields produce no geometry") {
    VoxelField solid(IVec3{8, 8, 8}, 1.0f);
    solid.fill([](const Vec3&) { return -1.0f; });
    CHECK(mc::polygonize(solid, IVec3{0}, solid.cell_count(), 0.0f, white).indices.empty());

    VoxelField air(IVec3{8, 8, 8}, 1.0f); // defaults to air
    CHECK(mc::polygonize(air, IVec3{0}, air.cell_count(), 0.0f, white).indices.empty());
}

TEST_CASE("MarchingTetra: a sphere is a closed, on-surface manifold") {
    VoxelField field(IVec3{24, 24, 24}, 0.5f, Vec3{-6.0f});
    const Vec3 center{0.0f};
    const f32 radius = 3.0f;
    field.fill([&](const Vec3& p) { return glm::length(p - center) - radius; });

    const MeshData mesh = mc::polygonize(field, IVec3{0}, field.cell_count(), 0.0f, white);
    REQUIRE(!mesh.vertices.empty());
    CHECK(mesh.indices.size() % 3 == 0);

    // Weld vertices by quantized position, then count how many triangles share
    // each edge. A closed manifold has every edge shared by exactly two.
    const f32 quant = field.voxel_size() * 0.001f;
    std::map<std::tuple<int, int, int>, int> ids;
    auto id_of = [&](const Vec3& p) {
        const std::tuple<int, int, int> key{static_cast<int>(std::lround(p.x / quant)),
                                            static_cast<int>(std::lround(p.y / quant)),
                                            static_cast<int>(std::lround(p.z / quant))};
        auto it = ids.find(key);
        if (it != ids.end()) {
            return it->second;
        }
        const int next = static_cast<int>(ids.size());
        ids.emplace(key, next);
        return next;
    };

    std::map<std::pair<int, int>, int> edge_count;
    auto add_edge = [&](int a, int b) {
        if (a > b) {
            std::swap(a, b);
        }
        ++edge_count[{a, b}];
    };
    for (usize i = 0; i < mesh.indices.size(); i += 3) {
        const int a = id_of(mesh.vertices[mesh.indices[i]].position);
        const int b = id_of(mesh.vertices[mesh.indices[i + 1]].position);
        const int c = id_of(mesh.vertices[mesh.indices[i + 2]].position);
        add_edge(a, b);
        add_edge(b, c);
        add_edge(c, a);
    }

    int boundary_edges = 0;
    for (const auto& [edge, count] : edge_count) {
        if (count != 2) {
            ++boundary_edges;
        }
    }
    CHECK(boundary_edges == 0); // watertight

    // Every vertex should sit on the sphere surface.
    for (const Vertex& v : mesh.vertices) {
        CHECK(glm::length(v.position - center) == doctest::Approx(radius).epsilon(0.1));
    }
}

TEST_CASE("Terrain: chunking, dirty tracking, and raycast (CPU)") {
    Terrain terrain(IVec3{33, 17, 33}, 0.5f, 16, Vec3{-8.0f, -4.0f, -8.0f});
    CHECK(terrain.chunk_count() == 2 * 1 * 2); // 32 cells / 16 -> 2 per x/z, 16/16 -> 1 y

    terrain.generate([](const Vec3& p) { return p.y; }); // surface at y = 0
    CHECK(terrain.any_dirty());

    const auto hit = terrain.raycast(Vec3{0.0f, 6.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->y == doctest::Approx(0.0f).epsilon(0.2));
}

TEST_CASE("Terrain: generate + rebuild meshes on a device (headless)") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_terrain_test";
    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance - skipping terrain GPU test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping terrain GPU test");
        return;
    }

    Terrain terrain(IVec3{25, 17, 25}, 0.5f, 8, Vec3{-6.0f, -4.0f, -6.0f});
    terrain.generate([](const Vec3& p) { return p.y; }); // a flat-ish floor at y = 0
    CHECK(terrain.any_dirty());

    terrain.rebuild_dirty(device);
    CHECK_FALSE(terrain.any_dirty());

    int mesh_count = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++mesh_count; });
    CHECK(mesh_count > 0); // the surface chunks produced geometry

    // Carving marks chunks dirty again.
    terrain.deform(Vec3{0.0f, 0.0f, 0.0f}, 2.0f, 2.0f);
    CHECK(terrain.any_dirty());
    terrain.rebuild_dirty(device);
    CHECK_FALSE(terrain.any_dirty());
}

TEST_CASE("Worldgen: biomes give smooth heights with both land and water") {
    const u32 seed = 1337u;
    const f32 h0 = worldgen::height(50.0f, 30.0f, seed);
    const f32 h1 = worldgen::height(50.2f, 30.0f, seed);
    CHECK(std::abs(h1 - h0) < 1.5f); // continuous

    bool has_land = false;
    bool has_water = false;
    for (int gz = 0; gz < 40; ++gz) {
        for (int gx = 0; gx < 40; ++gx) {
            const f32 x = static_cast<f32>(gx) * 18.0f - 360.0f;
            const f32 z = static_cast<f32>(gz) * 18.0f - 360.0f;
            const f32 h = worldgen::height(x, z, seed);
            if (h > worldgen::water_level + 1.0f) has_land = true;
            if (h < worldgen::water_level - 0.5f) has_water = true;
        }
    }
    CHECK(has_land);
    CHECK(has_water);

    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 peak = worldgen::surface_color(Vec3{0.0f, 13.0f, 0.0f}, up, seed);
    const Vec3 deep = worldgen::surface_color(Vec3{0.0f, -6.0f, 0.0f}, up, seed);
    CHECK(peak.r > deep.r); // snow brighter than wet silt
    for (const Vec3& c : {peak, deep}) {
        CHECK(c.r >= 0.0f);
        CHECK(c.r <= 1.0f);
        CHECK(c.g >= 0.0f);
        CHECK(c.g <= 1.0f);
        CHECK(c.b >= 0.0f);
        CHECK(c.b <= 1.0f);
    }
}

TEST_CASE("Trees: low-poly meshes + deterministic, on-land scatter") {
    const primitives::TreeMeshData pine = primitives::tree(0);
    const primitives::TreeMeshData round = primitives::tree(1);
    CHECK_FALSE(pine.trunk.indices.empty());
    CHECK_FALSE(pine.foliage.indices.empty());
    CHECK_FALSE(round.foliage.indices.empty());

    const u32 seed = 1337u;
    const auto a = scatter_trees(2, 3, 8.0f, seed);
    const auto b = scatter_trees(2, 3, 8.0f, seed);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].position.x == doctest::Approx(b[i].position.x));
        CHECK(a[i].variant == b[i].variant);
    }

    int total = 0;
    bool underwater = false;
    for (int cz = -24; cz < 24; ++cz) {
        for (int cx = -24; cx < 24; ++cx) {
            for (const TreeInstance& t : scatter_trees(cx, cz, 8.0f, seed)) {
                ++total;
                if (t.position.y < worldgen::water_level) {
                    underwater = true;
                }
                CHECK(t.scale > 0.5f);
                CHECK((t.variant == 0 || t.variant == 1));
            }
        }
    }
    CHECK(total > 0);
    CHECK_FALSE(underwater);
}

TEST_CASE("Vegetation: grass + flowers bake deterministically onto land") {
    CHECK_FALSE(primitives::grass_tuft().indices.empty());
    CHECK_FALSE(primitives::flower().indices.empty());

    const u32 seed = 1337u;
    const MeshData a = build_vegetation(2, 3, 8.0f, seed);
    const MeshData b = build_vegetation(2, 3, 8.0f, seed);
    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices.size() == b.indices.size());

    usize total_indices = 0;
    f32 min_y = 1e9f;
    bool any = false;
    for (int cz = -16; cz < 16; ++cz) {
        for (int cx = -16; cx < 16; ++cx) {
            const MeshData v = build_vegetation(cx, cz, 8.0f, seed);
            total_indices += v.indices.size();
            for (const Vertex& vert : v.vertices) {
                any = true;
                min_y = std::min(min_y, vert.position.y);
            }
        }
    }
    CHECK(total_indices > 0);
    REQUIRE(any);
    CHECK(min_y >= worldgen::water_level); // nothing grows below the waterline
}

TEST_CASE("Props: library builds geometry + lights, scatter is deterministic & on land") {
    PropLibrary lib;
    REQUIRE(lib.bushes().size() >= 1);
    REQUIRE(lib.rocks().size() >= 1);
    REQUIRE(lib.houses().size() >= 1);
    CHECK_FALSE(lib.bushes()[0].parts.empty());
    CHECK_FALSE(lib.rocks()[0].parts.empty());

    // Every house has geometry and at least one lantern light.
    for (const PropDef& house : lib.houses()) {
        CHECK_FALSE(house.parts.empty());
        CHECK(house.lights.size() >= 1);
        bool has_emissive = false;
        for (const PropPart& part : house.parts) {
            if (part.layer == PropLayer::Emissive && !part.mesh.indices.empty()) has_emissive = true;
        }
        CHECK(has_emissive); // glowing windows / lantern glass
    }

    const u32 seed = 1337u;
    const auto a = scatter_props(1, 2, 8.0f, seed);
    const auto b = scatter_props(1, 2, 8.0f, seed);
    REQUIRE(a.size() == b.size());

    int bushes = 0, rocks = 0, houses = 0;
    bool underwater = false;
    for (int cz = -20; cz < 20; ++cz) {
        for (int cx = -20; cx < 20; ++cx) {
            for (const PropInstance& p : scatter_props(cx, cz, 8.0f, seed)) {
                if (p.position.y < worldgen::water_level) underwater = true;
                if (p.category == PropCategory::Bush) ++bushes;
                else if (p.category == PropCategory::Rock) ++rocks;
                else ++houses;
            }
        }
    }
    CHECK(bushes > 0);
    CHECK(rocks > 0);
    CHECK(houses > 0);
    CHECK_FALSE(underwater);
}

TEST_CASE("StreamingTerrain: streams + unloads chunks around a moving focus") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_streaming_test";
    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance - skipping streaming terrain test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping streaming terrain test");
        return;
    }

    StreamingTerrain terrain(123u, 0.5f, 16, 2); // view radius 2 -> up to 5x5 chunks

    // Pump enough frames for the budgeted loader/mesher to fill in around origin.
    for (int i = 0; i < 30; ++i) {
        terrain.update(Vec3{0.0f, 0.0f, 0.0f}, device);
    }
    CHECK(terrain.loaded_chunk_count() >= 9);
    int meshes = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++meshes; });
    CHECK(meshes > 0); // the surface passes through every column chunk

    // The world is infinite: raycast far from the origin still finds ground.
    const auto far_hit = terrain.raycast(Vec3{800.0f, 30.0f, 800.0f}, Vec3{0.0f, -1.0f, 0.0f}, 60.0f);
    CHECK(far_hit.has_value());

    // Move the focus far away; old chunks unload, new ones load.
    for (int i = 0; i < 60; ++i) {
        terrain.update(Vec3{800.0f, 0.0f, 800.0f}, device);
    }
    CHECK(terrain.loaded_chunk_count() >= 9);
    CHECK(terrain.loaded_chunk_count() <= 64); // bounded - origin chunks were evicted

    int meshes_far = 0;
    terrain.for_each_mesh([&](const Mesh&) { ++meshes_far; });
    CHECK(meshes_far > 0);
}
