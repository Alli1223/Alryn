#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Vertex.h>

#include <functional>
#include <vector>

namespace alryn {

// Number of bones that can influence one vertex (linear-blend skinning).
inline constexpr int kMaxInfluences = 4;

// A vertex of a skinned mesh: its BIND-POSE position/normal plus up to kMaxInfluences bone influences
// (indices into the skeleton + weights that sum to 1). `material` is a colour-zone id resolved to a
// colour at skin time (so one mesh can be multi-coloured: skin, cloth, metal, ...).
struct SkinVertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    u8 material = 0;
    int bones[kMaxInfluences] = {0, 0, 0, 0};
    f32 weights[kMaxInfluences] = {1.0f, 0.0f, 0.0f, 0.0f};

    // Set the (up to 4) influences from a small list, normalising the weights.
    void set_weights(std::initializer_list<std::pair<int, f32>> w);
};

// A continuous mesh bound to a skeleton: bind-pose vertices + indices + the per-bone INVERSE-BIND
// matrices (inverse of each bone's bind-pose joint frame). Deform it with the posed joint matrices
// (CharacterModel::joint_matrices) to get renderable geometry. Pure data + maths - no GPU.
struct SkinnedMesh {
    std::vector<SkinVertex> vertices;
    std::vector<u32> indices;
    std::vector<Mat4> inverse_bind; // inverse_bind[b] = inverse(bind joint matrix of bone b)

    usize bone_count() const { return inverse_bind.size(); }
    u32 add_vertex(const SkinVertex& v) {
        vertices.push_back(v);
        return static_cast<u32>(vertices.size() - 1);
    }
    void triangle(u32 a, u32 b, u32 c) {
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    }
};

// CPU linear-blend skinning. For each vertex: skin = Σ wᵢ · (jointMatrices[boneᵢ] · inverse_bind[boneᵢ]);
// out position = skin · bind position, out normal = normalize(skin₃ₓ₃ · bind normal). `palette`
// resolves the vertex material id to a colour (identity-ish grey if not supplied). Fills `out`
// (resized to the vertex count) so a per-frame skin reuses one buffer with no allocation.
void skin(const SkinnedMesh& mesh, const std::vector<Mat4>& joint_matrices, std::vector<Vertex>& out,
          const std::function<Vec3(u8)>& palette = {});

} // namespace alryn
