#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Platform/Events.h> // KeyCode

#include <memory>
#include <utility>
#include <vector>

namespace alryn::ui {

class DrawList;

// Axis-aligned rectangle in pixels (origin top-left).
struct Rect {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;

    bool contains(const Vec2& p) const {
        return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
    }
    Vec2 center() const { return Vec2{x + w * 0.5f, y + h * 0.5f}; }
    Vec4 xywh() const { return Vec4{x, y, w, h}; }
};

// Base class for every UI element. Buttons, labels, panels, sliders... all derive
// from Widget and override the protected on_* hooks. A Widget owns a list of child
// widgets; update/draw recurse down the tree and pointer/keyboard input is routed
// front-to-back (last-drawn child gets first chance to consume an event).
//
// This is the single extension point for custom UI: derive Widget, override
// on_draw() to paint yourself with the DrawList, and the on_pointer_*/on_key/
// on_text hooks to react to input.
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    Rect bounds{};
    bool visible = true;
    bool enabled = true;

    // Construct a child of type T in place and return a reference to it.
    template <class T, class... Args>
    T& add(Args&&... args) {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *child;
        children_.push_back(std::move(child));
        return ref;
    }
    Widget& add_child(std::unique_ptr<Widget> child) {
        Widget& ref = *child;
        children_.push_back(std::move(child));
        return ref;
    }
    void clear_children() { children_.clear(); }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }

    // ---- Tree traversal (called by UIContext / parent widgets) --------------
    void update(f32 dt);
    void draw(DrawList& draw_list);
    bool dispatch_pointer_move(const Vec2& p);
    bool dispatch_pointer_down(const Vec2& p, int button);
    bool dispatch_pointer_up(const Vec2& p, int button);
    bool dispatch_text(char c);
    bool dispatch_key(KeyCode key);

    // ---- Focus navigation (keyboard / controller) ---------------------------
    // UIContext drives these so a menu can be operated without a pointer: it tracks a focused
    // widget, moves focus between the focusable ones in screen order, and activates / adjusts the
    // focused one. Defaults make a widget non-focusable + inert, so containers (Panel/Label) are
    // skipped; the interactive widgets override them. `on_activate` is a "click", `on_nav` nudges a
    // value left/right (slider/stepper/swatches).
    virtual bool can_focus() const { return false; }
    virtual void on_activate() {}
    virtual void on_nav(int dir) { (void)dir; }

protected:
    // Override these in subclasses. Pointer hooks return true to consume the event.
    virtual void on_update(f32 dt) { (void)dt; }
    virtual void on_draw(DrawList& draw_list) { (void)draw_list; }
    virtual bool on_pointer_move(const Vec2& p) { (void)p; return false; }
    virtual bool on_pointer_down(const Vec2& p, int button) { (void)p; (void)button; return false; }
    virtual bool on_pointer_up(const Vec2& p, int button) { (void)p; (void)button; return false; }
    virtual bool on_text(char c) { (void)c; return false; }
    virtual bool on_key(KeyCode key) { (void)key; return false; }

    std::vector<std::unique_ptr<Widget>> children_;
};

} // namespace alryn::ui
