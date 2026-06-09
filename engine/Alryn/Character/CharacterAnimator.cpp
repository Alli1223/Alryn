#include <Alryn/Character/CharacterAnimator.h>

#include <algorithm>
#include <cmath>

namespace alryn {

void CharacterAnimator::update(f32 speed, Timestep dt) {
    const f32 dts = dt.seconds;
    if (dts <= 0.0f) {
        return;
    }
    const f32 target = glm::clamp(speed / 6.0f, 0.0f, 1.0f); // 6 m/s = full stride
    stride_ += (target - stride_) * std::min(1.0f, dts * 8.0f);

    // Step cadence scales with speed; freeze the phase when essentially idle.
    const f32 cadence = stride_ > 0.1f ? (3.0f + speed * 1.4f) : 0.0f;
    phase_ += dts * cadence;
    if (phase_ > TwoPi) {
        phase_ -= TwoPi;
    }
}

std::vector<Quat> CharacterAnimator::pose(const CharacterModel& model) const {
    std::vector<Quat> pose(model.bone_count(), QuatIdentity);

    const f32 s = stride_;
    const f32 ph = phase_;
    const f32 leg_amp = 0.7f;
    const f32 arm_amp = 0.55f;
    const f32 knee_amp = 0.8f;
    const f32 elbow_amp = 0.35f;
    const auto swing = [](f32 angle) { return glm::angleAxis(angle, Vec3{1.0f, 0.0f, 0.0f}); };

    const std::vector<Bone>& bones = model.bones();
    for (usize i = 0; i < bones.size(); ++i) {
        switch (bones[i].part) {
            case BonePart::UpperLegL: pose[i] = swing(std::sin(ph) * leg_amp * s); break;
            case BonePart::UpperLegR: pose[i] = swing(-std::sin(ph) * leg_amp * s); break;
            case BonePart::LowerLegL: pose[i] = swing(std::max(0.0f, -std::sin(ph)) * knee_amp * s); break;
            case BonePart::LowerLegR: pose[i] = swing(std::max(0.0f, std::sin(ph)) * knee_amp * s); break;
            case BonePart::UpperArmL: pose[i] = swing(-std::sin(ph) * arm_amp * s); break;
            case BonePart::UpperArmR: pose[i] = swing(std::sin(ph) * arm_amp * s); break;
            case BonePart::LowerArmL: pose[i] = swing(elbow_amp * s); break;
            case BonePart::LowerArmR: pose[i] = swing(elbow_amp * s); break;
            default: break;
        }
    }
    return pose;
}

} // namespace alryn
