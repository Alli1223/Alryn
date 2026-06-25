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

void ClientApp::draw_cloth(PlayerVisual& v, const Mat4& root, const std::vector<Mat4>& jmats,
                           const Vec3& tint) {
    if (v.cloth.empty()) {
        return;
    }
    // World-space wind: a slowly-veering breeze that strengthens with the storminess (weather_amt_).
    const f32 ws = 1.2f + weather_amt_ * 9.0f + 0.8f * std::sin(elapsed_ * 1.7f);
    const f32 wdir = elapsed_ * 0.15f;
    const Vec3 wind = Vec3{std::cos(wdir), 0.0f, std::sin(wdir)} * ws;
    const Mat4 inv_root = glm::inverse(root);
    const Mat3 root_rot{root};

    for (ClothInstance& c : v.cloth) {
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
        for (usize k = 0; k < c.chains.size(); ++k) {
            c.chains[k].step(anchor_of(k), wind, 9.5f, frame_dt_);
        }

        // Build the mesh in LOCAL space (positions relative to `root`) so the dynamic mesh's bounds
        // stay near the body and frustum culling at `root` is correct; draw it at `root`.
        MeshData md;
        if (c.ring) {
            std::vector<ClothChain> local = c.chains;
            for (ClothChain& ch : local) {
                for (Vec3& p : ch.pos) {
                    p = Vec3{inv_root * Vec4{p, 1.0f}};
                }
            }
            build_cloth_tube(local, true, c.color, md);
        } else {
            ClothChain local = c.chains[0];
            for (Vec3& p : local.pos) {
                p = Vec3{inv_root * Vec4{p, 1.0f}};
            }
            build_cloth_mesh(local, glm::normalize(c.side_local), c.color, md);
        }
        if (md.indices.empty()) {
            continue;
        }
        if (!c.mesh.valid()) {
            c.mesh.create(renderer_->device(), md);
        } else {
            c.mesh.update_vertices(md.vertices); // constant vertex count per piece
        }
        renderer_->draw(c.mesh, root, Vec4{tint, 1.0f});
    }
}

} // namespace alryn::game
