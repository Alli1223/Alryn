#pragma once

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Combat/Villager.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Game/Contract.h>
#include <Alryn/Game/GameManager.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Physics/CollisionWorld.h>
#include <Alryn/Physics/Projectile.h>
#include <Alryn/Terrain/WorldSampler.h>
#include <Alryn/World/PropLibrary.h>

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        f32 health = kPlayerMaxHealth;
        f32 since_hit = kPlayerRegenDelay; // seconds since last damaged (regen gate)
        f32 melee_cd = 0.0f;               // seconds until the next melee swing can land
        f32 water = 0.0f;                  // bucket fill for firefighting (dormant siege)
        i32 wood = 0;                      // barricades buildable today (dormant siege)
    };

    bool start(u16 port, u32 seed, u32 max_clients = 16);
    void stop();
    bool running() const { return server_.running(); }

    void tick(Timestep dt);

    // A town building that enemies can set alight (Phase 4). Static position; only
    // the fire amount changes. Tracked for towns near players, like villagers.
    struct HouseFire {
        Vec3 position{0.0f};
        f32 yaw = 0.0f;
        u32 vseed = 0;   // which town (for the win/lose tally)
        f32 fire = 0.0f; // 0..1; >= 1 means it has burnt down
        bool destroyed = false;
    };

    // A player-built barricade: blocks the enemy until they hack it down.
    struct Barricade {
        Vec3 position{0.0f};
        f32 yaw = 0.0f;
        f32 health = 0.0f;
    };

    usize player_count() const { return players_.size(); }
    usize enemy_count() const { return enemies_.size(); }
    // --- Wagon-contract loop (the active game mode; see Game/Contracts.cpp) ---
    u32 money() const { return money_; }
    ContractPhase contract_phase() const { return contract_phase_; }
    usize offer_count() const { return offers_.size(); }
    usize ambusher_count() const { return ambush_.size(); }
    const Wagon& active_wagon() const { return active_; }
    WagonMode active_mode() const { return active_mode_; }
    usize villager_count() const { return villagers_.size(); }
    usize house_count() const { return houses_.size(); }
    usize barricade_count() const { return barricades_.size(); }
    u32 seed() const { return sampler_.seed(); }
    f32 time_of_day() const { return time_of_day_; }
    net::MatchOutcome outcome() const { return outcome_; }
    const std::unordered_map<net::PlayerId, ServerPlayer>& players() const { return players_; }
    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::unordered_map<u32, Villager>& villagers() const { return villagers_; }
    const std::unordered_map<u32, HouseFire>& houses() const { return houses_; }
    const std::vector<Barricade>& barricades() const { return barricades_; }

private:
    Vec3 spawn_point(net::PlayerId id) const;
    // Peaceful townsfolk: spawn one per cottage in towns near players and let them
    // stroll the plaza (no combat - the siege villager AI is dormant in SiegeMode).
    void update_townsfolk(Timestep dt, const DensitySampler& density);
    // --- Wagon-contract loop (Game/Contracts.cpp) ---
    void update_contracts(Timestep dt, const DensitySampler& density);
    void generate_offers();                       // offer wagons from the town players are in
    void accept_contract(const Wagon& chosen, WagonMode mode);
    void update_wagon(Timestep dt, const DensitySampler& density);  // drive / tow the cargo
    void update_ambush(Timestep dt, const DensitySampler& density); // ambushers + player combat
    // --- Dormant night siege (Combat/SiegeMode.cpp; not driven in the transport game) ---
    void player_attack(ServerPlayer& player, const net::PlayerInput& in);
    void player_build(ServerPlayer& player, const net::PlayerInput& in); // place a barricade
    void spawn_wave();             // drops a wave of enemies on a defended town
    void update_enemies(Timestep dt, const DensitySampler& density);
    void update_villagers(Timestep dt, const DensitySampler& density); // villagers + guards
    void update_fires(Timestep dt);          // grow fires, mark burnt, judge defeat
    void update_phases(Timestep dt);         // day/night rhythm + win/restart
    void reset_match();                      // fresh siege after a verdict
    void update_player_firefighting(Timestep dt); // players fetch water + douse/repair
    // Nearest burning house to `p`, or nullptr. With `include_ruins`, burnt-down houses
    // (being repaired in the prep lull) count too.
    HouseFire* nearest_fire(const Vec3& p, f32 max_dist, bool include_ruins = false);
    // World position of the well in the town nearest `p` (the town centre), if any.
    std::optional<Vec3> nearest_well(const Vec3& p) const;
    static Collider barricade_collider(const Barricade& b);
    void append_barricades(const Vec3& pos, std::vector<Collider>& out) const;

    net::NetServer server_;
    GameManager manager_;                     // day/night clock + game-mode orchestration
    WorldSampler sampler_;
    PropLibrary prop_lib_;                    // house wall colliders come from here
    std::optional<CollisionWorld> collision_; // built in start() once the seed is known
    std::vector<Collider> collider_scratch_;  // reused per player each tick
    std::unordered_map<net::PlayerId, ServerPlayer> players_;
    std::vector<Projectile> projectiles_;     // live thrown bodies
    std::vector<Enemy> enemies_;              // live hostile NPCs
    std::unordered_map<u32, Villager> villagers_; // townsfolk + guards (Villager.kind)
    std::unordered_map<u32, HouseFire> houses_;   // burnable buildings near players
    std::unordered_map<u32, u32> town_house_total_; // vseed -> #houses (for the tally)
    std::vector<Barricade> barricades_;           // player-built defences
    u32 next_enemy_id_ = 1;
    u32 wave_ = 0;            // = nights survived
    u32 spawn_index_ = 0;     // distinct layout per wave spawn
    net::MatchPhase phase_ = net::MatchPhase::Prep;
    f32 phase_timer_ = 0.0f;  // HUD countdown to the next dusk/dawn
    f32 reset_timer_ = 0.0f;  // holds the win/lose banner before the next siege
    f32 night_wave_timer_ = 0.0f; // to the next reinforcement wave during a night
    bool was_night_ = false;  // night state last tick (to catch dusk/dawn edges)
    f32 time_of_day_ = 0.30f; // day/night clock (0..1), advanced each tick
    f32 day_seconds_ = 120.0f;
    net::MatchOutcome outcome_ = net::MatchOutcome::Ongoing;
    u8 houses_standing_ = 0;
    u8 houses_total_ = 0;

    // --- Wagon-contract loop state (the active game mode) ---
    ContractPhase contract_phase_ = ContractPhase::Offer;
    std::vector<Wagon> offers_;          // wagons offered in the current town
    Wagon active_;                       // the accepted cargo (valid while Active/Settle)
    WagonMode active_mode_ = WagonMode::Parked;
    net::PlayerId tower_ = 0;            // player currently hauling the wagon (manual)
    std::unordered_set<net::PlayerId> riders_; // players sitting on the back of the wagon
    std::optional<Villager> driver_;     // hired teamster NPC pulling the wagon (driver mode)
    std::vector<Vec2> driver_path_;      // A* path the teamster is following (around obstacles)
    usize driver_path_i_ = 0;            // current node in driver_path_
    f32 driver_repath_ = 0.0f;           // seconds until the path is recomputed
    std::vector<Enemy> ambush_;          // ambushers attacking the active wagon
    std::unordered_map<net::PlayerId, std::pair<u32, u8>> votes_; // player -> (wagon id, mode)
    u32 money_ = 0;                      // shared party wallet
    u8 contract_outcome_ = 0;            // 0 none, 1 delivered, 2 wrecked (settle banner)
    u32 offer_town_vseed_ = 0;           // which town the current offers are from (0 = none)
    f32 settle_timer_ = 0.0f;            // banner hold before the next offer
    u32 next_wagon_id_ = 1;
    u32 next_ambush_id_ = 1;

    u32 tick_ = 0;
};

} // namespace alryn
