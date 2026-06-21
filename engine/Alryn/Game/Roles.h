#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

namespace alryn {

// The three playable combat roles. Each spawns with its own weapon (rendered in-hand),
// a tuned stat block (health / speed / damage / mitigation), and three cooldown-gated
// abilities. Server-authoritative: the role + the ability the player invokes this tick
// ride in PlayerInput; the role rides back in PlayerState so every client renders the
// right weapon. Pure data + maths so the tuning is shared by the server and the tests.
enum class PlayerRole : u8 {
    Knight = 0, // tank: sword + shield, tough, mitigates damage, taunts
    Hunter = 1, // damage: bow, fragile but hits hard at range
    Cleric = 2, // healer: staff, mends allies and smites foes
};

inline constexpr u8 kRoleCount = 3;
inline constexpr u8 kAbilitySlots = 4; // abilities per role (keys 1/2/3/4)

// Per-role base stats. `move_speed` overrides the character controller's walk speed;
// `damage_reduction` is the fraction of incoming damage soaked before any active buff.
struct RoleStats {
    f32 max_health = 100.0f;
    f32 move_speed = 6.0f;       // m/s (controller walk speed for this role)
    f32 melee_damage = 30.0f;    // per melee swing
    f32 ranged_damage = 26.0f;   // per thrown/loosed projectile
    f32 damage_reduction = 0.0f; // 0..1 fraction of incoming damage ignored
};

inline RoleStats role_stats(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return {170.0f, 5.4f, 42.0f, 18.0f, 0.30f};
        case PlayerRole::Hunter: return {95.0f, 6.6f, 22.0f, 40.0f, 0.0f};
        case PlayerRole::Cleric: return {110.0f, 6.0f, 18.0f, 24.0f, 0.10f};
    }
    return {};
}

inline const char* role_name(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return "KNIGHT";
        case PlayerRole::Hunter: return "HUNTER";
        case PlayerRole::Cleric: return "CLERIC";
    }
    return "?";
}

// A castable ability: a display name and its cooldown (seconds). Slots map to keys 1/2/3.
struct AbilityDef {
    const char* name = "";
    f32 cooldown = 1.0f;
};

// The three abilities of a role, indexed by slot (0..kAbilitySlots-1).
//   Knight: Shield Bash (cone burst + knockback) / Bulwark (heavy mitigation) / Taunt (pull aggro)
//   Hunter: Power Shot (one heavy arrow) / Volley (spread of arrows) / Dash (burst of speed)
//   Cleric: Heal (mend the nearest ally) / Sanctuary (heal everyone nearby) / Smite (holy bolt)
inline AbilityDef ability_def(PlayerRole role, u8 slot) {
    switch (role) {
        case PlayerRole::Knight:
            switch (slot) {
                case 0: return {"SHIELD BASH", 4.0f};
                case 1: return {"BULWARK", 12.0f};
                case 2: return {"CONSECRATION", 12.0f};
                default: return {"TAUNT", 8.0f};
            }
        case PlayerRole::Hunter:
            switch (slot) {
                case 0: return {"POWER SHOT", 3.0f};
                case 1: return {"VOLLEY", 6.0f};
                case 2: return {"DASH", 5.0f};
                default: return {"PIERCING SHOT", 6.0f};
            }
        case PlayerRole::Cleric:
            switch (slot) {
                case 0: return {"HEAL", 2.5f};
                case 1: return {"SANCTUARY", 11.0f};
                case 2: return {"SMITE", 4.0f};
                default: return {"AEGIS", 9.0f};
            }
    }
    return {};
}

// --- Ability tuning (shared by the server and the headless tests) -----------------
inline constexpr f32 kBashDamage = 55.0f;       // Knight shield bash, plus knockback
inline constexpr f32 kBashKnockback = 3.5f;     // metres an enemy is shoved
inline constexpr f32 kBlockReduction = 0.5f;    // extra damage reduction while a shield is held up
inline constexpr f32 kBulwarkReduction = 0.55f; // extra damage reduction while raised
inline constexpr f32 kBulwarkDuration = 5.0f;
inline constexpr f32 kTauntDuration = 5.0f;
inline constexpr f32 kTauntRadius = 12.0f;       // Knight Taunt: pull nearby enemies' aggro
// Knight Consecration: a holy ground aura at the player's feet that taunts enemies inside it
// and burns them for a little damage over its lifetime (a taunt-plus-damage zone).
inline constexpr f32 kConsecrationRadius = 4.5f;
inline constexpr f32 kConsecrationDuration = 6.0f;
inline constexpr f32 kConsecrationDPS = 11.0f; // damage/sec to enemies standing in it
inline constexpr f32 kPowerShotMult = 2.4f;     // x the Hunter's ranged damage
inline constexpr f32 kPierceMult = 3.4f;        // x ranged damage - Piercing Shot, a heavy bolt
inline constexpr f32 kVolleyMult = 0.9f;        // per arrow of the 3-arrow spread
inline constexpr f32 kDashSpeedMult = 1.7f;     // walk-speed multiplier while dashing
inline constexpr f32 kDashDuration = 1.6f;
inline constexpr f32 kHealAmount = 45.0f;       // Cleric single-target mend
inline constexpr f32 kSanctuaryAmount = 30.0f;  // AoE heal to all nearby allies
inline constexpr f32 kHealRadius = 16.0f;
inline constexpr f32 kSmiteDamage = 60.0f;      // Cleric holy bolt

// Cleric channelled AOE heal (hold right mouse to charge; auto-casts a ground aura at full).
inline constexpr f32 kHealChargeTime = 1.6f;    // seconds of channelling to release the aura
inline constexpr f32 kHealAuraDuration = 6.0f;  // how long the ground aura lingers
inline constexpr f32 kHealAuraRadius = 5.0f;    // its radius (metres)
inline constexpr f32 kHealAuraRate = 16.0f;     // hp/sec restored to allies standing in it

// Cleric Aegis: a protective shield placed on a friendly player/NPC that absorbs incoming
// damage until it's spent or fades. Rendered as a glowing sphere around the target.
inline constexpr f32 kAegisAmount = 65.0f;      // damage the shield soaks before breaking
inline constexpr f32 kAegisDuration = 12.0f;    // seconds before an unbroken shield fades
inline constexpr f32 kAegisRange = 18.0f;       // how far an ally can be shielded

// --- Ground auras (extensible: add a kind + a row in aura_props, plus a server effect) -------
// A ground area-of-effect that applies a per-tick effect to whoever stands in it. New aura
// types only need: an enum value, a row here (radius/duration/colour/light), a case in the
// server's update_auras effect switch, and - since the client is data-driven - nothing else.
enum class AuraKind : u8 {
    Heal = 0,         // Cleric: heals allies inside
    Consecration = 1, // Knight: taunts + burns enemies inside
};

struct AuraProps {
    f32 radius = 4.0f;
    f32 duration = 6.0f;
    Vec3 color{1.0f}; // VFX + light tint
    f32 light = 0.0f; // night-time light intensity (0 = casts no light)
};

inline AuraProps aura_props(AuraKind kind) {
    switch (kind) {
        case AuraKind::Heal:
            return {kHealAuraRadius, kHealAuraDuration, Vec3{0.4f, 1.0f, 0.6f}, 1.6f};
        case AuraKind::Consecration:
            return {kConsecrationRadius, kConsecrationDuration, Vec3{1.0f, 0.6f, 0.22f}, 1.8f};
    }
    return {};
}

} // namespace alryn
