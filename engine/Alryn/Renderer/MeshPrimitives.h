#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

namespace alryn::primitives {

// Axis-aligned cube centred at the origin. 24 vertices / 36 indices so each face
// has its own flat normal (the faceted low-poly look).
MeshData cube(f32 size = 1.0f, const Vec3& color = Vec3{0.8f, 0.8f, 0.8f});

// Flat ground plane in the XZ plane, centred at the origin, made of cells x cells
// quads. Also used as the water surface (its vertices are wave-displaced in the
// water shader).
MeshData grid(u32 cells = 16, f32 cell_size = 1.0f, const Vec3& color = Vec3{0.35f, 0.5f, 0.3f});

// Unit (diameter 1, centred) low-poly round shapes - flat-shaded for the faceted
// look. Scale by a bone's dimensions like the cube. Default white so a per-draw
// tint can colour them.
MeshData sphere(int longitude = 10, int latitude = 7, const Vec3& color = Vec3{1.0f});
MeshData cylinder(int sides = 10, const Vec3& color = Vec3{1.0f});

// A low-poly tree split into an opaque trunk and (alpha-blendable) foliage so the
// foliage can fade when a player walks under it. The trunk base sits at y = 0.
struct TreeMeshData {
    MeshData trunk;
    MeshData foliage;
};
TreeMeshData tree(int variant);

} // namespace alryn::primitives
