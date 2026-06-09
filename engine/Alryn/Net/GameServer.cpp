#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Density.h>
#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/WorldGen.h>

#include <cmath>

namespace alryn {

namespace {
constexpr f32 kEditRadius = 2.5f;
constexpr f32 kEditAmount = 2.0f;
} // namespace

bool GameServer::start(u16 port, u32 seed, u32 max_clients) {
    sampler_.set_seed(seed);
    if (!server_.start(port, max_clients)) {
        return false;
    }
    ALRYN_INFO("Game server up (seed {})", seed);
    return true;
}

void GameServer::stop() {
    server_.stop();
    players_.clear();
}

Vec3 GameServer::spawn_point(net::PlayerId id) const {
    const u32 seed = sampler_.seed();
    const f32 base_x = -4.0f + static_cast<f32>(id % 5u) * 2.0f;
    const f32 base_z = -4.0f + static_cast<f32>((id / 5u) % 5u) * 2.0f;

    // Spiral outward, preferring a forested spot (so you spawn among trees), then
    // any dry land, then the start.
    f32 x = base_x;
    f32 z = base_z;
    bool land_found = false;
    bool forest_found = false;
    for (int i = 0; i < 200 && !forest_found; ++i) {
        const f32 cx = base_x + std::cos(static_cast<f32>(i) * 2.4f) * static_cast<f32>(i) * 1.2f;
        const f32 cz = base_z + std::sin(static_cast<f32>(i) * 2.4f) * static_cast<f32>(i) * 1.2f;
        const f32 h = worldgen::height(cx, cz, seed);
        if (h <= worldgen::water_level + 0.8f) {
            continue;
        }
        if (!land_found) {
            x = cx;
            z = cz;
            land_found = true;
        }
        if (h < 7.0f && worldgen::moisture(cx, cz, seed) > 0.15f) {
            x = cx;
            z = cz;
            forest_found = true;
        }
    }

    Vec3 spawn{x, 30.0f, z};
    const DensitySampler density = sampler_.as_sampler();
    if (const auto ground = raycast_density(density, Vec3{x, 30.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 80.0f)) {
        spawn.y = ground->y + 0.2f;
    }
    return spawn;
}

void GameServer::tick(Timestep dt) {
    const DensitySampler density = sampler_.as_sampler();

    for (const net::ServerEvent& e : server_.poll()) {
        switch (e.type) {
            case net::ServerEventType::ClientConnected: {
                ServerPlayer player;
                player.controller.set_position(spawn_point(e.client));
                players_.emplace(e.client, std::move(player));
                server_.send_welcome(e.client, net::Welcome{e.client, sampler_.seed()});
                ALRYN_INFO("Player {} joined ({} online)", e.client, players_.size());
                break;
            }
            case net::ServerEventType::InputReceived: {
                const auto it = players_.find(e.client);
                if (it == players_.end()) {
                    break;
                }
                it->second.input = e.input;
                if (e.input.dig) {
                    sampler_.add_edit(e.input.aim, kEditRadius, kEditAmount);
                    server_.broadcast_deform(net::DeformEvent{e.input.aim, kEditRadius, kEditAmount});
                }
                if (e.input.add) {
                    sampler_.add_edit(e.input.aim, kEditRadius, -kEditAmount);
                    server_.broadcast_deform(net::DeformEvent{e.input.aim, kEditRadius, -kEditAmount});
                }
                break;
            }
            case net::ServerEventType::ClientDisconnected: {
                players_.erase(e.client);
                server_.broadcast_player_left(e.client);
                ALRYN_INFO("Player {} left ({} online)", e.client, players_.size());
                break;
            }
        }
    }

    for (auto& [id, player] : players_) {
        player.controller.update(density, player.input.move, player.input.jump, dt);
    }

    net::Snapshot snapshot;
    snapshot.tick = ++tick_;
    snapshot.players.reserve(players_.size());
    for (const auto& [id, player] : players_) {
        snapshot.players.push_back({id, player.controller.position(), player.input.yaw});
    }
    server_.broadcast_snapshot(snapshot);
}

} // namespace alryn
