#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <vector>

namespace alryn {

// A spring-bone cloth chain: a line of nodes hanging from an anchor on the body. Each frame the chain
// is Verlet-integrated under gravity + wind, then distance constraints hold the segment lengths so it
// keeps its shape - so it hangs down, swings with the anchor's motion (inertia) and blows in the wind.
// Pure maths (no GPU), so it's headless-testable. A cloth sheet mesh is built from the node positions.
//
// Detach() releases the anchor: the whole chain then free-falls (keeping its momentum) and tumbles to
// the ground - a cloak cut off or blown away. Until then node[0] is pinned to the live anchor.
struct ClothChain {
    std::vector<Vec3> pos;  // node world positions; pos[0] is the anchor end
    std::vector<Vec3> prev; // previous positions (Verlet) - velocity is pos - prev
    f32 seg_len = 0.12f;    // rest length between consecutive nodes
    f32 half_width = 0.3f;  // the sheet extends this far either side of the centre line
    f32 stiffness = 0.7f;   // constraint relaxation (0..1) - higher = stiffer cloth
    f32 damping = 0.04f;    // velocity loss per step (0 = none)
    f32 wind_gain = 1.0f;   // how strongly wind pushes this piece
    bool attached = true;   // false once cut/blown off (the anchor stops driving it)
    f32 fall_age = 0.0f;    // seconds since detaching (for fade-out)

    // Seat the chain hanging from `anchor` along `hang_dir` (normalised), `segments` nodes after the
    // anchor, each `seg` apart, sheet half-width `width`.
    void init(const Vec3& anchor, const Vec3& hang_dir, int segments, f32 seg, f32 width);

    // Advance the sim by dt. While attached, node[0] is pinned to `anchor`; `wind` is a world-space
    // force (m/s^2-ish) and `gravity` is the downward magnitude. Detached chains ignore `anchor`.
    void step(const Vec3& anchor, const Vec3& wind, f32 gravity, f32 dt);

    void detach() { attached = false; fall_age = 0.0f; }
    bool settled() const; // detached + nearly stopped (so it can fade / be removed)
};

// Build a (double-sided) cloth-sheet MeshData from the chain: a strip of quads `half_width` either side
// of the node line, along `side` (the sheet's left-right axis, world space). `color` tints it. The
// sheet tapers slightly toward the hem. Suitable to upload to a dynamic Mesh and draw each frame.
void build_cloth_mesh(const ClothChain& c, const Vec3& side, const Vec3& color, MeshData& out);

} // namespace alryn
