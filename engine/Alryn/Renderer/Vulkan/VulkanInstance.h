#pragma once

#include <Alryn/Core/NonCopyable.h>

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace alryn::vk {

struct InstanceConfig {
    std::string app_name = "Alryn Application";
    bool enable_validation = false;
    // Platform/surface extensions to enable (e.g. from Window::required_instance_extensions()).
    std::vector<const char*> extensions;
};

// RAII wrapper over VkInstance plus the optional debug-utils messenger used for
// validation-layer output. Move-only; destroys on scope exit.
class Instance : public NonCopyable {
public:
    Instance() = default;
    ~Instance();

    Instance(Instance&& other) noexcept;
    Instance& operator=(Instance&& other) noexcept;

    bool create(const InstanceConfig& config);
    void destroy();

    VkInstance handle() const { return instance_; }
    bool valid() const { return instance_ != VK_NULL_HANDLE; }
    bool validation_enabled() const { return messenger_ != VK_NULL_HANDLE; }

private:
    static bool validation_layer_supported();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

} // namespace alryn::vk
