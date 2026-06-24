#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Terrain/TreeScatter.h>
#include <Alryn/Terrain/VoxelField.h>
#include <Alryn/Terrain/WorldSampler.h>
#include <Alryn/World/Prop.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace alryn {

namespace vk {
class Device;
}

// An effectively-infinite terrain: chunks are generated/meshed on demand around a
// focus point (the local player) and unloaded when far away. Geometry comes from
// the same density function the server collides against, so what you see matches
// where you can stand. Mesh deletion is deferred a few frames (frames-in-flight)
// so streaming never stalls the GPU.
class StreamingTerrain {
public:
    StreamingTerrain(u32 seed, f32 voxel_size = 0.5f, int chunk_voxels = 16, int view_radius = 3);
    ~StreamingTerrain(); // stops + joins the generation worker

    // Applies a replicated terrain edit (dig/build) and re-meshes affected chunks.
    void apply_edit(const Vec3& center, f32 radius, f32 amount);

    // Streams chunks toward `focus` and meshes/unloads within a per-frame budget.
    void update(const Vec3& focus, const vk::Device& device);

    template <typename Fn>
    void for_each_mesh(Fn fn) const {
        for (const auto& [key, chunk] : chunks_) {
            if (chunk.mesh.valid()) {
                fn(chunk.mesh);
            }
        }
    }

    template <typename Fn>
    void for_each_vegetation_mesh(Fn fn) const {
        for (const auto& [key, chunk] : chunks_) {
            if (chunk.vegetation.valid()) {
                fn(chunk.vegetation);
            }
        }
    }

    template <typename Fn>
    void for_each_tree(Fn fn) const {
        for (const auto& [key, chunk] : chunks_) {
            for (const TreeInstance& tree : chunk.trees) {
                fn(tree);
            }
        }
    }

    template <typename Fn>
    void for_each_prop(Fn fn) const {
        for (const auto& [key, chunk] : chunks_) {
            for (const PropInstance& prop : chunk.props) {
                fn(prop);
            }
        }
    }

    f32 density(const Vec3& p) const { return sampler_(p); }
    DensitySampler sampler() const { return sampler_.as_sampler(); }
    std::optional<Vec3> raycast(const Vec3& origin, const Vec3& dir, f32 max_dist) const {
        return raycast_density(sampler_.as_sampler(), origin, dir, max_dist);
    }

    usize loaded_chunk_count() const { return chunks_.size(); }
    u32 seed() const { return sampler_.seed(); }

private:
    struct Chunk {
        IVec2 coord{0};
        std::unique_ptr<VoxelField> field;
        Mesh mesh;
        Mesh vegetation;              // baked grass + flowers (worldgen-derived)
        std::vector<TreeInstance> trees;
        std::vector<PropInstance> props; // bushes, rocks, houses
        bool needs_mesh = true;
        bool vegetation_built = false; // vegetation only depends on the seed, build once
    };
    struct PendingDelete {
        Mesh mesh;
        int frames_left = 0;
    };

    // A chunk to generate off the main thread (carries a thread-safe density snapshot).
    struct GenRequest {
        int cx = 0;
        int cz = 0;
        DensitySampler density;
    };
    // The CPU result of generating a chunk: everything but the GPU upload (done on the
    // main thread in update()).
    struct GenResult {
        int cx = 0;
        int cz = 0;
        std::unique_ptr<VoxelField> field;
        std::vector<TreeInstance> trees;
        std::vector<PropInstance> props;
        MeshData terrain;
        MeshData vegetation;
    };

    static i64 key_of(int cx, int cz) {
        return (static_cast<i64>(cx) << 32) | static_cast<i64>(static_cast<u32>(cz));
    }
    IVec2 chunk_of(const Vec3& p) const;
    void mesh_chunk(Chunk& chunk, const vk::Device& device); // re-mesh after an edit
    void retire_mesh(Mesh&& mesh);
    void worker_loop();                       // background generation thread
    GenResult generate(const GenRequest& req) const; // the heavy CPU work, thread-safe

    WorldSampler sampler_;
    f32 voxel_size_;
    int chunk_voxels_;
    int view_radius_;
    f32 chunk_world_;
    // The vertical band each chunk column meshes. It MUST cover the full terrain height range
    // (worldgen::height, capped to worldgen::max_terrain_height) - otherwise tall mountains rise
    // above the meshed band and you walk up the (still-solid) density function on an invisible floor.
    f32 y_min_ = -11.0f;
    f32 y_max_ = 30.0f;
    int y_voxels_;

    std::unordered_map<i64, Chunk> chunks_;
    std::vector<PendingDelete> trash_;

    // Async generation: the main thread enqueues coords + a density snapshot; the
    // worker fills/meshes them on the CPU and posts results back for GPU upload.
    std::thread worker_;
    std::mutex in_mutex_;
    std::condition_variable in_cv_;
    std::deque<GenRequest> in_queue_;
    std::mutex out_mutex_;
    std::vector<GenResult> out_queue_;
    std::unordered_set<i64> pending_; // coords in flight (enqueued, not yet stored)
    std::atomic<bool> stop_{false};
};

} // namespace alryn
