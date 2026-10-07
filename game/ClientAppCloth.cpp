// ClientApp - simulated flowing cloth (capes, robe skirts, ...) on characters.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::setup_cloth(PlayerVisual& v, PlayerRole role, const Equipment& eq) {
    for (ClothInstance& c : v.cloth) {
        retire_mesh(std::move(c.mesh)); // defer the GPU free past the frames in flight (gear rebuild)
    }
    v.cloth.clear();
    // Fit the body + worn gear the cloth collides with (once per build), then dress the role's pieces
    // (Character/ClothRig::outfit_cloth - capes, robe / surcoat / tunic skirts, stole, mantle).
    v.cloth_body = fit_body_colliders(v.model, v.body_skin, v.outfit_skin);
    std::vector<ClothPiece> pieces =
        outfit_cloth(v.model, outfit_kind_for_role(static_cast<u8>(role)), eq, v.cloth_body);
    for (ClothPiece& p : pieces) {
        ClothInstance c;
        static_cast<ClothPiece&>(c) = std::move(p);
        v.cloth.push_back(std::move(c));
    }
}

void ClientApp::setup_noble_cape(PlayerVisual& v) {
    for (ClothInstance& c : v.cloth) {
        retire_mesh(std::move(c.mesh)); // defer the GPU free past the frames in flight
    }
    v.cloth.clear();
    v.cloth_body = fit_body_colliders(v.model, v.body_skin, v.outfit_skin);
    ClothInstance c;
    static_cast<ClothPiece&>(c) = noble_cape(v.model, v.cloth_body);
    v.cloth.push_back(std::move(c));
}

void ClientApp::detach_cloth(ClothInstance& c, const Vec3& impulse) {
    if (c.detached || !c.inited) {
        return; // not seated yet, or already gone
    }
    c.detached = true;
    c.detach_age = 0.0f;
    for (ClothChain& ch : c.chains) {
        ch.detach(); // unpin the anchor -> the whole chain free-falls
        for (usize i = 0; i < ch.pos.size(); ++i) {
            ch.prev[i] = ch.pos[i] - impulse; // a one-shot velocity kick so it flutters away
        }
    }
}

void ClientApp::update_cloth_triggers() {
    if (!have_snapshot_) {
        return;
    }
    const bool storm = weather_amt_ > 0.72f; // a strong storm tears cloth away
    const f32 wdir = elapsed_ * 0.15f;
    const Vec3 wind_dir{std::cos(wdir), 0.0f, std::sin(wdir)};
    for (const net::PlayerState& p : snapshot_.players) {
        const auto it = visuals_.find(p.id);
        if (it == visuals_.end()) {
            continue;
        }
        PlayerVisual& v = it->second;
        const u8 h = p.health;                                             // 0..100 percent of role max
        const bool hit = (v.last_health != 255 && h + 6u < v.last_health); // dropped > 6% since last tick
        v.last_health = h;
        for (ClothInstance& c : v.cloth) {
            if (c.detached || !c.inited) {
                continue;
            }
            if (hit && frand() < 0.4f) {
                detach_cloth(c, rand_dir() * frand(0.03f, 0.06f) + Vec3{0.0f, 0.04f, 0.0f}); // cut
            } else if (storm && frand() < frame_dt_ * 0.2f) {
                detach_cloth(c, wind_dir * frand(0.05f, 0.09f) + Vec3{0.0f, 0.03f, 0.0f}); // blown off
            }
        }
    }
}

void ClientApp::draw_cloth(PlayerVisual& v, const Mat4& root, const std::vector<Mat4>& jmats,
                           const Vec3& tint, f32 ground) {
    if (v.cloth.empty()) {
        return;
    }
    // Perf: don't simulate / draw cloth for far-off characters (same generous cull as the body).
    if (glm::distance(Vec3{root[3]}, camera_.position()) > character::skin_cull_dist) {
        return;
    }
    if (v.cloth_body.caps.empty()) {
        v.cloth_body = fit_body_colliders(v.model, v.body_skin, v.outfit_skin); // safety net
    }
    // The body as posed THIS frame: capsules over the torso, hips, head, limbs + bulky gear, so attached
    // cloth rests on the back, drapes over the shoulders and is pushed aside by a striding leg or a
    // swinging arm instead of clipping through them.
    pose_body_colliders(v.cloth_body, jmats, cloth_colliders_);
    // World-space wind: a slowly-veering breeze that strengthens with the storminess (weather_amt_).
    const f32 ws = 1.2f + weather_amt_ * 9.0f + 0.8f * std::sin(elapsed_ * 1.7f);
    const f32 wdir = elapsed_ * 0.15f;
    const Vec3 wind = Vec3{std::cos(wdir), 0.0f, std::sin(wdir)} * ws;
    ClothEnv env;
    env.wind = wind;
    env.gravity = 9.5f;
    env.dt = frame_dt_;
    env.body = cloth_colliders_;
    env.ground = ground; // long hems pool on the floor under the feet instead of sinking through it
    const Mat4 inv_root = glm::inverse(root);
    constexpr f32 kLinger = 6.0f, kSink = 0.8f; // a fallen piece lies on the ground, then sinks + despawns

    for (usize ci = 0; ci < v.cloth.size();) {
        ClothInstance& c = v.cloth[ci];
        if (c.detached) {
            c.detach_age += frame_dt_;
            if (c.detach_age > kLinger + kSink) { // despawn the fallen piece
                retire_mesh(std::move(c.mesh));
                v.cloth.erase(v.cloth.begin() + static_cast<std::ptrdiff_t>(ci));
                continue;
            }
            const f32 sink = (c.detach_age > kLinger) ? 1.8f * frame_dt_ : 0.0f; // sink into the ground at the end
            for (ClothChain& ch : c.chains) {
                ch.step(Vec3{0.0f}, wind, 9.5f, frame_dt_); // free-fall (anchor ignored) + catch the wind
                if (sink > 0.0f) {
                    for (usize i = 0; i < ch.pos.size(); ++i) {
                        ch.pos[i].y -= sink;
                        ch.prev[i].y -= sink;
                    }
                }
            }
        } else {
            step_cloth(c, v.model, jmats, root, env); // seats it on first use, then steps vs the body
        }

        // Attached: build the mesh in LOCAL space (relative to root) so it culls correctly + draw at
        // root. Detached: the piece is a free WORLD object - build in world + draw with identity.
        const bool world_space = c.detached;
        auto localize = [&](ClothChain& ch) {
            if (!world_space) {
                for (Vec3& p : ch.pos) {
                    p = Vec3{inv_root * Vec4{p, 1.0f}};
                }
            }
        };
        MeshData md;
        if (c.ring) {
            std::vector<ClothChain> local = c.chains;
            for (ClothChain& ch : local) {
                localize(ch);
            }
            // Attached cloth is built in the character's frame, so its outer faces look away from the
            // body's own axis (x = z = 0); a fallen piece just faces away from its own middle.
            const Vec3 body_axis{0.0f};
            build_cloth_tube(local, c.closed, c.color, md, c.device, c.device_color,
                             world_space ? nullptr : &body_axis);
        } else {
            ClothChain local = c.chains[0];
            localize(local);
            build_cloth_mesh(local, glm::normalize(c.side_local), c.color, md,
                             world_space ? Vec3{0.0f} : c.drape_local);
        }
        if (!md.indices.empty()) {
            if (!c.mesh.valid()) {
                c.mesh.create(renderer_->device(), md);
            } else {
                c.mesh.update_vertices(md.vertices); // constant vertex count per piece
            }
            renderer_->draw(c.mesh, world_space ? Mat4{1.0f} : root, Vec4{tint, 1.0f});
        }
        ++ci;
    }
}

} // namespace alryn::game
