#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/SkinnedMesh.h>

namespace alryn {

// Material/colour zones for a skinned mesh, resolved to colours from the CharacterPalette at skin
// time. Mirrors BoneColor 1:1 (same order) so the body and the skinned outfit (OutfitMesh) share one
// resolver: Skin/Shirt/Pants/Hair are the base body; Primary/Accent/Metal/Dark/Glow are equipment.
enum class BodyMaterial : u8 {
    Skin = 0,
    Shirt = 1,
    Pants = 2,
    Hair = 3,
    Eye = 4,
    Primary = 5,
    Accent = 6,
    Metal = 7,
    Dark = 8,
    Glow = 9,
};

// Resolves a body/outfit material id to a colour from the palette. Shared by the client + the headless
// preview so the skinned body and outfit colour identically.
Vec3 body_material_color(const CharacterPalette& pal, BodyMaterial mat);

// Builds a CONTINUOUS low-poly humanoid skinned mesh from `model`'s bind-pose skeleton: lofted tube
// limbs/torso/neck that share a ring at each joint and a head/hands/feet, with each ring weighted to
// the bone(s) it spans so the surface bends smoothly at the elbows/knees/shoulders/hips when posed.
// Pure geometry; deform it with CharacterModel::joint_matrices(...) via skin().
SkinnedMesh build_body_mesh(const CharacterModel& model);

} // namespace alryn
