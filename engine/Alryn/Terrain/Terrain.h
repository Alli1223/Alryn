#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/VoxelField.h>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace alryn {

namespace vk {
class Device;
}

// Deformable voxel terrain: a VoxelField split into cubic chunks, each with its
// own GPU mesh. Editing the field marks the overlapping chunks dirty;
// rebuild_dirty() re-meshes them. Chunks share the field's border samples, so
// neighbours meet seamlessly.
class Terrain {
public:
    Terrain(const IVec3& dims, f32 voxel_size, int chunk_cells, const Vec3& origin = Vec3{0.0f});

    void set_colorize(mc::ColorFn colorize) { colorize_ = std::move(colorize); }

    // Fills the field from a density function and marks all chunks dirty.
    void generate(const std::function<f32(const Vec3&)>& density);

    // Edits the field in a sphere: positive amount carves (dig), negative adds.
    void deform(const Vec3& center, f32 radius, f32 amount);

    std::optional<Vec3> raycast(const Vec3& origin, const Vec3& dir, f32 max_dist) const {
        return field_.raycast(origin, dir, max_dist, iso_);
    }

    // Re-meshes dirty chunks. Waits for the device to idle first (simple + safe;
    // a later optimisation can stage/async this).
    void rebuild_dirty(const vk::Device& device);
    bool any_dirty() const;

    template <typename Fn>
    void for_each_mesh(Fn fn) const {
        for (const Chunk& chunk : chunks_) {
            if (chunk.mesh.valid()) {
                fn(chunk.mesh);
            }
        }
    }

    VoxelField& field() { return field_; }
    const VoxelField& field() const { return field_; }
    usize chunk_count() const { return chunks_.size(); }

private:
    struct Chunk {
        IVec3 cell_min{0};
        IVec3 cell_max{0};
        Mesh mesh;
        bool dirty = true;
    };

    void mark_dirty(const VoxelField::Region& region);

    VoxelField field_;
    std::vector<Chunk> chunks_;
    int chunk_cells_;
    f32 iso_ = 0.0f;
    mc::ColorFn colorize_;
};

} // namespace alryn
