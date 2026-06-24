#include <Alryn/Terrain/StreamingTerrain.h>

#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Terrain/MarchingTetra.h>
#include <Alryn/Terrain/PropScatter.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/VegetationScatter.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace alryn {

namespace {

constexpr int kTrashHold = 3;     // frames to keep a retired mesh before destroying it
constexpr int kUploadBudget = 4;  // generated chunks uploaded to the GPU per frame
constexpr int kEditMeshBudget = 4; // chunks re-meshed after an edit per frame

} // namespace

StreamingTerrain::StreamingTerrain(u32 seed, f32 voxel_size, int chunk_voxels, int view_radius)
    : sampler_(seed), voxel_size_(voxel_size), chunk_voxels_(chunk_voxels),
      view_radius_(view_radius), chunk_world_(static_cast<f32>(chunk_voxels) * voxel_size),
      y_voxels_(static_cast<int>((y_max_ - y_min_) / voxel_size)) {
    worker_ = std::thread(&StreamingTerrain::worker_loop, this);
}

StreamingTerrain::~StreamingTerrain() {
    stop_.store(true);
    in_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

IVec2 StreamingTerrain::chunk_of(const Vec3& p) const {
    return IVec2{static_cast<int>(std::floor(p.x / chunk_world_)),
                 static_cast<int>(std::floor(p.z / chunk_world_))};
}

// The heavy, thread-safe part of streaming a chunk: build + fill the voxel field,
// scatter trees/props, polygonise the terrain and bake the vegetation - all on the
// CPU using only the immutable seed + the density snapshot. No GPU, no shared state.
StreamingTerrain::GenResult StreamingTerrain::generate(const GenRequest& req) const {
    const u32 seed = sampler_.seed();
    GenResult res;
    res.cx = req.cx;
    res.cz = req.cz;
    const Vec3 origin{static_cast<f32>(req.cx) * chunk_world_, y_min_,
                      static_cast<f32>(req.cz) * chunk_world_};
    const IVec3 dims{chunk_voxels_ + 1, y_voxels_ + 1, chunk_voxels_ + 1};
    res.field = std::make_unique<VoxelField>(dims, voxel_size_, origin);
    // Column-cached fill: density(p) = p.y - height(p.x,p.z) + sparse edits. height() (the expensive
    // multi-octave noise) is CONSTANT down a column, so compute it once per (x,z) and just subtract y
    // per voxel - so the tall band that covers the mountains costs ~one height() per column, not per
    // voxel. (set() clamps to [-1,1], matching the old fill.)
    for (int xi = 0; xi < dims.x; ++xi) {
        for (int zi = 0; zi < dims.z; ++zi) {
            const f32 wx = origin.x + static_cast<f32>(xi) * voxel_size_;
            const f32 wz = origin.z + static_cast<f32>(zi) * voxel_size_;
            const f32 h = worldgen::height(wx, wz, req.seed);
            for (int yi = 0; yi < dims.y; ++yi) {
                const f32 wy = origin.y + static_cast<f32>(yi) * voxel_size_;
                f32 value = wy - h;
                for (const WorldEdit& e : req.edits) { // sparse runtime deformations, usually none
                    const f32 d = glm::length(Vec3{wx, wy, wz} - e.center);
                    if (d < e.radius) {
                        value += e.amount * (1.0f - d / e.radius);
                    }
                }
                res.field->set(xi, yi, zi, value);
            }
        }
    }
    res.trees = scatter_trees(req.cx, req.cz, chunk_world_, seed);
    res.props = scatter_props(req.cx, req.cz, chunk_world_, seed);
    res.terrain = mc::polygonize(
        *res.field, IVec3{0}, res.field->cell_count(), 0.0f, [seed](const Vec3& p, const Vec3& n) {
            const f32 up = glm::clamp(n.y, 0.0f, 1.0f);
            Vec3 c = roads::tint_surface(worldgen::surface_color(p, n, seed), p, up, seed);
            return town_path_tint(c, p, up, seed); // dirt paths between houses + market
        });
    res.vegetation = build_vegetation(req.cx, req.cz, chunk_world_, seed);
    return res;
}

// The background worker: pulls generation requests and posts finished CPU data back.
void StreamingTerrain::worker_loop() {
    for (;;) {
        GenRequest req;
        {
            std::unique_lock<std::mutex> lock(in_mutex_);
            in_cv_.wait(lock, [this] { return stop_.load() || !in_queue_.empty(); });
            if (stop_.load()) {
                return;
            }
            req = std::move(in_queue_.front());
            in_queue_.pop_front();
        }
        GenResult res = generate(req);
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            out_queue_.push_back(std::move(res));
        }
    }
}

void StreamingTerrain::retire_mesh(Mesh&& mesh) {
    if (mesh.valid()) {
        trash_.push_back({std::move(mesh), kTrashHold});
    }
}

// Re-meshes a chunk's terrain after an edit (the field already exists + is edited).
// Vegetation is seed-only, so it's left untouched. Synchronous, but edits touch only
// a couple of nearby chunks, on a deliberate player action.
void StreamingTerrain::mesh_chunk(Chunk& chunk, const vk::Device& device) {
    if (!chunk.field) {
        return;
    }
    const u32 seed = sampler_.seed();
    const MeshData data = mc::polygonize(
        *chunk.field, IVec3{0}, chunk.field->cell_count(), 0.0f,
        [seed](const Vec3& pos, const Vec3& normal) {
            const f32 up = glm::clamp(normal.y, 0.0f, 1.0f);
            Vec3 c = roads::tint_surface(worldgen::surface_color(pos, normal, seed), pos, up, seed);
            return town_path_tint(c, pos, up, seed);
        });
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

    // 2. Enqueue nearby chunks (nearest rings first) that aren't loaded or already in
    //    flight, then re-sort the whole pending queue by distance to the CURRENT focus so
    //    the worker always builds the chunk closest to the player next - even after the
    //    player has moved (stale far requests from a previous position fall to the back).
    //    The worker fills + meshes them off the main thread, so movement never stalls.
    {
        std::lock_guard<std::mutex> lock(in_mutex_);
        for (int r = 0; r <= view_radius_; ++r) {
            for (int dz = -r; dz <= r; ++dz) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != r) {
                        continue; // only the ring at Chebyshev distance r
                    }
                    const int cx = center.x + dx;
                    const int cz = center.y + dz; // IVec2 stores (chunk_x, chunk_z)
                    const i64 key = key_of(cx, cz);
                    if (chunks_.count(key) == 0 && pending_.count(key) == 0) {
                        in_queue_.push_back({cx, cz, sampler_.seed(), sampler_.edits()});
                        pending_.insert(key);
                    }
                }
            }
        }
        auto dist2 = [&](const GenRequest& q) {
            const i64 ddx = q.cx - center.x;
            const i64 ddz = q.cz - center.y;
            return ddx * ddx + ddz * ddz;
        };
        std::sort(in_queue_.begin(), in_queue_.end(),
                  [&](const GenRequest& a, const GenRequest& b) { return dist2(a) < dist2(b); });
    }
    in_cv_.notify_all();

    // 3. Upload finished chunks (just a host-visible memcpy - cheap), a few per frame.
    std::vector<GenResult> ready;
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        const int take = std::min<int>(kUploadBudget, static_cast<int>(out_queue_.size()));
        ready.insert(ready.end(), std::make_move_iterator(out_queue_.begin()),
                     std::make_move_iterator(out_queue_.begin() + take));
        out_queue_.erase(out_queue_.begin(), out_queue_.begin() + take);
    }
    const int unload_dist = view_radius_ + 2;
    for (GenResult& res : ready) {
        pending_.erase(key_of(res.cx, res.cz));
        // Drop results for chunks the focus has already moved away from.
        if (std::max(std::abs(res.cx - center.x), std::abs(res.cz - center.y)) > unload_dist) {
            continue;
        }
        Chunk chunk;
        chunk.coord = IVec2{res.cx, res.cz};
        chunk.field = std::move(res.field);
        chunk.trees = std::move(res.trees);
        chunk.props = std::move(res.props);
        if (!res.terrain.indices.empty()) {
            chunk.mesh.create(device, res.terrain);
        }
        if (!res.vegetation.indices.empty()) {
            chunk.vegetation.create(device, res.vegetation);
        }
        chunk.needs_mesh = false;
        chunk.vegetation_built = true;
        chunks_.emplace(key_of(res.cx, res.cz), std::move(chunk));
    }

    // 4. Re-mesh chunks dirtied by edits (synchronous; few per frame).
    int edit_budget = kEditMeshBudget;
    for (auto& [key, chunk] : chunks_) {
        if (edit_budget <= 0) {
            break;
        }
        if (chunk.needs_mesh) {
            mesh_chunk(chunk, device);
            --edit_budget;
        }
    }

    // 5. Unload chunks that drifted out of range.
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        const int dx = std::abs(it->second.coord.x - center.x);
        const int dz = std::abs(it->second.coord.y - center.y);
        if (std::max(dx, dz) > unload_dist) {
            retire_mesh(std::move(it->second.mesh));
            retire_mesh(std::move(it->second.vegetation));
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace alryn
