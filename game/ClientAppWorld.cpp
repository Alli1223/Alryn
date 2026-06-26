// ClientApp - enemies, villagers, props and fire rendering + their visuals.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

CharacterAppearance ClientApp::enemy_look() {
    CharacterAppearance a;
    a.skin = 0;
    a.hair_color = 0;
    a.eyes = EyeStyle::Sharp;
    a.ears = EarStyle::Pointed;
    a.hair = HairStyle::Spiky;
    return a;
}

void ClientApp::update_enemy_visuals(Timestep dt) {
    if (!have_snapshot_) {
        return;
    }
    for (const net::EnemyState& en : snapshot_.enemies) {
        const auto [it, created] = enemy_visuals_.try_emplace(en.id);
        EnemyVisual& v = it->second;
        if (created) {
            v.model = CharacterModel::create(en.id ^ 0xE0E0u, enemy_look());
            v.body_skin = build_body_mesh(v.model);
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
        if (live) {
            ++it;
        } else {
            retire_mesh(std::move(it->second.body_mesh)); // defer the GPU free past the frames in flight
            it = enemy_visuals_.erase(it);
        }
    }
}

void ClientApp::draw_enemies() {
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
        // 3 = archer (sickly green, carries a bow), 4 = shield-bearer (steel, carries a shield),
        // 5 = healer (pale mystic, floats a glowing green orb).
        const f32 scale = en.kind == 2 ? 1.5f : 1.0f;
        const Mat4 root = glm::translate(Mat4{1.0f}, en.position) *
                          glm::rotate(Mat4{1.0f}, HalfPi - en.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                          glm::scale(Mat4{1.0f}, Vec3{scale}) * v.animator.body_offset();
        const Vec3 tint = en.kind == 1   ? Vec3{1.5f, 0.7f, 0.18f}
                          : en.kind == 2 ? Vec3{1.05f, 0.26f, 0.4f}
                          : en.kind == 3 ? Vec3{0.5f, 0.85f, 0.45f}
                          : en.kind == 4 ? Vec3{0.62f, 0.66f, 0.78f}
                          : en.kind == 5 ? Vec3{0.78f, 0.82f, 0.70f}
                          : en.kind == kEnemySapper ? Vec3{0.5f, 0.42f, 0.30f} // dark, hunched
                                                    : Vec3{1.3f, 0.32f, 0.3f};
        const std::vector<Quat> pose = v.animator.pose(v.model);
        if (v.body_skin.vertices.empty()) {
            v.body_skin = build_body_mesh(v.model);
        }
        skin_and_draw(v.model, v.body_skin, v.body_mesh, root, pose, tint); // continuous skinned body
        const std::vector<Mat4> emats = v.model.bone_matrices(root, pose);
        draw_rig(v.model, emats, tint, /*attachments_only=*/true); // face/hair on top
        // The lone last raider is ENRAGED (server gives it a speed/damage boost) - a red angry aura
        // so the climax reads (derived from the snapshot: one non-brute ambusher left).
        if (snapshot_.enemies.size() == 1 && en.kind != 2) {
            const f32 puls = 0.6f + 0.4f * std::sin(elapsed_ * 14.0f);
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, en.position + Vec3{0.0f, 1.0f, 0.0f}) *
                                     glm::scale(Mat4{1.0f}, Vec3{1.3f}),
                                 Vec4{1.0f, 0.18f, 0.12f, 0.4f * puls});
        }
        if (en.kind == 2 && (en.action == 2 || en.action == 3)) {
            // Brute slam telegraph: a pulsing red danger ring on the ground while it winds up
            // (action 2 - dodge out of it!), then a bright shockwave burst on the strike (action 3).
            const Vec3 c{en.position.x, en.position.y + 0.06f, en.position.z};
            if (en.action == 2) {
                const f32 puls = 0.5f + 0.35f * std::sin(elapsed_ * 12.0f);
                renderer_->draw_transparent(
                    shape_sphere_,
                    glm::translate(Mat4{1.0f}, c) *
                        glm::scale(Mat4{1.0f}, Vec3{kSlamRadius, 0.05f, kSlamRadius}),
                    Vec4{0.95f, 0.2f, 0.15f, 0.28f + 0.18f * puls});
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, c) *
                                         glm::scale(Mat4{1.0f}, Vec3{kSlamRadius * 1.03f, 0.04f,
                                                                     kSlamRadius * 1.03f}),
                                     Vec4{1.0f, 0.3f, 0.2f, 0.32f * puls});
            } else { // action 3: the slam landed
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, c + Vec3{0.0f, 0.2f, 0.0f}) *
                                         glm::scale(Mat4{1.0f}, Vec3{kSlamRadius * 1.2f, 0.5f,
                                                                     kSlamRadius * 1.2f}),
                                     Vec4{1.0f, 0.55f, 0.25f, 0.6f});
            }
        }
        if (en.kind == kEnemySapper) {
            // A lit-fuse satchel charge on its back: a dark box + a fast-sparking emissive fuse tip,
            // so the bomber reads as "intercept me before I reach the cart!".
            const Vec3 back = en.position -
                              Vec3{std::cos(en.yaw), 0.0f, std::sin(en.yaw)} * 0.22f +
                              Vec3{0.0f, 1.05f, 0.0f};
            renderer_->draw(shape_box_,
                            glm::translate(Mat4{1.0f}, back) *
                                glm::scale(Mat4{1.0f}, Vec3{0.26f, 0.26f, 0.18f}),
                            Vec4{0.22f, 0.18f, 0.13f, 1.0f}); // satchel charge
            const f32 spark = 0.6f + 0.4f * std::sin(elapsed_ * 26.0f + en.position.x);
            const Vec3 fuse = back + Vec3{0.0f, 0.22f, 0.0f};
            renderer_->draw_emissive(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, fuse) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.09f * spark}),
                                     Vec4{1.0f, 0.7f, 0.2f, 1.0f});
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, fuse) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.42f}),
                                 Vec4{1.0f, 0.5f, 0.15f, 0.5f * spark});
        }
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
            if (en.action == 2) {
                // Aiming a heavy shot: a charging glow swells at the bow (telegraph - dodge it!).
                const f32 chg = 0.5f + 0.5f * std::sin(elapsed_ * 16.0f);
                renderer_->draw_glow(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, hand) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.28f + 0.1f * chg}),
                                     Vec4{1.0f, 0.4f, 0.18f, 0.55f});
                renderer_->draw_emissive(shape_sphere_,
                                         glm::translate(Mat4{1.0f}, hand) *
                                             glm::scale(Mat4{1.0f}, Vec3{0.10f}),
                                         Vec4{1.0f, 0.7f, 0.3f, 1.0f});
            }
        }
        if (en.kind == 5) {
            // A healer floats a pulsing green orb of mending magic above its hand + a soft glow.
            const f32 puls = 0.85f + 0.15f * std::sin(elapsed_ * 6.0f + en.position.x);
            const Vec3 orb = en.position + Vec3{std::cos(en.yaw), 0.0f, std::sin(en.yaw)} * 0.42f +
                             Vec3{0.0f, 1.35f, 0.0f};
            renderer_->draw_emissive(shape_sphere_,
                                     glm::translate(Mat4{1.0f}, orb) *
                                         glm::scale(Mat4{1.0f}, Vec3{0.16f * puls}),
                                     Vec4{0.4f, 1.0f, 0.5f, 1.0f});
            renderer_->draw_glow(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, orb) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.5f}),
                                 Vec4{0.4f, 1.0f, 0.5f, 0.35f * puls});
        }
        if (en.kind == 4) {
            // A large round shield held out front (a wide wooden plate + a steel boss),
            // facing the way it marches - the side it blocks from.
            const Vec3 fwd{std::cos(en.yaw), 0.0f, std::sin(en.yaw)};
            const Vec3 mid = en.position + fwd * 0.5f + Vec3{0.0f, 1.0f, 0.0f};
            const Mat4 face = glm::translate(Mat4{1.0f}, mid) *
                              glm::rotate(Mat4{1.0f}, -en.yaw, Vec3{0.0f, 1.0f, 0.0f});
            renderer_->draw(shape_box_,
                            face * glm::scale(Mat4{1.0f}, Vec3{0.07f, 0.62f, 0.52f}),
                            Vec4{0.40f, 0.28f, 0.16f, 1.0f}); // wooden plank face
            renderer_->draw(shape_sphere_,
                            glm::translate(Mat4{1.0f}, mid + fwd * 0.06f) *
                                glm::scale(Mat4{1.0f}, Vec3{0.06f, 0.16f, 0.16f}),
                            Vec4{0.66f, 0.70f, 0.80f, 1.0f}); // steel boss
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

// Rebuilds the list of town gates near the player and eases each toward open (1) when a
// networked player / villager / enemy is close, else shut (0). Client-side + visual only.
void ClientApp::update_gates(Timestep dt) {
    gates_.clear();
    if (!have_snapshot_ || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    const f32 ease = glm::clamp(dt.seconds * 4.0f, 0.0f, 1.0f);
    constexpr f32 kOpenRange = 8.0f; // doors begin opening within this of the gate
    const int vcx = static_cast<int>(std::floor(feet.x / worldgen::village_cell));
    const int vcz = static_cast<int>(std::floor(feet.z / worldgen::village_cell));
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto v = worldgen::village_at(vcx + dx, vcz + dz, world_seed_);
            if (!v) {
                continue;
            }
            for (const detail::VillageGate& g : detail::village_gate_points(*v, world_seed_)) {
                if (glm::length(Vec2{g.pos.x - feet.x, g.pos.y - feet.z}) > 95.0f) {
                    continue; // only animate gates near the player
                }
                f32 nd = 1e9f;
                auto consider = [&](const Vec3& p) {
                    nd = std::min(nd, glm::length(Vec2{p.x - g.pos.x, p.z - g.pos.y}));
                };
                for (const net::PlayerState& p : snapshot_.players) consider(p.position);
                for (const net::VillagerState& vl : snapshot_.villagers) consider(vl.position);
                for (const net::EnemyState& en : snapshot_.enemies) consider(en.position);
                const f32 target = glm::smoothstep(kOpenRange, kOpenRange * 0.55f, nd);
                const u64 id = (static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(g.pos.x)))) << 32) |
                               static_cast<u64>(static_cast<u32>(static_cast<i32>(std::lround(g.pos.y))));
                f32& o = gate_open_[id];
                o = glm::mix(o, target, ease);
                const f32 gy = worldgen::height(g.pos.x, g.pos.y, world_seed_);
                gates_.push_back(GateVisual{Vec3{g.pos.x, gy, g.pos.y},
                                            glm::normalize(g.pos - v->center), g.half, o});
            }
        }
    }
}

// Draws each nearby gate as two planked door leaves, hinged at the sides of the opening and
// swinging outward (from across the gap when shut to pointing along the radial when open).
void ClientApp::draw_gates() {
    if (renderer_ == nullptr) {
        return;
    }
    constexpr f32 doorH = 2.6f; // a touch taller than the wall
    const Vec4 wood{0.46f, 0.32f, 0.18f, 1.0f};
    for (const GateVisual& g : gates_) {
        const f32 leaf_w = g.half; // each leaf spans the opening half-width (wider for multi-road gates)
        const Vec2 rad = g.radial;
        const Vec2 tang{-rad.y, rad.x}; // along the wall, across the opening
        const Vec2 gxz{g.pos.x, g.pos.z};
        const f32 open_yaw = std::atan2(-rad.y, rad.x); // fully open: leaf points outward
        for (f32 side : {-1.0f, 1.0f}) {
            const Vec2 hinge = gxz + tang * (side * leaf_w);
            const Vec2 closed = -side * tang; // hinge -> opening centre when shut
            const f32 base_yaw = std::atan2(-closed.y, closed.x);
            f32 delta = open_yaw - base_yaw; // shortest path closed -> open (~90 deg)
            while (delta > Pi) delta -= TwoPi;
            while (delta < -Pi) delta += TwoPi;
            const f32 yaw = base_yaw + delta * g.open;
            const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{hinge.x, g.pos.y, hinge.y}) *
                           glm::rotate(Mat4{1.0f}, yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{leaf_w, doorH, 1.0f});
            renderer_->draw(gate_door_mesh_, m, wood);
        }
    }
}

void ClientApp::draw_bridges() {
    if (renderer_ == nullptr || world_seed_ == 0) {
        return;
    }
    const Vec3 feet = local_feet();
    for (const roads::Bridge& b : roads::bridges(Vec2{feet.x, feet.z}, 100.0f, world_seed_)) {
        // The deck meets each bank flush (no lip): position at the mid-bank height and PITCH the span
        // so the back end sits at bank_a and the front end at bank_b; the mesh adds the arch hump. This
        // matches roads::bridge_deck_y exactly, so the visible deck is the walkable deck.
        const f32 base_y = (b.bank_a + b.bank_b) * 0.5f;
        const f32 pitch = std::asin(glm::clamp((b.bank_b - b.bank_a) / b.length, -0.6f, 0.6f));
        const Mat4 m = glm::translate(Mat4{1.0f}, Vec3{b.center.x, base_y, b.center.y}) *
                       glm::rotate(Mat4{1.0f}, -b.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                       glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
                       glm::scale(Mat4{1.0f}, Vec3{b.length, 1.0f, 1.0f});
        renderer_->draw(b.kind == 1 ? bridge_mesh_wood_ : bridge_mesh_stone_, m, Vec4{1.0f});
    }
}

void ClientApp::draw_barricades() {
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

void ClientApp::draw_fires() {
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

f32 ClientApp::house_burn(const Vec3& p) const {
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

void ClientApp::draw_prop(const PropInstance& p) {
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
        : p.category == PropCategory::Fountain  ? gpu_fountains_
        : p.category == PropCategory::Decor     ? gpu_decor_
        : p.category == PropCategory::River     ? gpu_rivers_
        : p.category == PropCategory::Crystal   ? gpu_crystals_
        : p.category == PropCategory::GlowShroom ? gpu_glow_shrooms_
        : p.category == PropCategory::Campfire  ? gpu_campfires_
        : p.category == PropCategory::Monument  ? gpu_monuments_
        : p.category == PropCategory::Watchtower ? gpu_watchtowers_
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
    const bool near_player = glm::length(p.position - local_feet()) < light::prop_cull_dist;
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

void ClientApp::update_villager_visuals(Timestep dt) {
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
        if (live) {
            ++it;
        } else {
            retire_mesh(std::move(it->second.body_mesh)); // defer the GPU free past the frames in flight
            retire_mesh(std::move(it->second.outfit_mesh));
            it = villager_visuals_.erase(it);
        }
    }
}

ClientApp::PlayerVisual& ClientApp::ensure_villager_visual(u32 id,
                                                           const CharacterAppearance& appearance) {
    const auto it = villager_visuals_.find(id);
    if (it != villager_visuals_.end()) {
        return it->second;
    }
    PlayerVisual v;
    v.appearance = appearance;
    v.model = CharacterModel::create(id ^ 0x55u, appearance);
    // Generic peasant garb (a belted tunic + trousers + cap), with a little per-NPC colour variety.
    Equipment eq;
    eq.outfit_tint = static_cast<u8>(id % 4u);
    apply_outfit(v.model, OutfitKind::Peasant, eq);
    v.body_skin = build_body_mesh(v.model);
    v.outfit_skin = build_outfit_mesh(v.model, OutfitKind::Peasant, eq);
    return villager_visuals_.emplace(id, std::move(v)).first->second;
}

void ClientApp::draw_villagers() {
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
        // Guards (kind 1) wear a steel tint + carry a spear; wall archers (kind 2) wear a
        // leather/steel tint + hold a bow; the rest are plain townsfolk.
        const Vec3 tint = vl.kind == 1   ? Vec3{0.72f, 0.76f, 0.86f}
                          : vl.kind == 2 ? Vec3{0.66f, 0.70f, 0.80f}
                                         : Vec3{1.0f};
        if (v.body_skin.vertices.empty()) {
            v.body_skin = build_body_mesh(v.model);
        }
        skin_and_draw(v.model, v.body_skin, v.body_mesh, root, pose, tint);     // continuous skinned body
        skin_and_draw(v.model, v.outfit_skin, v.outfit_mesh, root, pose, tint); // peasant tunic + trousers
        const std::vector<Mat4> mats = v.model.bone_matrices(root, pose);
        draw_rig(v.model, mats, tint, /*attachments_only=*/true); // face/hair/cap/apron on top
        if (vl.kind == 1) {
            draw_held_spear(v.model, mats);
        } else if (vl.kind == 2) {
            const std::vector<Bone>& bones = v.model.bones();
            int hand = -1;
            for (usize i = 0; i < bones.size(); ++i) {
                if (bones[i].part == BonePart::LowerArmR) {
                    hand = static_cast<int>(i);
                    break;
                }
            }
            if (hand >= 0) {
                const Vec3 grip = Vec3{mats[hand][3]};
                const Vec3 fwd{std::cos(vl.yaw), 0.0f, std::sin(vl.yaw)};
                const Vec3 hpos = grip + fwd * 0.12f;
                const Vec4 wood{0.4f, 0.26f, 0.12f, 1.0f};
                renderer_->draw(shape_box_,
                                glm::translate(Mat4{1.0f}, hpos) *
                                    glm::scale(Mat4{1.0f}, Vec3{0.05f, 1.2f, 0.06f}),
                                wood); // bow stave
                for (f32 s : {1.0f, -1.0f}) {
                    renderer_->draw(shape_box_,
                                    glm::translate(Mat4{1.0f}, hpos + Vec3{0.05f, s * 0.55f, 0.0f}) *
                                        glm::scale(Mat4{1.0f}, Vec3{0.05f, 0.2f, 0.05f}),
                                    wood); // recurved tips
                }
            }
        }
    }
}

Mat4 ClientApp::orient_to(const Vec3& dir) {
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

void ClientApp::draw_walls() {
    if (!have_snapshot_ || renderer_ == nullptr) {
        return;
    }
    for (const net::WallState& wl : snapshot_.walls) {
        const f32 hp = static_cast<f32>(wl.health) / 255.0f;
        const f32 len = wl.length;
        const int n = std::max(4, static_cast<int>(len));
        // Same rotated frame as the server's box collider (translate * rotateY(yaw)), so the
        // visible chunks line up with what NPCs path around.
        const Mat4 root = glm::translate(Mat4{1.0f}, wl.position) *
                          glm::rotate(Mat4{1.0f}, wl.yaw, Vec3{0.0f, 1.0f, 0.0f});
        for (int i = 0; i < n; ++i) {
            const u32 h = static_cast<u32>(i) * 2654435761u +
                          static_cast<u32>(std::lround(wl.position.x * 7.0f) * 40503);
            const f32 r0 = static_cast<f32>(h & 255u) / 255.0f;
            const f32 r1 = static_cast<f32>((h >> 8) & 255u) / 255.0f;
            const f32 r2 = static_cast<f32>((h >> 16) & 255u) / 255.0f;
            const f32 lx = ((static_cast<f32>(i) + 0.5f) / static_cast<f32>(n) - 0.5f) * len;
            const f32 hh = kRockWallHeight * (0.7f + 0.5f * r0) * (0.4f + 0.6f * hp); // crumbles
            const f32 cw = len / static_cast<f32>(n) * (0.75f + 0.4f * r1);
            const Vec3 col = glm::mix(Vec3{0.24f, 0.22f, 0.20f}, Vec3{0.48f, 0.45f, 0.42f}, r2) *
                             (0.5f + 0.5f * hp);
            const Mat4 m = root *
                           glm::translate(Mat4{1.0f}, Vec3{lx, hh * 0.5f, (r1 - 0.5f) * 0.3f}) *
                           glm::rotate(Mat4{1.0f}, (r2 - 0.5f) * 0.4f, Vec3{0.0f, 1.0f, 0.0f}) *
                           glm::scale(Mat4{1.0f}, Vec3{cw, hh, kRockWallThick * (0.8f + 0.3f * r0)});
            renderer_->draw(shape_box_, m, Vec4{col, 1.0f});
        }
    }
}

} // namespace alryn::game
