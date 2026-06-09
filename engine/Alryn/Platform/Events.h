#pragma once

#include <Alryn/Core/Event.h>
#include <Alryn/Core/Types.h>

#include <format>
#include <string>

namespace alryn {

using KeyCode = i32;     // GLFW key codes (GLFW_KEY_*)
using MouseCode = i32;   // GLFW mouse button codes (GLFW_MOUSE_BUTTON_*)

// ---- Window events -------------------------------------------------------

class WindowCloseEvent : public Event {
public:
    ALRYN_EVENT_CLASS_TYPE(WindowClose)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryApplication)
};

class WindowResizeEvent : public Event {
public:
    WindowResizeEvent(u32 width, u32 height) : width_(width), height_(height) {}
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    std::string to_string() const override {
        return std::format("WindowResize({}, {})", width_, height_);
    }
    ALRYN_EVENT_CLASS_TYPE(WindowResize)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryApplication)

private:
    u32 width_;
    u32 height_;
};

// ---- Keyboard events -----------------------------------------------------

class KeyEvent : public Event {
public:
    KeyCode key() const { return key_; }
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryKeyboard)

protected:
    explicit KeyEvent(KeyCode key) : key_(key) {}
    KeyCode key_;
};

class KeyPressedEvent : public KeyEvent {
public:
    explicit KeyPressedEvent(KeyCode key, bool repeat = false) : KeyEvent(key), repeat_(repeat) {}
    bool is_repeat() const { return repeat_; }
    std::string to_string() const override {
        return std::format("KeyPressed({}{})", key_, repeat_ ? ", repeat" : "");
    }
    ALRYN_EVENT_CLASS_TYPE(KeyPressed)

private:
    bool repeat_;
};

class KeyReleasedEvent : public KeyEvent {
public:
    explicit KeyReleasedEvent(KeyCode key) : KeyEvent(key) {}
    ALRYN_EVENT_CLASS_TYPE(KeyReleased)
};

// ---- Mouse events --------------------------------------------------------

class MouseMovedEvent : public Event {
public:
    MouseMovedEvent(f32 x, f32 y) : x_(x), y_(y) {}
    f32 x() const { return x_; }
    f32 y() const { return y_; }
    ALRYN_EVENT_CLASS_TYPE(MouseMoved)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)

private:
    f32 x_;
    f32 y_;
};

class MouseScrolledEvent : public Event {
public:
    MouseScrolledEvent(f32 x_offset, f32 y_offset) : x_(x_offset), y_(y_offset) {}
    f32 x_offset() const { return x_; }
    f32 y_offset() const { return y_; }
    ALRYN_EVENT_CLASS_TYPE(MouseScrolled)
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse)

private:
    f32 x_;
    f32 y_;
};

class MouseButtonEvent : public Event {
public:
    MouseCode button() const { return button_; }
    ALRYN_EVENT_CLASS_CATEGORY(EventCategoryInput | EventCategoryMouse | EventCategoryMouseButton)

protected:
    explicit MouseButtonEvent(MouseCode button) : button_(button) {}
    MouseCode button_;
};

class MouseButtonPressedEvent : public MouseButtonEvent {
public:
    explicit MouseButtonPressedEvent(MouseCode button) : MouseButtonEvent(button) {}
    ALRYN_EVENT_CLASS_TYPE(MouseButtonPressed)
};

class MouseButtonReleasedEvent : public MouseButtonEvent {
public:
    explicit MouseButtonReleasedEvent(MouseCode button) : MouseButtonEvent(button) {}
    ALRYN_EVENT_CLASS_TYPE(MouseButtonReleased)
};

} // namespace alryn
