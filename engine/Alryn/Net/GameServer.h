#pragma once

#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Terrain/VoxelField.h>

#include <memory>
#include <unordered_map>

namespace alryn {

// The authoritative simulation. Owns the world voxel field and one
// CharacterController per connected client. Each tick() it: drains network
// events (spawn/despawn/input), applies terrain edits, steps every player from
// their latest input, and broadcasts a Snapshot. Used by the dedicated server and
// exercised directly by tests.
class GameServer {
public:
    struct ServerPlayer {
        CharacterController controller;
        net::PlayerInput input;
    };

    bool start(u16 port, u32 seed, u32 max_clients = 16);
    void stop();
    bool running() const { return server_.running(); }

    void tick(Timestep dt);

    usize player_count() const { return players_.size(); }
    u32 seed() const { return seed_; }
    const std::unordered_map<net::PlayerId, ServerPlayer>& players() const { return players_; }

private:
    Vec3 spawn_point(net::PlayerId id) const;

    net::NetServer server_;
    std::unique_ptr<VoxelField> field_;
    std::unordered_map<net::PlayerId, ServerPlayer> players_;
    u32 tick_ = 0;
    u32 seed_ = 0;
};

} // namespace alryn
