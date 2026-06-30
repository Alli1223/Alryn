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

    // ---- Focus navigation (controller / keyboard) ---------------------------
    // Operate the menu without a pointer: a focused widget is highlighted with an accent ring (drawn
    // in render()) and acted on directly. Focus order is the on-screen layout (top-to-bottom, then
    // left-to-right). A menu rebuild (the focusable set changes) drops stale focus automatically.
    void focus_move(int dir);  // -1 previous (up) / +1 next (down) in screen order
    void focus_nav(int dir);   // -1/+1 left/right: adjust the focused widget (slider/stepper/swatch)
    void focus_activate();     // A / Enter: click / toggle the focused widget (or focus the first)
    void clear_focus() { focus_index_ = -1; }
    bool has_focus() const { return focus_index_ >= 0; }

private:
    std::vector<Widget*> collect_focusable() const; // visible+enabled+can_focus, in screen order

    Widget root_;
    int focus_index_ = -1;       // index into collect_focusable() order (-1 = nothing focused)
    usize last_focus_count_ = 0; // detect a menu rebuild (count change) -> drop stale focus
};

} // namespace alryn::ui
