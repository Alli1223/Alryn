#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Renderer/Mesh.h> // MeshData

#include <functional>

namespace alryn {

class VoxelField;

namespace mc {

// Per-vertex colour from world position + (flat) face normal.
using ColorFn = std::function<Vec3(const Vec3& world_pos, const Vec3& normal)>;

// Extracts the isosurface of `field` over cells [cell_min, cell_max) into
// flat-shaded MeshData (unique vertices per triangle, per-face normals oriented
// toward air). Uses marching tetrahedra (Kuhn 6-tet decomposition), which is
// watertight by construction and produces the faceted low-poly look.
MeshData polygonize(const VoxelField& field, const IVec3& cell_min, const IVec3& cell_max, f32 iso,
                    const ColorFn& colorize);

} // namespace mc
} // namespace alryn
