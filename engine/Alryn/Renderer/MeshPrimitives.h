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

// A rounded sausage (a cylinder with domed hemispherical caps) of diameter 1 and
// total height 1, centred. Scale by a bone's dimensions for soft, bubbly limbs.
MeshData capsule(int sides = 12, int cap_rings = 3, const Vec3& color = Vec3{1.0f});

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

// A low-poly fern: a rosette of arching, drooping fronds rooted at y = 0. Reads as
// forest-floor bracken. `variant` changes the frond count/spread.
MeshData fern(int variant = 0, const Vec3& color = Vec3{0.20f, 0.42f, 0.22f});

// A taller grassy clump - wider, more upright arching blades than grass_tuft, for
// meadow / forest-clearing variety.
MeshData tall_grass(int blades = 6, const Vec3& color = Vec3{0.34f, 0.52f, 0.26f});

// A low-poly mushroom: pale stem + a coloured cap, sitting on y = 0. `spots` adds
// white flecks (the classic toadstool). Scale for tiny sprouts to fat boletes.
MeshData mushroom(const Vec3& cap = Vec3{0.74f, 0.18f, 0.15f}, f32 scale = 1.0f, bool spots = true);

// A small leafy ground plant (clover / forest herb): a few flat leaflets fanned
// low to the ground around a short stem.
MeshData ground_leaf(int variant = 0, const Vec3& color = Vec3{0.26f, 0.46f, 0.24f});

// Tall marsh reeds / cattails for the bog: a clump of thin, upright arching blades
// rooted at y = 0, some topped with a brown cattail seed-head. Sways in the wind.
MeshData reed(int blades = 4, const Vec3& color = Vec3{0.34f, 0.44f, 0.26f});

// A low-poly desert cactus (saguaro) rooted at y = 0: a tall ribbed green column with
// one or two up-bent arms, capped with rounded tips. `variant` toggles the arm count.
MeshData cactus(int variant = 0, const Vec3& color = Vec3{0.30f, 0.45f, 0.26f});

// A fallen log lying along +X on y = 0 (octagonal), bark sides with paler cut ends
// and a hint of moss on top. A collidable bit of forest-floor debris.
MeshData fallen_log(int variant = 0, const Vec3& color = Vec3{0.34f, 0.26f, 0.18f});

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
// `variant`. Cool stone grey by default (faces vary in shade, upward faces get
// moss/lichen, and the base tapers flush to the ground).
MeshData rock(int variant = 0, const Vec3& color = Vec3{0.40f, 0.42f, 0.47f});

// An axis-aligned box from (min) to (max) with one flat normal per face, colour
// `color`. Building block for houses, lanterns and other prop geometry.
MeshData box(const Vec3& min, const Vec3& max, const Vec3& color);

// A wooden CRATE in the box (min..max): a light planked body `color` framed by darker reinforcing
// corner posts + a base rail and a lid-seam band (standing a hair proud), so it reads as a slatted
// crate rather than a plain box. Frame thickness scales with the box, so it works at any crate size.
MeshData crate(const Vec3& min, const Vec3& max, const Vec3& color);

} // namespace alryn::primitives
