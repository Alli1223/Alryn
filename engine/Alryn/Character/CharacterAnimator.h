#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>

#include <vector>

namespace alryn {

// Procedural, deliberately *wobbly* walk/idle animation - a soft "Gang Beasts /
// Wobbly Life" feel. Limbs swing in circular/elliptical arcs (not flat pendulums),
// the whole body squashes-and-stretches and waddles in step, leans eagerly into
// movement, and even a standing character gently breathes. A "stride" amplitude eases
// the limb motion in/out; the jelly body wobble lives in body_offset(). No skeletal
// data files - it's all derived from the rig.
class CharacterAnimator {
public:
    void update(f32 speed, Timestep dt);

    // Per-bone pose (rotations) for `model`, indexed to match model.bones().
    std::vector<Quat> pose(const CharacterModel& model) const;

    // A squash/stretch + bob + sway + lean transform for the whole body, to be composed
    // with the character's root matrix (root * body_offset()). This is the bouncy,
    // blobby secondary motion; identity-ish when perfectly still apart from a soft breathe.
    Mat4 body_offset() const;

    // A static seated pose (thighs forward, shins down, hands on the lap) for a character
    // sitting on a wagon. Not phase-driven, so it's a plain static helper.
    static std::vector<Quat> sit_pose(const CharacterModel& model);

    f32 phase() const { return phase_; }
    f32 stride() const { return stride_; }

private:
    f32 phase_ = 0.0f;  // walk-cycle phase (radians)
    f32 stride_ = 0.0f; // 0 = idle .. 1 = full walk (eased amplitude)
    f32 wobble_ = 0.0f; // free-running phase for the always-on idle breathe/jelly
    f32 speed_ = 0.0f;  // latest movement speed (drives the forward lean)
};

} // namespace alryn
