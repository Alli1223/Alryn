#pragma once

#include <Alryn/Core/Types.h>
#include <Alryn/Net/Protocol.h>

#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace alryn::net {

enum class ClientEventType {
    Connected,
    Disconnected,
    WelcomeReceived,
    SnapshotReceived,
    DeformReceived,
    PlayerLeft,
};

struct ClientEvent {
    ClientEventType type;
    Welcome welcome;     // valid for WelcomeReceived
    Snapshot snapshot;   // valid for SnapshotReceived
    DeformEvent deform;  // valid for DeformReceived
    PlayerId player = 0; // valid for PlayerLeft
};

// Client-side transport over ENet. connect() starts an async handshake; poll()
// surfaces Connected once it completes, then snapshots/welcome/deform messages.
class NetClient {
public:
    NetClient() = default;
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    bool connect(const std::string& host, u16 port);
    void disconnect();
    bool connected() const { return connected_; }

    std::vector<ClientEvent> poll(u32 timeout_ms = 0);
    void send_input(const PlayerInput& input); // unreliable

private:
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    bool connected_ = false;
};

} // namespace alryn::net
