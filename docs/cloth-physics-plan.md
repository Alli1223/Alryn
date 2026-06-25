# Cloth Physics for Character Outfits

Branch: `skinned-characters` (continues the character work). Goal: the **flowing** cloth on outfits
(robe skirts, capes, tabards, stoles, mantles, hoods, loose sleeves) moves with **physics** - it
sways with movement, blows in the wind, and can be **detached** (cut by a hit, torn off by a storm,
or a test key) to fall to the ground. A **core under-layer** (the body's tunic + shorts, woven into
the skinned body) is permanent, so a character who loses their robe is still decently clothed.

## Decisions (from the user)
- **Client-side visual only** - each client simulates + reacts to hits/wind locally; no protocol or
  server changes. Cloth loss is cosmetic.
- **Detach triggers**: taking a hit (cut), strong wind / storms (blown off), and a debug/test key.
- **Sim**: spring-bone (mass-spring) cloth - cheap, integrates with the skinned-mesh pipeline, good
  for capes/robes. Not a full per-vertex cloth solver (overkill for low-poly + many characters).

## Architecture
- **Spring-bone chains.** A flowing cloth piece is a CHAIN of cloth bones hanging from an ANCHOR on
  the body (cape: from the upper back/shoulders; skirt: a ring of chains from the waist; tabard: down
  the chest). Each frame the chain is simulated: each bone springs toward its rest direction, pulled
  by gravity + dragged by the anchor's world motion (inertia) + pushed by wind, with damping. Pure
  maths in `Character/ClothRig.{h,cpp}`, headless-testable.
- **Skinned cloth mesh.** Each piece's mesh (a tapered strip / panel / cone) is a `SkinnedMesh`
  weighted down its chain, so it bends as the chain swings - drawn through the existing lit pipeline
  like the body/outfit. The cloth bones live *after* the body bones in the matrix array the mesh is
  skinned with (body joints from `CharacterModel::joint_matrices` + the simulated cloth-bone frames).
- **The under-layer stays.** The skinned BODY already carries a dark under-tunic (Shirt) + shorts
  (Pants) zone. That is the permanent layer. The flowing cloth pieces sit ON TOP and are the only
  detachable things; the tight base + rigid armour (plate, bracers, helm) stay attached.
- **Detachment.** A piece can be released: its anchor stops driving the chain, the chain falls under
  gravity (keeping its momentum), the whole piece drifts + tumbles to the ground and fades out. A
  client `ClothInstance` per piece per character holds the sim state + an `attached` flag.

## Status: COMPLETE
All phases done (branch `skinned-characters`). Every **flowing** outfit piece now simulates as
spring-bone cloth that hangs, sways with movement, blows in the wind, drapes over the body, and can be
**cut (on a hit), blown off (in a storm) or dropped (debug key `C`)** - fluttering to the ground and
leaving the permanent under-layer (the body's tunic + shorts) on:
- **Capes** (paladin / high prophet / beastmaster) + **robe skirts** (Mage / Cleric, an 8-panel tube) +
  the minor pieces - **tabard** (paladin), **stole** (priest / prophet), **mantle** (warden).
- The tight base (tunic torso, trousers, armour) + **hoods** stay rigid (a hood is snug on the head,
  not flowing) - those are the under-layer / fixed gear.
- Body-collision cylinder keeps skirts outside the legs; far-off characters skip the sim (perf).
Commits: phase 1 `a5243e1`, 2a `5cf0659`, 2b `4c747bf`, 3 `5a9c961`, 4 `6683d15`, minor pieces (this).

## Phases (each ends green + committed)
1. **Cloth sim core + a cape.** `ClothRig` spring-bone chain sim (gravity + wind + anchor inertia +
   damping) + a cloth-strip `SkinnedMesh` builder. Wire ONE piece - a back **cape** - into the client
   for cloth-wearers; step it each frame from the character's anchor + the renderer wind. Verify: a
   unit test (the chain settles below the anchor under gravity; deflects downwind) + a settled-pose
   render.
2. **Flowing-piece refactor.** Move the flowing cloth OUT of the rigid skinned outfit / decorative
   primitives into `ClothInstance`s: robe skirts (Mage/Cleric), tabard + cape (Knight paladin),
   stole/mantle (Cleric), warden/beastmaster cape (Hunter), loose sleeves + hood where they flow. The
   tight base (tunic torso, trousers, armour) stays rigid on the body bones.
3. **Detachment.** Release a piece -> free-fall + settle + fade. Triggers: a hit (the client sees the
   local/▒remote player's health drop in the snapshot), strong wind (the weather amount crosses a
   threshold, per-piece chance), and a debug key. The under-layer remains.
4. **Polish.** Body-collision push-out (cloth doesn't sink through the legs), per-role cloth tuning
   (stiffness/length/wind response), perf (skip distant sims, reuse the skin scratch), and tests.

## Conventions
- Sim + mesh stay headless-testable (pure data + maths in `Character/`); the client only wires anchors,
  wind, dt + draws. Re-render with `asset_preview` (an `ALRYN_POSE=cloth` settled-pose mode) to verify.
- Every phase ends green (build + tests) and is committed.
