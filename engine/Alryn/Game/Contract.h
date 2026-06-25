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
inline constexpr f32 kTowMaxSlack = 1.6f;      // a hired puller slows as the cart lags this far
                                               // past the hitch (so it can't drag a stuck cart)
inline constexpr f32 kCartTurnRate = 2.6f;     // max rad/s the towed cart can swing its heading
                                               // (a cart can't pivot in place - it trails)
inline constexpr f32 kManualRewardMult = 1.6f; // hauling it yourself pays more
inline constexpr u32 kAmbushPerDifficulty = 3; // enemies per difficulty star
inline constexpr f32 kDeliverRadius = 11.0f;   // within this of the dest centre = delivered
inline constexpr f32 kCarriageSpeed = 5.0f;    // player-driven carriage top speed (m/s)
inline constexpr f32 kCarriageTurnRate = 1.6f; // rein steering rate (rad/s)

// Bigger vehicles carry more cargo, so they pay more: +30% per capacity unit over 1.
inline f32 capacity_reward_mult(u32 capacity) {
    return 1.0f + 0.3f * static_cast<f32>(capacity > 0 ? capacity - 1 : 0);
}

// --- Load, hauling effort & capsizing ------------------------------------------
// How many cargo crates a vehicle of this capacity carries (reward scales by the share
// delivered, so spilled-and-lost goods cost money).
inline u8 goods_for_capacity(u32 capacity) {
    return static_cast<u8>(2u * (capacity > 0 ? capacity : 1u));
}
inline constexpr f32 kTowSizePenalty = 0.34f; // a hauled cart slows the puller per capacity unit
inline constexpr f32 kDamageSpeedFloor = 0.5f; // a wrecked-but-rolling cart moves at half speed
// How much slower a player hauls a cart of this `capacity` at this `health_frac` (0..1).
// Bigger and more damaged => slower. Returns a multiplier in (0, 1].
inline f32 tow_speed_factor(u32 capacity, f32 health_frac) {
    const f32 size = 1.0f / (1.0f + kTowSizePenalty * static_cast<f32>(capacity > 0 ? capacity - 1 : 0));
    const f32 hf = health_frac < 0.0f ? 0.0f : (health_frac > 1.0f ? 1.0f : health_frac);
    const f32 dmg = kDamageSpeedFloor + (1.0f - kDamageSpeedFloor) * hf;
    return size * dmg;
}
// Damage-only speed multiplier (for horse/teamster/driven carriage motion).
inline f32 damage_speed_factor(f32 health_frac) {
    const f32 hf = health_frac < 0.0f ? 0.0f : (health_frac > 1.0f ? 1.0f : health_frac);
    return kDamageSpeedFloor + (1.0f - kDamageSpeedFloor) * hf;
}
inline constexpr f32 kGoodPickupRange = 2.0f; // how close to pick up a spilled crate (E)
inline constexpr f32 kGoodLoadRange = 3.0f;   // how close to the cart to load a carried crate (E)
// Cargo box physics: each crate is a little body that slides on the cart bed. The bed walls are
// SOLID - crates slam into them and bounce, they never pass through from sliding - so normal
// hauling + hard turns keep the load aboard. A crate only spills *over* a wall when the bed is
// tilted steeply (a hill/bump), so the load is lost on rough ground, not on a quick pull-away.
inline constexpr f32 kCargoGravity = 16.0f;     // pulls crates downhill along a terrain-tilted bed
inline constexpr f32 kCargoFriction = 2.4f;     // bed friction (per-second velocity damping)
inline constexpr f32 kCargoRestitution = 0.3f;  // how bouncily a crate rebounds off a solid wall
inline constexpr f32 kCargoHalf = 0.2f;         // crate half-size (kept off the bed walls)
inline constexpr f32 kCargoInertia = 1.0f;      // how strongly cart acceleration slings crates
inline constexpr f32 kCargoMaxAccel = 18.0f;    // clamp on the cart accel (a tow "snap" can't fling crates)
inline constexpr f32 kCargoVertGravity = 18.0f; // gravity pulling an airborne crate back to the bed floor
inline constexpr f32 kCargoFloorBounce = 0.2f;  // how bouncily a crate lands back on the bed floor
inline constexpr f32 kCargoMaxLift = 1.5f;      // cap on how high a bump can toss a crate (sanity)
inline constexpr f32 kWagonDamage = 9.0f;      // an ambusher's hit on the wagon
inline constexpr u32 kMaxOffers = 4;           // wagons offered per town
inline constexpr f32 kSettleSeconds = 6.0f;    // banner hold before re-offering

// --- Wheel breakdown: a wheel can come off mid-haul; the cart halts until a player fetches the
// fallen wheel and re-attaches it (a slow channel - bandits may strike while it's down, see #22).
// Tuned so a wheel comes off only about once or twice per haul (the timer counts only while the cart
// is actually ROLLING, so it's roughly the driving time, not wall-clock).
inline constexpr f32 kWheelBreakAvgTime = 95.0f; // upper bound on rolling-seconds before a wheel sheds
inline constexpr f32 kWheelBreakMinTime = 55.0f; // never breaks again sooner than this
inline constexpr f32 kWheelRepairTime = 6.0f;    // seconds of holding the wheel by the cart to refit
inline constexpr f32 kWheelPickupRange = 2.5f;   // how close to grab the fallen wheel (E)
inline constexpr f32 kWheelAttachRange = 4.0f;   // carry it this close to the cart for it to refit
// A shed wheel is a physical object: it bursts off with a speed + direction and ROLLS until friction
// stops it (then it lies waiting to be fetched), so the players have to chase it down.
inline constexpr f32 kWheelRollSpeed = 5.5f;     // initial roll speed when the wheel pops off (m/s)
inline constexpr f32 kWheelRollDrag = 1.15f;     // velocity decay per second (friction) - guarantees a stop
inline constexpr f32 kWheelRollStop = 0.5f;      // below this speed the wheel settles and stops
inline constexpr f32 kWheelBounce = 0.55f;       // velocity kept when it bounces off a wall/building
// A stranded (wheel-off) cart draws opportunist bandits: small waves on a timer while it's down.
inline constexpr f32 kBanditFirstDelay = 3.0f;   // grace after the break before bandits show
inline constexpr f32 kBanditWaveInterval = 8.0f; // seconds between bandit waves during a repair
inline constexpr u32 kBanditWaveSize = 2;        // bandits per wave
inline constexpr usize kRepairBanditCap = 8;     // don't pile on past this many ambushers at once

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
    u8 type = 0;                  // VehicleType index (cart / wagon / carriage)
    u8 difficulty = 1;            // 1..3 -> ambush count
    std::vector<Vec2> route;      // road polyline source -> dest (driver path)
    f32 progress = 0.0f;          // index-space progress along `route` (driver)
    u8 ambush_waves_spawned = 0;  // how many ambush waves have triggered
    u8 goods_total = 0;           // crates a full load carries (reward scales by aboard/total)
};

// Payout for delivering a contract of this `distance` (world units) + `difficulty`,
// hauled manually or by a hired driver. Longer + harder + manual = more money.
inline u32 contract_reward(f32 distance, u8 difficulty, bool manual) {
    const f32 diff_bonus = 1.0f + 0.25f * static_cast<f32>(difficulty - 1);
    const f32 base = distance * 1.5f * diff_bonus;
    return static_cast<u32>(std::lround(base * (manual ? kManualRewardMult : 1.0f)));
}

// Per-contract MODIFIER: a deterministic flavour on each offer (derived from its id) that varies the
// pay + ambush size, so the board mixes safe low-pay escorts with dangerous, well-paid hauls instead
// of every contract feeling the same. The client derives the SAME modifier from the (networked) wagon
// id, so no extra wire field is needed.
enum class ContractModifier : u8 {
    Standard = 0,
    Hazardous = 1, // dangerous + well paid
    Bulk = 2,      // a big heavy load, extra pay + an extra raider
    Safe = 3,      // an easy, lightly-paid escort
};
struct ModifierEffect {
    f32 pay_mult;
    int ambush_delta;
};
inline ModifierEffect modifier_effect(ContractModifier m) {
    switch (m) {
        case ContractModifier::Hazardous: return {1.6f, 2};
        case ContractModifier::Bulk: return {1.35f, 1};
        case ContractModifier::Safe: return {0.75f, -1};
        default: return {1.0f, 0};
    }
}
// Deterministic per offer id (so server + client agree). ~55% standard, then hazardous / bulk / safe.
inline ContractModifier contract_modifier(u32 id) {
    u32 h = id * 2654435761u;
    h ^= h >> 13;
    const u32 r = h % 100u;
    if (r < 55u) return ContractModifier::Standard;
    if (r < 73u) return ContractModifier::Hazardous;
    if (r < 87u) return ContractModifier::Bulk;
    return ContractModifier::Safe;
}
inline const char* modifier_name(ContractModifier m) {
    switch (m) {
        case ContractModifier::Hazardous: return "HAZARDOUS";
        case ContractModifier::Bulk: return "BULK CARGO";
        case ContractModifier::Safe: return "SAFE ROUTE";
        default: return "STANDARD";
    }
}

// How many ambushers attack a contract of this difficulty over its journey, adjusted by the modifier
// (at least one). The default modifier keeps the old behaviour for existing callers/tests.
inline u32 ambush_count(u8 difficulty, ContractModifier mod = ContractModifier::Standard) {
    const int base = static_cast<int>(difficulty) * static_cast<int>(kAmbushPerDifficulty);
    const int n = base + modifier_effect(mod).ambush_delta;
    return static_cast<u32>(n < 1 ? 1 : n);
}

} // namespace alryn
