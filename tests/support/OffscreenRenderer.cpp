#include "OffscreenRenderer.h"

#include <Alryn/Core/Paths.h>

#include <fstream>

namespace alryn::test {

namespace {

// Matches the push_constant block in mesh.vert / mesh.frag (256 bytes).
struct PushConstants {
    Mat4 mvp;
    Mat4 model;
    Mat4 light_vp;
    Vec4 tint;
    Vec4 params;
    Vec4 sun;
    Vec4 sun_color;
};

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

// Mirrors the renderer's Lights UBO (std140) so the shots exercise the real mesh.frag fog +
// grading. Lighting arrays stay zeroed (count = 0); only the atmosphere fields are filled.
struct GpuSpot {
    Vec4 a, b, c, d;
    Mat4 vp;
};
struct GpuPoint {
    Vec4 a, b, c;
};
struct LightUbo {
    i32 count[4];
    GpuSpot spots[9];
    GpuPoint points[48];
    Vec4 player_peek;
    Vec4 cam_pos;
    Vec4 fog_color;  // rgb = fog colour, w = density
    Vec4 screen;     // xy = resolution, z = gloom
    Vec4 fog_volume; // x = road fog-bank strength (0 in shots), y = ground ref
};
constexpr VkDeviceSize kLightUboSize = sizeof(LightUbo);

void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
             VkImageLayout new_layout, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
             VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

OffscreenRenderer::~OffscreenRenderer() { shutdown(); }

bool OffscreenRenderer::init(u32 width, u32 height) {
    width_ = width;
    height_ = height;

    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_offscreen";
    if (!instance_.create(instance_config)) {
        return false;
    }
    if (!device_.create(instance_.handle()) || !device_.dynamic_rendering_enabled()) {
        return false;
    }

    if (!color_.create(device_, width_, height_, kColorFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT) ||
        !depth_.create(device_, width_, height_, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        return false;
    }
    if (!create_descriptors()) {
        return false;
    }

    vk::PipelineConfig config;
    config.vertex_spv = shader_path("mesh.vert.spv").string();
    config.fragment_spv = shader_path("mesh.frag.spv").string();
    config.color_format = kColorFormat;
    config.depth_format = kDepthFormat;
    config.push_constant_size = sizeof(PushConstants);
    config.cull_mode = VK_CULL_MODE_NONE;
    config.descriptor_set_layout = set_layout_;
    if (!pipeline_.create(device_, config)) {
        return false; // shaders not compiled (no glslc) -> caller skips
    }

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width_) * height_ * 4;
    if (!readback_.create(device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return false;
    }

    ready_ = true;
    return true;
}

// set 0: 0 = sun shadow map, 1 = spot atlas, 2 = light UBO. We supply 1x1 dummy
// depth textures + a zeroed UBO (count = 0) so the shader samples no lighting.
bool OffscreenRenderer::create_descriptors() {
    VkDescriptorSetLayoutBinding binds[3]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1] = binds[0];
    binds[1].binding = 1;
    binds[2].binding = 2;
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[2].descriptorCount = 1;
    binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = binds;
    if (vkCreateDescriptorSetLayout(device_.handle(), &layout_info, nullptr, &set_layout_) !=
        VK_SUCCESS) {
        return false;
    }

    if (!shadow_dummy_.create(device_, 1, 1, kDepthFormat,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT)) {
        return false;
    }
    device_.immediate_submit([&](VkCommandBuffer cmd) {
        barrier(cmd, shadow_dummy_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT);
    });

    if (!light_ubo_.create(device_, kLightUboSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        return false;
    }
    std::vector<u8> zero(kLightUboSize, 0);
    light_ubo_.upload(zero.data(), zero.size());

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_.handle(), &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device_.handle(), &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = pool_;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_.handle(), &set_alloc, &set_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo dummy{};
    dummy.sampler = sampler_;
    dummy.imageView = shadow_dummy_.view();
    dummy.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo ubo{};
    ubo.buffer = light_ubo_.handle();
    ubo.range = kLightUboSize;
    VkWriteDescriptorSet writes[3]{};
    for (int b = 0; b < 3; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = set_;
        writes[b].dstBinding = static_cast<u32>(b);
        writes[b].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &dummy;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &dummy;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &ubo;
    vkUpdateDescriptorSets(device_.handle(), 3, writes, 0, nullptr);
    return true;
}

Mesh* OffscreenRenderer::upload(const MeshData& data) {
    auto mesh = std::make_unique<Mesh>();
    if (!mesh->create(device_, data)) {
        return nullptr;
    }
    Mesh* raw = mesh.get();
    meshes_.push_back(std::move(mesh));
    return raw;
}

std::vector<u8> OffscreenRenderer::render(const std::vector<Draw>& draws, const Mat4& view,
                                          const Mat4& proj, const Vec3& background,
                                          const Vec3& sun_dir, const std::string& ppm_path) {
    std::vector<u8> out(static_cast<usize>(width_) * height_ * 4, 0);
    if (!ready_) {
        return out;
    }

    // Feed the shared atmosphere UBO so the shots show the real fog + grading: fog fades distant
    // geometry toward the background colour; the camera position comes from the view matrix.
    {
        LightUbo ubo{};
        ubo.cam_pos = Vec4{Vec3{glm::inverse(view)[3]}, 0.0f};
        ubo.fog_color = Vec4{background, 0.005f}; // light haze (the shot cameras sit far back)
        ubo.screen = Vec4{static_cast<f32>(width_), static_cast<f32>(height_), 0.0f, 0.0f};
        light_ubo_.upload(&ubo, sizeof(ubo));
    }

    device_.immediate_submit([&](VkCommandBuffer cmd) {
        barrier(cmd, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        barrier(cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = color_.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{background.r, background.g, background.b, 1.0f}};
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = depth_.view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {width_, height_}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);

        const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {width_, height_}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        pipeline_.bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &set_,
                                0, nullptr);

        for (const Draw& d : draws) {
            if (d.mesh == nullptr || !d.mesh->valid()) {
                continue;
            }
            PushConstants push{};
            push.model = d.model;
            push.mvp = proj * view * d.model;
            push.light_vp = Mat4{1.0f};
            push.tint = d.tint;
            push.params = Vec4{0.0f};
            push.sun = Vec4{glm::normalize(sun_dir), 1.0f};
            push.sun_color = Vec4{1.0f, 1.0f, 1.0f, 0.0f}; // white sun, no shadows
            vkCmdPushConstants(cmd, pipeline_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants), &push);
            d.mesh->bind(cmd);
            d.mesh->draw(cmd);
        }
        vkCmdEndRendering(cmd);

        barrier(cmd, color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width_, height_, 1};
        vkCmdCopyImageToBuffer(cmd, color_.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_.handle(), 1, &copy);
    });

    const auto* pixels = static_cast<const u8*>(readback_.map());
    std::copy(pixels, pixels + out.size(), out.begin());
    readback_.unmap();

    if (!ppm_path.empty()) {
        std::ofstream ppm(ppm_path, std::ios::binary);
        ppm << "P6\n" << width_ << " " << height_ << "\n255\n";
        for (u32 y = 0; y < height_; ++y) {
            for (u32 x = 0; x < width_; ++x) {
                const usize idx = (static_cast<usize>(y) * width_ + x) * 4;
                ppm.put(static_cast<char>(out[idx]));
                ppm.put(static_cast<char>(out[idx + 1]));
                ppm.put(static_cast<char>(out[idx + 2]));
            }
        }
    }
    return out;
}

void OffscreenRenderer::shutdown() {
    ready_ = false;
    if (device_.valid()) {
        device_.wait_idle();
    }
    meshes_.clear(); // free GPU meshes before the device
    pipeline_.destroy();
    readback_.destroy();
    light_ubo_.destroy();
    shadow_dummy_.destroy();
    depth_.destroy();
    color_.destroy();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
    device_.destroy();
    instance_.destroy();
}

} // namespace alryn::test
