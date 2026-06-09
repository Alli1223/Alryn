#include <Alryn/Renderer/Vulkan/VulkanImage.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h> // find_memory_type
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <utility>

namespace alryn::vk {

bool Image::create(const Device& device, u32 width, u32 height, VkFormat format,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect,
                   VkMemoryPropertyFlags memory_properties) {
    device_ = device.handle();
    format_ = format;
    extent_ = {width, height};

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateImage failed ({}x{})", width, height);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex =
        Buffer::find_memory_type(device.physical(), requirements.memoryTypeBits, memory_properties);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory_) != VK_SUCCESS) {
        ALRYN_ERROR("vkAllocateMemory failed for image");
        destroy();
        return false;
    }
    vkBindImageMemory(device_, image_, memory_, 0);

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateImageView failed");
        destroy();
        return false;
    }
    return true;
}

void Image::destroy() {
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image_, nullptr);
        image_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
    extent_ = {0, 0};
    format_ = VK_FORMAT_UNDEFINED;
}

Image::~Image() {
    destroy();
}

Image::Image(Image&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, {0, 0})) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        view_ = std::exchange(other.view_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_ = std::exchange(other.extent_, {0, 0});
    }
    return *this;
}

} // namespace alryn::vk
