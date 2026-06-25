#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/SkinnedMesh.h>

namespace alryn {

// Material/colour zones for the body mesh (resolved to colours from the CharacterPalette at skin time).
enum class BodyMaterial : u8 { Skin = 0, Shirt = 1, Pants = 2, Hair = 3 };

// Builds a CONTINUOUS low-poly humanoid skinned mesh from `model`'s bind-pose skeleton: lofted tube
// limbs/torso/neck that share a ring at each joint and a head/hands/feet, with each ring weighted to
// the bone(s) it spans so the surface bends smoothly at the elbows/knees/shoulders/hips when posed.
// Pure geometry; deform it with CharacterModel::joint_matrices(...) via skin().
SkinnedMesh build_body_mesh(const CharacterModel& model);

} // namespace alryn
