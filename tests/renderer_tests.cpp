#include <doctest/doctest.h>

#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>

#include <string>

using namespace alryn;

// Brings up a real Vulkan instance + logical device with no surface (headless).
// Skips (rather than fails) on machines/CI without a usable Vulkan device.
TEST_CASE("Vulkan: headless instance + device bring-up") {
    vk::Instance instance;
    vk::InstanceConfig instance_config;
    instance_config.app_name = "alryn_tests";
    instance_config.enable_validation = false; // don't require the layer package in CI

    if (!instance.create(instance_config)) {
        MESSAGE("No Vulkan instance available - skipping device test");
        return;
    }
    CHECK(instance.valid());
    CHECK(instance.handle() != VK_NULL_HANDLE);

    vk::Device device;
    if (!device.create(instance.handle())) { // headless: surface == VK_NULL_HANDLE
        MESSAGE("No suitable Vulkan device - skipping");
        return;
    }

    CHECK(device.valid());
    CHECK(device.handle() != VK_NULL_HANDLE);
    CHECK(device.queues().has_graphics());
    CHECK(device.graphics_queue() != VK_NULL_HANDLE);
    const std::string gpu_name = std::string{"Selected GPU: "} + device.device_name();
    MESSAGE(gpu_name);

    device.wait_idle();
    // RAII tears down the device then the instance as the scope exits.
}
