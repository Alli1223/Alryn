#include <Alryn/Alryn.h>

#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Terrain/WorldGen.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace alryn;

namespace key {
constexpr KeyCode W = 87, A = 65, S = 83, D = 68, Space = 32, Escape = 256;
}

constexpr u16 kPort = 24650;

// --------------------------------------------------------------------------
//  Dedicated server: headless, owns the authoritative GameServer, ticks ~60 Hz.
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
        std::this_thread::sleep_for(std::chrono::milliseconds(15)); // ~60 Hz tick
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
//  Windowed multiplayer client.
// --------------------------------------------------------------------------
class ClientApp : public Application {
public:
    ClientApp(std::string host, bool host_local, u64 max_frames)
        : Application(make_config(max_frames)), host_(std::move(host)), host_local_(host_local) {}

protected:
    void on_init() override {
        renderer_ = renderer();
        if (renderer_ == nullptr) {
            return;
        }
        if (window() != nullptr) {
            window()->set_cursor_captured(true);
        }
        avatar_.create(renderer_->device(),
                       primitives::cube(1.0f, Vec3{0.95f, 0.55f, 0.15f})); // other players
        marker_.create(renderer_->device(), primitives::cube(1.0f, Vec3{1.0f, 0.85f, 0.2f}));

        // Default: host an in-process listen server so a bare `make run` is
        // immediately playable. Falls back to pure client if the port is taken
        // (e.g. a dedicated --server is already running on this machine).
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
        ALRYN_INFO("Connecting to {}:{} ... WASD move, mouse look, SPACE jump, "
                   "left-click dig, right-click add, ESC exit.",
                   host_, kPort);
    }

    void on_update(Timestep dt) override {
        if (host_local_ && local_server_.running()) {
            local_server_.tick(dt); // we are the authoritative server too
        }
        for (const net::ClientEvent& e : client_.poll()) {
            switch (e.type) {
                case net::ClientEventType::WelcomeReceived:
                    my_id_ = e.welcome.your_id;
                    build_terrain(e.welcome.seed);
                    break;
                case net::ClientEventType::SnapshotReceived:
                    snapshot_ = e.snapshot;
                    have_snapshot_ = true;
                    break;
                case net::ClientEventType::DeformReceived:
                    if (terrain_ != nullptr) {
                        terrain_->deform(e.deform.center, e.deform.radius, e.deform.amount);
                    }
                    break;
                default:
                    break;
            }
        }

        if (Input* in = input()) {
            const Vec2 look = in->mouse_delta();
            yaw_ += look.x * sensitivity_;
            pitch_ = glm::clamp(pitch_ - look.y * sensitivity_, radians(-89.0f), radians(89.0f));
        }

        update_aim();

        if (client_.connected()) {
            const Vec3 forward_flat{std::cos(yaw_), 0.0f, std::sin(yaw_)};
            // right = forward x up (camera's +X / screen-right)
            const Vec3 right_flat{-forward_flat.z, 0.0f, forward_flat.x};
            Vec3 move{0.0f};
            bool jump = false;
            if (Input* in = input()) {
                if (in->key_down(key::W)) move += forward_flat;
                if (in->key_down(key::S)) move -= forward_flat;
                if (in->key_down(key::D)) move += right_flat;
                if (in->key_down(key::A)) move -= right_flat;
                jump = in->key_down(key::Space);
            }
            net::PlayerInput packet;
            packet.sequence = ++sequence_;
            packet.move = move;
            packet.yaw = yaw_;
            packet.pitch = pitch_;
            packet.jump = jump;
            packet.dig = pending_dig_;
            packet.add = pending_add_;
            packet.aim = aim_;
            client_.send_input(packet);
            pending_dig_ = false;
            pending_add_ = false;
        }

        if (terrain_ != nullptr && terrain_->any_dirty() && renderer_ != nullptr) {
            terrain_->rebuild_dirty(renderer_->device());
        }
    }

    void on_render() override {
        if (renderer_ == nullptr || terrain_ == nullptr) {
            return;
        }
        const Vec3 eye = local_eye();
        const Vec3 look = look_direction();
        camera_.set_perspective(radians(70.0f), renderer_->aspect(), 0.05f, 400.0f);
        camera_.look_at(eye, eye + look);
        renderer_->set_camera(camera_);

        terrain_->for_each_mesh([&](const Mesh& mesh) { renderer_->draw(mesh, Mat4{1.0f}); });

        if (have_snapshot_) {
            for (const net::PlayerState& p : snapshot_.players) {
                if (p.id == my_id_) {
                    continue; // first person: don't draw ourselves
                }
                const Mat4 model = glm::translate(Mat4{1.0f}, p.position + Vec3{0.0f, 0.9f, 0.0f}) *
                                   glm::rotate(Mat4{1.0f}, p.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                                   glm::scale(Mat4{1.0f}, Vec3{0.6f, 1.8f, 0.6f});
                renderer_->draw(avatar_, model);
            }
        }

        if (aim_valid_) {
            renderer_->draw(marker_, glm::translate(Mat4{1.0f}, aim_) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.3f}));
        }
    }

    void on_event(Event& event) override {
        EventDispatcher dispatcher{event};
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
                close();
            }
            return false;
        });
    }

    void on_shutdown() override {
        avatar_.destroy();
        marker_.destroy();
        terrain_.reset();
        client_.disconnect();
        local_server_.stop();
    }

private:
    void build_terrain(u32 seed) {
        terrain_ = std::make_unique<Terrain>(worldgen::dims, worldgen::voxel_size, 16, worldgen::origin);
        terrain_->generate([seed](const Vec3& p) { return worldgen::density(p, seed); });
        terrain_->rebuild_dirty(renderer_->device());
        ALRYN_INFO("World ready (seed {}, {} chunks)", seed, terrain_->chunk_count());
    }

    Vec3 look_direction() const {
        return Vec3{std::cos(pitch_) * std::cos(yaw_), std::sin(pitch_),
                    std::cos(pitch_) * std::sin(yaw_)};
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

    Vec3 local_eye() const { return local_feet() + Vec3{0.0f, 1.6f, 0.0f}; }

    void update_aim() {
        aim_valid_ = false;
        if (terrain_ == nullptr) {
            return;
        }
        if (const auto hit = terrain_->raycast(local_eye(), look_direction(), 80.0f)) {
            aim_ = *hit;
            aim_valid_ = true;
        }
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

    std::string host_;
    bool host_local_ = true;
    GameServer local_server_;
    Renderer* renderer_ = nullptr;
    net::NetClient client_;
    std::unique_ptr<Terrain> terrain_;
    Mesh avatar_;
    Mesh marker_;
    Camera camera_;

    net::PlayerId my_id_ = 0;
    net::Snapshot snapshot_;
    bool have_snapshot_ = false;

    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    f32 sensitivity_ = 0.0025f;
    u32 sequence_ = 0;
    bool pending_dig_ = false;
    bool pending_add_ = false;
    Vec3 aim_{0.0f};
    bool aim_valid_ = false;
};

// --------------------------------------------------------------------------
//  Headless "bot": connects and walks in a circle so we can demo two players.
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
            // Oscillate near the spawn so the bot stays in view of the client.
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
    bool joining = false; // true once an explicit --host= asks us to join a server
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
        ClientApp app{host, !joining, frames};
        app.run();
    }
    return 0;
}
