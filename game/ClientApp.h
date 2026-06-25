#pragma once

// Windowed multiplayer client (isometric third-person view). Owns the renderer,
// the menu/HUD, an optional in-process listen server, the networked snapshot and
// all of the client-side visuals. The implementation is split across several
// ClientApp*.cpp translation units grouped by concern (menu, input, character,
// world, wagons, vfx, hud); this header is the single class declaration they share.

#include <Alryn/Alryn.h>

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/ClothRig.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/OutfitMesh.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Character/Weapon.h>
#include <Alryn/Combat/Enemy.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Terrain/RoadNetwork.h>
#include <Alryn/Terrain/StreamingTerrain.h>
#include <Alryn/Terrain/WorldGen.h>
#include <Alryn/UI/UI.h>
#include <Alryn/World/PropLibrary.h>
#include <Alryn/World/VehicleTypes.h>
#include <Alryn/World/Village.h>

#include "GameConfig.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace alryn::game {

using namespace alryn; // engine types (Vec3, Renderer, net::, ui::, worldgen:: ...)

// --------------------------------------------------------------------------
//  Windowed multiplayer client (isometric third-person view).
// --------------------------------------------------------------------------
class ClientApp : public Application {
public:
    ClientApp(std::string host, bool host_local, u64 max_frames, bool auto_start)
        : Application(make_config(max_frames)), host_(std::move(host)), host_local_(host_local),
          auto_start_(auto_start), host_ip_(host_) {}

protected:
    void on_init() override;

    // Leaves the menu and connects: optionally hosting an in-process listen
    // server, then connecting the client. Terrain is created once the server's
    // Welcome arrives (see the SnapshotReceived/WelcomeReceived handling).
    void enter_game(bool host_local, std::string host);

    // Disconnects and returns to the main menu.
    void return_to_menu();

    // ---- In-game pause menu --------------------------------------------------
    void enter_pause() {
        paused_ = true;
        show_screen(Screen::Pause);
    }
    void resume() {
        paused_ = false;
        ui_.root().clear_children();
    }
    // ESC: in the main menu it backs out/quits; in-game it toggles the pause menu
    // (and Settings opened from pause backs out to the pause menu, not the game).
    void escape_pressed();
    void settings_back() { show_screen(paused_ ? Screen::Pause : Screen::Main); }

    // ---- Menu construction --------------------------------------------------
    enum class Screen { Main, Join, Settings, Customise, Class, Pause };

    Vec2 pointer_pos() {
        if (Input* in = input()) {
            return in->mouse_position();
        }
        return Vec2{0.0f};
    }

    void menu_escape() {
        if (current_screen_ == Screen::Main) {
            close();
        } else {
            show_screen(Screen::Main);
        }
    }

    void show_screen(Screen screen) {
        current_screen_ = screen;
        rebuild_ui();
    }

    // Rebuilds the current screen's widgets for the live framebuffer size. Called
    // on navigation and on resize so the menu always stays centred.
    void rebuild_ui();

    void build_pause(f32 w, f32 h);

    void add_title(f32 w, f32 h, const char* heading, const char* sub);

    void build_main(f32 w, f32 h);

    void build_join(f32 w, f32 h);

    void build_settings(f32 w, f32 h);

    void build_customise(f32 w, f32 h);

    // Class-selection screen shown when hosting / joining (the player picks their combat role
    // "on joining"). Selecting a class re-lays the screen to highlight it; START enters the game
    // with the pending host/join intent recorded when this screen was opened.
    void build_class(f32 w, f32 h);

    void rebuild_preview() {
        preview_model_ = CharacterModel::create(kPreviewSeed, appearance_);
        // Show the role's full (master) outfit in the chosen colour, so the turntable previews the
        // class + the colour pick (in-game you start ragged and buy up to this).
        Equipment eq = equip_loadout_;
        eq.outfit_tier = 3;
        eq.weapon_tier = 3;
        apply_outfit(preview_model_, outfit_kind_for_role(static_cast<u8>(role_)), eq);
    }

    void apply_resolution(usize idx);

    void on_update(Timestep dt) override;

    void on_render() override;

    // Renders the customisation turntable: the live character centred in the area
    // left of the controls panel. The camera distance + horizontal offset are
    // derived from the window aspect and the panel position so the whole avatar
    // (head to feet) always fits without clipping, in any window shape.
    void draw_preview();

    // Draws a posed character's bones as primitives, each with its palette colour (times `tint`)
    // and shape mesh. The tint lets enemies read as hostile without new models. With
    // `attachments_only`, draws just the face/hair/equipment pieces that ride on top of the skinned
    // body (Bone::attachment) - the continuous body mesh covers the core body + joint fillers.
    void draw_rig(const CharacterModel& model, const std::vector<Mat4>& mats,
                  const Vec3& tint = Vec3{1.0f}, bool attachments_only = false);

    // Draws a spear gripped in the character's right hand (anchored to the lower-arm
    // bone so it swings with the animation, not a stick floating beside them).
    void draw_held_spear(const CharacterModel& model, const std::vector<Mat4>& mats);

    // World position of a hand (the far end of a forearm), in the forearm JOINT frame.
    static Mat4 hand_frame(const CharacterModel& model, const std::vector<Mat4>& jmats, BonePart arm);

    // The role's weapon(s), RIGIDLY gripped in the hand JOINT frame so they rotate WITH the arm - a
    // Knight's sword swings with the attack animation (it IS the blade you hold) and the shield
    // raises with the block. NOTE: the rig's bone labels are mirrored - the *L* arm is on the
    // player's RIGHT (the main-hand weapon), the *R* arm on their LEFT (the off-hand shield/dagger).
    // Built from the shared modular weapon_pieces (Character/Weapon.h).
    void draw_role_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                          PlayerRole role, const Equipment& eq);
    void draw_weapon(WeaponType type, const Mat4& hand, const CharacterPalette& pal,
                     EquipmentTier tier);
    const Mesh& shape_mesh(BoneShape s) const; // BoneShape -> the matching unit shape mesh

    // The Cleric's staff held VERTICAL like a walking stick: the hand grips the top, the shaft
    // drops to the ground, and as they walk the tip plants ahead then drifts back (and lifts to
    // swing forward again), synced to the gait. Idle = a still, upright staff.
    void draw_cleric_staff(const CharacterModel& model, const std::vector<Mat4>& jmats,
                           const Vec3& feet, const CharacterAnimator& anim, f32 yaw);

    // Standing IDLE poses (when a player is still + not acting), so they don't just hang both arms
    // down: a staff/mace user (Mage / Cleric) rests their weapon hand on the weapon planted like a
    // walking stick; the Hunter holds the bow lowered at their side. apply_idle_stance overrides the
    // weapon-arm bones in `pose`; draw_planted_weapon draws the weapon stood on the ground.
    void apply_idle_stance(const CharacterModel& model, std::vector<Quat>& pose, PlayerRole role,
                           f32 weight = 1.0f) const;
    void draw_planted_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                             const Vec3& feet, PlayerRole role, const Equipment& eq);

    // Cast an ability by its index (0..kAbilityCount-1) for the local player: gate on the client
    // cooldown estimate, queue it for the server (as index+1), mirror the cooldown for the HUD, and
    // play the cast VFX + any buff aura instantly so it feels responsive (server stays authoritative).
    void cast_ability(u8 ability);

    // Press a hotbar slot (0..kAbilitySlots-1): casts whatever ability the player has equipped there.
    void cast_bar_slot(u8 slot) {
        if (slot < kAbilitySlots && bar_[slot] >= 0) {
            cast_ability(static_cast<u8>(bar_[slot]));
        }
    }

    // Equip / unequip an ability (from a skills-tree click): if it's already on the bar, clear that
    // slot; otherwise drop it into the first empty slot (replacing the last slot if the bar is full).
    void equip_ability(u8 ability);

    // Mouse interaction with the bottom action bar (hit-tested against ability_slot_rects_): begin a
    // drag on press, follow the cursor, and on release swap the two slots to reorder the bar. Returns
    // true if the press/release was on the bar (so the in-game handler can swallow it from melee).
    bool abilitybar_press(const Vec2& p);
    bool abilitybar_release(const Vec2& p);

    // A click inside the open skills tree: hit-test the ability nodes and equip/unequip the one hit.
    void skills_click(const Vec2& p);

    // A quick flourish at the hand for a Hunter/Cleric primary attack (the projectile itself is
    // server-spawned + networked; this is just the instant local muzzle/cast feedback).
    void spawn_primary_vfx();

    void on_event(Event& event) override;

    void on_shutdown() override;

private:
    // A flowing cloth piece on a character: a spring-bone ClothChain simulated in WORLD space (so it
    // lags with the body's motion), rebuilt into a dynamic mesh each frame. The anchor rides a body
    // joint; the chain hangs along `hang_local` (the character's local frame). Detachable (phase 3).
    struct ClothInstance {
        std::vector<ClothChain> chains;      // 1 = a flat sheet (cape); N = a ring tube (skirt / robe)
        std::vector<Vec3> anchor_locals;     // per-chain anchor offset (local frame, from `anchor` bone)
        std::vector<Vec3> hang_locals;       // per-chain rest hang direction (local)
        Mesh mesh;                           // dynamic, rebuilt from the sim each frame
        Vec3 color{0.5f};
        BonePart anchor = BonePart::Torso;   // body joint the piece rides
        Vec3 side_local{1.0f, 0.0f, 0.0f};   // sheet left-right axis (cape only)
        bool ring = false;                   // closed tube (skirt) vs flat sheet (cape)
        int segments = 5;
        f32 seg = 0.13f;
        f32 half_width = 0.22f;
        f32 collide_r = 0.2f; // body collision-cylinder radius for this piece (tight for a back cape,
                              // wide for a leg skirt) - keeps it OUT of the body so it drapes/rests on it
        bool inited = false;
        bool detached = false; // cut / blown off: the chains free-fall in world space, then despawn
        f32 detach_age = 0.0f; // seconds since detaching (lingers on the ground, then sinks + is removed)
    };

    // Cut / blow a cloth piece off a character: free its chains (free-fall) with a velocity kick, so it
    // flutters away. `impulse` is a per-step world velocity (the cut/wind direction).
    void detach_cloth(ClothInstance& c, const Vec3& impulse);
    // Per-frame detach triggers for all players' cloth: a cut when health drops, a blow-off in a storm.
    void update_cloth_triggers();

    struct PlayerVisual {
        CharacterModel model;
        CharacterAnimator animator;
        CharacterAppearance appearance;
        Equipment equipment;  // the gear the model is built for (rebuild when it changes)
        u8 role = 255;  // PlayerRole the model is built for (255 = none yet -> force a build)
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        bool has_last = false;
        u8 last_action = 0;     // to fire a swing once on the rising edge of a networked action
        u8 last_health = 255;   // previous snapshot health % (255 = unseen) - a drop can cut cloth
        SkinnedMesh body_skin;  // continuous body geometry + bone weights (built with the model)
        Mesh body_mesh;         // dynamic GPU mesh, re-skinned from the posed joints every frame
        SkinnedMesh outfit_skin; // continuous worn equipment (armoured/clothed limbs, torso, skirt)
        Mesh outfit_mesh;        // dynamic GPU mesh for the outfit, re-skinned with the same joints
        std::vector<ClothInstance> cloth; // simulated flowing pieces (cape, skirt, ...)
    };

    // Set up a character's flowing cloth pieces for its role + gear (called when the visual is built).
    void setup_cloth(PlayerVisual& v, PlayerRole role, const Equipment& eq);
    // Step + rebuild + draw a character's cloth pieces. World-space sim (anchor from the posed joints,
    // renderer wind), mesh localised to `root` so culling stays correct.
    void draw_cloth(PlayerVisual& v, const Mat4& root, const std::vector<Mat4>& jmats, const Vec3& tint);

    // Skins one continuous SkinnedMesh with `model`'s posed joints (in LOCAL space) into the dynamic GPU
    // mesh `gpu` (created on first use) and draws it at `root` (its model matrix) through the lit
    // pipeline. Shared by players (body + outfit), villagers and enemies.
    void skin_and_draw(const CharacterModel& model, const SkinnedMesh& src, Mesh& gpu, const Mat4& root,
                       const std::vector<Quat>& pose, const Vec3& tint = Vec3{1.0f});

    // Skins the player's continuous body + worn outfit and draws them; the face/hair/gear attachment
    // primitives are laid on top by the caller.
    void draw_skinned_body(PlayerVisual& v, const Mat4& root, const std::vector<Quat>& pose,
                           const Vec3& tint = Vec3{1.0f});

    // A dynamic GPU mesh can't be freed the instant its owner (a slain enemy / culled villager) goes
    // away - a frame that drew it may still be in flight. Retire it here; tick_mesh_graveyard frees it
    // a few frames later (mirrors the terrain mesh deferral).
    void retire_mesh(Mesh&& m);
    void tick_mesh_graveyard();

    // A networked enemy's renderable: one shared hostile model, animated from
    // snapshot position deltas (no animation data on the wire).
    struct EnemyVisual {
        CharacterModel model = CharacterModel::create(0u, enemy_look());
        CharacterAnimator animator;
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        u8 last_action = 0;
        SkinnedMesh body_skin; // continuous body, built on first sight; re-skinned each frame
        Mesh body_mesh;        // dynamic GPU mesh
    };

    PlayerVisual& ensure_visual(net::PlayerId id, const CharacterAppearance& appearance, u8 role,
                                const Equipment& equipment);

    // Advances the time of day and feeds the renderer a moving sun + sky colour.
    // When connected, the server owns the clock (so lighting matches when villagers
    // sleep); otherwise we advance it locally. ALRYN_TIME (0..1) pins the starting
    // time; ALRYN_DAY_SECONDS sets cycle length.
    void update_day_night(Timestep dt);

    void update_camera();

    void update_visuals(Timestep dt);

    // The local player's authoritative state from the latest snapshot (or null before one arrives).
    const net::PlayerState* local_player() const {
        if (have_snapshot_) {
            for (const net::PlayerState& p : snapshot_.players) {
                if (p.id == my_id_) {
                    return &p;
                }
            }
        }
        return nullptr;
    }

    // The local player's health fraction (0..1) from the snapshot.
    f32 local_health() const {
        if (have_snapshot_) {
            for (const net::PlayerState& p : snapshot_.players) {
                if (p.id == my_id_) {
                    return static_cast<f32>(p.health) / 100.0f;
                }
            }
        }
        return 1.0f;
    }

    // Tracks a damage flash: when the local player's health drops, flare the screen red.
    void update_feedback(Timestep dt);

    // ---- Particle VFX ------------------------------------------------------------
    void emit(const Vec3& pos, const Vec3& vel, const Vec4& color, f32 life, f32 size,
              u8 style = 0, f32 gravity = 0.0f, f32 drag = 1.6f);

    // A spray of `n` motes from `center`, biased upward by `up` (m/s), with random speed.
    void emit_burst(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                    u8 style = 1, f32 up = 0.0f, f32 gravity = 0.0f);

    // A flat expanding ring of motes on the ground (radius grows via outward velocity).
    void emit_ring(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                   u8 style = 1);

    void update_particles(Timestep dt);

    // The glowing ground disc + soft dome of each ground aura, plus a soft light at night so the
    // aura lights its surroundings. Colour comes from the shared aura_props table (data-driven, so
    // a new aura kind renders + lights itself with no extra client code). Rising motes are emitted
    // in update_particles. Drawn additively so it brightens the ground without occluding.
    void draw_auras();

    // The Aegis protective bubble around any shielded player / NPC: a softly pulsing translucent
    // shell + an additive glow, brighter while the shield is strong. (Shimmer motes orbit it from
    // update_particles.)
    void draw_shields();

    // Co-op buff auras under empowered (fiery ring) / hasted (green ring) players, so allies can
    // read who the Cleric/Hunter/Mage has buffed. Pulses; driven by PlayerState.buffs bitflags.
    void draw_buffs();

    void draw_oxen(const Vec3& pos, f32 yaw); // a yoked pair of draft oxen pulling the wagon
    void update_deer(Timestep dt);            // wander/graze/flee the ambient deer (client-side)
    void draw_deer();
    void draw_particles();
    // Ambient wildlife VFX (no networking): a flock of birds drifting across the sky by day, and
    // at night a slow gliding owl plus fireflies. The fireflies are anchored to fixed WORLD cells
    // (each drifts gently about its own spot), so the player walks past them through the world
    // rather than carrying a screen-locked swarm.
    void draw_ambient_life();

    // The showy burst for an ability cast, played for whoever cast it (the local player on
    // keypress for instant feel; remote players when the snapshot reports their `cast`).
    void spawn_ability_vfx(PlayerRole role, u8 slot, const Vec3& feet, f32 yaw, const Vec3& aim);

    // Decays the local buff auras (emitting trailing motes while active) and plays cast VFX
    // for remote players from the snapshot's `cast` field (deduped by tick so each fires once).
    void update_ability_vfx(Timestep dt);

    // A menacing low-poly look shared by all enemies (dark skin, sharp eyes, spiky
    // hair); a red tint at draw time makes them read as hostile.
    static CharacterAppearance enemy_look();

    // Animate enemies from snapshot deltas, like remote players, and drop visuals
    // for enemies that have died / left the snapshot.
    void update_enemy_visuals(Timestep dt);

    void draw_enemies();

    // Town gates that swing open when a player or NPC approaches. Purely client-side + visual
    // (the gap is already passable): update_gates rebuilds the nearby-gate list each frame and
    // eases each gate's open amount toward "someone is near"; draw_gates draws the two leaves.
    void update_gates(Timestep dt);
    void draw_gates();
    // Plank bridges where a road crosses a river: gathered deterministically (roads::bridges) near
    // the player and drawn as the unit bridge mesh stretched to each crossing's span, level with the
    // road on the banks.
    void draw_bridges();

    // Player-built barricades: a low palisade of wooden stakes + rails, darkening as
    // the enemy hacks it down (health from the snapshot).
    void draw_barricades();

    // Mage rock walls: a row of jagged stone chunks raised across the caster's facing (rendered in
    // the same rotated frame as the server collider, so the visible wall matches what NPCs route
    // around). The wall crumbles - shorter + darker - as enemies smash its health down.
    void draw_walls();

    // Floating health bars above combatants (enemies always; guards always; villagers
    // only when hurt), projected from world space into the 2D UI overlay.
    void draw_health_bars();

    // Burning houses: a cluster of flickering emissive flame tongues that engulf the
    // whole cottage (spread across its footprint, licking up to the roof), dark smoke
    // billowing above, an additive firelight bloom and a strong warm light (day or
    // night). The server sends position + intensity; a low intensity is the smouldering
    // ember of a burnt-down ruin (small flames, lots of smoke).
    void draw_fires();

    // The burning intensity (0..1) of the house at world position `p`, from the
    // server's fire list - used to char the cottage and swap its cosy glow for flames.
    f32 house_burn(const Vec3& p) const;

    // The cart's terrain-following orientation + bob. It simply sits ON the ground: pitched
    // along its travel direction and rolled across it to match the slope under the wheels (no
    // tilt/flip dynamics - any drama is the physical cargo sliding around). Bob is a speed jiggle.
    void wagon_orient(const net::WagonState& wg, f32 moved, f32& pitch, f32& roll, f32& bob) const;

    Mat4 wagon_model(const net::WagonState& wg, f32 moved) const;

    // Re-applies the cart's lean + bob to a rider's flat (server) seat position, so a seated
    // player/driver rides with the cart instead of floating above its tilt.
    Vec3 attach_to_wagon(const net::WagonState& wg, const Vec3& flat_world) const;

    // The single active cargo wagon being hauled (riders attach to it), or nullptr.
    const net::WagonState* active_wagon() const {
        if (have_snapshot_ && snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active) &&
            !snapshot_.wagons.empty()) {
            return &snapshot_.wagons.front();
        }
        return nullptr;
    }

    // This frame's cart displacement (for the bob), from the position cached in draw_wagons.
    f32 wagon_frame_move(const net::WagonState& wg) const {
        const auto it = wagon_prev_.find(wg.id);
        if (it == wagon_prev_.end()) {
            return 0.0f;
        }
        return glm::length(Vec2{wg.position.x - it->second.x, wg.position.z - it->second.z});
    }

    // Draws the networked wagons (the parked offers, then the active cargo): the body
    // plus four wheels that spin as the cart rolls (roll accumulated from its motion). The
    // wagon you're voting for is tinted gold; a damaged one darkens toward wrecked.
    void draw_wagons();

    // Cargo crates: in-bed ones (loose==0) ride in the cart (their position is bed-local, so we
    // place them through the cart transform - they slide + tilt + bob with it); fallen ones
    // (loose==1) lie on the ground at a world position until picked up (E).
    void draw_goods();

    // A crate held in front of a player who is carrying a spilled good back to the cart.
    void draw_carried_good(const Vec3& feet, f32 yaw);

    // Renders a wagon's two verlet harness traces as a chain of short oriented links (the
    // node positions are simulated in update_ropes from the authoritative endpoints).
    void draw_ropes(u32 id);

    // Simulates the harness traces: two ropes (left/right) per horse-drawn wagon, each a
    // verlet chain pinned to the carriage shaft tip and the horse's collar, sagging under
    // gravity and swinging as the rig moves - so the rope is real physics, not a fixed line.
    void update_ropes(Timestep dt);

    // Draws the carriage's horse with a simple diagonal leg gait driven by its motion.
    void draw_horse(const Vec3& pos, f32 yaw);

    // Project a world point to screen pixels; returns false if behind/off camera.
    bool world_to_screen(const Vec3& world, f32 W, f32 H, Vec2& out) const;

    // The in-game HUD: shared party money, the wagon-contract objective (choose an offer,
    // or the active delivery + a destination arrow), and the local player's health bar.
    void draw_hud();

    // The floating contract panel shown beside a wagon you've walked up to: where it's bound
    // (town name), how far, the danger, and the pay - plus ACCEPT / CANCEL buttons (or, once
    // accepted, a WAITING tally + CANCEL). Stores the button rects for click hit-testing.
    void draw_contract_panel(ui::DrawList& draw, const net::WagonState& wg, bool accepted, f32 W,
                             f32 H, f32 ts, const Vec3& feet);

    // The role's signature accent colour (also tints the ability bar + icons).
    static Vec3 role_color(PlayerRole role);

    // The three role abilities (keys 1/2/3) as a polished bottom-centre bar: a backing
    // panel, one rounded slot each with a vector icon, a key badge, the name, and a radial
    // cooldown wipe (a dark overlay that drains as the ability recovers + the seconds left).
    void draw_ability_bar(ui::DrawList& draw, f32 W, f32 H, f32 ts);

    // A vector glyph for ability (role, slot), centred at (cx,cy) with ~r radius, drawn from
    // rounded-cap lines + rects so it reads at a glance (sword / shield / bow / cross / bolt …).
    void draw_ability_icon(ui::DrawList& draw, PlayerRole role, u8 slot, f32 cx, f32 cy, f32 r,
                           const Vec4& c);

    // A bold arrow near the top of the screen pointing from the player toward the wagon's
    // destination (world bearing mapped through the fixed iso camera).
    void draw_dest_arrow(ui::DrawList& draw, const Vec3& from, const Vec3& to, f32 W);

    // Full-screen world map: the towns near the player and the roads between them
    // (computed deterministically from the shared seed via roads::gather + village_at),
    // with the player's position + facing. Toggled with M.
    void draw_map();

    // Full-screen skills tree (toggled with K): the chosen role's crest branching to its
    // four cooldown-gated abilities, each with its key, icon, cooldown and a description.
    // Medieval-styled; purely an info overlay (world input is frozen while it's open).
    void draw_skills();
    void draw_wardrobe();              // the gear/wardrobe overlay (U)
    void wardrobe_click(const Vec2& p); // buy / recolour / change-weapon hit-testing

    // Weather: precipitation + lightning, driven by the eased `weather_amt_` (from the networked
    // weather). `draw_rain` is the WORLD-SPACE rain - a column of falling streaks anchored to world
    // cells around the player (so the camera sees real parallax) at a constant fall speed (so fading
    // rain never appears to run backwards); drawn in the 3D scene pass. `draw_weather` is just the
    // genuinely screen-space part: the full-screen lightning flash. The sky/sun/fog/wind are
    // modulated in update_day_night.
    void draw_rain();
    void draw_weather();

    void draw_prop(const PropInstance& p);

    void draw_character(PlayerVisual& v, const Vec3& feet, f32 yaw, bool seated = false,
                        int role = -1);

    // ---- Village NPCs (server-authoritative; the player defends them) -------
    // Villagers are simulated on the server (wander/sleep/flee, killable by enemies)
    // and arrive in the snapshot; the client just renders + animates them, rebuilding
    // a model when its appearance first appears, and culls visuals that have died /
    // left the snapshot.
    void update_villager_visuals(Timestep dt);

    PlayerVisual& ensure_villager_visual(u32 id, const CharacterAppearance& appearance);

    void draw_villagers();

    void send_input();

    Vec3 local_feet() const {
        if (have_snapshot_) {
            for (const net::PlayerState& p : snapshot_.players) {
                if (p.id == my_id_) {
                    return p.position;
                }
            }
        }
        return Vec3{0.0f, 5.0f, 0.0f};
    }

    // The id of the offered wagon the local player is standing next to (within
    // kWagonStartRange), or 0. Walking up to a parked offer is how a haul is started.
    static constexpr f32 kWagonStartRange = 4.0f;
    u32 nearest_offer_in_range() const;
    bool wagon_offered(u32 id) const {
        for (const net::WagonState& wg : snapshot_.wagons) {
            if (wg.id == id) {
                return true;
            }
        }
        return false;
    }
    const net::WagonState* wagon_by_id(u32 id) const {
        for (const net::WagonState& wg : snapshot_.wagons) {
            if (wg.id == id) {
                return &wg;
            }
        }
        return nullptr;
    }
    static bool in_rect(const Vec2& p, const ui::Rect& r) {
        return r.w > 0.0f && p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
    }

    // A deterministic medieval name for the town centred at `c` (so the contract panel can say
    // where the wagon is bound). Stable per town because `c` is the town's fixed centre.
    static std::string town_name(const Vec3& c);


    // Unproject the cursor through the iso camera onto the terrain (for digging).
    void update_aim();

    // A rotation that maps the mesh's local +Z axis onto `dir` (used to point arrows along
    // their flight). Falls back to identity for a degenerate direction.
    static Mat4 orient_to(const Vec3& dir);

    Mat4 tree_model(const TreeInstance& t) const {
        return glm::translate(Mat4{1.0f}, t.position) *
               glm::rotate(Mat4{1.0f}, t.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
               glm::scale(Mat4{1.0f}, Vec3{t.scale});
    }
    usize tree_index(const TreeInstance& t) const {
        return tree_library_.empty() ? 0 : static_cast<usize>(t.variant) % tree_library_.size();
    }

    static ApplicationConfig make_config(u64 max_frames);

    enum class AppState { Menu, Playing };

    std::string host_;
    bool host_local_ = true;
    bool auto_start_ = false;
    AppState state_ = AppState::Menu;
    GameServer local_server_;
    Renderer* renderer_ = nullptr;

    // Menu / settings.
    ui::UIContext ui_;
    Screen current_screen_ = Screen::Main;
    bool paused_ = false;   // in-game pause menu (overlaid on the live game)
    UVec2 ui_extent_{0, 0}; // last framebuffer size the menu was laid out for
    std::string host_ip_ = "127.0.0.1";
    Vec3 menu_sky_{0.05f, 0.06f, 0.09f};
    bool vsync_ = true;
    usize res_index_ = 0;
    int render_distance_ = 4;

    // Character customisation + its turntable preview.
    static constexpr u32 kPreviewSeed = 7u;
    CharacterAppearance appearance_;
    // The local player's gear loadout sent to the server (which clamps the tiers to what's owned).
    // Tiers default to master = "equip the best I own"; the tint + weapon are set by customise/wardrobe.
    Equipment equip_loadout_{3, 3, 0, 0};
    PlayerRole role_ = PlayerRole::Knight;          // chosen combat role (weapon + abilities)
    u8 pending_ability_ = 0;                         // ability index+1 invoked this frame (0 = none)
    // Mage elemental combo casting: hold Ctrl (casting_), tap element keys (1-4 or W/A/S/D) to fill
    // `combo_`, release Ctrl to cast the spell `spell_for_combo` resolves. `pending_spell_` is sent.
    bool casting_ = false;
    u8 combo_[kMaxCombo] = {};
    u8 combo_n_ = 0;
    u8 pending_spell_ = 0; // SpellId to send this frame (0 = none)
    f32 mage_cd_ = 0.0f;   // client-side Mage spell-cooldown estimate (for the HUD + cast gating)
    // Maps an element/digit key to an Element (0..3) while casting, or -1.
    static int key_to_element(KeyCode k);
    // Resolve the queued combo into a SpellId (0 = none).
    u8 resolve_combo() const;
    // Queue a Mage spell to cast this frame: gate on the client cooldown estimate, send it, mirror
    // the cooldown for the HUD, and play the instant cast VFX. Used by the hotkeys, combos + click.
    void cast_mage_spell(SpellId sp);
    f32 ability_cd_[kAbilityCount] = {};             // client-side HUD cooldown estimate (per ability)

    // The customisable action bar: which ability index sits in each hotbar slot (keys 1..4);
    // -1 = empty. Defaults to the first four abilities so the bar is populated out of the box.
    // Edited from the skills tree (click to equip) and by click-dragging slots to reorder.
    int bar_[kAbilitySlots] = {0, 1, 2, 3};
    int drag_slot_ = -1;                             // bar slot being click-dragged (-1 = none)
    ui::Rect ability_slot_rects_[kAbilitySlots] = {}; // bar slot rects (from draw_ability_bar)
    ui::Rect skill_node_rects_[kAbilityCount] = {};  // tree node rects (from draw_skills)

    // Pending host/join intent recorded when the Class screen opens; START there enters the game.
    bool pending_host_local_ = true;
    std::string pending_host_ip_ = "127.0.0.1";
    f32 bulwark_fx_ = 0.0f;                          // local: Knight shield-dome aura timer
    f32 dash_fx_ = 0.0f;                             // local: Hunter speed-trail aura timer
    f32 heal_charge_fx_ = 0.0f;                      // local: Cleric heal-channel charge (0..kHealChargeTime)
    std::unordered_map<net::PlayerId, u32> ability_fx_tick_; // dedupe networked cast VFX

    // A lightweight client-side particle (ability VFX, projectile trails). Drawn as an
    // emissive or additive-glow sphere that fades + shrinks over its life.
    struct Particle {
        Vec3 pos{0.0f};
        Vec3 vel{0.0f};
        f32 life = 0.0f;
        f32 max_life = 1.0f;
        f32 size = 0.1f;
        f32 gravity = 0.0f;
        f32 drag = 1.6f;
        Vec4 color{1.0f};
        u8 style = 0; // 0 = emissive, 1 = additive glow
    };
    std::vector<Particle> particles_;
    u32 fx_rng_ = 0x9e3779b9u;
    f32 frand();
    f32 frand(f32 a, f32 b) { return a + (b - a) * frand(); }
    Vec3 rand_dir();
    CharacterModel preview_model_ = CharacterModel::create(kPreviewSeed, CharacterAppearance{});
    CharacterAnimator preview_anim_;
    f32 preview_turn_ = 0.6f;
    ui::Rect customise_panel_{}; // the controls card; the preview fills the area left of it
    net::NetClient client_;
    std::unique_ptr<StreamingTerrain> terrain_;
    struct TreeVisual {
        Mesh trunk;
        Mesh foliage;
    };

    struct GpuPropPart {
        Mesh mesh;
        PropLayer layer = PropLayer::Opaque;
    };
    struct GpuProp {
        std::vector<GpuPropPart> parts;
        std::vector<PropLight> lights; // lantern / hearth / brazier spot lights
        Vec2 footprint{0.0f};          // house interior half-extents (0 = not a house)
        f32 wall_height = 0.0f;
    };

    std::unordered_map<net::PlayerId, PlayerVisual> visuals_;
    std::unordered_map<u32, EnemyVisual> enemy_visuals_;     // networked hostile NPCs
    std::unordered_map<u32, PlayerVisual> villager_visuals_; // networked town NPCs
    std::vector<TreeVisual> tree_library_;
    PropLibrary prop_lib_;
    std::vector<GpuProp> gpu_bushes_;
    std::vector<GpuProp> gpu_rocks_;
    std::vector<GpuProp> gpu_logs_;
    std::vector<GpuProp> gpu_fences_;
    std::vector<GpuProp> gpu_fence_rails_;
    std::vector<GpuProp> gpu_lanterns_;
    std::vector<GpuProp> gpu_houses_;
    std::vector<GpuProp> gpu_walls_;
    std::vector<GpuProp> gpu_gates_;
    std::vector<GpuProp> gpu_wells_;
    std::vector<GpuProp> gpu_bridges_;
    std::vector<GpuProp> gpu_markets_;
    std::vector<GpuProp> gpu_paths_;
    std::vector<GpuProp> gpu_planters_;
    std::vector<GpuProp> gpu_fountains_;
    std::vector<GpuProp> gpu_decor_;
    std::vector<GpuProp> gpu_rivers_;
    std::vector<GpuProp> gpu_crystals_;
    std::vector<GpuProp> gpu_glow_shrooms_;
    std::vector<GpuProp> gpu_campfires_;
    std::vector<GpuProp> gpu_monuments_;
    std::vector<GpuProp> gpu_watchtowers_;
    Mesh shape_box_;
    Mesh shape_sphere_;
    Mesh shape_cylinder_;
    Mesh shape_capsule_;
    Mesh shape_rounded_;
    std::vector<Vertex> skin_scratch_; // reused buffer for CPU-skinning a body each frame (no per-frame alloc)
    std::vector<std::pair<int, Mesh>> mesh_graveyard_; // retired NPC body meshes, freed after a few frames
    Mesh marker_;
    Mesh water_mesh_;
    Mesh bridge_mesh_stone_; // unit stone arch bridge (x:-0.5..0.5), stretched per river crossing
    Mesh bridge_mesh_wood_;  // unit wooden plank bridge (Bridge.kind picks stone vs wood)
    Mesh gate_door_mesh_; // a unit gate-door leaf (x:0..1 hinge->free), drawn x2 per town gate

    // Town gates near the player, rebuilt each frame (open: 0 closed .. 1 swung open).
    struct GateVisual {
        Vec3 pos;     // gate opening centre, on the ground
        Vec2 radial;  // outward radial direction (xz)
        f32 half = 2.6f; // opening half-width (matches the wall gap; wider gates span more roads)
        f32 open = 0.0f;
    };
    std::vector<GateVisual> gates_;
    std::unordered_map<u64, f32> gate_open_; // eased open amount, keyed by gate position hash

    f32 elapsed_ = 0.0f;
    f32 frame_dt_ = 1.0f / 60.0f; // last frame's dt, for per-frame sims stepped during rendering (cloth)
    f32 time_of_day_ = daynight::start_time; // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset
    f32 day_seconds_ = daynight::default_day_seconds;
    f32 sun_intensity_ = 1.0f; // cached from the day/night cycle (0 night .. 1 day)
    f32 fog_gloom_ = 0.0f;     // eased 0..1 town-gloom factor (denser/cooler fog + grade in towns)
    f32 fog_patch_ = 0.0f;     // eased 0..1 road fog-bank strength (occasional dense volumetric mist)
    f32 weather_amt_ = 0.0f;   // eased 0..1 storminess (from the networked weather) - rain/sky/wind
    f32 lightning_ = 0.0f;     // current lightning-flash brightness (decays)
    f32 lightning_cd_ = 4.0f;  // seconds until the next storm flash
    f32 cam_distance_ = iso::distance; // scroll-wheel zoom
    Camera camera_;

    net::PlayerId my_id_ = 0;
    u32 world_seed_ = 0;     // shared world seed (from Welcome) - for the map's town/road graph
    bool map_open_ = false;  // full-screen map overlay (M)
    bool skills_open_ = false; // full-screen skills tree overlay (K)
    bool wardrobe_open_ = false; // gear / wardrobe overlay (U): buy tiers, recolour, change weapon
    u8 pending_buy_ = 0;       // shop: the gear tier we're trying to buy up to (sent in PlayerInput.buy)
    ui::Rect wardrobe_buy_rect_ = {};        // the "buy upgrade" button (from draw_wardrobe)
    ui::Rect wardrobe_weapon_rect_ = {};     // the "change weapon" button
    ui::Rect wardrobe_swatch_rects_[8] = {}; // the recolour swatches

    // World-map view state: a pannable, zoomable terrain minimap. `map_center_` is the world XZ
    // the map is centred on (set to the player when opened, then moved by dragging); `map_ppm_` is
    // the current pixels-per-metre (written by draw_map, read by the drag handler in on_update).
    Vec2 map_center_{0.0f};
    f32 map_zoom_ = 1.0f;
    f32 map_ppm_ = 1.0f;
    bool map_dragging_ = false;
    Vec2 map_drag_last_{0.0f};
    // Cached terrain-relief raster (rebuilt only when the view changes): one (rect, colour) tile per
    // grid cell, so panning/zooming doesn't recompute world noise every frame.
    std::vector<std::pair<Vec4, Vec4>> map_tiles_;
    Vec2 map_raster_center_{1e9f, 1e9f};
    f32 map_raster_zoom_ = -1.0f;
    UVec2 map_raster_ext_{0, 0};
    void rebuild_map_raster(const Vec4& panel, f32 ppm);
    net::Snapshot snapshot_;
    bool have_snapshot_ = false;

    f32 face_yaw_ = 0.0f;
    u32 sequence_ = 0;
    bool pending_add_ = false;
    bool pending_fire_ = false;
    bool pending_attack_ = false;
    bool pending_build_ = false;
    bool pending_local_swing_ = false; // play our own swing animation this frame (left-click)
    bool blocking_ = false;            // Knight holding the shield up (right mouse held)
    bool pending_rally_ = false;
    bool pending_grab_ = false; // one-shot hitch/unhitch the nearest wagon
    u32 selected_wagon_ = 0;    // wagon id this client has ACCEPTED (its vote; 0 = none)
    u32 near_wagon_ = 0;        // offered wagon currently in range (shows its info panel)
    u32 panel_wagon_ = 0;       // wagon the on-screen Accept/Cancel buttons act on
    ui::Rect accept_btn_{};     // screen rect of the panel's ACCEPT button (0 = not shown)
    ui::Rect cancel_btn_{};     // screen rect of the panel's CANCEL button
    u8 vote_mode_ = 1;          // 1 = hire driver, 2 = haul manually
    std::vector<Mesh> vehicle_meshes_; // one body mesh per VehicleType (cart/wagon/carriage)
    Mesh wagon_wheel_mesh_;     // a single wheel, drawn x4 (scaled per type) and spun
    Mesh horse_body_mesh_;      // the carriage puller
    Mesh horse_leg_mesh_;       // a single leg, drawn x4 with a gait swing
    Mesh ox_body_mesh_;         // a draft ox (the cargo wagon is pulled by a yoked pair)
    Mesh ox_leg_mesh_;
    Mesh deer_body_mesh_;       // ambient wildlife: deer that graze + flee near the player
    Mesh deer_leg_mesh_;
    // A client-side ambient deer (not networked - pure ambiance: wander, graze, flee the player).
    struct Deer {
        Vec3 pos{0.0f};
        f32 yaw = 0.0f;
        f32 gait = 0.0f;
        Vec3 target{0.0f};
        f32 retarget = 0.0f;
        bool fleeing = false;
    };
    std::vector<Deer> deer_;
    Mesh rope_mesh_;            // a unit harness-trace link, drawn per rope segment
    Mesh goods_mesh_;           // a cargo crate (spilled on the ground / carried by a player)
    std::unordered_map<u32, f32> wagon_roll_;  // accumulated wheel spin per wagon id
    std::unordered_map<u32, Vec3> wagon_prev_; // last wagon position (to derive roll)
    // A shed wheel rolling on the ground: derive its heading + rolling spin from its networked
    // position so the client can render it upright, rolling the way it travels.
    struct FallenWheel {
        Vec3 prev{0.0f};
        Vec2 heading{1.0f, 0.0f};
        f32 spin = 0.0f;
        bool init = false;
    };
    std::unordered_map<u32, FallenWheel> wheel_fx_; // per wagon id, the loose wheel's roll state
    f32 horse_gait_ = 0.0f;     // horse leg-swing phase (from its motion)
    Vec3 horse_prev_{0.0f};
    // Verlet harness traces: two ropes (left/right) per horse-drawn wagon, simulated each
    // frame between the carriage shaft tips and the horse's collar so they sag + swing.
    static constexpr int kRopeNodes = 7;
    struct RopeTrace {
        Vec3 pos[kRopeNodes];
        Vec3 prev[kRopeNodes];
        bool init = false;
    };
    std::unordered_map<u32, std::array<RopeTrace, 2>> wagon_ropes_;
    f32 hit_flash_ = 0.0f;   // red damage-flash intensity (decays)
    f32 last_health_ = 1.0f; // last seen local health fraction (to detect hits)
    Vec3 aim_{0.0f};
    bool aim_valid_ = false;
};

} // namespace alryn::game
