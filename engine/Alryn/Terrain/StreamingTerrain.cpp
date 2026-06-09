#include <Alryn/Terrain/StreamingTerrain.h>

#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Terrain/MarchingTetra.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace alryn {

namespace {

constexpr int kTrashHold = 3; // frames to keep a retired mesh before destroying it
constexpr int kLoadBudget = 12;
constexpr int kMeshBudget = 6;

} // namespace

StreamingTerrain::StreamingTerrain(u32 seed, f32 voxel_size, int chunk_voxels, int view_radius)
    : sampler_(seed), voxel_size_(voxel_size), chunk_voxels_(chunk_voxels),
      view_radius_(view_radius), chunk_world_(static_cast<f32>(chunk_voxels) * voxel_size),
      y_voxels_(static_cast<int>((y_max_ - y_min_) / voxel_size)) {}

IVec2 StreamingTerrain::chunk_of(const Vec3& p) const {
    return IVec2{static_cast<int>(std::floor(p.x / chunk_world_)),
                 static_cast<int>(std::floor(p.z / chunk_world_))};
}

StreamingTerrain::Chunk& StreamingTerrain::ensure_chunk(int cx, int cz) {
    const i64 key = key_of(cx, cz);
    const auto it = chunks_.find(key);
    if (it != chunks_.end()) {
        return it->second;
    }
    Chunk chunk;
    chunk.coord = IVec2{cx, cz};
    const Vec3 origin{static_cast<f32>(cx) * chunk_world_, y_min_, static_cast<f32>(cz) * chunk_world_};
    const IVec3 dims{chunk_voxels_ + 1, y_voxels_ + 1, chunk_voxels_ + 1};
    chunk.field = std::make_unique<VoxelField>(dims, voxel_size_, origin);
    chunk.field->fill([this](const Vec3& wp) { return sampler_(wp); });
    chunk.trees = scatter_trees(cx, cz, chunk_world_, sampler_.seed());
    chunk.needs_mesh = true;
    return chunks_.emplace(key, std::move(chunk)).first->second;
}

void StreamingTerrain::retire_mesh(Mesh&& mesh) {
    if (mesh.valid()) {
        trash_.push_back({std::move(mesh), kTrashHold});
    }
}

void StreamingTerrain::mesh_chunk(Chunk& chunk, const vk::Device& device) {
    if (!chunk.field) {
        return;
    }
    const u32 seed = sampler_.seed();
    const MeshData data = mc::polygonize(
        *chunk.field, IVec3{0}, chunk.field->cell_count(), 0.0f,
        [seed](const Vec3& pos, const Vec3& normal) { return worldgen::surface_color(pos, normal, seed); });
    retire_mesh(std::move(chunk.mesh)); // defer deleting the old mesh (may be in flight)
    chunk.mesh = Mesh{};
    if (!data.indices.empty()) {
        chunk.mesh.create(device, data);
    }
    chunk.needs_mesh = false;
}

void StreamingTerrain::apply_edit(const Vec3& center, f32 radius, f32 amount) {
    sampler_.add_edit(center, radius, amount);
    for (auto& [key, chunk] : chunks_) {
        if (!chunk.field) {
            continue;
        }
        const Vec3 lo = chunk.field->origin();
        const Vec3 hi = lo + Vec3{chunk_world_, static_cast<f32>(y_voxels_) * voxel_size_, chunk_world_};
        const Vec3 nearest = glm::clamp(center, lo, hi);
        if (glm::length(nearest - center) <= radius) {
            chunk.field->apply_sphere(center, radius, amount);
            chunk.needs_mesh = true;
        }
    }
}

void StreamingTerrain::update(const Vec3& focus, const vk::Device& device) {
    // 1. Retire expired meshes (safe to destroy once the GPU is past them).
    for (auto it = trash_.begin(); it != trash_.end();) {
        if (--it->frames_left <= 0) {
            it->mesh.destroy();
            it = trash_.erase(it);
        } else {
            ++it;
        }
    }

    const IVec2 center = chunk_of(focus);

    // 2. Load nearby chunks (nearest rings first), within a per-frame budget.
    int load_budget = kLoadBudget;
    for (int r = 0; r <= view_radius_ && load_budget > 0; ++r) {
        for (int dz = -r; dz <= r && load_budget > 0; ++dz) {
            for (int dx = -r; dx <= r && load_budget > 0; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) {
                    continue; // only the ring at Chebyshev distance r
                }
                const int cx = center.x + dx;
                const int cz = center.y + dz; // IVec2 stores (chunk_x, chunk_z)
                if (chunks_.find(key_of(cx, cz)) == chunks_.end()) {
                    ensure_chunk(cx, cz);
                    --load_budget;
                }
            }
        }
    }

    // 3. Mesh chunks that need it, within a per-frame budget.
    int mesh_budget = kMeshBudget;
    for (auto& [key, chunk] : chunks_) {
        if (mesh_budget <= 0) {
            break;
        }
        if (chunk.needs_mesh) {
            mesh_chunk(chunk, device);
            --mesh_budget;
        }
    }

    // 4. Unload chunks that drifted out of range.
    const int unload_dist = view_radius_ + 2;
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const int dx = std::abs(it->second.coord.x - center.x);
        const int dz = std::abs(it->second.coord.y - center.y);
        if (std::max(dx, dz) > unload_dist) {
            retire_mesh(std::move(it->second.mesh));
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace alryn
