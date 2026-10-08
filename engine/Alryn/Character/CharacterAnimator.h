#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>

#include <cmath>
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
    // `heading` is how much of the movement is along the way the character faces: 1 walking
    // forward, -1 straight backwards (a character facing its aim while retreating). Backpedalling
    // runs the stride in reverse - the feet step back - and trims the forward lean.
    void update(f32 speed, Timestep dt, f32 heading = 1.0f);

    // --- Action layer ---------------------------------------------------------------
    // The locomotion (walk/idle) above is the BASE layer. On top of it the animator can
    // play an upper-body ACTION that blends in over the base with a per-bone mask, so a
    // character can e.g. swing or block while their legs keep walking. Triggered by the
    // game (left-click -> swing, hold shield -> block); the legs/locomotion are untouched.

    // Trigger a one-shot sword attack: a heavy diagonal FOREHAND cleave (anticipation up over the
    // right shoulder, an accelerating cut down across the body, follow-through, recovery). Clicking
    // again once the blow has landed chains a rising BACKHAND cut from wherever the blade is (and the
    // next one a forehand again) - a flowing combo instead of snapping back to rest. A click before the
    // current blow lands is ignored (the swing is committed). `lead_in` starts a fresh swing that many
    // seconds in (part-way up the wind-up) - for a strike that has already been decided elsewhere (an
    // enemy's networked blow) and should land quickly.
    void play_swing(f32 lead_in = 0.0f);
    // Trigger a one-shot spell cast (the weapon arm thrusts the staff/hand forward + up). For the
    // Mage/Cleric; blends over locomotion like the swing.
    void play_cast();
    // Hold/release a shield-up guard (left arm raised across the body); eases in/out.
    void set_blocking(bool blocking);

    // HEAVY attack wind-up while the attack is held: `amount` 0 (not charging) .. 1 (fully wound up),
    // in a pose per `style` - 0 a sword heaved up overhead in both hands (Knight), 1 a bow drawn back
    // to the cheek (Hunter), 2 a staff raised high, gathering power (Cleric / Mage). Eases in/out, and
    // the body trembles with the strain at a full charge.
    void set_charge(f32 amount, u8 style);
    f32 charge() const { return charge_; }
    u8 charge_style() const { return charge_style_; }
    // The release of a heavy: the Knight's overhead EARTHSPLITTER chop down into the ground ahead
    // (style 0, from wherever the wind-up held the sword), else the loosed shot / hurled orb.
    void play_heavy(u8 style);
    // True while the overhead heavy chop is playing.
    bool heavy_swinging() const { return swing_t_ >= 0.0f && swing_kind_ == 2; }

    bool swinging() const { return swing_t_ >= 0.0f; }
    // Seconds until the current swing's blow lands (<= 0 once it has, or when not swinging).
    f32 swing_until_impact() const;
    // True during the fast, cutting part of the swing (the blade-tip trail is drawn then).
    bool swing_cutting() const;
    // The extra rotation of the main-hand weapon within the hand frame for the current swing: it turns
    // the blade over (the wrist) so it cuts through its arc instead of pointing along the forearm.
    // Identity when not swinging. Apply as hand_frame * mat4_cast(weapon_wrist(model)).
    Quat weapon_wrist(const CharacterModel& model) const;

    // The swing is authored as model-independent PARAMETERS keyed over time: the sword hand's position
    // relative to the shoulder (in arm-length units, torso space: +Y up, +Z forward, -X the player's
    // right), the blade's direction, and the body's coil/lean/crouch. The arm itself is then solved
    // with two-bone IK for the actual rig (solve_swing_arm).
    struct SwingParams {
        Vec3 hand{-0.05f, -0.97f, 0.15f}; // hand offset from the shoulder / arm length
        Vec3 blade{0.0f, -1.0f, 0.1f};    // blade direction (torso space)
        f32 coil = 0.0f;                  // torso yaw (- = coiled to the right)
        f32 lean = 0.0f;                  // torso pitch (+ = leaning into the blow)
        f32 guard = 0.0f;                 // off (shield) arm: 1 = brought forward for balance
        f32 crouch = 0.0f;                // weight transfer: knee bend + dip/lunge (when standing)
    };
    SwingParams swing_params() const; // the parameters at the current swing time (+ envelope below)
    f32 swing_env() const;
    f32 swing_stance() const;         // 0..1 how much the legs lunge (1 standing, 0 walking)            // 0..1 blend of the swing over the locomotion pose
    struct ArmSolve {
        Quat upper{1.0f, 0.0f, 0.0f, 0.0f};
        Quat lower{1.0f, 0.0f, 0.0f, 0.0f};
        Quat wrist{1.0f, 0.0f, 0.0f, 0.0f};
    };
    static ArmSolve solve_swing_arm(const CharacterModel& model, const SwingParams& p);
    // The blade's axis in the hand frame: held weapons continue down the forearm (-Y) tilted forward
    // 0.35 rad (Character/Weapon.cpp builds them that way), so the wrist turns THIS onto the keyed blade.
    static Vec3 blade_axis() { return glm::normalize(Vec3{0.0f, -std::cos(0.35f), std::sin(0.35f)}); }

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
    void overlay_charge(const CharacterModel& model, std::vector<Quat>& pose) const;
    // The Knight's overhead wind-up, as swing parameters (so the IK arm + the release chop share them).
    SwingParams charge_params() const;

    static constexpr f32 kSwingDur = 0.6f;     // length of one attack swing (seconds)
    static constexpr f32 kSwingImpact = 0.27f; // when the blow lands (a later click chains a new cut)
    static constexpr f32 kCastDur = 0.55f;  // length of one spell cast (seconds)
    static constexpr f32 kHeavyDur = 0.8f;     // the overhead heavy chop, start to recovered
    static constexpr f32 kHeavyImpact = 0.24f; // when it bites into the ground (the server lands it at 0.26 s)

    f32 phase_ = 0.0f;     // walk-cycle phase (radians)
    f32 stride_ = 0.0f;    // 0 = idle .. 1 = full walk (eased amplitude)
    f32 wobble_ = 0.0f;    // free-running phase for the always-on idle breathe/jelly
    f32 speed_ = 0.0f;     // latest movement speed (drives the forward lean)
    f32 dir_ = 1.0f;       // eased stride direction: 1 stepping forward .. -1 stepping back
    f32 swing_t_ = -1.0f;  // swing playback time; < 0 = not swinging
    u8 swing_kind_ = 0;    // 0 = forehand cleave, 1 = backhand (the combo's return cut)
    SwingParams swing_from_{}; // where this swing starts (rest, or wherever the last cut left the blade)
    bool swing_from_rest_ = true; // started from rest -> ease the whole swing in from the walk pose
    f32 swing_start_ = 0.0f;      // swing time it started at (a lead-in skips part of the wind-up)
    f32 cast_t_ = -1.0f;   // cast playback time; < 0 = not casting
    bool blocking_ = false; // shield-up held?
    f32 block_w_ = 0.0f;   // eased 0..1 block blend weight
    f32 charge_target_ = 0.0f; // the heavy wind-up being held (0..1)
    f32 charge_ = 0.0f;        // eased wind-up amount
    f32 charge_w_ = 0.0f;      // eased 0..1 blend of the wind-up pose over the locomotion
    u8 charge_style_ = 0;      // 0 overhead sword, 1 drawn bow, 2 raised staff
};

} // namespace alryn
