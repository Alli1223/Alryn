#pragma once

#include <Alryn/Character/CharacterAppearance.h>
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

// Skin/Shirt/Pants/Hair/Eye are the base body; Primary/Accent/Metal/Dark are equipment colours
// (Primary = the player's chosen heraldic colour, Accent = the tier trim/gold, Metal = steel,
// Dark = leather/straps). Glow marks an emissive piece (mage eyes, lit visor, gem).
enum class BoneColor : u8 { Skin, Shirt, Pants, Hair, Eye, Primary, Accent, Metal, Dark, Glow };
enum class BoneShape : u8 { Box, Sphere, Cylinder, RoundedBox, Capsule };

struct Bone {
    BonePart part = BonePart::None;
    int parent = -1;         // index of parent bone, -1 for the root
    Vec3 joint_offset{0.0f}; // joint position relative to the parent joint (bind pose)
    Vec3 box_size{0.1f};     // shape dimensions (scales the unit shape)
    Vec3 box_center{0.0f};   // shape centre relative to this joint
    BoneColor color = BoneColor::Skin;
    BoneShape shape = BoneShape::Box;
    Quat box_rotation = QuatIdentity; // bind-pose orientation of the shape (e.g. a diagonal strap)
    // True for face/hair/equipment pieces that ride ON TOP of the skinned body (drawn as primitives);
    // false for the core body + joint fillers, which the continuous skinned mesh (BodyMesh) replaces.
    bool attachment = false;
};

struct CharacterPalette {
    Vec3 skin{0.85f, 0.65f, 0.5f};
    Vec3 shirt{0.2f, 0.45f, 0.8f};
    Vec3 pants{0.2f, 0.2f, 0.28f};
    Vec3 hair{0.3f, 0.2f, 0.12f};
    Vec3 eye{0.09f, 0.08f, 0.10f};
    // Equipment colours (set by Character/Outfit from the worn Equipment).
    Vec3 primary{0.30f, 0.40f, 0.66f}; // player's chosen heraldic / cloth colour
    Vec3 accent{0.80f, 0.65f, 0.30f};  // tier trim (brass -> gold)
    Vec3 metal{0.70f, 0.73f, 0.80f};   // steel / plate
    Vec3 dark{0.20f, 0.16f, 0.12f};    // leather, straps, under-layers
    Vec3 glow{0.4f, 0.8f, 1.0f};       // emissive accents (eyes / gems)
};

// A procedurally-generated low-poly humanoid: a small skeleton of boxes with
// per-character proportions and colours derived from a seed (e.g. a player id).
// Pure data + maths (no GPU), so it's testable headlessly. Parents always precede
// their children, so a single forward pass computes all bone transforms.
class CharacterModel {
public:
    // A random character from a seed (the 13-bone body only).
    static CharacterModel generate(u32 seed);
    // A character with seed-derived proportions/clothes but the player's chosen
    // skin/eyes/ears/hair, adding face + hair feature bones on top of the body.
    static CharacterModel create(u32 seed, const CharacterAppearance& appearance);

    const std::vector<Bone>& bones() const { return bones_; }
    const CharacterPalette& palette() const { return palette_; }
    CharacterPalette& palette() { return palette_; } // mutable, so Outfit can set equipment colours
    usize bone_count() const { return bones_.size(); }

    // Index of the first bone with this part (-1 if none). Used by Character/Outfit to parent
    // equipment pieces onto the body (e.g. a breastplate onto the torso).
    int bone_index(BonePart part) const;
    // Append an equipment/outfit bone (its `parent` must already exist - parents precede children).
    void add_bone(const Bone& b) { bones_.push_back(b); }

    f32 height() const { return height_; }
    f32 eye_height() const { return eye_height_; }

    // World-space box transforms for every bone, given the character's root
    // transform and a per-bone pose (rotation at each joint). pose may be shorter
    // than bone_count() (missing entries are treated as identity).
    std::vector<Mat4> bone_matrices(const Mat4& root, const std::vector<Quat>& pose) const;

    // World-space JOINT frames (position + orientation, no box scale) for every bone.
    // This is what you rigidly attach a held prop (sword, shield, bow) to, so it rotates
    // with the limb - unlike bone_matrices(), whose columns are scaled by box_size.
    std::vector<Mat4> joint_matrices(const Mat4& root, const std::vector<Quat>& pose) const;

private:
    // Appends face + hair feature bones (parented to the head) per the appearance.
    static void add_features(CharacterModel& model, const CharacterAppearance& appearance);

    std::vector<Bone> bones_;
    CharacterPalette palette_;
    f32 height_ = 1.8f;
    f32 eye_height_ = 1.6f;
};

} // namespace alryn
