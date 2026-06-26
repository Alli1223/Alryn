#pragma once

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Combat/Villager.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Game/Contract.h>
#include <Alryn/Game/GameManager.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Physics/CollisionWorld.h>
#include <Alryn/Physics/Projectile.h>
#include <Alryn/Terrain/WorldSampler.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace alryn {

class VehicleType; // World/VehicleTypes.h - the cart/wagon/carriage layout (Contracts.cpp)

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
        PlayerRole role = PlayerRole::Knight;
        f32 max_health = kPlayerMaxHealth; // from the role; health is clamped to this
        f32 health = kPlayerMaxHealth;
        f32 since_hit = kPlayerRegenDelay;    // seconds since last damaged (regen gate)
        bool used_second_wind = false;        // a once-per-haul clutch save (reset at contract start)
        f32 roll_timer = 0.0f;                // dodge roll: while > 0 the player is rolling (i-frames)
        f32 roll_cd = 0.0f;                   // seconds until the next dodge roll is available
        Vec3 roll_dir{0.0f};                  // locked roll direction (set when the roll begins)
        f32 melee_cd = 0.0f;                  // seconds until the next melee swing can land
        f32 ability_cd[kAbilityCount] = {}; // per-ability cooldown timers (indexed by ability)
        f32 bulwark_timer = 0.0f;             // Knight: extra damage reduction while > 0
        f32 dash_timer = 0.0f;                // Hunter: walk-speed boost while > 0
        f32 heal_charge = 0.0f;               // Cleric: seconds of channelling a heal aura (right mouse)
        f32 shield_hp = 0.0f;                 // Aegis: damage the shield can still absorb
        f32 shield_timer = 0.0f;              // seconds before an unspent Aegis shield fades
        f32 spell_cd = 0.0f;                  // Mage: seconds until the next combo spell can cast
        f32 damage_boost_timer = 0.0f;        // Empower: x outgoing damage while > 0 (co-op buff)
        f32 haste_timer = 0.0f;               // War Horn: x walk speed while > 0 (co-op buff)
        u8 cast_fx = 0;                       // ability/spell that fired this tick (for the snapshot's VFX)
        Equipment equipment;                  // authoritative worn gear (look + the stat bonus below)
        u8 owned_tier = 0;                    // highest gear tier bought from a shop (clamps equipment)

        // Multiplier applied to this player's outgoing damage (Empower buff x the weapon tier bonus).
        f32 outgoing_mult() const {
            return (damage_boost_timer > 0.0f ? kDamageBoostMult : 1.0f) *
                   equipment_bonus(equipment).damage_mult;
        }
        f32 water = 0.0f;                     // bucket fill for firefighting (dormant siege)
        i32 wood = 0;                         // barricades buildable today (dormant siege)
        bool carrying = false;                // hauling a spilled cargo crate back to the cart

        // Incoming damage after role mitigation + the armour tier + a held shield block + bulwark.
        f32 mitigated(f32 raw) const {
            const bool guarding = input.block && role == PlayerRole::Knight; // Cleric block = channel
            f32 r = role_stats(role).damage_reduction + equipment_bonus(equipment).mitigation_add +
                    (guarding ? kBlockReduction : 0.0f) + (bulwark_timer > 0.0f ? kBulwarkReduction : 0.0f);
            return raw * (1.0f - glm::clamp(r, 0.0f, 0.9f));
        }

        // Apply `raw` incoming damage: mitigate it, then soak it into the Aegis shield first and
        // spill the rest onto health. Resets the regen gate.
        void take_damage(f32 raw) {
            if (roll_timer > 0.0f) {
                // i-frames: a dodge roll evades the hit entirely - and a PERFECT DODGE (rolling through
                // a hit) rewards a brief outgoing-damage boost (reuses the empower buff): dodge into
                // danger, hit harder. Short + refreshed per evaded hit, capped by the roll cooldown.
                damage_boost_timer = std::max(damage_boost_timer, kPerfectDodgeBuff);
                return;
            }
            f32 d = mitigated(raw);
            if (shield_hp > 0.0f) {
                const f32 absorbed = std::min(shield_hp, d);
                shield_hp -= absorbed;
                d -= absorbed;
            }
            health -= d;
            since_hit = 0.0f;
        }

        // Mend `amount` health, capped at the role's max (e.g. a melee-kill lifesteal).
        void heal(f32 amount) { health = std::min(max_health, health + amount); }

        // SECOND WIND: the first lethal blow of a haul leaves the player clinging on at kSecondWindHealth
        // instead of dying - a once-per-contract clutch save. Returns true if it triggered (so the death
        // handler skips the respawn).
        bool try_second_wind() {
            if (used_second_wind) {
                return false;
            }
            used_second_wind = true;
            health = kSecondWindHealth;
            since_hit = 0.0f; // still in combat - no regen yet
            return true;
        }
    };

    // A cargo crate that bounced out of the bed and is lying on the ground (world position)
    // until a player picks it up (E) and carries it back to the cart.
    struct GroundGood {
        u32 id = 0;
        Vec3 position{0.0f};
    };

    // A cargo crate riding in the cart bed: a little body that slides on the bed floor (its
    // position + velocity are in the cart's LOCAL xz frame; x = fore/aft, y = lateral) and can
    // be tossed upward by a bump (h = height above the floor, vh = vertical velocity). It only
    // escapes by clearing the bed wall (h >= wall) - never by sliding through a side.
    struct CargoBox {
        u32 id = 0;
        Vec2 local{0.0f};
        Vec2 vel{0.0f};
        f32 h = 0.0f;
        f32 vh = 0.0f;
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

    // A Mage-summoned rock wall: a row of stone that times out, and which enemies + the hired
    // teamster must path AROUND (its colliders are fed into the NPC collision scratch).
    struct Wall {
        Vec3 position{0.0f};
        f32 yaw = 0.0f;
        f32 length = kRockWallLength;
        f32 health = kRockWallHealth;
        f32 ttl = kRockWallTtl;
    };

    // A ground aura: a disc that affects whoever stands in it until its life runs out.
    // kind 0 = Cleric heal (heals allies), kind 1 = Knight consecration (taunts + burns enemies).
    struct Aura {
        Vec3 position{0.0f};
        f32 ttl = 0.0f;
        f32 radius = 0.0f;
        u8 kind = 0;
        net::PlayerId owner = 0; // who cast it (consecration taunts enemies toward the owner)
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
    bool wheel_off() const { return wheel_off_; }   // a wheel has come off the active cart
    Vec3 wheel_pos() const { return wheel_pos_; }    // the fallen/carried wheel's world position
    f32 wheel_repair() const { return wheel_repair_; } // 0..1 re-attach progress
    void force_wheel_break();                        // trigger a break now (test / debug hook)
    void debug_place_player(net::PlayerId id, const Vec3& pos); // move a player (test / debug hook)
    // Unlock a gear tier for a player (raises owned_tier so they can equip up to it). The town shop
    // calls this on a purchase; also a test hook.
    void unlock_tier(net::PlayerId id, u8 tier);
    void debug_add_money(u32 amount) { money_ += amount; } // test/debug hook (normally from deliveries)
    usize villager_count() const { return villagers_.size(); }
    usize house_count() const { return houses_.size(); }
    usize barricade_count() const { return barricades_.size(); }
    usize good_count() const { return goods_.size(); }        // loose crates on the ground
    usize cargo_count() const { return cargo_.size(); }       // crates still in the bed
    u8 wagon_goods_aboard() const { return static_cast<u8>(cargo_.size()); }
    u32 seed() const { return sampler_.seed(); }
    f32 time_of_day() const { return time_of_day_; }
    net::MatchOutcome outcome() const { return outcome_; }
    const std::unordered_map<net::PlayerId, ServerPlayer>& players() const { return players_; }
    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::unordered_map<u32, Villager>& villagers() const { return villagers_; }
    const std::unordered_map<u32, HouseFire>& houses() const { return houses_; }
    const std::vector<Barricade>& barricades() const { return barricades_; }
    const std::vector<Wall>& walls() const { return walls_; }

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
    void update_wheel(Timestep dt, const DensitySampler& density);  // wheel break / fetch / refit
    void update_cargo(Timestep dt, const DensitySampler& density);  // slide the bed crates, eject on bumps
    void end_contract_cleanup();                // clear haul state on delivery / wreck
    void append_wagon_colliders(std::vector<Collider>& out) const; // block players from carts
    void seat_occupants(const VehicleType& vt); // place pilot/riders/seated-driver on the vehicle
    // The active cart's bed is a moving platform: its top-surface height where (x,z) is over the
    // footprint (else a large-negative sentinel), so the controller can stand a player on top.
    f32 wagon_top_at(f32 x, f32 z) const;
    // After the cart moves, carry any player standing on top along with it (delta = this tick's move).
    void carry_top_riders(const Vec2& delta, const VehicleType& vt);
    void update_ambush(Timestep dt, const DensitySampler& density); // ambushers + player combat
    // --- Roles, weapons & abilities (Game/Abilities.cpp) ---
    void sync_player_role(ServerPlayer& player); // adopt the chosen role each tick (stats/speed)
    void update_abilities(Timestep dt, const DensitySampler& density); // tick cooldowns + cast
    void update_auras(Timestep dt); // Cleric channel charge + ground-aura ticking (heal/consecrate)
    void spawn_aura(AuraKind kind, const Vec3& pos, net::PlayerId owner); // radius/duration from table
    // --- Mage elemental combo spells (Game/Abilities.cpp) ---
    void update_spells(Timestep dt, const DensitySampler& density); // resolve Mage combo casts
    void cast_spell(ServerPlayer& player, net::PlayerId id, SpellId spell); // apply one spell's effect
    void update_walls(Timestep dt);                                 // age out raised rock walls
    static void wall_colliders(const Wall& w, std::vector<Collider>& out); // 1-3 boxes for the span
    void append_walls(const Vec3& pos, std::vector<Collider>& out) const;  // feed walls to NPC pathing
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
    std::vector<Wall> walls_;                     // Mage rock walls (NPCs path around them)
    std::vector<Aura> auras_;                     // ground auras (heal / consecration)
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
    net::PlayerId tower_ = 0;            // player hand-hauling a cart/wagon (manual)
    net::PlayerId pilot_ = 0;            // player driving a carriage from the top seat (manual)
    std::unordered_set<net::PlayerId> riders_; // players sitting on the wagon (passengers)
    std::optional<Villager> driver_;     // hired NPC: teamster pulling, or seated carriage driver
    bool has_horse_ = false;             // carriage: a horse is the puller
    Vec3 horse_pos_{0.0f};
    f32 horse_yaw_ = 0.0f;
    Vec3 wagon_prev_pos_{0.0f};          // last tick's cart position (to derive velocity)
    Vec2 wagon_vel_{0.0f};               // cart xz velocity (to derive acceleration for cargo inertia)
    f32 wagon_vy_ = 0.0f;                // cart vertical velocity (to derive bump jolts for cargo)
    std::vector<CargoBox> cargo_;        // crates riding in the bed (slide around physically)
    std::vector<GroundGood> goods_;      // crates that bounced out onto the ground (pickups)
    u32 next_good_id_ = 1;
    std::vector<Vec2> driver_path_;      // A* path the teamster is following (around obstacles)
    usize driver_path_i_ = 0;            // current node in driver_path_
    f32 driver_repath_ = 0.0f;           // seconds until the path is recomputed
    f32 driver_stuck_ = 0.0f;            // seconds the puller has gone without getting closer
    f32 driver_best_dist_ = 1e9f;        // closest the puller has gotten to the current waypoint
    f32 driver_snag_ = 0.0f;             // seconds the cart has been snagged (tow-gate pinned low)
    // Wheel-breakdown event: a wheel works loose mid-haul, the cart halts, and a player must fetch
    // the fallen wheel and hold it by the cart to refit it.
    bool wheel_off_ = false;             // a wheel is currently off (cart halted until refitted)
    u8 wheel_index_ = 0;                 // which axle shed (0..3) - random per break
    Vec3 wheel_pos_{0.0f};               // the fallen wheel's world position (follows a carrier)
    Vec2 wheel_vel_{0.0f};               // the shed wheel's roll velocity (xz) while loose on the ground
    net::PlayerId wheel_carrier_ = 0;    // player carrying the wheel (0 = lying on the ground)
    f32 wheel_repair_ = 0.0f;            // 0..1 re-attach progress (builds while held by the cart)
    f32 wheel_break_cd_ = 0.0f;          // rolling-seconds until the next possible break
    f32 bandit_cd_ = 0.0f;               // seconds until the next bandit wave while a wheel is off
    std::vector<Enemy> ambush_;          // ambushers attacking the active wagon
    std::unordered_map<net::PlayerId, std::pair<u32, u8>> votes_; // player -> (wagon id, mode)
    u32 money_ = 0;                      // shared party wallet
    u32 delivery_streak_ = 0;            // consecutive perfect (full-cargo) deliveries -> a pay bonus
    u8 rig_level_ = 0;                   // wagon-rig upgrade level the party has bought (money sink)
    f32 contract_elapsed_ = 0.0f;        // seconds the active haul has been under way (rush-bonus clock)
    u32 contract_kills_ = 0;             // ambushers the party has felled this haul (-> kill bounty)
    u8 contract_outcome_ = 0;            // 0 none, 1 delivered, 2 wrecked (settle banner)
    u32 offer_town_vseed_ = 0;           // which town the current offers are from (0 = none)
    f32 settle_timer_ = 0.0f;            // banner hold before the next offer
    u32 next_wagon_id_ = 1;
    u32 next_ambush_id_ = 1;

    u32 tick_ = 0;
};

} // namespace alryn
