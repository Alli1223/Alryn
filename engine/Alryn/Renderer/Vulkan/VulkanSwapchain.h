#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace alryn::vk {

class Device;

// Owns the swapchain and its image views. Rebuildable on resize (passes the old
// handle to the new one so the driver can reuse resources).
class Swapchain : public NonCopyable {
public:
    Swapchain() = default;
    ~Swapchain();

    bool create(const Device& device, VkSurfaceKHR surface, u32 width, u32 height, bool vsync = true);
    bool recreate(u32 width, u32 height);
    void destroy();

    VkResult acquire_next(VkSemaphore signal_semaphore, u32& out_index);
    VkResult present(VkQueue queue, VkSemaphore wait_semaphore, u32 image_index);

    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    u32 image_count() const { return static_cast<u32>(images_.size()); }
    VkImage image(u32 i) const { return images_[i]; }
    VkImageView view(u32 i) const { return views_[i]; }
    bool valid() const { return swapchain_ != VK_NULL_HANDLE; }

private:
    bool build(u32 width, u32 height, VkSwapchainKHR old_swapchain);
    void destroy_views();

    const Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
    bool vsync_ = true;

    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
};

} // namespace alryn::vk
