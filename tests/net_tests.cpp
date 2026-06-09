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
    input.aim = Vec3{3.0f, 4.0f, 5.0f};

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
    CHECK(out.aim.z == doctest::Approx(5.0f));

    Snapshot snapshot;
    snapshot.tick = 7;
    snapshot.players.push_back({1, Vec3{1.0f, 2.0f, 3.0f}, 0.5f});
    snapshot.players.push_back({2, Vec3{4.0f, 5.0f, 6.0f}, 1.5f});
    ByteWriter ws;
    write(ws, snapshot);
    ByteReader rs(ws.bytes(), ws.size());
    Snapshot decoded;
    REQUIRE(read(rs, decoded));
    CHECK(decoded.tick == 7);
    REQUIRE(decoded.players.size() == 2);
    CHECK(decoded.players[1].id == 2);
    CHECK(decoded.players[1].position.y == doctest::Approx(5.0f));

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
