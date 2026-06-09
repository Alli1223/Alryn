#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Engine/Subsystem.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanImage.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/Vulkan/VulkanPipeline.h>
#include <Alryn/Renderer/Vulkan/VulkanSwapchain.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace alryn {

class Window;
class Mesh;
class Camera;

struct RendererConfig {
    bool enable_validation = true;
    bool vsync = true;
    Vec3 clear_color{0.10f, 0.12f, 0.18f};
};

// The forward renderer, exposed as an engine Subsystem. Owns the whole Vulkan
// stack (instance, surface, device, swapchain, depth buffer, pipeline, per-frame
// command buffers + sync) and renders to the window each frame.
//
// Per-frame usage (driven by Application):
//     renderer.set_camera(cam);
//     if (renderer.begin_frame()) {
//         renderer.draw(mesh, model);  // any number of times
//         renderer.end_frame();
//     }
class Renderer : public Subsystem {
public:
    Renderer(Window& window, RendererConfig config = {});
    ~Renderer() override;

    const char* name() const override { return "Renderer"; }
    bool on_init(Engine& engine) override;
    void on_shutdown() override;

    void set_camera(const Camera& camera);
    void set_time(f32 seconds) { time_ = seconds; }
    bool begin_frame();

    // Opaque geometry (terrain, characters). tint multiplies the vertex colour.
    void draw(const Mesh& mesh, const Mat4& model, const Vec4& tint = Vec4{1.0f});
    // Alpha-blended geometry (foliage). tint.a is the opacity. Draw after opaque.
    void draw_transparent(const Mesh& mesh, const Mat4& model, const Vec4& tint);
    // Animated water surface (its own shader). Draw after opaque.
    void draw_water(const Mesh& mesh, const Mat4& model);

    void end_frame();

    void request_resize() { needs_resize_ = true; }

    vk::Device& device() { return device_; }
    f32 aspect() const;
    VkExtent2D extent() const { return swapchain_.extent(); }

private:
    struct FrameSync {
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
    };

    static constexpr u32 kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    bool create_depth();
    bool create_pipelines();
    bool create_sync_and_commands();
    void recreate_swapchain();
    void submit(const vk::Pipeline& pipeline, const Mesh& mesh, const Mat4& model, const Vec4& tint);

    Window& window_;
    RendererConfig config_;

    vk::Instance instance_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    vk::Device device_;
    vk::Swapchain swapchain_;
    vk::Image depth_;
    vk::Pipeline pipeline_opaque_;
    vk::Pipeline pipeline_foliage_;
    vk::Pipeline pipeline_water_;
    VkPipeline current_pipeline_ = VK_NULL_HANDLE; // avoids redundant binds within a frame

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<FrameSync> frames_;
    std::vector<VkSemaphore> render_finished_; // one per swapchain image

    u32 frame_index_ = 0;
    u32 image_index_ = 0;
    bool frame_active_ = false;
    bool needs_resize_ = false;

    Mat4 view_{1.0f};
    Mat4 projection_{1.0f};
    Vec3 camera_position_{0.0f};
    f32 time_ = 0.0f;
};

} // namespace alryn
