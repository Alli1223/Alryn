#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <Alryn/Net/ByteBuffer.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/World/VehicleTypes.h>

using namespace alryn;
using namespace alryn::net;

TEST_CASE("Net: message serialization round-trips") {
    PlayerInput input;
    input.sequence = 42;
    input.move = Vec3{1.0f, 0.0f, -1.0f};
    input.yaw = 1.2f;
    input.pitch = -0.3f;
    input.jump = true;
    input.add = true;
    input.attack = true;
    input.build = true;
    input.rally = true;
    input.grab = true;
    input.aim = Vec3{3.0f, 4.0f, 5.0f};
    input.vote_wagon = 12345u;
    input.vote_mode = 2;
    input.throttle = 1.0f;
    input.steer = -1.0f;
    input.role = static_cast<u8>(PlayerRole::Hunter);
    input.ability = 2; // casting ability slot 2 (key 2)
    input.block = true;
    input.appearance = CharacterAppearance{3, 5, EyeStyle::Sleepy, EarStyle::Pointed,
                                           HairStyle::Ponytail};

    ByteWriter w;
    write(w, input);
    ByteReader r(w.bytes(), w.size());
    PlayerInput out;
    REQUIRE(read(r, out));
    CHECK(out.sequence == 42);
    CHECK(out.move.x == doctest::Approx(1.0f));
    CHECK(out.move.z == doctest::Approx(-1.0f));
    CHECK(out.jump);
    CHECK_FALSE(out.dig);
    CHECK(out.add);
    CHECK(out.attack);
    CHECK(out.build);
    CHECK(out.rally);
    CHECK(out.aim.z == doctest::Approx(5.0f));
    CHECK(out.grab);
    CHECK(out.vote_wagon == 12345u);
    CHECK(out.vote_mode == 2);
    CHECK(out.throttle == doctest::Approx(1.0f));
    CHECK(out.steer == doctest::Approx(-1.0f));
    CHECK(out.role == static_cast<u8>(PlayerRole::Hunter));
    CHECK(out.ability == 2);
    CHECK(out.block);
    CHECK(out.appearance == input.appearance); // cosmetics survive the round-trip

    Snapshot snapshot;
    snapshot.tick = 7;
    snapshot.time_of_day = 0.625f;
    snapshot.outcome = static_cast<u8>(MatchOutcome::Lost);
    snapshot.phase = static_cast<u8>(MatchPhase::Combat);
    snapshot.phase_timer = 7.5f;
    snapshot.wave = 4;
    snapshot.houses_standing = 9;
    snapshot.houses_total = 12;
    snapshot.players.push_back(
        {1, Vec3{1.0f, 2.0f, 3.0f}, 0.5f, 100, 8, 0, 0, 0, 0, 2, 0, CharacterAppearance{}}); // Knight blocking
    snapshot.players.push_back(
        {2, Vec3{4.0f, 5.0f, 6.0f}, 1.5f, 73, 3, 1, 1, 2, 3, 0, 128, // seated+carrying, Cleric, slot3, shielded
         CharacterAppearance{2, 0, EyeStyle::Sharp, EarStyle::Small, HairStyle::Spiky}});
    snapshot.enemies.push_back({40u, Vec3{7.0f, 1.0f, -2.0f}, 0.8f, 1, 200, 1}); // swinging
    snapshot.enemies.push_back({41u, Vec3{9.0f, 1.5f, -4.0f}, 2.0f, 0, 60, 0});
    snapshot.villagers.push_back(
        {900u, Vec3{2.0f, 0.5f, 8.0f}, 1.1f, 40, 0, 0,
         CharacterAppearance{4, 1, EyeStyle::Round, EarStyle::Pointed, HairStyle::Ponytail}});
    snapshot.villagers.push_back(
        {901u, Vec3{3.0f, 0.5f, 9.0f}, 0.2f, 200, 1, 90, CharacterAppearance{}}); // a guard (kind 1), shielded
    snapshot.fires.push_back({Vec3{10.0f, 1.0f, 12.0f}, 0.7f, 180});
    snapshot.barricades.push_back({Vec3{5.0f, 0.3f, -7.0f}, 1.2f, 240});
    snapshot.money = 1234u;
    snapshot.contract_phase = static_cast<u8>(ContractPhase::Active);
    snapshot.contract_outcome = 1;
    snapshot.wagons.push_back({55u, Vec3{2.0f, 0.5f, 3.0f}, 0.9f, Vec3{40.0f, 0.0f, 60.0f},
                               Vec3{6.0f, 0.5f, 4.0f}, 1.2f, 500u, 2 /*carriage*/, 200,
                               static_cast<u8>(WagonMode::Manual), 3, 1, 1 /*has_horse*/,
                               4 /*aboard*/, 6 /*total*/});
    snapshot.goods.push_back({77u, Vec3{12.0f, 1.0f, 13.0f}, 1});  // a fallen crate (loose)
    snapshot.goods.push_back({78u, Vec3{0.2f, 0.55f, -0.1f}, 0}); // a crate in the bed (local pos)
    snapshot.auras.push_back({Vec3{3.0f, 1.0f, -5.0f}, 5.0f, 0}); // a Cleric heal aura
    ByteWriter ws;
    write(ws, snapshot);
    ByteReader rs(ws.bytes(), ws.size());
    Snapshot decoded;
    REQUIRE(read(rs, decoded));
    CHECK(decoded.tick == 7);
    REQUIRE(decoded.players.size() == 2);
    CHECK(decoded.players[1].id == 2);
    CHECK(decoded.players[1].position.y == doctest::Approx(5.0f));
    CHECK(decoded.players[1].appearance.hair == HairStyle::Spiky);
    CHECK(decoded.players[1].appearance.eyes == EyeStyle::Sharp);
    CHECK(decoded.players[0].health == 100);
    CHECK(decoded.players[1].health == 73);
    CHECK(decoded.players[0].build_stock == 8);
    CHECK(decoded.players[1].build_stock == 3);
    CHECK(decoded.players[0].role == 0); // Knight
    CHECK(decoded.players[1].role == 2); // Cleric
    CHECK(decoded.players[0].cast == 0);
    CHECK(decoded.players[1].cast == 3); // casting ability slot 3
    CHECK(decoded.players[0].action == 2); // blocking
    CHECK(decoded.players[1].action == 0);
    CHECK(decoded.players[0].shield == 0);
    CHECK(decoded.players[1].shield == 128); // Aegis shielded
    CHECK(decoded.enemies[0].action == 1); // swinging
    CHECK(decoded.enemies[1].action == 0);
    CHECK(decoded.time_of_day == doctest::Approx(0.625f));
    CHECK(decoded.outcome == static_cast<u8>(MatchOutcome::Lost));
    CHECK(decoded.phase == static_cast<u8>(MatchPhase::Combat));
    CHECK(decoded.phase_timer == doctest::Approx(7.5f));
    CHECK(decoded.wave == 4);
    CHECK(decoded.houses_standing == 9);
    CHECK(decoded.houses_total == 12);
    REQUIRE(decoded.fires.size() == 1);
    CHECK(decoded.fires[0].intensity == 180);
    CHECK(decoded.fires[0].position.z == doctest::Approx(12.0f));
    REQUIRE(decoded.enemies.size() == 2);
    CHECK(decoded.enemies[0].id == 40u);
    CHECK(decoded.enemies[0].health == 200);
    CHECK(decoded.enemies[1].position.x == doctest::Approx(9.0f));
    CHECK(decoded.enemies[1].kind == 0);
    REQUIRE(decoded.villagers.size() == 2);
    CHECK(decoded.villagers[0].id == 900u);
    CHECK(decoded.villagers[0].health == 40);
    CHECK(decoded.villagers[0].position.z == doctest::Approx(8.0f));
    CHECK(decoded.villagers[0].appearance.hair == HairStyle::Ponytail);
    CHECK(decoded.villagers[0].kind == 0);          // a villager
    CHECK(decoded.villagers[1].kind == 1);          // a guard
    CHECK(decoded.villagers[0].shield == 0);
    CHECK(decoded.villagers[1].shield == 90);       // Aegis shielded NPC
    CHECK(decoded.villagers[1].health == 200);
    REQUIRE(decoded.barricades.size() == 1);
    CHECK(decoded.barricades[0].health == 240);
    CHECK(decoded.barricades[0].position.x == doctest::Approx(5.0f));
    CHECK(decoded.barricades[0].yaw == doctest::Approx(1.2f));
    CHECK(decoded.money == 1234u);
    CHECK(decoded.contract_phase == static_cast<u8>(ContractPhase::Active));
    CHECK(decoded.contract_outcome == 1);
    REQUIRE(decoded.wagons.size() == 1);
    CHECK(decoded.wagons[0].id == 55u);
    CHECK(decoded.wagons[0].reward == 500u);
    CHECK(decoded.wagons[0].dest.z == doctest::Approx(60.0f));
    CHECK(decoded.wagons[0].mode == static_cast<u8>(WagonMode::Manual));
    CHECK(decoded.wagons[0].difficulty == 3);
    CHECK(decoded.wagons[0].type == 2);            // carriage
    CHECK(decoded.wagons[0].has_horse == 1);
    CHECK(decoded.wagons[0].horse_pos.x == doctest::Approx(6.0f));
    CHECK(decoded.wagons[0].horse_yaw == doctest::Approx(1.2f));
    CHECK(decoded.wagons[0].goods_aboard == 4);
    CHECK(decoded.wagons[0].goods_total == 6);
    CHECK(decoded.players[0].seated == 0);
    CHECK(decoded.players[1].seated == 1);
    CHECK(decoded.players[0].carrying == 0);
    CHECK(decoded.players[1].carrying == 1);
    REQUIRE(decoded.goods.size() == 2);
    CHECK(decoded.goods[0].id == 77u);
    CHECK(decoded.goods[0].loose == 1);
    CHECK(decoded.goods[0].position.z == doctest::Approx(13.0f));
    CHECK(decoded.goods[1].id == 78u);
    CHECK(decoded.goods[1].loose == 0); // a crate riding in the bed
    REQUIRE(decoded.auras.size() == 1);
    CHECK(decoded.auras[0].radius == doctest::Approx(5.0f));
    CHECK(decoded.auras[0].position.z == doctest::Approx(-5.0f));

    Welcome welcome{77, 0xABCDu};
    ByteWriter ww;
    write(ww, welcome);
    ByteReader rw(ww.bytes(), ww.size());
    Welcome welcome_out;
    REQUIRE(read(rw, welcome_out));
    CHECK(welcome_out.your_id == 77);
    CHECK(welcome_out.seed == 0xABCDu);

    // A truncated buffer fails safely rather than reading garbage.
    ByteReader truncated(w.bytes(), 3);
    PlayerInput partial;
    read(truncated, partial);
    CHECK_FALSE(truncated.ok());
}

// Drives a real ENet client + server over localhost through the whole pipeline:
// connect -> welcome -> input -> snapshot. Skips if the socket can't bind.
//
// Note: ENet dispatches the server's CONNECT event one round-trip AFTER the
// client connects, so we keep servicing both sides and handle every event in a
// single pump rather than stopping the moment the client reports connected.
TEST_CASE("Net: client/server loopback exchange over localhost") {
    const u16 port = 24655;
    NetServer server;
    if (!server.start(port)) {
        MESSAGE("Could not bind server (port busy / no sockets) - skipping loopback test");
        return;
    }

    NetClient client;
    REQUIRE(client.connect("127.0.0.1", port));

    PlayerId assigned = 0;
    bool server_saw_connect = false;
    bool got_welcome = false;
    Welcome welcome{};
    bool got_input = false;
    PlayerInput received{};
    bool got_snapshot = false;
    Snapshot snapshot_out{};

    auto pump = [&](int iterations) {
        for (int i = 0; i < iterations; ++i) {
            for (const ServerEvent& e : server.poll(2)) {
                if (e.type == ServerEventType::ClientConnected) {
                    server_saw_connect = true;
                    assigned = e.client;
                    server.send_welcome(e.client, Welcome{e.client, 12345u});
                } else if (e.type == ServerEventType::InputReceived) {
                    got_input = true;
                    received = e.input;
                }
            }
            for (const ClientEvent& e : client.poll(2)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    got_welcome = true;
                    welcome = e.welcome;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    got_snapshot = true;
                    snapshot_out = e.snapshot;
                }
            }
        }
    };

    // Connect + welcome.
    pump(100);
    REQUIRE(client.connected());
    CHECK(server_saw_connect);
    CHECK(server.client_count() == 1);
    REQUIRE(got_welcome);
    CHECK(welcome.your_id == assigned);
    CHECK(welcome.seed == 12345u);

    // Client input reaches the server.
    PlayerInput input;
    input.sequence = 99;
    input.move = Vec3{1.0f, 0.0f, 0.0f};
    input.jump = true;
    client.send_input(input);
    pump(100);
    REQUIRE(got_input);
    CHECK(received.sequence == 99);
    CHECK(received.jump);

    // Server snapshot reaches the client.
    Snapshot snapshot;
    snapshot.tick = 7;
    snapshot.players.push_back({assigned, Vec3{1.0f, 2.0f, 3.0f}, 0.5f});
    server.broadcast_snapshot(snapshot);
    pump(100);
    REQUIRE(got_snapshot);
    CHECK(snapshot_out.tick == 7);
    REQUIRE(snapshot_out.players.size() == 1);
    CHECK(snapshot_out.players[0].id == assigned);
    CHECK(snapshot_out.players[0].position.y == doctest::Approx(2.0f));
}

// The full server-authoritative loop: two clients join a GameServer, the server
// simulates both, and each client's snapshot shows the other player moving.
TEST_CASE("GameServer: two clients join and see each other move") {
    GameServer server;
    if (!server.start(24656, 777u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }

    NetClient a;
    NetClient b;
    REQUIRE(a.connect("127.0.0.1", 24656));
    REQUIRE(b.connect("127.0.0.1", 24656));

    PlayerId a_id = 0;
    PlayerId b_id = 0;
    Snapshot b_snapshot{};
    bool b_has_snapshot = false;

    auto pump = [&](int iterations, const PlayerInput* a_input) {
        for (int i = 0; i < iterations; ++i) {
            if (a_input != nullptr && a_id != 0) {
                a.send_input(*a_input);
            }
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : a.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    a_id = e.welcome.your_id;
                }
            }
            for (const ClientEvent& e : b.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    b_id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    b_snapshot = e.snapshot;
                    b_has_snapshot = true;
                }
            }
        }
    };

    pump(150, nullptr); // connect + settle on the ground
    REQUIRE(a_id != 0);
    REQUIRE(b_id != 0);
    CHECK(a_id != b_id);
    CHECK(server.player_count() == 2);
    REQUIRE(b_has_snapshot);

    // b's snapshot lists both players.
    bool sees_a = false;
    bool sees_b = false;
    f32 a_x_before = 0.0f;
    for (const PlayerState& p : b_snapshot.players) {
        if (p.id == a_id) {
            sees_a = true;
            a_x_before = p.position.x;
        }
        if (p.id == b_id) {
            sees_b = true;
        }
    }
    CHECK(sees_a);
    CHECK(sees_b);

    // a walks +x; b should observe a's position change (server-authoritative).
    PlayerInput walk;
    walk.move = Vec3{1.0f, 0.0f, 0.0f};
    pump(150, &walk);

    f32 a_x_after = a_x_before;
    for (const PlayerState& p : b_snapshot.players) {
        if (p.id == a_id) {
            a_x_after = p.position.x;
        }
    }
    CHECK(a_x_after > a_x_before + 0.5f);
}

// The transport game has no night siege (it's preserved but dormant in
// Combat/SiegeMode.cpp). A player joins in a town: the day/night clock advances and
// rides in the snapshot, peaceful townsfolk populate the town, and no enemies / fires
// ever spawn.
TEST_CASE("GameServer: no siege - players join, peaceful townsfolk, nothing attacks") {
    GameServer server;
    if (!server.start(24657, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }

    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24657));

    PlayerId id = 0;
    Snapshot snap{};
    bool have_snap = false;
    auto pump = [&](int iterations) {
        for (int i = 0; i < iterations; ++i) {
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                    have_snap = true;
                }
            }
        }
    };

    pump(120); // connect + settle on the ground
    REQUIRE(id != 0);
    REQUIRE(have_snap);
    const f32 t0 = snap.time_of_day;

    // No combat anywhere (siege is dormant): no enemies, no house fires.
    CHECK(server.enemy_count() == 0);
    CHECK(server.house_count() == 0);
    CHECK(snap.enemies.empty());
    CHECK(snap.fires.empty());
    CHECK(snap.outcome == static_cast<u8>(MatchOutcome::Ongoing));

    // Peaceful townsfolk populate the town the player spawned in (kind 0); a garrisoned town
    // also posts kind-2 archer guards up on the walls (elevated above the ground).
    CHECK(server.villager_count() > 0);
    CHECK(snap.villagers.size() == server.villager_count());
    int wall_guards = 0;
    for (const VillagerState& vs : snap.villagers) {
        CHECK((vs.kind == 0 || vs.kind == 2));
        if (vs.kind == 2) {
            ++wall_guards;
            // Standing up on the wall, well above the terrain at their spot (not on the street).
            const f32 g = worldgen::height(vs.position.x, vs.position.z, 4242u);
            CHECK(vs.position.y > g + 1.5f);
        }
    }
    const std::string gmsg = "wall guards on the spawn town: " + std::to_string(wall_guards);
    MESSAGE(gmsg);

    // Run a long while (across what used to be dusk): still no enemy/fire siege spawns.
    pump(600);
    CHECK(server.enemy_count() == 0);
    CHECK(server.house_count() == 0);

    // The day/night clock advanced, server-authoritative, mirrored in the snapshot.
    CHECK(snap.time_of_day != doctest::Approx(t0));
}

// The wagon-transport loop: a player in a town is offered wagons, votes one (solo =
// instant consensus), and once accepted the cargo is networked + ambushers spawn.
TEST_CASE("GameServer: a wagon contract is offered, accepted by vote, and ambushed") {
    GameServer server;
    if (!server.start(24658, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24658));

    PlayerId id = 0;
    Snapshot snap{};
    bool have_snap = false;
    PlayerInput intent{}; // the intent we resend each pump (votes ride in here)
    auto pump = [&](int iterations) {
        for (int i = 0; i < iterations; ++i) {
            c.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                    have_snap = true;
                }
            }
        }
    };

    pump(150); // connect + spawn in a town + receive the offers
    REQUIRE(id != 0);
    REQUIRE(have_snap);
    if (server.offer_count() == 0) {
        MESSAGE("Spawn town has no road-connected neighbour - skipping contract flow");
        return;
    }
    CHECK(server.contract_phase() == ContractPhase::Offer);
    REQUIRE_FALSE(snap.wagons.empty());
    CHECK(snap.money == 0u);

    // Vote for the first offered wagon, hiring a driver. Solo -> consensus is immediate.
    intent.vote_wagon = snap.wagons[0].id;
    intent.vote_mode = 1;
    pump(20);
    CHECK(server.contract_phase() == ContractPhase::Active);
    REQUIRE_FALSE(snap.wagons.empty());
    CHECK(snap.wagons[0].mode == static_cast<u8>(WagonMode::Driver));
    // The networked horse flag matches the accepted vehicle's type (seed-independent):
    // only horse-drawn types (carriages) ride with a horse.
    const bool horse_drawn = vehicle_type(snap.wagons[0].type).horse_drawn();
    CHECK(snap.wagons[0].has_horse == (horse_drawn ? 1 : 0));

    // Once the cargo has driven clear of the town walls, ambushers spawn + are networked
    // (in the enemy list). The hired driver is slow, so poll until it has left town.
    for (int t = 0; t < 40 && server.ambusher_count() == 0; ++t) {
        pump(60);
    }
    CHECK(server.ambusher_count() > 0);
    CHECK_FALSE(snap.enemies.empty());
}

// Hiring a horse-drawn carriage puts a horse out front pulling and a driver seated up top.
// Carriages are offered only on long routes, so we scan a few seeds until one offers a
// carriage, then accept it with a hired driver and check the horse + seated driver are
// networked. Skips (rather than fails) if no scanned seed offers a carriage.
TEST_CASE("GameServer: hiring a carriage spawns a pulling horse + a seated driver") {
    bool tested = false;
    for (u32 attempt = 0; attempt < 12 && !tested; ++attempt) {
        GameServer server;
        const u16 port = static_cast<u16>(24670 + attempt);
        if (!server.start(port, 7000u + attempt * 131u)) {
            continue; // port busy - try the next
        }
        NetClient c;
        if (!c.connect("127.0.0.1", port)) {
            continue;
        }
        PlayerId id = 0;
        Snapshot snap{};
        PlayerInput intent{};
        auto pump = [&](int iterations) {
            for (int i = 0; i < iterations; ++i) {
                c.send_input(intent);
                server.tick(Timestep{1.0f / 60.0f});
                for (const ClientEvent& e : c.poll(1)) {
                    if (e.type == ClientEventType::WelcomeReceived) {
                        id = e.welcome.your_id;
                    } else if (e.type == ClientEventType::SnapshotReceived) {
                        snap = e.snapshot;
                    }
                }
            }
        };
        pump(150);
        if (id == 0 || server.contract_phase() != ContractPhase::Offer) {
            continue;
        }
        // Find a carriage among the offers (a horse-drawn vehicle type).
        u32 carriage_id = 0;
        for (const WagonState& w : snap.wagons) {
            if (vehicle_type(w.type).horse_drawn()) {
                carriage_id = w.id;
                break;
            }
        }
        if (carriage_id == 0) {
            continue; // this seed's town offers no carriage - try another
        }

        // Hire a driver for the carriage (solo -> consensus is immediate).
        intent.vote_wagon = carriage_id;
        intent.vote_mode = 1;
        pump(20);
        REQUIRE(server.contract_phase() == ContractPhase::Active);
        REQUIRE_FALSE(snap.wagons.empty());
        const WagonState& wg = snap.wagons[0];
        CHECK(vehicle_type(wg.type).horse_drawn());
        CHECK(wg.has_horse == 1);
        // The horse is placed out in front (away from the wagon body).
        const f32 horse_gap = glm::length(wg.horse_pos - wg.position);
        CHECK(horse_gap > 1.0f);
        // A hired carriage driver rides up top, networked as a seated villager (kind 3).
        bool seated_driver = false;
        for (const VillagerState& v : snap.villagers) {
            if (v.kind == 3) {
                seated_driver = true;
            }
        }
        CHECK(seated_driver);
        tested = true;
    }
    if (!tested) {
        MESSAGE("No scanned seed offered a carriage - skipping carriage horse/driver check");
    }
}

// Regression: the hired puller (teamster) should walk a smooth route, never stepping
// backward and forward on a node. We sample its motion en route and assert it never sharply
// reverses direction frame-to-frame (the old A* snapped to the start cell centre on every
// repath, causing that jitter).
TEST_CASE("GameServer: a hired puller follows the route without back-and-forth jitter") {
    GameServer server;
    if (!server.start(24668, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24668));

    PlayerId id = 0;
    Snapshot snap{};
    PlayerInput intent{};
    auto pump = [&](int iterations) {
        for (int i = 0; i < iterations; ++i) {
            c.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                }
            }
        }
    };
    pump(150);
    REQUIRE(id != 0);
    if (server.offer_count() == 0) {
        MESSAGE("Spawn town has no road-connected neighbour - skipping puller jitter check");
        return;
    }
    // Hire a driver for the first offer (solo -> immediate consensus).
    intent.vote_wagon = snap.wagons[0].id;
    intent.vote_mode = 1;
    pump(20);
    REQUIRE(server.contract_phase() == ContractPhase::Active);

    // Find the puller's position each tick: the teamster (villager kind 2) for a cart/wagon,
    // or the horse for a carriage. Track consecutive movement directions and count sharp
    // reversals (a near-180-degree flip = the back-and-forth jitter).
    auto puller_pos = [&](bool& ok) -> Vec3 {
        for (const VillagerState& v : snap.villagers) {
            if (v.kind == 2 || v.kind == 3) { // teamster (walking) or seated driver
                ok = true;
                return v.position;
            }
        }
        if (!snap.wagons.empty() && snap.wagons[0].has_horse) {
            ok = true;
            return snap.wagons[0].horse_pos;
        }
        ok = false;
        return Vec3{0.0f};
    };

    Vec3 prev{0.0f};
    Vec3 prev_dir{0.0f};
    bool have_prev = false;
    int samples = 0;
    int reversals = 0;
    for (int t = 0; t < 600; ++t) {
        pump(1);
        if (server.contract_phase() != ContractPhase::Active) {
            break; // delivered / wrecked
        }
        bool ok = false;
        const Vec3 p = puller_pos(ok);
        if (!ok) {
            continue;
        }
        if (have_prev) {
            Vec3 step = p - prev;
            step.y = 0.0f;
            if (glm::length(step) > 0.02f) { // only count real motion
                const Vec3 dir = glm::normalize(step);
                if (glm::length(prev_dir) > 0.0f && glm::dot(dir, prev_dir) < -0.5f) {
                    ++reversals;
                }
                prev_dir = dir;
                ++samples;
            }
        }
        prev = p;
        have_prev = true;
    }
    if (samples < 20) {
        MESSAGE("Puller didn't travel enough to judge smoothness - skipping");
        return;
    }
    // A smooth route has no sharp reversals; allow a tiny margin for a genuine hairpin.
    CHECK(reversals <= 1);
}

// An accepted contract starts with a full bed of cargo crates, all networked (in-bed, none
// loose on the ground yet).
TEST_CASE("GameServer: an accepted contract starts fully loaded with goods") {
    GameServer server;
    if (!server.start(24671, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24671));
    PlayerId id = 0;
    Snapshot snap{};
    PlayerInput intent{};
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            c.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                }
            }
        }
    };
    pump(150);
    REQUIRE(id != 0);
    if (server.offer_count() == 0) {
        MESSAGE("Spawn town has no road-connected neighbour - skipping load check");
        return;
    }
    intent.vote_wagon = snap.wagons[0].id;
    intent.vote_mode = 1;
    pump(20);
    REQUIRE(server.contract_phase() == ContractPhase::Active);
    REQUIRE_FALSE(snap.wagons.empty());
    const WagonState& wg = snap.wagons[0];
    CHECK(wg.goods_total > 0);
    CHECK(wg.goods_aboard == wg.goods_total);             // fully loaded at the depot
    CHECK(server.wagon_goods_aboard() == wg.goods_total); // crates in the bed
    CHECK(server.good_count() == 0);                      // none fallen out yet
    // The bed crates are networked (loose == 0); none loose on the ground.
    int in_bed = 0;
    int loose = 0;
    for (const GoodState& g : snap.goods) {
        (g.loose != 0 ? loose : in_bed) += 1;
    }
    CHECK(in_bed == wg.goods_total);
    CHECK(loose == 0);
}

// Driving a carriage hard in a circle slides the physical cargo around the bed, but the SOLID
// walls keep it aboard on flat ground - it doesn't spill just from acceleration / hard turns.
// Scans seeds for a carriage, walks the player to it, takes the reins, then circles. Skips if
// no carriage can be reached (degrades gracefully, never flakes).
TEST_CASE("GameServer: a carriage's walls keep the cargo in under hard driving") {
    bool tested = false;
    for (u32 attempt = 0; attempt < 12 && !tested; ++attempt) {
        GameServer server;
        const u16 port = static_cast<u16>(24672 + attempt);
        if (!server.start(port, 7000u + attempt * 131u)) {
            continue;
        }
        NetClient c;
        if (!c.connect("127.0.0.1", port)) {
            continue;
        }
        PlayerId id = 0;
        Snapshot snap{};
        PlayerInput intent{};
        auto pump = [&](int n) {
            for (int i = 0; i < n; ++i) {
                c.send_input(intent);
                server.tick(Timestep{1.0f / 60.0f});
                for (const ClientEvent& e : c.poll(1)) {
                    if (e.type == ClientEventType::WelcomeReceived) {
                        id = e.welcome.your_id;
                    } else if (e.type == ClientEventType::SnapshotReceived) {
                        snap = e.snapshot;
                    }
                }
            }
        };
        auto me = [&]() -> const PlayerState* {
            for (const PlayerState& p : snap.players) {
                if (p.id == id) {
                    return &p;
                }
            }
            return nullptr;
        };
        pump(150);
        if (id == 0 || server.contract_phase() != ContractPhase::Offer) {
            continue;
        }
        u32 cid = 0;
        for (const WagonState& w : snap.wagons) {
            if (vehicle_type(w.type).horse_drawn()) {
                cid = w.id;
                break;
            }
        }
        if (cid == 0) {
            continue; // no carriage offered this seed
        }
        intent.vote_wagon = cid;
        intent.vote_mode = 2; // haul manually -> a carriage is driven from the top
        pump(20);
        if (server.contract_phase() != ContractPhase::Active || snap.wagons.empty()) {
            continue;
        }
        // Walk to the parked carriage and take the reins (E within grab range).
        bool piloting = false;
        for (int t = 0; t < 600 && !piloting; ++t) {
            const PlayerState* p = me();
            if (p == nullptr || snap.wagons.empty()) {
                pump(1);
                continue;
            }
            Vec3 d = snap.wagons[0].position - p->position;
            d.y = 0.0f;
            const f32 dist = glm::length(d);
            if (dist > 3.0f) {
                intent.move = d / dist;
                intent.grab = false;
            } else {
                intent.move = Vec3{0.0f};
                intent.grab = true; // board / take the reins
            }
            pump(1);
            intent.grab = false;
            const PlayerState* p2 = me();
            piloting = p2 != nullptr && p2->seated != 0;
        }
        if (!piloting) {
            continue; // couldn't reach the carriage this seed - try another
        }
        // Record the crates' starting bed-local positions, then drive a hard circle.
        const u8 loaded = snap.wagons.empty() ? 0 : snap.wagons[0].goods_aboard;
        std::vector<u32> ids;
        std::vector<Vec2> start;
        for (const GoodState& g : snap.goods) {
            if (g.loose == 0) {
                ids.push_back(g.id);
                start.push_back(Vec2{g.position.x, g.position.z});
            }
        }
        intent.move = Vec3{0.0f};
        intent.throttle = 1.0f;
        intent.steer = 1.0f;
        for (int t = 0; t < 300; ++t) {
            pump(1);
        }
        // The crates slid around the bed...
        f32 max_move = 0.0f;
        for (const GoodState& g : snap.goods) {
            if (g.loose != 0) {
                continue;
            }
            for (usize k = 0; k < ids.size(); ++k) {
                if (ids[k] == g.id) {
                    max_move = std::max(max_move, glm::length(Vec2{g.position.x, g.position.z} - start[k]));
                }
            }
        }
        CHECK(max_move > 0.1f); // they really did slide (physics is live)
        // ...but the solid walls kept the whole load aboard (nothing spilled on flat ground).
        CHECK(server.good_count() == 0);
        CHECK(server.wagon_goods_aboard() == loaded);
        tested = true;
    }
    if (!tested) {
        MESSAGE("No carriage could be reached to drive - skipping cargo-containment check");
    }
}

// A player who walks straight at a parked wagon is blocked by it - they can't pass through to
// its centre.
TEST_CASE("GameServer: players are blocked from walking through a wagon") {
    GameServer server;
    if (!server.start(24690, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24690));
    PlayerId id = 0;
    Snapshot snap{};
    PlayerInput intent{};
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            c.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                }
            }
        }
    };
    auto me = [&]() -> const PlayerState* {
        for (const PlayerState& p : snap.players) {
            if (p.id == id) {
                return &p;
            }
        }
        return nullptr;
    };
    pump(150);
    REQUIRE(id != 0);
    if (server.offer_count() == 0 || snap.wagons.empty()) {
        MESSAGE("Spawn town has no parked wagons - skipping collision check");
        return;
    }
    // Aim at the nearest parked wagon and walk straight into it for a few seconds.
    const PlayerState* p0 = me();
    REQUIRE(p0 != nullptr);
    Vec3 target = snap.wagons[0].position;
    f32 best = 1e9f;
    for (const WagonState& w : snap.wagons) {
        const f32 d = glm::length(w.position - p0->position);
        if (d < best) {
            best = d;
            target = w.position;
        }
    }
    f32 min_dist = 1e9f;
    for (int t = 0; t < 400; ++t) {
        const PlayerState* p = me();
        if (p == nullptr) {
            pump(1);
            continue;
        }
        Vec3 d = target - p->position;
        d.y = 0.0f;
        const f32 dist = glm::length(d);
        const f32 flat = glm::length(Vec2{p->position.x - target.x, p->position.z - target.z});
        if (flat < min_dist) {
            min_dist = flat;
        }
        intent.move = dist > 0.1f ? d / dist : Vec3{0.0f};
        pump(1);
    }
    intent.move = Vec3{0.0f};
    // If the player reached the cart at all, the box collider must have stopped them well shy
    // of its centre (without collision they'd walk right onto it, min_dist ~0).
    if (min_dist < 1.7f) {
        CHECK(min_dist > 0.6f);
    } else {
        MESSAGE("Couldn't path to the wagon (blocked by town geometry) - collision not exercised");
    }
}

// The cargo crates collide with each other: as the cart accelerates from the depot they pile
// up, and the bed solver keeps them from overlapping (sliding through one another).
TEST_CASE("GameServer: cargo crates don't overlap each other in the bed") {
    GameServer server;
    if (!server.start(24691, 4242u)) {
        MESSAGE("Could not bind game server - skipping");
        return;
    }
    NetClient c;
    REQUIRE(c.connect("127.0.0.1", 24691));
    PlayerId id = 0;
    Snapshot snap{};
    PlayerInput intent{};
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            c.send_input(intent);
            server.tick(Timestep{1.0f / 60.0f});
            for (const ClientEvent& e : c.poll(1)) {
                if (e.type == ClientEventType::WelcomeReceived) {
                    id = e.welcome.your_id;
                } else if (e.type == ClientEventType::SnapshotReceived) {
                    snap = e.snapshot;
                }
            }
        }
    };
    pump(150);
    REQUIRE(id != 0);
    if (server.offer_count() == 0) {
        MESSAGE("Spawn town has no road-connected neighbour - skipping crate-overlap check");
        return;
    }
    intent.vote_wagon = snap.wagons[0].id;
    intent.vote_mode = 1; // hire a driver so the cart hauls itself out of town
    pump(20);
    REQUIRE(server.contract_phase() == ContractPhase::Active);

    // While the cart travels, the in-bed crates (loose == 0, positions are bed-local) must stay
    // separated. AABB crates of side 2*kCargoHalf overlap only if BOTH axes are within that.
    constexpr f32 full = 2.0f * kCargoHalf;
    f32 worst_overlap = 0.0f;
    int max_seen = 0;
    for (int t = 0; t < 240; ++t) {
        pump(1);
        std::vector<Vec3> bed;
        for (const GoodState& g : snap.goods) {
            if (g.loose == 0) {
                bed.push_back(g.position); // x, (floor y), z = bed-local
            }
        }
        max_seen = std::max(max_seen, static_cast<int>(bed.size()));
        for (usize i = 0; i < bed.size(); ++i) {
            for (usize j = i + 1; j < bed.size(); ++j) {
                const f32 ox = full - std::abs(bed[i].x - bed[j].x);
                const f32 oz = full - std::abs(bed[i].z - bed[j].z);
                if (ox > 0.0f && oz > 0.0f) {
                    worst_overlap = std::max(worst_overlap, std::min(ox, oz));
                }
            }
        }
    }
    REQUIRE(max_seen >= 2);        // a meaningful test needs at least two crates aboard
    CHECK(worst_overlap < 0.12f);  // never deeply interpenetrating (small solver residual only)
}
