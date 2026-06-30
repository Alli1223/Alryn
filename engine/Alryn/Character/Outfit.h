#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/Equipment.h>

namespace alryn {

// The outfit themes. The first four are 1:1 with the playable roles; Peasant is the generic NPC garb
// (townsfolk). The caller maps PlayerRole -> OutfitKind so the Character/ module stays independent of
// Game/Roles.
enum class OutfitKind : u8 {
    Plate = 0,   // Knight  - cloth gambeson -> chainmail -> steel plate
    Leather = 1, // Hunter  - leather jerkin -> warden -> beastmaster
    Holy = 2,    // Cleric  - monk robe -> priest -> high prophet
    Robe = 3,    // Mage    - patched robe -> elementalist -> archmage
    Peasant = 4, // NPC townsfolk - a plain tunic + trousers, no tiers
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

// The DESIGN tier (silhouette family) a gear tier renders as, matching the reference art's three looks:
// 0 = basic cloth/leather (squire / apprentice / acolyte / hunter), 1 = rare (chainmail / elementalist
// / priest / warden), 2 = legendary (paladin / archmage / high prophet / beastmaster). The starting
// rags + worn kit share the basic silhouette (the trim accent still brightens per gear tier); a Fine
// outfit is the rare design; Master is the legendary one.
inline int outfit_design_tier(EquipmentTier t) {
    return t == EquipmentTier::Master ? 2 : t == EquipmentTier::Fine ? 1 : 0;
}

// Sets the model's equipment palette from `equip` (primary = chosen colour, accent = tier trim,
// metal/dark from the tier), then appends the themed outfit bones (helm / armour / robe / ...),
// scaled by the outfit tier. Call AFTER CharacterModel::create (so the body + face features exist);
// the pieces parent onto the body bones, so they animate with it.
void apply_outfit(CharacterModel& model, OutfitKind kind, const Equipment& equip);

} // namespace alryn
