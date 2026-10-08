// Side quests, charged heavy attacks and terrain deformation (Game/SideQuest.h, Roles.h heavy tuning,
// Terrain/WorldSampler.h): the pure formulas, the wire format, and the server flows end to end.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Game/SideQuest.h>
#include <Alryn/Net/ByteBuffer.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/Terrain/WorldSampler.h>

using namespace alryn;
using namespace alryn::net;

namespace {
// A connected test client + a pump that ticks the server and drains the client's events.
struct Harness {
    GameServer server;
    NetClient client;
    PlayerId id = 0;
    Snapshot snap{};
    bool have_snap = false;
    int deforms = 0;
    PlayerInput intent{};

    bool start(u16 port, u32 seed) { return server.start(port, seed) && client.connect("127.0.0.1", port); }
    void pump(int iterations) {
        for (int i = 0; i < iterations; ++i) {
            client.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : client.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                    have_snap = true;
                } else if (e.type == ClientEventType::DeformReceived) {
                    ++deforms;
                }
            }
        }
    }
    const GameServer::QuestRun* offered(QuestKind kind) const {
        for (const GameServer::QuestRun& q : server.quests()) {
            if (q.kind == kind && q.phase == QuestPhase::Offered) {
                return &q;
            }
        }
        return nullptr;
    }
    // Stand the hero at `p` (on the ground there) and let the move settle.
    void place(const Vec3& p) {
        server.debug_place_player(id, Vec3{p.x, worldgen::height(p.x, p.z, server.seed()) + 0.5f, p.z});
        pump(6);
    }
};
} // namespace

TEST_CASE("WorldEdits: the grid sums exactly what a brute-force pass over every edit would") {
    WorldEdits grid;
    std::vector<WorldEdit> all;
    u32 s = 12345u;
    auto rnd = [&](f32 lo, f32 hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<f32>(s >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 300; ++i) {
        const WorldEdit e{Vec3{rnd(-60.0f, 60.0f), rnd(-2.0f, 6.0f), rnd(-60.0f, 60.0f)}, rnd(0.5f, 4.0f), rnd(-2.0f, 2.0f)};
        grid.add(e);
        all.push_back(e);
    }
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p{rnd(-64.0f, 64.0f), rnd(-3.0f, 7.0f), rnd(-64.0f, 64.0f)};
        f32 brute = 0.0f;
        for (const WorldEdit& e : all) {
            const f32 d = glm::length(p - e.center);
            if (d < e.radius) {
                brute += e.amount * (1.0f - d / e.radius);
            }
        }
        REQUIRE(grid.sum(p) == doctest::Approx(brute).epsilon(1e-5));
    }
    // A chunk's overlapping() set holds every edit that can touch it (and the sampler agrees).
    const std::vector<WorldEdit> near = grid.overlapping(Vec2{0.0f}, Vec2{8.0f});
    for (const WorldEdit& e : all) {
        const bool reaches = e.center.x + e.radius >= 0.0f && e.center.x - e.radius <= 8.0f &&
                             e.center.z + e.radius >= 0.0f && e.center.z - e.radius <= 8.0f;
        const bool listed = std::any_of(near.begin(), near.end(), [&](const WorldEdit& n) {
            return n.center == e.center && n.radius == e.radius && n.amount == e.amount;
        });
        CHECK(reaches == listed);
    }
}

TEST_CASE("Heavy attacks + earthworks + side quests: the pure tuning holds together") {
    // A heavy always out-hits the basic, and a fuller charge hits harder.
    CHECK(heavy_damage_mult(0.0f) > 1.0f);
    CHECK(heavy_damage_mult(1.0f) > heavy_damage_mult(0.5f));
    CHECK(heavy_damage_mult(2.0f) == doctest::Approx(heavy_damage_mult(1.0f))); // clamped
    CHECK(drawn_shot_speed(1.0f) > drawn_shot_speed(0.0f));
    CHECK(comet_radius(1.0f) > comet_radius(0.0f));
    for (u8 r = 0; r < kRoleCount; ++r) {
        CHECK(heavy_charge_time(static_cast<PlayerRole>(r)) > kChargeTapTime);
        CHECK(basic_attack_cooldown(static_cast<PlayerRole>(r)) > 0.0f);
    }
    // A shallow scrape doesn't slow anyone; a real pit does, more the deeper it is (to a floor).
    CHECK(mire_mult(0.0f) == doctest::Approx(1.0f));
    CHECK(mire_mult(kMireDepth) == doctest::Approx(1.0f));
    CHECK(mire_mult(0.6f) < 1.0f);
    CHECK(mire_mult(1.2f) <= mire_mult(0.6f));
    CHECK(mire_mult(5.0f) == doctest::Approx(kMireSlow));
    // Wolves are beasts with their own health; a pack's alpha is the tough one.
    CHECK(is_beast(kEnemyWolf));
    CHECK(is_beast(kEnemyAlpha));
    CHECK_FALSE(is_beast(kEnemyWarlord));
    CHECK(enemy_max_health(kEnemyAlpha) > enemy_max_health(kEnemyWolf));
    // Quests: more danger, more pay; the goal is what the HUD counts to.
    for (u8 k = 0; k < kQuestKinds; ++k) {
        const auto kind = static_cast<QuestKind>(k);
        CHECK(quest_reward(kind, 3) > quest_reward(kind, 1));
        CHECK(quest_goal(kind, 2) >= 1);
        CHECK(std::string{quest_title(kind)}.size() > 3);
    }
    CHECK(quest_goal(QuestKind::Herbs, 1) == kHerbCount);
    CHECK(quest_goal(QuestKind::Treasure, 1) == kTreasureDigs);
    CHECK(quest_goal(QuestKind::BanditCamp, 2) == quest_foe_count(QuestKind::BanditCamp, 2));
}

TEST_CASE("Net: the heavy-attack, side-quest and status fields round-trip") {
    PlayerInput in;
    in.charge = 200;
    in.heavy_seq = 7;
    in.heavy_power = 255;
    in.quest_pick = 0xDEADBEEFu;
    in.quest_abandon = true;
    in.dig = true;
    ByteWriter w;
    write(w, in);
    ByteReader r(w.bytes(), w.size());
    PlayerInput out;
    REQUIRE(read(r, out));
    CHECK(out.charge == 200);
    CHECK(out.heavy_seq == 7);
    CHECK(out.heavy_power == 255);
    CHECK(out.quest_pick == 0xDEADBEEFu);
    CHECK(out.quest_abandon);
    CHECK(out.dig);

    Snapshot s;
    PlayerState p;
    p.id = 3;
    p.charge = 90;
    p.heavy_seq = 4;
    p.heavy_power = 250;
    s.players.push_back(p);
    s.enemies.push_back({9u, Vec3{1.0f}, 0.5f, kEnemyAlpha, 200, 4, static_cast<u8>(kStatusMired | kStatusQuest)});
    s.quests.push_back({11u, static_cast<u8>(QuestKind::Treasure), static_cast<u8>(QuestPhase::Active), 2, 3, 4, 240u,
                        Vec3{10.0f, 2.0f, -30.0f}, Vec3{1.0f, 0.5f, 2.0f}});
    s.quest_items.push_back({5u, Vec3{10.0f, 1.5f, -30.0f}, 1, 1});
    ByteWriter ws;
    write(ws, s);
    ByteReader rs(ws.bytes(), ws.size());
    Snapshot d;
    REQUIRE(read(rs, d));
    REQUIRE(d.players.size() == 1);
    CHECK(d.players[0].charge == 90);
    CHECK(d.players[0].heavy_seq == 4);
    CHECK(d.players[0].heavy_power == 250);
    REQUIRE(d.enemies.size() == 1);
    CHECK(d.enemies[0].kind == kEnemyAlpha);
    CHECK(d.enemies[0].action == 4);
    CHECK((d.enemies[0].status & kStatusMired) != 0);
    CHECK((d.enemies[0].status & kStatusQuest) != 0);
    REQUIRE(d.quests.size() == 1);
    CHECK(d.quests[0].id == 11u);
    CHECK(d.quests[0].kind == static_cast<u8>(QuestKind::Treasure));
    CHECK(d.quests[0].progress == 3);
    CHECK(d.quests[0].goal == 4);
    CHECK(d.quests[0].reward == 240u);
    CHECK(d.quests[0].site.z == doctest::Approx(-30.0f));
    CHECK(d.quests[0].board.x == doctest::Approx(1.0f));
    REQUIRE(d.quest_items.size() == 1);
    CHECK(d.quest_items[0].kind == 1);
    CHECK(d.quest_items[0].state == 1);
}

TEST_CASE("GameServer: a town's notice board posts side quests out in the wilds") {
    Harness h;
    if (!h.start(24781, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    h.pump(120);
    REQUIRE(h.id != 0);
    REQUIRE_FALSE(h.server.quests().empty());
    CHECK(h.server.quests().size() <= kQuestOffers);
    CHECK(h.server.active_quest() == nullptr);
    const u32 seed = h.server.seed();
    for (const GameServer::QuestRun& q : h.server.quests()) {
        CHECK(q.phase == QuestPhase::Offered);
        CHECK(q.reward == quest_reward(q.kind, q.danger));
        // Out in the wilds: on dry land, clear of every town, a good walk from the board.
        CHECK_FALSE(worldgen::inside_village(q.site.x, q.site.z, seed, 20.0f));
        CHECK(q.site.y > worldgen::water_level);
        CHECK(glm::length(Vec2{q.site.x - q.board.x, q.site.z - q.board.z}) > 40.0f);
    }
    // ...and every client sees the board.
    CHECK(h.snap.quests.size() == h.server.quests().size());
}

TEST_CASE("GameServer: a moonpetal harvest - pick every petal (E) and get paid") {
    // Find a world whose spawn town offers the harvest.
    for (u32 seed : {4242u, 777u, 31337u, 1337u, 99u, 2024u, 5150u, 8u}) {
        Harness h;
        if (!h.start(static_cast<u16>(24790 + seed % 7u), seed)) {
            continue;
        }
        h.pump(120);
        const GameServer::QuestRun* q = h.offered(QuestKind::Herbs);
        if (q == nullptr) {
            continue;
        }
        const u32 qid = q->id;
        const Vec3 site = q->site;
        const u32 reward = q->reward;
        h.server.set_debug_god(true); // the meadow's wolves can't end the test early
        REQUIRE(h.server.debug_accept_quest(qid));
        REQUIRE(h.server.active_quest() != nullptr);
        CHECK(h.server.quests().size() == 1); // the other offers came down
        h.place(site);                        // walking up wakes the meadow
        REQUIRE(h.server.active_quest() != nullptr);
        std::vector<Vec3> petals;
        for (const GameServer::QuestItem& it : h.server.quest_items()) {
            if (it.quest == qid && it.kind == 0u) {
                petals.push_back(it.position);
            }
        }
        REQUIRE(petals.size() == kHerbCount);
        const u32 money_before = h.server.money();
        for (const Vec3& p : petals) {
            h.place(p);
            h.intent.grab = true;
            h.pump(1);
            h.intent.grab = false;
            h.pump(1);
        }
        // Every petal picked: the quest is won (its banner up), the party paid.
        CHECK(h.server.active_quest() == nullptr);
        REQUIRE_FALSE(h.server.quests().empty());
        CHECK(h.server.quests().front().phase == QuestPhase::Complete);
        CHECK(h.server.money() >= money_before + reward);
        return;
    }
    FAIL("no test world offered a moonpetal harvest");
}

TEST_CASE("GameServer: buried treasure - dig at the X until the chest turns up, then open it") {
    for (u32 seed : {4242u, 777u, 31337u, 1337u, 99u, 2024u, 5150u, 8u}) {
        Harness h;
        if (!h.start(static_cast<u16>(24800 + seed % 7u), seed)) {
            continue;
        }
        h.pump(120);
        const GameServer::QuestRun* q = h.offered(QuestKind::Treasure);
        if (q == nullptr) {
            continue;
        }
        const u32 qid = q->id;
        const Vec3 site = q->site;
        h.server.set_debug_god(true);
        REQUIRE(h.server.debug_accept_quest(qid));
        h.place(site + Vec3{1.5f, 0.0f, 0.0f});
        const usize edits_before = h.server.terrain_edit_count();
        // Spade strikes on the X (paced by the dig cooldown) - each one really moves the earth.
        h.intent.aim = site;
        for (u8 k = 0; k < kTreasureDigs; ++k) {
            h.intent.dig = true;
            h.pump(1);
            h.intent.dig = false;
            h.pump(static_cast<int>(kDigCooldown * 60.0f) + 4);
        }
        CHECK(h.server.terrain_edit_count() >= edits_before + kTreasureDigs);
        CHECK(h.deforms >= static_cast<int>(kTreasureDigs)); // the client saw the ground give way
        const GameServer::QuestItem* chest = nullptr;
        for (const GameServer::QuestItem& it : h.server.quest_items()) {
            if (it.quest == qid && it.kind == 1u) {
                chest = &it;
            }
        }
        REQUIRE(chest != nullptr);
        CHECK(chest->state == 1u);                                // unearthed
        CHECK(h.server.active_quest()->progress == kTreasureDigs);
        CHECK_FALSE(h.snap.enemies.empty()); // the grave-robbers come running
        const u32 money_before = h.server.money();
        h.intent.grab = true;
        h.pump(1);
        h.intent.grab = false;
        h.pump(1);
        CHECK(h.server.active_quest() == nullptr);
        CHECK(h.server.money() > money_before);
        return;
    }
    FAIL("no test world offered buried treasure");
}

TEST_CASE("GameServer: the spade only bites in the wilds, within arm's reach; a late joiner gets the history") {
    Harness h;
    if (!h.start(24810, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    h.pump(120);
    REQUIRE(h.id != 0);
    REQUIRE_FALSE(h.server.quests().empty());
    // In town (where we spawn): the paving won't take a spade.
    const Vec3 feet = h.snap.players.front().position;
    h.intent.aim = feet + Vec3{1.0f, 0.0f, 0.0f};
    h.intent.dig = true;
    h.pump(1);
    h.intent.dig = false;
    h.pump(2);
    CHECK(h.server.terrain_edit_count() == 0u);
    // Out at a quest site: it does - but not at a spot across the field.
    const Vec3 site = h.server.quests().front().site;
    h.place(site);
    h.intent.aim = site + Vec3{40.0f, 0.0f, 0.0f};
    h.intent.dig = true;
    h.pump(1);
    h.intent.dig = false;
    h.pump(40);
    CHECK(h.server.terrain_edit_count() == 0u);
    h.intent.aim = site + Vec3{1.0f, 0.0f, 0.5f};
    h.intent.dig = true;
    h.pump(1);
    h.intent.dig = false;
    h.pump(40);
    REQUIRE(h.server.terrain_edit_count() == 1u);

    // A second hero joining now is caught up on that dig.
    NetClient late;
    REQUIRE(late.connect("127.0.0.1", 24810));
    int late_deforms = 0;
    for (int i = 0; i < 90; ++i) {
        h.client.send_input(h.intent);
        h.server.tick(Timestep{1.0f / 60.0f});
        h.client.poll(1);
        for (const ClientEvent& e : late.poll(1)) {
            late_deforms += e.type == ClientEventType::DeformReceived ? 1 : 0;
        }
    }
    CHECK(late_deforms == 1);
}

TEST_CASE("GameServer: a full-charge EARTHSPLITTER cleaves the foes ahead and craters the ground") {
    Harness h;
    if (!h.start(24812, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    h.intent.role = static_cast<u8>(PlayerRole::Knight);
    h.pump(120);
    REQUIRE(h.id != 0);
    REQUIRE_FALSE(h.server.quests().empty());
    const Vec3 site = h.server.quests().front().site; // out in the wilds (craters aren't dug in towns)
    h.place(site);
    const Vec3 feet = h.snap.players.front().position;
    const f32 yaw = 0.7f;
    // Three raiders standing still in a line just ahead.
    const u8 kinds[] = {0u, 0u, 0u};
    h.server.debug_spawn_bestiary(feet, yaw, kinds, /*frozen=*/true);
    h.intent.yaw = yaw;
    h.pump(4);
    REQUIRE(h.snap.enemies.size() == 3);
    const usize edits_before = h.server.terrain_edit_count();
    // Wind up (the charge shows on the wire) and let it go at full power.
    h.intent.charge = 255;
    h.pump(3);
    CHECK(h.snap.players.front().charge == 255);
    h.intent.charge = 0;
    h.intent.heavy_seq = 1;
    h.intent.heavy_power = 255;
    h.pump(30); // the blow lands kHeavyWindup after the release
    CHECK(h.snap.players.front().heavy_seq == 1); // every client is told it came down
    // Grunts (60 hp) can't stand a full-charge blow (42 x 3.6 + the quake).
    CHECK(h.snap.enemies.empty());
    CHECK(h.server.terrain_edit_count() == edits_before + 1); // the crater
    // The same seq again is the same release, not a new one: nothing more happens.
    const u8 kinds2[] = {2u};
    h.server.debug_spawn_bestiary(feet, yaw, kinds2, /*frozen=*/true);
    h.pump(30);
    REQUIRE(h.snap.enemies.size() == 1);
    CHECK(h.snap.enemies.front().health == 255);
}

TEST_CASE("GameServer: ranged heavies loose their own shots (a drawn arrow, a sunburst, a comet)") {
    Harness h;
    if (!h.start(24813, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    h.pump(60);
    REQUIRE(h.id != 0);
    struct Case {
        PlayerRole role;
        u8 kind;
    };
    const Case cases[] = {{PlayerRole::Hunter, 8}, {PlayerRole::Cleric, 9}, {PlayerRole::Mage, 10}};
    u8 seq = 0;
    for (const Case& c : cases) {
        h.intent.role = static_cast<u8>(c.role);
        h.pump(10);
        h.intent.aim = h.snap.players.front().position + Vec3{12.0f, 1.0f, 0.0f};
        h.intent.heavy_seq = ++seq;
        h.intent.heavy_power = 255;
        h.pump(5); // the release reaches the server, the shot flies, the snapshot comes back
        const bool loosed = std::any_of(h.snap.projectiles.begin(), h.snap.projectiles.end(),
                                        [&](const ProjectileState& pr) { return pr.kind == c.kind; });
        CHECK(loosed);
    }
    // And the basic attacks are paced: mashing fire every tick looses far fewer than one shot a tick.
    h.intent.role = static_cast<u8>(PlayerRole::Hunter);
    h.pump(30);
    const auto arrows = [&] {
        return std::count_if(h.snap.projectiles.begin(), h.snap.projectiles.end(),
                             [](const ProjectileState& pr) { return pr.kind == 3; });
    };
    const auto before = arrows();
    h.intent.fire = true;
    h.pump(30); // half a second of mashing
    h.intent.fire = false;
    const auto loosed = arrows() - before;
    CHECK(loosed >= 1);
    CHECK(loosed <= 3);
}
