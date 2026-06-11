#include <Alryn/Alryn.h>

#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Terrain/StreamingTerrain.h>
#include <Alryn/UI/UI.h>
#include <Alryn/World/PropLibrary.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using namespace alryn;

namespace key {
constexpr KeyCode W = 87, A = 65, S = 83, D = 68, F = 70, Space = 32, Escape = 256;
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
        shape_rounded_.create(renderer_->device(), primitives::rounded_box(0.12f, Vec3{1.0f}));

        // Upload the forest-prop catalogue (bushes, rocks, logs) to GPU meshes.
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
                out.push_back(std::move(gp));
            }
        };
        upload_props(prop_lib_.bushes(), gpu_bushes_);
        upload_props(prop_lib_.rocks(), gpu_rocks_);
        upload_props(prop_lib_.logs(), gpu_logs_);

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
            if (paused_) {
                aim_valid_ = false; // no dig-marker while the pause menu is up
            } else {
                update_aim();
            }
            send_input();
            update_visuals(dt);

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
            for (const net::PlayerState& p : snapshot_.players) {
                const auto it = visuals_.find(p.id);
                if (it != visuals_.end()) {
                    draw_character(it->second, p.position, p.yaw);
                }
            }
        }

        // Tree trunks (opaque).
        terrain_->for_each_tree([&](const TreeInstance& t) {
            renderer_->draw(tree_library_[tree_index(t)].trunk, tree_model(t));
        });

        // Discrete props: rocks/houses (opaque + emissive), bushes (foliage).
        terrain_->for_each_prop([&](const PropInstance& p) { draw_prop(p); });

        // Thrown projectiles (server-simulated; we just render their positions).
        if (have_snapshot_) {
            for (const net::ProjectileState& pr : snapshot_.projectiles) {
                const Mat4 m = glm::translate(Mat4{1.0f}, pr.position) *
                               glm::scale(Mat4{1.0f}, Vec3{0.36f});
                renderer_->draw(shape_sphere_, m, Vec4{0.55f, 0.5f, 0.45f, 1.0f});
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
            const f32 canopy = 2.6f * t.scale;
            f32 alpha = dxz < canopy ? glm::mix(0.18f, 1.0f, dxz / canopy) : 1.0f;
            // Fade canopies close to the camera so they don't block the view.
            const Vec3 cc = t.position + Vec3{0.0f, 3.2f * t.scale, 0.0f};
            alpha = std::min(alpha, glm::mix(0.06f, 1.0f, glm::smoothstep(2.5f, 7.0f,
                                                                          glm::length(cc - cam_eye))));
            renderer_->draw_transparent(tree_library_[tree_index(t)].foliage, tree_model(t),
                                        Vec4{t.tint, alpha});
        });
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

        const Mat4 root = glm::rotate(Mat4{1.0f}, preview_turn_, Vec3{0.0f, 1.0f, 0.0f});
        const std::vector<Quat> pose = preview_anim_.pose(preview_model_);
        draw_rig(preview_model_, preview_model_.bone_matrices(root, pose));
    }

    // Draws every bone of a posed character with its palette colour + shape mesh.
    void draw_rig(const CharacterModel& model, const std::vector<Mat4>& mats) {
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
                                : bones[i].shape == BoneShape::RoundedBox ? shape_rounded_
                                                                          : shape_box_;
            renderer_->draw(shape, mats[i], Vec4{color, 1.0f});
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

        // In-game input (not paused).
        dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
            if (e.button() == 0) {
                pending_dig_ = true;
            } else if (e.button() == 1) {
                pending_add_ = true;
            }
            return false;
        });
        dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
            if (e.key() == key::Escape) {
                escape_pressed(); // opens the pause menu
            } else if (e.key() == key::F) {
                pending_fire_ = true; // throw a projectile toward the cursor
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
        shape_rounded_.destroy();
        for (auto& tv : tree_library_) {
            tv.trunk.destroy();
            tv.foliage.destroy();
        }
        tree_library_.clear();
        for (std::vector<GpuProp>* set : {&gpu_bushes_, &gpu_rocks_, &gpu_logs_}) {
            for (GpuProp& gp : *set) {
                for (GpuPropPart& part : gp.parts) {
                    part.mesh.destroy();
                }
            }
            set->clear();
        }
        marker_.destroy();
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
    // ALRYN_TIME (0..1) pins the starting time; ALRYN_DAY_SECONDS sets cycle length.
    void update_day_night(Timestep dt) {
        time_of_day_ += dt.seconds / day_seconds_;
        time_of_day_ -= std::floor(time_of_day_);

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
            v.animator.update(v.speed, dt);
        }
    }

    void draw_prop(const PropInstance& p) {
        const std::vector<GpuProp>& set = p.category == PropCategory::Bush   ? gpu_bushes_
                                          : p.category == PropCategory::Rock ? gpu_rocks_
                                                                             : gpu_logs_;
        if (set.empty()) {
            return;
        }
        const GpuProp& gp = set[p.variant % set.size()];
        const Mat4 m = glm::translate(Mat4{1.0f}, p.position) *
                       glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{p.scale});
        for (const GpuPropPart& part : gp.parts) {
            if (part.layer == PropLayer::Foliage) {
                renderer_->draw_transparent(part.mesh, m, Vec4{1.0f});
            } else {
                renderer_->draw(part.mesh, m);
            }
        }
    }

    void draw_character(PlayerVisual& v, const Vec3& feet, f32 yaw) {
        const Mat4 root = glm::translate(Mat4{1.0f}, feet) *
                          glm::rotate(Mat4{1.0f}, HalfPi - yaw, Vec3{0.0f, 1.0f, 0.0f});
        const std::vector<Quat> pose = v.animator.pose(v.model);
        draw_rig(v.model, v.model.bone_matrices(root, pose));
    }

    void send_input() {
        if (!client_.connected()) {
            return;
        }
        // Movement is relative to the fixed camera: W goes "into" the screen.
        const f32 cam_yaw = radians(iso::yaw_deg);
        const Vec3 cam_fwd{-std::cos(cam_yaw), 0.0f, -std::sin(cam_yaw)};
        const Vec3 cam_right{-cam_fwd.z, 0.0f, cam_fwd.x};
        Vec3 move{0.0f};
        bool jump = false;
        // While the pause menu is up the player holds still (and ignores WASD/SPACE
        // that would otherwise leak through to movement).
        if (Input* in = input(); in != nullptr && !paused_) {
            if (in->key_down(key::W)) move += cam_fwd;
            if (in->key_down(key::S)) move -= cam_fwd;
            if (in->key_down(key::D)) move += cam_right;
            if (in->key_down(key::A)) move -= cam_right;
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
        packet.dig = pending_dig_;
        packet.add = pending_add_;
        packet.fire = pending_fire_;
        packet.aim = aim_;
        packet.appearance = appearance_;
        client_.send_input(packet);
        pending_dig_ = false;
        pending_add_ = false;
        pending_fire_ = false;
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
    };

    std::unordered_map<net::PlayerId, PlayerVisual> visuals_;
    std::vector<TreeVisual> tree_library_;
    PropLibrary prop_lib_;
    std::vector<GpuProp> gpu_bushes_;
    std::vector<GpuProp> gpu_rocks_;
    std::vector<GpuProp> gpu_logs_;
    Mesh shape_box_;
    Mesh shape_sphere_;
    Mesh shape_cylinder_;
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
    net::Snapshot snapshot_;
    bool have_snapshot_ = false;

    f32 face_yaw_ = 0.0f;
    u32 sequence_ = 0;
    bool pending_dig_ = false;
    bool pending_add_ = false;
    bool pending_fire_ = false;
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
