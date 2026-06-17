#include <doctest/doctest.h>

#include <Alryn/Net/ByteBuffer.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>

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
    snapshot.players.push_back({1, Vec3{1.0f, 2.0f, 3.0f}, 0.5f, 100, 8, CharacterAppearance{}});
    snapshot.players.push_back(
        {2, Vec3{4.0f, 5.0f, 6.0f}, 1.5f, 73, 3,
         CharacterAppearance{2, 0, EyeStyle::Sharp, EarStyle::Small, HairStyle::Spiky}});
    snapshot.enemies.push_back({40u, Vec3{7.0f, 1.0f, -2.0f}, 0.8f, 1, 200});
    snapshot.enemies.push_back({41u, Vec3{9.0f, 1.5f, -4.0f}, 2.0f, 0, 60});
    snapshot.villagers.push_back(
        {900u, Vec3{2.0f, 0.5f, 8.0f}, 1.1f, 40, 0,
         CharacterAppearance{4, 1, EyeStyle::Round, EarStyle::Pointed, HairStyle::Ponytail}});
    snapshot.villagers.push_back(
        {901u, Vec3{3.0f, 0.5f, 9.0f}, 0.2f, 200, 1, CharacterAppearance{}}); // a guard (kind 1)
    snapshot.fires.push_back({Vec3{10.0f, 1.0f, 12.0f}, 0.7f, 180});
    snapshot.barricades.push_back({Vec3{5.0f, 0.3f, -7.0f}, 1.2f, 240});
    snapshot.money = 1234u;
    snapshot.contract_phase = static_cast<u8>(ContractPhase::Active);
    snapshot.contract_outcome = 1;
    snapshot.wagons.push_back({55u, Vec3{2.0f, 0.5f, 3.0f}, 0.9f, Vec3{40.0f, 0.0f, 60.0f}, 500u, 200,
                               static_cast<u8>(WagonMode::Manual), 3, 1});
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

    // Peaceful townsfolk populate the town the player spawned in, and are networked as
    // kind 0 (plain villagers, not guards).
    CHECK(server.villager_count() > 0);
    CHECK(snap.villagers.size() == server.villager_count());
    for (const VillagerState& vs : snap.villagers) {
        CHECK(vs.kind == 0);
    }

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

    // Once the cargo has driven clear of the town walls, ambushers spawn + are networked
    // (in the enemy list). The hired driver is slow, so poll until it has left town.
    for (int t = 0; t < 40 && server.ambusher_count() == 0; ++t) {
        pump(60);
    }
    CHECK(server.ambusher_count() > 0);
    CHECK_FALSE(snap.enemies.empty());
}
