#pragma once

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <algorithm>
#include <cmath>

// SIDE QUESTS: work pinned to every town's NOTICE BOARD alongside the wagon contracts - something to
// do besides escorting a cart. Walk up to the board, pick one, and the party heads out into the wilds:
//   BANDIT CAMP  - a raider camp sits on a hill out there; burn it out (its warlord too).
//   WOLF HUNT    - a pack of dire wolves is savaging the herds; hunt them and their alpha down.
//   TREASURE     - an old map marks an X. DIG there (Q) - the earth really gives way - until the chest
//                  turns up, then crack it open (E). Grave-robbers may have had the same idea.
//   MOONPETALS   - an apothecary wants glowing moonpetals from a far meadow (E to pick); the wolves
//                  that den there don't like visitors.
// One quest runs at a time for the party (alongside any haul). Pure data + formulas here, like
// Contract.h; the simulation lives in Game/SideQuests.cpp (driven by GameServer).
namespace alryn {

enum class QuestKind : u8 {
    BanditCamp = 0,
    WolfHunt = 1,
    Treasure = 2,
    Herbs = 3,
};
inline constexpr u8 kQuestKinds = 4;

enum class QuestPhase : u8 {
    Offered = 0,  // pinned on the board
    Active = 1,   // the party took it - head out to the site
    Complete = 2, // done: a celebratory banner (the reward is paid) before the board refreshes
};

inline const char* quest_title(QuestKind k) {
    switch (k) {
        case QuestKind::BanditCamp: return "CLEAR THE BANDIT CAMP";
        case QuestKind::WolfHunt: return "WOLF HUNT";
        case QuestKind::Treasure: return "BURIED TREASURE";
        case QuestKind::Herbs: return "MOONPETAL HARVEST";
    }
    return "SIDE QUEST";
}
// The flavour line on the notice board.
inline const char* quest_blurb(QuestKind k) {
    switch (k) {
        case QuestKind::BanditCamp: return "Raiders have dug in on a hill outside town. Burn them out.";
        case QuestKind::WolfHunt: return "Dire wolves are savaging the herds. Hunt the pack and its alpha.";
        case QuestKind::Treasure: return "An old map marks an X in the wilds. Dig it up [Q] - finders keepers.";
        case QuestKind::Herbs: return "The apothecary pays well for moonpetals. Mind the wolves' den.";
    }
    return "";
}
// The HUD objective line while it's under way.
inline const char* quest_objective(QuestKind k) {
    switch (k) {
        case QuestKind::BanditCamp: return "SLAY THE RAIDERS AT THE CAMP";
        case QuestKind::WolfHunt: return "HUNT DOWN THE WOLF PACK";
        case QuestKind::Treasure: return "DIG AT THE X  [Q]";
        case QuestKind::Herbs: return "GATHER MOONPETALS  [E]";
    }
    return "";
}

// --- Tuning ------------------------------------------------------------------------------------
inline constexpr u8 kQuestOffers = 3;           // quests pinned on a town's board at once
inline constexpr f32 kQuestBoardRange = 3.6f;   // stand this close to the notice board to read it
inline constexpr f32 kQuestSiteMin = 75.0f;     // a site lies this far out from the town centre...
inline constexpr f32 kQuestSiteMax = 150.0f;    // ...up to this far
inline constexpr f32 kQuestWakeRange = 46.0f;   // a site's foes show themselves once a hero comes this close
inline constexpr f32 kQuestLeash = 24.0f;       // site guards don't chase further than this from their camp
inline constexpr f32 kQuestAggro = 15.0f;       // ...and only take notice of a hero this close
inline constexpr f32 kQuestBannerSeconds = 6.0f;
inline constexpr f32 kQuestSiteRadius = 14.0f;  // the camp / den / meadow footprint drawn on the map

// Treasure: spade strikes within reach of the X turn the chest up; it's opened with E.
inline constexpr f32 kTreasureDigReach = 2.4f;  // a dig this close to the X counts
inline constexpr u8 kTreasureDigs = 4;          // strikes to turn the chest up
inline constexpr f32 kChestOpenRange = 2.6f;
inline constexpr u8 kTreasureRobbers = 2;       // grave-robbers who come running when it's unearthed
// Moonpetals: glowing flowers scattered round the meadow, picked with E.
inline constexpr u8 kHerbCount = 5;
inline constexpr f32 kHerbScatter = 15.0f;
inline constexpr f32 kHerbPickRange = 2.2f;
inline constexpr u8 kHerbGuards = 2;            // wolves denned in the meadow
// The spade (Q, anywhere): a quick scoop of earth at the aim, within reach of the hero.
inline constexpr f32 kDigReach = 4.2f;
inline constexpr f32 kDigRadius = 1.5f;
inline constexpr f32 kDigAmount = 0.8f;
inline constexpr f32 kDigCooldown = 0.45f;

// How many foes hold a camp / den at this danger (1..3) - a warlord / alpha among them.
inline u8 quest_foe_count(QuestKind k, u8 danger) {
    const u8 d = std::clamp<u8>(danger, 1, 3);
    switch (k) {
        case QuestKind::BanditCamp: return static_cast<u8>(3 + 2 * d); // 5 / 7 / 9
        case QuestKind::WolfHunt: return static_cast<u8>(3 + d);       // 4 / 5 / 6
        case QuestKind::Treasure: return kTreasureRobbers;
        case QuestKind::Herbs: return kHerbGuards;
    }
    return 0;
}
// The goal the progress counts to: foes felled (camp / hunt), digs (treasure), petals (herbs).
inline u8 quest_goal(QuestKind k, u8 danger) {
    switch (k) {
        case QuestKind::BanditCamp:
        case QuestKind::WolfHunt: return quest_foe_count(k, danger);
        case QuestKind::Treasure: return kTreasureDigs;
        case QuestKind::Herbs: return kHerbCount;
    }
    return 1;
}
// Party-wallet payout: a base per kind, more for danger.
inline u32 quest_reward(QuestKind k, u8 danger) {
    const u32 d = std::clamp<u8>(danger, 1, 3);
    switch (k) {
        case QuestKind::BanditCamp: return 110u + 70u * d;
        case QuestKind::WolfHunt: return 90u + 55u * d;
        case QuestKind::Treasure: return 140u + 50u * d;
        case QuestKind::Herbs: return 80u + 35u * d;
    }
    return 100u;
}
// XP to every hero on completion.
inline u32 quest_xp(QuestKind k, u8 danger) {
    const u32 d = std::clamp<u8>(danger, 1, 3);
    return (k == QuestKind::BanditCamp ? 90u : k == QuestKind::WolfHunt ? 80u : 60u) + 30u * d;
}

} // namespace alryn
