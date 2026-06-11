#include <Alryn/Character/CharacterModel.h>

#include <random>

namespace alryn {

namespace {

struct Rng {
    std::mt19937 engine;
    explicit Rng(u32 seed) : engine(seed == 0 ? 1u : seed) {}
    f32 range(f32 a, f32 b) {
        const f32 t = static_cast<f32>(engine() & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
        return a + (b - a) * t;
    }
    u32 next() { return engine(); }
};

Vec3 hsv(f32 h, f32 s, f32 v) {
    h *= 6.0f;
    const int i = static_cast<int>(h);
    const f32 f = h - static_cast<f32>(i);
    const f32 p = v * (1.0f - s);
    const f32 q = v * (1.0f - s * f);
    const f32 t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

} // namespace

// Appends face (eyes/ears) and hair feature bones, all parented to the head
// (bone index 2), positioned/sized/shaped from the chosen appearance. Geometry is
// derived from the head bone so it tracks the character's proportions.
void CharacterModel::add_features(CharacterModel& m, const CharacterAppearance& app) {
    constexpr int kHeadIndex = 2;
    if (static_cast<usize>(kHeadIndex) >= m.bones_.size()) {
        return;
    }
    const Bone& head = m.bones_[kHeadIndex];
    const f32 hs = head.box_size.x; // head extent (≈ width/height)
    const f32 r = hs * 0.5f;        // head half-extent
    const Vec3 c = head.box_center; // head centre in the head-joint frame

    // A feature bone: joint at the head joint, geometry offset via box_center.
    auto add = [&](Vec3 center, Vec3 size, BoneColor color, BoneShape shape) {
        m.bones_.push_back(Bone{BonePart::None, kHeadIndex, Vec3{0.0f}, size, center, color, shape});
    };

    // ---- Eyes (on the front face, +Z) ----
    {
        const f32 fz = c.z + r * 0.96f;       // front surface
        const f32 ey = c.y + r * 0.12f;       // a touch above centre
        f32 ex = r * 0.42f;                   // horizontal spacing
        Vec3 size{hs * 0.16f, hs * 0.16f, hs * 0.10f};
        BoneShape shape = BoneShape::Sphere;
        switch (app.eyes) {
            case EyeStyle::Round: break;
            case EyeStyle::Wide:
                ex = r * 0.52f;
                size = Vec3{hs * 0.20f, hs * 0.20f, hs * 0.10f};
                break;
            case EyeStyle::Sleepy:
                size = Vec3{hs * 0.20f, hs * 0.08f, hs * 0.10f};
                shape = BoneShape::RoundedBox;
                break;
            case EyeStyle::Sharp:
                size = Vec3{hs * 0.18f, hs * 0.11f, hs * 0.10f};
                shape = BoneShape::Box;
                break;
        }
        add(Vec3{-ex, ey, fz}, size, BoneColor::Eye, shape);
        add(Vec3{ex, ey, fz}, size, BoneColor::Eye, shape);
    }

    // ---- Ears (on the sides, ±X) ----
    {
        const f32 sx = r * 0.96f;
        switch (app.ears) {
            case EarStyle::Round:
                add(Vec3{-sx, c.y, c.z}, Vec3{hs * 0.16f}, BoneColor::Skin, BoneShape::Sphere);
                add(Vec3{sx, c.y, c.z}, Vec3{hs * 0.16f}, BoneColor::Skin, BoneShape::Sphere);
                break;
            case EarStyle::Pointed: {
                const Vec3 size{hs * 0.11f, hs * 0.30f, hs * 0.12f};
                add(Vec3{-sx, c.y + r * 0.22f, c.z}, size, BoneColor::Skin, BoneShape::Box);
                add(Vec3{sx, c.y + r * 0.22f, c.z}, size, BoneColor::Skin, BoneShape::Box);
                break;
            }
            case EarStyle::Small:
                add(Vec3{-sx, c.y, c.z}, Vec3{hs * 0.10f}, BoneColor::Skin, BoneShape::Sphere);
                add(Vec3{sx, c.y, c.z}, Vec3{hs * 0.10f}, BoneColor::Skin, BoneShape::Sphere);
                break;
        }
    }

    // ---- Hair (on/around the top) ----
    switch (app.hair) {
        case HairStyle::Bald:
            break;
        case HairStyle::Short:
            add(Vec3{c.x, c.y + r * 0.5f, c.z}, Vec3{hs * 1.06f, hs * 0.62f, hs * 1.06f},
                BoneColor::Hair, BoneShape::RoundedBox);
            break;
        case HairStyle::Spiky:
            add(Vec3{c.x, c.y + r * 0.42f, c.z}, Vec3{hs * 1.04f, hs * 0.5f, hs * 1.04f},
                BoneColor::Hair, BoneShape::RoundedBox);
            add(Vec3{c.x, c.y + r * 1.05f, c.z}, Vec3{hs * 0.55f, hs * 0.7f, hs * 0.55f},
                BoneColor::Hair, BoneShape::Sphere);
            break;
        case HairStyle::Mohawk:
            add(Vec3{c.x, c.y + r * 0.95f, c.z}, Vec3{hs * 0.18f, hs * 0.55f, hs * 1.0f},
                BoneColor::Hair, BoneShape::RoundedBox);
            break;
        case HairStyle::Ponytail:
            add(Vec3{c.x, c.y + r * 0.55f, c.z}, Vec3{hs * 1.06f, hs * 0.6f, hs * 1.06f},
                BoneColor::Hair, BoneShape::RoundedBox);
            add(Vec3{c.x, c.y + r * 0.1f, c.z - r * 0.95f}, Vec3{hs * 0.34f, hs * 0.7f, hs * 0.34f},
                BoneColor::Hair, BoneShape::Cylinder);
            break;
    }
}

CharacterModel CharacterModel::create(u32 seed, const CharacterAppearance& appearance) {
    CharacterModel m = generate(seed);
    m.palette_.skin = skin_color(appearance.skin);
    m.palette_.hair = hair_color_of(appearance.hair_color);
    m.palette_.eye = Vec3{0.09f, 0.08f, 0.10f};
    add_features(m, appearance);
    return m;
}

CharacterModel CharacterModel::generate(u32 seed) {
    Rng rng(seed);
    CharacterModel m;

    const f32 hscale = rng.range(0.85f, 1.05f); // overall height (short + cute)
    const f32 build = rng.range(0.95f, 1.25f);  // width / bulk (stubby)

    // Chibi proportions: a big head over a short torso with stubby little limbs.
    const f32 leg_upper = 0.26f * hscale;
    const f32 leg_lower = 0.24f * hscale;
    const f32 leg_len = leg_upper + leg_lower;
    const f32 torso = 0.42f * hscale;
    const f32 head = 0.40f * hscale;
    const f32 arm_upper = 0.20f * hscale;
    const f32 arm_lower = 0.18f * hscale;

    // Palette.
    static const Vec3 skin_tones[] = {{0.86f, 0.66f, 0.52f}, {0.80f, 0.58f, 0.45f},
                                      {0.65f, 0.45f, 0.34f}, {0.50f, 0.34f, 0.26f},
                                      {0.92f, 0.76f, 0.66f}};
    m.palette_.skin = skin_tones[rng.next() % 5u];
    m.palette_.shirt = hsv(rng.range(0.0f, 1.0f), rng.range(0.45f, 0.85f), rng.range(0.55f, 0.9f));
    m.palette_.pants = hsv(rng.range(0.0f, 1.0f), rng.range(0.15f, 0.45f), rng.range(0.25f, 0.5f));

    m.height_ = leg_len + torso + head;
    m.eye_height_ = leg_len + torso * 0.9f + head * 0.55f;

    auto add = [&](BonePart part, int parent, Vec3 joint, Vec3 size, Vec3 center, BoneColor color,
                   BoneShape shape) {
        m.bones_.push_back({part, parent, joint, size, center, color, shape});
    };

    const f32 hip_w = 0.11f * build;
    const f32 shoulder_y = torso * 0.78f;
    const f32 shoulder_x = 0.40f * build * 0.5f + 0.08f;

    // Rounder body: sphere torso/head/pelvis, cylinder limbs, little round feet.
    // Parents always precede children (indices 0..12).
    add(BonePart::Pelvis, -1, {0.0f, leg_len, 0.0f}, {0.32f * build, 0.24f, 0.24f * build},
        {0.0f, 0.0f, 0.0f}, BoneColor::Pants, BoneShape::RoundedBox);
    add(BonePart::Torso, 0, {0.0f, 0.10f, 0.0f}, {0.42f * build, torso, 0.30f * build},
        {0.0f, torso * 0.5f, 0.0f}, BoneColor::Shirt, BoneShape::RoundedBox);
    add(BonePart::Head, 1, {0.0f, torso * 0.9f, 0.0f}, {head, head * 0.96f, head},
        {0.0f, head * 0.5f, 0.0f}, BoneColor::Skin, BoneShape::RoundedBox);
    add(BonePart::UpperArmL, 1, {-shoulder_x, shoulder_y, 0.0f}, {0.15f, arm_upper, 0.15f},
        {0.0f, -arm_upper * 0.5f, 0.0f}, BoneColor::Shirt, BoneShape::Cylinder);
    add(BonePart::LowerArmL, 3, {0.0f, -arm_upper, 0.0f}, {0.13f, arm_lower, 0.13f},
        {0.0f, -arm_lower * 0.5f, 0.0f}, BoneColor::Skin, BoneShape::Cylinder);
    add(BonePart::UpperArmR, 1, {shoulder_x, shoulder_y, 0.0f}, {0.15f, arm_upper, 0.15f},
        {0.0f, -arm_upper * 0.5f, 0.0f}, BoneColor::Shirt, BoneShape::Cylinder);
    add(BonePart::LowerArmR, 5, {0.0f, -arm_upper, 0.0f}, {0.13f, arm_lower, 0.13f},
        {0.0f, -arm_lower * 0.5f, 0.0f}, BoneColor::Skin, BoneShape::Cylinder);
    add(BonePart::UpperLegL, 0, {-hip_w, 0.0f, 0.0f}, {0.18f, leg_upper, 0.18f},
        {0.0f, -leg_upper * 0.5f, 0.0f}, BoneColor::Pants, BoneShape::Cylinder);
    add(BonePart::LowerLegL, 7, {0.0f, -leg_upper, 0.0f}, {0.16f, leg_lower, 0.16f},
        {0.0f, -leg_lower * 0.5f, 0.0f}, BoneColor::Pants, BoneShape::Cylinder);
    add(BonePart::UpperLegR, 0, {hip_w, 0.0f, 0.0f}, {0.18f, leg_upper, 0.18f},
        {0.0f, -leg_upper * 0.5f, 0.0f}, BoneColor::Pants, BoneShape::Cylinder);
    add(BonePart::LowerLegR, 9, {0.0f, -leg_upper, 0.0f}, {0.16f, leg_lower, 0.16f},
        {0.0f, -leg_lower * 0.5f, 0.0f}, BoneColor::Pants, BoneShape::Cylinder);
    add(BonePart::FootL, 8, {0.0f, -leg_lower, 0.0f}, {0.19f, 0.14f, 0.27f}, {0.0f, 0.03f, 0.06f},
        BoneColor::Pants, BoneShape::Sphere);
    add(BonePart::FootR, 10, {0.0f, -leg_lower, 0.0f}, {0.19f, 0.14f, 0.27f}, {0.0f, 0.03f, 0.06f},
        BoneColor::Pants, BoneShape::Sphere);

    return m;
}

std::vector<Mat4> CharacterModel::bone_matrices(const Mat4& root,
                                                const std::vector<Quat>& pose) const {
    std::vector<Mat4> joint(bones_.size());
    std::vector<Mat4> box(bones_.size());
    for (usize i = 0; i < bones_.size(); ++i) {
        const Bone& b = bones_[i];
        const Mat4& parent = (b.parent < 0) ? root : joint[static_cast<usize>(b.parent)];
        const Quat rotation = i < pose.size() ? pose[i] : QuatIdentity;
        joint[i] = parent * glm::translate(Mat4{1.0f}, b.joint_offset) * glm::mat4_cast(rotation);
        box[i] = joint[i] * glm::translate(Mat4{1.0f}, b.box_center) *
                 glm::scale(Mat4{1.0f}, b.box_size);
    }
    return box;
}

} // namespace alryn
