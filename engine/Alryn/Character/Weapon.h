#pragma once

#include <Alryn/Character/CharacterModel.h> // CharacterPalette, BoneShape
#include <Alryn/Character/Equipment.h>
#include <Alryn/Core/Math.h>

#include <vector>

namespace alryn {

// A held weapon, modular like the outfit: a TYPE chosen from the role's allowed set (so the player
// can change weapons) at the equipment's tier + colours. Built as a list of shape pieces in
// grip-local space, so BOTH the client and the headless preview can render the same weapon by
// transforming the pieces by the character's hand-joint frame.
enum class WeaponType : u8 { None, Sword, Dagger, Bow, Staff, Mace, Shield };

struct WeaponPiece {
    BoneShape shape = BoneShape::Box;
    Mat4 local{1.0f}; // transform relative to the grip (the hand-joint frame)
    Vec3 color{0.7f};
    bool emissive = false; // glowing pieces (a mage's orb) drawn through the emissive pass
};

// The pieces of a weapon in grip-local space. The tier scales materials/detail; `pal` supplies the
// colours (metal/accent/dark/primary/glow), so the weapon matches the worn outfit's tier + colour.
std::vector<WeaponPiece> weapon_pieces(WeaponType type, EquipmentTier tier, const CharacterPalette& pal);

// The role's main-hand weapon (weapon_index cycles the role's options) + its off-hand (shield /
// dagger / none). Roles: 0 Knight, 1 Hunter, 2 Cleric, 3 Mage.
WeaponType role_weapon(u8 role, u8 weapon_index);
WeaponType role_offhand(u8 role);
u8 role_weapon_count(u8 role); // number of main-hand options (for cycling in the wardrobe)
const char* weapon_name(WeaponType type);

} // namespace alryn
