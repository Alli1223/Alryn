#include <Alryn/World/PropLibrary.h>

#include <Alryn/Renderer/MeshPrimitives.h>

namespace alryn {

namespace {

struct Rng {
    u32 s;
    explicit Rng(u32 seed) : s(seed == 0 ? 1u : seed) {}
    f32 next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<f32>(s & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
    }
    f32 range(f32 a, f32 b) { return a + (b - a) * next(); }
};

// Appends src (transformed) into dst, recomputing normals from the linear part.
void append_mesh(MeshData& dst, const MeshData& src, const Mat4& xf) {
    const Mat3 nm{xf};
    const u32 base = static_cast<u32>(dst.vertices.size());
    for (Vertex v : src.vertices) {
        v.position = Vec3{xf * Vec4{v.position, 1.0f}};
        v.normal = glm::normalize(nm * v.normal);
        dst.vertices.push_back(v);
    }
    for (u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
}

// One flat-shaded quad (a,b,c,d CCW) with an auto-computed normal.
void quad(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
          const Vec3& color) {
    const Vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, color});
    m.vertices.push_back({b, n, color});
    m.vertices.push_back({c, n, color});
    m.vertices.push_back({d, n, color});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
}

void tri(MeshData& m, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color) {
    const Vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.push_back({a, n, color});
    m.vertices.push_back({b, n, color});
    m.vertices.push_back({c, n, color});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}

// Merges a prop's parts/lights into accumulating opaque/emissive meshes (lanterns
// have no foliage), transforming geometry and lights by `xf`.
void merge_prop(MeshData& opaque, MeshData& emissive, std::vector<PropLight>& lights,
                const PropDef& src, const Mat4& xf) {
    for (const PropPart& part : src.parts) {
        append_mesh(part.layer == PropLayer::Emissive ? emissive : opaque, part.mesh, xf);
    }
    const Mat3 nm{xf};
    for (PropLight light : src.lights) {
        light.offset = Vec3{xf * Vec4{light.offset, 1.0f}};
        light.direction = glm::normalize(nm * light.direction);
        lights.push_back(light);
    }
}

} // namespace

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
    return def;
}

PropDef PropLibrary::build_lantern() {
    PropDef def;
    def.name = "lantern";
    const Vec3 frame{0.14f, 0.12f, 0.10f};
    const Vec3 glow{1.0f, 0.80f, 0.45f};
    MeshData opaque;
    MeshData emissive;
    // Warm glass box (the light source), centred at the origin.
    append_mesh(emissive, primitives::box({-0.11f, -0.14f, -0.11f}, {0.11f, 0.14f, 0.11f}, glow),
                Mat4{1.0f});
    // Dark caps top and bottom + a back strip suggesting a wall bracket.
    append_mesh(opaque, primitives::box({-0.14f, 0.14f, -0.14f}, {0.14f, 0.2f, 0.14f}, frame),
                Mat4{1.0f});
    append_mesh(opaque, primitives::box({-0.13f, -0.2f, -0.13f}, {0.13f, -0.14f, 0.13f}, frame),
                Mat4{1.0f});
    append_mesh(opaque, primitives::box({-0.03f, 0.18f, -0.2f}, {0.03f, 0.62f, -0.1f}, frame),
                Mat4{1.0f});
    def.parts.push_back({std::move(opaque), PropLayer::Opaque});
    def.parts.push_back({std::move(emissive), PropLayer::Emissive});
    PropLight light;
    light.offset = Vec3{0.0f, -0.05f, 0.12f};
    light.direction = glm::normalize(Vec3{0.0f, -0.45f, 1.0f});
    light.color = Vec3{1.0f, 0.74f, 0.42f};
    light.range = 15.0f;
    light.intensity = 1.5f;
    light.cone_deg = 85.0f;
    def.lights.push_back(light);
    return def;
}

PropDef PropLibrary::build_house(u32 seed) {
    Rng rng(seed * 2654435761u + 17u);
    PropDef def;
    def.name = "house";

    const f32 w = rng.range(1.8f, 2.6f);  // half width  (x)
    const f32 d = rng.range(1.8f, 2.5f);  // half depth  (z)
    const f32 h = rng.range(2.2f, 2.8f);  // wall height
    const f32 rh = rng.range(1.1f, 1.6f); // roof rise
    const f32 oh = 0.35f;                 // roof overhang
    const f32 t = 0.16f;                  // wall thickness
    const f32 dw = 0.6f;                  // door half-width
    const f32 dh = 2.0f;                  // door height

    static const Vec3 wall_cols[] = {{0.83f, 0.75f, 0.60f}, {0.74f, 0.66f, 0.55f},
                                     {0.60f, 0.45f, 0.34f}, {0.70f, 0.72f, 0.74f}};
    static const Vec3 roof_cols[] = {{0.55f, 0.26f, 0.20f}, {0.40f, 0.30f, 0.24f},
                                     {0.34f, 0.38f, 0.42f}};
    const Vec3 wall_col = wall_cols[seed % 4];
    const Vec3 roof_col = roof_cols[seed % 3];
    const Vec3 wood{0.34f, 0.22f, 0.14f};
    const Vec3 glow{1.0f, 0.85f, 0.5f};

    MeshData opaque;
    MeshData emissive;
    MeshData roof;

    // A wall slab (lo..hi box) that also registers a collider over its xz extent.
    auto wall = [&](const Vec3& lo, const Vec3& hi) {
        append_mesh(opaque, primitives::box(lo, hi, wall_col), Mat4{1.0f});
        BoxCollider c;
        c.center = Vec3{(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
        c.half_extents = Vec2{(hi.x - lo.x) * 0.5f, (hi.z - lo.z) * 0.5f};
        c.height = h;
        def.colliders.push_back(c);
    };

    // Floor + four walls (front split around a door opening, so you can walk in).
    append_mesh(opaque, primitives::box({-w, -0.05f, -d}, {w, 0.06f, d}, wood), Mat4{1.0f});
    wall({-w, 0.0f, -d}, {w, h, -d + t});       // back
    wall({-w, 0.0f, -d}, {-w + t, h, d});        // left
    wall({w - t, 0.0f, -d}, {w, h, d});          // right
    wall({-w, 0.0f, d - t}, {-dw, h, d});        // front-left
    wall({dw, 0.0f, d - t}, {w, h, d});          // front-right
    append_mesh(opaque, primitives::box({-dw, dh, d - t}, {dw, h, d}, wall_col), Mat4{1.0f}); // lintel

    // Gable roof (separate part so it can fade when you're inside).
    const f32 xl = -w - oh;
    const f32 xr = w + oh;
    const f32 zf = d + oh;
    const f32 zb = -d - oh;
    const f32 yb = h;
    const f32 yt = h + rh;
    quad(roof, {xl, yb, zf}, {xr, yb, zf}, {xr, yt, 0.0f}, {xl, yt, 0.0f}, roof_col); // +Z slope
    quad(roof, {xr, yb, zb}, {xl, yb, zb}, {xl, yt, 0.0f}, {xr, yt, 0.0f}, roof_col); // -Z slope
    tri(roof, {xl, yb, zb}, {xl, yb, zf}, {xl, yt, 0.0f}, roof_col);                  // -X gable
    tri(roof, {xr, yb, zf}, {xr, yb, zb}, {xr, yt, 0.0f}, roof_col);                  // +X gable

    // Door panel (set into the opening) + glowing windows.
    append_mesh(opaque, primitives::box({-dw + 0.05f, 0.0f, d - 0.02f}, {dw - 0.05f, dh, d}, wood),
                Mat4{1.0f});
    auto window = [&](const Vec3& lo, const Vec3& hi) {
        append_mesh(emissive, primitives::box(lo, hi, glow), Mat4{1.0f});
    };
    window({dw + 0.2f, 1.0f, d - 0.02f}, {w - 0.2f, 1.7f, d + 0.03f});   // front right
    window({-w + 0.2f, 1.0f, d - 0.02f}, {-dw - 0.2f, 1.7f, d + 0.03f}); // front left
    window({w - 0.02f, 1.0f, -d * 0.4f}, {w + 0.03f, 1.7f, d * 0.4f});   // +X side
    window({-w - 0.03f, 1.0f, -d * 0.4f}, {-w + 0.02f, 1.7f, d * 0.4f}); // -X side

    // Two lanterns flanking the door, casting light + shadows outward.
    const PropDef lantern = build_lantern();
    merge_prop(opaque, emissive, def.lights, lantern,
               glm::translate(Mat4{1.0f}, Vec3{dw + 0.35f, 2.0f, d + 0.16f}));
    merge_prop(opaque, emissive, def.lights, lantern,
               glm::translate(Mat4{1.0f}, Vec3{-dw - 0.35f, 2.0f, d + 0.16f}));

    def.parts.push_back({std::move(opaque), PropLayer::Opaque});
    def.parts.push_back({std::move(emissive), PropLayer::Emissive});
    def.parts.push_back({std::move(roof), PropLayer::Roof});
    def.footprint = Vec2{w, d};
    def.wall_height = h;
    return def;
}

PropLibrary::PropLibrary() {
    for (int i = 0; i < 3; ++i) {
        bushes_.push_back(build_bush(i));
    }
    for (int i = 0; i < 3; ++i) {
        rocks_.push_back(build_rock(i));
    }
    for (u32 i = 0; i < 4; ++i) {
        houses_.push_back(build_house(i + 1));
    }
}

const PropDef& PropLibrary::resolve(const PropInstance& inst) const {
    switch (inst.category) {
        case PropCategory::Bush: return bushes_[inst.variant % bushes_.size()];
        case PropCategory::Rock: return rocks_[inst.variant % rocks_.size()];
        case PropCategory::House: return houses_[inst.variant % houses_.size()];
    }
    return bushes_[0];
}

} // namespace alryn
