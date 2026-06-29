// ClientApp - lifecycle, per-frame update/render orchestration and visuals bookkeeping.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

namespace {
// A smooth low-frequency value noise (0..1) over the world XZ plane, used to decide WHICH
// stretches of road are socked in with a dense fog bank (so the mist is occasional, not on
// every road). Features are ~50 m across, so banks come and go as you travel.
f32 fog_bank_noise(f32 x, f32 z) {
    auto vhash = [](i32 ix, i32 iz) {
        u32 h = static_cast<u32>(ix) * 374761393u + static_cast<u32>(iz) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<f32>((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
    };
    constexpr f32 freq = 0.02f;
    const f32 fx = x * freq, fz = z * freq;
    const i32 ix = static_cast<i32>(std::floor(fx)), iz = static_cast<i32>(std::floor(fz));
    f32 tx = fx - static_cast<f32>(ix), tz = fz - static_cast<f32>(iz);
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    return glm::mix(glm::mix(vhash(ix, iz), vhash(ix + 1, iz), tx),
                    glm::mix(vhash(ix, iz + 1), vhash(ix + 1, iz + 1), tx), tz);
}
} // namespace

void ClientApp::on_init() {
    renderer_ = renderer();
    if (renderer_ == nullptr) {
        return;
    }
    if (window() != nullptr) {
        window()->set_cursor_captured(false); // free cursor for click-to-dig
    }

    // Medieval restyle of the shared UI theme: stained dark wood panels framed in aged
    // gold, with warm parchment text - an illuminated-manuscript feel for the menus.
    // (The theme is mutable by design so games can restyle; see UI/Theme.h.)
    {
        ui::Theme& th = ui::theme();
        th.panel = Vec4{0.13f, 0.10f, 0.07f, 0.96f};
        th.panel_border = Vec4{0.74f, 0.56f, 0.28f, 0.55f};
        th.overlay = Vec4{0.04f, 0.03f, 0.02f, 0.62f};
        th.text = Vec4{0.94f, 0.88f, 0.74f, 1.0f};
        th.text_muted = Vec4{0.70f, 0.61f, 0.46f, 1.0f};
        th.accent = Vec4{0.78f, 0.57f, 0.24f, 1.0f};
        th.accent_hover = Vec4{0.93f, 0.73f, 0.36f, 1.0f};
        th.button = Vec4{0.22f, 0.16f, 0.10f, 1.0f};
        th.button_hover = Vec4{0.31f, 0.23f, 0.14f, 1.0f};
        th.button_press = Vec4{0.16f, 0.11f, 0.07f, 1.0f};
        th.track = Vec4{0.24f, 0.18f, 0.11f, 1.0f};
        th.knob = Vec4{0.86f, 0.73f, 0.46f, 1.0f};
        th.radius = 7.0f;
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
    ox_body_mesh_.create(renderer_->device(), build_ox_body());
    ox_leg_mesh_.create(renderer_->device(), build_ox_leg());
    deer_body_mesh_.create(renderer_->device(), build_deer_body());
    deer_leg_mesh_.create(renderer_->device(), build_deer_leg());
    fish_body_mesh_.create(renderer_->device(), build_fish_body());
    // A unit rope segment (along +Y, 0..1) for the verlet harness traces; scaled per link.
    rope_mesh_.create(renderer_->device(),
                      primitives::box(Vec3{-0.5f, 0.0f, -0.5f}, Vec3{0.5f, 1.0f, 0.5f},
                                      Vec3{0.16f, 0.11f, 0.06f}));
    // A cargo crate (rides on the cart bed / spilled on the ground / carried back to the cart) -
    // a framed slatted crate so the goods you escort read as crates, not plain boxes.
    goods_mesh_.create(renderer_->device(),
                       primitives::crate(Vec3{-0.22f, 0.0f, -0.22f}, Vec3{0.22f, 0.44f, 0.22f},
                                         Vec3{0.55f, 0.40f, 0.22f}));
    // A large wave grid that follows the player; the water shader animates it.
    water_mesh_.create(renderer_->device(), primitives::grid(80, 2.0f, Vec3{0.1f, 0.3f, 0.4f}));
    // A unit-length plank bridge, stretched per river crossing where a road bridges a river.
    bridge_mesh_stone_.create(renderer_->device(), PropLibrary::build_arch_bridge().parts[0].mesh);
    bridge_mesh_wood_.create(renderer_->device(), PropLibrary::build_plank_bridge().parts[0].mesh);
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
    // Smoother character shapes (more subdivisions + a stronger bevel) so the rig + gear read as
    // rounded, contoured forms rather than hard blocks.
    shape_sphere_.create(renderer_->device(), primitives::sphere(18, 12, Vec3{1.0f}));
    shape_cylinder_.create(renderer_->device(), primitives::cylinder(16, Vec3{1.0f}));
    shape_capsule_.create(renderer_->device(), primitives::capsule(18, 6, Vec3{1.0f}));
    shape_rounded_.create(renderer_->device(), primitives::rounded_box(0.32f, Vec3{1.0f}));
    shape_quad_.create(renderer_->device(), primitives::grid(1, 1.0f, Vec3{1.0f})); // flat up-quad

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
    upload_props(prop_lib_.crystals(), gpu_crystals_);
    upload_props(prop_lib_.glow_shrooms(), gpu_glow_shrooms_);
    upload_props(prop_lib_.campfires(), gpu_campfires_);
    upload_props(prop_lib_.monuments(), gpu_monuments_);
    upload_props(prop_lib_.watchtowers(), gpu_watchtowers_);

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
        const u32 seed = world_seed();
        if (local_server_.start(kPort, seed)) {
            ALRYN_INFO("Hosting a local listen server on port {} (world seed {})", kPort, seed);
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
    for (auto& [frames, m] : mesh_graveyard_) {
        m.destroy();
    }
    mesh_graveyard_.clear();
    have_snapshot_ = false;
    my_id_ = 0;
    state_ = AppState::Menu;
    show_screen(Screen::Main);
}

void ClientApp::on_update(Timestep dt) {
    elapsed_ += dt.seconds;
    frame_dt_ = glm::clamp(dt.seconds, 1.0f / 240.0f, 1.0f / 20.0f); // clamp so a hitch can't explode the cloth
    // Client-side haul clock for the rush-bonus HUD readout (the server's payout is authoritative).
    haul_elapsed_ = (snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active))
                        ? haul_elapsed_ + dt.seconds
                        : 0.0f;
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
        // World map: drag to pan, scroll to zoom (the camera ignores scroll while it's open).
        if (map_open_) {
            if (Input* in = input()) {
                const Vec2 m = in->mouse_position();
                if (in->mouse_down(0)) {
                    if (map_dragging_) {
                        map_center_ -= (m - map_drag_last_) / std::max(map_ppm_, 1e-4f);
                    }
                    map_dragging_ = true;
                    map_drag_last_ = m;
                } else {
                    map_dragging_ = false;
                }
                const f32 s = in->scroll_delta();
                if (s != 0.0f) {
                    map_zoom_ = glm::clamp(map_zoom_ * std::pow(1.18f, s), 0.4f, 4.0f);
                }
            }
        }
        if (paused_ || map_open_ || skills_open_) {
            aim_valid_ = false; // no dig-marker while a menu / overlay is up
        } else {
            update_aim();
        }
        for (f32& cd : ability_cd_) { // tick the local HUD cooldown estimate
            if (cd > 0.0f) {
                cd -= dt.seconds;
            }
        }
        if (mage_cd_ > 0.0f) { // Mage spell-cooldown estimate
            mage_cd_ -= dt.seconds;
        }
        update_ability_vfx(dt); // buff auras + remote cast VFX
        update_particles(dt);
        send_input();
        update_visuals(dt);
        update_enemy_visuals(dt);
        update_villager_visuals(dt);
        update_cloth_triggers(); // cut cloth on a hit / blow it off in a storm
        tick_mesh_graveyard(); // free retired NPC body meshes once their frames-in-flight have passed
        update_gates(dt);
        update_feedback(dt);
        update_debug(dt);
        update_wagon_smooth(dt); // ease wagon render positions toward the snapshot (kills jitter)
        update_ropes(dt);
        update_deer(dt);
        update_fish(dt);

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
    draw_bridges();
    draw_fires();
    draw_barricades();
    draw_walls();
    draw_wagons();
    draw_goods();
    draw_auras();
    draw_shields();
    draw_buffs();
    draw_particles();
    draw_deer();         // ambient wildlife grazing in the meadows
    draw_fish();         // ambient fish swimming in nearby water (opaque - the water tints them)
    draw_surf();         // foam waves lapping along the nearby shoreline
    draw_ambient_life(); // flock of birds by day, fireflies + an owl by night

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
            } else if (pr.kind == 5) { // Mage fireball: a blazing orange orb + glow
                renderer_->draw_emissive(shape_sphere_,
                                         glm::translate(Mat4{1.0f}, pr.position) *
                                             glm::scale(Mat4{1.0f}, Vec3{0.34f}),
                                         Vec4{1.0f, 0.55f, 0.18f, 1.0f});
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, pr.position) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.85f}),
                                     Vec4{1.0f, 0.45f, 0.15f, 0.55f});
            } else if (pr.kind == 6) { // Mage frost bolt: a pale cyan shard + glow
                renderer_->draw_emissive(shape_sphere_,
                                         glm::translate(Mat4{1.0f}, pr.position) *
                                             glm::scale(Mat4{1.0f}, Vec3{0.28f}),
                                         Vec4{0.6f, 0.86f, 1.0f, 1.0f});
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, pr.position) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.6f}),
                                     Vec4{0.5f, 0.8f, 1.0f, 0.45f});
            } else if (pr.kind == 7) { // Mage boulder: a tumbling grey rock
                renderer_->draw(shape_sphere_,
                                glm::translate(Mat4{1.0f}, pr.position) *
                                    glm::rotate(Mat4{1.0f}, elapsed_ * 6.0f, Vec3{0.3f, 1.0f, 0.2f}) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.45f}),
                                Vec4{0.4f, 0.37f, 0.34f, 1.0f});
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

    draw_rain();    // world-space falling streaks (depth-tested against the scene)
    draw_weather(); // screen-space lightning flash, behind the HUD
    draw_health_bars();
    draw_hud();
    if (map_open_) {
        draw_map();
    }
    if (skills_open_) {
        draw_skills();
    }
    if (wardrobe_open_) {
        draw_wardrobe();
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
    visuals_.clear();           // PlayerVisuals own dynamic body/outfit GPU meshes (freed via ~Mesh)
    enemy_visuals_.clear();     // EnemyVisuals own a dynamic body mesh
    villager_visuals_.clear();
    for (auto& [frames, m] : mesh_graveyard_) {
        m.destroy();
    }
    mesh_graveyard_.clear();
    shape_box_.destroy();
    shape_sphere_.destroy();
    shape_cylinder_.destroy();
    shape_capsule_.destroy();
    shape_rounded_.destroy();
    shape_quad_.destroy();
    for (auto& tv : tree_library_) {
        tv.trunk.destroy();
        tv.foliage.destroy();
    }
    tree_library_.clear();
    for (std::vector<GpuProp>* set : {&gpu_bushes_, &gpu_rocks_, &gpu_logs_, &gpu_fences_,
                                      &gpu_fence_rails_, &gpu_lanterns_, &gpu_houses_,
                                      &gpu_walls_, &gpu_gates_, &gpu_wells_, &gpu_bridges_,
                                      &gpu_markets_, &gpu_paths_, &gpu_planters_,
                                      &gpu_fountains_, &gpu_decor_, &gpu_rivers_, &gpu_crystals_,
                                      &gpu_glow_shrooms_, &gpu_campfires_,
                                      &gpu_monuments_, &gpu_watchtowers_}) {
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
    ox_body_mesh_.destroy();
    ox_leg_mesh_.destroy();
    deer_body_mesh_.destroy();
    deer_leg_mesh_.destroy();
    fish_body_mesh_.destroy();
    rope_mesh_.destroy();
    goods_mesh_.destroy();
    water_mesh_.destroy();
    bridge_mesh_stone_.destroy();
    bridge_mesh_wood_.destroy();
    gate_door_mesh_.destroy();
    terrain_.reset();
    client_.disconnect();
    local_server_.stop();
}

ClientApp::PlayerVisual& ClientApp::ensure_visual(net::PlayerId id,
                                                  const CharacterAppearance& appearance, u8 role,
                                                  const Equipment& equipment) {
    auto build = [&](PlayerVisual& v) {
        v.appearance = appearance;
        v.role = role;
        v.equipment = equipment;
        v.model = CharacterModel::create(id, appearance);
        const OutfitKind kind = outfit_kind_for_role(role);
        apply_outfit(v.model, kind, equipment); // the networked, server-clamped gear (decorative attachments)
        v.body_skin = build_body_mesh(v.model);            // continuous body for this character's proportions
        v.outfit_skin = build_outfit_mesh(v.model, kind, equipment); // continuous worn armour/cloth
        setup_cloth(v, static_cast<PlayerRole>(role % kRoleCount), equipment); // flowing simulated cloth
    };
    const auto it = visuals_.find(id);
    if (it != visuals_.end()) {
        // Rebuild the model if the player changed their look, role, or gear.
        if (!(it->second.appearance == appearance) || it->second.role != role ||
            !(it->second.equipment == equipment)) {
            build(it->second);
        }
        return it->second;
    }
    PlayerVisual v;
    build(v);
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

    const Vec3 horizon{1.0f, 0.46f, 0.24f}; // deep warm gold at the horizon (golden hour)
    const Vec3 noon{1.0f, 0.93f, 0.78f};    // warm daylight (not a clinical white)
    const Vec3 sun_color = glm::mix(horizon, noon, glm::smoothstep(0.0f, 0.32f, h));

    const Vec3 sky_night{0.03f, 0.04f, 0.09f};
    const Vec3 sky_day{0.34f, 0.55f, 0.82f}; // a clear, vibrant daytime blue
    const Vec3 sky_dusk{0.92f, 0.44f, 0.26f};
    Vec3 sky = glm::mix(sky_night, sky_day, intensity);
    const f32 dusk = glm::clamp(1.0f - std::abs(h) * 3.5f, 0.0f, 1.0f) * intensity;
    sky = glm::mix(sky, sky_dusk, dusk * 0.6f);

    // Weather: a rolling storm (networked, server-authoritative) greys + darkens the sky, dims +
    // cools the sun, whips up the wind (plants thrash), and - in a heavy storm - flickers
    // lightning. Eased so it builds in smoothly as you travel into it.
    const f32 weather_target = have_snapshot_ ? static_cast<f32>(snapshot_.weather) / 255.0f : 0.0f;
    weather_amt_ += (weather_target - weather_amt_) * std::min(1.0f, dt.seconds * 0.5f);
    const f32 wz = weather_amt_;
    const Vec3 storm_sky = glm::mix(Vec3{0.10f, 0.11f, 0.14f}, Vec3{0.32f, 0.34f, 0.39f}, intensity);
    sky = glm::mix(sky, storm_sky, wz * 0.85f);
    const f32 storm_intensity = intensity * (1.0f - 0.6f * wz); // sun smothered by cloud
    sun_intensity_ = storm_intensity;                           // so lanterns/windows light up
    const Vec3 storm_sun = glm::mix(sun_color, Vec3{0.62f, 0.64f, 0.70f}, wz * 0.7f);

    renderer_->set_sun(sun_dir, storm_sun, storm_intensity);
    renderer_->set_sky_color(sky);
    renderer_->set_wind(0.12f + wz * 0.7f);

    // Lightning flashes in a heavy storm (decays fast; thunder would need an audio system).
    if (wz > 0.55f) {
        lightning_cd_ -= dt.seconds;
        if (lightning_cd_ <= 0.0f) {
            lightning_ = 1.0f;
            lightning_cd_ = frand(2.5f, 7.0f) / std::max(wz, 0.5f);
        }
    }
    lightning_ = std::max(0.0f, lightning_ - dt.seconds * 3.5f);

    // Atmospheric fog (the V-Rising/Valheim depth haze). The fog colour tracks the sky but
    // desaturated, so distant land melts into the horizon; nights + dawn carry more mist, and
    // a town wraps the player in a gloomier, denser, slightly cooler haze.
    const f32 night = 1.0f - intensity;
    const Vec3 feet = local_feet();
    const f32 gloom_target =
        worldgen::inside_village(feet.x, feet.z, world_seed_, 6.0f) ? 1.0f : 0.0f;
    fog_gloom_ += (gloom_target - fog_gloom_) * std::min(1.0f, dt.seconds * 2.0f); // eased
    const f32 sky_lum = glm::dot(sky, Vec3{0.2126f, 0.7152f, 0.0722f});
    Vec3 fog_color = glm::mix(sky, Vec3{sky_lum}, 0.18f);                  // keep the haze coloured (atmospheric)
    fog_color = glm::mix(fog_color, Vec3{0.10f, 0.12f, 0.16f}, fog_gloom_ * 0.35f); // town gloom
    fog_color = glm::mix(fog_color, Vec3{sky_lum * 0.9f}, wz * 0.4f);                // grey storm haze
    const f32 density =
        0.0058f + night * 0.0035f + fog_gloom_ * 0.003f + wz * 0.008f; // a touch more distance haze (depth)

    // Occasional dense fog banks on the roads: near a road, a slow noise along the way decides
    // which stretches are socked in. Eased slowly so you walk smoothly into and out of a bank.
    const f32 road_dist = roads::distance(feet.x, feet.z, world_seed_);
    const f32 near_road = 1.0f - glm::smoothstep(4.0f, 16.0f, road_dist); // 1 on the road
    const f32 bank = glm::smoothstep(0.58f, 0.82f, fog_bank_noise(feet.x, feet.z));
    const f32 patch_target = near_road * bank;
    fog_patch_ += (patch_target - fog_patch_) * std::min(1.0f, dt.seconds * 0.7f);
    renderer_->set_fog(fog_color, density, fog_gloom_, fog_patch_);
}

void ClientApp::update_camera() {
    // Scroll wheel zooms by scaling the camera pull-back distance (the map consumes scroll instead
    // while it's open, so the camera doesn't lurch behind the overlay).
    if (Input* in = input(); in != nullptr && !map_open_) {
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
    const Vec3 want = local_feet() + Vec3{0.0f, cam::target_height, 0.0f};
    const f32 dt = glm::clamp(frame_dt_, 0.0001f, 0.1f); // clamp so a frame hitch can't fling the camera
    // Follow-smoothing: the camera target GLIDES toward the player instead of snapping every frame (a
    // softer, less rigid feel), but SNAPS on a big jump (first frame / respawn / teleport) so it never
    // sails across the world.
    if (glm::length(want - cam_target_) > 8.0f) {
        cam_target_ = want;
    } else {
        cam_target_ += (want - cam_target_) * (1.0f - std::exp(-12.0f * dt));
    }
    // Combat zoom: pull in slightly while an ambush is on (enemies present) for intensity, ease back out.
    const f32 zoom_goal = snapshot_.enemies.empty() ? 1.0f : 0.86f;
    combat_zoom_ += (zoom_goal - combat_zoom_) * (1.0f - std::exp(-2.5f * dt));
    Vec3 eye = cam_target_ + dir_to_cam * (cam_distance_ * combat_zoom_);
    // Camera-terrain collision: never let the eye sink into a hillside - going up a hill, the fixed
    // iso offset can bury the camera in the slope, clipping through the ground. Lift the eye to stay a
    // clearance above the terrain at its own position (smooth, since the height field is continuous).
    if (world_seed_ != 0) {
        const f32 floor = worldgen::height(eye.x, eye.z, world_seed_) + 1.4f;
        if (eye.y < floor) {
            eye.y = floor;
        }
    }
    camera_.set_perspective(radians(iso::fov_deg), renderer_->aspect(), cam::near_plane,
                            cam::far_plane);
    camera_.look_at(eye, cam_target_);
}

void ClientApp::update_visuals(Timestep dt) {
    if (!have_snapshot_) {
        return;
    }
    for (const net::PlayerState& p : snapshot_.players) {
        PlayerVisual& v = ensure_visual(p.id, p.appearance, p.role, p.equipment);
        f32 measured = 0.0f;
        if (v.has_last && dt.seconds > 0.0001f) {
            Vec3 d = p.position - v.last_pos;
            d.y = 0.0f;
            measured = glm::length(d) / dt.seconds;
        }
        v.speed = glm::mix(v.speed, measured, 0.3f);
        v.last_pos = p.position;
        v.has_last = true;

        // Wading splash: moving through water kicks up droplets + a ripple at the waterline. Paced
        // by distance travelled while submerged, and only for players near enough to see.
        const f32 wsub = worldgen::water_level - p.position.y;
        if (wsub > 0.06f && v.speed > 0.8f &&
            glm::length(p.position - local_feet()) < 55.0f) {
            v.splash_acc += v.speed * dt.seconds;
            if (v.splash_acc > 0.55f) {
                v.splash_acc = 0.0f;
                emit_splash(Vec3{p.position.x, worldgen::water_level, p.position.z}, v.speed);
            }
        } else {
            v.splash_acc = 0.0f;
        }

        // Drive the action layer. The local player uses its own input for zero-latency
        // feedback; remote players follow the networked `action` field.
        const bool is_local = p.id == my_id_;
        v.animator.set_blocking(is_local ? blocking_ : (p.action == 2));
        // Pick the attack animation by the role's weapon: a ranged bow/staff casts (forward thrust),
        // a melee sword/mace swings.
        auto attack_anim = [&]() {
            const WeaponType wt = role_weapon(p.role, 0);
            if (wt == WeaponType::Bow || wt == WeaponType::Staff) {
                v.animator.play_cast();
            } else {
                v.animator.play_swing();
            }
        };
        if (is_local) {
            if (pending_local_swing_) {
                attack_anim();
            }
        } else if (p.action == 1 && v.last_action != 1) {
            attack_anim(); // rising edge of a remote attack
        }
        // Dodge roll: kick up a dust puff at the feet on the rising edge (local + remote).
        if (p.action == 3 && v.last_action != 3) {
            emit_burst(p.position + Vec3{0.0f, 0.12f, 0.0f}, Vec4{0.74f, 0.69f, 0.58f, 0.65f}, 12,
                       2.4f, 0.45f, 0.14f, 1, 0.5f, 3.5f);
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

    // Hit marker: the server bumps the local player's hit_fx counter whenever one of our
    // attacks lands a confirmed hit (melee swing, arrow, bolt, thrown rock - all of them).
    // When it ticks up we pop a crisp screen-centre marker + a little impact spark, so every
    // attack that connects feels like it did something.
    if (const net::PlayerState* lp = local_player()) {
        if (!hit_fx_init_) {
            last_hit_fx_ = lp->hit_fx; // adopt the first value so joining mid-fight doesn't false-pop
            hit_fx_init_ = true;
        } else if (lp->hit_fx != last_hit_fx_) {
            hit_marker_ = 1.0f;
            // A burst of sparks at the point we're aiming at (the enemy we struck is right there).
            if (aim_valid_) {
                emit_burst(aim_ + Vec3{0.0f, 0.9f, 0.0f}, Vec4{1.0f, 0.92f, 0.6f, 1.0f}, 14, 4.5f, 0.35f,
                           0.12f, /*style=*/1, /*up=*/1.5f);
            }
            last_hit_fx_ = lp->hit_fx;
        }
    }
    hit_marker_ = std::max(0.0f, hit_marker_ - dt.seconds * 3.2f);
}

void ClientApp::update_debug(Timestep dt) {
    // Client frame rate, sampled over a ~0.4 s window so the number is readable, not jittery.
    fps_accum_ += dt.seconds;
    ++fps_frames_;
    if (fps_accum_ >= 0.4f) {
        fps_ = static_cast<f32>(fps_frames_) / fps_accum_;
        frame_ms_ = 1000.0f * fps_accum_ / static_cast<f32>(fps_frames_);
        fps_accum_ = 0.0f;
        fps_frames_ = 0;
    }
    // Server tick rate, measured from how fast the authoritative snapshot tick advances (works for a
    // listen server AND a remote one, since it just reads the networked tick counter).
    if (have_snapshot_) {
        if (snapshot_.tick > tps_last_tick_) {
            tps_ticks_ += snapshot_.tick - tps_last_tick_;
        }
        tps_last_tick_ = snapshot_.tick;
    }
    tps_accum_ += dt.seconds;
    if (tps_accum_ >= 0.4f) {
        server_tps_ = static_cast<f32>(tps_ticks_) / tps_accum_;
        tps_accum_ = 0.0f;
        tps_ticks_ = 0;
    }
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
