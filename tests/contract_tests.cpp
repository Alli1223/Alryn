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
