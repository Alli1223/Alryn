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
        case BodyMaterial::PrimaryShade: return pal.primary * 0.66f;
        case BodyMaterial::Linen: return pal.linen;
        case BodyMaterial::Straw: return pal.straw;
        case BodyMaterial::Mail: return pal.metal * Vec3{0.68f, 0.70f, 0.75f};
        case BodyMaterial::Leather: return glm::mix(pal.dark, Vec3{0.46f, 0.31f, 0.18f}, 0.55f);
    }
    return pal.skin;
}

namespace {
// The seed/race width factor the model was generated with (the torso box is 0.38 m * build).
f32 build_of(const CharacterModel& m) {
    const int iT = m.bone_index(BonePart::Torso);
    return iT < 0 ? 1.0f : m.bones()[static_cast<usize>(iT)].box_size.x / 0.38f;
}
} // namespace

std::vector<TorsoRing> torso_profile(const CharacterModel& m) {
    const f32 b = build_of(m);
    const f32 d = 0.55f + 0.45f * b; // depth grows slower than width: a broad dwarf isn't a barrel
    // Seat -> hips -> a nipped waist -> a full chest -> the shoulder girdle -> sloping trapezius -> neck.
    return {
        {-0.07f, 0.165f * b, 0.125f * d, -0.014f},
        {0.16f, 0.158f * b, 0.118f * d, -0.005f},
        {0.38f, 0.145f * b, 0.11f * d, 0.004f},
        {0.6f, 0.172f * b, 0.128f * d, 0.016f},
        {0.78f, 0.186f * b, 0.12f * d, 0.008f},
        {0.91f, 0.148f * b, 0.098f * d, -0.012f},
        {1.0f, 0.062f * b, 0.058f * d, -0.008f},
    };
}

TorsoRing torso_ring_at(const std::vector<TorsoRing>& prof, f32 t) {
    if (prof.empty()) {
        return {t, 0.16f, 0.12f, 0.0f};
    }
    if (t <= prof.front().t) {
        return {t, prof.front().rx, prof.front().rz, prof.front().dz};
    }
    for (usize i = 1; i < prof.size(); ++i) {
        if (t <= prof[i].t) {
            const TorsoRing& a = prof[i - 1];
            const TorsoRing& b = prof[i];
            const f32 f = (t - a.t) / std::max(b.t - a.t, 1e-4f);
            return {t, glm::mix(a.rx, b.rx, f), glm::mix(a.rz, b.rz, f), glm::mix(a.dz, b.dz, f)};
        }
    }
    return {t, prof.back().rx, prof.back().rz, prof.back().dz};
}

SkinnedMesh build_body_mesh(const CharacterModel& model) {
    SkinnedMesh sm;
    const std::vector<Mat4> J = model.joint_matrices(Mat4{1.0f}, {}); // bind joint frames
    sm.inverse_bind.resize(J.size());
    for (usize b = 0; b < J.size(); ++b) {
        sm.inverse_bind[b] = glm::inverse(J[b]);
    }
    if (model.bone_count() < 13) {
        return sm;
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
    const f32 b = build_of(model);
    const int iP = bi(BonePart::Pelvis), iT = bi(BonePart::Torso), iH = bi(BonePart::Head);

    // Torso: an elliptical loft (wider than deep) through the shared torso profile - seat, hips, waist,
    // chest, shoulder girdle, neck - weighted pelvis -> torso -> a touch of head at the neck.
    const Vec3 pelvis = jp(BonePart::Pelvis);
    const Vec3 neck = jp(BonePart::Head);
    {
        std::vector<Vec3> pts;
        std::vector<Vec2> radii;
        std::vector<Weights> w;
        for (const TorsoRing& r : torso_profile(model)) {
            pts.push_back(glm::mix(pelvis, neck, r.t) + Vec3{0.0f, 0.0f, r.dz});
            radii.emplace_back(r.rx, r.rz);
            if (r.t < 0.25f) {
                w.push_back({{iP, 1.0f}});
            } else if (r.t < 0.5f) {
                w.push_back({{iP, 0.5f}, {iT, 0.5f}});
            } else if (r.t < 0.95f) {
                w.push_back({{iT, 1.0f}});
            } else {
                w.push_back({{iT, 0.85f}, {iH, 0.15f}});
            }
        }
        loft(sm, pts, radii, w, BodyMaterial::Shirt, true, false, 12);
    }

    // Neck + head. The head is a shaped ovoid: the jaw narrows to a chin that sits a touch forward, the
    // nape tucks in and the face is a little flatter - so it reads as a head, not a ball, once the
    // eyes / nose / hair features sit on it.
    const Bone& hb = model.bones()[static_cast<usize>(iH)];
    const Vec3 head_c = neck + Vec3{0.0f, hb.box_center.y, 0.0f};
    loft(sm, {neck - Vec3{0.0f, 0.03f, 0.0f}, neck + Vec3{0.0f, 0.08f, 0.0f}},
         {Vec2{0.056f * b, 0.06f}, Vec2{0.054f * b, 0.058f}}, {{{iT, 0.4f}, {iH, 0.6f}}, {{iH, 1.0f}}},
         BodyMaterial::Skin, false, false, 10);
    shaped_ellipsoid(
        sm, head_c, Vec3{hb.box_size.x * 0.5f, hb.box_size.y * 0.5f, hb.box_size.z * 0.52f},
        [&](SkinVertex& v, const Vec3&) { set_w(v, {{iH, 1.0f}}); }, BodyMaterial::Skin,
        [](Vec3 p) {
            if (p.y < 0.0f) {
                const f32 k = -p.y;
                p.x *= 1.0f - 0.26f * k;                              // the jaw narrows to the chin
                p.z *= p.z > 0.0f ? 1.0f + 0.05f * k : 1.0f - 0.16f * k; // chin forward, nape in
                p.y *= 1.05f;                                         // a touch longer jaw
            }
            if (p.z > 0.75f) {
                p.z = 0.75f + (p.z - 0.75f) * 0.6f; // a flatter face plane
            }
            return p;
        },
        [](const Vec3&) { return true; }, 8, 12);

    // Arms: a rounded shoulder (deltoid), then shoulder -> elbow -> wrist with a bicep + forearm swell,
    // weight-blended at the shoulder (torso) + elbow; a mitten hand with a thumb.
    auto arm = [&](BonePart up, BonePart lo) {
        const int iU = bi(up), iL = bi(lo);
        const Vec3 sh = jp(up), el = jp(lo);
        const Vec3 dir = glm::normalize(el - sh);
        const Vec3 wr = el + dir * seg(lo);
        const f32 r = rad(up);
        sphere(sm, sh, r * 1.16f, {{iU, 0.7f}, {iT, 0.3f}}, BodyMaterial::Shirt);
        const auto c = [](f32 v) { return Vec2{v, v}; };
        loft(sm, {sh, glm::mix(sh, el, 0.45f), el, glm::mix(el, wr, 0.3f), wr},
             {c(r * 1.08f), c(r * 1.0f), c(r * 0.84f), c(r * 0.9f), c(r * 0.64f)},
             {{{iT, 0.45f}, {iU, 0.55f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}}, {{iL, 1.0f}}, {{iL, 1.0f}}},
             BodyMaterial::Skin, false, false, 10);
        // The hand: broad fore-aft, thin across (palm to the thigh), hanging on down the forearm - a
        // mitten + thumb, low-res (they're small, and skinned every frame).
        const auto hand_w = [&](SkinVertex& v, const Vec3&) { set_w(v, {{iL, 1.0f}}); };
        const auto as_is = [](const Vec3& n) { return n; };
        const auto all = [](const Vec3&) { return true; };
        shaped_ellipsoid(sm, wr + dir * (r * 0.75f), Vec3{r * 0.42f, r * 0.82f, r * 0.62f}, hand_w,
                         BodyMaterial::Skin, as_is, all, 4, 7);
        shaped_ellipsoid(sm, wr + dir * (r * 0.42f) + Vec3{0.0f, 0.0f, r * 0.5f}, Vec3{r * 0.26f, r * 0.42f, r * 0.26f},
                         hand_w, BodyMaterial::Skin, as_is, all, 3, 5); // thumb
    };
    arm(BonePart::UpperArmL, BonePart::LowerArmL);
    arm(BonePart::UpperArmR, BonePart::LowerArmR);

    // Legs: hip -> knee -> ankle with a thigh + calf swell, weight-blended at the hip (pelvis) + knee,
    // then a shoe: a flat-soled heel -> arch -> ball -> toe loft standing on the ground.
    auto leg = [&](BonePart up, BonePart lo, BonePart foot) {
        const int iU = bi(up), iL = bi(lo), iF = bi(foot);
        const Vec3 hp = jp(up), kn = jp(lo);
        const Vec3 an = kn + glm::normalize(kn - hp) * seg(lo);
        const f32 r = rad(up);
        const auto c = [](f32 v) { return Vec2{v, v}; };
        loft(sm,
             {hp + Vec3{0.0f, 0.03f, 0.0f}, glm::mix(hp, kn, 0.4f), kn, glm::mix(kn, an, 0.3f), an},
             {c(r * 1.12f), c(r * 1.0f), c(r * 0.8f), c(r * 0.86f), c(r * 0.6f)},
             {{{iP, 0.4f}, {iU, 0.6f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}}, {{iL, 1.0f}}, {{iL, 1.0f}}},
             BodyMaterial::Pants, false, false, 10);
        const Bone& fb = model.bones()[static_cast<usize>(iF)];
        const f32 len = fb.box_size.z, hw = fb.box_size.x * 0.5f;
        const f32 sole = 0.004f; // the bind pose stands on y = 0 (the ankle joint sits above it)
        auto at = [&](f32 z, f32 h) { return Vec3{an.x, sole + h, an.z + z * len}; };
        loft(sm, {at(-0.2f, 0.042f), at(0.06f, 0.045f), at(0.36f, 0.032f), at(0.58f, 0.022f)},
             {Vec2{hw * 0.74f, 0.042f}, Vec2{hw * 0.86f, 0.045f}, Vec2{hw * 0.92f, 0.032f}, Vec2{hw * 0.72f, 0.022f}},
             {{{iF, 1.0f}}, {{iF, 1.0f}}, {{iF, 1.0f}}, {{iF, 1.0f}}}, BodyMaterial::Pants, true, true, 10,
             Vec3{1.0f, 0.0f, 0.0f});
    };
    leg(BonePart::UpperLegL, BonePart::LowerLegL, BonePart::FootL);
    leg(BonePart::UpperLegR, BonePart::LowerLegR, BonePart::FootR);

    smooth_normals(sm);
    return sm;
}

} // namespace alryn
