#include <Alryn/World/PropLibrary.h>

#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/RoadNetwork.h> // shared bridge deck geometry (build_plank_bridge)

#include <utility>

namespace alryn {

PropDef PropLibrary::build_bush(int variant) {
    PropDef def;
    def.name = "bush";
    // Autumn shrub palette to match the reference: mostly green, with gold + russet turners.
    static const Vec3 shrub[] = {{0.24f, 0.43f, 0.20f},  // green
                                 {0.30f, 0.46f, 0.22f},  // lighter green
                                 {0.62f, 0.46f, 0.18f},  // golden autumn
                                 {0.58f, 0.30f, 0.15f},  // russet autumn
                                 {0.30f, 0.40f, 0.18f}}; // olive green
    def.parts.push_back({primitives::bush(variant, shrub[variant % 5]), PropLayer::Foliage});
    return def;
}

PropDef PropLibrary::build_rock(int variant) {
    PropDef def;
    def.name = "rock";
    def.parts.push_back({primitives::rock(variant), PropLayer::Opaque});
    // A boulder you bump into: a box over the squashed sphere's footprint (~±0.45 in xz,
    // sitting on the ground) so the player and a towed wagon are pushed around it.
    BoxCollider c;
    c.center = Vec3{0.0f};
    c.half_extents = Vec2{0.45f, 0.45f};
    c.height = 0.6f;
    def.colliders.push_back(c);
    return def;
}

PropDef PropLibrary::build_log(int variant) {
    PropDef def;
    def.name = "log";
    const Vec3 bark{0.34f, 0.26f, 0.18f};
    def.parts.push_back({primitives::fallen_log(variant, bark), PropLayer::Opaque});
    // The log lies along local +X; a box over its xz extent blocks the player. It
    // rotates with the prop's yaw, so it stays aligned with the visible log.
    BoxCollider c;
    c.center = Vec3{0.0f};
    c.half_extents = Vec2{0.85f, 0.26f};
    c.height = 0.55f;
    def.colliders.push_back(c);
    return def;
}

namespace {
// Appends one axis-aligned box into `m`.
void add_box(MeshData& m, const Vec3& lo, const Vec3& hi, const Vec3& color) {
    const MeshData b = primitives::box(lo, hi, color);
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.insert(m.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (u32 i : b.indices) {
        m.indices.push_back(base + i);
    }
}
// Flat-shaded quad (a,b,c,d CCW) / triangle with auto normals (for roofs).
void add_quad(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              const Vec3& color) {
    const Vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, color, 0.0f});
    m.vertices.push_back({b, n, color, 0.0f});
    m.vertices.push_back({c, n, color, 0.0f});
    m.vertices.push_back({d, n, color, 0.0f});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
}
// Like add_tri, but orients the flat normal away from `center` (winding-independent).
void emit_tri(MeshData& m, Vec3 a, Vec3 b, Vec3 c, const Vec3& center, const Vec3& color) {
    Vec3 n = glm::cross(b - a, c - a);
    const f32 len = glm::length(n);
    if (len <= 1e-9f) {
        return;
    }
    n /= len;
    if (glm::dot(n, (a + b + c) / 3.0f - center) < 0.0f) {
        n = -n;
        std::swap(b, c);
    }
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, color, 0.0f});
    m.vertices.push_back({b, n, color, 0.0f});
    m.vertices.push_back({c, n, color, 0.0f});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}
void add_tri(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color) {
    const Vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, color, 0.0f});
    m.vertices.push_back({b, n, color, 0.0f});
    m.vertices.push_back({c, n, color, 0.0f});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}

// Deterministic 0..1 hash for procedural jitter (stone shades, thatch lumps, ...).
inline f32 hashf(u32 s) {
    u32 v = s * 2654435761u + 0x9E3779B9u;
    v ^= v >> 15;
    v *= 0x2545F491u;
    v ^= v >> 13;
    return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
}

// Textured masonry: lay a running-bond grid of slightly-proud, shade-jittered stone blocks across a
// vertical wall FACE, so a stone wall reads as stacked individual stones (not a flat box). `along_x`:
// the face spans the x axis at constant z=`face` (else spans z at x=`face`); `outdir` (+1/-1) is the
// outward direction. Covers the in-plane span [a0,a1] x height [y0,y1].
void stone_face(MeshData& m, bool along_x, f32 face, f32 outdir, f32 a0, f32 a1, f32 y0, f32 y1,
                const Vec3& base, u32 seed) {
    const f32 ch = 0.32f;
    const int rows = std::max(1, static_cast<int>(std::round((y1 - y0) / ch)));
    const f32 rh = (y1 - y0) / static_cast<f32>(rows);
    u32 s = seed * 131u + 7u;
    for (int r = 0; r < rows; ++r) {
        const f32 yl = y0 + static_cast<f32>(r) * rh;
        const f32 yh = yl + rh - 0.035f;
        f32 a = a0 - (r % 2 ? 0.17f : 0.0f); // running-bond stagger
        while (a < a1 - 0.04f) {
            const f32 bw = 0.3f + 0.25f * hashf(s);
            const f32 left = std::max(a, a0);
            const f32 right = std::min(a1, a + bw);
            const f32 p = 0.05f + 0.025f * hashf(s + 1);
            const Vec3 c = base * (0.82f + 0.3f * hashf(s + 2));
            if (right - left > 0.07f) {
                if (along_x) {
                    const f32 z0 = outdir > 0.0f ? face - 0.02f : face - p;
                    const f32 z1 = outdir > 0.0f ? face + p : face + 0.02f;
                    add_box(m, {left + 0.03f, yl, z0}, {right - 0.03f, yh, z1}, c);
                } else {
                    const f32 x0 = outdir > 0.0f ? face - 0.02f : face - p;
                    const f32 x1 = outdir > 0.0f ? face + p : face + 0.02f;
                    add_box(m, {x0, yl, left + 0.03f}, {x1, yh, right - 0.03f}, c);
                }
            }
            a += bw;
            ++s;
        }
    }
}

// Prominent half-timber framing over one rectangular wall panel: thick end posts, sill + head
// plates, evenly spaced studs and two diagonal corner braces - all standing clearly PROUD of the
// daub (so the beams stick out, the reference look). `along_x`: panel runs along x at z=`face`
// (else along z at x=`face`); `outdir` +1/-1 outward. Spans [a_lo,a_hi] x [y0,y1]; skips a central
// opening of half-width `gap` (<=0 for none).
void timber_frame(MeshData& m, bool along_x, f32 face, f32 outdir, f32 a_lo, f32 a_hi, f32 y0,
                  f32 y1, f32 gap, const Vec3& col) {
    const f32 proud = 0.12f, tk = 0.1f;
    const f32 p0 = outdir > 0.0f ? face - 0.02f : face - proud;
    const f32 p1 = outdir > 0.0f ? face + proud : face + 0.02f;
    const f32 pf = outdir > 0.0f ? face + proud : face - proud;
    auto bar = [&](f32 al, f32 ah, f32 yl, f32 yh) {
        if (along_x) {
            add_box(m, {al, yl, p0}, {ah, yh, p1}, col);
        } else {
            add_box(m, {p0, yl, al}, {p1, yh, ah}, col);
        }
    };
    bar(a_lo, a_hi, y0, y0 + 0.16f);     // sill
    bar(a_lo, a_hi, y1 - 0.16f, y1);     // head plate
    bar(a_lo, a_lo + 2.0f * tk, y0, y1); // end post (left)
    bar(a_hi - 2.0f * tk, a_hi, y0, y1); // end post (right)
    const f32 mid = (a_lo + a_hi) * 0.5f;
    const int studs = std::max(2, static_cast<int>(std::round((a_hi - a_lo) / 0.8f)));
    const u32 hbase = static_cast<u32>(std::abs(a_lo) * 53.0f + std::abs(face) * 97.0f);
    for (int i = 1; i < studs; ++i) {
        // slight per-stud position + width jitter so the framing isn't a dead-straight grid
        const f32 jit = (hashf(hbase + static_cast<u32>(i) * 9u) - 0.5f) * 0.14f;
        const f32 a = glm::mix(a_lo, a_hi, static_cast<f32>(i) / static_cast<f32>(studs)) + jit;
        if (gap > 0.0f && std::abs(a - mid) < gap) {
            continue;
        }
        const f32 hw = tk * (0.85f + 0.3f * hashf(hbase + static_cast<u32>(i) * 5u + 1u));
        bar(a - hw, a + hw, y0, y1);
    }
    auto brace = [&](Vec2 q0, Vec2 q1) {
        Vec2 dir = q1 - q0;
        const f32 dl = glm::length(dir);
        if (dl <= 0.4f) return;
        dir /= dl;
        const Vec2 nrm{-dir.y * 0.1f, dir.x * 0.1f};
        auto P = [&](const Vec2& v) { return along_x ? Vec3{v.x, v.y, pf} : Vec3{pf, v.y, v.x}; };
        add_quad(m, P(q0 + nrm), P(q1 + nrm), P(q1 - nrm), P(q0 - nrm), col);
    };
    brace({a_lo + 0.22f, y1 - 0.22f}, {a_lo + 1.0f, y0 + 0.22f});
    brace({a_hi - 0.22f, y1 - 0.22f}, {a_hi - 1.0f, y0 + 0.22f});
}

// A framed, warm-lit window centred at `ctr` on a wall face. `across`/`up` are unit in-plane axes,
// `outward` the outward face normal; half-size (hw,hh). Adds the glowing pane (em) and a cross
// mullion + proud timber frame (op) so the window reads as framed.
void lit_window(MeshData& op, MeshData& em, const Vec3& ctr, const Vec3& across, const Vec3& up,
                const Vec3& outward, f32 hw, f32 hh, const Vec3& glow, const Vec3& frame) {
    add_quad(em, ctr - across * hw - up * hh, ctr + across * hw - up * hh, ctr + across * hw + up * hh,
             ctr - across * hw + up * hh, glow);
    const Vec3 o = outward * 0.05f;
    add_quad(op, ctr - across * 0.04f - up * hh + o, ctr + across * 0.04f - up * hh + o,
             ctr + across * 0.04f + up * hh + o, ctr - across * 0.04f + up * hh + o, frame);
    add_quad(op, ctr - across * hw - up * 0.04f + o, ctr + across * hw - up * 0.04f + o,
             ctr + across * hw + up * 0.04f + o, ctr - across * hw + up * 0.04f + o, frame);
    const Vec3 of = outward * 0.07f;
    const f32 fb = 0.1f;
    auto fq = [&](f32 c0, f32 c1, f32 u0, f32 u1) {
        add_quad(op, ctr + across * c0 + up * u0 + of, ctr + across * c1 + up * u0 + of,
                 ctr + across * c1 + up * u1 + of, ctr + across * c0 + up * u1 + of, frame);
    };
    fq(-hw - fb, hw + fb, hh, hh + fb);    // top rail
    fq(-hw - fb, hw + fb, -hh - fb, -hh);   // bottom rail
    fq(-hw - fb, -hw, -hh - fb, hh + fb);   // left jamb
    fq(hw, hw + fb, -hh - fb, hh + fb);     // right jamb
}

// A stepped-shingle (or thatch) gable roof over a [-w,w]x[-d,d] footprint, into `shell`. For the
// hand-built Synty look the roofline SWOOPS - the ridge sags in the middle and the eave corners kick
// up - and each slope is a grid of INDIVIDUAL chunky tiles (jittered course lines, per-tile depth +
// shade, small gaps), not flat bands. Plus dark eave fascia, rake barge-boards, a curved ridge cap
// and `fill` daub gable triangles. Ridge runs along the longer horizontal axis. Returns apex height.
f32 gable_roof(MeshData& shell, f32 w, f32 d, f32 h, f32 rr, f32 oh, bool thatch, const Vec3& roof,
               const Vec3& trim, const Vec3& fill, u32 seed) {
    const bool gx = w >= d;
    const f32 W = (gx ? w : d) + oh;  // ridge/eave half along the ridge axis (incl. overhang)
    const f32 EH = (gx ? d : w) + oh; // eave offset (the ridge sits at 0)
    const f32 gwall = gx ? w : d;     // gable-end wall position (no overhang)
    const f32 dwall = gx ? d : w;     // wall half on the eave axis
    const int courses = std::max(4, static_cast<int>(std::round(rr / 0.32f)));
    const int rows = thatch ? std::max(4, courses - 1) : std::max(6, courses + 2);
    const int seg = std::max(5, static_cast<int>(std::round(W / 0.62f))); // strips -> curve + tile width
    const f32 b = 0.04f;
    const f32 step = thatch ? 0.2f : 0.13f;
    const Vec3 lipc = thatch ? roof * 0.58f : trim;
    // The swoop: ridge sags `sag` in the middle, eave corners kick up `kick`.
    const f32 sag = (thatch ? 0.16f : 0.12f) * rr;
    const f32 kick = thatch ? 0.12f : 0.18f;
    auto ridgeY = [&](f32 u) { return h + rr - sag * (1.0f - u * u); };
    auto eaveY = [&](f32 u) { return h + kick * (u * u); };
    auto P = [&](f32 s, f32 e, f32 y) { return gx ? Vec3{s, y, e} : Vec3{e, y, s}; };
    auto eavePt = [&](f32 s, f32 dir) { return P(s, dir * EH, eaveY(s / W)); };
    auto ridgePt = [&](f32 s) { return P(s, 0.0f, ridgeY(s / W)); };
    // a quad with an explicit (outward) normal, so a tile lights correctly whichever way it's wound.
    auto quadN = [&](const Vec3& a, const Vec3& bb, const Vec3& c, const Vec3& e, const Vec3& col,
                     const Vec3& nn) {
        const u32 base = static_cast<u32>(shell.vertices.size());
        shell.vertices.push_back({a, nn, col, 0.0f});
        shell.vertices.push_back({bb, nn, col, 0.0f});
        shell.vertices.push_back({c, nn, col, 0.0f});
        shell.vertices.push_back({e, nn, col, 0.0f});
        shell.indices.insert(shell.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
    };
    auto build_slope = [&](f32 dir) {
        for (int sg = 0; sg < seg; ++sg) {
            const f32 sA = glm::mix(-W, W, static_cast<f32>(sg) / seg);     // full strip (under-deck)
            const f32 sB = glm::mix(-W, W, static_cast<f32>(sg + 1) / seg);
            const f32 s0 = sA + 0.018f, s1 = sB - 0.018f;                   // tiles (small gaps)
            auto pt = [&](f32 s, f32 t) { return glm::mix(eavePt(s, dir), ridgePt(s), t); };
            const Vec3 n = glm::normalize(glm::cross(pt(s1, 0.0f) - pt(s0, 0.0f), pt(s0, 1.0f) - pt(s0, 0.0f))) * dir;
            // a solid dark under-deck spanning the whole strip, so the gaps between tiles read as
            // shadow rather than see-through to the interior / gable behind.
            quadN(glm::mix(eavePt(sA, dir), ridgePt(sA), 0.0f) + n * (b * 0.4f),
                  glm::mix(eavePt(sB, dir), ridgePt(sB), 0.0f) + n * (b * 0.4f),
                  glm::mix(eavePt(sB, dir), ridgePt(sB), 1.0f) + n * (b * 0.4f),
                  glm::mix(eavePt(sA, dir), ridgePt(sA), 1.0f) + n * (b * 0.4f), roof * 0.42f, n);
            for (int k = 0; k < rows; ++k) {
                const f32 jt = 0.45f / static_cast<f32>(rows);
                const f32 t0 = glm::clamp(static_cast<f32>(k) / rows + (hashf(seed + sg * 17u + k) - 0.5f) * jt, 0.0f, 1.0f);
                const f32 t1 = glm::clamp(static_cast<f32>(k + 1) / rows + (hashf(seed + sg * 17u + k + 1u) - 0.5f) * jt, 0.0f, 0.999f);
                if (t1 <= t0 + 0.02f) {
                    continue;
                }
                const f32 hi = step * (0.8f + 0.5f * hashf(seed + sg * 7u + k));
                const Vec3 col = roof * (0.78f + 0.26f * hashf(seed + sg * 11u + k));
                const Vec3 LL = pt(s0, t0), LH = pt(s0, t1), RL = pt(s1, t0), RH = pt(s1, t1);
                quadN(LL + n * b, RL + n * b, RH + n * (b + hi), LH + n * (b + hi), col, n); // tile tread
                quadN(LH + n * (b + hi), RH + n * (b + hi), RH + n * b, LH + n * b, lipc, n); // step lip
            }
            // eave fascia (a dark board hung under the strip's eave, following the curve)
            const f32 ya = eaveY(s0 / W), yb = eaveY(s1 / W);
            const f32 ylo = std::min(ya, yb) - 0.22f, yhi = std::max(ya, yb) + 0.04f;
            const f32 e0 = dir > 0.0f ? EH - 0.03f : -EH - 0.11f, e1 = dir > 0.0f ? EH + 0.11f : -EH + 0.03f;
            if (gx) {
                add_box(shell, {s0, ylo, e0}, {s1, yhi, e1}, trim);
            } else {
                add_box(shell, {e0, ylo, s0}, {e1, yhi, s1}, trim);
            }
        }
    };
    build_slope(1.0f);
    build_slope(-1.0f);
    // a ridge cap that follows the sagging ridge curve
    for (int sg = 0; sg < seg; ++sg) {
        const f32 s0 = glm::mix(-W, W, static_cast<f32>(sg) / seg);
        const f32 s1 = glm::mix(-W, W, static_cast<f32>(sg + 1) / seg);
        const f32 y0 = ridgeY(s0 / W) + b + step, y1 = ridgeY(s1 / W) + b + step;
        const f32 lo = std::min(y0, y1) - 0.07f, hy = std::max(y0, y1) + 0.12f;
        if (gx) {
            add_box(shell, {s0, lo, -0.13f}, {s1, hy, 0.13f}, trim);
        } else {
            add_box(shell, {-0.13f, lo, s0}, {0.13f, hy, s1}, trim);
        }
    }
    // gable-end daub triangle + rake barge-boards, both ends
    const Vec3 mid{0.0f, ridgeY(gwall / W) * 0.5f, 0.0f};
    for (f32 dir : {-1.0f, 1.0f}) {
        const f32 se = dir * gwall;
        emit_tri(shell, P(se, -dwall, h), P(se, dwall, h), P(se, 0.0f, ridgeY(gwall / W)), mid, fill);
        const Vec3 out = gx ? Vec3{dir, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, dir};
        const Vec3 ro = out * 0.06f, dn{0.0f, 0.18f, 0.0f};
        const Vec3 ap = ridgePt(dir * W) + ro;
        add_quad(shell, eavePt(dir * W, 1.0f) + ro, ap, ap - dn, eavePt(dir * W, 1.0f) + ro - dn, trim);
        add_quad(shell, eavePt(dir * W, -1.0f) + ro, ap, ap - dn, eavePt(dir * W, -1.0f) + ro - dn, trim);
    }
    return ridgeY(1.0f) + b + step; // tallest point (the gable apexes)
}

// ---- Shared yard decor (used by houses + the pub garden) ------------------------------------
// A low-poly barrel (stacked boxes, slightly belled, with dark iron hoops).
void add_barrel(MeshData& m, const Vec3& base, f32 r, f32 ht) {
    const Vec3 stave{0.46f, 0.3f, 0.16f}, hoop{0.18f, 0.16f, 0.15f};
    add_box(m, {base.x - r, base.y, base.z - r}, {base.x + r, base.y + ht, base.z + r}, stave);
    add_box(m, {base.x - r * 1.12f, base.y + ht * 0.12f, base.z - r * 1.12f},
            {base.x + r * 1.12f, base.y + ht * 0.9f, base.z + r * 1.12f}, stave * 1.05f);
    add_box(m, {base.x - r * 1.16f, base.y + ht * 0.2f, base.z - r * 1.16f},
            {base.x + r * 1.16f, base.y + ht * 0.32f, base.z + r * 1.16f}, hoop);
    add_box(m, {base.x - r * 1.16f, base.y + ht * 0.66f, base.z - r * 1.16f},
            {base.x + r * 1.16f, base.y + ht * 0.78f, base.z + r * 1.16f}, hoop);
}
// A stack of cut logs (cut faces lighter, facing +z), in a pyramid - a woodpile against a wall.
void add_woodpile(MeshData& m, const Vec3& c, f32 len, int rows) {
    const Vec3 bark{0.36f, 0.25f, 0.15f}, cut{0.62f, 0.46f, 0.28f};
    const f32 lr = 0.13f, gap = lr * 2.05f;
    for (int r = 0; r < rows; ++r) {
        const int cnt = 5 - r;
        if (cnt < 1) break;
        for (int i = 0; i < cnt; ++i) {
            const f32 x = c.x + (static_cast<f32>(i) - (cnt - 1) * 0.5f) * gap;
            const f32 y = c.y + static_cast<f32>(r) * (lr * 1.85f);
            add_box(m, {x - lr, y, c.z - len * 0.5f}, {x + lr, y + lr * 1.7f, c.z + len * 0.5f},
                    bark * (0.88f + 0.22f * hashf(static_cast<u32>(x * 53.0f) + r * 7u)));
            add_box(m, {x - lr * 0.88f, y + 0.02f, c.z + len * 0.5f - 0.02f},
                    {x + lr * 0.88f, y + lr * 1.6f, c.z + len * 0.5f + 0.03f}, cut);
        }
    }
}
// Stone steps descending from a door at z=`fz`, centred on x=`cx`.
void add_stone_steps(MeshData& m, f32 cx, f32 fz, f32 hw) {
    const Vec3 s{0.6f, 0.61f, 0.63f};
    add_box(m, {cx - hw, 0.0f, fz}, {cx + hw, 0.3f, fz + 0.28f}, s * 0.96f);          // upper step
    add_box(m, {cx - hw - 0.12f, 0.0f, fz + 0.24f}, {cx + hw + 0.12f, 0.16f, fz + 0.62f}, s); // lower step
}
// An axis-aligned run of fence (posts + two rails) from a to b (which differ on one horizontal axis).
void add_fence_run(MeshData& m, const Vec3& a, const Vec3& b) {
    const Vec3 wood{0.42f, 0.3f, 0.17f};
    const f32 len = glm::length(b - a);
    const int n = std::max(1, static_cast<int>(std::round(len / 0.95f)));
    for (int i = 0; i <= n; ++i) {
        const Vec3 p = glm::mix(a, b, static_cast<f32>(i) / static_cast<f32>(n));
        add_box(m, {p.x - 0.055f, 0.0f, p.z - 0.055f}, {p.x + 0.055f, 0.72f, p.z + 0.055f}, wood);
    }
    const Vec3 lo = glm::min(a, b), hi = glm::max(a, b);
    for (f32 ry : {0.28f, 0.54f}) {
        add_box(m, {lo.x - 0.04f, ry, lo.z - 0.04f}, {hi.x + 0.04f, ry + 0.08f, hi.z + 0.04f}, wood * 1.05f);
    }
}
// A clump of grass blades.
void add_grass_tuft(MeshData& m, const Vec3& c) {
    const Vec3 g{0.36f, 0.56f, 0.24f};
    for (int i = 0; i < 5; ++i) {
        const f32 ang = static_cast<f32>(i) * 1.7f + hashf(static_cast<u32>(c.x * 31.0f + c.z * 17.0f));
        const f32 dx = std::cos(ang) * 0.09f, dz = std::sin(ang) * 0.09f;
        add_box(m, {c.x + dx - 0.025f, 0.0f, c.z + dz - 0.025f},
                {c.x + dx + 0.025f, 0.2f + 0.14f * hashf(i + 9u), c.z + dz + 0.025f},
                g * (0.82f + 0.32f * hashf(i)));
    }
}
// A low leafy bush (a few overlapping green lumps).
void add_leafy_bush(MeshData& m, const Vec3& c, f32 r) {
    const Vec3 g{0.3f, 0.5f, 0.24f};
    add_box(m, {c.x - r, c.y, c.z - r}, {c.x + r, c.y + r * 1.2f, c.z + r}, g);
    add_box(m, {c.x - r * 0.7f, c.y + r * 0.45f, c.z - r * 0.7f}, {c.x + r * 0.7f, c.y + r * 1.5f, c.z + r * 0.7f}, g * 1.1f);
    add_box(m, {c.x - r * 0.5f, c.y + r * 0.3f, c.z - r * 1.1f}, {c.x + r * 0.5f, c.y + r * 1.2f, c.z - r * 0.4f}, g * 0.95f);
}
// A terracotta plant pot with a few foliage blades (red flower optional).
void add_plant_pot(MeshData& m, const Vec3& c, f32 r, bool flower) {
    const Vec3 pot{0.68f, 0.36f, 0.2f}, soil{0.24f, 0.16f, 0.1f}, leaf{0.34f, 0.55f, 0.24f};
    add_box(m, {c.x - r, c.y, c.z - r}, {c.x + r, c.y + r * 1.5f, c.z + r}, pot);
    add_box(m, {c.x - r * 1.1f, c.y + r * 1.45f, c.z - r * 1.1f}, {c.x + r * 1.1f, c.y + r * 1.62f, c.z + r * 1.1f}, pot * 1.06f);
    add_box(m, {c.x - r * 0.8f, c.y + r * 1.5f, c.z - r * 0.8f}, {c.x + r * 0.8f, c.y + r * 1.62f, c.z + r * 0.8f}, soil);
    for (int i = 0; i < 5; ++i) {
        const f32 ang = static_cast<f32>(i) * 1.3f;
        const f32 dx = std::cos(ang) * r * 0.5f, dz = std::sin(ang) * r * 0.5f;
        const f32 top = c.y + r * 1.6f + 0.28f + 0.12f * hashf(i + 3u);
        add_box(m, {c.x + dx - 0.04f, c.y + r * 1.5f, c.z + dz - 0.04f}, {c.x + dx + 0.04f, top, c.z + dz + 0.04f}, leaf);
        if (flower) {
            add_box(m, {c.x + dx - 0.05f, top, c.z + dz - 0.05f}, {c.x + dx + 0.05f, top + 0.08f, c.z + dz + 0.05f},
                    Vec3{0.78f, 0.2f, 0.2f});
        }
    }
}
// A pewter tankard (a small mug with a frothy head + handle), sitting at `c`.
void add_tankard(MeshData& m, const Vec3& c) {
    const Vec3 pewter{0.56f, 0.43f, 0.28f};
    add_box(m, {c.x - 0.05f, c.y, c.z - 0.05f}, {c.x + 0.05f, c.y + 0.12f, c.z + 0.05f}, pewter);
    add_box(m, {c.x - 0.045f, c.y + 0.1f, c.z - 0.045f}, {c.x + 0.045f, c.y + 0.14f, c.z + 0.045f}, Vec3{0.95f, 0.88f, 0.6f});
    add_box(m, {c.x + 0.05f, c.y + 0.03f, c.z - 0.015f}, {c.x + 0.085f, c.y + 0.09f, c.z + 0.015f}, pewter);
}
// A trestle picnic table with two bench seats (axis-aligned, long axis along z), at `c`.
void add_picnic_table(MeshData& m, const Vec3& c) {
    const Vec3 wood{0.5f, 0.36f, 0.2f}, leg = wood * 0.82f;
    const f32 tw = 0.55f, tl = 1.05f, th = 0.62f;
    add_box(m, {c.x - tw, c.y + th, c.z - tl}, {c.x + tw, c.y + th + 0.07f, c.z + tl}, wood); // top
    for (f32 sz : {-tl + 0.16f, tl - 0.16f}) {
        add_box(m, {c.x - tw + 0.04f, c.y, c.z + sz - 0.06f}, {c.x - tw + 0.16f, c.y + th, c.z + sz + 0.06f}, leg);
        add_box(m, {c.x + tw - 0.16f, c.y, c.z + sz - 0.06f}, {c.x + tw - 0.04f, c.y + th, c.z + sz + 0.06f}, leg);
    }
    for (f32 sx : {-1.0f, 1.0f}) {
        add_box(m, {c.x + sx * (tw + 0.16f) - 0.12f, c.y + 0.34f, c.z - tl}, {c.x + sx * (tw + 0.16f) + 0.12f, c.y + 0.4f, c.z + tl}, wood * 0.96f);
        for (f32 sz : {-tl + 0.16f, tl - 0.16f}) {
            add_box(m, {c.x + sx * (tw + 0.16f) - 0.05f, c.y, c.z + sz - 0.04f}, {c.x + sx * (tw + 0.16f) + 0.05f, c.y + 0.34f, c.z + sz + 0.04f}, leg);
        }
    }
}
// ---- Blacksmith workshop props -------------------------------------------------------------
// An anvil on a stump, optionally with a glowing red-hot bar resting on top.
void add_anvil(MeshData& op, MeshData& em, const Vec3& c, bool hot) {
    const Vec3 iron{0.16f, 0.16f, 0.19f}, wood{0.4f, 0.28f, 0.16f};
    add_box(op, {c.x - 0.22f, c.y, c.z - 0.22f}, {c.x + 0.22f, c.y + 0.5f, c.z + 0.22f}, wood * 0.85f); // stump
    add_box(op, {c.x - 0.12f, c.y + 0.5f, c.z - 0.28f}, {c.x + 0.12f, c.y + 0.66f, c.z + 0.28f}, iron); // waist
    add_box(op, {c.x - 0.22f, c.y + 0.66f, c.z - 0.18f}, {c.x + 0.18f, c.y + 0.8f, c.z + 0.18f}, iron * 1.15f); // body
    add_box(op, {c.x + 0.18f, c.y + 0.68f, c.z - 0.08f}, {c.x + 0.44f, c.y + 0.78f, c.z + 0.08f}, iron * 1.1f); // horn
    if (hot) {
        add_box(em, {c.x - 0.08f, c.y + 0.8f, c.z - 0.06f}, {c.x + 0.2f, c.y + 0.86f, c.z + 0.06f}, Vec3{1.7f, 0.7f, 0.2f});
    }
}
// A wooden bucket (or pail) with blue water at the brim.
void add_bucket(MeshData& m, const Vec3& c, f32 r, f32 ht) {
    const Vec3 wood{0.42f, 0.3f, 0.17f};
    add_box(m, {c.x - r, c.y, c.z - r}, {c.x + r, c.y + ht, c.z + r}, wood);
    add_box(m, {c.x - r * 1.06f, c.y + ht * 0.66f, c.z - r * 1.06f}, {c.x + r * 1.06f, c.y + ht * 0.78f, c.z + r * 1.06f}, wood * 0.78f);
    add_box(m, {c.x - r * 0.86f, c.y + ht * 0.86f, c.z - r * 0.86f}, {c.x + r * 0.86f, c.y + ht, c.z + r * 0.86f}, Vec3{0.26f, 0.46f, 0.56f}); // water
}
// A sword standing upright (point up), e.g. leaning against a wall - steel blade, gold guard + pommel.
void add_sword(MeshData& m, const Vec3& c) {
    const Vec3 steel{0.72f, 0.74f, 0.8f}, grip{0.3f, 0.2f, 0.12f}, gold{0.74f, 0.58f, 0.27f};
    add_box(m, {c.x - 0.045f, c.y + 0.32f, c.z - 0.02f}, {c.x + 0.045f, c.y + 1.32f, c.z + 0.02f}, steel); // blade
    add_box(m, {c.x - 0.14f, c.y + 0.28f, c.z - 0.03f}, {c.x + 0.14f, c.y + 0.34f, c.z + 0.03f}, gold);    // crossguard
    add_box(m, {c.x - 0.035f, c.y + 0.13f, c.z - 0.025f}, {c.x + 0.035f, c.y + 0.28f, c.z + 0.025f}, grip); // grip
    add_box(m, {c.x - 0.05f, c.y + 0.09f, c.z - 0.035f}, {c.x + 0.05f, c.y + 0.14f, c.z + 0.035f}, gold);   // pommel
}
// A round/heater shield standing on edge (a slab with a rim + central boss), face colour `face`.
void add_shield(MeshData& m, const Vec3& c, const Vec3& face) {
    const Vec3 rim{0.5f, 0.4f, 0.22f}, boss{0.66f, 0.68f, 0.74f};
    add_box(m, {c.x - 0.3f, c.y + 0.1f, c.z - 0.04f}, {c.x + 0.3f, c.y + 0.78f, c.z}, face);       // face
    add_box(m, {c.x - 0.34f, c.y + 0.22f, c.z - 0.05f}, {c.x + 0.34f, c.y + 0.66f, c.z + 0.01f}, face); // wider middle
    add_box(m, {c.x - 0.32f, c.y + 0.2f, c.z - 0.05f}, {c.x + 0.32f, c.y + 0.26f, c.z + 0.01f}, rim); // rim bands
    add_box(m, {c.x - 0.32f, c.y + 0.62f, c.z - 0.05f}, {c.x + 0.32f, c.y + 0.68f, c.z + 0.01f}, rim);
    add_box(m, {c.x - 0.08f, c.y + 0.4f, c.z - 0.07f}, {c.x + 0.08f, c.y + 0.5f, c.z + 0.01f}, boss); // boss
}
// A sturdy workbench (a thick top on four legs) with a small iron vice on top.
void add_workbench(MeshData& m, const Vec3& c) {
    const Vec3 wood{0.45f, 0.32f, 0.18f};
    add_box(m, {c.x - 0.75f, c.y + 0.7f, c.z - 0.34f}, {c.x + 0.75f, c.y + 0.82f, c.z + 0.34f}, wood); // top
    for (f32 sx : {-0.64f, 0.64f}) {
        for (f32 sz : {-0.26f, 0.26f}) {
            add_box(m, {c.x + sx - 0.05f, c.y, c.z + sz - 0.05f}, {c.x + sx + 0.05f, c.y + 0.7f, c.z + sz + 0.05f}, wood * 0.84f);
        }
    }
    add_box(m, {c.x + 0.52f, c.y + 0.82f, c.z - 0.1f}, {c.x + 0.68f, c.y + 0.98f, c.z + 0.1f}, Vec3{0.2f, 0.2f, 0.22f}); // vice
}
// An A-frame tool rack holding a few hammers / tongs.
void add_tool_rack(MeshData& m, const Vec3& c) {
    const Vec3 wood{0.4f, 0.28f, 0.16f}, iron{0.2f, 0.2f, 0.22f};
    add_box(m, {c.x - 0.5f, c.y, c.z - 0.05f}, {c.x - 0.42f, c.y + 1.05f, c.z + 0.05f}, wood); // post L
    add_box(m, {c.x + 0.42f, c.y, c.z - 0.05f}, {c.x + 0.5f, c.y + 1.05f, c.z + 0.05f}, wood); // post R
    add_box(m, {c.x - 0.5f, c.y + 0.98f, c.z - 0.05f}, {c.x + 0.5f, c.y + 1.06f, c.z + 0.05f}, wood); // top bar
    for (int i = 0; i < 3; ++i) {
        const f32 hx = c.x - 0.3f + static_cast<f32>(i) * 0.3f;
        add_box(m, {hx - 0.022f, c.y + 0.5f, c.z}, {hx + 0.022f, c.y + 0.98f, c.z + 0.04f}, wood); // handle
        add_box(m, {hx - 0.09f, c.y + 0.46f, c.z - 0.01f}, {hx + 0.09f, c.y + 0.58f, c.z + 0.05f}, iron); // hammer head
    }
}
// A small two-wheeled handcart loaded with logs.
void add_handcart(MeshData& m, const Vec3& c) {
    const Vec3 wood{0.46f, 0.31f, 0.16f}, dark{0.3f, 0.2f, 0.11f}, logc{0.52f, 0.37f, 0.21f};
    add_box(m, {c.x - 0.55f, c.y + 0.38f, c.z - 0.62f}, {c.x + 0.55f, c.y + 0.52f, c.z + 0.62f}, wood); // bed
    add_box(m, {c.x - 0.55f, c.y + 0.52f, c.z - 0.62f}, {c.x + 0.55f, c.y + 0.82f, c.z - 0.54f}, wood); // far rail
    add_box(m, {c.x - 0.55f, c.y + 0.52f, c.z + 0.54f}, {c.x + 0.55f, c.y + 0.82f, c.z + 0.62f}, wood); // near rail
    add_box(m, {c.x - 0.62f, c.y + 0.52f, c.z - 0.62f}, {c.x - 0.55f, c.y + 0.8f, c.z + 0.62f}, wood);  // end
    for (f32 sz : {-0.66f, 0.66f}) { // two spoked wheels (octagonal-ish slab)
        add_box(m, {c.x - 0.42f, c.y + 0.02f, c.z + sz - 0.05f}, {c.x - 0.26f, c.y + 0.46f, c.z + sz + 0.05f}, dark);
        add_box(m, {c.x - 0.46f, c.y + 0.14f, c.z + sz - 0.05f}, {c.x - 0.22f, c.y + 0.34f, c.z + sz + 0.05f}, dark);
        add_box(m, {c.x - 0.5f, c.y + 0.2f, c.z + sz - 0.04f}, {c.x - 0.18f, c.y + 0.28f, c.z + sz + 0.04f}, dark);
    }
    for (int i = 0; i < 3; ++i) {
        add_box(m, {c.x - 0.4f + static_cast<f32>(i) * 0.28f, c.y + 0.52f, c.z - 0.55f},
                {c.x - 0.18f + static_cast<f32>(i) * 0.28f, c.y + 0.66f, c.z + 0.55f}, logc * (0.9f + 0.2f * hashf(i + 2u)));
    }
}
// A patch of irregular flagstones laid on the ground around `c` (jittered flat slabs).
void add_flagstones(MeshData& m, const Vec3& c, f32 r) {
    const Vec3 s{0.58f, 0.58f, 0.6f};
    for (int i = 0; i < 9; ++i) {
        const f32 fx = c.x + (hashf(i * 13u + 1u) - 0.5f) * r * 1.8f;
        const f32 fz = c.z + (hashf(i * 13u + 7u) - 0.5f) * r * 1.8f;
        const f32 sz = 0.22f + 0.16f * hashf(i * 13u + 3u);
        add_box(m, {fx - sz, 0.0f, fz - sz}, {fx + sz, 0.05f, fz + sz}, s * (0.86f + 0.24f * hashf(i * 13u + 5u)));
    }
}
// A projecting front-gable porch over the door (lower than the main ridge) - gives the house a
// cross-gable / T-shaped "stance" with the door tucked under it. Adds to `shell` (the fade roof
// shell), with stone footings into `op`.
void add_front_gable(MeshData& shell, MeshData& op, MeshData& em, const Vec3& wall_col,
                     const Vec3& roof, const Vec3& trim, const Vec3& frame, const Vec3& found,
                     const Vec3& glow, f32 w, f32 d, f32 h, bool half_timber, bool thatch, u32 seed) {
    const f32 bw = std::min(0.95f, w * 0.42f); // bay half-width (x)
    const f32 pd = bw + 0.3f;                  // projection half-depth (z) > bw so the ridge runs z
    const f32 ph = std::min(h - 0.1f, 2.55f);
    const f32 xc = -w * 0.4f;                  // offset to the front-left, leaving the door clear
    const f32 zc = d + pd - 0.12f;
    const f32 zfront = zc + pd;
    // a CLOSED projecting bay: front + two side walls (open at the back into the house), so it reads
    // as a cross-gable wing, not a dark porch.
    add_box(shell, {xc - bw, 0.0f, d - 0.1f}, {xc - bw + 0.14f, ph, zfront}, wall_col);  // left side
    add_box(shell, {xc + bw - 0.14f, 0.0f, d - 0.1f}, {xc + bw, ph, zfront}, wall_col);  // right side
    add_box(shell, {xc - bw, 0.0f, zfront - 0.14f}, {xc + bw, ph, zfront}, wall_col);    // front
    add_box(op, {xc - bw - 0.05f, 0.0f, d - 0.08f}, {xc + bw + 0.05f, 0.5f, zfront + 0.05f}, found); // footing
    if (half_timber) {
        timber_frame(shell, true, zfront, 1.0f, xc - bw, xc + bw, 0.0f, ph, 0.45f, frame);
        timber_frame(shell, false, xc - bw, -1.0f, d - 0.05f, zfront - 0.05f, 0.0f, ph, 0.0f, frame);
        timber_frame(shell, false, xc + bw, 1.0f, d - 0.05f, zfront - 0.05f, 0.0f, ph, 0.0f, frame);
    }
    lit_window(op, em, {xc, 1.3f, zfront + 0.06f}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.36f, 0.42f, glow, frame);
    MeshData wr; // build the bay gable centred (ridge along z), then translate to the bay
    gable_roof(wr, bw, pd, ph, pd, 0.42f, thatch, roof, trim, wall_col, seed);
    for (auto& v : wr.vertices) {
        v.position.x += xc;
        v.position.z += zc;
    }
    const u32 base = static_cast<u32>(shell.vertices.size());
    shell.vertices.insert(shell.vertices.end(), wr.vertices.begin(), wr.vertices.end());
    for (u32 i : wr.indices) {
        shell.indices.push_back(base + i);
    }
}
// A lean-to (catslide) shed roof projecting from one side (`side`=+1/-1), lower than the main eave -
// breaks the box with a lower roof section. Adds a low outer wall + a mono-pitch slope.
void add_leanto(MeshData& shell, MeshData& op, const Vec3& wall_col, const Vec3& roof,
                const Vec3& trim, const Vec3& found, f32 w, f32 d, f32 h, f32 side) {
    const f32 lw = 1.35f;           // projection
    const f32 lh = h * 0.55f;       // outer (low) wall height
    const f32 dz = d * 0.78f;       // it runs along most of the side
    const f32 xo = side * (w + lw); // outer wall x
    const f32 xi = side * w;        // inner (against the house) x
    const f32 lo = std::min(xo, xi), hix = std::max(xo, xi);
    add_box(op, {lo - 0.05f, 0.0f, -dz - 0.05f}, {hix + 0.05f, 0.45f, dz + 0.05f}, found);       // footing
    add_box(shell, {std::min(xo, xo - side * 0.14f), 0.0f, -dz}, {std::max(xo, xo - side * 0.14f), lh, dz}, wall_col); // outer wall
    add_box(shell, {lo, 0.0f, -dz - 0.06f}, {hix, lh, -dz}, wall_col);                            // end walls
    add_box(shell, {lo, 0.0f, dz}, {hix, lh, dz + 0.06f}, wall_col);
    // mono-pitch slope from the main eave (at xi, height h-0.1) down to the outer wall (at xo, lh)
    const Vec3 a{xi, h - 0.1f, -dz - 0.12f}, b2{xo, lh + 0.12f, -dz - 0.12f};
    const Vec3 c2{xo, lh + 0.12f, dz + 0.12f}, dd{xi, h - 0.1f, dz + 0.12f};
    add_quad(shell, a, b2, c2, dd, roof * 0.92f);
    add_box(shell, {std::min(xo, xo - side * 0.16f), lh + 0.05f, -dz - 0.14f}, {std::max(xo, xo - side * 0.16f), lh + 0.2f, dz + 0.14f}, trim); // eave fascia
}
} // namespace

// A single fence POST (a stout little pillar with a chamfer cap). Rails connect one post
// to the next as a separate, length-stretched prop (build_fence_rail), so a run of posts
// is joined by rails of whatever length the gap happens to be.
PropDef PropLibrary::build_fence(int variant) {
    PropDef def;
    def.name = "fence_post";
    const Vec3 wood = variant % 2 == 0 ? Vec3{0.42f, 0.30f, 0.18f} : Vec3{0.36f, 0.26f, 0.16f};
    MeshData m;
    add_box(m, {-0.075f, 0.0f, -0.075f}, {0.075f, 1.0f, 0.075f}, wood);        // post
    add_box(m, {-0.095f, 0.9f, -0.095f}, {0.095f, 1.04f, 0.095f}, wood * 0.85f); // cap
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{0.1f, 0.1f};
    c.height = 1.0f;
    def.colliders.push_back(c);
    return def;
}

// A fence RAIL span: two horizontal rails modelled UNIT length along local +X (x in
// -0.5..0.5). The scatter places it at the midpoint between two posts, yawed along the
// road, and stretches it (PropInstance::length) to exactly bridge the gap - so rails vary
// in length and butt onto the posts. The collider stretches with it (CollisionWorld scales
// the local box along +X by the same length).
PropDef PropLibrary::build_fence_rail(int variant) {
    PropDef def;
    def.name = "fence_rail";
    const Vec3 wood = (variant % 2 == 0 ? Vec3{0.42f, 0.30f, 0.18f} : Vec3{0.36f, 0.26f, 0.16f}) * 1.1f;
    MeshData m;
    add_box(m, {-0.5f, 0.34f, -0.028f}, {0.5f, 0.44f, 0.028f}, wood); // lower rail
    add_box(m, {-0.5f, 0.66f, -0.028f}, {0.5f, 0.76f, 0.028f}, wood); // upper rail
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.center = Vec3{0.0f, 0.0f, 0.0f};
    c.half_extents = Vec2{0.5f, 0.08f}; // unit half-length in X; scaled by the gap at scatter
    c.height = 0.85f;
    def.colliders.push_back(c);
    return def;
}

// A path lantern: a post topped with a dark frame around glowing glass, plus a warm
// downward spot light that lights the trail at night.
PropDef PropLibrary::build_lantern_post() {
    PropDef def;
    def.name = "lantern";
    const Vec3 wood{0.30f, 0.22f, 0.14f};
    const Vec3 frame{0.13f, 0.13f, 0.15f};
    const Vec3 glow{1.0f, 0.78f, 0.4f}; // warm amber flame
    MeshData op;
    MeshData em;
    add_box(op, {-0.05f, 0.0f, -0.05f}, {0.05f, 1.7f, 0.05f}, wood);          // post
    add_box(op, {-0.11f, 1.68f, -0.11f}, {0.11f, 1.74f, 0.11f}, frame);        // top cap
    add_box(op, {-0.11f, 1.42f, -0.11f}, {0.11f, 1.46f, 0.11f}, frame);        // bottom cap
    add_box(em, {-0.085f, 1.46f, -0.085f}, {0.085f, 1.68f, 0.085f}, glow);     // glass
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    PropLight l;
    l.offset = Vec3{0.0f, 1.55f, 0.0f};
    l.direction = glm::normalize(Vec3{0.0f, -1.0f, 0.0f});
    l.color = Vec3{1.0f, 0.74f, 0.42f};
    l.range = 14.0f;
    l.intensity = 2.1f; // a warmer, brighter pool on the street (the reference's lit torches)
    l.cone_deg = 135.0f;
    def.lights.push_back(l);
    BoxCollider c;
    c.half_extents = Vec2{0.1f, 0.1f};
    c.height = 1.7f;
    def.colliders.push_back(c);
    return def;
}

namespace {
// A medieval house style: half-extents (w,d), per-storey height, storey count, gable
// rise and a material flavour (0 wattle-and-daub, 1 stone, 2 dark timber). `thatch`
// picks a golden straw roof vs stepped shingle tiles. Varying these gives small/large,
// squat/tall and one/two-storey homes.
struct HouseStyle {
    f32 w, d, story_h;
    int stories;
    f32 roof_rise;
    int material;
    bool thatch;
};
constexpr HouseStyle kHouseStyles[kHouseVariants] = {
    {3.2f, 2.8f, 2.4f, 1, 1.9f, 0, true},  // classic thatched cottage
    {4.7f, 2.6f, 2.3f, 1, 1.4f, 0, false}, // long house (wide, shingled)
    {2.7f, 2.7f, 2.3f, 2, 1.1f, 1, false}, // stone townhouse, two storeys
    {3.0f, 3.0f, 2.4f, 2, 1.6f, 2, false}, // timber two-storey, shingled
    {4.1f, 3.4f, 2.6f, 1, 2.1f, 0, false}, // manor (big, steep tiled roof)
    {2.5f, 2.3f, 2.2f, 1, 1.7f, 1, true},  // small stone hut, thatched
    {3.3f, 2.5f, 2.3f, 2, 1.3f, 0, false}, // tall narrow cottage, shingled
    {3.8f, 2.9f, 2.4f, 1, 1.9f, 2, true},  // timber-framed hall, thatched
};
} // namespace

// The (w,d) footprint half-extents of a house variant, so the village layout can keep
// houses from intersecting walls, the market and each other without building the mesh.
Vec2 PropLibrary::house_half_extents(u32 variant) {
    if (variant == kHouseTownhouse) return Vec2{2.25f, 2.2f};  // jettied top storey extent
    if (variant == kHousePub) return Vec2{3.0f, 2.6f};
    if (variant == kHouseBlacksmith) return Vec2{3.1f, 2.7f};
    const HouseStyle& st = kHouseStyles[variant % kHouseVariants];
    return Vec2{st.w, st.d};
}

// A medieval village house, varied by `variant` (see kHouseStyles): cottages,
// longhouses, two-storey townhouses and manors in daub / stone / timber. Real window
// openings per storey, a furnished ground floor (hearth + fire + a bed where the
// resident sleeps), interior lights and a dollhouse shell that fades when you step in.
// Indices kHouseVariants.. dispatch to the special landmark buildings.
PropDef PropLibrary::build_house(u32 variant) {
    if (variant == kHouseTownhouse) return build_townhouse();
    if (variant == kHousePub) return build_pub();
    if (variant == kHouseBlacksmith) return build_blacksmith();
    const HouseStyle st = kHouseStyles[variant % kHouseVariants];
    PropDef def;
    def.name = "house";

    const f32 w = st.w;          // half width (x)
    const f32 d = st.d;          // half depth (z); +z is the front
    const f32 sh = st.story_h;   // per-storey wall height
    const f32 h = sh * static_cast<f32>(st.stories); // total wall height
    const f32 t = 0.18f;         // wall thickness
    const f32 dw = 0.55f;        // front-door half-width
    const f32 dh = 1.95f;        // door height
    const f32 rr = st.roof_rise * 1.22f; // gable rise (steepened toward the reference look)
    const f32 oh = 0.62f;                // roof overhang (eaves project past the walls)

    auto rnd = [&](u32 s) {
        u32 v = (variant * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    // Bright lime-washed daub infill (cottages) - clean and creamy like the reference - mossy
    // fieldstone, and dark oak.
    static const Vec3 daub_cols[] = {{0.97f, 0.91f, 0.74f}, {0.96f, 0.87f, 0.66f},
                                     {0.95f, 0.83f, 0.60f}};
    const Vec3 stone{0.62f, 0.56f, 0.45f};      // warm lime-mortared fieldstone (walls)
    const Vec3 found_stone{0.47f, 0.51f, 0.58f}; // cooler blue-grey foundation stone (contrasts daub)
    const Vec3 timber{0.24f, 0.16f, 0.10f};
    const Vec3 frame_col{0.20f, 0.13f, 0.08f}; // exposed oak half-timbering
    const Vec3 wall_col = st.material == 1 ? stone
                          : st.material == 2 ? glm::mix(daub_cols[variant % 3], timber, 0.22f)
                                             : daub_cols[variant % 3];
    const Vec3 floor_c{0.32f, 0.25f, 0.17f};
    // Roofs come in two builds: chunky stepped wood/clay shingles in a warm earthy palette
    // (browns, terracotta, weathered wood, muted slate) or a golden straw thatch - matching the
    // Synty-style reference (a town of brown-shingled + thatched cottages, not gaudy slates).
    const bool thatch = st.thatch;
    static const Vec3 shingle_palette[] = {
        {0.58f, 0.31f, 0.16f}, // rich warm brown shingle (reference image 1)
        {0.86f, 0.45f, 0.18f}, // vivid terracotta clay
        {0.72f, 0.25f, 0.17f}, // deep red clay tile
        {0.55f, 0.37f, 0.18f}, // weathered wood shingle
        {0.31f, 0.45f, 0.62f}, // blue slate
        {0.32f, 0.52f, 0.36f}, // mossy green slate
    };
    const Vec3 thatch_col{0.92f, 0.71f, 0.30f}; // golden straw
    const Vec3 roof_base = thatch ? thatch_col : shingle_palette[(variant * 3u + 1u) % 6u];
    const Vec3 roof = glm::mix(roof_base, roof_base * 0.86f, rnd(1));
    const Vec3 trim{0.19f, 0.13f, 0.09f};       // dark timber roof trim (fascia / barge / ridge cap)
    const bool half_timber = st.material != 1;  // daub + timber houses get exposed framing
    const Vec3 wood{0.40f, 0.28f, 0.16f};
    const Vec3 fire{1.0f, 0.55f, 0.15f};
    const Vec3 win_glow{1.0f, 0.82f, 0.42f};

    // shell (walls + roof + floors, fades when you're inside), opaque furniture, emissive.
    MeshData shell, op, em;

    auto wall = [&](const Vec3& lo, const Vec3& hi) {
        add_box(shell, lo, hi, wall_col);
        BoxCollider c;
        c.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        c.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        c.height = h;
        def.colliders.push_back(c);
    };
    // A wall with a window opening cut out (up to 4 sub-boxes) + a warm lit pane with a
    // mullion cross behind it. `along_x` = wall runs along x; `c` is the window centre on
    // that axis, [y0,y1] its height. A full-wall collider still blocks movement.
    const f32 ww = 0.6f;       // window half-width
    const Vec3 mull{0.14f, 0.10f, 0.07f};
    auto win_wall = [&](const Vec3& lo, const Vec3& hi, bool along_x, f32 c, f32 y0, f32 y1) {
        if (along_x) {
            if (c - ww > lo.x) add_box(shell, lo, {c - ww, hi.y, hi.z}, wall_col);
            if (c + ww < hi.x) add_box(shell, {c + ww, lo.y, lo.z}, hi, wall_col);
            add_box(shell, {c - ww, lo.y, lo.z}, {c + ww, y0, hi.z}, wall_col);
            add_box(shell, {c - ww, y1, lo.z}, {c + ww, hi.y, hi.z}, wall_col);
        } else {
            if (c - ww > lo.z) add_box(shell, lo, {hi.x, hi.y, c - ww}, wall_col);
            if (c + ww < hi.z) add_box(shell, {lo.x, lo.y, c + ww}, hi, wall_col);
            add_box(shell, {lo.x, lo.y, c - ww}, {hi.x, y0, c + ww}, wall_col);
            add_box(shell, {lo.x, y1, c - ww}, {hi.x, hi.y, c + ww}, wall_col);
        }
        // Lit pane + mullion at the opening's inner face.
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const f32 whh = (y1 - y0) * 0.5f;
        const f32 my = (y0 + y1) * 0.5f;
        Vec3 ctr, across, outward;
        if (along_x) {
            const bool front = lo.z > 0.0f;
            ctr = Vec3{c, my, front ? hi.z - t : lo.z + t};
            across = Vec3{1.0f, 0.0f, 0.0f};
            outward = Vec3{0.0f, 0.0f, front ? 1.0f : -1.0f};
        } else {
            const bool right = lo.x > 0.0f;
            ctr = Vec3{right ? lo.x + t : hi.x - t, my, c};
            across = Vec3{0.0f, 0.0f, 1.0f};
            outward = Vec3{right ? 1.0f : -1.0f, 0.0f, 0.0f};
        }
        add_quad(em, ctr - across * ww - up * whh, ctr + across * ww - up * whh,
                 ctr + across * ww + up * whh, ctr - across * ww + up * whh, win_glow);
        const Vec3 o = outward * 0.05f;
        add_quad(op, ctr - across * 0.04f - up * whh + o, ctr + across * 0.04f - up * whh + o,
                 ctr + across * 0.04f + up * whh + o, ctr - across * 0.04f + up * whh + o, mull);
        add_quad(op, ctr - across * ww - up * 0.04f + o, ctr + across * ww - up * 0.04f + o,
                 ctr + across * ww + up * 0.04f + o, ctr - across * ww + up * 0.04f + o, mull);
        // A proud timber frame around the opening, so the window reads as framed (reference look).
        const Vec3 of = outward * 0.07f;
        const f32 fb = 0.1f;
        auto fquad = [&](f32 a0, f32 a1, f32 u0, f32 u1) {
            add_quad(op, ctr + across * a0 + up * u0 + of, ctr + across * a1 + up * u0 + of,
                     ctr + across * a1 + up * u1 + of, ctr + across * a0 + up * u1 + of, frame_col);
        };
        fquad(-ww - fb, ww + fb, whh, whh + fb);   // top rail
        fquad(-ww - fb, ww + fb, -whh - fb, -whh);  // bottom rail
        fquad(-ww - fb, -ww, -whh - fb, whh + fb);  // left jamb
        fquad(ww, ww + fb, -whh - fb, whh + fb);    // right jamb
        BoxCollider col;
        col.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        col.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        col.height = h;
        def.colliders.push_back(col);
    };
    auto furn = [&](const Vec3& lo, const Vec3& hi, const Vec3& c) {
        add_box(op, lo, hi, c);
        BoxCollider col;
        col.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        col.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        col.height = hi.y;
        def.colliders.push_back(col);
    };

    add_box(op, {-w, -0.04f, -d}, {w, 0.05f, d}, floor_c); // ground floor

    // Build each storey's four walls. The ground floor's front wall is split around the
    // door; every wall gets a centred window. (For a wider front, two windows.)
    for (int s = 0; s < st.stories; ++s) {
        const f32 y0 = static_cast<f32>(s) * sh;
        const f32 y1 = y0 + sh;
        const f32 wy0 = y0 + 0.85f, wy1 = std::min(y1 - 0.2f, y0 + 2.0f);
        win_wall({-w, y0, -d}, {w, y1, -d + t}, true, 0.0f, wy0, wy1);   // back
        win_wall({-w, y0, -d}, {-w + t, y1, d}, false, 0.0f, wy0, wy1);  // left
        win_wall({w - t, y0, -d}, {w, y1, d}, false, 0.0f, wy0, wy1);    // right
        if (s == 0) {
            win_wall({-w, y0, d - t}, {-dw, y1, d}, true, (-w - dw) * 0.5f, wy0, wy1); // front-L
            wall({dw, y0, d - t}, {w, y1, d});                                          // front-R
            add_box(shell, {-dw, dh, d - t}, {dw, y1, d}, wall_col);                    // door lintel
        } else {
            win_wall({-w, y0, d - t}, {w, y1, d}, true, 0.0f, wy0, wy1); // upper front window
        }
        // A storey-band timber rim.
        add_box(shell, {-w, y1 - 0.1f, -d}, {w, y1, -d + t}, timber);
        add_box(shell, {-w, y1 - 0.1f, d - t}, {w, y1, d}, timber);
    }
    // A floor slab between storeys (part of the fade shell so the interior stays visible).
    if (st.stories == 2) {
        add_box(shell, {-w + t, sh - 0.12f, -d + t}, {w - t, sh, d - t}, timber);
    }

    // ---- Exposed timber framing (half-timbered / Tudor look) on the daub + timber
    // houses: corner posts, sill/head plates, studs and a diagonal brace per panel, all
    // standing slightly proud of the lime-washed infill. Stone houses stay bare masonry.
    if (half_timber) {
        constexpr f32 proud = 0.09f;
        auto post = [&](f32 sx, f32 sz) {
            const f32 x0 = sx > 0.0f ? w - 0.08f : -w - proud;
            const f32 x1 = sx > 0.0f ? w + proud : -w + 0.08f;
            const f32 z0 = sz > 0.0f ? d - 0.08f : -d - proud;
            const f32 z1 = sz > 0.0f ? d + proud : -d + 0.08f;
            add_box(shell, {x0, 0.0f, z0}, {x1, h, z1}, frame_col);
        };
        post(1, 1);
        post(1, -1);
        post(-1, 1);
        post(-1, -1);
        // Frames one wall panel (a storey of one face): bold plates, evenly-spaced studs + a
        // diagonal brace - the prominent exposed timbering of the reference.
        auto panel = [&](bool along_x, f32 face, f32 out, f32 a_lo, f32 a_hi, f32 y0, f32 y1) {
            const f32 p0 = std::min(face, face + out * proud);
            const f32 p1 = std::max(face, face + out * proud);
            auto bar = [&](f32 al, f32 ah, f32 yl, f32 yh) {
                if (along_x) {
                    add_box(shell, {al, yl, p0}, {ah, yh, p1}, frame_col);
                } else {
                    add_box(shell, {p0, yl, al}, {p1, yh, ah}, frame_col);
                }
            };
            bar(a_lo, a_hi, y0, y0 + 0.15f);  // sill plate
            bar(a_lo, a_hi, y1 - 0.15f, y1);  // head plate
            const int studs = std::max(3, static_cast<int>(std::round((a_hi - a_lo) / 0.72f)));
            for (int i = 0; i <= studs; ++i) {
                const f32 a = glm::mix(a_lo, a_hi, static_cast<f32>(i) / static_cast<f32>(studs));
                if (std::abs(a) < 0.82f) {
                    continue; // leave the central window / door opening clear
                }
                bar(a - 0.08f, a + 0.08f, y0, y1);
            }
            // Diagonal corner braces (the classic Tudor cross-timber), one each side of the opening.
            const f32 pf = face + out * proud;
            auto brace = [&](Vec2 q0, Vec2 q1) {
                Vec2 dir = q1 - q0;
                const f32 dl = glm::length(dir);
                if (dl <= 0.4f) return;
                dir /= dl;
                const Vec2 nrm{-dir.y * 0.09f, dir.x * 0.09f};
                auto P = [&](const Vec2& v) {
                    return along_x ? Vec3{v.x, v.y, pf} : Vec3{pf, v.y, v.x};
                };
                add_quad(shell, P(q0 + nrm), P(q1 + nrm), P(q1 - nrm), P(q0 - nrm), frame_col);
            };
            brace({a_lo + 0.18f, y1 - 0.18f}, {a_lo + 1.1f, y0 + 0.18f}); // left brace (down-out)
            brace({a_hi - 0.18f, y1 - 0.18f}, {a_hi - 1.1f, y0 + 0.18f}); // right brace
        };
        for (int s = 0; s < st.stories; ++s) {
            const f32 y0 = static_cast<f32>(s) * sh;
            const f32 y1 = y0 + sh;
            panel(true, d, 1.0f, -w + 0.12f, w - 0.12f, y0, y1);    // front
            panel(true, -d, -1.0f, -w + 0.12f, w - 0.12f, y0, y1);  // back
            panel(false, w, 1.0f, -d + 0.12f, d - 0.12f, y0, y1);   // right
            panel(false, -w, -1.0f, -d + 0.12f, d - 0.12f, y0, y1); // left
        }
    }

    // ---- Stone foundation: mortared fieldstone, proud of the daub/timber above, built in a wider
    // base plinth + a main course (a stepped stone base) with per-segment shade jitter so it reads
    // as stacked stone, not a smooth plinth. The front face is split around the doorway.
    {
        const f32 fy = std::min(1.4f, h * 0.4f); // foundation height (taller on 2-storey homes)
        const f32 fo = 0.16f;                    // clearly proud of the wall face above
        const f32 po = fo + 0.07f, ph = 0.24f;   // plinth (wider, short bottom course)
        auto fc = [&](u32 s) { return found_stone * (0.86f + 0.2f * rnd(s)); };
        add_box(op, {-w - po, 0.0f, -d - po}, {w + po, ph, -d + po}, fc(90)); // base plinth: back
        add_box(op, {-w - po, 0.0f, -d}, {-w + po, ph, d}, fc(91));           //   left
        add_box(op, {w - po, 0.0f, -d}, {w + po, ph, d}, fc(92));             //   right
        add_box(op, {-w - po, 0.0f, d - po}, {w + po, ph, d + po}, fc(93));   //   front (doorstep)
        add_box(op, {-w - fo, ph, -d - fo}, {w + fo, fy, -d + fo}, fc(94));   // main course: back
        add_box(op, {-w - fo, ph, -d}, {-w + fo, fy, d}, fc(95));             //   left
        add_box(op, {w - fo, ph, -d}, {w + fo, fy, d}, fc(96));               //   right
        add_box(op, {-w - fo, ph, d - fo}, {-dw - 0.06f, fy, d + fo}, fc(97)); //  front L of door
        add_box(op, {dw + 0.06f, ph, d - fo}, {w + fo, fy, d + fo}, fc(98));   //  front R of door
    }

    // ---- A planked front door (vertical boards + cross battens + an iron handle) in a proud
    // timber frame, filling the doorway. Opaque so it stays when the roof shell fades.
    {
        const Vec3 doorwood{0.36f, 0.23f, 0.13f};
        const f32 dz0 = d - 0.02f, dz1 = d + 0.07f; // leaf just proud of the front face
        const f32 fz1 = d + 0.12f;
        add_box(op, {-dw - 0.1f, 0.0f, dz0}, {-dw, dh + 0.1f, fz1}, frame_col);     // left jamb
        add_box(op, {dw, 0.0f, dz0}, {dw + 0.1f, dh + 0.1f, fz1}, frame_col);       // right jamb
        add_box(op, {-dw - 0.1f, dh, dz0}, {dw + 0.1f, dh + 0.1f, fz1}, frame_col); // lintel
        const int planks = 4;
        for (int i = 0; i < planks; ++i) {
            const f32 x0 = glm::mix(-dw + 0.04f, dw - 0.04f, static_cast<f32>(i) / planks);
            const f32 x1 =
                glm::mix(-dw + 0.04f, dw - 0.04f, static_cast<f32>(i + 1) / planks) - 0.02f;
            add_box(op, {x0, 0.04f, dz0}, {x1, dh - 0.04f, dz1},
                    doorwood * (0.88f + 0.14f * static_cast<f32>(i % 2)));
        }
        add_box(op, {-dw + 0.02f, 0.34f, dz1 - 0.01f}, {dw - 0.02f, 0.49f, dz1 + 0.04f},
                doorwood * 0.7f); // lower batten
        add_box(op, {-dw + 0.02f, dh - 0.55f, dz1 - 0.01f}, {dw - 0.02f, dh - 0.4f, dz1 + 0.04f},
                doorwood * 0.7f); // upper batten
        add_box(op, {dw - 0.2f, 0.98f, dz1}, {dw - 0.1f, 1.12f, dz1 + 0.07f},
                Vec3{0.12f, 0.12f, 0.13f}); // iron handle
    }

    // ---- Gable roof: the swooping, individually-tiled shingle/thatch roof (shared `gable_roof`
    // helper - ridge sags, eave corners kick up, tiles jittered per course for the hand-built look).
    const bool gable_x = w >= d;
    const f32 zf = d + oh;
    gable_roof(shell, w, d, h, rr, oh, thatch, roof, trim, wall_col, variant * 13u + 5u);

    // ---- A front dormer (a small gabled window poking up out of the roof) on taller, gable-fronted
    // houses - the distinctive reference detail. A daub body sits on the front slope with a lit
    // mullioned window and its own little hip roof.
    if (gable_x && st.stories >= 2) {
        const f32 dz = d * 0.40f;                          // how far forward along the roof
        const f32 dy = h + rr * (1.0f - dz / zf) - 0.12f;  // roof height there (sit slightly into it)
        const f32 dhw = 0.52f, body = 0.95f, depth = 0.55f;
        const f32 fz = dz + depth; // the dormer's front face
        add_box(op, {-dhw, dy, dz}, {dhw, dy + body, fz}, wall_col); // body
        add_box(em, {-dhw + 0.12f, dy + 0.22f, fz}, {dhw - 0.12f, dy + body - 0.1f, fz + 0.05f},
                win_glow); // lit pane
        add_box(op, {-0.04f, dy + 0.22f, fz}, {0.04f, dy + body - 0.1f, fz + 0.06f},
                frame_col); // mullion
        const Vec3 apex{0.0f, dy + body + 0.42f, (dz + fz) * 0.5f}; // little hip roof to an apex
        const Vec3 fl{-dhw - 0.08f, dy + body, fz + 0.08f}, fr{dhw + 0.08f, dy + body, fz + 0.08f};
        const Vec3 bl{-dhw - 0.08f, dy + body, dz - 0.08f}, br{dhw + 0.08f, dy + body, dz - 0.08f};
        add_tri(shell, fl, fr, apex, roof);
        add_tri(shell, fr, br, apex, roof);
        add_tri(shell, br, bl, apex, roof);
        add_tri(shell, bl, fl, apex, roof);
    }

    // ---- Per-variant roof massing so houses aren't plain boxes: some get a projecting front-gable
    // porch (a cross-gable / T stance with the door tucked under it), some a lower lean-to to one
    // side. With the dormer above, the roofline reads varied + characterful.
    const int massing = static_cast<int>(variant % 3u);
    if (massing == 0) {
        add_front_gable(shell, op, em, wall_col, roof, trim, frame_col, found_stone, win_glow, w, d, h,
                        half_timber, thatch, variant * 17u + 3u);
    } else if (massing == 1) {
        add_leanto(shell, op, wall_col, roof, trim, found_stone, w, d, h, (variant & 1u) ? 1.0f : -1.0f);
    }

    // ---- Yard details around the house (the cosy reference touches): stone steps at the door, a
    // barrel + a stacked woodpile by the wall, a short garden fence, plant pots, a bush and grass
    // tufts. Kept close to the walls so they read as the house's own little plot.
    add_stone_steps(op, 0.0f, d + 0.1f, dw + 0.12f);
    add_barrel(op, {-w + 0.45f, 0.0f, d + 0.5f}, 0.3f, 0.78f);
    add_woodpile(op, {w - 0.6f, 0.0f, d + 0.45f}, 0.95f, 3);
    add_fence_run(op, {w + 0.35f, 0.0f, d + 0.15f}, {w + 1.45f, 0.0f, d + 0.15f});
    add_plant_pot(op, {-w - 0.4f, 0.0f, d - 0.45f}, 0.16f, (variant % 2u) == 0u);
    add_leafy_bush(op, {w + 0.6f, 0.0f, -d + 0.7f}, 0.32f);
    add_grass_tuft(op, {w + 0.55f, 0.0f, d - 0.2f});
    add_grass_tuft(op, {-w - 0.5f, 0.0f, d - 0.9f});
    add_grass_tuft(op, {w + 0.95f, 0.0f, d - 0.4f});

    // ---- Ground floor: a stone hearth with fire + chimney (back-left), a table, and
    // the resident's straw bed (back-right, where the villager sleeps). ----
    add_box(op, {-w + t, 0.0f, -d + t}, {-w + t + 1.2f, 1.4f, -d + t + 0.6f}, stone); // hearth
    // A stout stone chimney stack against the back wall, built from a few slightly offset + shaded
    // courses so it reads as stacked masonry, with a corbelled cap and a clay pot (reference look).
    const Vec3 chim_stone{0.48f, 0.49f, 0.52f}; // cool grey fieldstone (matches the foundation)
    const f32 ccx = -w + t + 0.42f, ccz = -d + t + 0.35f; // stack centre (back-left)
    const f32 ctop = h + rr + 0.5f;                       // a touch above the ridge cap
    const int ccourses = 4;
    for (int k = 0; k < ccourses; ++k) {
        const f32 y0 = glm::mix(1.3f, ctop, static_cast<f32>(k) / static_cast<f32>(ccourses));
        const f32 y1 = glm::mix(1.3f, ctop, static_cast<f32>(k + 1) / static_cast<f32>(ccourses));
        const f32 hw = 0.44f, hd = 0.4f;
        const f32 jx = (rnd(70 + static_cast<u32>(k)) - 0.5f) * 0.1f; // per-course facet jitter
        const f32 jz = (rnd(80 + static_cast<u32>(k)) - 0.5f) * 0.1f;
        add_box(op, {ccx - hw + jx, y0, ccz - hd + jz}, {ccx + hw + jx, y1 + 0.02f, ccz + hd + jz},
                chim_stone * (0.9f + 0.16f * static_cast<f32>(k % 2)));
    }
    add_box(op, {ccx - 0.54f, ctop, ccz - 0.5f}, {ccx + 0.54f, ctop + 0.16f, ccz + 0.5f},
            chim_stone * 0.82f); // corbelled cap
    add_box(op, {ccx - 0.2f, ctop + 0.16f, ccz - 0.2f}, {ccx + 0.2f, ctop + 0.44f, ccz + 0.2f},
            Vec3{0.55f, 0.28f, 0.2f}); // clay pot
    add_box(em, {-w + t + 0.15f, 0.08f, -d + t + 0.1f}, {-w + t + 1.0f, 0.5f, -d + t + 0.5f}, fire);
    furn({-0.55f, 0.0f, -0.3f}, {0.55f, 0.74f, 0.6f}, wood); // table

    const f32 bx0 = w - t - 1.45f, bx1 = w - t;
    furn({bx0, 0.0f, -d + t}, {bx1, 0.45f, -d + t + 1.9f}, wood); // bed frame
    add_box(op, {bx0 + 0.02f, 0.45f, -d + t}, {bx1 - 0.02f, 0.6f, -d + t + 1.6f},
            Vec3{0.78f, 0.70f, 0.42f}); // straw mattress
    add_box(op, {bx0, 0.45f, -d + t}, {bx1, 0.72f, -d + t + 0.34f}, Vec3{0.7f, 0.62f, 0.4f}); // pillow

    // ---- Interior lights (hearth fire + a near-ceiling lamp on each storey), plus a
    // candle on the table. With the real openings they spill warm light + shadows out. ----
    add_box(em, {-0.08f, 0.76f, 0.0f}, {0.08f, 0.96f, 0.16f}, win_glow);                  // candle flame
    add_box(op, {-0.06f, 0.74f, 0.02f}, {0.06f, 0.84f, 0.14f}, Vec3{0.9f, 0.86f, 0.7f});  // candle
    PropLight hearth;
    hearth.offset = Vec3{-w + t + 0.7f, 1.0f, -d + t + 0.5f};
    hearth.direction = glm::normalize(Vec3{0.5f, -0.1f, 1.0f});
    hearth.color = Vec3{1.0f, 0.58f, 0.28f};
    hearth.range = 13.0f;
    hearth.intensity = 4.2f;
    hearth.cone_deg = 160.0f;
    def.lights.push_back(hearth);
    PropLight lamp; // lights the top storey (or the rest of a one-storey home)
    lamp.offset = Vec3{0.6f, h - 0.25f, 0.2f};
    lamp.direction = glm::normalize(Vec3{0.0f, -1.0f, 0.1f});
    lamp.color = Vec3{1.0f, 0.82f, 0.55f};
    lamp.range = 11.0f;
    lamp.intensity = 3.6f;
    lamp.cone_deg = 172.0f;
    def.lights.push_back(lamp);
    add_box(em, {0.45f, h - 0.3f, 0.05f}, {0.75f, h - 0.12f, 0.35f}, win_glow); // lamp glow

    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    def.parts.push_back({std::move(shell), PropLayer::Roof});
    def.footprint = Vec2{w, d};
    def.wall_height = h;
    def.bed_spot = Vec3{w - t - 0.7f, 0.0f, -d + t + 0.9f}; // on the bed
    def.door_spot = Vec3{0.0f, 0.0f, d + 0.7f};            // just outside the front door
    return def;
}

namespace {
// Shared medieval palette for the special buildings (townhouse / pub / blacksmith).
const Vec3 kDaub{0.96f, 0.89f, 0.70f};   // warm lime-washed daub infill
const Vec3 kFrame{0.23f, 0.14f, 0.08f};  // dark exposed oak timber
const Vec3 kStone{0.62f, 0.62f, 0.64f};  // light grey fieldstone
const Vec3 kTrim{0.20f, 0.13f, 0.09f};   // dark roof trim
const Vec3 kRoofBrown{0.58f, 0.32f, 0.17f}; // rich warm brown shingle
const Vec3 kGlow{1.0f, 0.82f, 0.42f};    // warm lit window
const Vec3 kWoodDk{0.38f, 0.26f, 0.15f};

// A wall-mounted lantern: a bracket, a glass box (emissive) and a warm spot light. Adds to op + em,
// and pushes a PropLight. `at` is the glass centre; `outward` the wall normal it hangs off.
void add_wall_lantern(MeshData& op, MeshData& em, PropDef& def, const Vec3& at, const Vec3& outward) {
    add_box(op, at - Vec3{0.04f, 0.26f, 0.04f}, at + Vec3{0.04f, 0.3f, 0.04f}, Vec3{0.16f, 0.13f, 0.1f});
    add_box(em, at - Vec3{0.07f, 0.1f, 0.07f}, at + Vec3{0.07f, 0.1f, 0.07f}, Vec3{1.5f, 1.15f, 0.55f});
    PropLight l;
    l.offset = at + outward * 0.1f;
    l.direction = glm::normalize(outward - Vec3{0.0f, 0.3f, 0.0f});
    l.color = Vec3{1.0f, 0.78f, 0.45f};
    l.range = 9.0f;
    l.intensity = 2.4f;
    l.cone_deg = 150.0f;
    def.lights.push_back(l);
}

// A planked door (vertical boards + battens + iron handle) in a proud timber frame, on the front
// face (+z) at z=`fz`, centred on x, half-width `dw`, height `dh`.
void add_plank_door(MeshData& op, f32 fz, f32 dw, f32 dh) {
    add_box(op, {-dw - 0.1f, 0.0f, fz}, {-dw, dh + 0.1f, fz + 0.16f}, kFrame);
    add_box(op, {dw, 0.0f, fz}, {dw + 0.1f, dh + 0.1f, fz + 0.16f}, kFrame);
    add_box(op, {-dw - 0.1f, dh, fz}, {dw + 0.1f, dh + 0.1f, fz + 0.16f}, kFrame);
    for (int i = 0; i < 4; ++i) {
        const f32 x0 = glm::mix(-dw + 0.04f, dw - 0.04f, static_cast<f32>(i) / 4.0f);
        const f32 x1 = glm::mix(-dw + 0.04f, dw - 0.04f, static_cast<f32>(i + 1) / 4.0f) - 0.02f;
        add_box(op, {x0, 0.04f, fz - 0.02f}, {x1, dh - 0.04f, fz + 0.09f},
                kWoodDk * (0.9f + 0.14f * static_cast<f32>(i % 2)));
    }
    add_box(op, {-dw + 0.02f, 0.34f, fz + 0.08f}, {dw - 0.02f, 0.47f, fz + 0.12f}, kWoodDk * 0.7f);
    add_box(op, {dw - 0.2f, 0.96f, fz + 0.09f}, {dw - 0.1f, 1.1f, fz + 0.14f}, Vec3{0.12f, 0.12f, 0.13f});
}
} // namespace

// A tall, narrow three-storey TOWNHOUSE: a stone ground floor, two jettied (overhanging) timber-
// framed upper storeys with lit windows, a steep shingled roof and a stone chimney.
PropDef PropLibrary::build_townhouse() {
    PropDef def;
    def.name = "townhouse";
    MeshData op, em;
    const f32 sh = 2.3f;
    const int stories = 3;
    const f32 w0 = 1.85f, d0 = 1.8f, jut = 0.2f;
    auto half = [&](int s) {
        const f32 j = jut * static_cast<f32>(std::min(s, 2));
        return Vec2{w0 + j, d0 + j};
    };
    const f32 wo = 0.1f; // window proud offset
    const Vec3 UP{0.0f, 1.0f, 0.0f};

    f32 y = 0.0f;
    for (int s = 0; s < stories; ++s) {
        const Vec2 e = half(s);
        const bool ground = (s == 0);
        add_box(op, {-e.x, y, -e.y}, {e.x, y + sh, e.y}, ground ? kStone : kDaub);
        if (s > 0) { // jetty soffit: a dark beam band under the overhang
            add_box(op, {-e.x, y - 0.13f, -e.y}, {e.x, y, e.y}, kFrame * 1.05f);
        }
        if (ground) {
            stone_face(op, true, e.y, 1.0f, -e.x, e.x, y, y + sh, kStone, 11);
            stone_face(op, true, -e.y, -1.0f, -e.x, e.x, y, y + sh, kStone, 12);
            stone_face(op, false, e.x, 1.0f, -e.y, e.y, y, y + sh, kStone, 13);
            stone_face(op, false, -e.x, -1.0f, -e.y, e.y, y, y + sh, kStone, 14);
        } else {
            timber_frame(op, true, e.y, 1.0f, -e.x + 0.04f, e.x - 0.04f, y, y + sh, 0.0f, kFrame);
            timber_frame(op, true, -e.y, -1.0f, -e.x + 0.04f, e.x - 0.04f, y, y + sh, 0.0f, kFrame);
            timber_frame(op, false, e.x, 1.0f, -e.y + 0.04f, e.y - 0.04f, y, y + sh, 0.0f, kFrame);
            timber_frame(op, false, -e.x, -1.0f, -e.y + 0.04f, e.y - 0.04f, y, y + sh, 0.0f, kFrame);
        }
        const f32 wy = y + sh * 0.52f;
        if (ground) {
            lit_window(op, em, {e.x * 0.55f, wy, e.y + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.3f, 0.4f, kGlow, kFrame);
        } else {
            lit_window(op, em, {-e.x * 0.5f, wy, e.y + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.32f, 0.44f, kGlow, kFrame);
            lit_window(op, em, {e.x * 0.5f, wy, e.y + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.32f, 0.44f, kGlow, kFrame);
        }
        lit_window(op, em, {e.x + wo, wy, 0}, {0, 0, 1}, UP, {1, 0, 0}, 0.32f, 0.44f, kGlow, kFrame);
        y += sh;
    }
    const f32 wallTop = static_cast<f32>(stories) * sh;
    const Vec2 et = half(stories - 1);
    add_plank_door(op, d0, 0.42f, 1.9f);
    const f32 apex = gable_roof(op, et.x, et.y, wallTop, et.x, 0.5f, false, kRoofBrown, kTrim, kDaub, 31);
    add_box(op, {et.x - 0.72f, wallTop - 0.5f, -et.y + 0.2f}, {et.x - 0.12f, apex + 0.5f, -et.y + 0.8f}, kStone * 0.9f);
    add_box(op, {et.x - 0.8f, apex + 0.5f, -et.y + 0.12f}, {et.x - 0.04f, apex + 0.66f, -et.y + 0.88f}, kStone * 0.78f);

    PropLight l;
    l.offset = Vec3{0.0f, wallTop * 0.5f, 0.0f};
    l.direction = Vec3{0.0f, -0.2f, 1.0f};
    l.color = Vec3{1.0f, 0.8f, 0.5f};
    l.range = 12.0f;
    l.intensity = 2.6f;
    l.cone_deg = 175.0f;
    def.lights.push_back(l);
    BoxCollider col;
    col.center = Vec3{0.0f, 0.0f, 0.0f};
    col.half_extents = Vec2{et.x, et.y};
    col.height = wallTop;
    def.colliders.push_back(col);
    def.footprint = Vec2{et.x, et.y};
    def.wall_height = wallTop;
    def.bed_spot = Vec3{0.0f, 0.0f, 0.0f};
    def.door_spot = Vec3{0.0f, 0.0f, d0 + 0.8f};
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A two-storey village PUB: a stone ground floor + timber-framed upper, a shingled roof with a
// front dormer, a hanging tavern sign on a bracket, glowing windows, a door lantern and a couple of
// barrels in the beer garden.
PropDef PropLibrary::build_pub() {
    PropDef def;
    def.name = "pub";
    MeshData op, em;
    const f32 w = 3.0f, d = 2.6f, sh = 2.4f;
    const int stories = 2;
    const f32 wo = 0.1f;
    const Vec3 UP{0.0f, 1.0f, 0.0f};
    const f32 wallTop = static_cast<f32>(stories) * sh;

    f32 y = 0.0f;
    for (int s = 0; s < stories; ++s) {
        const bool ground = (s == 0);
        add_box(op, {-w, y, -d}, {w, y + sh, d}, ground ? kStone : kDaub);
        if (ground) {
            stone_face(op, true, d, 1.0f, -w, w, y, y + sh, kStone, 21);
            stone_face(op, true, -d, -1.0f, -w, w, y, y + sh, kStone, 22);
            stone_face(op, false, w, 1.0f, -d, d, y, y + sh, kStone, 23);
            stone_face(op, false, -w, -1.0f, -d, d, y, y + sh, kStone, 24);
        } else {
            add_box(op, {-w, y - 0.1f, -d}, {w, y, d}, kFrame * 1.05f); // mid floor band
            timber_frame(op, true, d, 1.0f, -w + 0.04f, w - 0.04f, y, y + sh, 0.55f, kFrame);
            timber_frame(op, true, -d, -1.0f, -w + 0.04f, w - 0.04f, y, y + sh, 0.0f, kFrame);
            timber_frame(op, false, w, 1.0f, -d + 0.04f, d - 0.04f, y, y + sh, 0.0f, kFrame);
            timber_frame(op, false, -w, -1.0f, -d + 0.04f, d - 0.04f, y, y + sh, 0.0f, kFrame);
        }
        const f32 wy = y + sh * 0.5f;
        if (ground) {
            lit_window(op, em, {-w * 0.55f, wy, d + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.4f, kGlow, kFrame);
            lit_window(op, em, {w * 0.55f, wy, d + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.4f, kGlow, kFrame);
        } else {
            lit_window(op, em, {-w * 0.5f, wy, d + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.46f, kGlow, kFrame);
            lit_window(op, em, {w * 0.5f, wy, d + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.46f, kGlow, kFrame);
        }
        lit_window(op, em, {w + wo, wy, 0}, {0, 0, 1}, UP, {1, 0, 0}, 0.34f, 0.44f, kGlow, kFrame);
        lit_window(op, em, {-w - wo, wy, 0.0f}, {0, 0, 1}, UP, {-1, 0, 0}, 0.34f, 0.44f, kGlow, kFrame);
        y += sh;
    }
    add_plank_door(op, d, 0.5f, 2.0f);
    add_box(em, {-0.46f, 0.1f, d - 0.05f}, {0.46f, 1.9f, d + 0.04f}, Vec3{1.2f, 0.85f, 0.45f}); // doorway glow

    const f32 apex = gable_roof(op, w, d, wallTop, w * 0.78f, 0.6f, false, kRoofBrown, kTrim, kDaub, 41);
    // front dormer (a little gabled lit window poking from the front roof slope)
    {
        const f32 zf = d + 0.6f, rr = w * 0.78f;
        const f32 dz = d * 0.45f;
        const f32 dy = wallTop + rr * (1.0f - dz / zf) - 0.12f;
        const f32 dhw = 0.5f, body = 0.9f, depth = 0.5f, fz = dz + depth;
        add_box(op, {-dhw, dy, dz}, {dhw, dy + body, fz}, kDaub);
        add_box(em, {-dhw + 0.1f, dy + 0.2f, fz}, {dhw - 0.1f, dy + body - 0.1f, fz + 0.05f}, kGlow);
        timber_frame(op, true, fz, 1.0f, -dhw, dhw, dy, dy + body, 0.3f, kFrame);
        const Vec3 ap{0.0f, dy + body + 0.4f, (dz + fz) * 0.5f};
        const Vec3 fl{-dhw - 0.08f, dy + body, fz + 0.08f}, fr{dhw + 0.08f, dy + body, fz + 0.08f};
        const Vec3 bl{-dhw - 0.08f, dy + body, dz - 0.08f}, br{dhw + 0.08f, dy + body, dz - 0.08f};
        add_tri(op, fl, fr, ap, kRoofBrown);
        add_tri(op, fr, br, ap, kRoofBrown);
        add_tri(op, br, bl, ap, kRoofBrown);
        add_tri(op, bl, fl, ap, kRoofBrown);
    }
    // chimney
    add_box(op, {-w + 0.3f, wallTop - 0.4f, -d + 0.25f}, {-w + 0.95f, apex + 0.55f, -d + 0.85f}, kStone * 0.88f);
    add_box(op, {-w + 0.22f, apex + 0.55f, -d + 0.17f}, {-w + 1.03f, apex + 0.72f, -d + 0.93f}, kStone * 0.76f);

    // hanging tavern sign: a bracket arm off the upper front-left, two chains + a board
    {
        const f32 ax = -w + 0.3f, ay = wallTop + 0.5f, az = d;
        add_box(op, {ax - 0.08f, ay - 0.08f, az}, {ax + 0.08f, ay + 0.08f, az + 1.1f}, kWoodDk); // arm
        add_box(op, {ax - 0.02f, ay - 0.5f, az + 0.78f}, {ax + 0.02f, ay, az + 0.82f}, Vec3{0.2f, 0.2f, 0.22f}); // chain L
        add_box(op, {ax - 0.02f, ay - 0.5f, az + 1.0f}, {ax + 0.02f, ay, az + 1.04f}, Vec3{0.2f, 0.2f, 0.22f}); // chain R
        add_box(op, {ax - 0.32f, ay - 1.0f, az + 0.74f}, {ax + 0.32f, ay - 0.5f, az + 1.08f}, kWoodDk * 0.9f); // board frame
        add_box(em, {ax - 0.26f, ay - 0.94f, az + 1.08f}, {ax + 0.26f, ay - 0.56f, az + 1.12f}, Vec3{0.5f, 0.6f, 0.72f}); // emblem panel
    }
    // ---- The beer garden, off to the front-right (+x): trestle picnic tables with tankards on
    // top, barrels, terracotta flower pots, leafy bushes, tufts of grass and a low boundary fence.
    add_wall_lantern(op, em, def, {0.85f, 1.7f, d + 0.12f}, {0.0f, 0.0f, 1.0f});
    for (int ti = 0; ti < 2; ++ti) {
        const Vec3 tc{w + 1.5f, 0.0f, d + 0.6f + static_cast<f32>(ti) * 2.5f};
        add_picnic_table(op, tc);
        add_tankard(op, {tc.x - 0.24f, 0.69f, tc.z - 0.4f});
        add_tankard(op, {tc.x + 0.22f, 0.69f, tc.z + 0.3f});
        add_tankard(op, {tc.x + 0.08f, 0.69f, tc.z - 0.1f});
    }
    add_barrel(op, {-w - 0.55f, 0.0f, d + 0.4f}, 0.32f, 0.82f);
    add_barrel(op, {-w - 0.5f, 0.0f, d + 1.1f}, 0.3f, 0.74f);
    add_barrel(op, {w + 0.5f, 0.0f, d + 4.0f}, 0.32f, 0.82f);
    add_plant_pot(op, {w + 0.42f, 0.0f, d + 0.4f}, 0.18f, true);
    add_plant_pot(op, {w + 2.75f, 0.0f, d + 1.6f}, 0.16f, true);
    add_leafy_bush(op, {w + 0.62f, 0.0f, -d + 0.9f}, 0.34f);
    add_leafy_bush(op, {-w - 0.72f, 0.0f, -d + 1.1f}, 0.3f);
    add_grass_tuft(op, {w + 1.0f, 0.0f, d - 0.3f});
    add_grass_tuft(op, {-w - 0.62f, 0.0f, d - 0.6f});
    add_grass_tuft(op, {w + 2.3f, 0.0f, d + 3.4f});
    add_grass_tuft(op, {w + 0.9f, 0.0f, d + 4.3f});
    add_fence_run(op, {w + 0.3f, 0.0f, d + 4.6f}, {w + 2.9f, 0.0f, d + 4.6f});

    PropLight interior;
    interior.offset = Vec3{0.0f, wallTop * 0.4f, 0.0f};
    interior.direction = Vec3{0.0f, -0.2f, 1.0f};
    interior.color = Vec3{1.0f, 0.78f, 0.45f};
    interior.range = 13.0f;
    interior.intensity = 3.2f;
    interior.cone_deg = 175.0f;
    def.lights.push_back(interior);
    BoxCollider col;
    col.center = Vec3{0.0f, 0.0f, 0.0f};
    col.half_extents = Vec2{w, d};
    col.height = wallTop;
    def.colliders.push_back(col);
    def.footprint = Vec2{w, d};
    def.wall_height = wallTop;
    def.bed_spot = Vec3{0.0f, 0.0f, 0.0f};
    def.door_spot = Vec3{0.0f, 0.0f, d + 0.9f};
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A village BLACKSMITH: a two-storey stone + jettied-timber workshop with a WIDE OPEN front bay you
// can walk into, a big RED-BRICK forge chimney (crenellated like a tower) with a stone hearth +
// roaring fire at its base, a grey slate roof, and a workshop full of props - an anvil with hot
// metal, a workbench, a tool rack, a water bucket, swords + shields, a handcart, barrels + firewood.
PropDef PropLibrary::build_blacksmith() {
    PropDef def;
    def.name = "blacksmith";
    MeshData op, em, glow;
    const f32 w = 3.2f, d = 2.7f;
    const f32 sg = 2.4f, su = 1.8f; // ground + upper storey heights
    const f32 t = 0.2f, jut = 0.22f;
    const f32 wo = 0.1f;
    const Vec3 UP{0.0f, 1.0f, 0.0f};
    const Vec3 fire{1.0f, 0.55f, 0.16f};
    const Vec3 slate{0.42f, 0.45f, 0.52f}; // grey slate roof
    const Vec3 brick{0.66f, 0.31f, 0.22f}; // red brick
    const Vec3 dirt{0.34f, 0.26f, 0.18f};

    auto wall = [&](const Vec3& lo, const Vec3& hi) { // a stone wall that also collides
        add_box(op, lo, hi, kStone);
        BoxCollider c;
        c.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        c.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        c.height = hi.y;
        def.colliders.push_back(c);
    };
    auto solid = [&](const Vec3& lo, const Vec3& hi) { // a collider without geometry (for props)
        BoxCollider c;
        c.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        c.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        c.height = hi.y;
        def.colliders.push_back(c);
    };

    add_box(op, {-w, -0.05f, -d}, {w, 0.05f, d}, dirt); // workshop floor

    // ---- Ground floor: stone walls, OPEN across the front-left (the walk-in workshop bay). The
    // enclosed room is on the right; the forge tower is on the left, so the open bay + forge read
    // from a front view.
    const f32 bayR = w - 1.7f; // the front wall starts here; everything left of it is open
    wall({-w, 0.0f, -d}, {-w + t, sg, d});      // left
    wall({-w, 0.0f, -d}, {w, sg, -d + t});      // back
    wall({w - t, 0.0f, -d}, {w, sg, d});        // right
    wall({bayR, 0.0f, d - t}, {w, sg, d});      // front (right enclosed room only)
    stone_face(op, false, -w, -1.0f, -d, d, 0.0f, sg, kStone, 31);
    stone_face(op, true, -d, -1.0f, -w, w, 0.0f, sg, kStone, 32);
    stone_face(op, false, w, 1.0f, -d, d, 0.0f, sg, kStone, 33);
    stone_face(op, true, d, 1.0f, bayR, w, 0.0f, sg, kStone, 34);
    lit_window(op, em, {(bayR + w) * 0.5f, 1.4f, d + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.4f, kGlow, kFrame);

    // ---- The open bay: timber posts + a lintel beam carrying the jettied upper storey ----
    add_box(op, {-w + t - 0.04f, 0.0f, d - 0.14f}, {-w + t + 0.13f, sg, d + 0.04f}, kWoodDk);  // left post
    add_box(op, {bayR - 0.13f, 0.0f, d - 0.14f}, {bayR + 0.04f, sg, d + 0.04f}, kWoodDk);      // right post
    add_box(op, {-w + t, sg - 0.3f, d - 0.14f}, {bayR + 0.04f, sg, d + 0.04f}, kWoodDk);       // lintel beam
    add_box(op, {bayR - 0.55f, sg - 0.32f, d - 0.16f}, {bayR, sg, d - 0.02f}, kWoodDk * 0.9f); // corner brace

    // ---- Upper storey: jettied (overhanging) timber-framed daub over the whole footprint ----
    const f32 uw = w + jut, ud = d + jut, ut = sg + su;
    add_box(op, {-uw, sg, -ud}, {uw, ut, ud}, kDaub);
    add_box(op, {-uw, sg - 0.16f, -ud}, {uw, sg, ud}, kFrame); // jetty soffit band
    timber_frame(op, true, ud, 1.0f, -uw + 0.04f, uw - 0.04f, sg, ut, 0.55f, kFrame);
    timber_frame(op, true, -ud, -1.0f, -uw + 0.04f, uw - 0.04f, sg, ut, 0.0f, kFrame);
    timber_frame(op, false, uw, 1.0f, -ud + 0.04f, ud - 0.04f, sg, ut, 0.0f, kFrame);
    timber_frame(op, false, -uw, -1.0f, -ud + 0.04f, ud - 0.04f, sg, ut, 0.0f, kFrame);
    lit_window(op, em, {uw * 0.45f, sg + su * 0.5f, ud + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.42f, kGlow, kFrame);

    // ---- Grey slate roof ----
    const f32 apex = gable_roof(op, uw, ud, ut, uw * 0.64f, 0.55f, false, slate, kTrim, kDaub, 51);

    // ---- The big RED-BRICK forge chimney on the left, set toward the FRONT so the hearth + fire
    // sit right at the bay opening (visible from outside) - a tower with a crenellated top.
    const f32 cx = -w + 0.62f, cz = 1.15f, chw = 0.82f, chd = 0.68f;
    const f32 cTop = apex + 1.7f;
    add_box(op, {cx - chw, 0.0f, cz - chd}, {cx + chw, cTop, cz + chd}, brick);
    stone_face(op, true, cz + chd, 1.0f, cx - chw, cx + chw, 0.9f, cTop, brick, 60);  // brick texture, front
    stone_face(op, false, cx + chw, 1.0f, cz - chd, cz + chd, 0.9f, cTop, brick, 61); // right
    stone_face(op, false, cx - chw, -1.0f, cz - chd, cz + chd, 0.9f, cTop, brick, 62); // left
    for (int i = 0; i < 4; ++i) { // merlons (front + back rows)
        const f32 sgw = (2.0f * chw - 0.12f) / 4.0f;
        const f32 bx = cx - chw + 0.06f + static_cast<f32>(i) * sgw;
        add_box(op, {bx, cTop, cz - chd}, {bx + sgw - 0.1f, cTop + 0.4f, cz - chd + 0.22f}, brick * 0.96f);
        add_box(op, {bx, cTop, cz + chd - 0.22f}, {bx + sgw - 0.1f, cTop + 0.4f, cz + chd}, brick * 0.96f);
    }
    for (int i = 0; i < 3; ++i) { // merlons (side rows)
        const f32 sgd = (2.0f * chd - 0.12f) / 3.0f;
        const f32 bz = cz - chd + 0.06f + static_cast<f32>(i) * sgd;
        add_box(op, {cx - chw, cTop, bz}, {cx - chw + 0.22f, cTop + 0.4f, bz + sgd - 0.1f}, brick * 0.92f);
        add_box(op, {cx + chw - 0.22f, cTop, bz}, {cx + chw, cTop + 0.4f, bz + sgd - 0.1f}, brick * 0.92f);
    }

    // ---- Stone forge hearth at the tower base, opening toward the bay (+z), with a roaring fire.
    const f32 hfz = cz + chd + 0.5f; // hearth front face (projects into the bay)
    add_box(op, {cx - 0.9f, 0.0f, cz + chd - 0.1f}, {cx + 0.9f, 1.6f, hfz}, kStone * 1.05f); // hearth mass
    stone_face(op, true, hfz, 1.0f, cx - 0.9f, cx + 0.9f, 0.0f, 1.6f, kStone, 63);
    add_box(op, {cx - 0.5f, 0.2f, hfz - 0.02f}, {cx + 0.5f, 1.05f, hfz + 0.06f}, Vec3{0.08f, 0.06f, 0.05f}); // dark mouth
    add_box(em, {cx - 0.42f, 0.25f, hfz - 0.06f}, {cx + 0.42f, 0.92f, hfz}, Vec3{0.9f, 0.5f, 0.2f});         // inner glow
    glow.vertices.clear();
    glow.indices.clear();
    add_box(glow, {cx - 0.4f, 0.28f, hfz - 0.14f}, {cx + 0.4f, 1.35f, hfz + 0.12f}, fire); // fire bloom
    for (int i = 0; i < 4; ++i) {
        const f32 tx = cx - 0.28f + static_cast<f32>(i) * 0.19f;
        const f32 fh = 1.0f + 0.5f * hashf(700u + static_cast<u32>(i));
        add_box(em, {tx - 0.09f, 0.3f, hfz - 0.1f}, {tx + 0.09f, fh, hfz + 0.08f}, Vec3{1.7f, 1.0f, 0.4f});
    }
    add_box(op, {cx - 0.95f, 1.6f, cz + chd - 0.1f}, {cx + 0.95f, 2.1f, hfz}, kStone * 0.95f);      // mantel
    add_box(op, {cx - 0.72f, 2.1f, cz + chd - 0.1f}, {cx + 0.72f, 2.75f, hfz - 0.32f}, kStone * 0.9f); // tapered hood
    solid({cx - 0.9f, 0.0f, cz + chd - 0.1f}, {cx + 0.9f, 1.6f, hfz}); // forge collider (don't walk through fire)
    PropLight forge;
    forge.offset = Vec3{cx, 1.0f, hfz};
    forge.direction = Vec3{0.0f, -0.1f, 1.0f};
    forge.color = Vec3{1.0f, 0.5f, 0.2f};
    forge.range = 16.0f;
    forge.intensity = 6.0f;
    forge.cone_deg = 178.0f;
    def.lights.push_back(forge);

    // ---- Workshop props inside the open bay (anvil near the opening, forge gear by the hearth) ----
    add_anvil(op, em, {0.35f, 0.0f, d - 0.85f}, true);
    solid({0.13f, 0.0f, d - 1.15f}, {0.79f, 0.85f, d - 0.55f}); // anvil collider
    add_workbench(op, {1.15f, 0.0f, -d + 0.7f});
    add_tool_rack(op, {bayR - 0.3f, 0.0f, d - 1.1f});
    add_bucket(op, {-0.4f, 0.0f, d - 1.0f}, 0.2f, 0.34f);
    add_sword(op, {cx + 1.05f, 0.0f, hfz + 0.12f});
    add_sword(op, {cx + 1.2f, 0.0f, hfz + 0.26f});
    add_shield(op, {cx + 1.42f, 0.0f, hfz + 0.3f}, Vec3{0.35f, 0.5f, 0.66f});
    for (int i = 0; i < 4; ++i) { // firewood stacked beside the hearth
        add_box(op, {cx + 0.6f + static_cast<f32>(i) * 0.12f, 0.05f, cz + chd + 0.1f},
                {cx + 0.7f + static_cast<f32>(i) * 0.12f, 0.5f, cz + chd + 0.55f}, kWoodDk * 1.1f);
    }

    // ---- Yard props out front / to the sides ----
    add_handcart(op, {w + 0.7f, 0.0f, d - 0.4f});
    add_barrel(op, {1.5f, 0.0f, d + 0.6f}, 0.3f, 0.82f);
    add_box(op, {1.23f, 0.7f, d + 0.32f}, {1.77f, 0.78f, d + 0.88f}, Vec3{0.26f, 0.46f, 0.56f}); // barrel water
    add_woodpile(op, {-1.4f, 0.0f, d + 1.0f}, 0.95f, 2);
    add_flagstones(op, {-0.2f, 0.0f, d + 0.7f}, 1.1f);
    add_grass_tuft(op, {-w - 0.5f, 0.0f, -d + 0.7f});
    add_grass_tuft(op, {w + 0.5f, 0.0f, d - 0.5f});
    add_leafy_bush(op, {-w - 0.55f, 0.0f, -d + 0.8f}, 0.3f);
    add_wall_lantern(op, em, def, {w + 0.02f, 1.9f, d - 0.6f}, {1.0f, 0.0f, 0.0f});

    def.footprint = Vec2{w, d};
    def.wall_height = ut;
    def.bed_spot = Vec3{w - 0.85f, 0.0f, -d + 0.6f}; // inside the enclosed corner (right)
    def.door_spot = Vec3{0.0f, 0.0f, d + 1.2f};       // walk in through the open bay
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    def.parts.push_back({std::move(glow), PropLayer::Glow});
    return def;
}

// A stone perimeter wall segment, running ALONG local +X so the village ring lays
// segments end to end (yawed to each side of the palisade).
PropDef PropLibrary::build_wall(int variant) {
    PropDef def;
    def.name = "wall";
    // Cool blue-grey ashlar (the reference's fortified town wall).
    const Vec3 stone = variant % 2 == 0 ? Vec3{0.55f, 0.57f, 0.61f} : Vec3{0.5f, 0.52f, 0.57f};
    constexpr f32 half_len = 1.6f;
    constexpr f32 thick = 0.62f; // a wide, chunky rampart (was 0.4)
    constexpr f32 wh = 2.85f;    // and taller (was 2.3)
    MeshData m;
    add_box(m, {-half_len, 0.0f, -thick}, {half_len, wh, thick}, stone);
    // Textured ashlar blocks on both faces, and a slightly battered (wider) base course.
    add_box(m, {-half_len, 0.0f, -thick - 0.08f}, {half_len, 0.55f, thick + 0.08f}, stone * 0.95f);
    stone_face(m, true, thick, 1.0f, -half_len, half_len, 0.5f, wh, stone, static_cast<u32>(variant) * 7u + 11u);
    stone_face(m, true, -thick, -1.0f, -half_len, half_len, 0.5f, wh, stone, static_cast<u32>(variant) * 7u + 23u);
    // A rampart walkway with crenellated merlons on BOTH edges (a real wall-walk between).
    add_box(m, {-half_len, wh, -thick - 0.1f}, {half_len, wh + 0.14f, thick + 0.1f}, stone * 1.05f); // parapet lip
    for (int i = -2; i <= 2; ++i) {
        const f32 cx = static_cast<f32>(i) * 0.62f;
        add_box(m, {cx - 0.22f, wh + 0.14f, thick - 0.16f}, {cx + 0.22f, wh + 0.52f, thick + 0.1f}, stone * 1.07f);
        add_box(m, {cx - 0.22f, wh + 0.14f, -thick - 0.1f}, {cx + 0.22f, wh + 0.52f, -thick + 0.16f}, stone * 1.07f);
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{half_len, thick};
    c.height = wh;
    def.colliders.push_back(c);
    return def;
}

// A stone gate tower (placed in pairs flanking each gate gap), with a fire brazier
// on top that lights the gate at night.
// A flanking gatehouse: a stout stone tower (taller than the wall) that stands either side of
// a gate. A battered base, a doorway + a lit window on the town-facing front (+z), arrow slits
// down the sides, a crenellated battlement, and a brazier on top - the lit gate marker for the
// night. Detail recesses are boxes buried in the wall with only a proud face showing, so no
// coplanar z-fighting. Placed in pairs by village_props, facing the town centre.
PropDef PropLibrary::build_gate() {
    PropDef def;
    def.name = "gatehouse";
    const Vec3 stone{0.55f, 0.57f, 0.61f}; // cool blue-grey ashlar (matches the wall)
    const Vec3 dark{0.2f, 0.16f, 0.13f};   // doorway / arrow-slit recesses
    const Vec3 wood{0.34f, 0.23f, 0.13f};
    const Vec3 fire{1.0f, 0.6f, 0.2f};
    const Vec3 lit{1.0f, 0.82f, 0.45f}; // warm lit window
    MeshData op;
    MeshData em;
    constexpr f32 r = 1.05f; // half-width footprint
    constexpr f32 gh = 4.9f; // tower height (the wall is 2.85)

    add_box(op, {-r - 0.18f, 0.0f, -r - 0.18f}, {r + 0.18f, 0.6f, r + 0.18f}, stone * 0.94f); // battered base
    add_box(op, {-r, 0.6f, -r}, {r, gh, r}, stone);                                           // body
    stone_face(op, true, r, 1.0f, -r, r, 0.6f, gh, stone, 41u);                               // textured faces
    stone_face(op, false, r, 1.0f, -r, r, 0.6f, gh, stone, 42u);
    stone_face(op, false, -r, -1.0f, -r, r, 0.6f, gh, stone, 43u);
    add_box(op, {-r - 0.07f, gh - 1.0f, -r - 0.07f}, {r + 0.07f, gh - 0.86f, r + 0.07f},
            stone * 1.04f); // string-course band

    // A tall ARCHED doorway on the front (+z): a dark recess with stepped corbels forming the arch,
    // a timber lintel and studded planks, plus a lit upper window. Buried so only the face shows.
    add_box(op, {-0.42f, 0.0f, r - 0.32f}, {0.42f, 2.0f, r + 0.03f}, dark);   // doorway recess
    add_box(op, {-0.34f, 2.0f, r - 0.3f}, {0.34f, 2.2f, r + 0.03f}, dark);    // arch step 1
    add_box(op, {-0.24f, 2.2f, r - 0.3f}, {0.24f, 2.36f, r + 0.03f}, dark);   // arch step 2
    add_box(op, {-0.46f, 1.9f, r - 0.14f}, {0.46f, 2.08f, r + 0.06f}, wood);  // timber lintel
    for (int i = 0; i < 4; ++i) {                                            // plank door leaves
        const f32 dx0 = glm::mix(-0.38f, 0.38f, static_cast<f32>(i) / 4.0f);
        const f32 dx1 = glm::mix(-0.38f, 0.38f, static_cast<f32>(i + 1) / 4.0f) - 0.02f;
        add_box(op, {dx0, 0.04f, r - 0.16f}, {dx1, 1.9f, r - 0.1f}, wood * (0.88f + 0.16f * static_cast<f32>(i % 2)));
    }
    add_box(em, {-0.24f, 3.0f, r - 0.25f}, {0.24f, 3.5f, r + 0.03f}, lit);    // lit window glow
    add_box(op, {-0.03f, 3.0f, r - 0.05f}, {0.03f, 3.5f, r + 0.05f}, wood);   // window mullion

    // Arrow slits down the two side faces (buried, proud face only).
    for (f32 sx : {-1.0f, 1.0f}) {
        for (f32 sy : {1.5f, 2.7f}) {
            add_box(op, {sx * r - 0.22f, sy, -0.08f}, {sx * r + 0.03f, sy + 0.7f, 0.08f}, dark);
        }
    }

    // Crenellated battlement: an overhanging parapet lip + merlons all round.
    add_box(op, {-r - 0.14f, gh, -r - 0.14f}, {r + 0.14f, gh + 0.12f, r + 0.14f}, stone * 1.05f);
    const f32 mt = gh + 0.12f;   // merlon base height
    const f32 mr = r + 0.14f;    // merlon ring radius
    for (int i = -1; i <= 1; ++i) {
        const f32 o = static_cast<f32>(i) * (mr * 0.62f);
        add_box(op, {o - 0.2f, mt, mr - 0.16f}, {o + 0.2f, mt + 0.36f, mr}, stone);   // +z edge
        add_box(op, {o - 0.2f, mt, -mr}, {o + 0.2f, mt + 0.36f, -mr + 0.16f}, stone); // -z edge
        add_box(op, {mr - 0.16f, mt, o - 0.2f}, {mr, mt + 0.36f, o + 0.2f}, stone);   // +x edge
        add_box(op, {-mr, mt, o - 0.2f}, {-mr + 0.16f, mt + 0.36f, o + 0.2f}, stone); // -x edge
    }

    // A brazier burning on the battlement.
    add_box(op, {-0.16f, mt, -0.16f}, {0.16f, mt + 0.28f, 0.16f}, wood);          // basket
    add_box(em, {-0.24f, mt + 0.28f, -0.24f}, {0.24f, mt + 0.64f, 0.24f}, fire);  // flame

    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    PropLight l;
    l.offset = Vec3{0.0f, mt + 0.5f, 0.0f};
    l.direction = glm::normalize(Vec3{0.0f, -1.0f, 0.0f});
    l.color = Vec3{1.0f, 0.7f, 0.35f};
    l.range = 15.0f;
    l.intensity = 1.5f; // match the street lanterns so lit gates aren't over-bright
    l.cone_deg = 130.0f;
    def.lights.push_back(l);
    BoxCollider c;
    c.half_extents = Vec2{r, r};
    c.height = gh;
    def.colliders.push_back(c);
    return def;
}

// A plain unlit stone tower (no brazier / light) - used for the periodic towers around the
// wall, so only the actual road gates have lit braziers (keeps the night lighting even).
PropDef PropLibrary::build_tower() {
    PropDef def;
    def.name = "tower";
    const Vec3 stone{0.53f, 0.55f, 0.59f}; // cool blue-grey (matches the wall)
    MeshData op;
    constexpr f32 r = 0.55f;
    constexpr f32 gh = 3.6f;
    add_box(op, {-r - 0.08f, 0.0f, -r - 0.08f}, {r + 0.08f, 0.5f, r + 0.08f}, stone * 0.94f); // battered base
    add_box(op, {-r, 0.0f, -r}, {r, gh, r}, stone);
    stone_face(op, true, r, 1.0f, -r, r, 0.5f, gh, stone, 71u);
    add_box(op, {-r - 0.1f, gh, -r - 0.1f}, {r + 0.1f, gh + 0.14f, r + 0.1f}, stone * 1.05f); // parapet lip
    for (int i = -1; i <= 1; ++i) {
        const f32 o = static_cast<f32>(i) * (r * 0.9f);
        add_box(op, {o - 0.14f, gh + 0.14f, r - 0.1f}, {o + 0.14f, gh + 0.46f, r + 0.1f}, stone * 1.07f); // +z merlons
        add_box(op, {o - 0.14f, gh + 0.14f, -r - 0.1f}, {o + 0.14f, gh + 0.46f, -r + 0.1f}, stone * 1.07f);
    }
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{r, r};
    c.height = gh;
    def.colliders.push_back(c);
    return def;
}

// A village well at the plaza centre: a square stone rim around dark water, two
// timber posts carrying a crossbeam, a hanging bucket and a little gable roof.
// Villagers and players draw water here to douse house fires.
PropDef PropLibrary::build_well() {
    PropDef def;
    def.name = "well";
    const Vec3 stone{0.52f, 0.51f, 0.49f};
    const Vec3 wood{0.36f, 0.25f, 0.15f};
    const Vec3 water{0.12f, 0.26f, 0.34f};
    MeshData m;
    constexpr f32 ro = 0.95f; // rim outer half-extent
    constexpr f32 ri = 0.62f; // rim inner half-extent
    constexpr f32 rim_h = 0.62f;
    // Square stone rim (four kerb walls) around the shaft.
    add_box(m, {-ro, 0.0f, -ro}, {ro, rim_h, -ri}, stone);
    add_box(m, {-ro, 0.0f, ri}, {ro, rim_h, ro}, stone);
    add_box(m, {-ro, 0.0f, -ri}, {-ri, rim_h, ri}, stone);
    add_box(m, {ri, 0.0f, -ri}, {ro, rim_h, ri}, stone);
    add_box(m, {-ri, 0.05f, -ri}, {ri, 0.42f, ri}, water); // dark water surface
    // Two posts + crossbeam carrying a bucket.
    add_box(m, {-ro + 0.05f, rim_h, -0.12f}, {-ro + 0.25f, rim_h + 1.6f, 0.12f}, wood);
    add_box(m, {ro - 0.25f, rim_h, -0.12f}, {ro - 0.05f, rim_h + 1.6f, 0.12f}, wood);
    add_box(m, {-ro, rim_h + 1.55f, -0.1f}, {ro, rim_h + 1.72f, 0.1f}, wood);
    add_box(m, {-0.22f, rim_h + 0.55f, -0.22f}, {0.22f, rim_h + 0.95f, 0.22f}, wood * 0.85f); // bucket
    // Little gable roof.
    const f32 yr = rim_h + 1.72f;
    add_quad(m, {-ro - 0.15f, yr, ro + 0.15f}, {ro + 0.15f, yr, ro + 0.15f},
             {ro + 0.15f, yr + 0.55f, 0.0f}, {-ro - 0.15f, yr + 0.55f, 0.0f}, wood * 1.1f);
    add_quad(m, {ro + 0.15f, yr, -ro - 0.15f}, {-ro - 0.15f, yr, -ro - 0.15f},
             {-ro - 0.15f, yr + 0.55f, 0.0f}, {ro + 0.15f, yr + 0.55f, 0.0f}, wood * 1.0f);
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{ro, ro};
    c.height = rim_h;
    def.colliders.push_back(c);
    return def;
}

// A raised covered walkway that joins two neighbouring houses (placed between a house
// pair). It runs along local +X; the village places it spanning the gap, lifted to
// upper-storey height. A plank deck + railings + a couple of support posts + a little
// pitched roof, all opaque.
PropDef PropLibrary::build_bridge() {
    PropDef def;
    def.name = "bridge";
    const Vec3 wood{0.42f, 0.30f, 0.18f};
    const Vec3 dark{0.30f, 0.21f, 0.13f};
    MeshData m;
    constexpr f32 hl = 3.4f;  // half length (x) - spans the gap between two houses
    constexpr f32 hw = 0.7f;  // half width (z)
    constexpr f32 dy = 2.5f;  // deck height (upper-storey floor level)
    add_box(m, {-hl, dy - 0.12f, -hw}, {hl, dy, hw}, wood);             // plank deck
    add_box(m, {-hl, dy, -hw}, {hl, dy + 0.5f, -hw + 0.1f}, dark);      // near railing
    add_box(m, {-hl, dy, hw - 0.1f}, {hl, dy + 0.5f, hw}, dark);        // far railing
    // Support posts down to the ground at each end.
    for (f32 sx : {-hl + 0.2f, hl - 0.2f}) {
        add_box(m, {sx - 0.12f, 0.0f, -hw + 0.1f}, {sx + 0.12f, dy, -hw + 0.34f}, dark);
        add_box(m, {sx - 0.12f, 0.0f, hw - 0.34f}, {sx + 0.12f, dy, hw - 0.1f}, dark);
    }
    // A little pitched roof over the walkway.
    const f32 ry = dy + 0.5f;
    add_quad(m, {-hl, ry, -hw - 0.15f}, {hl, ry, -hw - 0.15f}, {hl, ry + 0.5f, 0.0f},
             {-hl, ry + 0.5f, 0.0f}, wood * 1.05f);
    add_quad(m, {hl, ry, hw + 0.15f}, {-hl, ry, hw + 0.15f}, {-hl, ry + 0.5f, 0.0f},
             {hl, ry + 0.5f, 0.0f}, wood * 0.95f);
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c; // the deck + posts block the gap underneath only lightly; post the ends
    c.center = Vec3{0.0f, 0.0f, 0.0f};
    c.half_extents = Vec2{hl, hw};
    c.height = 0.2f; // low: you can walk under it
    def.colliders.push_back(c);
    return def;
}

// The town's central marketplace: a stone market cross in the middle with four
// timber-and-cloth trading stalls at the corners. Placed at the plaza centre; reads
// as the heart of the town (where the goods cart will load/unload in a later stage).
// A big central market SQUARE (the heart of a sprawling town): a tall stepped market cross
// with a little capped roof, eight striped trading stalls ringing the plaza on its four
// sides (facing inward), and clusters of market goods - barrels, crates and produce sacks.
// Footprint ~ kMarketHalf (Village.h reserves it so houses + streets keep clear).
PropDef PropLibrary::build_market() {
    PropDef def;
    def.name = "market";
    const Vec3 wood{0.40f, 0.28f, 0.16f};
    const Vec3 dark{0.28f, 0.19f, 0.11f};
    const Vec3 stone{0.55f, 0.54f, 0.51f};
    const Vec3 crate{0.52f, 0.40f, 0.24f};
    const Vec3 sack{0.66f, 0.58f, 0.40f};
    const Vec3 cream{0.86f, 0.81f, 0.71f};
    const Vec3 cloths[6] = {{0.74f, 0.24f, 0.22f}, {0.25f, 0.45f, 0.62f}, {0.36f, 0.55f, 0.30f},
                            {0.80f, 0.62f, 0.22f}, {0.55f, 0.34f, 0.58f}, {0.78f, 0.45f, 0.20f}};
    const Vec3 produce[5] = {{0.82f, 0.24f, 0.18f}, {0.86f, 0.5f, 0.16f}, {0.4f, 0.6f, 0.24f},
                             {0.86f, 0.74f, 0.26f}, {0.62f, 0.32f, 0.5f}}; // apples / squash / cabbage / corn / plums
    MeshData m;

    // Market cross at the very centre: a broad stepped stone base, a tall timber post, a
    // stone cap and a small pyramidal roof - the town's meeting point.
    add_box(m, {-1.5f, 0.0f, -1.5f}, {1.5f, 0.3f, 1.5f}, stone);
    add_box(m, {-1.1f, 0.3f, -1.1f}, {1.1f, 0.56f, 1.1f}, stone * 1.04f);
    add_box(m, {-0.7f, 0.56f, -0.7f}, {0.7f, 0.82f, 0.7f}, stone);
    add_box(m, {-0.2f, 0.82f, -0.2f}, {0.2f, 3.5f, 0.2f}, wood);
    add_box(m, {-0.62f, 3.5f, -0.62f}, {0.62f, 3.74f, 0.62f}, stone);
    const Vec3 apex{0.0f, 4.4f, 0.0f};
    add_tri(m, {-0.78f, 3.74f, 0.78f}, {0.78f, 3.74f, 0.78f}, apex, stone * 1.07f);
    add_tri(m, {0.78f, 3.74f, -0.78f}, {-0.78f, 3.74f, -0.78f}, apex, stone * 0.96f);
    add_tri(m, {0.78f, 3.74f, 0.78f}, {0.78f, 3.74f, -0.78f}, apex, stone * 1.02f);
    add_tri(m, {-0.78f, 3.74f, -0.78f}, {-0.78f, 3.74f, 0.78f}, apex, stone * 0.99f);

    // A striped trading stall centred at (cx,cz), facing inward. `axis` 0 = counter runs along
    // x (north/south sides), 1 = along z (east/west); `sdir` is the outward normal so the
    // awning slopes down toward the plaza and the goods sit on the customer side.
    auto stall = [&](f32 cx, f32 cz, int axis, f32 sdir, const Vec3& cloth) {
        const f32 hw = 1.15f; // half-extent along the counter
        const f32 hd = 0.6f;  // half-depth (toward / away from centre)
        const Vec3 lo = axis == 0 ? Vec3{cx - hw, 0.0f, cz - hd} : Vec3{cx - hd, 0.0f, cz - hw};
        const Vec3 hi = axis == 0 ? Vec3{cx + hw, 0.0f, cz + hd} : Vec3{cx + hd, 0.0f, cz + hw};
        add_box(m, lo, {hi.x, 0.9f, hi.z}, wood);                       // counter body
        add_box(m, {lo.x, 0.9f, lo.z}, {hi.x, 1.0f, hi.z}, dark);       // counter top
        // Colourful produce piles + crates on the counter (the "more detailed" goods).
        for (int g = 0; g < 3; ++g) {
            const f32 u = -hw + 0.45f + static_cast<f32>(g) * (hw * 0.72f);
            const Vec3 gc = axis == 0 ? Vec3{cx + u, 1.0f, cz} : Vec3{cx, 1.0f, cz + u};
            const u32 gi = static_cast<u32>(std::abs(cx) * 3.0f + std::abs(cz) * 2.0f) + static_cast<u32>(g);
            if (g == 1) {
                add_box(m, gc - Vec3{0.2f, 0.0f, 0.2f}, gc + Vec3{0.2f, 0.22f, 0.2f}, crate); // a crate
            } else {
                add_box(m, gc - Vec3{0.2f, 0.0f, 0.18f}, gc + Vec3{0.2f, 0.16f, 0.18f}, produce[gi % 5]);
                add_box(m, gc - Vec3{0.12f, 0.0f, 0.1f}, gc + Vec3{0.14f, 0.24f, 0.12f}, produce[(gi + 2u) % 5]);
            }
        }
        constexpr f32 ph = 2.2f;
        for (f32 ex : {lo.x + 0.08f, hi.x - 0.08f}) {
            for (f32 ez : {lo.z + 0.08f, hi.z - 0.08f}) {
                add_box(m, {ex - 0.06f, 0.0f, ez - 0.06f}, {ex + 0.06f, ph, ez + 0.06f}, dark);
            }
        }
        // Striped awning sloping down toward the plaza (the customer side).
        constexpr int strips = 4;
        const f32 back = axis == 0 ? cz - sdir * (hd + 0.2f) : cx - sdir * (hd + 0.2f);
        const f32 front = axis == 0 ? cz + sdir * (hd + 0.55f) : cx + sdir * (hd + 0.55f);
        for (int k = 0; k < strips; ++k) {
            const f32 u0 = -hw + (2.0f * hw) * static_cast<f32>(k) / static_cast<f32>(strips);
            const f32 u1 = -hw + (2.0f * hw) * static_cast<f32>(k + 1) / static_cast<f32>(strips);
            const Vec3 c = (k % 2 == 0) ? cloth : cream;
            if (axis == 0) {
                add_quad(m, {cx + u0, ph + 0.45f, back}, {cx + u1, ph + 0.45f, back},
                         {cx + u1, ph, front}, {cx + u0, ph, front}, c);
            } else {
                add_quad(m, {back, ph + 0.45f, cz + u0}, {back, ph + 0.45f, cz + u1},
                         {front, ph, cz + u1}, {front, ph, cz + u0}, c);
            }
        }
        BoxCollider col;
        col.center = Vec3{cx, 0.0f, cz};
        col.half_extents = axis == 0 ? Vec2{hw, hd} : Vec2{hd, hw};
        col.height = 1.0f;
        def.colliders.push_back(col);
    };
    constexpr f32 R = 6.2f;   // stall ring radius
    constexpr f32 off = 2.7f; // two stalls per side, offset along it
    stall(-off, R, 0, 1.0f, cloths[0]);
    stall(off, R, 0, 1.0f, cloths[1]);   // north side
    stall(-off, -R, 0, -1.0f, cloths[2]);
    stall(off, -R, 0, -1.0f, cloths[3]); // south side
    stall(R, -off, 1, 1.0f, cloths[4]);
    stall(R, off, 1, 1.0f, cloths[5]);   // east side
    stall(-R, -off, 1, -1.0f, cloths[1]);
    stall(-R, off, 1, -1.0f, cloths[0]); // west side

    // Market goods scattered around the plaza: barrels, crate stacks and produce sacks.
    auto barrel = [&](f32 x, f32 z) {
        add_box(m, {x - 0.32f, 0.0f, z - 0.32f}, {x + 0.32f, 0.8f, z + 0.32f}, wood * 1.05f);
        add_box(m, {x - 0.36f, 0.18f, z - 0.36f}, {x + 0.36f, 0.3f, z + 0.36f}, dark);  // hoop
        add_box(m, {x - 0.36f, 0.5f, z - 0.36f}, {x + 0.36f, 0.62f, z + 0.36f}, dark);  // hoop
    };
    auto box_pile = [&](f32 x, f32 z) {
        add_box(m, {x - 0.34f, 0.0f, z - 0.34f}, {x + 0.34f, 0.6f, z + 0.34f}, crate);
        add_box(m, {x - 0.1f, 0.6f, z - 0.28f}, {x + 0.4f, 1.1f, z + 0.22f}, crate * 0.9f);
    };
    auto sacks = [&](f32 x, f32 z) {
        add_box(m, {x - 0.28f, 0.0f, z - 0.22f}, {x + 0.28f, 0.42f, z + 0.22f}, sack);
        add_box(m, {x - 0.1f, 0.0f, z + 0.12f}, {x + 0.34f, 0.36f, z + 0.5f}, sack * 0.92f);
    };
    barrel(2.6f, 2.6f);
    barrel(3.2f, 2.0f);
    box_pile(-3.0f, 2.4f);
    sacks(-2.4f, -3.0f);
    box_pile(2.5f, -2.8f);
    barrel(-3.2f, -2.0f);

    // A stone WELL in the plaza (a round rim of dark water, two posts + a windlass crossbeam, a
    // hanging bucket, and a little pitched roof) - the town's water source + meeting point.
    {
        const f32 wx = 3.4f, wz = -3.4f;
        const Vec3 wstone{0.56f, 0.57f, 0.6f};
        add_box(m, {wx - 0.78f, 0.0f, wz - 0.78f}, {wx + 0.78f, 0.32f, wz + 0.78f}, wstone * 0.95f); // base
        add_box(m, {wx - 0.66f, 0.32f, wz - 0.66f}, {wx + 0.66f, 0.74f, wz + 0.66f}, wstone);        // rim
        add_box(m, {wx - 0.5f, 0.6f, wz - 0.5f}, {wx + 0.5f, 0.66f, wz + 0.5f}, Vec3{0.1f, 0.13f, 0.18f}); // water
        for (f32 sx : {-1.0f, 1.0f}) {
            add_box(m, {wx + sx * 0.56f - 0.06f, 0.74f, wz - 0.06f}, {wx + sx * 0.56f + 0.06f, 2.35f, wz + 0.06f}, wood);
        }
        add_box(m, {wx - 0.64f, 2.24f, wz - 0.08f}, {wx + 0.64f, 2.4f, wz + 0.08f}, wood);     // crossbeam
        add_box(m, {wx - 0.22f, 2.18f, wz - 0.11f}, {wx + 0.22f, 2.36f, wz + 0.11f}, dark);    // windlass
        add_box(m, {wx - 0.02f, 1.45f, wz - 0.02f}, {wx + 0.02f, 2.18f, wz + 0.02f}, dark);    // rope
        add_box(m, {wx - 0.17f, 1.12f, wz - 0.17f}, {wx + 0.17f, 1.45f, wz + 0.17f}, wood * 0.92f); // bucket
        const Vec3 wapex{wx, 3.05f, wz};
        add_tri(m, {wx - 0.85f, 2.4f, wz + 0.85f}, {wx + 0.85f, 2.4f, wz + 0.85f}, wapex, dark * 1.3f);
        add_tri(m, {wx + 0.85f, 2.4f, wz - 0.85f}, {wx - 0.85f, 2.4f, wz - 0.85f}, wapex, dark * 1.1f);
        add_tri(m, {wx + 0.85f, 2.4f, wz + 0.85f}, {wx + 0.85f, 2.4f, wz - 0.85f}, wapex, dark * 1.2f);
        add_tri(m, {wx - 0.85f, 2.4f, wz - 0.85f}, {wx - 0.85f, 2.4f, wz + 0.85f}, wapex, dark * 1.15f);
        BoxCollider wc;
        wc.center = Vec3{wx, 0.0f, wz};
        wc.half_extents = Vec2{0.78f, 0.78f};
        wc.height = 0.74f;
        def.colliders.push_back(wc);
    }
    // A couple of hay bales by the market (golden cubes bound with twine).
    auto hay = [&](f32 x, f32 z) {
        add_box(m, {x - 0.42f, 0.0f, z - 0.32f}, {x + 0.42f, 0.6f, z + 0.32f}, Vec3{0.78f, 0.64f, 0.26f});
        add_box(m, {x - 0.44f, 0.18f, z - 0.34f}, {x + 0.44f, 0.26f, z + 0.34f}, Vec3{0.6f, 0.48f, 0.18f});
        add_box(m, {x - 0.44f, 0.4f, z - 0.34f}, {x + 0.44f, 0.48f, z + 0.34f}, Vec3{0.6f, 0.48f, 0.18f});
    };
    hay(-3.6f, 3.4f);
    hay(-2.9f, 3.7f);

    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider base; // the market cross blocks the very centre
    base.half_extents = Vec2{1.5f, 1.5f};
    base.height = 0.5f;
    def.colliders.push_back(base);
    return def;
}

// A low-poly goods cart BODY: an open plank bed on axles with side rails, a couple of
// cargo crates, and a draw tongue out the front (+X = forward). The wheels are a separate
// mesh (build_wagon_wheel) so the client can spin them as the cart rolls. It's a networked
// transport entity (not scattered), so the client builds this once and draws it per wagon.
PropDef PropLibrary::build_wagon() {
    PropDef def;
    def.name = "wagon";
    const Vec3 wood{0.45f, 0.32f, 0.18f};
    const Vec3 dark{0.27f, 0.19f, 0.11f};
    const Vec3 metal{0.30f, 0.30f, 0.33f};
    const Vec3 crate{0.52f, 0.40f, 0.24f};
    MeshData m;

    // Axles between the wheel pairs (the wheels themselves spin separately).
    for (f32 wx : {-0.8f, 0.8f}) {
        add_box(m, {wx - 0.06f, kWagonWheelRadius - 0.05f, -0.62f},
                {wx + 0.06f, kWagonWheelRadius + 0.05f, 0.62f}, metal);
    }

    // Plank bed + low side/end rails.
    add_box(m, {-1.0f, 0.6f, -0.58f}, {1.0f, 0.72f, 0.58f}, wood);          // bed floor
    add_box(m, {-1.0f, 0.72f, 0.50f}, {1.0f, 1.05f, 0.58f}, wood);          // right rail
    add_box(m, {-1.0f, 0.72f, -0.58f}, {1.0f, 1.05f, -0.50f}, wood);        // left rail
    add_box(m, {0.92f, 0.72f, -0.58f}, {1.0f, 1.05f, 0.58f}, wood);         // front board
    add_box(m, {-1.0f, 0.72f, -0.58f}, {-0.92f, 1.15f, 0.58f}, wood);       // back board (taller)

    // Cargo crates on the bed.
    add_box(m, {-0.55f, 0.72f, -0.34f}, {0.05f, 1.18f, 0.28f}, crate);
    add_box(m, {0.12f, 0.72f, -0.1f}, {0.62f, 1.02f, 0.4f}, crate * 0.9f);
    add_box(m, {0.18f, 0.72f, -0.42f}, {0.58f, 0.98f, -0.06f}, crate * 1.05f);

    // Draw tongue + handle out the front.
    add_box(m, {1.0f, 0.5f, -0.07f}, {1.9f, 0.62f, 0.07f}, dark);
    add_box(m, {1.8f, 0.5f, -0.35f}, {1.9f, 0.62f, 0.35f}, dark);

    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{1.0f, 0.6f};
    c.height = 1.1f;
    def.colliders.push_back(c);
    return def;
}

// A single cart wheel: an octagon in the XY plane with its axle along +Z, centred at the
// origin, so the client can place it at each axle and rotate it about Z to roll.
PropDef PropLibrary::build_wagon_wheel() {
    PropDef def;
    def.name = "wagon_wheel";
    const Vec3 dark{0.22f, 0.15f, 0.09f};
    const Vec3 hub{0.30f, 0.30f, 0.33f};
    MeshData m;
    constexpr int n = 8;
    constexpr f32 ht = 0.08f;
    constexpr f32 r = kWagonWheelRadius;
    const Vec3 fc{0.0f, 0.0f, ht};
    const Vec3 bc{0.0f, 0.0f, -ht};
    for (int i = 0; i < n; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(n);
        const Vec3 f0{std::cos(a0) * r, std::sin(a0) * r, ht};
        const Vec3 f1{std::cos(a1) * r, std::sin(a1) * r, ht};
        const Vec3 b0{std::cos(a0) * r, std::sin(a0) * r, -ht};
        const Vec3 b1{std::cos(a1) * r, std::sin(a1) * r, -ht};
        add_tri(m, fc, f0, f1, dark);      // front cap
        add_tri(m, bc, b1, b0, dark);      // back cap
        add_quad(m, f1, f0, b0, b1, dark); // rim
    }
    // Crossed spokes + hub (pale), so the spin reads clearly as it rolls.
    add_box(m, {-r * 0.92f, -0.05f, -0.04f}, {r * 0.92f, 0.05f, 0.04f}, hub); // spoke
    add_box(m, {-0.05f, -r * 0.92f, -0.04f}, {0.05f, r * 0.92f, 0.04f}, hub); // spoke
    add_box(m, {-0.12f, -0.12f, -ht - 0.03f}, {0.12f, 0.12f, ht + 0.03f}, hub); // hub
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    return def;
}

// A worn, muddy cobbled street tile: rough rounded rocks of varied size and earthy tone,
// packed (some nearly touching, with worn muddy gaps) into a dark mud bed SUNK into the
// ground. No neat grid, no bright whites - the stones are irregular lumps with jittered,
// tilted tops so it reads as dirty hand-laid rock, not tiles. Every stone rises to its own
// height clear of the bed, so no two faces are coplanar (no z-fighting). Tiles abut
// edge-to-edge (no overlap) into one continuous street. NO collider (you walk/drive on it).
PropDef PropLibrary::build_path_tile() {
    PropDef def;
    def.name = "path_tile";
    // Light, warm grey-tan flagstones (clean and pale like the reference - NOT dark muddy rocks),
    // laid as flat irregular slabs over warm packed earth.
    const Vec3 earth{0.4f, 0.31f, 0.2f}; // warm dirt showing between the slabs
    const Vec3 stones[6] = {{0.68f, 0.66f, 0.62f}, {0.62f, 0.6f, 0.56f}, {0.72f, 0.69f, 0.63f},
                            {0.66f, 0.62f, 0.55f}, {0.58f, 0.57f, 0.55f}, {0.7f, 0.65f, 0.57f}};
    MeshData m;
    constexpr f32 hx = 1.18f, hz = 1.18f;
    add_box(m, {-hx, -0.12f, -hz}, {hx, 0.02f, hz}, earth); // earth bed, top ~ at ground

    auto rnd = [](int i, int j, int salt) {
        const u32 h = (static_cast<u32>(i * 73856093) ^ static_cast<u32>(j * 19349663) ^
                       static_cast<u32>(salt * 83492791));
        return static_cast<f32>((h >> 9) & 0xFFFFu) / 65535.0f;
    };
    // Irregular HEXAGONAL cobbles laid on a brick-offset grid (alternate rows shifted half a cell
    // so the hexes interlock). Each stone is sized a little under its cell, so a mortar GAP of bare
    // earth always shows between neighbouring cobbles - and each hexagon is individually rotated and
    // has jittered corner radii, so they read as varied hand-cut stones, not a tidy tiling.
    constexpr int cols = 5, rows = 5;
    const f32 cellx = (2.0f * hx) / static_cast<f32>(cols);
    const f32 cellz = (2.0f * hz) / static_cast<f32>(rows);
    for (int j = 0; j < rows; ++j) {
        const f32 rowoff = (j & 1) ? cellx * 0.5f : 0.0f; // brick/hex offset on alternate rows
        for (int i = 0; i < cols; ++i) {
            if (rnd(i, j, 11) < 0.10f) {
                continue; // an occasional missing cobble (trodden-out, bare earth shows)
            }
            // The hexagon's outer radius, kept under half the cell so the mortar gap is guaranteed.
            const f32 R = glm::min(cellx, cellz) * (0.40f + rnd(i, j, 3) * 0.07f);
            f32 cx = -hx + (static_cast<f32>(i) + 0.5f) * cellx + rowoff +
                     (rnd(i, j, 1) - 0.5f) * cellx * 0.18f;
            f32 cz = -hz + (static_cast<f32>(j) + 0.5f) * cellz + (rnd(i, j, 2) - 0.5f) * cellz * 0.18f;
            cx = glm::clamp(cx, -hx + R, hx - R); // keep it inside the tile (no seam overhang)
            cz = glm::clamp(cz, -hz + R, hz - R);
            const f32 rot = rnd(i, j, 8) * TwoPi; // each hexagon turned a random way
            const f32 top = 0.07f + rnd(i, j, 4) * 0.04f; // a low FLAT cobble, slightly proud
            const f32 base = top - (0.05f + rnd(i, j, 7) * 0.03f);
            const f32 shade = 0.9f + rnd(i, j, 6) * 0.2f;
            const Vec3 col = stones[(i + j * 2 + static_cast<int>(rnd(i, j, 5) * 6.0f)) % 6] * shade;
            // 6 jittered corners -> an irregular hexagon.
            Vec2 hexv[6];
            for (int k = 0; k < 6; ++k) {
                const f32 a = rot + TwoPi * static_cast<f32>(k) / 6.0f;
                const f32 rr = R * (0.82f + rnd(i, j, 30 + k) * 0.30f);
                hexv[k] = Vec2{cx + std::cos(a) * rr, cz + std::sin(a) * rr};
            }
            const Vec3 axis{cx, top * 0.5f, cz};
            const Vec3 ctr{cx, top, cz};
            auto T = [&](const Vec2& c) { return Vec3{c.x, top, c.y}; };
            auto B = [&](const Vec2& c) { return Vec3{c.x, base, c.y}; };
            for (int k = 0; k < 6; ++k) {
                const Vec2& a = hexv[k];
                const Vec2& b = hexv[(k + 1) % 6];
                emit_tri(m, ctr, T(a), T(b), axis, col);          // flat top (fan from centre)
                emit_tri(m, B(a), B(b), T(b), axis, col * 0.85f); // short straight side
                emit_tri(m, B(a), T(b), T(a), axis, col * 0.85f);
            }
        }
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    return def;
}

// A pot of greenery (stone/wood planter + a leafy bush + a couple of flowers) to green up
// the town plaza/streets.
PropDef PropLibrary::build_planter() {
    PropDef def;
    def.name = "planter";
    const Vec3 pot{0.46f, 0.36f, 0.28f};
    const Vec3 soil{0.20f, 0.14f, 0.10f};
    MeshData op;
    add_box(op, {-0.4f, 0.0f, -0.4f}, {0.4f, 0.5f, 0.4f}, pot);       // pot body
    add_box(op, {-0.45f, 0.5f, -0.45f}, {0.45f, 0.6f, 0.45f}, pot * 1.1f); // rim
    add_box(op, {-0.34f, 0.45f, -0.34f}, {0.34f, 0.52f, 0.34f}, soil);     // soil
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({primitives::bush(0, Vec3{0.26f, 0.46f, 0.22f}), PropLayer::Foliage});
    BoxCollider c;
    c.half_extents = Vec2{0.42f, 0.42f};
    c.height = 0.6f;
    def.colliders.push_back(c);
    return def;
}

// A round stone plaza fountain: a low circular basin of water with a central tiered spout.
PropDef PropLibrary::build_fountain() {
    PropDef def;
    def.name = "fountain";
    const Vec3 stone{0.55f, 0.54f, 0.50f};
    const Vec3 water{0.18f, 0.34f, 0.46f};
    MeshData op;
    MeshData em;
    constexpr int n = 12;
    constexpr f32 ro = 1.7f, ri = 1.35f, rim_h = 0.5f;
    // Octa/dodeca-gonal stone rim + water surface, built as wedges.
    for (int i = 0; i < n; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(n);
        const Vec3 o0{std::cos(a0) * ro, 0.0f, std::sin(a0) * ro};
        const Vec3 o1{std::cos(a1) * ro, 0.0f, std::sin(a1) * ro};
        const Vec3 i0{std::cos(a0) * ri, 0.0f, std::sin(a0) * ri};
        const Vec3 i1{std::cos(a1) * ri, 0.0f, std::sin(a1) * ri};
        const Vec3 up{0.0f, rim_h, 0.0f};
        add_quad(op, o0, o1, o1 + up, o0 + up, stone);             // outer rim wall
        add_quad(op, i0 + up, i1 + up, o1 + up, o0 + up, stone * 1.05f); // rim top
        add_quad(op, i0, i0 + Vec3{0, rim_h * 0.7f, 0}, i1 + Vec3{0, rim_h * 0.7f, 0}, i1,
                 stone * 0.9f); // inner wall
        const Vec3 wc{0.0f, rim_h * 0.7f, 0.0f};
        add_tri(op, wc, Vec3{i1.x, rim_h * 0.7f, i1.z}, Vec3{i0.x, rim_h * 0.7f, i0.z}, water);
    }
    // Central tiered spout.
    add_box(op, {-0.25f, rim_h * 0.7f, -0.25f}, {0.25f, 1.4f, 0.25f}, stone);
    add_box(op, {-0.5f, 1.0f, -0.5f}, {0.5f, 1.15f, 0.5f}, stone * 1.05f);
    add_box(em, {-0.18f, 1.4f, -0.18f}, {0.18f, 1.6f, 0.18f}, water * 1.4f); // glinting water top
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    BoxCollider c;
    c.half_extents = Vec2{ro, ro};
    c.height = rim_h;
    def.colliders.push_back(c);
    return def;
}

// Medieval town clutter that fills out a town (see kDecorVariants). Each is a small low-poly
// prop with a collider where you'd bump into it. Placed by the village layout around houses,
// along the streets and in the market plaza.
PropDef PropLibrary::build_decor(int variant) {
    PropDef def;
    const Vec3 wood{0.42f, 0.29f, 0.16f};
    const Vec3 dark{0.27f, 0.18f, 0.10f};
    const Vec3 stone{0.52f, 0.51f, 0.48f};
    const Vec3 hay{0.80f, 0.68f, 0.30f};
    const Vec3 cream{0.85f, 0.80f, 0.70f};
    const Vec3 water{0.18f, 0.34f, 0.42f};
    MeshData m;
    auto collider = [&](f32 hx, f32 hz, f32 h) {
        BoxCollider c;
        c.half_extents = Vec2{hx, hz};
        c.height = h;
        def.colliders.push_back(c);
    };
    switch (variant % static_cast<int>(kDecorVariants)) {
        case 0: // a barrel
            def.name = "barrel";
            add_box(m, {-0.33f, 0.0f, -0.33f}, {0.33f, 0.9f, 0.33f}, wood);
            add_box(m, {-0.37f, 0.16f, -0.37f}, {0.37f, 0.28f, 0.37f}, dark);  // iron hoop
            add_box(m, {-0.37f, 0.56f, -0.37f}, {0.37f, 0.68f, 0.37f}, dark);  // iron hoop
            add_box(m, {-0.29f, 0.9f, -0.29f}, {0.29f, 0.96f, 0.29f}, wood * 0.9f); // lid
            collider(0.36f, 0.36f, 0.9f);
            break;
        case 1: // a stack of crates
            def.name = "crates";
            add_box(m, {-0.42f, 0.0f, -0.42f}, {0.42f, 0.72f, 0.42f}, wood * 1.08f);
            add_box(m, {-0.44f, 0.31f, -0.44f}, {0.44f, 0.4f, 0.44f}, dark); // banding
            add_box(m, {-0.18f, 0.72f, -0.36f}, {0.5f, 1.34f, 0.3f}, wood);   // crate on top
            add_box(m, {-0.5f, 0.0f, 0.16f}, {0.04f, 0.52f, 0.66f}, wood * 0.92f); // crate beside
            collider(0.58f, 0.58f, 0.72f);
            break;
        case 2: // hay bales
            def.name = "hay";
            add_box(m, {-0.55f, 0.0f, -0.4f}, {0.55f, 0.6f, 0.4f}, hay);
            add_box(m, {-0.5f, 0.0f, 0.42f}, {0.42f, 0.56f, 1.08f}, hay * 0.95f);
            add_box(m, {-0.42f, 0.6f, -0.34f}, {0.46f, 1.12f, 0.3f}, hay * 1.04f); // stacked bale
            add_box(m, {-0.46f, 0.28f, -0.41f}, {0.46f, 0.34f, -0.39f}, dark);     // binding twine
            collider(0.6f, 0.72f, 0.6f);
            break;
        case 3: { // a small standalone market stall with an awning + goods on the counter
            def.name = "stall";
            const Vec3 cloth{0.30f, 0.48f, 0.60f};
            add_box(m, {-1.0f, 0.0f, -0.45f}, {1.0f, 0.82f, 0.45f}, wood);  // counter
            add_box(m, {-1.0f, 0.82f, -0.45f}, {1.0f, 0.92f, 0.45f}, dark); // counter top
            for (f32 ex : {-0.92f, 0.92f}) {
                for (f32 ez : {-0.37f, 0.37f}) {
                    add_box(m, {ex - 0.05f, 0.0f, ez - 0.05f}, {ex + 0.05f, 2.1f, ez + 0.05f}, dark);
                }
            }
            for (int k = 0; k < 4; ++k) { // striped awning sloping to the front
                const f32 x0 = -1.0f + 0.5f * static_cast<f32>(k);
                const f32 x1 = -1.0f + 0.5f * static_cast<f32>(k + 1);
                const Vec3 c = (k % 2 == 0) ? cloth : cream;
                add_quad(m, {x0, 2.3f, -0.6f}, {x1, 2.3f, -0.6f}, {x1, 2.0f, 0.7f}, {x0, 2.0f, 0.7f}, c);
            }
            add_box(m, {-0.7f, 0.92f, -0.2f}, {-0.3f, 1.2f, 0.2f}, wood * 1.1f);         // crate of goods
            add_box(m, {0.2f, 0.92f, -0.2f}, {0.6f, 1.12f, 0.2f}, Vec3{0.6f, 0.5f, 0.3f}); // sacks
            collider(1.0f, 0.45f, 0.85f);
            break;
        }
        case 4: // a signpost with two pointing boards
            def.name = "signpost";
            add_box(m, {-0.07f, 0.0f, -0.07f}, {0.07f, 1.85f, 0.07f}, wood);
            add_box(m, {0.05f, 1.35f, -0.04f}, {0.95f, 1.7f, 0.04f}, cream);          // board (+x)
            add_box(m, {-0.95f, 0.95f, -0.04f}, {-0.05f, 1.3f, 0.04f}, cream * 0.92f); // board (-x)
            collider(0.1f, 0.1f, 1.6f);
            break;
        case 5: // a stone water trough
            def.name = "trough";
            add_box(m, {-0.95f, 0.0f, -0.42f}, {0.95f, 0.5f, 0.42f}, stone);
            add_box(m, {-0.8f, 0.16f, -0.3f}, {0.8f, 0.46f, 0.3f}, water); // water surface inside
            collider(0.95f, 0.42f, 0.5f);
            break;
        case 6: // a stacked woodpile (split logs)
            def.name = "woodpile";
            for (int row = 0; row < 3; ++row) {
                const f32 y0 = static_cast<f32>(row) * 0.28f;
                const int n = 5 - row;
                for (int k = 0; k < n; ++k) {
                    const f32 x = -0.7f + 0.32f * static_cast<f32>(k) + 0.16f * static_cast<f32>(row);
                    add_box(m, {x - 0.15f, y0, -0.6f}, {x + 0.15f, y0 + 0.28f, 0.6f},
                            wood * (0.85f + 0.06f * static_cast<f32>(k % 3)));
                }
            }
            collider(0.85f, 0.62f, 0.85f);
            break;
        default: // 7: a cluster of produce sacks + a basket
            def.name = "sacks";
            add_box(m, {-0.3f, 0.0f, -0.24f}, {0.3f, 0.46f, 0.24f}, Vec3{0.66f, 0.58f, 0.40f});
            add_box(m, {0.1f, 0.0f, 0.1f}, {0.56f, 0.38f, 0.56f}, Vec3{0.6f, 0.52f, 0.34f});
            add_box(m, {-0.5f, 0.0f, 0.12f}, {-0.08f, 0.34f, 0.54f}, Vec3{0.7f, 0.62f, 0.42f});
            add_box(m, {-0.2f, 0.46f, -0.18f}, {0.2f, 0.6f, 0.18f}, Vec3{0.5f, 0.7f, 0.3f}); // greens on top
            collider(0.56f, 0.56f, 0.46f);
            break;
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    return def;
}

// A low-poly medieval STONE ARCH BRIDGE carrying a road over a river (Bridge.kind 0). Built as a
// UNIT span along local +X (half-length 0.5) that the client stretches to the river width, pitches to
// meet each bank, and places at bank level; the roadway is a gentle ARCH (matching roads::bridge_deck_y)
// with a single chunky semicircular arch opening for the river, stone abutments, a cobbled deck and low
// parapet walls. Few, big facets for the faceted low-poly look. Faces local +X (the road's heading).
PropDef PropLibrary::build_arch_bridge() {
    PropDef def;
    def.name = "arch_bridge";
    const Vec3 stone{0.62f, 0.60f, 0.55f};  // warm pale ashlar
    const Vec3 dark{0.42f, 0.40f, 0.37f};   // shaded under-arch
    const Vec3 cobble{0.56f, 0.52f, 0.47f}; // roadway
    MeshData m;
    constexpr f32 hl = 0.5f;                  // unit half-length (X); the client scales it
    const f32 hw = roads::bridge_half_width;   // deck half-width (Z) - matches the collision deck
    const f32 rise = roads::bridge_arch_rise;  // deck hump at the crown - matches bridge_deck_y
    constexpr f32 ax = 0.42f;                  // arch opening half-span (unit X)
    constexpr f32 spring_y = -1.3f;            // arch springing line (below the deck)
    constexpr f32 arch_h = 1.2f;               // arch height (crown above the springing)
    constexpr int seg = 7;                     // few, chunky facets (low-poly)
    auto deck_top = [&](f32 x) { const f32 t = x / hl; return rise * (1.0f - t * t); };
    auto soffit = [&](f32 x) {
        const f32 r = std::abs(x) / ax;
        return r < 1.0f ? spring_y + arch_h * std::sqrt(std::max(0.0f, 1.0f - r * r)) : -2.0f;
    };
    for (int i = 1; i <= seg; ++i) {
        const f32 x0 = glm::mix(-hl, hl, static_cast<f32>(i - 1) / static_cast<f32>(seg));
        const f32 x1 = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(seg));
        const f32 d0 = deck_top(x0), d1 = deck_top(x1);
        const f32 s0 = soffit(x0), s1 = soffit(x1);
        const Vec3 axis{(x0 + x1) * 0.5f, (d0 + s0) * 0.5f, 0.0f};
        add_quad(m, {x0, d0, -hw}, {x1, d1, -hw}, {x1, d1, hw}, {x0, d0, hw}, cobble); // roadway
        for (f32 sz : {-1.0f, 1.0f}) { // the two spandrel faces (the visible arch)
            const f32 z = sz * hw;
            emit_tri(m, {x0, s0, z}, {x1, s1, z}, {x1, d1, z}, axis, stone);
            emit_tri(m, {x0, s0, z}, {x1, d1, z}, {x0, d0, z}, axis, stone);
        }
        add_quad(m, {x0, s0, -hw}, {x0, s0, hw}, {x1, s1, hw}, {x1, s1, -hw}, dark); // arch underside
    }
    // Chunky low parapet walls along each side, following the deck hump.
    for (int i = 1; i <= seg; ++i) {
        const f32 x0 = glm::mix(-hl, hl, static_cast<f32>(i - 1) / static_cast<f32>(seg));
        const f32 x1 = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(seg));
        const f32 d0 = deck_top(x0), d1 = deck_top(x1);
        for (f32 sz : {-1.0f, 1.0f}) {
            const f32 z = sz * (hw - 0.14f);
            for (f32 zo : {-0.14f, 0.14f}) {
                const Vec3 axis{(x0 + x1) * 0.5f, d0 + 0.28f, z + zo};
                emit_tri(m, {x0, d0, z + zo}, {x1, d1, z + zo}, {x1, d1 + 0.56f, z + zo}, axis, stone);
                emit_tri(m, {x0, d0, z + zo}, {x1, d1 + 0.56f, z + zo}, {x0, d0 + 0.56f, z + zo}, axis,
                         stone);
            }
            add_quad(m, {x0, d0 + 0.56f, z - 0.14f}, {x1, d1 + 0.56f, z - 0.14f},
                     {x1, d1 + 0.56f, z + 0.14f}, {x0, d0 + 0.56f, z + 0.14f}, dark); // parapet cap
        }
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    return def;
}

// A low-poly WOODEN plank bridge carrying a road over a river (Bridge.kind 1). Same UNIT span + deck
// arch as the stone bridge (so the walkable deck matches roads::bridge_deck_y), but built from timber:
// chunky cross-planks for the roadway, a couple of trestle leg-pairs dropping to the riverbed, and
// post-and-rail railings down each side. Warm wood tones. Faces local +X (the road's heading).
PropDef PropLibrary::build_plank_bridge() {
    PropDef def;
    def.name = "plank_bridge";
    const Vec3 plank{0.46f, 0.31f, 0.18f};  // warm timber
    const Vec3 plank2{0.40f, 0.26f, 0.15f}; // a darker plank (alternating)
    const Vec3 post{0.34f, 0.22f, 0.13f};   // posts / beams (darker)
    MeshData m;
    constexpr f32 hl = 0.5f;
    const f32 hw = roads::bridge_half_width;
    const f32 rise = roads::bridge_arch_rise;
    auto deck_top = [&](f32 x) { const f32 t = x / hl; return rise * (1.0f - t * t); };
    constexpr int planks = 11; // chunky cross-planks across the span
    constexpr f32 deck_thick = 0.16f;
    // Two stringer beams running the length under the planks (the deck's spine).
    for (f32 sz : {-1.0f, 1.0f}) {
        const f32 z = sz * (hw - 0.22f);
        for (int i = 1; i <= planks; ++i) {
            const f32 x0 = glm::mix(-hl, hl, static_cast<f32>(i - 1) / static_cast<f32>(planks));
            const f32 x1 = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(planks));
            const f32 d0 = deck_top(x0), d1 = deck_top(x1);
            add_box(m, {x0, std::min(d0, d1) - deck_thick - 0.18f, z - 0.1f},
                    {x1, std::min(d0, d1) - deck_thick, z + 0.1f}, post); // stringer segment
        }
    }
    // Cross-planks forming the roadway (alternating shade), following the arch hump.
    for (int i = 0; i < planks; ++i) {
        const f32 x0 = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(planks)) + 0.004f;
        const f32 x1 = glm::mix(-hl, hl, static_cast<f32>(i + 1) / static_cast<f32>(planks)) - 0.004f;
        const f32 d = deck_top((x0 + x1) * 0.5f);
        add_box(m, {x0, d - deck_thick, -hw}, {x1, d, hw}, (i % 2 == 0) ? plank : plank2);
    }
    // A couple of trestle leg-pairs dropping from the deck to the riverbed (under the arch).
    for (f32 lx : {-0.26f, 0.26f}) {
        const f32 d = deck_top(lx);
        for (f32 sz : {-1.0f, 1.0f}) {
            const f32 z = sz * (hw - 0.18f);
            // a splayed leg: top under the deck, foot kicked outward + down to the bed
            const Vec3 top{lx, d - deck_thick, z};
            const Vec3 foot{lx + 0.0f, -1.9f, z + sz * 0.18f};
            const Vec3 axis = (top + foot) * 0.5f;
            for (f32 zo : {-0.07f, 0.07f}) {
                emit_tri(m, {top.x - 0.07f, top.y, top.z + zo}, {top.x + 0.07f, top.y, top.z + zo},
                         {foot.x + 0.07f, foot.y, foot.z + zo}, axis, post);
                emit_tri(m, {top.x - 0.07f, top.y, top.z + zo}, {foot.x + 0.07f, foot.y, foot.z + zo},
                         {foot.x - 0.07f, foot.y, foot.z + zo}, axis, post);
            }
        }
        // a cross-brace beam tying the two legs together
        add_box(m, {lx - 0.06f, -1.0f, -hw + 0.1f}, {lx + 0.06f, -0.84f, hw - 0.1f}, post);
    }
    // Post-and-rail railings down each side (posts every couple of planks + a top rail).
    constexpr int rposts = 6;
    for (f32 sz : {-1.0f, 1.0f}) {
        const f32 z = sz * (hw - 0.06f);
        for (int i = 0; i <= rposts; ++i) {
            const f32 x = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(rposts));
            const f32 d = deck_top(x);
            add_box(m, {x - 0.04f, d, z - 0.05f}, {x + 0.04f, d + 0.62f, z + 0.05f}, post); // post
        }
        // top rail following the hump (segmented so it arcs)
        for (int i = 1; i <= rposts; ++i) {
            const f32 x0 = glm::mix(-hl, hl, static_cast<f32>(i - 1) / static_cast<f32>(rposts));
            const f32 x1 = glm::mix(-hl, hl, static_cast<f32>(i) / static_cast<f32>(rposts));
            const f32 d0 = deck_top(x0), d1 = deck_top(x1);
            const Vec3 axis{(x0 + x1) * 0.5f, (d0 + d1) * 0.5f + 0.52f, z};
            for (f32 zo : {-0.05f, 0.05f}) {
                emit_tri(m, {x0, d0 + 0.46f, z + zo}, {x1, d1 + 0.46f, z + zo},
                         {x1, d1 + 0.58f, z + zo}, axis, plank);
                emit_tri(m, {x0, d0 + 0.46f, z + zo}, {x1, d1 + 0.58f, z + zo},
                         {x0, d0 + 0.58f, z + zo}, axis, plank);
            }
        }
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    return def;
}

// A sunken river-channel tile for a river-town: a stone-lined canal running along local +X,
// with a teal water surface set just into the ground between two raised stone embankments
// (so it reads as a recessed channel without carving the terrain). Tiles abut end-to-end
// (x = -2..2) into one continuous river. The faint shimmer strip is emissive. No collider -
// the stream is shallow; the streets cross it on stone bridges.
PropDef PropLibrary::build_river() {
    PropDef def;
    def.name = "river";
    const Vec3 water{0.16f, 0.33f, 0.44f};
    const Vec3 stone{0.48f, 0.47f, 0.45f};
    const Vec3 earth{0.30f, 0.23f, 0.15f};
    MeshData op;
    MeshData em;
    constexpr f32 hl = 2.0f;  // half-length along the river (local x)
    constexpr f32 wb = 3.0f;  // water half-width
    constexpr f32 bank = 3.6f; // outer bank half-width

    // Water surface, just above the ground plane (hides the flat terrain beneath it).
    add_box(op, {-hl, 0.0f, -wb}, {hl, 0.1f, wb}, water);
    // A brighter shimmer strip down the middle (emissive, so it glints like moving water).
    add_box(em, {-hl, 0.11f, -0.7f}, {hl, 0.13f, 0.7f}, Vec3{0.32f, 0.55f, 0.62f});

    // Raised stone embankments either side, with an earthy outer slope down to the ground.
    for (f32 s : {-1.0f, 1.0f}) {
        const f32 zin = s * wb;
        const f32 zwall = s * (wb + 0.3f);
        const f32 zout = s * bank;
        add_box(op, {-hl, 0.0f, std::min(zin, zwall)}, {hl, 0.55f, std::max(zin, zwall)}, stone); // wall
        // earthen slope from the wall top-outer down to ground at the bank edge
        add_quad(op, {-hl, 0.55f, zwall}, {hl, 0.55f, zwall}, {hl, 0.0f, zout}, {-hl, 0.0f, zout}, earth);
    }
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A glowing magic CRYSTAL cluster: a few angular gem shards of varying size + tilt sprouting from a
// small dark rock base, in a colour by `variant` (amethyst / sapphire / cyan / emerald). The shards
// are emissive (glow at night) + an additive glow halo, and the def carries a coloured point light
// so the cluster pools magical light in the dark - the signature detail from the reference art.
PropDef PropLibrary::build_crystal(int variant) {
    PropDef def;
    def.name = "crystal";
    static const Vec3 cols[kCrystalVariants] = {
        {0.62f, 0.30f, 0.92f}, // amethyst purple
        {0.32f, 0.5f, 1.0f},   // sapphire blue
        {0.30f, 0.86f, 0.96f}, // cyan
        {0.32f, 0.92f, 0.5f},  // emerald
    };
    const Vec3 col = cols[variant % kCrystalVariants];
    const Vec3 rock{0.26f, 0.25f, 0.30f};
    MeshData op, em;

    auto rnd = [&](u32 s) {
        u32 v = (static_cast<u32>(variant) * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    // A faceted gem shard: a 4-sided spike from `base` along `dir`, widest a third of the way up
    // (the gem girdle) then tapering to a point.
    auto shard = [&](MeshData& m, const Vec3& base, const Vec3& dir, f32 len, f32 r, const Vec3& c) {
        const Vec3 up = glm::normalize(dir);
        const Vec3 a = glm::normalize(glm::cross(up, std::abs(up.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
        const Vec3 b = glm::cross(up, a);
        const Vec3 tip = base + up * len;
        const Vec3 mid = base + up * (len * 0.34f);
        const Vec3 ctr = base + up * (len * 0.5f);
        const Vec3 b0 = base + a * r * 0.35f, b1 = base + b * r * 0.35f, b2 = base - a * r * 0.35f, b3 = base - b * r * 0.35f;
        const Vec3 m0 = mid + a * r, m1 = mid + b * r, m2 = mid - a * r, m3 = mid - b * r;
        const Vec3 bb[4] = {b0, b1, b2, b3};
        const Vec3 mm[4] = {m0, m1, m2, m3};
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            emit_tri(m, bb[i], bb[j], mm[j], ctr, c);  // girdle side
            emit_tri(m, bb[i], mm[j], mm[i], ctr, c);
            emit_tri(m, mm[i], mm[j], tip, ctr, c * 1.12f); // facet to the point (brighter)
        }
    };

    // a small dark rocky base
    add_box(op, {-0.5f, -0.2f, -0.5f}, {0.5f, 0.18f, 0.5f}, rock);
    add_box(op, {-0.34f, 0.12f, -0.34f}, {0.36f, 0.32f, 0.34f}, rock * 1.1f);

    // a cluster of shards: one tall central spike + several smaller ones leaning out around it
    const int n = 4 + static_cast<int>(rnd(1) * 3.0f);
    shard(em, Vec3{0.0f, 0.18f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.5f + rnd(2) * 0.7f, 0.26f, col);
    for (int i = 0; i < n; ++i) {
        const f32 ang = TwoPi * (static_cast<f32>(i) / static_cast<f32>(n)) + rnd(i * 4 + 3) * 0.8f;
        const Vec3 out{std::cos(ang), 0.0f, std::sin(ang)};
        const Vec3 base = out * (0.18f + rnd(i * 4 + 4) * 0.22f) + Vec3{0.0f, 0.16f, 0.0f};
        const Vec3 dir = glm::normalize(out * (0.5f + rnd(i * 4 + 5) * 0.5f) + Vec3{0.0f, 1.0f, 0.0f});
        const f32 len = 0.7f + rnd(i * 4 + 6) * 0.9f;
        shard(em, base, dir, len, 0.14f + rnd(i * 4 + 3) * 0.1f, col * (0.9f + 0.2f * rnd(i)));
    }

    PropLight l;
    l.offset = Vec3{0.0f, 0.9f, 0.0f};
    l.direction = glm::normalize(Vec3{0.0f, 1.0f, 0.0f});
    l.color = col;
    l.range = 9.0f;
    l.intensity = 2.0f;
    l.cone_deg = 360.0f;
    def.lights.push_back(l);

    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    BoxCollider c;
    c.half_extents = Vec2{0.45f, 0.45f};
    c.height = 0.6f;
    def.colliders.push_back(c);
    return def;
}

// A bioluminescent MUSHROOM cluster: a few toadstools whose caps + underglow glow softly in a colour
// by `variant` (cyan / amber / violet), with a dim coloured light - magical forest-floor detail that
// reads at night. No collider (you walk over them).
PropDef PropLibrary::build_glow_shroom(int variant) {
    PropDef def;
    def.name = "glow_shroom";
    static const Vec3 cols[kGlowShroomVariants] = {
        {0.32f, 0.85f, 0.95f}, // cyan
        {1.0f, 0.6f, 0.22f},   // amber
        {0.7f, 0.42f, 0.95f},  // violet
    };
    const Vec3 glow = cols[variant % kGlowShroomVariants];
    const Vec3 stem{0.86f, 0.84f, 0.78f};
    MeshData op, em;
    auto rnd = [&](u32 s) {
        u32 v = (static_cast<u32>(variant) * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    const int n = 3 + static_cast<int>(rnd(1) * 3.0f);
    for (int i = 0; i < n; ++i) {
        const f32 ang = TwoPi * (static_cast<f32>(i) / static_cast<f32>(n)) + rnd(i * 5 + 2);
        const f32 rad = (i == 0) ? 0.0f : 0.18f + rnd(i * 5 + 3) * 0.28f;
        const f32 cxx = std::cos(ang) * rad, czz = std::sin(ang) * rad;
        const f32 sc = (i == 0 ? 1.0f : 0.55f + rnd(i * 5 + 4) * 0.5f);
        const f32 sh = 0.32f * sc, cr = 0.2f * sc;
        add_box(op, {cxx - 0.05f * sc, 0.0f, czz - 0.05f * sc}, {cxx + 0.05f * sc, sh, czz + 0.05f * sc}, stem);
        // glowing cap (emissive) - a little domed disc
        add_box(em, {cxx - cr, sh, czz - cr}, {cxx + cr, sh + 0.1f * sc, czz + cr}, glow);
        add_box(em, {cxx - cr * 0.6f, sh + 0.08f * sc, czz - cr * 0.6f}, {cxx + cr * 0.6f, sh + 0.16f * sc, czz + cr * 0.6f}, glow * 1.1f);
        // a faint underglow disc just above the ground
        add_box(em, {cxx - cr * 1.3f, 0.01f, czz - cr * 1.3f}, {cxx + cr * 1.3f, 0.04f, czz + cr * 1.3f}, glow * 0.5f);
    }
    PropLight l;
    l.offset = Vec3{0.0f, 0.25f, 0.0f};
    l.direction = Vec3{0.0f, 1.0f, 0.0f};
    l.color = glow;
    l.range = 5.5f;
    l.intensity = 1.1f;
    l.cone_deg = 360.0f;
    def.lights.push_back(l);
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A cosy CAMPFIRE: a ring of stones around a small stack of logs with a flickering emissive flame, an
// additive glow bloom and a warm point light - a rest-spot the dark wilderness lights up at night.
PropDef PropLibrary::build_campfire() {
    PropDef def;
    def.name = "campfire";
    const Vec3 stone{0.46f, 0.45f, 0.47f};
    const Vec3 wood{0.34f, 0.24f, 0.15f};
    const Vec3 char_{0.12f, 0.1f, 0.1f};
    const Vec3 fire{1.0f, 0.55f, 0.16f};
    MeshData op, em;
    // ring of stones
    for (int i = 0; i < 7; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 7.0f;
        const f32 sx = std::cos(a) * 0.62f, sz = std::sin(a) * 0.62f;
        add_box(op, {sx - 0.16f, 0.0f, sz - 0.16f}, {sx + 0.16f, 0.2f, sz + 0.16f}, stone * (0.9f + 0.2f * static_cast<f32>(i % 2)));
    }
    // a couple of charred logs crossed in the middle
    add_box(op, {-0.42f, 0.05f, -0.1f}, {0.42f, 0.18f, 0.1f}, char_);
    add_box(op, {-0.1f, 0.05f, -0.42f}, {0.1f, 0.18f, 0.42f}, wood * 0.7f);
    add_box(em, {-0.22f, 0.1f, -0.22f}, {0.22f, 0.24f, 0.22f}, Vec3{0.4f, 0.12f, 0.05f}); // embers
    // flickering flame tongues (emissive) + an additive bloom (glow pass)
    for (int i = 0; i < 5; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 5.0f;
        const f32 fx = std::cos(a) * 0.12f, fz = std::sin(a) * 0.12f;
        const f32 fh = 0.5f + 0.35f * std::abs(std::sin(static_cast<f32>(i) * 1.7f));
        add_box(em, {fx - 0.1f, 0.18f, fz - 0.1f}, {fx + 0.1f, fh, fz + 0.1f}, fire * (0.95f + 0.1f * static_cast<f32>(i % 2)));
    }
    add_box(em, {-0.14f, 0.22f, -0.14f}, {0.14f, 0.95f, 0.14f}, Vec3{1.6f, 1.0f, 0.4f}); // bright core
    PropLight l;
    l.offset = Vec3{0.0f, 0.6f, 0.0f};
    l.direction = Vec3{0.0f, 1.0f, 0.0f};
    l.color = Vec3{1.0f, 0.62f, 0.28f};
    l.range = 13.0f;
    l.intensity = 4.5f;
    l.cone_deg = 360.0f;
    def.lights.push_back(l);
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    BoxCollider c;
    c.half_extents = Vec2{0.7f, 0.7f};
    c.height = 0.2f;
    def.colliders.push_back(c);
    return def;
}

// A weathered stone MONUMENT - an ancient wilderness landmark by `variant`: 0 a tall carved obelisk
// on a stepped plinth, 1 a broken/leaning pillar with rubble, 2 a trio of rough standing stones.
// Mossy grey stone, faceted; a collider so you can't walk through it.
PropDef PropLibrary::build_monument(int variant) {
    PropDef def;
    def.name = "monument";
    const Vec3 stone{0.5f, 0.51f, 0.49f};
    const Vec3 dark{0.4f, 0.41f, 0.39f};
    const Vec3 moss{0.32f, 0.42f, 0.26f};
    MeshData m;
    auto rnd = [&](u32 s) {
        u32 v = (static_cast<u32>(variant) * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    f32 cr = 0.7f;
    if (variant % kMonumentVariants == 0) {
        // carved obelisk on a stepped plinth
        add_box(m, {-0.85f, 0.0f, -0.85f}, {0.85f, 0.28f, 0.85f}, stone * 0.95f);
        add_box(m, {-0.62f, 0.28f, -0.62f}, {0.62f, 0.52f, 0.62f}, stone);
        add_box(m, {-0.34f, 0.52f, -0.34f}, {0.34f, 3.6f, 0.34f}, stone * 1.04f); // shaft
        add_box(m, {-0.36f, 1.3f, -0.36f}, {0.36f, 1.5f, 0.36f}, dark);           // carved band
        add_box(m, {-0.36f, 2.4f, -0.36f}, {0.36f, 2.6f, 0.36f}, dark);
        // a small pyramidal cap
        const Vec3 apex{0.0f, 4.05f, 0.0f};
        add_tri(m, {-0.34f, 3.6f, 0.34f}, {0.34f, 3.6f, 0.34f}, apex, stone * 1.06f);
        add_tri(m, {0.34f, 3.6f, -0.34f}, {-0.34f, 3.6f, -0.34f}, apex, stone * 0.96f);
        add_tri(m, {0.34f, 3.6f, 0.34f}, {0.34f, 3.6f, -0.34f}, apex, stone);
        add_tri(m, {-0.34f, 3.6f, -0.34f}, {-0.34f, 3.6f, 0.34f}, apex, stone);
        add_box(m, {-0.36f, 0.5f, -0.36f}, {0.0f, 0.9f, -0.32f}, moss); // moss patch
        cr = 0.55f;
    } else if (variant % kMonumentVariants == 1) {
        // a broken, leaning pillar + rubble at the base
        add_box(m, {-0.7f, 0.0f, -0.7f}, {0.7f, 0.22f, 0.7f}, stone * 0.95f);
        add_box(m, {-0.34f, 0.22f, -0.34f}, {0.34f, 1.7f, 0.34f}, stone); // standing stub
        add_box(m, {-0.3f, 1.55f, -0.3f}, {0.3f, 1.75f, 0.3f}, dark);     // jagged broken top
        // a fallen broken section lying beside it
        add_box(m, {0.5f, 0.0f, -0.25f}, {1.7f, 0.4f, 0.25f}, stone * 1.02f);
        add_box(m, {-0.55f, 0.0f, 0.5f}, {-0.1f, 0.26f, 0.95f}, dark);    // rubble
        add_box(m, {-0.34f, 0.6f, 0.3f}, {0.0f, 0.95f, 0.36f}, moss);     // moss
        cr = 0.6f;
    } else {
        // a trio of rough standing stones in a loose ring
        for (int i = 0; i < 3; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / 3.0f + rnd(i) * 0.5f;
            const f32 sx = std::cos(a) * 0.55f, sz = std::sin(a) * 0.55f;
            const f32 hgt = 1.5f + rnd(i * 3 + 1) * 1.1f;
            const f32 w = 0.32f + rnd(i * 3 + 2) * 0.16f;
            const f32 lean = (rnd(i * 3 + 3) - 0.5f) * 0.3f;
            add_box(m, {sx - w + lean, 0.0f, sz - w}, {sx + w + lean, hgt, sz + w}, stone * (0.92f + 0.16f * rnd(i)));
            add_box(m, {sx - w * 0.5f + lean, hgt * 0.5f, sz - w - 0.02f}, {sx + lean, hgt * 0.7f, sz - w + 0.04f}, moss);
        }
        cr = 0.95f;
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{cr, cr};
    c.height = 1.6f;
    def.colliders.push_back(c);
    return def;
}

// A wooden WATCHTOWER: four braced legs carrying a railed platform with a small pitched-roof lookout
// cabin on top - a wilderness landmark you can see from afar (a brazier could light it at night).
PropDef PropLibrary::build_watchtower() {
    PropDef def;
    def.name = "watchtower";
    const Vec3 wood{0.42f, 0.3f, 0.18f};
    const Vec3 dark{0.32f, 0.22f, 0.13f};
    const Vec3 roof{0.46f, 0.27f, 0.17f};
    MeshData m;
    constexpr f32 r = 1.0f;   // leg spread (half)
    constexpr f32 ph = 3.4f;  // platform height
    // four legs, splayed slightly outward at the base
    for (f32 sx : {-1.0f, 1.0f}) {
        for (f32 sz : {-1.0f, 1.0f}) {
            const f32 bx = sx * (r + 0.35f), bz = sz * (r + 0.35f);
            const f32 tx = sx * r, tz = sz * r;
            // a leaning leg approximated by a thin box from base to platform (axis-aligned-ish)
            add_box(m, {std::min(bx, tx) - 0.1f, 0.0f, std::min(bz, tz) - 0.1f},
                    {std::min(bx, tx) + 0.1f, ph, std::min(bz, tz) + 0.1f}, wood);
        }
    }
    // cross-braces (a couple of diagonal-ish horizontal rings)
    for (f32 by : {1.1f, 2.2f}) {
        add_box(m, {-r - 0.1f, by, -r - 0.05f}, {r + 0.1f, by + 0.12f, -r + 0.05f}, dark);
        add_box(m, {-r - 0.1f, by, r - 0.05f}, {r + 0.1f, by + 0.12f, r + 0.05f}, dark);
        add_box(m, {-r - 0.05f, by, -r - 0.1f}, {-r + 0.05f, by + 0.12f, r + 0.1f}, dark);
        add_box(m, {r - 0.05f, by, -r - 0.1f}, {r + 0.05f, by + 0.12f, r + 0.1f}, dark);
    }
    // platform deck + a railing
    add_box(m, {-r - 0.25f, ph, -r - 0.25f}, {r + 0.25f, ph + 0.14f, r + 0.25f}, wood * 1.05f);
    for (f32 sz : {-1.0f, 1.0f}) {
        add_box(m, {-r - 0.25f, ph + 0.14f, sz * (r + 0.2f) - 0.05f}, {r + 0.25f, ph + 0.7f, sz * (r + 0.2f) + 0.05f}, wood);
    }
    add_box(m, {-r - 0.25f, ph + 0.14f, -r - 0.25f}, {-r - 0.15f, ph + 0.7f, r + 0.25f}, wood);
    add_box(m, {r + 0.15f, ph + 0.14f, -r - 0.25f}, {r + 0.25f, ph + 0.7f, r + 0.25f}, wood);
    // a small lookout cabin (back wall + posts) with a pitched roof
    add_box(m, {-r, ph + 0.14f, -r}, {r, ph + 1.6f, -r + 0.16f}, wood * 0.95f); // back wall
    add_box(m, {-r, ph + 0.14f, -r}, {-r + 0.14f, ph + 1.6f, r * 0.4f}, wood);  // side posts
    add_box(m, {r - 0.14f, ph + 0.14f, -r}, {r, ph + 1.6f, r * 0.4f}, wood);
    const f32 ry = ph + 1.6f;
    add_tri(m, {-r - 0.2f, ry, r + 0.2f}, {r + 0.2f, ry, r + 0.2f}, {0.0f, ry + 0.7f, -r * 0.3f}, roof);
    add_tri(m, {r + 0.2f, ry, -r - 0.2f}, {-r - 0.2f, ry, -r - 0.2f}, {0.0f, ry + 0.7f, -r * 0.3f}, roof * 0.92f);
    add_tri(m, {r + 0.2f, ry, r + 0.2f}, {r + 0.2f, ry, -r - 0.2f}, {0.0f, ry + 0.7f, -r * 0.3f}, roof);
    add_tri(m, {-r - 0.2f, ry, -r - 0.2f}, {-r - 0.2f, ry, r + 0.2f}, {0.0f, ry + 0.7f, -r * 0.3f}, roof);
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    // colliders on the four legs (the bay between them is walkable)
    for (f32 sx : {-1.0f, 1.0f}) {
        for (f32 sz : {-1.0f, 1.0f}) {
            BoxCollider c;
            c.center = Vec3{sx * r, 0.0f, sz * r};
            c.half_extents = Vec2{0.22f, 0.22f};
            c.height = ph;
            def.colliders.push_back(c);
        }
    }
    return def;
}

// An arched stone road bridge spanning a river (local +X = the road across the channel). A
// gently arched stone deck wide enough for the cart, with low parapets and stone abutments;
// the arch springs from the banks. Placed where a town avenue crosses the river.
PropDef PropLibrary::build_stone_bridge() {
    PropDef def;
    def.name = "stone_bridge";
    const Vec3 stone{0.54f, 0.53f, 0.50f};
    const Vec3 dark{0.40f, 0.39f, 0.37f};
    MeshData m;
    constexpr f32 hw = 1.9f; // half-width (across the road, local z) - a cart fits
    constexpr int seg = 7;   // deck segments arching across
    constexpr f32 span = 4.6f; // half-span along the road (local x)
    f32 prev_x = -span;
    f32 prev_y = 0.0f;
    auto arch_y = [&](f32 t) { return 0.9f * std::sin(t * Pi); }; // 0..1 -> arch height
    for (int i = 0; i <= seg; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(seg);
        const f32 x = glm::mix(-span, span, t);
        const f32 y = arch_y(t);
        if (i > 0) {
            // deck plank between prev and current (a sloped box)
            const f32 x0 = prev_x, x1 = x;
            const f32 y0 = prev_y, y1 = y;
            add_quad(m, {x0, y0 + 0.5f, -hw}, {x1, y1 + 0.5f, -hw}, {x1, y1 + 0.5f, hw}, {x0, y0 + 0.5f, hw},
                     stone); // deck top
            add_box(m, {std::min(x0, x1), -0.1f, -hw}, {std::max(x0, x1), std::min(y0, y1) + 0.5f, hw},
                    stone * 0.96f); // deck body down to the water
            // parapets
            for (f32 s : {-1.0f, 1.0f}) {
                add_quad(m, {x0, y0 + 0.5f, s * hw}, {x1, y1 + 0.5f, s * hw},
                         {x1, y1 + 0.95f, s * hw}, {x0, y0 + 0.95f, s * hw}, dark);
            }
        }
        prev_x = x;
        prev_y = y;
    }
    // Stone abutments at each bank end.
    for (f32 s : {-1.0f, 1.0f}) {
        add_box(m, {s * span - 0.4f, -0.2f, -hw - 0.1f}, {s * span + 0.4f, 0.5f, hw + 0.1f}, stone * 0.92f);
    }
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    // The deck blocks nothing (you drive over it); the parapets are thin - no collider needed.
    return def;
}

PropLibrary::PropLibrary() {
    for (int i = 0; i < 3; ++i) {
        bushes_.push_back(build_bush(i));
    }
    for (int i = 0; i < 3; ++i) {
        rocks_.push_back(build_rock(i));
    }
    for (int i = 0; i < 3; ++i) {
        logs_.push_back(build_log(i));
    }
    for (int i = 0; i < 2; ++i) {
        fences_.push_back(build_fence(i));
        fence_rails_.push_back(build_fence_rail(i));
    }
    lanterns_.push_back(build_lantern_post());
    for (u32 i = 0; i < kHouseDefs; ++i) {
        houses_.push_back(build_house(i));
    }
    for (int i = 0; i < 2; ++i) {
        walls_.push_back(build_wall(i));
    }
    gates_.push_back(build_gate());  // variant 0: lit gate tower
    gates_.push_back(build_tower()); // variant 1: plain unlit wall tower
    wells_.push_back(build_well());
    bridges_.push_back(build_bridge());       // variant 0: covered walkway between houses
    bridges_.push_back(build_stone_bridge()); // variant 1: arched stone road bridge over a river
    markets_.push_back(build_market());
    paths_.push_back(build_path_tile());
    planters_.push_back(build_planter());
    fountains_.push_back(build_fountain());
    for (u32 i = 0; i < kDecorVariants; ++i) {
        decor_.push_back(build_decor(static_cast<int>(i)));
    }
    rivers_.push_back(build_river());
    for (u32 i = 0; i < kCrystalVariants; ++i) {
        crystals_.push_back(build_crystal(static_cast<int>(i)));
    }
    for (u32 i = 0; i < kGlowShroomVariants; ++i) {
        glow_shrooms_.push_back(build_glow_shroom(static_cast<int>(i)));
    }
    campfires_.push_back(build_campfire());
    for (u32 i = 0; i < kMonumentVariants; ++i) {
        monuments_.push_back(build_monument(static_cast<int>(i)));
    }
    watchtowers_.push_back(build_watchtower());
}

const PropDef& PropLibrary::resolve(const PropInstance& inst) const {
    switch (inst.category) {
        case PropCategory::Bush: return bushes_[inst.variant % bushes_.size()];
        case PropCategory::Rock: return rocks_[inst.variant % rocks_.size()];
        case PropCategory::Log: return logs_[inst.variant % logs_.size()];
        case PropCategory::Fence: return fences_[inst.variant % fences_.size()];
        case PropCategory::FenceRail: return fence_rails_[inst.variant % fence_rails_.size()];
        case PropCategory::Lantern: return lanterns_[inst.variant % lanterns_.size()];
        case PropCategory::House: return houses_[inst.variant % houses_.size()];
        case PropCategory::Wall: return walls_[inst.variant % walls_.size()];
        case PropCategory::Gate: return gates_[inst.variant % gates_.size()];
        case PropCategory::Well: return wells_[inst.variant % wells_.size()];
        case PropCategory::Bridge: return bridges_[inst.variant % bridges_.size()];
        case PropCategory::Market: return markets_[inst.variant % markets_.size()];
        case PropCategory::Path: return paths_[inst.variant % paths_.size()];
        case PropCategory::Planter: return planters_[inst.variant % planters_.size()];
        case PropCategory::Fountain: return fountains_[inst.variant % fountains_.size()];
        case PropCategory::Decor: return decor_[inst.variant % decor_.size()];
        case PropCategory::River: return rivers_[inst.variant % rivers_.size()];
        case PropCategory::Crystal: return crystals_[inst.variant % crystals_.size()];
        case PropCategory::GlowShroom: return glow_shrooms_[inst.variant % glow_shrooms_.size()];
        case PropCategory::Campfire: return campfires_[inst.variant % campfires_.size()];
        case PropCategory::Monument: return monuments_[inst.variant % monuments_.size()];
        case PropCategory::Watchtower: return watchtowers_[inst.variant % watchtowers_.size()];
    }
    return bushes_[0];
}

} // namespace alryn
