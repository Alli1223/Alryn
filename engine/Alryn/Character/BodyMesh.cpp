#include <Alryn/Character/BodyMesh.h>

#include <Alryn/Character/SkinBuilder.h>

namespace alryn {

using namespace skinbuild;

Vec3 body_material_color(const CharacterPalette& pal, BodyMaterial mat) {
    switch (mat) {
        case BodyMaterial::Skin: return pal.skin;
        case BodyMaterial::Shirt: return pal.shirt;
        case BodyMaterial::Pants: return pal.pants;
        case BodyMaterial::Hair: return pal.hair;
        case BodyMaterial::Eye: return pal.eye;
        case BodyMaterial::Primary: return pal.primary;
        case BodyMaterial::Accent: return pal.accent;
        case BodyMaterial::Metal: return pal.metal;
        case BodyMaterial::Dark: return pal.dark;
        case BodyMaterial::Glow: return pal.glow;
    }
    return pal.skin;
}

SkinnedMesh build_body_mesh(const CharacterModel& model) {
    SkinnedMesh sm;
    const std::vector<Mat4> J = model.joint_matrices(Mat4{1.0f}, {}); // bind joint frames
    sm.inverse_bind.resize(J.size());
    for (usize b = 0; b < J.size(); ++b) {
        sm.inverse_bind[b] = glm::inverse(J[b]);
    }

    auto bi = [&](BonePart p) { return model.bone_index(p); };
    auto jp = [&](BonePart p) {
        const int i = bi(p);
        return i < 0 ? Vec3{0.0f} : Vec3{J[static_cast<usize>(i)][3]};
    };
    auto seg = [&](BonePart p) {
        const int i = bi(p);
        return i < 0 ? 0.3f : -2.0f * model.bones()[static_cast<usize>(i)].box_center.y;
    };
    auto rad = [&](BonePart p) {
        const int i = bi(p);
        return i < 0 ? 0.08f : model.bones()[static_cast<usize>(i)].box_size.x * 0.5f;
    };

    const int iP = bi(BonePart::Pelvis), iT = bi(BonePart::Torso), iH = bi(BonePart::Head);

    // Torso: pelvis -> chest (the head joint is the neck base). Tapered, weighted pelvis -> torso.
    const Vec3 pelvis = jp(BonePart::Pelvis);
    const Vec3 neck = jp(BonePart::Head);
    const Vec3 hip_c{pelvis.x, pelvis.y, pelvis.z};
    const f32 tw = rad(BonePart::Torso) > 0.05f ? 0.21f : 0.21f;
    tube(sm, {hip_c - Vec3{0, 0.04f, 0}, glm::mix(pelvis, neck, 0.35f), glm::mix(pelvis, neck, 0.72f), neck},
         {tw * 1.05f, tw * 1.1f, tw, tw * 0.86f},
         {{{iP, 1.0f}}, {{iP, 0.5f}, {iT, 0.5f}}, {{iT, 1.0f}}, {{iT, 0.85f}, {iH, 0.15f}}},
         BodyMaterial::Shirt, true, false);

    // Neck + head. The head is an OVOID (taller than wide, a touch deeper front-to-back) rather than a
    // bare ball, so the face reads as a head once the nose/brow/eye features sit on it.
    const Vec3 head_c = neck + Vec3{0.0f, model.bones()[static_cast<usize>(iH)].box_center.y, 0.0f};
    const Vec3 hbox = model.bones()[static_cast<usize>(iH)].box_size;
    tube(sm, {neck - Vec3{0, 0.02f, 0}, neck + Vec3{0, 0.06f, 0}}, {0.07f, 0.075f},
         {{{iT, 0.4f}, {iH, 0.6f}}, {{iH, 1.0f}}}, BodyMaterial::Skin, false, false);
    ellipsoid(sm, head_c, Vec3{hbox.x * 0.56f, hbox.y * 0.52f, hbox.z * 0.58f}, {{iH, 1.0f}},
              BodyMaterial::Skin);

    // Arms: shoulder -> elbow -> wrist, weight-blended at the shoulder (torso) + elbow.
    auto arm = [&](BonePart up, BonePart lo) {
        const int iU = bi(up), iL = bi(lo);
        const Vec3 sh = jp(up), el = jp(lo);
        const Vec3 wr = el + glm::normalize(el - sh) * seg(lo);
        const f32 r = rad(up);
        // A rounded shoulder (deltoid) that moves with the arm - it caps the open top of the arm
        // tube so no hollow shows when the arm swings, and rounds the shoulder into the torso.
        sphere(sm, sh, r * 1.18f, {{iU, 0.7f}, {iT, 0.3f}}, BodyMaterial::Shirt);
        tube(sm,
             {sh, glm::mix(sh, el, 0.5f), el, glm::mix(el, wr, 0.55f), wr},
             {r * 1.15f, r * 0.95f, r * 0.9f, r * 0.82f, r * 0.72f},
             {{{iT, 0.45f}, {iU, 0.55f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}}, {{iL, 1.0f}},
              {{iL, 1.0f}}},
             BodyMaterial::Skin, false, false);
        sphere(sm, wr, r * 0.82f, {{iL, 1.0f}}, BodyMaterial::Skin); // hand
    };
    arm(BonePart::UpperArmL, BonePart::LowerArmL);
    arm(BonePart::UpperArmR, BonePart::LowerArmR);

    // Legs: hip -> knee -> ankle, weight-blended at the hip (pelvis) + knee, then a foot.
    auto leg = [&](BonePart up, BonePart lo, BonePart foot) {
        const int iU = bi(up), iL = bi(lo), iF = bi(foot);
        const Vec3 hp = jp(up), kn = jp(lo);
        const Vec3 an = kn + glm::normalize(kn - hp) * seg(lo);
        const f32 r = rad(up);
        tube(sm,
             {hp + Vec3{0, 0.02f, 0}, glm::mix(hp, kn, 0.5f), kn, glm::mix(kn, an, 0.55f), an},
             {r * 1.1f, r * 0.96f, r * 0.9f, r * 0.84f, r * 0.78f},
             {{{iP, 0.4f}, {iU, 0.6f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}}, {{iL, 1.0f}},
              {{iL, 1.0f}}},
             BodyMaterial::Pants, false, false);
        // Foot: a small box-ish ring sweep forward.
        const Vec3 fwd{0.0f, 0.0f, 1.0f};
        tube(sm, {an, an + Vec3{0, -0.05f, 0.02f}, an + Vec3{0, -0.08f, 0.16f}},
             {r * 0.8f, r * 0.85f, r * 0.7f}, {{{iF, 1.0f}}, {{iF, 1.0f}}, {{iF, 1.0f}}},
             BodyMaterial::Pants, true, true);
        (void)fwd;
    };
    leg(BonePart::UpperLegL, BonePart::LowerLegL, BonePart::FootL);
    leg(BonePart::UpperLegR, BonePart::LowerLegR, BonePart::FootR);

    smooth_normals(sm);
    return sm;
}

} // namespace alryn
