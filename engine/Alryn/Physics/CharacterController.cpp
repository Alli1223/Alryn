#include <Alryn/Physics/CharacterController.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace alryn {

CharacterController::CharacterController(CharacterConfig config) : config_(config) {}

void CharacterController::set_position(const Vec3& feet) {
    position_ = feet;
    velocity_ = Vec3{0.0f};
    on_ground_ = false;
}

bool CharacterController::wall_at(const DensitySampler& density, const Vec3& feet) const {
    const f32 r = config_.radius;
    const std::array<Vec2, 5> offsets = {Vec2{0.0f, 0.0f}, Vec2{r, 0.0f}, Vec2{-r, 0.0f},
                                         Vec2{0.0f, r}, Vec2{0.0f, -r}};
    // Sample above the step height so small steps don't count as walls.
    const std::array<f32, 3> heights = {config_.step_height + 0.2f, config_.height * 0.5f,
                                        config_.height - 0.1f};
    for (const Vec2& o : offsets) {
        for (f32 h : heights) {
            if (density(feet + Vec3{o.x, h, o.y}) < 0.0f) {
                return true;
            }
        }
    }
    return false;
}

std::optional<f32> CharacterController::ground_height(const DensitySampler& density, f32 x, f32 z,
                                                      f32 top_y) const {
    const auto hit =
        raycast_density(density, Vec3{x, top_y, z}, Vec3{0.0f, -1.0f, 0.0f}, top_y + 60.0f);
    if (hit) {
        return hit->y;
    }
    return std::nullopt;
}

void CharacterController::update(const DensitySampler& density, const Vec3& move_dir, bool jump,
                                 Timestep dt, std::span<const Collider> colliders) {
    const f32 dts = dt.seconds;
    if (dts <= 0.0f) {
        return;
    }

    Vec3 horizontal{move_dir.x, 0.0f, move_dir.z};
    const f32 len = glm::length(horizontal);
    if (len > 1.0f) {
        horizontal /= len;
    }

    Vec3 p = position_;

    // Horizontal movement, resolved per axis and blocked by walls.
    {
        Vec3 candidate = p;
        candidate.x += horizontal.x * config_.walk_speed * dts;
        if (!wall_at(density, candidate)) {
            p.x = candidate.x;
        }
    }
    {
        Vec3 candidate = p;
        candidate.z += horizontal.z * config_.walk_speed * dts;
        if (!wall_at(density, candidate)) {
            p.z = candidate.z;
        }
    }

    // Push the capsule out of solid props (trees, house walls). A couple of
    // iterations settle overlapping colliders (e.g. a wall corner).
    if (!colliders.empty()) {
        Vec2 xz{p.x, p.z};
        for (int iter = 0; iter < 2; ++iter) {
            for (const Collider& c : colliders) {
                xz = resolve_collider(c, xz, config_.radius, p.y, config_.height);
            }
        }
        p.x = xz.x;
        p.z = xz.y;
    }

    // Vertical: gravity/jump in the air, ground-following when grounded.
    const f32 top = p.y + config_.height + 2.0f;
    const std::optional<f32> ground = ground_height(density, p.x, p.z, top);

    if (!on_ground_) {
        velocity_.y = std::max(velocity_.y - config_.gravity * dts, -config_.max_fall);
        p.y += velocity_.y * dts;
        if (ground && p.y <= *ground) {
            p.y = *ground;
            velocity_.y = 0.0f;
            on_ground_ = true;
        }
    } else if (jump) {
        velocity_.y = config_.jump_speed;
        on_ground_ = false;
        p.y += velocity_.y * dts;
    } else if (ground) {
        const f32 diff = *ground - p.y;
        if (std::abs(diff) <= config_.step_height) {
            p.y = *ground; // follow the slope / small step
            velocity_.y = 0.0f;
        } else if (*ground < p.y - config_.step_height) {
            on_ground_ = false; // walked off an edge
            velocity_.y = 0.0f;
        }
        // ground far above (cliff) => stay; the wall check blocks walking into it
    } else {
        on_ground_ = false; // nothing underneath
        velocity_.y = 0.0f;
    }

    velocity_.x = horizontal.x * config_.walk_speed;
    velocity_.z = horizontal.z * config_.walk_speed;
    position_ = p;
}

} // namespace alryn
