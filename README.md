# Alryn Engine

A from-scratch, modern **C++23** game engine rendering with **Vulkan**, aimed at
**low-poly, deformable-terrain** games in the spirit of *Deep Rock Galactic* and
*Astroneer*. Networking is **server-authoritative**: one server owns the
simulation, clients send input/requests, the server broadcasts authoritative
state to everyone.

> Status: early. See [`CLAUDE.md`](CLAUDE.md) for the architecture, conventions,
> and the milestone roadmap.

## Building

### Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
make run          # convenience: build + launch the game
```

### Windows

Prerequisites: **Visual Studio 2022/2026** (MSVC with the C++ workload),
**CMake 3.24+**, and the **LunarG Vulkan SDK** (`winget install
KhronosGroup.VulkanSDK` - provides the headers, `vulkan-1.lib`, and the `glslc`
shader compiler). The installer sets `VULKAN_SDK`; open a fresh terminal after
installing so it is on the environment.

The build uses the multi-config Visual Studio generator:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug --target alryn_game
.\build\bin\Debug\alryn_game.exe
```

Or use the convenience wrapper (configure + build + run, the rough equivalent of
`make run`):

```powershell
.\run.ps1                # windowed client
.\run.ps1 -- 300         # run 300 frames then exit (smoke test)
.\run.ps1 -Config Release
```

## Running the tests

```sh
ctest --test-dir build --output-on-failure     # Linux
ctest --test-dir build -C Debug --output-on-failure   # Windows (multi-config)
# or run the binary directly:  ./build/bin/alryn_tests
```

## Layout

| Path           | Purpose                                                        |
|----------------|---------------------------------------------------------------|
| `engine/`      | The `alryn` static library (Core, Scene, Platform, Renderer…) |
| `game/`        | Sample game built on top of the engine                        |
| `tests/`       | doctest-based unit & integration tests                        |
| `shaders/`     | GLSL sources compiled to SPIR-V at build time                 |
| `cmake/`       | Build helpers (warnings, shader compilation)                  |

## Dependencies

System-first, with network fallbacks: **Vulkan SDK**, **GLFW**, **GLM**,
**doctest** (fetched). See `CLAUDE.md` for the full list and rationale.
