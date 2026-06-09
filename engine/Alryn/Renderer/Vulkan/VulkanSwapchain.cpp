#include <Alryn/Renderer/Vulkan/VulkanSwapchain.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace alryn::vk {

namespace {

VkSurfaceFormatKHR choose_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.front();
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes, bool vsync) {
    if (!vsync) {
        for (VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                return m; // low-latency triple buffering
            }
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR; // always supported, vsync
}

} // namespace

bool Swapchain::create(const Device& device, VkSurfaceKHR surface, u32 width, u32 height,
                       bool vsync) {
    device_ = &device;
    surface_ = surface;
    vsync_ = vsync;
    return build(width, height, VK_NULL_HANDLE);
}

bool Swapchain::recreate(u32 width, u32 height) {
    return build(width, height, swapchain_);
}

bool Swapchain::build(u32 width, u32 height, VkSwapchainKHR old_swapchain) {
    const VkPhysicalDevice physical = device_->physical();

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps);

    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &format_count, formats.data());

    u32 mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count, modes.data());

    if (formats.empty() || modes.empty()) {
        ALRYN_ERROR("Surface reports no formats/present modes");
        return false;
    }

    const VkSurfaceFormatKHR surface_format = choose_format(formats);
    const VkPresentModeKHR present_mode = choose_present_mode(modes, vsync_);

    VkExtent2D extent;
    if (caps.currentExtent.width != std::numeric_limits<u32>::max()) {
        extent = caps.currentExtent;
    } else {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false; // minimised; caller should retry later
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_;
    info.minImageCount = image_count;
    info.imageFormat = surface_format.format;
    info.imageColorSpace = surface_format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // graphics == present family here
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;

    VkSwapchainKHR new_swapchain = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateSwapchainKHR(device_->handle(), &info, nullptr, &new_swapchain);
    if (result != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateSwapchainKHR failed: {}", result_string(result));
        return false;
    }

    // Tear down old views/handle now that the new swapchain exists.
    destroy_views();
    if (old_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->handle(), old_swapchain, nullptr);
    }
    swapchain_ = new_swapchain;
    format_ = surface_format.format;
    extent_ = extent;

    u32 actual_count = 0;
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, nullptr);
    images_.resize(actual_count);
    vkGetSwapchainImagesKHR(device_->handle(), swapchain_, &actual_count, images_.data());

    views_.resize(actual_count);
    for (u32 i = 0; i < actual_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        ALRYN_VK_CHECK(vkCreateImageView(device_->handle(), &view_info, nullptr, &views_[i]));
    }

    ALRYN_INFO("Swapchain: {}x{}, {} images, {}", extent_.width, extent_.height, actual_count,
               present_mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "mailbox");
    return true;
}

VkResult Swapchain::acquire_next(VkSemaphore signal_semaphore, u32& out_index) {
    return vkAcquireNextImageKHR(device_->handle(), swapchain_, std::numeric_limits<u64>::max(),
                                 signal_semaphore, VK_NULL_HANDLE, &out_index);
}

VkResult Swapchain::present(VkQueue queue, VkSemaphore wait_semaphore, u32 image_index) {
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &image_index;
    return vkQueuePresentKHR(queue, &info);
}

void Swapchain::destroy_views() {
    for (VkImageView view : views_) {
        vkDestroyImageView(device_->handle(), view, nullptr);
    }
    views_.clear();
}

void Swapchain::destroy() {
    if (device_ == nullptr) {
        return;
    }
    destroy_views();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_->handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    images_.clear();
    extent_ = {0, 0};
    format_ = VK_FORMAT_UNDEFINED;
}

Swapchain::~Swapchain() {
    destroy();
}

} // namespace alryn::vk
