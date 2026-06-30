#include <Alryn/Platform/Input.h>

#include <GLFW/glfw3.h>

#include <cmath>

namespace alryn {

namespace {
// Radial deadzone + rescale: ignore tiny stick drift, then ramp the rest from 0 at the edge of the
// deadzone up to 1 at full deflection, so there's no sudden jump out of the dead region.
Vec2 deadzone(Vec2 v, f32 dz) {
    const f32 m = std::sqrt(v.x * v.x + v.y * v.y);
    if (m < dz) {
        return Vec2{0.0f};
    }
    const f32 scaled = (m - dz) / (1.0f - dz);
    return v * (std::min(scaled, 1.0f) / m);
}
} // namespace

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
    poll_gamepad();
}

// GLFW exposes gamepads through a polling API (not the event stream), so we sample the first
// connected controller each frame. glfwGetGamepadState returns false unless the joystick is present
// AND has a gamepad mapping (GLFW ships the SDL controller DB), so unknown sticks are ignored.
void Input::poll_gamepad() {
    pad_prev_ = pad_buttons_;
    pad_buttons_.fill(0);
    gamepad_present_ = false;
    left_stick_ = Vec2{0.0f};
    right_stick_ = Vec2{0.0f};
    left_trigger_ = 0.0f;
    right_trigger_ = 0.0f;
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        GLFWgamepadstate state;
        if (glfwGetGamepadState(jid, &state) != GLFW_TRUE) {
            continue;
        }
        gamepad_present_ = true;
        constexpr f32 dz = 0.20f;
        left_stick_ = deadzone(Vec2{state.axes[GLFW_GAMEPAD_AXIS_LEFT_X],
                                    state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]}, dz);
        right_stick_ = deadzone(Vec2{state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X],
                                     state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]}, dz);
        // GLFW reports triggers on [-1 (released), +1 (pressed)] -> remap to [0, 1].
        left_trigger_ = (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
        right_trigger_ = (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
        for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST && b < kPadButtons; ++b) {
            pad_buttons_[static_cast<usize>(b)] = state.buttons[b];
        }
        break; // the first connected gamepad drives everything
    }
}

} // namespace alryn
