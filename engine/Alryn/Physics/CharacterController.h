#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

#include <functional>
#include <optional>
#include <span>

namespace alryn {

struct CharacterConfig {
    f32 radius = 0.4f;
    f32 height = 1.8f;       // capsule height (feet to head)
    f32 eye_height = 1.6f;   // camera height above feet
    f32 walk_speed = 6.0f;   // m/s
    f32 jump_speed = 8.0f;   // m/s initial
    f32 gravity = 22.0f;     // m/s^2
    f32 max_fall = 45.0f;    // terminal velocity
    f32 step_height = 0.6f;  // max slope/step to climb without jumping
};

// A simple kinematic FPS controller that collides against a DensitySampler
// (so it works against the infinite, streamed world): walls block horizontal
// motion, the feet snap to the surface to walk slopes/steps, and gravity +
// jumping handle airtime. Position is the feet; camera sits at eye_position().
class CharacterController {
public:
    explicit CharacterController(CharacterConfig config = {});

    void set_position(const Vec3& feet);
    const Vec3& position() const { return position_; } // feet
    Vec3 eye_position() const { return position_ + Vec3{0.0f, config_.eye_height, 0.0f}; }
    const Vec3& velocity() const { return velocity_; }
    bool on_ground() const { return on_ground_; }
    // Launch the capsule ballistically (an Ally Toss / a knockback): `v.xz` is an UNCLAMPED horizontal
    // velocity carried until landing (unlike walk input, which is clamped to walk_speed); `v.y` is the
    // upward kick. The launch integrates + is wall-checked in update() and clears on landing.
    void launch(const Vec3& v) {
        launch_vel_ = Vec3{v.x, 0.0f, v.z};
        velocity_.y = v.y;
        on_ground_ = false;
    }
    bool airborne_launch() const { return glm::length(launch_vel_) > 0.05f; }
    const CharacterConfig& config() const { return config_; }
    void set_walk_speed(f32 s) { config_.walk_speed = s; }
    void set_jump_speed(f32 s) { config_.jump_speed = s; } // race passives scale the spring

    // move_dir: desired world-space horizontal direction (xz; y ignored, length<=1).
    // `colliders` are static props (trees/walls) the capsule is pushed out of.
    // `platform` (optional): a walkable surface ABOVE the terrain at (x,z) - returns its height, or a
    // large-negative value where there's none. Used so the feet stand on a bridge deck over a river
    // (the terrain there is the carved river bed; the deck is the higher walkable ground).
    void update(const DensitySampler& density, const Vec3& move_dir, bool jump, Timestep dt,
                std::span<const Collider> colliders = {},
                const std::function<f32(f32, f32)>& platform = {});

private:
    bool wall_at(const DensitySampler& density, const Vec3& feet) const;
    std::optional<f32> ground_height(const DensitySampler& density, f32 x, f32 z, f32 top_y) const;

    CharacterConfig config_;
    Vec3 position_{0.0f};
    Vec3 velocity_{0.0f};
    Vec3 launch_vel_{0.0f}; // ballistic horizontal velocity from a toss/knockback (decays; clears on land)
    bool on_ground_ = false;
};

} // namespace alryn
