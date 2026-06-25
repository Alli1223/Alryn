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

    auto add_cape = [&](const Vec3& color, f32 width, f32 wind_gain) {
        ClothInstance c;
        c.anchor = BonePart::Head;                       // the neck / upper-back joint
        c.anchor_local = Vec3{0.0f, -0.05f, -0.12f};     // just behind + below the neck
        c.hang_local = Vec3{0.0f, -1.0f, -0.22f};        // hangs down + a touch back
        c.side_local = Vec3{1.0f, 0.0f, 0.0f};           // wide left-right across the back
        c.segments = 5;
        c.seg = 0.13f;
        c.half_width = width;
        c.color = color;
        c.chain.stiffness = 0.72f;
        c.chain.damping = 0.06f;
        c.chain.wind_gain = wind_gain;
        v.cloth.push_back(std::move(c));
    };

    // Capes exist on the legendary tier of the cape-wearing roles (paladin / high prophet / beastmaster).
    if (vt == 2 && (role == PlayerRole::Knight || role == PlayerRole::Cleric)) {
        add_cape(pal.primary, 0.26f, 1.2f);
    } else if (vt == 2 && role == PlayerRole::Hunter) {
        add_cape(pal.dark, 0.24f, 1.5f); // the beastmaster's tattered dark cape (catches more wind)
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
        const Vec3 anchor = Vec3{(abone * glm::translate(Mat4{1.0f}, c.anchor_local))[3]};
        if (!c.inited) {
            const Vec3 hang = glm::normalize(root_rot * c.hang_local);
            c.chain.init(anchor, hang, c.segments, c.seg, c.half_width);
            c.inited = true;
        }
        c.chain.step(anchor, wind, 9.5f, frame_dt_);

        // Build the sheet in LOCAL space (positions relative to `root`), so the dynamic mesh's bounds
        // stay near the body and frustum culling at `root` is correct; draw it at `root`.
        ClothChain local = c.chain;
        for (Vec3& p : local.pos) {
            p = Vec3{inv_root * Vec4{p, 1.0f}};
        }
        MeshData md;
        build_cloth_mesh(local, glm::normalize(c.side_local), c.color, md);
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
