#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>

#include <vector>

namespace alryn {

// Procedural walk/idle animation. Advances a cycle phase from movement speed and
// produces per-bone joint rotations: legs and arms swing out of phase, knees and
// elbows bend, and a "stride" amplitude eases the whole thing in/out so a standing
// character is neutral. No skeletal data files - it's all derived from the rig.
class CharacterAnimator {
public:
    void update(f32 speed, Timestep dt);

    // Per-bone pose (rotations) for `model`, indexed to match model.bones().
    std::vector<Quat> pose(const CharacterModel& model) const;

    // A static seated pose (thighs forward, shins down, hands on the lap) for a character
    // sitting on a wagon. Not phase-driven, so it's a plain static helper.
    static std::vector<Quat> sit_pose(const CharacterModel& model);

    f32 phase() const { return phase_; }
    f32 stride() const { return stride_; }

private:
    f32 phase_ = 0.0f;  // walk-cycle phase (radians)
    f32 stride_ = 0.0f; // 0 = idle .. 1 = full walk (eased amplitude)
};

} // namespace alryn
