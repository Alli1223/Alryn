#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Engine/Subsystem.h>
#include <Alryn/Platform/Events.h>

#include <array>
#include <unordered_set>

namespace alryn {

// Polled input state, maintained from the event stream. Register it as a
// subsystem (Application does this for windowed apps) and query it each frame:
//
//     if (input.key_down(GLFW_KEY_W)) ...
//     Vec2 look = input.mouse_delta();   // for mouse-look
class Input : public Subsystem {
public:
    const char* name() const override { return "Input"; }
    void on_event(Event& event) override;
    void on_update(Timestep dt) override;

    bool key_down(KeyCode key) const { return keys_.contains(key); }
    bool mouse_down(MouseCode button) const { return buttons_.contains(button); }

    Vec2 mouse_position() const { return mouse_position_; }
    Vec2 mouse_delta() const { return mouse_delta_; }   // movement since last frame
    f32 scroll_delta() const { return scroll_delta_; }  // wheel ticks this frame (+ = up)

    // --- Gamepad (the first connected controller, polled each frame in on_update) -------------
    // Sticks are radial-deadzoned to [-1, 1] (x = right, y = DOWN per the screen/GLFW convention -
    // negate y for "up = forward"); triggers are 0..1; buttons use GLFW_GAMEPAD_BUTTON_* indices
    // (see the pad:: constants in GameConfig).
    bool gamepad_present() const { return gamepad_present_; }
    Vec2 left_stick() const { return left_stick_; }
    Vec2 right_stick() const { return right_stick_; }
    f32 left_trigger() const { return left_trigger_; }
    f32 right_trigger() const { return right_trigger_; }
    bool pad_down(int button) const {
        return button >= 0 && button < kPadButtons && pad_buttons_[static_cast<usize>(button)] != 0;
    }
    bool pad_pressed(int button) const { // rising edge this frame
        return button >= 0 && button < kPadButtons &&
               pad_buttons_[static_cast<usize>(button)] != 0 &&
               pad_prev_[static_cast<usize>(button)] == 0;
    }

private:
    void poll_gamepad();

    std::unordered_set<KeyCode> keys_;
    std::unordered_set<MouseCode> buttons_;
    Vec2 mouse_position_{0.0f};
    Vec2 last_mouse_{0.0f};
    Vec2 mouse_delta_{0.0f};
    f32 scroll_accum_ = 0.0f;
    f32 scroll_delta_ = 0.0f;
    bool first_mouse_ = true;

    static constexpr int kPadButtons = 15; // GLFW_GAMEPAD_BUTTON_LAST + 1
    bool gamepad_present_ = false;
    Vec2 left_stick_{0.0f};
    Vec2 right_stick_{0.0f};
    f32 left_trigger_ = 0.0f;
    f32 right_trigger_ = 0.0f;
    std::array<u8, kPadButtons> pad_buttons_{};
    std::array<u8, kPadButtons> pad_prev_{};
};

} // namespace alryn
