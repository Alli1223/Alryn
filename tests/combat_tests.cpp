#include <doctest/doctest.h>

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Combat/Villager.h>
#include <Alryn/Core/Density.h>

#include <span>

using namespace alryn;

namespace {
// Flat ground at y = 0: solid below, air above (density == height).
DensitySampler flat_ground() {
    return [](const Vec3& p) { return p.y; };
}
} // namespace

TEST_CASE("Combat: melee cone hits what's in front, misses behind / wide / far") {
    const Vec3 origin{0.0f, 0.0f, 0.0f};
    const f32 yaw = 0.0f; // facing +x

    CHECK(in_attack_cone(origin, yaw, Vec3{2.0f, 0.0f, 0.0f}, kMeleeRange, kMeleeConeCos));
    CHECK(in_attack_cone(origin, yaw, Vec3{1.5f, 0.0f, 0.6f}, kMeleeRange, kMeleeConeCos));
    CHECK_FALSE(in_attack_cone(origin, yaw, Vec3{-2.0f, 0.0f, 0.0f}, kMeleeRange, kMeleeConeCos));
    CHECK_FALSE(in_attack_cone(origin, yaw, Vec3{0.0f, 0.0f, 2.0f}, kMeleeRange, kMeleeConeCos));
    CHECK_FALSE(in_attack_cone(origin, yaw, Vec3{5.0f, 0.0f, 0.0f}, kMeleeRange, kMeleeConeCos));
}

TEST_CASE("Combat: an enemy marches toward its goal and stays on the ground") {
    const DensitySampler density = flat_ground();
    const std::span<const Collider> none{};
    const Vec3 goal{12.0f, 0.0f, 0.0f};

    Enemy e;
    e.position = Vec3{0.0f, 0.0f, 0.0f};
    e.home = goal;

    const f32 start = glm::length(goal - e.position);
    for (int i = 0; i < 60; ++i) {
        step_enemy(e, density, none, goal, Timestep{1.0f / 60.0f});
    }
    const f32 after = glm::length(goal - e.position);
    CHECK(after < start - 1.0f);            // it advanced
    CHECK(e.position.y == doctest::Approx(0.0f).epsilon(0.05)); // hugged the ground
    CHECK(e.yaw == doctest::Approx(0.0f).epsilon(0.05));        // faced +x (toward goal)

    // Given enough time it arrives and effectively stops at the goal.
    for (int i = 0; i < 600; ++i) {
        step_enemy(e, density, none, goal, Timestep{1.0f / 60.0f});
    }
    CHECK(glm::length(goal - e.position) < 0.2f);
}

TEST_CASE("Combat: a villager walks toward its goal (e.g. fleeing / heading to bed)") {
    const DensitySampler density = flat_ground();
    const std::span<const Collider> none{};
    const Vec3 bed{0.0f, 0.0f, 10.0f};

    Villager v;
    v.position = Vec3{0.0f, 0.0f, 0.0f};
    v.appearance = villager_look(1234u);

    const f32 start = glm::length(bed - v.position);
    for (int i = 0; i < 120; ++i) {
        step_villager(v, density, none, bed, Timestep{1.0f / 60.0f});
    }
    CHECK(glm::length(bed - v.position) < start - 1.0f);
    CHECK(v.position.y == doctest::Approx(0.0f).epsilon(0.05));

    // villager_look is deterministic for a given id.
    CHECK(villager_look(1234u) == villager_look(1234u));
}

TEST_CASE("Combat: damage tuning kills an enemy in a few blows") {
    // A couple of melee swings (or a melee + a thrown rock) should finish an enemy.
    CHECK(kMeleeDamage * 2.0f >= kEnemyMaxHealth);
    CHECK(kMeleeDamage + kThrowDamage >= kEnemyMaxHealth);

    Enemy e;
    e.health = kEnemyMaxHealth;
    e.health -= kMeleeDamage;
    CHECK(e.health > 0.0f); // survives one hit
    e.health -= kMeleeDamage;
    CHECK(e.health <= 0.0f); // dies on the second
}
