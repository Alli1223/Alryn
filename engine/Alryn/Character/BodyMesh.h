#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/SkinnedMesh.h>

namespace alryn {

// Material/colour zones for a skinned mesh, resolved to colours from the CharacterPalette at skin
// time. The first ten mirror BoneColor 1:1 (same order) so the body and the skinned outfit (OutfitMesh)
// share one resolver: Skin/Shirt/Pants/Hair are the base body; Primary/Accent/Metal/Dark/Glow are
// equipment. The rest are skinned-garment shades derived from the palette (no BoneColor twin).
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
    PrimaryShade = 10, // the cloth colour in shadow - quilting lines, a lining, an under-layer
    Linen = 11,        // undyed linen (a coif, a shirt, a priest's alb)
    Straw = 12,        // woven straw (a sun hat)
    Mail = 13,         // riveted mail: the steel, darkened by its rings
    Leather = 14,      // a lighter tan leather (boots, belts) beside the dark straps
};

// The body's torso cross-sections, shared by the body and the outfit builder so garments follow the
// same chest / waist / hip shape: `t` runs pelvis joint (0) -> neck base (1); half-width `rx`, half-
// depth `rz` and a forward offset `dz` (chest out, seat back), all in metres for this character.
struct TorsoRing {
    f32 t, rx, rz, dz;
};
std::vector<TorsoRing> torso_profile(const CharacterModel& model);
// The profile's cross-section at any t, interpolated between its rings (clamped at the ends).
TorsoRing torso_ring_at(const std::vector<TorsoRing>& profile, f32 t);

// Resolves a body/outfit material id to a colour from the palette. Shared by the client + the headless
// preview so the skinned body and outfit colour identically.
Vec3 body_material_color(const CharacterPalette& pal, BodyMaterial mat);

// Builds a CONTINUOUS low-poly humanoid skinned mesh from `model`'s bind-pose skeleton: lofted tube
// limbs/torso/neck that share a ring at each joint and a head/hands/feet, with each ring weighted to
// the bone(s) it spans so the surface bends smoothly at the elbows/knees/shoulders/hips when posed.
// Pure geometry; deform it with CharacterModel::joint_matrices(...) via skin().
SkinnedMesh build_body_mesh(const CharacterModel& model);

} // namespace alryn
