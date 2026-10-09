#pragma once

// Windowed multiplayer client (isometric third-person view). Owns the renderer,
// the menu/HUD, an optional in-process listen server, the networked snapshot and
// all of the client-side visuals. The implementation is split across several
// ClientApp*.cpp translation units grouped by concern (menu, input, character,
// world, wagons, vfx, hud); this header is the single class declaration they share.

#include <Alryn/Alryn.h>

#include <Alryn/Audio/Audio.h>
#include <Alryn/Core/Paths.h>
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
#include "Profile.h"

#include <Alryn/Game/Progression.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    enum class Screen { Main, Join, Settings, Customise, Class, Pause, Heroes };

    Vec2 pointer_pos() {
        if (Input* in = input()) {
            return in->mouse_position();
        }
        return Vec2{0.0f};
    }

    // ALRYN_UI_SCRIPT: a ';'-separated list of menu steps run one every ~20 frames through the REAL
    // pointer/keyboard dispatch - "click:<button or card label>", "type:<text>", "key:<keycode>",
    // "wait:<frames>" - so a
    // menu flow (e.g. forging a hero) can be replayed headless for repros + screenshots.
    void run_ui_script();
    std::vector<std::string> ui_script_;
    usize ui_script_step_ = 0;
    int ui_script_wait_ = 60;

    // ALRYN_SCREEN: a screen/overlay to open straight away for scripted screenshot runs.
    static std::string_view dev_screen() {
        const char* s = std::getenv("ALRYN_SCREEN");
        return s != nullptr ? std::string_view{s} : std::string_view{};
    }

    void menu_escape() {
        switch (current_screen_) {
            case Screen::Main: close(); break;
            case Screen::Customise: // step back through hero creation, or out of editing
                if (creating_) {
                    show_screen(Screen::Class);
                } else {
                    select_hero(hero_index_);
                    show_screen(Screen::Heroes);
                }
                break;
            case Screen::Class:
                creating_ = false;
                show_screen(roster_.heroes.empty() ? Screen::Main : Screen::Heroes);
                break;
            case Screen::Join: show_screen(Screen::Heroes); break;
            default: show_screen(Screen::Main); break;
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

    // A screen's heading block (gold display title, ornament rule, optional subtitle). Returns the
    // y just below it, so the screen's card can sit underneath.
    f32 add_title(f32 w, f32 h, const char* heading, const char* sub);

    // The living backdrop behind the menus: a slow camera drift over a real town in a fixed
    // showcase world at golden hour, streamed locally (no server). Dropped on entering a game.
    void update_menu_scene(Timestep dt);
    void draw_menu_scene();

    void build_main(f32 w, f32 h);

    void build_join(f32 w, f32 h);

    void build_settings(f32 w, f32 h);

    void build_customise(f32 w, f32 h);

    // Class-selection screen shown when hosting / joining (the player picks their combat role
    // "on joining"). Selecting a class re-lays the screen to highlight it; START enters the game
    // with the pending host/join intent recorded when this screen was opened.
    void build_class(f32 w, f32 h);

    // ---- Saved heroes (game/Profile.h) ------------------------------------------------------
    // The hero-select screen: the roster as cards (pick one, or forge a new hero), the chosen hero on
    // the turntable, and EDIT / DELETE / JOIN / HOST. The spine of the menu flow:
    //   Main -> Heroes -> (new hero: Class -> Customise) -> Host / Join -> in game.
    void build_heroes(f32 w, f32 h);
    void select_hero(int index);  // adopt roster hero `index` as the active hero (class, look, colour, bar)
    void begin_new_hero();        // start forging a hero (Class screen, then Customise)
    void commit_hero();           // store the active hero's look in the roster (adding it if new) + save
    // Pull the local player's live progression (from the snapshot, once the server has restored our
    // hero) into the active hero; saves every few seconds, or now with `force_save`.
    void sync_hero_progress(bool force_save);
    // The menu screens that show the hero turntable instead of the town backdrop.
    bool menu_shows_preview() const {
        return current_screen_ == Screen::Customise || current_screen_ == Screen::Heroes;
    }
    // What the local hero can use right now (starters + learned) and their level - live from the
    // server once it has adopted our saved hero, else from the saved hero itself.
    u8 known_mask() const;
    u8 hero_level() const;

    // Re-dress the customise turntable avatar for the current look / role / colour pick: the same
    // skinned body, outfit, attachments and simulated cloth as in game.
    void rebuild_preview();

    void apply_resolution(usize idx);

    void on_update(Timestep dt) override;

    void on_render() override;

    // Renders the customisation turntable: the live character centred in the area
    // left of the controls panel. The camera distance + horizontal offset are
    // derived from the window aspect and the panel position so the whole avatar
    // (head to feet) always fits without clipping, in any window shape.
    // The turntable's little studio: no haze, dry ground, a clean key light (see ClientAppMenu.cpp).
    void set_preview_studio();
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
    // `wrist` turns the main-hand weapon within the hand (the swing's blade-over - see
    // CharacterAnimator::weapon_wrist).
    void draw_role_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                          PlayerRole role, const Equipment& eq, bool offhand = true,
                          const Quat& wrist = QuatIdentity);
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
                             const Vec3& feet, PlayerRole role, const Equipment& eq, bool offhand = true);

    // The hand-held LANTERN (L): the off-hand arm raised to hold it out ahead (blended by `weight`).
    void apply_lantern_pose(const CharacterModel& model, std::vector<Quat>& pose, f32 weight) const;

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

    // A click inside the open skills tree: hit-test the UPGRADE buttons (buy a rank) then the ability
    // nodes (equip/unequip the one hit).
    void skills_click(const Vec2& p);

    // Request a town-shop ability upgrade (raise this ability's rank by one). The server gates it on
    // being in a town + the party affording the cost; the request is held a few ticks for reliability.
    void request_ability_upgrade(u8 ability);

    // A quick flourish at the hand for a Hunter/Cleric primary attack (the projectile itself is
    // server-spawned + networked; this is just the instant local muzzle/cast feedback).
    void spawn_primary_vfx();

    void on_event(Event& event) override;

    void on_shutdown() override;

private:
    // A flowing cloth piece on a character (Character/ClothRig: simulated in WORLD space so it lags
    // with the body's motion, colliding with the posed body + gear), plus the dynamic mesh rebuilt
    // from it each frame. Detachable (cut / blown off).
    struct ClothInstance : ClothPiece {
        Mesh mesh; // dynamic, rebuilt from the sim each frame
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
        f32 heading = 1.0f;     // movement along the facing (1 forward .. -1 backpedalling), smoothed
        f32 splash_acc = 0.0f;  // distance-through-water accumulator, paces the wading splash VFX
        bool has_last = false;
        u8 last_action = 0;     // to fire a swing once on the rising edge of a networked action
        u8 last_health = 255;   // previous snapshot health % (255 = unseen) - a drop can cut cloth
        u8 last_buffs = 0;      // previous co-op buff bits - a rising edge pops floating combat text
        u8 last_shield = 0;     // previous Aegis strength - a fresh ward pops "WARDED!"
        u8 last_level = 0;      // previous networked level - a rise pops "LEVEL UP!" over them
        bool lantern = false;   // holding a lit lantern out (local: instant; remote: from the snapshot)
        Vec3 last_tip{0.0f};    // the sword tip last frame (the cut's smear is laid between the two)
        bool tip_valid = false;
        f32 lantern_w = 0.0f;   // eased 0..1 arm raise as the lantern comes out / is put away
        u8 last_heavy = 0;      // previous networked heavy_seq - a change plays a remote heavy release
        bool heavy_init = false;
        bool charge_full = false; // the "fully charged" flare has played for the current wind-up
        f32 seen = 0.0f;        // seconds this visual has existed (a join-time restore isn't a level-up)
        SkinnedMesh body_skin;  // continuous body geometry + bone weights (built with the model)
        Mesh body_mesh;         // dynamic GPU mesh, re-skinned from the posed joints every frame
        SkinnedMesh outfit_skin; // continuous worn equipment (armoured/clothed limbs, torso, skirt)
        Mesh outfit_mesh;        // dynamic GPU mesh for the outfit, re-skinned with the same joints
        std::vector<ClothInstance> cloth; // simulated flowing pieces (cape, skirt, ...)
        BodyColliders cloth_body;         // body + gear capsules the cloth drapes over (fitted with the model)
    };

    // Set up a character's flowing cloth pieces for its role + gear (called when the visual is built).
    void setup_cloth(PlayerVisual& v, PlayerRole role, const Equipment& eq);
    void setup_noble_cape(PlayerVisual& v); // a large flowing red cape for the carriage noble
    // Step + rebuild + draw a character's cloth pieces. World-space sim (anchor from the posed joints,
    // renderer wind), mesh localised to `root` so culling stays correct.
    // `ground` is the floor height under the feet (where a long hem pools).
    void draw_cloth(PlayerVisual& v, const Mat4& root, const std::vector<Mat4>& jmats, const Vec3& tint,
                    f32 ground);

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

    // A networked enemy's renderable: a bandit model dressed per kind (melee Brigand / ranged
    // Outlaw), animated from snapshot position deltas (no animation data on the wire).
    struct EnemyVisual {
        CharacterModel model = CharacterModel::create(0u, enemy_look());
        CharacterAnimator animator;
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        u8 kind = 0;             // bandit kind, kept for the death VFX after it leaves the snapshot
        u8 last_health = 255;    // last networked health (0..255) - felled vs self-detonated sapper
        u8 last_action = 0;
        u8 last_status = 0;      // to detect a chill->shatter transition for the VFX
        f32 hurt = 0.0f;         // hit flash (1 on a fresh wound, decays) - the body blanches + recoils
        f32 gait = 0.0f;         // a wolf's run cycle (driven by distance covered)
        f32 jaw = 0.0f;          // a wolf's jaw: eased open on a bite / snarl
        Vec3 last_yaw_dir{1.0f, 0.0f, 0.0f}; // facing last frame (a wolf's body leans into its turns)
        SkinnedMesh body_skin;   // continuous body, built on first sight; re-skinned each frame
        Mesh body_mesh;          // dynamic GPU mesh (body)
        SkinnedMesh outfit_skin; // worn bandit leather/cloth, skinned like the body
        Mesh outfit_mesh;        // dynamic GPU mesh (outfit)
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

    // ---- Debug / testing overlay (F1; F2 godmode, F3 stop wagon ambushes) ----------
    void update_debug(Timestep dt);                                 // samples FPS + server tick rate
    void draw_debug(ui::DrawList& draw, f32 H);                     // the overlay panel + toggles
    void draw_nav_paths(ui::DrawList& draw, f32 W, f32 H);          // NPC pathfinding routes as lines
    void apply_debug_flags();                                       // push god/no-ambush to the listen server
    bool debug_click(const Vec2& p);                                // hit-test the overlay's toggles

    // ---- Floating combat text ------------------------------------------------------
    // A short world-anchored label ("SHATTER!", "EMPOWERED!", "CANNONBALL!") that pops over the
    // spot it happened, drifts up and fades - the Gauntlet-style readout that makes the co-op
    // combos legible at a glance. Drawn in the HUD pass via world_to_screen.
    struct FloatText {
        Vec3 world{0.0f};
        std::string text;
        Vec4 color{1.0f};
        f32 age = 0.0f;
        f32 life = 1.1f;
        f32 size = 22.0f; // px, before the pop-in ease
    };
    void combat_text(const Vec3& world, std::string text, const Vec4& color, f32 size = 22.0f);
    void draw_combat_text(ui::DrawList& draw, f32 W, f32 H);

    // ---- Particle VFX ------------------------------------------------------------
    // Spawns one particle; false if the pool is full (it was dropped).
    bool emit(const Vec3& pos, const Vec3& vel, const Vec4& color, f32 life, f32 size,
              u8 style = 0, f32 gravity = 0.0f, f32 drag = 1.6f);

    // A spray of `n` motes from `center`, biased upward by `up` (m/s), with random speed.
    void emit_burst(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                    u8 style = 1, f32 up = 0.0f, f32 gravity = 0.0f);

    // A flat expanding ring of motes on the ground (radius grows via outward velocity).
    void emit_ring(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                   u8 style = 1);

    // A splash where something wades through water at `at` (sit `at.y` at the water surface):
    // a spray of droplets that arc up and fall, plus an outward ripple ring. `intensity` (the
    // mover's speed) scales the count + height.
    void emit_splash(const Vec3& at, f32 intensity);

    // A glowing mote that starts `hot` and cools toward `cool` as it ages (fire embers, sparks).
    void emit_ember(const Vec3& pos, const Vec3& vel, const Vec3& hot, const Vec3& cool, f32 life,
                    f32 size, f32 gravity = -1.0f, f32 alpha = 0.95f);

    // Ages particles, beams, meteors, glyphs + flash lights; tracks the networked spell bolts (so a
    // vanishing one bursts on impact) and the Aegis shields (pop-in / hit / break animation).
    void update_particles(Timestep dt);

    // ---- Spell lights -------------------------------------------------------------------
    // A transient omni light (an impact flash, a spell burst) fading out over `life` seconds.
    void flash_light(const Vec3& pos, const Vec3& color, f32 strength, f32 range, f32 life);
    // A light for THIS frame only (a bolt in flight, a beam, a shield, an aura).
    void fx_light(const Vec3& pos, const Vec3& color, f32 strength, f32 range);
    // Hands the frame's VFX lights to the renderer (nearest the player first, capped so the town's
    // lanterns keep their share), scaled so spells light the dark but still tint the ground by day.
    void draw_fx_lights();

    // ---- Beams, glyphs + meteors --------------------------------------------------------
    // A magical beam from `a` to `b`. `style`: 0 = an arcane ray wrapped in twin spiralling strands,
    // 1 = a crackling jagged arc, 2 = a pillar of light striking down from `a` (the sky) onto `b`.
    // `grow` > 0 makes the head race out from `a` at that speed (m/s) instead of appearing at once.
    void beam(const Vec3& a, const Vec3& b, const Vec4& color, f32 width, f32 life, u8 style = 0,
              f32 grow = 0.0f);
    // A glowing rune circle on the ground (a cast sigil): twin rings, a `points`-pointed star and
    // orbiting rune marks, scaling in and spinning while it fades over `life` seconds.
    void glyph(const Vec3& center, f32 radius, const Vec4& color, f32 life, int points = 6);
    // Draws one rune circle at `intensity` (shared by the timed glyphs and the ground auras).
    void draw_glyph(const Vec3& center, f32 radius, const Vec3& color, f32 intensity, int points,
                    f32 spin);
    // A glowing circle of streak sprites in the plane spanned by `u`/`v` (rings, shield rims).
    // `shimmer` (0..1) runs bright bands around it.
    void sprite_circle(const Vec3& c, const Vec3& u, const Vec3& v, f32 radius, int segs, f32 width,
                       const Vec4& color, f32 shimmer = 0.0f);
    // Traces a sphere's silhouette (the circle it shows the camera) in light: a fresnel-style rim
    // that makes a translucent shell read as a glassy bubble from any angle.
    void sphere_rim(const Vec3& c, f32 radius, const Vec3& color, f32 intensity, int segs, f32 width);
    // An arcane ray: soft halo + white-hot core + twin strands spiralling around it.
    void draw_ray(const Vec3& a, const Vec3& b, const Vec3& color, f32 width, f32 intensity, f32 phase);
    // A crackling lightning-like arc that re-jitters many times a second.
    void draw_arc(const Vec3& a, const Vec3& b, const Vec3& color, f32 width, f32 intensity, u32 seed);
    // Draws the live beams, glyphs, falling meteors and the local Mage's orbiting combo orbs.
    void draw_spell_fx();
    void meteor_impact(const Vec3& at);

    // The burst where a networked spell bolt struck (or came to rest), by projectile kind.
    void projectile_impact(u8 kind, const Vec3& at);
    void track_projectiles();
    // True if a spell bolt has already burst on landing (its resting body is hidden, not drawn).
    bool projectile_spent(const net::ProjectileState& pr) const;
    // Aegis shield animation state per shielded player / villager (pop-in, hit flash, shatter).
    void update_shields(Timestep dt);

    // The Mage spell VFX (cast sigil, beams, the spell itself) for whoever cast it: the local player
    // on the keypress, remote Mages from the snapshot's `cast` + `cast_aim`.
    void spawn_spell_vfx(SpellId spell, const Vec3& feet, f32 yaw, const Vec3& aim);

    // The glowing ground disc + soft dome of each ground aura, plus a soft light at night so the
    // aura lights its surroundings. Colour comes from the shared aura_props table (data-driven, so
    // a new aura kind renders + lights itself with no extra client code). Rising motes are emitted
    // in update_particles. Drawn additively so it brightens the ground without occluding.
    void draw_auras();

    // The Aegis protective bubble around any shielded player / NPC: a large faceted shell with a
    // glowing fresnel-style rim, gyroscope rings and a ground halo, lighting its surroundings. It pops
    // in with an overshoot, flashes when it soaks a blow and shatters when spent (update_shields).
    void draw_shields();

    // A Cleric's max-Aegis DOME (Snapshot.bubbles): a huge shell with a shimmering rim, a slowly
    // turning geodesic cage, a rune ring where it meets the ground and its own light - the
    // ranged-blocking bubble the party shelters in.
    void draw_bubbles();

    // Co-op buff auras under empowered (fiery ring) / hasted (green ring) players, so allies can
    // read who the Cleric/Hunter/Mage has buffed. Pulses; driven by PlayerState.buffs bitflags.
    void draw_buffs();

    void draw_oxen(const Vec3& pos, f32 yaw); // a yoked pair of draft oxen pulling the wagon
    void update_deer(Timestep dt);            // wander/graze/flee the ambient deer (client-side)
    void draw_deer();
    void update_fish(Timestep dt);            // swim/dart the ambient fish in nearby water (client-side)
    void draw_fish();
    void draw_surf();                         // foam waves washing along the nearby shoreline
    void draw_particles();
    // Ambient wildlife VFX (no networking): a flock of birds drifting across the sky by day, and
    // at night a slow gliding owl plus fireflies. The fireflies are anchored to fixed WORLD cells
    // (each drifts gently about its own spot), so the player walks past them through the world
    // rather than carrying a screen-locked swarm.
    void draw_ambient_life();

    // The showy burst for an ability cast, played for whoever cast it (the local player on
    // keypress for instant feel; remote players when the snapshot reports their `cast`). `rank` is
    // the caster's upgrade rank for it (a max-rank Heal chains its beams on to more allies).
    void spawn_ability_vfx(PlayerRole role, u8 slot, const Vec3& feet, f32 yaw, const Vec3& aim,
                           u8 rank = 0);

    // Decays the local buff auras (emitting trailing motes while active) and plays cast VFX
    // for remote players from the snapshot's `cast` field (deduped by tick so each fires once).
    void update_ability_vfx(Timestep dt);

    // A menacing low-poly look shared by all enemies (dark skin, sharp eyes, spiky
    // hair); a red tint at draw time makes them read as hostile.
    static CharacterAppearance enemy_look();
    // How far a seated figure's root sits below the bench top: the sit pose folds the thighs level, so
    // the hips rest just above the seat (whatever the race's leg length).
    static f32 seat_drop(const CharacterModel& m) { return m.hip_height() - 0.18f; }

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

    // Eases each wagon's render position toward its authoritative snapshot position (called once
    // per frame in on_update, before any wagon drawing). Removes the inter-snapshot jitter.
    void update_wagon_smooth(Timestep dt);
    // True if `feet` is standing ON TOP of the cart's bed (over its footprint, at deck height) - the
    // same test the server uses to carry deck riders along with the moving wagon.
    bool on_wagon_deck(const net::WagonState& wg, const Vec3& feet) const;

    // Eases every networked character (players, enemies, villagers) toward its authoritative
    // position/yaw, IN the snapshot itself - so every draw site, VFX and camera path sees the
    // smoothed motion without changes. The authoritative target is kept per entity (refreshed
    // when a new snapshot tick arrives); a big jump (spawn / teleport) snaps instead of gliding.
    // This is what keeps other players' movement smooth between snapshots and under jitter/loss.
    void update_net_smooth(Timestep dt);

    // The smoothed render position for a wagon (falls back to the raw position before it's seeded).
    Vec3 wagon_render_pos(const net::WagonState& wg) const {
        const auto it = wagon_smooth_.find(wg.id);
        return it != wagon_smooth_.end() && it->second.init ? it->second.pos : wg.position;
    }
    // This frame's smoothed cart displacement (for the bob + wheel spin).
    f32 wagon_frame_move(const net::WagonState& wg) const {
        const auto it = wagon_smooth_.find(wg.id);
        return it != wagon_smooth_.end() ? glm::length(it->second.step) : 0.0f;
    }
    Vec2 wagon_frame_step(const net::WagonState& wg) const {
        const auto it = wagon_smooth_.find(wg.id);
        return it != wagon_smooth_.end() ? it->second.step : Vec2{0.0f};
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

    // ---- Identity + progression HUD ----------------------------------------------------------
    // A soft ring in every player's identity colour at their feet (3D, with the scene) - the quickest
    // way to tell who is who in a scrum.
    void draw_player_rings();
    // Name plates over the other players (name + level in their colour, a health sliver), and arrows
    // at the screen edge pointing to teammates who are off-screen.
    void draw_nameplates(ui::DrawList& draw, f32 W, f32 H);
    // Party frames down the left edge: each teammate's colour, class crest, name, level and health.
    void draw_party_frames(ui::DrawList& draw, f32 W, f32 H, f32 ts);
    // Level-up + journey-step celebrations (edge-triggered from the snapshot) and their banners.
    void update_progress_fx(Timestep dt);
    void draw_progress_fx(ui::DrawList& draw, f32 W, f32 H);
    // Skill tree: ask the server to learn ability `code` (1..7) or raise talent (8 + t). Held a few
    // ticks so the server sees a rising edge even if a packet drops; it spends one point per press.
    void request_learn(u8 code);
    // The journey log (J): the hero's record and every journey goal - done, current (with progress)
    // and still ahead - with their rewards. The linear campaign at a glance.
    void draw_journal();
    // Guides a new hero to their next goal in the world: while the journey asks them to find work /
    // set out, the nearest contract wagon gets a bobbing gold marker (and an edge pointer when it's
    // off-screen). Also announces each town as the player walks into it.
    void draw_journey_guide(ui::DrawList& draw, f32 W, f32 H);
    void update_town_arrival(Timestep dt);

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
    void draw_minimap(ui::DrawList& draw, const Vec3& feet, f32 W, f32 H);

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
    // The lantern itself, hanging from the raised off-hand: an iron frame round a glowing glass core
    // that swings as the hero moves, plus its warm light (a real shadow-caster for the local player).
    void draw_lantern(const CharacterModel& model, const std::vector<Mat4>& jmats, PlayerVisual& v, bool local);

    // ---- Village NPCs (server-authoritative; the player defends them) -------
    // Villagers are simulated on the server (wander/sleep/flee, killable by enemies)
    // and arrive in the snapshot; the client just renders + animates them, rebuilding
    // a model when its appearance first appears, and culls visuals that have died /
    // left the snapshot.
    void update_villager_visuals(Timestep dt);

    PlayerVisual& ensure_villager_visual(u32 id, const CharacterAppearance& appearance, u8 kind = 0, u8 role = 0);

    void draw_villagers();

    void send_input();

    // Controller support: sample the gamepad each frame and fold it into the same input path as
    // mouse/keyboard (the left stick is added in send_input; this handles buttons, triggers, zoom
    // and menu toggles). Sets `using_gamepad_` - the active input device, auto-switched by activity
    // - so the aim follows the right stick instead of the cursor while the pad is in use.
    void apply_gamepad(Timestep dt);

    // The left-click primary attack, role-specific - shared by the mouse button and the pad trigger.
    void primary_action();
    // Hold-to-charge (Roles.h): the primary button went down / came up. A quick tap is the basic attack
    // (primary_action); a hold winds up a HEAVY, unleashed on release (heavy_release).
    void attack_press();
    void attack_release();
    void cancel_charge(); // a menu / overlay opened mid-wind-up: drop it (no attack)
    void heavy_release();
    // Per-frame wind-up: builds the charge while held, flares at a full charge, gathers VFX at the hand.
    void update_charge(f32 dt);
    // The visible weight of a heavy, for whoever threw it: the Knight's blow cracking the ground (at
    // `feet` + its facing, `power` 0..1), a Hunter's drawn-bow snap, a caster's hurled orb.
    void heavy_impact_fx(const Vec3& feet, f32 yaw, f32 power, PlayerRole role, bool local);
    // The swirl of power gathering at a hero's weapon while they wind up a heavy (drawn every frame).
    void draw_charge_fx(PlayerVisual& v, const std::vector<Mat4>& jmats, PlayerRole role, f32 charge);
    // The charge gauge under the local hero (the HUD pass).
    void draw_charge_meter(ui::DrawList& draw, f32 W, f32 H);
    // A terrain edit arrived (a dig, a crater, raised earth): the dirt it throws up.
    void deform_fx(const Vec3& center, f32 radius, f32 amount);

    // ---- Side quests (ClientAppQuests.cpp) ---------------------------------------------------------
    // The quest under way (nullptr if none) / the offers on the board of the town we're in.
    const net::QuestState* active_quest() const;
    // The notice board the local hero is standing at (its quests are listed), if any.
    bool near_quest_board() const;
    // The board's panel (offers + ACCEPT, or the quest under way + ABANDON), click rects stored.
    void draw_quest_panel(ui::DrawList& draw, f32 W, f32 H, f32 ts);
    bool quest_panel_click(const Vec2& p);
    // The quest's place in the world: a bandit camp (tents, a fire, a war banner), the treasure X and
    // chest, the moonpetals, a beacon over the site - and the waypoint arrow + completion banner.
    void draw_quest_world();
    void draw_quest_hud(ui::DrawList& draw, f32 W, f32 H, f32 ts);
    void update_quest_fx(Timestep dt);
    // Scripted screenshots (a hosted game): ALRYN_SCREEN=quest takes a side quest (ALRYN_QUEST_KIND
    // 0..3 picks which) and stands the hero within sight of its site; =board stands them at the notice
    // board. ALRYN_HOLD=1 holds the primary attack down (the heavy's wind-up + gauge).
    void dev_quest_setup();
    bool dev_setup_done_ = false;
    f32 dev_setup_wait_ = 0.0f;

    // ---- Life on the roads (ClientAppRoads.cpp) -----------------------------------------------------
    // A wayfarer's kit by its role (a pack + staff, a pilgrim's lantern by night, a guard's spear ...).
    void draw_wayfarer_kit(const net::VillagerState& vl, PlayerVisual& v, const Mat4& root, const std::vector<Quat>& pose);
    // The merchant caravans rolling between towns: their carts (eased, wheels turning), beasts + loads.
    void update_caravans(Timestep dt);
    void draw_caravans();
    // The roadside traveller's errand: the one in the snapshot (nullptr if none); E by them takes it on.
    const net::ErrandState* current_errand() const;
    bool errand_talk();
    void update_errand_fx(Timestep dt);
    void draw_errand_world(); // the goat / satchels / stuck cart + the traveller's bundle and fire
    void draw_errand_hud(ui::DrawList& draw, f32 W, f32 H, f32 ts);
    // A waypoint diamond over `at` (an edge pointer when off-screen) with its distance.
    void draw_waypoint(ui::DrawList& draw, f32 W, f32 H, f32 ts, const Vec3& at, const Vec4& col, f32 hide_within);
    // Snow drifting down in the high country (the peaks + the alpine plateaus' snowbound towns).
    void draw_snowfall();
    // Scripted screenshots: ALRYN_SCREEN=city / snowtown / hamlet / village stands the hero in the nearest
    // such settlement; =road / caravan sets them out on the road with traffic coming; =errand puts a
    // traveller in trouble ahead (ALRYN_ERRAND_KIND 0..4, ALRYN_ERRAND_TAKE=1 takes it at once).
    void dev_road_setup();
    bool dev_road_done_ = false;
    bool dev_road_placed_ = false;
    Vec2 dev_road_dir_{1.0f, 0.0f};

    // ---- Enemy looks (ClientAppWorld.cpp) -------------------------------------------------------------
    // A dire wolf (or the alpha, `alpha`): a lean low-poly quadruped with hackles, a snapping jaw and
    // ember eyes, run-cycled from its motion; `action` crouches it (2) or stretches it into a pounce (4).
    // `pose_root` (a felled wolf, lying where it fell) replaces the live posture with a fixed frame.
    void draw_wolf(EnemyVisual& v, const net::EnemyState& en, const Vec3& tint, const Mat4* pose_root = nullptr);
    // The ferocity layer on a raider: ember-lit eyes, war paint, spikes + horns by rank, the rage of the
    // last raider standing, the warlord's banner, mud when bogged in a pit.
    void draw_enemy_menace(EnemyVisual& v, const net::EnemyState& en, const std::vector<Mat4>& jmats, f32 scale);
    // A felled foe keeps its body a moment: it pitches over and sinks away (enemy_deaths_).
    void draw_enemy_deaths(Timestep dt);
    // Knight: start a buffered sword swing once the current one allows (see swing_queued_).
    void update_sword_pacing(f32 dt);
    // The feedback of one of OUR attacks landing (marker, camera kick, sparks, impact sound).
    void land_hit_feedback();

    // The yaw to draw a player with: ours (unless seated on the wagon) turns with our live aim
    // straight away rather than waiting on the server's echo; everyone else uses their networked
    // facing.
    f32 display_yaw(const net::PlayerState& p) const {
        return (p.id == my_id_ && p.seated == 0) ? face_yaw_ : p.yaw;
    }

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
    // The listen server runs on its OWN thread at a steady ~60 Hz, decoupled from the
    // render loop - a slow or hitching host frame must not stall everyone's snapshots.
    // `server_mutex_` guards every cross-thread touch of local_server_ (the tick loop
    // vs the debug hooks / nav-path overlay on the main thread).
    GameServer local_server_;
    std::thread server_thread_;
    std::mutex server_mutex_;
    std::atomic<bool> server_thread_run_{false};
    void start_local_server_thread();
    void stop_local_server(); // joins the tick thread, then stops the server
    Renderer* renderer_ = nullptr;

    // Menu / settings.
    ui::UIContext ui_;
    Screen current_screen_ = Screen::Main;
    bool paused_ = false;   // in-game pause menu (overlaid on the live game)
    UVec2 ui_extent_{0, 0}; // last framebuffer size the menu was laid out for
    std::string host_ip_ = "127.0.0.1";
    Vec3 menu_sky_{0.05f, 0.06f, 0.09f};
    std::unique_ptr<StreamingTerrain> menu_terrain_; // the menu backdrop's world slice
    Vec3 menu_focus_{0.0f};                          // the showcase town centre (ground height)
    f32 menu_cam_t_ = 0.0f;                          // seconds into the backdrop's camera drift
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
    ui::Rect skill_upgrade_rects_[kAbilityCount] = {}; // tree UPGRADE-button rects (from draw_skills)
    u8 ability_rank_[kAbilityCount] = {};            // local player's current-role upgrade ranks (snapshot)
    u8 pending_upgrade_ = 0;                          // ability index+1 to buy-upgrade (sent while held)
    int upgrade_hold_ = 0;                            // ticks left to hold pending_upgrade_ (rising-edge buy)
    u8 pending_learn_ = 0;                            // skill-tree learn request (see request_learn)
    int learn_hold_ = 0;                              // ticks left to hold pending_learn_
    ui::Rect skill_learn_rects_[kAbilityCount] = {};  // tree LEARN-button rects (from draw_skills)
    ui::Rect talent_rects_[kTalentCount] = {};        // talent "+" button rects (from draw_skills)
    int skill_hover_ = -1;                            // ability node under the cursor (detail card)

    // ---- Heroes + live progression ----
    Roster roster_;                 // the saved heroes (loaded on init)
    Hero hero_;                     // the active hero (being played, edited or forged)
    int hero_index_ = -1;           // roster slot hero_ came from (-1 = a new, unsaved hero)
    bool creating_ = false;         // the Class -> Customise screens are forging a new hero
    bool confirm_delete_ = false;   // DELETE was pressed once - the next press deletes
    bool restore_acked_ = false;    // the server has adopted our saved hero (live progress is trusted)
    f32 hero_save_cd_ = 0.0f;       // seconds until the next autosave of the hero's progress
    f32 session_time_ = 0.0f;       // seconds played this session (folded into the hero on save)
    net::HeroProgress live_progress_{}; // the local player's progression from the latest snapshot
    u8 live_level_ = 1;
    u8 live_color_ = 0;
    u8 last_level_seen_ = 0;        // previous level (0 = not seen yet) - an increase celebrates
    u8 last_journey_seen_ = 255;    // previous journey step (255 = not seen yet)
    f32 levelup_fx_ = 0.0f;         // level-up banner timer (counts down)
    f32 journey_fx_ = 0.0f;         // journey-step banner timer
    u8 journey_fx_step_ = 0;        // the step that just completed
    f32 objective_bottom_ = 0.0f;   // screen y under the HUD's objective card (the journey toast hangs there)

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
        Vec3 tail{-1.0f}; // colour it cools toward as it ages (x < 0 = keeps `color`)
        u8 style = 0; // 0 = emissive, 1 = additive glow sprite (streaks along its velocity), 2 = a soft
                      // translucent puff of dust / smoke that billows out as it fades
    };
    std::vector<Particle> particles_;

    // Spell lights: timed flashes, plus the per-frame sources gathered while updating + drawing.
    struct FlashLight {
        Vec3 pos{0.0f};
        Vec3 color{1.0f};
        f32 strength = 1.0f;
        f32 range = 6.0f;
        f32 life = 0.3f;
        f32 max_life = 0.3f;
    };
    struct FxLight {
        Vec3 pos{0.0f};
        Vec3 color{1.0f}; // already scaled by strength
        f32 range = 6.0f;
    };
    std::vector<FlashLight> flash_lights_;
    std::vector<FxLight> frame_fx_lights_;

    struct Beam {
        Vec3 a{0.0f};
        Vec3 b{0.0f};
        Vec4 color{1.0f};
        f32 width = 0.1f;
        f32 life = 0.4f;
        f32 max_life = 0.4f;
        f32 grow = 0.0f; // head speed (m/s); 0 = full length at once
        f32 age = 0.0f;
        u8 style = 0;
        u32 seed = 0;
    };
    std::vector<Beam> beams_;
    struct Glyph {
        Vec3 center{0.0f};
        f32 radius = 1.0f;
        Vec4 color{1.0f};
        f32 life = 0.8f;
        f32 max_life = 0.8f;
        f32 spin = 0.0f; // starting angle, so stacked sigils don't line up
        int points = 6;
    };
    std::vector<Glyph> glyphs_;
    // A Meteor in flight: a blazing rock streaking down out of the sky onto `to`.
    struct MeteorFx {
        Vec3 from{0.0f};
        Vec3 to{0.0f};
        f32 t = 0.0f;
        f32 dur = 0.45f;
    };
    std::vector<MeteorFx> meteors_;
    // Networked spell bolts carry no id, so they're matched snapshot-to-snapshot by kind + nearness;
    // one that vanishes (struck) or stops dead (landed) bursts where it was.
    struct ProjectileTrack {
        Vec3 pos{0.0f};
        Vec3 dir{0.0f, 0.0f, 1.0f};
        u8 kind = 0;
        bool seen = false;
        bool resting = false;
    };
    std::vector<ProjectileTrack> proj_tracks_;
    u32 proj_track_tick_ = 0;
    struct ShieldFx {
        f32 age = 0.0f;  // seconds since the ward went up (drives the pop-in)
        f32 hit = 0.0f;  // flash when it soaks a blow (decays)
        u8 last = 0;     // previous networked strength
        u32 stamp = 0;   // last update that saw it (for pruning)
    };
    std::unordered_map<u64, ShieldFx> shield_fx_; // keyed (0 << 32 | player id) / (1 << 32 | villager id)
    u32 shield_stamp_ = 0;
    std::vector<FloatText> float_texts_; // live floating combat labels (aged in update_feedback)
    u32 fx_rng_ = 0x9e3779b9u;
    f32 frand();
    f32 frand(f32 a, f32 b) { return a + (b - a) * frand(); }
    Vec3 rand_dir();
    PlayerVisual preview_; // the customise turntable avatar, dressed exactly as in game (rebuild_preview)
    CharacterAnimator preview_anim_;
    f32 preview_turn_ = 0.6f;
    f32 preview_zoom_ = 0.0f; // 0 = full-length (hero roster) .. 1 = head + shoulders (the creator)
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
        // A Door part (see PropPart): how it opens - swung about `hinge`, or lifted - plus the centre
        // + half-width of the shut leaf, which "someone is near the door" is measured from.
        Vec3 hinge{0.0f};
        f32 swing = 0.0f;
        f32 lift = 0.0f;
        Vec3 center{0.0f};
        f32 reach = 0.0f;
    };
    struct GpuProp {
        std::vector<GpuPropPart> parts;
        std::vector<PropLight> lights; // lantern / hearth / brazier spot lights
        Vec2 footprint{0.0f};          // house interior half-extents (0 = not a house)
        f32 wall_height = 0.0f;
        Vec3 chimney_spot{0.0f};       // local chimney-pot top (zero = no hearth smoke)
    };
    // How far open (0 shut .. 1 open) the door `part` of prop instance `p` (drawn with `m`) is: it
    // eases open while anyone - a hero, a townsperson, a traveller, a raider - stands near it, and
    // swings shut once they've gone. Client-side + visual only (doorways never block).
    f32 door_open(const PropInstance& p, const Mat4& m, const GpuPropPart& part);

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
    Mesh shape_quad_; // a flat single-sided up-facing unit quad (shore foam streaks)
    std::vector<Vertex> skin_scratch_; // reused buffer for CPU-skinning a body each frame (no per-frame alloc)
    std::vector<ClothCollider> cloth_colliders_; // reused buffer: a character's posed cloth colliders
    std::vector<std::pair<int, Mesh>> mesh_graveyard_; // retired NPC body meshes, freed after a few frames
    Mesh marker_;
    Mesh ring_mesh_; // a flat unit annulus (r 0.56..0.70) - each player's identity ring at their feet
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
    // Every house door near the player: its eased open amount + when it was last drawn (stale ones
    // are pruned in update_gates). Keyed by the door's quantised world position.
    struct DoorAnim {
        f32 open = 0.0f;
        f32 seen = 0.0f;
    };
    std::unordered_map<u64, DoorAnim> door_open_;

    f32 elapsed_ = 0.0f;
    f32 haul_elapsed_ = 0.0f; // seconds the active haul has run (client-side, for the rush-bonus HUD)
    f32 frame_dt_ = 1.0f / 60.0f; // last frame's dt, for per-frame sims stepped during rendering (cloth)
    f32 time_of_day_ = daynight::start_time; // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset
    f32 day_seconds_ = daynight::default_day_seconds;
    f32 sun_intensity_ = 1.0f; // cached from the day/night cycle (0 night .. 1 day)
    f32 fog_gloom_ = 0.0f;     // eased 0..1 town-gloom factor (denser/cooler fog + grade in towns)
    f32 fog_patch_ = 0.0f;     // eased 0..1 road fog-bank strength (occasional dense volumetric mist)
    f32 weather_amt_ = 0.0f;   // eased 0..1 storminess (from the networked weather) - rain/sky/wind
    f32 wetness_ = 0.0f;       // eased 0..1 rain-soaked ground (soaks fast in a storm, dries slowly)
    f32 lightning_ = 0.0f;     // current lightning-flash brightness (decays)
    f32 lightning_cd_ = 4.0f;  // seconds until the next storm flash
    f32 cam_distance_ = iso::distance; // scroll-wheel zoom
    Vec3 cam_target_{0.0f};            // smoothed camera look-at target (glides toward the player)
    f32 combat_zoom_ = 1.0f;           // distance multiplier eased toward 0.86 while an ambush is on
    Camera camera_;

    net::PlayerId my_id_ = 0;
    u32 world_seed_ = 0;     // shared world seed (from Welcome) - for the map's town/road graph
    bool map_open_ = false;  // full-screen map overlay (M)
    bool skills_open_ = false; // full-screen skills tree overlay (K)
    bool wardrobe_open_ = false; // gear / wardrobe overlay (U): buy tiers, recolour, change weapon
    bool journal_open_ = false;  // the journey log overlay (J): every goal, its reward + the hero's record
    // Any of the full-screen overlays (map / skills / gear / journal) is up - world input is frozen.
    bool overlay_open() const { return map_open_ || skills_open_ || wardrobe_open_ || journal_open_; }
    void close_overlays() { map_open_ = skills_open_ = wardrobe_open_ = journal_open_ = false; }
    u32 town_vseed_ = 0;            // the town the local player stands in (0 = out on the roads)
    f32 town_banner_ = 0.0f;        // "you have arrived" banner timer
    std::string town_banner_name_;  // the town it announces
    std::string town_banner_sub_;   // ...and what kind of place it is (a hamlet / a snowbound city ...)
    u8 pending_buy_ = 0;       // shop: the gear tier we're trying to buy up to (sent in PlayerInput.buy)
    u8 pending_buy_rig_ = 0;   // shop: the wagon-rig level we're trying to buy up to (PlayerInput.buy_rig)
    ui::Rect wardrobe_buy_rect_ = {};        // the "buy upgrade" button (from draw_wardrobe)
    ui::Rect wardrobe_rig_rect_ = {};        // the "reinforce wagon" button
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
    // Cached terrain-relief raster: fine (rect, colour) tiles drawn in ONE instanced call
    // (Renderer::draw_ui_tiles). The cache is built with some overscan beyond the panel and
    // tracked between rebuilds by a screen-space pan/zoom transform, so dragging stays smooth
    // while worldgen is only re-sampled every few frames / after real movement.
    std::vector<Renderer::UITile> map_tiles_;
    Vec2 map_raster_center_{1e9f, 1e9f}; // world XZ the raster was built around
    f32 map_raster_zoom_ = -1.0f;
    f32 map_raster_ppm_ = 1.0f;      // pixels-per-metre the raster was built at
    f32 map_raster_overscan_ = 0.0f; // extra px rastered beyond the visible area, each side
    int map_raster_cooldown_ = 0;    // frames until the next non-urgent rebuild
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
    bool pending_dodge_ = false;       // dodge-roll this tick (Shift)
    bool pending_local_swing_ = false; // play our own swing animation this frame (left-click)
    f32 swing_face_lock_ = 0.0f;       // seconds the facing stays on the last sword swing's aim
    bool blocking_ = false;            // Knight holding the shield up (right mouse held)
    bool pending_rally_ = false;
    bool pending_grab_ = false; // one-shot hitch/unhitch the nearest wagon
    bool pending_toss_ = false; // Ally Toss combo: one-shot hurl the nearest teammate (G)
    bool conduit_held_ = false; // Power Conduit combo: Cleric channelling a beam to an ally (hold V)
    bool lantern_on_ = false;   // the local hero is holding a lit lantern out (toggled with L)
    // Hold-to-charge heavy attacks (see attack_press / update_charge).
    bool attack_held_ = false;  // the primary button (left mouse / right trigger) is down
    f32 attack_hold_t_ = 0.0f;  // seconds it's been held
    f32 charge_ = 0.0f;         // 0..1 the heavy's wind-up (after the tap window)
    bool charge_full_ = false;  // the full-charge flare has played this hold
    u8 heavy_seq_ = 0;          // bumps on each heavy released (sent every tick; the server acts on a change)
    u8 heavy_power_ = 0;        // the charge (1..255) of the last heavy released
    f32 heavy_face_lock_ = 0.0f; // the facing holds on a heavy's aim while the blow comes down
    bool pending_dig_ = false;  // Q: a spade-strike at the aim this tick
    // A Knight's heavy blow lands a beat after the release: its ground-cracking VFX wait for it.
    struct PendingImpact {
        Vec3 feet{0.0f};
        f32 yaw = 0.0f;
        f32 power = 0.0f;
        f32 in = 0.0f;
        PlayerRole role = PlayerRole::Knight;
        bool local = false;
    };
    std::vector<PendingImpact> pending_impacts_;
    // Side quests: the board's panel (click rects) + the pick / abandon requests (held a few ticks).
    ui::Rect quest_accept_rects_[kQuestOffers] = {};
    u32 quest_accept_ids_[kQuestOffers] = {};
    ui::Rect quest_abandon_rect_{};
    u32 pending_quest_pick_ = 0;
    int quest_pick_hold_ = 0;
    int quest_abandon_hold_ = 0;
    u32 last_quest_done_ = 0;      // the last quest whose completion we celebrated
    f32 quest_banner_ = 0.0f;      // "QUEST COMPLETE" banner timer
    std::string quest_banner_text_;
    u8 last_quest_progress_ = 0;   // to pop "+1" when the active quest advances
    u32 last_quest_id_ = 0;
    // A felled foe's body, kept a moment to pitch over + sink into the ground (by its last pose).
    struct EnemyDeath {
        EnemyVisual v;
        Vec3 pos{0.0f};
        f32 yaw = 0.0f;
        f32 t = 0.0f;
        f32 scale = 1.0f;
        Vec3 fall{1.0f, 0.0f, 0.0f}; // the way it topples (away from the blow)
    };
    std::vector<EnemyDeath> enemy_deaths_;
    Mesh wolf_body_mesh_;   // a dire wolf (body + head), faces +X; legs + jaw drawn separately
    Mesh wolf_leg_mesh_;
    Mesh wolf_jaw_mesh_;
    Mesh tent_mesh_;        // a bandit camp's ragged A-frame tent
    Mesh goat_body_mesh_;   // an errand's runaway goat (legs drawn x4)
    Mesh goat_leg_mesh_;
    struct GoatGait {
        Vec3 prev{0.0f};
        f32 phase = 0.0f;
        bool init = false;
    };
    std::unordered_map<u32, GoatGait> goat_gait_;
    // A merchant caravan's eased render state (the cart + its beast) and the gait of its wheels + legs.
    struct CaravanSmooth {
        Vec3 pos{0.0f};
        Vec3 beast{0.0f};
        Vec3 prev_beast{0.0f};
        f32 roll = 0.0f;
        f32 gait = 0.0f;
        bool init = false;
    };
    std::unordered_map<u32, CaravanSmooth> caravan_smooth_;
    // The errand's edges (taken / progress / done) + what its traveller says.
    u32 last_errand_id_ = 0;
    u8 last_errand_phase_ = 255;
    u8 last_errand_progress_ = 0;
    f32 errand_banner_ = 0.0f;
    std::string errand_banner_text_;
    std::string errand_say_;
    f32 errand_say_t_ = 0.0f;
    f32 snow_amt_ = 0.0f; // eased snowfall strength (the high country)
    // Controller state. `using_gamepad_` is the active input device (auto-switched: any pad activity
    // selects it, any mouse motion selects KBM) and decides whether the aim follows the right stick
    // or the cursor. The trigger edges are tracked here because triggers are analog axes, not buttons.
    bool using_gamepad_ = false;
    bool pad_lt_prev_ = false; // left-trigger held last frame (secondary-action edge)
    bool pad_rt_prev_ = false; // right-trigger held last frame (primary-action edge)
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
    Mesh fish_body_mesh_;       // ambient wildlife: small fish that swim in the water near the player
    // A client-side ambient fish (not networked): swims just under the surface, darts from the player.
    struct Fish {
        Vec3 pos{0.0f};
        f32 yaw = 0.0f;
        f32 wiggle = 0.0f; // swim-tail phase
        Vec3 target{0.0f};
        f32 retarget = 0.0f;
        f32 scale = 1.0f;
        Vec3 tint{1.0f}; // biome colour (tropical bright vs silver/dark)
        bool darting = false;
    };
    std::vector<Fish> fish_;
    Mesh rope_mesh_;            // a unit harness-trace link, drawn per rope segment
    Mesh goods_mesh_;           // a cargo crate (spilled on the ground / carried by a player)
    Mesh cargo_weapons_mesh_;   // crate of arms (CargoKind::Weapons)
    Mesh cargo_casks_mesh_;     // cask of ale (CargoKind::Casks)
    // The cargo mesh for the active wagon's CargoKind (weapons crate / ale cask / default crate).
    const Mesh& cargo_mesh() const;
    // Per-cask roll: an ale cask rolls about its long axis as it slides fore/aft in the bed. The
    // roll angle is accumulated client-side from the change in the cask's cart-local position
    // (keyed by good id), so casks visibly trundle around the back of the wagon.
    struct CaskRoll {
        Vec3 prev{0.0f};
        f32 roll = 0.0f;
        bool seen = false;
    };
    std::unordered_map<u32, CaskRoll> cask_roll_;
    std::unordered_map<u32, f32> wagon_roll_;  // accumulated wheel spin per wagon id
    // Smoothed render state per wagon: the raw authoritative position arrives in lumpy snapshot
    // steps, so we ease a render position toward it each frame. Everything visual (the cart mesh,
    // its bob/tilt, the wheels, and ANY rider/driver attached to it) is driven off this smoothed
    // state, so they all move together and the cart doesn't jitter between snapshots.
    struct WagonSmooth {
        Vec3 pos{0.0f};  // smoothed render position
        Vec2 step{0.0f}; // this frame's smoothed xz displacement (drives the bob + wheel roll)
        f32 splash_acc = 0.0f; // distance-through-water accumulator, paces the wading splash VFX
        bool init = false;
    };
    std::unordered_map<u32, WagonSmooth> wagon_smooth_;
    // Smoothed render state per networked character (players / enemies / villagers - see
    // update_net_smooth). `target` is the last authoritative position (refreshed on a new
    // snapshot tick); `pos`/`yaw` ease toward it each frame and are written back into the
    // snapshot, so everything downstream renders the smoothed motion.
    struct NetSmooth {
        Vec3 pos{0.0f};
        Vec3 target{0.0f};
        f32 yaw = 0.0f;
        f32 target_yaw = 0.0f;
        u32 stamp = 0; // last update_net_smooth pass that saw this entity (for pruning)
        bool init = false;
    };
    std::unordered_map<u64, NetSmooth> net_smooth_; // keyed by (entity kind << 32) | id
    u32 net_smooth_stamp_ = 0;
    u32 net_smooth_tick_ = 0; // snapshot tick the targets were last refreshed from
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
    f32 hit_marker_ = 0.0f;  // hit-marker pop intensity when OUR attack lands (decays); drawn at screen centre
    f32 hit_fx_hold_ = -1.0f; // a landed sword blow's feedback, held until our swing cuts through (s; < 0 none)
    f32 swing_ready_in_ = 0.0f; // Knight: seconds until the next sword swing may start (the swing's pacing)
    bool swing_queued_ = false; // Knight: a click arrived mid-swing - it chains as soon as the pacing allows
    f32 cam_shake_ = 0.0f;   // camera kick when a blow lands (decays fast) - sells the weight of a hit
    u8 last_hit_fx_ = 0;     // last seen local hit_fx counter (server bumps it on a confirmed hit)
    bool hit_fx_init_ = false; // seen the first snapshot value yet (so a fresh join doesn't pop a marker)
    u32 last_money_ = 0;     // last seen party wallet, to pop a "+$n" when loot/pay lands
    bool money_init_ = false;
    u32 money_gain_ = 0;     // size of the latest gain (shown while the pulse lasts)
    f32 money_pulse_ = 0.0f; // "+$n" pop intensity beside the money counter (decays)
    f32 hud_hp_ghost_ = 1.0f;    // the health gauge's damage trail (drains down to the live value)
    f32 hud_wagon_ghost_ = 1.0f; // ...and the wagon gauge's
    u8 last_wheel_off_ = 0;  // previous wagon wheel_off flag - the rising edge plays the crack
    u8 last_outcome_ = 0;    // previous contract outcome - an edge plays the fanfare / wreck boom

    // Debug / testing overlay (F1) state + sampled performance metrics.
    bool debug_open_ = false;       // the overlay is showing
    bool debug_god_ = false;        // godmode: players + active wagon invincible
    bool debug_no_ambush_ = false;  // no wagon ambushes spawn
    bool debug_paths_ = false;      // draw NPC pathfinding routes as world-space lines (listen server only)
    f32 fps_ = 0.0f;                // sampled client frames/sec
    f32 frame_ms_ = 0.0f;           // sampled client frame time (ms)
    f32 fps_accum_ = 0.0f;          // wall time accumulated in the current FPS sample window
    int fps_frames_ = 0;            // frames counted in the current window
    f32 server_tps_ = 0.0f;         // sampled server ticks/sec (from snapshot.tick deltas)
    u32 tps_last_tick_ = 0;         // last snapshot tick seen
    f32 tps_accum_ = 0.0f;          // wall time accumulated in the current TPS window
    u32 tps_ticks_ = 0;             // server ticks counted in the current window
    ui::Rect god_btn_{};            // clickable toggle rects in the overlay
    ui::Rect noatk_btn_{};
    ui::Rect npc_paths_btn_{};
    Vec3 aim_{0.0f};
    bool aim_valid_ = false;
};

} // namespace alryn::game
