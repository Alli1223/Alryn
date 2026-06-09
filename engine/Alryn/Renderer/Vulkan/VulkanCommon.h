#pragma once

#include <Alryn/Core/Log.h>

#include <vulkan/vulkan.h>

#include <cstdlib>

namespace alryn::vk {

// Human-readable VkResult, for diagnostics.
const char* result_string(VkResult result);

// Records a pipeline barrier that transitions a single-mip, single-layer image
// between layouts. Uses classic (sync1) barriers so it works regardless of
// whether synchronization2 is enabled.
void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                   VkImageLayout old_layout, VkImageLayout new_layout,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                   VkAccessFlags src_access, VkAccessFlags dst_access);

} // namespace alryn::vk

// Aborts with a clear message if a Vulkan call doesn't return VK_SUCCESS. Use for
// calls that must not fail; use explicit handling for expected failures (e.g.
// VK_ERROR_OUT_OF_DATE_KHR on resize).
#define ALRYN_VK_CHECK(expr)                                                            \
    do {                                                                               \
        const VkResult _alryn_vk_result = (expr);                                      \
        if (_alryn_vk_result != VK_SUCCESS) {                                          \
            ALRYN_FATAL("Vulkan error {} from `{}` ({}:{})",                           \
                        ::alryn::vk::result_string(_alryn_vk_result), #expr, __FILE__, \
                        __LINE__);                                                      \
            std::abort();                                                              \
        }                                                                              \
    } while (false)
