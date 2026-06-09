#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <cstdlib>
#include <cstring>
#include <utility>

namespace alryn::vk {

u32 Buffer::find_memory_type(VkPhysicalDevice physical, u32 type_bits,
                             VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physical, &mem);
    for (u32 i = 0; i < mem.memoryTypeCount; ++i) {
        const bool type_ok = (type_bits & (1u << i)) != 0;
        const bool props_ok = (mem.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_ok && props_ok) {
            return i;
        }
    }
    ALRYN_FATAL("No Vulkan memory type satisfies the requested properties");
    std::abort();
}

bool Buffer::create(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags memory_properties) {
    device_ = device.handle();
    size_ = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &buffer_info, nullptr, &buffer_) != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateBuffer failed (size {})", size);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex =
        find_memory_type(device.physical(), requirements.memoryTypeBits, memory_properties);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &memory_) != VK_SUCCESS) {
        ALRYN_ERROR("vkAllocateMemory failed (size {})", requirements.size);
        destroy();
        return false;
    }

    vkBindBufferMemory(device_, buffer_, memory_, 0);
    return true;
}

void* Buffer::map() {
    if (mapped_ == nullptr) {
        ALRYN_VK_CHECK(vkMapMemory(device_, memory_, 0, VK_WHOLE_SIZE, 0, &mapped_));
    }
    return mapped_;
}

void Buffer::unmap() {
    if (mapped_ != nullptr) {
        vkUnmapMemory(device_, memory_);
        mapped_ = nullptr;
    }
}

void Buffer::upload(const void* data, VkDeviceSize size, VkDeviceSize offset) {
    void* dst = map();
    std::memcpy(static_cast<u8*>(dst) + offset, data, static_cast<usize>(size));
    unmap();
}

void Buffer::destroy() {
    unmap();
    if (buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
    size_ = 0;
    device_ = VK_NULL_HANDLE;
}

Buffer::~Buffer() {
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      memory_(std::exchange(other.memory_, VK_NULL_HANDLE)),
      size_(std::exchange(other.size_, 0)),
      mapped_(std::exchange(other.mapped_, nullptr)) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
        mapped_ = std::exchange(other.mapped_, nullptr);
    }
    return *this;
}

} // namespace alryn::vk
