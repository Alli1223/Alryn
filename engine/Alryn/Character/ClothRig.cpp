#include <Alryn/Character/ClothRig.h>

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

void ClothChain::step(const Vec3& anchor, const Vec3& wind, f32 gravity, f32 dt) {
    if (pos.size() < 2) {
        return;
    }
    if (attached) {
        pos[0] = anchor; // node 0 is pinned to the live attachment point
        prev[0] = anchor;
    } else {
        fall_age += dt;
    }

    // Verlet integration: carry velocity (pos - prev), add gravity + wind.
    const Vec3 accel = Vec3{0.0f, -gravity, 0.0f} + wind * wind_gain;
    const f32 retain = 1.0f - damping;
    const f32 dt2 = dt * dt;
    const usize first = attached ? 1u : 0u;
    for (usize i = first; i < pos.size(); ++i) {
        const Vec3 vel = (pos[i] - prev[i]) * retain;
        prev[i] = pos[i];
        pos[i] += vel + accel * dt2;
    }

    // Distance constraints hold the segment lengths (and so the cloth's shape). A few relaxation
    // iterations; the pinned anchor (node 0, while attached) only moves its child.
    for (int k = 0; k < 8; ++k) {
        for (usize i = 1; i < pos.size(); ++i) {
            Vec3 d = pos[i] - pos[i - 1];
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

void build_cloth_mesh(const ClothChain& c, const Vec3& side, const Vec3& color, MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    const usize n = c.pos.size();
    if (n < 2) {
        return;
    }
    const Vec3 s = glm::length(side) > 1e-5f ? glm::normalize(side) : Vec3{1.0f, 0.0f, 0.0f};

    auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& d, const Vec3& nrm) {
        const u32 base = static_cast<u32>(out.vertices.size());
        out.vertices.push_back(Vertex{a, nrm, color, 0.0f});
        out.vertices.push_back(Vertex{b, nrm, color, 0.0f});
        out.vertices.push_back(Vertex{d, nrm, color, 0.0f});
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
    };
    auto width_at = [&](usize i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(n - 1);
        return c.half_width * (1.0f - 0.18f * t); // taper a touch toward the hem
    };

    for (usize i = 0; i + 1 < n; ++i) {
        const f32 w0 = width_at(i), w1 = width_at(i + 1);
        const Vec3 l0 = c.pos[i] - s * w0, r0 = c.pos[i] + s * w0;
        const Vec3 l1 = c.pos[i + 1] - s * w1, r1 = c.pos[i + 1] + s * w1;
        Vec3 nrm = glm::cross(c.pos[i + 1] - c.pos[i], s);
        nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : Vec3{0.0f, 0.0f, 1.0f};
        // Double-sided so the cloth is lit + visible from both faces.
        tri(l0, r0, r1, nrm);
        tri(l0, r1, l1, nrm);
        tri(l0, r1, r0, -nrm);
        tri(l0, l1, r1, -nrm);
    }
}

void build_cloth_tube(const std::vector<ClothChain>& chains, bool closed, const Vec3& color,
                      MeshData& out) {
    out.vertices.clear();
    out.indices.clear();
    const usize nc = chains.size();
    if (nc < 2 || chains[0].pos.size() < 2) {
        return;
    }
    const usize rows = chains[0].pos.size();

    auto tri = [&](const Vec3& a, const Vec3& b, const Vec3& d, const Vec3& nrm) {
        const u32 base = static_cast<u32>(out.vertices.size());
        out.vertices.push_back(Vertex{a, nrm, color, 0.0f});
        out.vertices.push_back(Vertex{b, nrm, color, 0.0f});
        out.vertices.push_back(Vertex{d, nrm, color, 0.0f});
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
    };

    for (usize i = 0; i + 1 < nc + (closed ? 1u : 0u); ++i) {
        const usize j = (i + 1) % nc;
        const ClothChain& ci = chains[i];
        const ClothChain& cj = chains[j];
        if (cj.pos.size() != rows) {
            continue;
        }
        for (usize r = 0; r + 1 < rows; ++r) {
            const Vec3 a = ci.pos[r], b = cj.pos[r];
            const Vec3 d = cj.pos[r + 1], e = ci.pos[r + 1];
            Vec3 nrm = glm::cross(b - a, e - a);
            nrm = glm::length(nrm) > 1e-5f ? glm::normalize(nrm) : Vec3{0.0f, 0.0f, 1.0f};
            tri(a, b, d, nrm);
            tri(a, d, e, nrm);
            tri(a, d, b, -nrm); // back face
            tri(a, e, d, -nrm);
        }
    }
}

} // namespace alryn
