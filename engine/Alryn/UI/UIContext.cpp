#include <Alryn/UI/UIContext.h>

#include <Alryn/UI/DrawList.h>
#include <Alryn/UI/Theme.h>

#include <algorithm>
#include <cmath>

namespace alryn::ui {

void UIContext::set_screen(f32 width, f32 height) {
    root_.bounds = Rect{0.0f, 0.0f, width, height};
}

void UIContext::update(f32 dt, const Vec2& pointer) {
    root_.dispatch_pointer_move(pointer);
    root_.update(dt);
    // A menu rebuild (show_screen clears + re-adds widgets) changes the focusable set; drop the now
    // stale focus index so it doesn't point at the wrong control on the new screen.
    const usize n = collect_focusable().size();
    if (n != last_focus_count_) {
        focus_index_ = -1;
        last_focus_count_ = n;
    }
}

void UIContext::render(Renderer& renderer) {
    DrawList draw_list{renderer};
    root_.draw(draw_list);
    // Focus ring: an accent outline around the focused control, drawn on top of the tree so it reads
    // clearly. Only shown while something is focused (i.e. the pad/keyboard is driving the menu).
    if (focus_index_ >= 0) {
        const std::vector<Widget*> f = collect_focusable();
        if (focus_index_ < static_cast<int>(f.size())) {
            const Rect& b = f[static_cast<usize>(focus_index_)]->bounds;
            constexpr f32 pad = 4.0f;
            draw_list.outline(Vec4{b.x - pad, b.y - pad, b.w + 2.0f * pad, b.h + 2.0f * pad},
                              theme().accent_hover, 2.5f, 9.0f);
        }
    }
}

// Walk the tree gathering the focusable controls (visible + enabled + can_focus), then order them by
// on-screen position - top-to-bottom, then left-to-right - so D-pad up/down moves the way the eye
// reads the menu. Parents gate their children's visibility.
std::vector<Widget*> UIContext::collect_focusable() const {
    std::vector<Widget*> out;
    auto gather = [&out](Widget& w, auto&& self) -> void {
        if (!w.visible) {
            return;
        }
        if (w.enabled && w.can_focus()) {
            out.push_back(&w);
        }
        for (const auto& child : w.children()) {
            self(*child, self);
        }
    };
    gather(const_cast<Widget&>(root_), gather);
    std::sort(out.begin(), out.end(), [](const Widget* a, const Widget* b) {
        const Vec2 ca = a->bounds.center();
        const Vec2 cb = b->bounds.center();
        if (std::abs(ca.y - cb.y) > 4.0f) {
            return ca.y < cb.y;
        }
        return ca.x < cb.x;
    });
    return out;
}

void UIContext::focus_move(int dir) {
    const std::vector<Widget*> f = collect_focusable();
    if (f.empty()) {
        focus_index_ = -1;
        return;
    }
    const int n = static_cast<int>(f.size());
    if (focus_index_ < 0) {
        focus_index_ = dir >= 0 ? 0 : n - 1; // first press lands on the first / last item
        return;
    }
    focus_index_ = ((focus_index_ + dir) % n + n) % n; // wrap around the ends
}

void UIContext::focus_nav(int dir) {
    const std::vector<Widget*> f = collect_focusable();
    if (focus_index_ >= 0 && focus_index_ < static_cast<int>(f.size())) {
        f[static_cast<usize>(focus_index_)]->on_nav(dir);
    }
}

void UIContext::focus_activate() {
    const std::vector<Widget*> f = collect_focusable();
    if (f.empty()) {
        focus_index_ = -1;
        return;
    }
    if (focus_index_ < 0 || focus_index_ >= static_cast<int>(f.size())) {
        focus_index_ = 0; // nothing focused yet: the first press just selects, doesn't fire
        return;
    }
    f[static_cast<usize>(focus_index_)]->on_activate();
}

} // namespace alryn::ui
