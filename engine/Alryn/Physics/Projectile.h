#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <span>

namespace alryn {

// A dynamic physics body (thrown rock / ball). Server-simulated; only the position
// is networked. The same step works for any simple sphere body - the foundation
// for "other physics items".
struct Projectile {
    Vec3 position{0.0f};
    Vec3 velocity{0.0f};
    f32 radius = 0.18f;
    f32 life = 6.0f; // seconds remaining
    u32 owner = 0;
    u8 kind = 0;          // 0 = thrown rock, 1 = enemy arrow
    bool hostile = false; // an enemy arrow: damages the town side, not enemies
    bool alive = true;
    bool resting = false;
};

namespace detail {

// Surface normal from the density gradient (points out of solid).
inline Vec3 density_normal(const DensitySampler& d, const Vec3& p) {
    const f32 e = 0.15f;
    const Vec3 g{d({p.x + e, p.y, p.z}) - d({p.x - e, p.y, p.z}),
                 d({p.x, p.y + e, p.z}) - d({p.x, p.y - e, p.z}),
                 d({p.x, p.y, p.z + e}) - d({p.x, p.y, p.z - e})};
    const f32 len = glm::length(g);
    return len > 1e-5f ? g / len : Vec3{0.0f, 1.0f, 0.0f};
}

} // namespace detail

// Integrates one projectile for `dt`: gravity, movement, then bounces off terrain
// (about the surface normal) and props (about the push direction), settling to rest
// when slow. Decrements life and clears `alive` when spent.
inline void step_projectile(Projectile& pr, const DensitySampler& density,
                            std::span<const Collider> colliders, Timestep dt,
                            f32 gravity = 18.0f, f32 restitution = 0.5f) {
    const f32 dts = dt.seconds;
    if (!pr.alive || dts <= 0.0f) {
        return;
    }
    pr.life -= dts;
    if (pr.life <= 0.0f) {
        pr.alive = false;
        return;
    }
    if (pr.resting) {
        return; // sits where it landed until its life runs out
    }

    pr.velocity.y -= gravity * dts;
    Vec3 next = pr.position + pr.velocity * dts;

    // Terrain bounce: if the step would enter solid, reflect off the surface.
    if (density(next) < 0.0f) {
        const Vec3 n = detail::density_normal(density, next);
        const f32 vn = glm::dot(pr.velocity, n);
        pr.velocity = (pr.velocity - 2.0f * vn * n) * restitution;
        next = pr.position + n * 0.02f;
        if (glm::length(pr.velocity) < 1.3f) {
            pr.velocity = Vec3{0.0f};
            pr.resting = true;
        }
    }

    // Prop bounce: reflect off the xz push direction of trees/walls.
    for (const Collider& c : colliders) {
        const Vec2 before{next.x, next.z};
        const Vec2 after = resolve_collider(c, before, pr.radius, next.y - pr.radius, pr.radius * 2.0f);
        const Vec2 push = after - before;
        if (glm::length(push) > 1e-4f) {
            const Vec2 n2 = glm::normalize(push);
            const Vec3 n{n2.x, 0.0f, n2.y};
            const f32 vn = glm::dot(pr.velocity, n);
            if (vn < 0.0f) {
                pr.velocity = (pr.velocity - 2.0f * vn * n) * restitution;
            }
            next.x = after.x;
            next.z = after.y;
        }
    }

    pr.position = next;
}

} // namespace alryn
