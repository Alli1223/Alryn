#pragma once

// Central place for the sample game's tunable constants - the values a maintainer
// is most likely to want to change, gathered out of the rendering/update code so
// they are named and discoverable rather than scattered magic numbers.

#include <Alryn/Core/Types.h>
#include <Alryn/Platform/Events.h>

#include <chrono>
#include <cstdlib>
#include <random>

namespace alryn::game {

// The UDP port the listen/dedicated server binds and clients connect to.
inline constexpr u16 kPort = 24650;

// The world seed the dedicated / in-process listen server starts from (clients receive it in the
// Welcome and regenerate identical terrain from it). It's RANDOMISED once per process launch, so
// every game starts in a fresh world - set `ALRYN_SEED=<number>` to pin a specific world (to replay
// a layout you liked, or for reproducible tests). Computed once and cached for the process so the
// whole launch agrees on one seed; the chosen seed is logged on join.
inline u32 world_seed() {
    static const u32 seed = []() -> u32 {
        if (const char* env = std::getenv("ALRYN_SEED")) {
            return static_cast<u32>(std::strtoul(env, nullptr, 10));
        }
        std::random_device rd;
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        return static_cast<u32>(rd()) ^ static_cast<u32>(now);
    }();
    return seed;
}

// Keyboard scancodes used by the client (GLFW GLFW_KEY_* values).
namespace key {
inline constexpr KeyCode W = 87, A = 65, S = 83, D = 68, E = 69, F = 70, H = 72, K = 75, M = 77,
                         U = 85, Space = 32, Escape = 256;
inline constexpr KeyCode Digit1 = 49, Digit2 = 50, Digit3 = 51, Digit4 = 52;
inline constexpr KeyCode Ctrl = 341, CtrlR = 345; // GLFW left/right control - Mage cast modifier
inline constexpr bool is_ctrl(KeyCode k) { return k == Ctrl || k == CtrlR; }
} // namespace key

// Fixed isometric-style third-person camera angle (the world rotates under it).
namespace iso {
inline constexpr f32 yaw_deg = 45.0f;   // compass direction we look from
inline constexpr f32 pitch_deg = 48.0f; // downward tilt (higher = more top-down)
inline constexpr f32 distance = 15.0f;  // default camera pull-back (smaller = closer)
inline constexpr f32 fov_deg = 30.0f;   // low-ish fov -> flatter, more "iso" look
} // namespace iso

// Third-person camera follow + scroll-wheel zoom.
namespace cam {
inline constexpr f32 min_distance = 4.0f;   // closest zoom
inline constexpr f32 max_distance = 45.0f;  // furthest zoom
inline constexpr f32 zoom_step = 0.88f;     // distance *= zoom_step^scroll
inline constexpr f32 target_height = 0.7f;  // look-at lifted above the player's feet
inline constexpr f32 near_plane = 0.5f;
inline constexpr f32 far_plane = 400.0f;
} // namespace cam

// Day/night cycle defaults (overridable via ALRYN_TIME / ALRYN_DAY_SECONDS).
namespace daynight {
inline constexpr f32 start_time = 0.35f;          // 0=midnight, 0.25=sunrise, 0.5=noon
inline constexpr f32 default_day_seconds = 120.0f; // length of a full cycle
inline constexpr f32 min_day_seconds = 5.0f;       // clamp for ALRYN_DAY_SECONDS
} // namespace daynight

// Client-side particle pool (ability VFX, projectile trails).
namespace vfx {
inline constexpr usize max_particles = 1400; // hard cap so a busy fight can't run away
} // namespace vfx

// Dynamic lighting cull radii: only sources near the player are submitted at night,
// keeping a lit-up town cheap.
namespace light {
inline constexpr f32 prop_cull_dist = 38.0f;       // house/lantern interior lights
inline constexpr f32 wagon_lamp_cull_dist = 40.0f; // a wagon's hanging lamp
} // namespace light

// Full-screen world map overlay (M).
namespace map {
inline constexpr f32 view_world = 480.0f; // metres from centre to the shorter edge
inline constexpr f32 margin_frac = 0.07f; // border around the map panel
} // namespace map

} // namespace alryn::game
