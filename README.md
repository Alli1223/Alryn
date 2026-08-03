<div align="center">

# ⚔️ ALRYN

### *A medieval wagon-escort adventure*

[![CI](https://github.com/Alli1223/Alryn/actions/workflows/ci.yml/badge.svg)](https://github.com/Alli1223/Alryn/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Alli1223/Alryn?label=download&color=c9a227)](https://github.com/Alli1223/Alryn/releases/latest)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)](https://github.com/Alli1223/Alryn/releases/latest)
[![Vulkan](https://img.shields.io/badge/renderer-Vulkan%201.3-red)](https://vulkan.org)

</div>

---

## The game

A cart of goods needs to reach the next town. The road does not want it to.

**Alryn** is a co-op, isometric fantasy adventure about hauling wagons between towns
across a world that is generated fresh every time you launch it. Gather in the market
square, weigh up the contracts on offer — longer routes and rougher roads pay better —
then either hire a driver and guard the cart, or grab the tongue and haul it yourself
for a bigger cut. Bandits will find you somewhere out in the hills. Some contracts carry
a noble in a covered carriage, and the raiders know exactly who is riding inside.

Bring friends. One player can hold a road, but nobody holds it for long.

## ✨ Highlights

- **⚒️ Four roles** — the **Knight** tanks and taunts, the **Hunter** kills at range, the
  **Cleric** mends and smites, the **Mage** chains elemental combos. Each has its own
  weapon, stat block and skill tree.
- **🧝 Three races** — Men, Dwarves and Elves, with real mechanical passives. An Elf throws
  an ally the farthest; a Dwarf lands the hardest. Yes, you can toss the dwarf.
- **🛒 Wagon contracts** — vote on the route, hire a driver or haul it manually, survive the
  ambush, deliver for coin. Escort the VIP carriage if the pay is worth the trouble.
- **🌍 A new world every launch** — procedural terrain, roads, forests and towns full of
  villagers going about their day. The ground itself is deformable.
- **🌙 A world that turns** — a full day/night cycle with lantern-lit towns after dark, and
  weather that builds from clear skies into a proper storm.
- **🌐 Drop-in co-op** — server-authoritative multiplayer. Host from the menu, or join a
  friend by IP.

## 🎮 Controls

| | |
|---|---|
| **WASD** / **Space** | Move / jump |
| **Shift** | Dodge roll |
| **Left click** | Primary attack (sword, arrow, spell) |
| **1–4** | Abilities — for the Mage, elements (hold **Ctrl** to combo) |
| **E** | Hitch / unhitch a wagon |
| **H** | Vote: hire a driver or haul it yourself |
| **G** | Ally toss |
| **M** / **K** / **U** | World map / skills / wardrobe |
| **Esc** | Pause |

Gamepads are supported.

---

## 📦 Download & play

1. Grab the build for your platform from the
   [**Releases**](https://github.com/Alli1223/Alryn/releases/latest) page —
   `Alryn-…-windows-x64.zip` or `Alryn-…-linux-x64.tar.gz`.
2. Extract it anywhere (keep the `shaders` folder next to the executable).
3. Run **`alryn_game.exe`** — or `./alryn_game` on Linux.

**You'll need:**

| | |
|---|---|
| **OS** | Windows 10 / 11 (64-bit), or Linux with glibc 2.39+ (Ubuntu 24.04 and newer) |
| **GPU** | Any GPU with up-to-date **Vulkan 1.3** drivers (NVIDIA / AMD / Intel, ~2018 or newer) |
| **Multiplayer** | UDP port **24650** open on the host |

## 🔨 Build from source

Needs **CMake 3.24+**, a **C++23** compiler, and the
[**LunarG Vulkan SDK**](https://vulkan.lunarg.com/sdk/home) (for `glslc` and the loader).
Everything else — GLFW, GLM, ENet, miniaudio — is fetched automatically.

<details>
<summary><b>Windows</b> (Visual Studio 2022 / 2026)</summary>

```powershell
winget install KhronosGroup.VulkanSDK   # then open a fresh terminal
.\run.ps1                               # configure + build + play
.\run.ps1 -Config Release               # optimised build
```

Or drive CMake directly:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target alryn_game
.\build\bin\Release\alryn_game.exe
```

</details>

<details>
<summary><b>Linux</b> (GCC 14+)</summary>

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/alryn_game
```

</details>

Run the tests with `ctest --test-dir build --output-on-failure` (add `-C Debug` on Windows).

<div align="center">
<sub>Built on a from-scratch C++23 Vulkan engine — no game engine, no middleware.</sub>
</div>
