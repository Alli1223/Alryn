#include <Alryn/Character/CharacterAnimator.h>

#include <algorithm>
#include <cmath>

namespace alryn {

void CharacterAnimator::update(f32 speed, Timestep dt) {
    const f32 dts = dt.seconds;
    if (dts <= 0.0f) {
        return;
    }
    speed_ = speed;
    const f32 target = glm::clamp(speed / 6.0f, 0.0f, 1.0f); // 6 m/s = full stride
    stride_ += (target - stride_) * std::min(1.0f, dts * 8.0f);

    // Step cadence scales with speed; freeze the phase when essentially idle.
    const f32 cadence = stride_ > 0.1f ? (3.0f + speed * 1.4f) : 0.0f;
    phase_ += dts * cadence;
    if (phase_ > TwoPi) {
        phase_ -= TwoPi;
    }

    // A slow, always-on phase that gives a standing character a soft breathe/jelly wobble.
    wobble_ += dts * 1.7f;
    if (wobble_ > TwoPi) {
        wobble_ -= TwoPi;
    }

    // Action layer: advance the one-shot swing/cast to completion, and ease the held-block weight.
    if (swing_t_ >= 0.0f) {
        swing_t_ += dts;
        if (swing_t_ > kSwingDur) {
            swing_t_ = -1.0f;
        }
    }
    if (cast_t_ >= 0.0f) {
        cast_t_ += dts;
        if (cast_t_ > kCastDur) {
            cast_t_ = -1.0f;
        }
    }
    const f32 target_block = blocking_ ? 1.0f : 0.0f;
    block_w_ += (target_block - block_w_) * std::min(1.0f, dts * 12.0f);
}

void CharacterAnimator::play_swing() { swing_t_ = 0.0f; cast_t_ = -1.0f; }
void CharacterAnimator::play_cast() { cast_t_ = 0.0f; swing_t_ = -1.0f; }
void CharacterAnimator::set_blocking(bool blocking) { blocking_ = blocking; }

std::vector<Quat> CharacterAnimator::pose(const CharacterModel& model) const {
    std::vector<Quat> pose(model.bone_count(), QuatIdentity);

    const f32 s = stride_;
    const f32 ph = phase_;
    const f32 sn = std::sin(ph);
    const f32 cs = std::cos(ph);
    // Amplitudes toned down from the chibi's floppy "Gang Beasts" wobble to suit the proportioned
    // adult rig - a lively but grounded walk that reads under the armour/robe.
    const f32 leg_amp = 0.55f;
    const f32 arm_amp = 0.5f;
    const f32 knee_amp = 0.7f;
    const f32 elbow_amp = 0.3f;
    const f32 arm_splay = 0.1f;   // a slight outward bow at the shoulders
    const f32 arm_circle = 0.1f;  // sideways arm sweep, 90deg out of phase => loose circular hands

    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Vec3 az{0.0f, 0.0f, 1.0f};
    const auto rx = [&](f32 a) { return glm::angleAxis(a, ax); };
    const auto ry = [&](f32 a) { return glm::angleAxis(a, ay); };
    const auto rz = [&](f32 a) { return glm::angleAxis(a, az); };

    const std::vector<Bone>& bones = model.bones();
    for (usize i = 0; i < bones.size(); ++i) {
        switch (bones[i].part) {
            // Legs stay a clean fore-aft swing (out of phase L/R) so footing reads clearly.
            case BonePart::UpperLegL: pose[i] = rx(sn * leg_amp * s); break;
            case BonePart::UpperLegR: pose[i] = rx(-sn * leg_amp * s); break;
            case BonePart::LowerLegL: pose[i] = rx(std::max(0.0f, -sn) * knee_amp * s); break;
            case BonePart::LowerLegR: pose[i] = rx(std::max(0.0f, sn) * knee_amp * s); break;
            // Arms swing fore-aft (X) AND in-out (Z, 90deg out of phase) so the hands trace
            // loose circles, with a constant outward splay even when idle - floppy and fun.
            case BonePart::UpperArmL:
                pose[i] = rz(-arm_splay - cs * arm_circle * s) * rx(-sn * arm_amp * s);
                break;
            case BonePart::UpperArmR:
                pose[i] = rz(arm_splay + cs * arm_circle * s) * rx(sn * arm_amp * s);
                break;
            case BonePart::LowerArmL:
                pose[i] = rx(elbow_amp * s + std::max(0.0f, sn) * 0.2f * s);
                break;
            case BonePart::LowerArmR:
                pose[i] = rx(elbow_amp * s + std::max(0.0f, -sn) * 0.2f * s);
                break;
            // Torso counter-twists against the hips and rolls side to side - the waddle.
            case BonePart::Torso:
                pose[i] = ry(-sn * 0.12f * s) * rz(sn * 0.06f * s) * rx(0.05f * s);
                break;
            // Head bobs (twice per stride) and tilts, lagging the body for a loose-neck look.
            case BonePart::Head:
                pose[i] = rx(-std::sin(ph * 2.0f) * 0.06f * s) * rz(-sn * 0.07f * s);
                break;
            // Hips twist + roll opposite the torso.
            case BonePart::Pelvis: pose[i] = ry(sn * 0.1f * s) * rz(-sn * 0.05f * s); break;
            default: break;
        }
    }

    // Blend the upper-body action over the locomotion base (a one-shot swing/cast takes priority over
    // a held block; the legs keep their walk/idle cycle underneath either way).
    if (swing_t_ >= 0.0f) {
        overlay_swing(model, pose);
    } else if (cast_t_ >= 0.0f) {
        overlay_cast(model, pose);
    } else if (block_w_ > 1e-3f) {
        overlay_block(model, pose);
    }
    return pose;
}

namespace {
// Slerp the masked bones of `pose` toward `target` by `weight` (0 leaves the base untouched,
// 1 fully adopts the action). `mask(part)` returns the per-bone influence (0..1).
template <typename TargetFn, typename MaskFn>
void blend_overlay(const CharacterModel& model, std::vector<Quat>& pose, f32 weight,
                   TargetFn target, MaskFn mask) {
    const std::vector<Bone>& bones = model.bones();
    for (usize i = 0; i < bones.size(); ++i) {
        const f32 w = weight * mask(bones[i].part);
        if (w <= 1e-3f) {
            continue;
        }
        pose[i] = glm::normalize(glm::slerp(pose[i], target(bones[i].part), glm::clamp(w, 0.0f, 1.0f)));
    }
}
} // namespace

// An overhead sword chop. The SWORD arm is on the player's right side, which (the rig's L/R
// labels are mirrored) is the *L*-suffixed arm; the SHIELD arm is the *R*-suffixed one. The
// shoulder swings about the body's left-right axis (the bone's local X) on a single smooth,
// monotonic angle: wind up overhead-and-back (+), sweep down to the FRONT (-), settle to rest.
// Only the upper body is masked, so the legs keep their walk/idle cycle underneath.
void CharacterAnimator::overlay_swing(const CharacterModel& model, std::vector<Quat>& pose) const {
    const f32 t = glm::clamp(swing_t_ / kSwingDur, 0.0f, 1.0f);
    const f32 env = glm::smoothstep(0.0f, 0.10f, t) * (1.0f - glm::smoothstep(0.86f, 1.0f, t));

    f32 a; // shoulder pitch (about local X: +raises the arm overhead-and-back, - drops it to the front)
    if (t < 0.26f) {
        a = glm::mix(0.0f, 2.55f, glm::smoothstep(0.0f, 0.26f, t)); // quick raise overhead (windup)
    } else if (t < 0.55f) {
        a = glm::mix(2.55f, -1.6f, glm::smoothstep(0.26f, 0.55f, t)); // CHOP down hard to the front
    } else {
        a = glm::mix(-1.6f, 0.0f, glm::smoothstep(0.55f, 1.0f, t)); // recover to rest
    }
    const f32 cock = glm::smoothstep(0.0f, 0.24f, t) * (1.0f - glm::smoothstep(0.26f, 0.5f, t));
    const f32 lean = glm::smoothstep(0.32f, 0.58f, t) * (1.0f - glm::smoothstep(0.60f, 0.95f, t));

    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Quat sword_upper = glm::angleAxis(a, ax);
    const Quat sword_lower = glm::angleAxis(0.8f * cock, ax); // cock the elbow, extend on impact
    const Quat torso = glm::angleAxis(0.22f * lean, ax) * glm::angleAxis(-0.25f * lean, ay);
    const Quat head = glm::angleAxis(0.1f * lean, ax);
    const Quat off_upper = glm::angleAxis(-0.25f * lean, ax); // shield arm counter-swings

    blend_overlay(
        model, pose, env,
        [&](BonePart p) -> Quat {
            switch (p) {
                case BonePart::UpperArmL: return sword_upper; // sword arm (player's right)
                case BonePart::LowerArmL: return sword_lower;
                case BonePart::Torso: return torso;
                case BonePart::Head: return head;
                case BonePart::UpperArmR: return off_upper;
                default: return QuatIdentity;
            }
        },
        [](BonePart p) -> f32 {
            switch (p) {
                case BonePart::UpperArmL:
                case BonePart::LowerArmL: return 1.0f;
                case BonePart::Torso: return 0.5f;
                case BonePart::Head: return 0.35f;
                case BonePart::UpperArmR: return 0.4f;
                default: return 0.0f; // legs + the rest keep walking
            }
        });
}

// A spell cast: the weapon arm (player's right = the L-suffixed bones) sweeps the staff/hand FORWARD
// and up, with a thrust pulse at the climax; the off hand raises in a gesture and the torso leans in.
// Only the upper body is masked, so the legs keep walking - you can cast on the move.
void CharacterAnimator::overlay_cast(const CharacterModel& model, std::vector<Quat>& pose) const {
    const f32 t = glm::clamp(cast_t_ / kCastDur, 0.0f, 1.0f);
    const f32 env = glm::smoothstep(0.0f, 0.12f, t) * (1.0f - glm::smoothstep(0.82f, 1.0f, t));

    f32 raise; // shoulder pitch (negative raises the arm forward + up)
    if (t < 0.35f) {
        raise = glm::mix(0.0f, -1.55f, glm::smoothstep(0.0f, 0.35f, t)); // bring the staff forward + up
    } else {
        raise = glm::mix(-1.55f, -1.2f, glm::smoothstep(0.35f, 1.0f, t)); // hold, then settle
    }
    const f32 thrust = glm::smoothstep(0.30f, 0.5f, t) * (1.0f - glm::smoothstep(0.5f, 0.72f, t)); // push
    const f32 gesture = glm::smoothstep(0.0f, 0.3f, t) * (1.0f - glm::smoothstep(0.7f, 1.0f, t));

    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Quat weapon_upper = glm::angleAxis(raise - 0.2f * thrust, ax);
    const Quat weapon_lower = glm::angleAxis(0.5f - 0.45f * thrust, ax); // elbow bent, extends on thrust
    const Quat off_upper = glm::angleAxis(-0.9f * gesture, ax) * glm::angleAxis(0.2f * gesture, ay);
    const Quat off_lower = glm::angleAxis(0.6f * gesture, ax);
    const Quat torso = glm::angleAxis(0.15f * gesture, ax);
    const Quat head = glm::angleAxis(-0.1f * gesture, ax); // look forward at the target

    blend_overlay(
        model, pose, env,
        [&](BonePart p) -> Quat {
            switch (p) {
                case BonePart::UpperArmL: return weapon_upper; // weapon arm (player's right)
                case BonePart::LowerArmL: return weapon_lower;
                case BonePart::UpperArmR: return off_upper;
                case BonePart::LowerArmR: return off_lower;
                case BonePart::Torso: return torso;
                case BonePart::Head: return head;
                default: return QuatIdentity;
            }
        },
        [](BonePart p) -> f32 {
            switch (p) {
                case BonePart::UpperArmL:
                case BonePart::LowerArmL: return 1.0f;
                case BonePart::UpperArmR:
                case BonePart::LowerArmR: return 0.7f;
                case BonePart::Torso: return 0.4f;
                case BonePart::Head: return 0.3f;
                default: return 0.0f; // legs keep walking
            }
        });
}

// Shield-up guard: the shield arm (player's left = the R-suffixed bones) lifts up and across the
// chest, the sword arm tucks back, the torso turns to present the shield. Held; weight eases.
void CharacterAnimator::overlay_block(const CharacterModel& model, std::vector<Quat>& pose) const {
    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Vec3 az{0.0f, 0.0f, 1.0f};
    const Quat upper_r = glm::angleAxis(-1.7f, ax) * glm::angleAxis(-0.5f, az); // raise up + across
    const Quat lower_r = glm::angleAxis(1.0f, ax) * glm::angleAxis(0.3f, az);   // forearm horizontal
    const Quat upper_l = glm::angleAxis(0.45f, ax) * glm::angleAxis(-0.3f, az); // sword tucked back
    const Quat lower_l = glm::angleAxis(0.7f, ax);
    const Quat torso = glm::angleAxis(-0.28f, ay) * glm::angleAxis(0.1f, ax); // turn shoulder in
    const Quat head = glm::angleAxis(0.12f, ax);

    blend_overlay(
        model, pose, block_w_,
        [&](BonePart p) -> Quat {
            switch (p) {
                case BonePart::UpperArmR: return upper_r; // shield arm (player's left)
                case BonePart::LowerArmR: return lower_r;
                case BonePart::UpperArmL: return upper_l;
                case BonePart::LowerArmL: return lower_l;
                case BonePart::Torso: return torso;
                case BonePart::Head: return head;
                default: return QuatIdentity;
            }
        },
        [](BonePart p) -> f32 {
            switch (p) {
                case BonePart::UpperArmR:
                case BonePart::LowerArmR: return 1.0f;
                case BonePart::UpperArmL:
                case BonePart::LowerArmL: return 0.6f;
                case BonePart::Torso: return 0.5f;
                case BonePart::Head: return 0.3f;
                default: return 0.0f;
            }
        });
}

Mat4 CharacterAnimator::body_offset() const {
    const f32 s = stride_;
    const f32 ph = phase_;
    const f32 idle = 1.0f - s;
    const f32 breathe = std::sin(wobble_);

    // Vertical bob: two dips per stride (one per footfall), plus a soft idle breathe.
    const f32 bob = -std::abs(std::sin(ph)) * 0.07f * s + breathe * 0.012f * idle;
    // Side-to-side waddle in step.
    const f32 sway = std::sin(ph) * 0.05f * s;
    // A little roll waddle to go with the sway.
    const f32 roll = std::sin(ph) * 0.05f * s;
    // Lean eagerly into movement (forward = +Z in the model's local frame).
    const f32 lean = glm::clamp(speed_ / 6.0f, 0.0f, 1.0f) * 0.16f;
    // Squash & stretch: squat wide at footfall, stretch tall mid-stride - the jelly bounce.
    const f32 q = std::abs(std::sin(ph));
    const f32 squash = (0.5f - q) * 0.14f * s + breathe * 0.02f * idle;
    const f32 sy = 1.0f + squash;
    const f32 sxz = 1.0f - squash * 0.6f;

    Mat4 m = glm::translate(Mat4{1.0f}, Vec3{sway, bob, 0.0f});
    m *= glm::rotate(Mat4{1.0f}, roll, Vec3{0.0f, 0.0f, 1.0f});
    m *= glm::rotate(Mat4{1.0f}, lean, Vec3{1.0f, 0.0f, 0.0f});
    m *= glm::scale(Mat4{1.0f}, Vec3{sxz, sy, sxz});
    return m;
}

std::vector<Quat> CharacterAnimator::sit_pose(const CharacterModel& model) {
    std::vector<Quat> pose(model.bone_count(), QuatIdentity);
    const auto rot = [](f32 a) { return glm::angleAxis(a, Vec3{1.0f, 0.0f, 0.0f}); };
    for (usize i = 0; i < model.bones().size(); ++i) {
        switch (model.bones()[i].part) {
            case BonePart::UpperLegL:
            case BonePart::UpperLegR: pose[i] = rot(-1.45f); break; // thighs forward (horizontal)
            case BonePart::LowerLegL:
            case BonePart::LowerLegR: pose[i] = rot(1.4f); break;   // shins drop down
            case BonePart::UpperArmL:
            case BonePart::UpperArmR: pose[i] = rot(-0.35f); break; // hands toward the lap
            case BonePart::LowerArmL:
            case BonePart::LowerArmR: pose[i] = rot(0.5f); break;
            case BonePart::Torso: pose[i] = rot(0.08f); break;      // a slight lean
            default: break;
        }
    }
    return pose;
}

} // namespace alryn
