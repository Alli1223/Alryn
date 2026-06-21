#pragma once

#include <Alryn/Character/CharacterAppearance.h>
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
    bool fire = false;   // throw a projectile toward `aim`
    bool attack = false; // melee swing (hits an enemy in front, else carves at `aim`)
    bool build = false;  // place a defensive barricade (day/prep only)
    bool rally = false;  // call the night early (skip the rest of the prep day)
    bool grab = false;   // hitch / unhitch the nearest wagon (manual hauling)
    Vec3 aim{0.0f};      // world point being aimed at (for dig/add/fire)
    u32 vote_wagon = 0;  // wagon id this player is voting to accept (0 = none)
    u8 vote_mode = 0;    // 0 = none, 1 = hire driver, 2 = haul manually
    f32 throttle = 0.0f; // carriage driving: forward/back (W/S), used when piloting
    f32 steer = 0.0f;    // carriage driving: rein left/right (A/D), used when piloting
    u8 role = 0;         // PlayerRole the player is in (sent each tick)
    u8 ability = 0;      // ability invoked this tick: 0 = none, 1/2/3 = slot+1 (keys 1/2/3)
    CharacterAppearance appearance; // the player's chosen look (sent each tick)
};

struct PlayerState {
    PlayerId id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    u8 health = 100;                // 0..100 percent of this player's role max, for the bar
    u8 build_stock = 0;             // barricades this player can still raise today
    u8 seated = 0;                  // 1 = riding/driving a wagon -> use the sit pose
    u8 carrying = 0;                // 1 = hauling a spilled good back to the cart
    u8 role = 0;                    // PlayerRole, so every client renders the right weapon
    u8 cast = 0;                    // ability fired this snapshot (0 = none, 1/2/3 = slot+1) -> VFX
    CharacterAppearance appearance; // so every client renders the right avatar
};

// A live enemy, broadcast each tick so clients can render + animate it.
struct EnemyState {
    u32 id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    u8 kind = 0;
    u8 health = 0; // 0..255 scaled from max, for a health bar / death fade
};

// A live villager or town guard, broadcast each tick. Appearance rides along so every
// client renders the right look without local generation. `kind`: 0 = villager, 1 = guard.
struct VillagerState {
    u32 id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    u8 health = 0; // 0..255 scaled from max
    u8 kind = 0;   // 0 = villager, 1 = guard
    CharacterAppearance appearance;
};

// A player-built barricade (defensive obstacle), broadcast so clients render it.
struct BarricadeState {
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    u8 health = 0; // 0..255 scaled from max
};

// A transport wagon, broadcast so clients render it: an offer (Parked) sitting in the
// plaza, or the active cargo en route. `dest` drives the destination arrow + map marker.
struct WagonState {
    u32 id = 0;
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    Vec3 dest{0.0f};      // destination town centre
    Vec3 horse_pos{0.0f}; // the pulling horse (carriages), if has_horse
    f32 horse_yaw = 0.0f;
    u32 reward = 0;
    u8 type = 0;       // VehicleType index (cart / wagon / carriage)
    u8 health = 255;   // 0..255 of kWagonHealth (active wagon)
    u8 mode = 0;       // WagonMode
    u8 difficulty = 1; // 1..3
    u8 votes = 0;      // players currently voting for this offer
    u8 has_horse = 0;  // 1 = render the pulling horse
    u8 goods_aboard = 0; // crates still in the bed
    u8 goods_total = 0;  // crates the full load carries (reward scales by aboard/total)
};

// A burning building, broadcast so clients render flames at its position. A low
// intensity is the smouldering ember of a house that has already burnt down.
struct FireState {
    Vec3 position{0.0f};
    f32 yaw = 0.0f;
    u8 intensity = 0; // 0..255 fire amount
};

// The defend-the-town objective outcome for the town nearest the players.
enum class MatchOutcome : u8 { Ongoing = 0, Won = 1, Lost = 2 };

// Which part of the round we're in: a calm prep/repair lull or a live wave.
enum class MatchPhase : u8 { Prep = 0, Combat = 1 };

// A live projectile, broadcast each tick so clients can render it. `dir` is the travel
// direction (frozen when it lands) so arrows render pointing the way they fly / stuck in.
struct ProjectileState {
    Vec3 position{0.0f};
    Vec3 dir{0.0f, 0.0f, 1.0f};
    u8 kind = 0;
};

// A cargo crate. `loose == 0`: riding in the cart bed - `position` is the crate's LOCAL
// position in the bed (the client places it via the cart transform so it rides + slides with
// the cart). `loose == 1`: spilled onto the ground - `position` is WORLD, and it can be picked
// up (E). Broadcast so clients render the load + the loose crates.
struct GoodState {
    u32 id = 0;
    Vec3 position{0.0f};
    u8 loose = 0;
};

struct Snapshot {
    u32 tick = 0;
    f32 time_of_day = 0.0f; // 0..1, server-authoritative day/night clock
    u8 outcome = 0;          // MatchOutcome for the defended town
    u8 phase = 0;            // MatchPhase (Prep / Combat)
    f32 phase_timer = 0.0f;  // seconds left in the prep lull (0 during a wave)
    u8 wave = 0;             // waves spawned so far
    u8 houses_standing = 0;  // un-burnt houses in the defended town
    u8 houses_total = 0;
    u32 money = 0;            // shared party wallet
    u8 contract_phase = 0;   // ContractPhase (Offer / Active / Settle)
    u8 contract_outcome = 0; // 0 none, 1 delivered, 2 wrecked (for the settle banner)
    std::vector<PlayerState> players;
    std::vector<ProjectileState> projectiles;
    std::vector<EnemyState> enemies;
    std::vector<VillagerState> villagers;
    std::vector<FireState> fires;
    std::vector<BarricadeState> barricades;
    std::vector<WagonState> wagons;
    std::vector<GoodState> goods; // crates spilled from a flipped cart
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
