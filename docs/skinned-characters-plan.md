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

## Task list (one item per loop)
1. **[done] SkinnedMesh data model + CPU skinning.** `Character/SkinnedMesh.h` - a vertex with bone
   indices + weights, the per-bone inverse-bind matrices, and `skin(mesh, jointMatrices) -> MeshData`.
   Pure maths, tested (a vertex weighted to a bone follows it; bind pose = identity).
2. **Mesh dynamic update.** `Mesh::update_vertices(MeshData)` re-uploads to the host-visible vertex
   buffer in place (same vertex count), so a character mesh can be re-skinned every frame.
3. **Procedural humanoid body mesh.** `Character/BodyMesh.{h,cpp}` - build a continuous low-poly
   humanoid `SkinnedMesh` from the skeleton: lofted tube limbs that SHARE a ring at each joint (so
   they're one surface), a torso, a head, hands + feet. Per-vertex bone weights blended across joints
   so elbows/knees bend smoothly. The big, iterative piece - compare to the references.
4. **Client skinned rendering.** A per-character dynamic `Mesh`; each frame skin by the animator's
   joint matrices + upload + draw via the lit pipeline. Replace `draw_rig`'s primitive loop.
5. **Headless preview.** Skin at a static / posed pose and render, so the harness compares the skinned
   character to the references (`asset_preview character`).
6. **Colour zones.** Vertices carry a material id (skin / cloth / hair / metal / accent / …) resolved
   from the palette, so the one mesh is multi-coloured.
7. **Skinned outfit / armour.** Per role, generate the outfit as additional skinned geometry (a
   contoured breastplate, pauldrons that flow into the arms, a draping robe) deformed by the bones -
   Plate / Leather / Holy / Robe - iterated against the references.
8. **Animation blend.** Verify walk + swing/cast/block deform the skinned mesh smoothly (the joint
   weights make the bends continuous); a headless check + posed renders.
9. **NPCs.** Villagers / enemies use the same skinned body.
10. **Perf.** Profile the per-frame skin + upload for many characters; cache / cull / LOD as needed.
11. **Final pass.** Skin-maths + body-mesh tests, posed renders vs the references, full suite green,
    docs.

## Conventions
- Keep the skinning + mesh generation **headless-testable** (pure data + maths in `Character/`).
- Re-render with `make character` after each change and compare to the reference art.
- Every loop ends green (build + tests) and is committed.
