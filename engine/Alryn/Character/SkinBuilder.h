#pragma once

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Core/Math.h>

#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

// Low-poly skinned-mesh building blocks shared by the body (BodyMesh) and the equipment (OutfitMesh):
// weighted rings lofted into tubes, spheres, and end caps, plus smooth-normal averaging. All operate
// on a SkinnedMesh in bind-pose space; the bone weights make the surface bend at the joints once posed.
namespace alryn::skinbuild {

inline constexpr int kSides = 8; // octagonal cross-sections (low-poly)
using Weights = std::initializer_list<std::pair<int, f32>>;

// Set (and normalise) up to kMaxInfluences bone weights on a vertex.
inline void set_w(SkinVertex& sv, const Weights& w) {
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

// A ring of kSides vertices around `center` in the plane spanned by (u, v), radius r.
inline u32 ring(SkinnedMesh& m, const Vec3& center, const Vec3& u, const Vec3& v, f32 r, const Weights& w,
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

// Bridge two equal rings into a band of quads (two triangles each).
inline void bridge(SkinnedMesh& m, u32 r0, u32 r1) {
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
inline void cap(SkinnedMesh& m, u32 r0, const Vec3& center, const Weights& w, BodyMaterial mat, bool flip) {
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
inline void perp_basis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 a = glm::length(axis) > 1e-5f ? glm::normalize(axis) : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 ref = std::abs(a.y) > 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    u = glm::normalize(glm::cross(a, ref));
    v = glm::normalize(glm::cross(a, u));
}

// A capped tube through `pts` (radii per point, weights per point) along a shared axis. Each ring may
// optionally be scaled non-uniformly across (u, v) by `squash` (z-flatten for a robe / breastplate).
inline void tube(SkinnedMesh& m, const std::vector<Vec3>& pts, const std::vector<f32>& radii,
                 const std::vector<Weights>& w, BodyMaterial mat, bool cap_start, bool cap_end,
                 f32 squash = 1.0f) {
    if (pts.size() < 2) {
        return;
    }
    Vec3 u, v;
    perp_basis(pts.back() - pts.front(), u, v);
    std::vector<u32> rings;
    rings.reserve(pts.size());
    for (usize i = 0; i < pts.size(); ++i) {
        const u32 first = static_cast<u32>(m.vertices.size());
        for (int s = 0; s < kSides; ++s) {
            const f32 a = TwoPi * static_cast<f32>(s) / static_cast<f32>(kSides);
            const Vec3 dir = u * std::cos(a) + v * (std::sin(a) * squash);
            SkinVertex sv;
            sv.position = pts[i] + dir * radii[i];
            sv.normal = glm::length(dir) > 1e-5f ? glm::normalize(dir) : u;
            sv.material = static_cast<u8>(mat);
            set_w(sv, w[i]);
            m.add_vertex(sv);
        }
        rings.push_back(first);
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

// A UV ellipsoid centred at `center` with per-axis radii, weighted to the given bone(s).
inline void ellipsoid(SkinnedMesh& m, const Vec3& center, const Vec3& radii, const Weights& w,
                      BodyMaterial mat) {
    constexpr int lat = 7, lon = 9;
    std::vector<u32> prev;
    for (int j = 0; j <= lat; ++j) {
        const f32 theta = Pi * static_cast<f32>(j) / static_cast<f32>(lat);
        const f32 y = std::cos(theta), rr = std::sin(theta);
        std::vector<u32> cur;
        for (int i = 0; i < lon; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(lon);
            const Vec3 n{rr * std::cos(a), y, rr * std::sin(a)};
            SkinVertex sv;
            sv.position = center + n * radii;
            sv.normal = glm::normalize(n / glm::max(radii, Vec3{1e-4f})); // ellipsoid surface normal
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

// A UV sphere centred at `center`, radius r, weighted to the given bone(s).
inline void sphere(SkinnedMesh& m, const Vec3& center, f32 r, const Weights& w, BodyMaterial mat) {
    ellipsoid(m, center, Vec3{r}, w, mat);
}

// Average face normals into shared vertices for a smooth-shaded surface.
inline void smooth_normals(SkinnedMesh& m) {
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

} // namespace alryn::skinbuild
