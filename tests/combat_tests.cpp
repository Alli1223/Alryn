#include <doctest/doctest.h>

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Combat/Villager.h>
#include <Alryn/Core/Density.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/WorldGen.h>

#include <span>

using namespace alryn;

namespace {
// Flat ground at y = 0: solid below, air above (density == height).
DensitySampler flat_ground() {
    return [](const Vec3& p) { return p.y; };
}
} // namespace

// Ambushers chasing the cart must cross a road bridge with it (the `platform` callback), not drop
// into the carved river the bridge spans.
TEST_CASE("Combat: an enemy crosses a bridge instead of falling into the river") {
    for (const u32 seed : {1337u, 4242u, 99u, 777u}) {
        const auto bs = roads::bridges(Vec2{0.0f, 0.0f}, 1500.0f, seed);
        if (bs.empty()) {
            continue;
        }
        const roads::Bridge& b = bs.front();
        const DensitySampler density = [seed](const Vec3& p) { return worldgen::density(p, seed); };
        const auto platform = [seed](f32 x, f32 z) { return roads::bridge_height(x, z, seed); };
        const std::span<const Collider> none{};
        constexpr Timestep step{1.0f / 60.0f};
        const Vec2 dir{std::cos(b.yaw), std::sin(b.yaw)};

        // March an enemy from one bank, across the deck, to the other bank.
        const Vec3 start{b.center.x - dir.x * b.length * 0.42f, 0.0f,
                         b.center.y - dir.y * b.length * 0.42f};
        Enemy e;
        e.position = Vec3{start.x, roads::bridge_height(start.x, start.z, seed), start.z};
        const Vec3 goal{b.center.x + dir.x * b.length * 0.6f, e.position.y,
                        b.center.y + dir.y * b.length * 0.6f};
        f32 worst_below_deck = 0.0f;
        for (int i = 0; i < 300; ++i) {
            step_enemy(e, density, none, goal, step, kEnemySpeed, platform);
            const f32 dh = roads::bridge_height(e.position.x, e.position.z, seed);
            if (dh > -1.0e8f) {
                worst_below_deck = std::min(worst_below_deck, e.position.y - dh);
            }
        }
        CHECK(worst_below_deck > -0.6f); // rode the deck the whole way, never dropped into the river

        // Sanity: WITHOUT the platform it follows the terrain down into the carved river (the old bug).
        Enemy e2;
        e2.position = Vec3{b.center.x, roads::bridge_height(b.center.x, b.center.y, seed), b.center.y};
        step_enemy(e2, density, none, e2.position, step, kEnemySpeed); // no platform
        CHECK(e2.position.y < roads::bridge_height(b.center.x, b.center.y, seed) - 0.5f);
        return;
    }
    MESSAGE("no bridge found near origin in the scanned seeds - skipping");
}

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

TEST_CASE("Combat: a hit knocks an enemy back, then it settles + presses on") {
    const DensitySampler density = flat_ground();
    const std::span<const Collider> none{};
    Enemy e;
    e.position = Vec3{0.0f, 0.0f, 0.0f};
    e.knockback = Vec3{6.0f, 0.0f, 0.0f}; // shoved +x by a hit
    const Vec3 goal = e.position;          // already at the goal, so only the knockback moves it first
    step_enemy(e, density, none, goal, Timestep{1.0f / 60.0f});
    CHECK(e.position.x > 0.0f);             // shoved in the knockback direction
    CHECK(glm::length(e.knockback) < 6.0f); // and decaying
    for (int i = 0; i < 90; ++i) {
        step_enemy(e, density, none, goal, Timestep{1.0f / 60.0f});
    }
    CHECK(glm::length(e.knockback) < 0.1f); // settles to rest
}

TEST_CASE("Combat: a heavy hit staggers an enemy - it reels in place until the stun wears off") {
    const DensitySampler density = flat_ground();
    const std::span<const Collider> none{};
    const Vec3 goal{12.0f, 0.0f, 0.0f};
    const Timestep step{1.0f / 60.0f};

    Enemy e;
    e.position = Vec3{0.0f, 0.0f, 0.0f};
    e.stagger = kStaggerDuration; // just took a heavy hit
    const f32 start = glm::length(goal - e.position);

    // While reeling (< kStaggerDuration s) it makes NO progress toward the goal.
    for (int i = 0; i < 20; ++i) { // 20/60 s < kStaggerDuration
        step_enemy(e, density, none, goal, step);
    }
    CHECK(glm::length(goal - e.position) == doctest::Approx(start).epsilon(0.01)); // held in place
    CHECK(e.stagger > 0.0f);          // still reeling
    CHECK(e.stagger < kStaggerDuration); // but the stun is wearing off

    // Let it fully wear off, then it presses on toward the goal again.
    for (int i = 0; i < 120; ++i) {
        step_enemy(e, density, none, goal, step);
    }
    CHECK(e.stagger <= 0.0f);
    CHECK(glm::length(goal - e.position) < start - 1.0f); // recovered: advancing again

    // Knockback still shoves a staggered enemy, so a heavy blow reads with impact even mid-stun.
    Enemy s;
    s.position = Vec3{0.0f, 0.0f, 0.0f};
    s.stagger = kStaggerDuration;
    s.knockback = Vec3{6.0f, 0.0f, 0.0f};
    step_enemy(s, density, none, s.position, step);
    CHECK(s.position.x > 0.0f); // shoved despite reeling

    CHECK(kStaggerThreshold > kMeleeDamage); // a basic swing doesn't reach it -> no stun-lock
    CHECK(kStaggerDuration > 0.0f);
}

TEST_CASE("Combat: a shield-bearer blocks frontal hits but is open from the flank/rear") {
    Enemy e;
    e.kind = kEnemyShield;
    e.position = Vec3{0.0f, 0.0f, 0.0f};
    e.yaw = 0.0f; // facing +x, so its shield covers the +x arc

    // A hit coming from the front (in front of its facing) is blocked.
    CHECK(enemy_blocks_hit(e, Vec3{3.0f, 0.0f, 0.0f}));
    CHECK(enemy_blocks_hit(e, Vec3{3.0f, 1.0f, 0.8f})); // front, slightly to the side + raised
    // A hit from the side or behind gets through.
    CHECK_FALSE(enemy_blocks_hit(e, Vec3{0.0f, 0.0f, 3.0f}));  // flank
    CHECK_FALSE(enemy_blocks_hit(e, Vec3{-3.0f, 0.0f, 0.0f})); // rear

    // The block applies the configured reduction (so flanking is worth much more damage).
    const f32 front = kMeleeDamage * (1.0f - kShieldReduction);
    CHECK(front < kMeleeDamage * 0.3f); // a frontal swing barely chips it
    CHECK(kMeleeDamage > front * 3.0f); // a flank swing hits far harder

    // Other enemy kinds never block (only the shield-bearer does).
    Enemy grunt;
    grunt.kind = 0;
    CHECK_FALSE(enemy_blocks_hit(grunt, Vec3{3.0f, 0.0f, 0.0f}));
}

TEST_CASE("Combat: a healer ambusher targets the most-wounded ally, not itself or full ones") {
    std::vector<Enemy> es(4);
    for (usize i = 0; i < es.size(); ++i) {
        es[i].id = static_cast<u32>(i + 1);
        es[i].kind = 0;
    }
    es[0].kind = kEnemyHealer;
    es[0].position = Vec3{0.0f, 0.0f, 0.0f};
    es[0].health = 20.0f; // the healer itself is hurt (should still be ignored)
    es[1].position = Vec3{2.0f, 0.0f, 0.0f};
    es[1].health = 50.0f; // lightly wounded, in range
    es[2].position = Vec3{3.0f, 0.0f, 0.0f};
    es[2].health = 12.0f; // badly wounded, in range
    es[3].position = Vec3{50.0f, 0.0f, 0.0f};
    es[3].health = 5.0f; // worst, but well out of range

    const std::span<const Enemy> span{es};
    CHECK(most_wounded_ally(es[0], span, kHealerRange) == 2); // the badly-wounded in-range ally
    // Mend that one to full -> it now picks the lightly-wounded one instead.
    es[2].health = enemy_max_health(es[2].kind);
    CHECK(most_wounded_ally(es[0], span, kHealerRange) == 1);
    // Everyone in range at full health -> nothing to mend.
    es[1].health = enemy_max_health(es[1].kind);
    CHECK(most_wounded_ally(es[0], span, kHealerRange) == -1);
    // It never targets itself, even when it's the only wounded thing nearby.
    es[1].position = Vec3{100.0f, 0.0f, 0.0f}; // push the only other wounded out of range
    es[1].health = 1.0f;
    CHECK(most_wounded_ally(es[0], span, kHealerRange) == -1);
}

TEST_CASE("Combat: a heavy blow sunders a shield-bearer's guard, which then recovers") {
    Enemy e;
    e.kind = kEnemyShield;
    e.position = Vec3{0.0f, 0.0f, 0.0f};
    e.yaw = 0.0f; // faces +x
    const Vec3 front{3.0f, 0.0f, 0.0f};

    CHECK(enemy_blocks_hit(e, front)); // normally blocks a frontal hit
    e.sunder_cd = kSunderDuration;     // a heavy blow staggered it
    CHECK_FALSE(enemy_blocks_hit(e, front)); // guard broken -> the front no longer blocks

    // Stepping the enemy recovers the guard after kSunderDuration.
    const DensitySampler density = flat_ground();
    const std::span<const Collider> none{};
    const Vec3 goal = e.position;
    for (f32 t = 0.0f; t < kSunderDuration + 0.1f; t += 1.0f / 60.0f) {
        step_enemy(e, density, none, goal, Timestep{1.0f / 60.0f});
    }
    CHECK(e.sunder_cd <= 0.0f);
    CHECK(enemy_blocks_hit(e, front)); // guard restored -> blocks again

    // The sunder threshold is set above basic attacks (so they don't trivially break the shield).
    CHECK(kSunderThreshold > kMeleeDamage);
}

TEST_CASE("Combat: a brute's slam is a radial AoE you can dodge out of") {
    const Vec3 brute{0.0f, 0.0f, 0.0f};
    CHECK(brute_slam_hits(brute, Vec3{2.0f, 0.0f, 0.0f}));                      // inside the ring
    CHECK(brute_slam_hits(brute, Vec3{0.0f, 5.0f, 3.0f}));                      // inside (y ignored)
    CHECK_FALSE(brute_slam_hits(brute, Vec3{kSlamRadius + 1.0f, 0.0f, 0.0f})); // dodged clear
    CHECK_FALSE(brute_slam_hits(brute, Vec3{0.0f, 0.0f, kSlamRadius + 0.5f})); // dodged clear
    // The slam reaches wider than a single melee swing (so it threatens a clustered party)...
    CHECK(kSlamRadius > kEnemyAttackRange);
    // ...but it telegraphs first (time to react) and hits harder than a normal swing.
    CHECK(kSlamWindup > 0.3f);
    CHECK(kSlamDamage > kEnemyAttackDamage);
}

TEST_CASE("Combat: the archer's aimed shot is heavy, telegraphed (dodgeable), and fast") {
    CHECK(kAimWindup > 0.3f);                     // telegraphs first -> time to read + dodge / break line
    CHECK(kAimedShotDamage > kEnemyAttackDamage);  // a heavy sniper hit (vs a basic melee swing)
    CHECK(kAimedArrowSpeed > 0.0f);                // and fast once it's loosed
}

TEST_CASE("Combat: the lone last raider enrages (faster + harder), but a brute never does") {
    CHECK(is_enraged(1, 0));            // the last grunt -> berserk
    CHECK(is_enraged(1, kEnemyShield)); // the last shield-bearer -> berserk
    CHECK(is_enraged(1, 3));            // the last archer -> berserk
    CHECK_FALSE(is_enraged(2, 0));      // more than one still up -> not yet
    CHECK_FALSE(is_enraged(5, 0));
    CHECK_FALSE(is_enraged(1, 2));      // a brute never enrages (already a slow, tough wall)
    CHECK(kEnrageMult > 1.0f);          // it really is a boost
}

TEST_CASE("Combat: a sapper is a fragile, fast bomber that hits the cargo hard") {
    CHECK(enemy_max_health(kEnemySapper) < enemy_max_health(0)); // fragile -> intercept it fast
    CHECK(kSapperSpeed > kEnemySpeed);                           // it rushes the cart
    CHECK(kSapperDamage > kEnemyAttackDamage);                   // a heavy detonation vs a basic hit
    CHECK(kSapperBlastRadius > 0.0f);                            // and a small player blast
    CHECK(kSapperBlastDamage > 0.0f);
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
