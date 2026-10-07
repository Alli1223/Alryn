#pragma once

#include <Alryn/Character/BodyMesh.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Core/Math.h>

#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

// Low-poly skinned-mesh building blocks shared by the body (BodyMesh) and the equipment (OutfitMesh):
// weighted elliptical rings lofted into limbs / torsos / garments, (shaped) ellipsoids, plus smooth-
// normal averaging. All operate on a SkinnedMesh in bind-pose space; the bone weights make the surface
// bend at the joints once posed.
namespace alryn::skinbuild {

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

// An ELLIPTICAL loft through `pts` with `sides` facets per ring: ring i is an ellipse of half-extent
// radii[i].x along `side` and radii[i].y along the "front" (both perpendicular to the overall path
// direction; for an upright path with side = +X the front is +Z). `weigh(vertex, ring, angle)` sets each
// vertex's bone weights (so a skirt can lean toward the nearer leg). `zig` (dagging) pulls every other
// vertex of the LAST ring further along the path - the scalloped / dagged hem of a medieval tunic or
// hood. Faces are wound outward whatever the path direction (checked against the widest ring), so the
// smooth normals light correctly. Returns the first vertex of each ring.
template <typename WeighFn>
inline std::vector<u32> loft_fn(SkinnedMesh& m, const std::vector<Vec3>& pts, const std::vector<Vec2>& radii,
                                WeighFn weigh, BodyMaterial mat, bool cap_start, bool cap_end, int sides = 10,
                                const Vec3& side_hint = Vec3{1.0f, 0.0f, 0.0f}, f32 zig = 0.0f) {
    std::vector<u32> rings;
    if (pts.size() < 2 || sides < 3) {
        return rings;
    }
    const Vec3 axis = glm::length(pts.back() - pts.front()) > 1e-5f ? glm::normalize(pts.back() - pts.front())
                                                                     : Vec3{0.0f, 1.0f, 0.0f};
    Vec3 side = side_hint - axis * glm::dot(side_hint, axis);
    side = glm::length(side) > 1e-4f ? glm::normalize(side) : Vec3{1.0f, 0.0f, 0.0f};
    // The ellipse is symmetric, so the front's sign doesn't matter (the winding is fixed up below).
    const Vec3 fr = glm::normalize(glm::cross(side, axis));
    rings.reserve(pts.size());
    usize widest = 0;
    for (usize i = 0; i < pts.size(); ++i) {
        const u32 first = static_cast<u32>(m.vertices.size());
        const bool last = i + 1 == pts.size();
        for (int s = 0; s < sides; ++s) {
            const f32 a = TwoPi * static_cast<f32>(s) / static_cast<f32>(sides);
            const Vec3 dir = side * (std::cos(a) * radii[i].x) + fr * (std::sin(a) * radii[i].y);
            SkinVertex sv;
            sv.position = pts[i] + dir;
            if (last && zig != 0.0f && (s % 2) == 0) {
                sv.position += axis * zig; // a dagged point hanging below the hem line
            }
            sv.normal = glm::length(dir) > 1e-6f ? glm::normalize(dir) : axis;
            sv.material = static_cast<u8>(mat);
            weigh(sv, i, a);
            m.add_vertex(sv);
        }
        rings.push_back(first);
        if (radii[i].x + radii[i].y > radii[widest].x + radii[widest].y) {
            widest = i;
        }
    }
    // Outward winding: test one quad on the widest ring against its radial direction.
    const usize r0 = widest + 1 < pts.size() ? widest : widest - 1;
    const Vec3 qa = m.vertices[rings[r0]].position;
    const Vec3 qb = m.vertices[rings[r0] + 1].position;
    const Vec3 qc = m.vertices[rings[r0 + 1] + 1].position;
    const Vec3 radial = (qa + qb + qc) / 3.0f - (pts[r0] + pts[r0 + 1]) * 0.5f;
    const bool flip = glm::dot(glm::cross(qb - qa, qc - qa), radial) < 0.0f;
    auto tri = [&](u32 a, u32 b, u32 c) {
        if (flip) {
            m.triangle(a, c, b);
        } else {
            m.triangle(a, b, c);
        }
    };
    for (usize i = 0; i + 1 < rings.size(); ++i) {
        for (int s = 0; s < sides; ++s) {
            const u32 a = rings[i] + static_cast<u32>(s);
            const u32 b = rings[i] + static_cast<u32>((s + 1) % sides);
            const u32 c = rings[i + 1] + static_cast<u32>((s + 1) % sides);
            const u32 d = rings[i + 1] + static_cast<u32>(s);
            tri(a, b, c);
            tri(a, c, d);
        }
    }
    auto cap_ring = [&](usize ri, bool at_start) {
        SkinVertex cv = m.vertices[rings[ri]];
        cv.position = pts[ri] + (at_start ? -axis : axis) * (0.25f * std::min(radii[ri].x, radii[ri].y));
        cv.normal = at_start ? -axis : axis;
        const u32 c = m.add_vertex(cv);
        for (int s = 0; s < sides; ++s) {
            const u32 a = rings[ri] + static_cast<u32>(s);
            const u32 b = rings[ri] + static_cast<u32>((s + 1) % sides);
            // Same handedness as the side wall: the start cap faces back down the path, the end cap ahead.
            if (at_start != flip) {
                m.triangle(c, b, a);
            } else {
                m.triangle(c, a, b);
            }
        }
    };
    if (cap_start) {
        cap_ring(0, true);
    }
    if (cap_end) {
        cap_ring(rings.size() - 1, false);
    }
    return rings;
}

// loft_fn with one weight set per ring.
inline std::vector<u32> loft(SkinnedMesh& m, const std::vector<Vec3>& pts, const std::vector<Vec2>& radii,
                             const std::vector<Weights>& w, BodyMaterial mat, bool cap_start, bool cap_end,
                             int sides = 10, const Vec3& side_hint = Vec3{1.0f, 0.0f, 0.0f}, f32 zig = 0.0f) {
    return loft_fn(
        m, pts, radii, [&](SkinVertex& sv, usize ring, f32) { set_w(sv, w[ring]); }, mat, cap_start, cap_end,
        sides, side_hint, zig);
}

// A UV ellipsoid whose unit-sphere points are first reshaped by `shape(n) -> n'` (a jaw that narrows
// to the chin, a hood's swept-back crown, ...), then scaled by `radii` about `center`. `keep(n)` can
// drop faces (a hood's face opening); `weigh(vertex, n)` sets each vertex's bone weights.
template <typename WeighFn, typename ShapeFn, typename KeepFn>
inline void shaped_ellipsoid(SkinnedMesh& m, const Vec3& center, const Vec3& radii, WeighFn weigh,
                             BodyMaterial mat, ShapeFn shape, KeepFn keep, int lat = 8, int lon = 12) {
    std::vector<u32> prev;
    std::vector<Vec3> prev_n;
    for (int j = 0; j <= lat; ++j) {
        const f32 theta = Pi * static_cast<f32>(j) / static_cast<f32>(lat);
        const f32 y = std::cos(theta), rr = std::sin(theta);
        std::vector<u32> cur;
        std::vector<Vec3> cur_n;
        for (int i = 0; i < lon; ++i) {
            const f32 a = TwoPi * static_cast<f32>(i) / static_cast<f32>(lon);
            const Vec3 n{rr * std::sin(a), y, rr * std::cos(a)}; // a = 0 faces +Z (the front)
            const Vec3 sp = shape(n);
            SkinVertex sv;
            sv.position = center + sp * radii;
            sv.normal = glm::normalize(sp / glm::max(radii, Vec3{1e-4f}));
            sv.material = static_cast<u8>(mat);
            weigh(sv, n);
            cur.push_back(m.add_vertex(sv));
            cur_n.push_back(n);
        }
        if (j > 0) {
            for (int i = 0; i < lon; ++i) {
                const usize i0 = static_cast<usize>(i), i1 = static_cast<usize>((i + 1) % lon);
                if (!keep(glm::normalize(prev_n[i0] + prev_n[i1] + cur_n[i0] + cur_n[i1]))) {
                    continue;
                }
                // Outward winding for an n-ordered (north -> south, +Z -> +X) grid.
                m.triangle(prev[i0], cur[i1], prev[i1]);
                m.triangle(prev[i0], cur[i0], cur[i1]);
            }
        }
        prev = cur;
        prev_n = cur_n;
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
