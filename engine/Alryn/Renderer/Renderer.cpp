#include <Alryn/Renderer/Renderer.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Core/Paths.h>
#include <Alryn/Platform/Window.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Scene/Camera.h>

#include <limits>

namespace alryn {

namespace {
// Must match the layout in shaders/mesh.vert.
struct PushConstants {
    Mat4 mvp;
    Mat4 model;
};
} // namespace

Renderer::Renderer(Window& window, RendererConfig config) : window_(window), config_(config) {}

Renderer::~Renderer() = default; // on_shutdown performs teardown

bool Renderer::on_init(Engine& /*engine*/) {
    vk::InstanceConfig instance_config;
    instance_config.app_name = "Alryn";
    instance_config.enable_validation = config_.enable_validation;
    instance_config.extensions = Window::required_instance_extensions();
    if (!instance_.create(instance_config)) {
        return false;
    }

    surface_ = window_.create_surface(instance_.handle());
    if (surface_ == VK_NULL_HANDLE) {
        ALRYN_ERROR("Failed to create a Vulkan surface for the window");
        return false;
    }

    vk::DeviceConfig device_config;
    device_config.surface = surface_;
    if (!device_.create(instance_.handle(), device_config)) {
        return false;
    }
    if (!device_.dynamic_rendering_enabled()) {
        ALRYN_ERROR("Selected GPU does not support Vulkan 1.3 dynamic rendering");
        return false;
    }

    const UVec2 fb = window_.framebuffer_size();
    if (!swapchain_.create(device_, surface_, fb.x, fb.y, config_.vsync)) {
        return false;
    }
    if (!create_depth()) {
        return false;
    }

    vk::PipelineConfig pipeline_config;
    pipeline_config.vertex_spv = shader_path("mesh.vert.spv").string();
    pipeline_config.fragment_spv = shader_path("mesh.frag.spv").string();
    pipeline_config.color_format = swapchain_.format();
    pipeline_config.depth_format = kDepthFormat;
    pipeline_config.push_constant_size = sizeof(PushConstants);
    pipeline_config.cull_mode = VK_CULL_MODE_NONE; // winding not yet verified per-primitive
    if (!pipeline_.create(device_, pipeline_config)) {
        return false;
    }

    if (!create_sync_and_commands()) {
        return false;
    }

    ALRYN_INFO("Renderer ready ({}x{})", swapchain_.extent().width, swapchain_.extent().height);
    return true;
}

bool Renderer::create_depth() {
    const VkExtent2D extent = swapchain_.extent();
    depth_.destroy();
    return depth_.create(device_, extent.width, extent.height, kDepthFormat,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool Renderer::create_sync_and_commands() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = *device_.queues().graphics;
    ALRYN_VK_CHECK(vkCreateCommandPool(device_.handle(), &pool_info, nullptr, &command_pool_));

    frames_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;

    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first wait returns immediately

    for (FrameSync& frame : frames_) {
        ALRYN_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc, &frame.cmd));
        ALRYN_VK_CHECK(vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &frame.image_available));
        ALRYN_VK_CHECK(vkCreateFence(device_.handle(), &fence_info, nullptr, &frame.in_flight));
    }

    render_finished_.resize(swapchain_.image_count());
    for (VkSemaphore& sem : render_finished_) {
        ALRYN_VK_CHECK(vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &sem));
    }
    return true;
}

void Renderer::set_camera(const Camera& camera) {
    view_ = camera.view();
    projection_ = camera.projection();
}

f32 Renderer::aspect() const {
    const VkExtent2D e = swapchain_.extent();
    return e.height == 0 ? 1.0f : static_cast<f32>(e.width) / static_cast<f32>(e.height);
}

void Renderer::recreate_swapchain() {
    const UVec2 fb = window_.framebuffer_size();
    if (fb.x == 0 || fb.y == 0) {
        return; // minimised; try again next frame
    }
    device_.wait_idle();
    if (!swapchain_.recreate(fb.x, fb.y)) {
        return;
    }
    create_depth();
    if (render_finished_.size() != swapchain_.image_count()) {
        for (VkSemaphore sem : render_finished_) {
            vkDestroySemaphore(device_.handle(), sem, nullptr);
        }
        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        render_finished_.resize(swapchain_.image_count());
        for (VkSemaphore& sem : render_finished_) {
            ALRYN_VK_CHECK(vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &sem));
        }
    }
}

bool Renderer::begin_frame() {
    if (needs_resize_) {
        recreate_swapchain();
        needs_resize_ = false;
    }

    FrameSync& frame = frames_[frame_index_];
    vkWaitForFences(device_.handle(), 1, &frame.in_flight, VK_TRUE,
                    std::numeric_limits<u64>::max());

    const VkResult acquire = swapchain_.acquire_next(frame.image_available, image_index_);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        ALRYN_ERROR("vkAcquireNextImageKHR failed: {}", vk::result_string(acquire));
        return false;
    }

    vkResetFences(device_.handle(), 1, &frame.in_flight);
    vkResetCommandBuffer(frame.cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ALRYN_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));

    vk::image_barrier(frame.cmd, swapchain_.image(image_index_), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vk::image_barrier(frame.cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = swapchain_.view(image_index_);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{config_.clear_color.r, config_.clear_color.g, config_.clear_color.b, 1.0f}};

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depth_.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, swapchain_.extent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(frame.cmd, &rendering);

    const VkExtent2D extent = swapchain_.extent();
    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width),
                              static_cast<f32>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(frame.cmd, 0, 1, &viewport);
    vkCmdSetScissor(frame.cmd, 0, 1, &scissor);
    pipeline_.bind(frame.cmd);

    frame_active_ = true;
    return true;
}

void Renderer::draw(const Mesh& mesh, const Mat4& model) {
    if (!frame_active_) {
        return;
    }
    FrameSync& frame = frames_[frame_index_];
    PushConstants push{};
    push.model = model;
    push.mvp = projection_ * view_ * model;
    vkCmdPushConstants(frame.cmd, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstants), &push);
    mesh.bind(frame.cmd);
    mesh.draw(frame.cmd);
}

void Renderer::end_frame() {
    if (!frame_active_) {
        return;
    }
    FrameSync& frame = frames_[frame_index_];

    vkCmdEndRendering(frame.cmd);
    vk::image_barrier(frame.cmd, swapchain_.image(image_index_), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
    ALRYN_VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkSemaphore signal = render_finished_[image_index_];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.image_available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signal;
    ALRYN_VK_CHECK(vkQueueSubmit(device_.graphics_queue(), 1, &submit, frame.in_flight));

    const VkResult present = swapchain_.present(device_.present_queue(), signal, image_index_);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        needs_resize_ = true;
    } else if (present != VK_SUCCESS) {
        ALRYN_ERROR("vkQueuePresentKHR failed: {}", vk::result_string(present));
    }

    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    frame_active_ = false;
}

void Renderer::on_shutdown() {
    if (device_.valid()) {
        device_.wait_idle();
    }
    for (FrameSync& frame : frames_) {
        if (frame.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.handle(), frame.image_available, nullptr);
        }
        if (frame.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device_.handle(), frame.in_flight, nullptr);
        }
    }
    frames_.clear();
    for (VkSemaphore sem : render_finished_) {
        vkDestroySemaphore(device_.handle(), sem, nullptr);
    }
    render_finished_.clear();
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_.handle(), command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    pipeline_.destroy();
    depth_.destroy();
    swapchain_.destroy();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_.handle(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    device_.destroy();
    instance_.destroy();
}

} // namespace alryn
