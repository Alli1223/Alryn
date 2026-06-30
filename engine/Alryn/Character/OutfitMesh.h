#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/Equipment.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/SkinnedMesh.h>

namespace alryn {

// Builds the worn equipment as a CONTINUOUS skinned mesh (deformed by the bones, like the body):
// armoured / clothed limbs, a torso shell, and a draping skirt that all flow across the joints, per
// outfit theme + tier. It is skinned and drawn exactly like the body. The small decorative silhouette
// pieces (helm, pauldron caps, tabard, mitre, quiver) remain primitive attachment bones layered on
// top by apply_outfit. Returns an empty mesh if the model has no skeleton.
SkinnedMesh build_outfit_mesh(const CharacterModel& model, OutfitKind kind, const Equipment& equip);

} // namespace alryn
