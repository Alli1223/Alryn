#include <doctest/doctest.h>

#include <Alryn/Core/Noise.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/Terrain.h>
#include <Alryn/Terrain/VoxelField.h>

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
