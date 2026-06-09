#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <utility>

namespace alryn::vk {

QueueFamilies Device::find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies families;

    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());

    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = props[i].queueFlags;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) && !families.graphics) {
            families.graphics = i;
        }
        if ((flags & VK_QUEUE_COMPUTE_BIT) && !families.compute) {
            families.compute = i;
        }
        if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) {
            families.transfer = i; // prefer a dedicated transfer queue
        }
        if (surface != VK_NULL_HANDLE && !families.present) {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported);
            if (supported == VK_TRUE) {
                families.present = i;
            }
        }
    }

    if (!families.transfer) {
        families.transfer = families.graphics;
    }
    return families;
}

bool Device::supports_extensions(VkPhysicalDevice device,
                                 const std::vector<const char*>& required) {
    if (required.empty()) {
        return true;
    }
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const char* name : required) {
        bool found = false;
        for (const auto& ext : available) {
            if (std::strcmp(ext.extensionName, name) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

int Device::rate_device(VkPhysicalDevice device, VkSurfaceKHR surface,
                        const std::vector<const char*>& required) {
    const QueueFamilies families = find_queue_families(device, surface);
    if (!families.has_graphics()) {
        return -1;
    }
    if (surface != VK_NULL_HANDLE && !families.present) {
        return -1;
    }
    if (!supports_extensions(device, required)) {
        return -1;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);

    int score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 1000;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 250;
    }
    score += static_cast<int>(props.limits.maxImageDimension2D);
    return score;
}

bool Device::create(VkInstance instance, const DeviceConfig& config) {
    std::vector<const char*> required_extensions = config.extensions;
    if (config.surface != VK_NULL_HANDLE) {
        required_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // ---- Pick the best physical device ------------------------------------
    u32 device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (device_count == 0) {
        ALRYN_ERROR("No Vulkan-capable physical devices found");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    int best_score = -1;
    for (VkPhysicalDevice candidate : devices) {
        const int score = rate_device(candidate, config.surface, required_extensions);
        if (score > best_score) {
            best_score = score;
            physical_ = candidate;
        }
    }
    if (physical_ == VK_NULL_HANDLE || best_score < 0) {
        ALRYN_ERROR("No suitable Vulkan physical device (need graphics{} queue + extensions)",
                    config.surface != VK_NULL_HANDLE ? "/present" : "");
        physical_ = VK_NULL_HANDLE;
        return false;
    }

    vkGetPhysicalDeviceProperties(physical_, &properties_);
    families_ = find_queue_families(physical_, config.surface);

    // ---- Query Vulkan 1.3 features we'd like ------------------------------
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &supported13;
    vkGetPhysicalDeviceFeatures2(physical_, &supported2);
    dynamic_rendering_ = supported13.dynamicRendering == VK_TRUE;

    // ---- Create the logical device + queues -------------------------------
    std::set<u32> unique_families{*families_.graphics};
    if (config.surface != VK_NULL_HANDLE && families_.present) {
        unique_families.insert(*families_.present);
    }

    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (u32 family : unique_families) {
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queue_infos.push_back(qi);
    }

    VkPhysicalDeviceVulkan13Features enable13{};
    enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enable13.dynamicRendering = supported13.dynamicRendering;
    enable13.synchronization2 = supported13.synchronization2;

    VkPhysicalDeviceFeatures2 enable2{};
    enable2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enable2.pNext = &enable13;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enable2; // pass features through pNext, so pEnabledFeatures stays null
    info.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    info.pQueueCreateInfos = queue_infos.data();
    info.enabledExtensionCount = static_cast<u32>(required_extensions.size());
    info.ppEnabledExtensionNames = required_extensions.data();

    const VkResult result = vkCreateDevice(physical_, &info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateDevice failed: {}", result_string(result));
        device_ = VK_NULL_HANDLE;
        return false;
    }

    vkGetDeviceQueue(device_, *families_.graphics, 0, &graphics_queue_);
    if (config.surface != VK_NULL_HANDLE && families_.present) {
        vkGetDeviceQueue(device_, *families_.present, 0, &present_queue_);
    }

    // ---- Command pool + fence for immediate_submit ------------------------
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = *families_.graphics;
    ALRYN_VK_CHECK(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_));

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ALRYN_VK_CHECK(vkCreateFence(device_, &fence_info, nullptr, &immediate_fence_));

    ALRYN_INFO("Vulkan device: '{}' (API {}.{}.{}, dynamic rendering: {})", properties_.deviceName,
               VK_API_VERSION_MAJOR(properties_.apiVersion),
               VK_API_VERSION_MINOR(properties_.apiVersion),
               VK_API_VERSION_PATCH(properties_.apiVersion), dynamic_rendering_ ? "yes" : "no");
    return true;
}

void Device::immediate_submit(const std::function<void(VkCommandBuffer)>& record) const {
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ALRYN_VK_CHECK(vkAllocateCommandBuffers(device_, &alloc_info, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ALRYN_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    record(cmd);
    ALRYN_VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    ALRYN_VK_CHECK(vkResetFences(device_, 1, &immediate_fence_));
    ALRYN_VK_CHECK(vkQueueSubmit(graphics_queue_, 1, &submit, immediate_fence_));
    ALRYN_VK_CHECK(vkWaitForFences(device_, 1, &immediate_fence_, VK_TRUE, UINT64_MAX));

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
}

void Device::wait_idle() const {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
}

void Device::destroy() {
    if (immediate_fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, immediate_fence_, nullptr);
        immediate_fence_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    present_queue_ = VK_NULL_HANDLE;
    families_ = {};
}

Device::~Device() {
    destroy();
}

Device::Device(Device&& other) noexcept
    : physical_(std::exchange(other.physical_, VK_NULL_HANDLE)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      families_(std::exchange(other.families_, {})),
      graphics_queue_(std::exchange(other.graphics_queue_, VK_NULL_HANDLE)),
      present_queue_(std::exchange(other.present_queue_, VK_NULL_HANDLE)),
      properties_(other.properties_),
      dynamic_rendering_(std::exchange(other.dynamic_rendering_, false)),
      command_pool_(std::exchange(other.command_pool_, VK_NULL_HANDLE)),
      immediate_fence_(std::exchange(other.immediate_fence_, VK_NULL_HANDLE)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        destroy();
        physical_ = std::exchange(other.physical_, VK_NULL_HANDLE);
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        families_ = std::exchange(other.families_, {});
        graphics_queue_ = std::exchange(other.graphics_queue_, VK_NULL_HANDLE);
        present_queue_ = std::exchange(other.present_queue_, VK_NULL_HANDLE);
        properties_ = other.properties_;
        dynamic_rendering_ = std::exchange(other.dynamic_rendering_, false);
        command_pool_ = std::exchange(other.command_pool_, VK_NULL_HANDLE);
        immediate_fence_ = std::exchange(other.immediate_fence_, VK_NULL_HANDLE);
    }
    return *this;
}

} // namespace alryn::vk
