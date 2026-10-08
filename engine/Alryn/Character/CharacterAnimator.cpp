#include <Alryn/Character/CharacterAnimator.h>

#include <algorithm>
#include <cmath>

namespace alryn {

void CharacterAnimator::update(f32 speed, Timestep dt, f32 heading) {
    const f32 dts = dt.seconds;
    if (dts <= 0.0f) {
        return;
    }
    speed_ = speed;
    const f32 target = glm::clamp(speed / 6.0f, 0.0f, 1.0f); // 6 m/s = full stride
    stride_ += (target - stride_) * std::min(1.0f, dts * 8.0f);
    // Step backwards when moving mostly against the facing (strafing keeps stepping forward).
    const f32 target_dir = heading < -0.3f ? -1.0f : 1.0f;
    dir_ += (target_dir - dir_) * std::min(1.0f, dts * 10.0f);

    // Step cadence scales with speed; freeze the phase when essentially idle.
    const f32 cadence = stride_ > 0.1f ? (3.0f + speed * 1.4f) : 0.0f;
    phase_ += dts * cadence * dir_;
    if (phase_ > TwoPi) {
        phase_ -= TwoPi;
    } else if (phase_ < 0.0f) {
        phase_ += TwoPi;
    }

    // A slow, always-on phase that gives a standing character a soft breathe/jelly wobble.
    wobble_ += dts * 1.7f;
    if (wobble_ > TwoPi) {
        wobble_ -= TwoPi;
    }

    // Action layer: advance the one-shot swing/cast to completion, and ease the held-block weight.
    if (swing_t_ >= 0.0f) {
        swing_t_ += dts;
        if (swing_t_ > (swing_kind_ == 0 ? kSwingDur : swing_kind_ == 2 ? kHeavyDur : 0.55f)) {
            swing_t_ = -1.0f;
        }
    }
    // The heavy wind-up: the amount follows the held charge (snapping back fast on a release), and the
    // pose blends in as soon as it starts.
    charge_ += (charge_target_ - charge_) * std::min(1.0f, dts * (charge_target_ > charge_ ? 14.0f : 22.0f));
    charge_w_ += ((charge_target_ > 0.0f ? 1.0f : 0.0f) - charge_w_) * std::min(1.0f, dts * (charge_target_ > 0.0f ? 10.0f : 16.0f));
    if (cast_t_ >= 0.0f) {
        cast_t_ += dts;
        if (cast_t_ > kCastDur) {
            cast_t_ = -1.0f;
        }
    }
    const f32 target_block = blocking_ ? 1.0f : 0.0f;
    block_w_ += (target_block - block_w_) * std::min(1.0f, dts * 12.0f);
}

void CharacterAnimator::play_cast() { cast_t_ = 0.0f; swing_t_ = -1.0f; }
void CharacterAnimator::set_blocking(bool blocking) { blocking_ = blocking; }
void CharacterAnimator::set_charge(f32 amount, u8 style) {
    charge_target_ = glm::clamp(amount, 0.0f, 1.0f);
    if (amount > 0.0f) {
        charge_style_ = style;
    }
}

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
    // a heavy wind-up, and that over a held block; the legs keep their walk/idle cycle underneath).
    if (swing_t_ >= 0.0f) {
        overlay_swing(model, pose);
    } else if (cast_t_ >= 0.0f) {
        overlay_cast(model, pose);
    } else if (charge_w_ > 1e-3f) {
        overlay_charge(model, pose);
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

// ---- The sword swing ------------------------------------------------------------------------
namespace {
// One key of a swing: the time it's reached (s) + the parameters there.
struct SwingKey {
    f32 t;
    Vec3 hand;
    Vec3 blade;
    f32 coil, lean, guard, crouch;
};
// Easing per segment: wind-ups decelerate into their peak (the anticipation hang), cuts ACCELERATE
// through the target (that's what reads as weight), follow-throughs coast out, recoveries settle.
enum class Ease : u8 { Out, In, Smooth };
f32 ease(Ease e, f32 u) {
    switch (e) {
        case Ease::Out: return 1.0f - (1.0f - u) * (1.0f - u);
        case Ease::In: return u * u * u;
        case Ease::Smooth: return u * u * (3.0f - 2.0f * u);
    }
    return u;
}
// The rest the swings start from / settle back to (the sword lowered at the side).
const SwingKey kRest{0.0f, Vec3{-0.05f, -0.97f, 0.15f}, Vec3{0.0f, -1.0f, 0.12f}, 0.0f, 0.0f, 0.0f, 0.0f};

// FOREHAND CLEAVE: up over the right shoulder, hang, then hard down and across to the left hip.
constexpr int kForeN = 6;
const SwingKey kFore[kForeN] = {
    {0.12f, Vec3{-0.34f, 0.62f, -0.18f}, Vec3{0.30f, 0.42f, -0.86f}, -0.55f, -0.12f, 1.0f, 0.15f}, // wind-up top
    {0.16f, Vec3{-0.38f, 0.66f, -0.24f}, Vec3{0.24f, 0.30f, -0.92f}, -0.62f, -0.14f, 1.0f, 0.2f},  // hang (cocked)
    {0.215f, Vec3{-0.02f, 0.18f, 0.86f}, Vec3{0.18f, 0.20f, 0.96f}, 0.10f, 0.20f, 0.6f, 0.8f},   // mid-cut (out front)
    {0.27f, Vec3{0.48f, -0.46f, 0.62f}, Vec3{0.72f, -0.50f, 0.48f}, 0.52f, 0.30f, 0.3f, 1.0f},   // IMPACT, low-left
    {0.36f, Vec3{0.60f, -0.70f, 0.24f}, Vec3{0.58f, -0.66f, -0.30f}, 0.62f, 0.26f, 0.2f, 0.8f},  // follow-through
    {0.60f, kRest.hand, kRest.blade, 0.0f, 0.0f, 0.0f, 0.0f},                                    // recovered
};
const Ease kForeEase[kForeN] = {Ease::Out, Ease::Smooth, Ease::In, Ease::In, Ease::Out, Ease::Smooth};

// BACKHAND (the combo's return cut): from low on the left, a quick cock, then rising hard across to
// high on the right - the blade ends cocked over the right shoulder, ready for another forehand.
constexpr int kBackN = 5;
const SwingKey kBack[kBackN] = {
    {0.07f, Vec3{0.62f, -0.62f, 0.05f}, Vec3{0.50f, -0.50f, -0.70f}, 0.70f, 0.20f, 0.4f, 0.7f},  // cock low-left
    {0.135f, Vec3{0.10f, -0.06f, 0.88f}, Vec3{0.10f, -0.10f, 0.99f}, 0.05f, 0.22f, 0.6f, 0.9f},  // mid-cut
    {0.20f, Vec3{-0.50f, 0.34f, 0.56f}, Vec3{-0.74f, 0.40f, 0.54f}, -0.48f, 0.12f, 0.7f, 1.0f},  // IMPACT, high-right
    {0.29f, Vec3{-0.46f, 0.52f, 0.10f}, Vec3{-0.40f, 0.60f, -0.70f}, -0.58f, 0.0f, 0.8f, 0.7f},  // follow-through
    {0.55f, kRest.hand, kRest.blade, 0.0f, 0.0f, 0.0f, 0.0f},                                    // recovered
};
const Ease kBackEase[kBackN] = {Ease::Out, Ease::In, Ease::In, Ease::Out, Ease::Smooth};

// HEAVY OVERHEAD (the Knight's charged EARTHSPLITTER): from the wind-up held high overhead, a last
// heave, then the blade comes over the top and is driven down into the ground ahead, the whole body
// dropping behind it - held a beat in the crater, then a slow recovery.
constexpr int kHeavyN = 5;
const SwingKey kHeavy[kHeavyN] = {
    {0.06f, Vec3{-0.12f, 1.0f, -0.24f}, Vec3{0.06f, 0.42f, -0.9f}, -0.22f, -0.24f, 1.0f, 0.4f},  // last heave
    {0.15f, Vec3{-0.03f, 0.5f, 0.78f}, Vec3{0.0f, 0.62f, 0.78f}, -0.04f, 0.12f, 0.8f, 0.6f},     // over the top
    {0.24f, Vec3{0.0f, -0.6f, 0.8f}, Vec3{0.0f, -0.82f, 0.57f}, 0.04f, 0.52f, 0.45f, 1.0f},      // IMPACT, in the ground
    {0.44f, Vec3{0.02f, -0.64f, 0.74f}, Vec3{0.0f, -0.86f, 0.5f}, 0.04f, 0.5f, 0.45f, 0.95f},    // held in the crater
    {0.8f, kRest.hand, kRest.blade, 0.0f, 0.0f, 0.0f, 0.0f},                                     // recovered
};
const Ease kHeavyEase[kHeavyN] = {Ease::Out, Ease::In, Ease::In, Ease::Out, Ease::Smooth};

Vec3 slerp_dir(const Vec3& a, const Vec3& b, f32 u) {
    const Vec3 na = glm::normalize(a), nb = glm::normalize(b);
    const Vec3 m = glm::mix(na, nb, u);
    return glm::length(m) > 1e-4f ? glm::normalize(m) : nb;
}
} // namespace

void CharacterAnimator::play_heavy(u8 style) {
    charge_target_ = 0.0f;
    if (style != 0) {
        play_cast(); // the loosed shot / hurled orb
        charge_ = 0.0f;
        charge_w_ = 0.0f;
        return;
    }
    // The chop starts from the wind-up pose (wherever the charge held the sword), fully blended in.
    swing_from_ = charge_w_ > 0.05f ? charge_params() : SwingParams{};
    swing_from_rest_ = charge_w_ <= 0.05f;
    swing_kind_ = 2;
    swing_start_ = 0.0f;
    swing_t_ = 0.0f;
    cast_t_ = -1.0f;
    charge_ = 0.0f;
    charge_w_ = 0.0f;
}

CharacterAnimator::SwingParams CharacterAnimator::charge_params() const {
    // Heaved up overhead, the blade laid back over the shoulders; the body leans back and sinks into
    // its legs as the charge builds, and at full strength it trembles with the strain.
    const f32 c = charge_;
    const f32 shake = c > 0.95f ? 0.035f * std::sin(wobble_ * 47.0f) : 0.0f;
    SwingParams p;
    p.hand = Vec3{-0.16f, 0.9f + 0.06f * c, -0.1f - 0.12f * c};
    p.blade = Vec3{0.1f, 0.36f - 0.1f * c, -0.93f};
    p.coil = -0.18f - 0.12f * c + shake;
    p.lean = -0.12f - 0.12f * c;
    p.guard = 1.0f;
    p.crouch = 0.2f + 0.35f * c;
    return p;
}

void CharacterAnimator::play_swing(f32 lead_in) {
    swing_start_ = 0.0f;
    if (swing_t_ >= 0.0f) {
        const f32 impact = swing_kind_ == 0 ? kSwingImpact : swing_kind_ == 2 ? kHeavyImpact : 0.2f;
        if (swing_t_ < impact || swing_kind_ == 2) {
            return; // committed: the blow hasn't landed yet (and a heavy chop plays out in full)
        }
        // Chain the combo: the next cut starts from wherever this one has the blade right now.
        swing_from_ = swing_params();
        swing_from_rest_ = false;
        swing_kind_ = swing_kind_ == 0 ? 1 : 0;
    } else {
        swing_from_ = SwingParams{};
        swing_from_rest_ = true;
        swing_kind_ = 0;
        swing_start_ = glm::clamp(lead_in, 0.0f, kSwingImpact - 0.05f);
    }
    swing_t_ = swing_start_;
    cast_t_ = -1.0f;
}

f32 CharacterAnimator::swing_until_impact() const {
    if (swing_t_ < 0.0f) {
        return 0.0f;
    }
    return (swing_kind_ == 0 ? kSwingImpact : swing_kind_ == 2 ? kHeavyImpact : 0.2f) - swing_t_;
}

CharacterAnimator::SwingParams CharacterAnimator::swing_params() const {
    if (swing_t_ < 0.0f && charge_w_ > 1e-3f && charge_style_ == 0) {
        return charge_params(); // holding the heavy wind-up overhead
    }
    const SwingKey* keys = swing_kind_ == 0 ? kFore : swing_kind_ == 2 ? kHeavy : kBack;
    const Ease* eases = swing_kind_ == 0 ? kForeEase : swing_kind_ == 2 ? kHeavyEase : kBackEase;
    const int n = swing_kind_ == 0 ? kForeN : swing_kind_ == 2 ? kHeavyN : kBackN;
    const f32 t = std::max(swing_t_, 0.0f);
    // The segment we're in: from the previous key (or the swing's start pose) to the next key.
    SwingKey from{0.0f, swing_from_.hand, swing_from_.blade, swing_from_.coil, swing_from_.lean, swing_from_.guard,
                  swing_from_.crouch};
    int i = 0;
    while (i < n && t > keys[i].t) {
        from = keys[i];
        ++i;
    }
    if (i >= n) {
        const SwingKey& k = keys[n - 1];
        return SwingParams{k.hand, k.blade, k.coil, k.lean, k.guard, k.crouch};
    }
    const SwingKey& to = keys[i];
    const f32 span = std::max(to.t - from.t, 1e-4f);
    const f32 u = ease(eases[i], glm::clamp((t - from.t) / span, 0.0f, 1.0f));
    // The hand travels on a gentle arc (bowed away from the shoulder) rather than a straight chord.
    Vec3 hand = glm::mix(from.hand, to.hand, u);
    const f32 reach = glm::mix(glm::length(from.hand), glm::length(to.hand), u);
    if (glm::length(hand) > 1e-4f) {
        hand = glm::normalize(hand) * reach;
    }
    SwingParams p;
    p.hand = hand;
    p.blade = slerp_dir(from.blade, to.blade, u);
    p.coil = glm::mix(from.coil, to.coil, u);
    p.lean = glm::mix(from.lean, to.lean, u);
    p.guard = glm::mix(from.guard, to.guard, u);
    p.crouch = glm::mix(from.crouch, to.crouch, u);
    return p;
}

f32 CharacterAnimator::swing_env() const {
    if (swing_t_ < 0.0f) {
        return 0.0f;
    }
    const f32 dur = swing_kind_ == 0 ? kSwingDur : swing_kind_ == 2 ? kHeavyDur : 0.55f;
    // A swing from rest blends in over the walk pose quickly; a chained cut is already fully in.
    const f32 in = swing_from_rest_ ? glm::smoothstep(swing_start_, swing_start_ + 0.06f, swing_t_) : 1.0f;
    const f32 out = 1.0f - glm::smoothstep(dur - 0.12f, dur, swing_t_);
    return in * out;
}

bool CharacterAnimator::swing_cutting() const {
    if (swing_t_ < 0.0f) {
        return false;
    }
    if (swing_kind_ == 2) {
        return swing_t_ > 0.1f && swing_t_ < 0.27f;
    }
    return swing_kind_ == 0 ? (swing_t_ > 0.17f && swing_t_ < 0.33f) : (swing_t_ > 0.08f && swing_t_ < 0.25f);
}

CharacterAnimator::ArmSolve CharacterAnimator::solve_swing_arm(const CharacterModel& model, const SwingParams& p) {
    ArmSolve out;
    const std::vector<Bone>& bones = model.bones();
    const int iu = model.bone_index(BonePart::UpperArmL);
    const int il = model.bone_index(BonePart::LowerArmL);
    if (iu < 0 || il < 0) {
        return out;
    }
    // The sword arm in TORSO space (its parent): shoulder joint, upper-arm + forearm lengths.
    const Vec3 shoulder = bones[static_cast<usize>(iu)].joint_offset;
    const f32 l1 = std::max(std::abs(bones[static_cast<usize>(il)].joint_offset.y), 0.05f);
    const f32 l2 = std::max(std::abs(bones[static_cast<usize>(il)].box_center.y * 2.0f), 0.05f);
    const Vec3 target = shoulder + p.hand * (l1 + l2);

    // Two-bone IK: the elbow sits on the circle of solutions, picked toward a pole (down, out to the
    // right and a touch back - where a swordsman's elbow goes).
    Vec3 d = target - shoulder;
    const f32 dist = glm::clamp(glm::length(d), 0.02f, (l1 + l2) * 0.995f);
    d = glm::length(d) > 1e-5f ? glm::normalize(d) : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 pole = glm::normalize(Vec3{-0.65f, -1.0f, -0.25f});
    Vec3 bend = pole - glm::dot(pole, d) * d;
    bend = glm::length(bend) > 1e-4f ? glm::normalize(bend) : glm::normalize(glm::cross(d, Vec3{1.0f, 0.0f, 0.0f}));
    const f32 cos_a = glm::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * dist), -1.0f, 1.0f);
    const f32 sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    const Vec3 elbow = shoulder + l1 * (d * cos_a + bend * sin_a);
    const Vec3 hand = shoulder + d * dist;
    const Vec3 u = glm::normalize(elbow - shoulder); // upper arm direction
    const Vec3 f = glm::normalize(hand - elbow);     // forearm direction

    // The upper arm's frame: the bone runs down its local -Y, and the elbow hinges about its local X
    // (the rig's forearm bends toward local -Z as the X angle grows) - so pick Z on the elbow side.
    const f32 cos_e = glm::clamp(glm::dot(u, f), -1.0f, 1.0f);
    Vec3 zl = -(f - cos_e * u);
    zl = glm::length(zl) > 1e-4f ? glm::normalize(zl) : glm::normalize(bend);
    const Vec3 yl = -u;
    const Vec3 xl = glm::normalize(glm::cross(yl, zl));
    zl = glm::cross(xl, yl);
    out.upper = glm::normalize(glm::quat_cast(Mat3{xl, yl, zl}));
    const f32 elbow_angle = std::acos(cos_e);
    out.lower = glm::angleAxis(elbow_angle, Vec3{1.0f, 0.0f, 0.0f});

    // The wrist: turn the blade (the hand frame's -Y) onto the keyed blade direction.
    const Quat fore = out.upper * out.lower;
    const Vec3 b_local = glm::normalize(glm::conjugate(fore) * glm::normalize(p.blade));
    const Vec3 from = blade_axis();
    const f32 c = glm::dot(from, b_local);
    if (c < -0.999f) {
        out.wrist = glm::angleAxis(Pi, Vec3{1.0f, 0.0f, 0.0f});
    } else {
        const Vec3 axis = glm::cross(from, b_local);
        out.wrist = glm::normalize(Quat{1.0f + c, axis.x, axis.y, axis.z});
    }
    return out;
}

Quat CharacterAnimator::weapon_wrist(const CharacterModel& model) const {
    if (swing_t_ < 0.0f && cast_t_ < 0.0f && charge_w_ > 1e-3f && charge_style_ == 0) {
        // The overhead wind-up lays the blade back over the shoulders.
        return glm::normalize(glm::slerp(QuatIdentity, solve_swing_arm(model, charge_params()).wrist, charge_w_));
    }
    const f32 env = swing_env();
    if (env <= 1e-3f) {
        return QuatIdentity;
    }
    return glm::normalize(glm::slerp(QuatIdentity, solve_swing_arm(model, swing_params()).wrist, env));
}

// How much of the swing's lunge the legs take: all of it standing, none once walking briskly.
f32 CharacterAnimator::swing_stance() const {
    return std::max(0.0f, 1.0f - stride_ * 2.5f);
}

// The sword swing over the locomotion pose: the IK-solved sword arm (player's right = the L-suffixed
// bones), the torso coiling then uncoiling into the blow (and leaning in), the head steadying on the
// target, the shield arm swinging out for balance, and - when standing - the knees bending as the
// weight drives forward onto the lead (left) foot. The legs stay on their walk cycle when moving.
void CharacterAnimator::overlay_swing(const CharacterModel& model, std::vector<Quat>& pose) const {
    const f32 env = swing_env();
    if (env <= 1e-3f) {
        return;
    }
    const SwingParams p = swing_params();
    const ArmSolve arm = solve_swing_arm(model, p);
    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Vec3 az{0.0f, 0.0f, 1.0f};
    const Quat torso = glm::angleAxis(p.coil, ay) * glm::angleAxis(p.lean, ax);
    const Quat head = glm::angleAxis(-0.6f * p.coil, ay) * glm::angleAxis(-0.55f * p.lean, ax);
    // Shield arm: forward + across for balance while winding up, swung out to the side on the cut.
    const Quat off_upper = glm::angleAxis(-0.75f * p.guard, ax) * glm::angleAxis(0.3f * (1.0f - p.guard) * env, az);
    const Quat off_lower = glm::angleAxis(0.7f * p.guard + 0.3f, ax);
    // Lunge + crouch into the blow, only when standing: it fades out as the stride builds, so a walking
    // swing keeps its legs on the walk cycle.
    const f32 stand = swing_stance();
    const f32 c = p.crouch;
    const Quat lead_thigh = glm::angleAxis(-0.42f * c, ax);
    const Quat lead_shin = glm::angleAxis(0.5f * c, ax);
    const Quat back_thigh = glm::angleAxis(0.16f * c, ax);
    const Quat back_shin = glm::angleAxis(0.42f * c, ax);

    blend_overlay(
        model, pose, env,
        [&](BonePart part) -> Quat {
            switch (part) {
                case BonePart::UpperArmL: return arm.upper; // sword arm (player's right)
                case BonePart::LowerArmL: return arm.lower;
                case BonePart::Torso: return torso;
                case BonePart::Head: return head;
                case BonePart::UpperArmR: return off_upper;
                case BonePart::LowerArmR: return off_lower;
                case BonePart::UpperLegR: return lead_thigh; // the player's left leg leads
                case BonePart::LowerLegR: return lead_shin;
                case BonePart::UpperLegL: return back_thigh;
                case BonePart::LowerLegL: return back_shin;
                default: return QuatIdentity;
            }
        },
        [stand](BonePart part) -> f32 {
            switch (part) {
                case BonePart::UpperArmL:
                case BonePart::LowerArmL: return 1.0f;
                case BonePart::Torso: return 0.85f;
                case BonePart::Head: return 0.6f;
                case BonePart::UpperArmR:
                case BonePart::LowerArmR: return 0.55f;
                case BonePart::UpperLegR:
                case BonePart::LowerLegR:
                case BonePart::UpperLegL:
                case BonePart::LowerLegL: return stand; // standing only
                default: return 0.0f;
            }
        });
}

// The HEAVY wind-up held while charging. Style 0 (the Knight) is the overhead sword - the same IK arm +
// coiled, crouched body as the swing, from charge_params(). Style 1 draws a bow: the bow arm (the
// player's right = the L-suffixed bones) thrust out toward the target, the string hand hauled back to
// the cheek. Style 2 raises the staff high overhead with the off hand reaching out, gathering power.
void CharacterAnimator::overlay_charge(const CharacterModel& model, std::vector<Quat>& pose) const {
    const Vec3 ax{1.0f, 0.0f, 0.0f};
    const Vec3 ay{0.0f, 1.0f, 0.0f};
    const Vec3 az{0.0f, 0.0f, 1.0f};
    const f32 c = charge_;
    const f32 shake = c > 0.95f ? 0.04f * std::sin(wobble_ * 51.0f) : 0.0f; // straining at full charge
    if (charge_style_ == 0) {
        const SwingParams p = charge_params();
        const ArmSolve arm = solve_swing_arm(model, p);
        const Quat torso = glm::angleAxis(p.coil, ay) * glm::angleAxis(p.lean, ax);
        const Quat head = glm::angleAxis(-0.5f * p.coil, ay) * glm::angleAxis(-0.6f * p.lean, ax);
        const Quat off_upper = glm::angleAxis(-1.9f, ax) * glm::angleAxis(-0.35f, az); // off hand up on the hilt
        const Quat off_lower = glm::angleAxis(1.1f, ax);
        const f32 stand = swing_stance();
        const Quat thigh_l = glm::angleAxis(-0.3f * p.crouch, ax), shin_l = glm::angleAxis(0.55f * p.crouch, ax);
        const Quat thigh_r = glm::angleAxis(0.2f * p.crouch, ax), shin_r = glm::angleAxis(0.45f * p.crouch, ax);
        blend_overlay(
            model, pose, charge_w_,
            [&](BonePart part) -> Quat {
                switch (part) {
                    case BonePart::UpperArmL: return arm.upper;
                    case BonePart::LowerArmL: return arm.lower;
                    case BonePart::UpperArmR: return off_upper;
                    case BonePart::LowerArmR: return off_lower;
                    case BonePart::Torso: return torso;
                    case BonePart::Head: return head;
                    case BonePart::UpperLegR: return thigh_l;
                    case BonePart::LowerLegR: return shin_l;
                    case BonePart::UpperLegL: return thigh_r;
                    case BonePart::LowerLegL: return shin_r;
                    default: return QuatIdentity;
                }
            },
            [stand](BonePart part) -> f32 {
                switch (part) {
                    case BonePart::UpperArmL:
                    case BonePart::LowerArmL: return 1.0f;
                    case BonePart::UpperArmR:
                    case BonePart::LowerArmR: return 0.75f;
                    case BonePart::Torso: return 0.9f;
                    case BonePart::Head: return 0.6f;
                    case BonePart::UpperLegR:
                    case BonePart::LowerLegR:
                    case BonePart::UpperLegL:
                    case BonePart::LowerLegL: return stand;
                    default: return 0.0f;
                }
            });
        return;
    }
    Quat w_upper, w_lower, o_upper, o_lower, torso, head;
    if (charge_style_ == 1) {
        // The bow arm straight out toward the target; the string hand drawn further back the longer it's held.
        w_upper = glm::angleAxis(-1.5f + shake, ax) * glm::angleAxis(0.12f, az);
        w_lower = glm::angleAxis(0.06f, ax);
        o_upper = glm::angleAxis(-1.35f, ax) * glm::angleAxis(-0.5f - 0.35f * c, az);
        o_lower = glm::angleAxis(1.2f + 1.0f * c, ax);
        torso = glm::angleAxis(0.32f, ay) * glm::angleAxis(-0.04f, ax);
        head = glm::angleAxis(-0.28f, ay);
    } else {
        // The staff thrust up overhead, the off hand reaching out - the gathered power swelling.
        w_upper = glm::angleAxis(-2.35f - 0.25f * c + shake, ax) * glm::angleAxis(-0.1f, az);
        w_lower = glm::angleAxis(0.35f, ax);
        o_upper = glm::angleAxis(-1.25f - 0.2f * c, ax) * glm::angleAxis(0.35f, az);
        o_lower = glm::angleAxis(0.7f, ax);
        torso = glm::angleAxis(-0.08f - 0.1f * c, ax);
        head = glm::angleAxis(-0.18f, ax);
    }
    blend_overlay(
        model, pose, charge_w_,
        [&](BonePart part) -> Quat {
            switch (part) {
                case BonePart::UpperArmL: return w_upper;
                case BonePart::LowerArmL: return w_lower;
                case BonePart::UpperArmR: return o_upper;
                case BonePart::LowerArmR: return o_lower;
                case BonePart::Torso: return torso;
                case BonePart::Head: return head;
                default: return QuatIdentity;
            }
        },
        [](BonePart part) -> f32 {
            switch (part) {
                case BonePart::UpperArmL:
                case BonePart::LowerArmL:
                case BonePart::UpperArmR:
                case BonePart::LowerArmR: return 1.0f;
                case BonePart::Torso: return 0.6f;
                case BonePart::Head: return 0.4f;
                default: return 0.0f;
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
    // Lean eagerly into movement (forward = +Z in the model's local frame); backpedalling only
    // tips back a touch.
    const f32 lean = glm::clamp(speed_ / 6.0f, 0.0f, 1.0f) * 0.16f * std::max(dir_, -0.35f);
    // Squash & stretch: squat wide at footfall, stretch tall mid-stride - the jelly bounce.
    const f32 q = std::abs(std::sin(ph));
    const f32 squash = (0.5f - q) * 0.14f * s + breathe * 0.02f * idle;
    const f32 sy = 1.0f + squash;
    const f32 sxz = 1.0f - squash * 0.6f;

    // A sword swing drops the hips + drives the body forward into the blow (standing only).
    const f32 crouch = swing_t_ >= 0.0f ? swing_params().crouch * swing_env() * swing_stance() : 0.0f;
    Mat4 m = glm::translate(Mat4{1.0f}, Vec3{sway, bob - 0.06f * crouch, 0.09f * crouch});
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
