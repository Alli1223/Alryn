#include <enet/enet.h>

#include <Alryn/Net/NetServer.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Net/NetCommon.h>

namespace alryn::net {

bool NetServer::start(u16 port, u32 max_clients) {
    if (host_ != nullptr) {
        return true;
    }
    if (!initialize_enet()) {
        ALRYN_ERROR("enet_initialize failed");
        return false;
    }
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    host_ = enet_host_create(&address, max_clients, 2, 0, 0);
    if (host_ == nullptr) {
        ALRYN_ERROR("Failed to create server host on port {}", port);
        shutdown_enet();
        return false;
    }
    ALRYN_INFO("Server listening on port {} (max {} clients)", port, max_clients);
    return true;
}

void NetServer::stop() {
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
        peers_.clear();
        ids_.clear();
        shutdown_enet();
    }
}

NetServer::~NetServer() {
    stop();
}

std::vector<ServerEvent> NetServer::poll(u32 timeout_ms) {
    std::vector<ServerEvent> events;
    if (host_ == nullptr) {
        return events;
    }
    ENetEvent event;
    int result = enet_host_service(host_, &event, timeout_ms);
    while (result > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                const PlayerId id = next_id_++;
                peers_[id] = event.peer;
                ids_[event.peer] = id;
                events.push_back({ServerEventType::ClientConnected, id, {}});
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                ByteReader reader(event.packet->data, event.packet->dataLength);
                const auto type = static_cast<MessageType>(reader.read_u8());
                if (type == MessageType::Input) {
                    PlayerInput input;
                    if (read(reader, input)) {
                        const auto it = ids_.find(event.peer);
                        if (it != ids_.end()) {
                            events.push_back({ServerEventType::InputReceived, it->second, input});
                        }
                    }
                }
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                const auto it = ids_.find(event.peer);
                if (it != ids_.end()) {
                    const PlayerId id = it->second;
                    peers_.erase(id);
                    ids_.erase(it);
                    events.push_back({ServerEventType::ClientDisconnected, id, {}});
                }
                break;
            }
            default:
                break;
        }
        result = enet_host_service(host_, &event, 0); // drain the rest non-blocking
    }
    return events;
}

void NetServer::send_packet(_ENetPeer* peer, const ByteWriter& message, bool reliable) {
    ENetPacket* packet = enet_packet_create(message.bytes(), message.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, reliable ? 0 : 1, packet);
}

void NetServer::broadcast_packet(const ByteWriter& message, bool reliable) {
    ENetPacket* packet = enet_packet_create(message.bytes(), message.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_host_broadcast(host_, reliable ? 0 : 1, packet);
}

void NetServer::send_welcome(PlayerId client, const Welcome& welcome) {
    const auto it = peers_.find(client);
    if (it == peers_.end()) {
        return;
    }
    ByteWriter message;
    message.write_u8(static_cast<u8>(MessageType::Welcome));
    write(message, welcome);
    send_packet(it->second, message, true);
}

void NetServer::broadcast_snapshot(const Snapshot& snapshot) {
    ByteWriter message;
    message.write_u8(static_cast<u8>(MessageType::Snapshot));
    write(message, snapshot);
    broadcast_packet(message, false);
}

void NetServer::broadcast_deform(const DeformEvent& deform) {
    ByteWriter message;
    message.write_u8(static_cast<u8>(MessageType::Deform));
    write(message, deform);
    broadcast_packet(message, true);
}

void NetServer::broadcast_player_left(PlayerId id) {
    ByteWriter message;
    message.write_u8(static_cast<u8>(MessageType::PlayerLeft));
    message.write_u32(id);
    broadcast_packet(message, true);
}

} // namespace alryn::net
