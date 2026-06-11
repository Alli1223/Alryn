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

// Unit box (-0.5..0.5) with its 12 edges and 8 corners chamfered by `bevel` -
// a rectangular shape with softly rounded (faceted) corners.
MeshData rounded_box(f32 bevel = 0.12f, const Vec3& color = Vec3{1.0f});

// A small clump of grass: `blades` tapered triangles fanned around the origin,
// rooted at y = 0, with a dark-to-light vertical colour gradient. Cheap (one
// triangle per blade) so thousands can be baked into a chunk's vegetation mesh.
MeshData grass_tuft(int blades = 4, const Vec3& color = Vec3{0.33f, 0.55f, 0.27f});

// A low-poly flower: a thin crossed stem rooted at y = 0 topped with a flat
// `blossom`-coloured petal ring around a yellow centre.
MeshData flower(const Vec3& blossom = Vec3{0.90f, 0.30f, 0.35f});

// A low-poly tree split into an opaque trunk and (alpha-blendable) foliage so the
// foliage can fade when a player walks under it. The trunk base sits at y = 0.
struct TreeMeshData {
    MeshData trunk;
    MeshData foliage;
};
TreeMeshData tree(int variant);

// A low-poly bush: a small cluster of leafy blobs rooted at y = 0 (no trunk).
MeshData bush(int variant = 0, const Vec3& color = Vec3{0.22f, 0.42f, 0.20f});

// A faceted low-poly boulder sitting on y = 0, deterministically deformed by
// `variant`. Greyish stone by default.
MeshData rock(int variant = 0, const Vec3& color = Vec3{0.46f, 0.46f, 0.50f});

// An axis-aligned box from (min) to (max) with one flat normal per face, colour
// `color`. Building block for houses, lanterns and other prop geometry.
MeshData box(const Vec3& min, const Vec3& max, const Vec3& color);

} // namespace alryn::primitives
