#pragma once

#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Terrain/WorldSampler.h>

#include <unordered_map>

namespace alryn {

// The authoritative simulation. Owns the world density (seed + replicated edits)
// and one CharacterController per connected client. Each tick() it drains network
// events (spawn/despawn/input), applies terrain edits, steps every player from
// their latest input against the density function, and broadcasts a Snapshot. The
// world is unbounded - collision samples the function, so players can roam freely.
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
    u32 seed() const { return sampler_.seed(); }
    const std::unordered_map<net::PlayerId, ServerPlayer>& players() const { return players_; }

private:
    Vec3 spawn_point(net::PlayerId id) const;

    net::NetServer server_;
    WorldSampler sampler_;
    std::unordered_map<net::PlayerId, ServerPlayer> players_;
    u32 tick_ = 0;
};

} // namespace alryn
