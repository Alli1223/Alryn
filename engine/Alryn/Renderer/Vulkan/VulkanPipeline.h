#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <string>

namespace alryn::vk {

class Device;

// Reads a compiled SPIR-V file into a shader module (VK_NULL_HANDLE on failure).
VkShaderModule load_shader_module(VkDevice device, const std::string& spirv_path);

struct PipelineConfig {
    std::string vertex_spv;
    std::string fragment_spv;
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_UNDEFINED; // UNDEFINED => no depth test
    u32 push_constant_size = 0;
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    bool blend = false;       // alpha blending (transparent materials)
    bool depth_write = true;  // write depth (turn off for transparent passes)
};

// A graphics pipeline for alryn::Vertex geometry, built for dynamic rendering
// (no VkRenderPass). Viewport/scissor are dynamic so it survives swapchain
// resizes without rebuilding.
class Pipeline : public NonCopyable {
public:
    Pipeline() = default;
    ~Pipeline();

    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;

    bool create(const Device& device, const PipelineConfig& config);
    void destroy();

    void bind(VkCommandBuffer cmd) const;

    VkPipeline handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

} // namespace alryn::vk
