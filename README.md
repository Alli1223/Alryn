# Alryn Engine

A from-scratch, modern **C++23** game engine rendering with **Vulkan**, aimed at
**low-poly, deformable-terrain** games in the spirit of *Deep Rock Galactic* and
*Astroneer*. Networking is **server-authoritative**: one server owns the
simulation, clients send input/requests, the server broadcasts authoritative
state to everyone.

> Status: early. See [`CLAUDE.md`](CLAUDE.md) for the architecture, conventions,
> and the milestone roadmap.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

## Running the tests

```sh
ctest --test-dir build --output-on-failure
# or run the binary directly:
./build/bin/alryn_tests
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
