#pragma once

#include <Alryn/Core/Event.h>
#include <Alryn/Core/Math.h>
#include <Alryn/Core/NonCopyable.h>
#include <Alryn/Core/Types.h>

#include <vulkan/vulkan.h>

#include <string>
#include <utility>
#include <vector>

struct GLFWwindow; // keep GLFW out of the public header

namespace alryn {

struct WindowConfig {
    std::string title = "Alryn";
    u32 width = 1280;
    u32 height = 720;
    bool resizable = true;
};

// RAII GLFW window with Vulkan surface helpers. Pushes input/window events to a
// callback so the Application can route them through the engine. Headless-safe:
// create() returns false (instead of crashing) when no display is available.
class Window : public NonCopyable {
public:
    Window() = default;
    ~Window();

    bool create(const WindowConfig& config);
    void destroy();

    void poll_events();
    bool should_close() const;
    void set_should_close(bool value);

    // Locks/hides the cursor for FPS mouse-look (true), or restores it (false).
    void set_cursor_captured(bool captured);
    bool cursor_captured() const { return cursor_captured_; }

    // Resize the (windowed) window to the given client size.
    void set_size(u32 width, u32 height);
    // Switch between borderless-fullscreen (matching the primary monitor) and a
    // windowed size. Restores the previous windowed geometry when leaving.
    void set_fullscreen(bool fullscreen);
    bool fullscreen() const { return fullscreen_; }

    void set_event_callback(EventCallback callback) { callback_ = std::move(callback); }

    UVec2 framebuffer_size() const;
    GLFWwindow* native() const { return window_; }
    const WindowConfig& config() const { return config_; }

    // ---- Vulkan integration ----
    static std::vector<const char*> required_instance_extensions();
    VkSurfaceKHR create_surface(VkInstance instance) const;

    // True if a windowing platform (display) is usable. Lets headless
    // environments and tests skip windowed paths gracefully.
    static bool platform_available();

private:
    void install_callbacks();

    GLFWwindow* window_ = nullptr;
    WindowConfig config_;
    EventCallback callback_;
    u32 fb_width_ = 0;
    u32 fb_height_ = 0;
    bool cursor_captured_ = false;
    bool fullscreen_ = false;
    // Saved windowed geometry, so leaving fullscreen restores it.
    int windowed_x_ = 0;
    int windowed_y_ = 0;
    int windowed_w_ = 0;
    int windowed_h_ = 0;
};

} // namespace alryn
