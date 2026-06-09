#include <enet/enet.h>

#include <Alryn/Net/NetClient.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Net/NetCommon.h>

namespace alryn::net {

bool NetClient::connect(const std::string& host, u16 port) {
    if (host_ != nullptr) {
        return true;
    }
    if (!initialize_enet()) {
        ALRYN_ERROR("enet_initialize failed");
        return false;
    }
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (host_ == nullptr) {
        ALRYN_ERROR("Failed to create client host");
        shutdown_enet();
        return false;
    }

    ENetAddress address{};
    enet_address_set_host(&address, host.c_str());
    address.port = port;
    peer_ = enet_host_connect(host_, &address, 2, 0);
    if (peer_ == nullptr) {
        ALRYN_ERROR("No available peers to connect to {}:{}", host, port);
        enet_host_destroy(host_);
        host_ = nullptr;
        shutdown_enet();
        return false;
    }
    ALRYN_INFO("Connecting to {}:{}...", host, port);
    return true; // handshake completes asynchronously; watch poll() for Connected
}

void NetClient::disconnect() {
    if (peer_ != nullptr && connected_) {
        enet_peer_disconnect(peer_, 0);
    }
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
        peer_ = nullptr;
        connected_ = false;
        shutdown_enet();
    }
}

NetClient::~NetClient() {
    disconnect();
}

std::vector<ClientEvent> NetClient::poll(u32 timeout_ms) {
    std::vector<ClientEvent> events;
    if (host_ == nullptr) {
        return events;
    }
    ENetEvent event;
    int result = enet_host_service(host_, &event, timeout_ms);
    while (result > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                connected_ = true;
                ClientEvent e;
                e.type = ClientEventType::Connected;
                events.push_back(e);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                ByteReader reader(event.packet->data, event.packet->dataLength);
                const auto type = static_cast<MessageType>(reader.read_u8());
                ClientEvent e;
                bool ok = false;
                if (type == MessageType::Welcome) {
                    ok = read(reader, e.welcome);
                    e.type = ClientEventType::WelcomeReceived;
                } else if (type == MessageType::Snapshot) {
                    ok = read(reader, e.snapshot);
                    e.type = ClientEventType::SnapshotReceived;
                } else if (type == MessageType::Deform) {
                    ok = read(reader, e.deform);
                    e.type = ClientEventType::DeformReceived;
                } else if (type == MessageType::PlayerLeft) {
                    e.player = reader.read_u32();
                    ok = reader.ok();
                    e.type = ClientEventType::PlayerLeft;
                }
                if (ok) {
                    events.push_back(e);
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                connected_ = false;
                ClientEvent e;
                e.type = ClientEventType::Disconnected;
                events.push_back(e);
                break;
            }
            default:
                break;
        }
        result = enet_host_service(host_, &event, 0);
    }
    return events;
}

void NetClient::send_input(const PlayerInput& input) {
    if (peer_ == nullptr || !connected_) {
        return;
    }
    ByteWriter message;
    message.write_u8(static_cast<u8>(MessageType::Input));
    write(message, input);
    ENetPacket* packet = enet_packet_create(message.bytes(), message.size(), 0);
    enet_peer_send(peer_, 1, packet);
}

} // namespace alryn::net
