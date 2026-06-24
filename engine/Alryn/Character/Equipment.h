#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <array>

namespace alryn {

// A player's worn gear is **modular but "one piece for the look"**: an OUTFIT (the whole worn set,
// themed by the player's role - plate for a Knight, a robe for a Mage, ...) at a TIER and a chosen
// primary COLOUR, plus a WEAPON at a tier. The tier picks the mesh detail + the trim (ragged cloth ->
// gold-trimmed master gear) AND a stat bonus, so nicer gear bought from town shops both looks better
// and makes the player more powerful. Pure data + maths so the tuning is shared by server, client and
// tests; the visual is built from this in Character/Outfit + Character/Weapon.

enum class EquipmentTier : u8 {
    Ragged = 0, // starting rags - no bonus
    Worn = 1,   // basic kit
    Fine = 2,   // a proper outfit
    Master = 3, // the reference-grade gear (gold trim, plume, glow)
};
inline constexpr u8 kTierCount = 4;

inline const char* tier_name(EquipmentTier t) {
    switch (t) {
        case EquipmentTier::Ragged: return "RAGGED";
        case EquipmentTier::Worn: return "WORN";
        case EquipmentTier::Fine: return "FINE";
        case EquipmentTier::Master: return "MASTER";
    }
    return "?";
}

// The worn gear. Defaults to the starting rags (tier 0, the first tint, the role's first weapon).
struct Equipment {
    u8 outfit_tier = 0;  // EquipmentTier of the worn outfit
    u8 weapon_tier = 0;  // EquipmentTier of the held weapon
    u8 outfit_tint = 0;  // index into outfit_tints() - the player-chosen primary colour
    u8 weapon_index = 0; // which weapon within the role's allowed set (for changing weapons)

    bool operator==(const Equipment&) const = default;

    EquipmentTier outfit() const { return static_cast<EquipmentTier>(outfit_tier % kTierCount); }
    EquipmentTier weapon() const { return static_cast<EquipmentTier>(weapon_tier % kTierCount); }
};

// Selectable outfit PRIMARY colours - the player recolours the cloth/primary panels of their gear
// from these (the steel/leather base + the tier's trim accent are not recoloured). Shared by the UI
// swatches, the outfit builder and the tests.
inline const std::array<Vec3, 8>& outfit_tints() {
    static const std::array<Vec3, 8> tints = {
        Vec3{0.24f, 0.34f, 0.66f}, // royal blue
        Vec3{0.62f, 0.20f, 0.22f}, // crimson
        Vec3{0.26f, 0.42f, 0.26f}, // forest green
        Vec3{0.42f, 0.28f, 0.58f}, // violet
        Vec3{0.20f, 0.46f, 0.50f}, // teal
        Vec3{0.78f, 0.78f, 0.80f}, // white / silver
        Vec3{0.50f, 0.34f, 0.18f}, // tan leather
        Vec3{0.14f, 0.15f, 0.18f}, // charcoal
    };
    return tints;
}
inline Vec3 outfit_tint_of(u8 index) {
    const auto& t = outfit_tints();
    return t[index % t.size()];
}

// The trim / accent colour a tier confers - dark bronze rags climbing to bright gold at master, so
// higher-tier gear reads as fancier regardless of the chosen primary colour (the reference art's
// gold edging is the top tier).
inline Vec3 tier_accent(EquipmentTier t) {
    switch (t) {
        case EquipmentTier::Ragged: return Vec3{0.34f, 0.30f, 0.24f}; // dull cord
        case EquipmentTier::Worn: return Vec3{0.55f, 0.45f, 0.28f};   // tarnished brass
        case EquipmentTier::Fine: return Vec3{0.78f, 0.62f, 0.28f};   // brass / soft gold
        case EquipmentTier::Master: return Vec3{0.95f, 0.78f, 0.30f}; // bright gold
    }
    return Vec3{0.6f};
}

// How polished the base metal/leather reads at a tier (0 grubby .. 1 gleaming) - the outfit builder
// uses it to brighten plate/steel at higher tiers.
inline f32 tier_sheen(EquipmentTier t) {
    return 0.55f + 0.15f * static_cast<f32>(static_cast<u8>(t));
}

// The stat bonus the gear grants ON TOP of role_stats: armour adds health + a slice of mitigation,
// the weapon tier multiplies damage. Ragged = nothing; each tier is a clear, buyable step up.
struct EquipBonus {
    f32 health_add = 0.0f;     // flat health from armour
    f32 mitigation_add = 0.0f; // added 0..1 damage reduction from armour
    f32 damage_mult = 1.0f;    // outgoing-damage multiplier from the weapon
};

inline EquipBonus equipment_bonus(const Equipment& e) {
    const f32 ot = static_cast<f32>(static_cast<u8>(e.outfit()));
    const f32 wt = static_cast<f32>(static_cast<u8>(e.weapon()));
    EquipBonus b;
    b.health_add = ot * 28.0f;        // +0 / +28 / +56 / +84
    b.mitigation_add = ot * 0.035f;   // +0 / .035 / .07 / .105
    b.damage_mult = 1.0f + wt * 0.2f; // x1 / x1.2 / x1.4 / x1.6
    return b;
}

// What it costs (party wallet) to buy a given tier from a town shop. Ragged is free (you start in it).
inline u32 tier_price(EquipmentTier t) {
    switch (t) {
        case EquipmentTier::Ragged: return 0;
        case EquipmentTier::Worn: return 70;
        case EquipmentTier::Fine: return 180;
        case EquipmentTier::Master: return 420;
    }
    return 0;
}

} // namespace alryn
