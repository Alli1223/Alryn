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
// Laden weight: a full load is heavy, so a fully-loaded cart tows slower; as crates spill or are lost
// the bed lightens and the haul quickens (though it pays less - reward scales by the share delivered).
// This is DISTINCT from the static capacity penalty (which is the vehicle's SIZE): it tracks the LIVE
// load `aboard` of `total`, so it's an emergent speed/pay tradeoff that shifts during the haul.
// Returns a multiplier in [1 - kLadenPenalty, 1]: 1 when empty, 1 - kLadenPenalty at a full load.
inline constexpr f32 kLadenPenalty = 0.25f; // a full load tows up to 25% slower than an empty bed
inline f32 load_speed_factor(u32 aboard, u32 total) {
    if (total == 0) {
        return 1.0f;
    }
    f32 frac = static_cast<f32>(aboard) / static_cast<f32>(total);
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    return 1.0f - kLadenPenalty * frac;
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

// Index of the route node nearest `pos` (the cart's current spot). Used to resume a hired driver from
// where a player left the cart (continuing forward from there) instead of its stale waypoint, so the
// haul never backtracks after a player hands it back. Returns 0 for an empty route.
inline int nearest_route_index(const Vec2& pos, const std::vector<Vec2>& route) {
    int besti = 0;
    f32 best = 1e18f;
    for (int i = 0; i < static_cast<int>(route.size()); ++i) {
        const f32 d = glm::length(route[static_cast<usize>(i)] - pos);
        if (d < best) {
            best = d;
            besti = i;
        }
    }
    return besti;
}

// Payout for delivering a contract of this `distance` (world units) + `difficulty`,
// hauled manually or by a hired driver. Longer + harder + manual = more money.
inline u32 contract_reward(f32 distance, u8 difficulty, bool manual) {
    const f32 diff_bonus = 1.0f + 0.25f * static_cast<f32>(difficulty - 1);
    const f32 base = distance * 1.5f * diff_bonus;
    return static_cast<u32>(std::lround(base * (manual ? kManualRewardMult : 1.0f)));
}

// An INTACT-delivery bonus: the payout scales up with the wagon's remaining health on arrival, so
// actively DEFENDING the cart from ambushers (not merely surviving) pays off - a pristine arrival
// earns the full bonus, a battered one earns little. (Cargo loss is already penalised separately by
// the share-of-load delivered.)
inline constexpr f32 kIntactBonus = 0.4f; // up to +40% pay for a full-health delivery
inline f32 intact_bonus_mult(f32 health_frac) {
    const f32 hf = health_frac < 0.0f ? 0.0f : (health_frac > 1.0f ? 1.0f : health_frac);
    return 1.0f + kIntactBonus * hf;
}

// LAST STAND: when the cargo wagon is badly battered (health below kLastStandThreshold) the defenders
// fight with desperate fury - their OUTGOING damage ramps up the closer the cart is to wrecking, a
// comeback chance to save a failing haul. (Pulls against the INTACT pay bonus, which rewards keeping
// it pristine - so it's drama, not a thing you farm.) Returns 1.0 until the wagon is hurt enough.
inline constexpr f32 kLastStandThreshold = 0.30f; // wagon health fraction below which it kicks in
inline constexpr f32 kLastStandMaxBonus = 0.6f;   // up to +60% damage as the wagon nears 0
inline f32 last_stand_mult(f32 wagon_health_frac) {
    const f32 hf = wagon_health_frac < 0.0f ? 0.0f : (wagon_health_frac > 1.0f ? 1.0f : wagon_health_frac);
    if (hf >= kLastStandThreshold) {
        return 1.0f;
    }
    const f32 t = (kLastStandThreshold - hf) / kLastStandThreshold; // 0 at the threshold -> 1 at zero
    return 1.0f + kLastStandMaxBonus * t;
}

// A RUSH bonus for a fast delivery: the payout gets a bonus that DECAYS from +kRushBonus when the haul
// starts down to 0 by the time a steady run would take (rush_expected_time). Hurrying - and risking the
// ambushers rather than stopping to fight them off (which the INTACT bonus rewards) - pays off too, so
// speed and safety pull against each other: a deliberate risk/reward choice, not one optimal play.
inline constexpr f32 kRushBonus = 0.25f; // up to +25% pay for a very fast delivery
inline f32 rush_expected_time(f32 route_dist) {
    return route_dist / kWagonManualSpeed * 1.6f; // a generous time budget (hitching, turns, ambushes)
}
inline f32 rush_bonus_mult(f32 elapsed, f32 expected) {
    if (expected <= 0.0f) {
        return 1.0f;
    }
    f32 t = 1.0f - elapsed / expected; // 1 at the start, 0 once the budget is spent
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return 1.0f + kRushBonus * t;
}

// A clean-delivery STREAK bonus: consecutive PERFECT deliveries (the whole load delivered, cart not
// wrecked) stack a pay multiplier, reset by any spilled-cargo or wrecked run. Rewards consistent,
// careful hauling across contracts (a meta-progression), on top of the per-delivery bonuses.
inline constexpr f32 kStreakBonusPer = 0.1f; // +10% pay per delivery in the streak
inline constexpr u32 kStreakMax = 5;          // capped at +50%
inline f32 streak_mult(u32 streak) {
    const u32 s = streak > kStreakMax ? kStreakMax : streak;
    return 1.0f + kStreakBonusPer * static_cast<f32>(s);
}

// KILL BOUNTY: every ambusher the party cuts down on a haul adds a flat sum to the delivery payout -
// so standing and FIGHTING the ambush pays off, pulling against the RUSH bonus (which rewards hurrying
// past the raiders) and reinforcing the INTACT bonus (defend the cart). Added, not multiplied, so it
// rewards the kills themselves regardless of the contract's size.
inline constexpr u32 kBountyPerKill = 12; // money per raider felled
inline u32 kill_bounty(u32 kills) { return kBountyPerKill * kills; }

// CONVOY bonus: the more players escorting a haul, the better the contract pays - a co-op incentive
// (the whole game is a multiplayer escort), and it offsets a bigger party clearing the ambush more
// easily. Modest + capped so solo play isn't taxed: +kConvoyBonusPer per escort beyond the first,
// capped at kConvoyMaxEscorts. Crucially solo (1 player, or 0 in an edge call) = 1.0, so the
// deterministic single-player haul tests (seed 4242) are unchanged. Multiplicative, so it scales
// with the contract's size - a reason to bring the party on the big, lucrative hauls.
inline constexpr f32 kConvoyBonusPer = 0.1f; // +10% pay per escort beyond the first
inline constexpr u32 kConvoyMaxEscorts = 3;  // capped at +30% (a 4-strong convoy)
inline f32 convoy_mult(u32 escorts) {
    const u32 extra = escorts > 1u ? escorts - 1u : 0u;
    const u32 capped = extra > kConvoyMaxEscorts ? kConvoyMaxEscorts : extra;
    return 1.0f + kConvoyBonusPer * static_cast<f32>(capped);
}

// UNSCATHED bonus: a haul where the party keeps everyone on their feet pays a premium - a reward for
// protecting each other (and a synergy with second wind, which spares a "down"), pulling against the
// RUSH bonus (hurrying past raiders risks casualties). Graded: full bonus at zero downs, fading to
// nothing once the party has been downed kUnscathedDownsFloor times. A "down" is a real respawn (a
// caught second wind is NOT a down). Applied at delivery only, so it never touches the haul sim.
inline constexpr f32 kUnscathedBonus = 0.2f;    // +20% pay for a no-casualty haul
inline constexpr u32 kUnscathedDownsFloor = 3;  // bonus fully gone after this many party downs
inline f32 unscathed_mult(u32 downs) {
    if (downs >= kUnscathedDownsFloor) {
        return 1.0f;
    }
    const f32 t = 1.0f - static_cast<f32>(downs) / static_cast<f32>(kUnscathedDownsFloor);
    return 1.0f + kUnscathedBonus * t;
}

// FIELD REPAIR: between ambush waves (no raiders alive) a player who stays near the cargo wagon
// patches it up, mending its health slowly back toward full - so clearing a wave then tending the
// cart is rewarded, and a battered haul can recover if you make the time. Pure + server-applied.
inline constexpr f32 kFieldRepairRange = 3.5f; // how close a player tends the cart
inline constexpr f32 kFieldRepairRate = 9.0f;  // hp/sec a tended cart mends (only when safe)
inline f32 field_repair(f32 health, f32 max_health, bool tended, f32 dt) {
    if (!tended || health >= max_health || dt <= 0.0f) {
        return health;
    }
    const f32 mended = health + kFieldRepairRate * dt;
    return mended > max_health ? max_health : mended;
}

// --- Wagon RIG upgrades (a money sink) -----------------------------------------
// The party spends money in town to permanently REINFORCE their rig - a tougher wagon that takes the
// road's punishment better. The first upgrade axis: more max health + an ambush-damage resist. Each
// level costs more; effects are pure functions so they're headless-testable + agreed server/client.
inline constexpr u8 kMaxRigLevel = 3;
inline u8 clamp_rig(u8 level) { return level > kMaxRigLevel ? kMaxRigLevel : level; }
// Price of buying the NEXT level (escalating): 220, 880, 1980.
inline u32 rig_price(u8 next_level) {
    const u32 n = next_level > kMaxRigLevel ? kMaxRigLevel : next_level;
    return 220u * n * n;
}
// Max wagon health at a rig level: +30% per level over the stock kWagonHealth.
inline f32 rig_max_health(u8 level) {
    return kWagonHealth * (1.0f + 0.30f * static_cast<f32>(clamp_rig(level)));
}
// Incoming-damage multiplier at a rig level: each level shaves 12% off ambush damage to the wagon.
inline f32 rig_damage_mult(u8 level) {
    return 1.0f - 0.12f * static_cast<f32>(clamp_rig(level));
}
// Tow-speed multiplier at a rig level: a reinforced rig has better axles + greased wheels, so it tows
// a touch faster (+6% per level, +18% at the cap) - a third reason to invest in the rig. MUST be 1.0
// at level 0 (stock) so the baseline haul speed - and the deterministic wheel/ambush tests that run on
// a stock rig - are unchanged.
inline f32 rig_speed_mult(u8 level) {
    return 1.0f + 0.06f * static_cast<f32>(clamp_rig(level));
}

// Per-contract MODIFIER: a deterministic flavour on each offer (derived from its id) that varies the
// pay + ambush size, so the board mixes safe low-pay escorts with dangerous, well-paid hauls instead
// of every contract feeling the same. The client derives the SAME modifier from the (networked) wagon
// id, so no extra wire field is needed.
enum class ContractModifier : u8 {
    Standard = 0,
    Hazardous = 1,  // dangerous + well paid
    Bulk = 2,       // a big heavy load, extra pay + an extra raider
    Safe = 3,       // an easy, lightly-paid escort
    Perishable = 4, // fresh goods: pays well, but the value SPOILS if you dawdle (a hard rush job)
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
        case ContractModifier::Perishable: return {1.3f, 0}; // good pay up front; spoilage is the catch
        default: return {1.0f, 0};
    }
}
// Deterministic per offer id (so server + client agree). ~55% standard, then a mix of the flavours.
inline ContractModifier contract_modifier(u32 id) {
    u32 h = id * 2654435761u;
    h ^= h >> 13;
    const u32 r = h % 100u;
    if (r < 55u) return ContractModifier::Standard;
    if (r < 70u) return ContractModifier::Hazardous;
    if (r < 82u) return ContractModifier::Bulk;
    if (r < 91u) return ContractModifier::Safe;
    return ContractModifier::Perishable;
}
inline const char* modifier_name(ContractModifier m) {
    switch (m) {
        case ContractModifier::Hazardous: return "HAZARDOUS";
        case ContractModifier::Bulk: return "BULK CARGO";
        case ContractModifier::Safe: return "SAFE ROUTE";
        case ContractModifier::Perishable: return "PERISHABLE";
        default: return "STANDARD";
    }
}

// Perishable cargo (the Perishable modifier) holds full value until a TIGHT freshness deadline, then
// its value spoils toward kSpoilFloor - a real penalty for dawdling, distinct from the rush BONUS
// (which only ever adds, never subtracts). Non-perishable cargo never spoils (always 1.0).
inline constexpr f32 kSpoilFloor = 0.5f;  // a fully-spoiled load still sells for half
inline constexpr f32 kSpoilWindow = 1.0f; // route-time budgets of decay from fresh -> floor
inline f32 perishable_deadline(f32 route_dist) {
    return route_dist / kWagonManualSpeed * 1.05f; // tighter than the rush budget (1.6x): a real deadline
}
inline f32 perishable_value_mult(f32 elapsed, f32 route_dist, ContractModifier m) {
    if (m != ContractModifier::Perishable) {
        return 1.0f;
    }
    const f32 deadline = perishable_deadline(route_dist);
    if (deadline <= 0.0f || elapsed <= deadline) {
        return 1.0f; // delivered fresh
    }
    const f32 over = (elapsed - deadline) / (deadline * kSpoilWindow); // 0 at the deadline, 1 a budget on
    const f32 t = over > 1.0f ? 1.0f : over;
    return 1.0f - (1.0f - kSpoilFloor) * t;
}

// How many ambushers attack a contract of this difficulty over its journey, adjusted by the modifier
// (at least one). The default modifier keeps the old behaviour for existing callers/tests.
inline u32 ambush_count(u8 difficulty, ContractModifier mod = ContractModifier::Standard) {
    const int base = static_cast<int>(difficulty) * static_cast<int>(kAmbushPerDifficulty);
    const int n = base + modifier_effect(mod).ambush_delta;
    return static_cast<u32>(n < 1 ? 1 : n);
}

} // namespace alryn
