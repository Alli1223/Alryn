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
inline constexpr u8 kAbilitySlots = 4; // action-bar slots / hotkeys (1/2/3/4)
inline constexpr u8 kAbilityCount = 6; // abilities available per role (the skills tree, K)

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

// A one-line descriptor of a role's fantasy (shown in the skills tree header).
inline const char* role_desc(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return "TANK - GUARDS ALLIES AND SOAKS DAMAGE";
        case PlayerRole::Hunter: return "RANGER - FAST, FRAGILE, DEADLY AT RANGE";
        case PlayerRole::Cleric: return "HEALER - MENDS ALLIES AND SMITES FOES";
    }
    return "";
}

// A castable ability: a display name, its cooldown (seconds), and a one-line
// description (shown in the skills tree opened with K). Slots map to keys 1/2/3/4.
struct AbilityDef {
    const char* name = "";
    f32 cooldown = 1.0f;
    const char* desc = "";
};

// The abilities of a role, indexed 0..kAbilityCount-1. The first four were the original
// kit; indices 4+ are the expanded skills shown in the tree (K). A player equips any
// kAbilitySlots of them onto the action bar (keys 1/2/3/4). The wire carries the ability
// INDEX (not the bar slot), so the server resolves the effect regardless of bar order.
inline AbilityDef ability_def(PlayerRole role, u8 ability) {
    switch (role) {
        case PlayerRole::Knight:
            switch (ability) {
                case 0: return {"SHIELD BASH", 4.0f,
                                "Frontal cone burst that damages and knocks foes back."};
                case 1: return {"BULWARK", 12.0f,
                                "Brace for heavy damage reduction for a few seconds."};
                case 2: return {"CONSECRATION", 12.0f,
                                "Holy ground that taunts and burns enemies within."};
                case 3: return {"TAUNT", 8.0f,
                                "Force nearby enemies to fixate their aggro on you."};
                case 4: return {"WHIRLWIND", 7.0f,
                                "A spinning cleave that hits every enemy around you."};
                default: return {"RALLY", 16.0f,
                                 "Heal yourself and nearby allies and steel them briefly."};
            }
        case PlayerRole::Hunter:
            switch (ability) {
                case 0: return {"POWER SHOT", 3.0f, "A single heavy arrow for big damage."};
                case 1: return {"VOLLEY", 6.0f, "Loose a spread of three arrows at once."};
                case 2: return {"DASH", 5.0f, "A burst of speed to reposition or escape."};
                case 3: return {"PIERCING SHOT", 6.0f,
                                "One bolt that punches through for huge damage."};
                case 4: return {"MULTISHOT", 9.0f, "A wide fan of five arrows in one draw."};
                default: return {"CALTROPS", 12.0f,
                                 "Scatter caltrops that wound enemies who cross them."};
            }
        case PlayerRole::Cleric:
            switch (ability) {
                case 0: return {"HEAL", 2.5f, "Mend the most injured ally in range."};
                case 1: return {"SANCTUARY", 11.0f, "Heal every ally standing nearby."};
                case 2: return {"SMITE", 4.0f, "Call down a holy bolt that hits foes hard."};
                case 3: return {"AEGIS", 9.0f, "Shield an ally, absorbing incoming damage."};
                case 4: return {"RENEW", 9.0f, "Lay a lingering heal aura at your feet."};
                default: return {"JUDGEMENT", 7.0f,
                                 "A heavy holy bolt that scorches a single foe."};
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

// --- Expanded abilities (skills tree indices 4+) ----------------------------------------
// Knight Whirlwind: a 360-degree cleave (no cone) around the knight, with a light shove.
inline constexpr f32 kWhirlwindDamage = 38.0f;
inline constexpr f32 kWhirlwindRadius = 3.4f;
inline constexpr f32 kWhirlwindKnockback = 1.6f;
// Knight Rally: heal self + nearby allies (within kHealRadius) and grant a brief bulwark.
inline constexpr f32 kRallyHeal = 40.0f;
// Hunter Multishot: a wide arrow fan (each arrow scaled from ranged damage).
inline constexpr int kMultishotArrows = 5;
inline constexpr f32 kMultishotMult = 0.8f;
// Cleric Judgement: a heavy single holy bolt.
inline constexpr f32 kJudgementDamage = 95.0f;
// Hazard ground aura (Hunter Caltrops): wounds enemies standing in it.
inline constexpr f32 kHazardRadius = 3.6f;
inline constexpr f32 kHazardDuration = 6.0f;
inline constexpr f32 kHazardDPS = 20.0f;

// --- Ground auras (extensible: add a kind + a row in aura_props, plus a server effect) -------
// A ground area-of-effect that applies a per-tick effect to whoever stands in it. New aura
// types only need: an enum value, a row here (radius/duration/colour/light), a case in the
// server's update_auras effect switch, and - since the client is data-driven - nothing else.
enum class AuraKind : u8 {
    Heal = 0,         // Cleric: heals allies inside
    Consecration = 1, // Knight: taunts + burns enemies inside
    Hazard = 2,       // Hunter (Caltrops): wounds enemies inside (no taunt)
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
        case AuraKind::Hazard:
            return {kHazardRadius, kHazardDuration, Vec3{0.85f, 0.78f, 0.3f}, 1.2f};
    }
    return {};
}

} // namespace alryn
