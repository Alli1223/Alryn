#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <cmath>
#include <functional>
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
    f32 taunt_cd = 0.0f;  // seconds it stays fixated on `taunt_by` (Knight taunt)
    u32 taunt_by = 0;     // player id that taunted it (forces it to target them)
    u8 kind = 0;
    bool alive = true;
    Vec3 knockback{0.0f}; // a hit shoves it back; this velocity decays each step (server-only)
    f32 sunder_cd = 0.0f; // shield-bearer: while > 0 its guard is broken (a heavy blow staggered it)
};

// Knockback on hits: an enemy gets shoved back along the hit direction at a velocity proportional to
// the damage (capped), decaying over ~half a second - so a solid hit reads with impact + opens space.
inline constexpr f32 kKnockbackPerDamage = 0.13f; // m/s of knockback per point of damage
inline constexpr f32 kKnockbackMax = 6.5f;        // cap (a huge hit can't fling an enemy across the map)
inline constexpr f32 kKnockbackDecay = 7.0f;      // exp decay rate (higher = snappier stop)

// Shield-bearer (kind 4): raises a shield that soaks most of a hit landing within its frontal arc,
// so it shrugs off attacks from the front and must be FLANKED (or shoved around) to be felled fast.
inline constexpr u8 kEnemyShield = 4u;
inline constexpr f32 kShieldBlockCos = 0.32f;  // shield covers the frontal arc (~71 deg half-angle)
inline constexpr f32 kShieldReduction = 0.80f; // fraction of a frontal hit the shield soaks
// SUNDER: a single HEAVY blow (a charged ability, an empowered swing - above this threshold) staggers
// a shield-bearer, dropping its guard for a few seconds so follow-up hits land full. Below the
// threshold, basic attacks just bounce off the front (the shield stays meaningful - flank or hit hard).
inline constexpr f32 kSunderThreshold = 55.0f; // raw damage that breaks the guard (basics 14-42 don't)
inline constexpr f32 kSunderDuration = 2.5f;   // seconds the guard stays down after a sunder

// True if a shield-bearer would block a hit arriving from world position `from` (the attacker's
// position, or a point back along a projectile's path) - i.e. `from` lies within its frontal arc AND
// its guard isn't currently sundered.
inline bool enemy_blocks_hit(const Enemy& e, const Vec3& from) {
    if (e.kind != kEnemyShield || e.sunder_cd > 0.0f) {
        return false;
    }
    Vec2 to{from.x - e.position.x, from.z - e.position.z};
    const f32 d = glm::length(to);
    if (d < 1e-4f) {
        return false;
    }
    const Vec2 facing{std::cos(e.yaw), std::sin(e.yaw)};
    return glm::dot(to / d, facing) >= kShieldBlockCos;
}

// Healer (kind 5): a fragile support raider that hangs back out of reach and mends the most-wounded
// nearby ally - so a wave can sustain itself unless the players FOCUS the healer down first.
inline constexpr u8 kEnemyHealer = 5u;
inline constexpr f32 kHealerRange = 7.0f;     // mends allies within this radius
inline constexpr f32 kHealerHealRate = 14.0f; // hp/sec funnelled to the most-wounded ally
inline constexpr f32 kHealerKeepDist = 9.0f;  // hangs this far back from its target (kites to stay safe)

// Index into `enemies` of the most-wounded living ally (below max health) within `range` of `healer`,
// excluding the healer itself; -1 if none needs mending. Pure, so the healer AI is headless-testable.
inline int most_wounded_ally(const Enemy& healer, std::span<const Enemy> enemies, f32 range) {
    int best = -1;
    f32 worst = 1.0f; // lowest health fraction wins (most wounded)
    for (usize i = 0; i < enemies.size(); ++i) {
        const Enemy& e = enemies[i];
        if (e.id == healer.id || !e.alive) {
            continue;
        }
        const f32 maxh = enemy_max_health(e.kind);
        if (e.health >= maxh - 0.01f) {
            continue; // already at full health
        }
        if (glm::length(e.position - healer.position) > range) {
            continue;
        }
        const f32 frac = e.health / maxh;
        if (frac < worst) {
            worst = frac;
            best = static_cast<int>(i);
        }
    }
    return best;
}

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
                       f32 speed = kEnemySpeed,
                       const std::function<f32(f32, f32)>& platform = {}) {
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
    // Knockback: a recent hit slides the enemy back (resolved against colliders), decaying to rest -
    // so a solid blow reads with impact + briefly opens space before it presses on toward the goal.
    if (const f32 kb = glm::length(Vec2{e.knockback.x, e.knockback.z}); kb > 0.05f) {
        Vec2 kxz{e.position.x + e.knockback.x * dts, e.position.z + e.knockback.z * dts};
        for (const Collider& c : colliders) {
            kxz = resolve_collider(c, kxz, kEnemyRadius, e.position.y, kEnemyHeight);
        }
        e.position.x = kxz.x;
        e.position.z = kxz.y;
        e.knockback *= std::exp(-kKnockbackDecay * dts);
    } else {
        e.knockback = Vec3{0.0f};
    }
    // Follow the terrain surface (drop onto the ground from just above), or a bridge deck when
    // crossing one: the optional `platform` (e.g. roads::bridge_height) is used when it sits near the
    // feet - the deck is flush with the banks at the ends, so they walk onto it smoothly - but NOT
    // when it's well above (so an enemy in the river isn't yanked up onto a bridge passing overhead).
    const Vec3 from{e.position.x, e.position.y + 6.0f, e.position.z};
    if (const auto ground = raycast_density(density, from, Vec3{0.0f, -1.0f, 0.0f}, 20.0f)) {
        f32 gy = ground->y;
        if (platform) {
            const f32 ph = platform(e.position.x, e.position.z);
            if (ph > -1e8f && ph > gy && ph <= e.position.y + 1.0f) {
                gy = ph;
            }
        }
        e.position.y = gy;
    }
    if (e.attack_cd > 0.0f) {
        e.attack_cd -= dts;
    }
    if (e.sunder_cd > 0.0f) {
        e.sunder_cd -= dts; // the shield-bearer's guard recovers after a sunder
    }
}

} // namespace alryn
