#include <doctest/doctest.h>

#include <Alryn/Game/Contract.h>
#include <Alryn/World/VehicleTypes.h>

using namespace alryn;

TEST_CASE("Contract: reward scales with distance, difficulty, and manual hauling") {
    // Longer routes pay more.
    CHECK(contract_reward(300.0f, 1, false) > contract_reward(150.0f, 1, false));
    // Harder contracts pay more.
    CHECK(contract_reward(200.0f, 3, false) > contract_reward(200.0f, 1, false));
    // Hauling it yourself pays more than hiring a driver.
    CHECK(contract_reward(200.0f, 2, true) > contract_reward(200.0f, 2, false));
    // The manual multiplier is applied (~kManualRewardMult).
    const f64 driver = contract_reward(240.0f, 1, false);
    const f64 manual = contract_reward(240.0f, 1, true);
    CHECK(manual == doctest::Approx(driver * kManualRewardMult).epsilon(0.05));
}

TEST_CASE("Contract: ambush count scales with difficulty") {
    CHECK(ambush_count(1) == kAmbushPerDifficulty);
    CHECK(ambush_count(3) == 3u * kAmbushPerDifficulty);
    CHECK(ambush_count(2) > ambush_count(1));
}

TEST_CASE("Contract: modifiers vary pay + ambush, deterministic and a mix") {
    // Hazardous + bulk pay more and add raiders; safe pays + fights less.
    CHECK(modifier_effect(ContractModifier::Hazardous).pay_mult > 1.0f);
    CHECK(modifier_effect(ContractModifier::Hazardous).ambush_delta > 0);
    CHECK(modifier_effect(ContractModifier::Bulk).pay_mult > 1.0f);
    CHECK(modifier_effect(ContractModifier::Safe).pay_mult < 1.0f);
    CHECK(modifier_effect(ContractModifier::Safe).ambush_delta < 0);
    // ambush_count applies the delta, defaults to standard, and never drops below one.
    CHECK(ambush_count(2, ContractModifier::Hazardous) == ambush_count(2) + 2u);
    CHECK(ambush_count(1, ContractModifier::Standard) == ambush_count(1));
    CHECK(ambush_count(1, ContractModifier::Safe) >= 1u);
    // Deterministic per id, with a real mix across ids (most standard, a healthy fraction flavoured).
    CHECK(contract_modifier(12345u) == contract_modifier(12345u));
    int nonstd = 0;
    for (u32 id = 1; id <= 200u; ++id) {
        if (contract_modifier(id) != ContractModifier::Standard) {
            ++nonstd;
        }
    }
    CHECK(nonstd > 20);
    CHECK(nonstd < 180);
}

TEST_CASE("Contract: perishable cargo holds value, then spoils if delivered late") {
    const f32 dist = 300.0f;
    const f32 deadline = perishable_deadline(dist);
    REQUIRE(deadline > 0.0f);
    // Fresh (at or before the deadline) -> full value.
    CHECK(perishable_value_mult(0.0f, dist, ContractModifier::Perishable) == doctest::Approx(1.0f));
    CHECK(perishable_value_mult(deadline, dist, ContractModifier::Perishable) == doctest::Approx(1.0f));
    // Past the deadline the value decays, monotonically, down to (never below) the floor.
    CHECK(perishable_value_mult(deadline * 1.5f, dist, ContractModifier::Perishable) < 1.0f);
    const f32 a = perishable_value_mult(deadline * 1.3f, dist, ContractModifier::Perishable);
    const f32 b = perishable_value_mult(deadline * 1.8f, dist, ContractModifier::Perishable);
    CHECK(b < a);
    CHECK(perishable_value_mult(deadline * 100.0f, dist, ContractModifier::Perishable) ==
          doctest::Approx(kSpoilFloor));
    // The freshness deadline is tighter than the rush budget - perishable demands real speed.
    CHECK(perishable_deadline(dist) < rush_expected_time(dist));
    // Non-perishable cargo never spoils, however slowly it arrives.
    CHECK(perishable_value_mult(deadline * 100.0f, dist, ContractModifier::Standard) ==
          doctest::Approx(1.0f));
    CHECK(perishable_value_mult(deadline * 100.0f, dist, ContractModifier::Bulk) ==
          doctest::Approx(1.0f));
    // Perishable pays a premium up front, and appears in the deterministic offer mix.
    CHECK(modifier_effect(ContractModifier::Perishable).pay_mult > 1.0f);
    bool seen = false;
    for (u32 id = 1; id <= 400u && !seen; ++id) {
        seen = contract_modifier(id) == ContractModifier::Perishable;
    }
    CHECK(seen);
}

TEST_CASE("Contract: intact-delivery bonus rewards a healthy wagon") {
    CHECK(intact_bonus_mult(1.0f) > intact_bonus_mult(0.5f));
    CHECK(intact_bonus_mult(0.5f) > intact_bonus_mult(0.0f));
    CHECK(intact_bonus_mult(0.0f) == doctest::Approx(1.0f));               // a wreck earns no bonus
    CHECK(intact_bonus_mult(1.0f) == doctest::Approx(1.0f + kIntactBonus)); // full health = full bonus
    CHECK(intact_bonus_mult(2.0f) == doctest::Approx(intact_bonus_mult(1.0f))); // clamped above 1
    CHECK(intact_bonus_mult(-1.0f) == doctest::Approx(1.0f));                    // clamped below 0
}

TEST_CASE("Contract: last stand ramps the defenders' damage as the wagon nears wrecking") {
    CHECK(last_stand_mult(1.0f) == doctest::Approx(1.0f));                // full health -> no bonus
    CHECK(last_stand_mult(kLastStandThreshold) == doctest::Approx(1.0f)); // at the threshold -> none yet
    CHECK(last_stand_mult(kLastStandThreshold * 0.5f) > 1.0f);            // below it -> a damage bonus
    CHECK(last_stand_mult(0.0f) == doctest::Approx(1.0f + kLastStandMaxBonus)); // near-wrecked -> max
    // It ramps up monotonically as the wagon takes more damage.
    CHECK(last_stand_mult(0.10f) > last_stand_mult(0.20f));
    CHECK(last_stand_mult(0.20f) > last_stand_mult(0.25f));
    CHECK(last_stand_mult(-0.5f) == doctest::Approx(1.0f + kLastStandMaxBonus)); // clamps below 0
    CHECK(last_stand_mult(2.0f) == doctest::Approx(1.0f));                       // clamps above 1
    // It pulls against the INTACT pay bonus: a pristine cart pays more but never triggers last stand.
    CHECK(last_stand_mult(1.0f) == doctest::Approx(1.0f));
    CHECK(intact_bonus_mult(0.1f) < intact_bonus_mult(1.0f));
}

TEST_CASE("Contract: a tended cart field-repairs between waves, capped at full") {
    const f32 max = 200.0f, dt = 1.0f;
    CHECK(field_repair(100.0f, max, true, dt) > 100.0f); // tended + damaged -> mends
    CHECK(field_repair(100.0f, max, true, dt) == doctest::Approx(100.0f + kFieldRepairRate));
    CHECK(field_repair(100.0f, max, false, dt) == doctest::Approx(100.0f)); // untended -> no change
    CHECK(field_repair(max, max, true, dt) == doctest::Approx(max));        // already full -> no change
    CHECK(field_repair(max - 1.0f, max, true, dt) == doctest::Approx(max)); // caps at full (no overshoot)
    CHECK(field_repair(100.0f, max, true, 0.0f) == doctest::Approx(100.0f)); // no time -> no change
    CHECK(field_repair(50.0f, max, true, dt) >= 50.0f);                      // only ever heals up
}

TEST_CASE("Contract: wagon rig upgrades cost more + make the cart tougher") {
    CHECK(rig_price(2) > rig_price(1)); // escalating cost
    CHECK(rig_price(3) > rig_price(2));
    CHECK(rig_max_health(1) > rig_max_health(0)); // each level = more max health
    CHECK(rig_max_health(kMaxRigLevel) > kWagonHealth);
    CHECK(rig_damage_mult(1) < rig_damage_mult(0)); // each level = less damage taken
    CHECK(rig_damage_mult(0) == doctest::Approx(1.0f)); // stock cart has no resist
    CHECK(rig_damage_mult(kMaxRigLevel) > 0.0f);        // resist never fully negates damage
    // Levels above the cap clamp (a client can't claim a level it didn't buy).
    CHECK(rig_max_health(static_cast<u8>(kMaxRigLevel + 5)) == doctest::Approx(rig_max_health(kMaxRigLevel)));
    CHECK(rig_damage_mult(static_cast<u8>(kMaxRigLevel + 5)) == doctest::Approx(rig_damage_mult(kMaxRigLevel)));
}

TEST_CASE("Contract: a reinforced rig tows faster, and a stock rig is exactly unchanged") {
    CHECK(rig_speed_mult(0) == doctest::Approx(1.0f)); // CRITICAL: stock rig = no tow-speed change
    CHECK(rig_speed_mult(1) > 1.0f);                   // each level tows a bit faster
    CHECK(rig_speed_mult(2) > rig_speed_mult(1));
    CHECK(rig_speed_mult(kMaxRigLevel) > rig_speed_mult(0)); // a maxed rig clearly tows faster
    CHECK(rig_speed_mult(static_cast<u8>(kMaxRigLevel + 5)) ==
          doctest::Approx(rig_speed_mult(kMaxRigLevel))); // clamps above the cap
}

TEST_CASE("Contract: rush bonus rewards a fast delivery + decays to nothing") {
    const f32 exp = rush_expected_time(300.0f);
    CHECK(exp > 0.0f);
    CHECK(rush_expected_time(600.0f) > exp);                                 // longer route = more budget
    CHECK(rush_bonus_mult(0.0f, exp) == doctest::Approx(1.0f + kRushBonus)); // instant = full bonus
    CHECK(rush_bonus_mult(exp, exp) == doctest::Approx(1.0f));               // at the budget = none
    CHECK(rush_bonus_mult(exp * 2.0f, exp) == doctest::Approx(1.0f));        // over budget = none (clamped)
    CHECK(rush_bonus_mult(exp * 0.4f, exp) > rush_bonus_mult(exp * 0.8f, exp)); // faster = more
    CHECK(rush_bonus_mult(10.0f, 0.0f) == doctest::Approx(1.0f));            // guard: no budget = no bonus
}

TEST_CASE("Contract: a clean-delivery streak stacks a capped pay bonus") {
    CHECK(streak_mult(0) == doctest::Approx(1.0f));                   // no streak = no bonus
    CHECK(streak_mult(1) == doctest::Approx(1.0f + kStreakBonusPer)); // first perfect run
    CHECK(streak_mult(2) > streak_mult(1));                           // stacks
    CHECK(streak_mult(3) > streak_mult(2));
    CHECK(streak_mult(kStreakMax) == doctest::Approx(1.0f + kStreakBonusPer * kStreakMax));
    CHECK(streak_mult(kStreakMax + 7) == doctest::Approx(streak_mult(kStreakMax))); // capped
    CHECK(streak_mult(kStreakMax) > streak_mult(0)); // a full streak clearly pays more than none
}

TEST_CASE("Contract: a kill bounty pays per raider felled, rewarding fighting the ambush") {
    CHECK(kill_bounty(0) == 0u);                  // no kills -> no bounty
    CHECK(kill_bounty(1) == kBountyPerKill);      // one raider down
    CHECK(kill_bounty(5) == 5u * kBountyPerKill); // scales linearly with kills
    CHECK(kill_bounty(5) > kill_bounty(2));       // more kills -> more bounty
    CHECK(kBountyPerKill > 0u);
}

TEST_CASE("Contract: a convoy bonus rewards a bigger escort party, solo unchanged") {
    CHECK(convoy_mult(0) == doctest::Approx(1.0f)); // edge (no players) = baseline
    CHECK(convoy_mult(1) == doctest::Approx(1.0f)); // solo = no bonus (deterministic haul tests safe)
    CHECK(convoy_mult(2) == doctest::Approx(1.0f + kConvoyBonusPer)); // one extra escort
    CHECK(convoy_mult(3) > convoy_mult(2));                            // stacks
    CHECK(convoy_mult(1 + kConvoyMaxEscorts) ==
          doctest::Approx(1.0f + kConvoyBonusPer * kConvoyMaxEscorts));  // capped at a full convoy
    CHECK(convoy_mult(20) == doctest::Approx(convoy_mult(1 + kConvoyMaxEscorts))); // stays capped
    CHECK(convoy_mult(4) > convoy_mult(1)); // a convoy clearly out-earns a lone hauler
}

TEST_CASE("Contract: an unscathed (no-casualty) haul pays a premium that fades with each down") {
    CHECK(unscathed_mult(0) == doctest::Approx(1.0f + kUnscathedBonus)); // nobody downed = full bonus
    CHECK(unscathed_mult(1) < unscathed_mult(0));                        // each casualty erodes it
    CHECK(unscathed_mult(2) < unscathed_mult(1));
    CHECK(unscathed_mult(kUnscathedDownsFloor) == doctest::Approx(1.0f));     // bonus gone at the floor
    CHECK(unscathed_mult(kUnscathedDownsFloor + 9) == doctest::Approx(1.0f)); // and stays gone
    CHECK(unscathed_mult(0) > unscathed_mult(kUnscathedDownsFloor)); // a clean haul clearly out-earns a costly one
    CHECK(kUnscathedBonus > 0.0f);
}

TEST_CASE("Contract: bigger vehicles pay more (capacity multiplier)") {
    CHECK(capacity_reward_mult(1) == doctest::Approx(1.0f));     // a cart: baseline pay
    CHECK(capacity_reward_mult(2) > capacity_reward_mult(1));    // a wagon: more
    CHECK(capacity_reward_mult(3) > capacity_reward_mult(2));    // a carriage: more again
    CHECK(capacity_reward_mult(0) == doctest::Approx(1.0f));     // never below baseline
}

TEST_CASE("Contract: hauling a bigger / more damaged cart is slower") {
    // At full health a bigger cart slows the puller more.
    CHECK(tow_speed_factor(1, 1.0f) == doctest::Approx(1.0f));
    CHECK(tow_speed_factor(2, 1.0f) < tow_speed_factor(1, 1.0f));
    CHECK(tow_speed_factor(3, 1.0f) < tow_speed_factor(2, 1.0f));
    // The same cart hauls slower as it takes damage.
    CHECK(tow_speed_factor(1, 0.0f) < tow_speed_factor(1, 1.0f));
    CHECK(tow_speed_factor(1, 0.0f) == doctest::Approx(kDamageSpeedFloor));
    // It never grinds fully to a stop, nor exceeds full speed.
    CHECK(tow_speed_factor(3, 0.0f) > 0.0f);
    CHECK(tow_speed_factor(1, 1.0f) <= 1.0f);
    // The damage-only factor mirrors the damage half.
    CHECK(damage_speed_factor(1.0f) == doctest::Approx(1.0f));
    CHECK(damage_speed_factor(0.0f) == doctest::Approx(kDamageSpeedFloor));
}

TEST_CASE("Contract: a laden cart tows slower, lightening + quickening as cargo spills") {
    const u32 total = goods_for_capacity(2);
    // An empty bed tows at full pace; a full load is the slowest.
    CHECK(load_speed_factor(0, total) == doctest::Approx(1.0f));
    CHECK(load_speed_factor(total, total) == doctest::Approx(1.0f - kLadenPenalty));
    CHECK(load_speed_factor(total, total) < load_speed_factor(0, total));
    // As crates spill, the cart speeds up monotonically (lighter load -> faster haul).
    CHECK(load_speed_factor(total, total) < load_speed_factor(total / 2, total));
    CHECK(load_speed_factor(total / 2, total) < load_speed_factor(0, total));
    // It never drops below the floor, never exceeds full speed, and clamps an over-count.
    CHECK(load_speed_factor(total, total) >= 1.0f - kLadenPenalty);
    CHECK(load_speed_factor(0, total) <= 1.0f);
    CHECK(load_speed_factor(total + 5, total) == doctest::Approx(1.0f - kLadenPenalty)); // clamp
    // No cargo concept (total 0) is a no-op multiplier.
    CHECK(load_speed_factor(0, 0) == doctest::Approx(1.0f));
}

TEST_CASE("Contract: bigger carts carry more goods, and lost goods cut the pay") {
    CHECK(goods_for_capacity(1) < goods_for_capacity(2));
    CHECK(goods_for_capacity(2) < goods_for_capacity(3));
    CHECK(goods_for_capacity(1) >= 1);
    // The delivery payout scales linearly with the share of goods aboard (server formula).
    const u32 reward = 1000;
    const u8 total = goods_for_capacity(2);
    auto payout = [&](u8 aboard) {
        const f32 frac = static_cast<f32>(aboard) / static_cast<f32>(total);
        return static_cast<u32>(std::lround(static_cast<f32>(reward) * frac));
    };
    CHECK(payout(total) == reward);    // full load -> full pay
    CHECK(payout(total / 2) < reward); // half the load lost -> less pay
    CHECK(payout(0) == 0u);            // nothing delivered -> nothing earned
}

TEST_CASE("VehicleTypes: registry exposes distinct cart/wagon/carriage layouts") {
    REQUIRE(vehicle_type_count() >= 3);

    // Each type is addressable and self-describes its layout.
    const VehicleType& cart = vehicle_type(0);
    const VehicleType& wagon = vehicle_type(1);
    const VehicleType& carriage = vehicle_type(2);

    // Carts have 2 wheels; wagons & carriages have 4.
    CHECK(cart.wheels().size() == 2);
    CHECK(wagon.wheels().size() == 4);
    CHECK(carriage.wheels().size() == 4);

    // Capacity (and therefore pay) grows with the vehicle.
    CHECK(cart.capacity() < wagon.capacity());
    CHECK(wagon.capacity() < carriage.capacity());

    // Only the carriage is horse-drawn with a driver seated up top.
    CHECK_FALSE(cart.horse_drawn());
    CHECK_FALSE(wagon.horse_drawn());
    CHECK(carriage.horse_drawn());
    CHECK(carriage.has_driver_seat());
    CHECK(carriage.driver_seat().y > 1.0f); // the box seat is raised above the bed

    // Every vehicle has a cargo bed with side rails, and an enclosed carriage's "walls" are
    // far taller (its load can never be launched out - the closed-cabin benefit) than an open
    // cart's low rails.
    for (u8 i = 0; i < vehicle_type_count(); ++i) {
        const CargoBed bed = vehicle_type(i).bed();
        CHECK(bed.hi.x > bed.lo.x);
        CHECK(bed.wall > 0.0f);
    }
    CHECK(cart.bed().wall < 1.0f);            // open cart: low rails, cargo can spill over
    CHECK(carriage.bed().wall > cart.bed().wall * 3.0f); // enclosed carriage: cargo never clears

    // Every vehicle has at least one passenger seat and a lamp mount.
    for (u8 i = 0; i < vehicle_type_count(); ++i) {
        CHECK_FALSE(vehicle_type(i).seats().empty());
        CHECK(vehicle_type(i).body().vertices.size() > 0);
    }

    // Out-of-range type indices wrap rather than crash (the type rides as a u8 on the wire).
    CHECK(&vehicle_type(vehicle_type_count()) == &vehicle_type(0));
}
