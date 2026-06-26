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
    Mage = 3,   // damage: elemental combo-caster (Magicka-style), summons a rock wall
};

inline constexpr u8 kRoleCount = 4;
inline constexpr u8 kAbilitySlots = 4; // action-bar slots / hotkeys (1/2/3/4)
inline constexpr u8 kAbilityCount = 7; // abilities available per role (the skills tree, K)
                                       // index 6 is each role's co-op / team ability

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
        case PlayerRole::Mage: return {100.0f, 6.0f, 14.0f, 34.0f, 0.0f}; // fragile elemental DPS
    }
    return {};
}

inline const char* role_name(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return "KNIGHT";
        case PlayerRole::Hunter: return "HUNTER";
        case PlayerRole::Cleric: return "CLERIC";
        case PlayerRole::Mage: return "MAGE";
    }
    return "?";
}

// A one-line descriptor of a role's fantasy (shown in the skills tree header).
inline const char* role_desc(PlayerRole role) {
    switch (role) {
        case PlayerRole::Knight: return "TANK - GUARDS ALLIES AND SOAKS DAMAGE";
        case PlayerRole::Hunter: return "RANGER - FAST, FRAGILE, DEADLY AT RANGE";
        case PlayerRole::Cleric: return "HEALER - MENDS ALLIES AND SMITES FOES";
        case PlayerRole::Mage: return "MAGE - ELEMENTAL COMBO CASTER (HOLD CTRL)";
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
                case 5: return {"RALLY", 16.0f,
                                "Heal yourself and nearby allies and steel them briefly."};
                default: return {"GUARDIAN LEAP", 12.0f,
                                 "Leap to an ally, shield them, and pull their attackers onto you."};
            }
        case PlayerRole::Hunter:
            switch (ability) {
                case 0: return {"POWER SHOT", 3.0f, "A single heavy arrow for big damage."};
                case 1: return {"VOLLEY", 6.0f, "Loose a spread of three arrows at once."};
                case 2: return {"DASH", 5.0f, "A burst of speed to reposition or escape."};
                case 3: return {"PIERCING SHOT", 6.0f,
                                "One bolt that punches through for huge damage."};
                case 4: return {"MULTISHOT", 9.0f, "A wide fan of five arrows in one draw."};
                case 5: return {"CALTROPS", 12.0f,
                                "Scatter caltrops that wound enemies who cross them."};
                default: return {"WAR HORN", 14.0f,
                                 "Sound a horn that hastes you and nearby allies."};
            }
        case PlayerRole::Cleric:
            switch (ability) {
                case 0: return {"HEAL", 2.5f, "Mend the most injured ally in range."};
                case 1: return {"SANCTUARY", 11.0f, "Heal every ally standing nearby."};
                case 2: return {"SMITE", 4.0f, "Call down a holy bolt that hits foes hard."};
                case 3: return {"AEGIS", 9.0f, "Shield an ally, absorbing incoming damage."};
                case 4: return {"RENEW", 9.0f, "Lay a lingering heal aura at your feet."};
                case 5: return {"JUDGEMENT", 7.0f,
                                "A heavy holy bolt that scorches a single foe."};
                default: return {"EMPOWER", 12.0f,
                                 "Bless an ally so their attacks hit much harder for a while."};
            }
        case PlayerRole::Mage:
            // The Mage casts by COMBO, not the hotbar: hold CTRL and tap elements (keys 1-4 or
            // W/A/S/D), then release to cast. Slots 0-3 are the four elements you queue; 4-5 list
            // the signature spells. The recipe -> spell mapping is spell_for_combo() below.
            switch (ability) {
                case 0: return {"FIRE", 1.5f, "CTRL+queue Fire. x1 = Fireball, x2 = Meteor."};
                case 1: return {"WATER", 1.5f, "CTRL+queue Water. x1 = Frost Bolt (chills foes)."};
                case 2: return {"EARTH", 1.5f, "CTRL+queue Earth. x1 = Boulder, x3 = ROCK WALL."};
                case 3: return {"NATURE", 1.5f, "CTRL+queue Nature. x1 = Healing Bloom."};
                case 4: return {"ROCK WALL", 8.0f,
                                "EARTH x3: raise a wall of rock - NPCs must path around it."};
                case 5: return {"METEOR", 7.0f, "FIRE x2: call a fiery blast down on your aim."};
                default: return {"RUNE OF VIGOUR", 1.5f,
                                 "NATURE x2: empower nearby allies' attacks (x1 = Healing Bloom)."};
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

// --- Mage: Magicka-style elemental combo casting -----------------------------------------------
// The Mage holds CTRL and queues ELEMENTS (keys 1-4 = Fire/Water/Earth/Nature, or W/A/S/D), then
// releases to cast. The queued multiset maps to a spell via spell_for_combo(); the client resolves
// the spell id and sends it in PlayerInput.spell, and the server applies the authoritative effect.
enum class Element : u8 { Fire = 0, Water = 1, Earth = 2, Nature = 3 };
inline constexpr u8 kMaxCombo = 3; // elements you can queue per cast

enum class SpellId : u8 {
    None = 0,
    Fireball,  // Fire           - a fiery bolt that bursts on impact
    FrostBolt, // Water          - a chilling bolt (slows/chips)
    Boulder,   // Earth          - a heavy rock that knocks back
    Meteor,    // Fire + Fire    - a blazing impact at the aim point
    HealBloom, // Nature         - a burst of healing around the caster
    RockWall,  // Earth x3       - a wall of rock NPCs must path around
    Empower,   // Nature + Nature - boost nearby allies' attack damage (co-op)
};

// Recognise a queued combo (counts per element) -> a spell. Order-independent; priority picks the
// signature combos (wall / meteor) first, then the dominant single element.
inline SpellId spell_for_combo(int fire, int water, int earth, int nature) {
    if (earth >= 3) return SpellId::RockWall;
    if (fire >= 2) return SpellId::Meteor;
    if (nature >= 2 && fire == 0 && earth == 0 && water == 0) return SpellId::Empower; // co-op buff
    if (nature >= 1 && fire == 0 && earth == 0 && water == 0) return SpellId::HealBloom;
    if (earth >= 1) return SpellId::Boulder;
    if (water >= 1) return SpellId::FrostBolt;
    if (fire >= 1) return SpellId::Fireball;
    return SpellId::None;
}

inline const char* spell_name(SpellId s) {
    switch (s) {
        case SpellId::Fireball: return "FIREBALL";
        case SpellId::FrostBolt: return "FROST BOLT";
        case SpellId::Boulder: return "BOULDER";
        case SpellId::Meteor: return "METEOR";
        case SpellId::HealBloom: return "HEALING BLOOM";
        case SpellId::RockWall: return "ROCK WALL";
        case SpellId::Empower: return "EMPOWER";
        default: return "";
    }
}

inline f32 spell_cooldown(SpellId s) {
    switch (s) {
        case SpellId::Meteor: return 7.0f;
        case SpellId::RockWall: return 8.0f;
        case SpellId::Boulder: return 2.0f;
        case SpellId::HealBloom: return 4.0f;
        case SpellId::Empower: return 9.0f; // a strong team buff - not spammable
        default: return 1.2f;               // Fireball / Frost Bolt - quick chain-casting
    }
}

// Mage spell tuning (shared by the server + tests).
inline constexpr f32 kFireballDamage = 38.0f;
inline constexpr f32 kFireballRadius = 2.4f;   // small splash
inline constexpr f32 kFrostBoltDamage = 28.0f;
inline constexpr f32 kBoulderDamage = 52.0f;
inline constexpr f32 kBoulderKnockback = 3.0f;
inline constexpr f32 kMeteorDamage = 72.0f;
inline constexpr f32 kMeteorRadius = 4.5f;
inline constexpr f32 kHealBloomAmount = 38.0f;
inline constexpr f32 kHealBloomRadius = 9.0f;
// --- Co-op / team buffs (the inter-player abilities) -------------------------------------------
inline constexpr f32 kDamageBoostMult = 1.5f;     // Empower: x outgoing damage while active
inline constexpr f32 kDamageBoostDuration = 8.0f;
// PERFECT DODGE: evading a hit with the roll's i-frames briefly grants the same outgoing-damage boost
// (dodge into danger -> hit harder). Kept SHORT - it refreshes on each evaded hit, but the roll's
// cooldown caps the i-frame uptime, so it can't be held up by spamming dodge.
inline constexpr f32 kPerfectDodgeBuff = 1.6f;    // seconds of damage boost on a perfect dodge
inline constexpr f32 kEmpowerRange = 18.0f;       // Cleric Empower: how far an ally can be blessed
inline constexpr f32 kHasteMult = 1.4f;           // War Horn: x walk speed while active
inline constexpr f32 kHasteDuration = 6.0f;
inline constexpr f32 kHasteRadius = 12.0f;        // allies near the Hunter get hasted
inline constexpr f32 kGuardLeapRange = 24.0f;     // Knight Guardian Leap: reach to an ally
inline constexpr f32 kGuardShieldAmount = 45.0f;  // shield placed on the leapt-to ally
inline constexpr f32 kGuardTauntRadius = 10.0f;   // enemies near the ally pulled onto the Knight

// Rock wall: a row of stone raised a few metres ahead of the caster; a collider NPCs route around.
inline constexpr f32 kRockWallLength = 7.0f;   // total span (perpendicular to facing)
inline constexpr f32 kRockWallThick = 1.4f;
inline constexpr f32 kRockWallHeight = 2.6f;
inline constexpr f32 kRockWallAhead = 3.2f;    // metres in front of the caster
inline constexpr f32 kRockWallTtl = 22.0f;     // seconds before it crumbles
inline constexpr f32 kRockWallHealth = 320.0f; // enemies can smash through it

} // namespace alryn
