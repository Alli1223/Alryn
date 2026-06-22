#include <Alryn/World/PropLibrary.h>

#include <Alryn/Renderer/MeshPrimitives.h>

#include <utility>

namespace alryn {

PropDef PropLibrary::build_bush(int variant) {
    PropDef def;
    def.name = "bush";
    static const Vec3 greens[] = {{0.22f, 0.42f, 0.20f}, {0.26f, 0.46f, 0.22f}, {0.30f, 0.40f, 0.18f}};
    def.parts.push_back({primitives::bush(variant, greens[variant % 3]), PropLayer::Foliage});
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
    const Vec3 glow{1.0f, 0.85f, 0.5f};
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
    l.color = Vec3{1.0f, 0.80f, 0.48f};
    l.range = 13.0f;
    l.intensity = 1.6f;
    l.cone_deg = 130.0f;
    def.lights.push_back(l);
    BoxCollider c;
    c.half_extents = Vec2{0.1f, 0.1f};
    c.height = 1.7f;
    def.colliders.push_back(c);
    return def;
}

namespace {
// A medieval house style: half-extents (w,d), per-storey height, storey count, gable
// rise and a material flavour (0 wattle-and-daub, 1 stone, 2 dark timber). Varying
// these gives small/large, squat/tall and one/two-storey homes.
struct HouseStyle {
    f32 w, d, story_h;
    int stories;
    f32 roof_rise;
    int material;
};
constexpr HouseStyle kHouseStyles[kHouseVariants] = {
    {3.2f, 2.8f, 2.4f, 1, 1.7f, 0}, // classic thatched cottage
    {4.7f, 2.6f, 2.3f, 1, 1.3f, 0}, // long house (wide, low roof)
    {2.7f, 2.7f, 2.3f, 2, 1.0f, 1}, // stone townhouse, two storeys
    {3.0f, 3.0f, 2.4f, 2, 1.5f, 2}, // timber two-storey
    {4.1f, 3.4f, 2.6f, 1, 2.0f, 0}, // manor (big, steep roof)
    {2.5f, 2.3f, 2.2f, 1, 1.6f, 1}, // small stone hut
    {3.3f, 2.5f, 2.3f, 2, 1.2f, 0}, // tall narrow cottage, two storeys
    {3.8f, 2.9f, 2.4f, 1, 1.7f, 2}, // timber-framed hall
};
} // namespace

// The (w,d) footprint half-extents of a house variant, so the village layout can keep
// houses from intersecting walls, the market and each other without building the mesh.
Vec2 PropLibrary::house_half_extents(u32 variant) {
    const HouseStyle& st = kHouseStyles[variant % kHouseVariants];
    return Vec2{st.w, st.d};
}

// A medieval village house, varied by `variant` (see kHouseStyles): cottages,
// longhouses, two-storey townhouses and manors in daub / stone / timber. Real window
// openings per storey, a furnished ground floor (hearth + fire + a bed where the
// resident sleeps), interior lights and a dollhouse shell that fades when you step in.
PropDef PropLibrary::build_house(u32 variant) {
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
    const f32 rr = st.roof_rise; // gable rise
    const f32 oh = 0.5f;         // roof overhang

    auto rnd = [&](u32 s) {
        u32 v = (variant * 2654435761u + s * 0x9E3779B9u);
        v ^= v >> 15;
        v *= 0x2545F491u;
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
    };
    // Warm lime-washed daub infill (cottages), mossy fieldstone, and dark oak.
    static const Vec3 daub_cols[] = {{0.90f, 0.84f, 0.69f}, {0.86f, 0.78f, 0.60f},
                                     {0.82f, 0.73f, 0.56f}};
    const Vec3 stone{0.57f, 0.53f, 0.45f}; // warm lime-mortared fieldstone
    const Vec3 timber{0.24f, 0.16f, 0.10f};
    const Vec3 frame_col{0.20f, 0.13f, 0.08f}; // exposed oak half-timbering
    const Vec3 wall_col = st.material == 1 ? stone
                          : st.material == 2 ? glm::mix(daub_cols[variant % 3], timber, 0.22f)
                                             : daub_cols[variant % 3];
    const Vec3 floor_c{0.32f, 0.25f, 0.17f};
    // Roof by material: golden thatch (daub cottages), terracotta clay tiles (stone
    // townhouses), weathered wood shingles (timber houses) - all medieval-town roofs.
    const Vec3 roof = st.material == 1
                          ? glm::mix(Vec3{0.67f, 0.32f, 0.21f}, Vec3{0.57f, 0.26f, 0.17f}, rnd(1))
                      : st.material == 2
                          ? glm::mix(Vec3{0.44f, 0.33f, 0.21f}, Vec3{0.35f, 0.26f, 0.17f}, rnd(1))
                          : glm::mix(Vec3{0.83f, 0.66f, 0.34f}, Vec3{0.70f, 0.54f, 0.27f}, rnd(1));
    const bool half_timber = st.material != 1; // daub + timber houses get exposed framing
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
        constexpr f32 proud = 0.06f;
        auto post = [&](f32 sx, f32 sz) {
            const f32 x0 = sx > 0.0f ? w - 0.05f : -w - proud;
            const f32 x1 = sx > 0.0f ? w + proud : -w + 0.05f;
            const f32 z0 = sz > 0.0f ? d - 0.05f : -d - proud;
            const f32 z1 = sz > 0.0f ? d + proud : -d + 0.05f;
            add_box(shell, {x0, 0.0f, z0}, {x1, h, z1}, frame_col);
        };
        post(1, 1);
        post(1, -1);
        post(-1, 1);
        post(-1, -1);
        // Frames one wall panel (a storey of one face): plates, studs + a diagonal brace.
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
            bar(a_lo, a_hi, y0, y0 + 0.13f);  // sill plate
            bar(a_lo, a_hi, y1 - 0.13f, y1);  // head plate
            const int studs = std::max(2, static_cast<int>(std::round((a_hi - a_lo) / 0.95f)));
            for (int i = 0; i <= studs; ++i) {
                const f32 a = glm::mix(a_lo, a_hi, static_cast<f32>(i) / static_cast<f32>(studs));
                if (std::abs(a) < 0.8f) {
                    continue; // leave the central window / door opening clear
                }
                bar(a - 0.06f, a + 0.06f, y0, y1);
            }
            // Diagonal brace across the panel (the classic Tudor cross-timber).
            Vec2 q0{a_lo + 0.3f, y0 + 0.15f};
            Vec2 q1{a_hi - 0.3f, y1 - 0.15f};
            Vec2 dir = q1 - q0;
            const f32 dl = glm::length(dir);
            if (dl > 0.4f) {
                dir /= dl;
                const Vec2 n{-dir.y * 0.07f, dir.x * 0.07f};
                const f32 pf = face + out * proud;
                auto P = [&](const Vec2& v) {
                    return along_x ? Vec3{v.x, v.y, pf} : Vec3{pf, v.y, v.x};
                };
                add_quad(shell, P(q0 + n), P(q1 + n), P(q1 - n), P(q0 - n), frame_col);
            }
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

    // ---- Gable roof along the longer horizontal axis, laid in stepped courses (thatch /
    // clay tile / wood shingle) with alternating shade per row for a tiled texture, a
    // ridge beam at the apex and gable-end infill.
    const bool gable_x = w >= d;
    const f32 xl = -w - oh, xr = w + oh, zf = d + oh, zb = -d - oh;
    const int courses = std::max(4, static_cast<int>(std::round(rr / 0.32f)));
    auto slope = [&](const Vec3& eaveL, const Vec3& eaveR, const Vec3& ridgeL, const Vec3& ridgeR) {
        for (int k = 0; k < courses; ++k) {
            const f32 t0 = static_cast<f32>(k) / static_cast<f32>(courses);
            const f32 t1 = static_cast<f32>(k + 1) / static_cast<f32>(courses);
            const Vec3 a0 = glm::mix(eaveL, ridgeL, t0), a1 = glm::mix(eaveL, ridgeL, t1);
            const Vec3 b0 = glm::mix(eaveR, ridgeR, t0), b1 = glm::mix(eaveR, ridgeR, t1);
            add_quad(shell, a0, b0, b1, a1, roof * (0.9f + 0.12f * static_cast<f32>(k % 2)));
        }
    };
    if (gable_x) {
        slope({xl, h, zf}, {xr, h, zf}, {xl, h + rr, 0.0f}, {xr, h + rr, 0.0f}); // front
        slope({xr, h, zb}, {xl, h, zb}, {xr, h + rr, 0.0f}, {xl, h + rr, 0.0f}); // back
        add_tri(shell, {xl, h, zb}, {xl, h, zf}, {xl, h + rr, 0.0f}, wall_col);
        add_tri(shell, {xr, h, zf}, {xr, h, zb}, {xr, h + rr, 0.0f}, wall_col);
        add_box(shell, {xl, h + rr - 0.12f, -0.09f}, {xr, h + rr + 0.07f, 0.09f}, timber); // ridge
    } else {
        slope({xr, h, zb}, {xr, h, zf}, {0.0f, h + rr, zb}, {0.0f, h + rr, zf}); // +x
        slope({xl, h, zf}, {xl, h, zb}, {0.0f, h + rr, zf}, {0.0f, h + rr, zb}); // -x
        add_tri(shell, {xl, h, zf}, {xr, h, zf}, {0.0f, h + rr, zf}, wall_col);
        add_tri(shell, {xr, h, zb}, {xl, h, zb}, {0.0f, h + rr, zb}, wall_col);
        add_box(shell, {-0.09f, h + rr - 0.12f, zb}, {0.09f, h + rr + 0.07f, zf}, timber); // ridge
    }

    // ---- Ground floor: a stone hearth with fire + chimney (back-left), a table, and
    // the resident's straw bed (back-right, where the villager sleeps). ----
    add_box(op, {-w + t, 0.0f, -d + t}, {-w + t + 1.2f, 1.4f, -d + t + 0.6f}, stone);          // hearth
    add_box(op, {-w + t, 1.4f, -d + t}, {-w + t + 0.65f, h + rr * 0.5f, -d + t + 0.5f}, stone); // chimney
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

// A stone perimeter wall segment, running ALONG local +X so the village ring lays
// segments end to end (yawed to each side of the palisade).
PropDef PropLibrary::build_wall(int variant) {
    PropDef def;
    def.name = "wall";
    const Vec3 stone = variant % 2 == 0 ? Vec3{0.50f, 0.49f, 0.47f} : Vec3{0.45f, 0.45f, 0.46f};
    constexpr f32 half_len = 1.6f;
    constexpr f32 thick = 0.4f;
    constexpr f32 wh = 2.3f;
    MeshData m;
    add_box(m, {-half_len, 0.0f, -thick}, {half_len, wh, thick}, stone);
    // A crenellated cap (alternating merlons) for a fortified look.
    for (int i = -2; i <= 2; ++i) {
        const f32 cx = static_cast<f32>(i) * 0.62f;
        add_box(m, {cx - 0.22f, wh, -thick}, {cx + 0.22f, wh + 0.28f, thick}, stone * 1.05f);
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
    const Vec3 stone{0.50f, 0.49f, 0.47f};
    const Vec3 dark{0.24f, 0.23f, 0.22f}; // doorway / arrow-slit recesses
    const Vec3 wood{0.34f, 0.23f, 0.13f};
    const Vec3 fire{1.0f, 0.6f, 0.2f};
    const Vec3 lit{1.0f, 0.82f, 0.45f}; // warm lit window
    MeshData op;
    MeshData em;
    constexpr f32 r = 1.0f;  // half-width footprint
    constexpr f32 gh = 4.4f; // tower height (the wall is 2.3)

    add_box(op, {-r - 0.16f, 0.0f, -r - 0.16f}, {r + 0.16f, 0.5f, r + 0.16f}, stone * 0.95f); // battered base
    add_box(op, {-r, 0.5f, -r}, {r, gh, r}, stone);                                           // body
    add_box(op, {-r - 0.07f, gh - 1.0f, -r - 0.07f}, {r + 0.07f, gh - 0.86f, r + 0.07f},
            stone * 1.04f); // string-course band

    // Doorway + a lit upper window on the front (+z), buried so only the face shows.
    add_box(op, {-0.33f, 0.0f, r - 0.3f}, {0.33f, 1.75f, r + 0.03f}, dark);   // doorway
    add_box(op, {-0.4f, 1.7f, r - 0.12f}, {0.4f, 1.9f, r + 0.05f}, wood);     // door lintel
    add_box(em, {-0.22f, 2.7f, r - 0.25f}, {0.22f, 3.2f, r + 0.03f}, lit);    // lit window glow
    add_box(op, {-0.03f, 2.7f, r - 0.05f}, {0.03f, 3.2f, r + 0.05f}, wood);   // window mullion

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
    const Vec3 stone{0.48f, 0.47f, 0.46f};
    MeshData op;
    constexpr f32 r = 0.45f;
    constexpr f32 gh = 3.1f;
    add_box(op, {-r, 0.0f, -r}, {r, gh, r}, stone);
    add_box(op, {-r - 0.08f, gh, -r - 0.08f}, {r + 0.08f, gh + 0.18f, r + 0.08f}, stone * 1.05f);
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
    const Vec3 mud{0.19f, 0.15f, 0.11f}; // wet muddy earth between the stones
    // Muted, dirty grey + earthy stone tones (no bright whites).
    const Vec3 stones[6] = {{0.45f, 0.44f, 0.42f}, {0.39f, 0.38f, 0.36f}, {0.51f, 0.49f, 0.45f},
                            {0.43f, 0.40f, 0.35f}, {0.36f, 0.35f, 0.34f}, {0.49f, 0.45f, 0.39f}};
    MeshData m;
    constexpr f32 hx = 1.18f, hz = 1.18f;
    add_box(m, {-hx, -0.22f, -hz}, {hx, 0.0f, hz}, mud); // mud bed, sunk in, top at ground

    auto rnd = [](int i, int j, int salt) {
        const u32 h = (static_cast<u32>(i * 73856093) ^ static_cast<u32>(j * 19349663) ^
                       static_cast<u32>(salt * 83492791));
        return static_cast<f32>((h >> 9) & 0xFFFFu) / 65535.0f;
    };
    constexpr int g = 5;
    const f32 cell = (2.0f * hx) / static_cast<f32>(g);
    for (int j = 0; j < g; ++j) {
        for (int i = 0; i < g; ++i) {
            if (rnd(i, j, 11) < 0.12f) {
                continue; // a worn, trodden-out muddy gap (no stone here)
            }
            const f32 cx = -hx + (static_cast<f32>(i) + 0.5f) * cell + (rnd(i, j, 1) - 0.5f) * cell * 0.5f;
            const f32 cz = -hz + (static_cast<f32>(j) + 0.5f) * cell + (rnd(i, j, 2) - 0.5f) * cell * 0.5f;
            const f32 r = cell * (0.44f + rnd(i, j, 3) * 0.26f); // varied size; some big, packed close
            const f32 top = 0.02f + rnd(i, j, 4) * 0.075f;       // worn-flat .. proud, each different
            const f32 dirt = 0.74f + rnd(i, j, 6) * 0.34f;       // per-stone dirt darkening
            const Vec3 col = stones[(i + j * 2 + static_cast<int>(rnd(i, j, 5) * 6.0f)) % 6] * dirt;
            // An irregular rounded rock: 4 jittered base corners sloping up to a small, lumpy,
            // off-centre top (each corner pushed independently) so the stone looks rough-hewn.
            const Vec3 axis{cx, top * 0.5f, cz};
            auto bc = [&](f32 sx, f32 sz, int s) {
                return Vec3{cx + sx * r * (0.78f + rnd(i, j, s) * 0.44f), 0.0f,
                            cz + sz * r * (0.78f + rnd(i, j, s + 1) * 0.44f)};
            };
            const f32 tr = r * 0.52f;
            auto tc = [&](f32 sx, f32 sz, int s) {
                return Vec3{cx + sx * tr * (0.6f + rnd(i, j, s) * 0.7f) + (rnd(i, j, s + 2) - 0.5f) * r * 0.2f,
                            top * (0.62f + rnd(i, j, s + 1) * 0.6f),
                            cz + sz * tr * (0.6f + rnd(i, j, s) * 0.7f) + (rnd(i, j, s + 3) - 0.5f) * r * 0.2f};
            };
            const Vec3 b00 = bc(-1, -1, 20), b10 = bc(1, -1, 22), b11 = bc(1, 1, 24), b01 = bc(-1, 1, 26);
            const Vec3 t00 = tc(-1, -1, 30), t10 = tc(1, -1, 32), t11 = tc(1, 1, 34), t01 = tc(-1, 1, 36);
            emit_tri(m, b00, b10, t10, axis, col); // four rough sloped sides
            emit_tri(m, b00, t10, t00, axis, col);
            emit_tri(m, b10, b11, t11, axis, col);
            emit_tri(m, b10, t11, t10, axis, col);
            emit_tri(m, b11, b01, t01, axis, col);
            emit_tri(m, b11, t01, t11, axis, col);
            emit_tri(m, b01, b00, t00, axis, col);
            emit_tri(m, b01, t00, t01, axis, col);
            emit_tri(m, t00, t10, t11, axis, col); // lumpy top
            emit_tri(m, t00, t11, t01, axis, col);
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
    for (u32 i = 0; i < kHouseVariants; ++i) {
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
    }
    return bushes_[0];
}

} // namespace alryn
