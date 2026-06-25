#include <Alryn/Character/BodyMesh.h>

#include <cmath>

namespace alryn {

namespace {

constexpr int kSides = 8; // octagonal cross-sections (low-poly)
using Weights = std::initializer_list<std::pair<int, f32>>;

void set_w(SkinVertex& sv, const Weights& w) {
    for (int i = 0; i < kMaxInfluences; ++i) {
        sv.bones[i] = 0;
        sv.weights[i] = 0.0f;
    }
    int k = 0;
    f32 tot = 0.0f;
    for (const auto& [b, wt] : w) {
        if (k >= kMaxInfluences || wt <= 0.0f) {
            continue;
        }
        sv.bones[k] = b;
        sv.weights[k] = wt;
        tot += wt;
        ++k;
    }
    if (tot > 1e-6f) {
        for (f32& wt : sv.weights) {
            wt /= tot;
        }
    } else {
        sv.weights[0] = 1.0f;
    }
}

// A ring of `kSides` vertices around `center` in the plane spanned by (u, v), radius r.
u32 ring(SkinnedMesh& m, const Vec3& center, const Vec3& u, const Vec3& v, f32 r, const Weights& w,
         BodyMaterial mat) {
    const u32 first = static_cast<u32>(m.vertices.size());
    for (int i = 0; i < kSides; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(kSides);
        const Vec3 dir = u * std::cos(a) + v * std::sin(a);
        SkinVertex sv;
        sv.position = center + dir * r;
        sv.normal = dir;
        sv.material = static_cast<u8>(mat);
        set_w(sv, w);
        m.add_vertex(sv);
    }
    return first;
}

void bridge(SkinnedMesh& m, u32 r0, u32 r1) {
    for (int i = 0; i < kSides; ++i) {
        const u32 a = r0 + static_cast<u32>(i);
        const u32 b = r0 + static_cast<u32>((i + 1) % kSides);
        const u32 c = r1 + static_cast<u32>((i + 1) % kSides);
        const u32 d = r1 + static_cast<u32>(i);
        m.triangle(a, b, c);
        m.triangle(a, c, d);
    }
}

// Close a ring with a fan to a centre vertex (a flat cap).
void cap(SkinnedMesh& m, u32 r0, const Vec3& center, const Weights& w, BodyMaterial mat, bool flip) {
    SkinVertex cv;
    cv.position = center;
    cv.normal = Vec3{0.0f, flip ? -1.0f : 1.0f, 0.0f};
    cv.material = static_cast<u8>(mat);
    set_w(cv, w);
    const u32 c = m.add_vertex(cv);
    for (int i = 0; i < kSides; ++i) {
        const u32 a = r0 + static_cast<u32>(i);
        const u32 b = r0 + static_cast<u32>((i + 1) % kSides);
        if (flip) {
            m.triangle(c, b, a);
        } else {
            m.triangle(c, a, b);
        }
    }
}

// A basis (u, v) perpendicular to `axis`.
void perp_basis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 a = glm::length(axis) > 1e-5f ? glm::normalize(axis) : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 ref = std::abs(a.y) > 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    u = glm::normalize(glm::cross(a, ref));
    v = glm::normalize(glm::cross(a, u));
}

// A capped tube through `pts` (radii per point, weights per point) along a shared axis.
void tube(SkinnedMesh& m, const std::vector<Vec3>& pts, const std::vector<f32>& radii,
          const std::vector<Weights>& w, BodyMaterial mat, bool cap_start, bool cap_end) {
    if (pts.size() < 2) {
        return;
    }
    Vec3 u, v;
    perp_basis(pts.back() - pts.front(), u, v);
    std::vector<u32> rings;
    rings.reserve(pts.size());
    for (usize i = 0; i < pts.size(); ++i) {
        rings.push_back(ring(m, pts[i], u, v, radii[i], w[i], mat));
    }
    for (usize i = 0; i + 1 < pts.size(); ++i) {
        bridge(m, rings[i], rings[i + 1]);
    }
    if (cap_start) {
        cap(m, rings.front(), pts.front(), w.front(), mat, true);
    }
    if (cap_end) {
        cap(m, rings.back(), pts.back(), w.back(), mat, false);
    }
}

// A UV sphere centred at `center`, radius r, all weighted to one bone.
void sphere(SkinnedMesh& m, const Vec3& center, f32 r, const Weights& w, BodyMaterial mat) {
    constexpr int lat = 6, lon = 8;
    std::vector<u32> prev;
    for (int j = 0; j <= lat; ++j) {
        const f32 theta = Pi * static_cast<f32>(j) / static_cast<f32>(lat);
        const f32 y = std::cos(theta), rr = std::sin(theta);
        std::vector<u32> cur;
        for (int i = 0; i < lon; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(lon);
            const Vec3 n{rr * std::cos(a), y, rr * std::sin(a)};
            SkinVertex sv;
            sv.position = center + n * r;
            sv.normal = n;
            sv.material = static_cast<u8>(mat);
            set_w(sv, w);
            cur.push_back(m.add_vertex(sv));
        }
        if (j > 0) {
            for (int i = 0; i < lon; ++i) {
                const u32 a = prev[static_cast<usize>(i)];
                const u32 b = prev[static_cast<usize>((i + 1) % lon)];
                const u32 c = cur[static_cast<usize>((i + 1) % lon)];
                const u32 d = cur[static_cast<usize>(i)];
                m.triangle(a, b, c);
                m.triangle(a, c, d);
            }
        }
        prev = cur;
    }
}

// Average face normals into shared vertices for a smooth-shaded surface.
void smooth_normals(SkinnedMesh& m) {
    for (SkinVertex& v : m.vertices) {
        v.normal = Vec3{0.0f};
    }
    for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
        SkinVertex& a = m.vertices[m.indices[i]];
        SkinVertex& b = m.vertices[m.indices[i + 1]];
        SkinVertex& c = m.vertices[m.indices[i + 2]];
        const Vec3 fn = glm::cross(b.position - a.position, c.position - a.position);
        a.normal += fn;
        b.normal += fn;
        c.normal += fn;
    }
    for (SkinVertex& v : m.vertices) {
        v.normal = glm::length(v.normal) > 1e-6f ? glm::normalize(v.normal) : Vec3{0.0f, 1.0f, 0.0f};
    }
}

} // namespace

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

    // Neck + head.
    const f32 hh = seg(BonePart::Head) < 0.0f ? 0.25f : 0.25f;
    const Vec3 head_c = neck + Vec3{0.0f, model.bones()[static_cast<usize>(iH)].box_center.y, 0.0f};
    tube(sm, {neck - Vec3{0, 0.02f, 0}, neck + Vec3{0, 0.06f, 0}}, {0.07f, 0.075f},
         {{{iT, 0.4f}, {iH, 0.6f}}, {{iH, 1.0f}}}, BodyMaterial::Skin, false, false);
    sphere(sm, head_c, model.bones()[static_cast<usize>(iH)].box_size.x * 0.56f, {{iH, 1.0f}},
           BodyMaterial::Skin);
    (void)hh;

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
