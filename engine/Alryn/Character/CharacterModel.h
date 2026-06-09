#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vector>

namespace alryn {

// Identifies a bone for the animation system to target.
enum class BonePart : u8 {
    None,
    Pelvis,
    Torso,
    Head,
    UpperArmL,
    LowerArmL,
    UpperArmR,
    LowerArmR,
    UpperLegL,
    LowerLegL,
    UpperLegR,
    LowerLegR,
    FootL,
    FootR,
};

enum class BoneColor : u8 { Skin, Shirt, Pants };
enum class BoneShape : u8 { Box, Sphere, Cylinder };

struct Bone {
    BonePart part = BonePart::None;
    int parent = -1;         // index of parent bone, -1 for the root
    Vec3 joint_offset{0.0f}; // joint position relative to the parent joint (bind pose)
    Vec3 box_size{0.1f};     // shape dimensions (scales the unit shape)
    Vec3 box_center{0.0f};   // shape centre relative to this joint
    BoneColor color = BoneColor::Skin;
    BoneShape shape = BoneShape::Box;
};

struct CharacterPalette {
    Vec3 skin{0.85f, 0.65f, 0.5f};
    Vec3 shirt{0.2f, 0.45f, 0.8f};
    Vec3 pants{0.2f, 0.2f, 0.28f};
};

// A procedurally-generated low-poly humanoid: a small skeleton of boxes with
// per-character proportions and colours derived from a seed (e.g. a player id).
// Pure data + maths (no GPU), so it's testable headlessly. Parents always precede
// their children, so a single forward pass computes all bone transforms.
class CharacterModel {
public:
    static CharacterModel generate(u32 seed);

    const std::vector<Bone>& bones() const { return bones_; }
    const CharacterPalette& palette() const { return palette_; }
    usize bone_count() const { return bones_.size(); }

    f32 height() const { return height_; }
    f32 eye_height() const { return eye_height_; }

    // World-space box transforms for every bone, given the character's root
    // transform and a per-bone pose (rotation at each joint). pose may be shorter
    // than bone_count() (missing entries are treated as identity).
    std::vector<Mat4> bone_matrices(const Mat4& root, const std::vector<Quat>& pose) const;

private:
    std::vector<Bone> bones_;
    CharacterPalette palette_;
    f32 height_ = 1.8f;
    f32 eye_height_ = 1.6f;
};

} // namespace alryn
