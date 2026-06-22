#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Engine/Subsystem.h>
#include <Alryn/Renderer/Vulkan/VulkanBuffer.h>
#include <Alryn/Renderer/Vulkan/VulkanDevice.h>
#include <Alryn/Renderer/Vulkan/VulkanImage.h>
#include <Alryn/Renderer/Vulkan/VulkanInstance.h>
#include <Alryn/Renderer/Vulkan/VulkanPipeline.h>
#include <Alryn/Renderer/Vulkan/VulkanSwapchain.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace alryn {

class Window;
class Mesh;
class Camera;

struct RendererConfig {
    bool enable_validation = true;
    bool vsync = true;
    Vec3 clear_color{0.10f, 0.12f, 0.18f};
};

// The forward renderer, exposed as an engine Subsystem. Owns the whole Vulkan
// stack (instance, surface, device, swapchain, depth buffer, pipeline, per-frame
// command buffers + sync) and renders to the window each frame.
//
// Per-frame usage (driven by Application):
//     renderer.set_camera(cam);
//     if (renderer.begin_frame()) {
//         renderer.draw(mesh, model);  // any number of times
//         renderer.end_frame();
//     }
class Renderer : public Subsystem {
public:
    Renderer(Window& window, RendererConfig config = {});
    ~Renderer() override;

    const char* name() const override { return "Renderer"; }
    bool on_init(Engine& engine) override;
    void on_shutdown() override;

    void set_camera(const Camera& camera);
    void set_time(f32 seconds) { time_ = seconds; }
    // The player's world position + wind strength, used to bend nearby vegetation.
    void set_player_position(const Vec3& p) { player_position_ = p; }
    void set_wind(f32 strength) { wind_strength_ = strength; }

    // Directional sun for the day/night cycle. `direction` points TO the sun
    // (normalized); `intensity` fades to 0 below the horizon (night).
    void set_sun(const Vec3& direction, const Vec3& color, f32 intensity);
    // Sky/background colour the frame is cleared to (drives the day/night sky).
    void set_sky_color(const Vec3& color) { sky_color_ = color; }

    // Atmospheric fog/haze sampled by the scene shaders for cinematic depth. `color` is the
    // colour distant geometry fades toward (usually a desaturated sky tone), `density` controls
    // how quickly (exp-squared with distance), and `gloom` (0..1) deepens the grade/vignette in
    // towns. Set it each frame from the day/night cycle (see ClientApp::update_day_night).
    // `patch` (0..1) layers an occasional dense, drifting, ground-hugging fog BANK on top - the
    // road mist; the client ramps it up on foggy road stretches (see ClientApp::update_day_night).
    void set_fog(const Vec3& color, f32 density, f32 gloom, f32 patch = 0.0f) {
        fog_color_ = color;
        fog_density_ = density;
        gloom_ = gloom;
        fog_patch_ = patch;
    }

    // A shadow-casting spotlight (e.g. a lantern). Submit each frame before
    // end_frame; the nearest few to the camera get rendered shadow maps.
    struct SpotLight {
        Vec3 position{0.0f};
        Vec3 direction{0.0f, -1.0f, 0.0f}; // normalized
        Vec3 color{1.0f};                  // rgb already scaled by intensity
        f32 range = 16.0f;
        f32 cone_inner_cos = 0.82f;
        f32 cone_outer_cos = 0.6f;
        // Indoor lights (e.g. a hearth) MUST be occluded by walls, so they only render
        // when they make the shadow budget; they're never added to the cheap unshadowed
        // pool (which would leak light straight through the walls).
        bool indoor = false;
        // Outdoor lights (lanterns) don't need shadows - rendering a shadow map per light
        // is the expensive part. Setting this false keeps them in the cheap unshadowed
        // pool, so a town full of lanterns costs almost nothing.
        bool cast_shadow = true;
        // A key light (e.g. the escorted wagon's lantern) claims a shadow atlas tile FIRST,
        // ahead of all the nearest-distance lights, so it always casts real shadows of the
        // player + nearby objects no matter how many other lights are around.
        bool priority = false;
    };
    void add_light(const SpotLight& light);

    bool begin_frame();

    // Opaque geometry (terrain, characters). tint multiplies the vertex colour.
    void draw(const Mesh& mesh, const Mat4& model, const Vec4& tint = Vec4{1.0f});
    // Opaque trunk/branch geometry (tree trunks). Solid and always rendered - unlike the
    // canopy foliage, trunks never dissolve, so the tree's wooden frame stays visible.
    void draw_cutout(const Mesh& mesh, const Mat4& model, const Vec4& tint = Vec4{1.0f});
    // Vegetation (grass/ferns/...): opaque + depth-written, but drawn with the wind
    // shader so it sways in the breeze and bends away from the player.
    void draw_vegetation(const Mesh& mesh, const Mat4& model);
    // Alpha-blended geometry (foliage). tint.a is the opacity. Draw after opaque.
    void draw_transparent(const Mesh& mesh, const Mat4& model, const Vec4& tint);
    // Animated water surface (its own shader). Draw after opaque.
    void draw_water(const Mesh& mesh, const Mat4& model);
    // Self-lit geometry (lantern glass, glowing windows): full-bright, casts shadows.
    void draw_emissive(const Mesh& mesh, const Mat4& model, const Vec4& tint = Vec4{1.0f});
    // Additive self-lit geometry (light shafts from windows). tint.a = intensity.
    void draw_glow(const Mesh& mesh, const Mat4& model, const Vec4& tint);

    // ---- 2D UI overlay (screen-space, drawn last over the 3D scene) ----------
    // Pixel coordinates: origin top-left, +x right, +y down, matching the window.
    // A filled (optionally bordered) rounded rectangle. `radius`/`border` in px.
    void draw_ui_rect(const Vec4& rect_xywh, const Vec4& color, f32 radius = 0.0f,
                      f32 border = 0.0f, const Vec4& border_color = Vec4{0.0f});
    // A rounded-cap line segment of the given thickness (vector-font strokes, etc.).
    void draw_ui_segment(const Vec2& p0, const Vec2& p1, f32 thickness, const Vec4& color);

    void end_frame();

    void request_resize() { needs_resize_ = true; }
    // Switches vsync on/off (recreates the swapchain with the new present mode).
    void set_vsync(bool enabled);
    bool vsync() const { return config_.vsync; }

    vk::Device& device() { return device_; }
    f32 aspect() const;
    VkExtent2D extent() const { return swapchain_.extent(); }

private:
    // Draw layers, ordered so a sort yields opaque -> vegetation -> emissive ->
    // water -> foliage.
    enum class Layer : u8 {
        Opaque = 0,
        Cutout = 1,     // opaque tree trunks/branches (solid, never dissolves)
        Vegetation = 2,
        Emissive = 3,
        Water = 4,
        Foliage = 5,
        Glow = 6 // additive light shafts, drawn last
    };
    struct DrawItem {
        const Mesh* mesh;
        Mat4 model;
        Vec4 tint;
        Layer layer;
        Vec4 sphere; // world-space bounding sphere (xyz centre, w radius) for culling
    };

    // One screen-space UI primitive (matches the ui.* push-constant block).
    struct UIDrawCmd {
        Vec4 rect{0.0f};
        Vec4 color{1.0f};
        Vec4 params{0.0f};
        Vec4 seg{0.0f};
        Vec4 border{0.0f};
    };

    struct FrameSync {
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkFence in_flight = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vk::Image shadow_map;                    // sun shadow map (per frame, no aliasing)
        vk::Image light_atlas;                   // spot-light shadow atlas (tiled)
        vk::Buffer light_ubo;                    // per-frame spot-light data
        VkDescriptorSet shadow_set = VK_NULL_HANDLE;
    };

    static constexpr u32 kFramesInFlight = 2;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr u32 kShadowSize = 2048;
    static constexpr u32 kMaxLights = 4;       // shadow-casting spot lights (atlas tiles)
    static constexpr u32 kMaxPointLights = 48; // extra lights that illuminate without shadows
    static constexpr u32 kAtlasSize = 2048;    // 2x2 tiles of 1024 (kMaxLights)
    static constexpr u32 kAtlasTiles = 2;      // per axis

    bool create_depth();
    bool create_pipelines();
    bool create_shadow_resources();
    bool create_sync_and_commands();
    void recreate_swapchain();
    Mat4 compute_light_matrix() const;
    void process_lights(); // pick nearest lights, build atlas matrices + UBO
    // The world-space bounding sphere of a draw (mesh local sphere transformed by the model),
    // cached on each DrawItem so the shadow/light/main passes can frustum-cull cheaply.
    static Vec4 world_sphere(const Mesh& mesh, const Mat4& model);
    void submit(const Mesh& mesh, const Mat4& model, const Vec4& tint, Layer layer);
    void push_constants(const Mat4& model, const Vec4& tint, bool vegetation = false);
    void record_shadow_pass(VkCommandBuffer cmd);
    void record_light_atlas_pass(VkCommandBuffer cmd);
    void record_main_pass(VkCommandBuffer cmd);
    void record_ui_pass(VkCommandBuffer cmd);

    Window& window_;
    RendererConfig config_;

    vk::Instance instance_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    vk::Device device_;
    vk::Swapchain swapchain_;
    vk::Image depth_;
    vk::Pipeline pipeline_opaque_;
    vk::Pipeline pipeline_cutout_; // opaque + peek-through dissolve (tree trunks)
    vk::Pipeline pipeline_foliage_;
    vk::Pipeline pipeline_water_;
    vk::Pipeline pipeline_emissive_;
    vk::Pipeline pipeline_glow_;
    vk::Pipeline pipeline_vegetation_;
    vk::Pipeline pipeline_shadow_;
    vk::Pipeline pipeline_ui_;
    VkPipeline current_pipeline_ = VK_NULL_HANDLE; // avoids redundant binds within a frame

    // Shadow map: a depth texture rendered from the sun, sampled in the main pass.
    VkDescriptorSetLayout shadow_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<FrameSync> frames_;
    std::vector<VkSemaphore> render_finished_; // one per swapchain image
    std::vector<DrawItem> draw_items_;         // deferred draws for the current frame
    std::vector<UIDrawCmd> ui_items_;          // deferred 2D UI primitives (drawn last)
    std::vector<SpotLight> pending_lights_;    // lights submitted this frame
    std::vector<Mat4> active_light_vp_;        // view-proj of the lights given atlas tiles

    u32 frame_index_ = 0;
    u32 image_index_ = 0;
    bool frame_active_ = false;
    bool needs_resize_ = false;

    Mat4 view_{1.0f};
    Mat4 projection_{1.0f};
    Vec3 camera_position_{0.0f};
    Vec3 player_position_{0.0f};
    f32 wind_strength_ = 0.12f;
    f32 time_ = 0.0f;

    // Lighting / day-night state, pushed to the shaders each draw.
    Vec3 sun_direction_{0.36f, 0.86f, 0.36f};
    Vec3 sun_color_{1.0f, 0.97f, 0.9f};
    f32 sun_intensity_ = 1.0f;
    Vec3 sky_color_ = config_.clear_color;
    Vec3 fog_color_{0.52f, 0.58f, 0.66f}; // atmospheric haze colour (set per-frame by the game)
    f32 fog_density_ = 0.011f;            // exp-squared distance fog density
    f32 gloom_ = 0.0f;                    // town gloom 0..1 (deepens grade + vignette)
    f32 fog_patch_ = 0.0f;                // road fog-bank strength 0..1 (dense volumetric mist)
    Mat4 light_view_proj_{1.0f};
    f32 shadow_strength_ = 0.0f; // 0 until the shadow pass is active
};

} // namespace alryn
