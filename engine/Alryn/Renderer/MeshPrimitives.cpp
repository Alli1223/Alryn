#include <Alryn/Renderer/MeshPrimitives.h>

#include <algorithm>
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

// Round, slightly tapered trunk: a `sides`-gon frustum from y=0..height (narrower at the
// top), centred on the axis. Faceted (flat per-face normals) for the low-poly look.
MeshData round_column(f32 width, f32 height, const Vec3& color, int sides = 7) {
    MeshData m;
    const f32 rb = width * 0.5f;          // base radius
    const f32 rt = width * 0.5f * 0.78f;  // taper in toward the top
    const Vec3 axis{0.0f, height * 0.5f, 0.0f};
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 b0{std::cos(a0) * rb, 0.0f, std::sin(a0) * rb};
        const Vec3 b1{std::cos(a1) * rb, 0.0f, std::sin(a1) * rb};
        const Vec3 t0{std::cos(a0) * rt, height, std::sin(a0) * rt};
        const Vec3 t1{std::cos(a1) * rt, height, std::sin(a1) * rt};
        emit_tri(m, b0, b1, t1, axis, color); // side (two tris, flat-shaded)
        emit_tri(m, b0, t1, t0, axis, color);
        emit_tri(m, Vec3{0.0f, height, 0.0f}, t0, t1, axis, color); // top cap
    }
    return m;
}

// Open cone with a `sides`-gon base and apex up; centre axis is x=z=0. (Kept for reuse - the pine
// now uses drooping tiers instead of plain cones.)
[[maybe_unused]] MeshData cone(f32 radius, f32 height, int sides, f32 base_y, const Vec3& color) {
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

// A faceted low-poly ball centred at `c`, radius `rxz` in xz and `ry` vertically (a flattenable
// sphere). `stacks` latitude bands x `slices` longitude segments; flat-shaded for the low-poly
// look. Far rounder than `blob` (an octahedron) - a CLUSTER of these reads as the soft, billowy
// multi-lobed tree canopy of the reference art.
MeshData round_blob(f32 rxz, f32 ry, const Vec3& c, const Vec3& color, int stacks = 4,
                    int slices = 7) {
    MeshData m;
    constexpr f32 top_light = 0.32f; // sun-lit top + shaded underside, baked per-face, so the canopy
                                     // reads with depth/dimension instead of a flat clump of one green
    auto vert = [&](int st, int sl) {
        const f32 phi = Pi * static_cast<f32>(st) / static_cast<f32>(stacks);   // 0 (top) .. Pi
        const f32 theta = TwoPi * static_cast<f32>(sl) / static_cast<f32>(slices);
        const f32 sr = std::sin(phi);
        return c + Vec3{std::cos(theta) * sr * rxz, std::cos(phi) * ry, std::sin(theta) * sr * rxz};
    };
    auto shade = [&](const Vec3& a, const Vec3& b, const Vec3& d) {
        const f32 fy = (a.y + b.y + d.y) / 3.0f;
        const f32 yf = glm::clamp((fy - c.y) / (ry > 1e-3f ? ry : 1.0f), -1.0f, 1.0f); // -1 base .. +1 top
        return color * (1.0f + top_light * yf);
    };
    for (int st = 0; st < stacks; ++st) {
        for (int sl = 0; sl < slices; ++sl) {
            const Vec3 a = vert(st, sl), b = vert(st, sl + 1);
            const Vec3 d0 = vert(st + 1, sl), d1 = vert(st + 1, sl + 1);
            emit_tri(m, a, d0, d1, c, shade(a, d0, d1)); // degenerate pole tris auto-dropped by emit_tri
            emit_tri(m, a, d1, b, c, shade(a, d1, b));
        }
    }
    return m;
}

// A tapered round limb (branch / twig): a `sides`-gon prism from `base` extending along
// `dir` for `length`, radius r0 at the base tapering to r1 at the tip. Used to grow
// branches off a trunk up into the canopy and small twigs poking out of the bark.
void limb(MeshData& m, const Vec3& base, const Vec3& dir, f32 length, f32 r0, f32 r1,
          const Vec3& color, int sides = 5) {
    const Vec3 axis = glm::normalize(dir);
    // An orthonormal frame around the limb axis to lay out the ring of side vertices.
    const Vec3 ref = std::abs(axis.y) > 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 u = glm::normalize(glm::cross(ref, axis));
    const Vec3 v = glm::cross(axis, u);
    const Vec3 tip = base + axis * length;
    const Vec3 center = base + axis * (length * 0.5f);
    for (int i = 0; i < sides; ++i) {
        const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 d0 = u * std::cos(a0) + v * std::sin(a0);
        const Vec3 d1 = u * std::cos(a1) + v * std::sin(a1);
        const Vec3 b0 = base + d0 * r0;
        const Vec3 b1 = base + d1 * r0;
        const Vec3 t0 = tip + d0 * r1;
        const Vec3 t1 = tip + d1 * r1;
        emit_tri(m, b0, b1, t1, center, color); // side (two tris, flat-shaded)
        emit_tri(m, b0, t1, t0, center, color);
    }
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

MeshData reed(int blades, const Vec3& color) {
    MeshData m;
    const Vec3 dark = color * 0.5f;
    const Vec3 tip = glm::clamp(color * 1.2f + Vec3{0.06f, 0.08f, 0.0f}, Vec3{0.0f}, Vec3{1.0f});
    for (int i = 0; i < blades; ++i) {
        const u32 h = static_cast<u32>(i) * 0x9E3779B9u + 7u;
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(blades) + vrnd(h, 1) * 0.7f;
        const f32 ca = std::cos(ang);
        const f32 sa = std::sin(ang);
        const Vec3 dir{ca, 0.0f, sa};
        const Vec3 perp{-sa, 0.0f, ca};
        const f32 height = 1.1f + vrnd(h, 2) * 0.8f; // tall marsh reeds
        const f32 lean = 0.08f + vrnd(h, 3) * 0.22f;
        std::vector<Vec3> rib = {Vec3{0.0f}, dir * (lean * 0.3f) + Vec3{0.0f, height * 0.5f, 0.0f},
                                 dir * (lean * 0.7f) + Vec3{0.0f, height * 0.82f, 0.0f},
                                 dir * lean + Vec3{0.0f, height, 0.0f}};
        ribbon(m, rib, 0.03f, 0.005f, perp, dark, tip);
        // Some blades carry a fat brown cattail seed-head just below the tip.
        if (vrnd(h, 4) < 0.34f) {
            const Vec3 headpos = dir * (lean * 0.85f) + Vec3{0.0f, height * 0.78f, 0.0f};
            MeshData head = round_column(0.07f, 0.2f, Vec3{0.34f, 0.19f, 0.08f}, 6);
            for (Vertex& v : head.vertices) {
                v.position += headpos;
            }
            append(m, head);
        }
    }
    return m;
}

MeshData cactus(int variant, const Vec3& color) {
    MeshData m;
    const f32 th = 1.35f + 0.55f * static_cast<f32>(variant & 1); // trunk height
    append(m, round_column(0.34f, th, color, 8));
    append(m, blob(0.17f, 0.15f, th, color)); // rounded crown
    // An up-bent arm: a short horizontal stub from the trunk that elbows skyward.
    auto arm = [&](f32 ang, f32 y) {
        const Vec3 out{std::cos(ang), 0.0f, std::sin(ang)};
        const Vec3 base{out.x * 0.18f, y, out.z * 0.18f};
        limb(m, base, out, 0.34f, 0.12f, 0.10f, color, 6);
        const Vec3 elbow = base + out * 0.34f;
        limb(m, elbow, Vec3{0.0f, 1.0f, 0.0f}, 0.5f, 0.10f, 0.09f, color, 6);
        MeshData cap = blob(0.1f, 0.1f, 0.0f, color);
        for (Vertex& v : cap.vertices) {
            v.position += elbow + Vec3{0.0f, 0.5f, 0.0f};
        }
        append(m, cap);
    };
    arm(0.0f, th * 0.5f);
    if (variant != 0) {
        arm(Pi, th * 0.62f); // a second arm on the opposite side
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
    // Cut ends with concentric end-grain RINGS (dark heartwood -> pale -> dark bark edge), so a cut
    // log reads as a real cut log rather than a flat blank disc.
    const Vec3 ring_cols[3] = {end_c * 0.74f, end_c * 1.06f, end_c * 0.86f};
    const f32 bands[4] = {0.0f, 0.4f, 0.72f, 1.0f};
    auto endv = [&](f32 x, int i, f32 t) {
        const Vec3 r = ring(i); // scale the rim vertex toward the centre (0, radius, 0)
        return Vec3{x, radius + (r.y - radius) * t, r.z * t};
    };
    for (const f32 ex : {x1, x0}) {
        const Vec3 nrm{ex > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f};
        const Vec3 inward{ex + (ex > 0.0f ? -1.0f : 1.0f), radius, 0.0f};
        for (int b = 0; b < 3; ++b) {
            for (int i = 0; i < sides; ++i) {
                if (b == 0) { // innermost band: a fan to the centre point
                    emit_tri(m, {ex, radius, 0.0f}, endv(ex, i, bands[1]), endv(ex, i + 1, bands[1]),
                             inward, ring_cols[0]);
                } else { // outer bands: rings of quads
                    add_quad(m, endv(ex, i, bands[b]), endv(ex, i + 1, bands[b]),
                             endv(ex, i + 1, bands[b + 1]), endv(ex, i, bands[b + 1]), nrm, ring_cols[b]);
                }
            }
        }
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
    const int blobs = 4 + static_cast<int>(rnd(1) * 3.0f);
    for (int i = 0; i < blobs; ++i) {
        const f32 ang = TwoPi * static_cast<f32>(i) / static_cast<f32>(blobs);
        const f32 r = 0.18f + rnd(i * 3 + 2) * 0.16f;
        const f32 rad = 0.34f + rnd(i * 3 + 3) * 0.18f;
        const f32 cy = 0.28f + rnd(i * 3 + 4) * 0.22f;
        // Soft, billowy leafy balls with a baked sun-lit-top gradient (round_blob, as the tree
        // canopies use) instead of the old hard 6-vertex octahedron - so a bush reads as foliage,
        // not a faceted green cone.
        MeshData b = round_blob(rad, rad * 0.88f, Vec3{0.0f, cy, 0.0f},
                                color * (0.85f + rnd(i * 3 + 5) * 0.3f), 3, 6);
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
    // Start from a low-poly sphere, jitter each vertex outward for facets, taper the lower
    // hemisphere inward and clamp it flush to y = 0 so the boulder sits ON the ground with
    // no floating overhang / dark hollow underneath.
    MeshData s = sphere(8, 5, color);
    for (Vertex& v : s.vertices) {
        const f32 sy = v.position.y; // -1..1 on the unit sphere
        const f32 key = std::floor(v.position.x * 13.0f) + std::floor(v.position.y * 7.0f) +
                        std::floor(v.position.z * 11.0f);
        const u32 hv = h ^ static_cast<u32>(static_cast<i32>(key) * 0x27D4EB2F);
        const f32 j = 0.7f + (static_cast<f32>((hv >> 8) & 0xFFFFu) / 65535.0f) * 0.6f;
        // Sit the WIDEST ring almost on the ground and dome upward from there, narrowing to
        // the crown: the whole lower hemisphere clamps flat into y = 0 (hidden under the rock)
        // so there is no belly that tucks under to cast a dark floating hollow.
        const f32 up = glm::clamp(sy, 0.0f, 1.0f);
        const f32 taper = sy < 0.0f ? 1.0f : glm::mix(1.0f, 0.80f, up); // only narrow the crown
        v.position.x *= j * taper;
        v.position.z *= j * taper;
        v.position.y = std::max(sy * (0.48f + rnd(2) * 0.22f), 0.0f); // dome from a flush ground rim
    }
    // De-index into independent facets so each gets its own crisp flat normal AND its own
    // stony colour (a shared-vertex sphere averages out to one smooth blob).
    MeshData m;
    const Vec3 moss{0.26f, 0.40f, 0.18f};   // damp green on top
    const Vec3 lichen{0.62f, 0.63f, 0.52f}; // pale crusty fleck
    for (usize i = 0; i + 2 < s.indices.size(); i += 3) {
        const Vec3& pa = s.vertices[s.indices[i]].position;
        const Vec3& pb = s.vertices[s.indices[i + 1]].position;
        const Vec3& pc = s.vertices[s.indices[i + 2]].position;
        Vec3 n = glm::cross(pb - pa, pc - pa);
        const f32 nlen = glm::length(n);
        if (nlen < 1e-7f) {
            continue; // skip the flattened-base degenerate faces
        }
        n /= nlen;
        const u32 fh = h ^ (static_cast<u32>(i) * 2654435761u);
        Vec3 col = color * (0.80f + 0.32f * (static_cast<f32>((fh >> 8) & 0xFFFFu) / 65535.0f));
        if (n.y > 0.30f) { // dapple moss / lichen onto upward-facing facets
            const f32 m01 = static_cast<f32>((fh >> 3) & 0xFFu) / 255.0f;
            if (m01 > 0.60f) {
                col = glm::mix(col, moss, glm::min((m01 - 0.60f) * 1.8f, 0.85f));
            } else if (m01 < 0.12f) {
                col = glm::mix(col, lichen, 0.5f);
            }
        }
        const u32 base = static_cast<u32>(m.vertices.size());
        m.vertices.push_back({pa, n, col, 0.0f});
        m.vertices.push_back({pb, n, col, 0.0f});
        m.vertices.push_back({pc, n, col, 0.0f});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
    }
    return m;
}

TreeMeshData tree(int variant) {
    TreeMeshData t;
    const Vec3 bark{0.34f, 0.24f, 0.16f};
    const Vec3 bark2{0.28f, 0.20f, 0.14f};
    const Vec3 birch{0.82f, 0.82f, 0.78f};
    const Vec3 leaf{0.16f, 0.40f, 0.19f};  // deep forest green
    const Vec3 leaf2{0.22f, 0.50f, 0.24f}; // lighter highlight green

    // Trunk radius at height y, matching round_column's base->top taper.
    auto trunk_radius = [](f32 width, f32 th, f32 y) {
        const f32 rb = width * 0.5f;
        return glm::mix(rb, rb * 0.78f, glm::clamp(y / th, 0.0f, 1.0f));
    };
    // Grow `count` branches up the upper trunk, angling outward + up into the canopy
    // (bark-coloured, in the opaque trunk mesh, so they stay solid when the leaves fade).
    auto add_branches = [&](f32 width, f32 th, const Vec3& wood, int count, f32 up_bias,
                            f32 len, int seed) {
        for (int i = 0; i < count; ++i) {
            const f32 frac = 0.5f + 0.45f * (static_cast<f32>(i) + vrnd(seed, i * 5)) /
                                                static_cast<f32>(count);
            const f32 y = th * frac;
            const f32 ang = TwoPi * (0.618f * static_cast<f32>(i) + vrnd(seed, i * 5 + 1));
            const Vec3 out{std::cos(ang), 0.0f, std::sin(ang)};
            const Vec3 dir = glm::normalize(out + Vec3{0.0f, up_bias, 0.0f});
            const f32 r = trunk_radius(width, th, y);
            const Vec3 base{out.x * r * 0.6f, y, out.z * r * 0.6f};
            const f32 l = len * (0.7f + 0.5f * vrnd(seed, i * 5 + 2));
            limb(t.trunk, base, dir, l, r * 0.42f, r * 0.12f, wood);
        }
    };
    // Scatter short twigs poking out of the bark for a rougher, more varied silhouette.
    auto add_twigs = [&](f32 width, f32 th, const Vec3& wood, int count, int seed) {
        for (int i = 0; i < count; ++i) {
            const f32 y = th * (0.3f + 0.6f * vrnd(seed, i * 7));
            const f32 ang = TwoPi * vrnd(seed, i * 7 + 1);
            const Vec3 out{std::cos(ang), 0.0f, std::sin(ang)};
            const Vec3 dir = glm::normalize(out + Vec3{0.0f, 0.3f + 0.5f * vrnd(seed, i * 7 + 2), 0.0f});
            const f32 r = trunk_radius(width, th, y);
            const Vec3 base{out.x * r * 0.7f, y, out.z * r * 0.7f};
            const f32 l = 0.25f + 0.4f * vrnd(seed, i * 7 + 3);
            limb(t.trunk, base, dir, l, std::max(r * 0.18f, 0.03f), 0.012f, wood, 4);
        }
    };
    // One drooping PINE TIER: a ring of `n` faceted branches that splay out from a central apex and
    // DROOP down at the tips, with jittered radius/droop per branch for a jagged, well-defined
    // low-poly conifer skirt (the reference look). Stacking tiers of decreasing radius builds the tree.
    auto pine_tier = [&](MeshData& m, f32 cy, f32 r, f32 tier_h, const Vec3& col, int n, int s) {
        const Vec3 apex{0.0f, cy + tier_h, 0.0f};
        const Vec3 axis{0.0f, cy, 0.0f};
        for (int i = 0; i < n; ++i) {
            const f32 a0 = TwoPi * static_cast<f32>(i) / static_cast<f32>(n);
            const f32 a1 = TwoPi * static_cast<f32>(i + 1) / static_cast<f32>(n);
            const f32 am = (a0 + a1) * 0.5f;
            const f32 rb = r * (0.62f + 0.2f * vrnd(static_cast<u32>(s), i * 3));      // inner (branches meet)
            const f32 rt = r * (1.0f + 0.16f * vrnd(static_cast<u32>(s), i * 3 + 1));   // branch-tip reach
            const f32 droop = 0.2f + 0.3f * vrnd(static_cast<u32>(s), i * 3 + 2);       // tip droops down
            const Vec3 b0{std::cos(a0) * rb, cy + 0.06f, std::sin(a0) * rb};
            const Vec3 b1{std::cos(a1) * rb, cy + 0.06f, std::sin(a1) * rb};
            const Vec3 tip{std::cos(am) * rt, cy - droop, std::sin(am) * rt};
            const Vec3 c = col * (0.82f + 0.3f * vrnd(static_cast<u32>(s), i * 3 + 1));
            emit_tri(m, apex, b0, tip, axis, c); // a drooping branch wedge (two facets)
            emit_tri(m, apex, tip, b1, axis, c);
        }
    };
    // A ROOT FLARE at the trunk base: a few short triangular buttress roots splaying to the ground.
    auto root_flare = [&](MeshData& m, f32 width, const Vec3& wood, int n, int s) {
        const f32 rb = width * 0.5f;
        for (int i = 0; i < n; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(n) + vrnd(static_cast<u32>(s), i) * 0.4f;
            const Vec3 out{std::cos(a), 0.0f, std::sin(a)};
            const Vec3 side{-out.z, 0.0f, out.x};
            const Vec3 top = out * rb * 0.7f + Vec3{0.0f, 0.55f, 0.0f};
            const Vec3 foot = out * rb * (1.7f + 0.6f * vrnd(static_cast<u32>(s), i + 3));
            const Vec3 w = side * rb * 0.55f;
            emit_tri(m, top, foot - w, foot + w, Vec3{0.0f, 0.4f, 0.0f}, wood * 0.92f);
        }
    };

    // A billowy deciduous CROWN: a big central faceted ball plus several smaller lobes clustered
    // around + above it, for the soft, rounded multi-lobed canopy of the reference trees. Baked
    // deep green; the per-tree autumn tint recolours it in TreeScatter.
    auto leafy_canopy = [&](MeshData& m, f32 cy, f32 r, const Vec3& c0, const Vec3& c1, int s) {
        append(m, round_blob(r, r * 0.9f, Vec3{0.0f, cy, 0.0f}, glm::mix(c0, c1, 0.25f)));
        const int lobes = 6;
        for (int i = 0; i < lobes; ++i) {
            const u32 us = static_cast<u32>(s);
            const f32 a = TwoPi * (0.618f * static_cast<f32>(i) + vrnd(us, i));
            const f32 dist = r * (0.42f + 0.30f * vrnd(us, i * 3 + 2));
            const f32 rr = r * (0.42f + 0.24f * vrnd(us, i * 3 + 1));
            const f32 hy = cy + r * (0.12f + 0.62f * vrnd(us, i * 3 + 3));
            const Vec3 ctr{std::cos(a) * dist, hy, std::sin(a) * dist};
            const Vec3 col = glm::mix(c0, c1, 0.35f + 0.6f * vrnd(us, i * 3));
            append(m, round_blob(rr, rr * 0.92f, ctr, col));
        }
    };

    switch (variant % 5) {
        case 0: { // detailed low-poly pine: tapered trunk + root flare + stacked drooping branch tiers
            const f32 th = 2.7f;
            t.trunk = round_column(0.36f, th, bark);
            root_flare(t.trunk, 0.36f, bark, 6, variant * 17 + 3);
            add_twigs(0.36f, th, bark, 3, variant * 17 + 2);
            const int tiers = 8;
            const f32 base_y = th - 1.0f, top_y = th + 2.8f;
            for (int k = 0; k < tiers; ++k) {
                const f32 f = static_cast<f32>(k) / static_cast<f32>(tiers - 1);
                const f32 cy = glm::mix(base_y, top_y, f);
                const f32 r = glm::mix(2.05f, 0.16f, std::pow(f, 1.12f)); // full, wide base -> point
                const f32 tier_h = glm::mix(0.92f, 0.42f, f);
                const Vec3 col = glm::mix(leaf, leaf2, f * 0.7f);
                pine_tier(t.foliage, cy, r, tier_h, col, 9, variant * 31 + k);
            }
            break;
        }
        case 1: { // round oak - tall trunk + a full billowy rounded crown
            const f32 th = 2.6f;
            t.trunk = round_column(0.36f, th, bark);
            add_branches(0.36f, th, bark, 5, 0.85f, 1.3f, variant * 17 + 1);
            add_twigs(0.36f, th, bark, 5, variant * 17 + 2);
            leafy_canopy(t.foliage, th + 1.5f, 1.95f, leaf, leaf2, variant * 31 + 1);
            break;
        }
        case 2: { // tall slender birch - pale trunk, high rounded canopy
            const f32 th = 3.8f;
            t.trunk = round_column(0.20f, th, birch);
            add_branches(0.20f, th, birch, 4, 1.1f, 0.8f, variant * 17 + 1);
            add_twigs(0.20f, th, birch, 4, variant * 17 + 2);
            leafy_canopy(t.foliage, th + 1.0f, 1.25f, leaf2, leaf2 * 1.1f, variant * 31 + 2);
            break;
        }
        case 3: { // big broad oak - thick trunk, wide low billowy crown
            const f32 th = 2.3f;
            t.trunk = round_column(0.46f, th, bark2);
            add_branches(0.46f, th, bark2, 6, 0.7f, 1.5f, variant * 17 + 1);
            add_twigs(0.46f, th, bark2, 6, variant * 17 + 2);
            leafy_canopy(t.foliage, th + 1.3f, 2.45f, leaf, leaf2, variant * 31 + 3);
            break;
        }
        default: { // weathered dead tree - bare-ish, sparse brown foliage
            const f32 th = 3.2f;
            t.trunk = round_column(0.26f, th, bark2);
            // Sparse canopy, so lean on gnarled bare branches + twigs for the dead look.
            add_branches(0.26f, th, bark2, 6, 0.5f, 1.4f, variant * 17 + 1);
            add_twigs(0.26f, th, bark2, 7, variant * 17 + 2);
            const Vec3 dead{0.40f, 0.33f, 0.22f};
            append(t.foliage, blob(0.8f, 0.55f, th + 0.5f, dead));
            append(t.foliage, blob(0.55f, 0.45f, th + 1.3f, dead * 1.08f));
            break;
        }
    }
    return t;
}

} // namespace alryn::primitives
