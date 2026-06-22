// ClientApp - lifecycle, per-frame update/render orchestration and visuals bookkeeping.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::on_init() {
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
        day_seconds_ = std::max(daynight::min_day_seconds, static_cast<f32>(std::atof(d)));
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
    // A unit gate-door leaf: a planked panel hinged at local x=0 extending to x=1, unit-tall in
    // y, thin in z, with two iron bands. Scaled to the gate size + swung open by the client.
    {
        MeshData door = primitives::box(Vec3{0.0f, 0.0f, -0.06f}, Vec3{1.0f, 1.0f, 0.06f},
                                        Vec3{0.40f, 0.27f, 0.14f});
        auto append = [&](const MeshData& b) {
            const u32 base = static_cast<u32>(door.vertices.size());
            door.vertices.insert(door.vertices.end(), b.vertices.begin(), b.vertices.end());
            for (u32 i : b.indices) {
                door.indices.push_back(base + i);
            }
        };
        const Vec3 iron{0.22f, 0.21f, 0.22f};
        append(primitives::box(Vec3{0.04f, 0.16f, -0.08f}, Vec3{0.96f, 0.27f, 0.08f}, iron));
        append(primitives::box(Vec3{0.04f, 0.66f, -0.08f}, Vec3{0.96f, 0.77f, 0.08f}, iron));
        gate_door_mesh_.create(renderer_->device(), door);
    }
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
    upload_props(prop_lib_.decor(), gpu_decor_);
    upload_props(prop_lib_.rivers(), gpu_rivers_);

    // Skip the menu when launched for scripted/CI runs (--host=... or a fixed
    // frame count); otherwise open the main menu and let the player choose.
    if (auto_start_) {
        enter_game(host_local_, host_);
    } else {
        show_screen(Screen::Main);
    }
}

void ClientApp::enter_game(bool host_local, std::string host) {
    host_ = std::move(host);
    host_local_ = host_local;
    if (host_local_) {
        if (local_server_.start(kPort, kWorldSeed)) {
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

void ClientApp::return_to_menu() {
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

void ClientApp::on_update(Timestep dt) {
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
        update_gates(dt);
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

void ClientApp::on_render() {
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
    draw_gates();
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

void ClientApp::on_shutdown() {
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
                                      &gpu_fountains_, &gpu_decor_, &gpu_rivers_}) {
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
    gate_door_mesh_.destroy();
    terrain_.reset();
    client_.disconnect();
    local_server_.stop();
}

ClientApp::PlayerVisual& ClientApp::ensure_visual(net::PlayerId id,
                                                  const CharacterAppearance& appearance) {
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

void ClientApp::update_day_night(Timestep dt) {
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

void ClientApp::update_camera() {
    // Scroll wheel zooms by scaling the camera pull-back distance.
    if (Input* in = input()) {
        const f32 s = in->scroll_delta();
        if (s != 0.0f) {
            cam_distance_ = glm::clamp(cam_distance_ * std::pow(cam::zoom_step, s),
                                       cam::min_distance, cam::max_distance);
        }
    }
    const f32 cam_yaw = radians(iso::yaw_deg);
    const f32 cam_pitch = radians(iso::pitch_deg);
    const Vec3 dir_to_cam{std::cos(cam_pitch) * std::cos(cam_yaw), std::sin(cam_pitch),
                          std::cos(cam_pitch) * std::sin(cam_yaw)};
    const Vec3 target = local_feet() + Vec3{0.0f, cam::target_height, 0.0f};
    const Vec3 eye = target + dir_to_cam * cam_distance_;
    camera_.set_perspective(radians(iso::fov_deg), renderer_->aspect(), cam::near_plane,
                            cam::far_plane);
    camera_.look_at(eye, target);
}

void ClientApp::update_visuals(Timestep dt) {
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

void ClientApp::update_feedback(Timestep dt) {
    const f32 hp = local_health();
    if (hp < last_health_ - 0.001f) {
        hit_flash_ = 1.0f;
    }
    last_health_ = hp;
    hit_flash_ = std::max(0.0f, hit_flash_ - dt.seconds * 1.8f);
}

ApplicationConfig ClientApp::make_config(u64 max_frames) {
    ApplicationConfig config;
    config.name = "Alryn Client";
    config.width = 1280;
    config.height = 720;
    config.headless = false;
    config.max_frames = max_frames;
    return config;
}

} // namespace alryn::game
