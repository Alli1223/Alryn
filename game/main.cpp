#include <Alryn/Alryn.h>

#include <Alryn/Character/CharacterAnimator.h>
#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Net/GameServer.h>
#include <Alryn/Net/NetClient.h>
#include <Alryn/Terrain/StreamingTerrain.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

using namespace alryn;

namespace key {
constexpr KeyCode W = 87, A = 65, S = 83, D = 68, Space = 32, Escape = 256;
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
    ClientApp(std::string host, bool host_local, u64 max_frames)
        : Application(make_config(max_frames)), host_(std::move(host)), host_local_(host_local) {}

protected:
    void on_init() override {
        renderer_ = renderer();
        if (renderer_ == nullptr) {
            return;
        }
        if (window() != nullptr) {
            window()->set_cursor_captured(false); // free cursor for click-to-dig
        }
        marker_.create(renderer_->device(), primitives::cube(1.0f, Vec3{1.0f, 0.85f, 0.2f}));
        // A large wave grid that follows the player; the water shader animates it.
        water_mesh_.create(renderer_->device(), primitives::grid(80, 2.0f, Vec3{0.1f, 0.3f, 0.4f}));
        // Shared tree meshes (trunk opaque, foliage alpha-blendable); instances vary.
        for (int v = 0; v < 2; ++v) {
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
        ALRYN_INFO("Connecting to {}:{} ... WASD move, mouse to aim, left-click dig, "
                   "right-click add, SPACE jump, ESC exit.",
                   host_, kPort);
    }

    void on_update(Timestep dt) override {
        if (host_local_ && local_server_.running()) {
            local_server_.tick(dt);
        }
        elapsed_ += dt.seconds;
        if (renderer_ != nullptr) {
            renderer_->set_time(elapsed_);
        }

        for (const net::ClientEvent& e : client_.poll()) {
            switch (e.type) {
                case net::ClientEventType::WelcomeReceived:
                    my_id_ = e.welcome.your_id;
                    terrain_ = std::make_unique<StreamingTerrain>(e.welcome.seed, 0.5f, 16, 4);
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
        update_aim();
        send_input();
        update_visuals(dt);

        if (terrain_ != nullptr && renderer_ != nullptr) {
            terrain_->update(local_feet(), renderer_->device());
        }
    }

    void on_render() override {
        if (renderer_ == nullptr || terrain_ == nullptr) {
            return;
        }
        renderer_->set_camera(camera_);

        terrain_->for_each_mesh([&](const Mesh& mesh) { renderer_->draw(mesh, Mat4{1.0f}); });

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

        if (aim_valid_) {
            renderer_->draw(marker_, glm::translate(Mat4{1.0f}, aim_) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.3f}));
        }

        // Transparent water surface, drawn after opaque geometry. Depth-tested but
        // not depth-written, so terrain above the waterline correctly hides it.
        const Vec3 feet = local_feet();
        renderer_->draw_water(water_mesh_, glm::translate(Mat4{1.0f}, Vec3{feet.x,
                                                          worldgen::water_level, feet.z}));

        // Tree foliage (transparent), drawn last. Fades out when the local player
        // is under the canopy so you can see yourself and the ground beneath.
        terrain_->for_each_tree([&](const TreeInstance& t) {
            const f32 dxz = glm::length(Vec2{t.position.x - feet.x, t.position.z - feet.z});
            const f32 canopy = 2.6f * t.scale;
            const f32 alpha = dxz < canopy ? glm::mix(0.18f, 1.0f, dxz / canopy) : 1.0f;
            renderer_->draw_transparent(tree_library_[tree_index(t)].foliage, tree_model(t),
                                        Vec4{t.tint, alpha});
        });
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
        visuals_.clear();
        shape_box_.destroy();
        shape_sphere_.destroy();
        shape_cylinder_.destroy();
        for (auto& tv : tree_library_) {
            tv.trunk.destroy();
            tv.foliage.destroy();
        }
        tree_library_.clear();
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
        Vec3 last_pos{0.0f};
        f32 speed = 0.0f;
        bool has_last = false;
    };

    PlayerVisual& ensure_visual(net::PlayerId id) {
        const auto it = visuals_.find(id);
        if (it != visuals_.end()) {
            return it->second;
        }
        PlayerVisual v;
        v.model = CharacterModel::generate(id);
        return visuals_.emplace(id, std::move(v)).first->second;
    }

    void update_camera() {
        const f32 cam_yaw = radians(iso::yaw_deg);
        const f32 cam_pitch = radians(iso::pitch_deg);
        const Vec3 dir_to_cam{std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                              std::cos(cam_pitch) * std::sin(cam_yaw)};
        const Vec3 target = local_feet() + Vec3{0.0f, 0.7f, 0.0f};
        const Vec3 eye = target + dir_to_cam * iso::distance;
        camera_.set_perspective(radians(iso::fov_deg), renderer_->aspect(), 0.5f, 400.0f);
        camera_.look_at(eye, target);
    }

    void update_visuals(Timestep dt) {
        if (!have_snapshot_) {
            return;
        }
        for (const net::PlayerState& p : snapshot_.players) {
            PlayerVisual& v = ensure_visual(p.id);
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

    void draw_character(PlayerVisual& v, const Vec3& feet, f32 yaw) {
        const Mat4 root = glm::translate(Mat4{1.0f}, feet) *
                          glm::rotate(Mat4{1.0f}, HalfPi - yaw, Vec3{0.0f, 1.0f, 0.0f});
        const std::vector<Quat> pose = v.animator.pose(v.model);
        const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
        const std::vector<Bone>& bones = v.model.bones();
        const CharacterPalette& pal = v.model.palette();
        for (usize i = 0; i < bones.size(); ++i) {
            const Vec3 color = bones[i].color == BoneColor::Skin    ? pal.skin
                               : bones[i].color == BoneColor::Shirt ? pal.shirt
                                                                    : pal.pants;
            const Mesh& shape = bones[i].shape == BoneShape::Sphere     ? shape_sphere_
                                : bones[i].shape == BoneShape::Cylinder ? shape_cylinder_
                                                                        : shape_box_;
            renderer_->draw(shape, mats[i], Vec4{color, 1.0f});
        }
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
        if (Input* in = input()) {
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
        packet.aim = aim_;
        client_.send_input(packet);
        pending_dig_ = false;
        pending_add_ = false;
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

    std::string host_;
    bool host_local_ = true;
    GameServer local_server_;
    Renderer* renderer_ = nullptr;
    net::NetClient client_;
    std::unique_ptr<StreamingTerrain> terrain_;
    struct TreeVisual {
        Mesh trunk;
        Mesh foliage;
    };

    std::unordered_map<net::PlayerId, PlayerVisual> visuals_;
    std::vector<TreeVisual> tree_library_;
    Mesh shape_box_;
    Mesh shape_sphere_;
    Mesh shape_cylinder_;
    Mesh marker_;
    Mesh water_mesh_;
    f32 elapsed_ = 0.0f;
    Camera camera_;

    net::PlayerId my_id_ = 0;
    net::Snapshot snapshot_;
    bool have_snapshot_ = false;

    f32 face_yaw_ = 0.0f;
    u32 sequence_ = 0;
    bool pending_dig_ = false;
    bool pending_add_ = false;
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
        ClientApp app{host, !joining, frames};
        app.run();
    }
    return 0;
}
