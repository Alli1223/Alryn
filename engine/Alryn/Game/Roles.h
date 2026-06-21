#pragma once

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
inline constexpr u8 kAbilitySlots = 3; // abilities per role (keys 1/2/3)

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
                default: return {"TAUNT", 10.0f};
            }
        case PlayerRole::Hunter:
            switch (slot) {
                case 0: return {"POWER SHOT", 3.0f};
                case 1: return {"VOLLEY", 6.0f};
                default: return {"DASH", 5.0f};
            }
        case PlayerRole::Cleric:
            switch (slot) {
                case 0: return {"HEAL", 2.5f};
                case 1: return {"SANCTUARY", 11.0f};
                default: return {"SMITE", 4.0f};
            }
    }
    return {};
}

// --- Ability tuning (shared by the server and the headless tests) -----------------
inline constexpr f32 kBashDamage = 55.0f;       // Knight shield bash, plus knockback
inline constexpr f32 kBashKnockback = 3.5f;     // metres an enemy is shoved
inline constexpr f32 kBulwarkReduction = 0.55f; // extra damage reduction while raised
inline constexpr f32 kBulwarkDuration = 5.0f;
inline constexpr f32 kTauntRadius = 12.0f;
inline constexpr f32 kTauntDuration = 5.0f;
inline constexpr f32 kPowerShotMult = 2.4f;     // x the Hunter's ranged damage
inline constexpr f32 kVolleyMult = 0.9f;        // per arrow of the 3-arrow spread
inline constexpr f32 kDashSpeedMult = 1.7f;     // walk-speed multiplier while dashing
inline constexpr f32 kDashDuration = 1.6f;
inline constexpr f32 kHealAmount = 45.0f;       // Cleric single-target mend
inline constexpr f32 kSanctuaryAmount = 30.0f;  // AoE heal to all nearby allies
inline constexpr f32 kHealRadius = 16.0f;
inline constexpr f32 kSmiteDamage = 60.0f;      // Cleric holy bolt

} // namespace alryn
