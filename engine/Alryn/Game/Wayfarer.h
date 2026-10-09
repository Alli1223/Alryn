#pragma once

#include <Alryn/Core/Types.h>

#include <algorithm>

// LIFE ON THE ROADS between the towns (the simulation lives in Game/Wayfarers.cpp, driven by GameServer):
//   WAYFARERS - folk walking the roads from town to town: a lone traveller with a pack + staff, a pilgrim
//               (lantern in hand by night), a band of adventurers, or a MERCHANT CARAVAN - a cart drawn by
//               an ox or a horse, its merchant at the beast's head and a guard or two alongside. They're
//               spawned out of sight ahead of / behind the party on the road it's travelling and walk on to
//               their destination, so the long roads between the (now far-flung) towns feel travelled.
//   ERRANDS   - every so often, on the road, a traveller in trouble waits by the wayside with a small job
//               for the party (talk to them - E - to take it on):
//                 LOST GOAT     - their goat bolted into the wilds: find it (E) and lead it back to them.
//                 BRIGANDS      - robbed on the road: the cutthroats are camped just off it. Drive them off.
//                 STUCK CART    - their cart's wheel has sunk in the mud: dig it free (the spade, Q).
//                 LOST PARCELS  - their satchels fell off the cart back along the road: gather them (E).
//                 WOLVES        - wolves have been stalking them: see the pack off.
//               Done, they pay what they can (the party's wallet) and every hero gains a little XP.
// Pure data + formulas here, like SideQuest.h.
namespace alryn {

enum class WayfarerRole : u8 {
    Traveller = 0,  // pack on the back, a walking staff
    Pilgrim = 1,    // a hooded robe + staff; carries a lantern by night
    Merchant = 2,   // leads a caravan's beast by the halter
    Guard = 3,      // a caravan's hired spear
    Adventurer = 4, // one of a band of sellswords on the road
};

enum class ErrandKind : u8 {
    LostGoat = 0,
    Brigands = 1,
    StuckCart = 2,
    LostParcels = 3,
    Wolves = 4,
};
inline constexpr u8 kErrandKinds = 5;

inline const char* errand_title(ErrandKind k) {
    switch (k) {
        case ErrandKind::LostGoat: return "THE RUNAWAY GOAT";
        case ErrandKind::Brigands: return "ROBBED ON THE ROAD";
        case ErrandKind::StuckCart: return "STUCK IN THE MUD";
        case ErrandKind::LostParcels: return "SPILLED SATCHELS";
        case ErrandKind::Wolves: return "STALKED BY WOLVES";
    }
    return "AN ERRAND";
}
// What the traveller says when the party walks up.
inline const char* errand_plea(ErrandKind k) {
    switch (k) {
        case ErrandKind::LostGoat: return "My goat bolted into the trees! Could you find her and lead her back?";
        case ErrandKind::Brigands: return "Brigands took my purse! They're camped just off the road - drive them off!";
        case ErrandKind::StuckCart: return "My cart's sunk to the axle. Could you dig the wheel free? I'll pay.";
        case ErrandKind::LostParcels: return "My satchels fell off the cart back along the road. Gather them for me?";
        case ErrandKind::Wolves: return "Wolves have trailed me for a mile. Please - see them off!";
    }
    return "";
}
// The HUD objective line while it's under way.
inline const char* errand_objective(ErrandKind k) {
    switch (k) {
        case ErrandKind::LostGoat: return "FIND THE GOAT [E] AND LEAD HER BACK";
        case ErrandKind::Brigands: return "DRIVE OFF THE BRIGANDS";
        case ErrandKind::StuckCart: return "DIG THE WHEEL FREE  [Q]";
        case ErrandKind::LostParcels: return "GATHER THE SATCHELS  [E]";
        case ErrandKind::Wolves: return "SEE OFF THE WOLVES";
    }
    return "";
}
// The thanks when it's done.
inline const char* errand_thanks(ErrandKind k) {
    switch (k) {
        case ErrandKind::LostGoat: return "Oh, there's my girl! Bless you.";
        case ErrandKind::Brigands: return "My purse! I owe you a share of it.";
        case ErrandKind::StuckCart: return "Free at last! Safe roads to you.";
        case ErrandKind::LostParcels: return "Every last one! Thank you, friends.";
        case ErrandKind::Wolves: return "They're gone... I can breathe again.";
    }
    return "Thank you!";
}

// --- Tuning ------------------------------------------------------------------------------------
// Wayfarers.
inline constexpr f32 kWayfarerSpeed = 1.35f;      // a steady walking pace
inline constexpr f32 kCaravanSpeed = 1.55f;       // the plodding beast + cart
inline constexpr f32 kWayfarerHurry = 2.3f;       // the pace multiplier when raiders are about
inline constexpr f32 kWayfarerSpawnMin = 75.0f;   // spawned this far (along the road) from the party...
inline constexpr f32 kWayfarerSpawnMax = 135.0f;  // ...up to this far - out of sight
inline constexpr f32 kWayfarerClear = 55.0f;      // never spawned within this (straight line) of a hero
inline constexpr f32 kWayfarerDespawn = 240.0f;   // gone once every hero is further than this
inline constexpr usize kMaxWayfarers = 28;        // walkers on the roads round the party at once
inline constexpr usize kMaxCaravans = 3;
inline constexpr int kWayfarerGroupsNear = 3;     // groups kept round each hero on the road
inline constexpr f32 kWayfarerScan = 2.5f;        // seconds between spawn checks
inline constexpr f32 kWayfarerKeepRight = 1.25f;  // how far off the centreline they walk (their right)

// Errands.
inline constexpr f32 kErrandCooldownMin = 45.0f;  // seconds of travel between one errand and the next...
inline constexpr f32 kErrandCooldownMax = 95.0f;  // ...up to this
inline constexpr f32 kErrandFirstDelay = 25.0f;   // the first one turns up a little way into the journey
inline constexpr f32 kErrandAheadMin = 48.0f;     // the traveller waits this far up the road...
inline constexpr f32 kErrandAheadMax = 70.0f;
inline constexpr f32 kErrandTalkRange = 3.6f;     // stand this close to take the errand on
inline constexpr f32 kErrandForget = 170.0f;      // an offer the party walked away from is withdrawn
inline constexpr f32 kErrandAbandon = 240.0f;     // an errand the party left far behind is dropped
inline constexpr f32 kErrandThanksSeconds = 9.0f; // the thanks + pay banner
inline constexpr f32 kGoatFollow = 1.7f;          // a led goat trots this far behind her new friend
inline constexpr f32 kGoatPickRange = 2.4f;
inline constexpr f32 kGoatHomeRange = 3.8f;       // bring her this close to her owner
inline constexpr u8 kParcelCount = 3;
inline constexpr f32 kParcelPickRange = 2.3f;
inline constexpr u8 kStuckDigs = 3;               // spade strikes to free the wheel
inline constexpr f32 kStuckDigReach = 3.4f;

inline u8 errand_goal(ErrandKind k, u8 danger) {
    const u8 d = std::clamp<u8>(danger, 1, 3);
    switch (k) {
        case ErrandKind::LostGoat: return 1;
        case ErrandKind::Brigands: return static_cast<u8>(1 + d); // 2 / 3 / 4 cutthroats
        case ErrandKind::StuckCart: return kStuckDigs;
        case ErrandKind::LostParcels: return kParcelCount;
        case ErrandKind::Wolves: return static_cast<u8>(1 + d);   // 2 / 3 / 4 wolves
    }
    return 1;
}
// What the traveller can spare (to the party's wallet) - small next to a contract, quick to earn.
inline u32 errand_reward(ErrandKind k, u8 danger) {
    const u32 d = std::clamp<u8>(danger, 1, 3);
    switch (k) {
        case ErrandKind::LostGoat: return 45u + 10u * d;
        case ErrandKind::Brigands: return 60u + 30u * d;
        case ErrandKind::StuckCart: return 40u + 10u * d;
        case ErrandKind::LostParcels: return 45u + 10u * d;
        case ErrandKind::Wolves: return 55u + 25u * d;
    }
    return 50u;
}
inline u32 errand_xp(ErrandKind k, u8 danger) {
    const u32 d = std::clamp<u8>(danger, 1, 3);
    return (k == ErrandKind::Brigands || k == ErrandKind::Wolves ? 45u : 30u) + 12u * d;
}

} // namespace alryn
