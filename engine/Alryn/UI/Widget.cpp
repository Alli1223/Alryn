#include <Alryn/UI/Widget.h>

namespace alryn::ui {

void Widget::update(f32 dt) {
    if (!visible) {
        return;
    }
    on_update(dt);
    for (auto& child : children_) {
        child->update(dt);
    }
}

void Widget::draw(DrawList& draw_list) {
    if (!visible) {
        return;
    }
    on_draw(draw_list);
    for (auto& child : children_) {
        child->draw(draw_list); // later children paint on top
    }
}

// Hover/move propagates to every visible widget so each can update its own hover
// state (a button must un-hover when the pointer leaves it).
bool Widget::dispatch_pointer_move(const Vec2& p) {
    if (!visible) {
        return false;
    }
    bool consumed = false;
    for (auto& child : children_) {
        consumed |= child->dispatch_pointer_move(p);
    }
    consumed |= on_pointer_move(p);
    return consumed;
}

bool Widget::dispatch_pointer_down(const Vec2& p, int button) {
    if (!visible) {
        return false;
    }
    // Front-to-back: the last-drawn (top-most) child gets first chance.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->dispatch_pointer_down(p, button)) {
            return true;
        }
    }
    return on_pointer_down(p, button);
}

bool Widget::dispatch_pointer_up(const Vec2& p, int button) {
    if (!visible) {
        return false;
    }
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->dispatch_pointer_up(p, button)) {
            return true;
        }
    }
    return on_pointer_up(p, button);
}

bool Widget::dispatch_text(char c) {
    if (!visible) {
        return false;
    }
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->dispatch_text(c)) {
            return true;
        }
    }
    return on_text(c);
}

bool Widget::dispatch_key(KeyCode key) {
    if (!visible) {
        return false;
    }
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->dispatch_key(key)) {
            return true;
        }
    }
    return on_key(key);
}

} // namespace alryn::ui
