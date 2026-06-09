#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <Alryn/Platform/Window.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Platform/Events.h>

namespace alryn {

namespace {

int g_glfw_refcount = 0;

void glfw_error_callback(int code, const char* description) {
    ALRYN_WARN("[glfw] ({}) {}", code, description);
}

bool ensure_glfw_init() {
    if (g_glfw_refcount > 0) {
        return true;
    }
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() != GLFW_TRUE) {
        return false;
    }
    ++g_glfw_refcount;
    return true;
}

void release_glfw() {
    if (g_glfw_refcount > 0 && --g_glfw_refcount == 0) {
        glfwTerminate();
    }
}

} // namespace

bool Window::platform_available() {
    return ensure_glfw_init();
}

bool Window::create(const WindowConfig& config) {
    config_ = config;
    if (!ensure_glfw_init()) {
        ALRYN_ERROR("GLFW init failed - no windowing platform available (headless?)");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan: don't create a GL context
    glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(static_cast<int>(config.width), static_cast<int>(config.height),
                               config.title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        ALRYN_ERROR("glfwCreateWindow failed");
        release_glfw();
        return false;
    }

    glfwSetWindowUserPointer(window_, this);
    install_callbacks();

    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    fb_width_ = static_cast<u32>(fb_w);
    fb_height_ = static_cast<u32>(fb_h);

    ALRYN_INFO("Window '{}' created ({}x{}, framebuffer {}x{})", config.title, config.width,
               config.height, fb_w, fb_h);
    return true;
}

void Window::destroy() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        release_glfw();
    }
}

Window::~Window() {
    destroy();
}

void Window::poll_events() {
    glfwPollEvents();
}

bool Window::should_close() const {
    return window_ == nullptr || glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::set_should_close(bool value) {
    if (window_ != nullptr) {
        glfwSetWindowShouldClose(window_, value ? GLFW_TRUE : GLFW_FALSE);
    }
}

void Window::set_cursor_captured(bool captured) {
    if (window_ == nullptr) {
        return;
    }
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (captured && glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    cursor_captured_ = captured;
}

UVec2 Window::framebuffer_size() const {
    if (window_ == nullptr) {
        return UVec2{0, 0};
    }
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    return UVec2{static_cast<u32>(w), static_cast<u32>(h)};
}

std::vector<const char*> Window::required_instance_extensions() {
    ensure_glfw_init();
    u32 count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions == nullptr) {
        return {};
    }
    return std::vector<const char*>(extensions, extensions + count);
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        ALRYN_ERROR("glfwCreateWindowSurface failed");
        return VK_NULL_HANDLE;
    }
    return surface;
}

void Window::install_callbacks() {
    // Captureless lambdas convert to GLFW C callbacks. They recover the Window
    // via the user pointer; being defined in a member function, they may touch
    // private members.
    glfwSetWindowCloseCallback(window_, [](GLFWwindow* w) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self != nullptr && self->callback_) {
            WindowCloseEvent event;
            self->callback_(event);
        }
    });

    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int width, int height) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self == nullptr) {
            return;
        }
        self->fb_width_ = static_cast<u32>(width);
        self->fb_height_ = static_cast<u32>(height);
        if (self->callback_) {
            WindowResizeEvent event{static_cast<u32>(width), static_cast<u32>(height)};
            self->callback_(event);
        }
    });

    glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self == nullptr || !self->callback_) {
            return;
        }
        if (action == GLFW_PRESS) {
            KeyPressedEvent event{key, false};
            self->callback_(event);
        } else if (action == GLFW_REPEAT) {
            KeyPressedEvent event{key, true};
            self->callback_(event);
        } else if (action == GLFW_RELEASE) {
            KeyReleasedEvent event{key};
            self->callback_(event);
        }
    });

    glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self != nullptr && self->callback_) {
            MouseMovedEvent event{static_cast<f32>(x), static_cast<f32>(y)};
            self->callback_(event);
        }
    });

    glfwSetScrollCallback(window_, [](GLFWwindow* w, double dx, double dy) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self != nullptr && self->callback_) {
            MouseScrolledEvent event{static_cast<f32>(dx), static_cast<f32>(dy)};
            self->callback_(event);
        }
    });

    glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int /*mods*/) {
        auto* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
        if (self == nullptr || !self->callback_) {
            return;
        }
        if (action == GLFW_PRESS) {
            MouseButtonPressedEvent event{button};
            self->callback_(event);
        } else if (action == GLFW_RELEASE) {
            MouseButtonReleasedEvent event{button};
            self->callback_(event);
        }
    });
}

} // namespace alryn
