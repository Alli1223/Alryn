#include <Alryn/Alryn.h>

#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
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

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using namespace alryn;

namespace key {
constexpr KeyCode W = 87, A = 65, S = 83, D = 68, E = 69, F = 70, H = 72, M = 77, Space = 32,
                  Escape = 256;
constexpr KeyCode Digit1 = 49, Digit2 = 50, Digit3 = 51, Digit4 = 52;
}

constexpr u16 kPort = 24650;

// Isometric-style third-person camera angle (fixed; the world rotates under it).
namespace iso {
constexpr f32 yaw_deg = 45.0f;    // compass direction we look from
constexpr f32 pitch_deg = 48.0f;  // downward tilt (higher = more top-down)
constexpr f32 distance = 15.0f;   // camera pull-back (smaller = closer/zoomed in)
constexpr f32 fov_deg = 30.0f;    // low-ish fov -> flatter, more "iso" look
} // namespace iso

// --------------------------------------------------------------------------
//  Dedicated server (headless): owns the authoritative GameServer, ~60 Hz.
// --------------------------------------------------------------------------
class ServerApp : public Application {
public:
    ServerApp() : Application(make_config()) {}

protected:
    void on_init() override {
        if (!server_.start(kPort, 1337u)) {
            ALRYN_FATAL("Failed to start server on port {}", kPort);
            close();
            return;
        }
        ALRYN_INFO("Dedicated server listening on port {} - Ctrl+C to stop.", kPort);
    }
    void on_update(Timestep dt) override {
        server_.tick(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    void on_shutdown() override { server_.stop(); }

private:
    static ApplicationConfig make_config() {
        ApplicationConfig config;
        config.name = "Alryn Server";
        config.headless = true;
        config.max_frames = 0;
        return config;
    }
    GameServer server_;
};

// --------------------------------------------------------------------------
//  Windowed multiplayer client (isometric third-person view).
// --------------------------------------------------------------------------
class ClientApp : public Application {
public:
    ClientApp(std::string host, bool host_local, u64 max_frames, bool auto_start)
        : Application(make_config(max_frames)), host_(std::move(host)), host_local_(host_local),
          auto_start_(auto_start), host_ip_(host_) {}

protected:
    void on_init() override {
        renderer_ = renderer();
        if (renderer_ == nullptr) {
            return;
        }
        if (window() != nullptr) {
            window()->set_cursor_captured(false); // free cursor for click-to-dig
        }
        if (const char* t = std::getenv("ALRYN_TIME")) {
            time_of_day_ = glm::clamp(static_cast<f32>(std::atof(t)), 0.0f, 1.0f);
        }
        if (const char* d = std::getenv("ALRYN_DAY_SECONDS")) {
            day_seconds_ = std::max(5.0f, static_cast<f32>(std::atof(d)));
        }
        marker_.create(renderer_->device(), primitives::cube(1.0f, Vec3{1.0f, 0.85f, 0.2f}));
        vehicle_meshes_.resize(vehicle_type_count());
        for (u8 i = 0; i < vehicle_type_count(); ++i) {
            vehicle_meshes_[i].create(renderer_->device(), vehicle_type(i).body());
        }
        wagon_wheel_mesh_.create(renderer_->device(), PropLibrary::build_wagon_wheel().parts[0].mesh);
        horse_body_mesh_.create(renderer_->device(), build_horse_body());
        horse_leg_mesh_.create(renderer_->device(), build_horse_leg());
        // A unit rope segment (along +Y, 0..1) for the verlet harness traces; scaled per link.
        rope_mesh_.create(renderer_->device(),
                          primitives::box(Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 1.0f, 0.5f},
                                          Vec3{0.16f, 0.11f, 0.06f}));
        // A cargo crate (spilled goods on the ground / carried back to the cart).
        goods_mesh_.create(renderer_->device(),
                           primitives::box(Vec3{-0.22f, 0.0f, -0.22f}, Vec3{0.22f, 0.44f, 0.22f},
                                           Vec3{0.55f, 0.40f, 0.22f}));
        // A large wave grid that follows the player; the water shader animates it.
        water_mesh_.create(renderer_->device(), primitives::grid(80, 2.0f, Vec3{0.1f, 0.3f, 0.4f}));
        // Shared tree meshes (trunk opaque, foliage alpha-blendable); instances vary.
        // Five variants: pine / oak / birch / broad oak / dead.
        for (int v = 0; v < 5; ++v) {
            const primitives::TreeMeshData td = primitives::tree(v);
            TreeVisual tv;
            tv.trunk.create(renderer_->device(), td.trunk);
            tv.foliage.create(renderer_->device(), td.foliage);
            tree_library_.push_back(std::move(tv));
        }
        // Shared white character shapes, tinted per bone with each player's palette.
        shape_box_.create(renderer_->device(), primitives::cube(1.0f, Vec3{1.0f}));
        shape_sphere_.create(renderer_->device(), primitives::sphere(10, 7, Vec3{1.0f}));
        shape_cylinder_.create(renderer_->device(), primitives::cylinder(10, Vec3{1.0f}));
        shape_capsule_.create(renderer_->device(), primitives::capsule(12, 3, Vec3{1.0f}));
        shape_rounded_.create(renderer_->device(), primitives::rounded_box(0.12f, Vec3{1.0f}));

        // Upload the prop catalogue (bushes, rocks, logs, fences, lanterns) to GPU.
        auto upload_props = [&](const std::vector<PropDef>& defs, std::vector<GpuProp>& out) {
            for (const PropDef& def : defs) {
                GpuProp gp;
                for (const PropPart& part : def.parts) {
                    if (part.mesh.indices.empty()) continue;
                    GpuPropPart gpp;
                    gpp.mesh.create(renderer_->device(), part.mesh);
                    gpp.layer = part.layer;
                    gp.parts.push_back(std::move(gpp));
                }
                gp.lights = def.lights;
                gp.footprint = def.footprint;
                gp.wall_height = def.wall_height;
                out.push_back(std::move(gp));
            }
        };
        upload_props(prop_lib_.bushes(), gpu_bushes_);
        upload_props(prop_lib_.rocks(), gpu_rocks_);
        upload_props(prop_lib_.logs(), gpu_logs_);
        upload_props(prop_lib_.fences(), gpu_fences_);
        upload_props(prop_lib_.fence_rails(), gpu_fence_rails_);
        upload_props(prop_lib_.lanterns(), gpu_lanterns_);
        upload_props(prop_lib_.houses(), gpu_houses_);
        upload_props(prop_lib_.walls(), gpu_walls_);
        upload_props(prop_lib_.gates(), gpu_gates_);
        upload_props(prop_lib_.wells(), gpu_wells_);
        upload_props(prop_lib_.bridges(), gpu_bridges_);
        upload_props(prop_lib_.markets(), gpu_markets_);
        upload_props(prop_lib_.paths(), gpu_paths_);
        upload_props(prop_lib_.planters(), gpu_planters_);
        upload_props(prop_lib_.fountains(), gpu_fountains_);

        // Skip the menu when launched for scripted/CI runs (--host=... or a fixed
        // frame count); otherwise open the main menu and let the player choose.
        if (auto_start_) {
            enter_game(host_local_, host_);
        } else {
            show_screen(Screen::Main);
        }
    }

    // Leaves the menu and connects: optionally hosting an in-process listen
    // server, then connecting the client. Terrain is created once the server's
    // Welcome arrives (see the SnapshotReceived/WelcomeReceived handling).
    void enter_game(bool host_local, std::string host) {
        host_ = std::move(host);
        host_local_ = host_local;
        if (host_local_) {
            if (local_server_.start(kPort, 1337u)) {
                ALRYN_INFO("Hosting a local listen server on port {}", kPort);
            } else {
                ALRYN_WARN("Port {} busy - joining the existing server instead", kPort);
                host_local_ = false;
            }
        }
        if (!client_.connect(host_, kPort)) {
            ALRYN_ERROR("Could not reach server at {}:{}", host_, kPort);
        }
        ALRYN_INFO("Connecting to {}:{} ... WASD move, mouse aim, left-click dig, "
                   "right-click add, F throw, SPACE jump, scroll zoom, ESC to menu.",
                   host_, kPort);
        state_ = AppState::Playing;
        ui_.root().clear_children(); // hide the menu while in-game
    }

    // Disconnects and returns to the main menu.
    void return_to_menu() {
        // The terrain owns GPU meshes that in-flight frames may still reference;
        // wait for the device to go idle before freeing them (else VK_ERROR_DEVICE_LOST).
        if (renderer_ != nullptr) {
            renderer_->device().wait_idle();
        }
        paused_ = false;
        client_.disconnect();
        local_server_.stop();
        terrain_.reset();
        visuals_.clear();
        enemy_visuals_.clear();
        villager_visuals_.clear();
        have_snapshot_ = false;
        my_id_ = 0;
        state_ = AppState::Menu;
        show_screen(Screen::Main);
    }

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
    void escape_pressed() {
        if (state_ == AppState::Menu) {
            menu_escape();
        } else if (!paused_) {
            enter_pause();
        } else if (current_screen_ == Screen::Settings) {
            show_screen(Screen::Pause);
        } else {
            resume();
        }
    }
    void settings_back() { show_screen(paused_ ? Screen::Pause : Screen::Main); }

    // ---- Menu construction --------------------------------------------------
    enum class Screen { Main, Join, Settings, Customise, Pause };

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
    void rebuild_ui() {
        if (renderer_ == nullptr) {
            return;
        }
        const VkExtent2D e = renderer_->extent();
        const f32 w = static_cast<f32>(e.width);
        const f32 h = static_cast<f32>(e.height);
        ui_.set_screen(w, h);
        ui_.root().clear_children();
        if (paused_) {
            // Dim the live game behind the pause UI.
            auto& dim = ui_.root().add<ui::Panel>();
            dim.bounds = ui::Rect{0.0f, 0.0f, w, h};
            dim.color = ui::theme().overlay;
            dim.border = Vec4{0.0f};
            dim.radius = 0.0f;
        }
        switch (current_screen_) {
            case Screen::Main: build_main(w, h); break;
            case Screen::Join: build_join(w, h); break;
            case Screen::Settings: build_settings(w, h); break;
            case Screen::Customise: build_customise(w, h); break;
            case Screen::Pause: build_pause(w, h); break;
        }
    }

    void build_pause(f32 w, f32 h) {
        auto& title = ui_.root().add<ui::Label>("PAUSED", std::min(w, h) * 0.07f, ui::TextAlign::Center);
        title.bounds = ui::Rect{0.0f, h * 0.24f, w, std::min(w, h) * 0.08f};

        constexpr f32 cw = 340.0f, pad = 26.0f, rh = 52.0f, gap = 14.0f;
        constexpr int rows = 4;
        const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
        const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
        auto& panel = ui_.root().add<ui::Panel>();
        panel.bounds = card;
        auto row = [&](int i) {
            return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                            card.w - pad * 2.0f, rh};
        };
        auto& resume = panel.add<ui::Button>("RESUME", [this] { this->resume(); });
        resume.primary = true;
        resume.bounds = row(0);
        panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(1);
        panel.add<ui::Button>("MAIN MENU", [this] { return_to_menu(); }).bounds = row(2);
        panel.add<ui::Button>("EXIT GAME", [this] { close(); }).bounds = row(3);
    }

    void add_title(f32 w, f32 h, const char* heading, const char* sub) {
        const f32 big = std::min(w, h) * 0.11f;
        auto& title = ui_.root().add<ui::Label>(heading, big, ui::TextAlign::Center);
        title.bounds = ui::Rect{0.0f, h * 0.15f, w, big};
        if (sub != nullptr) {
            auto& s = ui_.root().add<ui::Label>(sub, 15.0f, ui::TextAlign::Center);
            s.bounds = ui::Rect{0.0f, h * 0.15f + big + 10.0f, w, 22.0f};
            s.color = ui::theme().text_muted;
        }
    }

    void build_main(f32 w, f32 h) {
        add_title(w, h, "ALRYN", "LOW - POLY  MULTIPLAYER  SANDBOX");
        constexpr f32 cw = 360.0f, pad = 26.0f, rh = 52.0f, gap = 13.0f;
        constexpr int rows = 5;
        const f32 ch = pad * 2.0f + rows * rh + (rows - 1) * gap;
        const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
        auto& panel = ui_.root().add<ui::Panel>();
        panel.bounds = card;
        auto row = [&](int i) {
            return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                            card.w - pad * 2.0f, rh};
        };
        auto& host = panel.add<ui::Button>("HOST GAME", [this] { enter_game(true, "127.0.0.1"); });
        host.primary = true;
        host.bounds = row(0);
        panel.add<ui::Button>("CUSTOMISE", [this] { show_screen(Screen::Customise); }).bounds = row(1);
        panel.add<ui::Button>("JOIN GAME", [this] { show_screen(Screen::Join); }).bounds = row(2);
        panel.add<ui::Button>("SETTINGS", [this] { show_screen(Screen::Settings); }).bounds = row(3);
        panel.add<ui::Button>("QUIT", [this] { close(); }).bounds = row(4);
    }

    void build_join(f32 w, f32 h) {
        add_title(w, h, "JOIN GAME", nullptr);
        constexpr f32 cw = 420.0f, pad = 26.0f, rh = 52.0f, gap = 16.0f;
        const f32 ch = pad * 2.0f + 4.0f * rh + 3.0f * gap;
        const ui::Rect card{(w - cw) * 0.5f, h * 0.40f, cw, ch};
        auto& panel = ui_.root().add<ui::Panel>();
        panel.bounds = card;
        auto row = [&](int i) {
            return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                            card.w - pad * 2.0f, rh};
        };
        auto& label = panel.add<ui::Label>("SERVER ADDRESS", 15.0f);
        label.bounds = row(0);
        label.color = ui::theme().text_muted;

        auto& field = panel.add<ui::TextField>(host_ip_);
        field.placeholder = "127.0.0.1";
        field.bounds = row(1);
        field.focused = true;
        field.filter = [](char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' ||
                   (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        };
        field.on_change = [this](const std::string& s) { host_ip_ = s; };

        auto& connect = panel.add<ui::Button>("CONNECT", [this] {
            enter_game(false, host_ip_.empty() ? std::string{"127.0.0.1"} : host_ip_);
        });
        connect.primary = true;
        connect.bounds = row(2);
        panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); }).bounds = row(3);
    }

    void build_settings(f32 w, f32 h) {
        add_title(w, h, "SETTINGS", nullptr);
        constexpr f32 cw = 460.0f, pad = 28.0f, rh = 54.0f, gap = 18.0f;
        const f32 ch = pad * 2.0f + 4.0f * rh + 3.0f * gap;
        const ui::Rect card{(w - cw) * 0.5f, h * 0.38f, cw, ch};
        auto& panel = ui_.root().add<ui::Panel>();
        panel.bounds = card;
        auto row = [&](int i) {
            return ui::Rect{card.x + pad, card.y + pad + static_cast<f32>(i) * (rh + gap),
                            card.w - pad * 2.0f, rh};
        };

        panel.add<ui::Toggle>("VSYNC", vsync_, [this](bool v) {
                 vsync_ = v;
                 if (renderer_ != nullptr) {
                     renderer_->set_vsync(v);
                 }
             }).bounds = row(0);

        std::vector<std::string> res{"1280 X 720", "1600 X 900", "1920 X 1080", "FULLSCREEN"};
        panel.add<ui::Stepper>("RESOLUTION", std::move(res), res_index_,
                               [this](usize i) { apply_resolution(i); })
            .bounds = row(1);

        auto& rd = panel.add<ui::Slider>("RENDER DISTANCE", static_cast<f32>(render_distance_), 2.0f,
                                         8.0f, [this](f32 v) {
                                             render_distance_ = static_cast<int>(std::lround(v));
                                         });
        rd.integer = true;
        rd.bounds = row(2);

        panel.add<ui::Button>("BACK", [this] { settings_back(); }).bounds = row(3);
    }

    void build_customise(f32 w, f32 h) {
        rebuild_preview();

        // Controls live in a panel on the right; the 3D preview fills the rest
        // (drawn in on_render). Lay the rows out with a running vertical cursor.
        constexpr f32 pw = 400.0f;
        const ui::Rect card{w - pw - 44.0f, h * 0.12f, pw, h * 0.76f};
        auto& panel = ui_.root().add<ui::Panel>();
        panel.bounds = card;
        customise_panel_ = card;

        const f32 x = card.x + 28.0f;
        const f32 cwid = card.w - 56.0f;
        f32 y = card.y + 26.0f;
        auto place = [&](ui::Widget& widget, f32 height, f32 after = 14.0f) {
            widget.bounds = ui::Rect{x, y, cwid, height};
            y += height + after;
        };
        auto caption = [&](const char* text) {
            auto& l = panel.add<ui::Label>(text, 14.0f);
            l.bounds = ui::Rect{x, y, cwid, 16.0f};
            l.color = ui::theme().text_muted;
            y += 22.0f;
        };

        auto& header = panel.add<ui::Label>("CHARACTER", 30.0f);
        place(header, 38.0f, 18.0f);

        caption("ROLE");
        place(panel.add<ui::Stepper>(
                  "ROLE", std::vector<std::string>{"KNIGHT (TANK)", "HUNTER (DMG)", "CLERIC (HEAL)"},
                  static_cast<usize>(role_),
                  [this](usize i) { role_ = static_cast<PlayerRole>(i % kRoleCount); }),
              46.0f);

        caption("SKIN TONE");
        place(panel.add<ui::SwatchRow>(
                  std::vector<Vec3>(skin_tones().begin(), skin_tones().end()), appearance_.skin,
                  [this](usize i) { appearance_.skin = static_cast<u8>(i); rebuild_preview(); }),
              40.0f);

        caption("HAIR COLOUR");
        place(panel.add<ui::SwatchRow>(
                  std::vector<Vec3>(hair_colors().begin(), hair_colors().end()),
                  appearance_.hair_color,
                  [this](usize i) { appearance_.hair_color = static_cast<u8>(i); rebuild_preview(); }),
              40.0f);

        place(panel.add<ui::Stepper>(
                  "EYES", std::vector<std::string>{"ROUND", "WIDE", "SLEEPY", "SHARP"},
                  static_cast<usize>(appearance_.eyes),
                  [this](usize i) { appearance_.eyes = static_cast<EyeStyle>(i); rebuild_preview(); }),
              46.0f);
        place(panel.add<ui::Stepper>(
                  "EARS", std::vector<std::string>{"ROUND", "POINTED", "SMALL"},
                  static_cast<usize>(appearance_.ears),
                  [this](usize i) { appearance_.ears = static_cast<EarStyle>(i); rebuild_preview(); }),
              46.0f);
        place(panel.add<ui::Stepper>(
                  "HAIR", std::vector<std::string>{"BALD", "SHORT", "SPIKY", "MOHAWK", "PONYTAIL"},
                  static_cast<usize>(appearance_.hair),
                  [this](usize i) { appearance_.hair = static_cast<HairStyle>(i); rebuild_preview(); }),
              46.0f);

        // Bottom action row: BACK + PLAY side by side.
        const f32 by = card.y + card.h - 50.0f - 24.0f;
        const f32 half = (cwid - 12.0f) * 0.5f;
        auto& back = panel.add<ui::Button>("BACK", [this] { show_screen(Screen::Main); });
        back.bounds = ui::Rect{x, by, half, 50.0f};
        auto& play = panel.add<ui::Button>("PLAY", [this] { enter_game(true, "127.0.0.1"); });
        play.primary = true;
        play.bounds = ui::Rect{x + half + 12.0f, by, half, 50.0f};
    }

    void rebuild_preview() {
        preview_model_ = CharacterModel::create(kPreviewSeed, appearance_);
    }

    void apply_resolution(usize idx) {
        res_index_ = idx;
        if (window() == nullptr) {
            return;
        }
        static constexpr UVec2 sizes[3] = {{1280, 720}, {1600, 900}, {1920, 1080}};
        if (idx < 3) {
            window()->set_fullscreen(false);
            window()->set_size(sizes[idx].x, sizes[idx].y);
        } else {
            window()->set_fullscreen(true);
        }
        if (renderer_ != nullptr) {
            renderer_->request_resize();
        }
    }

    void on_update(Timestep dt) override {
        elapsed_ += dt.seconds;
        if (renderer_ != nullptr) {
            renderer_->set_time(elapsed_);
        }

        if (state_ == AppState::Playing) {
            if (host_local_ && local_server_.running()) {
                local_server_.tick(dt);
            }
            if (renderer_ != nullptr) {
                update_day_night(dt);
            }

            for (const net::ClientEvent& e : client_.poll()) {
                switch (e.type) {
                    case net::ClientEventType::WelcomeReceived:
                        my_id_ = e.welcome.your_id;
                        world_seed_ = e.welcome.seed;
                        terrain_ = std::make_unique<StreamingTerrain>(e.welcome.seed, 0.5f, 16,
                                                                      render_distance_);
                        ALRYN_INFO("Joined as player {} (world seed {})", my_id_, e.welcome.seed);
                        break;
                    case net::ClientEventType::SnapshotReceived:
                        snapshot_ = e.snapshot;
                        have_snapshot_ = true;
                        break;
                    case net::ClientEventType::DeformReceived:
                        if (terrain_ != nullptr) {
                            terrain_->apply_edit(e.deform.center, e.deform.radius, e.deform.amount);
                        }
                        break;
                    default:
                        break;
                }
            }

            update_camera();
            if (renderer_ != nullptr) {
                renderer_->set_player_position(local_feet()); // bends nearby vegetation
            }
            if (paused_ || map_open_) {
                aim_valid_ = false; // no dig-marker while the pause menu / map is up
            } else {
                update_aim();
            }
            for (f32& cd : ability_cd_) { // tick the local HUD cooldown estimate
                if (cd > 0.0f) {
                    cd -= dt.seconds;
                }
            }
            update_ability_vfx(dt); // buff auras + remote cast VFX
            update_particles(dt);
            send_input();
            update_visuals(dt);
            update_enemy_visuals(dt);
            update_villager_visuals(dt);
            update_feedback(dt);
            update_ropes(dt);

            if (terrain_ != nullptr && renderer_ != nullptr) {
                terrain_->update(local_feet(), renderer_->device());
            }
        } else if (renderer_ != nullptr) {
            renderer_->set_sky_color(menu_sky_); // calm backdrop behind the menu
            if (current_screen_ == Screen::Customise) {
                preview_turn_ += dt.seconds * 0.6f; // slow turntable
                preview_anim_.update(0.0f, dt);     // idle pose
                renderer_->set_sun(glm::normalize(Vec3{0.35f, 0.85f, 0.45f}),
                                   Vec3{1.0f, 0.96f, 0.9f}, 1.0f);
            }
        }

        // Keep the UI sized to the framebuffer; when the window (framebuffer) size
        // actually changes, relayout the current menu so it re-fits and re-centres.
        // We detect it here (after the swapchain has recreated) rather than on the
        // resize event, whose extent is still stale at that point.
        if (renderer_ != nullptr) {
            const VkExtent2D e = renderer_->extent();
            ui_.set_screen(static_cast<f32>(e.width), static_cast<f32>(e.height));
            if (e.width != ui_extent_.x || e.height != ui_extent_.y) {
                ui_extent_ = UVec2{e.width, e.height};
                if (state_ == AppState::Menu || paused_) {
                    rebuild_ui();
                }
            }
        }
        ui_.update(dt.seconds, pointer_pos());
    }

    void on_render() override {
        if (renderer_ == nullptr) {
            return;
        }
        if (state_ == AppState::Playing && terrain_ != nullptr) {
        renderer_->set_camera(camera_);

        terrain_->for_each_mesh([&](const Mesh& mesh) { renderer_->draw(mesh, Mat4{1.0f}); });
        terrain_->for_each_vegetation_mesh(
            [&](const Mesh& mesh) { renderer_->draw_vegetation(mesh, Mat4{1.0f}); });

        if (have_snapshot_) {
            const net::WagonState* aw = active_wagon();
            for (const net::PlayerState& p : snapshot_.players) {
                const auto it = visuals_.find(p.id);
                if (it != visuals_.end()) {
                    // Seated riders attach to the cart's tilt + bob so they ride with it.
                    const Vec3 feet =
                        (p.seated != 0 && aw != nullptr) ? attach_to_wagon(*aw, p.position) : p.position;
                    draw_character(it->second, feet, p.yaw, p.seated != 0, static_cast<int>(p.role));
                }
                if (p.carrying != 0) {
                    draw_carried_good(p.position, p.yaw);
                }
            }
        }
        draw_villagers();
        draw_enemies();
        draw_fires();
        draw_barricades();
        draw_wagons();
        draw_goods();
        draw_auras();
        draw_shields();
        draw_particles();

        // Tree trunks (opaque, but they obey the peek-through dissolve so a trunk between
        // the camera and the player melts away just like the foliage does).
        terrain_->for_each_tree([&](const TreeInstance& t) {
            renderer_->draw_cutout(tree_library_[tree_index(t)].trunk, tree_model(t));
        });

        // Discrete props: rocks/houses (opaque + emissive), bushes (foliage).
        terrain_->for_each_prop([&](const PropInstance& p) { draw_prop(p); });

        // Projectiles (server-simulated). kind 0 = thrown rock (grey sphere), 1 =
        // enemy arrow (a slim dark bolt).
        if (have_snapshot_) {
            for (const net::ProjectileState& pr : snapshot_.projectiles) {
                if (pr.kind == 4) { // cleric arcane bolt: a violet glowing orb + halo
                    renderer_->draw_emissive(shape_sphere_,
                                             glm::translate(Mat4{1.0f}, pr.position) *
                                                 glm::scale(Mat4{1.0f}, Vec3{0.26f}),
                                             Vec4{0.72f, 0.5f, 1.0f, 1.0f});
                    renderer_->draw_glow(shape_sphere_,
                                         glm::translate(Mat4{1.0f}, pr.position) *
                                             glm::scale(Mat4{1.0f}, Vec3{0.6f}),
                                         Vec4{0.6f, 0.4f, 1.0f, 0.5f});
                } else if (pr.kind == 2) { // cleric holy bolt: a bright glowing mote
                    renderer_->draw_emissive(shape_sphere_,
                                             glm::translate(Mat4{1.0f}, pr.position) *
                                                 glm::scale(Mat4{1.0f}, Vec3{0.3f}),
                                             Vec4{0.6f, 1.0f, 0.78f, 1.0f});
                } else if (pr.kind == 1 || pr.kind == 3) {
                    // An arrow: orient the long (local +Z) axis along its travel direction so it
                    // always points the way it flies (and the way it's stuck in on landing).
                    const Vec4 col = pr.kind == 1 ? Vec4{0.3f, 0.22f, 0.14f, 1.0f}  // enemy arrow
                                                  : Vec4{0.72f, 0.62f, 0.4f, 1.0f}; // friendly arrow
                    renderer_->draw(shape_box_,
                                    glm::translate(Mat4{1.0f}, pr.position) * orient_to(pr.dir) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.06f, 0.06f, 0.6f}),
                                    col);
                } else {
                    renderer_->draw(shape_sphere_,
                                    glm::translate(Mat4{1.0f}, pr.position) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.36f}),
                                    Vec4{0.55f, 0.5f, 0.45f, 1.0f});
                }
            }
        }

        if (aim_valid_) {
            renderer_->draw(marker_, glm::translate(Mat4{1.0f}, aim_) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.3f}));
        }

        // Transparent water surface, drawn after opaque geometry. Depth-tested but
        // not depth-written, so terrain above the waterline correctly hides it.
        const Vec3 feet = local_feet();
        renderer_->draw_water(water_mesh_, glm::translate(Mat4{1.0f}, Vec3{feet.x,
                                                          worldgen::water_level, feet.z}));

        // Tree foliage (transparent), drawn last. Fades when the local player is
        // under the canopy, AND when a canopy is near the camera - in a dense forest
        // the third-person camera sits among the trees, so foliage right at the eye
        // is faded out to keep the player and the scene visible.
        const Vec3 cam_eye = camera_.position();
        terrain_->for_each_tree([&](const TreeInstance& t) {
            const f32 dxz = glm::length(Vec2{t.position.x - feet.x, t.position.z - feet.z});
            // Clear a generous bubble around the character so you always see them, even in
            // thick forest (the radius scales with the big canopies).
            const f32 canopy = std::max(2.6f * t.scale, 7.0f);
            f32 alpha = glm::mix(0.12f, 1.0f, glm::smoothstep(canopy * 0.35f, canopy, dxz));
            // Fade canopies near the camera / between it and the player so they don't block
            // the view - the distance band scales with the tree so big trees clear early.
            const Vec3 cc = t.position + Vec3{0.0f, 3.2f * t.scale, 0.0f};
            const f32 cam_fade = glm::mix(0.05f, 1.0f,
                                          glm::smoothstep(3.0f, 5.0f + t.scale * 1.6f,
                                                          glm::length(cc - cam_eye)));
            alpha = std::min(alpha, cam_fade);
            renderer_->draw_transparent(tree_library_[tree_index(t)].foliage, tree_model(t),
                                        Vec4{t.tint, alpha});
        });

        draw_health_bars();
        draw_hud();
        if (map_open_) {
            draw_map();
        }
        }

        if (state_ == AppState::Menu && current_screen_ == Screen::Customise) {
            draw_preview();
        }

        ui_.render(*renderer_); // 2D menu overlay, drawn on top of the scene
    }

    // Renders the customisation turntable: the live character centred in the area
    // left of the controls panel. The camera distance + horizontal offset are
    // derived from the window aspect and the panel position so the whole avatar
    // (head to feet) always fits without clipping, in any window shape.
    void draw_preview() {
        const VkExtent2D ext = renderer_->extent();
        const f32 W = static_cast<f32>(ext.width);
        const f32 H = static_cast<f32>(ext.height);
        if (W <= 0.0f || H <= 0.0f) {
            return;
        }
        const f32 aspect = W / H;
        const f32 panel_left = customise_panel_.w > 0.0f ? customise_panel_.x : W;

        const f32 fovy = radians(32.0f);
        const f32 tan_v = std::tan(fovy * 0.5f);
        const f32 height = preview_model_.height();
        const f32 ty = height * 0.5f;          // look at the avatar's middle
        const f32 half_h = height * 0.62f;     // half-height + headroom (covers hair)
        const f32 half_w = 0.55f;              // generous half-width
        const f32 free_frac = glm::clamp(panel_left / W, 0.25f, 1.0f);

        // Distance that fits both the height and the (panel-limited) width, + margin.
        const f32 dist_v = half_h / tan_v;
        const f32 dist_h = half_w / (tan_v * aspect * std::max(free_frac * 0.85f, 0.1f));
        const f32 dist = std::max(dist_v, dist_h) * 1.08f;

        // Offset the look-at so world x=0 projects to the centre of the free area.
        const f32 ndc_x = 2.0f * (panel_left * 0.5f / W) - 1.0f;
        const f32 target_x = -ndc_x * tan_v * aspect * dist;

        const Vec3 target{target_x, ty, 0.0f};
        const Vec3 eye = target + Vec3{0.0f, ty * 0.18f, dist};
        camera_.set_perspective(fovy, aspect, 0.1f, 50.0f);
        camera_.look_at(eye, target);
        renderer_->set_camera(camera_);

        const Mat4 root = glm::rotate(Mat4{1.0f}, preview_turn_, Vec3{0.0f, 1.0f, 0.0f}) *
                          preview_anim_.body_offset(); // soft idle breathe on the turntable
        const std::vector<Quat> pose = preview_anim_.pose(preview_model_);
        draw_rig(preview_model_, preview_model_.bone_matrices(root, pose));
    }

    // Draws every bone of a posed character with its palette colour (times `tint`)
    // and shape mesh. The tint lets enemies read as hostile without new models.
    void draw_rig(const CharacterModel& model, const std::vector<Mat4>& mats,
                  const Vec3& tint = Vec3{1.0f}) {
        const std::vector<Bone>& bones = model.bones();
        const CharacterPalette& pal = model.palette();
        for (usize i = 0; i < bones.size(); ++i) {
            const Vec3 color = bones[i].color == BoneColor::Skin    ? pal.skin
                               : bones[i].color == BoneColor::Shirt ? pal.shirt
                               : bones[i].color == BoneColor::Pants ? pal.pants
                               : bones[i].color == BoneColor::Hair  ? pal.hair
                                                                    : pal.eye;
            const Mesh& shape = bones[i].shape == BoneShape::Sphere       ? shape_sphere_
                                : bones[i].shape == BoneShape::Cylinder   ? shape_cylinder_
                                : bones[i].shape == BoneShape::Capsule    ? shape_capsule_
                                : bones[i].shape == BoneShape::RoundedBox ? shape_rounded_
                                                                          : shape_box_;
            renderer_->draw(shape, mats[i], Vec4{color * tint, 1.0f});
        }
    }

    // Draws a spear gripped in the character's right hand (anchored to the lower-arm
    // bone so it swings with the animation, not a stick floating beside them).
    void draw_held_spear(const CharacterModel& model, const std::vector<Mat4>& mats) {
        const std::vector<Bone>& bones = model.bones();
        int hand = -1;
        for (usize i = 0; i < bones.size(); ++i) {
            if (bones[i].part == BonePart::LowerArmR) {
                hand = static_cast<int>(i);
                break;
            }
        }
        if (hand < 0) {
            return;
        }
        const Vec3 grip = Vec3{mats[hand][3]}; // world position of the forearm/hand
        const Mat4 shaft = glm::translate(Mat4{1.0f}, grip + Vec3{0.0f, 0.45f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{0.055f, 1.9f, 0.055f});
        renderer_->draw(shape_box_, shaft, Vec4{0.4f, 0.28f, 0.16f, 1.0f}); // wooden haft
        const Mat4 head = glm::translate(Mat4{1.0f}, grip + Vec3{0.0f, 1.45f, 0.0f}) *
                          glm::scale(Mat4{1.0f}, Vec3{0.1f, 0.34f, 0.05f});
        renderer_->draw(shape_box_, head, Vec4{0.7f, 0.73f, 0.8f, 1.0f}); // steel spearhead
    }

    // World position of a hand (the far end of a forearm), in the forearm JOINT frame.
    static Mat4 hand_frame(const CharacterModel& model, const std::vector<Mat4>& jmats, BonePart arm) {
        const std::vector<Bone>& bones = model.bones();
        for (usize i = 0; i < bones.size(); ++i) {
            if (bones[i].part == arm) {
                const f32 wrist = bones[i].box_center.y * 2.0f; // far end of the forearm (local -Y)
                return jmats[i] * glm::translate(Mat4{1.0f}, Vec3{0.0f, wrist, 0.0f});
            }
        }
        return Mat4{1.0f};
    }

    // The role's weapon, RIGIDLY gripped in the hand JOINT frame so it rotates WITH the arm - a
    // Knight's sword swings with the attack animation (it IS the blade you hold) and the shield
    // raises with the block. NOTE: the rig's bone labels are mirrored - the *L* arm is on the
    // player's RIGHT (the weapon hand), the *R* arm on their LEFT (the shield hand). The Cleric's
    // staff is handled separately (draw_cleric_staff) so it can behave like a walking stick.
    void draw_role_weapon(const CharacterModel& model, const std::vector<Mat4>& jmats,
                          PlayerRole role) {
        const Mat4 weapon_hand = hand_frame(model, jmats, BonePart::LowerArmL); // player's right
        switch (role) {
            case PlayerRole::Knight: {
                // Sword: blade continues along the forearm (local -Y), tilted slightly forward.
                const Mat4 grip = weapon_hand * glm::rotate(Mat4{1.0f}, -0.35f, Vec3{1.0f, 0.0f, 0.0f});
                renderer_->draw(shape_box_,
                                grip * glm::scale(Mat4{1.0f}, Vec3{0.30f, 0.05f, 0.07f}),
                                Vec4{0.36f, 0.26f, 0.12f, 1.0f}); // crossguard
                renderer_->draw(shape_box_,
                                grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.12f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.22f, 0.05f}),
                                Vec4{0.3f, 0.2f, 0.1f, 1.0f}); // grip handle (into the fist)
                renderer_->draw(shape_box_,
                                grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, -0.62f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.07f, 1.1f, 0.16f}),
                                Vec4{0.80f, 0.84f, 0.92f, 1.0f}); // blade
                // Shield on the off (player's left) hand, in FRONT of the forearm (local +Z).
                const Mat4 shield_hand = hand_frame(model, jmats, BonePart::LowerArmR);
                renderer_->draw(shape_rounded_,
                                shield_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 0.18f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.58f, 0.74f, 0.13f}),
                                Vec4{0.46f, 0.33f, 0.2f, 1.0f}); // shield
                renderer_->draw(shape_sphere_,
                                shield_hand * glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 0.25f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.13f}),
                                Vec4{0.82f, 0.7f, 0.32f, 1.0f}); // boss
                break;
            }
            case PlayerRole::Hunter: {
                // A vertical bow held in the hand: stave along the forearm axis.
                renderer_->draw(shape_box_,
                                weapon_hand * glm::scale(Mat4{1.0f}, Vec3{0.05f, 1.5f, 0.06f}),
                                Vec4{0.4f, 0.26f, 0.12f, 1.0f}); // stave
                for (f32 s : {1.0f, -1.0f}) {
                    renderer_->draw(shape_box_,
                                    weapon_hand *
                                        glm::translate(Mat4{1.0f}, Vec3{0.07f, s * 0.66f, 0.0f}) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.22f, 0.05f}),
                                    Vec4{0.4f, 0.26f, 0.12f, 1.0f}); // recurved tips
                }
                break;
            }
            case PlayerRole::Cleric:
                break; // staff drawn by draw_cleric_staff (walking-stick behaviour)
        }
    }

    // The Cleric's staff held VERTICAL like a walking stick: the hand grips the top, the shaft
    // drops to the ground, and as they walk the tip plants ahead then drifts back (and lifts to
    // swing forward again), synced to the gait. Idle = a still, upright staff.
    void draw_cleric_staff(const CharacterModel& model, const std::vector<Mat4>& jmats,
                           const Vec3& feet, const CharacterAnimator& anim, f32 yaw) {
        const Mat4 hand = hand_frame(model, jmats, BonePart::LowerArmL);
        const Vec3 top = Vec3{hand[3]};
        const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
        const f32 stride = anim.stride();
        const f32 ph = anim.phase();
        const f32 swing = std::cos(ph) * 0.42f * stride;             // tip: +ahead .. -behind
        const f32 reach = std::max(0.6f, top.y - feet.y + 0.05f);    // length down to the ground
        const Vec3 dir = glm::normalize(Vec3{facing.x * swing, -1.0f, facing.z * swing});
        const Vec3 bottom = top + dir * reach;
        const Vec3 mid = (top + bottom) * 0.5f;
        renderer_->draw(shape_box_,
                        glm::translate(Mat4{1.0f}, mid) * orient_to(bottom - top) *
                            glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.05f, reach}),
                        Vec4{0.46f, 0.31f, 0.16f, 1.0f}); // shaft
        renderer_->draw_emissive(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, top + Vec3{0.0f, 0.16f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.18f}),
                                 Vec4{0.65f, 0.55f, 1.0f, 1.0f}); // glowing orb at the head
    }

    // Fire ability slot (0/1/2) for the local player: gate on the client cooldown estimate,
    // queue it for the server, mirror the cooldown for the HUD, and play the cast VFX + any
    // buff aura instantly so it feels responsive (the server stays authoritative).
    void cast_ability(u8 slot) {
        if (slot >= kAbilitySlots || ability_cd_[slot] > 0.0f) {
            return;
        }
        pending_ability_ = static_cast<u8>(slot + 1);
        ability_cd_[slot] = ability_def(role_, slot).cooldown;
        spawn_ability_vfx(role_, slot, local_feet(), face_yaw_, aim_valid_ ? aim_ : local_feet());
        if (role_ == PlayerRole::Knight && slot == 1) {
            bulwark_fx_ = kBulwarkDuration;
        } else if (role_ == PlayerRole::Hunter && slot == 2) {
            dash_fx_ = kDashDuration;
        }
    }

    // A quick flourish at the hand for a Hunter/Cleric primary attack (the projectile itself is
    // server-spawned + networked; this is just the instant local muzzle/cast feedback).
    void spawn_primary_vfx() {
        const Vec3 feet = local_feet();
        const Vec3 facing{std::cos(face_yaw_), 0.0f, std::sin(face_yaw_)};
        const Vec3 hand = feet + Vec3{0.0f, 1.1f, 0.0f} + facing * 0.5f;
        Vec3 dir = facing;
        if (aim_valid_) {
            Vec3 d = aim_ - hand;
            if (glm::length(d) > 0.3f) {
                dir = glm::normalize(d);
            }
        }
        if (role_ == PlayerRole::Hunter) {
            emit(hand, Vec3{0.0f}, Vec4{0.85f, 1.0f, 0.7f, 1.0f}, 0.12f, 0.3f, 1); // bow flash
            for (int i = 0; i < 9; ++i) {
                emit(hand, dir * frand(4.0f, 9.0f) + rand_dir() * 1.0f,
                     Vec4{0.7f, 1.0f, 0.65f, 0.9f}, 0.3f, 0.1f, 1);
            }
        } else if (role_ == PlayerRole::Cleric) {
            // Arcane motes gather + burst forward in violet.
            emit(hand, Vec3{0.0f}, Vec4{0.7f, 0.45f, 1.0f, 1.0f}, 0.18f, 0.42f, 1);
            for (int i = 0; i < 16; ++i) {
                const Vec3 v = dir * frand(2.5f, 7.0f) + rand_dir() * 1.6f;
                emit(hand + rand_dir() * 0.25f, v, Vec4{0.78f, 0.5f, 1.0f, 0.95f}, 0.45f, 0.12f, 1);
            }
        }
    }

    void on_event(Event& event) override {
        EventDispatcher dispatcher{event};

        // (Window resizes are handled in on_update once the swapchain has the new
        // size, so the menu relayout reads the correct extent.)

        // Route input to the UI whenever it's showing: the main menu, or the
        // in-game pause menu overlaid on the (still-live) game.
        if (state_ == AppState::Menu || paused_) {
            dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
                return ui_.pointer_down(pointer_pos(), e.button());
            });
            dispatcher.dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
                return ui_.pointer_up(pointer_pos(), e.button());
            });
            dispatcher.dispatch<KeyTypedEvent>([&](KeyTypedEvent& e) {
                return ui_.text(static_cast<char>(e.codepoint()));
            });
            dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
                if (e.key() == key::Escape) {
                    escape_pressed();
                    return true;
                }
                return ui_.key(e.key());
            });
            return;
        }

        // Full-screen map overlay (M). While it's open, world input is frozen and all
        // other in-game input is swallowed.
        {
            bool consumed = false;
            dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
                if (e.key() == key::M) {
                    map_open_ = !map_open_;
                    consumed = true;
                    return true;
                }
                if (map_open_ && e.key() == key::Escape) {
                    map_open_ = false;
                    consumed = true;
                    return true;
                }
                return false;
            });
            if (map_open_ || consumed) {
                return;
            }
        }

        // In-game input (not paused).
        dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
            if (e.button() == 0) {
                // A click on the contract panel's ACCEPT / CANCEL buttons takes priority over melee.
                const Vec2 p = pointer_pos();
                if (in_rect(p, accept_btn_)) {
                    selected_wagon_ = panel_wagon_; // accept -> this becomes our vote
                    return true;
                }
                if (in_rect(p, cancel_btn_)) {
                    selected_wagon_ = 0; // withdraw the offer
                    return true;
                }
                // Primary attack is role-specific: the Knight swings the held sword, the Hunter
                // looses an arrow, the Cleric casts a damage spell (both fire a role projectile
                // the server picks). Only the Knight melees + plays the sword swing.
                if (role_ == PlayerRole::Knight) {
                    pending_attack_ = true;      // melee swing (carves terrain if nothing to hit)
                    pending_local_swing_ = true; // swing the actual held sword on our own model
                } else {
                    pending_fire_ = true;        // Hunter arrow / Cleric arcane bolt
                    spawn_primary_vfx();         // muzzle / cast flourish at the hand
                }
            } else if (e.button() == 1) {
                // Right mouse is held: a Knight raises their shield, a Cleric channels a heal
                // aura (charges while held). Everyone else builds terrain.
                if (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric) {
                    blocking_ = true;
                } else {
                    pending_add_ = true;
                }
            }
            return false;
        });
        dispatcher.dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
            if (e.button() == 1) {
                blocking_ = false; // lower the shield / stop channelling
            }
            return false;
        });
        dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
            if (e.key() == key::Escape) {
                escape_pressed(); // opens the pause menu
            } else if (e.key() == key::F) {
                pending_fire_ = true; // throw a rock toward the cursor
            } else if (e.key() == key::E) {
                pending_grab_ = true; // hitch / unhitch the nearest wagon (manual haul)
            } else if (e.key() == key::H) {
                vote_mode_ = vote_mode_ == 1 ? 2 : 1; // toggle hire driver / haul manually
            } else if (e.key() == key::Digit1 || e.key() == key::Digit2 ||
                       e.key() == key::Digit3 || e.key() == key::Digit4) {
                cast_ability(static_cast<u8>(e.key() - key::Digit1)); // abilities (wagons start by walking up)
            }
            return false;
        });
    }

    void on_shutdown() override {
        // Frees GPU resources while the device is still alive; wait for any
        // in-flight frame to finish first so nothing is freed mid-use.
        if (renderer_ != nullptr) {
            renderer_->device().wait_idle();
        }
        visuals_.clear();
        shape_box_.destroy();
        shape_sphere_.destroy();
        shape_cylinder_.destroy();
        shape_capsule_.destroy();
        shape_rounded_.destroy();
        for (auto& tv : tree_library_) {
            tv.trunk.destroy();
            tv.foliage.destroy();
        }
        tree_library_.clear();
        for (std::vector<GpuProp>* set : {&gpu_bushes_, &gpu_rocks_, &gpu_logs_, &gpu_fences_,
                                          &gpu_fence_rails_, &gpu_lanterns_, &gpu_houses_,
                                          &gpu_walls_, &gpu_gates_, &gpu_wells_, &gpu_bridges_,
                                          &gpu_markets_, &gpu_paths_, &gpu_planters_,
                                          &gpu_fountains_}) {
            for (GpuProp& gp : *set) {
                for (GpuPropPart& part : gp.parts) {
                    part.mesh.destroy();
                }
            }
            set->clear();
        }
        marker_.destroy();
        for (Mesh& vm : vehicle_meshes_) {
            vm.destroy();
        }
        vehicle_meshes_.clear();
        wagon_wheel_mesh_.destroy();
        horse_body_mesh_.destroy();
        horse_leg_mesh_.destroy();
        rope_mesh_.destroy();
        goods_mesh_.destroy();
        water_mesh_.destroy();
        terrain_.reset();
        client_.disconnect();
        local_server_.stop();
    }

private:
    struct PlayerVisual {
        CharacterModel model;
        CharacterAnimator animator;
        CharacterAppearance appearance;
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        bool has_last = false;
        u8 last_action = 0; // to fire a swing once on the rising edge of a networked action
    };

    // A networked enemy's renderable: one shared hostile model, animated from
    // snapshot position deltas (no animation data on the wire).
    struct EnemyVisual {
        CharacterModel model = CharacterModel::create(0u, enemy_look());
        CharacterAnimator animator;
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        u8 last_action = 0;
    };

    PlayerVisual& ensure_visual(net::PlayerId id, const CharacterAppearance& appearance) {
        const auto it = visuals_.find(id);
        if (it != visuals_.end()) {
            // Rebuild the model if the player changed their look.
            if (!(it->second.appearance == appearance)) {
                it->second.appearance = appearance;
                it->second.model = CharacterModel::create(id, appearance);
            }
            return it->second;
        }
        PlayerVisual v;
        v.appearance = appearance;
        v.model = CharacterModel::create(id, appearance);
        return visuals_.emplace(id, std::move(v)).first->second;
    }

    // Advances the time of day and feeds the renderer a moving sun + sky colour.
    // When connected, the server owns the clock (so lighting matches when villagers
    // sleep); otherwise we advance it locally. ALRYN_TIME (0..1) pins the starting
    // time; ALRYN_DAY_SECONDS sets cycle length.
    void update_day_night(Timestep dt) {
        if (have_snapshot_) {
            time_of_day_ = snapshot_.time_of_day; // authoritative day/night clock
        } else {
            time_of_day_ += dt.seconds / day_seconds_;
            time_of_day_ -= std::floor(time_of_day_);
        }

        // The sun arcs east -> overhead -> west; below the horizon is night.
        const f32 a = (time_of_day_ - 0.25f) * TwoPi;
        const Vec3 sun_dir = glm::normalize(Vec3{std::cos(a), std::sin(a), 0.28f});
        const f32 h = sun_dir.y;
        const f32 intensity = glm::smoothstep(-0.04f, 0.18f, h);
        sun_intensity_ = intensity;

        const Vec3 horizon{1.0f, 0.5f, 0.28f};
        const Vec3 noon{1.0f, 0.96f, 0.88f};
        const Vec3 sun_color = glm::mix(horizon, noon, glm::smoothstep(0.0f, 0.32f, h));

        const Vec3 sky_night{0.03f, 0.04f, 0.08f};
        const Vec3 sky_day{0.46f, 0.62f, 0.82f};
        const Vec3 sky_dusk{0.85f, 0.45f, 0.30f};
        Vec3 sky = glm::mix(sky_night, sky_day, intensity);
        const f32 dusk = glm::clamp(1.0f - std::abs(h) * 3.5f, 0.0f, 1.0f) * intensity;
        sky = glm::mix(sky, sky_dusk, dusk * 0.6f);

        renderer_->set_sun(sun_dir, sun_color, intensity);
        renderer_->set_sky_color(sky);
    }

    void update_camera() {
        // Scroll wheel zooms by scaling the camera pull-back distance.
        if (Input* in = input()) {
            const f32 s = in->scroll_delta();
            if (s != 0.0f) {
                cam_distance_ = glm::clamp(cam_distance_ * std::pow(0.88f, s), 4.0f, 45.0f);
            }
        }
        const f32 cam_yaw = radians(iso::yaw_deg);
        const f32 cam_pitch = radians(iso::pitch_deg);
        const Vec3 dir_to_cam{std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                              std::cos(cam_pitch) * std::sin(cam_yaw)};
        const Vec3 target = local_feet() + Vec3{0.0f, 0.7f, 0.0f};
        const Vec3 eye = target + dir_to_cam * cam_distance_;
        camera_.set_perspective(radians(iso::fov_deg), renderer_->aspect(), 0.5f, 400.0f);
        camera_.look_at(eye, target);
    }

    void update_visuals(Timestep dt) {
        if (!have_snapshot_) {
            return;
        }
        for (const net::PlayerState& p : snapshot_.players) {
            PlayerVisual& v = ensure_visual(p.id, p.appearance);
            f32 measured = 0.0f;
            if (v.has_last && dt.seconds > 0.0001f) {
                Vec3 d = p.position - v.last_pos;
                d.y = 0.0f;
                measured = glm::length(d) / dt.seconds;
            }
            v.speed = glm::mix(v.speed, measured, 0.3f);
            v.last_pos = p.position;
            v.has_last = true;

            // Drive the action layer. The local player uses its own input for zero-latency
            // feedback; remote players follow the networked `action` field.
            const bool is_local = p.id == my_id_;
            v.animator.set_blocking(is_local ? blocking_ : (p.action == 2));
            if (is_local) {
                if (pending_local_swing_) {
                    v.animator.play_swing();
                }
            } else if (p.action == 1 && v.last_action != 1) {
                v.animator.play_swing(); // rising edge of a remote swing
            }
            v.last_action = p.action;
            v.animator.update(v.speed, dt);
        }
        pending_local_swing_ = false;
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
    void update_feedback(Timestep dt) {
        const f32 hp = local_health();
        if (hp < last_health_ - 0.001f) {
            hit_flash_ = 1.0f;
        }
        last_health_ = hp;
        hit_flash_ = std::max(0.0f, hit_flash_ - dt.seconds * 1.8f);
    }

    // ---- Particle VFX ------------------------------------------------------------
    void emit(const Vec3& pos, const Vec3& vel, const Vec4& color, f32 life, f32 size,
              u8 style = 0, f32 gravity = 0.0f, f32 drag = 1.6f) {
        if (particles_.size() > 1400) {
            return; // hard cap so a busy fight can't run away with the pool
        }
        Particle p;
        p.pos = pos;
        p.vel = vel;
        p.color = color;
        p.life = life;
        p.max_life = life;
        p.size = size;
        p.style = style;
        p.gravity = gravity;
        p.drag = drag;
        particles_.push_back(p);
    }

    // A spray of `n` motes from `center`, biased upward by `up` (m/s), with random speed.
    void emit_burst(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                    u8 style = 1, f32 up = 0.0f, f32 gravity = 0.0f) {
        for (int i = 0; i < n; ++i) {
            Vec3 d = rand_dir();
            d.y = std::abs(d.y) * 0.6f;
            emit(center, d * frand(0.3f, 1.0f) * speed + Vec3{0.0f, up, 0.0f},
                 Vec4{Vec3{color}, color.a * frand(0.7f, 1.0f)}, life * frand(0.7f, 1.0f),
                 size * frand(0.7f, 1.2f), style, gravity);
        }
    }

    // A flat expanding ring of motes on the ground (radius grows via outward velocity).
    void emit_ring(const Vec3& center, const Vec4& color, int n, f32 speed, f32 life, f32 size,
                   u8 style = 1) {
        for (int i = 0; i < n; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(n) + frand(-0.1f, 0.1f);
            const Vec3 dir{std::cos(a), 0.0f, std::sin(a)};
            emit(center + Vec3{0.0f, 0.15f, 0.0f}, dir * speed + Vec3{0.0f, frand(0.3f, 1.0f), 0.0f},
                 color, life, size, style, 0.0f, 2.4f);
        }
    }

    void update_particles(Timestep dt) {
        const f32 s = dt.seconds;
        for (Particle& p : particles_) {
            p.life -= s;
            p.vel *= std::max(0.0f, 1.0f - p.drag * s);
            p.vel.y -= p.gravity * s;
            p.pos += p.vel * s;
        }
        std::erase_if(particles_, [](const Particle& p) { return p.life <= 0.0f; });

        // Glowing trails behind in-flight holy bolts / arrows.
        if (have_snapshot_) {
            for (const net::ProjectileState& pr : snapshot_.projectiles) {
                if (pr.kind == 2) {
                    emit(pr.position, rand_dir() * 0.3f, Vec4{0.55f, 1.0f, 0.8f, 0.9f}, 0.4f, 0.16f, 1);
                } else if (pr.kind == 4) { // arcane bolt: a swirling violet trail
                    emit(pr.position, rand_dir() * 0.5f, Vec4{0.72f, 0.45f, 1.0f, 0.95f}, 0.45f, 0.15f, 1);
                } else if (pr.kind == 3) {
                    emit(pr.position, Vec3{0.0f}, Vec4{0.85f, 0.95f, 0.7f, 0.5f}, 0.25f, 0.07f, 1);
                }
            }
            // Motes rising out of each ground aura, tinted by its kind (heal = gentle, drifting;
            // consecration = flickering holy-fire that climbs faster).
            for (const net::AuraState& a : snapshot_.auras) {
                const AuraProps props = aura_props(static_cast<AuraKind>(a.kind));
                const bool fire = static_cast<AuraKind>(a.kind) == AuraKind::Consecration;
                for (int i = 0; i < (fire ? 3 : 2); ++i) {
                    const f32 ang = frand(0.0f, TwoPi);
                    const f32 rr = frand(0.0f, a.radius);
                    const Vec3 p = a.position + Vec3{std::cos(ang) * rr, 0.05f, std::sin(ang) * rr};
                    const f32 rise = fire ? frand(1.4f, 3.0f) : frand(0.9f, 2.0f);
                    emit(p, Vec3{0.0f, rise, 0.0f}, Vec4{props.color, 0.88f}, fire ? 0.5f : 0.8f,
                         fire ? 0.13f : 0.1f, 1, fire ? -1.4f : -0.8f);
                }
            }
            // Shimmer sparkles orbiting each Aegis shield bubble.
            auto sparkle = [&](const Vec3& feet, f32 strength) {
                if (strength > 0.02f) {
                    const Vec3 d = rand_dir();
                    emit(feet + Vec3{0.0f, 1.0f, 0.0f} + d * 1.15f, d * 0.2f,
                         Vec4{0.6f, 0.85f, 1.0f, 0.8f * strength}, 0.4f, 0.08f, 1);
                }
            };
            for (const net::PlayerState& p : snapshot_.players) {
                sparkle(p.position, static_cast<f32>(p.shield) / 255.0f);
            }
            for (const net::VillagerState& v : snapshot_.villagers) {
                sparkle(v.position, static_cast<f32>(v.shield) / 255.0f);
            }
        }
    }

    // The glowing ground disc + soft dome of each ground aura, plus a soft light at night so the
    // aura lights its surroundings. Colour comes from the shared aura_props table (data-driven, so
    // a new aura kind renders + lights itself with no extra client code). Rising motes are emitted
    // in update_particles. Drawn additively so it brightens the ground without occluding.
    void draw_auras() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        const f32 night = 1.0f - sun_intensity_;
        for (const net::AuraState& a : snapshot_.auras) {
            const AuraProps props = aura_props(static_cast<AuraKind>(a.kind));
            const f32 r = a.radius;
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.08f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{r, 0.1f, r}),
                                 Vec4{props.color, 0.32f}); // ground disc
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, a.position + Vec3{0.0f, 0.3f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{r * 0.95f, r * 0.5f, r * 0.95f}),
                                 Vec4{props.color, 0.1f}); // soft dome
            // A gentle (unshadowed) light so the aura illuminates the ground at night.
            if (props.light > 0.0f && night > 0.12f) {
                Renderer::SpotLight sl;
                sl.position = a.position + Vec3{0.0f, 2.0f, 0.0f};
                sl.direction = Vec3{0.0f, -1.0f, 0.0f};
                sl.color = props.color * (props.light * night);
                sl.range = r * 2.4f;
                sl.cone_outer_cos = std::cos(glm::radians(75.0f));
                sl.cone_inner_cos = std::cos(glm::radians(45.0f));
                sl.cast_shadow = false;
                renderer_->add_light(sl);
            }
        }
    }

    // The Aegis protective bubble around any shielded player / NPC: a softly pulsing translucent
    // shell + an additive glow, brighter while the shield is strong. (Shimmer motes orbit it from
    // update_particles.)
    void draw_shields() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        const f32 pulse = 1.0f + 0.04f * std::sin(elapsed_ * 6.0f);
        auto bubble = [&](const Vec3& feet, f32 strength) {
            if (strength <= 0.02f) {
                return;
            }
            const Vec3 c = feet + Vec3{0.0f, 1.0f, 0.0f};
            const f32 rad = 1.15f * pulse;
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{rad}),
                                 Vec4{0.45f, 0.7f, 1.0f, 0.16f * strength});
            renderer_->draw_transparent(shape_sphere_,
                                        glm::translate(Mat4{1.0f}, c) *
                                            glm::scale(Mat4{1.0f}, Vec3{rad * 1.02f}),
                                        Vec4{0.55f, 0.8f, 1.0f, 0.13f * strength});
        };
        for (const net::PlayerState& p : snapshot_.players) {
            bubble(p.position, static_cast<f32>(p.shield) / 255.0f);
        }
        for (const net::VillagerState& v : snapshot_.villagers) {
            bubble(v.position, static_cast<f32>(v.shield) / 255.0f);
        }
    }

    void draw_particles() {
        if (renderer_ == nullptr) {
            return;
        }
        for (const Particle& p : particles_) {
            const f32 t = glm::clamp(p.life / p.max_life, 0.0f, 1.0f);
            const f32 sz = p.size * (0.35f + 0.65f * t);
            const Mat4 m = glm::translate(Mat4{1.0f}, p.pos) * glm::scale(Mat4{1.0f}, Vec3{sz});
            const Vec4 col{Vec3{p.color}, p.color.a * t};
            if (p.style == 1) {
                renderer_->draw_glow(shape_sphere_, m, col);
            } else {
                renderer_->draw_emissive(shape_sphere_, m, col);
            }
        }
    }

    // The showy burst for an ability cast, played for whoever cast it (the local player on
    // keypress for instant feel; remote players when the snapshot reports their `cast`).
    void spawn_ability_vfx(PlayerRole role, u8 slot, const Vec3& feet, f32 yaw, const Vec3& aim) {
        const Vec3 chest = feet + Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 facing{std::cos(yaw), 0.0f, std::sin(yaw)};
        Vec3 to_aim = aim - chest;
        to_aim.y = 0.0f;
        const Vec3 fwd = glm::length(to_aim) > 0.3f ? glm::normalize(to_aim) : facing;
        switch (role) {
            case PlayerRole::Knight:
                if (slot == 0) { // Shield Bash: a steel shockwave punched forward
                    const Vec3 c = chest + fwd * 1.2f;
                    emit(c, Vec3{0.0f}, Vec4{0.9f, 0.95f, 1.0f, 1.0f}, 0.18f, 0.7f, 1);
                    for (int i = 0; i < 26; ++i) {
                        const f32 a = yaw + frand(-0.7f, 0.7f);
                        const Vec3 d{std::cos(a), frand(0.0f, 0.5f), std::sin(a)};
                        emit(c, d * frand(6.0f, 12.0f), Vec4{0.8f, 0.88f, 1.0f, 0.95f}, 0.4f, 0.16f, 1,
                             6.0f);
                    }
                } else if (slot == 1) { // Bulwark: a golden dome flares up
                    emit_ring(feet, Vec4{1.0f, 0.85f, 0.4f, 0.95f}, 22, 3.0f, 0.6f, 0.16f);
                    emit_burst(chest, Vec4{1.0f, 0.82f, 0.35f, 0.9f}, 14, 2.5f, 0.7f, 0.14f, 1, 1.5f);
                } else if (slot == 2) { // Consecration: a holy-fire ring erupts from the ground
                    emit_ring(feet, Vec4{1.0f, 0.72f, 0.28f, 0.95f}, 30, kConsecrationRadius * 1.6f,
                              0.6f, 0.2f);
                    emit_burst(feet + Vec3{0.0f, 0.2f, 0.0f}, Vec4{1.0f, 0.6f, 0.2f, 0.9f}, 22, 2.5f,
                               0.7f, 0.16f, 1, 3.0f);
                } else { // Taunt: a red warcry pulse + upward embers
                    emit_ring(feet, Vec4{1.0f, 0.3f, 0.25f, 0.95f}, 24, 7.0f, 0.5f, 0.18f);
                    emit_burst(chest, Vec4{1.0f, 0.4f, 0.3f, 0.9f}, 14, 3.0f, 0.55f, 0.15f, 1, 3.0f);
                }
                break;
            case PlayerRole::Hunter:
                if (slot == 2) { // Dash: a green backward kick of speed-motes
                    emit_burst(feet + Vec3{0.0f, 0.4f, 0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.9f}, 18,
                               -4.0f, 0.5f, 0.13f, 1);
                    for (int i = 0; i < 12; ++i) {
                        emit(chest - facing * frand(0.0f, 0.8f), -facing * frand(2.0f, 5.0f),
                             Vec4{0.7f, 1.0f, 0.7f, 0.8f}, 0.4f, 0.12f, 1);
                    }
                } else { // Power Shot / Volley: a bright muzzle spray along the shot
                    const Vec3 c = chest + fwd * 0.8f;
                    emit(c, Vec3{0.0f}, Vec4{0.8f, 1.0f, 0.7f, 1.0f}, 0.14f, 0.4f, 1);
                    const f32 spread = slot == 1 ? 0.4f : 0.15f;
                    for (int i = 0; i < (slot == 1 ? 22 : 12); ++i) {
                        const f32 a = std::atan2(fwd.z, fwd.x) + frand(-spread, spread);
                        emit(c, Vec3{std::cos(a), frand(-0.1f, 0.2f), std::sin(a)} * frand(5.0f, 11.0f),
                             Vec4{0.7f, 1.0f, 0.65f, 0.9f}, 0.35f, 0.12f, 1);
                    }
                }
                break;
            case PlayerRole::Cleric:
                if (slot == 1) { // Sanctuary: a wide radiant ring + a column of light
                    emit_ring(feet, Vec4{0.6f, 1.0f, 0.8f, 0.95f}, 30, 6.0f, 0.8f, 0.2f);
                    for (int i = 0; i < 24; ++i) {
                        emit(feet + Vec3{frand(-1.2f, 1.2f), frand(0.0f, 0.3f), frand(-1.2f, 1.2f)},
                             Vec3{0.0f, frand(2.0f, 4.5f), 0.0f}, Vec4{0.7f, 1.0f, 0.85f, 0.9f}, 0.9f,
                             0.14f, 1, -1.0f);
                    }
                } else if (slot == 2) { // Smite: a holy flash punched forward (the bolt travels)
                    const Vec3 c = chest + fwd * 0.8f;
                    emit(c, Vec3{0.0f}, Vec4{0.85f, 1.0f, 0.9f, 1.0f}, 0.2f, 0.6f, 1);
                    emit_burst(c, Vec4{0.7f, 1.0f, 0.85f, 0.95f}, 16, 6.0f, 0.4f, 0.14f, 1);
                } else if (slot == 3) { // Aegis: a cyan ward flares at the caster (sphere is on the target)
                    emit(chest, Vec3{0.0f}, Vec4{0.55f, 0.85f, 1.0f, 1.0f}, 0.2f, 0.5f, 1);
                    emit_ring(feet, Vec4{0.5f, 0.8f, 1.0f, 0.9f}, 18, 3.0f, 0.5f, 0.14f);
                    emit_burst(chest, Vec4{0.6f, 0.9f, 1.0f, 0.9f}, 14, 2.5f, 0.6f, 0.13f, 1, 1.5f);
                } else { // Heal: gentle motes rising around the caster
                    for (int i = 0; i < 18; ++i) {
                        emit(feet + Vec3{frand(-0.5f, 0.5f), frand(0.1f, 0.4f), frand(-0.5f, 0.5f)},
                             Vec3{frand(-0.3f, 0.3f), frand(1.6f, 3.2f), frand(-0.3f, 0.3f)},
                             Vec4{0.55f, 1.0f, 0.7f, 0.9f}, 0.8f, 0.12f, 1, -1.2f);
                    }
                    emit_ring(feet, Vec4{0.55f, 1.0f, 0.7f, 0.8f}, 14, 2.0f, 0.5f, 0.13f);
                }
                break;
        }
    }

    // Decays the local buff auras (emitting trailing motes while active) and plays cast VFX
    // for remote players from the snapshot's `cast` field (deduped by tick so each fires once).
    void update_ability_vfx(Timestep dt) {
        const Vec3 feet = local_feet();
        if (bulwark_fx_ > 0.0f) { // a golden shield dome orbiting the local Knight
            bulwark_fx_ -= dt.seconds;
            const f32 a = elapsed_ * 5.0f;
            for (int i = 0; i < 3; ++i) {
                const f32 ang = a + TwoPi * static_cast<f32>(i) / 3.0f;
                emit(feet + Vec3{std::cos(ang) * 0.9f, 0.9f + 0.4f * std::sin(a * 0.7f), std::sin(ang) * 0.9f},
                     Vec3{0.0f}, Vec4{1.0f, 0.82f, 0.35f, 0.7f}, 0.25f, 0.13f, 1);
            }
        }
        if (dash_fx_ > 0.0f) { // a green speed-trail behind the local Hunter
            dash_fx_ -= dt.seconds;
            emit(feet + Vec3{0.0f, 0.5f, 0.0f}, Vec3{0.0f}, Vec4{0.6f, 1.0f, 0.6f, 0.6f}, 0.3f, 0.12f, 1);
        }
        // Cleric heal channel (right mouse held): mirror the server's charge for a charge bar +
        // gathering VFX; a burst of green motes converges on the staff, and on a full charge it
        // releases (resets) - the actual aura is server-spawned + networked.
        if (role_ == PlayerRole::Cleric && blocking_) {
            heal_charge_fx_ += dt.seconds;
            const Vec3 head = feet + Vec3{0.0f, 1.5f, 0.0f};
            for (int i = 0; i < 3; ++i) {
                const Vec3 from = head + rand_dir() * frand(1.0f, 2.2f);
                emit(from, (head - from) * frand(2.0f, 4.0f), Vec4{0.5f, 1.0f, 0.7f, 0.9f}, 0.4f, 0.1f, 1);
            }
            if (heal_charge_fx_ >= kHealChargeTime) {
                emit_ring(feet, Vec4{0.5f, 1.0f, 0.7f, 0.95f}, 28, 5.0f, 0.7f, 0.18f);
                emit_burst(feet + Vec3{0.0f, 0.3f, 0.0f}, Vec4{0.6f, 1.0f, 0.8f, 0.9f}, 20, 3.0f, 0.8f, 0.14f, 1, 2.0f);
                heal_charge_fx_ = 0.0f;
            }
        } else {
            heal_charge_fx_ = 0.0f;
        }
        if (!have_snapshot_) {
            return;
        }
        for (const net::PlayerState& p : snapshot_.players) {
            if (p.cast == 0 || p.id == my_id_) {
                continue; // local casts are played instantly on keypress
            }
            if (ability_fx_tick_[p.id] == snapshot_.tick) {
                continue; // already played for this snapshot
            }
            ability_fx_tick_[p.id] = snapshot_.tick;
            const Vec3 facing{std::cos(p.yaw), 0.0f, std::sin(p.yaw)};
            spawn_ability_vfx(static_cast<PlayerRole>(p.role % kRoleCount),
                              static_cast<u8>(p.cast - 1), p.position, p.yaw, p.position + facing);
        }
    }

    // A menacing low-poly look shared by all enemies (dark skin, sharp eyes, spiky
    // hair); a red tint at draw time makes them read as hostile.
    static CharacterAppearance enemy_look() {
        CharacterAppearance a;
        a.skin = 0;
        a.hair_color = 0;
        a.eyes = EyeStyle::Sharp;
        a.ears = EarStyle::Pointed;
        a.hair = HairStyle::Spiky;
        return a;
    }

    // Animate enemies from snapshot deltas, like remote players, and drop visuals
    // for enemies that have died / left the snapshot.
    void update_enemy_visuals(Timestep dt) {
        if (!have_snapshot_) {
            return;
        }
        for (const net::EnemyState& en : snapshot_.enemies) {
            const auto [it, created] = enemy_visuals_.try_emplace(en.id);
            EnemyVisual& v = it->second;
            if (created) {
                v.model = CharacterModel::create(en.id ^ 0xE0E0u, enemy_look());
                v.last_pos = en.position;
            }
            f32 measured = 0.0f;
            if (dt.seconds > 0.0001f) {
                Vec3 d = en.position - v.last_pos;
                d.y = 0.0f;
                measured = glm::length(d) / dt.seconds;
            }
            v.speed = glm::mix(v.speed, measured, 0.3f);
            v.last_pos = en.position;
            if (en.action == 1 && v.last_action != 1) {
                v.animator.play_swing(); // the enemy just struck - play the swing
            }
            v.last_action = en.action;
            v.animator.update(v.speed, dt);
        }
        for (auto it = enemy_visuals_.begin(); it != enemy_visuals_.end();) {
            const bool live = std::any_of(snapshot_.enemies.begin(), snapshot_.enemies.end(),
                                          [&](const net::EnemyState& e) { return e.id == it->first; });
            it = live ? std::next(it) : enemy_visuals_.erase(it);
        }
    }

    void draw_enemies() {
        if (!have_snapshot_) {
            return;
        }
        for (const net::EnemyState& en : snapshot_.enemies) {
            const auto it = enemy_visuals_.find(en.id);
            if (it == enemy_visuals_.end()) {
                continue;
            }
            EnemyVisual& v = it->second;
            // 0 = grunt (dark red), 1 = torch-bearer (fiery), 2 = brute (big + dark),
            // 3 = archer (sickly green, carries a bow).
            const f32 scale = en.kind == 2 ? 1.5f : 1.0f;
            const Mat4 root = glm::translate(Mat4{1.0f}, en.position) *
                              glm::rotate(Mat4{1.0f}, HalfPi - en.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                              glm::scale(Mat4{1.0f}, Vec3{scale}) * v.animator.body_offset();
            const Vec3 tint = en.kind == 1   ? Vec3{1.5f, 0.7f, 0.18f}
                              : en.kind == 2 ? Vec3{1.05f, 0.26f, 0.4f}
                              : en.kind == 3 ? Vec3{0.5f, 0.85f, 0.45f}
                                             : Vec3{1.3f, 0.32f, 0.3f};
            const std::vector<Mat4> emats = v.model.bone_matrices(root, v.animator.pose(v.model));
            draw_rig(v.model, emats, tint);
            if (en.kind == 3) {
                // A bow held out front (a curved stave + string).
                const Vec3 hand = en.position +
                                  Vec3{std::cos(en.yaw), 0.0f, std::sin(en.yaw)} * 0.4f +
                                  Vec3{0.0f, 1.0f, 0.0f};
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, hand) *
                                    glm::rotate(Mat4{1.0f}, en.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.04f, 0.85f, 0.04f}),
                                Vec4{0.34f, 0.22f, 0.12f, 1.0f});
            }
            if (en.kind == 1) {
                // The torch: a wooden haft + a flickering emissive flame held aloft, with
                // a warm point of light around it.
                const f32 flick = 0.85f + 0.15f * std::sin(elapsed_ * 13.0f + en.position.x);
                const Vec3 hand = en.position + Vec3{std::cos(en.yaw), 0.0f, std::sin(en.yaw)} * 0.45f +
                                  Vec3{0.0f, 1.15f, 0.0f};
                const Mat4 haft = glm::translate(Mat4{1.0f}, hand - Vec3{0.0f, 0.25f, 0.0f}) *
                                  glm::scale(Mat4{1.0f}, Vec3{0.06f, 0.5f, 0.06f});
                renderer_->draw(shape_box_, haft, Vec4{0.32f, 0.2f, 0.1f, 1.0f});
                const Mat4 flame = glm::translate(Mat4{1.0f}, hand + Vec3{0.0f, 0.18f, 0.0f}) *
                                   glm::scale(Mat4{1.0f}, Vec3{0.18f, 0.3f * flick, 0.18f});
                renderer_->draw_emissive(shape_sphere_, flame, Vec4{1.0f, 0.6f, 0.2f, 1.0f});
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, hand) *
                                         glm::scale(Mat4{1.0f}, Vec3{1.1f}),
                                     Vec4{1.0f, 0.55f, 0.2f, 0.4f * flick});
                Renderer::SpotLight sl;
                sl.position = hand + Vec3{0.0f, 1.5f, 0.0f};
                sl.direction = Vec3{0.0f, -1.0f, 0.0f};
                sl.color = Vec3{1.0f, 0.55f, 0.22f} * (2.4f * flick);
                sl.range = 8.0f;
                const f32 half = glm::radians(70.0f);
                sl.cone_outer_cos = std::cos(half);
                sl.cone_inner_cos = std::cos(half * 0.5f);
                renderer_->add_light(sl);
            }
        }
    }

    // Player-built barricades: a low palisade of wooden stakes + rails, darkening as
    // the enemy hacks it down (health from the snapshot).
    void draw_barricades() {
        if (!have_snapshot_) {
            return;
        }
        for (const net::BarricadeState& b : snapshot_.barricades) {
            const f32 hp = static_cast<f32>(b.health) / 255.0f;
            const Vec3 wood = glm::mix(Vec3{0.22f, 0.14f, 0.08f}, Vec3{0.46f, 0.32f, 0.19f}, hp);
            const Vec4 c{wood, 1.0f};
            const Mat4 m = glm::translate(Mat4{1.0f}, b.position) *
                           glm::rotate(Mat4{1.0f}, b.yaw, Vec3{0.0f, 1.0f, 0.0f});
            auto bar = [&](const Vec3& off, const Vec3& scale) {
                renderer_->draw(shape_box_, m * glm::translate(Mat4{1.0f}, off) *
                                                glm::scale(Mat4{1.0f}, scale),
                                c);
            };
            bar({0.0f, 0.5f, 0.0f}, {2.0f, 0.14f, 0.12f});  // lower rail
            bar({0.0f, 0.9f, 0.0f}, {2.0f, 0.14f, 0.12f});  // upper rail
            for (int i = -2; i <= 2; ++i) {
                const f32 x = static_cast<f32>(i) * 0.45f;
                bar({x, 0.6f, 0.0f}, {0.13f, 1.25f, 0.13f}); // upright stakes
            }
        }
    }

    // Floating health bars above combatants (enemies always; guards always; villagers
    // only when hurt), projected from world space into the 2D UI overlay.
    void draw_health_bars() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        const VkExtent2D ext = renderer_->extent();
        const f32 W = static_cast<f32>(ext.width);
        const f32 H = static_cast<f32>(ext.height);
        const Mat4 vp = camera_.view_projection();
        auto bar = [&](const Vec3& world, f32 frac, const Vec3& col) {
            const Vec4 clip = vp * Vec4{world, 1.0f};
            if (clip.w <= 0.05f) {
                return; // behind the camera
            }
            const Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
            if (std::abs(ndc.x) > 1.15f || std::abs(ndc.y) > 1.15f) {
                return; // off-screen
            }
            const f32 sx = (ndc.x * 0.5f + 0.5f) * W;
            const f32 sy = (ndc.y * 0.5f + 0.5f) * H;
            const f32 bw = glm::clamp(300.0f / clip.w, 16.0f, 48.0f);
            const f32 bh = 4.5f;
            frac = glm::clamp(frac, 0.0f, 1.0f);
            renderer_->draw_ui_rect(Vec4{sx - bw * 0.5f - 1.0f, sy - 1.0f, bw + 2.0f, bh + 2.0f},
                                    Vec4{0.04f, 0.04f, 0.05f, 0.65f}, 1.5f);
            renderer_->draw_ui_rect(Vec4{sx - bw * 0.5f, sy, bw * frac, bh}, Vec4{col, 0.95f}, 1.0f);
        };
        for (const net::EnemyState& en : snapshot_.enemies) {
            const f32 hgt = en.kind == 2 ? 3.0f : 2.2f;
            bar(en.position + Vec3{0.0f, hgt, 0.0f}, static_cast<f32>(en.health) / 255.0f,
                Vec3{0.92f, 0.26f, 0.2f});
        }
        for (const net::VillagerState& vl : snapshot_.villagers) {
            if (vl.kind == 0 && vl.health >= 250) {
                continue; // hide bars over healthy villagers (only show the hurt)
            }
            const Vec3 col = vl.kind == 1 ? Vec3{0.45f, 0.72f, 0.96f} : Vec3{0.42f, 0.86f, 0.42f};
            bar(vl.position + Vec3{0.0f, 2.15f, 0.0f}, static_cast<f32>(vl.health) / 255.0f, col);
        }
    }

    // Burning houses: a cluster of flickering emissive flame tongues that engulf the
    // whole cottage (spread across its footprint, licking up to the roof), dark smoke
    // billowing above, an additive firelight bloom and a strong warm light (day or
    // night). The server sends position + intensity; a low intensity is the smouldering
    // ember of a burnt-down ruin (small flames, lots of smoke).
    void draw_fires() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        // A cheap deterministic [0,1) hash for per-tongue variation.
        auto frac = [](f32 x) { return x - std::floor(x); };
        const f32 night = 1.0f - sun_intensity_;
        for (const net::FireState& fs : snapshot_.fires) {
            const f32 in = static_cast<f32>(fs.intensity) / 255.0f;
            const f32 ember = in < 0.35f ? 1.0f : 0.0f; // burnt-out ruin?
            const f32 vigour = glm::clamp(in, 0.25f, 1.0f);
            const f32 ph = fs.position.x * 1.3f + fs.position.z * 0.7f; // per-fire phase
            const Vec3 base = fs.position;

            // Flame tongues spread over the ~6x5.5m footprint, rising toward the roof.
            const int tongues = ember > 0.0f ? 5 : 13;
            for (int i = 0; i < tongues; ++i) {
                const f32 fi = static_cast<f32>(i);
                const f32 a = frac(fi * 0.61f) * TwoPi + ph;
                const f32 spread = ember > 0.0f ? 1.0f : 2.0f;
                const f32 ox = std::cos(a) * (0.3f + frac(fi * 0.37f) * spread);
                const f32 oz = std::sin(a) * (0.3f + frac(fi * 0.53f) * spread * 0.9f);
                const f32 flick = std::sin(elapsed_ * (5.0f + fi * 0.7f) + ph + fi);
                const f32 grow = ember > 0.0f ? 0.55f : (1.4f + frac(fi * 0.29f) * 1.9f);
                const f32 height = grow * vigour * (0.82f + 0.18f * flick);
                const f32 width = (0.32f + frac(fi * 0.41f) * 0.32f) * vigour;
                const Vec3 col = glm::mix(Vec3{1.0f, 0.42f, 0.08f}, Vec3{1.0f, 0.88f, 0.35f},
                                          frac(fi * 0.71f) * 0.7f);
                const Mat4 m = glm::translate(Mat4{1.0f}, base + Vec3{ox + 0.12f * flick,
                                                                      height * 0.5f, oz}) *
                               glm::scale(Mat4{1.0f}, Vec3{width, height, width});
                renderer_->draw_emissive(shape_sphere_, m, Vec4{col, 1.0f});
            }

            // Smoke: dark puffs rising and fattening as they climb.
            const int puffs = 4;
            for (int i = 0; i < puffs; ++i) {
                const f32 t = frac(elapsed_ * 0.16f + static_cast<f32>(i) / puffs + ph);
                const f32 yy = 3.2f + t * 5.0f;
                const f32 ss = (0.9f + t * 2.2f) * (0.6f + 0.4f * vigour);
                const f32 alpha = (1.0f - t) * (ember > 0.0f ? 0.5f : 0.38f);
                const Mat4 m = glm::translate(Mat4{1.0f},
                                              base + Vec3{0.7f * std::sin(elapsed_ * 0.6f + i),
                                                          yy, 0.5f * std::cos(elapsed_ * 0.5f + i)}) *
                               glm::scale(Mat4{1.0f}, Vec3{ss});
                renderer_->draw_transparent(shape_sphere_, m, Vec4{0.09f, 0.08f, 0.08f, alpha});
            }

            // Additive firelight bloom.
            const f32 bloom = 0.6f * vigour * (0.85f + 0.15f * std::sin(elapsed_ * 9.0f + ph));
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, base + Vec3{0.0f, 1.6f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{3.6f * vigour}),
                                 Vec4{1.0f, 0.5f, 0.18f, bloom});

            // A strong warm light pooling around the blaze (brightest at night).
            if (in > 0.15f) {
                Renderer::SpotLight sl;
                sl.position = base + Vec3{0.0f, 4.5f, 0.0f};
                sl.direction = Vec3{0.0f, -1.0f, 0.0f};
                sl.color = Vec3{1.0f, 0.5f, 0.2f} * ((0.6f + 1.4f * night) * vigour);
                sl.range = 15.0f;
                const f32 half = glm::radians(62.0f);
                sl.cone_outer_cos = std::cos(half);
                sl.cone_inner_cos = std::cos(half * 0.6f);
                renderer_->add_light(sl);
            }
        }
    }

    // The burning intensity (0..1) of the house at world position `p`, from the
    // server's fire list - used to char the cottage and swap its cosy glow for flames.
    f32 house_burn(const Vec3& p) const {
        if (!have_snapshot_) {
            return 0.0f;
        }
        f32 burn = 0.0f;
        for (const net::FireState& fs : snapshot_.fires) {
            if (glm::length(Vec2{fs.position.x - p.x, fs.position.z - p.z}) < 2.5f) {
                burn = std::max(burn, static_cast<f32>(fs.intensity) / 255.0f);
            }
        }
        return burn;
    }

    // The cart's terrain-following orientation + bob. It simply sits ON the ground: pitched
    // along its travel direction and rolled across it to match the slope under the wheels (no
    // tilt/flip dynamics - any drama is the physical cargo sliding around). Bob is a speed jiggle.
    void wagon_orient(const net::WagonState& wg, f32 moved, f32& pitch, f32& roll, f32& bob) const {
        const f32 cy = std::cos(wg.yaw);
        const f32 sy = std::sin(wg.yaw);
        const Vec2 fwd{cy, sy};  // world xz the cart faces (local +X under rotate(-yaw))
        const Vec2 lat{-sy, cy}; // world xz across the cart (local +Z)
        constexpr f32 d = 0.75f;
        auto height = [&](const Vec2& o) {
            return worldgen::height(wg.position.x + o.x, wg.position.z + o.y, world_seed_);
        };
        const f32 slope_f = (height(fwd * d) - height(fwd * -d)) / (2.0f * d);
        const f32 slope_l = (height(lat * d) - height(lat * -d)) / (2.0f * d);
        pitch = glm::clamp(std::atan(slope_f), -0.6f, 0.6f); // nose up going uphill
        roll = glm::clamp(-std::atan(slope_l), -0.6f, 0.6f); // lean across the slope
        bob = std::sin(elapsed_ * 11.0f + wg.position.x * 0.7f) * 0.04f *
              glm::clamp(moved * 12.0f, 0.0f, 1.0f);
    }

    Mat4 wagon_model(const net::WagonState& wg, f32 moved) const {
        // The wagon mesh faces local +X. The server yaw has forward = (cos, sin) in world
        // xz, and glm::rotate(-yaw) maps local +X exactly there - so the tongue points the
        // way the cart travels (toward its puller).
        f32 pitch, roll, bob;
        wagon_orient(wg, moved, pitch, roll, bob);
        return glm::translate(Mat4{1.0f}, Vec3{wg.position.x, wg.position.y + bob, wg.position.z}) *
               glm::rotate(Mat4{1.0f}, -wg.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
               glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
               glm::rotate(Mat4{1.0f}, roll, Vec3{1.0f, 0.0f, 0.0f});
    }

    // Re-applies the cart's lean + bob to a rider's flat (server) seat position, so a seated
    // player/driver rides with the cart instead of floating above its tilt.
    Vec3 attach_to_wagon(const net::WagonState& wg, const Vec3& flat_world) const {
        f32 pitch, roll, bob;
        wagon_orient(wg, wagon_frame_move(wg), pitch, roll, bob);
        const Mat4 tilt = glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
                          glm::rotate(Mat4{1.0f}, roll, Vec3{1.0f, 0.0f, 0.0f});
        const Vec3 off = Vec3{tilt * Vec4{flat_world - wg.position, 0.0f}};
        return wg.position + off + Vec3{0.0f, bob, 0.0f};
    }

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
    void draw_wagons() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        for (const net::WagonState& wg : snapshot_.wagons) {
            const f32 hp = static_cast<f32>(wg.health) / 255.0f;
            Vec3 tint{1.0f};
            if (wg.mode == static_cast<u8>(WagonMode::Parked) && wg.id == selected_wagon_) {
                tint = Vec3{1.4f, 1.25f, 0.7f}; // your pick glows
            }
            tint = glm::mix(Vec3{0.35f, 0.28f, 0.22f}, tint, glm::clamp(hp, 0.25f, 1.0f));

            const VehicleType& vt = vehicle_type(wg.type);

            // Spin the wheels at their true rolling speed: with no slip the wheel turns
            // (ground distance / wheel radius) radians. Sign it by the travel direction so a
            // reversing carriage rolls its wheels backward.
            f32& roll = wagon_roll_[wg.id];
            f32 moved = 0.0f;
            const auto prev = wagon_prev_.find(wg.id);
            if (prev != wagon_prev_.end()) {
                const Vec2 delta{wg.position.x - prev->second.x, wg.position.z - prev->second.z};
                moved = glm::length(delta);
                const Vec2 fwd{std::cos(wg.yaw), std::sin(wg.yaw)};
                roll -= glm::dot(delta, fwd) / vt.wheel_radius(); // signed distance / radius
            }
            wagon_prev_[wg.id] = wg.position;

            const Mat4 m = wagon_model(wg, moved);
            renderer_->draw(vehicle_meshes_[wg.type % vehicle_meshes_.size()], m, Vec4{tint, 1.0f});

            // Wheels (scaled to the type's radius), spun by the accumulated roll.
            const f32 wscale = vt.wheel_radius() / kWagonWheelRadius;
            for (const Vec3& off : vt.wheels()) {
                const Mat4 wm = m * glm::translate(Mat4{1.0f}, off) *
                                glm::rotate(Mat4{1.0f}, roll, Vec3{0.0f, 0.0f, 1.0f}) *
                                glm::scale(Mat4{1.0f}, Vec3{wscale});
                renderer_->draw(wagon_wheel_mesh_, wm, Vec4{tint, 1.0f});
            }

            // The lamp: an emissive glow always, plus a warm light at night when near.
            const Vec3 lampw = Vec3{m * Vec4{vt.lamp(), 1.0f}};
            const f32 night = 1.0f - sun_intensity_;
            const f32 glow = glm::mix(1.0f, 0.5f, sun_intensity_);
            renderer_->draw_emissive(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, lampw) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.16f}),
                                     Vec4{glow, glow * 0.82f, glow * 0.5f, 1.0f});
            const bool is_active_cargo =
                snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active);
            if (night > 0.1f && glm::length(lampw - local_feet()) < 40.0f) {
                Renderer::SpotLight sl;
                sl.position = lampw + Vec3{0.0f, 0.6f, 0.0f}; // lift it so shadows rake outward
                sl.direction = Vec3{0.0f, -1.0f, 0.0f};
                sl.color = Vec3{1.0f, 0.82f, 0.5f} * (1.7f * night);
                sl.range = 18.0f;
                sl.cone_outer_cos = std::cos(glm::radians(74.0f));
                sl.cone_inner_cos = std::cos(glm::radians(40.0f));
                // The ACTIVE escorted cargo is the scene's KEY light: a top-priority real
                // shadow-caster (the most expensive light), throwing crisp shadows of the player
                // + nearby props/enemies as it rolls. Parked offers are cheap unshadowed lamps.
                sl.cast_shadow = is_active_cargo;
                sl.priority = is_active_cargo;
                renderer_->add_light(sl);
            }

            // The pulling horse (carriages), with a simple walking leg gait + harness ropes.
            if (wg.has_horse) {
                draw_horse(wg.horse_pos, wg.horse_yaw);
                draw_ropes(wg.id);
            }
        }
    }

    // Cargo crates: in-bed ones (loose==0) ride in the cart (their position is bed-local, so we
    // place them through the cart transform - they slide + tilt + bob with it); fallen ones
    // (loose==1) lie on the ground at a world position until picked up (E).
    void draw_goods() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        const net::WagonState* aw = active_wagon();
        const Mat4 cart = aw != nullptr ? wagon_model(*aw, wagon_frame_move(*aw)) : Mat4{1.0f};
        for (const net::GoodState& g : snapshot_.goods) {
            Mat4 m;
            if (g.loose == 0 && aw != nullptr) {
                // Bed-local position -> world via the cart transform (centred crate on the floor).
                m = cart * glm::translate(Mat4{1.0f}, g.position + Vec3{0.0f, kCargoHalf, 0.0f});
            } else {
                // A slight per-crate lean so fallen goods look tumbled rather than neatly placed.
                m = glm::translate(Mat4{1.0f}, g.position) *
                    glm::rotate(Mat4{1.0f}, static_cast<f32>(g.id) * 1.3f, Vec3{0.0f, 1.0f, 0.0f}) *
                    glm::rotate(Mat4{1.0f}, 0.18f, Vec3{1.0f, 0.0f, 0.0f});
            }
            renderer_->draw(goods_mesh_, m);
        }
    }

    // A crate held in front of a player who is carrying a spilled good back to the cart.
    void draw_carried_good(const Vec3& feet, f32 yaw) {
        const Vec3 fwd{std::cos(yaw), 0.0f, std::sin(yaw)};
        const Vec3 pos = feet + Vec3{0.0f, 0.85f, 0.0f} + fwd * 0.35f;
        const Mat4 m = glm::translate(Mat4{1.0f}, pos) *
                       glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f});
        renderer_->draw(goods_mesh_, m);
    }

    // Renders a wagon's two verlet harness traces as a chain of short oriented links (the
    // node positions are simulated in update_ropes from the authoritative endpoints).
    void draw_ropes(u32 id) {
        const auto it = wagon_ropes_.find(id);
        if (it == wagon_ropes_.end()) {
            return;
        }
        constexpr f32 thick = 0.04f;
        const Vec4 col{0.16f, 0.11f, 0.06f, 1.0f};
        for (const RopeTrace& r : it->second) {
            if (!r.init) {
                continue;
            }
            for (int i = 0; i < kRopeNodes - 1; ++i) {
                const Vec3 a = r.pos[i];
                const Vec3 d = r.pos[i + 1] - a;
                const f32 L = glm::length(d);
                if (L < 1e-4f) {
                    continue;
                }
                // Build a basis whose +Y maps along the link (the rope mesh runs y = 0..1).
                const Vec3 up = d / L;
                const Vec3 ref = std::abs(up.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
                const Vec3 bx = glm::normalize(glm::cross(ref, up));
                const Vec3 bz = glm::cross(up, bx);
                Mat4 m{1.0f};
                m[0] = Vec4{bx * thick, 0.0f};
                m[1] = Vec4{up * L, 0.0f};
                m[2] = Vec4{bz * thick, 0.0f};
                m[3] = Vec4{a, 1.0f};
                renderer_->draw(rope_mesh_, m, col);
            }
        }
    }

    // Simulates the harness traces: two ropes (left/right) per horse-drawn wagon, each a
    // verlet chain pinned to the carriage shaft tip and the horse's collar, sagging under
    // gravity and swinging as the rig moves - so the rope is real physics, not a fixed line.
    void update_ropes(Timestep dt) {
        if (!have_snapshot_) {
            wagon_ropes_.clear();
            return;
        }
        const f32 h = std::min(dt.seconds, 1.0f / 30.0f); // clamp the step for stability
        auto to_world = [](const Vec3& center, f32 yaw, const Vec3& local) {
            return center + Vec3{glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                 Vec4{local, 0.0f}};
        };
        for (const net::WagonState& wg : snapshot_.wagons) {
            if (!wg.has_horse) {
                continue;
            }
            std::array<RopeTrace, 2>& ropes = wagon_ropes_[wg.id];
            for (int side = 0; side < 2; ++side) {
                const f32 s = side == 0 ? 1.0f : -1.0f;
                const Vec3 A = to_world(wg.position, wg.yaw, Vec3{2.4f, 0.75f, 0.12f * s}); // shaft tip
                const Vec3 B = to_world(wg.horse_pos, wg.horse_yaw, Vec3{0.45f, 1.05f, 0.18f * s}); // collar
                RopeTrace& r = ropes[side];
                if (!r.init) {
                    for (int i = 0; i < kRopeNodes; ++i) {
                        const f32 t = static_cast<f32>(i) / static_cast<f32>(kRopeNodes - 1);
                        r.pos[i] = glm::mix(A, B, t);
                        r.prev[i] = r.pos[i];
                    }
                    r.init = true;
                }
                r.pos[0] = A; // pin the ends to the live attachment points
                r.prev[0] = A;
                r.pos[kRopeNodes - 1] = B;
                r.prev[kRopeNodes - 1] = B;
                for (int i = 1; i < kRopeNodes - 1; ++i) { // verlet: gravity + damping
                    const Vec3 cur = r.pos[i];
                    const Vec3 vel = (r.pos[i] - r.prev[i]) * 0.94f;
                    r.pos[i] = cur + vel + Vec3{0.0f, -12.0f, 0.0f} * (h * h);
                    r.prev[i] = cur;
                }
                const f32 seg = glm::length(B - A) * 1.08f / static_cast<f32>(kRopeNodes - 1);
                for (int it = 0; it < 10; ++it) { // satisfy link lengths (a touch slack = sag)
                    for (int i = 0; i < kRopeNodes - 1; ++i) {
                        const Vec3 d = r.pos[i + 1] - r.pos[i];
                        const f32 l = glm::length(d);
                        if (l < 1e-5f) {
                            continue;
                        }
                        const Vec3 corr = d * ((l - seg) / l);
                        const bool af = i != 0;
                        const bool bf = i + 1 != kRopeNodes - 1;
                        if (af && bf) {
                            r.pos[i] += corr * 0.5f;
                            r.pos[i + 1] -= corr * 0.5f;
                        } else if (af) {
                            r.pos[i] += corr;
                        } else if (bf) {
                            r.pos[i + 1] -= corr;
                        }
                    }
                }
            }
        }
        // Cull ropes whose wagon is gone / no longer horse-drawn.
        for (auto it = wagon_ropes_.begin(); it != wagon_ropes_.end();) {
            const bool live = std::any_of(snapshot_.wagons.begin(), snapshot_.wagons.end(),
                                          [&](const net::WagonState& w) {
                                              return w.id == it->first && w.has_horse != 0;
                                          });
            it = live ? std::next(it) : wagon_ropes_.erase(it);
        }
    }

    // Draws the carriage's horse with a simple diagonal leg gait driven by its motion.
    void draw_horse(const Vec3& pos, f32 yaw) {
        const f32 moved = glm::length(Vec2{pos.x - horse_prev_.x, pos.z - horse_prev_.z});
        horse_gait_ += moved * 2.2f;
        horse_prev_ = pos;
        const Mat4 base = glm::translate(Mat4{1.0f}, pos) *
                          glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f});
        renderer_->draw(horse_body_mesh_, base);
        for (int k = 0; k < 4; ++k) {
            const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f; // diagonal pairs
            const f32 swing = std::sin(horse_gait_) * 0.5f * sign;
            const Mat4 lm = base * glm::translate(Mat4{1.0f}, kHorseLegs[k]) *
                            glm::rotate(Mat4{1.0f}, swing, Vec3{0.0f, 0.0f, 1.0f});
            renderer_->draw(horse_leg_mesh_, lm);
        }
    }

    // Project a world point to screen pixels; returns false if behind/off camera.
    bool world_to_screen(const Vec3& world, f32 W, f32 H, Vec2& out) const {
        const Vec4 clip = camera_.view_projection() * Vec4{world, 1.0f};
        if (clip.w <= 0.05f) {
            return false;
        }
        const Vec2 ndc{clip.x / clip.w, clip.y / clip.w};
        out = Vec2{(ndc.x * 0.5f + 0.5f) * W, (ndc.y * 0.5f + 0.5f) * H};
        return std::abs(ndc.x) < 1.3f && std::abs(ndc.y) < 1.3f;
    }

    // The in-game HUD: shared party money, the wagon-contract objective (choose an offer,
    // or the active delivery + a destination arrow), and the local player's health bar.
    void draw_hud() {
        if (renderer_ == nullptr || !have_snapshot_) {
            return;
        }
        f32 hp = 1.0f;
        for (const net::PlayerState& p : snapshot_.players) {
            if (p.id == my_id_) {
                hp = static_cast<f32>(p.health) / 100.0f;
                break;
            }
        }
        const VkExtent2D ext = renderer_->extent();
        const f32 W = static_cast<f32>(ext.width);
        const f32 H = static_cast<f32>(ext.height);
        ui::DrawList draw{*renderer_};
        const f32 ts = glm::clamp(H * 0.026f, 15.0f, 30.0f);
        const Vec3 feet = local_feet();
        const u8 phase = snapshot_.contract_phase;

        // Shared party money, top-right.
        const std::string money = std::format("$ {}", snapshot_.money);
        draw.text(Vec2{W - draw.text_width(money, ts) - 24.0f, 22.0f}, money, ts,
                  Vec4{0.96f, 0.86f, 0.4f, 1.0f});

        if (phase == static_cast<u8>(ContractPhase::Offer)) {
            draw.text(Vec2{24.0f, 22.0f}, "WALK UP TO A WAGON TO VIEW ITS CONTRACT", ts,
                      Vec4{0.94f, 0.86f, 0.58f, 1.0f});
            // A small floating tag over every offered wagon so you can see where they are and
            // pick which to walk to (gold $ reward; the one you're next to is highlighted).
            for (const net::WagonState& wg : snapshot_.wagons) {
                if (wg.id == selected_wagon_ || wg.id == near_wagon_) {
                    continue; // the focused wagon gets the full panel instead
                }
                Vec2 sp;
                if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
                    const std::string tag = std::format("$ {}", wg.reward);
                    draw.text(Vec2{sp.x - draw.text_width(tag, ts * 0.7f) * 0.5f, sp.y}, tag, ts * 0.7f,
                              Vec4{0.9f, 0.82f, 0.45f, 0.95f});
                }
            }
            // The full contract panel for the focused wagon (the accepted one, else the one in range).
            accept_btn_ = cancel_btn_ = ui::Rect{};
            panel_wagon_ = selected_wagon_ != 0 ? selected_wagon_ : near_wagon_;
            if (const net::WagonState* wg = wagon_by_id(panel_wagon_)) {
                draw_contract_panel(draw, *wg, selected_wagon_ == panel_wagon_, W, H, ts, feet);
            }
        } else if (phase == static_cast<u8>(ContractPhase::Active) && !snapshot_.wagons.empty()) {
            const net::WagonState& wg = snapshot_.wagons.front();
            const VehicleType& vt = vehicle_type(wg.type);
            const f32 dist = glm::length(Vec2{wg.dest.x - feet.x, wg.dest.z - feet.z});
            const bool manual = wg.mode == static_cast<u8>(WagonMode::Manual);
            std::string title = vt.name();
            for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            draw.text(Vec2{24.0f, 22.0f},
                      std::format("DELIVER THE {}   $ {}{}   ~{}m", title, wg.reward,
                                  manual ? " (manual)" : "", static_cast<int>(dist)),
                      ts, Vec4{0.94f, 0.86f, 0.58f, 1.0f});
            // Cargo load (pay scales with the share delivered).
            const bool short_load = wg.goods_aboard < wg.goods_total;
            draw.text(Vec2{24.0f, 22.0f + ts * 1.5f},
                      std::format("GOODS {}/{}", wg.goods_aboard, wg.goods_total), ts * 0.72f,
                      short_load ? Vec4{0.95f, 0.7f, 0.35f, 1.0f} : Vec4{0.66f, 0.78f, 0.7f, 1.0f});
            // Wagon health bar.
            const f32 wf = static_cast<f32>(wg.health) / 255.0f;
            draw.rect(Vec4{24.0f, 22.0f + ts * 2.6f, 220.0f, 12.0f}, Vec4{0.05f, 0.05f, 0.07f, 0.7f},
                      3.0f);
            draw.rect(Vec4{24.0f, 22.0f + ts * 2.6f, 220.0f * std::max(wf, 0.0f), 12.0f},
                      Vec4{glm::mix(Vec3{0.8f, 0.3f, 0.2f}, Vec3{0.5f, 0.7f, 0.4f}, wf), 0.95f}, 3.0f);
            // Contextual E hint: righting a flipped cart / handling goods take priority.
            bool carrying = false;
            for (const net::PlayerState& pp : snapshot_.players) {
                if (pp.id == my_id_) {
                    carrying = pp.carrying != 0;
                }
            }
            bool near_good = false;
            for (const net::GoodState& g : snapshot_.goods) {
                if (g.loose != 0 && glm::length(g.position - feet) < kGoodPickupRange + 0.5f) {
                    near_good = true;
                }
            }
            std::string hint;
            if (carrying) {
                hint = "[E] load the crate into the cart";
            } else if (near_good) {
                hint = "[E] pick up the fallen crate";
            } else if (!manual) {
                hint = "[E] ride on the wagon";
            } else if (vt.horse_drawn()) {
                hint = "[E] drive (W/S throttle, A/D rein) / ride";
            } else {
                hint = "[E] haul / ride the wagon";
            }
            draw.text(Vec2{24.0f, 22.0f + ts * 4.0f}, hint, ts * 0.72f, Vec4{0.66f, 0.78f, 0.7f, 1.0f});
            // Warn when crates have bounced out and aren't recovered (pay is dropping).
            if (short_load) {
                const char* warn = "CARGO SPILLING - RECOVER THE CRATES";
                draw.text(Vec2{(W - draw.text_width(warn, ts * 0.9f)) * 0.5f, H * 0.3f}, warn,
                          ts * 0.9f, Vec4{0.95f, 0.6f, 0.3f, 1.0f});
            }
            draw_dest_arrow(draw, feet, Vec3{wg.dest.x, feet.y, wg.dest.z}, W);
        }

        // Settle banner.
        if (snapshot_.contract_outcome != 0) {
            const bool ok = snapshot_.contract_outcome == 1;
            const char* msg = ok ? "WAGON DELIVERED" : "WAGON LOST";
            const Vec4 c = ok ? Vec4{0.55f, 0.9f, 0.55f, 1.0f} : Vec4{0.95f, 0.4f, 0.36f, 1.0f};
            const f32 bs = glm::clamp(H * 0.06f, 28.0f, 76.0f);
            draw.text(Vec2{(W - draw.text_width(msg, bs)) * 0.5f, H * 0.34f}, msg, bs, c);
        }

        draw.text(Vec2{24.0f, H - 52.0f}, "[M] MAP", ts * 0.72f, Vec4{0.72f, 0.80f, 0.88f, 1.0f});

        // Health bar.
        const f32 bw = std::min(320.0f, W * 0.28f);
        const f32 bh = 18.0f;
        const f32 x = 24.0f;
        const f32 y = H - 24.0f - bh;
        draw.rect(Vec4{x - 3.0f, y - 3.0f, bw + 6.0f, bh + 6.0f}, Vec4{0.05f, 0.05f, 0.07f, 0.7f},
                  5.0f);
        const Vec3 col = glm::mix(Vec3{0.85f, 0.2f, 0.18f}, Vec3{0.35f, 0.8f, 0.35f}, hp);
        draw.rect(Vec4{x, y, bw * std::max(hp, 0.0f), bh}, Vec4{col, 0.95f}, 4.0f);
        // Role label above the health bar.
        draw.text(Vec2{x, y - ts * 0.95f}, role_name(role_), ts * 0.72f,
                  Vec4{0.74f, 0.82f, 0.92f, 1.0f});

        // Cleric heal-channel charge bar (centre screen while charging the AOE heal).
        if (role_ == PlayerRole::Cleric && heal_charge_fx_ > 0.001f) {
            const f32 frac = glm::clamp(heal_charge_fx_ / kHealChargeTime, 0.0f, 1.0f);
            const f32 cw = std::min(360.0f, W * 0.32f);
            const f32 cx = (W - cw) * 0.5f;
            const f32 cy = H * 0.62f;
            draw.rect(Vec4{cx - 3.0f, cy - 3.0f, cw + 6.0f, 18.0f + 6.0f},
                      Vec4{0.04f, 0.06f, 0.05f, 0.8f}, 5.0f);
            draw.rect(Vec4{cx, cy, cw * frac, 18.0f}, Vec4{0.45f, 1.0f, 0.7f, 0.95f}, 4.0f);
            draw.text(Vec2{cx, cy - ts * 0.95f}, "CHANNELLING HEAL...", ts * 0.7f,
                      Vec4{0.7f, 1.0f, 0.85f, 1.0f});
        }

        draw_ability_bar(draw, W, H, ts);
    }

    // The floating contract panel shown beside a wagon you've walked up to: where it's bound
    // (town name), how far, the danger, and the pay - plus ACCEPT / CANCEL buttons (or, once
    // accepted, a WAITING tally + CANCEL). Stores the button rects for click hit-testing.
    void draw_contract_panel(ui::DrawList& draw, const net::WagonState& wg, bool accepted, f32 W,
                             f32 H, f32 ts, const Vec3& feet) {
        const f32 pw = glm::clamp(W * 0.24f, 280.0f, 380.0f);
        const f32 ph = ts * 9.6f;
        // Anchor beside the wagon on screen; clamp on-screen, fall back to centre if off-camera.
        Vec2 sp;
        Vec2 anchor{(W - pw) * 0.5f, H * 0.26f};
        if (world_to_screen(wg.position + Vec3{0.0f, 2.6f, 0.0f}, W, H, sp)) {
            anchor = Vec2{sp.x - pw * 0.5f, sp.y - ph - 12.0f};
        }
        anchor.x = glm::clamp(anchor.x, 12.0f, W - pw - 12.0f);
        anchor.y = glm::clamp(anchor.y, 12.0f, H - ph - 120.0f);
        const f32 px = anchor.x, py = anchor.y;

        const Vec3 accent{0.96f, 0.86f, 0.45f};
        draw.rect(Vec4{px, py, pw, ph}, Vec4{0.05f, 0.06f, 0.09f, 0.92f}, Vec4{accent, 0.85f}, 2.0f,
                  10.0f);
        const f32 ix = px + 18.0f;
        f32 iy = py + 16.0f;

        // Heading: bound-for town name.
        draw.text(Vec2{ix, iy}, "CARGO CONTRACT", ts * 0.62f, Vec4{0.7f, 0.75f, 0.82f, 1.0f});
        iy += ts * 1.2f;
        draw.text(Vec2{ix, iy}, std::format("TO {}", town_name(Vec3{wg.dest.x, 0.0f, wg.dest.z})),
                  ts * 1.05f, Vec4{0.98f, 0.92f, 0.7f, 1.0f});
        iy += ts * 1.7f;

        // Distance + danger + pay.
        const f32 dist = glm::length(Vec2{wg.dest.x - feet.x, wg.dest.z - feet.z});
        draw.text(Vec2{ix, iy}, std::format("DISTANCE   ~{} m", static_cast<int>(dist)), ts * 0.8f,
                  Vec4{0.82f, 0.85f, 0.9f, 1.0f});
        iy += ts * 1.25f;
        const char* danger = wg.difficulty <= 1 ? "LOW" : wg.difficulty == 2 ? "MODERATE" : "HIGH";
        const Vec4 dcol = wg.difficulty <= 1 ? Vec4{0.55f, 0.85f, 0.55f, 1.0f}
                          : wg.difficulty == 2 ? Vec4{0.95f, 0.8f, 0.4f, 1.0f}
                                               : Vec4{0.95f, 0.45f, 0.4f, 1.0f};
        draw.text(Vec2{ix, iy}, std::format("DANGER     {} {}", danger,
                                            std::string(wg.difficulty, '*')),
                  ts * 0.8f, dcol);
        iy += ts * 1.25f;
        draw.text(Vec2{ix, iy}, std::format("PAY        $ {}", wg.reward), ts * 0.9f,
                  Vec4{0.98f, 0.88f, 0.42f, 1.0f});

        // Mode hint (toggled with H).
        const char* mode = vote_mode_ == 2 ? "HAUL MANUALLY (+pay)" : "HIRE A DRIVER";
        draw.text(Vec2{ix, py + ph - ts * 2.9f}, std::format("[H] {}", mode), ts * 0.66f,
                  Vec4{0.66f, 0.78f, 0.7f, 1.0f});

        // Buttons.
        const f32 bw = (pw - 18.0f * 2.0f - 12.0f) * 0.5f;
        const f32 bh = ts * 1.7f;
        const f32 by = py + ph - bh - 14.0f;
        if (accepted) {
            const int total = static_cast<int>(snapshot_.players.size());
            draw.rect(Vec4{ix, by, bw, bh}, Vec4{0.12f, 0.16f, 0.13f, 0.95f}, 6.0f);
            draw.text(Vec2{ix + 10.0f, by + bh * 0.5f - ts * 0.34f},
                      std::format("WAITING {}/{}", wg.votes, total), ts * 0.62f,
                      Vec4{0.7f, 0.9f, 0.7f, 1.0f});
            cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
            draw.rect(Vec4{cancel_btn_.x, cancel_btn_.y, bw, bh}, Vec4{0.22f, 0.12f, 0.12f, 0.95f},
                      Vec4{0.8f, 0.4f, 0.4f, 0.8f}, 1.5f, 6.0f);
            draw.text(Vec2{cancel_btn_.x + bw * 0.5f - draw.text_width("CANCEL", ts * 0.7f) * 0.5f,
                           by + bh * 0.5f - ts * 0.36f},
                      "CANCEL", ts * 0.7f, Vec4{0.95f, 0.8f, 0.8f, 1.0f});
        } else {
            accept_btn_ = ui::Rect{ix, by, bw, bh};
            draw.rect(Vec4{accept_btn_.x, accept_btn_.y, bw, bh}, Vec4{0.14f, 0.3f, 0.16f, 0.97f},
                      Vec4{0.5f, 0.9f, 0.5f, 0.95f}, 2.0f, 6.0f);
            draw.text(Vec2{accept_btn_.x + bw * 0.5f - draw.text_width("ACCEPT", ts * 0.78f) * 0.5f,
                           by + bh * 0.5f - ts * 0.4f},
                      "ACCEPT", ts * 0.78f, Vec4{0.85f, 1.0f, 0.85f, 1.0f});
            cancel_btn_ = ui::Rect{ix + bw + 12.0f, by, bw, bh};
            draw.rect(Vec4{cancel_btn_.x, cancel_btn_.y, bw, bh}, Vec4{0.16f, 0.17f, 0.2f, 0.95f},
                      Vec4{0.6f, 0.62f, 0.66f, 0.8f}, 1.5f, 6.0f);
            draw.text(Vec2{cancel_btn_.x + bw * 0.5f - draw.text_width("CANCEL", ts * 0.7f) * 0.5f,
                           by + bh * 0.5f - ts * 0.36f},
                      "CANCEL", ts * 0.7f, Vec4{0.85f, 0.87f, 0.9f, 1.0f});
        }
    }

    // The role's signature accent colour (also tints the ability bar + icons).
    static Vec3 role_color(PlayerRole role) {
        switch (role) {
            case PlayerRole::Knight: return Vec3{0.56f, 0.7f, 0.96f};  // steel blue
            case PlayerRole::Hunter: return Vec3{0.55f, 0.92f, 0.55f}; // forest green
            case PlayerRole::Cleric: return Vec3{0.97f, 0.86f, 0.46f}; // holy gold
        }
        return Vec3{1.0f};
    }

    // The three role abilities (keys 1/2/3) as a polished bottom-centre bar: a backing
    // panel, one rounded slot each with a vector icon, a key badge, the name, and a radial
    // cooldown wipe (a dark overlay that drains as the ability recovers + the seconds left).
    void draw_ability_bar(ui::DrawList& draw, f32 W, f32 H, f32 ts) {
        const Vec3 accent = role_color(role_);
        const f32 slot = glm::clamp(H * 0.092f, 56.0f, 84.0f);
        const f32 gap = slot * 0.16f;
        const f32 pad = slot * 0.16f;
        const f32 total = slot * kAbilitySlots + gap * (kAbilitySlots - 1);
        const f32 sy = H - slot - pad - 26.0f;
        const f32 x0 = (W - total) * 0.5f;

        // Backing panel.
        draw.rect(Vec4{x0 - pad, sy - pad, total + pad * 2.0f, slot + pad * 2.0f},
                  Vec4{0.04f, 0.05f, 0.07f, 0.72f}, 10.0f);

        f32 sx = x0;
        for (u8 i = 0; i < kAbilitySlots; ++i) {
            const AbilityDef ab = ability_def(role_, i);
            const f32 frac =
                ab.cooldown > 0.0f ? glm::clamp(ability_cd_[i] / ab.cooldown, 0.0f, 1.0f) : 0.0f;
            const bool ready = frac <= 0.0f;
            const f32 r = slot * 0.16f;

            // Slot face + an accent border that lights up when the ability is ready.
            draw.rect(Vec4{sx, sy, slot, slot}, Vec4{0.11f, 0.13f, 0.17f, 0.95f},
                      Vec4{accent, ready ? 0.95f : 0.28f}, ready ? 2.5f : 1.5f, r);

            // Icon, tinted by readiness.
            const Vec4 icol{ready ? accent : accent * 0.45f, ready ? 1.0f : 0.7f};
            draw_ability_icon(draw, role_, i, sx + slot * 0.5f, sy + slot * 0.42f, slot * 0.24f, icol);

            // Cooldown wipe: a dark overlay draining from the top + the seconds remaining.
            if (!ready) {
                draw.rect(Vec4{sx, sy, slot, slot * frac}, Vec4{0.02f, 0.02f, 0.03f, 0.62f}, r);
                const std::string secs = std::format("{:.0f}", std::ceil(ability_cd_[i]));
                draw.text(Vec2{sx + slot * 0.5f - draw.text_width(secs, ts) * 0.5f,
                               sy + slot * 0.5f - ts * 0.5f},
                          secs, ts, Vec4{0.95f, 0.96f, 1.0f, 0.95f});
            }

            // Key badge (top-left) + ability name (bottom).
            draw.rect(Vec4{sx + 4.0f, sy + 4.0f, slot * 0.26f, slot * 0.26f},
                      Vec4{accent, 0.9f}, slot * 0.07f);
            draw.text(Vec2{sx + 4.0f + slot * 0.08f, sy + 4.0f + slot * 0.04f},
                      std::format("{}", i + 1), slot * 0.18f, Vec4{0.05f, 0.06f, 0.08f, 1.0f});
            const f32 nsz = slot * 0.155f;
            draw.text(Vec2{sx + slot * 0.5f - draw.text_width(ab.name, nsz) * 0.5f, sy + slot * 0.76f},
                      ab.name, nsz, Vec4{ready ? Vec3{0.92f, 0.94f, 0.98f} : Vec3{0.55f, 0.58f, 0.62f}, 1.0f});
            sx += slot + gap;
        }
    }

    // A vector glyph for ability (role, slot), centred at (cx,cy) with ~r radius, drawn from
    // rounded-cap lines + rects so it reads at a glance (sword / shield / bow / cross / bolt …).
    void draw_ability_icon(ui::DrawList& draw, PlayerRole role, u8 slot, f32 cx, f32 cy, f32 r,
                           const Vec4& c) {
        const f32 th = r * 0.34f;
        auto L = [&](Vec2 a, Vec2 b) { draw.line(Vec2{cx, cy} + a, Vec2{cx, cy} + b, th, c); };
        auto arrow = [&](Vec2 tail, Vec2 tip) { // a line with a two-stroke head
            draw.line(Vec2{cx, cy} + tail, Vec2{cx, cy} + tip, th * 0.8f, c);
            const Vec2 d = glm::normalize(tip - tail) * (r * 0.45f);
            const Vec2 p{-d.y, d.x};
            draw.line(Vec2{cx, cy} + tip, Vec2{cx, cy} + tip - d + p * 0.6f, th * 0.8f, c);
            draw.line(Vec2{cx, cy} + tip, Vec2{cx, cy} + tip - d - p * 0.6f, th * 0.8f, c);
        };
        switch (role) {
            case PlayerRole::Knight:
                if (slot == 0) { // sword
                    L(Vec2{0.0f, -r}, Vec2{0.0f, r * 0.55f});
                    L(Vec2{-r * 0.5f, r * 0.35f}, Vec2{r * 0.5f, r * 0.35f}); // crossguard
                    L(Vec2{0.0f, r * 0.55f}, Vec2{0.0f, r});                  // grip
                } else if (slot == 1) { // shield
                    draw.rect(Vec4{cx - r * 0.7f, cy - r * 0.8f, r * 1.4f, r * 1.1f},
                              Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.8f, r * 0.25f);
                    L(Vec2{-r * 0.7f, r * 0.28f}, Vec2{0.0f, r});             // taper to a point
                    L(Vec2{r * 0.7f, r * 0.28f}, Vec2{0.0f, r});
                } else if (slot == 2) { // consecration: a flame over a ground line
                    L(Vec2{-r, r}, Vec2{r, r}); // ground
                    L(Vec2{0.0f, r}, Vec2{-r * 0.35f, -r * 0.2f}); // flame left edge
                    L(Vec2{0.0f, r}, Vec2{r * 0.35f, -r * 0.2f});  // flame right edge
                    L(Vec2{-r * 0.35f, -r * 0.2f}, Vec2{0.0f, -r}); // tip
                    L(Vec2{r * 0.35f, -r * 0.2f}, Vec2{0.0f, -r});
                } else { // taunt: shout waves
                    for (int k = 0; k < 3; ++k) {
                        const f32 o = r * (0.1f + 0.4f * static_cast<f32>(k));
                        L(Vec2{o, -r * 0.6f}, Vec2{o + r * 0.35f, 0.0f});
                        L(Vec2{o + r * 0.35f, 0.0f}, Vec2{o, r * 0.6f});
                    }
                }
                break;
            case PlayerRole::Hunter:
                if (slot == 0) { // single arrow
                    arrow(Vec2{-r * 0.8f, r * 0.8f}, Vec2{r * 0.8f, -r * 0.8f});
                } else if (slot == 1) { // volley: three arrows
                    for (int k = -1; k <= 1; ++k) {
                        const f32 o = static_cast<f32>(k) * r * 0.55f;
                        arrow(Vec2{-r * 0.7f + o, r * 0.7f}, Vec2{r * 0.7f + o, -r * 0.7f});
                    }
                } else if (slot == 2) { // dash: three forward chevrons
                    for (int k = 0; k < 3; ++k) {
                        const f32 o = -r * 0.7f + static_cast<f32>(k) * r * 0.6f;
                        L(Vec2{o, -r * 0.7f}, Vec2{o + r * 0.5f, 0.0f});
                        L(Vec2{o + r * 0.5f, 0.0f}, Vec2{o, r * 0.7f});
                    }
                } else { // piercing shot: one long bold arrow
                    arrow(Vec2{-r, r * 0.55f}, Vec2{r, -r * 0.55f});
                    L(Vec2{-r * 0.5f, -r * 0.55f}, Vec2{-r * 0.5f, r * 0.55f}); // bowstring hint
                }
                break;
            case PlayerRole::Cleric:
                if (slot == 0 || slot == 1) { // healing cross (sanctuary adds rays)
                    draw.rect(Vec4{cx - r * 0.24f, cy - r * 0.85f, r * 0.48f, r * 1.7f}, c, r * 0.1f);
                    draw.rect(Vec4{cx - r * 0.85f, cy - r * 0.24f, r * 1.7f, r * 0.48f}, c, r * 0.1f);
                    if (slot == 1) {
                        for (int k = 0; k < 4; ++k) {
                            const f32 a = TwoPi * (static_cast<f32>(k) + 0.5f) / 4.0f;
                            const Vec2 d{std::cos(a), std::sin(a)};
                            L(d * r * 0.7f, d * r * 1.05f);
                        }
                    }
                } else if (slot == 2) { // smite: a lightning bolt
                    L(Vec2{r * 0.3f, -r}, Vec2{-r * 0.35f, 0.0f});
                    L(Vec2{-r * 0.35f, 0.0f}, Vec2{r * 0.2f, 0.0f});
                    L(Vec2{r * 0.2f, 0.0f}, Vec2{-r * 0.3f, r});
                } else { // aegis: a shield bubble (a ring with a small cross)
                    draw.rect(Vec4{cx - r * 0.85f, cy - r * 0.85f, r * 1.7f, r * 1.7f},
                              Vec4{0.0f, 0.0f, 0.0f, 0.0f}, c, th * 0.7f, r * 0.85f); // ring
                    L(Vec2{0.0f, -r * 0.4f}, Vec2{0.0f, r * 0.4f});
                    L(Vec2{-r * 0.4f, 0.0f}, Vec2{r * 0.4f, 0.0f});
                }
                break;
        }
    }

    // A bold arrow near the top of the screen pointing from the player toward the wagon's
    // destination (world bearing mapped through the fixed iso camera).
    void draw_dest_arrow(ui::DrawList& draw, const Vec3& from, const Vec3& to, f32 W) {
        const f32 cam_yaw = radians(iso::yaw_deg);
        const Vec2 cam_fwd{-std::cos(cam_yaw), -std::sin(cam_yaw)};
        const Vec2 cam_right{std::sin(cam_yaw), -std::cos(cam_yaw)};
        const Vec2 v{to.x - from.x, to.z - from.z};
        Vec2 d{glm::dot(v, cam_right), -glm::dot(v, cam_fwd)};
        if (glm::length(d) < 1e-3f) {
            return;
        }
        d = glm::normalize(d);
        const Vec2 perp{-d.y, d.x};
        const Vec2 c{W * 0.5f, 84.0f};
        const Vec4 amber{0.98f, 0.78f, 0.32f, 1.0f};
        const f32 L = 34.0f, hw = 13.0f;
        const Vec2 tip = c + d * L;
        draw.line(c - d * L * 0.6f, tip, 6.0f, amber);
        draw.line(tip, tip - d * hw + perp * hw, 6.0f, amber);
        draw.line(tip, tip - d * hw - perp * hw, 6.0f, amber);
        draw.text(Vec2{c.x - draw.text_width("DESTINATION", 16.0f) * 0.5f, c.y + 22.0f},
                  "DESTINATION", 16.0f, amber);
    }

    // Full-screen world map: the towns near the player and the roads between them
    // (computed deterministically from the shared seed via roads::gather + village_at),
    // with the player's position + facing. Toggled with M.
    void draw_map() {
        if (renderer_ == nullptr) {
            return;
        }
        const VkExtent2D ext = renderer_->extent();
        const f32 W = static_cast<f32>(ext.width);
        const f32 H = static_cast<f32>(ext.height);
        ui::DrawList draw{*renderer_};

        // Dim the world, then a framed map panel.
        draw.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.03f, 0.04f, 0.06f, 0.86f});
        const f32 mg = std::min(W, H) * 0.07f;
        const Vec4 panel{mg, mg, W - 2.0f * mg, H - 2.0f * mg};
        draw.rect(panel, Vec4{0.09f, 0.10f, 0.13f, 0.97f}, Vec4{0.35f, 0.34f, 0.3f, 1.0f}, 2.0f, 10.0f);
        draw.text(Vec2{panel.x + 22.0f, panel.y + 18.0f}, "WORLD MAP", 30.0f,
                  Vec4{0.92f, 0.9f, 0.96f, 1.0f});

        // Map projection: centre on the player, a fixed world span across the panel.
        const f32 inner = std::min(panel.z, panel.w) * 0.5f - 28.0f;
        const f32 view_world = 480.0f; // metres from centre to edge of the shorter side
        const f32 ppm = inner / view_world;
        const Vec2 mc{panel.x + panel.z * 0.5f, panel.y + panel.w * 0.5f};
        const Vec3 feet = local_feet();
        auto to_screen = [&](f32 wx, f32 wz) {
            return Vec2{mc.x + (wx - feet.x) * ppm, mc.y + (wz - feet.z) * ppm};
        };
        auto in_panel = [&](const Vec2& p) {
            return p.x > panel.x + 4.0f && p.x < panel.x + panel.z - 4.0f &&
                   p.y > panel.y + 36.0f && p.y < panel.y + panel.w - 4.0f;
        };

        // Roads between towns.
        const Vec2 origin{feet.x, feet.z};
        for (const roads::Segment& s : roads::gather(origin, view_world * 1.6f, world_seed_)) {
            const Vec2 a = to_screen(s.a.x, s.a.y);
            const Vec2 b = to_screen(s.b.x, s.b.y);
            if (in_panel(a) || in_panel(b)) {
                draw.line(a, b, 2.5f, Vec4{0.62f, 0.52f, 0.36f, 1.0f});
            }
        }

        // Towns (markers sized by town extent).
        const int reach = static_cast<int>(view_world * 1.4f / worldgen::village_cell) + 1;
        const int ccx = static_cast<int>(std::floor(feet.x / worldgen::village_cell));
        const int ccz = static_cast<int>(std::floor(feet.z / worldgen::village_cell));
        for (int dz = -reach; dz <= reach; ++dz) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const auto v = worldgen::village_at(ccx + dx, ccz + dz, world_seed_);
                if (!v) {
                    continue;
                }
                const Vec2 c = to_screen(v->center.x, v->center.y);
                if (!in_panel(c)) {
                    continue;
                }
                const f32 r = glm::clamp(v->half * ppm, 5.0f, 26.0f);
                draw.rect(Vec4{c.x - r, c.y - r, 2.0f * r, 2.0f * r}, Vec4{0.78f, 0.7f, 0.5f, 0.95f},
                          Vec4{0.25f, 0.22f, 0.16f, 1.0f}, 1.5f, 3.0f);
            }
        }

        // The player: a bright dot with a facing tick.
        const Vec2 me = to_screen(feet.x, feet.z);
        const Vec2 dir{std::cos(face_yaw_), std::sin(face_yaw_)};
        draw.line(me, me + dir * 14.0f, 3.0f, Vec4{0.55f, 0.85f, 1.0f, 1.0f});
        draw.rect(Vec4{me.x - 5.0f, me.y - 5.0f, 10.0f, 10.0f}, Vec4{0.4f, 0.7f, 1.0f, 1.0f}, 5.0f);

        draw.text(Vec2{panel.x + 22.0f, panel.y + panel.w - 30.0f}, "M / ESC  CLOSE", 18.0f,
                  Vec4{0.7f, 0.72f, 0.78f, 1.0f});
    }

    void draw_prop(const PropInstance& p) {
        const std::vector<GpuProp>& set =
            p.category == PropCategory::Bush      ? gpu_bushes_
            : p.category == PropCategory::Rock    ? gpu_rocks_
            : p.category == PropCategory::Log     ? gpu_logs_
            : p.category == PropCategory::Fence   ? gpu_fences_
            : p.category == PropCategory::FenceRail ? gpu_fence_rails_
            : p.category == PropCategory::Lantern ? gpu_lanterns_
            : p.category == PropCategory::House    ? gpu_houses_
            : p.category == PropCategory::Wall     ? gpu_walls_
            : p.category == PropCategory::Gate     ? gpu_gates_
            : p.category == PropCategory::Well     ? gpu_wells_
            : p.category == PropCategory::Bridge   ? gpu_bridges_
            : p.category == PropCategory::Market   ? gpu_markets_
            : p.category == PropCategory::Path     ? gpu_paths_
            : p.category == PropCategory::Planter  ? gpu_planters_
                                                   : gpu_fountains_;
        if (set.empty()) {
            return;
        }
        const GpuProp& gp = set[p.variant % set.size()];
        const Mat4 m = glm::translate(Mat4{1.0f}, p.position) *
                       glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{p.scale * p.length, p.scale, p.scale});
        const f32 glow = glm::mix(1.0f, 0.45f, sun_intensity_); // emissive dims by day

        // Fade a house roof when the local player is inside its footprint.
        bool inside = false;
        if (gp.footprint.x > 0.0f) {
            const Vec3 rel = local_feet() - p.position;
            const f32 cs = std::cos(-p.yaw);
            const f32 sn = std::sin(-p.yaw);
            const Vec2 lp{rel.x * cs - rel.z * sn, rel.x * sn + rel.z * cs};
            inside = std::abs(lp.x) < gp.footprint.x + 0.4f && std::abs(lp.y) < gp.footprint.y + 0.4f &&
                     rel.y > -1.5f && rel.y < gp.wall_height + 2.0f;
        }
        // A burning cottage chars toward black and its cosy window-glow gives way to
        // flames (drawn separately in draw_fires).
        const f32 burn = gp.footprint.x > 0.0f ? house_burn(p.position) : 0.0f;
        const f32 cozy = 1.0f - burn;          // cosy emissive / interior light fades out
        const Vec3 charcoal = glm::mix(Vec3{1.0f}, Vec3{0.14f, 0.12f, 0.11f}, burn);
        const Vec4 char_tint{charcoal, 1.0f};

        const f32 night = 1.0f - sun_intensity_;
        for (const GpuPropPart& part : gp.parts) {
            if (part.layer == PropLayer::Foliage) {
                renderer_->draw_transparent(part.mesh, m, Vec4{1.0f});
            } else if (part.layer == PropLayer::Emissive) {
                const f32 e = glow * cozy;
                renderer_->draw_emissive(part.mesh, m, Vec4{e, e, e, 1.0f});
            } else if (part.layer == PropLayer::Glow) {
                // Window light shafts: additive, dusk/night only, gone once ablaze.
                if (night > 0.05f && cozy > 0.05f) {
                    renderer_->draw_glow(part.mesh, m, Vec4{1.0f, 1.0f, 1.0f, night * 0.6f * cozy});
                }
            } else if (part.layer == PropLayer::Roof) {
                if (inside) {
                    renderer_->draw_transparent(part.mesh, m, Vec4{charcoal, 0.18f});
                } else {
                    renderer_->draw(part.mesh, m, char_tint);
                }
            } else {
                renderer_->draw(part.mesh, m, char_tint);
            }
        }

        // Interior + lantern spot lights, added at dusk/night. Only props near the player
        // submit lights at all (distant town lights are skipped) - this keeps the lit-up
        // night cheap. House interiors cast shadows (occluded by walls); lanterns/braziers
        // are cheap unshadowed pools, so a town full of lanterns barely costs anything.
        constexpr f32 kLightCullDist = 38.0f;
        const bool near_player = glm::length(p.position - local_feet()) < kLightCullDist;
        if (night > 0.12f && cozy > 0.1f && near_player && !gp.lights.empty()) {
            const Mat3 rot{m};
            for (const PropLight& pl : gp.lights) {
                Renderer::SpotLight sl;
                sl.position = Vec3{m * Vec4{pl.offset, 1.0f}};
                sl.direction = glm::normalize(rot * pl.direction);
                sl.color = pl.color * (pl.intensity * night * cozy);
                sl.range = pl.range;
                const f32 half = glm::radians(pl.cone_deg * 0.5f);
                sl.cone_outer_cos = std::cos(half);
                sl.cone_inner_cos = std::cos(half * 0.65f);
                // House hearth/lamp/candle light is INDOORS: it must be occluded by the walls,
                // so it casts a real shadow (or isn't drawn). A standalone WILD lantern (one
                // dotted through the forest, not inside a town) also casts shadows so it grounds
                // the player + props in the dark woods. Town lanterns / gate braziers stay in
                // the cheap unshadowed pool (a whole town of them would swamp the shadow budget).
                const bool wild_lantern =
                    p.category == PropCategory::Lantern &&
                    !worldgen::inside_village(p.position.x, p.position.z, world_seed_, 2.0f);
                sl.indoor = (p.category == PropCategory::House);
                sl.cast_shadow = (p.category == PropCategory::House) || wild_lantern;
                renderer_->add_light(sl);
            }
        }
    }

    void draw_character(PlayerVisual& v, const Vec3& feet, f32 yaw, bool seated = false,
                        int role = -1) {
        // Seated riders sink onto the bench (the sit pose folds the legs forward).
        const Vec3 base = seated ? feet - Vec3{0.0f, 0.42f, 0.0f} : feet;
        Mat4 root = glm::translate(Mat4{1.0f}, base) *
                    glm::rotate(Mat4{1.0f}, HalfPi - yaw, Vec3{0.0f, 1.0f, 0.0f});
        // The blobby squash/sway/lean wobble rides on the root when on foot (not seated).
        if (!seated) {
            root = root * v.animator.body_offset();
        }
        const std::vector<Quat> pose =
            seated ? CharacterAnimator::sit_pose(v.model) : v.animator.pose(v.model);
        const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
        draw_rig(v.model, mats);
        if (role >= 0) {
            // Weapons attach to the JOINT frames (orientation + position) so they swing with
            // the arm, unlike the box mats whose columns are scaled by box_size.
            const std::vector<Mat4> jmats = v.model.joint_matrices(root, pose);
            const PlayerRole r = static_cast<PlayerRole>(role % kRoleCount);
            if (r == PlayerRole::Cleric) {
                draw_cleric_staff(v.model, jmats, feet, v.animator, yaw);
            } else {
                draw_role_weapon(v.model, jmats, r);
            }
            // A steel motion trail off the real blade tip while a Knight is mid-swing (the sword
            // is on the player's right = the L-suffixed bone).
            if (r == PlayerRole::Knight && v.animator.swinging()) {
                const Mat4 grip = hand_frame(v.model, jmats, BonePart::LowerArmL) *
                                  glm::rotate(Mat4{1.0f}, -0.35f, Vec3{1.0f, 0.0f, 0.0f});
                const Vec3 tip = Vec3{(grip * glm::translate(Mat4{1.0f}, Vec3{0.0f, -1.15f, 0.0f}))[3]};
                emit(tip, Vec3{0.0f}, Vec4{0.92f, 0.96f, 1.0f, 0.8f}, 0.16f, 0.17f, 1);
            }
        }
    }

    // ---- Village NPCs (server-authoritative; the player defends them) -------
    // Villagers are simulated on the server (wander/sleep/flee, killable by enemies)
    // and arrive in the snapshot; the client just renders + animates them, rebuilding
    // a model when its appearance first appears, and culls visuals that have died /
    // left the snapshot.
    void update_villager_visuals(Timestep dt) {
        if (!have_snapshot_) {
            return;
        }
        for (const net::VillagerState& vl : snapshot_.villagers) {
            PlayerVisual& v = ensure_villager_visual(vl.id, vl.appearance);
            f32 measured = 0.0f;
            if (v.has_last && dt.seconds > 0.0001f) {
                Vec3 d = vl.position - v.last_pos;
                d.y = 0.0f;
                measured = glm::length(d) / dt.seconds;
            }
            v.speed = glm::mix(v.speed, measured, 0.3f);
            v.last_pos = vl.position;
            v.has_last = true;
            v.animator.update(v.speed, dt);
        }
        for (auto it = villager_visuals_.begin(); it != villager_visuals_.end();) {
            const bool live = std::any_of(snapshot_.villagers.begin(), snapshot_.villagers.end(),
                                          [&](const net::VillagerState& s) { return s.id == it->first; });
            it = live ? std::next(it) : villager_visuals_.erase(it);
        }
    }

    PlayerVisual& ensure_villager_visual(u32 id, const CharacterAppearance& appearance) {
        const auto it = villager_visuals_.find(id);
        if (it != villager_visuals_.end()) {
            return it->second;
        }
        PlayerVisual v;
        v.appearance = appearance;
        v.model = CharacterModel::create(id ^ 0x55u, appearance);
        return villager_visuals_.emplace(id, std::move(v)).first->second;
    }

    void draw_villagers() {
        if (!have_snapshot_) {
            return;
        }
        for (const net::VillagerState& vl : snapshot_.villagers) {
            const auto it = villager_visuals_.find(vl.id);
            if (it == villager_visuals_.end()) {
                continue;
            }
            PlayerVisual& v = it->second;
            // Kind 3 is a hired carriage driver sitting on the top seat (sit pose), attached to
            // the cart's tilt + bob so they ride with it.
            const bool seated = vl.kind == 3;
            const net::WagonState* aw = seated ? active_wagon() : nullptr;
            const Vec3 seat = (aw != nullptr) ? attach_to_wagon(*aw, vl.position) : vl.position;
            const Vec3 base = seated ? seat - Vec3{0.0f, 0.42f, 0.0f} : vl.position;
            Mat4 root = glm::translate(Mat4{1.0f}, base) *
                        glm::rotate(Mat4{1.0f}, HalfPi - vl.yaw, Vec3{0.0f, 1.0f, 0.0f});
            if (!seated) {
                root = root * v.animator.body_offset();
            }
            const std::vector<Quat> pose =
                seated ? CharacterAnimator::sit_pose(v.model) : v.animator.pose(v.model);
            const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
            // Guards (kind 1) wear a steel tint + carry a spear gripped in the hand.
            if (vl.kind == 1) {
                draw_rig(v.model, mats, Vec3{0.72f, 0.76f, 0.86f});
                draw_held_spear(v.model, mats);
            } else {
                draw_rig(v.model, mats);
            }
        }
    }

    void send_input() {
        if (!client_.connected()) {
            return;
        }
        if (paused_ || map_open_) {
            blocking_ = false; // drop the guard if a release got swallowed by the menu/map
        }
        // Movement is relative to the fixed camera: W goes "into" the screen.
        const f32 cam_yaw = radians(iso::yaw_deg);
        const Vec3 cam_fwd{-std::cos(cam_yaw), 0.0f, -std::sin(cam_yaw)};
        const Vec3 cam_right{-cam_fwd.z, 0.0f, cam_fwd.x};
        Vec3 move{0.0f};
        bool jump = false;
        f32 throttle = 0.0f; // raw W/S - drives a carriage when piloting (else ignored)
        f32 steer = 0.0f;    // raw A/D - reins the horses (RDR-style) when piloting
        // While the pause menu is up the player holds still (and ignores WASD/SPACE
        // that would otherwise leak through to movement).
        if (Input* in = input(); in != nullptr && !paused_ && !map_open_) {
            if (in->key_down(key::W)) { move += cam_fwd; throttle += 1.0f; }
            if (in->key_down(key::S)) { move -= cam_fwd; throttle -= 1.0f; }
            if (in->key_down(key::D)) { move += cam_right; steer += 1.0f; } // rein right
            if (in->key_down(key::A)) { move -= cam_right; steer -= 1.0f; } // rein left
            jump = in->key_down(key::Space);
        }
        if (glm::length(move) > 0.01f) {
            face_yaw_ = std::atan2(move.z, move.x); // face the way we walk
        }

        net::PlayerInput packet;
        packet.sequence = ++sequence_;
        packet.move = move;
        packet.yaw = face_yaw_;
        packet.jump = jump;
        packet.throttle = throttle;
        packet.steer = steer;
        packet.add = pending_add_;
        packet.fire = pending_fire_;
        packet.attack = pending_attack_;
        packet.build = pending_build_;
        packet.rally = pending_rally_;
        packet.grab = pending_grab_;
        packet.aim = aim_;
        // A haul starts by walking up to a parked wagon (which pops its info panel) and pressing
        // ACCEPT. `near_wagon_` is the offer you're standing by; `selected_wagon_` is the one you
        // accepted (your vote). Drop a stale vote if its offer is gone (town changed).
        const bool offering = snapshot_.contract_phase == static_cast<u8>(ContractPhase::Offer);
        near_wagon_ = offering ? nearest_offer_in_range() : 0;
        if (selected_wagon_ != 0 && (!offering || !wagon_offered(selected_wagon_))) {
            selected_wagon_ = 0;
        }
        packet.vote_wagon = offering ? selected_wagon_ : 0;
        packet.vote_mode = vote_mode_;
        packet.role = static_cast<u8>(role_);
        packet.ability = pending_ability_;
        // Right-mouse hold: Knight shield guard / Cleric heal channel.
        packet.block = blocking_ && (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric);
        packet.appearance = appearance_;
        client_.send_input(packet);
        pending_ability_ = 0;
        pending_add_ = false;
        pending_fire_ = false;
        pending_attack_ = false;
        pending_build_ = false;
        pending_rally_ = false;
        pending_grab_ = false;
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
    u32 nearest_offer_in_range() const {
        if (!have_snapshot_ || snapshot_.contract_phase != static_cast<u8>(ContractPhase::Offer)) {
            return 0;
        }
        const Vec3 feet = local_feet();
        u32 best_id = 0;
        f32 best = kWagonStartRange;
        for (const net::WagonState& wg : snapshot_.wagons) {
            const f32 d = glm::length(Vec2{wg.position.x - feet.x, wg.position.z - feet.z});
            if (d < best) {
                best = d;
                best_id = wg.id;
            }
        }
        return best_id;
    }
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
    static std::string town_name(const Vec3& c) {
        static const char* pre[] = {"Oak",  "Stone", "Black", "White", "Raven", "Wolf",
                                    "Ash",  "Fern",  "Mill",  "Hart",  "Bram",  "Thorn",
                                    "Wind", "Frost", "Elder", "Gold"};
        static const char* suf[] = {"ford",   "ton",  "wick", "field",  "bury", "dale",
                                    "stead",  "hollow", "gate", "moor", "bridge", "haven",
                                    "shire",  "mere", "crest", "wood"};
        u32 h = static_cast<u32>(static_cast<i32>(std::lround(c.x)) * 73856093) ^
                static_cast<u32>(static_cast<i32>(std::lround(c.z)) * 19349663);
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        std::string name = pre[h % 16u];
        name += suf[(h >> 8) % 16u];
        return name;
    }


    // Unproject the cursor through the iso camera onto the terrain (for digging).
    void update_aim() {
        aim_valid_ = false;
        if (terrain_ == nullptr || renderer_ == nullptr) {
            return;
        }
        const VkExtent2D extent = renderer_->extent();
        if (extent.width == 0 || extent.height == 0) {
            return;
        }
        Vec2 cursor{static_cast<f32>(extent.width) * 0.5f, static_cast<f32>(extent.height) * 0.5f};
        if (Input* in = input()) {
            cursor = in->mouse_position();
        }
        const Vec2 ndc{2.0f * cursor.x / static_cast<f32>(extent.width) - 1.0f,
                       2.0f * cursor.y / static_cast<f32>(extent.height) - 1.0f};
        const Mat4 inv = glm::inverse(camera_.view_projection());
        const Vec4 near_h = inv * Vec4{ndc.x, ndc.y, 0.0f, 1.0f};
        const Vec4 far_h = inv * Vec4{ndc.x, ndc.y, 1.0f, 1.0f};
        const Vec3 origin = Vec3{near_h} / near_h.w;
        const Vec3 dir = glm::normalize(Vec3{far_h} / far_h.w - origin);
        if (const auto hit = terrain_->raycast(origin, dir, 300.0f)) {
            aim_ = *hit;
            aim_valid_ = true;
        }
    }

    // A rotation that maps the mesh's local +Z axis onto `dir` (used to point arrows along
    // their flight). Falls back to identity for a degenerate direction.
    static Mat4 orient_to(const Vec3& dir) {
        const f32 len = glm::length(dir);
        if (len < 1e-4f) {
            return Mat4{1.0f};
        }
        const Vec3 f = dir / len;
        const Vec3 up = std::abs(f.y) > 0.99f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 r = glm::normalize(glm::cross(up, f));
        const Vec3 u = glm::cross(f, r);
        Mat4 m{1.0f};
        m[0] = Vec4{r, 0.0f};
        m[1] = Vec4{u, 0.0f};
        m[2] = Vec4{f, 0.0f};
        return m;
    }

    Mat4 tree_model(const TreeInstance& t) const {
        return glm::translate(Mat4{1.0f}, t.position) *
               glm::rotate(Mat4{1.0f}, t.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
               glm::scale(Mat4{1.0f}, Vec3{t.scale});
    }
    usize tree_index(const TreeInstance& t) const {
        return tree_library_.empty() ? 0 : static_cast<usize>(t.variant) % tree_library_.size();
    }

    static ApplicationConfig make_config(u64 max_frames) {
        ApplicationConfig config;
        config.name = "Alryn Client";
        config.width = 1280;
        config.height = 720;
        config.headless = false;
        config.max_frames = max_frames;
        return config;
    }

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
    PlayerRole role_ = PlayerRole::Knight;          // chosen combat role (weapon + abilities)
    u8 pending_ability_ = 0;                         // ability invoked this frame (0 = none)
    f32 ability_cd_[kAbilitySlots] = {0.0f, 0.0f, 0.0f}; // client-side HUD cooldown estimate
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
    f32 frand() { // xorshift32 -> [0,1)
        fx_rng_ ^= fx_rng_ << 13;
        fx_rng_ ^= fx_rng_ >> 17;
        fx_rng_ ^= fx_rng_ << 5;
        return static_cast<f32>(fx_rng_ & 0xffffffu) / static_cast<f32>(0x1000000u);
    }
    f32 frand(f32 a, f32 b) { return a + (b - a) * frand(); }
    Vec3 rand_dir() {
        const f32 z = frand(-1.0f, 1.0f);
        const f32 a = frand(0.0f, TwoPi);
        const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        return Vec3{r * std::cos(a), z, r * std::sin(a)};
    }
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
    Mesh shape_box_;
    Mesh shape_sphere_;
    Mesh shape_cylinder_;
    Mesh shape_capsule_;
    Mesh shape_rounded_;
    Mesh marker_;
    Mesh water_mesh_;
    f32 elapsed_ = 0.0f;
    f32 time_of_day_ = 0.35f; // 0=midnight, 0.25=sunrise, 0.5=noon, 0.75=sunset
    f32 day_seconds_ = 120.0f;
    f32 sun_intensity_ = 1.0f; // cached from the day/night cycle (0 night .. 1 day)
    f32 cam_distance_ = iso::distance; // scroll-wheel zoom
    Camera camera_;

    net::PlayerId my_id_ = 0;
    u32 world_seed_ = 0;     // shared world seed (from Welcome) - for the map's town/road graph
    bool map_open_ = false;  // full-screen map overlay (M)
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
    Mesh rope_mesh_;            // a unit harness-trace link, drawn per rope segment
    Mesh goods_mesh_;           // a cargo crate (spilled on the ground / carried by a player)
    std::unordered_map<u32, f32> wagon_roll_;  // accumulated wheel spin per wagon id
    std::unordered_map<u32, Vec3> wagon_prev_; // last wagon position (to derive roll)
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

// --------------------------------------------------------------------------
//  Headless "bot": connects and walks so we can demo other players moving.
// --------------------------------------------------------------------------
static void run_bot(const std::string& host, f32 seconds) {
    Log::init(LogLevel::Info);
    net::NetClient client;
    if (!client.connect(host, kPort)) {
        ALRYN_ERROR("Bot could not connect to {}:{}", host, kPort);
        return;
    }
    Clock clock;
    f32 elapsed = 0.0f;
    u32 sequence = 0;
    while (elapsed < seconds) {
        client.poll(2);
        const f32 dt = static_cast<f32>(clock.restart());
        elapsed += dt;
        if (client.connected()) {
            net::PlayerInput packet;
            packet.sequence = ++sequence;
            packet.move = Vec3{std::sin(elapsed * 1.3f) * 0.8f, 0.0f, std::cos(elapsed * 0.9f) * 0.8f};
            packet.yaw = elapsed;
            client.send_input(packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    client.disconnect();
}

int main(int argc, char** argv) {
    std::string mode = "client";
    std::string host = "127.0.0.1";
    bool joining = false;
    u64 frames = 0;
    f32 bot_seconds = 60.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            mode = "server";
        } else if (arg == "--bot") {
            mode = "bot";
        } else if (arg.rfind("--host=", 0) == 0) {
            host = arg.substr(7);
            joining = true;
        } else {
            frames = std::strtoull(arg.c_str(), nullptr, 10);
            bot_seconds = static_cast<f32>(frames > 0 ? frames : 60);
        }
    }

    if (mode == "server") {
        ServerApp app;
        app.run();
    } else if (mode == "bot") {
        run_bot(host, bot_seconds);
    } else {
        const bool auto_start = joining || frames > 0;
        ClientApp app{host, !joining, frames, auto_start};
        app.run();
    }
    return 0;
}
