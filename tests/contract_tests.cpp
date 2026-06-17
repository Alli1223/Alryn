#include <doctest/doctest.h>

#include <Alryn/Game/Contract.h>

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
