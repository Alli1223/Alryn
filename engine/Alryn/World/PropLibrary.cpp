#include <Alryn/World/PropLibrary.h>

#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Terrain/RoadNetwork.h> // shared bridge deck geometry (build_plank_bridge)

#include <algorithm>
#include <utility>

namespace alryn {

namespace {
void make_snowy_house(PropDef& def); // (defined further down, with the other snow helpers)
void add_mesh(MeshData& dst, const MeshData& src, const Mat4& xf, const Vec3& color);
} // namespace

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

    // About half the windows carry a FLOWER BOX on the sill: a little planked trough proud of the
    // wall, spilling greenery and a mix of bright blooms - the lived-in, storybook-town touch.
    const u32 wseed = static_cast<u32>(std::lround(ctr.x * 37.0f) * 73856093) ^
                      static_cast<u32>(std::lround(ctr.y * 41.0f) * 19349663) ^
                      static_cast<u32>(std::lround(ctr.z * 43.0f) * 83492791);
    if (hashf(wseed) < 0.55f) {
        static const Vec3 blooms[5] = {{0.78f, 0.16f, 0.18f}, {0.86f, 0.70f, 0.18f}, {0.58f, 0.30f, 0.70f},
                                       {0.82f, 0.80f, 0.74f}, {0.86f, 0.40f, 0.56f}};
        const Vec3 box_col{0.40f, 0.27f, 0.15f};
        const Vec3 leaf{0.26f, 0.48f, 0.20f};
        const f32 bw = hw + fb + 0.04f;   // half-width along the wall
        const f32 by0 = -hh - fb - 0.2f;  // under the sill
        const f32 by1 = -hh - fb - 0.02f;
        const f32 bo = 0.24f;             // how far it stands out from the wall
        auto boxq = [&](f32 c0, f32 c1, f32 u0, f32 u1, f32 o0, f32 o1, const Vec3& col) {
            // An axis-free little box in the window's (across, up, outward) frame.
            const Vec3 p000 = ctr + across * c0 + up * u0 + outward * o0;
            const Vec3 a = across * (c1 - c0), u = up * (u1 - u0), o = outward * (o1 - o0);
            const Vec3 mid = p000 + (a + u + o) * 0.5f;
            const Vec3 P[8] = {p000, p000 + a, p000 + a + o, p000 + o, p000 + u, p000 + a + u, p000 + a + o + u, p000 + o + u};
            const int F[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}};
            for (const auto& f : F) {
                emit_tri(op, P[f[0]], P[f[1]], P[f[2]], mid, col);
                emit_tri(op, P[f[0]], P[f[2]], P[f[3]], mid, col);
            }
        };
        boxq(-bw, bw, by0, by1, 0.02f, bo, box_col);                        // the trough
        boxq(-bw + 0.04f, bw - 0.04f, by1, by1 + 0.07f, 0.06f, bo - 0.04f, leaf); // a bed of leaves
        const int n = std::max(3, static_cast<int>(bw / 0.11f));
        for (int i = 0; i < n; ++i) {
            const f32 t = -bw + 0.08f + (2.0f * bw - 0.16f) * (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
            const f32 ht = 0.08f + 0.1f * hashf(wseed + static_cast<u32>(i) * 7u);
            const f32 oo = 0.08f + 0.1f * hashf(wseed + static_cast<u32>(i) * 13u);
            boxq(t - 0.035f, t + 0.035f, by1, by1 + ht, oo, oo + 0.06f, leaf * 0.9f); // stem + leaves
            const Vec3 bc = blooms[(wseed / 7u + static_cast<u32>(i) * 3u) % 5u];
            boxq(t - 0.05f, t + 0.05f, by1 + ht, by1 + ht + 0.07f, oo - 0.01f, oo + 0.08f, bc); // the bloom
        }
    }
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
            // (P swaps x and z for a ridge along z - a mirror - which flips the cross product's sense.)
            const Vec3 n = glm::normalize(glm::cross(pt(s1, 0.0f) - pt(s0, 0.0f), pt(s0, 1.0f) - pt(s0, 0.0f))) * dir *
                           (gx ? 1.0f : -1.0f);
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
// A belled wooden barrel centred at `c`: octagonal staves bowing out at mid-height (a sin profile),
// per-stave shade, an octagonal lid + two proud octagonal iron hoops. Shared by the yard decor helper
// and the standalone barrel decor prop, so a round belled barrel looks the same everywhere.
void belled_barrel(MeshData& m, const Vec3& c, f32 r, f32 ht, const Vec3& stave, const Vec3& hoop) {
    constexpr int sides = 8;
    auto ang = [](int s) { return TwoPi * (static_cast<f32>(s) + 0.5f) / static_cast<f32>(sides); };
    auto pt = [&](f32 rr, f32 y, f32 a) {
        return Vec3{c.x + std::cos(a) * rr, c.y + y, c.z + std::sin(a) * rr};
    };
    auto rad = [&](f32 t) { return r * (0.82f + 0.28f * std::sin(t * Pi)); }; // narrow ends, wide middle
    const Vec3 axis{c.x, c.y + ht * 0.5f, c.z};
    constexpr int rings = 4;
    for (int s = 0; s < sides; ++s) {
        const f32 a0 = ang(s), a1 = ang(s + 1);
        const Vec3 sc = stave * (0.88f + 0.20f * hashf(static_cast<u32>(s) * 5u + 1u));
        for (int rr = 0; rr < rings; ++rr) {
            const f32 t0 = static_cast<f32>(rr) / rings, t1 = static_cast<f32>(rr + 1) / rings;
            const Vec3 b0 = pt(rad(t0), t0 * ht, a0), b1 = pt(rad(t0), t0 * ht, a1);
            const Vec3 u0 = pt(rad(t1), t1 * ht, a0), u1 = pt(rad(t1), t1 * ht, a1);
            emit_tri(m, b0, b1, u1, axis, sc);
            emit_tri(m, b0, u1, u0, axis, sc);
        }
        emit_tri(m, Vec3{c.x, c.y + ht, c.z}, pt(rad(1.0f) * 0.96f, ht, a0),
                 pt(rad(1.0f) * 0.96f, ht, a1), Vec3{c.x, c.y - 1.0f, c.z}, stave * 0.95f); // lid fan
    }
    for (const f32 t : {0.22f, 0.78f}) {
        const f32 y = t * ht, rr = rad(t) + r * 0.06f;
        for (int s = 0; s < sides; ++s) {
            const f32 a0 = ang(s), a1 = ang(s + 1);
            emit_tri(m, pt(rr, y - ht * 0.06f, a0), pt(rr, y - ht * 0.06f, a1), pt(rr, y + ht * 0.06f, a1),
                     axis, hoop);
            emit_tri(m, pt(rr, y - ht * 0.06f, a0), pt(rr, y + ht * 0.06f, a1), pt(rr, y + ht * 0.06f, a0),
                     axis, hoop);
        }
    }
}
// A low-poly belled barrel for house/pub/blacksmith yards (sits on the ground at `base`).
void add_barrel(MeshData& m, const Vec3& base, f32 r, f32 ht) {
    belled_barrel(m, base, r, ht, Vec3{0.46f, 0.30f, 0.16f}, Vec3{0.18f, 0.16f, 0.15f});
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
// An oriented square-section beam from `a` to `b` (half-thickness `r`) - ropes, poles, braces that
// don't run along an axis.
void add_beam(MeshData& m, const Vec3& a, const Vec3& b, f32 r, const Vec3& color) {
    const Vec3 d = b - a;
    const f32 len = glm::length(d);
    if (len < 1e-4f) {
        return;
    }
    const Vec3 f = d / len;
    const Vec3 ref = std::abs(f.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 s = glm::normalize(glm::cross(f, ref)) * r;
    const Vec3 u = glm::normalize(glm::cross(s, f)) * r;
    const Vec3 P[8] = {a - s - u, a + s - u, a + s + u, a - s + u, b - s - u, b + s - u, b + s + u, b - s + u};
    const Vec3 mid = (a + b) * 0.5f;
    const int F[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (const auto& q : F) {
        emit_tri(m, P[q[0]], P[q[1]], P[q[2]], mid, color);
        emit_tri(m, P[q[0]], P[q[2]], P[q[3]], mid, color);
    }
}

// A string of pennant BUNTING from `a` to `b`, sagging `sag` metres in the middle: a dark cord with
// double-sided triangular flags in festive colours hanging off it every ~0.55 m.
void add_bunting(MeshData& m, const Vec3& a, const Vec3& b, f32 sag) {
    static const Vec3 flags[5] = {{0.70f, 0.14f, 0.14f}, {0.84f, 0.68f, 0.18f}, {0.18f, 0.32f, 0.66f},
                                  {0.20f, 0.50f, 0.22f}, {0.82f, 0.80f, 0.74f}};
    const Vec3 cord{0.2f, 0.15f, 0.1f};
    auto at = [&](f32 t) { return glm::mix(a, b, t) - Vec3{0.0f, sag * (1.0f - (2.0f * t - 1.0f) * (2.0f * t - 1.0f)), 0.0f}; };
    constexpr int segs = 10;
    for (int i = 0; i < segs; ++i) {
        add_beam(m, at(static_cast<f32>(i) / segs), at(static_cast<f32>(i + 1) / segs), 0.016f, cord);
    }
    const f32 len = glm::length(b - a);
    const int n = std::max(3, static_cast<int>(len / 0.55f));
    Vec3 side = glm::cross(glm::normalize(b - a), Vec3{0.0f, 1.0f, 0.0f});
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : Vec3{0.0f, 0.0f, 1.0f};
    const f32 hw = 0.4f * len / static_cast<f32>(n) * 0.5f / std::max(len, 1e-3f); // half-width in t
    for (int i = 0; i < n; ++i) {
        const f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(n);
        const Vec3 p0 = at(t - hw * 1.6f), p1 = at(t + hw * 1.6f);
        const Vec3 tip = at(t) - Vec3{0.0f, 0.36f, 0.0f};
        const Vec3 col = flags[static_cast<u32>(i) % 5u];
        const Vec3 c = (p0 + p1 + tip) / 3.0f;
        emit_tri(m, p0, p1, tip, c - side, col);        // both faces: it's cloth
        emit_tri(m, p0, p1, tip, c + side, col * 0.9f);
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
} // namespace

// A single fence POST (a stout little pillar with a chamfer cap). Rails connect one post
// to the next as a separate, length-stretched prop (build_fence_rail), so a run of posts
// is joined by rails of whatever length the gap happens to be.
PropDef PropLibrary::build_fence(int variant) {
    PropDef def;
    def.name = "fence_post";
    const Vec3 wood = variant % 2 == 0 ? Vec3{0.42f, 0.30f, 0.18f} : Vec3{0.36f, 0.26f, 0.16f};
    MeshData m;
    add_box(m, {-0.075f, 0.0f, -0.075f}, {0.075f, 0.92f, 0.075f}, wood);            // post
    add_box(m, {-0.095f, 0.88f, -0.095f}, {0.095f, 0.95f, 0.095f}, wood * 0.86f);   // chamfer collar
    // a pointed pyramidal cap (a weather-shedding fence-post top, not a flat block)
    const Vec3 apex{0.0f, 1.13f, 0.0f}, ctr{0.0f, 0.95f, 0.0f};
    constexpr f32 cr = 0.095f;
    const Vec3 q0{-cr, 0.95f, -cr}, q1{cr, 0.95f, -cr}, q2{cr, 0.95f, cr}, q3{-cr, 0.95f, cr};
    emit_tri(m, q0, q1, apex, ctr, wood);
    emit_tri(m, q1, q2, apex, ctr, wood);
    emit_tri(m, q2, q3, apex, ctr, wood);
    emit_tri(m, q3, q0, apex, ctr, wood);
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
    add_box(op, {-0.055f, 0.0f, -0.055f}, {0.055f, 1.42f, 0.055f}, wood);      // post
    add_box(op, {-0.10f, 0.0f, -0.10f}, {0.10f, 0.12f, 0.10f}, wood * 0.85f);  // base block
    add_box(op, {-0.13f, 1.40f, -0.13f}, {0.13f, 1.46f, 0.13f}, frame);        // lantern floor plate
    add_box(em, {-0.085f, 1.47f, -0.085f}, {0.085f, 1.76f, 0.085f}, glow);     // glowing glass panes
    // Four metal corner posts caging the glass.
    for (const f32 sx : {-0.10f, 0.10f}) {
        for (const f32 sz : {-0.10f, 0.10f}) {
            add_box(op, {sx - 0.018f, 1.44f, sz - 0.018f}, {sx + 0.018f, 1.79f, sz + 0.018f}, frame);
        }
    }
    // Peaked roof cap (four faces to an apex) + a finial spike, instead of a flat lid.
    const Vec3 apex{0.0f, 1.98f, 0.0f};
    const Vec3 cap_c{0.0f, 1.82f, 0.0f}; // centre, so emit_tri orients the roof faces outward
    constexpr f32 cr = 0.155f;
    const Vec3 r0{-cr, 1.77f, -cr}, r1{cr, 1.77f, -cr}, r2{cr, 1.77f, cr}, r3{-cr, 1.77f, cr};
    emit_tri(op, r0, r1, apex, cap_c, frame);
    emit_tri(op, r1, r2, apex, cap_c, frame);
    emit_tri(op, r2, r3, apex, cap_c, frame);
    emit_tri(op, r3, r0, apex, cap_c, frame);
    add_box(op, {-0.025f, 1.96f, -0.025f}, {0.025f, 2.06f, 0.025f}, frame); // finial spike
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    PropLight l;
    l.offset = Vec3{0.0f, 1.6f, 0.0f};
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
// Shared medieval palette for the special buildings (townhouse / pub / blacksmith / bakery / shop).
const Vec3 kDaub{0.96f, 0.89f, 0.70f};   // warm lime-washed daub infill
const Vec3 kFrame{0.23f, 0.14f, 0.08f};  // dark exposed oak timber
const Vec3 kStone{0.62f, 0.62f, 0.64f};  // light grey fieldstone
const Vec3 kTrim{0.20f, 0.13f, 0.09f};   // dark roof trim
const Vec3 kRoofBrown{0.58f, 0.32f, 0.17f}; // rich warm brown shingle
const Vec3 kGlow{1.0f, 0.82f, 0.42f};    // warm lit window
const Vec3 kWoodDk{0.38f, 0.26f, 0.15f};

// A wall-mounted lantern: a bracket, a glass box (emissive) and a warm spot light. Adds to op + em,
// and pushes a PropLight (an outdoor one - it hangs outside the walls). `at` is the glass centre;
// `outward` the wall normal it hangs off.
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
    l.spill = true;
    def.lights.push_back(l);
}

// A box collider over [lo, hi] (prop-local) that blocks from lo.y up to hi.y - so an upper storey's
// wall only blocks up there, not across the doorway beneath it.
void add_collider(PropDef& def, const Vec3& lo, const Vec3& hi) {
    BoxCollider c;
    c.center = Vec3{(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
    c.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
    c.height = hi.y - lo.y;
    def.colliders.push_back(c);
}

// A planked door leaf (vertical boards, two battens, an iron handle by its free edge), modelled SHUT
// across the doorway from x0 (its hinge side) to x1 on the front face z = fz, as its own animated Door
// part that swings INTO the building (-z) as it opens (or `outward`). The frame round it is the caller's.
void add_door_leaf(PropDef& def, f32 x0, f32 x1, f32 fz, f32 dh, const Vec3& wood, bool outward = false) {
    MeshData leaf;
    const f32 lo = std::min(x0, x1), hi = std::max(x0, x1);
    const int planks = std::max(2, static_cast<int>(std::round((hi - lo) / 0.27f)));
    for (int i = 0; i < planks; ++i) {
        const f32 a = glm::mix(lo + 0.02f, hi - 0.02f, static_cast<f32>(i) / static_cast<f32>(planks));
        const f32 b = glm::mix(lo + 0.02f, hi - 0.02f, static_cast<f32>(i + 1) / static_cast<f32>(planks)) - 0.015f;
        add_box(leaf, {a, 0.03f, fz - 0.04f}, {b, dh - 0.03f, fz + 0.04f}, wood * (0.88f + 0.14f * static_cast<f32>(i % 2)));
    }
    add_box(leaf, {lo + 0.03f, 0.34f, fz + 0.035f}, {hi - 0.03f, 0.49f, fz + 0.075f}, wood * 0.7f);      // lower batten
    add_box(leaf, {lo + 0.03f, dh - 0.55f, fz + 0.035f}, {hi - 0.03f, dh - 0.4f, fz + 0.075f}, wood * 0.7f); // upper batten
    const f32 hx = x1 > x0 ? hi - 0.17f : lo + 0.07f;
    add_box(leaf, {hx, 0.98f, fz + 0.06f}, {hx + 0.1f, 1.12f, fz + 0.12f}, Vec3{0.12f, 0.12f, 0.13f}); // handle
    PropPart part;
    part.mesh = std::move(leaf);
    part.layer = PropLayer::Door;
    part.hinge = Vec3{x0, 0.0f, fz};
    part.swing = (x1 > x0 ? 1.0f : -1.0f) * (outward ? -1.62f : 1.62f); // ~93 degrees, in (or out)
    def.parts.push_back(std::move(part));
}

// A doorway's proud timber frame (jambs + lintel) on the front face z = fz.
void add_door_frame(MeshData& op, f32 dw, f32 dh, f32 fz, const Vec3& frame) {
    add_box(op, {-dw - 0.1f, 0.0f, fz - 0.06f}, {-dw, dh + 0.1f, fz + 0.12f}, frame);
    add_box(op, {dw, 0.0f, fz - 0.06f}, {dw + 0.1f, dh + 0.1f, fz + 0.12f}, frame);
    add_box(op, {-dw - 0.1f, dh, fz - 0.06f}, {dw + 0.1f, dh + 0.1f, fz + 0.12f}, frame);
}

// A quad with its own colour at each corner, drawn double-sided (an additive glow sheet).
void glow_quad(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& ca, const Vec3& cb,
               const Vec3& cc, const Vec3& cd) {
    Vec3 n = glm::cross(b - a, c - a);
    n = glm::length(n) > 1e-6f ? glm::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, ca, 0.0f});
    m.vertices.push_back({b, n, cb, 0.0f});
    m.vertices.push_back({c, n, cc, 0.0f});
    m.vertices.push_back({d, n, cd, 0.0f});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base,   // front
                                       base, base + 2, base + 1, base + 2, base, base + 3}); // back
}

// The warm light of a lit window spilling OUT of the building after dark: a soft additive shaft from
// the sill down to the ground beneath and a pool of light on the ground there (the Glow part - drawn
// only at night), and (`light`) a real unshadowed spot just outside, so the ground + whoever's standing
// there are lit. `win` is the window centre on the OUTER wall face, `sill` its bottom edge's height.
void add_window_spill(PropDef& def, MeshData& glow, const Vec3& win, const Vec3& across, const Vec3& outward, f32 hw,
                      f32 sill, bool light) {
    const Vec3 warm{1.0f, 0.62f, 0.3f};
    const Vec3 none{0.0f};
    const f32 reach = 1.7f + sill * 0.35f;
    const Vec3 foot = Vec3{win.x, 0.06f, win.z} + outward * 0.1f;
    const Vec3 nl = foot - across * (hw * 1.15f), nr = foot + across * (hw * 1.15f);
    const Vec3 fl = foot + outward * reach - across * (hw * 2.0f), fr = foot + outward * reach + across * (hw * 2.0f);
    glow_quad(glow, nl, nr, fr, fl, warm * 0.32f, warm * 0.32f, none, none); // the pool on the ground
    const Vec3 sl = Vec3{win.x, sill, win.z} + outward * 0.08f - across * hw;
    const Vec3 sr = Vec3{win.x, sill, win.z} + outward * 0.08f + across * hw;
    glow_quad(glow, sl, sr, fr, fl, warm * 0.15f, warm * 0.15f, none, none); // the shaft down to it
    if (light) {
        PropLight l;
        l.offset = Vec3{win.x, sill + 0.1f, win.z} + outward * 0.45f;
        l.direction = glm::normalize(outward * 0.85f + Vec3{0.0f, -1.0f, 0.0f});
        l.color = Vec3{1.0f, 0.7f, 0.4f};
        l.range = 6.5f;
        l.intensity = 1.35f;
        l.cone_deg = 125.0f;
        l.spill = true;
        def.lights.push_back(l);
    }
}

// How a home's ground floor is furnished.
enum class Furnish : u8 {
    Cottage, // a hearth, a table + candle, the resident's bed
    Tavern,  // a hearth, a bar counter with casks behind it, tables + stools with tankards
    Bakery,  // a great domed bread oven, a kneading table heaped with loaves, flour sacks, bread shelves
    Shop,    // shelves of goods along the walls, a counter by the door
};

// A medieval home's build: half-extents (w,d) of its ground floor, per-storey height, storey count,
// gable rise, a material flavour (0 wattle-and-daub, 1 stone, 2 dark timber, 3 a stone ground floor
// under daub + timber), `thatch` for a golden straw roof (else stepped shingles), `jetty` - each upper
// storey overhangs the one below by this - and how it's furnished. Varying these gives small/large,
// squat/tall, one- to three-storey homes.
struct HomeSpec {
    f32 w, d, story_h;
    int stories;
    f32 roof_rise;
    int material;
    bool thatch;
    f32 jetty = 0.0f;
    Furnish furnish = Furnish::Cottage;
};
constexpr HomeSpec kHouseStyles[kHouseVariants] = {
    {3.2f, 2.8f, 2.4f, 1, 1.9f, 0, true},                // classic thatched cottage
    {4.7f, 2.6f, 2.3f, 1, 1.4f, 0, false},               // long house (wide, shingled)
    {2.7f, 2.7f, 2.3f, 2, 1.1f, 1, false},               // stone townhouse, two storeys
    {3.0f, 3.0f, 2.4f, 2, 1.6f, 2, false},               // timber two-storey, shingled
    {4.1f, 3.4f, 2.6f, 1, 2.1f, 0, false},               // manor (big, steep tiled roof)
    {2.5f, 2.3f, 2.2f, 1, 1.7f, 1, true},                // small stone hut, thatched
    {3.3f, 2.5f, 2.3f, 2, 1.3f, 0, false},               // tall narrow cottage, shingled
    {3.8f, 2.9f, 2.4f, 1, 1.9f, 2, true},                // timber-framed hall, thatched
    {3.3f, 2.6f, 2.4f, 2, 1.5f, 3, false, 0.3f},         // jettied merchant's house over a stone ground floor
    {2.4f, 2.2f, 2.2f, 1, 1.6f, 0, true},                // a tiny thatched croft
    {4.4f, 3.0f, 2.5f, 2, 1.7f, 1, false},               // a big stone farmhouse
    {3.5f, 2.6f, 2.3f, 2, 1.5f, 2, true, 0.25f},         // jettied timber house, thatched
    {5.2f, 2.8f, 2.4f, 1, 1.6f, 3, false},               // a stone + timber longhall
    {2.8f, 2.6f, 2.25f, 3, 1.3f, 3, false, 0.2f},        // a tall three-storey jettied house
};
// The special buildings that are homes underneath (their extras are added on top).
constexpr HomeSpec kTownhouseSpec{2.0f, 1.95f, 2.3f, 3, 1.8f, 3, false, 0.2f};
constexpr HomeSpec kPubSpec{3.0f, 2.6f, 2.4f, 2, 1.9f, 3, false, 0.0f, Furnish::Tavern};
constexpr HomeSpec kBakerySpec{3.1f, 2.6f, 2.4f, 1, 1.7f, 1, true, 0.0f, Furnish::Bakery};
constexpr HomeSpec kShopSpec{3.0f, 2.6f, 2.3f, 2, 1.5f, 3, false, 0.25f, Furnish::Shop};

// A storey's half-extents (an upper storey jetties out over the one below, up to two steps).
Vec2 storey_extents(const HomeSpec& st, int s) {
    const f32 j = st.jetty * static_cast<f32>(std::min(s, 2));
    return Vec2{st.w + j, st.d + j};
}
Vec2 home_extents(const HomeSpec& st) { return storey_extents(st, st.stories - 1); }

// A projecting front-gable bay off the front-left of the house (lower than the main ridge) - gives the
// house a cross-gable / T-shaped "stance". Kept clear of the doorway (|x| <= dw); its walls collide.
// Adds to `shell` (the fade roof shell), with stone footings into `op`.
void add_front_gable(PropDef& def, MeshData& shell, MeshData& op, MeshData& em, MeshData& glow, const Vec3& wall_col,
                     const Vec3& roof, const Vec3& trim, const Vec3& frame, const Vec3& found, const Vec3& glowc, f32 w, f32 d,
                     f32 h, f32 dw, bool half_timber, bool thatch, u32 seed) {
    const f32 bw = std::min(0.95f, (w - dw - 0.3f) * 0.5f); // bay half-width (x)
    if (bw < 0.55f) {
        return; // too narrow a front for a bay beside the door
    }
    const f32 pd = bw + 0.3f;                  // projection half-depth (z) > bw so the ridge runs z
    const f32 ph = std::min(h - 0.1f, 2.55f);
    const f32 xc = -w + bw + 0.12f;            // tucked into the front-left corner, clear of the door
    const f32 zc = d + pd - 0.12f;
    const f32 zfront = zc + pd;
    // a CLOSED projecting bay: front + two side walls (open at the back onto the house front).
    add_box(shell, {xc - bw, 0.0f, d - 0.1f}, {xc - bw + 0.14f, ph, zfront}, wall_col);  // left side
    add_box(shell, {xc + bw - 0.14f, 0.0f, d - 0.1f}, {xc + bw, ph, zfront}, wall_col);  // right side
    add_box(shell, {xc - bw, 0.0f, zfront - 0.14f}, {xc + bw, ph, zfront}, wall_col);    // front
    add_collider(def, {xc - bw, 0.0f, d}, {xc + bw, ph, zfront});
    // footing - run on down into the ground: the bay reaches past the house's levelled pad, where the
    // ground starts easing back to its natural slope, so no gap opens beneath it
    add_box(op, {xc - bw - 0.05f, -0.6f, d - 0.08f}, {xc + bw + 0.05f, 0.5f, zfront + 0.05f}, found);
    if (half_timber) {
        timber_frame(shell, true, zfront, 1.0f, xc - bw, xc + bw, 0.0f, ph, 0.45f, frame);
        timber_frame(shell, false, xc - bw, -1.0f, d - 0.05f, zfront - 0.05f, 0.0f, ph, 0.0f, frame);
        timber_frame(shell, false, xc + bw, 1.0f, d - 0.05f, zfront - 0.05f, 0.0f, ph, 0.0f, frame);
    }
    lit_window(op, em, {xc, 1.3f, zfront + 0.06f}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 0.36f, 0.42f, glowc, frame);
    add_window_spill(def, glow, {xc, 1.3f, zfront + 0.02f}, {1, 0, 0}, {0, 0, 1}, 0.36f, 0.88f, false);
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
// breaks the box with a lower roof section. Adds a low outer wall + a mono-pitch slope (it collides).
void add_leanto(PropDef& def, MeshData& shell, MeshData& op, const Vec3& wall_col, const Vec3& roof, const Vec3& trim,
                const Vec3& found, f32 w, f32 d, f32 h, f32 side) {
    const f32 lw = 1.35f;           // projection
    const f32 lh = h * 0.55f;       // outer (low) wall height
    const f32 dz = d * 0.78f;       // it runs along most of the side
    const f32 xo = side * (w + lw); // outer wall x
    const f32 xi = side * w;        // inner (against the house) x
    const f32 lo = std::min(xo, xi), hix = std::max(xo, xi);
    add_box(op, {lo - 0.05f, -0.6f, -dz - 0.05f}, {hix + 0.05f, 0.45f, dz + 0.05f}, found);      // footing (into the ground)
    add_box(shell, {std::min(xo, xo - side * 0.14f), 0.0f, -dz}, {std::max(xo, xo - side * 0.14f), lh, dz}, wall_col); // outer wall
    add_box(shell, {lo, 0.0f, -dz - 0.06f}, {hix, lh, -dz}, wall_col);                            // end walls
    add_box(shell, {lo, 0.0f, dz}, {hix, lh, dz + 0.06f}, wall_col);
    add_collider(def, {lo, 0.0f, -dz - 0.06f}, {hix, lh, dz + 0.06f});
    // mono-pitch slope from the main eave (at xi, height h-0.1) down to the outer wall (at xo, lh)
    const Vec3 a{xi, h - 0.1f, -dz - 0.12f}, b2{xo, lh + 0.12f, -dz - 0.12f};
    const Vec3 c2{xo, lh + 0.12f, dz + 0.12f}, dd{xi, h - 0.1f, dz + 0.12f};
    const Vec3 under = (a + c2) * 0.5f - Vec3{0.0f, 1.0f, 0.0f}; // the slope faces up, away from here
    emit_tri(shell, a, b2, c2, under, roof * 0.92f);
    emit_tri(shell, a, c2, dd, under, roof * 0.92f);
    add_box(shell, {std::min(xo, xo - side * 0.16f), lh + 0.05f, -dz - 0.14f}, {std::max(xo, xo - side * 0.16f), lh + 0.2f, dz + 0.14f}, trim); // eave fascia
}

// A medieval home built from `st` (see HomeSpec): real walls with window openings per storey (each
// window glowing at night + spilling its light onto the ground outside), a doorway with a hinged
// planked door that swings open as anyone comes near, floors between the storeys, a stone foundation,
// exposed half-timbering, a swooping shingle / thatch roof with a dormer, a front-gable bay or a
// lean-to for character, the cosy yard dressing, and a ground floor furnished by `st.furnish`. The
// whole shell (walls + roof + floors) fades when you step inside (dollhouse view), and every wall
// collides only over its own storey's height - so the doorway beneath an upper storey stays open.
PropDef build_home(const HomeSpec& st, u32 variant, const char* name) {
    PropDef def;
    def.name = name;

    const f32 w = st.w;          // ground floor half width (x)
    const f32 d = st.d;          // ground floor half depth (z); +z is the front
    const f32 sh = st.story_h;   // per-storey wall height
    const f32 h = sh * static_cast<f32>(st.stories); // total wall height
    const f32 t = 0.18f;         // wall thickness
    const f32 dw = 0.62f;        // front-door half-width (wide enough to walk through comfortably)
    const f32 dh = 1.98f;        // door height
    const f32 rr = st.roof_rise * 1.22f; // gable rise (steepened toward the reference look)
    const f32 oh = 0.62f;                // roof overhang (eaves project past the walls)
    const Vec2 top = home_extents(st);   // the top storey's extents (the roof sits on these)
    // Per-variant massing so houses aren't plain boxes: some cottages get a projecting front-gable bay
    // (front-left), some a lower lean-to to one side. (A jettied front has no room for either.) The
    // windows they cover don't spill light outside.
    const int massing = static_cast<int>(variant % 3u);
    const bool plain = st.jetty <= 0.0f && st.furnish == Furnish::Cottage;
    const bool bay = plain && massing == 0 && (w - dw - 0.3f) * 0.5f >= 0.55f;
    const f32 leanto_side = plain && massing == 1 ? ((variant & 1u) ? 1.0f : -1.0f) : 0.0f;

    auto rnd = [&](u32 s) {
        u32 v = (variant * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    // Bright lime-washed daub infill (cottages) - clean and creamy like the reference - mossy
    // fieldstone, and dark oak.
    static const Vec3 daub_cols[] = {{0.97f, 0.91f, 0.74f}, {0.96f, 0.87f, 0.66f}, {0.95f, 0.83f, 0.60f}};
    const Vec3 stone{0.62f, 0.56f, 0.45f};      // warm lime-mortared fieldstone (walls)
    const Vec3 found_stone{0.47f, 0.51f, 0.58f}; // cooler blue-grey foundation stone (contrasts daub)
    const Vec3 timber{0.24f, 0.16f, 0.10f};
    const Vec3 frame_col{0.20f, 0.13f, 0.08f}; // exposed oak half-timbering
    const Vec3 daub = daub_cols[variant % 3];
    // A storey's wall colour + whether it shows half-timbering (material 3: stone below, daub above).
    auto storey_col = [&](int s) {
        if (st.material == 1 || (st.material == 3 && s == 0)) {
            return stone;
        }
        return st.material == 2 ? glm::mix(daub, timber, 0.22f) : daub;
    };
    auto storey_timbered = [&](int s) { return st.material == 0 || st.material == 2 || (st.material == 3 && s > 0); };
    const Vec3 wall_col = storey_col(st.stories - 1); // the gables + dormers match the top storey
    const Vec3 plank{0.55f, 0.38f, 0.22f};    // honey oak floorboards (lighter than the furniture)
    const Vec3 flagstone{0.58f, 0.55f, 0.50f}; // warm grey flags (the stone houses' floors)
    const Vec3 subfloor{0.15f, 0.11f, 0.07f};  // the dark joints between boards / flags
    // Roofs come in two builds: chunky stepped wood/clay shingles in a warm earthy palette (browns,
    // terracotta, weathered wood, muted slate) or a golden straw thatch.
    const bool thatch = st.thatch;
    static const Vec3 shingle_palette[] = {
        {0.58f, 0.31f, 0.16f}, {0.86f, 0.45f, 0.18f}, {0.72f, 0.25f, 0.17f},
        {0.55f, 0.37f, 0.18f}, {0.31f, 0.45f, 0.62f}, {0.32f, 0.52f, 0.36f},
    };
    const Vec3 thatch_col{0.92f, 0.71f, 0.30f}; // golden straw
    const Vec3 roof_base = thatch ? thatch_col : shingle_palette[(variant * 3u + 1u) % 6u];
    const Vec3 roof = glm::mix(roof_base, roof_base * 0.86f, rnd(1));
    const Vec3 trim{0.19f, 0.13f, 0.09f};
    const Vec3 wood{0.40f, 0.28f, 0.16f};
    const Vec3 fire{1.0f, 0.55f, 0.15f};
    const Vec3 win_glow{1.0f, 0.82f, 0.42f};

    // shell (walls + roof + floors, fades when you're inside), opaque furniture, emissive, the window
    // light spill (glow).
    MeshData shell, op, em, glow;

    const f32 ww = 0.6f; // window half-width
    const Vec3 mull{0.14f, 0.10f, 0.07f};
    // A wall with a window opening cut out (up to 4 sub-boxes) + a warm lit pane with a mullion cross
    // behind it, and a collider over the wall's own storey. `along_x` = wall runs along x; `c` is the
    // window centre on that axis, [y0,y1] its height. A ground-floor window also spills its light out.
    auto win_wall = [&](const Vec3& lo, const Vec3& hi, bool along_x, f32 c, f32 y0, f32 y1, const Vec3& col, bool ground,
                        bool spill_light) {
        if (along_x) {
            if (c - ww > lo.x) add_box(shell, lo, {c - ww, hi.y, hi.z}, col);
            if (c + ww < hi.x) add_box(shell, {c + ww, lo.y, lo.z}, hi, col);
            add_box(shell, {c - ww, lo.y, lo.z}, {c + ww, y0, hi.z}, col);
            add_box(shell, {c - ww, y1, lo.z}, {c + ww, hi.y, hi.z}, col);
        } else {
            if (c - ww > lo.z) add_box(shell, lo, {hi.x, hi.y, c - ww}, col);
            if (c + ww < hi.z) add_box(shell, {lo.x, lo.y, c + ww}, hi, col);
            add_box(shell, {lo.x, lo.y, c - ww}, {hi.x, y0, c + ww}, col);
            add_box(shell, {lo.x, y1, c - ww}, {hi.x, hi.y, c + ww}, col);
        }
        // Lit pane + mullion at the opening's inner face.
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const f32 whh = (y1 - y0) * 0.5f;
        const f32 my = (y0 + y1) * 0.5f;
        Vec3 ctr, across, outward;
        f32 outer;
        if (along_x) {
            const bool front = lo.z > 0.0f;
            ctr = Vec3{c, my, front ? hi.z - t : lo.z + t};
            across = Vec3{1.0f, 0.0f, 0.0f};
            outward = Vec3{0.0f, 0.0f, front ? 1.0f : -1.0f};
            outer = front ? hi.z : lo.z;
        } else {
            const bool right = lo.x > 0.0f;
            ctr = Vec3{right ? lo.x + t : hi.x - t, my, c};
            across = Vec3{0.0f, 0.0f, 1.0f};
            outward = Vec3{right ? 1.0f : -1.0f, 0.0f, 0.0f};
            outer = right ? hi.x : lo.x;
        }
        add_quad(em, ctr - across * ww - up * whh, ctr + across * ww - up * whh, ctr + across * ww + up * whh,
                 ctr - across * ww + up * whh, win_glow);
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
        fquad(-ww - fb, ww + fb, -whh - fb, -whh); // bottom rail
        fquad(-ww - fb, -ww, -whh - fb, whh + fb); // left jamb
        fquad(ww, ww + fb, -whh - fb, whh + fb);   // right jamb
        add_collider(def, lo, hi);
        if (ground) {
            Vec3 face = ctr;
            if (along_x) {
                face.z = outer;
            } else {
                face.x = outer;
            }
            add_window_spill(def, glow, face, across, outward, ww, y0, spill_light);
        }
    };
    auto wall = [&](const Vec3& lo, const Vec3& hi, const Vec3& col) {
        add_box(shell, lo, hi, col);
        add_collider(def, lo, hi);
    };
    auto furn = [&](const Vec3& lo, const Vec3& hi, const Vec3& c) {
        add_box(op, lo, hi, c);
        add_collider(def, Vec3{lo.x, 0.0f, lo.z}, hi);
    };

    // ---- The ground floor: floorboards (flagstones in a stone ground floor) over a dark sub-floor
    // that shows through the joints. The ground under a house is levelled flat (worldgen::height), so
    // the floor lies right on it rather than being buried by the slope it stands on.
    add_box(op, {-w, -0.3f, -d}, {w, 0.02f, d}, subfloor);
    if (st.material == 1 || st.material == 3) {
        const int nx = std::max(3, static_cast<int>(std::round(2.0f * w / 0.75f)));
        const int nz = std::max(3, static_cast<int>(std::round(2.0f * d / 0.75f)));
        const f32 sx = 2.0f * w / static_cast<f32>(nx);
        const f32 sz = 2.0f * d / static_cast<f32>(nz);
        constexpr f32 gap = 0.025f;
        for (int j = 0; j < nz; ++j) {
            const f32 z0 = -d + static_cast<f32>(j) * sz;
            const f32 shift = (j % 2 == 1) ? sx * 0.5f : 0.0f;
            for (int i = -1; i < nx; ++i) {
                const f32 x0 = std::max(-w, -w + static_cast<f32>(i) * sx + shift);
                const f32 x1 = std::min(w, -w + static_cast<f32>(i + 1) * sx + shift);
                if (x1 - x0 < 0.1f) {
                    continue;
                }
                const f32 shade = 0.82f + 0.26f * rnd(300u + static_cast<u32>(j * 31 + i + 1));
                add_box(op, {x0 + gap, 0.02f, z0 + gap}, {x1 - gap, 0.055f, z0 + sz - gap}, flagstone * shade);
            }
        }
    } else {
        // Boards run along the house's long axis, each with one butt joint at a hashed spot so the
        // joints stagger across the floor.
        const bool along_x = w >= d;
        const f32 len = along_x ? w : d;
        const f32 span = along_x ? d : w;
        const int boards = std::max(4, static_cast<int>(std::round(2.0f * span / 0.32f)));
        constexpr f32 gap = 0.012f;
        for (int i = 0; i < boards; ++i) {
            const f32 a0 = glm::mix(-span, span, static_cast<f32>(i) / static_cast<f32>(boards)) + gap;
            const f32 a1 = glm::mix(-span, span, static_cast<f32>(i + 1) / static_cast<f32>(boards)) - gap;
            const f32 joint = glm::mix(-len * 0.6f, len * 0.6f, rnd(400u + static_cast<u32>(i)));
            for (int s = 0; s < 2; ++s) {
                const f32 l0 = s == 0 ? -len : joint + gap;
                const f32 l1 = s == 0 ? joint - gap : len;
                const Vec3 c = plank * (0.84f + 0.26f * rnd(500u + static_cast<u32>(i * 2 + s)));
                if (along_x) {
                    add_box(op, {l0, 0.02f, a0}, {l1, 0.055f, a1}, c);
                } else {
                    add_box(op, {a0, 0.02f, l0}, {a1, 0.055f, l1}, c);
                }
            }
        }
    }

    // Build each storey's four walls (an upper storey jetties out over the one below). The ground
    // floor's front wall is split around the door; every wall gets a centred window (light spills out
    // of the ground floor's).
    for (int s = 0; s < st.stories; ++s) {
        const Vec2 e = storey_extents(st, s);
        const f32 ws = e.x, ds = e.y;
        const Vec3 col = storey_col(s);
        const f32 y0 = static_cast<f32>(s) * sh;
        const f32 y1 = y0 + sh;
        const f32 wy0 = y0 + 0.85f, wy1 = std::min(y1 - 0.2f, y0 + 2.0f);
        const bool ground = s == 0;
        win_wall({-ws, y0, -ds}, {ws, y1, -ds + t}, true, 0.0f, wy0, wy1, col, ground, false); // back
        win_wall({-ws, y0, -ds}, {-ws + t, y1, ds}, false, 0.0f, wy0, wy1, col, ground && leanto_side >= 0.0f, true); // left
        win_wall({ws - t, y0, -ds}, {ws, y1, ds}, false, 0.0f, wy0, wy1, col, ground && leanto_side <= 0.0f, true);   // right
        if (ground) {
            win_wall({-ws, y0, ds - t}, {-dw, y1, ds}, true, (-ws - dw) * 0.5f, wy0, wy1, col, !bay, true); // front-L
            wall({dw, y0, ds - t}, {ws, y1, ds}, col);                                                       // front-R
            add_box(shell, {-dw, dh, ds - t}, {dw, y1, ds}, col);                                            // door lintel
        } else {
            win_wall({-ws, y0, ds - t}, {ws, y1, ds}, true, 0.0f, wy0, wy1, col, false, false); // upper front window
            // The floor of this storey (in the fading shell, so the interior stays visible), and - jettied
            // out - a dark soffit band under the overhang.
            const Vec2 pe = storey_extents(st, s - 1);
            add_box(shell, {-ws + t, y0 - 0.12f, -ds + t}, {ws - t, y0, ds - t}, timber);
            if (st.jetty > 0.0f && (ws > pe.x + 0.01f)) {
                add_box(shell, {-ws, y0 - 0.16f, -ds}, {ws, y0, ds}, timber * 0.9f);
            }
        }
        // A storey-band timber rim.
        add_box(shell, {-ws, y1 - 0.1f, -ds}, {ws, y1, -ds + t}, timber);
        add_box(shell, {-ws, y1 - 0.1f, ds - t}, {ws, y1, ds}, timber);
    }

    // ---- Exposed timber framing (half-timbered / Tudor look) on the daub + timber storeys: corner
    // posts, sill/head plates, studs and a diagonal brace per panel, all standing slightly proud of the
    // lime-washed infill. Stone storeys stay bare masonry.
    {
        constexpr f32 proud = 0.09f;
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
            bar(a_lo, a_hi, y0, y0 + 0.15f); // sill plate
            bar(a_lo, a_hi, y1 - 0.15f, y1); // head plate
            const int studs = std::max(3, static_cast<int>(std::round((a_hi - a_lo) / 0.72f)));
            for (int i = 0; i <= studs; ++i) {
                const f32 a = glm::mix(a_lo, a_hi, static_cast<f32>(i) / static_cast<f32>(studs));
                if (std::abs(a) < 0.82f) {
                    continue; // leave the central window / door opening clear
                }
                bar(a - 0.08f, a + 0.08f, y0, y1);
            }
            const f32 pf = face + out * proud;
            auto brace = [&](Vec2 q0, Vec2 q1) {
                Vec2 dir = q1 - q0;
                const f32 dl = glm::length(dir);
                if (dl <= 0.4f) return;
                dir /= dl;
                const Vec2 nrm{-dir.y * 0.09f, dir.x * 0.09f};
                auto P = [&](const Vec2& v) { return along_x ? Vec3{v.x, v.y, pf} : Vec3{pf, v.y, v.x}; };
                add_quad(shell, P(q0 + nrm), P(q1 + nrm), P(q1 - nrm), P(q0 - nrm), frame_col);
            };
            brace({a_lo + 0.18f, y1 - 0.18f}, {a_lo + 1.1f, y0 + 0.18f});
            brace({a_hi - 0.18f, y1 - 0.18f}, {a_hi - 1.1f, y0 + 0.18f});
        };
        for (int s = 0; s < st.stories; ++s) {
            if (!storey_timbered(s)) {
                continue;
            }
            const Vec2 e = storey_extents(st, s);
            const f32 y0 = static_cast<f32>(s) * sh;
            const f32 y1 = y0 + sh;
            for (const f32 sx : {-1.0f, 1.0f}) { // corner posts for this storey
                for (const f32 sz : {-1.0f, 1.0f}) {
                    const f32 x0 = sx > 0.0f ? e.x - 0.08f : -e.x - proud, x1 = sx > 0.0f ? e.x + proud : -e.x + 0.08f;
                    const f32 z0 = sz > 0.0f ? e.y - 0.08f : -e.y - proud, z1 = sz > 0.0f ? e.y + proud : -e.y + 0.08f;
                    add_box(shell, {x0, y0, z0}, {x1, y1, z1}, frame_col);
                }
            }
            panel(true, e.y, 1.0f, -e.x + 0.12f, e.x - 0.12f, y0, y1);   // front
            panel(true, -e.y, -1.0f, -e.x + 0.12f, e.x - 0.12f, y0, y1); // back
            panel(false, e.x, 1.0f, -e.y + 0.12f, e.y - 0.12f, y0, y1);  // right
            panel(false, -e.x, -1.0f, -e.y + 0.12f, e.y - 0.12f, y0, y1); // left
        }
    }

    // ---- Stone foundation: mortared fieldstone, proud of the walls above, built in a wider base
    // plinth + a main course with per-segment shade jitter. The front face is split around the doorway
    // (and the doorstep is low enough to stride over).
    {
        const f32 fy = std::min(1.4f, h * 0.4f);
        const f32 fo = 0.16f;
        const f32 po = fo + 0.07f, ph = 0.24f;
        auto fc = [&](u32 s) { return found_stone * (0.86f + 0.2f * rnd(s)); };
        add_box(op, {-w - po, 0.0f, -d - po}, {w + po, ph, -d + po}, fc(90));
        add_box(op, {-w - po, 0.0f, -d}, {-w + po, ph, d}, fc(91));
        add_box(op, {w - po, 0.0f, -d}, {w + po, ph, d}, fc(92));
        add_box(op, {-w - po, 0.0f, d - po}, {-dw - 0.06f, ph, d + po}, fc(93));
        add_box(op, {dw + 0.06f, 0.0f, d - po}, {w + po, ph, d + po}, fc(93));
        add_box(op, {-w - fo, ph, -d - fo}, {w + fo, fy, -d + fo}, fc(94));
        add_box(op, {-w - fo, ph, -d}, {-w + fo, fy, d}, fc(95));
        add_box(op, {w - fo, ph, -d}, {w + fo, fy, d}, fc(96));
        add_box(op, {-w - fo, ph, d - fo}, {-dw - 0.06f, fy, d + fo}, fc(97));
        add_box(op, {dw + 0.06f, ph, d - fo}, {w + fo, fy, d + fo}, fc(98));
    }

    // ---- The front door: a proud timber frame (op) round a planked leaf that swings open (Door part).
    add_door_frame(op, dw, dh, d, frame_col);
    add_door_leaf(def, -dw, dw, d + 0.02f, dh, Vec3{0.36f, 0.23f, 0.13f});

    // ---- Gable roof: the swooping, individually-tiled shingle/thatch roof over the top storey.
    const bool gable_x = top.x >= top.y;
    const f32 zf = top.y + oh;
    gable_roof(shell, top.x, top.y, h, rr, oh, thatch, roof, trim, wall_col, variant * 13u + 5u);

    // ---- A front dormer (a small gabled lit window poking up out of the roof) on taller, gable-fronted
    // houses - the distinctive reference detail.
    if (gable_x && st.stories >= 2) {
        const f32 dz = top.y * 0.40f;
        const f32 dy = h + rr * (1.0f - dz / zf) - 0.12f;
        const f32 dhw = 0.52f, body = 0.95f, depth = 0.55f;
        const f32 fz = dz + depth;
        add_box(op, {-dhw, dy, dz}, {dhw, dy + body, fz}, wall_col);
        add_box(em, {-dhw + 0.12f, dy + 0.22f, fz}, {dhw - 0.12f, dy + body - 0.1f, fz + 0.05f}, win_glow);
        add_box(op, {-0.04f, dy + 0.22f, fz}, {0.04f, dy + body - 0.1f, fz + 0.06f}, frame_col);
        const Vec3 apex{0.0f, dy + body + 0.42f, (dz + fz) * 0.5f};
        const Vec3 fl{-dhw - 0.08f, dy + body, fz + 0.08f}, fr{dhw + 0.08f, dy + body, fz + 0.08f};
        const Vec3 bl{-dhw - 0.08f, dy + body, dz - 0.08f}, br{dhw + 0.08f, dy + body, dz - 0.08f};
        const Vec3 under{0.0f, dy + body - 0.5f, (dz + fz) * 0.5f};
        emit_tri(shell, fl, fr, apex, under, roof);
        emit_tri(shell, fr, br, apex, under, roof);
        emit_tri(shell, br, bl, apex, under, roof);
        emit_tri(shell, bl, fl, apex, under, roof);
    }

    // ---- The massing (see above): a front-gable bay or a lean-to.
    if (bay) {
        add_front_gable(def, shell, op, em, glow, wall_col, roof, trim, frame_col, found_stone, win_glow, w, d, h, dw,
                        storey_timbered(0), thatch, variant * 17u + 3u);
    } else if (leanto_side != 0.0f) {
        add_leanto(def, shell, op, storey_col(0), roof, trim, found_stone, w, d, h, leanto_side);
    }

    // ---- Yard details around the house (the cosy reference touches): stone steps at the door, a barrel
    // + a stacked woodpile by the wall, a short garden fence, plant pots, a bush and grass tufts - kept
    // clear of the doorway.
    add_stone_steps(op, 0.0f, d + 0.1f, dw + 0.12f);
    add_barrel(op, bay ? Vec3{-w - 0.5f, 0.0f, 0.3f} : Vec3{-w + 0.45f, 0.0f, d + 0.5f}, 0.3f, 0.78f);
    add_woodpile(op, {w - 0.6f, 0.0f, d + 0.45f}, 0.95f, 3);
    add_fence_run(op, {w + 0.35f, 0.0f, d + 0.15f}, {w + 1.45f, 0.0f, d + 0.15f});
    add_plant_pot(op, {-w - 0.4f, 0.0f, d - 0.45f}, 0.16f, (variant % 2u) == 0u);
    add_leafy_bush(op, leanto_side > 0.0f ? Vec3{w + 0.5f, 0.0f, -d - 0.45f} : Vec3{w + 0.6f, 0.0f, -d + 0.7f}, 0.32f);
    add_grass_tuft(op, {w + 0.55f, 0.0f, d - 0.2f});
    add_grass_tuft(op, {-w - 0.5f, 0.0f, d - 0.9f});
    add_grass_tuft(op, {w + 0.95f, 0.0f, d - 0.4f});

    // ---- The hearth + chimney (back-left) every home has: a stone hearth with its fire, a stout stone
    // stack against the back wall in a few offset courses, a corbelled cap and a clay pot.
    const Vec3 chim_stone{0.48f, 0.49f, 0.52f};
    const f32 ccx = -w + t + 0.42f, ccz = -d + t + 0.35f;
    const f32 ctop = h + rr + 0.5f;
    if (st.furnish != Furnish::Bakery) {
        furn({-w + t, 0.0f, -d + t}, {-w + t + 1.2f, 1.4f, -d + t + 0.6f}, stone); // hearth
        add_box(em, {-w + t + 0.15f, 0.08f, -d + t + 0.1f}, {-w + t + 1.0f, 0.5f, -d + t + 0.5f}, fire);
    }
    for (int k = 0; k < 4; ++k) {
        const f32 y0 = glm::mix(1.3f, ctop, static_cast<f32>(k) / 4.0f);
        const f32 y1 = glm::mix(1.3f, ctop, static_cast<f32>(k + 1) / 4.0f);
        const f32 jx = (rnd(70 + static_cast<u32>(k)) - 0.5f) * 0.1f;
        const f32 jz = (rnd(80 + static_cast<u32>(k)) - 0.5f) * 0.1f;
        // (The stack rises against the back wall of the GROUND storey; a jettied storey wraps round it.)
        add_box(op, {ccx - 0.44f + jx, y0, ccz - 0.4f + jz}, {ccx + 0.44f + jx, y1 + 0.02f, ccz + 0.4f + jz},
                chim_stone * (0.9f + 0.16f * static_cast<f32>(k % 2)));
    }
    add_box(op, {ccx - 0.54f, ctop, ccz - 0.5f}, {ccx + 0.54f, ctop + 0.16f, ccz + 0.5f}, chim_stone * 0.82f);
    add_box(op, {ccx - 0.2f, ctop + 0.16f, ccz - 0.2f}, {ccx + 0.2f, ctop + 0.44f, ccz + 0.2f}, Vec3{0.55f, 0.28f, 0.2f});
    def.chimney_spot = Vec3{ccx, ctop + 0.5f, ccz};

    // ---- Furnishings, by the home's trade. ----
    const f32 bx0 = w - t - 1.45f, bx1 = w - t;
    switch (st.furnish) {
        case Furnish::Cottage: {
            furn({-0.55f, 0.0f, -0.3f}, {0.55f, 0.74f, 0.6f}, wood); // table
            add_box(em, {-0.08f, 0.76f, 0.0f}, {0.08f, 0.96f, 0.16f}, win_glow);                 // candle flame
            add_box(op, {-0.06f, 0.74f, 0.02f}, {0.06f, 0.84f, 0.14f}, Vec3{0.9f, 0.86f, 0.7f}); // candle
            for (const f32 sx : {-0.8f, 0.8f}) {                                                 // two stools
                add_box(op, {sx - 0.18f, 0.0f, 0.0f}, {sx + 0.18f, 0.45f, 0.36f}, wood * 0.85f);
            }
            furn({bx0, 0.0f, -d + t}, {bx1, 0.45f, -d + t + 1.9f}, wood); // bed frame
            add_box(op, {bx0 + 0.02f, 0.45f, -d + t}, {bx1 - 0.02f, 0.6f, -d + t + 1.6f}, Vec3{0.78f, 0.70f, 0.42f}); // mattress
            add_box(op, {bx0, 0.45f, -d + t}, {bx1, 0.72f, -d + t + 0.34f}, Vec3{0.7f, 0.62f, 0.4f});                 // pillow
            add_box(op, {bx0 + 0.04f, 0.6f, -d + t + 0.6f}, {bx1 - 0.04f, 0.66f, -d + t + 1.55f},
                    glm::mix(Vec3{0.55f, 0.2f, 0.18f}, Vec3{0.25f, 0.35f, 0.55f}, rnd(9))); // a woollen blanket
            def.bed_spot = Vec3{w - t - 0.7f, 0.0f, -d + t + 0.9f};
            break;
        }
        case Furnish::Tavern: {
            // The bar: a counter along the right side, casks racked behind it, tankards along the top.
            const f32 cx0 = w - t - 1.55f, cx1 = w - t - 1.0f;
            furn({cx0, 0.0f, -d + t + 0.9f}, {cx1, 1.05f, d - 1.3f}, wood * 0.9f);
            add_box(op, {cx0 - 0.06f, 1.05f, -d + t + 0.85f}, {cx1 + 0.06f, 1.12f, d - 1.25f}, wood * 1.15f); // bar top
            for (int i = 0; i < 3; ++i) {
                const f32 z = -d + t + 1.3f + static_cast<f32>(i) * 0.8f;
                add_barrel(op, {w - t - 0.45f, 0.0f, z}, 0.3f, 0.82f);
                add_tankard(op, {(cx0 + cx1) * 0.5f, 1.12f, z - 0.2f});
            }
            add_collider(def, {w - t - 0.8f, 0.0f, -d + t + 0.9f}, {w - t, 0.85f, d - 1.3f}); // the cask rack
            // Two tables with stools in the room, a tankard or two on each.
            for (int i = 0; i < 2; ++i) {
                const Vec3 tc{-w + 1.55f, 0.0f, -0.6f + static_cast<f32>(i) * 1.7f};
                furn({tc.x - 0.5f, 0.0f, tc.z - 0.4f}, {tc.x + 0.5f, 0.74f, tc.z + 0.4f}, wood);
                add_tankard(op, {tc.x - 0.2f, 0.74f, tc.z});
                add_tankard(op, {tc.x + 0.18f, 0.74f, tc.z + 0.12f});
                for (const f32 sx : {-0.78f, 0.78f}) {
                    add_box(op, {tc.x + sx - 0.16f, 0.0f, tc.z - 0.16f}, {tc.x + sx + 0.16f, 0.45f, tc.z + 0.16f}, wood * 0.85f);
                }
            }
            def.bed_spot = Vec3{w - t - 0.5f, 0.0f, d - 0.8f}; // the keeper, behind the bar
            break;
        }
        case Furnish::Bakery: {
            // The great domed BREAD OVEN in the back-left corner: a brick dome on a stone plinth, its
            // mouth glowing at the front, a flue up into the chimney.
            const Vec3 brick{0.66f, 0.36f, 0.24f};
            const Vec3 oc{-w + t + 0.95f, 0.0f, -d + t + 0.9f};
            furn({oc.x - 0.85f, 0.0f, oc.z - 0.85f}, {oc.x + 0.85f, 0.7f, oc.z + 0.85f}, stone * 0.9f); // plinth
            add_mesh(op, primitives::sphere(10, 6), glm::translate(Mat4{1.0f}, oc + Vec3{0.0f, 0.75f, 0.0f}) *
                                                        glm::scale(Mat4{1.0f}, Vec3{1.55f, 1.25f, 1.55f}), brick);
            add_box(op, {oc.x - 0.36f, 0.72f, oc.z + 0.62f}, {oc.x + 0.36f, 1.22f, oc.z + 0.82f}, Vec3{0.08f, 0.06f, 0.05f}); // mouth
            add_box(em, {oc.x - 0.3f, 0.74f, oc.z + 0.6f}, {oc.x + 0.3f, 1.12f, oc.z + 0.8f}, Vec3{1.4f, 0.7f, 0.25f});    // embers
                    // A long kneading table heaped with loaves; shelves of bread on the back wall; flour sacks.
            furn({-0.3f, 0.0f, -0.45f}, {1.4f, 0.82f, 0.45f}, wood);
            for (int i = 0; i < 6; ++i) {
                const f32 lx = -0.15f + static_cast<f32>(i % 3) * 0.5f, lz = -0.2f + static_cast<f32>(i / 3) * 0.38f;
                add_mesh(op, primitives::sphere(7, 4), glm::translate(Mat4{1.0f}, Vec3{lx, 0.9f, lz}) *
                                                           glm::scale(Mat4{1.0f}, Vec3{0.34f, 0.16f, 0.2f}),
                         Vec3{0.78f, 0.52f, 0.24f});
            }
            for (int k = 0; k < 2; ++k) {
                const f32 sy = 1.0f + static_cast<f32>(k) * 0.55f;
                add_box(op, {w - t - 2.2f, sy, -d + t}, {w - t - 0.2f, sy + 0.05f, -d + t + 0.4f}, wood * 0.9f);
                for (int i = 0; i < 4; ++i) {
                    add_mesh(op, primitives::sphere(7, 4),
                             glm::translate(Mat4{1.0f}, Vec3{w - t - 2.0f + static_cast<f32>(i) * 0.5f, sy + 0.12f, -d + t + 0.2f}) *
                                 glm::scale(Mat4{1.0f}, Vec3{0.32f, 0.15f, 0.2f}),
                             Vec3{0.74f, 0.5f, 0.22f} * (0.9f + 0.1f * static_cast<f32>(i % 2)));
                }
            }
            for (int i = 0; i < 3; ++i) {
                add_box(op, {w - t - 0.55f, 0.0f, -0.4f + static_cast<f32>(i) * 0.5f}, {w - t - 0.1f, 0.6f, -0.02f + static_cast<f32>(i) * 0.5f},
                        Vec3{0.86f, 0.82f, 0.72f}); // flour sacks
            }
            add_collider(def, {w - t - 0.55f, 0.0f, -0.4f}, {w - t, 0.6f, 0.98f});
            PropLight ovenl;
            ovenl.offset = oc + Vec3{0.0f, 1.0f, 1.1f};
            ovenl.direction = Vec3{0.3f, -0.2f, 1.0f};
            ovenl.color = Vec3{1.0f, 0.55f, 0.22f};
            ovenl.range = 9.0f;
            ovenl.intensity = 3.4f;
            ovenl.cone_deg = 170.0f;
            def.lights.push_back(ovenl);
            def.bed_spot = Vec3{0.6f, 0.0f, -0.95f}; // the baker, behind the kneading table
            break;
        }
        case Furnish::Shop: {
            // Shelves of goods along the back + left walls, a counter by the door on the right.
            static const Vec3 goods[] = {{0.62f, 0.2f, 0.18f}, {0.25f, 0.4f, 0.6f}, {0.8f, 0.68f, 0.3f},
                                         {0.32f, 0.5f, 0.3f}, {0.55f, 0.42f, 0.28f}, {0.7f, 0.7f, 0.66f}};
            auto shelf_run = [&](bool along_x, f32 face, f32 a0, f32 a1) {
                if (a1 - a0 < 0.5f) {
                    return;
                }
                if (along_x) {
                    add_collider(def, {a0, 0.0f, face}, {a1, 1.9f, face + 0.42f});
                } else {
                    add_collider(def, {face, 0.0f, a0}, {face + 0.42f, 1.9f, a1});
                }
                for (int k = 0; k < 3; ++k) {
                    const f32 sy = 0.5f + static_cast<f32>(k) * 0.55f;
                    if (along_x) {
                        add_box(op, {a0, sy, face}, {a1, sy + 0.05f, face + 0.42f}, wood * 0.9f);
                    } else {
                        add_box(op, {face, sy, a0}, {face + 0.42f, sy + 0.05f, a1}, wood * 0.9f);
                    }
                    const int n = static_cast<int>((a1 - a0) / 0.32f);
                    for (int i = 0; i < n; ++i) {
                        const f32 a = a0 + 0.12f + static_cast<f32>(i) * 0.32f;
                        const f32 gh = 0.14f + 0.18f * rnd(600u + static_cast<u32>(k * 31 + i));
                        const Vec3 gc = goods[(static_cast<u32>(i) * 7u + static_cast<u32>(k) * 3u + variant) % 6u];
                        if (along_x) {
                            add_box(op, {a, sy + 0.05f, face + 0.08f}, {a + 0.2f, sy + 0.05f + gh, face + 0.34f}, gc);
                        } else {
                            add_box(op, {face + 0.08f, sy + 0.05f, a}, {face + 0.34f, sy + 0.05f + gh, a + 0.2f}, gc);
                        }
                    }
                }
            };
            // (each run leaves the wall's window clear)
            shelf_run(true, -d + t, -w + t + 1.3f, -ww - 0.12f);
            shelf_run(true, -d + t, ww + 0.12f, w - t - 0.1f);
            shelf_run(false, -w + t, -d + t + 0.9f, -ww - 0.12f);
            shelf_run(false, -w + t, ww + 0.12f, d - 1.2f);
            furn({dw + 0.25f, 0.0f, d - 1.75f}, {w - t - 0.2f, 0.95f, d - 1.2f}, wood * 0.95f); // the counter
            add_box(op, {dw + 0.4f, 0.95f, d - 1.65f}, {dw + 0.62f, 1.1f, d - 1.45f}, Vec3{0.75f, 0.6f, 0.2f}); // scales
            def.bed_spot = Vec3{w - t - 0.6f, 0.0f, -0.4f}; // the shopkeeper, behind the counter
            break;
        }
    }

    // ---- Interior lights (hearth fire + a near-ceiling lamp on the top storey) - walled in, so they
    // shine out only through the openings (a shadow-mapped indoor light).
    PropLight hearth;
    hearth.offset = Vec3{-w + t + 0.7f, 1.0f, -d + t + 0.5f};
    hearth.direction = glm::normalize(Vec3{0.5f, -0.1f, 1.0f});
    hearth.color = Vec3{1.0f, 0.58f, 0.28f};
    hearth.range = 13.0f;
    hearth.intensity = 4.2f;
    hearth.cone_deg = 160.0f;
    def.lights.push_back(hearth);
    PropLight lamp;
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
    def.parts.push_back({std::move(glow), PropLayer::Glow});
    def.footprint = Vec2{w, d};
    def.wall_height = h;
    def.door_spot = Vec3{0.0f, 0.0f, d + 0.8f};       // just outside the front door
    def.inside_spot = Vec3{0.0f, 0.0f, d - t - 0.75f}; // just inside it
    return def;
}
} // namespace

// The (w,d) footprint half-extents of a house variant, so the village layout can keep
// houses from intersecting walls, the market and each other without building the mesh.
Vec2 PropLibrary::house_half_extents(u32 variant) {
    variant %= kHouseDefs; // a snowbound twin stands on the same footprint
    if (variant == kHouseTownhouse) return home_extents(kTownhouseSpec);
    if (variant == kHouseChapel) return Vec2{3.2f, 5.1f}; // the nave + its buttresses
    if (variant == kHouseKeep) return Vec2{5.1f, 5.1f};   // the keep + its corner turrets
    if (variant == kHousePub) return home_extents(kPubSpec);
    if (variant == kHouseBlacksmith) return Vec2{3.42f, 2.92f}; // the jettied upper storey
    if (variant == kHouseBakery) return home_extents(kBakerySpec);
    if (variant == kHouseShop) return home_extents(kShopSpec);
    return home_extents(kHouseStyles[variant % kHouseVariants]);
}

// A medieval village house, varied by `variant` (see kHouseStyles): cottages, crofts, longhouses,
// farmhouses, jettied merchants' houses, two- and three-storey townhouses, manors in daub / stone /
// timber - every one with a hinged door you can walk through, a furnished interior and windows that
// spill their light out after dark. Indices kHouseVariants.. dispatch to the special buildings.
PropDef PropLibrary::build_house(u32 variant) {
    if (variant >= kHouseDefs) { // a snowbound town's twin (see kSnowHouses)
        PropDef def = build_house(variant % kHouseDefs);
        make_snowy_house(def);
        return def;
    }
    if (variant == kHouseChapel) return build_chapel();
    if (variant == kHouseKeep) return build_keep();
    if (variant == kHouseTownhouse) return build_townhouse();
    if (variant == kHousePub) return build_pub();
    if (variant == kHouseBlacksmith) return build_blacksmith();
    if (variant == kHouseBakery) return build_bakery();
    if (variant == kHouseShop) return build_shop();
    return build_home(kHouseStyles[variant % kHouseVariants], variant, "house");
}

// A tall, narrow three-storey TOWNHOUSE: a stone ground floor under two jettied (overhanging)
// timber-framed upper storeys with lit windows, a steep shingled roof and a stone chimney - a home
// like any other inside (hearth, table, bed).
PropDef PropLibrary::build_townhouse() {
    return build_home(kTownhouseSpec, 31u, "townhouse");
}

// The village PUB: a stone ground floor under a timber-framed upper storey, a shingled roof with a
// dormer, a hanging tavern sign on a bracket, glowing windows, a door lantern, a beer garden off the
// front-right - and inside, a bar with casks racked behind it and tables to drink at.
PropDef PropLibrary::build_pub() {
    PropDef def = build_home(kPubSpec, 41u, "pub");
    MeshData op, em;
    const f32 w = kPubSpec.w, d = kPubSpec.d;
    const f32 wallTop = kPubSpec.story_h * static_cast<f32>(kPubSpec.stories);
    // hanging tavern sign: a bracket arm off the upper front-left, two chains + a board
    {
        const f32 ax = -w + 0.3f, ay = wallTop + 0.5f, az = d;
        add_box(op, {ax - 0.08f, ay - 0.08f, az}, {ax + 0.08f, ay + 0.08f, az + 1.1f}, kWoodDk); // arm
        add_box(op, {ax - 0.02f, ay - 0.5f, az + 0.78f}, {ax + 0.02f, ay, az + 0.82f}, Vec3{0.2f, 0.2f, 0.22f});
        add_box(op, {ax - 0.02f, ay - 0.5f, az + 1.0f}, {ax + 0.02f, ay, az + 1.04f}, Vec3{0.2f, 0.2f, 0.22f});
        add_box(op, {ax - 0.32f, ay - 1.0f, az + 0.74f}, {ax + 0.32f, ay - 0.5f, az + 1.08f}, kWoodDk * 0.9f);
        add_box(em, {ax - 0.26f, ay - 0.94f, az + 1.08f}, {ax + 0.26f, ay - 0.56f, az + 1.12f}, Vec3{0.5f, 0.6f, 0.72f});
    }
    add_wall_lantern(op, em, def, {0.95f, 1.75f, d + 0.12f}, {0.0f, 0.0f, 1.0f});
    // ---- The beer garden, off to the front-right (+x): trestle picnic tables with tankards on top,
    // barrels, terracotta flower pots, leafy bushes, tufts of grass and a low boundary fence.
    for (int ti = 0; ti < 2; ++ti) {
        const Vec3 tc{w + 1.5f, 0.0f, d + 0.6f + static_cast<f32>(ti) * 2.5f};
        add_picnic_table(op, tc);
        add_tankard(op, {tc.x - 0.24f, 0.69f, tc.z - 0.4f});
        add_tankard(op, {tc.x + 0.22f, 0.69f, tc.z + 0.3f});
        add_tankard(op, {tc.x + 0.08f, 0.69f, tc.z - 0.1f});
    }
    add_barrel(op, {w + 0.5f, 0.0f, d + 4.0f}, 0.32f, 0.82f);
    add_plant_pot(op, {w + 2.75f, 0.0f, d + 1.6f}, 0.16f, true);
    add_grass_tuft(op, {w + 2.3f, 0.0f, d + 3.4f});
    add_grass_tuft(op, {w + 0.9f, 0.0f, d + 4.3f});
    add_fence_run(op, {w + 0.3f, 0.0f, d + 4.6f}, {w + 2.9f, 0.0f, d + 4.6f});
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A BAKERY: a stone, thatched shop-house with the great domed bread oven glowing inside, loaves heaped
// on the kneading table + racked on the shelves, flour sacks by the wall - and outside, a hanging sign
// with a golden loaf and a stall table of fresh bread by the door.
PropDef PropLibrary::build_bakery() {
    PropDef def = build_home(kBakerySpec, 53u, "bakery");
    MeshData op;
    const f32 w = kBakerySpec.w, d = kBakerySpec.d;
    // The sign: a bracket off the front-right corner with a board + a golden loaf on it.
    const f32 ax = w - 0.3f, ay = 2.35f, az = d;
    add_box(op, {ax - 0.06f, ay - 0.06f, az}, {ax + 0.06f, ay + 0.06f, az + 0.9f}, kWoodDk);
    add_box(op, {ax - 0.28f, ay - 0.7f, az + 0.56f}, {ax + 0.28f, ay - 0.2f, az + 0.86f}, kWoodDk * 0.9f);
    add_mesh(op, primitives::sphere(8, 5), glm::translate(Mat4{1.0f}, Vec3{ax, ay - 0.45f, az + 0.9f}) *
                                               glm::scale(Mat4{1.0f}, Vec3{0.4f, 0.2f, 0.12f}),
             Vec3{0.86f, 0.62f, 0.26f});
    // A stall table of loaves out front, left of the door.
    const Vec3 sc{-w + 0.9f, 0.0f, d + 1.0f};
    add_box(op, {sc.x - 0.6f, 0.7f, sc.z - 0.35f}, {sc.x + 0.6f, 0.78f, sc.z + 0.35f}, kWoodDk * 1.1f);
    for (const f32 lx : {-0.5f, 0.5f}) {
        add_box(op, {sc.x + lx - 0.05f, 0.0f, sc.z - 0.3f}, {sc.x + lx + 0.05f, 0.7f, sc.z - 0.2f}, kWoodDk);
        add_box(op, {sc.x + lx - 0.05f, 0.0f, sc.z + 0.2f}, {sc.x + lx + 0.05f, 0.7f, sc.z + 0.3f}, kWoodDk);
    }
    for (int i = 0; i < 5; ++i) {
        add_mesh(op, primitives::sphere(7, 4),
                 glm::translate(Mat4{1.0f}, Vec3{sc.x - 0.42f + static_cast<f32>(i) * 0.21f, 0.86f, sc.z + (i % 2 ? 0.1f : -0.12f)}) *
                     glm::scale(Mat4{1.0f}, Vec3{0.24f, 0.13f, 0.16f}),
                 Vec3{0.8f, 0.55f, 0.24f});
    }
    add_collider(def, {sc.x - 0.6f, 0.0f, sc.z - 0.35f}, {sc.x + 0.6f, 0.8f, sc.z + 0.35f});
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    return def;
}

// A merchant's SHOP: a stone ground floor under a jettied timber upper storey, a striped awning over the
// front window, crates + barrels of wares set out by the door and a hanging sign - and inside, shelves
// of goods along the walls and a counter with the merchant's scales.
PropDef PropLibrary::build_shop() {
    PropDef def = build_home(kShopSpec, 67u, "shop");
    MeshData op, em;
    const f32 w = kShopSpec.w, d = kShopSpec.d;
    // The striped awning over the front-left window (canvas strips on a slope, on two posts).
    const f32 x0 = -w + 0.15f, x1 = -0.85f;
    const f32 ytop = 2.25f, ylow = 1.85f, zout = d + 1.25f;
    const int strips = std::max(3, static_cast<int>((x1 - x0) / 0.32f));
    for (int i = 0; i < strips; ++i) {
        const f32 a = glm::mix(x0, x1, static_cast<f32>(i) / static_cast<f32>(strips));
        const f32 b = glm::mix(x0, x1, static_cast<f32>(i + 1) / static_cast<f32>(strips));
        const Vec3 col = i % 2 == 0 ? Vec3{0.72f, 0.2f, 0.18f} : Vec3{0.92f, 0.88f, 0.78f};
        const Vec3 under{(a + b) * 0.5f, ylow - 1.0f, d + 0.6f};
        emit_tri(op, {a, ytop, d + 0.05f}, {b, ytop, d + 0.05f}, {b, ylow, zout}, under, col);
        emit_tri(op, {a, ytop, d + 0.05f}, {b, ylow, zout}, {a, ylow, zout}, under, col);
    }
    for (const f32 px : {x0 + 0.05f, x1 - 0.05f}) {
        add_box(op, {px - 0.04f, 0.0f, zout - 0.06f}, {px + 0.04f, ylow, zout + 0.02f}, kWoodDk);
    }
    // Wares out front: crates + a barrel under the awning.
    add_box(op, {x0 + 0.2f, 0.0f, d + 0.4f}, {x0 + 0.75f, 0.5f, d + 0.95f}, Vec3{0.5f, 0.36f, 0.2f});
    add_box(op, {x0 + 0.28f, 0.5f, d + 0.48f}, {x0 + 0.68f, 0.62f, d + 0.88f}, Vec3{0.8f, 0.3f, 0.2f}); // apples
    add_barrel(op, {x1 - 0.35f, 0.0f, d + 0.7f}, 0.28f, 0.75f);
    add_collider(def, {x0 + 0.2f, 0.0f, d + 0.4f}, {x0 + 0.75f, 0.6f, d + 0.95f});
    // The sign: a board hung off the front-right corner.
    const f32 ax = w - 0.3f, ay = 2.25f + kShopSpec.jetty, az = d + kShopSpec.jetty;
    add_box(op, {ax - 0.06f, ay - 0.06f, az}, {ax + 0.06f, ay + 0.06f, az + 0.85f}, kWoodDk);
    add_box(op, {ax - 0.3f, ay - 0.62f, az + 0.5f}, {ax + 0.3f, ay - 0.16f, az + 0.82f}, kWoodDk * 0.9f);
    add_box(em, {ax - 0.2f, ay - 0.54f, az + 0.82f}, {ax + 0.2f, ay - 0.24f, az + 0.85f}, Vec3{0.85f, 0.7f, 0.3f});
    add_wall_lantern(op, em, def, {0.95f, 1.8f, d + 0.12f}, {0.0f, 0.0f, 1.0f});
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
    MeshData op, em, glow, shell;
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
    // Big planked bay doors hung on the posts, folding open (outward) whenever anyone comes near.
    {
        const f32 x0 = -w + t + 0.13f, x1 = bayR - 0.13f, xm = (x0 + x1) * 0.5f;
        add_door_leaf(def, x0, xm - 0.01f, d + 0.09f, sg - 0.34f, kWoodDk * 1.15f, true);
        add_door_leaf(def, x1, xm + 0.01f, d + 0.09f, sg - 0.34f, kWoodDk * 1.15f, true);
    }

    // ---- Upper storey: jettied (overhanging) timber-framed daub over the whole footprint ----
    const f32 uw = w + jut, ud = d + jut, ut = sg + su;
    // (In the fading shell, with the roof: step into the workshop and the upper storey fades away.)
    add_box(shell, {-uw, sg, -ud}, {uw, ut, ud}, kDaub);
    add_box(shell, {-uw, sg - 0.16f, -ud}, {uw, sg, ud}, kFrame); // jetty soffit band
    timber_frame(shell, true, ud, 1.0f, -uw + 0.04f, uw - 0.04f, sg, ut, 0.55f, kFrame);
    timber_frame(shell, true, -ud, -1.0f, -uw + 0.04f, uw - 0.04f, sg, ut, 0.0f, kFrame);
    timber_frame(shell, false, uw, 1.0f, -ud + 0.04f, ud - 0.04f, sg, ut, 0.0f, kFrame);
    timber_frame(shell, false, -uw, -1.0f, -ud + 0.04f, ud - 0.04f, sg, ut, 0.0f, kFrame);
    lit_window(shell, em, {uw * 0.45f, sg + su * 0.5f, ud + wo}, {1, 0, 0}, UP, {0, 0, 1}, 0.34f, 0.42f, kGlow, kFrame);

    // ---- Grey slate roof ----
    const f32 apex = gable_roof(shell, uw, ud, ut, uw * 0.64f, 0.55f, false, slate, kTrim, kDaub, 51);

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
    def.chimney_spot = Vec3{cx, cTop + 0.45f, cz}; // forge smoke billows from the tower
    solid({cx - chw, 0.0f, cz - chd}, {cx + chw, cTop, cz + chd}); // (you can't walk into the tower)

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
    add_barrel(op, {2.2f, 0.0f, d + 0.6f}, 0.3f, 0.82f); // (clear of the bay door's swing)
    add_box(op, {1.93f, 0.7f, d + 0.32f}, {2.47f, 0.78f, d + 0.88f}, Vec3{0.26f, 0.46f, 0.56f}); // barrel water
    add_woodpile(op, {-1.4f, 0.0f, d + 1.0f}, 0.95f, 2);
    add_flagstones(op, {-0.2f, 0.0f, d + 0.7f}, 1.1f);
    add_grass_tuft(op, {-w - 0.5f, 0.0f, -d + 0.7f});
    add_grass_tuft(op, {w + 0.5f, 0.0f, d - 0.5f});
    add_leafy_bush(op, {-w - 0.55f, 0.0f, -d + 0.8f}, 0.3f);
    add_wall_lantern(op, em, def, {w + 0.02f, 1.9f, d - 0.6f}, {1.0f, 0.0f, 0.0f});

    add_window_spill(def, glow, {(bayR + w) * 0.5f, 1.4f, d}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, 0.34f, 1.0f, true);

    def.footprint = Vec2{w, d};
    def.wall_height = ut;
    def.bed_spot = Vec3{w - 0.85f, 0.0f, -d + 0.6f}; // inside the enclosed corner (right)
    def.door_spot = Vec3{-0.5f, 0.0f, d + 1.0f};      // walk in through the open bay
    def.inside_spot = Vec3{-0.6f, 0.0f, 0.9f};        // between the anvil and the forge
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    def.parts.push_back({std::move(glow), PropLayer::Glow});
    def.parts.push_back({std::move(shell), PropLayer::Roof});
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

    // A fire-basket brazier on the battlement: a dark iron bowl (octagonal, tapering out to the rim)
    // on a short stand, holding glowing coals with a few flame tongues licking up - a proper brazier
    // rather than a glowing cube.
    const Vec3 iron{0.17f, 0.15f, 0.15f};
    const Vec3 coal{1.0f, 0.5f, 0.16f};
    auto cup = [](MeshData& dst, f32 y0, f32 y1, f32 r0, f32 r1, const Vec3& col) {
        const Vec3 ctr{0.0f, (y0 + y1) * 0.5f, 0.0f}; // axis point -> emit_tri faces normals outward
        for (int i = 0; i < 8; ++i) {
            const f32 a0 = static_cast<f32>(i) / 8.0f * TwoPi;
            const f32 a1 = static_cast<f32>(i + 1) / 8.0f * TwoPi;
            const Vec3 b0{r0 * std::cos(a0), y0, r0 * std::sin(a0)}, b1{r0 * std::cos(a1), y0, r0 * std::sin(a1)};
            const Vec3 t0{r1 * std::cos(a0), y1, r1 * std::sin(a0)}, t1{r1 * std::cos(a1), y1, r1 * std::sin(a1)};
            emit_tri(dst, b0, b1, t1, ctr, col);
            emit_tri(dst, b0, t1, t0, ctr, col);
        }
    };
    auto tongue = [](MeshData& dst, const Vec3& base, f32 h, f32 w, const Vec3& col) {
        const Vec3 tip{base.x, base.y + h, base.z};
        const Vec3 a{base.x - w, base.y, base.z - w}, b{base.x + w, base.y, base.z - w};
        const Vec3 c{base.x + w, base.y, base.z + w}, d{base.x - w, base.y, base.z + w};
        emit_tri(dst, a, b, tip, base, col);
        emit_tri(dst, b, c, tip, base, col);
        emit_tri(dst, c, d, tip, base, col);
        emit_tri(dst, d, a, tip, base, col);
    };
    add_box(op, {-0.06f, mt - 0.1f, -0.06f}, {0.06f, mt + 0.04f, 0.06f}, iron); // short stand
    cup(op, mt + 0.02f, mt + 0.3f, 0.09f, 0.2f, iron);                          // iron bowl (tapers out)
    add_box(em, {-0.15f, mt + 0.18f, -0.15f}, {0.15f, mt + 0.3f, 0.15f}, coal); // glowing coals
    tongue(em, {0.0f, mt + 0.28f, 0.0f}, 0.36f, 0.11f, fire);                   // central flame
    tongue(em, {0.08f, mt + 0.28f, 0.05f}, 0.24f, 0.07f, fire);                 // side flame tongues
    tongue(em, {-0.07f, mt + 0.28f, -0.06f}, 0.27f, 0.07f, fire);

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
    const Vec3 slitc{0.10f, 0.11f, 0.14f}; // dark arrow-slit recess
    MeshData op;
    constexpr f32 r = 0.55f;
    constexpr f32 gh = 3.6f;
    add_box(op, {-r - 0.08f, 0.0f, -r - 0.08f}, {r + 0.08f, 0.5f, r + 0.08f}, stone * 0.94f); // battered base
    add_box(op, {-r, 0.0f, -r}, {r, gh, r}, stone);
    // Textured running-bond masonry on ALL FOUR faces (was just one), so the tower reads as stone
    // from every angle it's seen along the wall, not flat on its sides.
    stone_face(op, true, r, 1.0f, -r, r, 0.5f, gh, stone, 71u);    // +z
    stone_face(op, true, -r, -1.0f, -r, r, 0.5f, gh, stone, 83u);  // -z
    stone_face(op, false, r, 1.0f, -r, r, 0.5f, gh, stone, 91u);   // +x
    stone_face(op, false, -r, -1.0f, -r, r, 0.5f, gh, stone, 97u); // -x
    // A tall arrow slit on each face (a dark recess set just proud of the masonry).
    add_box(op, {-0.05f, gh * 0.46f, r - 0.02f}, {0.05f, gh * 0.72f, r + 0.07f}, slitc);
    add_box(op, {-0.05f, gh * 0.46f, -r - 0.07f}, {0.05f, gh * 0.72f, -r + 0.02f}, slitc);
    add_box(op, {r - 0.02f, gh * 0.30f, -0.05f}, {r + 0.07f, gh * 0.56f, 0.05f}, slitc);
    add_box(op, {-r - 0.07f, gh * 0.30f, -0.05f}, {-r + 0.02f, gh * 0.56f, 0.05f}, slitc);
    add_box(op, {-r - 0.1f, gh, -r - 0.1f}, {r + 0.1f, gh + 0.14f, r + 0.1f}, stone * 1.05f); // parapet lip
    // Crenellations ringing ALL FOUR sides (were only on +z / -z), for a complete battlement.
    for (int i = -1; i <= 1; ++i) {
        const f32 o = static_cast<f32>(i) * (r * 0.9f);
        add_box(op, {o - 0.14f, gh + 0.14f, r - 0.1f}, {o + 0.14f, gh + 0.46f, r + 0.1f}, stone * 1.07f); // +z
        add_box(op, {o - 0.14f, gh + 0.14f, -r - 0.1f}, {o + 0.14f, gh + 0.46f, -r + 0.1f}, stone * 1.07f); // -z
        add_box(op, {r - 0.1f, gh + 0.14f, o - 0.14f}, {r + 0.1f, gh + 0.46f, o + 0.14f}, stone * 1.07f); // +x
        add_box(op, {-r - 0.1f, gh + 0.14f, o - 0.14f}, {-r + 0.1f, gh + 0.46f, o + 0.14f}, stone * 1.07f); // -x
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
    const Vec3 stone{0.50f, 0.50f, 0.49f};
    const Vec3 coping{0.61f, 0.60f, 0.57f};
    const Vec3 wood{0.36f, 0.25f, 0.15f};
    const Vec3 darkwood{0.25f, 0.17f, 0.10f};
    const Vec3 shingle{0.46f, 0.30f, 0.20f};
    const Vec3 rope{0.60f, 0.52f, 0.36f};
    const Vec3 iron{0.30f, 0.31f, 0.34f};
    const Vec3 water{0.09f, 0.20f, 0.28f};
    MeshData m;
    constexpr f32 ro = 0.95f;   // rim outer half-extent
    constexpr f32 ri = 0.60f;   // rim inner half-extent (the shaft opening)
    constexpr f32 rim_h = 0.66f;
    // Solid square stone curb (four kerb walls) around the shaft...
    add_box(m, {-ro, 0.0f, -ro}, {ro, rim_h, -ri}, stone);
    add_box(m, {-ro, 0.0f, ri}, {ro, rim_h, ro}, stone);
    add_box(m, {-ro, 0.0f, -ri}, {-ri, rim_h, ri}, stone);
    add_box(m, {ri, 0.0f, -ri}, {ro, rim_h, ri}, stone);
    // ...textured with proud, shade-jittered running-bond blocks on the four outer faces.
    stone_face(m, true, -ro, -1.0f, -ro, ro, 0.0f, rim_h, stone, 11u);
    stone_face(m, true, ro, 1.0f, -ro, ro, 0.0f, rim_h, stone, 23u);
    stone_face(m, false, -ro, -1.0f, -ro, ro, 0.0f, rim_h, stone, 37u);
    stone_face(m, false, ro, 1.0f, -ro, ro, 0.0f, rim_h, stone, 51u);
    // A lighter coping cap lipping over the curb (a ring of four boxes, shaft left open).
    const f32 cy0 = rim_h, cy1 = rim_h + 0.11f, co = ro + 0.07f;
    add_box(m, {-co, cy0, -co}, {co, cy1, -ri}, coping);
    add_box(m, {-co, cy0, ri}, {co, cy1, co}, coping);
    add_box(m, {-co, cy0, -ri}, {-ri, cy1, ri}, coping);
    add_box(m, {ri, cy0, -ri}, {co, cy1, ri}, coping);
    add_box(m, {-ri + 0.04f, 0.30f, -ri + 0.04f}, {ri - 0.04f, 0.40f, ri - 0.04f}, water); // dark water below
    // Two posts rising from the cap to carry the roof + windlass.
    const f32 post_top = cy1 + 1.70f;
    for (f32 px : {-0.80f, 0.80f}) {
        add_box(m, {px - 0.09f, cy1, -0.09f}, {px + 0.09f, post_top, 0.09f}, wood);
    }
    // The windlass: a horizontal wooden roller spanning the posts, with a wound rope and an
    // iron crank handle you'd turn to raise the bucket.
    const f32 wy = post_top - 0.20f;
    add_box(m, {-0.80f, wy - 0.10f, -0.10f}, {0.80f, wy + 0.10f, 0.10f}, darkwood);     // roller
    add_box(m, {-0.30f, wy - 0.11f, -0.11f}, {0.30f, wy + 0.11f, 0.11f}, rope * 0.85f); // rope wound on it
    add_box(m, {0.80f, wy - 0.04f, 0.10f}, {0.90f, wy + 0.04f, 0.36f}, iron);           // crank arm
    add_box(m, {0.80f, wy - 0.30f, 0.30f}, {0.90f, wy + 0.04f, 0.38f}, wood);           // crank grip
    // The bucket: a staved wooden pail with two iron bands + a bail, hung on a rope from the
    // windlass, dangling just above the rim.
    const f32 bb0 = cy1 + 0.30f, bb1 = bb0 + 0.34f;
    add_box(m, {-0.02f, bb1 + 0.12f, -0.02f}, {0.02f, wy, 0.02f}, rope); // rope down from the roller
    add_box(m, {-0.17f, bb0, -0.17f}, {0.17f, bb1, 0.17f}, wood * 0.92f);
    add_box(m, {-0.185f, bb0 + 0.03f, -0.185f}, {0.185f, bb0 + 0.08f, 0.185f}, iron); // lower band
    add_box(m, {-0.185f, bb1 - 0.08f, -0.185f}, {0.185f, bb1 - 0.03f, 0.185f}, iron); // upper band
    add_box(m, {-0.17f, bb1, -0.02f}, {0.17f, bb1 + 0.12f, 0.02f}, iron);             // bail handle
    // A proper shingled gable roof on the posts (shared stepped-shingle builder); a modest
    // footprint + rise keeps the eave "swoop" gentle at this small scale.
    gable_roof(m, ro + 0.16f, ro + 0.16f, post_top, 0.42f, 0.14f, false, shingle, darkwood, wood, 7u);
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
    const Vec3 under{0.0f, ry - 0.5f, 0.0f}; // both slopes face up + out, away from beneath the ridge
    emit_tri(m, {-hl, ry, -hw - 0.15f}, {hl, ry, -hw - 0.15f}, {hl, ry + 0.5f, 0.0f}, under, wood * 1.05f);
    emit_tri(m, {-hl, ry, -hw - 0.15f}, {hl, ry + 0.5f, 0.0f}, {-hl, ry + 0.5f, 0.0f}, under, wood * 1.05f);
    emit_tri(m, {hl, ry, hw + 0.15f}, {-hl, ry, hw + 0.15f}, {-hl, ry + 0.5f, 0.0f}, under, wood * 0.95f);
    emit_tri(m, {hl, ry, hw + 0.15f}, {-hl, ry + 0.5f, 0.0f}, {hl, ry + 0.5f, 0.0f}, under, wood * 0.95f);
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
        // Striped awning sloping down toward the plaza, with a scalloped striped valance (fringe)
        // hanging off its front lip - the classic market-stall look (was just a flat sloped quad).
        constexpr int strips = 4;
        const f32 back = axis == 0 ? cz - sdir * (hd + 0.2f) : cx - sdir * (hd + 0.2f);
        const f32 front = axis == 0 ? cz + sdir * (hd + 0.55f) : cx + sdir * (hd + 0.55f);
        auto P = [&](f32 u, f32 y, f32 perp) {
            return axis == 0 ? Vec3{cx + u, y, perp} : Vec3{perp, y, cz + u};
        };
        const Vec3 sc{cx, ph, cz}; // stall-top centre, to orient the fringe normals outward
        for (int k = 0; k < strips; ++k) {
            const f32 u0 = -hw + (2.0f * hw) * static_cast<f32>(k) / static_cast<f32>(strips);
            const f32 u1 = -hw + (2.0f * hw) * static_cast<f32>(k + 1) / static_cast<f32>(strips);
            const f32 um = 0.5f * (u0 + u1);
            const Vec3 c = (k % 2 == 0) ? cloth : cream;
            add_quad(m, P(u0, ph + 0.45f, back), P(u1, ph + 0.45f, back), P(u1, ph, front),
                     P(u0, ph, front), c); // awning panel
            // Valance: a striped band off the front lip + a downward scallop point per strip.
            if (axis == 0) {
                add_box(m, {cx + u0, ph - 0.18f, front - 0.03f}, {cx + u1, ph, front + 0.03f}, c);
            } else {
                add_box(m, {front - 0.03f, ph - 0.18f, cz + u0}, {front + 0.03f, ph, cz + u1}, c);
            }
            emit_tri(m, P(u0, ph - 0.18f, front), P(u1, ph - 0.18f, front), P(um, ph - 0.36f, front),
                     sc, c);
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

    // Market-day dressing: a tall banner pole at each corner of the stall square (crimson + royal
    // blue alternating, gold finials), with pennant BUNTING strung from the top of the market cross
    // out to every pole and round the square between them - a festive canopy high over the stalls.
    {
        constexpr f32 cp = 5.5f;   // corner pole offset (clear of the stalls + the well)
        constexpr f32 top = 4.5f;  // pole height
        const Vec3 gold{0.80f, 0.62f, 0.24f};
        const Vec3 cloths2[2] = {{0.60f, 0.12f, 0.12f}, {0.16f, 0.26f, 0.58f}};
        const Vec3 corners[4] = {{cp, 0.0f, cp}, {-cp, 0.0f, cp}, {-cp, 0.0f, -cp}, {cp, 0.0f, -cp}};
        for (int i = 0; i < 4; ++i) {
            const Vec3& c = corners[i];
            add_box(m, {c.x - 0.18f, 0.0f, c.z - 0.18f}, {c.x + 0.18f, 0.32f, c.z + 0.18f}, stone); // footing
            add_box(m, {c.x - 0.07f, 0.0f, c.z - 0.07f}, {c.x + 0.07f, top, c.z + 0.07f}, dark);   // pole
            add_box(m, {c.x - 0.07f, top, c.z - 0.07f}, {c.x + 0.07f, top + 0.22f, c.z + 0.07f}, gold);
            // A pennon hanging from the pole, facing out from the square's centre.
            const Vec3 out = glm::normalize(Vec3{c.x, 0.0f, c.z});
            const Vec3 side{-out.z, 0.0f, out.x};
            const Vec3 a0 = c + Vec3{0.0f, top - 0.15f, 0.0f}, a1 = c + Vec3{0.0f, top - 1.45f, 0.0f};
            const Vec3 tipp = c + side * 1.0f + Vec3{0.0f, top - 0.8f, 0.0f};
            emit_tri(m, a0, a1, tipp, c + out * 0.5f + Vec3{0.0f, top - 0.8f, 0.0f}, cloths2[i % 2]);
            emit_tri(m, a0, a1, tipp, c - out * 0.5f + Vec3{0.0f, top - 0.8f, 0.0f}, cloths2[i % 2] * 0.9f);
            BoxCollider pc;
            pc.center = c;
            pc.half_extents = Vec2{0.18f, 0.18f};
            pc.height = 1.2f;
            def.colliders.push_back(pc);
        }
        const Vec3 crown{0.0f, 3.62f, 0.0f}; // under the market cross's cap
        for (int i = 0; i < 4; ++i) {
            const Vec3 pole_top = corners[i] + Vec3{0.0f, top - 0.05f, 0.0f};
            add_bunting(m, crown, pole_top, 0.32f);                                         // spoke
            add_bunting(m, pole_top, corners[(i + 1) % 4] + Vec3{0.0f, top - 0.05f, 0.0f}, 0.6f); // rim
        }
    }

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
    // Vertical plank staves on the long side rails (per-plank shade), so the bed reads as planked
    // boards, not a smooth box.
    for (int i = 0; i < 8; ++i) {
        const f32 x = -0.95f + 1.9f * (static_cast<f32>(i) + 0.5f) / 8.0f;
        const Vec3 pc = wood * (0.84f + 0.26f * hashf(static_cast<u32>(i) * 7u + 3u));
        add_box(m, {x - 0.07f, 0.72f, 0.575f}, {x + 0.07f, 1.05f, 0.605f}, pc);   // +z face plank
        add_box(m, {x - 0.07f, 0.72f, -0.605f}, {x + 0.07f, 1.05f, -0.575f}, pc); // -z face plank
    }
    // Iron corner brackets binding the rails.
    for (const f32 cx : {-0.95f, 0.95f}) {
        for (const f32 cz : {-0.54f, 0.54f}) {
            add_box(m, {cx - 0.05f, 0.96f, cz - 0.06f}, {cx + 0.05f, 1.08f, cz + 0.06f}, metal);
        }
    }

    // Cargo: a varied, stacked, lashed load so the haul reads as properly loaded (not a couple of bare
    // crates) - crates + burlap sacks tied down under rope straps.
    const Vec3 sack{0.50f, 0.44f, 0.32f};
    add_box(m, {-0.55f, 0.72f, -0.34f}, {0.05f, 1.18f, 0.28f}, crate);         // big crate (front-left)
    add_box(m, {-0.5f, 1.18f, -0.26f}, {-0.04f, 1.44f, 0.2f}, crate * 1.08f);  // a crate stacked on top
    add_box(m, {0.12f, 0.72f, -0.1f}, {0.62f, 1.02f, 0.4f}, crate * 0.9f);     // crate (right)
    add_box(m, {0.18f, 0.72f, -0.42f}, {0.58f, 0.98f, -0.06f}, crate * 1.05f); // crate (right-back)
    add_box(m, {-0.04f, 0.72f, 0.2f}, {0.32f, 0.96f, 0.46f}, sack);            // burlap sack in a gap
    add_box(m, {0.36f, 1.02f, 0.04f}, {0.64f, 1.2f, 0.36f}, sack * 0.92f);     // sack atop the right crate
    for (f32 rx : {-0.32f, 0.34f}) {                                           // rope straps over the load
        add_box(m, {rx - 0.03f, 1.14f, -0.56f}, {rx + 0.03f, 1.2f, 0.56f}, dark);
    }

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
    const Vec3 wood{0.44f, 0.30f, 0.17f};
    const Vec3 band{0.20f, 0.17f, 0.14f};
    const Vec3 soil{0.18f, 0.13f, 0.09f};
    MeshData op;
    // A round, tapered wooden tub (octagonal, narrower at the base) with iron-band hoops, a rim lip
    // and soil - a proper barrel-planter, not a square box.
    constexpr int sides = 8;
    constexpr f32 rb = 0.30f, rt = 0.42f, h = 0.56f;
    const Vec3 axis{0.0f, h * 0.5f, 0.0f};
    auto pt = [](f32 r, f32 y, f32 a) { return Vec3{std::cos(a) * r, y, std::sin(a) * r}; };
    auto ang = [](int s) { return TwoPi * (static_cast<f32>(s) + 0.5f) / static_cast<f32>(sides); };
    // tapered stave walls (per-stave shade)
    for (int s = 0; s < sides; ++s) {
        const f32 a0 = ang(s), a1 = ang(s + 1);
        const Vec3 b0 = pt(rb, 0.0f, a0), b1 = pt(rb, 0.0f, a1), t0 = pt(rt, h, a0), t1 = pt(rt, h, a1);
        const Vec3 c = wood * (0.86f + 0.22f * hashf(static_cast<u32>(s) * 5u + 1u));
        emit_tri(op, b0, b1, t1, axis, c);
        emit_tri(op, b0, t1, t0, axis, c);
    }
    // two iron-band hoops + a lighter rim lip, each a short proud octagonal ring
    auto ring_band = [&](f32 y0, f32 y1, f32 r, const Vec3& col) {
        for (int s = 0; s < sides; ++s) {
            const f32 a0 = ang(s), a1 = ang(s + 1);
            emit_tri(op, pt(r, y0, a0), pt(r, y0, a1), pt(r, y1, a1), axis, col);
            emit_tri(op, pt(r, y0, a0), pt(r, y1, a1), pt(r, y1, a0), axis, col);
        }
    };
    ring_band(0.09f, 0.16f, rb + (rt - rb) * 0.16f + 0.02f, band); // lower hoop
    ring_band(0.40f, 0.47f, rb + (rt - rb) * 0.78f + 0.02f, band); // upper hoop
    ring_band(h - 0.02f, h + 0.06f, rt + 0.03f, wood * 1.12f);     // rim lip
    // soil disc filling the top (a triangle fan, facing up)
    const Vec3 below{0.0f, -1.0f, 0.0f};
    const f32 sy = h - 0.02f;
    for (int s = 0; s < sides; ++s) {
        emit_tri(op, Vec3{0.0f, sy, 0.0f}, pt(rt - 0.05f, sy, ang(s)), pt(rt - 0.05f, sy, ang(s + 1)),
                 below, soil);
    }
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    // A compact plant sitting IN the pot (shrunk + raised onto the soil) so the tub shows.
    MeshData fol = primitives::bush(0, Vec3{0.26f, 0.46f, 0.22f});
    for (Vertex& v : fol.vertices) {
        v.position *= 0.60f;
        v.position.y += 0.50f;
    }
    def.parts.push_back({std::move(fol), PropLayer::Foliage});
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
    const Vec3 coping{0.65f, 0.64f, 0.59f};
    const Vec3 water{0.15f, 0.40f, 0.54f};
    const Vec3 spray{0.55f, 0.80f, 0.92f}; // bright frothy water (emissive, so it glints)
    MeshData op;
    MeshData em;
    // A round tier (a scaled unit cylinder) - its outward/cap normals stay valid under axis scaling.
    auto add_cyl = [](MeshData& dst, f32 r, f32 y0, f32 y1, const Vec3& col) {
        const MeshData c = primitives::cylinder(14, col);
        const f32 sy = y1 - y0, cy = (y0 + y1) * 0.5f;
        const u32 base = static_cast<u32>(dst.vertices.size());
        for (Vertex v : c.vertices) {
            v.position.x *= r * 2.0f;
            v.position.z *= r * 2.0f;
            v.position.y = v.position.y * sy + cy;
            dst.vertices.push_back(v);
        }
        for (u32 idx : c.indices) {
            dst.indices.push_back(base + idx);
        }
    };
    constexpr int n = 12;
    constexpr f32 ro = 1.7f, ri = 1.35f, rim_h = 0.5f;
    const f32 wlevel = rim_h * 0.7f;
    // Dodecagonal stone rim + water surface, built as wedges (per-wedge shade reads as masonry).
    for (int i = 0; i < n; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(n);
        const Vec3 o0{std::cos(a0) * ro, 0.0f, std::sin(a0) * ro};
        const Vec3 o1{std::cos(a1) * ro, 0.0f, std::sin(a1) * ro};
        const Vec3 i0{std::cos(a0) * ri, 0.0f, std::sin(a0) * ri};
        const Vec3 i1{std::cos(a1) * ri, 0.0f, std::sin(a1) * ri};
        const Vec3 up{0.0f, rim_h, 0.0f};
        const Vec3 lo{0.0f, wlevel, 0.0f};
        const Vec3 sj = stone * (0.86f + 0.26f * hashf(static_cast<u32>(i) * 7u + 3u));
        add_quad(op, o0, o0 + up, o1 + up, o1, sj);                 // outer rim wall (outward normal)
        add_quad(op, i0 + up, i1 + up, o1 + up, o0 + up, coping);   // rim top coping (faces up)
        add_quad(op, i0, i1, i1 + lo, i0 + lo, stone * 0.88f);      // inner wall (faces the water)
        add_tri(op, Vec3{0.0f, wlevel, 0.0f}, Vec3{i1.x, wlevel, i1.z}, Vec3{i0.x, wlevel, i0.z},
                water); // basin water surface
    }
    // Round, tiered "wedding-cake" centrepiece: a pedestal -> lower bowl -> stem -> upper bowl,
    // each bowl catching water, topped by a bubbling-water finial.
    add_cyl(op, 0.30f, wlevel, 0.74f, stone);  // lower pedestal
    add_cyl(op, 0.64f, 0.74f, 0.88f, coping);  // lower bowl lip
    add_cyl(op, 0.56f, 0.84f, 0.90f, water);   // water in the lower bowl
    add_cyl(op, 0.18f, 0.90f, 1.26f, stone);   // stem
    add_cyl(op, 0.40f, 1.26f, 1.39f, coping);  // upper bowl lip
    add_cyl(op, 0.33f, 1.35f, 1.41f, water);   // water in the upper bowl
    // The finial: a little dome of bright water welling up + spilling, emissive so it glints.
    add_cyl(em, 0.10f, 1.41f, 1.58f, spray);
    add_cyl(em, 0.16f, 1.54f, 1.66f, spray);
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
    if (variant % static_cast<int>(kDecorVariants) == kDecorBrazier) {
        return build_brazier();
    }
    if (variant % static_cast<int>(kDecorVariants) == kDecorSnowman) {
        return build_snowman();
    }
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
    // A wooden CRATE: a light planked body framed by darker reinforcing corner posts + a base rail
    // and a lid seam band (each standing a hair proud), so it reads as a slatted crate rather than a
    // plain coloured box. Shared by the crate stack + the market-stall goods crate.
    auto crate = [&](const Vec3& lo, const Vec3& hi, const Vec3& body) {
        add_box(m, lo, hi, body);
        const Vec3 frame = dark * 1.2f;
        constexpr f32 p = 0.045f, e = 0.014f;
        for (f32 sx : {lo.x, hi.x}) { // four vertical corner posts (edge reinforcement)
            for (f32 sz : {lo.z, hi.z}) {
                add_box(m, {sx - p, lo.y - e, sz - p}, {sx + p, hi.y + e, sz + p}, frame);
            }
        }
        const f32 lid = hi.y - 0.16f * (hi.y - lo.y); // a lid seam near the top
        add_box(m, {lo.x - e, lid - 0.02f, lo.z - e}, {hi.x + e, lid + 0.02f, hi.z + e}, frame * 0.92f);
        add_box(m, {lo.x - e, lo.y + 0.05f, lo.z - e}, {hi.x + e, lo.y + 0.09f, hi.z + e},
                frame * 0.92f); // base band
    };
    switch (variant % static_cast<int>(kDecorVariants)) {
        case 0: // a belled wooden barrel (shared with the yard helper)
            def.name = "barrel";
            belled_barrel(m, Vec3{0.0f, 0.0f, 0.0f}, 0.38f, 0.9f, wood, dark);
            collider(0.38f, 0.38f, 0.9f);
            break;
        case 1: // a stack of reinforced wooden crates
            def.name = "crates";
            crate({-0.42f, 0.0f, -0.42f}, {0.42f, 0.72f, 0.42f}, wood * 1.08f); // base crate
            crate({-0.18f, 0.72f, -0.36f}, {0.5f, 1.34f, 0.3f}, wood);          // crate on top
            crate({-0.5f, 0.0f, 0.16f}, {0.04f, 0.52f, 0.66f}, wood * 0.92f);   // crate beside
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
            // Striped awning sloping to the front. Emit via emit_tri with a centre BELOW so the
            // normals face UP and the visible top is lit (a plain add_quad here faced its normal
            // down, leaving the awning near-black). A short valance hangs off the lit front edge.
            for (int k = 0; k < 4; ++k) {
                const f32 x0 = -1.0f + 0.5f * static_cast<f32>(k);
                const f32 x1 = -1.0f + 0.5f * static_cast<f32>(k + 1);
                const Vec3 c = (k % 2 == 0) ? cloth : cream;
                const Vec3 below{0.5f * (x0 + x1), 0.6f, 0.05f}; // under the awning -> normals point up
                const Vec3 p0{x0, 2.3f, -0.6f}, p1{x1, 2.3f, -0.6f}, p2{x1, 2.0f, 0.7f}, p3{x0, 2.0f, 0.7f};
                emit_tri(m, p0, p1, p2, below, c);
                emit_tri(m, p0, p2, p3, below, c);
                add_box(m, {x0, 1.78f, 0.66f}, {x1, 2.0f, 0.72f}, c * 0.92f); // valance off the front edge
            }
            crate({-0.7f, 0.92f, -0.2f}, {-0.3f, 1.2f, 0.2f}, wood * 1.1f);             // crate of goods
            add_box(m, {0.2f, 0.92f, -0.2f}, {0.6f, 1.12f, 0.2f}, Vec3{0.6f, 0.5f, 0.3f}); // sacks
            collider(1.0f, 0.45f, 0.85f);
            break;
        }
        case 4: { // a signpost with two POINTED directional boards + a capped post
            def.name = "signpost";
            add_box(m, {-0.07f, 0.0f, -0.07f}, {0.07f, 1.85f, 0.07f}, wood);
            add_box(m, {-0.1f, 1.85f, -0.1f}, {0.1f, 1.95f, 0.1f}, wood * 0.85f); // post cap
            // A plank that points: a rectangle from x0 to x1 with a pyramidal arrow tip beyond x1.
            auto board = [&](f32 x0, f32 x1, f32 ymid, f32 dir, const Vec3& col) {
                constexpr f32 yh = 0.17f, zh = 0.04f;
                const f32 lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
                add_box(m, {lo, ymid - yh, -zh}, {hi, ymid + yh, zh}, col);
                const Vec3 P{x1 + dir * 0.22f, ymid, 0.0f}, ctr{x1, ymid, 0.0f};
                const Vec3 A{x1, ymid + yh, zh}, B{x1, ymid - yh, zh}, C{x1, ymid - yh, -zh},
                    D{x1, ymid + yh, -zh};
                emit_tri(m, A, B, P, ctr, col);
                emit_tri(m, B, C, P, ctr, col);
                emit_tri(m, C, D, P, ctr, col);
                emit_tri(m, D, A, P, ctr, col);
            };
            board(0.05f, 0.82f, 1.55f, 1.0f, cream);            // upper board points +x
            board(-0.05f, -0.82f, 1.12f, -1.0f, cream * 0.92f); // lower board points -x
            collider(0.1f, 0.1f, 1.6f);
            break;
        }
        case 5: { // a stone water trough - a HOLLOW basin holding water (was a solid block hiding it)
            def.name = "trough";
            constexpr f32 ox = 0.95f, oz = 0.42f, th = 0.5f, wl = 0.13f;
            add_box(m, {-ox, 0.0f, -oz}, {ox, th, -oz + wl}, stone);             // -z wall
            add_box(m, {-ox, 0.0f, oz - wl}, {ox, th, oz}, stone * 0.96f);        // +z wall
            add_box(m, {-ox, 0.0f, -oz + wl}, {-ox + wl, th, oz - wl}, stone * 1.04f); // -x end
            add_box(m, {ox - wl, 0.0f, -oz + wl}, {ox, th, oz - wl}, stone * 1.04f);    // +x end
            add_box(m, {-ox + wl, 0.0f, -oz + wl}, {ox - wl, 0.14f, oz - wl}, stone * 0.88f); // floor
            add_box(m, {-ox + wl, 0.14f, -oz + wl}, {ox - wl, 0.42f, oz - wl}, water);  // water (now visible)
            collider(0.95f, 0.42f, 0.5f);
            break;
        }
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
        case kDecorBench: { // a wooden street bench: seat along x, back to -z (sitters face +z)
            def.name = "bench";
            const Vec3 plank = wood * 1.12f;
            add_box(m, {-0.85f, 0.42f, -0.2f}, {0.85f, 0.48f, -0.01f}, plank);        // seat planks
            add_box(m, {-0.85f, 0.42f, 0.01f}, {0.85f, 0.48f, 0.2f}, plank * 0.94f);
            for (f32 ex : {-0.7f, 0.7f}) {
                add_box(m, {ex - 0.06f, 0.0f, -0.17f}, {ex + 0.06f, 0.42f, 0.17f}, dark);  // leg slab
                add_box(m, {ex - 0.05f, 0.42f, -0.25f}, {ex + 0.05f, 0.95f, -0.17f}, dark); // back post
                add_box(m, {ex - 0.06f, 0.6f, -0.2f}, {ex + 0.06f, 0.66f, 0.18f}, dark);    // armrest
            }
            add_box(m, {-0.85f, 0.64f, -0.26f}, {0.85f, 0.73f, -0.2f}, plank * 0.95f); // back rail
            add_box(m, {-0.85f, 0.82f, -0.26f}, {0.85f, 0.93f, -0.2f}, plank);         // top rail
            collider(0.85f, 0.26f, 0.5f);
            break;
        }
        case kDecorFlowerCart: { // a flower-seller's handcart heaped with bright blooms
            def.name = "flower_cart";
            static const Vec3 blooms[6] = {{0.80f, 0.16f, 0.18f}, {0.88f, 0.72f, 0.18f}, {0.58f, 0.30f, 0.72f},
                                           {0.84f, 0.82f, 0.76f}, {0.88f, 0.42f, 0.58f}, {0.92f, 0.50f, 0.16f}};
            add_box(m, {-0.62f, 0.42f, -0.42f}, {0.62f, 0.54f, 0.42f}, wood);              // bed
            add_box(m, {-0.62f, 0.54f, -0.42f}, {0.62f, 0.78f, -0.36f}, wood * 0.9f);      // sides
            add_box(m, {-0.62f, 0.54f, 0.36f}, {0.62f, 0.78f, 0.42f}, wood * 0.9f);
            add_box(m, {0.56f, 0.54f, -0.42f}, {0.62f, 0.78f, 0.42f}, wood * 0.9f);
            add_box(m, {-0.62f, 0.54f, -0.42f}, {-0.56f, 0.78f, 0.42f}, wood * 0.9f);
            for (f32 sz : {-0.48f, 0.48f}) { // two spoked wheels
                add_box(m, {-0.16f, 0.02f, sz - 0.05f}, {0.16f, 0.5f, sz + 0.05f}, dark);
                add_box(m, {-0.24f, 0.1f, sz - 0.05f}, {0.24f, 0.42f, sz + 0.05f}, dark);
            }
            add_box(m, {-1.3f, 0.5f, -0.3f}, {-0.62f, 0.56f, -0.24f}, dark); // handles
            add_box(m, {-1.3f, 0.5f, 0.24f}, {-0.62f, 0.56f, 0.3f}, dark);
            add_box(m, {-0.58f, 0.0f, -0.05f}, {-0.5f, 0.42f, 0.05f}, dark);  // prop leg
            const Vec3 leaf{0.27f, 0.50f, 0.21f};
            add_box(m, {-0.56f, 0.54f, -0.36f}, {0.56f, 0.74f, 0.36f}, leaf * 0.85f); // a mound of greenery
            for (int i = 0; i < 18; ++i) {
                const f32 fx = -0.5f + 1.0f * hashf(static_cast<u32>(i) * 17u + 3u);
                const f32 fz = -0.3f + 0.6f * hashf(static_cast<u32>(i) * 29u + 11u);
                const f32 fy = 0.74f + 0.12f * hashf(static_cast<u32>(i) * 5u + 7u);
                add_box(m, {fx - 0.06f, fy - 0.04f, fz - 0.06f}, {fx + 0.06f, fy + 0.06f, fz + 0.06f},
                        blooms[static_cast<u32>(i) % 6u]);
            }
            collider(0.7f, 0.5f, 0.8f);
            break;
        }
        case kDecorBannerRed:
        case kDecorBannerBlue: { // a tall pole hung with a swallow-tailed heraldic banner
            def.name = "banner";
            const bool red = (variant % static_cast<int>(kDecorVariants)) == kDecorBannerRed;
            const Vec3 cloth = red ? Vec3{0.60f, 0.12f, 0.12f} : Vec3{0.16f, 0.26f, 0.58f};
            const Vec3 gold{0.80f, 0.62f, 0.24f};
            add_box(m, {-0.07f, 0.0f, -0.07f}, {0.07f, 4.2f, 0.07f}, dark);             // pole
            add_box(m, {-0.13f, 0.0f, -0.13f}, {0.13f, 0.3f, 0.13f}, stone);             // stone footing
            add_box(m, {-0.5f, 3.86f, -0.04f}, {0.5f, 3.94f, 0.04f}, dark);             // crossbar
            add_box(m, {-0.06f, 4.2f, -0.06f}, {0.06f, 4.42f, 0.06f}, gold);             // finial
            add_box(m, {-0.42f, 2.15f, 0.06f}, {0.42f, 3.86f, 0.1f}, cloth);             // the banner
            add_box(m, {-0.44f, 3.68f, 0.05f}, {0.44f, 3.78f, 0.11f}, gold);             // gold top band
            add_box(m, {-0.36f, 2.15f, 0.05f}, {-0.3f, 3.68f, 0.11f}, gold * 0.92f);     // side trims
            add_box(m, {0.3f, 2.15f, 0.05f}, {0.36f, 3.68f, 0.11f}, gold * 0.92f);
            // A gold lozenge emblem in the middle, on both faces of the cloth.
            for (const f32 sz : {0.115f, 0.045f}) {
                const Vec3 T{0.0f, 3.25f, sz}, B{0.0f, 2.75f, sz}, L{-0.2f, 3.0f, sz}, R{0.2f, 3.0f, sz};
                const Vec3 inside{0.0f, 3.0f, 0.08f}; // the cloth's mid-plane: normals face away from it
                emit_tri(m, T, R, B, inside, gold);
                emit_tri(m, T, B, L, inside, gold);
            }
            // The swallow-tail: two hanging points below the banner.
            for (f32 sx : {-1.0f, 1.0f}) {
                const Vec3 a{sx * 0.42f, 2.15f, 0.08f}, b{0.0f, 2.15f, 0.08f}, tip{sx * 0.24f, 1.7f, 0.08f};
                emit_tri(m, a, b, tip, Vec3{sx * 0.2f, 1.9f, 0.0f}, cloth * 0.92f);
                emit_tri(m, a, b, tip, Vec3{sx * 0.2f, 1.9f, 0.3f}, cloth * 0.92f);
            }
            collider(0.14f, 0.14f, 2.0f);
            break;
        }
        case kDecorNoticeBoard: { // the town notice board: posts, a shingled hood, pinned notices
            def.name = "notice_board";
            const Vec3 parch{0.80f, 0.74f, 0.58f};
            for (f32 ex : {-0.82f, 0.82f}) {
                add_box(m, {ex - 0.08f, 0.0f, -0.08f}, {ex + 0.08f, 2.3f, 0.08f}, dark); // posts
            }
            add_box(m, {-0.76f, 0.75f, -0.05f}, {0.76f, 1.95f, 0.05f}, wood);           // the board
            add_box(m, {-0.8f, 0.7f, -0.07f}, {0.8f, 0.78f, 0.07f}, dark);              // frame
            add_box(m, {-0.8f, 1.93f, -0.07f}, {0.8f, 2.0f, 0.07f}, dark);
            // A little two-slope shingle hood keeping the rain off.
            const Vec3 roofc{0.36f, 0.22f, 0.14f};
            const Vec3 below{0.0f, 1.6f, 0.0f};
            emit_tri(m, {-1.0f, 2.3f, 0.45f}, {1.0f, 2.3f, 0.45f}, {1.0f, 2.62f, 0.0f}, below, roofc);
            emit_tri(m, {-1.0f, 2.3f, 0.45f}, {1.0f, 2.62f, 0.0f}, {-1.0f, 2.62f, 0.0f}, below, roofc);
            emit_tri(m, {-1.0f, 2.3f, -0.45f}, {1.0f, 2.3f, -0.45f}, {1.0f, 2.62f, 0.0f}, below, roofc * 0.9f);
            emit_tri(m, {-1.0f, 2.3f, -0.45f}, {1.0f, 2.62f, 0.0f}, {-1.0f, 2.62f, 0.0f}, below, roofc * 0.9f);
            // Pinned notices (contracts, bounties, a wanted poster) on the front face.
            const f32 notes[6][4] = {{-0.66f, 1.45f, 0.26f, 0.36f}, {-0.3f, 1.5f, 0.24f, 0.3f},
                                     {0.08f, 1.38f, 0.3f, 0.42f},  {0.46f, 1.5f, 0.22f, 0.3f},
                                     {-0.52f, 0.9f, 0.3f, 0.36f},  {0.2f, 0.88f, 0.34f, 0.34f}};
            for (int i = 0; i < 6; ++i) {
                const f32* n = notes[i];
                const Vec3 pc = parch * (0.9f + 0.18f * hashf(static_cast<u32>(i) * 3u + 1u));
                add_box(m, {n[0], n[1], 0.05f}, {n[0] + n[2], n[1] + n[3], 0.07f}, pc);
                add_box(m, {n[0] + n[2] * 0.42f, n[1] + n[3] - 0.07f, 0.065f},
                        {n[0] + n[2] * 0.58f, n[1] + n[3] - 0.03f, 0.085f}, Vec3{0.62f, 0.12f, 0.1f}); // wax seal
                for (int ln = 0; ln < 3; ++ln) { // inked lines
                    const f32 ly = n[1] + n[3] * (0.25f + 0.18f * static_cast<f32>(ln));
                    add_box(m, {n[0] + 0.04f, ly, 0.068f}, {n[0] + n[2] - 0.05f, ly + 0.02f, 0.075f},
                            Vec3{0.22f, 0.18f, 0.14f});
                }
            }
            collider(0.9f, 0.12f, 2.0f);
            break;
        }
        case kDecorFlowerBarrel: { // a half-barrel planter overflowing with blooms
            def.name = "flower_barrel";
            static const Vec3 blooms[5] = {{0.80f, 0.16f, 0.18f}, {0.88f, 0.72f, 0.18f}, {0.58f, 0.30f, 0.72f},
                                           {0.84f, 0.82f, 0.76f}, {0.88f, 0.42f, 0.58f}};
            belled_barrel(m, Vec3{0.0f}, 0.34f, 0.5f, wood, dark);
            const Vec3 leaf{0.26f, 0.5f, 0.2f};
            add_box(m, {-0.28f, 0.5f, -0.28f}, {0.28f, 0.62f, 0.28f}, leaf * 0.85f);
            for (int i = 0; i < 9; ++i) {
                const f32 a = static_cast<f32>(i) * 2.399f;
                const f32 rr = 0.08f + 0.17f * hashf(static_cast<u32>(i) * 11u + 5u);
                const f32 fx = std::cos(a) * rr, fz = std::sin(a) * rr;
                const f32 ht = 0.66f + 0.14f * hashf(static_cast<u32>(i) * 7u + 2u);
                add_box(m, {fx - 0.03f, 0.6f, fz - 0.03f}, {fx + 0.03f, ht, fz + 0.03f}, leaf);
                add_box(m, {fx - 0.06f, ht, fz - 0.06f}, {fx + 0.06f, ht + 0.08f, fz + 0.06f}, blooms[i % 5]);
            }
            collider(0.34f, 0.34f, 0.5f);
            break;
        }
        case kDecorBunting: { // a sagging string of pennants along local x (-0.5..0.5), stretched to span
            def.name = "bunting";
            static const Vec3 flags[5] = {{0.70f, 0.14f, 0.14f}, {0.84f, 0.68f, 0.18f}, {0.18f, 0.32f, 0.66f},
                                          {0.20f, 0.50f, 0.22f}, {0.82f, 0.80f, 0.74f}};
            constexpr f32 top = 3.92f, sag = 0.55f;
            constexpr int segs = 16;
            auto rope_y = [&](f32 x) { return top - sag * (1.0f - 4.0f * x * x); };
            for (int i = 0; i < segs; ++i) {
                const f32 x0 = -0.5f + static_cast<f32>(i) / segs, x1 = -0.5f + static_cast<f32>(i + 1) / segs;
                const f32 y = (rope_y(x0) + rope_y(x1)) * 0.5f;
                add_box(m, {x0, y - 0.018f, -0.018f}, {x1, y + 0.018f, 0.018f}, dark);
            }
            constexpr int pennants = 15;
            for (int i = 0; i < pennants; ++i) {
                const f32 xc = -0.5f + (static_cast<f32>(i) + 0.5f) / pennants;
                const f32 hwid = 0.42f / pennants;
                const f32 y = rope_y(xc);
                const Vec3 a{xc - hwid, rope_y(xc - hwid), 0.0f}, b{xc + hwid, rope_y(xc + hwid), 0.0f};
                const Vec3 tip{xc, y - 0.42f, 0.0f};
                const Vec3 col = flags[static_cast<u32>(i) % 5u];
                emit_tri(m, a, b, tip, Vec3{xc, y - 0.2f, -1.0f}, col); // both faces (it's a cloth)
                emit_tri(m, a, b, tip, Vec3{xc, y - 0.2f, 1.0f}, col * 0.9f);
            }
            break; // no collider - it hangs well overhead
        }
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
        add_quad(m, {x0, d0, hw}, {x1, d1, hw}, {x1, d1, -hw}, {x0, d0, -hw}, cobble); // roadway (CCW: normal up)
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
            add_quad(m, {x0, d0 + 0.56f, z + 0.14f}, {x1, d1 + 0.56f, z + 0.14f},
                     {x1, d1 + 0.56f, z - 0.14f}, {x0, d0 + 0.56f, z - 0.14f}, dark); // parapet cap (CCW: normal up)
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
    // Match the reflective open-water palette (a clean medium blue), not the old muddy teal, so the
    // town canal reads as the same water as the lakes/sea rather than a saturated strip.
    const Vec3 water{0.12f, 0.40f, 0.58f};
    const Vec3 stone{0.48f, 0.47f, 0.45f};
    const Vec3 earth{0.30f, 0.23f, 0.15f};
    MeshData op;
    MeshData em;
    constexpr f32 hl = 2.0f;  // half-length along the river (local x)
    constexpr f32 wb = 3.0f;  // water half-width
    constexpr f32 bank = 3.6f; // outer bank half-width

    // Water surface, just above the ground plane (hides the flat terrain beneath it).
    add_box(op, {-hl, 0.0f, -wb}, {hl, 0.1f, wb}, water);
    // A soft brighter sheen down the middle - kept gentle + close to the water colour so the
    // repeated per-tile strip doesn't read as a hard segmented bar down the canal.
    add_box(em, {-hl, 0.11f, -1.0f}, {hl, 0.12f, 1.0f}, Vec3{0.20f, 0.46f, 0.60f});

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
    // A domed, 8-sided mushroom cap (a lower flare band + a top cone to the apex) - reads as a
    // toadstool cap, not a box. emit_tri orients each face's normal away from the cap centre.
    auto glow_cap = [](MeshData& dst, f32 cx, f32 cz, f32 base_y, f32 r, f32 h, const Vec3& col) {
        constexpr int sides = 8;
        const Vec3 ctr{cx, base_y + h * 0.45f, cz};
        const Vec3 apex{cx, base_y + h, cz};
        const f32 r1 = r * 0.62f, y1 = base_y + h * 0.62f;
        for (int s = 0; s < sides; ++s) {
            const f32 a0 = TwoPi * static_cast<f32>(s) / sides, a1 = TwoPi * static_cast<f32>(s + 1) / sides;
            const Vec3 b0{cx + std::cos(a0) * r, base_y, cz + std::sin(a0) * r};
            const Vec3 b1{cx + std::cos(a1) * r, base_y, cz + std::sin(a1) * r};
            const Vec3 m0{cx + std::cos(a0) * r1, y1, cz + std::sin(a0) * r1};
            const Vec3 m1{cx + std::cos(a1) * r1, y1, cz + std::sin(a1) * r1};
            emit_tri(dst, b0, b1, m1, ctr, col); // lower flare band (two facets)
            emit_tri(dst, b0, m1, m0, ctr, col);
            emit_tri(dst, m0, m1, apex, ctr, col); // top cone to the apex
        }
    };
    const int n = 3 + static_cast<int>(rnd(1) * 3.0f);
    for (int i = 0; i < n; ++i) {
        const f32 ang = TwoPi * (static_cast<f32>(i) / static_cast<f32>(n)) + rnd(i * 5 + 2);
        const f32 rad = (i == 0) ? 0.0f : 0.18f + rnd(i * 5 + 3) * 0.28f;
        const f32 cxx = std::cos(ang) * rad, czz = std::sin(ang) * rad;
        const f32 sc = (i == 0 ? 1.0f : 0.55f + rnd(i * 5 + 4) * 0.5f);
        const f32 sh = 0.32f * sc, cr = 0.2f * sc;
        add_box(op, {cxx - 0.05f * sc, 0.0f, czz - 0.05f * sc}, {cxx + 0.05f * sc, sh, czz + 0.05f * sc}, stem);
        // glowing DOMED cap (emissive toadstool, not a box)
        glow_cap(em, cxx, czz, sh, cr, 0.22f * sc, glow);
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
    // Layered TAPERED flame tongues (emissive pyramids to a point) rising from the embers - orange
    // outside, yellow inside, a white-hot core - so the fire reads as flickering flames, not a box.
    auto flame = [&](f32 cx, f32 cz, f32 base_r, f32 h, const Vec3& col) {
        const Vec3 apex{cx, h, cz};
        const Vec3 ctr{cx, h * 0.45f, cz}; // for emit_tri's outward-normal orientation
        const Vec3 b0{cx - base_r, 0.16f, cz - base_r}, b1{cx + base_r, 0.16f, cz - base_r};
        const Vec3 b2{cx + base_r, 0.16f, cz + base_r}, b3{cx - base_r, 0.16f, cz + base_r};
        emit_tri(em, b0, b1, apex, ctr, col);
        emit_tri(em, b1, b2, apex, ctr, col);
        emit_tri(em, b2, b3, apex, ctr, col);
        emit_tri(em, b3, b0, apex, ctr, col);
    };
    for (int i = 0; i < 5; ++i) {
        const f32 a = TwoPi * static_cast<f32>(i) / 5.0f;
        const f32 fx = std::cos(a) * 0.14f, fz = std::sin(a) * 0.14f;
        const f32 fh = 0.40f + 0.30f * std::abs(std::sin(static_cast<f32>(i) * 1.7f));
        flame(fx, fz, 0.11f, fh, fire * (0.92f + 0.12f * static_cast<f32>(i % 2))); // outer orange tongues
    }
    flame(0.0f, 0.02f, 0.11f, 0.92f, Vec3{1.35f, 0.85f, 0.28f}); // tall yellow inner tongue
    flame(0.0f, 0.0f, 0.07f, 1.05f, Vec3{1.7f, 1.15f, 0.5f});    // white-hot core
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
    MeshData em;          // emissive glyphs (only the carved obelisk glows)
    bool glowing = false; // -> push the emissive part + a soft arcane light
    auto rnd = [&](u32 s) {
        u32 v = (static_cast<u32>(variant) * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    f32 cr = 0.7f;
    if (variant % kMonumentVariants == 0) {
        // a carved obelisk on a stepped plinth - masonry-textured, with a glowing ancient rune
        add_box(m, {-0.85f, 0.0f, -0.85f}, {0.85f, 0.28f, 0.85f}, stone * 0.95f);
        add_box(m, {-0.62f, 0.28f, -0.62f}, {0.62f, 0.52f, 0.62f}, stone);
        add_box(m, {-0.34f, 0.52f, -0.34f}, {0.34f, 3.6f, 0.34f}, stone * 1.04f); // shaft core
        stone_face(m, true, 0.34f, 1.0f, -0.34f, 0.34f, 0.52f, 3.5f, stone, 11u);   // +z masonry
        stone_face(m, true, -0.34f, -1.0f, -0.34f, 0.34f, 0.52f, 3.5f, stone, 23u); // -z
        stone_face(m, false, 0.34f, 1.0f, -0.34f, 0.34f, 0.52f, 3.5f, stone, 37u);  // +x
        stone_face(m, false, -0.34f, -1.0f, -0.34f, 0.34f, 0.52f, 3.5f, stone, 51u); // -x
        // a small pyramidal cap
        const Vec3 apex{0.0f, 4.05f, 0.0f};
        add_tri(m, {-0.34f, 3.6f, 0.34f}, {0.34f, 3.6f, 0.34f}, apex, stone * 1.06f);
        add_tri(m, {0.34f, 3.6f, -0.34f}, {-0.34f, 3.6f, -0.34f}, apex, stone * 0.96f);
        add_tri(m, {0.34f, 3.6f, 0.34f}, {0.34f, 3.6f, -0.34f}, apex, stone);
        add_tri(m, {-0.34f, 3.6f, -0.34f}, {-0.34f, 3.6f, 0.34f}, apex, stone);
        add_box(m, {-0.36f, 0.5f, -0.36f}, {0.0f, 0.9f, -0.32f}, moss); // moss patch
        // Glowing carved rune: an emissive band ringing the shaft + a vertical glyph, proud of the
        // masonry (an arcane cyan), so it reads as ancient magic and pools soft light at night.
        const Vec3 rune{0.34f, 0.82f, 0.95f};
        add_box(em, {-0.30f, 1.66f, 0.355f}, {0.30f, 1.95f, 0.40f}, rune);   // +z band
        add_box(em, {-0.30f, 1.66f, -0.40f}, {0.30f, 1.95f, -0.355f}, rune); // -z band
        add_box(em, {0.355f, 1.66f, -0.30f}, {0.40f, 1.95f, 0.30f}, rune);   // +x band
        add_box(em, {-0.40f, 1.66f, -0.30f}, {-0.355f, 1.95f, 0.30f}, rune); // -x band
        add_box(em, {-0.05f, 1.30f, 0.355f}, {0.05f, 2.35f, 0.40f}, rune);   // vertical glyph (front)
        add_box(em, {-0.05f, 1.30f, -0.40f}, {0.05f, 2.35f, -0.355f}, rune); // vertical glyph (back)
        glowing = true;
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
    if (glowing) {
        def.parts.push_back({std::move(em), PropLayer::Emissive});
        PropLight l;
        l.offset = Vec3{0.0f, 1.85f, 0.0f};
        l.direction = glm::normalize(Vec3{0.0f, -1.0f, 0.0f});
        l.color = Vec3{0.30f, 0.66f, 0.82f}; // soft arcane glow
        l.range = 9.0f;
        l.intensity = 1.1f;
        l.cone_deg = 150.0f;
        def.lights.push_back(l);
    }
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
    // Stepped (tiered) wooden roof - reads as a tiled pagoda-ish cap, not a flat pyramid slab.
    const f32 br = r + 0.25f;
    add_box(m, {-br, ry, -br}, {br, ry + 0.22f, br}, roof);
    add_box(m, {-br * 0.66f, ry + 0.20f, -br * 0.66f}, {br * 0.66f, ry + 0.44f, br * 0.66f}, roof * 0.93f);
    add_box(m, {-br * 0.34f, ry + 0.42f, -br * 0.34f}, {br * 0.34f, ry + 0.66f, br * 0.34f}, roof * 1.06f);
    // a flagpole + red pennant on the peak (a watchtower signal), drawn both sides
    add_box(m, {-0.04f, ry + 0.66f, -0.04f}, {0.04f, ry + 1.5f, 0.04f}, dark);
    add_tri(m, {0.04f, ry + 1.42f, 0.0f}, {0.04f, ry + 1.12f, 0.0f}, {0.52f, ry + 1.27f, 0.0f},
            Vec3{0.72f, 0.2f, 0.16f});
    add_tri(m, {0.04f, ry + 1.12f, 0.0f}, {0.04f, ry + 1.42f, 0.0f}, {0.52f, ry + 1.27f, 0.0f},
            Vec3{0.6f, 0.16f, 0.13f});
    // a ladder climbing the +x face from the ground up to the platform (two rails + rungs)
    const f32 lx = r + 0.32f;
    for (const f32 lz : {-0.32f, 0.32f}) {
        add_box(m, {lx - 0.04f, 0.0f, lz - 0.04f}, {lx + 0.04f, ph + 0.1f, lz + 0.04f}, dark);
    }
    for (f32 lr = 0.35f; lr < ph; lr += 0.42f) {
        add_box(m, {lx - 0.06f, lr, -0.34f}, {lx + 0.07f, lr + 0.06f, 0.34f}, wood * 0.9f);
    }
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
            add_quad(m, {x0, y0 + 0.5f, hw}, {x1, y1 + 0.5f, hw}, {x1, y1 + 0.5f, -hw}, {x0, y0 + 0.5f, -hw},
                     stone); // deck top (CCW: normal up)
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

namespace {

// Appends `src` (any primitive) to `dst` through the transform `xf` (normals rotated with it).
void add_mesh(MeshData& dst, const MeshData& src, const Mat4& xf, const Vec3& color) {
    const Mat3 nrm = glm::transpose(glm::inverse(Mat3{xf}));
    const u32 base = static_cast<u32>(dst.vertices.size());
    for (const Vertex& v : src.vertices) {
        dst.vertices.push_back({Vec3{xf * Vec4{v.position, 1.0f}}, glm::normalize(nrm * v.normal), color, 0.0f});
    }
    for (u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
}
// An upright `sides`-sided prism (a round tower / post) centred on (c.x, c.z), from y0 to y1.
void add_prism(MeshData& m, const Vec3& c, f32 r, f32 y0, f32 y1, int sides, const Vec3& col) {
    const Vec3 mid{c.x, (y0 + y1) * 0.5f, c.z};
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 p0{c.x + std::cos(a0) * r, y0, c.z + std::sin(a0) * r};
        const Vec3 p1{c.x + std::cos(a1) * r, y0, c.z + std::sin(a1) * r};
        const Vec3 q0{p0.x, y1, p0.z}, q1{p1.x, y1, p1.z};
        const Vec3 shade = col * (0.9f + 0.12f * static_cast<f32>(i % 2));
        emit_tri(m, p0, p1, q1, mid, shade);
        emit_tri(m, p0, q1, q0, mid, shade);
        emit_tri(m, q0, q1, Vec3{c.x, y1, c.z}, Vec3{c.x, y1 - 1.0f, c.z}, col); // top cap
    }
}
// A cone / spire (`sides` facets) on a base circle at height y0, rising to its point at y0 + h.
void add_cone(MeshData& m, const Vec3& c, f32 r, f32 y0, f32 h, int sides, const Vec3& col) {
    const Vec3 apex{c.x, y0 + h, c.z};
    const Vec3 inner{c.x, y0 + h * 0.3f, c.z};
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 p0{c.x + std::cos(a0) * r, y0, c.z + std::sin(a0) * r};
        const Vec3 p1{c.x + std::cos(a1) * r, y0, c.z + std::sin(a1) * r};
        emit_tri(m, p0, p1, apex, inner, col * (0.88f + 0.14f * static_cast<f32>(i % 2)));
    }
}
// An iron fire basket at height `y` (bowl + glowing coals + licking flame tongues) into op / em.
void add_fire_basket(MeshData& op, MeshData& em, const Vec3& c, f32 scale) {
    const Vec3 iron{0.17f, 0.15f, 0.15f};
    const Vec3 coal{1.0f, 0.5f, 0.16f};
    const Vec3 fire{1.0f, 0.6f, 0.2f};
    const f32 s = scale;
    const Vec3 ctr{c.x, c.y + 0.14f * s, c.z};
    for (int i = 0; i < 8; ++i) { // the bowl, flaring out to its rim
        const f32 a0 = TwoPi * static_cast<f32>(i) / 8.0f, a1 = TwoPi * static_cast<f32>(i + 1) / 8.0f;
        const Vec3 b0 = c + Vec3{std::cos(a0) * 0.1f * s, 0.0f, std::sin(a0) * 0.1f * s};
        const Vec3 b1 = c + Vec3{std::cos(a1) * 0.1f * s, 0.0f, std::sin(a1) * 0.1f * s};
        const Vec3 t0 = c + Vec3{std::cos(a0) * 0.24f * s, 0.3f * s, std::sin(a0) * 0.24f * s};
        const Vec3 t1 = c + Vec3{std::cos(a1) * 0.24f * s, 0.3f * s, std::sin(a1) * 0.24f * s};
        emit_tri(op, b0, b1, t1, ctr, iron);
        emit_tri(op, b0, t1, t0, ctr, iron);
    }
    add_box(em, c + Vec3{-0.18f, 0.17f, -0.18f} * s, c + Vec3{0.18f, 0.29f, 0.18f} * s, coal);
    auto tongue = [&](const Vec3& base, f32 h, f32 w, const Vec3& col) {
        const Vec3 tip = base + Vec3{0.0f, h, 0.0f};
        const Vec3 a = base + Vec3{-w, 0.0f, -w}, b = base + Vec3{w, 0.0f, -w};
        const Vec3 cc = base + Vec3{w, 0.0f, w}, d = base + Vec3{-w, 0.0f, w};
        emit_tri(em, a, b, tip, base, col);
        emit_tri(em, b, cc, tip, base, col);
        emit_tri(em, cc, d, tip, base, col);
        emit_tri(em, d, a, tip, base, col);
    };
    tongue(c + Vec3{0.0f, 0.27f, 0.0f} * s, 0.5f * s, 0.13f * s, fire);
    tongue(c + Vec3{0.09f, 0.27f, 0.05f} * s, 0.32f * s, 0.08f * s, fire * 0.95f);
    tongue(c + Vec3{-0.08f, 0.27f, -0.06f} * s, 0.36f * s, 0.08f * s, Vec3{1.3f, 0.85f, 0.3f});
}

// Lays SNOW over every sky-facing surface of a def above `min_y`: each upward-facing triangle of its
// opaque / roof-shell parts gets a slightly lifted, cool-white twin (in the same layer, so a house's
// snowy roof still fades with its shell when you step inside). Kept below the renderer's blow-out
// range + leaning blue, so it reads as snow and not cream (see worldgen snow).
void add_snow(PropDef& def, f32 min_y) {
    std::vector<PropPart> caps;
    for (const PropPart& part : def.parts) {
        if (part.layer != PropLayer::Opaque && part.layer != PropLayer::Roof) {
            continue;
        }
        MeshData snow;
        const MeshData& m = part.mesh;
        for (usize i = 0; i + 2 < m.indices.size(); i += 3) {
            const Vertex& a = m.vertices[m.indices[i]];
            const Vertex& b = m.vertices[m.indices[i + 1]];
            const Vertex& c = m.vertices[m.indices[i + 2]];
            const Vec3 n = a.normal;
            if (n.y < 0.42f || std::min({a.position.y, b.position.y, c.position.y}) < min_y) {
                continue; // walls, undersides + everything down at ground level stay clear
            }
            const f32 lift = 0.035f + 0.05f * n.y;
            const Vec3 off = n * lift + Vec3{0.0f, 0.02f, 0.0f};
            const f32 j = hashf(static_cast<u32>(std::lround((a.position.x + a.position.z) * 31.0f + a.position.y * 17.0f)));
            const Vec3 col = glm::mix(Vec3{0.56f, 0.64f, 0.82f}, Vec3{0.68f, 0.75f, 0.92f}, glm::smoothstep(0.42f, 0.9f, n.y)) *
                             (0.95f + 0.08f * j);
            const u32 base = static_cast<u32>(snow.vertices.size());
            snow.vertices.push_back({a.position + off, n, col, 0.0f});
            snow.vertices.push_back({b.position + off, n, col, 0.0f});
            snow.vertices.push_back({c.position + off, n, col, 0.0f});
            snow.indices.insert(snow.indices.end(), {base, base + 1, base + 2});
        }
        if (!snow.indices.empty()) {
            caps.push_back({std::move(snow), part.layer});
        }
    }
    for (PropPart& p : caps) {
        def.parts.push_back(std::move(p));
    }
}

// The snowbound twin of a town building: snow on its roof and every ledge, a lantern hung by the door
// against the long nights and a hearth stoked up for the cold (a warmer, brighter fire glow).
void make_snowy_house(PropDef& def) {
    // Everything sky-facing from head height up: the roofs (porches + lean-tos included), chimney caps,
    // dormers, hoods - but not the sills, steps + yard clutter below (nor the furniture indoors).
    add_snow(def, 1.6f);
    // Firelit windows glow warmer + brighter against the long winter nights.
    for (PropPart& part : def.parts) {
        if (part.layer == PropLayer::Emissive) {
            for (Vertex& v : part.mesh.vertices) {
                v.color *= Vec3{1.3f, 1.18f, 1.0f};
            }
        }
    }
    MeshData op, em;
    const Vec3 at{0.95f, 2.05f, def.door_spot.z - 0.58f};
    add_box(op, at - Vec3{0.04f, 0.26f, 0.04f}, at + Vec3{0.04f, 0.3f, 0.04f}, Vec3{0.16f, 0.13f, 0.1f});
    add_box(op, at + Vec3{-0.03f, 0.26f, -0.3f}, at + Vec3{0.03f, 0.31f, 0.0f}, Vec3{0.16f, 0.13f, 0.1f}); // bracket
    add_box(em, at - Vec3{0.08f, 0.11f, 0.08f}, at + Vec3{0.08f, 0.11f, 0.08f}, Vec3{1.5f, 1.12f, 0.55f});
    PropLight l;
    l.offset = at + Vec3{0.0f, 0.0f, 0.2f};
    l.direction = glm::normalize(Vec3{0.0f, -0.6f, 1.0f});
    l.color = Vec3{1.0f, 0.74f, 0.42f};
    l.range = 9.0f;
    l.intensity = 2.2f;
    l.cone_deg = 150.0f;
    l.spill = true;
    for (PropLight& h : def.lights) {
        h.intensity *= 1.25f; // stoked for the cold
    }
    def.lights.push_back(l);
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
}

// Bake per-def vertex AO: every Opaque/Roof part is darkened by hemisphere rays cast
// against all Opaque/Roof parts of the same def, so eaves shade walls, doorways fall
// dark and clutter sits INTO its surroundings instead of floating on them. Emissive/
// Glow parts (lit windows, lantern glass) neither receive nor occlude, and Foliage
// keeps its airy alpha-blended look.
void bake_def_ao(PropDef& def) {
    std::vector<const MeshData*> occluders;
    for (const PropPart& p : def.parts) {
        if (p.layer == PropLayer::Opaque || p.layer == PropLayer::Roof) {
            occluders.push_back(&p.mesh);
        }
    }
    if (occluders.empty()) {
        return;
    }
    for (PropPart& p : def.parts) {
        if (p.layer == PropLayer::Opaque || p.layer == PropLayer::Roof || p.layer == PropLayer::Door) {
            p.mesh.bake_vertex_ao(occluders);
        }
    }
}

} // namespace

// A stone CHAPEL: a tall nave (long along z) under a steep slate roof, buttressed, with lit lancet
// windows down its sides, and a square bell tower over the west door crowned with an octagonal spire
// and a gilded cross. Through its double doors you step into the tower's vestibule (the bell rope hangs
// there) and on through an arch into the nave: pews either side of the aisle and an altar lit with
// candles under a great gilded cross. Every town and city raises one; the door lantern lights its steps.
PropDef PropLibrary::build_chapel() {
    PropDef def;
    def.name = "chapel";
    MeshData shell, op, em, glow;
    const Vec3 stone{0.62f, 0.62f, 0.63f};
    const Vec3 slate{0.30f, 0.34f, 0.42f};
    const Vec3 trim{0.20f, 0.18f, 0.18f};
    const Vec3 dark{0.12f, 0.1f, 0.1f};
    const Vec3 glass{1.05f, 0.74f, 0.42f};
    const Vec3 gold{0.86f, 0.68f, 0.26f};
    const Vec3 wood{0.36f, 0.23f, 0.13f};
    const f32 w = 2.9f, d = 5.0f, h = 4.4f, t = 0.3f;
    const f32 tz0 = d - 3.0f, tz1 = d; // the tower over the front (+z) end
    const f32 tw = 1.5f, th = 9.0f;
    const f32 dw = 0.62f, dh = 2.4f; // the west door
    const f32 aw = 0.75f, ah = 2.75f; // the arch from the vestibule into the nave
    const f32 vh = 3.3f;              // the vestibule's ceiling
    auto wall = [&](const Vec3& lo, const Vec3& hi, const Vec3& col) {
        add_box(shell, lo, hi, col);
        add_collider(def, lo, hi);
    };

    // ---- A flagged floor through the nave + vestibule, over dark joints.
    add_box(op, {-w, -0.3f, -d}, {w, 0.02f, tz1}, stone * 0.35f);
    {
        const int nx = 7, nz = 16;
        const f32 sx = 2.0f * (w - t) / static_cast<f32>(nx), sz = (tz1 - t + d - t) / static_cast<f32>(nz);
        for (int j = 0; j < nz; ++j) {
            for (int i = 0; i < nx; ++i) {
                const f32 x0 = -w + t + static_cast<f32>(i) * sx, z0 = -d + t + static_cast<f32>(j) * sz;
                add_box(op, {x0 + 0.03f, 0.02f, z0 + 0.03f}, {x0 + sx - 0.03f, 0.05f, z0 + sz - 0.03f},
                        stone * (0.78f + 0.2f * hashf(900u + static_cast<u32>(j * 13 + i))));
            }
        }
    }

    // ---- The nave: dressed-stone walls on a low plinth, buttressed down both sides. Its front wall
    // opens through an arch into the tower's vestibule.
    wall({-w, 0.0f, -d}, {-w + t, h, tz0}, stone);   // left
    wall({w - t, 0.0f, -d}, {w, h, tz0}, stone);     // right
    wall({-w, 0.0f, -d}, {w, h, -d + t}, stone);     // back (the altar end)
    wall({-w, 0.0f, tz0 - t}, {-aw, h, tz0}, stone); // front, either side of the arch
    wall({aw, 0.0f, tz0 - t}, {w, h, tz0}, stone);
    add_box(shell, {-aw, ah, tz0 - t}, {aw, h, tz0}, stone); // over the arch
    stone_face(shell, false, w, 1.0f, -d, tz0, 0.0f, h, stone, 301u);
    stone_face(shell, false, -w, -1.0f, -d, tz0, 0.0f, h, stone, 302u);
    stone_face(shell, true, -d, -1.0f, -w, w, 0.0f, h, stone, 303u);
    add_box(op, {-w - 0.12f, 0.0f, -d - 0.12f}, {w + 0.12f, 0.45f, -d}, stone * 0.92f); // plinth: back
    add_box(op, {-w - 0.12f, 0.0f, -d}, {-w, 0.45f, tz0}, stone * 0.92f);              //   left
    add_box(op, {w, 0.0f, -d}, {w + 0.12f, 0.45f, tz0}, stone * 0.92f);                //   right
    for (const f32 z : {-d + 0.2f, -d + 2.3f, 0.4f, tz0 - 0.3f}) { // buttresses down both sides
        for (const f32 sx : {-1.0f, 1.0f}) {
            const f32 x0 = sx > 0.0f ? w : -w - 0.32f, x1 = sx > 0.0f ? w + 0.32f : -w;
            add_box(shell, {x0, 0.0f, z - 0.22f}, {x1, h * 0.8f, z + 0.22f}, stone * 0.94f);
            add_box(shell, {sx > 0.0f ? w : -w - 0.2f, h * 0.8f, z - 0.2f}, {sx > 0.0f ? w + 0.2f : -w, h * 0.92f, z + 0.2f},
                    stone * 0.9f);
            add_collider(def, {x0, 0.0f, z - 0.22f}, {x1, h * 0.8f, z + 0.22f});
        }
    }
    // Tall lancet windows between the buttresses, glowing warm (candles within) inside and out, each
    // spilling its light onto the ground below (the middle pair light it for real).
    for (const f32 z : {-d + 1.25f, -0.75f, 1.35f}) {
        for (const f32 sx : {-1.0f, 1.0f}) {
            const f32 x0 = sx > 0.0f ? w - 0.02f : -w - 0.06f, x1 = sx > 0.0f ? w + 0.06f : -w + 0.02f;
            add_box(em, {x0, 1.3f, z - 0.26f}, {x1, 3.1f, z + 0.26f}, glass);
            const f32 i0 = sx > 0.0f ? w - t - 0.04f : -w + t, i1 = sx > 0.0f ? w - t : -w + t + 0.04f;
            add_box(em, {i0, 1.3f, z - 0.26f}, {i1, 3.1f, z + 0.26f}, glass * 0.8f); // the inner face
            add_box(shell, {x0 - 0.02f, 3.1f, z - 0.36f}, {x1 + 0.02f, 3.32f, z + 0.36f}, trim); // hood
            add_box(shell, {x0, 2.1f, z - 0.27f}, {x1 + 0.01f, 2.16f, z + 0.27f}, trim);         // transom
            add_window_spill(def, glow, {sx * w, 2.2f, z}, {0.0f, 0.0f, 1.0f}, {sx, 0.0f, 0.0f}, 0.34f, 1.3f,
                             z == -0.75f);
        }
    }
    // A steep slate roof over the nave: built with its ridge along x (gable_roof's well-trodden path),
    // then turned to run down the nave and slid back over it.
    {
        MeshData roof;
        const f32 nd = (tz0 + d) * 0.5f; // nave half-length
        gable_roof(roof, nd, w, h, 2.8f, 0.45f, false, slate, trim, stone, 77u);
        const Mat4 xf = glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, (tz0 - d) * 0.5f}) *
                        glm::rotate(Mat4{1.0f}, HalfPi, Vec3{0.0f, 1.0f, 0.0f});
        const Mat3 rot{xf};
        for (Vertex& v : roof.vertices) {
            v.position = Vec3{xf * Vec4{v.position, 1.0f}};
            v.normal = rot * v.normal;
        }
        const u32 base = static_cast<u32>(shell.vertices.size());
        shell.vertices.insert(shell.vertices.end(), roof.vertices.begin(), roof.vertices.end());
        for (u32 i : roof.indices) {
            shell.indices.push_back(base + i);
        }
    }

    // ---- The bell tower: hollow at its foot (the vestibule, entered through the west door), a solid
    // stone shaft above, a rose window, an open belfry with its bell, a corbelled parapet + the spire.
    const Vec3 tstone = stone * 0.97f;
    wall({-tw, 0.0f, tz0}, {-tw + t, vh, tz1}, tstone); // left
    wall({tw - t, 0.0f, tz0}, {tw, vh, tz1}, tstone);   // right
    wall({-tw, 0.0f, tz1 - t}, {-dw, vh, tz1}, tstone); // front, either side of the door
    wall({dw, 0.0f, tz1 - t}, {tw, vh, tz1}, tstone);
    add_box(shell, {-dw, dh, tz1 - t}, {dw, vh, tz1}, tstone); // over the door
    add_box(shell, {-tw, vh, tz0}, {tw, th, tz1}, tstone);     // the shaft
    stone_face(shell, true, tz1, 1.0f, -tw, tw, dh + 0.5f, th, stone, 304u);
    stone_face(shell, true, tz1, 1.0f, -tw, -dw - 0.14f, 0.0f, dh + 0.5f, stone, 307u);
    stone_face(shell, true, tz1, 1.0f, dw + 0.14f, tw, 0.0f, dh + 0.5f, stone, 308u);
    stone_face(shell, false, tw, 1.0f, tz0, tz1, 0.0f, th, stone, 305u);
    stone_face(shell, false, -tw, -1.0f, tz0, tz1, 0.0f, th, stone, 306u);
    // The west door: a pointed stone hood over a pair of planked leaves that swing in.
    add_door_frame(op, dw, dh, tz1, trim);
    for (int i = 0; i < 3; ++i) {
        const f32 hw = dw + 0.2f - static_cast<f32>(i) * 0.24f;
        add_box(shell, {-hw, dh + 0.1f + static_cast<f32>(i) * 0.16f, tz1}, {hw, dh + 0.26f + static_cast<f32>(i) * 0.16f, tz1 + 0.12f},
                stone * 0.86f);
    }
    add_door_leaf(def, -dw, 0.0f, tz1 - 0.06f, dh - 0.02f, wood);
    add_door_leaf(def, dw, 0.0f, tz1 - 0.06f, dh - 0.02f, wood);
    add_box(op, {-0.8f, 0.0f, tz1}, {0.8f, 0.14f, tz1 + 0.7f}, stone * 0.9f); // the step
    for (int i = 0; i < 8; ++i) {                                               // the rose window
        const f32 a0 = TwoPi * static_cast<f32>(i) / 8.0f, a1 = TwoPi * static_cast<f32>(i + 1) / 8.0f;
        const Vec3 c{0.0f, 4.2f, tz1 + 0.04f};
        add_tri(em, c, c + Vec3{std::cos(a0) * 0.55f, std::sin(a0) * 0.55f, 0.0f},
                c + Vec3{std::cos(a1) * 0.55f, std::sin(a1) * 0.55f, 0.0f},
                i % 2 == 0 ? glass : glass * Vec3{0.75f, 0.6f, 1.1f});
    }
    add_box(shell, {-0.04f, 3.65f, tz1 + 0.02f}, {0.04f, 4.75f, tz1 + 0.07f}, trim);
    add_box(shell, {-0.55f, 4.16f, tz1 + 0.02f}, {0.55f, 4.24f, tz1 + 0.07f}, trim);
    // The belfry: an opening in each face (dark), the bronze bell hung inside.
    const f32 tzm = (tz0 + tz1) * 0.5f;
    add_box(shell, {-0.5f, 6.6f, tz1 - 0.3f}, {0.5f, 8.0f, tz1 + 0.03f}, dark);
    add_box(shell, {tw - 0.3f, 6.6f, tzm - 0.5f}, {tw + 0.03f, 8.0f, tzm + 0.5f}, dark);
    add_box(shell, {-tw - 0.03f, 6.6f, tzm - 0.5f}, {-tw + 0.3f, 8.0f, tzm + 0.5f}, dark);
    add_cone(shell, Vec3{0.0f, 0.0f, tzm}, 0.42f, 6.9f, 0.75f, 8, gold * 0.85f); // the bell
    add_box(shell, {-tw - 0.12f, th, tz0 - 0.12f}, {tw + 0.12f, th + 0.18f, tz1 + 0.12f}, stone * 1.04f); // parapet
    for (const f32 cx : {-tw, tw}) { // little corner pinnacles
        for (const f32 cz : {tz0, tz1}) {
            add_prism(shell, Vec3{cx, 0.0f, cz}, 0.16f, th + 0.18f, th + 0.6f, 6, stone);
            add_cone(shell, Vec3{cx, 0.0f, cz}, 0.2f, th + 0.6f, 0.55f, 6, slate);
        }
    }
    add_cone(shell, Vec3{0.0f, 0.0f, tzm}, tw * 0.95f, th + 0.18f, 4.6f, 8, slate); // the spire
    const Vec3 cross_c{0.0f, th + 4.75f, tzm};
    add_box(shell, cross_c + Vec3{-0.05f, 0.0f, -0.05f}, cross_c + Vec3{0.05f, 0.75f, 0.05f}, gold);
    add_box(shell, cross_c + Vec3{-0.26f, 0.42f, -0.05f}, cross_c + Vec3{0.26f, 0.52f, 0.05f}, gold);
    add_box(shell, {-tw + t, vh - 0.25f, tz0}, {tw - t, vh, tz1 - t}, wood * 0.8f); // the vestibule's ceiling
    // The bell rope, hanging down through the vestibule ceiling.
    add_box(op, {0.86f, 0.95f, 3.28f}, {0.9f, vh - 0.25f, 3.32f}, Vec3{0.72f, 0.6f, 0.4f});
    add_box(op, {0.83f, 0.75f, 3.25f}, {0.93f, 0.97f, 3.35f}, Vec3{0.62f, 0.2f, 0.18f});

    // ---- The nave within: pews either side of the aisle, and the altar on its dais at the far end with
    // candles on it + tall candlestands, under a great gilded cross.
    for (int i = 0; i < 4; ++i) {
        const f32 zc = -2.9f + static_cast<f32>(i) * 1.05f;
        for (const f32 sx : {-1.0f, 1.0f}) {
            const f32 x0 = sx > 0.0f ? 0.92f : -w + t + 0.15f, x1 = sx > 0.0f ? w - t - 0.15f : -0.92f;
            add_box(op, {x0, 0.42f, zc - 0.22f}, {x1, 0.5f, zc + 0.22f}, wood * 1.1f);   // seat
            add_box(op, {x0, 0.5f, zc + 0.14f}, {x1, 1.0f, zc + 0.22f}, wood);           // back
            add_box(op, {x0, 0.0f, zc - 0.22f}, {x0 + 0.07f, 0.92f, zc + 0.22f}, wood * 0.85f); // ends
            add_box(op, {x1 - 0.07f, 0.0f, zc - 0.22f}, {x1, 0.92f, zc + 0.22f}, wood * 0.85f);
            add_collider(def, {x0, 0.0f, zc - 0.22f}, {x1, 1.0f, zc + 0.22f});
        }
    }
    const f32 az = -d + t; // the altar end's inner face
    add_box(op, {-1.7f, 0.0f, az}, {1.7f, 0.16f, az + 1.7f}, stone * 0.85f); // the dais
    add_box(op, {-0.9f, 0.16f, az + 0.35f}, {0.9f, 1.0f, az + 1.0f}, stone * 1.05f); // the altar
    add_box(op, {-0.95f, 1.0f, az + 0.3f}, {0.95f, 1.05f, az + 1.05f}, Vec3{0.92f, 0.9f, 0.84f}); // its cloth
    add_box(op, {-0.3f, 0.5f, az + 1.0f}, {0.3f, 1.0f, az + 1.06f}, Vec3{0.6f, 0.12f, 0.14f});      // the frontal
    add_collider(def, {-0.95f, 0.0f, az + 0.3f}, {0.95f, 1.05f, az + 1.05f});
    for (const f32 cx : {-0.6f, -0.2f, 0.25f, 0.65f}) { // candles on the altar
        const f32 ch = 0.14f + 0.08f * hashf(950u + static_cast<u32>(cx * 10.0f + 10.0f));
        add_box(op, {cx - 0.04f, 1.05f, az + 0.6f}, {cx + 0.04f, 1.05f + ch, az + 0.68f}, Vec3{0.94f, 0.9f, 0.78f});
        add_box(em, {cx - 0.03f, 1.05f + ch, az + 0.61f}, {cx + 0.03f, 1.13f + ch, az + 0.67f}, Vec3{1.4f, 1.0f, 0.5f});
    }
    for (const f32 sx : {-1.0f, 1.0f}) { // tall iron candlestands either side
        const Vec3 c{sx * 1.35f, 0.16f, az + 0.8f};
        add_box(op, c + Vec3{-0.16f, 0.0f, -0.16f}, c + Vec3{0.16f, 0.06f, 0.16f}, Vec3{0.16f, 0.15f, 0.15f});
        add_box(op, c + Vec3{-0.03f, 0.0f, -0.03f}, c + Vec3{0.03f, 1.3f, 0.03f}, Vec3{0.16f, 0.15f, 0.15f});
        add_box(op, c + Vec3{-0.05f, 1.3f, -0.05f}, c + Vec3{0.05f, 1.5f, 0.05f}, Vec3{0.94f, 0.9f, 0.78f});
        add_box(em, c + Vec3{-0.035f, 1.5f, -0.035f}, c + Vec3{0.035f, 1.62f, 0.035f}, Vec3{1.4f, 1.0f, 0.5f});
    }
    add_box(op, {-0.08f, 1.5f, az}, {0.08f, 3.7f, az + 0.06f}, gold);   // the great cross
    add_box(op, {-0.6f, 2.85f, az}, {0.6f, 3.0f, az + 0.06f}, gold);
    for (const f32 sx : {-1.0f, 1.0f}) { // hangings on the end wall
        add_box(op, {sx * 2.0f - 0.35f, 1.3f, az}, {sx * 2.0f + 0.35f, 3.6f, az + 0.04f}, Vec3{0.24f, 0.3f, 0.55f});
        add_box(op, {sx * 2.0f - 0.1f, 2.3f, az + 0.04f}, {sx * 2.0f + 0.1f, 2.6f, az + 0.06f}, gold);
    }

    add_wall_lantern(op, em, def, Vec3{1.0f, 2.3f, tz1 + 0.14f}, Vec3{0.0f, 0.0f, 1.0f});
    PropLight inner; // candlelight in the nave
    inner.offset = Vec3{0.0f, 3.4f, -0.5f};
    inner.direction = glm::normalize(Vec3{0.0f, -1.0f, -0.4f});
    inner.color = Vec3{1.0f, 0.76f, 0.46f};
    inner.range = 11.0f;
    inner.intensity = 3.2f;
    inner.cone_deg = 175.0f;
    def.lights.push_back(inner);

    def.footprint = Vec2{w, d};
    def.wall_height = h;
    def.door_spot = Vec3{0.0f, 0.0f, d + 0.9f};
    def.inside_spot = Vec3{0.0f, 0.0f, 1.15f}; // in the nave, behind the last pews
    def.bed_spot = Vec3{0.0f, 0.0f, az + 1.5f}; // the priest, before the altar
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    def.parts.push_back({std::move(shell), PropLayer::Roof});
    def.parts.push_back({std::move(glow), PropLayer::Glow});
    return def;
}

// A great city's KEEP: a massive square stone tower on a battered base, round corner turrets under
// conical slate caps, a crenellated roof-walk with fire braziers, arrow slits and lit windows, and a
// gate hung with the city's banners whose iron portcullis rises as anyone comes near - into the GREAT
// HALL within: a long feasting table, the lord's throne on its dais, braziers and banners.
PropDef PropLibrary::build_keep() {
    PropDef def;
    def.name = "keep";
    MeshData shell, op, em, glow;
    const Vec3 stone{0.53f, 0.55f, 0.6f};
    const Vec3 slate{0.28f, 0.31f, 0.4f};
    const Vec3 iron{0.18f, 0.17f, 0.17f};
    const Vec3 lit{1.0f, 0.8f, 0.44f};
    const Vec3 banner{0.62f, 0.12f, 0.12f};
    const Vec3 gold{0.86f, 0.68f, 0.26f};
    const Vec3 wood{0.36f, 0.24f, 0.14f};
    const f32 r = 4.0f, h = 10.5f, tr = 1.05f, t = 0.55f;
    const f32 gw = 1.1f, gh = 3.4f; // the gate arch (half-width, height)
    const f32 hh = 4.4f;            // the great hall's ceiling
    const f32 bo = 0.4f, bh = 1.1f; // the battered base: how far it stands out, how high

    // ---- The walls (on a battered base, split at the gate), with a collider over each + its base.
    auto wall = [&](const Vec3& lo, const Vec3& hi, const Vec3& clo, const Vec3& chi) {
        add_box(shell, lo, hi, stone);
        add_collider(def, clo, chi);
    };
    wall({-r, 0.0f, -r}, {r, h, -r + t}, {-r - bo, 0.0f, -r - bo}, {r + bo, h, -r + t});       // back
    wall({-r, 0.0f, -r}, {-r + t, h, r}, {-r - bo, 0.0f, -r - bo}, {-r + t, h, r + bo});       // left
    wall({r - t, 0.0f, -r}, {r, h, r}, {r - t, 0.0f, -r - bo}, {r + bo, h, r + bo});           // right
    wall({-r, 0.0f, r - t}, {-gw, h, r}, {-r - bo, 0.0f, r - t}, {-gw, h, r + bo});            // front, by the gate
    wall({gw, 0.0f, r - t}, {r, h, r}, {gw, 0.0f, r - t}, {r + bo, h, r + bo});
    add_box(shell, {-gw, gh, r - t}, {gw, h, r}, stone); // over the gate
    add_box(shell, {-r + t, hh, -r + t}, {r - t, hh + 0.3f, r - t}, wood * 0.7f); // the hall's ceiling
    add_box(op, {-r - bo, 0.0f, -r - bo}, {r + bo, bh, -r + t}, stone * 0.9f); // the battered base
    add_box(op, {-r - bo, 0.0f, -r + t}, {-r + t, bh, r + bo}, stone * 0.9f);
    add_box(op, {r - t, 0.0f, -r + t}, {r + bo, bh, r + bo}, stone * 0.9f);
    add_box(op, {-r + t, 0.0f, r - t}, {-gw, bh, r + bo}, stone * 0.9f);
    add_box(op, {gw, 0.0f, r - t}, {r - t, bh, r + bo}, stone * 0.9f);
    stone_face(shell, true, r, 1.0f, -r, r, gh + 0.3f, h, stone, 401u);
    stone_face(shell, true, r, 1.0f, -r, -gw - 0.24f, bh, gh + 0.3f, stone, 405u);
    stone_face(shell, true, r, 1.0f, gw + 0.24f, r, bh, gh + 0.3f, stone, 406u);
    stone_face(shell, true, -r, -1.0f, -r, r, bh, h, stone, 402u);
    stone_face(shell, false, r, 1.0f, -r, r, bh, h, stone, 403u);
    stone_face(shell, false, -r, -1.0f, -r, r, bh, h, stone, 404u);
    add_box(shell, {-r - 0.1f, h * 0.55f, -r - 0.1f}, {r + 0.1f, h * 0.55f + 0.16f, r + 0.1f}, stone * 1.05f); // string course
    // The roof-walk: a parapet lip + merlons round all four sides.
    add_box(shell, {-r - 0.18f, h, -r - 0.18f}, {r + 0.18f, h + 0.2f, r + 0.18f}, stone * 1.05f);
    for (int i = -3; i <= 3; ++i) {
        const f32 o = static_cast<f32>(i) * 1.05f;
        const f32 m0 = h + 0.2f, m1 = h + 0.75f, e = r + 0.18f;
        add_box(shell, {o - 0.3f, m0, e - 0.26f}, {o + 0.3f, m1, e}, stone);
        add_box(shell, {o - 0.3f, m0, -e}, {o + 0.3f, m1, -e + 0.26f}, stone);
        add_box(shell, {e - 0.26f, m0, o - 0.3f}, {e, m1, o + 0.3f}, stone);
        add_box(shell, {-e, m0, o - 0.3f}, {-e + 0.26f, m1, o + 0.3f}, stone);
    }
    // Round corner turrets rising past the roof-walk, under conical slate caps, each with a lit window.
    for (const f32 sx : {-1.0f, 1.0f}) {
        for (const f32 sz : {-1.0f, 1.0f}) {
            const Vec3 c{sx * r, 0.0f, sz * r};
            add_prism(shell, c, tr, 0.0f, h + 1.7f, 10, stone * 0.97f);
            add_prism(shell, c, tr + 0.14f, h + 1.7f, h + 1.95f, 10, stone * 1.04f);
            add_cone(shell, c, tr + 0.25f, h + 1.95f, 2.4f, 10, slate);
            const Vec3 wc = c + Vec3{sx * (tr - 0.02f), h + 0.7f, 0.0f};
            add_box(em, wc - Vec3{0.06f, 0.3f, 0.14f}, wc + Vec3{0.06f, 0.3f, 0.14f}, lit);
            add_collider(def, c - Vec3{tr, 0.0f, tr}, c + Vec3{tr, h, tr});
        }
    }
    // Arrow slits + lit windows on every face.
    for (int f = 0; f < 4; ++f) {
        const Vec3 out = f == 0 ? Vec3{0.0f, 0.0f, 1.0f}
                         : f == 1 ? Vec3{0.0f, 0.0f, -1.0f}
                         : f == 2 ? Vec3{1.0f, 0.0f, 0.0f}
                                  : Vec3{-1.0f, 0.0f, 0.0f};
        const Vec3 across{std::abs(out.z), 0.0f, std::abs(out.x)};
        for (const f32 a : {-2.2f, 2.2f}) {
            const Vec3 sc = out * (r + 0.02f) + across * a;
            const Vec3 lo_s = sc + Vec3{0.0f, 3.0f, 0.0f} - across * 0.07f - out * 0.12f;
            const Vec3 hi_s = sc + Vec3{0.0f, 4.2f, 0.0f} + across * 0.07f + out * 0.04f;
            add_box(shell, glm::min(lo_s, hi_s), glm::max(lo_s, hi_s), Vec3{0.1f, 0.1f, 0.12f});
            const Vec3 lo_w = sc + Vec3{0.0f, 7.2f, 0.0f} - across * 0.32f - out * 0.1f;
            const Vec3 hi_w = sc + Vec3{0.0f, 8.4f, 0.0f} + across * 0.32f + out * 0.05f;
            add_box(em, glm::min(lo_w, hi_w), glm::max(lo_w, hi_w), lit);
            const Vec3 lo_h = sc + Vec3{0.0f, 8.4f, 0.0f} - across * 0.42f;
            const Vec3 hi_h = sc + Vec3{0.0f, 8.6f, 0.0f} + across * 0.42f + out * 0.12f;
            add_box(shell, glm::min(lo_h, hi_h), glm::max(lo_h, hi_h), stone * 0.9f); // window hood
        }
    }
    // The gate (front, +z): a dressed-stone arch round the opening, and the iron PORTCULLIS - a Door
    // part that rises up into the wall above as anyone comes near.
    add_box(shell, {-gw - 0.24f, 0.0f, r}, {-gw, gh + 0.24f, r + 0.12f}, stone * 0.82f);
    add_box(shell, {gw, 0.0f, r}, {gw + 0.24f, gh + 0.24f, r + 0.12f}, stone * 0.82f);
    add_box(shell, {-gw, gh, r}, {gw, gh + 0.24f, r + 0.12f}, stone * 0.82f);
    {
        MeshData gate;
        const f32 gz = r - 0.32f;
        for (int i = -3; i <= 3; ++i) {
            const f32 x = static_cast<f32>(i) * 0.3f;
            add_box(gate, {x - 0.035f, 0.12f, gz - 0.035f}, {x + 0.035f, gh - 0.02f, gz + 0.035f}, iron);
            add_box(gate, {x - 0.02f, 0.0f, gz - 0.02f}, {x + 0.02f, 0.12f, gz + 0.02f}, iron * 0.8f); // the spike
        }
        for (int j = 0; j < 5; ++j) {
            const f32 y = 0.45f + static_cast<f32>(j) * 0.62f;
            add_box(gate, {-gw + 0.06f, y - 0.035f, gz - 0.05f}, {gw - 0.06f, y + 0.035f, gz + 0.05f}, iron);
        }
        PropPart part;
        part.mesh = std::move(gate);
        part.layer = PropLayer::Door;
        part.lift = gh - 0.35f;
        def.parts.push_back(std::move(part));
    }
    // The city's banners hung down the front, either side of the gate.
    for (const f32 x : {-2.6f, 2.6f}) {
        const f32 z = r + 0.06f;
        add_box(shell, {x - 0.62f, h - 0.6f, z - 0.02f}, {x + 0.62f, h - 0.45f, z + 0.1f}, iron); // the pole
        add_quad(shell, {x - 0.55f, h - 0.55f, z + 0.04f}, {x - 0.55f, h - 4.6f, z + 0.04f}, {x, h - 5.1f, z + 0.04f},
                 {x, h - 0.55f, z + 0.04f}, banner);
        add_quad(shell, {x, h - 0.55f, z + 0.04f}, {x, h - 5.1f, z + 0.04f}, {x + 0.55f, h - 4.6f, z + 0.04f},
                 {x + 0.55f, h - 0.55f, z + 0.04f}, banner * 0.92f);
        add_box(shell, {x - 0.16f, h - 2.6f, z + 0.05f}, {x + 0.16f, h - 2.1f, z + 0.08f}, gold); // the device
    }
    // Fire braziers blazing on the roof-walk's front corners (outdoor lights).
    for (const f32 x : {-2.4f, 2.4f}) {
        add_box(op, {x - 0.06f, h + 0.2f, r - 0.86f}, {x + 0.06f, h + 0.55f, r - 0.74f}, iron);
        add_fire_basket(op, em, Vec3{x, h + 0.55f, r - 0.8f}, 1.2f);
        PropLight l;
        l.offset = Vec3{x, h + 1.4f, r - 0.8f};
        l.direction = Vec3{0.0f, -1.0f, 0.3f};
        l.color = Vec3{1.0f, 0.66f, 0.32f};
        l.range = 16.0f;
        l.intensity = 2.2f;
        l.cone_deg = 160.0f;
        l.spill = true;
        def.lights.push_back(l);
    }

    // ---- The great hall: a flagged floor, a long feasting table down the middle with benches, the
    // lord's throne on its dais against the back wall between two banners, braziers burning either
    // side of the gate, and the firelight spilling out through it.
    const f32 in = r - t; // the hall's inner half-extent
    add_box(op, {-in, -0.3f, -in}, {in, 0.02f, in}, stone * 0.4f);
    add_box(op, {-gw, -0.3f, in}, {gw, 0.02f, r}, stone * 0.4f); // under the gate
    {
        const int n = 9;
        const f32 s = 2.0f * in / static_cast<f32>(n);
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const f32 x0 = -in + static_cast<f32>(i) * s, z0 = -in + static_cast<f32>(j) * s;
                add_box(op, {x0 + 0.03f, 0.02f, z0 + 0.03f}, {x0 + s - 0.03f, 0.05f, z0 + s - 0.03f},
                        stone * (0.8f + 0.22f * hashf(1200u + static_cast<u32>(j * 17 + i))));
            }
        }
    }
    const f32 tz = -1.1f; // the feasting table
    add_box(op, {-2.2f, 0.72f, tz - 0.45f}, {2.2f, 0.82f, tz + 0.45f}, wood * 1.1f);
    for (const f32 lx : {-2.0f, 2.0f}) {
        for (const f32 lz : {-0.35f, 0.35f}) {
            add_box(op, {lx - 0.06f, 0.0f, tz + lz - 0.06f}, {lx + 0.06f, 0.72f, tz + lz + 0.06f}, wood * 0.8f);
        }
    }
    for (const f32 bz : {-0.8f, 0.8f}) { // benches either side
        add_box(op, {-2.0f, 0.38f, tz + bz - 0.16f}, {2.0f, 0.46f, tz + bz + 0.16f}, wood);
        for (const f32 lx : {-1.8f, 1.8f}) {
            add_box(op, {lx - 0.05f, 0.0f, tz + bz - 0.12f}, {lx + 0.05f, 0.38f, tz + bz + 0.12f}, wood * 0.8f);
        }
    }
    add_collider(def, {-2.2f, 0.0f, tz - 0.96f}, {2.2f, 0.82f, tz + 0.96f});
    for (int i = 0; i < 5; ++i) { // tankards + platters down the table
        const f32 x = -1.7f + static_cast<f32>(i) * 0.85f;
        add_tankard(op, {x, 0.82f, tz + (i % 2 ? 0.18f : -0.2f)});
        add_box(op, {x + 0.2f, 0.82f, tz - 0.12f}, {x + 0.52f, 0.85f, tz + 0.16f}, Vec3{0.7f, 0.68f, 0.62f});
        add_mesh(op, primitives::sphere(7, 4), glm::translate(Mat4{1.0f}, Vec3{x + 0.36f, 0.9f, tz + 0.02f}) *
                                                   glm::scale(Mat4{1.0f}, Vec3{0.22f, 0.12f, 0.16f}),
                 i % 2 ? Vec3{0.78f, 0.52f, 0.24f} : Vec3{0.62f, 0.3f, 0.2f});
    }
    const f32 bz = -in; // the back wall's inner face
    add_box(op, {-1.4f, 0.0f, bz}, {1.4f, 0.22f, bz + 1.5f}, stone * 0.85f); // the dais
    add_box(op, {-0.5f, 0.22f, bz + 0.15f}, {0.5f, 2.3f, bz + 0.32f}, wood * 0.9f); // the throne's tall back
    add_box(op, {-0.45f, 0.22f, bz + 0.32f}, {0.45f, 0.72f, bz + 0.95f}, wood * 0.9f); // its seat
    add_box(op, {-0.38f, 0.72f, bz + 0.34f}, {0.38f, 0.8f, bz + 0.9f}, banner);      // the cushion
    add_box(op, {-0.38f, 0.8f, bz + 0.32f}, {0.38f, 2.1f, bz + 0.36f}, banner);
    for (const f32 sx : {-1.0f, 1.0f}) {
        add_box(op, {sx * 0.5f - 0.08f, 0.72f, bz + 0.32f}, {sx * 0.5f + 0.08f, 1.05f, bz + 0.95f}, wood * 0.8f); // arms
        add_box(op, {sx * 0.5f - 0.09f, 2.3f, bz + 0.15f}, {sx * 0.5f + 0.09f, 2.5f, bz + 0.33f}, gold);           // finials
        // Banners on the back wall, and a shield either side.
        add_box(op, {sx * 2.1f - 0.5f, 1.2f, bz}, {sx * 2.1f + 0.5f, 3.9f, bz + 0.04f}, banner);
        add_box(op, {sx * 2.1f - 0.14f, 2.9f, bz + 0.04f}, {sx * 2.1f + 0.14f, 3.3f, bz + 0.07f}, gold);
        add_shield(op, {sx * 1.15f, 1.6f, bz + 0.06f}, Vec3{0.35f, 0.5f, 0.66f});
    }
    add_collider(def, {-0.55f, 0.0f, bz}, {0.55f, 2.3f, bz + 0.95f});
    for (const f32 sx : {-1.0f, 1.0f}) { // braziers either side of the gate
        const Vec3 c{sx * 2.5f, 0.0f, 1.9f};
        add_box(op, c + Vec3{-0.07f, 0.0f, -0.07f}, c + Vec3{0.07f, 0.95f, 0.07f}, iron);
        add_box(op, c + Vec3{-0.22f, 0.0f, -0.22f}, c + Vec3{0.22f, 0.08f, 0.22f}, iron);
        add_fire_basket(op, em, c + Vec3{0.0f, 0.95f, 0.0f}, 1.0f);
        add_collider(def, c - Vec3{0.25f, 0.0f, 0.25f}, c + Vec3{0.25f, 1.3f, 0.25f});
    }
    PropLight hall; // the hall's firelight
    hall.offset = Vec3{0.0f, hh - 0.4f, 0.4f};
    hall.direction = glm::normalize(Vec3{0.0f, -1.0f, 0.15f});
    hall.color = Vec3{1.0f, 0.66f, 0.36f};
    hall.range = 12.0f;
    hall.intensity = 4.0f;
    hall.cone_deg = 172.0f;
    def.lights.push_back(hall);
    add_window_spill(def, glow, {0.0f, 1.7f, r}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, gw * 0.8f, 1.0f, true);

    def.footprint = Vec2{r + tr, r + tr};
    def.wall_height = h;
    def.door_spot = Vec3{0.0f, 0.0f, r + 1.4f};
    def.inside_spot = Vec3{0.0f, 0.0f, 1.2f};
    def.bed_spot = Vec3{0.0f, 0.0f, bz + 1.2f}; // the lord, before the throne
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    def.parts.push_back({std::move(shell), PropLayer::Roof});
    def.parts.push_back({std::move(glow), PropLayer::Glow});
    return def;
}

// A street BRAZIER (Decor kDecorBrazier): an iron fire-basket on a stout post, blazing - the warm
// lights a snowbound town keeps burning along its streets.
PropDef PropLibrary::build_brazier() {
    PropDef def;
    def.name = "brazier";
    MeshData op, em;
    const Vec3 iron{0.17f, 0.15f, 0.15f};
    add_box(op, {-0.32f, 0.0f, -0.32f}, {0.32f, 0.12f, 0.32f}, Vec3{0.42f, 0.42f, 0.44f}); // a stone footing
    add_prism(op, Vec3{0.0f}, 0.07f, 0.12f, 1.25f, 6, iron);
    for (int i = 0; i < 3; ++i) { // three splayed legs
        const f32 a = TwoPi * static_cast<f32>(i) / 3.0f;
        add_beam(op, Vec3{std::cos(a) * 0.3f, 0.1f, std::sin(a) * 0.3f}, Vec3{0.0f, 0.7f, 0.0f}, 0.03f, iron);
    }
    add_fire_basket(op, em, Vec3{0.0f, 1.22f, 0.0f}, 1.25f);
    PropLight l;
    l.offset = Vec3{0.0f, 1.9f, 0.0f};
    l.direction = Vec3{0.0f, -1.0f, 0.0f};
    l.color = Vec3{1.0f, 0.64f, 0.3f};
    l.range = 12.0f;
    l.intensity = 2.3f;
    l.cone_deg = 170.0f;
    def.lights.push_back(l);
    BoxCollider c;
    c.half_extents = Vec2{0.3f, 0.3f};
    c.height = 1.4f;
    def.colliders.push_back(c);
    def.parts.push_back({std::move(op), PropLayer::Opaque});
    def.parts.push_back({std::move(em), PropLayer::Emissive});
    return def;
}

// A SNOWMAN (Decor kDecorSnowman): three rolled snowballs, coal eyes + buttons, a carrot nose, stick
// arms, a knitted scarf and a battered hat - the townsfolk's handiwork in a snowbound town.
PropDef PropLibrary::build_snowman() {
    PropDef def;
    def.name = "snowman";
    MeshData m;
    const Vec3 snow{0.66f, 0.73f, 0.9f};
    const Vec3 coal{0.08f, 0.08f, 0.09f};
    const MeshData ball = primitives::sphere(9, 6);
    auto sphere_at = [&](const Vec3& c, f32 d, const Vec3& col) {
        add_mesh(m, ball, glm::translate(Mat4{1.0f}, c) * glm::scale(Mat4{1.0f}, Vec3{d}), col);
    };
    sphere_at({0.0f, 0.4f, 0.0f}, 0.92f, snow);
    sphere_at({0.0f, 1.02f, 0.0f}, 0.66f, snow * 1.02f);
    sphere_at({0.0f, 1.5f, 0.0f}, 0.46f, snow * 1.04f);
    for (const f32 x : {-0.09f, 0.09f}) { // coal eyes
        add_box(m, {x - 0.03f, 1.55f, 0.2f}, {x + 0.03f, 1.61f, 0.24f}, coal);
    }
    for (const f32 y : {0.88f, 1.04f, 1.2f}) { // coal buttons
        const f32 z = 0.3f - std::abs(y - 1.04f) * 0.5f;
        add_box(m, {-0.03f, y, z}, {0.03f, y + 0.06f, z + 0.04f}, coal);
    }
    // The carrot nose, poking out the front.
    const Vec3 tip{0.0f, 1.5f, 0.46f};
    const Vec3 nose_c{0.0f, 1.5f, 0.3f};
    emit_tri(m, {-0.04f, 1.47f, 0.22f}, {0.04f, 1.47f, 0.22f}, tip, nose_c, Vec3{0.9f, 0.42f, 0.12f});
    emit_tri(m, {0.04f, 1.47f, 0.22f}, {0.0f, 1.54f, 0.22f}, tip, nose_c, Vec3{0.84f, 0.38f, 0.1f});
    emit_tri(m, {0.0f, 1.54f, 0.22f}, {-0.04f, 1.47f, 0.22f}, tip, nose_c, Vec3{0.88f, 0.4f, 0.11f});
    // Stick arms.
    const Vec3 twig{0.3f, 0.2f, 0.12f};
    add_beam(m, Vec3{0.28f, 1.1f, 0.0f}, Vec3{0.75f, 1.42f, 0.05f}, 0.025f, twig);
    add_beam(m, Vec3{-0.28f, 1.1f, 0.0f}, Vec3{-0.72f, 1.3f, 0.12f}, 0.025f, twig);
    // A red knitted scarf + a battered black hat.
    add_prism(m, Vec3{0.0f}, 0.28f, 1.26f, 1.36f, 10, Vec3{0.7f, 0.14f, 0.12f});
    add_box(m, {0.12f, 0.96f, 0.18f}, {0.26f, 1.3f, 0.26f}, Vec3{0.66f, 0.13f, 0.11f}); // the scarf's tail
    add_prism(m, Vec3{0.0f}, 0.27f, 1.68f, 1.71f, 10, coal * 1.6f);
    add_prism(m, Vec3{0.0f}, 0.17f, 1.71f, 1.98f, 10, coal * 1.6f);
    def.parts.push_back({std::move(m), PropLayer::Opaque});
    BoxCollider c;
    c.half_extents = Vec2{0.42f, 0.42f};
    c.height = 1.6f;
    def.colliders.push_back(c);
    return def;
}

PropLibrary::PropLibrary(bool bake_ao) {
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

    // One-time vertex-AO bake over the whole catalogue (see bake_def_ao above). Skipped
    // for never-rendered copies (the server's collider-only library). Defs run
    // sequentially; the bake itself threads across each mesh's vertices, which keeps
    // the few HEAVY defs (houses) on all cores instead of serialised on one.
    if (bake_ao) {
        for (auto* catalogue :
             {&bushes_, &rocks_, &logs_, &fences_, &fence_rails_, &lanterns_, &houses_,
              &walls_, &gates_, &wells_, &bridges_, &markets_, &paths_, &planters_,
              &fountains_, &decor_, &rivers_, &crystals_, &glow_shrooms_, &campfires_,
              &monuments_, &watchtowers_}) {
            for (PropDef& def : *catalogue) {
                bake_def_ao(def);
            }
        }
    }

    // Snowbound towns' twins of every building, wall and gate tower + the market (kSnowHouses ...): the
    // AO-baked originals with snow laid over them (the snow itself needs no AO).
    for (u32 i = 0; i < kHouseDefs; ++i) {
        PropDef snowy = houses_[i];
        make_snowy_house(snowy);
        houses_.push_back(std::move(snowy));
    }
    for (usize i = 0; i < 2; ++i) {
        PropDef snowy = walls_[i];
        add_snow(snowy, 0.45f);
        walls_.push_back(std::move(snowy));
    }
    for (usize i = 0; i < 2; ++i) {
        PropDef snowy = gates_[i];
        add_snow(snowy, 0.5f);
        gates_.push_back(std::move(snowy));
    }
    {
        PropDef snowy = markets_[0];
        add_snow(snowy, 0.8f);
        markets_.push_back(std::move(snowy));
    }
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
