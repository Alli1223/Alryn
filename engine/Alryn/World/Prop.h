#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <string>
#include <vector>

namespace alryn {

// Which pipeline a prop part is drawn with.
//   Opaque   - lit + shadowed (trunks, rocks, logs, posts, walls, furniture)
//   Foliage  - alpha-blended, lit, no depth write (leaves, bushes)
//   Emissive - self-lit, full bright (lantern glass, hearth fire, lit windows)
//   Roof     - opaque, but the client fades it when the player is inside (the whole
//              house shell: outer walls + roof + partitions, for a dollhouse view)
//   Glow     - self-lit + additive, depth-tested (light shafts spilling from windows)
enum class PropLayer : u8 { Opaque, Foliage, Emissive, Roof, Glow };

struct PropPart {
    MeshData mesh;
    PropLayer layer = PropLayer::Opaque;
};

// A static box collider in the prop's local space (e.g. a fallen log / fence). xz
// blocking only; `height` is the vertical extent upward from center.y.
struct BoxCollider {
    Vec3 center{0.0f};
    Vec2 half_extents{0.5f};
    f32 height = 1.0f;
    f32 yaw = 0.0f;
};

// A spotlight attached to a prop (e.g. a path lantern), in the prop's local space.
struct PropLight {
    Vec3 offset{0.0f};                  // local position of the light
    Vec3 direction{0.0f, -1.0f, 0.0f};  // local spot direction
    Vec3 color{1.0f, 0.78f, 0.45f};     // warm lantern glow
    f32 range = 12.0f;
    f32 intensity = 1.4f;
    f32 cone_deg = 110.0f;
};

// A catalogue entry: a multi-part low-poly prop plus collision boxes + lights.
// `footprint` (houses) is the interior xz half-extent so the client can fade the
// roof when the local player steps inside.
struct PropDef {
    std::string name;
    std::vector<PropPart> parts;
    std::vector<BoxCollider> colliders;
    std::vector<PropLight> lights;
    Vec2 footprint{0.0f};
    f32 wall_height = 0.0f;
    // Houses: where the resident villager sleeps + a spot just outside the door, in
    // the house's local space. Vary per house variant so any shape places NPCs right.
    Vec3 bed_spot{0.0f};
    Vec3 door_spot{0.0f, 0.0f, 3.5f};
};

// Categories the world scatter can place. Trees and ground vegetation keep their
// own optimised paths; these are the discrete props: forest debris, the
// fences/lanterns that line the winding paths, and the medieval village pieces
// (cottages, perimeter walls and gate towers).
enum class PropCategory : u8 {
    Bush, Rock, Log, Fence, Lantern, House, Wall, Gate, Well, Bridge, Market,
    Path, Planter, Fountain
};

// How many distinct house variants `PropLibrary` builds (cottages, longhouses, two-
// storey houses, manors, ...). The village scatter picks `variant % kHouseVariants`.
inline constexpr u32 kHouseVariants = 8;

struct PropInstance {
    PropCategory category = PropCategory::Bush;
    u8 variant = 0;
    Vec3 position{0.0f}; // base on the ground
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
};

} // namespace alryn
