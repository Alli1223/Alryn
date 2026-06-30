#include <Alryn/Character/SkinnedMesh.h>

#include <cmath>

namespace alryn {

void SkinVertex::set_weights(std::initializer_list<std::pair<int, f32>> w) {
    for (int i = 0; i < kMaxInfluences; ++i) {
        bones[i] = 0;
        weights[i] = 0.0f;
    }
    f32 total = 0.0f;
    int i = 0;
    for (const auto& [b, wt] : w) {
        if (i >= kMaxInfluences || wt <= 0.0f) {
            continue;
        }
        bones[i] = b;
        weights[i] = wt;
        total += wt;
        ++i;
    }
    if (total > 1e-6f) {
        for (f32& wt : weights) {
            wt /= total;
        }
    } else {
        weights[0] = 1.0f; // degenerate -> pin to bone 0
    }
}

void skin(const SkinnedMesh& mesh, const std::vector<Mat4>& joint_matrices, std::vector<Vertex>& out,
          const std::function<Vec3(u8)>& palette) {
    const usize n = mesh.vertices.size();
    out.resize(n);
    const usize bones = mesh.inverse_bind.size();

    for (usize v = 0; v < n; ++v) {
        const SkinVertex& sv = mesh.vertices[v];
        Mat4 skin_mat{0.0f};
        f32 total_w = 0.0f;
        for (int i = 0; i < kMaxInfluences; ++i) {
            const f32 w = sv.weights[i];
            if (w <= 0.0f) {
                continue;
            }
            const usize b = static_cast<usize>(sv.bones[i]);
            if (b >= bones || b >= joint_matrices.size()) {
                continue;
            }
            skin_mat += w * (joint_matrices[b] * mesh.inverse_bind[b]);
            total_w += w;
        }
        if (total_w <= 1e-6f) {
            skin_mat = Mat4{1.0f}; // unweighted vertex stays put
        } else if (std::abs(total_w - 1.0f) > 1e-4f) {
            skin_mat *= (1.0f / total_w); // renormalise (defensive)
        }

        Vertex& o = out[v];
        o.position = Vec3{skin_mat * Vec4{sv.position, 1.0f}};
        o.normal = glm::normalize(Mat3{skin_mat} * sv.normal);
        o.color = palette ? palette(sv.material) : Vec3{0.8f};
        o.sway = 0.0f;
    }
}

} // namespace alryn
