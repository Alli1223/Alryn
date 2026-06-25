# Skinned-Mesh Characters

Branch: `skinned-characters` (off `player-equipment-overhaul`). Goal: replace the stacked-primitive
character rendering with a **continuous humanoid mesh** whose vertices are **weighted to the skeleton
bones** (linear-blend skinning), so limbs are one smooth surface that bends at the joints — matching
the smooth, contoured reference art instead of reading as rounded boxes.

## Architecture (decided)
- **CPU skinning into a per-character dynamic mesh.** Each frame: compute the animator's posed JOINT
  matrices, CPU-skin the bind-pose mesh (`pos' = Σ wᵢ · jointPosedᵢ · invBindᵢ · pos`), and re-upload
  the vertices to a host-visible `Mesh`, drawn through the **existing lit pipeline** — so the skinned
  character keeps shadows / fog / hemispheric ambient / tonemap for free, with **no new shader,
  pipeline or descriptor**. Character counts are low (a few players + nearby NPCs), so the per-frame
  skin + upload is cheap.
- The skeleton stays `CharacterModel` (bones + `CharacterAnimator` poses). The body becomes a generated
  skinned mesh instead of per-bone primitives. Weapons keep attaching to the hand joint (rigid).

## Task list (one item per loop) — COMPLETE
1. **[done] SkinnedMesh data model + CPU skinning.** `Character/SkinnedMesh.{h,cpp}` - a vertex with
   bone indices + weights, the per-bone inverse-bind matrices, and `skin(mesh, jointMatrices, out,
   palette)`. Pure maths, tested (a vertex weighted to a bone follows it; bind pose = identity).
2. **[done] Mesh dynamic update.** `Mesh::update_vertices(verts)` re-uploads to the host-visible vertex
   buffer in place (same vertex count), so a character mesh can be re-skinned every frame.
3. **[done] Procedural humanoid body mesh.** `Character/BodyMesh.{h,cpp}` - a continuous low-poly
   humanoid `SkinnedMesh` from the skeleton: lofted octagonal tube limbs/torso/neck sharing a ring at
   each joint, head/hand spheres, a foot sweep, rounded deltoid shoulders, smooth normals. Per-vertex
   weights blended across joints so elbows/knees/shoulders/hips bend smoothly. Helpers live in
   `Character/SkinBuilder.h` (shared with the outfit).
4. **[done] Client skinned rendering.** Each `PlayerVisual` carries the body `SkinnedMesh` + a dynamic
   `Mesh`; `skin_and_draw` skins in LOCAL space with the posed joints, re-uploads, and draws at the
   world `root` via the lit pipeline. Face/hair/gear ride on top as primitives (`Bone::attachment`;
   `draw_rig(attachments_only)`). The core body + joint fillers are the skinned mesh.
5. **[done] Headless preview.** `asset_preview character <role>` mirrors the live path (skinned body +
   outfit + attachment primitives + weapon); `ALRYN_POSE`/`ALRYN_TIER` drive posed/tiered renders.
6. **[done] Colour zones.** `BodyMaterial` mirrors `BoneColor` 1:1 (skin/shirt/pants/hair + primary/
   accent/metal/dark/glow); a shared `body_material_color(palette, mat)` resolves both body + outfit.
7. **[done] Skinned outfit / armour.** `Character/OutfitMesh.{h,cpp}` builds the worn equipment as
   continuous skinned geometry - armoured/clothed limbs, a torso shell, a draping skirt - weighted to
   the same bones so it flows with the body. Plate / Robe / Holy / Leather. `Outfit.cpp` now emits only
   the decorative silhouette pieces (pauldrons/helm/tabard/hood/quiver/…) on top.
8. **[done] Animation blend.** Swing/cast/block overlay deforms the skinned mesh smoothly (verified by
   posed renders + a headless test: the upper-body action shifts the mesh while the legs keep walking).
9. **[done] NPCs.** Villagers + enemies render through the same `skin_and_draw` (per-kind tints, held
   weapons on top). A deferred `mesh_graveyard_` frees a slain/culled NPC's GPU mesh past the frames
   in flight.
10. **[done] Perf.** Realistic counts are low (server-culled); `skin_and_draw` distance-culls beyond
    `character::skin_cull_dist` (100 m) so a pathological town/battle doesn't skin far-off-screen NPCs.
11. **[done] Final pass.** Skin-maths + body/outfit mesh tests + action-blend test; the four-role
    roster renders as continuous skinned figures matching the reference style; full suite green.

## Result
The whole roster - players (Knight / Hunter / Cleric / Mage), villagers, guards, archers, and every
enemy kind - now renders as a **single continuous skinned mesh that bends at the joints**, with the
worn outfit as continuous skinned geometry over it, instead of stacked rigid primitives. Drawn through
the existing lit pipeline (shadows / fog / hemispheric ambient / tonemap for free), re-skinned +
re-uploaded each frame, distance-culled. CPU linear-blend skinning; no new shader, pipeline or
descriptor. Branch `skinned-characters`.

## Conventions
- Keep the skinning + mesh generation **headless-testable** (pure data + maths in `Character/`).
- Re-render with `asset_preview character <role>` after each change and compare to the reference art.
- Every loop ends green (build + tests) and is committed.
