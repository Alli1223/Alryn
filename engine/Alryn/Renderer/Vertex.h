#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace alryn {

// Interleaved vertex for the forward renderer. Flat (per-face) normals give the
// faceted low-poly look; per-vertex colour lets meshes/terrain tint themselves.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec3 color;

    static VkVertexInputBindingDescription binding_description() {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = static_cast<u32>(sizeof(Vertex));
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 3> attribute_descriptions() {
        return {{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, position))},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, normal))},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<u32>(offsetof(Vertex, color))},
        }};
    }
};

} // namespace alryn
