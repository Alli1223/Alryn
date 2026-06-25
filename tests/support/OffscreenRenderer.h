#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanImage.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/Vulkan/VulkanPipeline.h>

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace alryn::test {

// A small headless renderer for test "screenshots": it draws flat-shaded meshes
// (the same mesh.vert/frag the game uses, lit by a sun, no shadows) to an offscreen
// image and reads the pixels back - no window or swapchain. init() returns false
// when a Vulkan device or the compiled shaders are unavailable, so callers skip
// gracefully (a CI runner has neither). This is the reusable basis for visual
// confirmation tests (house interiors, characters, props, ...).
class OffscreenRenderer {
public:
    struct Draw {
        const Mesh* mesh = nullptr;
        Mat4 model{1.0f};
        Vec4 tint{1.0f};
    };

    OffscreenRenderer() = default;
    ~OffscreenRenderer();
    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    bool init(u32 width = 480, u32 height = 320);
    bool ready() const { return ready_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

    // Uploads a mesh owned by the renderer (freed before the device on shutdown).
    Mesh* upload(const MeshData& data);

    // Renders the draws and returns RGBA8 pixels (width*height*4, row-major). When
    // ppm_path is non-empty also writes a binary P6 PPM there.
    // `sun_color`: rgb = sun colour, w = intensity. Default {1,1,1,1} = the original full white key
    // (scene-shot baselines unchanged). Pass a dimmer/warmer key (e.g. character previews) so surfaces
    // don't blow out near-white and materials read with proper contrast - matching the moodier in-game look.
    std::vector<u8> render(const std::vector<Draw>& draws, const Mat4& view, const Mat4& proj,
                           const Vec3& background, const Vec3& sun_dir, const std::string& ppm_path = "",
                           const Vec4& sun_color = Vec4{1.0f});

    void shutdown();

private:
    bool create_descriptors();

    bool ready_ = false;
    u32 width_ = 0;
    u32 height_ = 0;

    vk::Instance instance_;
    vk::Device device_;
    vk::Image color_;
    vk::Image depth_;
    vk::Image shadow_dummy_;
    vk::Buffer light_ubo_;
    vk::Buffer readback_;
    vk::Pipeline pipeline_;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Mesh>> meshes_;
};

} // namespace alryn::test
