#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <cmath>
#include <vector>

// The wagon-transport contract loop: in a town, several wagons are offered (one per
// connected destination town), each paying by distance and carrying a difficulty that
// sets how many enemies ambush it en route. Players agree on one, then either hire a
// driver (auto-drives, less pay) or haul it manually (grab & tow, more pay). Pure data +
// formulas here so the loop is headless-testable; the simulation lives in
// Game/Contracts.cpp (driven by GameServer).
namespace alryn {

enum class WagonMode : u8 {
    Parked = 0,   // an offer sitting in the plaza
    Driver = 1,   // accepted, hired driver auto-drives it
    Manual = 2,   // accepted, hauled by a player
    Delivered = 3,
    Wrecked = 4,
};

enum class ContractPhase : u8 {
    Offer = 0,  // wagons offered, players voting
    Active = 1, // a contract is under way
    Settle = 2, // brief delivered/wrecked banner before the next offer
};

inline constexpr f32 kWagonHealth = 200.0f;
inline constexpr f32 kWagonDriverSpeed = 2.1f; // hired driver pace (m/s)
inline constexpr f32 kWagonManualSpeed = 3.0f; // cap while a player hauls it
inline constexpr f32 kWagonGrabRange = 3.0f;   // how close to hitch a wagon
inline constexpr f32 kHitchDist = 2.0f;        // puller stands at the cart's draw-tongue tip
inline constexpr f32 kManualRewardMult = 1.6f; // hauling it yourself pays more
inline constexpr u32 kAmbushPerDifficulty = 3; // enemies per difficulty star
inline constexpr f32 kDeliverRadius = 11.0f;   // within this of the dest centre = delivered
inline constexpr f32 kWagonDamage = 9.0f;      // an ambusher's hit on the wagon
inline constexpr u32 kMaxOffers = 4;           // wagons offered per town
inline constexpr f32 kSettleSeconds = 6.0f;    // banner hold before re-offering

// A transport wagon: an offer while Parked, the active cargo once accepted.
struct Wagon {
    u32 id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    f32 health = kWagonHealth;
    Vec2 source{0.0f};            // origin town centre
    Vec2 dest{0.0f};              // destination town centre
    f32 source_half = 20.0f;      // origin town half-width (ambush holds until past it)
    u32 reward = 0;               // base payout (driver); manual multiplies it
    u8 difficulty = 1;            // 1..3 -> ambush count
    std::vector<Vec2> route;      // road polyline source -> dest (driver path)
    f32 progress = 0.0f;          // index-space progress along `route` (driver)
    u8 ambush_waves_spawned = 0;  // how many ambush waves have triggered
};

// Payout for delivering a contract of this `distance` (world units) + `difficulty`,
// hauled manually or by a hired driver. Longer + harder + manual = more money.
inline u32 contract_reward(f32 distance, u8 difficulty, bool manual) {
    const f32 diff_bonus = 1.0f + 0.25f * static_cast<f32>(difficulty - 1);
    const f32 base = distance * 1.5f * diff_bonus;
    return static_cast<u32>(std::lround(base * (manual ? kManualRewardMult : 1.0f)));
}

// How many ambushers attack a contract of this difficulty over its journey.
inline u32 ambush_count(u8 difficulty) {
    return static_cast<u32>(difficulty) * kAmbushPerDifficulty;
}

} // namespace alryn
