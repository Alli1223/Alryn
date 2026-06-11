#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <string>
#include <vector>

namespace alryn {

// Which pipeline a prop part is drawn with.
//   Opaque   - lit + shadowed (walls, trunks, rocks)
//   Foliage  - alpha-blended, lit, no depth write (leaves, bushes)
//   Emissive - self-lit, full bright (lit windows, lantern glass)
//   Roof     - opaque normally, but the client fades it when you're inside
enum class PropLayer : u8 { Opaque, Foliage, Emissive, Roof };

struct PropPart {
    MeshData mesh;
    PropLayer layer = PropLayer::Opaque;
};

// A static box collider in the prop's local space (e.g. a house wall). xz blocking
// only; `height` is the vertical extent upward from center.y.
struct BoxCollider {
    Vec3 center{0.0f};
    Vec2 half_extents{0.5f};
    f32 height = 2.0f;
    f32 yaw = 0.0f;
};

// A spotlight attached to a prop (e.g. a lantern), in the prop's local space.
// Transformed to world space when the prop is placed.
struct PropLight {
    Vec3 offset{0.0f};                 // local position of the light
    Vec3 direction{0.0f, -0.3f, 1.0f}; // local spot direction
    Vec3 color{1.0f, 0.78f, 0.45f};    // warm lantern glow
    f32 range = 16.0f;
    f32 intensity = 1.4f;
    f32 cone_deg = 80.0f;
};

// A catalogue entry: a multi-part low-poly prop plus any attached lights and
// collision boxes. `footprint`/`wall_height` (houses) let the client tell when the
// local player is inside, to fade the roof.
struct PropDef {
    std::string name;
    std::vector<PropPart> parts;
    std::vector<PropLight> lights;
    std::vector<BoxCollider> colliders;
    Vec2 footprint{0.0f}; // xz half-extents of the enclosing walls (0 = not enclosable)
    f32 wall_height = 0.0f;
};

// Categories the world scatter can place. Trees/flowers keep their own optimized
// paths; these are the discrete props placed via PropScatter.
enum class PropCategory : u8 { Bush, Rock, House };

struct PropInstance {
    PropCategory category = PropCategory::Bush;
    u8 variant = 0;
    Vec3 position{0.0f}; // base on the ground
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
};

} // namespace alryn
