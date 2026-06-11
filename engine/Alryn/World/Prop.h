#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <string>
#include <vector>

namespace alryn {

// Which pipeline a prop part is drawn with.
//   Opaque  - lit + shadowed (trunks, rocks, logs)
//   Foliage - alpha-blended, lit, no depth write (leaves, bushes)
enum class PropLayer : u8 { Opaque, Foliage };

struct PropPart {
    MeshData mesh;
    PropLayer layer = PropLayer::Opaque;
};

// A static box collider in the prop's local space (e.g. a fallen log). xz blocking
// only; `height` is the vertical extent upward from center.y.
struct BoxCollider {
    Vec3 center{0.0f};
    Vec2 half_extents{0.5f};
    f32 height = 1.0f;
    f32 yaw = 0.0f;
};

// A catalogue entry: a multi-part low-poly prop plus any collision boxes.
struct PropDef {
    std::string name;
    std::vector<PropPart> parts;
    std::vector<BoxCollider> colliders;
};

// Categories the world scatter can place. Trees and ground vegetation keep their
// own optimised paths; these are the discrete forest props placed via PropScatter.
enum class PropCategory : u8 { Bush, Rock, Log };

struct PropInstance {
    PropCategory category = PropCategory::Bush;
    u8 variant = 0;
    Vec3 position{0.0f}; // base on the ground
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
};

} // namespace alryn
