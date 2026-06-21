#pragma once

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Physics/Collider.h>

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
    const CharacterConfig& config() const { return config_; }
    void set_walk_speed(f32 s) { config_.walk_speed = s; }

    // move_dir: desired world-space horizontal direction (xz; y ignored, length<=1).
    // `colliders` are static props (trees/walls) the capsule is pushed out of.
    void update(const DensitySampler& density, const Vec3& move_dir, bool jump, Timestep dt,
                std::span<const Collider> colliders = {});

private:
    bool wall_at(const DensitySampler& density, const Vec3& feet) const;
    std::optional<f32> ground_height(const DensitySampler& density, f32 x, f32 z, f32 top_y) const;

    CharacterConfig config_;
    Vec3 position_{0.0f};
    Vec3 velocity_{0.0f};
    bool on_ground_ = false;
};

} // namespace alryn
