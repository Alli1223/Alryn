#pragma once

#include <Alryn/Core/Types.h>
#include <Alryn/Net/Protocol.h>

#include <unordered_map>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace alryn::net {

enum class ServerEventType { ClientConnected, ClientDisconnected, InputReceived };

struct ServerEvent {
    ServerEventType type;
    PlayerId client = 0;
    PlayerInput input; // valid for InputReceived
};

// Authoritative-server transport over ENet. Assigns a PlayerId per connection,
// surfaces connect/disconnect/input events, and sends welcome/snapshot/deform.
// The simulation itself lives in the server application (which calls poll() and
// broadcast_snapshot() each tick).
class NetServer {
public:
    NetServer() = default;
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool start(u16 port, u32 max_clients = 16);
    void stop();
    bool running() const { return host_ != nullptr; }

    // Services the network and returns events since the last call. timeout_ms is
    // how long to block waiting for the first event (0 = non-blocking).
    std::vector<ServerEvent> poll(u32 timeout_ms = 0);

    void send_welcome(PlayerId client, const Welcome& welcome); // reliable
    void broadcast_snapshot(const Snapshot& snapshot);          // unreliable
    void broadcast_deform(const DeformEvent& deform);           // reliable
    void broadcast_player_left(PlayerId id);                    // reliable

    usize client_count() const { return peers_.size(); }

private:
    void send_packet(_ENetPeer* peer, const ByteWriter& message, bool reliable);
    void broadcast_packet(const ByteWriter& message, bool reliable);

    _ENetHost* host_ = nullptr;
    std::unordered_map<PlayerId, _ENetPeer*> peers_;
    std::unordered_map<_ENetPeer*, PlayerId> ids_;
    PlayerId next_id_ = 1;
};

} // namespace alryn::net
