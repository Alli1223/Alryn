#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Net/ByteBuffer.h>

#include <vector>

namespace alryn::net {

using PlayerId = u32;

// First byte of every packet.
enum class MessageType : u8 {
    Welcome = 1,      // server -> client: your id + world seed
    Input = 2,        // client -> server: movement intent + actions
    Snapshot = 3,     // server -> client: authoritative world state
    PlayerLeft = 4,   // server -> client: a player disconnected
    Deform = 5,       // server -> client: authoritative terrain edit
};

// Client -> server: what the player wants to do this tick. The server is
// authoritative; this is intent only.
struct PlayerInput {
    u32 sequence = 0;
    Vec3 move{0.0f}; // world-space xz movement direction
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    bool jump = false;
    bool dig = false;
    bool add = false;
    Vec3 aim{0.0f}; // world point being aimed at (for dig/add)
};

struct PlayerState {
    PlayerId id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
};

struct Snapshot {
    u32 tick = 0;
    std::vector<PlayerState> players;
};

struct Welcome {
    PlayerId your_id = 0;
    u32 seed = 0;
};

struct DeformEvent {
    Vec3 center{0.0f};
    f32 radius = 0.0f;
    f32 amount = 0.0f;
};

// Payload (de)serialization (no leading type byte; callers write/read that).
void write(ByteWriter& w, const PlayerInput& in);
void write(ByteWriter& w, const Snapshot& s);
void write(ByteWriter& w, const Welcome& welcome);
void write(ByteWriter& w, const DeformEvent& deform);

bool read(ByteReader& r, PlayerInput& in);
bool read(ByteReader& r, Snapshot& s);
bool read(ByteReader& r, Welcome& welcome);
bool read(ByteReader& r, DeformEvent& deform);

} // namespace alryn::net
