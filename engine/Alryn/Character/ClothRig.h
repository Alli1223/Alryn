#pragma once

#include <Alryn/Character/CharacterModel.h>
#include <Alryn/Character/Equipment.h>
#include <Alryn/Character/Outfit.h>
#include <Alryn/Character/SkinnedMesh.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <span>
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
    f32 damping = 0.04f;    // velocity loss per step (0 = none) - air drag on the world-space motion
    f32 inner_damping = 0.0f; // loss of the motion RELATIVE to the anchor per step - settles the swing
                              // without the drag that would fly a cape out like a flag at a walk
    f32 wind_gain = 1.0f;   // how strongly wind pushes this piece
    bool attached = true;   // false once cut/blown off (the anchor stops driving it)
    f32 fall_age = 0.0f;    // seconds since detaching (for fade-out)

    // Seat the chain hanging from `anchor` along `hang_dir` (normalised), `segments` nodes after the
    // anchor, each `seg` apart, sheet half-width `width`.
    void init(const Vec3& anchor, const Vec3& hang_dir, int segments, f32 seg, f32 width);

    // Advance the sim by dt. While attached, node[0] is pinned to `anchor`; `wind` is a world-space
    // force (m/s^2-ish) and `gravity` is the downward magnitude. Detached chains ignore `anchor`.
    void step(const Vec3& anchor, const Vec3& wind, f32 gravity, f32 dt);

    // The two halves of step(), for callers that interleave their own constraints (step_cloth):
    // pin node 0 + Verlet-integrate (`retain` = 1 - damping, `inner_keep` = 1 - inner_damping, for this
    // step), then ONE relaxation pass of the segment-length constraints.
    void integrate(const Vec3& anchor, const Vec3& accel, f32 retain, f32 dt, f32 inner_keep = 1.0f);
    void relax();

    void detach() { attached = false; fall_age = 0.0f; }
    bool settled() const; // detached + nearly stopped (so it can fade / be removed)
};

// ---- Body collision ----------------------------------------------------------------------------

// Which part of the body a collider covers - a cloth piece collides with a chosen set of them (a cape
// with everything, so it stays behind the arms; a skirt hung from the belt only with the hips + legs,
// so the arms swing past it and the broad chest doesn't push its waistband out).
enum BodyGroup : u8 {
    kBodyTorso = 1,
    kBodyHead = 2,
    kBodyArms = 4,
    kBodyHips = 8,
    kBodyLegs = 16,
    kBodyAll = 0xFF,
};

// A capsule the cloth can't pass through - the segment a..b swollen by radius r (a == b: a sphere).
struct ClothCollider {
    Vec3 a{0.0f};
    Vec3 b{0.0f};
    f32 r = 0.1f;
    u8 group = kBodyTorso;
};

// A collision capsule fixed to one bone, in that bone's JOINT frame (bind pose) - posed every frame by
// the live joint matrices, so the colliders lean, twist and swing with the body.
struct BodyCapsule {
    int bone = 0;
    Vec3 a{0.0f};
    Vec3 b{0.0f};
    f32 r = 0.1f;
    u8 group = kBodyTorso;
};

// The cloth-collision shape of one dressed character: capsules over the torso (a broad stadium of three
// spines), hips, head, arms, legs and feet, plus the bulky worn gear (pauldrons, gorget, quiver), each
// FITTED around the actual bind-pose geometry - the skinned body + outfit and the outfit's attachment
// pieces - so a plate knight's cape clears the plate and a peasant's sits close to the tunic.
struct BodyColliders {
    std::vector<BodyCapsule> caps;
    // The fitted torso cross-section in the Torso joint frame: half-width (x), half-depth (z) and the
    // depth centre - what a cape collar / robe neckline wraps around.
    f32 torso_half_width = 0.26f;
    f32 torso_half_depth = 0.22f;
    f32 torso_center_z = 0.0f;
};

BodyColliders fit_body_colliders(const CharacterModel& model, const SkinnedMesh& body,
                                 const SkinnedMesh& outfit);

// World-space colliders for this frame from the posed joint frames (CharacterModel::joint_matrices).
void pose_body_colliders(const BodyColliders& body, const std::vector<Mat4>& joints,
                         std::vector<ClothCollider>& out);

// Pushes one cloth node out of every collider (inflated by `margin`, the cloth's thickness) and takes
// out the velocity it carried INTO the surface, with `friction` on the slide (Verlet: `prev` is
// adjusted). `drape` (unit, or zero) is the side of the body the cloth belongs on: a node found on the
// far side of a capsule's axis - whipped through the body by a sudden turn - is pushed back out on the
// drape side, so a cape never ends up draped across the chest. `groups` = the BodyGroups it collides with.
// `body_step` is the body's own motion this step: contact friction drags the cloth along WITH the body
// (cloth resting on a walking figure moves with it, not braked toward a standstill).
void collide_cloth_node(Vec3& p, Vec3& prev, std::span<const ClothCollider> body, const Vec3& drape,
                        f32 margin, f32 friction, u8 groups = kBodyAll, const Vec3& body_step = Vec3{0.0f});

// ---- Worn cloth pieces -------------------------------------------------------------------------

// A heraldic / ecclesiastical device woven into a cloth piece (flags): a band down the front panel (a
// surcoat cross's foot, a vestment's orphrey) and / or a trim band round the hem.
enum ClothDevice : u8 {
    kDeviceNone = 0,
    kDevicePale = 1,
    kDeviceHem = 2,
};

// One flowing cloth piece on a character (a cape, a robe skirt, a stole): one or more chains hanging
// from a body bone, simulated as a single sheet - along-chain AND across-chain distance constraints
// interleaved with the body collisions - so it keeps its width, drapes over the shoulders, rests on
// the back and is pushed aside by a swinging leg instead of passing through it.
struct ClothPiece {
    std::vector<ClothChain> chains;  // 1 = a flat sheet (stole); N = a cape / robe (tube builder)
    std::vector<Vec3> anchor_locals; // per-chain anchor offset in the `anchor` bone's joint frame
    std::vector<Vec3> hang_locals;   // per-chain rest hang direction (character-local)
    std::vector<f32> cross_rest;     // rest distance between neighbouring chains, per pair per row
    Vec3 color{0.5f};
    BonePart anchor = BonePart::Torso;  // body joint the piece rides
    Vec3 side_local{1.0f, 0.0f, 0.0f};  // flat sheet: its left-right axis (character-local)
    Vec3 drape_local{0.0f};             // side of the body it belongs on (-Z = behind); zero = all round
    bool ring = false;                  // multi-chain (cape/skirt via tube builder) vs 1-chain sheet
    bool closed = true;                 // ring: closed tube (skirt) vs open sheet (cape)
    u8 collide = kBodyAll;              // BodyGroups it rests on (a skirt: hips + legs only)
    u8 device = kDeviceNone;            // ClothDevice flags woven into it
    f32 weave = 0.7f;                   // how far it may bunch across (fraction of its rest width): a
                                        // cape gathers into folds, a robe keeps its A-line hem
    Vec3 device_color{0.8f, 0.65f, 0.3f};
    int segments = 5;
    f32 seg = 0.13f;
    f32 half_width = 0.22f;
    bool inited = false;
    bool detached = false; // cut / blown off: the chains free-fall in world space, then despawn
    f32 detach_age = 0.0f; // seconds since detaching (lingers on the ground, then sinks + is removed)
};

// The per-frame world the cloth is stepped in.
struct ClothEnv {
    Vec3 wind{0.0f};
    f32 gravity = 9.5f;
    f32 dt = 1.0f / 60.0f;
    std::span<const ClothCollider> body; // the posed body colliders (empty = none)
    f32 margin = 0.03f;                  // cloth thickness kept off the body surface
    f32 friction = 0.35f;                // tangential velocity lost on contact (cloth grips the body)
    f32 ground = -1e9f;                  // floor height: a long hem pools here instead of sinking through
};

// The flowing pieces a character wears for its outfit + gear (the legendary capes, the robe / surcoat /
// tunic skirts, the cleric's stole, the warden's mantle), seated against the fitted body so the collar
// and neckline start just outside the worn gear. Colours come from the model's palette (apply_outfit
// first). Townsfolk + bandits wear none.
std::vector<ClothPiece> outfit_cloth(const CharacterModel& model, OutfitKind kind, const Equipment& eq,
                                     const BodyColliders& body);
// The carriage noble's grand, floor-length crimson cloak.
ClothPiece noble_cape(const CharacterModel& model, const BodyColliders& body);

// Seats (on first use) + steps an attached piece: anchors from the posed joint frames, the chains
// hanging along their rest directions turned by `root`, then a sheet step against `env.body`.
void step_cloth(ClothPiece& c, const CharacterModel& model, const std::vector<Mat4>& joints,
                const Mat4& root, const ClothEnv& env);

// Build a (double-sided) cloth-sheet MeshData from the chain: a strip of quads `half_width` either side
// of the node line, along `side` (the sheet's left-right axis, world space). `color` tints it. The
// sheet tapers slightly toward the hem. Suitable to upload to a dynamic Mesh and draw each frame.
// `outward` (optional) is the way its outer face looks (off the body): the inner face sits a hair
// behind it, so each side lights with its own normal instead of z-fighting.
void build_cloth_mesh(const ClothChain& c, const Vec3& side, const Vec3& color, MeshData& out,
                      const Vec3& outward = Vec3{0.0f});

// Build a (double-sided) cloth TUBE from a ring of chains (a skirt / robe): each adjacent pair of
// chains is bridged row-by-row into quads, wrapping cyclically when `closed`. All chains must have the
// same node count. `color` tints it; `device` (ClothDevice flags) picks panels out in `device_color` -
// the pale on the panel facing +Z (build attached cloth in the character's local frame). Each quad's
// outer face looks away from `axis` (a vertical line through it - the body's, x = z = 0 locally; by
// default the cloth's own centre), its inner face a hair behind, so both sides light correctly.
void build_cloth_tube(const std::vector<ClothChain>& chains, bool closed, const Vec3& color,
                      MeshData& out, u8 device = kDeviceNone, const Vec3& device_color = Vec3{0.8f},
                      const Vec3* axis = nullptr);

} // namespace alryn
