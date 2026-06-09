#include <Alryn/Renderer/Vulkan/VulkanInstance.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>

#include <cstring>
#include <utility>

namespace alryn::vk {

namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ALRYN_ERROR("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ALRYN_WARN("[vulkan] {}", data->pMessage);
    } else {
        ALRYN_DEBUG("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE; // don't abort the triggering call
}

void populate_debug_info(VkDebugUtilsMessengerCreateInfoEXT& info) {
    info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debug_callback;
}

} // namespace

bool Instance::validation_layer_supported() {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, kValidationLayer) == 0) {
            return true;
        }
    }
    return false;
}

bool Instance::create(const InstanceConfig& config) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = config.app_name.c_str();
    app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.pEngineName = "Alryn";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = config.extensions;
    std::vector<const char*> layers;

    bool want_validation = config.enable_validation;
    if (want_validation && !validation_layer_supported()) {
        ALRYN_WARN("Validation requested but '{}' is not installed - continuing without it. "
                   "(Install the vulkan-validation-layers package to enable it.)",
                   kValidationLayer);
        want_validation = false;
    }
    if (want_validation) {
        layers.push_back(kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<u32>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<u32>(layers.size());
    info.ppEnabledLayerNames = layers.data();

    // Chain a messenger so we also capture messages during instance creation.
    VkDebugUtilsMessengerCreateInfoEXT debug_info{};
    if (want_validation) {
        populate_debug_info(debug_info);
        info.pNext = &debug_info;
    }

    const VkResult result = vkCreateInstance(&info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        ALRYN_ERROR("vkCreateInstance failed: {}", result_string(result));
        instance_ = VK_NULL_HANDLE;
        return false;
    }

    if (want_validation) {
        auto create_fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_fn != nullptr) {
            create_fn(instance_, &debug_info, nullptr, &messenger_);
        }
    }

    ALRYN_INFO("Vulkan instance created (validation: {})", validation_enabled() ? "on" : "off");
    return true;
}

void Instance::destroy() {
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy_fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_fn != nullptr) {
            destroy_fn(instance_, messenger_, nullptr);
        }
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

Instance::~Instance() {
    destroy();
}

Instance::Instance(Instance&& other) noexcept
    : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
      messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE)) {}

Instance& Instance::operator=(Instance&& other) noexcept {
    if (this != &other) {
        destroy();
        instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
        messenger_ = std::exchange(other.messenger_, VK_NULL_HANDLE);
    }
    return *this;
}

} // namespace alryn::vk
