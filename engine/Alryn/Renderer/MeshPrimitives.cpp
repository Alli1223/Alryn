#include <Alryn/Renderer/MeshPrimitives.h>

#include <cmath>
#include <utility>
#include <vector>

namespace alryn::primitives {

namespace {

// Appends one quad (two triangles, CCW from the outside) with a shared flat
// normal and colour.
void add_quad(MeshData& mesh, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              const Vec3& normal, const Vec3& color) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back({a, normal, color});
    mesh.vertices.push_back({b, normal, color});
    mesh.vertices.push_back({c, normal, color});
    mesh.vertices.push_back({d, normal, color});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
}

void append(MeshData& dst, const MeshData& src) {
    const u32 base = static_cast<u32>(dst.vertices.size());
    dst.vertices.insert(dst.vertices.end(), src.vertices.begin(), src.vertices.end());
    for (u32 i : src.indices) {
        dst.indices.push_back(base + i);
    }
}

// Emits a flat-shaded triangle, orienting its normal away from `center`.
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
    m.vertices.push_back({a, n, color});
    m.vertices.push_back({b, n, color});
    m.vertices.push_back({c, n, color});
    m.indices.push_back(base);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
}

// Box from y=0..height, width x width, centred on the axis (a tree trunk).
MeshData box_column(f32 width, f32 height, const Vec3& color) {
    MeshData m;
    const f32 r = width * 0.5f;
    add_quad(m, {r, 0, r}, {r, 0, -r}, {r, height, -r}, {r, height, r}, {1, 0, 0}, color);
    add_quad(m, {-r, 0, -r}, {-r, 0, r}, {-r, height, r}, {-r, height, -r}, {-1, 0, 0}, color);
    add_quad(m, {-r, 0, r}, {r, 0, r}, {r, height, r}, {-r, height, r}, {0, 0, 1}, color);
    add_quad(m, {r, 0, -r}, {-r, 0, -r}, {-r, height, -r}, {r, height, -r}, {0, 0, -1}, color);
    add_quad(m, {-r, height, r}, {r, height, r}, {r, height, -r}, {-r, height, -r}, {0, 1, 0}, color);
    return m;
}

// Open cone with a `sides`-gon base and apex up; centre axis is x=z=0.
MeshData cone(f32 radius, f32 height, int sides, f32 base_y, const Vec3& color) {
    MeshData m;
    const Vec3 apex{0.0f, base_y + height, 0.0f};
    const Vec3 axis{0.0f, base_y + height * 0.4f, 0.0f};
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 b0{std::cos(a0) * radius, base_y, std::sin(a0) * radius};
        const Vec3 b1{std::cos(a1) * radius, base_y, std::sin(a1) * radius};
        emit_tri(m, apex, b0, b1, axis, color);
    }
    return m;
}

// Squashed octahedron "blob" centred at (0, cy, 0) (leafy canopy).
MeshData blob(f32 rxz, f32 ry, f32 cy, const Vec3& color) {
    MeshData m;
    const Vec3 v[6] = {{rxz, cy, 0.0f},  {-rxz, cy, 0.0f},     {0.0f, cy, rxz},
                       {0.0f, cy, -rxz}, {0.0f, cy + ry, 0.0f}, {0.0f, cy - ry, 0.0f}};
    const int faces[8][3] = {{4, 0, 2}, {4, 2, 1}, {4, 1, 3}, {4, 3, 0},
                             {5, 2, 0}, {5, 1, 2}, {5, 3, 1}, {5, 0, 3}};
    const Vec3 center{0.0f, cy, 0.0f};
    for (const auto& f : faces) {
        emit_tri(m, v[f[0]], v[f[1]], v[f[2]], center, color);
    }
    return m;
}

// A tapered, double-sided strip following the polyline `pts` (width w0 -> wT),
// with `perp` the horizontal width axis. Normals are up-biased so blades/fronds
// stay lit. Colour fades base -> tip. Used for grass, ferns, reeds.
void ribbon(MeshData& m, const std::vector<Vec3>& pts, f32 w0, f32 wT, const Vec3& perp,
            const Vec3& c0, const Vec3& cT) {
    const usize n = pts.size();
    if (n < 2) {
        return;
    }
    for (usize i = 0; i + 1 < n; ++i) {
        const f32 t0 = static_cast<f32>(i) / static_cast<f32>(n - 1);
        const f32 t1 = static_cast<f32>(i + 1) / static_cast<f32>(n - 1);
        const f32 wa = glm::mix(w0, wT, t0);
        const f32 wb = glm::mix(w0, wT, t1);
        const Vec3 ca = glm::mix(c0, cT, t0);
        const Vec3 cb = glm::mix(c0, cT, t1);
        const Vec3 a0 = pts[i] - perp * wa;
        const Vec3 a1 = pts[i] + perp * wa;
        const Vec3 b0 = pts[i + 1] - perp * wb;
        const Vec3 b1 = pts[i + 1] + perp * wb;
        Vec3 nrm = glm::cross(b0 - a0, a1 - a0);
        const f32 len = glm::length(nrm);
        nrm = len > 1e-6f ? nrm / len : Vec3{0.0f, 1.0f, 0.0f};
        if (nrm.y < 0.0f) {
            nrm = -nrm;
        }
        nrm = glm::normalize(nrm + Vec3{0.0f, 0.6f, 0.0f});
        const u32 base = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({a0, nrm, ca});
        m.vertices.push_back({a1, nrm, ca});
        m.vertices.push_back({b1, nrm, cb});
        m.vertices.push_back({b0, nrm, cb});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
    }
}

// Small deterministic hash -> [0,1) for procedural variants.
f32 vrnd(u32 h, int salt) {
    const u32 v = (h ^ (static_cast<u32>(salt) * 0x9E3779B9u)) * 0x85EBCA77u;
    return static_cast<f32>((v >> 9) & 0xFFFFu) / 65536.0f;
}

} // namespace

MeshData cube(f32 size, const Vec3& color) {
    const f32 h = size * 0.5f;
    MeshData mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);

    // Each face: 4 corners CCW as seen from outside, plus its outward normal.
    add_quad(mesh, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1}, color);   // +Z
    add_quad(mesh, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1}, color); // -Z
    add_quad(mesh, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0}, color);   // +X
    add_quad(mesh, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0}, color); // -X
    add_quad(mesh, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0}, color);   // +Y
    add_quad(mesh, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0}, color); // -Y
    return mesh;
}

MeshData grid(u32 cells, f32 cell_size, const Vec3& color) {
    MeshData mesh;
    const f32 extent = static_cast<f32>(cells) * cell_size;
    const f32 origin = -extent * 0.5f;
    const Vec3 up{0.0f, 1.0f, 0.0f};

    for (u32 z = 0; z < cells; ++z) {
        for (u32 x = 0; x < cells; ++x) {
            const f32 x0 = origin + static_cast<f32>(x) * cell_size;
            const f32 z0 = origin + static_cast<f32>(z) * cell_size;
            const f32 x1 = x0 + cell_size;
            const f32 z1 = z0 + cell_size;

            // Subtle checkerboard tint so the flat ground reads as separate cells.
            const bool dark = ((x + z) & 1u) != 0;
            const Vec3 cell_color = dark ? color * 0.85f : color;

            add_quad(mesh, {x0, 0.0f, z1}, {x1, 0.0f, z1}, {x1, 0.0f, z0}, {x0, 0.0f, z0}, up,
                     cell_color);
        }
    }
    return mesh;
}

MeshData sphere(int longitude, int latitude, const Vec3& color) {
    MeshData m;
    const f32 r = 0.5f;
    const auto point = [&](f32 lat, f32 lon) {
        return Vec3{r * std::cos(lat) * std::cos(lon), r * std::sin(lat), r * std::cos(lat) * std::sin(lon)};
    };
    for (int j = 0; j < latitude; ++j) {
        const f32 t0 = Pi * static_cast<f32>(j) / static_cast<f32>(latitude) - HalfPi;
        const f32 t1 = Pi * static_cast<f32>(j + 1) / static_cast<f32>(latitude) - HalfPi;
        for (int i = 0; i < longitude; ++i) {
            const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(longitude);
            const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(longitude);
            emit_tri(m, point(t0, a0), point(t0, a1), point(t1, a1), Vec3{0.0f}, color);
            emit_tri(m, point(t0, a0), point(t1, a1), point(t1, a0), Vec3{0.0f}, color);
        }
    }
    return m;
}

MeshData cylinder(int sides, const Vec3& color) {
    MeshData m;
    const f32 r = 0.5f;
    const f32 hy = 0.5f;
    const Vec3 top{0.0f, hy, 0.0f};
    const Vec3 bot{0.0f, -hy, 0.0f};
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 t0{std::cos(a0) * r, hy, std::sin(a0) * r};
        const Vec3 t1{std::cos(a1) * r, hy, std::sin(a1) * r};
        const Vec3 b0{std::cos(a0) * r, -hy, std::sin(a0) * r};
        const Vec3 b1{std::cos(a1) * r, -hy, std::sin(a1) * r};
        emit_tri(m, t0, b0, b1, Vec3{0.0f}, color); // side
        emit_tri(m, t0, b1, t1, Vec3{0.0f}, color);
        emit_tri(m, top, t0, t1, Vec3{0.0f}, color); // top cap
        emit_tri(m, bot, b1, b0, Vec3{0.0f}, color); // bottom cap
    }
    return m;
}

MeshData capsule(int sides, int cap_rings, const Vec3& color) {
    MeshData m;
    const f32 r = 0.5f;          // x/z radius (fills the unit box width)
    const f32 mid = 0.16f;       // half-length of the straight cylindrical middle
    const f32 cap = 0.5f - mid;  // vertical extent of each rounded (hemispherical) cap

    // Ring profile from bottom pole to top pole: (y, radius). The caps are quarter
    // ellipses, so the ends are softly domed instead of flat - the bubbly look.
    std::vector<std::pair<f32, f32>> rings;
    rings.reserve(static_cast<usize>(cap_rings) * 2 + 2);
    for (int j = 0; j <= cap_rings; ++j) { // bottom cap (pole -> equator)
        const f32 a = HalfPi * static_cast<f32>(j) / static_cast<f32>(cap_rings);
        rings.emplace_back(-mid - cap * std::cos(a), r * std::sin(a));
    }
    for (int j = cap_rings; j >= 0; --j) { // top cap (equator -> pole)
        const f32 a = HalfPi * static_cast<f32>(j) / static_cast<f32>(cap_rings);
        rings.emplace_back(mid + cap * std::cos(a), r * std::sin(a));
    }

    for (usize k = 0; k + 1 < rings.size(); ++k) {
        const auto [y0, r0] = rings[k];
        const auto [y1, r1] = rings[k + 1];
        for (int i = 0; i < sides; ++i) {
            const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
            const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
            const Vec3 p00{std::cos(a0) * r0, y0, std::sin(a0) * r0};
            const Vec3 p01{std::cos(a1) * r0, y0, std::sin(a1) * r0};
            const Vec3 p10{std::cos(a0) * r1, y1, std::sin(a0) * r1};
            const Vec3 p11{std::cos(a1) * r1, y1, std::sin(a1) * r1};
            if (r0 < 1e-5f) { // bottom pole -> fan
                emit_tri(m, p10, p11, Vec3{0.0f, y0, 0.0f}, Vec3{0.0f}, color);
            } else if (r1 < 1e-5f) { // top pole -> fan
                emit_tri(m, p00, Vec3{0.0f, y1, 0.0f}, p01, Vec3{0.0f}, color);
            } else {
                emit_tri(m, p00, p01, p11, Vec3{0.0f}, color);
                emit_tri(m, p00, p11, p10, Vec3{0.0f}, color);
            }
        }
    }
    return m;
}

MeshData rounded_box(f32 bevel, const Vec3& color) {
    MeshData m;
    const f32 h = 0.5f;        // half-extent
    const f32 i = h - bevel;   // inset of the flat faces
    auto quad = [&](const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
        emit_tri(m, a, b, c, Vec3{0.0f}, color);
        emit_tri(m, a, c, d, Vec3{0.0f}, color);
    };

    // 6 inset faces.
    for (f32 s : {-1.0f, 1.0f}) {
        quad({s * h, -i, -i}, {s * h, i, -i}, {s * h, i, i}, {s * h, -i, i}); // X
        quad({-i, s * h, -i}, {i, s * h, -i}, {i, s * h, i}, {-i, s * h, i}); // Y
        quad({-i, -i, s * h}, {i, -i, s * h}, {i, i, s * h}, {-i, i, s * h}); // Z
    }
    // 12 edge chamfers.
    for (f32 sa : {-1.0f, 1.0f}) {
        for (f32 sb : {-1.0f, 1.0f}) {
            quad({sa * h, sb * i, -i}, {sa * i, sb * h, -i}, {sa * i, sb * h, i},
                 {sa * h, sb * i, i}); // along Z
            quad({-i, sa * h, sb * i}, {-i, sa * i, sb * h}, {i, sa * i, sb * h},
                 {i, sa * h, sb * i}); // along X
            quad({sb * i, -i, sa * h}, {sb * h, -i, sa * i}, {sb * h, i, sa * i},
                 {sb * i, i, sa * h}); // along Y
        }
    }
    // 8 corner triangles.
    for (f32 sx : {-1.0f, 1.0f}) {
        for (f32 sy : {-1.0f, 1.0f}) {
            for (f32 sz : {-1.0f, 1.0f}) {
                emit_tri(m, {sx * h, sy * i, sz * i}, {sx * i, sy * h, sz * i},
                         {sx * i, sy * i, sz * h}, Vec3{0.0f}, color);
            }
        }
    }
    return m;
}

MeshData grass_tuft(int blades, const Vec3& color) {
    MeshData m;
    const Vec3 root_c = color * 0.55f;                                       // dark at the base
    const Vec3 tip_c = glm::clamp(color * 1.2f + Vec3{0.06f, 0.08f, 0.02f},  // bright at the tip
                                  Vec3{0.0f}, Vec3{1.0f});
    for (int i = 0; i < blades; ++i) {
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(blades) + 0.4f;
        const f32 ca = std::cos(ang);
        const f32 sa = std::sin(ang);
        const f32 h = 0.30f + 0.06f * static_cast<f32>((i * 7) % 5) / 5.0f; // slight height variety
        const f32 w = 0.045f;
        const f32 lean = 0.12f;
        const Vec3 perp{-sa, 0.0f, ca};                   // blade width axis
        const Vec3 root{ca * 0.03f, 0.0f, sa * 0.03f};    // fanned out from centre
        const Vec3 b0 = root - perp * w;
        const Vec3 b1 = root + perp * w;
        const Vec3 tip = root + Vec3{ca * lean, h, sa * lean};
        const Vec3 n = glm::normalize(Vec3{ca * 0.3f, 0.7f, sa * 0.3f}); // up-biased so it stays lit
        const u32 base = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({b0, n, root_c});
        m.vertices.push_back({b1, n, root_c});
        m.vertices.push_back({tip, n, tip_c});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    return m;
}

MeshData flower(const Vec3& blossom) {
    MeshData m;
    const Vec3 stem_c{0.30f, 0.46f, 0.24f};
    const Vec3 centre_c{1.0f, 0.85f, 0.30f};
    const f32 stem_h = 0.28f;
    const f32 sw = 0.02f;

    // Stem: two crossed vertical quads so it reads from any angle.
    add_quad(m, {-sw, 0.0f, 0.0f}, {sw, 0.0f, 0.0f}, {sw, stem_h, 0.0f}, {-sw, stem_h, 0.0f},
             {0, 0, 1}, stem_c);
    add_quad(m, {0.0f, 0.0f, -sw}, {0.0f, 0.0f, sw}, {0.0f, stem_h, sw}, {0.0f, stem_h, -sw},
             {1, 0, 0}, stem_c);

    // Blossom: a flat ring of petals around a yellow centre.
    const Vec3 centre{0.0f, stem_h + 0.02f, 0.0f};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const int petals = 5;
    const f32 r = 0.12f;
    for (int i = 0; i < petals; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(petals);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(petals);
        const Vec3 p0 = centre + Vec3{std::cos(a0) * r, 0.0f, std::sin(a0) * r};
        const Vec3 p1 = centre + Vec3{std::cos(a1) * r, 0.0f, std::sin(a1) * r};
        const u32 base = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({centre, up, centre_c});
        m.vertices.push_back({p0, up, blossom});
        m.vertices.push_back({p1, up, blossom});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    return m;
}

MeshData fern(int variant, const Vec3& color) {
    MeshData m;
    const u32 h = static_cast<u32>(variant) * 2654435761u + 13u;
    const int fronds = 5 + static_cast<int>(vrnd(h, 1) * 4.0f);
    const Vec3 dark = color * 0.62f;
    const Vec3 light = glm::clamp(color * 1.3f + Vec3{0.04f, 0.08f, 0.0f}, Vec3{0.0f}, Vec3{1.0f});
    for (int f = 0; f < fronds; ++f) {
        const f32 ang = TwoPi * static_cast<f32>(f) / static_cast<f32>(fronds) + vrnd(h, f * 5) * 0.7f;
        const f32 ca = std::cos(ang);
        const f32 sa = std::sin(ang);
        const Vec3 dir{ca, 0.0f, sa};
        const Vec3 perp{-sa, 0.0f, ca};
        const f32 reach = 0.42f + vrnd(h, f * 5 + 1) * 0.28f;
        const f32 height = 0.42f + vrnd(h, f * 5 + 2) * 0.22f;
        // An arch that rises then droops back toward the ground at full reach.
        std::vector<Vec3> rib;
        constexpr int segs = 5;
        for (int k = 0; k <= segs; ++k) {
            const f32 t = static_cast<f32>(k) / static_cast<f32>(segs);
            rib.push_back(dir * (reach * t) + Vec3{0.0f, height * std::sin(t * Pi * 0.9f), 0.0f});
        }
        ribbon(m, rib, 0.06f, 0.012f, perp, dark, light);
    }
    return m;
}

MeshData tall_grass(int blades, const Vec3& color) {
    MeshData m;
    const Vec3 dark = color * 0.55f;
    const Vec3 tip = glm::clamp(color * 1.25f + Vec3{0.05f, 0.08f, 0.0f}, Vec3{0.0f}, Vec3{1.0f});
    for (int i = 0; i < blades; ++i) {
        const u32 h = static_cast<u32>(i) * 0x9E3779B9u + 1u;
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(blades) + vrnd(h, 1) * 0.6f;
        const f32 ca = std::cos(ang);
        const f32 sa = std::sin(ang);
        const Vec3 dir{ca, 0.0f, sa};
        const Vec3 perp{-sa, 0.0f, ca};
        const f32 height = 0.6f + vrnd(h, 2) * 0.35f;
        const f32 lean = 0.18f + vrnd(h, 3) * 0.18f;
        // Mostly upright, arching over near the tip.
        std::vector<Vec3> rib = {
            Vec3{0.0f}, dir * (lean * 0.3f) + Vec3{0.0f, height * 0.45f, 0.0f},
            dir * (lean * 0.7f) + Vec3{0.0f, height * 0.8f, 0.0f},
            dir * lean + Vec3{0.0f, height, 0.0f}};
        ribbon(m, rib, 0.04f, 0.008f, perp, dark, tip);
    }
    return m;
}

MeshData mushroom(const Vec3& cap, f32 scale, bool spots) {
    MeshData m;
    const Vec3 stem_c{0.93f, 0.90f, 0.82f};
    const f32 stem_h = 0.18f * scale;
    const f32 stem_r = 0.05f * scale;
    // Stem: a short octagonal column.
    constexpr int sides = 8;
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / sides;
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / sides;
        const Vec3 p0{std::cos(a0) * stem_r, 0.0f, std::sin(a0) * stem_r};
        const Vec3 p1{std::cos(a1) * stem_r, 0.0f, std::sin(a1) * stem_r};
        add_quad(m, p0, p1, p1 + Vec3{0.0f, stem_h, 0.0f}, p0 + Vec3{0.0f, stem_h, 0.0f},
                 glm::normalize(Vec3{std::cos((a0 + a1) * 0.5f), 0.0f, std::sin((a0 + a1) * 0.5f)}),
                 stem_c);
    }
    // Cap: a low cone seated on the stem, slightly overhanging.
    const f32 cap_r = 0.16f * scale;
    const f32 cap_h = 0.12f * scale;
    const Vec3 apex{0.0f, stem_h + cap_h, 0.0f};
    const Vec3 axis{0.0f, stem_h + cap_h * 0.4f, 0.0f};
    const f32 cap_y = stem_h - 0.02f * scale;
    for (int i = 0; i < 10; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / 10.0f;
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / 10.0f;
        const Vec3 b0{std::cos(a0) * cap_r, cap_y, std::sin(a0) * cap_r};
        const Vec3 b1{std::cos(a1) * cap_r, cap_y, std::sin(a1) * cap_r};
        emit_tri(m, apex, b0, b1, axis, cap);
        emit_tri(m, b0, b1, axis, axis + Vec3{0.0f, -1.0f, 0.0f}, cap * 0.7f); // underside
    }
    if (spots) {
        const Vec3 white{0.96f, 0.95f, 0.92f};
        for (int s = 0; s < 5; ++s) {
            const f32 a = TwoPi * static_cast<f32>(s) / 5.0f + 0.4f;
            const f32 rr = cap_r * 0.55f;
            const Vec3 c{std::cos(a) * rr, stem_h + cap_h * 0.62f, std::sin(a) * rr};
            const f32 d = 0.022f * scale;
            add_quad(m, c + Vec3{-d, 0, -d}, c + Vec3{d, 0, -d}, c + Vec3{d, 0.004f, d},
                     c + Vec3{-d, 0.004f, d}, Vec3{0, 1, 0}, white);
        }
    }
    return m;
}

MeshData ground_leaf(int variant, const Vec3& color) {
    MeshData m;
    const u32 h = static_cast<u32>(variant) * 374761393u + 3u;
    const int leaves = 3 + static_cast<int>(vrnd(h, 1) * 3.0f);
    const Vec3 dark = color * 0.7f;
    const Vec3 light = glm::clamp(color * 1.2f, Vec3{0.0f}, Vec3{1.0f});
    for (int i = 0; i < leaves; ++i) {
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(leaves) + vrnd(h, i + 2) * 0.5f;
        const f32 ca = std::cos(ang);
        const f32 sa = std::sin(ang);
        const Vec3 dir{ca, 0.0f, sa};
        const Vec3 perp{-sa, 0.0f, ca};
        const f32 len = 0.14f + vrnd(h, i + 9) * 0.08f;
        // A small flat leaf lifted just off the ground.
        std::vector<Vec3> rib = {Vec3{0.0f, 0.02f, 0.0f}, dir * (len * 0.5f) + Vec3{0.0f, 0.06f, 0.0f},
                                 dir * len + Vec3{0.0f, 0.05f, 0.0f}};
        ribbon(m, rib, 0.05f, 0.02f, perp, dark, light);
    }
    return m;
}

MeshData fallen_log(int variant, const Vec3& color) {
    MeshData m;
    const u32 h = static_cast<u32>(variant) * 2246822519u + 5u;
    const f32 length = 1.3f + vrnd(h, 1) * 0.8f;
    const f32 radius = 0.20f + vrnd(h, 2) * 0.08f;
    constexpr int sides = 8;
    const f32 x0 = -length * 0.5f;
    const f32 x1 = length * 0.5f;
    const Vec3 end_c{0.62f, 0.50f, 0.36f}; // pale cut wood
    const Vec3 moss{0.30f, 0.45f, 0.25f};
    auto ring = [&](int i) {
        const f32 a = TwoPi * static_cast<f32>(i) / sides;
        return Vec3{0.0f, radius + std::sin(a) * radius, std::cos(a) * radius};
    };
    for (int i = 0; i < sides; ++i) {
        const Vec3 r0 = ring(i);
        const Vec3 r1 = ring(i + 1);
        const f32 a = TwoPi * (static_cast<f32>(i) + 0.5f) / sides;
        const Vec3 nrm{0.0f, std::sin(a), std::cos(a)};
        const bool top = nrm.y > 0.45f;
        const Vec3 bark = (top ? glm::mix(color, moss, 0.5f) : color) * (0.85f + vrnd(h, i + 3) * 0.3f);
        add_quad(m, {x0, r0.y, r0.z}, {x1, r0.y, r0.z}, {x1, r1.y, r1.z}, {x0, r1.y, r1.z}, nrm, bark);
    }
    // Cut ends (tris fanned to the axis).
    for (int i = 0; i < sides; ++i) {
        const Vec3 r0 = ring(i);
        const Vec3 r1 = ring(i + 1);
        emit_tri(m, {x1, radius, 0.0f}, {x1, r0.y, r0.z}, {x1, r1.y, r1.z}, {x1 - 1.0f, radius, 0.0f},
                 end_c);
        emit_tri(m, {x0, radius, 0.0f}, {x0, r0.y, r0.z}, {x0, r1.y, r1.z}, {x0 + 1.0f, radius, 0.0f},
                 end_c);
    }
    return m;
}

MeshData box(const Vec3& lo, const Vec3& hi, const Vec3& color) {
    MeshData m;
    add_quad(m, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
             {0, 0, 1}, color); // +Z
    add_quad(m, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z},
             {0, 0, -1}, color); // -Z
    add_quad(m, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z},
             {1, 0, 0}, color); // +X
    add_quad(m, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z},
             {-1, 0, 0}, color); // -X
    add_quad(m, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
             {0, 1, 0}, color); // +Y
    add_quad(m, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
             {0, -1, 0}, color); // -Y
    return m;
}

MeshData bush(int variant, const Vec3& color) {
    MeshData m;
    const u32 h = static_cast<u32>(variant) * 2654435761u + 1u;
    auto rnd = [&](int salt) {
        const u32 v = (h ^ (static_cast<u32>(salt) * 0x9E3779B9u));
        return static_cast<f32>((v >> 8) & 0xFFFFu) / 65535.0f;
    };
    const int blobs = 3 + static_cast<int>(rnd(1) * 2.0f);
    for (int i = 0; i < blobs; ++i) {
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(blobs);
        const f32 r = 0.18f + rnd(i * 3 + 2) * 0.16f;
        const f32 rad = 0.34f + rnd(i * 3 + 3) * 0.18f;
        const f32 cy = 0.28f + rnd(i * 3 + 4) * 0.22f;
        MeshData b = blob(rad, rad * 0.85f, cy, color * (0.85f + rnd(i * 3 + 5) * 0.3f));
        for (Vertex& v : b.vertices) {
            v.position.x += std::cos(ang) * r;
            v.position.z += std::sin(ang) * r;
        }
        append(m, b);
    }
    return m;
}

MeshData rock(int variant, const Vec3& color) {
    const u32 h = static_cast<u32>(variant) * 374761393u + 7u;
    auto rnd = [&](int salt) {
        const u32 v = (h ^ (static_cast<u32>(salt) * 0x85EBCA77u));
        return static_cast<f32>((v >> 9) & 0xFFFFu) / 65535.0f;
    };
    // Start from a low-poly sphere, then jitter each vertex outward for facets.
    MeshData m = sphere(8, 5, color);
    for (Vertex& v : m.vertices) {
        const f32 key = std::floor(v.position.x * 13.0f) + std::floor(v.position.y * 7.0f) +
                        std::floor(v.position.z * 11.0f);
        const u32 hv = h ^ static_cast<u32>(static_cast<i32>(key) * 0x27D4EB2F);
        const f32 j = 0.7f + (static_cast<f32>((hv >> 8) & 0xFFFFu) / 65535.0f) * 0.6f;
        v.position.x *= j;
        v.position.z *= j;
        v.position.y = v.position.y * (0.45f + rnd(2) * 0.2f) + 0.28f; // squashed, sat on ground
    }
    m.recompute_flat_normals();
    return m;
}

TreeMeshData tree(int variant) {
    TreeMeshData t;
    const Vec3 bark{0.34f, 0.24f, 0.16f};
    const Vec3 bark2{0.28f, 0.20f, 0.14f};
    const Vec3 birch{0.82f, 0.82f, 0.78f};
    const Vec3 leaf{0.16f, 0.40f, 0.19f};  // deep forest green
    const Vec3 leaf2{0.22f, 0.50f, 0.24f}; // lighter highlight green

    switch (variant % 5) {
        case 0: { // tall pine - stacked cones
            const f32 th = 2.8f;
            t.trunk = box_column(0.30f, th, bark);
            append(t.foliage, cone(1.6f, 2.3f, 7, th - 0.7f, leaf));
            append(t.foliage, cone(1.2f, 2.0f, 7, th + 0.6f, leaf * 1.05f));
            append(t.foliage, cone(0.82f, 1.7f, 7, th + 1.9f, leaf2));
            append(t.foliage, cone(0.46f, 1.3f, 7, th + 3.1f, leaf2 * 1.05f));
            break;
        }
        case 1: { // round oak - tall trunk + stacked blobs
            const f32 th = 2.6f;
            t.trunk = box_column(0.36f, th, bark);
            append(t.foliage, blob(1.9f, 1.5f, th + 1.1f, leaf));
            append(t.foliage, blob(1.35f, 1.1f, th + 2.2f, leaf2));
            append(t.foliage, blob(0.85f, 0.75f, th + 3.0f, leaf2 * 1.06f));
            break;
        }
        case 2: { // tall slender birch - pale trunk, high small canopy
            const f32 th = 3.8f;
            t.trunk = box_column(0.20f, th, birch);
            append(t.foliage, blob(1.0f, 1.4f, th + 0.7f, leaf2));
            append(t.foliage, blob(0.72f, 0.95f, th + 1.8f, leaf2 * 1.08f));
            break;
        }
        case 3: { // big broad oak - thick trunk, wide low canopy
            const f32 th = 2.3f;
            t.trunk = box_column(0.46f, th, bark2);
            append(t.foliage, blob(2.5f, 1.3f, th + 1.0f, leaf));
            append(t.foliage, blob(1.8f, 1.1f, th + 2.0f, leaf2));
            append(t.foliage, blob(1.1f, 0.85f, th + 2.8f, leaf2 * 1.05f));
            break;
        }
        default: { // weathered dead tree - bare-ish, sparse brown foliage
            const f32 th = 3.2f;
            t.trunk = box_column(0.26f, th, bark2);
            const Vec3 dead{0.40f, 0.33f, 0.22f};
            append(t.foliage, blob(0.8f, 0.55f, th + 0.5f, dead));
            append(t.foliage, blob(0.55f, 0.45f, th + 1.3f, dead * 1.08f));
            break;
        }
    }
    return t;
}

} // namespace alryn::primitives
