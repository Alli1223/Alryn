#include <doctest/doctest.h>

#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>

using namespace alryn;

TEST_CASE("Primitives: cube has per-face flat normals") {
    const MeshData data = primitives::cube(2.0f, Vec3{1.0f, 0.0f, 0.0f});
    CHECK(data.vertices.size() == 24); // 4 verts * 6 faces (not shared -> flat shading)
    CHECK(data.indices.size() == 36);  // 2 tris * 3 * 6 faces

    for (const Vertex& v : data.vertices) {
        CHECK(glm::length(v.normal) == doctest::Approx(1.0f));
        CHECK(v.color.r == doctest::Approx(1.0f));
    }

    // Every index must reference a real vertex.
    for (u32 index : data.indices) {
        CHECK(index < data.vertices.size());
    }
}

TEST_CASE("Primitives: recompute_flat_normals matches authored cube normals") {
    MeshData data = primitives::cube(1.0f);
    // Stash authored normals, zero them, recompute, and compare.
    std::vector<Vec3> authored;
    for (const Vertex& v : data.vertices) {
        authored.push_back(v.normal);
    }
    for (Vertex& v : data.vertices) {
        v.normal = Vec3{0.0f};
    }
    data.recompute_flat_normals();
    for (usize i = 0; i < data.vertices.size(); ++i) {
        CHECK(glm::length(data.vertices[i].normal - authored[i]) == doctest::Approx(0.0f).epsilon(0.001));
    }
}

TEST_CASE("Primitives: grid produces a quad per cell") {
    const u32 cells = 4;
    const MeshData data = primitives::grid(cells, 1.0f);
    CHECK(data.vertices.size() == cells * cells * 4);
    CHECK(data.indices.size() == cells * cells * 6);
    for (const Vertex& v : data.vertices) {
        CHECK(v.position.y == doctest::Approx(0.0f));
        CHECK(v.normal.y == doctest::Approx(1.0f));
    }
}

TEST_CASE("Vulkan: host-visible buffer upload + readback (headless)") {
    vk::Instance instance;
    vk::InstanceConfig config;
    config.app_name = "alryn_tests";
    if (!instance.create(config)) {
        MESSAGE("No Vulkan instance - skipping buffer test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping buffer test");
        return;
    }

    constexpr VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    vk::Buffer buffer;
    const float source[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(buffer.create(device, sizeof(source), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host_visible));
    buffer.upload(source, sizeof(source));

    const auto* readback = static_cast<const float*>(buffer.map());
    CHECK(readback[0] == doctest::Approx(1.0f));
    CHECK(readback[3] == doctest::Approx(4.0f));
    buffer.unmap();

    SUBCASE("upload a cube mesh to the GPU") {
        Mesh mesh;
        REQUIRE(mesh.create(device, primitives::cube()));
        CHECK(mesh.valid());
        CHECK(mesh.index_count() == 36);
        mesh.destroy();
    }
}
