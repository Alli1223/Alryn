#include <Alryn/Character/OutfitMesh.h>

#include <Alryn/Character/SkinBuilder.h>

#include <algorithm>

namespace alryn {

using namespace skinbuild;

namespace {

// Shared bind-pose accessors for a character skeleton (the joint frames, per-part metrics and the torso
// profile the body mesh is built from), so every garment overlays the SAME body with the SAME bone
// weights and bends in lockstep with it when posed.
struct Rig {
    const CharacterModel& model;
    std::vector<Mat4> J; // bind joint frames
    std::vector<TorsoRing> prof;

    explicit Rig(const CharacterModel& m)
        : model(m), J(m.joint_matrices(Mat4{1.0f}, {})), prof(torso_profile(m)) {}

    int bi(BonePart p) const { return model.bone_index(p); }
    Vec3 jp(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? Vec3{0.0f} : Vec3{J[static_cast<usize>(i)][3]};
    }
    f32 seg(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? 0.3f : -2.0f * model.bones()[static_cast<usize>(i)].box_center.y;
    }
    f32 rad(BonePart p) const {
        const int i = bi(p);
        return i < 0 ? 0.08f : model.bones()[static_cast<usize>(i)].box_size.x * 0.5f;
    }
    Vec3 pelvis() const { return jp(BonePart::Pelvis); }
    Vec3 neck() const { return jp(BonePart::Head); }
    f32 shoulder_y() const { return jp(BonePart::UpperArmL).y; }
    f32 shoulder_x() const { return std::abs(jp(BonePart::UpperArmL).x); }
    f32 arm_r() const { return rad(BonePart::UpperArmL); }
    f32 leg_r() const { return rad(BonePart::UpperLegL); }

    // The body's torso cross-section at t (pelvis 0 -> neck 1), interpolated between profile rings.
    TorsoRing ring(f32 t) const { return torso_ring_at(prof, t); }
    Vec3 torso_at(f32 t) const { return glm::mix(pelvis(), neck(), t) + Vec3{0.0f, 0.0f, ring(t).dz}; }
    f32 t_at_y(f32 y) const { return (y - pelvis().y) / std::max(neck().y - pelvis().y, 1e-4f); }

    // Torso weights matching the body loft (pelvis below the hips, a 50/50 waist, torso above, a touch
    // of head at the neck), linearly blended between its rings so a garment deforms with the skin.
    void torso_weigh(SkinVertex& v, f32 t) const {
        const int iP = bi(BonePart::Pelvis), iT = bi(BonePart::Torso), iH = bi(BonePart::Head);
        if (t <= 0.16f) {
            set_w(v, {{iP, 1.0f}});
        } else if (t <= 0.38f) {
            const f32 k = 0.5f * (t - 0.16f) / 0.22f;
            set_w(v, {{iP, 1.0f - k}, {iT, k}});
        } else if (t <= 0.6f) {
            const f32 k = 0.5f + 0.5f * (t - 0.38f) / 0.22f;
            set_w(v, {{iP, 1.0f - k}, {iT, k}});
        } else if (t <= 0.91f) {
            set_w(v, {{iT, 1.0f}});
        } else {
            const f32 k = 0.15f * glm::clamp((t - 0.91f) / 0.09f, 0.0f, 1.0f);
            set_w(v, {{iT, 1.0f - k}, {iH, k}});
        }
    }
    Vec3 head_c() const {
        return neck() + Vec3{0.0f, model.bones()[static_cast<usize>(bi(BonePart::Head))].box_center.y, 0.0f};
    }
    Vec3 head_r() const {
        const Vec3 s = model.bones()[static_cast<usize>(bi(BonePart::Head))].box_size;
        return Vec3{s.x * 0.5f, s.y * 0.5f, s.z * 0.52f};
    }
};

// ---- Garment builders --------------------------------------------------------------------------

// A garment shell over the torso from t0 to t1 (0 = pelvis joint, 1 = neck), `inflate` metres off the
// body all round. `chest` pushes the front of the chest out further (a plate cuirass's swell).
void shell(SkinnedMesh& sm, const Rig& r, f32 t0, f32 t1, f32 inflate, BodyMaterial mat, f32 chest = 0.0f) {
    std::vector<f32> ts{t0};
    for (const TorsoRing& tr : r.prof) {
        if (tr.t > t0 + 0.02f && tr.t < t1 - 0.02f) {
            ts.push_back(tr.t);
        }
    }
    ts.push_back(t1);
    std::vector<Vec3> pts;
    std::vector<Vec2> radii;
    for (const f32 t : ts) {
        const TorsoRing tr = r.ring(t);
        const f32 swell = chest * std::max(0.0f, 1.0f - std::abs(t - 0.62f) / 0.3f);
        pts.push_back(r.torso_at(t) + Vec3{0.0f, 0.0f, swell * 0.5f});
        radii.emplace_back(tr.rx + inflate, tr.rz + inflate + swell * 0.5f);
    }
    loft_fn(sm, pts, radii, [&](SkinVertex& v, usize i, f32) { r.torso_weigh(v, ts[i]); }, mat, false, false, 12);
}

// A narrow band round the torso at t (a belt, a quilting seam, a hem trim): `half_h` tall, standing
// `inflate` off the body.
void band(SkinnedMesh& sm, const Rig& r, f32 t, f32 half_h, f32 inflate, BodyMaterial mat) {
    const f32 span = std::max(r.neck().y - r.pelvis().y, 0.1f);
    const f32 dt = half_h / span;
    const f32 ts[3] = {t - dt, t, t + dt};
    std::vector<Vec3> pts;
    std::vector<Vec2> radii;
    for (int i = 0; i < 3; ++i) {
        const TorsoRing tr = r.ring(ts[i]);
        const f32 bulge = i == 1 ? 0.006f : 0.0f;
        pts.push_back(r.torso_at(ts[i]));
        radii.emplace_back(tr.rx + inflate + bulge, tr.rz + inflate + bulge);
    }
    loft_fn(sm, pts, radii, [&](SkinVertex& v, usize i, f32) { r.torso_weigh(v, ts[i]); }, mat, false, false, 12);
}

// A skirt from torso height `t_top` down past the hips to a hem `hem` of the way to the ground (0.5 ~
// the knee), `inflate` off the body and widening by `flare` at the hem; `zig` dags the hem. Below the
// hips each vertex leans toward the nearer leg's weights, so the skirt strides with the legs instead of
// the thighs punching through it.
void skirt(SkinnedMesh& sm, const Rig& r, f32 t_top, f32 hem, f32 inflate, f32 flare, BodyMaterial mat,
           f32 zig = 0.0f, int rings = 4) {
    const Vec3 P = r.pelvis();
    const TorsoRing top = r.ring(t_top);
    const TorsoRing seat = r.ring(-0.07f);
    // Clear the thighs at the hip (they reach out past the seat ring).
    const f32 thigh = std::abs(r.jp(BonePart::UpperLegL).x) + r.leg_r() * 1.18f;
    const f32 hx = std::max(seat.rx, thigh) + inflate + 0.008f;
    const f32 hz = std::max(seat.rz, r.leg_r() * 1.2f) + inflate + 0.008f;
    const f32 y_hip = P.y - 0.05f;
    const f32 y_hem = P.y * (1.0f - hem);
    std::vector<Vec3> pts{r.torso_at(t_top), Vec3{P.x, y_hip, P.z + seat.dz}};
    std::vector<Vec2> radii{Vec2{top.rx + inflate, top.rz + inflate}, Vec2{hx, hz}};
    for (int k = 1; k <= rings - 2; ++k) {
        const f32 f = static_cast<f32>(k) / static_cast<f32>(rings - 2);
        pts.emplace_back(P.x, glm::mix(y_hip, y_hem, f), P.z + seat.dz);
        radii.emplace_back(hx + flare * f, hz + flare * f * 0.85f);
    }
    const int iP = r.bi(BonePart::Pelvis);
    const int iL = r.bi(BonePart::UpperLegL), iR = r.bi(BonePart::UpperLegR);
    const f32 drop = std::max(P.y - y_hem, 0.05f);
    loft_fn(
        sm, pts, radii,
        [&](SkinVertex& v, usize i, f32) {
            if (i == 0) {
                r.torso_weigh(v, t_top);
                return;
            }
            const f32 f = glm::clamp((P.y - v.position.y) / drop, 0.0f, 1.0f);
            const f32 legw = 0.62f * f;
            const f32 s = glm::smoothstep(-0.55f, 0.55f, (v.position.x - P.x) / std::max(hx, 0.05f));
            set_w(v, {{iP, 1.0f - legw}, {iR, legw * s}, {iL, legw * (1.0f - s)}});
        },
        mat, false, false, 12, Vec3{1.0f, 0.0f, 0.0f}, zig);
}

// A sleeve (or a clad arm / leg): a loft over the limb from the root joint, weighted across the
// shoulder/elbow (or hip/knee) exactly like the body. `coverage` (0..1) is how far down the lower
// segment it reaches; `bell` widens the cuff (a wide medieval sleeve); `zig` dags it.
void clad_limb(SkinnedMesh& sm, const Rig& r, BonePart up, BonePart lo, f32 rscale, f32 coverage,
               BodyMaterial mat, bool arm, f32 bell = 0.0f, f32 zig = 0.0f) {
    const int iU = r.bi(up), iL = r.bi(lo);
    const int iRoot = arm ? r.bi(BonePart::Torso) : r.bi(BonePart::Pelvis);
    if (iU < 0 || iL < 0) {
        return;
    }
    const Vec3 a = r.jp(up), b = r.jp(lo);
    const Vec3 dir = glm::normalize(b - a);
    const Vec3 end = b + dir * (r.seg(lo) * coverage);
    const f32 rr = r.rad(up) * rscale;
    const Vec3 a0 = arm ? a : a + Vec3{0.0f, 0.03f, 0.0f};
    const auto c = [](f32 v) { return Vec2{v, v}; };
    const f32 tail = arm ? 0.66f + 0.26f * (1.0f - coverage) : 0.62f + 0.28f * (1.0f - coverage);
    loft(sm, {a0, glm::mix(a, b, 0.45f), b, glm::mix(b, end, 0.5f), end},
         {c(rr * (arm ? 1.08f : 1.12f)), c(rr), c(rr * 0.86f), c(rr * 0.9f + bell * 0.4f), c(rr * tail + bell)},
         {{{iRoot, arm ? 0.45f : 0.4f}, {iU, arm ? 0.55f : 0.6f}}, {{iU, 1.0f}}, {{iU, 0.5f}, {iL, 0.5f}},
          {{iL, 1.0f}}, {{iL, 1.0f}}},
         mat, false, false, 10, Vec3{1.0f, 0.0f, 0.0f}, zig);
}

// A cuff / bracer: a short loft over the forearm from `from` to `to` (fractions of the forearm).
void bracer(SkinnedMesh& sm, const Rig& r, BonePart up, BonePart lo, f32 from, f32 to, f32 rscale,
            BodyMaterial mat) {
    const int iL = r.bi(lo);
    const Vec3 a = r.jp(up), b = r.jp(lo);
    const Vec3 dir = glm::normalize(b - a);
    const f32 len = r.seg(lo), rr = r.rad(up) * rscale;
    const auto c = [](f32 v) { return Vec2{v, v}; };
    loft(sm, {b + dir * (len * from), b + dir * (len * glm::mix(from, to, 0.5f)), b + dir * (len * to)},
         {c(rr * 0.86f), c(rr * 0.84f), c(rr * 0.74f)}, {{{iL, 1.0f}}, {{iL, 1.0f}}, {{iL, 1.0f}}}, mat, false,
         false, 10);
}

// A boot (or shoe / sabaton): a shaft up the shin to `shaft` of the lower leg (0 = a shoe), an optional
// turned-down cuff, and a foot shell over the body's foot; `toe` stretches the toe (a pointed sabaton).
void boot(SkinnedMesh& sm, const Rig& r, BonePart lo, BonePart foot, f32 shaft, BodyMaterial mat,
          BodyMaterial cuff_mat, bool cuff, f32 toe = 1.0f, f32 puff = 0.014f) {
    const int iL = r.bi(lo), iF = r.bi(foot);
    if (iL < 0 || iF < 0) {
        return;
    }
    const Vec3 kn = r.jp(lo), an = r.jp(foot);
    const Vec3 up = glm::normalize(kn - an);
    const f32 lr = r.leg_r();
    const auto c = [](f32 v) { return Vec2{v, v}; };
    if (shaft > 0.05f) {
        const f32 h = shaft * r.seg(lo);
        loft(sm, {an - up * 0.012f, an + up * (h * 0.5f), an + up * h},
             {c(lr * 0.64f + puff), c(lr * 0.8f + puff), c(lr * 0.86f + puff + 0.006f)},
             {{{iL, 1.0f}}, {{iL, 1.0f}}, {{iL, 1.0f}}}, mat, false, false, 10);
        if (cuff) {
            loft(sm, {an + up * (h + 0.006f), an + up * (h - 0.055f)},
                 {c(lr * 0.86f + puff + 0.012f), c(lr * 0.86f + puff + 0.032f)}, {{{iL, 1.0f}}, {{iL, 1.0f}}},
                 cuff_mat, false, false, 10);
        }
    } else {
        loft(sm, {an - up * 0.02f, an + up * 0.035f}, {c(lr * 0.62f + puff), c(lr * 0.6f + puff)},
             {{{iL, 1.0f}}, {{iL, 1.0f}}}, mat, false, false, 10);
    }
    const Bone& fb = r.model.bones()[static_cast<usize>(iF)];
    const f32 len = fb.box_size.z, hw = fb.box_size.x * 0.5f;
    auto at = [&](f32 z, f32 h) { return Vec3{an.x, 0.004f + h + puff * 0.3f, an.z + z * len}; };
    const f32 tz = 0.36f + 0.22f * toe;
    loft(sm, {at(-0.22f, 0.042f), at(0.06f, 0.046f), at(0.36f, 0.033f), at(tz, 0.02f)},
         {Vec2{hw * 0.74f + puff, 0.042f + puff}, Vec2{hw * 0.86f + puff, 0.046f + puff},
          Vec2{hw * 0.92f + puff, 0.033f + puff}, Vec2{hw * (toe > 1.2f ? 0.4f : 0.7f) + puff * 0.5f, 0.02f + puff * 0.6f}},
         {{{iF, 1.0f}}, {{iF, 1.0f}}, {{iF, 1.0f}}, {{iF, 1.0f}}}, mat, true, true, 10, Vec3{1.0f, 0.0f, 0.0f});
}

// Hose / trousers / chausses over both legs, down to `coverage` of the shin.
void legs(SkinnedMesh& sm, const Rig& r, f32 rscale, f32 coverage, BodyMaterial mat) {
    clad_limb(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, rscale, coverage, mat, false);
    clad_limb(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, rscale, coverage, mat, false);
}
void sleeves(SkinnedMesh& sm, const Rig& r, f32 rscale, f32 coverage, BodyMaterial mat, f32 bell = 0.0f,
             f32 zig = 0.0f) {
    clad_limb(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, rscale, coverage, mat, true, bell, zig);
    clad_limb(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, rscale, coverage, mat, true, bell, zig);
}
void boots(SkinnedMesh& sm, const Rig& r, f32 shaft, BodyMaterial mat, BodyMaterial cuff_mat, bool cuff,
           f32 toe = 1.0f, f32 puff = 0.014f) {
    boot(sm, r, BonePart::LowerLegL, BonePart::FootL, shaft, mat, cuff_mat, cuff, toe, puff);
    boot(sm, r, BonePart::LowerLegR, BonePart::FootR, shaft, mat, cuff_mat, cuff, toe, puff);
}

// A short shoulder cape (the gugel / chaperon capelet, a mail cape, a mantle) from the neck out over the
// shoulders, dropping `drop` below the shoulder line; `zig` dags its hem. The sides lean on the upper
// arms so it rides the shoulders as they swing.
void capelet(SkinnedMesh& sm, const Rig& r, f32 drop, f32 zig, BodyMaterial mat, f32 spread = 1.0f) {
    const Vec3 nk = r.neck();
    const f32 sy = r.shoulder_y(), sx = r.shoulder_x(), ar = r.arm_r();
    const f32 zc = r.ring(0.8f).dz;
    const f32 rz = r.ring(0.78f).rz;
    const std::vector<Vec3> pts{Vec3{nk.x, nk.y + 0.045f, nk.z - 0.008f}, Vec3{nk.x, sy + 0.05f, zc},
                                Vec3{nk.x, sy - drop * 0.45f, zc}, Vec3{nk.x, sy - drop, zc}};
    const std::vector<Vec2> radii{Vec2{0.078f, 0.078f}, Vec2{sx + ar * 0.95f * spread, rz + 0.05f},
                                  Vec2{sx + ar * 1.3f * spread, rz + 0.075f},
                                  Vec2{sx + ar * 1.48f * spread, rz + 0.09f}};
    const int iT = r.bi(BonePart::Torso), iH = r.bi(BonePart::Head);
    const int iL = r.bi(BonePart::UpperArmL), iR = r.bi(BonePart::UpperArmR);
    loft_fn(
        sm, pts, radii,
        [&](SkinVertex& v, usize i, f32 a) {
            if (i == 0) {
                set_w(v, {{iT, 0.6f}, {iH, 0.4f}});
                return;
            }
            const f32 side = std::abs(std::cos(a)) * (i >= 2 ? 0.32f : 0.12f);
            set_w(v, {{iT, 1.0f - side}, {v.position.x > 0.0f ? iR : iL, side}});
        },
        mat, false, false, 14, Vec3{1.0f, 0.0f, 0.0f}, zig);
}

// A stand-up collar round the neck flaring onto the top of the chest (a priest's amice, a mage's high
// collar): `flare` metres out from the body at its foot.
void collar(SkinnedMesh& sm, const Rig& r, BodyMaterial mat, f32 flare, f32 rise) {
    const Vec3 nk = r.neck();
    const TorsoRing lo = r.ring(0.88f);
    const int iT = r.bi(BonePart::Torso), iH = r.bi(BonePart::Head);
    loft(sm, {r.torso_at(0.88f), Vec3{nk.x, nk.y + rise * 0.5f, nk.z - 0.004f}, Vec3{nk.x, nk.y + rise, nk.z - 0.008f}},
         {Vec2{lo.rx + flare, lo.rz + flare}, Vec2{0.08f, 0.08f}, Vec2{0.078f + rise * 0.3f, 0.078f + rise * 0.3f}},
         {{{iT, 1.0f}}, {{iT, 0.8f}, {iH, 0.2f}}, {{iT, 0.65f}, {iH, 0.35f}}}, mat, false, false, 14);
}

// A hood: a shell over the head with the FACE left open, an optional peak swept back off the crown that
// runs on into a hanging liripipe tail of `tail` metres, over a dagged shoulder capelet. The classic
// medieval chaperon / gugel (a mail coif with no peak, tail or dagging).
void hood(SkinnedMesh& sm, const Rig& r, BodyMaterial mat, f32 cape_drop, f32 zig, f32 tail, bool peak) {
    const Vec3 hc = r.head_c(), hr = r.head_r();
    const int iH = r.bi(BonePart::Head), iT = r.bi(BonePart::Torso);
    const Vec3 sc = hc + Vec3{0.0f, 0.01f, -0.014f};
    const Vec3 sr = hr * Vec3{1.17f, 1.13f, 1.17f};
    shaped_ellipsoid(
        sm, sc, sr,
        [&](SkinVertex& v, const Vec3& n) {
            if (n.y < -0.4f) {
                set_w(v, {{iH, 0.75f}, {iT, 0.25f}});
            } else {
                set_w(v, {{iH, 1.0f}});
            }
        },
        mat,
        [&](Vec3 n) {
            if (peak && n.y > 0.1f && n.z < 0.2f) {
                const f32 k = (n.y - 0.1f) * std::min(1.0f, 0.2f - n.z);
                n += Vec3{0.0f, 0.22f * k, -0.38f * k}; // the crown sweeps back to a point
            }
            if (n.y < -0.3f) {
                n.x *= 1.0f + 0.25f * (-n.y - 0.3f); // flares out over the neck into the capelet
                n.z *= 1.0f + 0.2f * (-n.y - 0.3f);
            }
            return n;
        },
        [](const Vec3& n) {
            const bool face = n.z > 0.5f && n.y > -0.66f && n.y < 0.5f;
            return !face && n.y > -0.86f; // open at the face + down into the neck
        },
        8, 14);
    capelet(sm, r, cape_drop, zig, mat);
    if (tail > 0.0f) {
        // The liripipe: from the hood's back point, a tapering tail hanging down the back.
        const Vec3 p0 = sc + Vec3{0.0f, sr.y * 0.92f, -sr.z * 0.92f};
        const Vec3 p1 = p0 + Vec3{0.0f, -tail * 0.25f, -0.06f};
        const Vec3 p2 = p1 + Vec3{0.0f, -tail * 0.4f, -0.015f};
        const Vec3 p3 = p2 + Vec3{0.0f, -tail * 0.35f, 0.01f};
        loft(sm, {p0, p1, p2, p3}, {Vec2{0.032f}, Vec2{0.026f}, Vec2{0.019f}, Vec2{0.011f}},
             {{{iH, 1.0f}}, {{iH, 0.5f}, {iT, 0.5f}}, {{iT, 1.0f}}, {{iT, 1.0f}}}, mat, false, true, 8);
    }
}

// A close-fitting cap over the crown (a linen coif tied under the chin, a skullcap, a spangenhelm when
// `point` raises it to a peak), open at the face.
void cap(SkinnedMesh& sm, const Rig& r, BodyMaterial mat, f32 scale, f32 point, f32 low) {
    const Vec3 hc = r.head_c(), hr = r.head_r();
    const int iH = r.bi(BonePart::Head);
    shaped_ellipsoid(
        sm, hc + Vec3{0.0f, 0.008f, -0.006f}, hr * Vec3{scale, scale * 0.98f, scale},
        [&](SkinVertex& v, const Vec3&) { set_w(v, {{iH, 1.0f}}); }, mat,
        [&](Vec3 n) {
            if (point > 0.0f && n.y > 0.0f) {
                const f32 k = n.y * n.y;
                n.x *= 1.0f - 0.32f * k;
                n.z *= 1.0f - 0.32f * k;
                n.y *= 1.0f + point * k;
            }
            return n;
        },
        [&](const Vec3& n) { return n.y > low && !(n.z > 0.55f && n.y < 0.42f); }, 7, 14);
}

// A brimmed hat: a brim of radius `brim`, a band, and a crown cone `height` tall whose tip bends back by
// `bend` (a wizard's hat) - or a low round crown (a straw sun hat).
void hat(SkinnedMesh& sm, const Rig& r, BodyMaterial mat, BodyMaterial band_mat, f32 brim, f32 height, f32 bend) {
    const Vec3 hc = r.head_c(), hr = r.head_r();
    const int iH = r.bi(BonePart::Head);
    const f32 y = hc.y + hr.y * 0.5f;
    const Vec3 c{hc.x, y, hc.z - 0.008f};
    const std::vector<Weights> w4{{{iH, 1.0f}}, {{iH, 1.0f}}, {{iH, 1.0f}}, {{iH, 1.0f}}};
    loft(sm, {c - Vec3{0.0f, 0.02f, 0.0f}, c}, {Vec2{brim, brim * 0.96f}, Vec2{hr.x * 1.06f, hr.z * 1.06f}},
         {{{iH, 1.0f}}, {{iH, 1.0f}}}, mat, false, false, 16); // the brim (drooping a touch at the edge)
    // The band stands just proud of the crown, so the two never fight over the same surface.
    loft(sm, {c - Vec3{0.0f, 0.004f, 0.0f}, c + Vec3{0.0f, 0.05f, 0.0f}},
         {Vec2{hr.x * 1.11f, hr.z * 1.11f}, Vec2{hr.x * 1.07f, hr.z * 1.07f}}, {{{iH, 1.0f}}, {{iH, 1.0f}}},
         band_mat, false, false, 16);
    const Vec3 b0 = c + Vec3{0.0f, 0.002f, 0.0f};
    loft(sm,
         {b0, b0 + Vec3{0.0f, height * 0.42f, -bend * 0.18f}, b0 + Vec3{0.0f, height * 0.78f, -bend * 0.6f},
          b0 + Vec3{0.0f, height, -bend}},
         {Vec2{hr.x * 1.02f, hr.z * 1.02f}, Vec2{hr.x * 0.66f, hr.z * 0.66f}, Vec2{hr.x * 0.3f, hr.z * 0.3f},
          Vec2{0.012f, 0.012f}},
         w4, mat, false, true, 12);
}

// A flat-topped great helm enclosing the head (the face behind a slit - the slit, breaths and cross are
// attachment pieces laid on its front).
void great_helm(SkinnedMesh& sm, const Rig& r, BodyMaterial mat) {
    const Vec3 hc = r.head_c(), hr = r.head_r();
    const int iH = r.bi(BonePart::Head), iT = r.bi(BonePart::Torso);
    auto at = [&](f32 k) { return Vec3{hc.x, hc.y + hr.y * k, hc.z + 0.004f}; };
    loft(sm, {at(-0.78f), at(-0.3f), at(0.3f), at(0.82f), at(1.0f)},
         {Vec2{hr.x * 1.12f, hr.z * 1.18f}, Vec2{hr.x * 1.2f, hr.z * 1.26f}, Vec2{hr.x * 1.2f, hr.z * 1.25f},
          Vec2{hr.x * 1.14f, hr.z * 1.17f}, Vec2{hr.x * 0.92f, hr.z * 0.94f}},
         {{{iH, 0.85f}, {iT, 0.15f}}, {{iH, 1.0f}}, {{iH, 1.0f}}, {{iH, 1.0f}}, {{iH, 1.0f}}}, mat, false, true, 12);
}

// Plate faulds: overlapping steel hoops stepping out over the hips below the cuirass.
void faulds(SkinnedMesh& sm, const Rig& r, int lames, BodyMaterial mat) {
    const Vec3 P = r.pelvis();
    const TorsoRing seat = r.ring(-0.07f);
    const f32 thigh = std::abs(r.jp(BonePart::UpperLegL).x) + r.leg_r() * 1.12f;
    const f32 hx = std::max(seat.rx, thigh);
    const int iP = r.bi(BonePart::Pelvis);
    for (int k = 0; k < lames; ++k) {
        const f32 top = P.y + 0.06f - static_cast<f32>(k) * 0.058f;
        const f32 grow = 0.03f + static_cast<f32>(k) * 0.012f;
        loft(sm, {Vec3{P.x, top, P.z + seat.dz}, Vec3{P.x, top - 0.07f, P.z + seat.dz}},
             {Vec2{hx + grow - 0.01f, seat.rz + grow - 0.01f}, Vec2{hx + grow + 0.008f, seat.rz + grow + 0.012f}},
             {{{iP, 1.0f}}, {{iP, 1.0f}}}, mat, false, false, 12);
    }
}

// A round piece of plate on a joint (a couter at the elbow, a poleyn at the knee).
void joint_plate(SkinnedMesh& sm, const Rig& r, BonePart up, BonePart lo, f32 size, BodyMaterial mat) {
    const int iU = r.bi(up), iL = r.bi(lo);
    const Vec3 j = r.jp(lo);
    const bool arm = up == BonePart::UpperArmL || up == BonePart::UpperArmR;
    const Vec3 off = arm ? Vec3{0.0f, 0.0f, -0.02f} : Vec3{0.0f, 0.0f, 0.03f}; // elbow behind, knee in front
    ellipsoid(sm, j + off, Vec3{size, size * 0.9f, size * 0.85f}, {{iU, 0.5f}, {iL, 0.5f}}, mat);
}

} // namespace

SkinnedMesh build_outfit_mesh(const CharacterModel& model, OutfitKind kind, const Equipment& equip) {
    SkinnedMesh sm;
    if (model.bone_count() < 13) {
        return sm;
    }
    const Rig r(model);
    sm.inverse_bind.resize(r.J.size());
    for (usize b = 0; b < r.J.size(); ++b) {
        sm.inverse_bind[b] = glm::inverse(r.J[b]);
    }
    const int vt = outfit_design_tier(equip.outfit());
    using M = BodyMaterial;
    const bool head = !equip.bare_head; // headwear (hood / helm / hat) - off on the creator's turntable

    switch (kind) {
        case OutfitKind::Plate: {
            if (vt == 0) {
                // SQUIRE - a quilted gambeson (padded jacket with a skirt to mid-thigh), hose, turned-cuff
                // leather boots, a sword belt, and a plain wool hood.
                shell(sm, r, -0.06f, 1.0f, 0.034f, M::Primary);
                for (const f32 t : {0.12f, 0.3f, 0.48f, 0.66f, 0.82f}) {
                    band(sm, r, t, 0.007f, 0.036f, M::PrimaryShade); // quilting seams
                }
                skirt(sm, r, 0.06f, 0.42f, 0.03f, 0.06f, M::Primary);
                sleeves(sm, r, 1.3f, 0.92f, M::Primary);
                legs(sm, r, 1.1f, 0.9f, M::Pants);
                boots(sm, r, 0.6f, M::Leather, M::Leather, true);
                band(sm, r, 0.22f, 0.02f, 0.05f, M::Dark); // sword belt
                if (head) hood(sm, r, M::Dark, 0.13f, 0.0f, 0.0f, false);
            } else if (vt == 1) {
                // KNIGHT - a mail hauberk to the knee with full sleeves, mail chausses, a mail coif under a
                // conical helm, and a cloth surcoat over it (the bodice here; the skirt is cloth).
                shell(sm, r, -0.06f, 1.0f, 0.03f, M::Mail);
                skirt(sm, r, 0.06f, 0.5f, 0.026f, 0.05f, M::Mail);
                sleeves(sm, r, 1.26f, 1.0f, M::Mail);
                legs(sm, r, 1.12f, 1.0f, M::Mail);
                boots(sm, r, 0.35f, M::Leather, M::Leather, false);
                shell(sm, r, 0.14f, 0.9f, 0.05f, M::Primary); // the surcoat bodice (sleeveless)
                band(sm, r, 0.26f, 0.022f, 0.062f, M::Dark);  // its belt (the cloth skirt hangs from here)
                if (head) {
                    hood(sm, r, M::Mail, 0.11f, 0.0f, 0.0f, false); // mail coif + its short mail cape
                    cap(sm, r, M::Metal, 1.3f, 0.55f, 0.28f);       // conical nasal helm
                }
            } else {
                // PALADIN - full plate: a swelling cuirass over faulds, plate arms with couters, plate legs
                // with poleyns, pointed sabatons, and a flat-topped great helm.
                shell(sm, r, -0.04f, 0.97f, 0.042f, M::Metal, 0.03f);
                faulds(sm, r, 3, M::Metal);
                sleeves(sm, r, 1.38f, 0.96f, M::Metal);
                joint_plate(sm, r, BonePart::UpperArmL, BonePart::LowerArmL, r.arm_r() * 1.25f, M::Metal);
                joint_plate(sm, r, BonePart::UpperArmR, BonePart::LowerArmR, r.arm_r() * 1.25f, M::Metal);
                legs(sm, r, 1.2f, 1.0f, M::Metal);
                joint_plate(sm, r, BonePart::UpperLegL, BonePart::LowerLegL, r.leg_r() * 1.08f, M::Metal);
                joint_plate(sm, r, BonePart::UpperLegR, BonePart::LowerLegR, r.leg_r() * 1.08f, M::Metal);
                boots(sm, r, 0.5f, M::Metal, M::Metal, false, 1.5f, 0.02f);
                band(sm, r, 0.2f, 0.02f, 0.058f, M::Dark); // sword belt
                if (head) great_helm(sm, r, M::Metal);
            }
            break;
        }
        case OutfitKind::Peasant: {
            // A belted knee-length wool tunic with long sleeves, hose with linen leg wraps, low leather
            // shoes - and (by the tint's high bits) a hood, a linen coif, a straw hat, or bare-headed.
            shell(sm, r, -0.06f, 1.0f, 0.022f, M::Primary);
            skirt(sm, r, 0.06f, 0.4f, 0.02f, 0.07f, M::Primary);
            sleeves(sm, r, 1.18f, 0.88f, M::Primary);
            legs(sm, r, 1.06f, 0.96f, M::Pants);
            for (const auto& [up, lo] : {std::pair{BonePart::UpperLegL, BonePart::LowerLegL},
                                         std::pair{BonePart::UpperLegR, BonePart::LowerLegR}}) {
                bracer(sm, r, up, lo, 0.25f, 0.95f, 1.22f, M::Linen); // leg wraps over the shins
            }
            boots(sm, r, 0.0f, M::Leather, M::Leather, false);
            band(sm, r, 0.22f, 0.016f, 0.036f, M::Dark); // belt
            switch ((equip.outfit_tint / 4u) % 4u) {
                case 0: hood(sm, r, M::Accent, 0.12f, 0.03f, 0.2f, true); break;
                case 1: cap(sm, r, M::Linen, 1.1f, 0.0f, -0.55f); break;
                case 2: hat(sm, r, M::Straw, M::Dark, 0.24f, 0.1f, 0.0f); break;
                default: break; // bare-headed
            }
            break;
        }
        case OutfitKind::Robe: {
            // A fitted robe bodice (the long skirt below the belt is simulated cloth), wide bell sleeves,
            // soft shoes; an apprentice's hood + liripipe, or a wizard's pointed hat.
            shell(sm, r, -0.06f, 1.0f, 0.03f, M::Primary);
            sleeves(sm, r, 1.24f, 1.0f, M::Primary, vt == 0 ? 0.05f : 0.08f, vt == 0 ? 0.02f : 0.0f);
            legs(sm, r, 1.06f, 0.96f, M::Pants);
            boots(sm, r, 0.2f, M::Dark, M::Dark, false);
            band(sm, r, 0.26f, vt == 0 ? 0.012f : 0.02f, 0.044f, vt == 2 ? M::Accent : vt == 1 ? M::Dark : M::Leather);
            if (vt == 0) {
                if (head) hood(sm, r, M::Primary, 0.14f, 0.025f, 0.32f, true);
            } else {
                if (head) hat(sm, r, M::Primary, M::Accent, vt == 2 ? 0.24f : 0.21f, vt == 2 ? 0.46f : 0.38f, 0.14f);
                capelet(sm, r, 0.1f, 0.0f, M::PrimaryShade, 0.92f); // a short mantle over the shoulders
                if (vt == 2) {
                    collar(sm, r, M::Accent, 0.05f, 0.08f); // a gilt high collar
                }
            }
            break;
        }
        case OutfitKind::Holy: {
            // A monk's habit / priest's vestment bodice (the long skirt is simulated cloth), wide sleeves,
            // sandals; the acolyte's cowl, or a linen amice collar under the priest's / prophet's headwear.
            shell(sm, r, -0.06f, 1.0f, 0.03f, M::Primary);
            sleeves(sm, r, 1.24f, 1.0f, M::Primary, 0.06f);
            legs(sm, r, 1.06f, 0.96f, M::Pants);
            boots(sm, r, 0.0f, M::Leather, M::Leather, false);
            band(sm, r, 0.26f, vt == 0 ? 0.01f : 0.02f, 0.044f, vt == 2 ? M::Accent : vt == 1 ? M::Dark : M::Linen);
            if (vt == 0) {
                if (head) hood(sm, r, M::Primary, 0.16f, 0.0f, 0.0f, false); // the monk's cowl
            } else {
                collar(sm, r, M::Linen, 0.06f, 0.035f); // the amice
            }
            break;
        }
        case OutfitKind::Leather: {
            if (vt == 2) {
                // BEASTMASTER - dark scale armour with sleeves, a dagged scale skirt, tall dark boots.
                shell(sm, r, -0.06f, 1.0f, 0.04f, M::Dark);
                skirt(sm, r, 0.06f, 0.36f, 0.036f, 0.06f, M::Dark, 0.035f);
                sleeves(sm, r, 1.24f, 0.8f, M::Dark);
                legs(sm, r, 1.1f, 0.9f, M::Pants);
                boots(sm, r, 0.75f, M::Dark, M::Leather, true);
                band(sm, r, 0.22f, 0.02f, 0.056f, M::Leather);
            } else {
                // HUNTER / WARDEN - a green wool tunic with a dagged hem under a sleeveless leather jerkin,
                // a hood + capelet + long liripipe (the woodsman's chaperon), hose, tall cuffed boots and
                // leather bracers.
                shell(sm, r, -0.06f, 1.0f, 0.022f, M::Primary);
                skirt(sm, r, 0.06f, 0.36f, 0.02f, 0.05f, M::Primary, 0.04f);
                sleeves(sm, r, 1.16f, 0.82f, M::Primary);
                shell(sm, r, 0.0f, 0.86f, 0.04f, M::Leather); // the jerkin
                skirt(sm, r, 0.12f, 0.16f, 0.036f, 0.03f, M::Leather);
                legs(sm, r, 1.06f, 0.96f, M::Pants);
                boots(sm, r, 0.82f, M::Leather, M::Leather, true);
                for (const auto& [up, lo] : {std::pair{BonePart::UpperArmL, BonePart::LowerArmL},
                                             std::pair{BonePart::UpperArmR, BonePart::LowerArmR}}) {
                    bracer(sm, r, up, lo, 0.2f, 0.92f, 1.34f, M::Leather);
                }
                band(sm, r, 0.2f, 0.018f, 0.056f, M::Dark);
                if (head) hood(sm, r, M::Primary, 0.15f, 0.035f, 0.42f, true);
            }
            break;
        }
        case OutfitKind::Brigand: {
            // A sleeveless leather jerkin over a dark shirt, a short dagged leather skirt, trousers, cuffed
            // boots and a rough hood + dagged capelet. Bare (wrapped) arms read as a melee bruiser.
            shell(sm, r, -0.06f, 0.96f, 0.032f, M::Dark);
            skirt(sm, r, 0.06f, 0.3f, 0.03f, 0.05f, M::Dark, 0.04f);
            legs(sm, r, 1.1f, 0.92f, M::Pants);
            boots(sm, r, 0.6f, M::Leather, M::Dark, true);
            band(sm, r, 0.2f, 0.024f, 0.05f, M::Leather);
            hood(sm, r, M::Dark, 0.13f, 0.04f, 0.0f, true);
            break;
        }
        case OutfitKind::Outlaw: {
            // A light leather jerkin with half-sleeves, trousers, tall boots, and a deep hood with a long
            // liripipe (a woodland poacher).
            shell(sm, r, -0.06f, 1.0f, 0.03f, M::Dark);
            skirt(sm, r, 0.06f, 0.26f, 0.028f, 0.04f, M::Dark);
            sleeves(sm, r, 1.12f, 0.5f, M::Dark);
            legs(sm, r, 1.1f, 0.95f, M::Pants);
            boots(sm, r, 0.8f, M::Leather, M::Leather, true);
            band(sm, r, 0.2f, 0.018f, 0.046f, M::Leather);
            hood(sm, r, M::Dark, 0.14f, 0.03f, 0.38f, true);
            break;
        }
    }

    smooth_normals(sm);
    return sm;
}

} // namespace alryn
