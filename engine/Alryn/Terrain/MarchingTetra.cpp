#include <Alryn/Terrain/MarchingTetra.h>

#include <Alryn/Terrain/VoxelField.h>

#include <algorithm>
#include <utility>

namespace alryn::mc {

namespace {

// Cube corner offsets (Paul Bourke ordering).
constexpr IVec3 kCorner[8] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},
                              {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}};

// Kuhn decomposition: 6 tetrahedra sharing the main diagonal (corner 0 - corner 6).
// The same decomposition for every cube makes shared faces split identically, so
// neighbouring cubes meet without cracks.
constexpr int kTet[6][4] = {{0, 6, 1, 2}, {0, 6, 2, 3}, {0, 6, 3, 7},
                            {0, 6, 7, 4}, {0, 6, 4, 5}, {0, 6, 5, 1}};

bool lex_less(const IVec3& a, const IVec3& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// Interpolates the isosurface crossing on an edge. Always interpolates from the
// lexicographically-smaller corner so the same edge yields a bit-identical point
// from any tetrahedron that shares it -> watertight welding.
Vec3 interp_edge(const IVec3& ga, f32 va, const Vec3& pa, const IVec3& gb, f32 vb, const Vec3& pb,
                 f32 iso) {
    if (!lex_less(ga, gb)) {
        return interp_edge(gb, vb, pb, ga, va, pa, iso);
    }
    const f32 denom = vb - va;
    const f32 t = denom != 0.0f ? std::clamp((iso - va) / denom, 0.0f, 1.0f) : 0.5f;
    return pa + (pb - pa) * t;
}

} // namespace

MeshData polygonize(const VoxelField& field, const IVec3& cell_min, const IVec3& cell_max, f32 iso,
                    const ColorFn& colorize) {
    MeshData mesh;

    // Emits one triangle, orienting the flat normal toward air (away from solid).
    auto emit = [&](Vec3 p0, Vec3 p1, Vec3 p2, const Vec3& inside_centroid) {
        Vec3 normal = glm::cross(p1 - p0, p2 - p0);
        const f32 length = glm::length(normal);
        if (length <= 1e-12f) {
            return; // degenerate
        }
        normal /= length;
        const Vec3 centroid = (p0 + p1 + p2) / 3.0f;
        if (glm::dot(normal, centroid - inside_centroid) < 0.0f) {
            normal = -normal;
            std::swap(p1, p2);
        }
        const Vec3 color = colorize(centroid, normal);
        const auto base = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back({p0, normal, color});
        mesh.vertices.push_back({p1, normal, color});
        mesh.vertices.push_back({p2, normal, color});
        mesh.indices.push_back(base);
        mesh.indices.push_back(base + 1);
        mesh.indices.push_back(base + 2);
    };

    for (int z = cell_min.z; z < cell_max.z; ++z) {
        for (int y = cell_min.y; y < cell_max.y; ++y) {
            for (int x = cell_min.x; x < cell_max.x; ++x) {
                const IVec3 base{x, y, z};
                IVec3 gc[8];
                f32 val[8];
                Vec3 pos[8];
                for (int i = 0; i < 8; ++i) {
                    gc[i] = base + kCorner[i];
                    val[i] = field.value(gc[i]);
                    pos[i] = field.world_position(gc[i]);
                }

                for (const auto& tet : kTet) {
                    int inside[4];
                    int outside[4];
                    int n_inside = 0;
                    int n_outside = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (val[tet[k]] < iso) {
                            inside[n_inside++] = k;
                        } else {
                            outside[n_outside++] = k;
                        }
                    }
                    if (n_inside == 0 || n_inside == 4) {
                        continue;
                    }

                    Vec3 inside_centroid{0.0f};
                    for (int k = 0; k < n_inside; ++k) {
                        inside_centroid += pos[tet[inside[k]]];
                    }
                    inside_centroid /= static_cast<f32>(n_inside);

                    auto edge = [&](int a, int b) {
                        return interp_edge(gc[tet[a]], val[tet[a]], pos[tet[a]], gc[tet[b]],
                                           val[tet[b]], pos[tet[b]], iso);
                    };

                    if (n_inside == 1) {
                        const int a = inside[0];
                        emit(edge(a, outside[0]), edge(a, outside[1]), edge(a, outside[2]),
                             inside_centroid);
                    } else if (n_inside == 3) {
                        const int o = outside[0];
                        emit(edge(o, inside[0]), edge(o, inside[1]), edge(o, inside[2]),
                             inside_centroid);
                    } else { // exactly two inside -> a quad (two triangles)
                        const int a = inside[0];
                        const int b = inside[1];
                        const int c = outside[0];
                        const int d = outside[1];
                        const Vec3 ac = edge(a, c);
                        const Vec3 ad = edge(a, d);
                        const Vec3 bd = edge(b, d);
                        const Vec3 bc = edge(b, c);
                        emit(ac, ad, bd, inside_centroid);
                        emit(ac, bd, bc, inside_centroid);
                    }
                }
            }
        }
    }
    return mesh;
}

} // namespace alryn::mc
