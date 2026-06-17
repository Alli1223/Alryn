#pragma once

#include <Alryn/Character/CharacterAppearance.h>
#include <Alryn/Combat/Enemy.h>
#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <cmath>
#include <span>

namespace alryn {

// Villager tuning, shared by the server simulation and the headless tests.
inline constexpr f32 kVillagerMaxHealth = 40.0f;
inline constexpr f32 kVillagerSpeed = 1.6f;      // strolling pace (m/s)
inline constexpr f32 kVillagerFleeSpeed = 2.4f;  // panicked run - slower than an enemy,
                                                 // so stragglers get caught unless the
                                                 // player intervenes (that's the point)
inline constexpr f32 kVillagerRadius = 0.35f;
inline constexpr f32 kVillagerFleeRadius = 9.0f; // notices an enemy within this range

// Firefighting: villagers (and players) carry water from the town well to douse
// burning houses. A full bucket is 1.0; pouring drains it while knocking the fire down.
inline constexpr f32 kWellRange = 2.6f;    // how close to the well to refill
inline constexpr f32 kDouseRange = 4.2f;   // how close to a fire's centre to pour on it
inline constexpr f32 kDouseRate = 0.55f;   // fire knocked down per second of pouring
inline constexpr f32 kWaterUseRate = 0.4f; // bucket drained per second of pouring

// A server-simulated townsfolk NPC: the thing the player defends. It wanders the
// town by day, sleeps in its bed at night, and flees from enemies. Identity is
// deterministic per (town, house) so it is stable across stream/cull. id, position,
// yaw, health and appearance are networked; the rest is server state.
struct Villager {
    u32 id = 0;
    Vec3 position{0.0f};
    Vec3 bed{0.0f};         // where it sleeps at night
    Vec2 home_center{0.0f}; // town centre, for day wandering
    Vec3 target{0.0f};      // current wander goal
    f32 home_half = 18.0f;
    f32 yaw = 0.0f;
    f32 speed = 0.0f;       // current move speed (for the client's gait)
    f32 health = kVillagerMaxHealth;
    f32 water = 0.0f;     // bucket fill 0..1 (for firefighting)
    f32 wait = 0.0f;
    f32 attack_cd = 0.0f; // guards: time until the next swing
    u32 rng = 1u;
    u8 kind = 0; // 0 = villager, 1 = guard (hunts + fights the enemy)
    CharacterAppearance appearance;
    bool alive = true;
};

// A deterministic per-villager look (skin/hair/face), stable for a given id so a
// townsperson keeps the same appearance whenever they stream back in.
inline CharacterAppearance villager_look(u32 id) {
    auto h = [&](u32 s) {
        u32 v = (id * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return v >> 9;
    };
    CharacterAppearance a;
    a.skin = static_cast<u8>(h(1) % 6u);
    a.hair_color = static_cast<u8>(h(2) % 6u);
    a.eyes = static_cast<EyeStyle>(h(3) % 4u);
    a.ears = static_cast<EarStyle>(h(4) % 3u);
    a.hair = static_cast<HairStyle>(h(5) % 5u);
    return a;
}

// Steers a villager toward `goal` for `dt` (move, follow the ground, face heading),
// pushing out of props. Mirrors step_enemy; the server picks `goal`/`speed` from the
// day/night + flee logic so the motion itself stays testable.
inline void step_villager(Villager& v, const DensitySampler& density,
                          std::span<const Collider> colliders, const Vec3& goal, Timestep dt,
                          f32 speed = kVillagerSpeed) {
    const f32 dts = dt.seconds;
    if (!v.alive || dts <= 0.0f) {
        return;
    }
    Vec3 to = goal - v.position;
    to.y = 0.0f;
    const f32 d = glm::length(to);
    if (d > 0.12f) {
        const Vec3 dir = to / d;
        const f32 step = std::min(d, speed * dts);
        Vec2 xz{v.position.x + dir.x * step, v.position.z + dir.z * step};
        for (const Collider& c : colliders) {
            xz = resolve_collider(c, xz, kVillagerRadius, v.position.y, kEnemyHeight);
        }
        v.position.x = xz.x;
        v.position.z = xz.y;
        v.yaw = std::atan2(dir.z, dir.x);
        v.speed = speed;
    } else {
        v.speed = 0.0f;
    }
    const Vec3 from{v.position.x, v.position.y + 6.0f, v.position.z};
    if (const auto ground = raycast_density(density, from, Vec3{0.0f, -1.0f, 0.0f}, 20.0f)) {
        v.position.y = ground->y;
    }
}

} // namespace alryn
