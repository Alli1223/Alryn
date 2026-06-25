// ClientApp - simulated flowing cloth (capes, robe skirts, ...) on characters.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::setup_cloth(PlayerVisual& v, PlayerRole role, const Equipment& eq) {
    for (ClothInstance& c : v.cloth) {
        retire_mesh(std::move(c.mesh)); // defer the GPU free past the frames in flight (gear rebuild)
    }
    v.cloth.clear();
    const int vt = outfit_design_tier(eq.outfit());
    const CharacterPalette& pal = v.model.palette();

    // A flat back cape: one chain hanging off the upper back, drawn as a wide sheet.
    auto add_cape = [&](const Vec3& color, f32 width, f32 wind_gain) {
        ClothInstance c;
        c.anchor = BonePart::Head; // the neck / upper-back joint
        c.ring = false;
        c.segments = 5;
        c.seg = 0.13f;
        c.half_width = width;
        c.color = color;
        c.side_local = Vec3{1.0f, 0.0f, 0.0f};
        c.anchor_locals = {Vec3{0.0f, -0.05f, -0.12f}}; // just behind + below the neck
        c.hang_locals = {Vec3{0.0f, -1.0f, -0.22f}};    // down + a touch back
        c.chains.resize(1);
        c.chains[0].stiffness = 0.72f;
        c.chains[0].damping = 0.06f;
        c.chains[0].wind_gain = wind_gain;
        v.cloth.push_back(std::move(c));
    };

    // A robe skirt: a ring of chains hanging off the waist, drawn as a closed tube around the legs.
    auto add_skirt = [&](const Vec3& color, f32 radius, int segs, f32 seg, f32 flare, f32 wind_gain) {
        ClothInstance c;
        c.anchor = BonePart::Pelvis;
        c.ring = true;
        c.segments = segs;
        c.seg = seg;
        c.color = color;
        constexpr int kN = 8; // panels around the waist
        c.chains.resize(kN);
        for (int i = 0; i < kN; ++i) {
            const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(kN);
            const Vec3 radial{std::sin(ang), 0.0f, std::cos(ang)};
            c.anchor_locals.push_back(radial * radius + Vec3{0.0f, 0.06f, 0.0f}); // the waist ring
            c.hang_locals.push_back(glm::normalize(radial * flare + Vec3{0.0f, -1.0f, 0.0f})); // down + flared
            c.chains[static_cast<usize>(i)].stiffness = 0.6f;
            c.chains[static_cast<usize>(i)].damping = 0.07f;
            c.chains[static_cast<usize>(i)].wind_gain = wind_gain;
        }
        v.cloth.push_back(std::move(c));
    };

    // Capes on the legendary tier of the cape-wearing roles (paladin / high prophet / beastmaster).
    if (vt == 2 && (role == PlayerRole::Knight || role == PlayerRole::Cleric)) {
        add_cape(pal.primary, 0.26f, 1.2f);
    } else if (vt == 2 && role == PlayerRole::Hunter) {
        add_cape(pal.dark, 0.24f, 1.5f); // the beastmaster's tattered dark cape
    }
    // Robe skirts on the robe-wearers (every tier): the Mage's mid-calf robe, the Cleric's long one.
    if (role == PlayerRole::Mage) {
        add_skirt(pal.primary, 0.17f, 6, 0.15f, 0.34f, 1.0f);
    } else if (role == PlayerRole::Cleric) {
        add_skirt(pal.primary, 0.17f, 7, 0.15f, 0.3f, 0.9f);
    }
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
                           const Vec3& tint) {
    if (v.cloth.empty()) {
        return;
    }
    // Perf: don't simulate / draw cloth for far-off characters (same generous cull as the body).
    if (glm::distance(Vec3{root[3]}, camera_.position()) > character::skin_cull_dist) {
        return;
    }
    // The body as a collision cylinder (feet axis), so attached cloth drapes OVER the legs/torso
    // instead of sinking through them. Pushes hanging nodes out to its surface.
    const Vec3 feet = Vec3{root[3]};
    constexpr f32 kBodyR = 0.2f;
    const f32 body_top = feet.y + 0.95f; // up to the lower chest
    auto collide = [&](Vec3& p) {
        if (p.y < feet.y - 0.05f || p.y > body_top) {
            return;
        }
        const f32 dx = p.x - feet.x, dz = p.z - feet.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < kBodyR * kBodyR && d2 > 1e-6f) {
            const f32 d = std::sqrt(d2);
            p.x = feet.x + dx / d * kBodyR;
            p.z = feet.z + dz / d * kBodyR;
        }
    };
    // World-space wind: a slowly-veering breeze that strengthens with the storminess (weather_amt_).
    const f32 ws = 1.2f + weather_amt_ * 9.0f + 0.8f * std::sin(elapsed_ * 1.7f);
    const f32 wdir = elapsed_ * 0.15f;
    const Vec3 wind = Vec3{std::cos(wdir), 0.0f, std::sin(wdir)} * ws;
    const Mat4 inv_root = glm::inverse(root);
    const Mat3 root_rot{root};
    constexpr f32 kLinger = 6.0f, kSink = 0.8f; // a fallen piece lies on the ground, then sinks + despawns

    for (usize ci = 0; ci < v.cloth.size();) {
        ClothInstance& c = v.cloth[ci];
        const int bi = v.model.bone_index(c.anchor);
        const Mat4 abone = (bi >= 0) ? jmats[static_cast<usize>(bi)] : root;
        auto anchor_of = [&](usize k) {
            return Vec3{(abone * glm::translate(Mat4{1.0f}, c.anchor_locals[k]))[3]};
        };
        if (!c.inited) {
            for (usize k = 0; k < c.chains.size(); ++k) {
                const Vec3 hang = glm::normalize(root_rot * c.hang_locals[k]);
                c.chains[k].init(anchor_of(k), hang, c.segments, c.seg, c.half_width);
            }
            c.inited = true;
        }

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
            for (usize k = 0; k < c.chains.size(); ++k) {
                c.chains[k].step(anchor_of(k), wind, 9.5f, frame_dt_);
                ClothChain& ch = c.chains[k];
                for (usize i = 1; i < ch.pos.size(); ++i) { // node 0 is the pinned anchor - leave it
                    collide(ch.pos[i]);
                    collide(ch.prev[i]);
                }
            }
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
            build_cloth_tube(local, true, c.color, md);
        } else {
            ClothChain local = c.chains[0];
            localize(local);
            build_cloth_mesh(local, glm::normalize(c.side_local), c.color, md);
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
