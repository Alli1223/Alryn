#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <cmath>

namespace alryn {

// A static world collider for the lightweight physics layer: vertical cylinders
// (tree trunks) and Y-rotated boxes (house walls). Blocks in xz only; the y range
// gates which heights it affects. Designed to be replaceable by a Jolt body later.
struct Collider {
    enum class Shape : u8 { Cylinder, Box };
    Shape shape = Shape::Cylinder;
    Vec3 center{0.0f}; // world; xz centre, y component unused for the range
    f32 y_min = 0.0f;
    f32 y_max = 0.0f;
    f32 radius = 0.5f; // cylinder radius (xz)
    Vec2 half{0.5f};   // box half-extents in its local xz frame
    f32 yaw = 0.0f;    // box rotation about Y (world)
};

// Pushes a vertical capsule (xz centre `pos`, radius `r`, spanning [foot, foot+height])
// out of `c` if it penetrates. Returns the corrected xz (caller keeps y).
inline Vec2 resolve_collider(const Collider& c, Vec2 pos, f32 r, f32 foot, f32 height) {
    if (foot + height < c.y_min || foot > c.y_max) {
        return pos; // no vertical overlap
    }
    const Vec2 ctr{c.center.x, c.center.z};
    if (c.shape == Collider::Shape::Cylinder) {
        const Vec2 d = pos - ctr;
        const f32 dist = glm::length(d);
        const f32 sep = r + c.radius;
        if (dist < sep) {
            return dist > 1e-4f ? pos + (d / dist) * (sep - dist) : pos + Vec2{sep, 0.0f};
        }
        return pos;
    }

    // Box: resolve in the box's local frame (rotate by -yaw), clamp, push out.
    const f32 cs = std::cos(-c.yaw);
    const f32 sn = std::sin(-c.yaw);
    const Vec2 rel = pos - ctr;
    const Vec2 local{rel.x * cs - rel.y * sn, rel.x * sn + rel.y * cs};
    const Vec2 clamped{glm::clamp(local.x, -c.half.x, c.half.x),
                       glm::clamp(local.y, -c.half.y, c.half.y)};
    const Vec2 delta = local - clamped;
    const f32 dist = glm::length(delta);
    if (dist >= r) {
        return pos;
    }
    Vec2 push_local;
    if (dist > 1e-4f) {
        push_local = (delta / dist) * (r - dist);
    } else {
        // Centre inside the box: eject along the least-penetrating axis.
        const f32 px = c.half.x - std::abs(local.x);
        const f32 pz = c.half.y - std::abs(local.y);
        if (px < pz) {
            push_local = Vec2{(local.x < 0.0f ? -1.0f : 1.0f) * (px + r), 0.0f};
        } else {
            push_local = Vec2{0.0f, (local.y < 0.0f ? -1.0f : 1.0f) * (pz + r)};
        }
    }
    const f32 cw = std::cos(c.yaw);
    const f32 sw = std::sin(c.yaw);
    return pos + Vec2{push_local.x * cw - push_local.y * sw, push_local.x * sw + push_local.y * cw};
}

} // namespace alryn
