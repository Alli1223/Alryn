#include <Alryn/Renderer/Vulkan/VulkanPipeline.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vertex.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <fstream>
#include <utility>
#include <vector>

namespace alryn::vk {

VkShaderModule load_shader_module(VkDevice device, const std::string& spirv_path) {
    std::ifstream file(spirv_path, std::ios::binary | std::ios::ate);
    if (!file) {
        ALRYN_ERROR("Failed to open shader '{}'", spirv_path);
        return VK_NULL_HANDLE;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        ALRYN_ERROR("Shader '{}' has invalid SPIR-V size {}", spirv_path, size);
        return VK_NULL_HANDLE;
    }
    std::vector<u32> code(static_cast<usize>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = static_cast<usize>(size);
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateShaderModule failed for '{}'", spirv_path);
        return VK_NULL_HANDLE;
    }
    return module;
}

bool Pipeline::create(const Device& device, const PipelineConfig& config) {
    device_ = device.handle();

    VkShaderModule vert = load_shader_module(device_, config.vertex_spv);
    VkShaderModule frag = VK_NULL_HANDLE;
    if (!config.depth_only) {
        frag = load_shader_module(device_, config.fragment_spv);
    }
    if (vert == VK_NULL_HANDLE || (!config.depth_only && frag == VK_NULL_HANDLE)) {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vert, nullptr);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    const u32 stage_count = config.depth_only ? 1u : 2u;

    const auto binding = Vertex::binding_description();
    const auto attributes = Vertex::attribute_descriptions();
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (!config.vertexless) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<u32>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1; // values supplied dynamically

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = config.cull_mode;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    if (config.depth_bias) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = config.depth_bias_constant;
        raster.depthBiasSlopeFactor = config.depth_bias_slope;
    }

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const bool has_depth = config.depth_format != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = (has_depth && config.depth_test) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = (has_depth && config.depth_write) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = (config.blend || config.additive) ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    // Additive: add src*alpha to the framebuffer (glows/light shafts). Alpha blend:
    // standard src_alpha / one_minus_src_alpha.
    blend_attachment.dstColorBlendFactor =
        config.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = config.depth_only ? 0u : 1u;
    color_blend.pAttachments = config.depth_only ? nullptr : &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = config.push_constant_size;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (config.push_constant_size > 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
    }
    if (config.descriptor_set_layout != VK_NULL_HANDLE) {
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &config.descriptor_set_layout;
    }
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        ALRYN_ERROR("vkCreatePipelineLayout failed");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    // Dynamic rendering: declare the attachment formats instead of a render pass.
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = config.depth_only ? 0u : 1u;
    rendering.pColorAttachmentFormats = config.depth_only ? nullptr : &config.color_format;
    rendering.depthAttachmentFormat = config.depth_format;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = stage_count;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = layout_;
    pipeline_info.renderPass = VK_NULL_HANDLE; // using dynamic rendering

    const VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);

    if (result != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateGraphicsPipelines failed: {}", result_string(result));
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void Pipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
}

void Pipeline::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

Pipeline::~Pipeline() {
    destroy();
}

Pipeline::Pipeline(Pipeline&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pipeline_(std::exchange(other.pipeline_, VK_NULL_HANDLE)),
      layout_(std::exchange(other.layout_, VK_NULL_HANDLE)) {}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pipeline_ = std::exchange(other.pipeline_, VK_NULL_HANDLE);
        layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
    }
    return *this;
}

} // namespace alryn::vk
