#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Vertex.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace alryn {

namespace vk {
class Device;
}

// CPU-side geometry. Build it on any thread, then upload to a GPU Mesh.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    // Assigns each triangle a single face normal (flat shading). Requires that
    // vertices are not shared across triangles - which is how the marching-cubes
    // mesher and the primitives below emit geometry.
    void recompute_flat_normals();
};

// GPU-resident mesh: vertex + index buffers and a draw helper.
class Mesh : public NonCopyable {
public:
    Mesh() = default;

    bool create(const vk::Device& device, const MeshData& data);
    void destroy();

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    u32 index_count() const { return index_count_; }
    bool valid() const { return index_count_ > 0; }

private:
    vk::Buffer vertex_buffer_;
    vk::Buffer index_buffer_;
    u32 index_count_ = 0;
};

} // namespace alryn
