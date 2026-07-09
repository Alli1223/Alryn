#include <Alryn/Renderer/Renderer.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Core/Paths.h>
#include <Alryn/Platform/Window.h>
#include <Alryn/Renderer/Mesh.h>
#include <Alryn/Renderer/Vulkan/VulkanCommon.h>
#include <Alryn/Scene/Camera.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace alryn {

namespace {
// Must match the push_constant block in the shaders (256 bytes).
struct PushConstants {
    Mat4 mvp;
    Mat4 model;
    Mat4 light_vp;  // world -> light clip space (shadow lookup)
    Vec4 tint;      // rgb colour multiplier, a = alpha
    Vec4 params;    // x = time, yzw = camera position
    Vec4 sun;       // xyz = normalized direction TO the sun, w = intensity
    Vec4 sun_color; // rgb = sun colour, w = shadow strength
};

// Must match the Lights UBO in mesh.frag (std140).
struct GpuSpot {
    Vec4 pos_range;       // xyz position, w range
    Vec4 dir_cos_inner;   // xyz direction, w cos(inner)
    Vec4 color_cos_outer; // rgb colour, w cos(outer)
    Vec4 atlas;           // xy tile offset, zw tile scale
    Mat4 view_proj;
};
// A light that illuminates but casts no shadow (no atlas tile / view-proj needed).
struct GpuPoint {
    Vec4 pos_range;       // xyz position, w range
    Vec4 dir_cos_inner;   // xyz direction, w cos(inner)
    Vec4 color_cos_outer; // rgb colour, w cos(outer)
};
struct LightUbo {
    i32 count[4]; // x = shadowed spot count, y = unshadowed point count
    GpuSpot spots[9];
    GpuPoint points[48]; // must match kMaxPointLights + mesh.frag
    Vec4 player_peek;    // xyz = player focus (world), w = tunnel radius (0 = off)
    Vec4 cam_pos;        // xyz = camera position (world)
    Vec4 fog_color;      // rgb = atmospheric fog/haze colour, w = density
    Vec4 screen;         // xy = framebuffer resolution (px), z = town "gloom" 0..1
    Vec4 fog_volume;     // x = road fog-bank strength 0..1, y = ground reference height
    Vec4 extra;          // xy = projection terms P22/P32 (view-depth reconstruction),
                         // z = ground wetness 0..1, w = unused
};

// Must match the push_constant block in ui.vert / ui.frag.
struct UIPush {
    Vec4 rect;
    Vec4 color;
    Vec4 params;
    Vec4 seg;
    Vec4 border;
    Vec2 screen;
};

// Must match the push_constant block in ssao.frag.
struct SsaoPush {
    Mat4 proj;     // camera projection (view -> clip)
    Mat4 inv_proj; // clip -> view
    Vec4 params;   // x = world radius (m), y = strength, z = view-space bias, w = unused
    Vec4 screen;   // xy = AO target resolution (px)
};
// Must match the push_constant block in ssao_blur.frag AND bloom_blur.frag.
struct SsaoBlurPush {
    Vec4 screen; // xy = target resolution (px)
};

// Must match the push_constant block in bloom_bright.frag.
struct BrightPush {
    Vec4 params; // x = knee start (luminance), y = knee end
    Vec4 screen; // xy = target resolution (px)
};
// Must match the push_constant block in godrays.frag.
struct RaysPush {
    Vec4 sun;    // xy = sun position in UV space, z = 1 if in front of the camera
    Vec4 params; // x = strength 0..1, y = per-step decay
    Vec4 screen; // xy = target resolution (px)
};
// Must match the push_constant block in composite.frag.
struct CompositePush {
    Vec4 sun_color; // rgb (tints the god rays)
    Vec4 params;    // x = bloom strength, y = rays strength
    Vec4 screen;    // xy = target resolution (px)
};

// Must match the push_constant block in sky.frag.
struct SkyPush {
    Vec4 zenith;      // rgb
    Vec4 horizon;     // rgb
    Vec4 sun_color;   // rgb, w = intensity
    Vec4 sun_screen;  // xy = sun pixel position, z = 1 if in front of the camera
    Vec4 moon_screen; // xy = moon pixel position, z = 1 if in front of the camera
    Vec4 params;      // x = time (s), y = cloud cover 0..1, z = night amount 0..1
    Vec4 screen;      // xy = viewport pixels
};

// A view-frustum extracted from a view-projection matrix (Gribb-Hartmann), used to cull
// draw items per pass: the camera frustum (main pass), the sun frustum (shadow pass) and
// each spot light's frustum (atlas pass - a light cannot light/shadow what's outside it).
struct Frustum {
    Vec4 planes[6]; // each plane.xyz = normal, plane.w = distance; inside => dot(n,p)+w >= 0
};
Frustum make_frustum(const Mat4& m) {
    // Rows of the (column-major glm) matrix: row(i) = (m[0][i], m[1][i], m[2][i], m[3][i]).
    auto row = [&](int i) { return Vec4{m[0][i], m[1][i], m[2][i], m[3][i]}; };
    const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Frustum f;
    f.planes[0] = r3 + r0; // left
    f.planes[1] = r3 - r0; // right
    f.planes[2] = r3 + r1; // bottom
    f.planes[3] = r3 - r1; // top
    f.planes[4] = r2;      // near  (Vulkan/zero-to-one clip: z >= 0)
    f.planes[5] = r3 - r2; // far   (z <= w)
    for (Vec4& p : f.planes) {
        const f32 len = glm::length(Vec3{p});
        if (len > 1e-6f) {
            p /= len;
        }
    }
    return f;
}
// True if the world sphere (xyz centre, w radius) is fully outside the frustum (-> cull it).
// A small margin keeps casters straddling the boundary, so nothing visibly pops.
bool sphere_outside(const Frustum& f, const Vec4& sphere) {
    const Vec3 c{sphere};
    const f32 r = sphere.w + 0.5f;
    for (const Vec4& p : f.planes) {
        if (glm::dot(Vec3{p}, c) + p.w < -r) {
            return true;
        }
    }
    return false;
}
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
    if (!create_depth() || !create_shadow_resources() || !create_pipelines() ||
        !create_sync_and_commands()) {
        return false;
    }

    ALRYN_INFO("Renderer ready ({}x{})", swapchain_.extent().width, swapchain_.extent().height);
    return true;
}

bool Renderer::create_depth() {
    const VkExtent2D extent = swapchain_.extent();
    depth_.destroy();
    if (!depth_.create(device_, extent.width, extent.height, kDepthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) {
        return false;
    }
    return create_ssao_targets();
}

bool Renderer::create_ssao_targets() {
    const VkExtent2D extent = swapchain_.extent();
    const u32 aw = std::max(1u, extent.width / kSsaoDivisor);
    const u32 ah = std::max(1u, extent.height / kSsaoDivisor);
    prepass_depth_.destroy();
    ssao_raw_.destroy();
    ssao_blur_.destroy();
    scene_color_.destroy();
    bloom_a_.destroy();
    bloom_b_.destroy();
    rays_.destroy();
    const VkImageUsageFlags color_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return prepass_depth_.create(device_, extent.width, extent.height, kDepthFormat,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT) &&
           ssao_raw_.create(device_, aw, ah, VK_FORMAT_R8_UNORM, color_usage,
                            VK_IMAGE_ASPECT_COLOR_BIT) &&
           ssao_blur_.create(device_, aw, ah, VK_FORMAT_R8_UNORM, color_usage,
                             VK_IMAGE_ASPECT_COLOR_BIT) &&
           // Post chain: the scene target matches the swapchain format so the main-pass
           // pipelines render into it unchanged; bloom/rays live at half res.
           scene_color_.create(device_, extent.width, extent.height, swapchain_.format(),
                               color_usage, VK_IMAGE_ASPECT_COLOR_BIT) &&
           bloom_a_.create(device_, aw, ah, VK_FORMAT_R8G8B8A8_UNORM, color_usage,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
           bloom_b_.create(device_, aw, ah, VK_FORMAT_R8G8B8A8_UNORM, color_usage,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
           rays_.create(device_, aw, ah, VK_FORMAT_R8_UNORM, color_usage,
                        VK_IMAGE_ASPECT_COLOR_BIT);
}

// (Re)points every offscreen-image descriptor at the current images: the SSAO chain,
// the main pass's AO + scene-depth bindings, and the bloom/rays/composite post chain.
// Called at startup and after every resize (all these images are extent-sized).
void Renderer::write_ssao_descriptors() {
    auto image_info = [&](const vk::Image& img) {
        VkDescriptorImageInfo info{};
        info.sampler = ssao_sampler_;
        info.imageView = img.view();
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return info;
    };
    const VkDescriptorImageInfo depth_info = image_info(prepass_depth_);
    const VkDescriptorImageInfo raw_info = image_info(ssao_raw_);
    const VkDescriptorImageInfo blur_info = image_info(ssao_blur_);
    const VkDescriptorImageInfo scene_info = image_info(scene_color_);
    const VkDescriptorImageInfo bloom_a_info = image_info(bloom_a_);
    const VkDescriptorImageInfo bloom_b_info = image_info(bloom_b_);
    const VkDescriptorImageInfo rays_info = image_info(rays_);

    std::vector<VkWriteDescriptorSet> writes;
    auto add = [&](VkDescriptorSet set, u32 binding, const VkDescriptorImageInfo* info) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = info;
        writes.push_back(w);
    };
    add(ssao_gen_set_, 0, &depth_info);
    add(ssao_blur_set_, 0, &raw_info);
    for (const FrameSync& frame : frames_) {
        add(frame.shadow_set, 3, &blur_info);
        add(frame.shadow_set, 4, &depth_info); // water reads the scene depth under it
    }
    add(bright_set_, 0, &scene_info);
    add(bloom_ab_set_, 0, &bloom_a_info);
    add(bloom_ba_set_, 0, &bloom_b_info);
    add(composite_set_, 0, &scene_info);
    add(composite_set_, 1, &bloom_a_info); // final blurred bloom lands back in a
    add(composite_set_, 2, &rays_info);
    vkUpdateDescriptorSets(device_.handle(), static_cast<u32>(writes.size()), writes.data(), 0,
                           nullptr);
}

bool Renderer::create_pipelines() {
    vk::PipelineConfig base;
    base.vertex_spv = shader_path("mesh.vert.spv").string();
    base.fragment_spv = shader_path("mesh.frag.spv").string();
    base.color_format = swapchain_.format();
    base.depth_format = kDepthFormat;
    base.push_constant_size = sizeof(PushConstants);
    base.cull_mode = VK_CULL_MODE_NONE;
    base.descriptor_set_layout = shadow_set_layout_; // shadow map sampler at set 0

    vk::PipelineConfig opaque = base; // terrain, characters
    opaque.blend = false;
    opaque.depth_write = true;
    if (!pipeline_opaque_.create(device_, opaque)) {
        return false;
    }

    vk::PipelineConfig vegetation = opaque; // grass/ferns: opaque, but wind-swayed
    vegetation.vertex_spv = shader_path("grass.vert.spv").string();
    // Plain lit shader (no peek-through): ground vegetation in front of the player
    // should never dissolve - only the tree canopy does (the foliage pipeline).
    if (!pipeline_vegetation_.create(device_, vegetation)) {
        return false;
    }

    vk::PipelineConfig cutout = opaque; // tree trunks + branches: solid, always rendered
    // Trunks use the plain lit shader too - the peek-through only thins the leafy
    // canopy, so the trunk and its branches stay visible between camera and player.
    if (!pipeline_cutout_.create(device_, cutout)) {
        return false;
    }

    vk::PipelineConfig foliage = base; // alpha-blended tree leaves
    foliage.fragment_spv = shader_path("foliage.frag.spv").string(); // peek-through canopy
    foliage.blend = true;
    foliage.depth_write = false;
    if (!pipeline_foliage_.create(device_, foliage)) {
        return false;
    }

    vk::PipelineConfig water = base; // animated transparent water
    water.vertex_spv = shader_path("water.vert.spv").string();
    water.fragment_spv = shader_path("water.frag.spv").string();
    water.blend = true;
    water.depth_write = false;
    if (!pipeline_water_.create(device_, water)) {
        return false;
    }

    vk::PipelineConfig emissive = base; // self-lit windows / lantern glass
    emissive.fragment_spv = shader_path("emissive.frag.spv").string();
    emissive.blend = false;
    emissive.depth_write = true;
    if (!pipeline_emissive_.create(device_, emissive)) {
        return false;
    }

    vk::PipelineConfig glow = emissive; // additive light shafts (depth-tested, no write)
    glow.blend = false;
    glow.additive = true;
    glow.depth_write = false;
    if (!pipeline_glow_.create(device_, glow)) {
        return false;
    }

    // Depth-only pass that renders the scene from the sun into the shadow map.
    vk::PipelineConfig shadow;
    shadow.vertex_spv = shader_path("shadow.vert.spv").string();
    shadow.depth_format = kDepthFormat;
    shadow.push_constant_size = sizeof(PushConstants);
    shadow.cull_mode = VK_CULL_MODE_NONE;
    shadow.depth_only = true;
    shadow.depth_write = true;
    shadow.depth_bias = true;
    if (!pipeline_shadow_.create(device_, shadow)) {
        return false;
    }

    // Camera depth prepass feeding the SSAO pass: same depth-only setup as the shadow
    // pass (shared shadow.vert), just no bias - it must match the main pass's depth.
    vk::PipelineConfig prepass = shadow;
    prepass.depth_bias = false;
    if (!pipeline_prepass_.create(device_, prepass)) {
        return false;
    }

    // Fullscreen SSAO generation + blur (vertexless triangle via sky.vert).
    vk::PipelineConfig ssao;
    ssao.vertex_spv = shader_path("sky.vert.spv").string();
    ssao.fragment_spv = shader_path("ssao.frag.spv").string();
    ssao.color_format = VK_FORMAT_R8_UNORM;
    ssao.depth_format = VK_FORMAT_UNDEFINED;
    ssao.push_constant_size = sizeof(SsaoPush);
    ssao.cull_mode = VK_CULL_MODE_NONE;
    ssao.vertexless = true;
    ssao.depth_test = false;
    ssao.depth_write = false;
    ssao.descriptor_set_layout = ssao_set_layout_;
    if (!pipeline_ssao_.create(device_, ssao)) {
        return false;
    }
    vk::PipelineConfig ssao_blur = ssao;
    ssao_blur.fragment_spv = shader_path("ssao_blur.frag.spv").string();
    ssao_blur.push_constant_size = sizeof(SsaoBlurPush);
    if (!pipeline_ssao_blur_.create(device_, ssao_blur)) {
        return false;
    }

    // Post chain: bloom bright pass + blur (half-res RGBA), god rays (half-res R8),
    // and the composite onto the swapchain - all vertexless fullscreen triangles.
    vk::PipelineConfig bright = ssao;
    bright.fragment_spv = shader_path("bloom_bright.frag.spv").string();
    bright.color_format = VK_FORMAT_R8G8B8A8_UNORM;
    bright.push_constant_size = sizeof(BrightPush);
    if (!pipeline_bright_.create(device_, bright)) {
        return false;
    }
    vk::PipelineConfig bloom_blur = bright;
    bloom_blur.fragment_spv = shader_path("bloom_blur.frag.spv").string();
    bloom_blur.push_constant_size = sizeof(SsaoBlurPush);
    if (!pipeline_bloom_blur_.create(device_, bloom_blur)) {
        return false;
    }
    vk::PipelineConfig rays = ssao;
    rays.fragment_spv = shader_path("godrays.frag.spv").string();
    rays.push_constant_size = sizeof(RaysPush);
    if (!pipeline_rays_.create(device_, rays)) {
        return false;
    }
    vk::PipelineConfig composite = ssao;
    composite.fragment_spv = shader_path("composite.frag.spv").string();
    composite.color_format = swapchain_.format();
    composite.push_constant_size = sizeof(CompositePush);
    composite.descriptor_set_layout = composite_set_layout_;
    if (!pipeline_composite_.create(device_, composite)) {
        return false;
    }

    // Screen-space UI overlay: vertexless rounded-rect / capsule SDF, alpha
    // blended, no depth (drawn in its own pass after the 3D scene).
    vk::PipelineConfig ui;
    ui.vertex_spv = shader_path("ui.vert.spv").string();
    ui.fragment_spv = shader_path("ui.frag.spv").string();
    ui.color_format = swapchain_.format();
    ui.depth_format = VK_FORMAT_UNDEFINED;
    ui.push_constant_size = sizeof(UIPush);
    ui.cull_mode = VK_CULL_MODE_NONE;
    ui.blend = true;
    ui.vertexless = true;
    if (!pipeline_ui_.create(device_, ui)) {
        return false;
    }

    // Gradient sky + sun disc: a vertexless fullscreen triangle drawn FIRST in the main pass.
    // depth_format matches the main pass (so it's render-compatible) but depth test + write are OFF,
    // so it fills the background and the scene geometry draws over it.
    vk::PipelineConfig sky;
    sky.vertex_spv = shader_path("sky.vert.spv").string();
    sky.fragment_spv = shader_path("sky.frag.spv").string();
    sky.color_format = swapchain_.format();
    sky.depth_format = kDepthFormat;
    sky.push_constant_size = sizeof(SkyPush);
    sky.cull_mode = VK_CULL_MODE_NONE;
    sky.vertexless = true;
    sky.depth_test = false;
    sky.depth_write = false;
    return pipeline_sky_.create(device_, sky);
}

bool Renderer::create_shadow_resources() {
    // set 0: binding 0 = sun shadow map, 1 = spot-light atlas, 2 = light UBO,
    //        3 = blurred SSAO, 4 = camera depth prepass (water shoreline foam).
    //        3 + 4 are written by write_ssao_descriptors.
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3] = bindings[0];
    bindings[3].binding = 3;
    bindings[4] = bindings[0];
    bindings[4].binding = 4;
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 5;
    layout_info.pBindings = bindings;
    ALRYN_VK_CHECK(
        vkCreateDescriptorSetLayout(device_.handle(), &layout_info, nullptr, &shadow_set_layout_));

    // The SSAO/bloom/rays passes each read a single sampled image.
    VkDescriptorSetLayoutCreateInfo ssao_layout_info{};
    ssao_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ssao_layout_info.bindingCount = 1;
    ssao_layout_info.pBindings = bindings; // binding 0: combined image sampler, fragment
    ALRYN_VK_CHECK(
        vkCreateDescriptorSetLayout(device_.handle(), &ssao_layout_info, nullptr, &ssao_set_layout_));

    // The composite pass reads three images: scene colour + blurred bloom + god rays.
    VkDescriptorSetLayoutBinding comp_bindings[3]{};
    for (u32 b = 0; b < 3; ++b) {
        comp_bindings[b] = bindings[0];
        comp_bindings[b].binding = b;
    }
    VkDescriptorSetLayoutCreateInfo comp_layout_info{};
    comp_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    comp_layout_info.bindingCount = 3;
    comp_layout_info.pBindings = comp_bindings;
    ALRYN_VK_CHECK(vkCreateDescriptorSetLayout(device_.handle(), &comp_layout_info, nullptr,
                                               &composite_set_layout_));

    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // per-frame main sets + SSAO inputs (2) + bloom chain inputs (3) + composite (3).
    pool_sizes[0].descriptorCount = 4 * kFramesInFlight + 8;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = kFramesInFlight;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kFramesInFlight + 6;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    ALRYN_VK_CHECK(vkCreateDescriptorPool(device_.handle(), &pool_info, nullptr, &descriptor_pool_));

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // off-map => lit
    ALRYN_VK_CHECK(vkCreateSampler(device_.handle(), &sampler_info, nullptr, &shadow_sampler_));

    VkSamplerCreateInfo ssao_sampler_info = sampler_info;
    ssao_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssao_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ssao_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ALRYN_VK_CHECK(vkCreateSampler(device_.handle(), &ssao_sampler_info, nullptr, &ssao_sampler_));
    return true;
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
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (FrameSync& frame : frames_) {
        ALRYN_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc, &frame.cmd));
        ALRYN_VK_CHECK(vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &frame.image_available));
        ALRYN_VK_CHECK(vkCreateFence(device_.handle(), &fence_info, nullptr, &frame.in_flight));

        // Per-frame shadow targets (so the two frames in flight never alias) + UBO.
        const VkImageUsageFlags shadow_usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!frame.shadow_map.create(device_, kShadowSize, kShadowSize, kDepthFormat, shadow_usage,
                                     VK_IMAGE_ASPECT_DEPTH_BIT) ||
            !frame.light_atlas.create(device_, kAtlasSize, kAtlasSize, kDepthFormat, shadow_usage,
                                      VK_IMAGE_ASPECT_DEPTH_BIT) ||
            !frame.light_ubo.create(device_, sizeof(LightUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }
        VkDescriptorSetAllocateInfo set_alloc{};
        set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_alloc.descriptorPool = descriptor_pool_;
        set_alloc.descriptorSetCount = 1;
        set_alloc.pSetLayouts = &shadow_set_layout_;
        ALRYN_VK_CHECK(vkAllocateDescriptorSets(device_.handle(), &set_alloc, &frame.shadow_set));

        VkDescriptorImageInfo sun_info{};
        sun_info.sampler = shadow_sampler_;
        sun_info.imageView = frame.shadow_map.view();
        sun_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo atlas_info{};
        atlas_info.sampler = shadow_sampler_;
        atlas_info.imageView = frame.light_atlas.view();
        atlas_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorBufferInfo ubo_info{};
        ubo_info.buffer = frame.light_ubo.handle();
        ubo_info.offset = 0;
        ubo_info.range = sizeof(LightUbo);

        VkWriteDescriptorSet writes[3]{};
        for (int b = 0; b < 3; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = frame.shadow_set;
            writes[b].dstBinding = static_cast<u32>(b);
            writes[b].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &sun_info;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &atlas_info;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &ubo_info;
        vkUpdateDescriptorSets(device_.handle(), 3, writes, 0, nullptr);
    }

    render_finished_.resize(swapchain_.image_count());
    for (VkSemaphore& sem : render_finished_) {
        ALRYN_VK_CHECK(vkCreateSemaphore(device_.handle(), &sem_info, nullptr, &sem));
    }

    // Pass-input sets: SSAO (depth -> AO gen, raw -> blur), the bloom chain, and the
    // composite; then point every offscreen descriptor at the current images.
    VkDescriptorSetLayout post_layouts[6] = {ssao_set_layout_, ssao_set_layout_,
                                             ssao_set_layout_, ssao_set_layout_,
                                             ssao_set_layout_, composite_set_layout_};
    VkDescriptorSet post_sets[6] = {};
    VkDescriptorSetAllocateInfo post_alloc{};
    post_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    post_alloc.descriptorPool = descriptor_pool_;
    post_alloc.descriptorSetCount = 6;
    post_alloc.pSetLayouts = post_layouts;
    ALRYN_VK_CHECK(vkAllocateDescriptorSets(device_.handle(), &post_alloc, post_sets));
    ssao_gen_set_ = post_sets[0];
    ssao_blur_set_ = post_sets[1];
    bright_set_ = post_sets[2];
    bloom_ab_set_ = post_sets[3];
    bloom_ba_set_ = post_sets[4];
    composite_set_ = post_sets[5];
    write_ssao_descriptors();
    return true;
}

void Renderer::set_camera(const Camera& camera) {
    view_ = camera.view();
    projection_ = camera.projection();
    camera_position_ = camera.position();
}

void Renderer::set_sun(const Vec3& direction, const Vec3& color, f32 intensity) {
    const f32 len = glm::length(direction);
    sun_direction_ = len > 1e-4f ? direction / len : Vec3{0.0f, 1.0f, 0.0f};
    sun_color_ = color;
    sun_intensity_ = intensity;
    shadow_strength_ = 0.72f * glm::clamp(intensity, 0.0f, 1.0f); // deep daytime shadows, fade out at dusk/night
}

// Orthographic light frustum centred on what the camera looks at, so the shadow
// map's resolution is spent on the visible area around the player.
Mat4 Renderer::compute_light_matrix() const {
    const Vec3 forward = -glm::normalize(Vec3{view_[0][2], view_[1][2], view_[2][2]});
    const Vec3 focus = camera_position_ + forward * 14.0f;
    const Vec3 light_pos = focus + sun_direction_ * 60.0f;
    const Vec3 up = std::abs(sun_direction_.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f}
                                                       : Vec3{0.0f, 1.0f, 0.0f};
    const Mat4 light_view = glm::lookAt(light_pos, focus, up);
    constexpr f32 s = 42.0f;
    Mat4 light_proj = glm::ortho(-s, s, -s, s, 1.0f, 140.0f);
    // Snap the light window to whole shadow texels: as the camera pans, the ortho
    // window otherwise slides continuously under the rasterised depth samples and
    // shadow edges shimmer/crawl. Rounding the projected world origin to texel
    // increments keeps the world->texel mapping identical frame to frame.
    const Vec4 origin = (light_proj * light_view) * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    const f32 half_size = static_cast<f32>(kShadowSize) * 0.5f;
    const Vec2 texel_pos{origin.x * half_size, origin.y * half_size};
    const Vec2 snap{(std::round(texel_pos.x) - texel_pos.x) / half_size,
                    (std::round(texel_pos.y) - texel_pos.y) / half_size};
    light_proj[3][0] += snap.x;
    light_proj[3][1] += snap.y;
    return light_proj * light_view;
}

void Renderer::add_light(const SpotLight& light) {
    if (frame_active_) {
        pending_lights_.push_back(light);
    }
}

// Picks the nearest kMaxLights to the camera, assigns each an atlas tile, and
// fills the per-frame light UBO + the matrices used by the atlas pass.
void Renderer::process_lights() {
    static_assert(kMaxLights <= 9, "LightUbo/shader hold up to 9 shadow spots");
    static_assert(kMaxPointLights <= 48, "LightUbo/shader hold up to 48 point lights");
    active_light_vp_.clear();
    LightUbo ubo{};

    std::vector<u32> order(pending_lights_.size());
    std::iota(order.begin(), order.end(), 0u);
    const Vec3 cam = camera_position_;
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        // Priority (key) lights first, then nearest-to-camera. A priority light is guaranteed
        // a shadow tile before any lantern/house can claim one.
        if (pending_lights_[a].priority != pending_lights_[b].priority) {
            return pending_lights_[a].priority;
        }
        return glm::length(pending_lights_[a].position - cam) <
               glm::length(pending_lights_[b].position - cam);
    });

    // Walk lights nearest-first. The few nearest that WANT a shadow (house interiors) get
    // an atlas tile + a shadow pass; everything else (lanterns, overflow) illuminates from
    // the cheap unshadowed pool. Rendering a shadow map per light is the costly part, so
    // keeping lanterns out of it lets a whole town glow for almost free.
    const f32 tile = 1.0f / static_cast<f32>(kAtlasTiles);
    u32 n = 0; // shadow-casting spots assigned
    u32 m = 0; // unshadowed point lights assigned
    for (const u32 idx : order) {
        const SpotLight& l = pending_lights_[idx];
        const Vec3 dir = glm::length(l.direction) > 1e-4f ? glm::normalize(l.direction)
                                                          : Vec3{0.0f, -1.0f, 0.0f};
        if (l.cast_shadow && n < kMaxLights) {
            const Vec3 up = std::abs(dir.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
            const Mat4 view = glm::lookAt(l.position, l.position + dir, up);
            const f32 outer = glm::clamp(l.cone_outer_cos, -0.999f, 0.999f);
            const f32 fov = glm::clamp(2.0f * std::acos(outer), 0.35f, 2.7f);
            const Mat4 vp = glm::perspective(fov, 1.0f, 0.25f, std::max(l.range, 1.0f)) * view;
            active_light_vp_.push_back(vp);
            const u32 col = n % kAtlasTiles;
            const u32 row = n / kAtlasTiles;
            GpuSpot& g = ubo.spots[n];
            g.pos_range = Vec4{l.position, l.range};
            g.dir_cos_inner = Vec4{dir, l.cone_inner_cos};
            g.color_cos_outer = Vec4{l.color, outer};
            g.atlas = Vec4{static_cast<f32>(col) * tile, static_cast<f32>(row) * tile, tile, tile};
            g.view_proj = vp;
            ++n;
        } else if (!l.indoor && m < kMaxPointLights) {
            // Unshadowed: lanterns + any shadow-overflow that isn't indoor. (An indoor
            // light with no tile is dropped - it would leak through the walls.)
            GpuPoint& p = ubo.points[m];
            p.pos_range = Vec4{l.position, l.range};
            p.dir_cos_inner = Vec4{dir, l.cone_inner_cos};
            p.color_cos_outer = Vec4{l.color, glm::clamp(l.cone_outer_cos, -0.999f, 0.999f)};
            ++m;
        }
    }

    ubo.count[0] = static_cast<i32>(n);
    ubo.count[1] = static_cast<i32>(m);
    // The foliage/vegetation/trunk shaders dissolve what sits in a wide tunnel running from
    // the camera to the player, so almost the whole view ahead of the character is cleared of
    // occluding canopy. Aim at the torso (feet + ~1.1 m).
    ubo.player_peek = Vec4{player_position_ + Vec3{0.0f, 1.1f, 0.0f}, 6.0f};
    ubo.cam_pos = Vec4{camera_position_, 0.0f};
    ubo.fog_color = Vec4{fog_color_, fog_density_};
    const VkExtent2D ext = swapchain_.extent();
    ubo.screen = Vec4{static_cast<f32>(ext.width), static_cast<f32>(ext.height), gloom_, 0.0f};
    ubo.fog_volume = Vec4{fog_patch_, player_position_.y, cloud_cover_, wind_strength_};
    // Projection terms so fragment shaders can turn a raw depth into metres in front of
    // the camera (water uses the scene depth under it for shoreline foam + soft edges).
    ubo.extra = Vec4{projection_[2][2], projection_[3][2], wetness_, 0.0f};
    frames_[frame_index_].light_ubo.upload(&ubo, sizeof(ubo));
}

f32 Renderer::aspect() const {
    const VkExtent2D e = swapchain_.extent();
    return e.height == 0 ? 1.0f : static_cast<f32>(e.width) / static_cast<f32>(e.height);
}

void Renderer::recreate_swapchain() {
    const UVec2 fb = window_.framebuffer_size();
    if (fb.x == 0 || fb.y == 0) {
        return;
    }
    device_.wait_idle();
    if (!swapchain_.recreate(fb.x, fb.y)) {
        return;
    }
    create_depth();
    write_ssao_descriptors(); // the AO images were recreated at the new size
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
    draw_items_.clear();
    ui_items_.clear();
    pending_lights_.clear();
    frame_active_ = true;
    return true;
}

void Renderer::push_constants(const Mat4& model, const Vec4& tint, bool vegetation) {
    FrameSync& frame = frames_[frame_index_];
    PushConstants push{};
    push.mvp = projection_ * view_ * model;
    push.model = model;
    push.light_vp = light_view_proj_;
    push.tint = tint;
    // mesh.frag ignores params; the vegetation shader (grass.vert) reads the player
    // position + wind there, while everything else carries the camera position (for
    // the water shader).
    push.params = vegetation
                      ? Vec4{time_, player_position_.x, player_position_.z, wind_strength_}
                      : Vec4{time_, camera_position_.x, camera_position_.y, camera_position_.z};
    push.sun = Vec4{sun_direction_, sun_intensity_};
    push.sun_color = Vec4{sun_color_, shadow_strength_};
    // Layouts of all main pipelines are compatible, so push via the opaque one.
    vkCmdPushConstants(frame.cmd, pipeline_opaque_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(PushConstants), &push);
}

// Pass 1: render scene depth from the sun's point of view into the shadow map.
void Renderer::record_shadow_pass(VkCommandBuffer cmd) {
    FrameSync& frame = frames_[frame_index_];
    vk::image_barrier(cmd, frame.shadow_map.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = frame.shadow_map.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // sampled in the main pass
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(kShadowSize),
                              static_cast<f32>(kShadowSize), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // At night the sun's shadow is faded to nothing (shadow_strength -> 0), so skip rendering
    // the whole scene into it - the cleared map is sampled but multiplied out. By day, only draw
    // casters inside the sun's ortho frustum.
    if (shadow_strength_ > 0.01f) {
        pipeline_shadow_.bind(cmd);
        const Frustum sun_frustum = make_frustum(light_view_proj_);
        for (const DrawItem& item : draw_items_) {
            if (item.layer == Layer::Water || item.layer == Layer::Glow) {
                continue; // the water surface doesn't cast shadows
            }
            if (sphere_outside(sun_frustum, item.sphere)) {
                continue;
            }
            PushConstants push{};
            push.mvp = light_view_proj_ * item.model;
            vkCmdPushConstants(cmd, pipeline_shadow_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants), &push);
            item.mesh->bind(cmd);
            item.mesh->draw(cmd);
        }
    }
    vkCmdEndRendering(cmd);

    vk::image_barrier(cmd, frame.shadow_map.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// Pass 1b: render scene depth from each active spot light into its atlas tile.
void Renderer::record_light_atlas_pass(VkCommandBuffer cmd) {
    FrameSync& frame = frames_[frame_index_];
    vk::image_barrier(cmd, frame.light_atlas.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = frame.light_atlas.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {kAtlasSize, kAtlasSize}};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    pipeline_shadow_.bind(cmd);
    const u32 tile = kAtlasSize / kAtlasTiles;
    for (u32 i = 0; i < active_light_vp_.size(); ++i) {
        const u32 col = i % kAtlasTiles;
        const u32 row = i / kAtlasTiles;
        const VkViewport vp{static_cast<f32>(col * tile), static_cast<f32>(row * tile),
                            static_cast<f32>(tile), static_cast<f32>(tile), 0.0f, 1.0f};
        const VkRect2D sc{{static_cast<i32>(col * tile), static_cast<i32>(row * tile)},
                          {tile, tile}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        // A spot light only lights/shadows what's inside its frustum (bounded by its range +
        // cone). Cull everything else - this is what stops a single lantern from re-rendering
        // the whole town into its tile, the dominant night-time cost.
        const Frustum light_frustum = make_frustum(active_light_vp_[i]);
        for (const DrawItem& item : draw_items_) {
            if (item.layer == Layer::Water || item.layer == Layer::Glow) {
                continue;
            }
            if (sphere_outside(light_frustum, item.sphere)) {
                continue;
            }
            PushConstants push{};
            push.mvp = active_light_vp_[i] * item.model;
            vkCmdPushConstants(cmd, pipeline_shadow_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(PushConstants), &push);
            item.mesh->bind(cmd);
            item.mesh->draw(cmd);
        }
    }
    vkCmdEndRendering(cmd);

    vk::image_barrier(cmd, frame.light_atlas.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

// Pass 1c: camera depth prepass -> raw SSAO (half res) -> blurred SSAO. The main pass
// samples the result at set 0 binding 3 to darken creases and contact points.
void Renderer::record_ssao_pass(VkCommandBuffer cmd) {
    // --- Depth prepass: the depth-writing layers from the camera's view ------------
    vk::image_barrier(cmd, prepass_depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                      0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    const VkExtent2D extent = swapchain_.extent();
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = prepass_depth_.view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // sampled by the AO pass
    depth.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width),
                              static_cast<f32>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    pipeline_prepass_.bind(cmd);
    const Mat4 view_proj = projection_ * view_;
    const Frustum cam_frustum = make_frustum(view_proj);
    for (const DrawItem& item : draw_items_) {
        // Only the layers that write depth in the main pass (water/foliage/glow don't).
        if (item.layer == Layer::Water || item.layer == Layer::Foliage ||
            item.layer == Layer::Glow) {
            continue;
        }
        if (sphere_outside(cam_frustum, item.sphere)) {
            continue;
        }
        PushConstants push{};
        push.mvp = view_proj * item.model;
        vkCmdPushConstants(cmd, pipeline_prepass_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(PushConstants), &push);
        item.mesh->bind(cmd);
        item.mesh->draw(cmd);
    }
    vkCmdEndRendering(cmd);

    vk::image_barrier(cmd, prepass_depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // --- Raw AO, then blur, both half-res fullscreen triangles ---------------------
    const u32 aw = std::max(1u, extent.width / kSsaoDivisor);
    const u32 ah = std::max(1u, extent.height / kSsaoDivisor);
    const VkViewport ao_viewport{0.0f, 0.0f, static_cast<f32>(aw), static_cast<f32>(ah),
                                 0.0f, 1.0f};
    const VkRect2D ao_scissor{{0, 0}, {aw, ah}};

    auto fullscreen_pass = [&](vk::Image& target, vk::Pipeline& pipeline, VkDescriptorSet set,
                               const void* push, u32 push_size) {
        vk::image_barrier(cmd, target.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is written
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea = {{0, 0}, {aw, ah}};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &info);
        vkCmdSetViewport(cmd, 0, 1, &ao_viewport);
        vkCmdSetScissor(cmd, 0, 1, &ao_scissor);
        pipeline.bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size,
                           push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vk::image_barrier(cmd, target.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    SsaoPush sp{};
    sp.proj = projection_;
    sp.inv_proj = glm::inverse(projection_);
    sp.params = Vec4{0.55f, 0.85f, 0.02f, 0.0f}; // radius (m), strength, view-space bias
    sp.screen = Vec4{static_cast<f32>(aw), static_cast<f32>(ah), 0.0f, 0.0f};
    fullscreen_pass(ssao_raw_, pipeline_ssao_, ssao_gen_set_, &sp, sizeof(sp));

    SsaoBlurPush bp{};
    bp.screen = Vec4{static_cast<f32>(aw), static_cast<f32>(ah), 0.0f, 0.0f};
    fullscreen_pass(ssao_blur_, pipeline_ssao_blur_, ssao_blur_set_, &bp, sizeof(bp));

    current_pipeline_ = VK_NULL_HANDLE; // the main pass rebinds from scratch
}

// Pass 2: forward-render to the offscreen scene target, sampling the shadow map for
// occlusion. The post pass then composites scene + bloom + god rays onto the swapchain.
void Renderer::record_main_pass(VkCommandBuffer cmd) {
    FrameSync& frame = frames_[frame_index_];
    vk::image_barrier(cmd, scene_color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vk::image_barrier(cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = scene_color_.view();
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{sky_color_.r, sky_color_.g, sky_color_.b, 1.0f}};

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
    vkCmdBeginRendering(cmd, &rendering);

    const VkExtent2D extent = swapchain_.extent();
    const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width),
                              static_cast<f32>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Gradient sky + sun disc behind everything (fullscreen, no depth test/write). Drawn first so the
    // scene geometry overwrites it; the colours track the day/night cycle.
    if (pipeline_sky_.valid()) {
        pipeline_sky_.bind(cmd);
        SkyPush sp{};
        sp.zenith = Vec4{sky_color_, 1.0f};
        sp.horizon = Vec4{fog_color_, 1.0f};
        sp.sun_color = Vec4{sun_color_, sun_intensity_};
        sp.screen = Vec4{static_cast<f32>(extent.width), static_cast<f32>(extent.height), 0.0f, 0.0f};
        sp.params = Vec4{time_, cloud_cover_, 1.0f - glm::clamp(sun_intensity_, 0.0f, 1.0f), 0.0f};
        // Project the sun (and the moon, opposite it) onto the screen for their discs.
        const Vec3 cam_pos = Vec3{glm::inverse(view_)[3]};
        auto project = [&](const Vec3& dir) {
            const Vec4 clip = projection_ * view_ * Vec4{cam_pos + dir * 1000.0f, 1.0f};
            if (clip.w <= 0.0f) {
                return Vec4{-1.0f, -1.0f, 0.0f, 0.0f};
            }
            const f32 nx = clip.x / clip.w, ny = clip.y / clip.w;
            return Vec4{(nx * 0.5f + 0.5f) * static_cast<f32>(extent.width),
                        (ny * 0.5f + 0.5f) * static_cast<f32>(extent.height), 1.0f, 0.0f};
        };
        sp.sun_screen = project(sun_direction_);
        sp.moon_screen = project(-sun_direction_);
        vkCmdPushConstants(cmd, pipeline_sky_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPush),
                           &sp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_opaque_.layout(), 0, 1,
                            &frame.shadow_set, 0, nullptr);

    current_pipeline_ = VK_NULL_HANDLE;
    const Frustum cam_frustum = make_frustum(projection_ * view_);
    for (const DrawItem& item : draw_items_) {
        if (sphere_outside(cam_frustum, item.sphere)) {
            continue; // off-screen: skip (props behind / beside the camera)
        }
        const vk::Pipeline& pipe = item.layer == Layer::Water        ? pipeline_water_
                                   : item.layer == Layer::Foliage    ? pipeline_foliage_
                                   : item.layer == Layer::Glow       ? pipeline_glow_
                                   : item.layer == Layer::Emissive   ? pipeline_emissive_
                                   : item.layer == Layer::Vegetation ? pipeline_vegetation_
                                   : item.layer == Layer::Cutout     ? pipeline_cutout_
                                                                     : pipeline_opaque_;
        if (pipe.handle() != current_pipeline_) {
            pipe.bind(cmd);
            current_pipeline_ = pipe.handle();
        }
        push_constants(item.model, item.tint, item.layer == Layer::Vegetation);
        item.mesh->bind(cmd);
        item.mesh->draw(cmd);
    }
    vkCmdEndRendering(cmd);
    // Hand the scene texture to the post pass (bloom bright-pass + composite read it).
    vk::image_barrier(cmd, scene_color_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT);
}

// Pass 2b: bloom (bright-pass + two box-blur ping-pongs), god rays (a radial march of
// the depth prepass toward the low sun), and the composite onto the swapchain. The
// swapchain image is left in COLOR_ATTACHMENT for the UI pass, which presents it.
void Renderer::record_post_pass(VkCommandBuffer cmd) {
    const VkExtent2D extent = swapchain_.extent();
    const u32 aw = std::max(1u, extent.width / kSsaoDivisor);
    const u32 ah = std::max(1u, extent.height / kSsaoDivisor);
    const VkViewport half_viewport{0.0f, 0.0f, static_cast<f32>(aw), static_cast<f32>(ah),
                                   0.0f, 1.0f};
    const VkRect2D half_scissor{{0, 0}, {aw, ah}};
    const Vec4 half_screen{static_cast<f32>(aw), static_cast<f32>(ah), 0.0f, 0.0f};

    auto fullscreen_pass = [&](vk::Image& target, vk::Pipeline& pipeline, VkDescriptorSet set,
                               const void* push, u32 push_size) {
        vk::image_barrier(cmd, target.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is written
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea = {{0, 0}, {aw, ah}};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &info);
        vkCmdSetViewport(cmd, 0, 1, &half_viewport);
        vkCmdSetScissor(cmd, 0, 1, &half_scissor);
        pipeline.bind(cmd);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size,
                           push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vk::image_barrier(cmd, target.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    // Bloom: bright pixels -> a, blur a -> b, blur b -> a (final bloom in bloom_a_).
    BrightPush brp{};
    brp.params = Vec4{0.55f, 0.9f, 0.0f, 0.0f}; // luminance knee
    brp.screen = half_screen;
    fullscreen_pass(bloom_a_, pipeline_bright_, bright_set_, &brp, sizeof(brp));
    SsaoBlurPush blp{};
    blp.screen = half_screen;
    fullscreen_pass(bloom_b_, pipeline_bloom_blur_, bloom_ab_set_, &blp, sizeof(blp));
    fullscreen_pass(bloom_a_, pipeline_bloom_blur_, bloom_ba_set_, &blp, sizeof(blp));

    // God rays: only when the sun sits LOW in the sky (dawn/dusk) and is on screen.
    const Vec3 cam_pos = Vec3{glm::inverse(view_)[3]};
    const Vec4 clip = projection_ * view_ * Vec4{cam_pos + sun_direction_ * 1000.0f, 1.0f};
    RaysPush rp{};
    rp.sun = Vec4{-1.0f, -1.0f, 0.0f, 0.0f};
    if (clip.w > 0.0f) {
        rp.sun = Vec4{(clip.x / clip.w) * 0.5f + 0.5f, (clip.y / clip.w) * 0.5f + 0.5f, 1.0f, 0.0f};
    }
    const f32 low_sun = glm::smoothstep(0.55f, 0.2f, sun_direction_.y); // strongest at the horizon
    rp.params = Vec4{sun_intensity_ * low_sun * 0.9f, 0.94f, 0.0f, 0.0f}; // strength, decay
    rp.screen = half_screen;
    fullscreen_pass(rays_, pipeline_rays_, ssao_gen_set_, &rp, sizeof(rp)); // reads prepass depth

    // Composite scene + bloom + rays onto the swapchain (full res).
    vk::image_barrier(cmd, swapchain_.image(image_index_), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = swapchain_.view(image_index_);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is written
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &info);
    const VkViewport full_viewport{0.0f, 0.0f, static_cast<f32>(extent.width),
                                   static_cast<f32>(extent.height), 0.0f, 1.0f};
    const VkRect2D full_scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &full_viewport);
    vkCmdSetScissor(cmd, 0, 1, &full_scissor);
    pipeline_composite_.bind(cmd);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_composite_.layout(), 0,
                            1, &composite_set_, 0, nullptr);
    CompositePush cp{};
    cp.sun_color = Vec4{sun_color_, 0.0f};
    cp.params = Vec4{0.75f, 1.0f, 0.0f, 0.0f}; // bloom strength, rays strength
    cp.screen = Vec4{static_cast<f32>(extent.width), static_cast<f32>(extent.height), 0.0f, 0.0f};
    vkCmdPushConstants(cmd, pipeline_composite_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cp),
                       &cp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    // The swapchain image stays in COLOR_ATTACHMENT_OPTIMAL for the UI pass,
    // which transitions it to PRESENT_SRC.
    current_pipeline_ = VK_NULL_HANDLE;
}

// Pass 3: screen-space UI overlay. A second rendering instance that loads (keeps)
// the 3D image and draws alpha-blended rounded-rect / capsule SDF primitives, then
// transitions the image to present.
void Renderer::record_ui_pass(VkCommandBuffer cmd) {
    if (!ui_items_.empty()) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = swapchain_.view(image_index_);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // keep the rendered scene
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, swapchain_.extent()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);

        const VkExtent2D extent = swapchain_.extent();
        const VkViewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width),
                                  static_cast<f32>(extent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        pipeline_ui_.bind(cmd);
        for (const UIDrawCmd& item : ui_items_) {
            UIPush push{};
            push.rect = item.rect;
            push.color = item.color;
            push.params = item.params;
            push.seg = item.seg;
            push.border = item.border;
            push.screen = Vec2{static_cast<f32>(extent.width), static_cast<f32>(extent.height)};
            vkCmdPushConstants(cmd, pipeline_ui_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(UIPush), &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
    }

    vk::image_barrier(cmd, swapchain_.image(image_index_), VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
}

Vec4 Renderer::world_sphere(const Mesh& mesh, const Mat4& model) {
    const Vec3 c = Vec3{model * Vec4{mesh.bounds_center(), 1.0f}};
    const f32 scale = std::max({glm::length(Vec3{model[0]}), glm::length(Vec3{model[1]}),
                                glm::length(Vec3{model[2]})});
    return Vec4{c, mesh.bounds_radius() * scale};
}

void Renderer::submit(const Mesh& mesh, const Mat4& model, const Vec4& tint, Layer layer) {
    if (frame_active_) {
        draw_items_.push_back({&mesh, model, tint, layer, world_sphere(mesh, model)});
    }
}

void Renderer::draw(const Mesh& mesh, const Mat4& model, const Vec4& tint) {
    submit(mesh, model, tint, Layer::Opaque);
}

void Renderer::draw_cutout(const Mesh& mesh, const Mat4& model, const Vec4& tint) {
    submit(mesh, model, tint, Layer::Cutout);
}

void Renderer::draw_vegetation(const Mesh& mesh, const Mat4& model) {
    submit(mesh, model, Vec4{1.0f}, Layer::Vegetation);
}

void Renderer::draw_glow(const Mesh& mesh, const Mat4& model, const Vec4& tint) {
    submit(mesh, model, tint, Layer::Glow);
}

void Renderer::draw_transparent(const Mesh& mesh, const Mat4& model, const Vec4& tint) {
    submit(mesh, model, tint, Layer::Foliage);
}

void Renderer::draw_water(const Mesh& mesh, const Mat4& model) {
    submit(mesh, model, Vec4{1.0f}, Layer::Water);
}

void Renderer::draw_emissive(const Mesh& mesh, const Mat4& model, const Vec4& tint) {
    submit(mesh, model, tint, Layer::Emissive);
}

void Renderer::draw_ui_rect(const Vec4& rect_xywh, const Vec4& color, f32 radius, f32 border,
                            const Vec4& border_color) {
    if (!frame_active_) {
        return;
    }
    UIDrawCmd cmd;
    cmd.rect = rect_xywh;
    cmd.color = color;
    cmd.params = Vec4{radius, 1.0f, 0.0f /*rect mode*/, border};
    cmd.border = border_color;
    ui_items_.push_back(cmd);
}

void Renderer::draw_ui_segment(const Vec2& p0, const Vec2& p1, f32 thickness, const Vec4& color) {
    if (!frame_active_) {
        return;
    }
    const f32 half = thickness * 0.5f;
    const f32 pad = half + 2.0f; // room for the anti-aliased edge
    const Vec2 lo{std::min(p0.x, p1.x) - pad, std::min(p0.y, p1.y) - pad};
    const Vec2 hi{std::max(p0.x, p1.x) + pad, std::max(p0.y, p1.y) + pad};
    UIDrawCmd cmd;
    cmd.rect = Vec4{lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
    cmd.color = color;
    cmd.params = Vec4{0.0f, 0.75f /*tighter AA = crisper text*/, 1.0f /*segment mode*/, half};
    cmd.seg = Vec4{p0.x, p0.y, p1.x, p1.y};
    ui_items_.push_back(cmd);
}

void Renderer::set_vsync(bool enabled) {
    if (config_.vsync == enabled) {
        return;
    }
    config_.vsync = enabled;
    swapchain_.set_vsync(enabled);
    needs_resize_ = true; // recreate_swapchain rebuilds with the new present mode
}

void Renderer::end_frame() {
    if (!frame_active_) {
        return;
    }
    FrameSync& frame = frames_[frame_index_];

    light_view_proj_ = compute_light_matrix();
    process_lights();
    std::stable_sort(draw_items_.begin(), draw_items_.end(),
                     [](const DrawItem& a, const DrawItem& b) { return a.layer < b.layer; });

    vkResetCommandBuffer(frame.cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    ALRYN_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));

    record_shadow_pass(frame.cmd);
    record_light_atlas_pass(frame.cmd);
    record_ssao_pass(frame.cmd);
    record_main_pass(frame.cmd);
    record_post_pass(frame.cmd);
    record_ui_pass(frame.cmd);

    ALRYN_VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkSemaphore signal = render_finished_[image_index_];
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &frame.image_available;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &frame.cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &signal;
    ALRYN_VK_CHECK(vkQueueSubmit(device_.graphics_queue(), 1, &submit_info, frame.in_flight));

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
    pipeline_opaque_.destroy();
    pipeline_cutout_.destroy();
    pipeline_foliage_.destroy();
    pipeline_water_.destroy();
    pipeline_emissive_.destroy();
    pipeline_glow_.destroy();
    pipeline_vegetation_.destroy();
    pipeline_shadow_.destroy();
    pipeline_prepass_.destroy();
    pipeline_ssao_.destroy();
    pipeline_ssao_blur_.destroy();
    pipeline_bright_.destroy();
    pipeline_bloom_blur_.destroy();
    pipeline_rays_.destroy();
    pipeline_composite_.destroy();
    pipeline_ui_.destroy();
    pipeline_sky_.destroy();
    if (shadow_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.handle(), shadow_sampler_, nullptr);
        shadow_sampler_ = VK_NULL_HANDLE;
    }
    if (ssao_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_.handle(), ssao_sampler_, nullptr);
        ssao_sampler_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_.handle(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (shadow_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), shadow_set_layout_, nullptr);
        shadow_set_layout_ = VK_NULL_HANDLE;
    }
    if (ssao_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), ssao_set_layout_, nullptr);
        ssao_set_layout_ = VK_NULL_HANDLE;
    }
    if (composite_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_.handle(), composite_set_layout_, nullptr);
        composite_set_layout_ = VK_NULL_HANDLE;
    }
    prepass_depth_.destroy();
    ssao_raw_.destroy();
    ssao_blur_.destroy();
    scene_color_.destroy();
    bloom_a_.destroy();
    bloom_b_.destroy();
    rays_.destroy();
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
