#pragma once

#include <Alryn/Combat/Enemy.h>
#include <Alryn/Combat/Villager.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Game/Contract.h>
#include <Alryn/Game/GameManager.h>
#include <Alryn/Game/Progression.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Game/SideQuest.h>
#include <Alryn/Net/NetServer.h>
#include <Alryn/Net/Protocol.h>
#include <Alryn/Physics/CharacterController.h>
#include <Alryn/Physics/CollisionWorld.h>
#include <Alryn/Physics/Projectile.h>
#include <Alryn/Terrain/WorldSampler.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <optional>
#include <string>
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
        f32 strike_in = 0.0f;                 // a committed swing's blow lands in this many seconds (0 = none)
        f32 basic_cd = 0.0f;                  // pacing of the ranged basic attack (arrow / bolt / thrown rock)
        // --- Charged HEAVY attacks (Roles.h) ---
        u8 heavy_seen = 0;                    // the last input.heavy_seq acted on (a change = a fresh release)
        f32 heavy_in = 0.0f;                  // Knight: the charged overhead blow lands in this many seconds
        f32 heavy_power = 0.0f;               // ...at this charge (0..1)
        u8 heavy_fx_seq = 0;                  // bumps per heavy released (-> every client plays it)
        u8 heavy_fx_power = 0;                // the charge (0..255) of the last heavy released
        f32 dig_cd = 0.0f;                    // pacing of the spade (Q)
        f32 earth_cd = 0.0f;                  // pacing of raising earth (right mouse, Hunter / Mage)
        bool input_fresh = false;             // a packet already landed this tick (later ones only ADD presses)
        f32 ability_cd[kAbilityCount] = {}; // per-ability cooldown timers (indexed by ability)
        f32 bulwark_timer = 0.0f;             // Knight: extra damage reduction while > 0
        f32 dash_timer = 0.0f;                // Hunter: walk-speed boost while > 0
        f32 heal_charge = 0.0f;               // Cleric: seconds of channelling a heal aura (right mouse)
        f32 shield_hp = 0.0f;                 // Aegis: damage the shield can still absorb
        f32 shield_timer = 0.0f;              // seconds before an unspent Aegis shield fades
        f32 spell_cd = 0.0f;                  // Mage: seconds until the next combo spell can cast
        f32 damage_boost_timer = 0.0f;        // Empower: x outgoing damage while > 0 (co-op buff)
        f32 haste_timer = 0.0f;               // War Horn: x walk speed while > 0 (co-op buff)
        // --- Gauntlet co-op combos ---
        f32 toss_timer = 0.0f;  // Ally Toss: while > 0 this player is airborne from a toss (i-frames + ballistic)
        f32 toss_cd = 0.0f;     // cooldown before this player can toss an ally again
        net::PlayerId conduit_target = 0; // Power Conduit: the ally this (Cleric) player is channelling to
        f32 rampage_timer = 0.0f;             // Rampage: kill-momentum window; stacks decay when it lapses
        u8 rampage_stacks = 0;                // current kill-momentum stacks (x outgoing damage)
        f32 parry_window = 0.0f;              // Knight: a parry window opens when the shield is raised
        u8 cast_fx = 0;                       // ability/spell that fired this tick (for the snapshot's VFX)
        u8 hit_fx = 0;                        // monotonic counter: bumps when this player's attack lands (-> hit marker)
        Equipment equipment;                  // authoritative worn gear (look + the stat bonus below)
        u8 owned_tier = 0;                    // highest gear tier bought from a shop (clamps equipment)
        // Per-(role,ability) upgrade rank bought from a town shop (see ability_max_rank). Persists
        // across role swaps; the CURRENT role's ranks are packed into PlayerState.ability_ranks.
        u8 ability_rank[kRoleCount * kAbilityCount] = {};
        u8 prev_upgrade = 0;                  // last tick's input.upgrade, for rising-edge buy dedupe
        // Upgrade rank of `ability` for this player's current role.
        u8 rank_of(u8 ability) const {
            return ability_rank[static_cast<u8>(role) * kAbilityCount + (ability % kAbilityCount)];
        }

        // --- Progression (Game/Progression.h; see Game/Progress.cpp) ---
        u8 color = 0;                       // identity colour (first free on join; a free preference is honoured)
        std::string name;                   // sanitised display name
        u32 xp = 0;                         // experience (level is derived)
        u8 level = 1;
        u8 known[kRoleCount] = {};          // abilities LEARNED per role (the starter kit is implied)
        u8 talents[kRoleCount] = {};        // packed talent ranks per role
        u8 journey = 0;                     // current JourneyGoal step
        u16 kills = 0;                      // lifetime raiders felled (shared credit)
        u16 deliveries = 0;                 // lifetime wagons delivered
        u8 best_danger = 0;                 // highest contract danger delivered
        bool restored = false;              // the client's saved hero has been adopted
        bool spawned = false;               // has had its first full heal (at its real max health)
        u8 prev_learn = 0;                  // last tick's input.learn, for rising-edge learn dedupe
        // Everything this player can use in their current role: the starter kit + what they learned.
        u8 known_mask() const {
            return static_cast<u8>(known[static_cast<u8>(role)] | starter_mask(role));
        }
        u8 talent_mask() const { return talents[static_cast<u8>(role)]; }
        // Cooldown multiplier from the race passive and the FOCUS talent.
        f32 cooldown_mult() const {
            return race_combat(input.appearance.race).cooldown_mult * talent_cooldown_mult(talent_mask());
        }

        // Multiplier applied to this player's outgoing damage (Empower buff x kill-momentum rampage x
        // the weapon tier bonus x the level + MIGHT talent growth).
        f32 outgoing_mult() const {
            return (damage_boost_timer > 0.0f ? kDamageBoostMult : 1.0f) * rampage_mult() *
                   equipment_bonus(equipment).damage_mult * level_damage_mult(level) *
                   talent_damage_mult(talent_mask());
        }
        // RAMPAGE: kill momentum. 1.0 at 0 stacks (idle/default), so it never perturbs an idle player.
        f32 rampage_mult() const { return 1.0f + kRampagePerStack * static_cast<f32>(rampage_stacks); }
        // Felling a raider adds a stack (capped) and refreshes the momentum window.
        void on_kill() {
            rampage_stacks = std::min<u8>(static_cast<u8>(rampage_stacks + 1), kRampageMaxStacks);
            rampage_timer = kRampageDuration;
        }
        // Tick the momentum window down; when it lapses the stacks clear.
        void decay_rampage(f32 dt) {
            if (rampage_timer > 0.0f) {
                rampage_timer -= dt;
                if (rampage_timer <= 0.0f) {
                    rampage_timer = 0.0f;
                    rampage_stacks = 0;
                }
            }
        }
        // PARRY: a Knight inside the window it opened on raising the shield turns the next melee blow.
        bool try_parry() const { return role == PlayerRole::Knight && parry_window > 0.0f; }
        void decay_parry(f32 dt) {
            if (parry_window > 0.0f) {
                parry_window -= dt;
            }
        }
        f32 water = 0.0f;                     // bucket fill for firefighting (dormant siege)
        i32 wood = 0;                         // barricades buildable today (dormant siege)
        bool carrying = false;                // hauling a spilled cargo crate back to the cart

        // Incoming damage after role mitigation + the race passive + the armour tier + a held
        // shield block + bulwark (a Dwarf's stoutness stacks with all of it, capped below 1).
        f32 mitigated(f32 raw) const {
            // Cleric block = channel; a Knight holding a lantern out has stowed the shield.
            const bool guarding = input.block && role == PlayerRole::Knight && !input.lantern;
            f32 r = role_stats(role).damage_reduction + equipment_bonus(equipment).mitigation_add +
                    race_combat(input.appearance.race).mitigation_add +
                    (guarding ? kBlockReduction : 0.0f) + (bulwark_timer > 0.0f ? kBulwarkReduction : 0.0f);
            return raw * (1.0f - glm::clamp(r, 0.0f, 0.9f));
        }

        bool invincible = false; // debug godmode: ignore all incoming damage (set per tick from the server flag)

        // Apply `raw` incoming damage: mitigate it, then soak it into the Aegis shield first and
        // spill the rest onto health. Resets the regen gate.
        void take_damage(f32 raw) {
            if (invincible) {
                return; // debug godmode
            }
            if (roll_timer > 0.0f) {
                // i-frames: a dodge roll evades the hit entirely - and a PERFECT DODGE (rolling through
                // a hit) rewards a brief outgoing-damage boost (reuses the empower buff): dodge into
                // danger, hit harder. Short + refreshed per evaded hit, capped by the roll cooldown.
                damage_boost_timer = std::max(damage_boost_timer, kPerfectDodgeBuff);
                return;
            }
            if (toss_timer > 0.0f) {
                return; // i-frames while flying through the air from an Ally Toss
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

    // --- Progression rules (Game/Progression.h) ---
    // With progression ON (the game turns it on; a bare server is a sandbox for tests + tools) a hero
    // can only cast what they have LEARNED in the skill tree, a contract's danger is capped by the
    // party's best level, and gold rank upgrades need the ability learned first. XP, levels, talents
    // and the journey are tracked either way.
    void set_progression(bool on) { progression_ = on; }
    bool progression() const { return progression_; }
    // Grant `xp` to one player (journey rewards, tests). Levels are re-derived.
    void grant_xp(net::PlayerId id, u32 xp);

    // --- Debug / testing hooks (driven by the client's debug overlay on a listen server) ---
    // Godmode: all players + the active cargo wagon ignore incoming damage.
    void set_debug_god(bool on) { debug_god_ = on; }
    bool debug_god() const { return debug_god_; }
    // Stop the wagon ambushes: no raiders spawn during a haul, and any in progress are cleared.
    void set_debug_no_ambush(bool on) { debug_no_ambush_ = on; }
    bool debug_no_ambush() const { return debug_no_ambush_; }

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

    // A Cleric max-rank Aegis DOME: a large protective bubble that FOLLOWS its caster and blocks
    // enemy ranged attacks (arrows) for everyone - and the cargo - inside it. Its health chips as it
    // absorbs shots; it pops early if fully battered, else fades when its lifetime runs out.
    struct BubbleShield {
        Vec3 position{0.0f};
        f32 radius = kAegisBubbleRadius;
        f32 health = kAegisBubbleHealth;
        f32 ttl = kAegisBubbleDuration;
        net::PlayerId owner = 0; // the caster it re-centres on each tick
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
    // A single NPC navigation polyline, for the client's pathfinding debug overlay. `points` is a
    // world-space line (>=2 pts) the client draws on screen; `kind` chooses the colour/meaning.
    struct DebugNavPath {
        std::vector<Vec3> points;
        u8 kind = 0; // 0 teamster A* path, 1 wagon road route, 2 villager goal, 3 ambusher goal
    };
    // The current NPC navigation routes (teamster A* path + wagon route + each NPC's goal line). Only
    // meaningful on a listen server (the client owns the sim); a remote client has no path data.
    std::vector<DebugNavPath> debug_nav_paths() const;
    f32 wheel_repair() const { return wheel_repair_; } // 0..1 re-attach progress
    void force_wheel_break();                        // trigger a break now (test / debug hook)
    void debug_place_player(net::PlayerId id, const Vec3& pos); // move a player (test / debug hook)
    // Wound a player directly (raw, no mitigation; floored above 0 so they don't respawn) - a
    // test hook for exercising heals/shields without simulating a whole ambush.
    void debug_hurt_player(net::PlayerId id, f32 damage);
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
    const std::vector<BubbleShield>& bubbles() const { return bubbles_; }
    // A hostile projectile at `pos` that has entered a live max-Aegis dome is absorbed: the dome's
    // health is chipped by `damage` and true is returned (the caller kills the shot). This is what
    // protects allies + the cargo from enemy ranged attacks. Public so the ambush loop + tests use it.
    bool bubble_absorbs(const Vec3& pos, f32 damage);

    // --- Terrain deformation ---
    // Carve (amount > 0) or raise (< 0) the ground with a sphere edit and replicate it to every client
    // (a client joining later is sent the whole history - see tick()).
    void deform(const Vec3& center, f32 radius, f32 amount);
    // Gouge a crater where a heavy blow / blast landed (on the ground under `at`). Refused in towns, on
    // bridges, in water, or once the world holds kMaxCraterEdits edits. True if one was made.
    bool crater(const Vec3& at, f32 radius, f32 depth);
    static constexpr usize kMaxCraterEdits = 1600;
    static constexpr usize kMaxWorldEdits = 6000; // digs + raised earth stop here too (a safety cap)
    usize terrain_edit_count() const { return sampler_.edits().size(); }

    // --- Side quests (Game/SideQuest.h; see Game/SideQuests.cpp) ---
    // A quest pinned on the board of the town the party is in (Offered), the one the party took
    // (Active), or a just-finished one showing its banner (Complete).
    struct QuestRun {
        u32 id = 0;
        QuestKind kind = QuestKind::BanditCamp;
        QuestPhase phase = QuestPhase::Offered;
        u8 danger = 1;
        u8 progress = 0;
        u8 goal = 1;
        u32 reward = 0;
        u32 xp = 0;
        Vec3 site{0.0f};   // the camp / den / X / meadow
        Vec3 board{0.0f};  // the notice board it was pinned on
        bool woken = false; // its foes have been placed (a hero came near)
        f32 banner = 0.0f;  // Complete: seconds the banner still shows
    };
    // A quest's pickups: a moonpetal (kind 0) or the treasure chest (kind 1: state 0 buried under the X,
    // 1 unearthed, 2 opened).
    struct QuestItem {
        u32 id = 0;
        u32 quest = 0;
        Vec3 position{0.0f};
        u8 kind = 0;
        u8 state = 0;
    };
    const std::vector<QuestRun>& quests() const { return quests_; }
    const std::vector<QuestItem>& quest_items() const { return quest_items_; }
    // The party's quest under way (nullptr if none).
    const QuestRun* active_quest() const;
    // Take an offered quest as if a player had picked it on the board (tests / debug).
    bool debug_accept_quest(u32 id);
    // Debug / screenshots: one of every foe (raiders of each kind, a wolf + an alpha) - or just `kinds` -
    // around `at` (a few: a line abreast along `yaw`), guarding where they stand. `frozen` ones hold
    // still facing along `yaw` (model close-ups).
    void debug_spawn_bestiary(const Vec3& at, f32 yaw, std::span<const u8> kinds = {}, bool frozen = false);
    // Where a town's notice board stands (the side quests are read there).
    static Vec3 notice_board(const worldgen::Village& town, u32 seed);

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
    void resync_driver_progress();  // when a player hands the cart back, resume the AI driver from the
                                    // route node nearest the cart's CURRENT position (no backtracking)
    void update_wheel(Timestep dt, const DensitySampler& density);  // wheel break / fetch / refit
    void update_cargo(Timestep dt, const DensitySampler& density);  // slide the bed crates, eject on bumps
    void end_contract_cleanup();                // clear haul state on delivery / wreck
    void append_wagon_colliders(std::vector<Collider>& out) const; // block players from carts
    void seat_occupants(const VehicleType& vt); // place pilot/riders/seated-driver on the vehicle
    void update_passenger(Timestep dt, const DensitySampler& density); // noble board/ride/disembark walk
    void start_passenger_disembark();                  // on delivery: send the noble off to a house
    std::optional<Vec3> town_house_door(Vec2 town_center, u32 pick); // a house's door in the named town
    // The active cart's bed is a moving platform: its top-surface height where (x,z) is over the
    // footprint (else a large-negative sentinel), so the controller can stand a player on top.
    f32 wagon_top_at(f32 x, f32 z) const;
    // After the cart moves, carry any player standing on top along with it (delta = this tick's move).
    void carry_top_riders(const Vec2& delta, const VehicleType& vt);
    void update_ambush(Timestep dt, const DensitySampler& density); // spawn the haul's ambush waves
    // Every hostile's AI (ambushers + quest foes), the players' attacks landing (melee, shots, heavy
    // blows, blasts), the dead culled + paid out, and the heroes' regen / respawn. Runs every tick.
    void update_combat(Timestep dt, const DensitySampler& density);
    void land_earthsplitter(ServerPlayer& player, f32 last_stand); // the Knight's charged heavy lands
    // --- Charged heavy attacks (Game/Abilities.cpp) ---
    void update_heavy(Timestep dt); // a released heavy: the Knight's blow is timed, the rest fire orbs
    // --- Side quests (Game/SideQuests.cpp) ---
    void update_quests(Timestep dt, const DensitySampler& density);
    void generate_quests(const worldgen::Village& town);
    void wake_quest(QuestRun& q, const DensitySampler& density); // place its camp / pack / chest / petals
    void finish_quest(QuestRun& q);                              // pay out + raise the banner
    void quest_foe_felled(u32 quest);                            // a camp raider / wolf down: progress
    void quest_dig(const Vec3& at);                              // a spade strike: does it hit the X?
    std::optional<Vec3> quest_site(const worldgen::Village& town, u32 salt) const;
    // --- Progression (Game/Progress.cpp) ---
    void assign_color(net::PlayerId id);              // the first identity colour no one else has
    void sync_identity(net::PlayerId id, ServerPlayer& player); // name + colour preference, each tick
    void restore_hero(ServerPlayer& player);          // adopt the client's saved hero (once per join)
    void update_progression(Timestep dt);             // learn requests + the journey, each tick
    void award_xp(ServerPlayer& player, u32 xp);      // add XP, re-derive the level
    // Every player within kXpShareRadius of a felled raider shares its XP (+ the journey kill tally).
    void award_kill(const Vec3& where, u8 kind);
    void award_delivery(u8 difficulty, f32 route_length); // every player: XP + the journey record
    u8 party_level() const;                           // the best level in the party (contract gating)
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
    // --- Cleric max-Aegis protective dome (Game/Abilities.cpp) ---
    void spawn_bubble(net::PlayerId owner); // raise a ranged-blocking dome around the caster
    void update_bubbles(Timestep dt);       // re-centre on the caster, age out, drop dead ones
    // --- Gauntlet co-op combos (Game/Abilities.cpp) ---
    void update_combos(Timestep dt);        // ticks combo cooldowns; the toss trigger + conduit channel
    // An Ally Toss landing: a radial burst on nearby enemies, scaled by the TOSSED ally's race
    // (a Dwarf is the heaviest cannonball).
    void toss_impact(const Vec3& at, Race race);
    // Damage multiplier applied to an ally's hit on an enemy standing in a FOCUS ZONE (a Knight
    // Consecration / Hunter Caltrops aura); 1.0 outside any zone.
    f32 focus_zone_mult(const Vec3& enemy_pos) const;
    // Amplify an ally's `base` damage to `e`: the FOCUS ZONE multiplier, plus ELEMENTAL SHATTER (extra
    // damage that consumes the chill when `e` is chilled and the hit is heavy). Called at every ally
    // damage site (melee / projectiles / abilities) so all of them get the combos. `allow_shatter` is
    // false for a Frost Bolt itself (so it applies chill rather than shattering its own).
    f32 combo_amp(Enemy& e, f32 base, bool allow_shatter = true);
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

    // One pre-computed townsfolk spawn slot in a town (a cottage dweller or a wall
    // guard): everything update_townsfolk needs to (re)spawn the villager cheaply.
    // The full house layout + garrison walk is far too expensive to run per tick,
    // so it's computed ONCE per town and cached here (keyed by vseed).
    struct TownSpawn {
        u32 id = 0;
        u8 kind = 0; // 0 = stroller, 2 = wall archer
        Vec3 position{0.0f};
        f32 yaw = 0.0f;
    };
    const std::vector<TownSpawn>& town_spawns(const worldgen::Village& v);

    net::NetServer server_;
    GameManager manager_;                     // day/night clock + game-mode orchestration
    WorldSampler sampler_;
    PropLibrary prop_lib_{false};             // colliders only - skip the vertex-AO bake
    std::optional<CollisionWorld> collision_; // built in start() once the seed is known
    std::vector<Collider> collider_scratch_;  // reused per player each tick
    std::unordered_map<net::PlayerId, ServerPlayer> players_;
    std::unordered_map<u32, std::vector<TownSpawn>> town_spawn_cache_; // vseed -> spawn slots
    f32 townsfolk_scan_cd_ = 0.0f; // seconds until the next spawn rescan (throttled)
    std::vector<Projectile> projectiles_;     // live thrown bodies
    std::vector<Enemy> enemies_;              // live hostile NPCs
    std::unordered_map<u32, Villager> villagers_; // townsfolk + guards (Villager.kind)
    std::unordered_map<u32, HouseFire> houses_;   // burnable buildings near players
    std::unordered_map<u32, u32> town_house_total_; // vseed -> #houses (for the tally)
    std::vector<Barricade> barricades_;           // player-built defences
    std::vector<Wall> walls_;                     // Mage rock walls (NPCs path around them)
    std::vector<Aura> auras_;                     // ground auras (heal / consecration)
    std::vector<BubbleShield> bubbles_;           // Cleric max-Aegis domes (block enemy ranged attacks)
    // A heavy orb's burst (Cleric Sunburst / Mage Comet), queued where it struck or landed and
    // resolved in update_combat: damage to foes in reach, heal to allies, maybe a crater.
    struct Blast {
        Vec3 at{0.0f};
        f32 radius = 0.0f;
        f32 damage = 0.0f;
        f32 heal = 0.0f;
        net::PlayerId owner = 0;
        bool crater = false;
    };
    std::vector<Blast> blasts_;
    // --- Side quests ---
    std::vector<QuestRun> quests_;
    std::vector<QuestItem> quest_items_;
    u32 quest_town_vseed_ = 0; // the town whose board the offers came from
    u32 quest_round_ = 0;      // bumps per finished / abandoned quest, so a board's next offers differ
    u32 next_quest_item_ = 1;
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
    bool progression_ = false;     // skill-tree gating + level-capped contract danger (see set_progression)
    bool debug_god_ = false;       // debug: players + active wagon ignore damage
    bool debug_no_ambush_ = false; // debug: no wagon ambushes spawn
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
    std::optional<Villager> passenger_;  // a noble riding a Passengers-kind covered wagon (kind 4)
    // The noble's lifecycle: walk from a source-town house TO the parked wagon (Boarding - the cart
    // waits), ride it (Aboard), then on delivery hop off and walk to a destination-town house before
    // vanishing (Leaving). A timer force-advances each walk so a snagged noble never stalls the haul.
    enum class PassengerPhase : u8 { None, Boarding, Aboard, Leaving };
    PassengerPhase passenger_phase_ = PassengerPhase::None;
    Vec3 passenger_target_{0.0f}; // where the noble is walking to (the wagon, or a house door)
    f32 passenger_timer_ = 0.0f;  // seconds spent in the current walk (board / disembark timeout)
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
    u32 contract_downs_ = 0;             // times a party member was downed this haul (-> unscathed bonus)
    u8 contract_outcome_ = 0;            // 0 none, 1 delivered, 2 wrecked (settle banner)
    u32 offer_town_vseed_ = 0;           // which town the current offers are from (0 = none)
    f32 settle_timer_ = 0.0f;            // banner hold before the next offer
    u32 next_wagon_id_ = 1;
    u32 next_ambush_id_ = 1;

    u32 tick_ = 0;

    // Tick profiler (see tick()): per-section ms accumulators over a logging window.
    // Sections: events, abilities, movement, townsfolk, contracts, snapshot.
    f64 prof_ms_[6] = {};
    f64 prof_max_ = 0.0;
    u32 prof_ticks_ = 0;
};

} // namespace alryn
