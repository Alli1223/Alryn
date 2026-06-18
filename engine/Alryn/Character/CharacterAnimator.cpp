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
}

std::vector<Quat> CharacterAnimator::pose(const CharacterModel& model) const {
    std::vector<Quat> pose(model.bone_count(), QuatIdentity);

    const f32 s = stride_;
    const f32 ph = phase_;
    const f32 sn = std::sin(ph);
    const f32 cs = std::cos(ph);
    const f32 leg_amp = 0.7f;
    const f32 arm_amp = 0.6f;
    const f32 knee_amp = 0.8f;
    const f32 elbow_amp = 0.4f;
    const f32 arm_splay = 0.2f;   // constant outward bow - relaxed, blobby arms
    const f32 arm_circle = 0.16f; // sideways arm sweep, 90deg out of phase => circular hands

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
    return pose;
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
