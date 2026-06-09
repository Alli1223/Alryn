#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <functional>
#include <optional>
#include <vector>

namespace alryn::vk {

struct QueueFamilies {
    std::optional<u32> graphics;
    std::optional<u32> present; // only set when a surface is provided
    std::optional<u32> compute;
    std::optional<u32> transfer;

    bool has_graphics() const { return graphics.has_value(); }
};

struct DeviceConfig {
    // VK_NULL_HANDLE => headless: no present queue / swapchain extension required.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::vector<const char*> extensions; // extra device extensions to require
};

// Selects a physical device and creates the logical device + queues. Enables
// Vulkan 1.3 dynamic rendering + synchronization2 when available (the renderer
// uses them to avoid render-pass/framebuffer objects). Also owns a small command
// pool for one-off GPU work via immediate_submit(). RAII.
class Device : public NonCopyable {
public:
    Device() = default;
    ~Device();

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    bool create(VkInstance instance, const DeviceConfig& config = {});
    void destroy();

    VkPhysicalDevice physical() const { return physical_; }
    VkDevice handle() const { return device_; }
    bool valid() const { return device_ != VK_NULL_HANDLE; }

    const QueueFamilies& queues() const { return families_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    VkQueue present_queue() const { return present_queue_; }

    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    const char* device_name() const { return properties_.deviceName; }
    bool dynamic_rendering_enabled() const { return dynamic_rendering_; }

    VkCommandPool command_pool() const { return command_pool_; }

    // Records and submits a one-shot command buffer, blocking until it finishes.
    // Used for layout transitions, staging copies, and the offscreen test.
    void immediate_submit(const std::function<void(VkCommandBuffer)>& record) const;

    // Blocks until the device is idle (call before tearing down resources).
    void wait_idle() const;

private:
    static QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface);
    static bool supports_extensions(VkPhysicalDevice device,
                                    const std::vector<const char*>& required);
    static int rate_device(VkPhysicalDevice device, VkSurfaceKHR surface,
                           const std::vector<const char*>& required);

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    QueueFamilies families_;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue present_queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    bool dynamic_rendering_ = false;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkFence immediate_fence_ = VK_NULL_HANDLE;
};

} // namespace alryn::vk
