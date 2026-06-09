#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

namespace alryn::primitives {

// Axis-aligned cube centred at the origin. 24 vertices / 36 indices so each face
// has its own flat normal (the faceted low-poly look).
MeshData cube(f32 size = 1.0f, const Vec3& color = Vec3{0.8f, 0.8f, 0.8f});

// Flat ground plane in the XZ plane, centred at the origin, made of cells x cells
// quads. A stand-in floor until the marching-cubes terrain lands.
MeshData grid(u32 cells = 16, f32 cell_size = 1.0f, const Vec3& color = Vec3{0.35f, 0.5f, 0.3f});

} // namespace alryn::primitives
