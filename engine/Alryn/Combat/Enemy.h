#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <cmath>
#include <span>

namespace alryn {

// Combat tuning, shared by the server simulation and the headless tests.
inline constexpr f32 kEnemyMaxHealth = 60.0f;
inline constexpr f32 kBruteMaxHealth = 170.0f; // a brute (kind 2) is a tough, slow wall

// Enemy kinds: 0 = grunt, 1 = torch-bearer (lights the night + burns houses),
// 2 = brute (bigger, much tougher, slower, hits harder).
inline f32 enemy_max_health(u8 kind) { return kind == 2 ? kBruteMaxHealth : kEnemyMaxHealth; }
inline constexpr f32 kEnemySpeed = 2.7f;       // march speed (m/s)
inline constexpr f32 kEnemyRadius = 0.4f;      // collision radius (xz)
inline constexpr f32 kEnemyHeight = 1.7f;      // capsule height for prop collision
inline constexpr f32 kEnemyAttackRange = 1.5f; // how close it gets before swinging
inline constexpr f32 kEnemyAttackDamage = 11.0f;
inline constexpr f32 kEnemyAttackInterval = 1.1f; // seconds between swings

inline constexpr f32 kPlayerMaxHealth = 100.0f;
inline constexpr f32 kPlayerRegen = 5.0f;       // hp/sec once out of combat
inline constexpr f32 kPlayerRegenDelay = 6.0f;  // seconds after a hit before regen

inline constexpr f32 kMeleeRange = 2.7f;        // player melee reach
inline constexpr f32 kMeleeDamage = 34.0f;      // per swing
inline constexpr f32 kMeleeConeCos = 0.35f;     // ~69° half-cone in front
inline constexpr f32 kThrowDamage = 28.0f;      // a thrown rock hitting an enemy

// A server-simulated hostile NPC. It marches toward `home` (the town plaza it is
// attacking), chases and strikes players that come near, and dies when its health
// runs out. Only id/position/yaw/kind/health are networked; the rest is server
// state. Pure data + maths so the AI is headless-testable, like Projectile.
struct Enemy {
    u32 id = 0;
    Vec3 position{0.0f};
    Vec3 home{0.0f}; // the objective it marches on (server-only)
    f32 yaw = 0.0f;
    f32 health = kEnemyMaxHealth;
    f32 attack_cd = 0.0f; // seconds until it can swing again
    u8 kind = 0;
    bool alive = true;
};

// True if `target` lies within `range` and inside the cone of half-angle
// acos(cone_cos) centred on the +xz heading `yaw`. Used for the player's melee
// swing (and reusable for any directional hit test).
inline bool in_attack_cone(const Vec3& origin, f32 yaw, const Vec3& target, f32 range,
                           f32 cone_cos) {
    Vec3 to = target - origin;
    to.y = 0.0f;
    const f32 d = glm::length(to);
    if (d > range || d < 1e-4f) {
        return d <= range; // point-blank counts as a hit
    }
    const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
    return glm::dot(to / d, facing) >= cone_cos;
}

// Integrates one enemy for `dt`: steers toward `goal` along the ground, pushing out
// of props, and faces its heading. Attack cooldown ticks down. Does not itself deal
// damage (the server decides that once it is in range) so the motion stays testable.
inline void step_enemy(Enemy& e, const DensitySampler& density,
                       std::span<const Collider> colliders, const Vec3& goal, Timestep dt,
                       f32 speed = kEnemySpeed) {
    const f32 dts = dt.seconds;
    if (!e.alive || dts <= 0.0f) {
        return;
    }
    Vec3 to = goal - e.position;
    to.y = 0.0f;
    const f32 d = glm::length(to);
    if (d > 0.05f) {
        const Vec3 dir = to / d;
        const f32 step = std::min(d, speed * dts);
        Vec2 xz{e.position.x + dir.x * step, e.position.z + dir.z * step};
        for (const Collider& c : colliders) {
            xz = resolve_collider(c, xz, kEnemyRadius, e.position.y, kEnemyHeight);
        }
        e.position.x = xz.x;
        e.position.z = xz.y;
        e.yaw = std::atan2(dir.z, dir.x);
    }
    // Follow the terrain surface (drop onto the ground from just above).
    const Vec3 from{e.position.x, e.position.y + 6.0f, e.position.z};
    if (const auto ground = raycast_density(density, from, Vec3{0.0f, -1.0f, 0.0f}, 20.0f)) {
        e.position.y = ground->y;
    }
    if (e.attack_cd > 0.0f) {
        e.attack_cd -= dts;
    }
}

} // namespace alryn
