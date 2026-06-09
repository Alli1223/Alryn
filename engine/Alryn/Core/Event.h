#pragma once

#include <Alryn/Core/Types.h>

#include <functional>
#include <string>

namespace alryn {

enum class EventType : u16 {
    None = 0,
    WindowClose,
    WindowResize,
    WindowFocus,
    WindowLostFocus,
    KeyPressed,
    KeyReleased,
    KeyTyped,
    MouseButtonPressed,
    MouseButtonReleased,
    MouseMoved,
    MouseScrolled,
};

// Bit flags so a handler can ask "is this any kind of input event?".
enum EventCategory : u16 {
    EventCategoryNone        = 0,
    EventCategoryApplication = 1 << 0,
    EventCategoryInput       = 1 << 1,
    EventCategoryKeyboard    = 1 << 2,
    EventCategoryMouse       = 1 << 3,
    EventCategoryMouseButton = 1 << 4,
};

// Polymorphic base for all events. Concrete events (window/key/mouse) live in
// Platform/Events.h and use the ALRYN_EVENT_CLASS_TYPE macro below.
class Event {
public:
    virtual ~Event() = default;

    virtual EventType type() const = 0;
    virtual const char* name() const = 0;
    virtual u16 category_flags() const = 0;
    virtual std::string to_string() const { return name(); }

    bool in_category(EventCategory category) const {
        return (category_flags() & category) != 0;
    }

    bool handled = false;
};

// Routes an event to a typed handler. The handler returns true if it consumed
// the event. Usage:
//   EventDispatcher d{event};
//   d.dispatch<WindowCloseEvent>([](WindowCloseEvent& e){ ...; return true; });
class EventDispatcher {
public:
    explicit EventDispatcher(Event& event) : event_(event) {}

    template <typename T, typename Fn>
    bool dispatch(Fn&& handler) {
        if (event_.type() == T::static_type()) {
            event_.handled |= handler(static_cast<T&>(event_));
            return true;
        }
        return false;
    }

private:
    Event& event_;
};

using EventCallback = std::function<void(Event&)>;

} // namespace alryn

#define ALRYN_EVENT_CLASS_TYPE(type_enum)                                          \
    static ::alryn::EventType static_type() { return ::alryn::EventType::type_enum; } \
    ::alryn::EventType type() const override { return static_type(); }              \
    const char* name() const override { return #type_enum; }

#define ALRYN_EVENT_CLASS_CATEGORY(flags)                                          \
    ::alryn::u16 category_flags() const override { return (flags); }
