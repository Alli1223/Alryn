#include <Alryn/Character/ClothRig.h>

#include <Alryn/Character/BodyMesh.h>

#include <algorithm>
#include <cmath>

namespace alryn {

void ClothChain::init(const Vec3& anchor, const Vec3& hang_dir, int segments, f32 seg, f32 width) {
    seg_len = seg;
    half_width = width;
    attached = true;
    fall_age = 0.0f;
    const Vec3 d = glm::length(hang_dir) > 1e-5f ? glm::normalize(hang_dir) : Vec3{0.0f, -1.0f, 0.0f};
    pos.clear();
    pos.reserve(static_cast<usize>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        pos.push_back(anchor + d * (seg * static_cast<f32>(i)));
    }
    prev = pos;
}

void ClothChain::integrate(const Vec3& anchor, const Vec3& accel, f32 retain, f32 dt, f32 inner_keep) {
    if (pos.size() < 2) {
        return;
    }
    Vec3 carry{0.0f}; // the anchor's own motion this step - inner damping settles the swing about it
    if (attached) {
        carry = anchor - pos[0];
        pos[0] = anchor; // node 0 is pinned to the live attachment point
        prev[0] = anchor;
    } else {
        fall_age += dt;
    }
    // Verlet integration: carry velocity (pos - prev), add gravity + wind.
    const f32 dt2 = dt * dt;
    const usize first = attached ? 1u : 0u;
    for (usize i = first; i < pos.size(); ++i) {
        const Vec3 vel = (carry + (pos[i] - prev[i] - carry) * inner_keep) * retain;
        prev[i] = pos[i];
        pos[i] += vel + accel * dt2;
    }
}

void ClothChain::relax() {
    // Distance constraints hold the segment lengths (and so the cloth's shape). The pinned anchor
    // (node 0, while attached) only moves its child.
    for (usize i = 1; i < pos.size(); ++i) {
        const Vec3 d = pos[i] - pos[i - 1];
        const f32 len = glm::length(d);
        if (len < 1e-5f) {
            continue;
        }
        const Vec3 corr = d * ((len - seg_len) / len * stiffness);
        if (i - 1 == 0 && attached) {
            pos[i] -= corr; // parent pinned: move only the child
        } else {
            pos[i - 1] += corr * 0.5f;
            pos[i] -= corr * 0.5f;
        }
    }
}

void ClothChain::step(const Vec3& anchor, const Vec3& wind, f32 gravity, f32 dt) {
    if (pos.size() < 2) {
        return;
    }
    integrate(anchor, Vec3{0.0f, -gravity, 0.0f} + wind * wind_gain, 1.0f - damping, dt);
    for (int k = 0; k < 8; ++k) {
        relax();
    }
}

bool ClothChain::settled() const {
    if (attached || fall_age < 0.6f) {
        return false;
    }
    f32 v = 0.0f;
    for (usize i = 0; i < pos.size(); ++i) {
        v += glm::length(pos[i] - prev[i]);
    }
    return v < 0.012f * static_cast<f32>(pos.size());
}

// ---- Body collision ----------------------------------------------------------------------------

namespace {

Vec3 closest_on_segment(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const f32 len2 = glm::dot(ab, ab);
    const f32 t = len2 > 1e-10f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}

f32 segment_distance(const Vec3& p, const Vec3& a, const Vec3& b) {
    return glm::length(p - closest_on_segment(p, a, b));
}

// The bone with the largest weight on a skinned vertex.
int dominant_bone(const SkinVertex& v) {
    int best = v.bones[0];
    f32 w = v.weights[0];
    for (int k = 1; k < kMaxInfluences; ++k) {
        if (v.weights[k] > w) {
            w = v.weights[k];
            best = v.bones[k];
        }
    }
    return best;
}

} // namespace

BodyColliders fit_body_colliders(const CharacterModel& model, const SkinnedMesh& body,
                                 const SkinnedMesh& outfit) {
    BodyColliders out;
    if (model.bone_count() < 13) {
        return out;
    }
    const std::vector<Bone>& bones = model.bones();
    const std::vector<Mat4> J = model.joint_matrices(Mat4{1.0f}, {}); // bind joint frames
    std::vector<Mat4> inv(J.size());
    for (usize i = 0; i < J.size(); ++i) {
        inv[i] = glm::inverse(J[i]);
    }
    const int iP = model.bone_index(BonePart::Pelvis);
    const int iT = model.bone_index(BonePart::Torso);
    const int iH = model.bone_index(BonePart::Head);
    const f32 pelvis_y = J[static_cast<usize>(iP)][3].y;

    // Every skinned vertex (body + outfit) with its dominant bone, in bind (model) space.
    struct Pt {
        Vec3 p;
        int bone;
    };
    std::vector<Pt> pts;
    pts.reserve(body.vertices.size() + outfit.vertices.size());
    for (const SkinnedMesh* sm : {&body, &outfit}) {
        for (const SkinVertex& v : sm->vertices) {
            pts.push_back({v.position, dominant_bone(v)});
        }
    }
    auto local = [&](int bone, const Vec3& p) { return Vec3{inv[static_cast<usize>(bone)] * Vec4{p, 1.0f}}; };
    auto add = [&](int bone, const Vec3& a, const Vec3& b, f32 r, u8 group) {
        out.caps.push_back({bone, a, b, r, group});
    };

    // A broad, flat body mass (torso / hips) as a STADIUM: three parallel vertical spines across its
    // width, each swollen to the depth - a far closer fit to a chest than one round capsule. Fitted to
    // the member points (in `bone`'s frame) over the vertical span [y_lo, y_hi].
    struct Stadium {
        f32 half_w, half_d, zc;
    };
    auto stadium = [&](int bone, u8 group, auto&& member, f32 y_lo, f32 y_hi, f32 max_r) -> Stadium {
        f32 w = 0.0f, zmin = 1e9f, zmax = -1e9f;
        std::vector<Vec3> m;
        for (const Pt& pt : pts) {
            if (member(pt)) {
                const Vec3 l = local(bone, pt.p);
                m.push_back(l);
                w = std::max(w, std::abs(l.x));
                zmin = std::min(zmin, l.z);
                zmax = std::max(zmax, l.z);
            }
        }
        if (m.empty()) {
            return {0.0f, 0.0f, 0.0f};
        }
        const f32 zc = 0.5f * (zmin + zmax);
        const f32 s = std::max(0.0f, w - 0.5f * (zmax - zmin)); // spine half-spacing
        f32 r = 0.0f;
        for (const Vec3& l : m) {
            const Vec3 q{glm::clamp(l.x, -s, s), glm::clamp(l.y, y_lo, y_hi), zc};
            r = std::max(r, glm::length(l - q));
        }
        r = std::min(r, max_r);
        for (const f32 x : {-s, 0.0f, s}) {
            if (s < 0.01f && x != 0.0f) {
                continue; // narrow enough for a single spine
            }
            add(bone, Vec3{x, y_lo, zc}, Vec3{x, y_hi, zc}, r, group);
        }
        return {s + r, r, zc};
    };

    // Torso: the chest + back from the waist up to the shoulder line (the dome rounds over the
    // shoulders + nape). Members: torso-weighted points plus the pelvis-weighted waist ring above the
    // hips - but nothing above the neck (a hood's collar or the top of its tail isn't the torso).
    const int iUAL = model.bone_index(BonePart::UpperArmL);
    const f32 shoulder_y = bones[static_cast<usize>(iUAL)].joint_offset.y; // in the Torso frame
    const f32 neck_y = J[static_cast<usize>(iH)][3].y + 0.02f;
    const Stadium torso = stadium(
        iT, kBodyTorso,
        [&](const Pt& pt) {
            return pt.p.y <= neck_y && (pt.bone == iT || (pt.bone == iP && pt.p.y >= pelvis_y - 0.02f));
        },
        -0.04f, shoulder_y, 0.42f);
    out.torso_half_width = torso.half_w;
    out.torso_half_depth = torso.half_d;
    out.torso_center_z = torso.zc;

    // Hips: everything the pelvis carries (the seat + waist, a tunic hem / gambeson skirt), as a
    // stadium whose spines run the height of those points - so the radius is the girth, not the
    // distance up to the waist.
    {
        f32 lo = 1e9f, hi = -1e9f;
        for (const Pt& pt : pts) {
            if (pt.bone == iP) {
                const f32 y = local(iP, pt.p).y;
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        }
        if (lo <= hi) {
            const f32 y0 = std::min(lo + 0.05f, 0.5f * (lo + hi)), y1 = std::max(hi - 0.03f, y0);
            stadium(iP, kBodyHips, [&](const Pt& pt) { return pt.bone == iP; }, y0, y1, 0.4f);
        }
    }

    // Head: a sphere over the skull, with slack for the hair / hood that ride on it.
    {
        const Vec3 c{0.0f, bones[static_cast<usize>(iH)].box_center.y, 0.0f};
        f32 r = 0.0f;
        for (const Pt& pt : pts) {
            if (pt.bone == iH) {
                r = std::max(r, glm::length(local(iH, pt.p) - c));
            }
        }
        add(iH, c, c, std::min(r * 1.12f, 0.3f), kBodyHead);
    }

    // Limbs: a capsule down each segment (joint -> child joint), radius fitted to its own points.
    auto limb = [&](BonePart part, u8 group) {
        const int b = model.bone_index(part);
        if (b < 0) {
            return;
        }
        const Vec3 a{0.0f};
        const Vec3 e{0.0f, 2.0f * bones[static_cast<usize>(b)].box_center.y, 0.0f}; // far end (-Y)
        f32 r = 0.0f;
        for (const Pt& pt : pts) {
            if (pt.bone == b) {
                r = std::max(r, segment_distance(local(b, pt.p), a, e));
            }
        }
        if (r > 0.0f) {
            add(b, a, e, std::min(r, 0.22f), group);
        }
    };
    for (BonePart p : {BonePart::UpperArmL, BonePart::LowerArmL, BonePart::UpperArmR, BonePart::LowerArmR}) {
        limb(p, kBodyArms);
    }
    for (BonePart p : {BonePart::UpperLegL, BonePart::LowerLegL, BonePart::UpperLegR, BonePart::LowerLegR}) {
        limb(p, kBodyLegs);
    }
    // Feet: a capsule heel -> toe through the boot's points.
    for (BonePart p : {BonePart::FootL, BonePart::FootR}) {
        const int b = model.bone_index(p);
        f32 zmin = 1e9f, zmax = -1e9f, ysum = 0.0f;
        std::vector<Vec3> m;
        for (const Pt& pt : pts) {
            if (pt.bone == b) {
                const Vec3 l = local(b, pt.p);
                m.push_back(l);
                zmin = std::min(zmin, l.z);
                zmax = std::max(zmax, l.z);
                ysum += l.y;
            }
        }
        if (m.empty()) {
            continue;
        }
        const f32 y = ysum / static_cast<f32>(m.size());
        const f32 half = 0.5f * (zmax - zmin);
        const f32 inset = std::min(half, 0.06f);
        const Vec3 a{0.0f, y, zmin + inset}, e{0.0f, y, zmax - inset};
        f32 r = 0.0f;
        for (const Vec3& l : m) {
            r = std::max(r, segment_distance(l, a, e));
        }
        add(b, a, e, std::min(r, 0.16f), kBodyLegs);
    }

    // Bulky worn gear on the trunk + shoulders (pauldrons, gorget, collars, quiver, satchels): each
    // attachment box becomes a capsule along its longest axis, swollen to its middle dimension. Thin
    // trims / straps / arrows are skipped, and so is anything already inside the body capsules.
    const usize core = out.caps.size();
    for (usize i = 0; i < bones.size(); ++i) {
        const Bone& bn = bones[i];
        if (!bn.attachment || bn.color == BoneColor::Glow || bn.parent < 0) {
            continue;
        }
        const BonePart pp = bones[static_cast<usize>(bn.parent)].part;
        const bool arm = pp == BonePart::UpperArmL || pp == BonePart::UpperArmR;
        if (!arm && pp != BonePart::Torso && pp != BonePart::Pelvis) {
            continue;
        }
        const Vec3 s = bn.box_size;
        int ax[3] = {0, 1, 2};
        std::sort(ax, ax + 3, [&](int x, int y) { return s[x] > s[y]; });
        if (s[ax[2]] < 0.06f) {
            continue; // a thin plate / strap / trim
        }
        Vec3 axis{0.0f};
        axis[ax[0]] = 1.0f;
        axis = bn.box_rotation * axis;
        const f32 r = std::min(0.5f * s[ax[1]] * 0.92f, 0.25f);
        const f32 half = 0.5f * (s[ax[0]] - s[ax[1]]);
        const Vec3 c = bn.box_center + bn.joint_offset; // the attachment's joint frame == its parent's
        const Vec3 a = c - axis * half, e = c + axis * half;
        bool inside = false;
        for (usize k = 0; k < core && !inside; ++k) {
            const BodyCapsule& cc = out.caps[k];
            inside = cc.bone == bn.parent && segment_distance(a, cc.a, cc.b) + r <= cc.r &&
                     segment_distance(e, cc.a, cc.b) + r <= cc.r;
        }
        if (!inside) {
            add(bn.parent, a, e, r, arm ? kBodyArms : pp == BonePart::Pelvis ? kBodyHips : kBodyTorso);
        }
    }
    return out;
}

void pose_body_colliders(const BodyColliders& body, const std::vector<Mat4>& joints,
                         std::vector<ClothCollider>& out) {
    out.clear();
    out.reserve(body.caps.size());
    for (const BodyCapsule& c : body.caps) {
        if (c.bone < 0 || static_cast<usize>(c.bone) >= joints.size()) {
            continue;
        }
        const Mat4& j = joints[static_cast<usize>(c.bone)];
        out.push_back({Vec3{j * Vec4{c.a, 1.0f}}, Vec3{j * Vec4{c.b, 1.0f}}, c.r, c.group});
    }
}

void collide_cloth_node(Vec3& p, Vec3& prev, std::span<const ClothCollider> body, const Vec3& drape,
                        f32 margin, f32 friction, u8 groups, const Vec3& body_step) {
    for (const ClothCollider& c : body) {
        if ((c.group & groups) == 0) {
            continue;
        }
        const Vec3 q = closest_on_segment(p, c.a, c.b);
        Vec3 d = p - q;
        const f32 r = c.r + margin;
        if (glm::dot(d, d) >= r * r) {
            continue;
        }
        // On the wrong side of this body part's axis (a fast turn swept it through): come back out
        // on the side the cloth belongs on, mirrored, rather than across the front.
        const f32 side = glm::dot(d, drape);
        if (side < 0.0f) {
            d -= drape * (2.0f * side);
        }
        const f32 dl = glm::length(d);
        Vec3 n;
        if (dl > 1e-5f) {
            n = d / dl;
        } else if (glm::dot(drape, drape) > 0.5f) {
            n = drape;
        } else {
            Vec3 u = c.b - c.a;
            u = glm::length(u) > 1e-5f ? glm::normalize(u) : Vec3{0.0f, 1.0f, 0.0f};
            n = glm::normalize(glm::cross(u, std::abs(u.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f}));
        }
        // Rest on the surface: keep the motion along it relative to the body (less friction), drop
        // the part going into it.
        const Vec3 v = p - prev - body_step;
        p = q + n * r;
        const f32 vn = glm::dot(v, n);
        const Vec3 vt = (v - n * vn) * (1.0f - friction);
        prev = p - (body_step + vt + n * std::max(vn, 0.0f));
    }
}

// ---- Worn cloth pieces -------------------------------------------------------------------------

namespace {

// Character-local measurements shared by the cloth recipes (bind pose, Torso joint frame).
struct ClothFrame {
    std::vector<ClothCollider> bind; // the body colliders in the bind pose (character space)
    Mat4 torso{1.0f};                // bind Torso joint frame
    Mat4 torso_inv{1.0f};
    f32 shoulder_y = 0.44f;   // shoulder joints, in the Torso frame
    f32 collar_height = 1.1f; // height of the shoulder line above the feet
    f32 span = 0.65f;         // pelvis joint -> neck base
    f32 torso_y0 = 0.04f;     // the Torso joint's height above the pelvis joint
    f32 pelvis_height = 0.7f; // the pelvis joint above the feet
    std::vector<TorsoRing> prof; // the body's torso cross-sections (BodyMesh)
    BodyColliders fit;
};

ClothFrame cloth_frame(const CharacterModel& m, const BodyColliders& body) {
    ClothFrame f;
    f.fit = body;
    f.prof = torso_profile(m);
    const std::vector<Mat4> J = m.joint_matrices(Mat4{1.0f}, {});
    pose_body_colliders(body, J, f.bind);
    const int iT = m.bone_index(BonePart::Torso);
    const int iU = m.bone_index(BonePart::UpperArmL);
    const int iP = m.bone_index(BonePart::Pelvis);
    const int iH = m.bone_index(BonePart::Head);
    if (iT >= 0) {
        f.torso = J[static_cast<usize>(iT)];
        f.torso_inv = glm::inverse(f.torso);
        f.torso_y0 = m.bones()[static_cast<usize>(iT)].joint_offset.y;
    }
    if (iU >= 0) {
        f.shoulder_y = m.bones()[static_cast<usize>(iU)].joint_offset.y;
        f.collar_height = J[static_cast<usize>(iU)][3].y;
    }
    if (iP >= 0 && iH >= 0) {
        f.pelvis_height = J[static_cast<usize>(iP)][3].y;
        f.span = J[static_cast<usize>(iH)][3].y - f.pelvis_height;
    }
    return f;
}

// Nudges a Torso-frame anchor point out of the bind-pose body + gear (so the pinned edge starts just
// outside whatever the character wears), moving it toward `drape` (Torso-frame, may be zero).
Vec3 seat_anchor(const ClothFrame& f, const Vec3& local, const Vec3& drape, u8 groups, f32 margin) {
    Vec3 p{f.torso * Vec4{local, 1.0f}};
    Vec3 prev = p;
    const Vec3 dw = glm::length(drape) > 1e-5f ? glm::normalize(Vec3{f.torso * Vec4{drape, 0.0f}}) : Vec3{0.0f};
    for (int k = 0; k < 4; ++k) {
        collide_cloth_node(p, prev, f.bind, dw, margin, 0.0f, groups);
    }
    return Vec3{f.torso_inv * Vec4{p, 1.0f}};
}

// A cloak: a ROW of panels anchored on an arc across the upper back (an open sheet, not a strip),
// following the fitted torso + shoulder gear, hanging behind the body and fanning slightly outward.
ClothPiece make_cape(const ClothFrame& f, int panels, f32 arc, int segments, f32 length, f32 clearance,
                     const Vec3& color, f32 wind_gain) {
    ClothPiece c;
    c.anchor = BonePart::Torso; // follows the shoulders' twist + lean (not the head)
    c.ring = true;              // multi-chain -> tube builder
    c.closed = false;           // an OPEN sheet across the back
    c.drape_local = Vec3{0.0f, 0.0f, -1.0f};
    c.collide = kBodyAll; // rests on the back + shoulders, behind the swinging arms + legs
    c.segments = segments;
    c.seg = length / static_cast<f32>(segments);
    c.color = color;
    const f32 W = f.fit.torso_half_width + clearance;
    const f32 D = f.fit.torso_half_depth + clearance;
    const f32 y = f.shoulder_y + 0.05f; // the collar rides the top of the shoulders
    for (int i = 0; i < panels; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(panels - 1); // 0..1, left -> right
        const f32 ang = glm::mix(-arc, arc, t);
        const Vec3 dir{std::sin(ang), 0.0f, -std::cos(ang)}; // around the back (-Z behind)
        const Vec3 at{std::sin(ang) * W, y, f.fit.torso_center_z - std::cos(ang) * D};
        c.anchor_locals.push_back(seat_anchor(f, at, dir, kBodyAll, 0.03f));
        c.hang_locals.push_back(glm::normalize(dir * 0.35f + Vec3{0.0f, -1.0f, 0.0f})); // down + fan out
        ClothChain ch;
        ch.stiffness = 0.7f;
        ch.damping = 0.012f;      // light air drag: a walk lifts the hem a little, a run more
        ch.inner_damping = 0.07f; // the swing about the shoulders settles
        ch.wind_gain = wind_gain;
        c.chains.push_back(ch);
    }
    return c;
}

// A flowing skirt (a robe's / habit's / surcoat's) hung from the BELT at torso height `t`: a closed
// ring of chains round the waist, just outside the skinned bodice + belt, hanging down over the hips +
// legs, which push it about as they stride. It only collides with the hips + legs - the arms swing
// past it and the broad chest above never pushes its waistband out. A panel faces straight ahead (for
// the device). `length` runs from the belt toward the ground.
ClothPiece make_skirt(const ClothFrame& f, f32 t, int segments, f32 length, f32 flare, f32 inflate,
                      const Vec3& color, f32 wind_gain) {
    ClothPiece c;
    c.anchor = BonePart::Torso;
    c.ring = true;
    c.closed = true;
    c.collide = kBodyHips | kBodyLegs;
    c.weave = 0.94f; // holds its flare: an A-line robe, not a hem gathering in round the ankles
    c.segments = segments;
    c.seg = length / static_cast<f32>(segments);
    c.color = color;
    constexpr int kN = 8; // panels around the body
    const TorsoRing r = torso_ring_at(f.prof, t);
    const f32 y = t * f.span - f.torso_y0; // Torso-frame height of the belt
    for (int i = 0; i < kN; ++i) {
        const f32 ang = (static_cast<f32>(i) + 0.5f) * TwoPi / static_cast<f32>(kN); // a panel centred in front
        const Vec3 radial{std::sin(ang), 0.0f, std::cos(ang)};
        const Vec3 at{radial.x * (r.rx + inflate), y, r.dz + radial.z * (r.rz + inflate)};
        c.anchor_locals.push_back(seat_anchor(f, at, Vec3{0.0f}, c.collide, 0.02f));
        c.hang_locals.push_back(glm::normalize(radial * flare + Vec3{0.0f, -1.0f, 0.0f})); // down + flared
        ClothChain ch;
        ch.stiffness = 0.62f;
        ch.damping = 0.012f;
        ch.inner_damping = 0.09f;
        ch.wind_gain = wind_gain;
        c.chains.push_back(ch);
    }
    return c;
}

// A single narrow sheet (a stole band / a short mantle) hanging from a Torso-frame point.
ClothPiece make_sheet(const ClothFrame& f, const Vec3& at, const Vec3& hang, const Vec3& drape, int segments,
                      f32 seg, f32 width, const Vec3& color, f32 wind_gain, u8 groups) {
    ClothPiece c;
    c.anchor = BonePart::Torso;
    c.ring = false;
    c.drape_local = drape;
    c.collide = groups;
    c.segments = segments;
    c.seg = seg;
    c.half_width = width;
    c.color = color;
    c.side_local = Vec3{1.0f, 0.0f, 0.0f};
    c.anchor_locals = {seat_anchor(f, at, drape, groups, 0.03f)};
    c.hang_locals = {glm::normalize(hang)};
    ClothChain ch;
    ch.stiffness = 0.72f;
    ch.damping = 0.012f;
    ch.inner_damping = 0.08f;
    ch.wind_gain = wind_gain;
    c.chains.push_back(ch);
    return c;
}

} // namespace

std::vector<ClothPiece> outfit_cloth(const CharacterModel& m, OutfitKind kind, const Equipment& eq,
                                     const BodyColliders& body) {
    std::vector<ClothPiece> out;
    if (m.bone_count() < 13) {
        return out;
    }
    const ClothFrame f = cloth_frame(m, body);
    const int vt = outfit_design_tier(eq.outfit());
    const CharacterPalette& pal = m.palette();
    const f32 H = f.collar_height;                          // lengths scale with the figure
    const f32 waist = f.pelvis_height + 0.26f * f.span;     // the belt line the skirts hang from

    // Cloaks: the legendary paladin / high prophet (a gilt-edged cape / cope) and beastmaster (a
    // tattered dark cape), and a plain travelling cloak on the rare-tier knight - hanging to the calves.
    if (vt == 2 && (kind == OutfitKind::Plate || kind == OutfitKind::Holy)) {
        ClothPiece cape = make_cape(f, 6, 0.95f, 6, H * 0.8f, 0.035f, pal.primary, 1.2f);
        cape.device = kDeviceHem;
        cape.device_color = pal.accent;
        out.push_back(std::move(cape));
    } else if (vt == 2 && kind == OutfitKind::Leather) {
        out.push_back(make_cape(f, 6, 0.95f, 6, H * 0.72f, 0.035f, pal.dark, 1.5f));
    } else if (vt == 1 && kind == OutfitKind::Plate) {
        out.push_back(make_cape(f, 5, 0.85f, 5, H * 0.66f, 0.035f, pal.dark, 1.2f));
    }

    // Long flowing skirts from the belt: the Mage's robe (a gilt hem from the rare tier), the Cleric's
    // habit / alb (a gilt orphrey down the front from the rare tier), the Knight's heraldic surcoat (the
    // foot of its cross runs down the front panel).
    if (kind == OutfitKind::Robe) {
        ClothPiece s = make_skirt(f, 0.26f, 8, waist * 0.97f, 0.14f, 0.046f, pal.primary, 1.0f);
        s.device = vt >= 1 ? kDeviceHem : kDeviceNone;
        s.device_color = pal.accent;
        out.push_back(std::move(s));
    } else if (kind == OutfitKind::Holy) {
        ClothPiece s = make_skirt(f, 0.26f, 8, waist * 1.0f, 0.12f, 0.046f, pal.primary, 0.9f);
        s.device = vt >= 1 ? static_cast<u8>(kDevicePale | (vt == 2 ? kDeviceHem : 0)) : kDeviceNone;
        s.device_color = pal.accent;
        out.push_back(std::move(s));
    } else if (kind == OutfitKind::Plate && vt == 1) {
        ClothPiece s = make_skirt(f, 0.26f, 5, waist * 0.55f, 0.14f, 0.066f, pal.primary, 0.7f);
        s.device = kDevicePale;
        s.device_color = pal.accent;
        out.push_back(std::move(s));
    }

    if (kind == OutfitKind::Holy && vt >= 1) {
        // Priest / prophet stole: two narrow bands hanging from the shoulders down the front.
        for (f32 ex : {-1.0f, 1.0f}) {
            const TorsoRing r = torso_ring_at(f.prof, 0.86f);
            out.push_back(make_sheet(f, Vec3{ex * 0.07f, 0.86f * f.span - f.torso_y0, r.dz + r.rz},
                                     Vec3{0.0f, -1.0f, 0.06f}, Vec3{0.0f, 0.0f, 1.0f}, 5, H * 0.1f, 0.04f,
                                     pal.dark, 0.5f, kBodyTorso | kBodyHips | kBodyLegs));
        }
    }
    if (kind == OutfitKind::Leather && vt == 1) {
        // Warden's shoulder mantle: a short, wide cape off the upper back.
        out.push_back(make_sheet(f, Vec3{0.0f, f.shoulder_y + 0.06f, f.fit.torso_center_z - f.fit.torso_half_depth},
                                 Vec3{0.0f, -1.0f, -0.32f}, Vec3{0.0f, 0.0f, -1.0f}, 3, H * 0.09f, 0.28f,
                                 pal.dark, 1.3f, kBodyAll));
    }
    return out;
}

ClothPiece noble_cape(const CharacterModel& m, const BodyColliders& body) {
    const ClothFrame f = cloth_frame(m, body);
    // A grand, oversized crimson cloak: a wide arc of LONG panels off the shoulder line, fuller +
    // longer than a player cape, so it sweeps behind the noble as the covered wagon rolls.
    ClothPiece c = make_cape(f, 7, 1.05f, 8, f.collar_height * 1.02f, 0.05f, Vec3{0.66f, 0.10f, 0.12f}, 1.3f);
    c.device = kDeviceHem;
    c.device_color = Vec3{0.92f, 0.76f, 0.32f}; // ermine-and-gold edging
    return c;
}

void step_cloth(ClothPiece& c, const CharacterModel& model, const std::vector<Mat4>& joints,
                const Mat4& root, const ClothEnv& env) {
    if (c.chains.empty() || c.anchor_locals.size() != c.chains.size()) {
        return;
    }
    const int bi = model.bone_index(c.anchor);
    const Mat4& abone = (bi >= 0 && static_cast<usize>(bi) < joints.size()) ? joints[static_cast<usize>(bi)] : root;
    const Mat3 root_rot{root};
    std::vector<Vec3> anchors(c.chains.size());
    for (usize k = 0; k < c.chains.size(); ++k) {
        anchors[k] = Vec3{abone * Vec4{c.anchor_locals[k], 1.0f}};
    }
    const Vec3 dl = root_rot * c.drape_local;
    const Vec3 drape = glm::length(dl) > 1e-5f ? glm::normalize(dl) : Vec3{0.0f};
    // A skirt belongs OUTSIDE the body all round: each node's drape side is straight out from the
    // waist's centre, so a striding leg that overtakes the front panel shoves it forward, not behind.
    const bool radial = c.ring && c.closed && glm::length(dl) <= 1e-5f;
    Vec3 waist{0.0f};
    for (const Vec3& a : anchors) {
        waist += a / static_cast<f32>(anchors.size());
    }
    auto drape_at = [&](const Vec3& p) {
        if (!radial) {
            return drape;
        }
        const Vec3 out{p.x - waist.x, 0.0f, p.z - waist.z};
        return glm::length(out) > 1e-4f ? glm::normalize(out) : Vec3{0.0f};
    };
    const usize n = c.chains.size();
    const usize rows = static_cast<usize>(c.segments) + 1;
    const usize pairs = n < 2 ? 0 : (c.closed ? n : n - 1);

    // One sub-step of the whole sheet: integrate, then relax the along-chain + across-chain lengths
    // interleaved with the body + ground collisions, so the constraints never drag a node back inside.
    std::vector<Vec3> body_step(n, Vec3{0.0f}); // each chain's anchor motion this sub-step (the body's)
    auto substep = [&](f32 h, f32 blend, const std::vector<Vec3>& from, const Vec3& wind) {
        for (usize k = 0; k < n; ++k) {
            ClothChain& ch = c.chains[k];
            const Vec3 accel = Vec3{0.0f, -env.gravity, 0.0f} + wind * ch.wind_gain;
            const f32 retain = std::pow(1.0f - ch.damping, h * 60.0f); // damping tuned at 60 Hz
            const f32 keep = std::pow(1.0f - ch.inner_damping, h * 60.0f);
            const Vec3 target = glm::mix(from[k], anchors[k], blend);
            body_step[k] = ch.pos.empty() ? Vec3{0.0f} : target - ch.pos[0];
            ch.integrate(target, accel, retain, h, keep);
        }
        for (int it = 0; it < 8; ++it) {
            for (ClothChain& ch : c.chains) {
                ch.relax();
            }
            for (usize pi = 0; pi < pairs; ++pi) {
                ClothChain& a = c.chains[pi];
                ClothChain& b = c.chains[(pi + 1) % n];
                for (usize r = 1; r < rows && r < a.pos.size() && r < b.pos.size(); ++r) {
                    const f32 rest = c.cross_rest[pi * rows + r];
                    const Vec3 d = b.pos[r] - a.pos[r];
                    const f32 len = glm::length(d);
                    if (len < 1e-5f) {
                        continue;
                    }
                    // The weave resists stretching; it may bunch up a little (folds) but not collapse.
                    const f32 target = len > rest ? rest : len < rest * c.weave ? rest * c.weave : len;
                    const Vec3 corr = d * ((len - target) / len * 0.3f);
                    a.pos[r] += corr;
                    b.pos[r] -= corr;
                }
            }
            if (it % 2 == 0) {
                continue; // collide every other pass (always on the last) - plenty, at half the cost
            }
            const f32 ground = env.ground + 0.015f;
            for (usize k = 0; k < n; ++k) {
                ClothChain& ch = c.chains[k];
                for (usize i = 1; i < ch.pos.size(); ++i) {
                    collide_cloth_node(ch.pos[i], ch.prev[i], env.body, drape_at(ch.pos[i]), env.margin,
                                       env.friction, c.collide, body_step[k]);
                    if (ch.pos[i].y < ground) {
                        ch.pos[i].y = ground;
                        ch.prev[i].y = std::min(ch.prev[i].y, ground);
                        ch.prev[i].x = glm::mix(ch.prev[i].x, ch.pos[i].x, env.friction);
                        ch.prev[i].z = glm::mix(ch.prev[i].z, ch.pos[i].z, env.friction);
                    }
                }
            }
        }
    };

    // A teleport (a respawn, a snap onto a seat): re-seat rather than whip the cloth across the world.
    if (c.inited && !c.chains[0].pos.empty() && glm::distance(c.chains[0].pos[0], anchors[0]) > 1.5f) {
        c.inited = false;
    }
    if (!c.inited) {
        for (usize k = 0; k < n; ++k) {
            const Vec3 hang = glm::normalize(root_rot * c.hang_locals[k]);
            c.chains[k].init(anchors[k], hang, c.segments, c.seg, c.half_width);
        }
        c.cross_rest.assign(pairs * rows, 0.0f);
        for (usize pi = 0; pi < pairs; ++pi) {
            const ClothChain& a = c.chains[pi];
            const ClothChain& b = c.chains[(pi + 1) % n];
            for (usize r = 0; r < rows; ++r) {
                c.cross_rest[pi * rows + r] = glm::length(b.pos[r] - a.pos[r]);
            }
        }
        c.inited = true;
        // Let it settle against the body before the first frame shows (no flailing on spawn).
        for (int k = 0; k < 30; ++k) {
            substep(1.0f / 60.0f, 1.0f, anchors, Vec3{0.0f});
        }
    }

    std::vector<Vec3> from(n);
    for (usize k = 0; k < n; ++k) {
        from[k] = c.chains[k].pos.empty() ? anchors[k] : c.chains[k].pos[0];
    }
    // Sub-step long frames so a fast limb can't skip a cloth node clean through itself.
    const int steps = glm::clamp(static_cast<int>(std::ceil(env.dt * 60.0f - 1e-3f)), 1, 4);
    const f32 h = env.dt / static_cast<f32>(steps);
    for (int s = 0; s < steps; ++s) {
        substep(h, static_cast<f32>(s + 1) / static_cast<f32>(steps), from, env.wind);
    }
}

void build_cloth_mesh(const ClothChain& c, const Vec3& side, const Vec3& color, MeshData& out,
                      const Vec3& outward) {
    out.vertices.clear();
    out.indices.clear();
    const usize n = c.pos.size();
    if (n < 2) {
        return;
    }
    const Vec3 s = glm::length(side) > 1e-5f ? glm::normalize(side) : Vec3{1.0f, 0.0f, 0.0f};
    const bool oriented = glm::length(outward) > 1e-5f;

    auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& d, const Vec3& na, const Vec3& nb, const Vec3& nd) {
        const u32 base = static_cast<u32>(out.vertices.size());
        out.vertices.push_back(Vertex{a, na, color, 0.0f});
        out.vertices.push_back(Vertex{b, nb, color, 0.0f});
        out.vertices.push_back(Vertex{d, nd, color, 0.0f});
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
    };
    auto width_at = [&](usize i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n - 1);
        return c.half_width * (1.0f - 0.18f * t); // taper a touch toward the hem
    };
    // Per-segment normals (facing `outward` when given), averaged into the nodes for soft shading.
    std::vector<Vec3> seg_n(n - 1), node_n(n, Vec3{0.0f});
    for (usize i = 0; i + 1 < n; ++i) {
        Vec3 nrm = glm::cross(c.pos[i + 1] - c.pos[i], s);
        nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : Vec3{0.0f, 0.0f, 1.0f};
        if (oriented && glm::dot(nrm, outward) < 0.0f) {
            nrm = -nrm;
        }
        seg_n[i] = nrm;
        node_n[i] += nrm;
        node_n[i + 1] += nrm;
    }
    for (usize i = 0; i < n; ++i) {
        node_n[i] = glm::length(node_n[i]) > 1e-5f ? glm::normalize(node_n[i]) : seg_n[std::min(i, n - 2)];
    }
    // The inner face sits a hair behind the outer one (so the side facing the camera always wins the
    // depth test and lights with its own normal); unoriented sheets stay coplanar as before.
    const f32 back = oriented ? 0.004f : 0.0f;
    for (usize i = 0; i + 1 < n; ++i) {
        const f32 w0 = width_at(i), w1 = width_at(i + 1);
        const Vec3 l0 = c.pos[i] - s * w0, r0 = c.pos[i] + s * w0;
        const Vec3 l1 = c.pos[i + 1] - s * w1, r1 = c.pos[i + 1] + s * w1;
        const Vec3 n0 = oriented ? node_n[i] : seg_n[i];
        const Vec3 n1 = oriented ? node_n[i + 1] : seg_n[i];
        tri(l0, r0, r1, n0, n0, n1);
        tri(l0, r1, l1, n0, n1, n1);
        const Vec3 o0 = -n0 * back, o1 = -n1 * back;
        tri(l0 + o0, r1 + o1, r0 + o0, -n0, -n1, -n0);
        tri(l0 + o0, l1 + o1, r1 + o1, -n0, -n1, -n1);
    }
}

void build_cloth_tube(const std::vector<ClothChain>& chains, bool closed, const Vec3& color,
                      MeshData& out, u8 device, const Vec3& device_color, const Vec3* axis) {
    out.vertices.clear();
    out.indices.clear();
    const usize nc = chains.size();
    if (nc < 2 || chains[0].pos.size() < 2) {
        return;
    }
    const usize rows = chains[0].pos.size();
    for (const ClothChain& ch : chains) {
        if (ch.pos.size() != rows) {
            return;
        }
    }
    const usize panels = closed ? nc : nc - 1;
    // The pale runs down the panel facing most nearly +Z (straight ahead, in the character's frame).
    usize front = nc;
    if ((device & kDevicePale) != 0u) {
        f32 best = -1e9f;
        for (usize i = 0; i < panels; ++i) {
            const f32 z = chains[i].pos[0].z + chains[(i + 1) % nc].pos[0].z;
            if (z > best) {
                best = z;
                front = i;
            }
        }
    }
    // Outer faces look away from a vertical axis: the body's when given, else the cloth's own centre.
    Vec3 ctr{0.0f};
    if (axis != nullptr) {
        ctr = *axis;
    } else {
        for (const ClothChain& ch : chains) {
            for (const Vec3& p : ch.pos) {
                ctr += p;
            }
        }
        ctr /= static_cast<f32>(nc * rows);
    }
    auto node = [&](usize i, usize r) -> const Vec3& { return chains[i % nc].pos[r]; };
    std::vector<Vec3> node_n(nc * rows, Vec3{0.0f});
    for (usize i = 0; i < panels; ++i) {
        for (usize r = 0; r + 1 < rows; ++r) {
            const Vec3 a = node(i, r), b = node(i + 1, r), d = node(i + 1, r + 1), e = node(i, r + 1);
            Vec3 nrm = glm::cross(d - a, b - e); // the diagonals' cross: robust for a skewed quad
            nrm = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : Vec3{0.0f, 0.0f, 1.0f};
            Vec3 away = (a + b + d + e) * 0.25f - ctr;
            away.y = 0.0f;
            if (glm::dot(nrm, away) < 0.0f) {
                nrm = -nrm;
            }
            for (const usize k : {i % nc, (i + 1) % nc}) {
                node_n[k * rows + r] += nrm;
                node_n[k * rows + r + 1] += nrm;
            }
        }
    }
    for (Vec3& v : node_n) {
        v = glm::length(v) > 1e-6f ? glm::normalize(v) : Vec3{0.0f, 0.0f, 1.0f};
    }

    Vec3 col = color;
    auto vtx = [&](const Vec3& p, const Vec3& nrm) {
        out.vertices.push_back(Vertex{p, nrm, col, 0.0f});
        out.indices.push_back(static_cast<u32>(out.vertices.size() - 1));
    };
    constexpr f32 kBack = 0.004f; // the inner face sits a hair behind the outer one
    for (usize i = 0; i < panels; ++i) {
        const usize ii = i % nc, j = (i + 1) % nc;
        for (usize r = 0; r + 1 < rows; ++r) {
            const bool hem = (device & kDeviceHem) != 0u && r + 2 == rows;
            col = (i == front || hem) ? device_color : color;
            const Vec3 a = node(ii, r), b = node(j, r), d = node(j, r + 1), e = node(ii, r + 1);
            const Vec3 na = node_n[ii * rows + r], nb = node_n[j * rows + r];
            const Vec3 nd = node_n[j * rows + r + 1], ne = node_n[ii * rows + r + 1];
            vtx(a, na);
            vtx(b, nb);
            vtx(d, nd);
            vtx(a, na);
            vtx(d, nd);
            vtx(e, ne);
            vtx(a - na * kBack, -na); // the inner face
            vtx(d - nd * kBack, -nd);
            vtx(b - nb * kBack, -nb);
            vtx(a - na * kBack, -na);
            vtx(e - ne * kBack, -ne);
            vtx(d - nd * kBack, -nd);
        }
    }
}

} // namespace alryn
