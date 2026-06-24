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

    // --- Action layer ---------------------------------------------------------------
    // The locomotion (walk/idle) above is the BASE layer. On top of it the animator can
    // play an upper-body ACTION that blends in over the base with a per-bone mask, so a
    // character can e.g. swing or block while their legs keep walking. Triggered by the
    // game (left-click -> swing, hold shield -> block); the legs/locomotion are untouched.

    // Trigger a one-shot attack swing (overhead diagonal slash of the right arm). Restarts
    // the swing if one is already mid-play.
    void play_swing();
    // Trigger a one-shot spell cast (the weapon arm thrusts the staff/hand forward + up). For the
    // Mage/Cleric; blends over locomotion like the swing.
    void play_cast();
    // Hold/release a shield-up guard (left arm raised across the body); eases in/out.
    void set_blocking(bool blocking);

    bool swinging() const { return swing_t_ >= 0.0f; }
    bool casting() const { return cast_t_ >= 0.0f; }
    bool blocking() const { return blocking_; }

    // Per-bone pose (rotations) for `model`, indexed to match model.bones(). Includes any
    // active action blended over the locomotion base.
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
    // Builds the upper-body action overlay (pose + per-bone blend weight) into `pose`,
    // slerping each masked bone from the locomotion base toward the action target.
    void overlay_swing(const CharacterModel& model, std::vector<Quat>& pose) const;
    void overlay_cast(const CharacterModel& model, std::vector<Quat>& pose) const;
    void overlay_block(const CharacterModel& model, std::vector<Quat>& pose) const;

    static constexpr f32 kSwingDur = 0.46f; // length of one attack swing (seconds)
    static constexpr f32 kCastDur = 0.55f;  // length of one spell cast (seconds)

    f32 phase_ = 0.0f;     // walk-cycle phase (radians)
    f32 stride_ = 0.0f;    // 0 = idle .. 1 = full walk (eased amplitude)
    f32 wobble_ = 0.0f;    // free-running phase for the always-on idle breathe/jelly
    f32 speed_ = 0.0f;     // latest movement speed (drives the forward lean)
    f32 swing_t_ = -1.0f;  // swing playback time; < 0 = not swinging
    f32 cast_t_ = -1.0f;   // cast playback time; < 0 = not casting
    bool blocking_ = false; // shield-up held?
    f32 block_w_ = 0.0f;   // eased 0..1 block blend weight
};

} // namespace alryn
