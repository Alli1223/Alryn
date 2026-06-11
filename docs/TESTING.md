# Testing & TDD workflow

Alryn grows in small, test-verified steps. Every feature ships with tests in the
same change, and **every checkpoint ends on a green build + green tests**. This
doc is the practical guide to working that way.

## TL;DR

```sh
make test                 # build + run the whole suite (what CI runs)
make ci                   # clean configure + build + test (mirror GitHub CI exactly)

make test-physics         # just the physics + collision tests (fast inner loop)
make test-terrain         # terrain meshing + world scatter (trees/props/vegetation)
make test-ui              # UI widgets + vector font
make test-net             # networking protocol + integration
make test-character       # character model + animation
make test-render          # renderer: device, buffers, offscreen + scene shots
make test-core            # core types/math/events
make test-scene           # scene graph / components / camera

make test-filter FILTER="*collision*"   # any subset by test-case name
make list-tests                          # list every test case
make shots                               # (re)generate offscreen screenshots
```

One doctest binary (`build/bin/alryn_tests`) holds every test; the `test-*`
targets just filter it by source file, so you build once and iterate on one area.

## The TDD loop

1. **Write the test first.** Add a `TEST_CASE` that asserts the new behaviour (or
   reproduces the bug). Put it in the file for that subsystem (`physics_tests.cpp`,
   `terrain_tests.cpp`, ...).
2. **Watch it fail** for the right reason: `make test-physics` (or whichever area).
   A regression test that passes before you fix anything isn't testing the fix.
3. **Make it pass** with the smallest change.
4. **Re-run the area, then the whole suite** (`make test`) to check for regressions.
5. Keep the build warning-free (first-party targets use the strict warning set).

### Worked example: the collider-orientation fix

The "you can walk through some rotated obstacles" bug was a rotation mismatch
between the collider transform and the mesh transform. The fix was guarded like this:

- Added `place_box: a rotated box keeps its orientation` in `physics_tests.cpp` -
  a **single isolated box** placed at a yaw, checked along its length (blocked) and
  off its thin side (clear). Isolating one box matters: with many props around, other
  mis-oriented colliders can mask the bug at any single point.
- Confirmed it **fails** with the old `c.yaw = +(yaw)` and **passes** with the fixed
  `c.yaw = -(yaw)` (`Physics/Collider.h::place_box`). You can reproduce the check by
  flipping that sign and running `make test-physics`.

## Testing philosophy

- **Logic is tested headlessly** - maths, scene graph, components, serialization,
  terrain meshing, collisions, UI widget behaviour, networking. These run anywhere,
  including CI, with no GPU or display.
- **GPU/display tests degrade gracefully.** A Vulkan device test, the offscreen
  render test, and the scene-shot tests all *skip* (not fail) when there is no
  usable device or the shaders aren't compiled. Window tests skip with no display.
- **Determinism.** World generation is a pure function of the seed, so scatter tests
  assert exact, repeatable results (e.g. a chunk's baked vegetation mesh has the same
  vertex count every time, and the tree scatter is reproducible over a chunk sweep).

## Visual confirmation (screenshots)

Some things are easiest to confirm by eye. Two headless paths produce images with
no window, so they work over SSH and can run/skip in CI:

- `tests/support/OffscreenRenderer` - a tiny reusable headless renderer (same
  `mesh.vert/frag` as the game, sun-lit, no shadows). It draws any meshes to an
  offscreen image and reads the pixels back.
- `tests/scene_shot_tests.cpp` - renders a **forest sampler** (the five tree
  variants, a fallen log, fern, mushroom, tall grass, …) and writes
  `build/bin/forest.ppm`, plus a pixel smoke check. `render_offscreen_tests.cpp`
  writes `offscreen_cube.ppm`.

```sh
make shots          # writes build/bin/*.ppm (and *.png if ImageMagick is present)
```

CI uploads any generated `*.ppm` as a build artifact for inspection. To inspect a
live, fully-shaded in-game scene (emissive glow, shadows, day/night), run the
windowed game and screenshot the window with your compositor's tool (e.g. `grim`).

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and every pull request:
installs the toolchain (GCC 14) + Vulkan loader + windowing dev libs, configures,
builds the engine/game/tests, and runs `ctest`. GPU/display tests skip on the
runner; all the logic tests run. Run the same thing locally with `make ci`.

## Adding tests for a new feature

1. Pick (or add) the subsystem test file.
2. Write `TEST_CASE`s for the happy path **and** the edge/regression cases.
3. If it's visual, add a scene shot via `OffscreenRenderer` with a pixel smoke check.
4. Register new test files in `tests/CMakeLists.txt`.
5. `make test` green before you call it done.
