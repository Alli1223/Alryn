#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace alryn {

// Interleaved vertex for the forward renderer. Flat (per-face) normals give the
// faceted low-poly look; per-vertex colour lets meshes/terrain tint themselves.
// `sway` is the vertex's height above its plant base (0 for static geometry); the
// vegetation shader (grass.vert) uses it to bend plants in wind / away from players.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec3 color;
    f32 sway = 0.0f;

    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = static_cast<u32>(sizeof(Vertex));
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 4> attribute_descriptions() {
        return {{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, position))},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, normal))},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, color))},
            {3, 0, VK_FORMAT_R32_SFLOAT, static_cast<u32>(offsetof(Vertex, sway))},
        }};
    }
};

} // namespace alryn
