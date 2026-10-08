#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Vertex.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>

#include <vulkan/vulkan.h>

#include <utility>
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

    // Bakes cheap ambient occlusion into the vertex COLOURS: short hemisphere rays are
    // cast from every vertex against `occluders` (triangle soup - usually the prop's own
    // opaque parts, this mesh included) and the hit fraction darkens the colour, so
    // creases, eaves, doorways and prop-against-prop contacts fall naturally dark.
    // Run once at asset-build time (see PropLibrary) - zero cost at runtime. `radius`
    // is how far a surface can reach to shade a vertex, `strength` the max darkening.
    void bake_vertex_ao(const std::vector<const MeshData*>& occluders, f32 radius = 0.9f,
                        f32 strength = 0.5f);
};

// GPU-resident mesh: vertex + index buffers and a draw helper.
class Mesh : public NonCopyable {
public:
    Mesh() = default;
    // Moves hand the GPU buffers over AND leave the source empty (invalid). The implicit move used to
    // copy index_count_ / vertex_bytes_, so a moved-from mesh still claimed valid() with null buffers -
    // and the next update_vertices() on it mapped memory on a null device (an abort in the Vulkan
    // loader, e.g. when the hero preview was rebuilt after being drawn).
    Mesh(Mesh&& other) noexcept
        : vertex_buffer_(std::move(other.vertex_buffer_)), index_buffer_(std::move(other.index_buffer_)),
          index_count_(std::exchange(other.index_count_, 0u)),
          vertex_bytes_(std::exchange(other.vertex_bytes_, VkDeviceSize{0})), bounds_center_(other.bounds_center_),
          bounds_radius_(std::exchange(other.bounds_radius_, 0.0f)) {}
    Mesh& operator=(Mesh&& other) noexcept {
        if (this != &other) {
            vertex_buffer_ = std::move(other.vertex_buffer_); // (Buffer's move-assign frees ours first)
            index_buffer_ = std::move(other.index_buffer_);
            index_count_ = std::exchange(other.index_count_, 0u);
            vertex_bytes_ = std::exchange(other.vertex_bytes_, VkDeviceSize{0});
            bounds_center_ = other.bounds_center_;
            bounds_radius_ = std::exchange(other.bounds_radius_, 0.0f);
        }
        return *this;
    }
    ~Mesh() = default;

    bool create(const vk::Device& device, const MeshData& data);
    void destroy();

    void bind(VkCommandBuffer cmd) const;
    void draw(VkCommandBuffer cmd) const;

    // Re-upload vertex data to the (host-visible) vertex buffer IN PLACE - for a skinned mesh that is
    // re-deformed every frame. Only positions/normals/colours move; the bytes must not exceed what the
    // mesh was created with (same vertex count).
    void update_vertices(const std::vector<Vertex>& vertices);

    u32 index_count() const { return index_count_; }
    bool valid() const { return index_count_ > 0; }

    // Local-space bounding sphere (computed in create), used for frustum / light culling.
    const Vec3& bounds_center() const { return bounds_center_; }
    f32 bounds_radius() const { return bounds_radius_; }

private:
    vk::Buffer vertex_buffer_;
    vk::Buffer index_buffer_;
    u32 index_count_ = 0;
    VkDeviceSize vertex_bytes_ = 0; // capacity of the vertex buffer (for in-place re-upload)
    Vec3 bounds_center_{0.0f};
    f32 bounds_radius_ = 0.0f;
};

} // namespace alryn
