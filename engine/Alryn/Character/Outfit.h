#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/Equipment.h>

namespace alryn {

// The four outfit themes, 1:1 with the playable roles. The caller maps PlayerRole -> OutfitKind so
// the Character/ module stays independent of Game/Roles.
enum class OutfitKind : u8 {
    Plate = 0,   // Knight  - steel plate + gold trim, plumed helm
    Leather = 1, // Hunter  - leather jerkin, hood + mask, back quiver
    Holy = 2,    // Cleric  - white/blue/gold robe, mitre
    Robe = 3,    // Mage    - hooded robe, gold trim
};

// Maps a role index (PlayerRole value) to its outfit theme.
inline OutfitKind outfit_kind_for_role(u8 role) {
    switch (role % 4u) {
        case 0: return OutfitKind::Plate;
        case 1: return OutfitKind::Leather;
        case 2: return OutfitKind::Holy;
        default: return OutfitKind::Robe;
    }
}

// Sets the model's equipment palette from `equip` (primary = chosen colour, accent = tier trim,
// metal/dark from the tier), then appends the themed outfit bones (helm / armour / robe / ...),
// scaled by the outfit tier. Call AFTER CharacterModel::create (so the body + face features exist);
// the pieces parent onto the body bones, so they animate with it.
void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip);

} // namespace alryn
