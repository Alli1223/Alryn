#pragma once

#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

namespace alryn::vk {

class Device;

// RAII VkBuffer + backing VkDeviceMemory. For now we use host-visible memory and
// upload via memcpy (simple, fine for small/dynamic meshes). Device-local memory
// with a staging buffer is a later optimisation for large static geometry.
class Buffer : public NonCopyable {
public:
    Buffer() = default;
    ~Buffer();

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    bool create(const Device& device, VkDeviceSize size, VkBufferUsageFlags usage,
                VkMemoryPropertyFlags memory_properties);
    void destroy();

    // Host-visible memory only: copies `size` bytes from `data` at `offset`.
    void upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

    void* map();
    void unmap();

    VkBuffer handle() const { return buffer_; }
    VkDeviceSize size() const { return size_; }
    bool valid() const { return buffer_ != VK_NULL_HANDLE; }

    static u32 find_memory_type(VkPhysicalDevice physical, u32 type_bits,
                                VkMemoryPropertyFlags properties);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize size_ = 0;
    void* mapped_ = nullptr;
};

} // namespace alryn::vk
