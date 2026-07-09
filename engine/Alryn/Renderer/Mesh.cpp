#include <Alryn/Renderer/Mesh.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <unordered_map>

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

namespace {

// Moller-Trumbore ray/triangle intersection; on hit, `t` is the distance along `dir`.
bool ray_hits_triangle(const Vec3& origin, const Vec3& dir, const Vec3& a, const Vec3& b,
                       const Vec3& c, f32& t) {
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = glm::cross(dir, e2);
    const f32 det = glm::dot(e1, p);
    if (std::abs(det) < 1e-7f) {
        return false; // parallel to the triangle plane
    }
    const f32 inv = 1.0f / det;
    const Vec3 s = origin - a;
    const f32 u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 q = glm::cross(s, e1);
    const f32 v = glm::dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = glm::dot(e2, q) * inv;
    return t > 0.0f;
}

} // namespace

void MeshData::bake_vertex_ao(const std::vector<const MeshData*>& occluders, f32 radius,
                              f32 strength) {
    // Flatten the occluders into one triangle soup (with centroid + bounding radius per
    // triangle for the cheap per-vertex hemisphere prune below).
    struct Tri {
        Vec3 a, b, c;
        Vec3 centroid;
        f32 bound;
    };
    std::vector<Tri> tris;
    for (const MeshData* m : occluders) {
        if (m == nullptr) {
            continue;
        }
        for (usize i = 0; i + 2 < m->indices.size(); i += 3) {
            Tri t{m->vertices[m->indices[i]].position, m->vertices[m->indices[i + 1]].position,
                  m->vertices[m->indices[i + 2]].position, Vec3{0.0f}, 0.0f};
            t.centroid = (t.a + t.b + t.c) * (1.0f / 3.0f);
            t.bound = std::sqrt(std::max({glm::dot(t.a - t.centroid, t.a - t.centroid),
                                          glm::dot(t.b - t.centroid, t.b - t.centroid),
                                          glm::dot(t.c - t.centroid, t.c - t.centroid)}));
            tris.push_back(t);
        }
    }
    if (tris.empty() || vertices.empty() || radius <= 0.0f) {
        return;
    }

    // Coarse spatial hash (cell = radius): a vertex's rays only reach triangles in its
    // 3x3x3 cell neighbourhood, which keeps the bake fast even for a whole house.
    const f32 cell = radius;
    auto cell_of = [cell](f32 v) { return static_cast<i64>(std::floor(v / cell)); };
    auto pack = [](i64 x, i64 y, i64 z) {
        return (x & 0x1FFFFF) | ((y & 0x1FFFFF) << 21) | ((z & 0x1FFFFF) << 42);
    };
    std::unordered_map<i64, std::vector<u32>> grid;
    for (u32 ti = 0; ti < tris.size(); ++ti) {
        const Tri& t = tris[ti];
        const Vec3 lo = glm::min(t.a, glm::min(t.b, t.c));
        const Vec3 hi = glm::max(t.a, glm::max(t.b, t.c));
        for (i64 x = cell_of(lo.x); x <= cell_of(hi.x); ++x) {
            for (i64 y = cell_of(lo.y); y <= cell_of(hi.y); ++y) {
                for (i64 z = cell_of(lo.z); z <= cell_of(hi.z); ++z) {
                    grid[pack(x, y, z)].push_back(ti);
                }
            }
        }
    }

    // Fixed 8-ray hemisphere pattern (tangent space, +Z = the vertex normal): a steep
    // ring and a shallow ring, cosine-weighted so overhead cover matters most.
    struct Sample {
        Vec3 dir;
        f32 weight;
    };
    static constexpr f32 kInvSqrt2 = 0.70710678f;
    static const Sample kSamples[8] = {
        {{0.5f, 0.0f, 0.866f}, 0.866f},   {{0.0f, 0.5f, 0.866f}, 0.866f},
        {{-0.5f, 0.0f, 0.866f}, 0.866f},  {{0.0f, -0.5f, 0.866f}, 0.866f},
        {{0.64f * kInvSqrt2, 0.64f * kInvSqrt2, 0.423f}, 0.423f},
        {{-0.64f * kInvSqrt2, 0.64f * kInvSqrt2, 0.423f}, 0.423f},
        {{-0.64f * kInvSqrt2, -0.64f * kInvSqrt2, 0.423f}, 0.423f},
        {{0.64f * kInvSqrt2, -0.64f * kInvSqrt2, 0.423f}, 0.423f},
    };

    // Per-CELL candidate lists (a cell's 3x3x3 neighbourhood, merged + deduped ONCE and
    // shared by every vertex that falls in the cell). The naive version re-gathered and
    // re-sorted this per VERTEX, which is what made a Debug-build bake crawl.
    std::unordered_map<i64, std::vector<u32>> regions;
    {
        std::vector<u32> merged;
        for (const Vertex& v : vertices) {
            const i64 cx = cell_of(v.position.x), cy = cell_of(v.position.y),
                      cz = cell_of(v.position.z);
            auto [it, inserted] = regions.try_emplace(pack(cx, cy, cz));
            if (!inserted) {
                continue; // this cell's region list is already built
            }
            merged.clear();
            for (i64 x = cx - 1; x <= cx + 1; ++x) {
                for (i64 y = cy - 1; y <= cy + 1; ++y) {
                    for (i64 z = cz - 1; z <= cz + 1; ++z) {
                        if (auto g = grid.find(pack(x, y, z)); g != grid.end()) {
                            merged.insert(merged.end(), g->second.begin(), g->second.end());
                        }
                    }
                }
            }
            std::sort(merged.begin(), merged.end());
            merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
            it->second = merged;
        }
    }

    // A hit this close counts as full occlusion - accept it and stop scanning the
    // candidate list (the near field dominates AO, and dense stone/timber walls would
    // otherwise be scanned in full for every ray).
    const f32 accept_t = radius * 0.25f;

    auto bake_range = [&](usize begin, usize end) {
        std::vector<u32> local; // hemisphere-pruned candidates, reused across vertices
        for (usize vi = begin; vi < end; ++vi) {
            Vertex& v = vertices[vi];
            const Vec3 n = glm::length(v.normal) > 1e-4f ? glm::normalize(v.normal)
                                                         : Vec3{0.0f, 1.0f, 0.0f};
            if (n.y < -0.75f) {
                continue; // downward faces (box undersides) are never really seen
            }
            const auto region = regions.find(
                pack(cell_of(v.position.x), cell_of(v.position.y), cell_of(v.position.z)));
            if (region == regions.end() || region->second.empty()) {
                continue;
            }

            // Hemisphere + range prune: a triangle can only occlude if some part of it
            // pokes ABOVE this vertex's surface plane and lies within the AO radius.
            // One dot product here saves 8 ray tests below - roof verts drop the whole
            // wall soup, wall verts drop everything behind the wall.
            local.clear();
            for (const u32 ti : region->second) {
                const Tri& tri = tris[ti];
                const Vec3 rel = tri.centroid - v.position;
                if (glm::dot(rel, n) + tri.bound < 0.02f) {
                    continue; // entirely behind the surface plane
                }
                const f32 reach = radius + tri.bound;
                if (glm::dot(rel, rel) > reach * reach) {
                    continue; // out of reach of any ray
                }
                local.push_back(ti);
            }
            if (local.empty()) {
                continue;
            }

            // Tangent basis around the normal.
            const Vec3 helper = std::abs(n.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f}
                                                      : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 tx = glm::normalize(glm::cross(helper, n));
            const Vec3 ty = glm::cross(n, tx);
            const Vec3 origin = v.position + n * 0.015f; // lift off the surface

            f32 occlusion = 0.0f;
            f32 total = 0.0f;
            for (const Sample& s : kSamples) {
                const Vec3 dir = tx * s.dir.x + ty * s.dir.y + n * s.dir.z;
                f32 nearest = radius;
                for (const u32 ti : local) {
                    const Tri& tri = tris[ti];
                    f32 t = 0.0f;
                    if (ray_hits_triangle(origin, dir, tri.a, tri.b, tri.c, t) && t > 0.02f &&
                        t < nearest) {
                        nearest = t;
                        if (nearest < accept_t) {
                            break;
                        }
                    }
                }
                if (nearest < radius) {
                    occlusion += s.weight * (1.0f - nearest / radius); // closer = darker
                }
                total += s.weight;
            }
            const f32 factor = 1.0f - strength * (occlusion / total);
            v.color *= factor;
        }
    };

    // Vertices are independent (grid + regions are read-only by now), so the heavy defs
    // (houses) spread across cores - this runs once per prop variant at startup.
    const usize n = vertices.size();
    const usize n_threads =
        std::clamp<usize>(std::thread::hardware_concurrency(), 1, n > 512 ? 16 : 1);
    if (n_threads <= 1) {
        bake_range(0, n);
        return;
    }
    std::vector<std::jthread> workers;
    workers.reserve(n_threads);
    const usize chunk = (n + n_threads - 1) / n_threads;
    for (usize t = 0; t < n_threads; ++t) {
        const usize begin = t * chunk;
        const usize end = std::min(begin + chunk, n);
        if (begin < end) {
            workers.emplace_back(bake_range, begin, end);
        }
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
    vertex_bytes_ = vertex_bytes;

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

void Mesh::update_vertices(const std::vector<Vertex>& vertices) {
    if (vertices.empty() || vertex_bytes_ == 0) {
        return;
    }
    const VkDeviceSize bytes = std::min<VkDeviceSize>(sizeof(Vertex) * vertices.size(), vertex_bytes_);
    vertex_buffer_.upload(vertices.data(), bytes); // host-visible + coherent: visible to the GPU at once
}

void Mesh::destroy() {
    vertex_buffer_.destroy();
    index_buffer_.destroy();
    index_count_ = 0;
    vertex_bytes_ = 0;
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
