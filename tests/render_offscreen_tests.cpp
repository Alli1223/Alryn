#include <doctest/doctest.h>

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Paths.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/MeshPrimitives.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanImage.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/Vulkan/VulkanPipeline.h>

#include <fstream>
#include <string>

using namespace alryn;

namespace {

struct PushConstants {
    Mat4 mvp;
    Mat4 model;
    Vec4 tint;
    Vec4 params;
};

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                   VkImageLayout old_layout, VkImageLayout new_layout,
                   VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                   VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

// Renders a lit cube to an offscreen image (no window/swapchain), reads the
// pixels back, and checks the centre is the cube and the corner is background.
// Also writes a PPM next to the test binary for eyeballing. Skips gracefully on
// machines without a usable Vulkan device or dynamic rendering.
TEST_CASE("Renderer: offscreen render of a lit cube (headless)") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_offscreen_test";
    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance - skipping offscreen render test");
        return;
    }
    vk::Device device;
    if (!device.create(instance.handle())) {
        MESSAGE("No Vulkan device - skipping offscreen render test");
        return;
    }
    if (!device.dynamic_rendering_enabled()) {
        MESSAGE("Device lacks dynamic rendering - skipping offscreen render test");
        return;
    }

    constexpr u32 kWidth = 256;
    constexpr u32 kHeight = 256;
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

    vk::Image color;
    REQUIRE(color.create(device, kWidth, kHeight, color_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT));
    vk::Image depth;
    REQUIRE(depth.create(device, kWidth, kHeight, depth_format,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT));

    vk::Pipeline pipeline;
    vk::PipelineConfig pipeline_config;
    pipeline_config.vertex_spv = shader_path("mesh.vert.spv").string();
    pipeline_config.fragment_spv = shader_path("mesh.frag.spv").string();
    pipeline_config.color_format = color_format;
    pipeline_config.depth_format = depth_format;
    pipeline_config.push_constant_size = sizeof(PushConstants);
    pipeline_config.cull_mode = VK_CULL_MODE_NONE;
    if (!pipeline.create(device, pipeline_config)) {
        MESSAGE("Pipeline/shaders unavailable - skipping (are shaders in build/bin/shaders?)");
        return;
    }

    Mesh cube;
    REQUIRE(cube.create(device, primitives::cube(1.5f, Vec3{0.9f, 0.2f, 0.2f})));

    vk::Buffer readback;
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    REQUIRE(readback.create(device, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    const Vec3 eye{2.0f, 1.6f, 2.8f};
    const Mat4 view = look_at(eye, Vec3{0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    const Mat4 proj =
        perspective(radians(50.0f), static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 50.0f);
    PushConstants push{};
    push.model = Mat4{1.0f};
    push.mvp = proj * view * push.model;
    push.tint = Vec4{1.0f};
    push.params = Vec4{0.0f};

    const Vec3 background{0.10f, 0.12f, 0.18f};

    device.immediate_submit([&](VkCommandBuffer cmd) {
        image_barrier(cmd, color.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        image_barrier(cmd, depth.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = color.view();
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue.color = {{background.r, background.g, background.b, 1.0f}};

        VkRenderingAttachmentInfo depth_attachment{};
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth.view();
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {kWidth, kHeight}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color_attachment;
        rendering.pDepthAttachment = &depth_attachment;

        vkCmdBeginRendering(cmd, &rendering);

        const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(kWidth), static_cast<f32>(kHeight),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        pipeline.bind(cmd);
        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(PushConstants), &push);
        cube.bind(cmd);
        cube.draw(cmd);

        vkCmdEndRendering(cmd);

        image_barrier(cmd, color.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, color.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.handle(), 1, &copy);
    });

    const auto* pixels = static_cast<const u8*>(readback.map());
    auto pixel_at = [&](u32 x, u32 y) {
        const usize idx = (static_cast<usize>(y) * kWidth + x) * 4;
        return Vec3{pixels[idx] / 255.0f, pixels[idx + 1] / 255.0f, pixels[idx + 2] / 255.0f};
    };

    const Vec3 center = pixel_at(kWidth / 2, kHeight / 2);
    const Vec3 corner = pixel_at(4, 4);

    // Centre = the lit red cube; corner = the bluish background.
    CHECK(center.r > 0.3f);
    CHECK(center.r > center.b);
    CHECK(corner.r < 0.3f);
    CHECK(corner.b > corner.r);

    // Dump a PPM for visual inspection (P6 binary, RGB).
    const std::string ppm = (executable_dir() / "offscreen_cube.ppm").string();
    std::ofstream out(ppm, std::ios::binary);
    out << "P6\n" << kWidth << " " << kHeight << "\n255\n";
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize idx = (static_cast<usize>(y) * kWidth + x) * 4;
            out.put(static_cast<char>(pixels[idx]));
            out.put(static_cast<char>(pixels[idx + 1]));
            out.put(static_cast<char>(pixels[idx + 2]));
        }
    }
    readback.unmap();
    const std::string wrote_message = "Wrote " + ppm;
    MESSAGE(wrote_message);
}
