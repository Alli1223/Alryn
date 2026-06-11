#include <Alryn/Platform/Input.h>

namespace alryn {

void Input::on_event(Event& event) {
    EventDispatcher dispatcher{event};
    dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
        keys_.insert(e.key());
        return false;
    });
    dispatcher.dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& e) {
        keys_.erase(e.key());
        return false;
    });
    dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
        buttons_.insert(e.button());
        return false;
    });
    dispatcher.dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
        buttons_.erase(e.button());
        return false;
    });
    dispatcher.dispatch<MouseMovedEvent>([&](MouseMovedEvent& e) {
        mouse_position_ = Vec2{e.x(), e.y()};
        if (first_mouse_) {
            last_mouse_ = mouse_position_;
            first_mouse_ = false;
        }
        return false;
    });
    dispatcher.dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& e) {
        scroll_accum_ += e.y_offset();
        return false;
    });
}

void Input::on_update(Timestep /*dt*/) {
    // Runs before the game's on_update, so the deltas reflect this frame's motion.
    mouse_delta_ = mouse_position_ - last_mouse_;
    last_mouse_ = mouse_position_;
    scroll_delta_ = scroll_accum_;
    scroll_accum_ = 0.0f;
}

} // namespace alryn
