#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Engine/Subsystem.h>
#include <Alryn/Platform/Events.h>

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

private:
    std::unordered_set<KeyCode> keys_;
    std::unordered_set<MouseCode> buttons_;
    Vec2 mouse_position_{0.0f};
    Vec2 last_mouse_{0.0f};
    Vec2 mouse_delta_{0.0f};
    f32 scroll_accum_ = 0.0f;
    f32 scroll_delta_ = 0.0f;
    bool first_mouse_ = true;
};

} // namespace alryn
