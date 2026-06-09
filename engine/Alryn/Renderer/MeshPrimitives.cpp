#include <Alryn/Renderer/MeshPrimitives.h>

namespace alryn::primitives {

namespace {

// Appends one quad (two triangles, CCW from the outside) with a shared flat
// normal and colour.
void add_quad(MeshData& mesh, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              const Vec3& normal, const Vec3& color) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({a, normal, color});
    mesh.vertices.push_back({b, normal, color});
    mesh.vertices.push_back({c, normal, color});
    mesh.vertices.push_back({d, normal, color});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
}

} // namespace

MeshData cube(f32 size, const Vec3& color) {
    const f32 h = size * 0.5f;
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);

    // Each face: 4 corners CCW as seen from outside, plus its outward normal.
    add_quad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1}, color);   // +Z
    add_quad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1}, color); // -Z
    add_quad(mesh, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0}, color);   // +X
    add_quad(mesh, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0}, color); // -X
    add_quad(mesh, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0}, color);   // +Y
    add_quad(mesh, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0}, color); // -Y
    return mesh;
}

MeshData grid(u32 cells, f32 cell_size, const Vec3& color) {
    MeshData mesh;
    const f32 extent = static_cast<f32>(cells) * cell_size;
    const f32 origin = -extent * 0.5f;
    const Vec3 up{0.0f, 1.0f, 0.0f};

    for (u32 z = 0; z < cells; ++z) {
        for (u32 x = 0; x < cells; ++x) {
            const f32 x0 = origin + static_cast<f32>(x) * cell_size;
            const f32 z0 = origin + static_cast<f32>(z) * cell_size;
            const f32 x1 = x0 + cell_size;
            const f32 z1 = z0 + cell_size;

            // Subtle checkerboard tint so the flat ground reads as separate cells.
            const bool dark = ((x + z) & 1u) != 0;
            const Vec3 cell_color = dark ? color * 0.85f : color;

            add_quad(mesh, {x0, 0.0f, z1}, {x1, 0.0f, z1}, {x1, 0.0f, z0}, {x0, 0.0f, z0}, up,
                     cell_color);
        }
    }
    return mesh;
}

} // namespace alryn::primitives
