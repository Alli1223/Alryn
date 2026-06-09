#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

namespace alryn::vk {

class Device;

// RAII image + backing memory + default image view. Used for depth buffers and
// offscreen colour targets.
class Image : public NonCopyable {
public:
    Image() = default;
    ~Image();

    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    bool create(const Device& device, u32 width, u32 height, VkFormat format,
                VkImageUsageFlags usage, VkImageAspectFlags aspect,
                VkMemoryPropertyFlags memory_properties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    void destroy();

    VkImage handle() const { return image_; }
    VkImageView view() const { return view_; }
    VkFormat format() const { return format_; }
    VkExtent2D extent() const { return extent_; }
    bool valid() const { return image_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
};

} // namespace alryn::vk
