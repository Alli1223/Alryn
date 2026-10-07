#pragma once

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Game/Roles.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace alryn {

// Character PROGRESSION: experience + levels, the per-role SKILL TREE (a small starter kit, with the
// rest of the kit unlocked tier by tier with skill points), passive TALENTS, the JOURNEY (a linear
// chain of goals that walks a new hero through the game) and the per-player IDENTITY COLOURS. Pure
// data + maths, like Roles.h, so the server (authoritative XP / unlocks), the client (skill tree UI,
// HUD, saved heroes) and the tests all share one source of truth.

// --- Player identity colours ------------------------------------------------------------------
// Every connected player gets their own bright colour (first free one, or their preferred pick if no
// one else has it) - drawn as the ring at their feet, their name plate, party frame and map pin, so a
// glance tells you who is who in a brawl.
inline constexpr u8 kPlayerColorCount = 8;
inline constexpr u8 kNoColor = 255; // PlayerInput.color_pref: "no preference"

inline const std::array<Vec3, kPlayerColorCount>& player_colors() {
    static const std::array<Vec3, kPlayerColorCount> colors = {
        Vec3{0.95f, 0.30f, 0.26f}, // red
        Vec3{0.30f, 0.58f, 1.00f}, // blue
        Vec3{0.36f, 0.88f, 0.38f}, // green
        Vec3{1.00f, 0.84f, 0.22f}, // yellow
        Vec3{0.78f, 0.42f, 1.00f}, // purple
        Vec3{1.00f, 0.56f, 0.16f}, // orange
        Vec3{0.24f, 0.90f, 0.90f}, // cyan
        Vec3{1.00f, 0.46f, 0.76f}, // pink
    };
    return colors;
}
inline Vec3 player_color(u8 index) { return player_colors()[index % kPlayerColorCount]; }
inline const char* player_color_name(u8 index) {
    static const char* names[kPlayerColorCount] = {"RED",    "BLUE",   "GREEN", "YELLOW",
                                                   "PURPLE", "ORANGE", "CYAN",  "PINK"};
    return names[index % kPlayerColorCount];
}

// Display names: short, printable, safe to draw. Anything else is dropped; an empty name falls back.
inline constexpr usize kMaxNameLength = 16;
inline bool name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
           c == '-' || c == '_' || c == '\'';
}

// --- Experience + levels ----------------------------------------------------------------------
inline constexpr u8 kMaxLevel = 12;

// XP needed to go from `level` to `level + 1` (a gentle ramp: 100, 160, 220, ...).
inline u32 xp_step(u8 level) { return 100u + 60u * static_cast<u32>(std::max<u8>(level, 1) - 1u); }

// Total XP needed to REACH `level` (level 1 = 0).
inline u32 xp_to_reach(u8 level) {
    u32 total = 0;
    for (u8 l = 1; l < std::min<u8>(level, kMaxLevel); ++l) {
        total += xp_step(l);
    }
    return total;
}
inline u32 max_xp() { return xp_to_reach(kMaxLevel); }

inline u8 level_for_xp(u32 xp) {
    u8 level = 1;
    while (level < kMaxLevel && xp >= xp_to_reach(static_cast<u8>(level + 1))) {
        ++level;
    }
    return level;
}

// Progress (0..1) through the current level, for the XP bar. 1 at max level.
inline f32 level_progress(u32 xp) {
    const u8 level = level_for_xp(xp);
    if (level >= kMaxLevel) {
        return 1.0f;
    }
    const u32 lo = xp_to_reach(level);
    return static_cast<f32>(xp - lo) / static_cast<f32>(xp_step(level));
}

// Skill points earned by `level`: one per level gained.
inline u8 skill_points_for_level(u8 level) { return static_cast<u8>(std::max<u8>(level, 1) - 1u); }

// Every level makes a hero a little sturdier and hit a little harder (on top of gear + talents), so
// levelling is felt even before a new skill is learned. Level 1 = exactly 1.0 (the old balance).
inline f32 level_health_mult(u8 level) { return 1.0f + 0.04f * static_cast<f32>(std::max<u8>(level, 1) - 1u); }
inline f32 level_damage_mult(u8 level) { return 1.0f + 0.03f * static_cast<f32>(std::max<u8>(level, 1) - 1u); }

// XP for felling a raider (scales with how tough it is) - shared by every player near the fight.
inline u32 enemy_xp(u8 kind) {
    return static_cast<u32>(std::max(6.0f, std::round(enemy_max_health(kind) * 0.25f)));
}
inline constexpr f32 kXpShareRadius = 45.0f; // players this close to a felled raider share its XP

// XP for delivering a contract: a base, plus danger, plus the length of the road.
inline u32 delivery_xp(u8 difficulty, f32 route_length) {
    return 60u + 40u * static_cast<u32>(difficulty) +
           static_cast<u32>(std::clamp(route_length, 0.0f, 900.0f) / 10.0f);
}

// --- The skill tree ---------------------------------------------------------------------------
// Each role's seven abilities hang in a small tree of four tiers. Tier 0 is the STARTER kit (known
// from the start, free); every other node costs one skill point, needs a character level and - except
// at the roots - its parent node learned first. `column` (0/1) lays the node out in the tree UI.
struct SkillNode {
    u8 tier = 0;        // 0 = starter .. 3
    u8 req_level = 1;   // character level needed to learn it
    i8 prereq = -1;     // ability index that must be known first (-1 = none)
    u8 column = 0;      // 0 = left branch, 1 = right branch
};
inline constexpr u8 kSkillTiers = 4;
inline constexpr u8 kTierLevel[kSkillTiers] = {1, 2, 4, 6};

inline SkillNode skill_node(PlayerRole role, u8 ability) {
    // {tier, req_level, prereq, column}
    switch (role) {
        case PlayerRole::Knight: // Bash / Taunt -> Bulwark / Whirlwind -> Rally / Consecration -> Leap
            switch (ability) {
                case 0: return {0, 1, -1, 0}; // Shield Bash
                case 3: return {0, 1, -1, 1}; // Taunt
                case 1: return {1, 2, 0, 0};  // Bulwark      <- Shield Bash
                case 4: return {1, 2, 3, 1};  // Whirlwind    <- Taunt
                case 5: return {2, 4, 1, 0};  // Rally        <- Bulwark
                case 2: return {2, 4, 4, 1};  // Consecration <- Whirlwind
                default: return {3, 6, 5, 0}; // Guardian Leap <- Rally
            }
        case PlayerRole::Hunter: // Power Shot / Dash -> Volley / Caltrops -> Piercing / Multishot -> Horn
            switch (ability) {
                case 0: return {0, 1, -1, 0}; // Power Shot
                case 2: return {0, 1, -1, 1}; // Dash
                case 1: return {1, 2, 0, 0};  // Volley       <- Power Shot
                case 5: return {1, 2, 2, 1};  // Caltrops     <- Dash
                case 3: return {2, 4, 1, 0};  // Piercing     <- Volley
                case 4: return {2, 4, 1, 1};  // Multishot    <- Volley
                default: return {3, 6, 5, 1}; // War Horn     <- Caltrops
            }
        case PlayerRole::Cleric: // Heal / Smite -> Sanctuary / Judgement -> Aegis / Renew -> Empower
            switch (ability) {
                case 0: return {0, 1, -1, 0}; // Heal
                case 2: return {0, 1, -1, 1}; // Smite
                case 1: return {1, 2, 0, 0};  // Sanctuary    <- Heal
                case 5: return {1, 2, 2, 1};  // Judgement    <- Smite
                case 3: return {2, 4, 0, 0};  // Aegis        <- Heal
                case 4: return {2, 4, 1, 1};  // Renew        <- Sanctuary
                default: return {3, 6, 3, 0}; // Empower      <- Aegis
            }
        case PlayerRole::Mage: // Fire / Water -> Earth / Nature -> Meteor / Rock Wall -> Vigour
            switch (ability) {
                case 0: return {0, 1, -1, 0}; // Fire
                case 1: return {0, 1, -1, 1}; // Water
                case 2: return {1, 2, -1, 1}; // Earth
                case 3: return {1, 2, -1, 0}; // Nature
                case 5: return {2, 4, 0, 0};  // Meteor       <- Fire
                case 4: return {2, 4, 2, 1};  // Rock Wall    <- Earth
                default: return {3, 6, 3, 0}; // Rune of Vigour <- Nature
            }
    }
    return {};
}

// The abilities a fresh hero of `role` already knows (tier 0), as a bitmask over ability indices.
inline u8 starter_mask(PlayerRole role) {
    u8 m = 0;
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (skill_node(role, a).tier == 0) {
            m = static_cast<u8>(m | (1u << a));
        }
    }
    return m;
}
inline constexpr u8 kAllAbilities = static_cast<u8>((1u << kAbilityCount) - 1u);
inline bool knows(u8 known_mask, u8 ability) { return ability < kAbilityCount && (known_mask >> ability) & 1u; }

// --- Talents: passive nodes, three per hero, three ranks each ---------------------------------
inline constexpr u8 kTalentCount = 3;
inline constexpr u8 kMaxTalentRank = 3;
enum class Talent : u8 { Vitality = 0, Might = 1, Focus = 2 };

inline const char* talent_name(u8 t) {
    switch (t) {
        case 0: return "VITALITY";
        case 1: return "MIGHT";
        default: return "FOCUS";
    }
}
inline const char* talent_desc(u8 t) {
    switch (t) {
        case 0: return "+8% maximum health per rank.";
        case 1: return "+7% damage dealt per rank.";
        default: return "-7% ability and spell cooldowns per rank.";
    }
}
// Character level needed to buy a talent's `rank` (1..3).
inline u8 talent_req_level(u8 rank) { return rank <= 1 ? 2 : (rank == 2 ? 5 : 8); }

// Talents ride packed 2 bits each (talent t in bits 2t..2t+1).
inline u8 talent_rank(u8 packed, u8 t) { return static_cast<u8>((packed >> (2 * t)) & 0x3u); }
inline u8 with_talent_rank(u8 packed, u8 t, u8 rank) {
    const u8 shift = static_cast<u8>(2 * t);
    return static_cast<u8>((packed & ~(0x3u << shift)) | ((std::min<u8>(rank, kMaxTalentRank) & 0x3u) << shift));
}
inline u8 talent_points(u8 packed) {
    return static_cast<u8>(talent_rank(packed, 0) + talent_rank(packed, 1) + talent_rank(packed, 2));
}
inline f32 talent_health_mult(u8 packed) { return 1.0f + 0.08f * static_cast<f32>(talent_rank(packed, 0)); }
inline f32 talent_damage_mult(u8 packed) { return 1.0f + 0.07f * static_cast<f32>(talent_rank(packed, 1)); }
inline f32 talent_cooldown_mult(u8 packed) { return 1.0f - 0.07f * static_cast<f32>(talent_rank(packed, 2)); }

// Skill points spent on a hero's learned abilities (beyond the free starters) + talents.
inline u8 points_spent(PlayerRole role, u8 known_mask, u8 talents) {
    const u8 learned = static_cast<u8>(known_mask & ~starter_mask(role) & kAllAbilities);
    return static_cast<u8>(std::popcount(learned) + talent_points(talents));
}
inline i32 points_available(PlayerRole role, u8 level, u8 known_mask, u8 talents) {
    return static_cast<i32>(skill_points_for_level(level)) -
           static_cast<i32>(points_spent(role, known_mask, talents));
}

// Can a hero at `level` (knowing `known_mask`, with `talents`) learn `ability` right now?
inline bool can_learn(PlayerRole role, u8 ability, u8 level, u8 known_mask, u8 talents) {
    if (ability >= kAbilityCount || knows(known_mask, ability)) {
        return false;
    }
    const SkillNode n = skill_node(role, ability);
    if (level < n.req_level) {
        return false;
    }
    if (n.prereq >= 0 && !knows(known_mask, static_cast<u8>(n.prereq))) {
        return false;
    }
    return points_available(role, level, known_mask, talents) > 0;
}
inline bool can_raise_talent(PlayerRole role, u8 talent, u8 level, u8 known_mask, u8 talents) {
    if (talent >= kTalentCount) {
        return false;
    }
    const u8 rank = talent_rank(talents, talent);
    return rank < kMaxTalentRank && level >= talent_req_level(static_cast<u8>(rank + 1)) &&
           points_available(role, level, known_mask, talents) > 0;
}

// A saved/claimed loadout is only honoured if it could really have been earned: every learned node's
// level + parent requirement holds and the points add up. Anything else falls back to the starters.
inline bool loadout_valid(PlayerRole role, u8 level, u8 known_mask, u8 talents) {
    known_mask = static_cast<u8>(known_mask | starter_mask(role));
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (!knows(known_mask, a)) {
            continue;
        }
        const SkillNode n = skill_node(role, a);
        if (level < n.req_level || (n.prereq >= 0 && !knows(known_mask, static_cast<u8>(n.prereq)))) {
            return false;
        }
    }
    for (u8 t = 0; t < kTalentCount; ++t) {
        const u8 r = talent_rank(talents, t);
        if (r > 0 && level < talent_req_level(r)) {
            return false;
        }
    }
    return points_available(role, level, known_mask, talents) >= 0;
}

// The ability index a Mage spell needs learned (its element, or the signature combo itself).
inline u8 spell_required_ability(SpellId s) {
    switch (s) {
        case SpellId::Fireball: return 0;  // Fire
        case SpellId::FrostBolt: return 1; // Water
        case SpellId::Boulder: return 2;   // Earth
        case SpellId::HealBloom: return 3; // Nature
        case SpellId::RockWall: return 4;
        case SpellId::Meteor: return 5;
        case SpellId::Empower: return 6;   // Rune of Vigour
        default: return 0;
    }
}

// spell_for_combo restricted to what the Mage has learned: a signature combo that isn't learned yet
// falls back to the plain element bolt (FIRE x2 without Meteor is just a Fireball), and an element
// that isn't learned contributes nothing.
inline SpellId spell_for_combo_known(int fire, int water, int earth, int nature, u8 known_mask) {
    if (!knows(known_mask, 0)) fire = 0;
    if (!knows(known_mask, 1)) water = 0;
    if (!knows(known_mask, 2)) earth = 0;
    if (!knows(known_mask, 3)) nature = 0;
    if (earth >= 3 && !knows(known_mask, 4)) earth = 1;   // no Rock Wall yet -> a Boulder
    if (fire >= 2 && !knows(known_mask, 5)) fire = 1;     // no Meteor yet -> a Fireball
    if (nature >= 2 && !knows(known_mask, 6)) nature = 1; // no Vigour yet -> a Healing Bloom
    return spell_for_combo(fire, water, earth, nature);
}

// --- The Journey: a linear chain of goals for a new hero ---------------------------------------
// Steps advance in order (server-checked each tick against the hero's lifetime stats), each paying an
// XP reward - the "what do I do next?" spine that walks a fresh hero from their first contract board
// to the most perilous roads. The client shows the current step on the objective card.
enum class JourneyGoal : u8 {
    ReadContract = 0, // walk up to a wagon on the contract board
    SetOut,           // accept a contract and leave town
    Deliver,          // deliver a wagon
    DefeatRaiders,    // fell raiders on the road (lifetime count)
    LearnSkill,       // spend a skill point (K)
    BuyGear,          // buy a gear tier in town (U)
    Danger2,          // deliver a danger 2+ contract
    Hone,             // raise an ability rank in a town (K)
    Level6,           // reach level 6
    Danger3,          // deliver a danger 3 contract
    Level10,          // reach level 10
    Done,
};
inline constexpr u8 kJourneySteps = static_cast<u8>(JourneyGoal::Done);
inline constexpr u16 kJourneyRaiders = 5; // DefeatRaiders target

struct JourneyStep {
    const char* title = "";
    const char* hint = "";
    u32 xp = 0;      // reward on completion
    u16 target = 0;  // count goal shown as "n / target" (0 = a one-off)
};

inline JourneyStep journey_step(u8 step) {
    switch (static_cast<JourneyGoal>(std::min<u8>(step, kJourneySteps))) {
        case JourneyGoal::ReadContract:
            return {"FIND WORK", "WALK UP TO A WAGON ON THE MARKET'S CONTRACT BOARD", 40, 0};
        case JourneyGoal::SetOut:
            return {"HIT THE ROAD", "ACCEPT A CONTRACT AND ESCORT THE WAGON OUT OF TOWN", 40, 0};
        case JourneyGoal::Deliver:
            return {"SAFE ARRIVAL", "DELIVER THE WAGON TO ITS DESTINATION", 90, 0};
        case JourneyGoal::DefeatRaiders:
            return {"ROAD WARDEN", "DEFEAT RAIDERS WHO AMBUSH YOUR WAGONS", 80, kJourneyRaiders};
        case JourneyGoal::LearnSkill:
            return {"A NEW TECHNIQUE", "OPEN THE SKILL TREE [K] AND LEARN A NEW SKILL", 60, 0};
        case JourneyGoal::BuyGear:
            return {"DRESSED FOR THE ROAD", "BUY BETTER GEAR IN A TOWN [U]", 70, 0};
        case JourneyGoal::Danger2:
            return {"DANGEROUS ROADS", "DELIVER A CONTRACT OF DANGER 2 OR MORE", 140, 0};
        case JourneyGoal::Hone:
            return {"HONED", "UPGRADE AN ABILITY'S RANK IN A TOWN [K]", 100, 0};
        case JourneyGoal::Level6:
            return {"VETERAN", "REACH LEVEL 6", 120, 0};
        case JourneyGoal::Danger3:
            return {"PERILOUS ROADS", "DELIVER A DANGER 3 CONTRACT", 220, 0};
        case JourneyGoal::Level10:
            return {"LEGEND OF THE ROADS", "REACH LEVEL 10", 300, 0};
        case JourneyGoal::Done:
            break;
    }
    return {"THE ROAD GOES ON", "EVERY ROAD IS YOURS - TAKE ANY CONTRACT", 0, 0};
}

// The hero's lifetime record that the journey (and the saved hero) tracks.
struct HeroRecord {
    u8 level = 1;
    u16 kills = 0;          // raiders felled (shared credit)
    u16 deliveries = 0;     // wagons delivered
    u8 best_danger = 0;     // the highest danger delivered
    bool learned = false;   // has spent a skill point
    bool geared = false;    // owns a bought gear tier
    bool honed = false;     // has raised an ability rank
    bool read_contract = false; // stood at an offered wagon
    bool set_out = false;   // a contract is under way
};

// Is journey step `step` satisfied by `r`? (Pure, so the server + tests agree.)
inline bool journey_met(u8 step, const HeroRecord& r) {
    switch (static_cast<JourneyGoal>(step)) {
        case JourneyGoal::ReadContract: return r.read_contract || r.set_out || r.deliveries > 0;
        case JourneyGoal::SetOut: return r.set_out || r.deliveries > 0;
        case JourneyGoal::Deliver: return r.deliveries > 0;
        case JourneyGoal::DefeatRaiders: return r.kills >= kJourneyRaiders;
        case JourneyGoal::LearnSkill: return r.learned;
        case JourneyGoal::BuyGear: return r.geared;
        case JourneyGoal::Danger2: return r.best_danger >= 2;
        case JourneyGoal::Hone: return r.honed;
        case JourneyGoal::Level6: return r.level >= 6;
        case JourneyGoal::Danger3: return r.best_danger >= 3;
        case JourneyGoal::Level10: return r.level >= 10;
        case JourneyGoal::Done: return false;
    }
    return false;
}

// Progress toward a counted step (for "n / target"), else 0.
inline u16 journey_count(u8 step, const HeroRecord& r) {
    return static_cast<JourneyGoal>(step) == JourneyGoal::DefeatRaiders
               ? std::min<u16>(r.kills, kJourneyRaiders)
               : 0;
}

// --- Level-gated contract danger --------------------------------------------------------------
// With progression on, a green party isn't sent down the deadliest roads: a contract's danger (the
// ambush size) is capped by the party's best level - so the same routes grow more dangerous, and more
// lucrative, as the heroes grow. The linear spine of the campaign.
inline u8 max_danger_for_level(u8 level) { return level >= 6 ? 3 : (level >= 3 ? 2 : 1); }

} // namespace alryn
