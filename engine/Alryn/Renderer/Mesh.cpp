#include <Alryn/Renderer/Mesh.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <algorithm>
#include <cmath>

namespace alryn {

void MeshData::recompute_flat_normals() {
    for (usize i = 0; i + 2 < indices.size(); i += 3) {
        Vertex& a = vertices[indices[i]];
        Vertex& b = vertices[indices[i + 1]];
        Vertex& c = vertices[indices[i + 2]];
        const Vec3 normal = glm::normalize(glm::cross(b.position - a.position, c.position - a.position));
        a.normal = normal;
        b.normal = normal;
        c.normal = normal;
    }
}

bool Mesh::create(const vk::Device& device, const MeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) {
        ALRYN_ERROR("Mesh::create called with empty geometry");
        return false;
    }

    const VkDeviceSize vertex_bytes = sizeof(Vertex) * data.vertices.size();
    const VkDeviceSize index_bytes = sizeof(u32) * data.indices.size();
    constexpr VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if (!vertex_buffer_.create(device, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host_visible)) {
        return false;
    }
    if (!index_buffer_.create(device, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host_visible)) {
        vertex_buffer_.destroy();
        return false;
    }

    vertex_buffer_.upload(data.vertices.data(), vertex_bytes);
    index_buffer_.upload(data.indices.data(), index_bytes);
    index_count_ = static_cast<u32>(data.indices.size());

    // Local-space bounding sphere from the AABB centre, for frustum / light culling.
    Vec3 lo = data.vertices[0].position;
    Vec3 hi = lo;
    for (const Vertex& v : data.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    bounds_center_ = (lo + hi) * 0.5f;
    f32 r2 = 0.0f;
    for (const Vertex& v : data.vertices) {
        r2 = std::max(r2, glm::dot(v.position - bounds_center_, v.position - bounds_center_));
    }
    bounds_radius_ = std::sqrt(r2);
    return true;
}

void Mesh::destroy() {
    vertex_buffer_.destroy();
    index_buffer_.destroy();
    index_count_ = 0;
}

void Mesh::bind(VkCommandBuffer cmd) const {
    const VkBuffer buffers[] = {vertex_buffer_.handle()};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

void Mesh::draw(VkCommandBuffer cmd) const {
    vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

} // namespace alryn
