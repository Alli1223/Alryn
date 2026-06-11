#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Platform/Events.h> // KeyCode
#include <Alryn/UI/Widget.h>

namespace alryn {
class Renderer;
}

namespace alryn::ui {

// Owns the root of a UI tree, tracks the screen size and routes input + drawing.
// Drive it from an Application: feed it pointer/keyboard events, call update()
// each frame, and render() inside on_render() (after the 3D scene). Build a menu
// by adding widgets as children of root().
class UIContext {
public:
    UIContext() = default;

    Widget& root() { return root_; }

    // Resize to the current framebuffer (root fills the whole screen).
    void set_screen(f32 width, f32 height);
    Vec2 screen() const { return Vec2{root_.bounds.w, root_.bounds.h}; }

    // Per-frame: refresh hover from the pointer position and tick animations.
    void update(f32 dt, const Vec2& pointer);
    // Emit the UI into the renderer's 2D overlay (call after the 3D draws).
    void render(Renderer& renderer);

    // Event entry points (forward these from Application::on_event / Input).
    bool pointer_down(const Vec2& p, int button) { return root_.dispatch_pointer_down(p, button); }
    bool pointer_up(const Vec2& p, int button) { return root_.dispatch_pointer_up(p, button); }
    void pointer_move(const Vec2& p) { root_.dispatch_pointer_move(p); }
    bool text(char c) { return root_.dispatch_text(c); }
    bool key(KeyCode k) { return root_.dispatch_key(k); }

private:
    Widget root_;
};

} // namespace alryn::ui
