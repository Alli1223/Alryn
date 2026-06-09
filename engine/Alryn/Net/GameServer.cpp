#include <Alryn/Net/GameServer.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Terrain/WorldGen.h>

namespace alryn {

namespace {
constexpr f32 kEditRadius = 2.5f;
constexpr f32 kEditAmount = 2.0f;
} // namespace

bool GameServer::start(u16 port, u32 seed, u32 max_clients) {
    seed_ = seed;
    field_ = std::make_unique<VoxelField>(worldgen::dims, worldgen::voxel_size, worldgen::origin);
    field_->fill([seed](const Vec3& p) { return worldgen::density(p, seed); });
    if (!server_.start(port, max_clients)) {
        field_.reset();
        return false;
    }
    ALRYN_INFO("Game server up (seed {})", seed);
    return true;
}

void GameServer::stop() {
    server_.stop();
    players_.clear();
    field_.reset();
}

Vec3 GameServer::spawn_point(net::PlayerId id) const {
    // Stagger spawns so players don't pile up; drop onto the terrain.
    const f32 x = -4.0f + static_cast<f32>(id % 5u) * 2.0f;
    const f32 z = -4.0f + static_cast<f32>((id / 5u) % 5u) * 2.0f;
    Vec3 spawn{x, 16.0f, z};
    if (const auto ground = field_->raycast(Vec3{x, 16.0f, z}, Vec3{0.0f, -1.0f, 0.0f}, 50.0f)) {
        spawn.y = ground->y + 0.2f;
    }
    return spawn;
}

void GameServer::tick(Timestep dt) {
    for (const net::ServerEvent& e : server_.poll()) {
        switch (e.type) {
            case net::ServerEventType::ClientConnected: {
                ServerPlayer player;
                player.controller.set_position(spawn_point(e.client));
                players_.emplace(e.client, std::move(player));
                server_.send_welcome(e.client, net::Welcome{e.client, seed_});
                ALRYN_INFO("Player {} joined ({} online)", e.client, players_.size());
                break;
            }
            case net::ServerEventType::InputReceived: {
                const auto it = players_.find(e.client);
                if (it == players_.end()) {
                    break;
                }
                it->second.input = e.input;
                // Terrain edits are authoritative: apply here, replicate to all.
                if (e.input.dig) {
                    field_->apply_sphere(e.input.aim, kEditRadius, kEditAmount);
                    server_.broadcast_deform(net::DeformEvent{e.input.aim, kEditRadius, kEditAmount});
                }
                if (e.input.add) {
                    field_->apply_sphere(e.input.aim, kEditRadius, -kEditAmount);
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

    // Step every player from their latest input against the authoritative field.
    for (auto& [id, player] : players_) {
        player.controller.update(*field_, player.input.move, player.input.jump, dt);
    }

    // Broadcast the authoritative snapshot.
    net::Snapshot snapshot;
    snapshot.tick = ++tick_;
    snapshot.players.reserve(players_.size());
    for (const auto& [id, player] : players_) {
        snapshot.players.push_back({id, player.controller.position(), player.input.yaw});
    }
    server_.broadcast_snapshot(snapshot);
}

} // namespace alryn
