#include <Alryn/UI/Widgets.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace alryn::ui {

Theme& theme() {
    static Theme t;
    return t;
}

namespace {
constexpr KeyCode kKeyBackspace = 259; // GLFW_KEY_BACKSPACE

// Frame-rate independent exponential approach of `current` toward `target`.
f32 approach(f32 current, f32 target, f32 dt, f32 speed = 14.0f) {
    return current + (target - current) * std::min(1.0f, dt * speed);
}

Vec4 with_alpha(const Vec4& c, f32 a) { return Vec4{c.r, c.g, c.b, c.a * a}; }
} // namespace

// ---- Panel ---------------------------------------------------------------
void Panel::on_draw(DrawList& dl) {
    if (!fill) {
        return;
    }
    dl.rect(bounds.xywh(), color, border, 1.5f, radius);
    // Medieval framing (procedural, no art assets): an inset hairline + gilded corner brackets in
    // the theme's accent, so a card reads as a carved, gold-edged board rather than a flat panel.
    // Driven by the theme accent, so it adapts if a game restyles; skipped on tiny panels.
    constexpr f32 inset = 4.0f;
    const f32 ix = bounds.x + inset, iy = bounds.y + inset;
    const f32 iw = bounds.w - 2.0f * inset, ih = bounds.h - 2.0f * inset;
    if (iw < 28.0f || ih < 28.0f) {
        return;
    }
    const Theme& th = theme();
    const Vec4 hair{th.accent.r, th.accent.g, th.accent.b, 0.30f};
    dl.outline(Vec4{ix, iy, iw, ih}, hair, 1.0f, std::max(radius - inset, 0.0f));
    const Vec4 gold = th.accent_hover;
    const f32 L = std::min(20.0f, std::min(iw, ih) * 0.26f);
    constexpr f32 t = 2.0f;
    const Vec2 tl{ix, iy}, tr{ix + iw, iy}, br{ix + iw, iy + ih}, bl{ix, iy + ih};
    dl.line(tl, tl + Vec2{L, 0.0f}, t, gold);
    dl.line(tl, tl + Vec2{0.0f, L}, t, gold);
    dl.line(tr, tr - Vec2{L, 0.0f}, t, gold);
    dl.line(tr, tr + Vec2{0.0f, L}, t, gold);
    dl.line(br, br - Vec2{L, 0.0f}, t, gold);
    dl.line(br, br - Vec2{0.0f, L}, t, gold);
    dl.line(bl, bl + Vec2{L, 0.0f}, t, gold);
    dl.line(bl, bl - Vec2{0.0f, L}, t, gold);
}

// ---- Label ---------------------------------------------------------------
void Label::on_draw(DrawList& dl) {
    const f32 y = bounds.y + (bounds.h - size) * 0.5f;
    f32 x = bounds.x;
    if (align == TextAlign::Center) {
        x = bounds.x + bounds.w * 0.5f;
    } else if (align == TextAlign::Right) {
        x = bounds.x + bounds.w;
    }
    dl.text(Vec2{x, y}, text, size, color, align);
}

// ---- Button --------------------------------------------------------------
void Button::on_update(f32 dt) {
    const f32 target = !enabled ? 0.0f : (pressed_ ? 1.0f : (hovered_ ? 0.6f : 0.0f));
    hover_anim_ = approach(hover_anim_, target, dt);
}

void Button::on_draw(DrawList& dl) {
    const Theme& th = theme();
    const Vec4 base = primary ? th.accent : th.button;
    const Vec4 hot = primary ? th.accent_hover : th.button_hover;
    Vec4 col = glm::mix(base, hot, glm::clamp(hover_anim_, 0.0f, 1.0f));
    if (pressed_) {
        col = primary ? glm::mix(col, Vec4{base.r * 0.8f, base.g * 0.8f, base.b * 0.8f, base.a}, 0.6f)
                      : glm::mix(col, th.button_press, 0.6f);
    }
    if (!enabled) {
        col = with_alpha(col, 0.4f);
    }
    dl.rect(bounds.xywh(), col, th.radius);

    const Vec4 txt = primary ? Vec4{1.0f, 1.0f, 1.0f, 1.0f} : th.text;
    dl.text(bounds.center() - Vec2{0.0f, text_size * 0.5f}, label, text_size,
            enabled ? txt : with_alpha(txt, 0.5f), TextAlign::Center);
}

bool Button::on_pointer_move(const Vec2& p) {
    hovered_ = enabled && bounds.contains(p);
    return false;
}

bool Button::on_pointer_down(const Vec2& p, int button) {
    if (enabled && button == 0 && bounds.contains(p)) {
        pressed_ = true;
        return true;
    }
    return false;
}

bool Button::on_pointer_up(const Vec2& p, int button) {
    if (!pressed_) {
        return false;
    }
    pressed_ = false;
    if (button == 0 && enabled && bounds.contains(p) && on_click) {
        on_click();
    }
    return true;
}

// ---- Toggle --------------------------------------------------------------
Rect Toggle::switch_rect() const {
    constexpr f32 w = 48.0f;
    constexpr f32 h = 26.0f;
    return Rect{bounds.x + bounds.w - w, bounds.y + (bounds.h - h) * 0.5f, w, h};
}

void Toggle::on_update(f32 dt) { anim_ = approach(anim_, value ? 1.0f : 0.0f, dt, 16.0f); }

void Toggle::on_draw(DrawList& dl) {
    const Theme& th = theme();
    const f32 y = bounds.y + (bounds.h - text_size) * 0.5f;
    dl.text(Vec2{bounds.x, y}, label, text_size, th.text);

    const Rect sw = switch_rect();
    const Vec4 track = glm::mix(th.track, th.accent, anim_);
    dl.rect(sw.xywh(), track, sw.h * 0.5f);

    const f32 pad = 3.0f;
    const f32 knob_d = sw.h - pad * 2.0f;
    const f32 kx = sw.x + pad + (sw.w - knob_d - pad * 2.0f) * anim_;
    dl.rect(Vec4{kx, sw.y + pad, knob_d, knob_d}, th.knob, knob_d * 0.5f);
}

bool Toggle::on_pointer_move(const Vec2& p) {
    hovered_ = bounds.contains(p);
    return false;
}

bool Toggle::on_pointer_down(const Vec2& p, int button) {
    if (button == 0 && bounds.contains(p)) {
        value = !value;
        if (on_change) {
            on_change(value);
        }
        return true;
    }
    return false;
}

// ---- Slider --------------------------------------------------------------
Rect Slider::track_rect() const {
    constexpr f32 h = 6.0f;
    return Rect{bounds.x, bounds.y + bounds.h - 14.0f, bounds.w, h};
}

f32 Slider::norm() const {
    const f32 span = max_value - min_value;
    return span > 1e-6f ? glm::clamp((value - min_value) / span, 0.0f, 1.0f) : 0.0f;
}

void Slider::set_from_pointer(f32 px) {
    const Rect tr = track_rect();
    const f32 t = tr.w > 1e-6f ? glm::clamp((px - tr.x) / tr.w, 0.0f, 1.0f) : 0.0f;
    f32 v = min_value + t * (max_value - min_value);
    if (integer) {
        v = std::round(v);
    }
    if (v != value) {
        value = v;
        if (on_change) {
            on_change(value);
        }
    }
}

void Slider::on_draw(DrawList& dl) {
    const Theme& th = theme();
    dl.text(Vec2{bounds.x, bounds.y}, label, text_size, th.text);

    // Value read-out, right aligned.
    char buf[32];
    if (integer) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(value)));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value));
    }
    dl.text(Vec2{bounds.x + bounds.w, bounds.y}, buf, text_size, th.text_muted, TextAlign::Right);

    const Rect tr = track_rect();
    dl.rect(tr.xywh(), th.track, tr.h * 0.5f);
    const f32 fill_w = tr.w * norm();
    dl.rect(Vec4{tr.x, tr.y, fill_w, tr.h}, th.accent, tr.h * 0.5f);

    const f32 knob_d = 18.0f;
    const f32 kx = tr.x + fill_w - knob_d * 0.5f;
    dl.rect(Vec4{kx, tr.y + tr.h * 0.5f - knob_d * 0.5f, knob_d, knob_d}, th.knob, knob_d * 0.5f);
}

bool Slider::on_pointer_move(const Vec2& p) {
    if (dragging_) {
        set_from_pointer(p.x);
        return true;
    }
    return false;
}

bool Slider::on_pointer_down(const Vec2& p, int button) {
    if (button == 0 && bounds.contains(p)) {
        dragging_ = true;
        set_from_pointer(p.x);
        return true;
    }
    return false;
}

bool Slider::on_pointer_up(const Vec2& /*p*/, int button) {
    if (dragging_ && button == 0) {
        dragging_ = false;
        return true;
    }
    return false;
}

// ---- Stepper -------------------------------------------------------------
Rect Stepper::left_arrow() const {
    return Rect{bounds.x + bounds.w - 180.0f, bounds.y + (bounds.h - 30.0f) * 0.5f, 30.0f, 30.0f};
}
Rect Stepper::right_arrow() const {
    return Rect{bounds.x + bounds.w - 30.0f, bounds.y + (bounds.h - 30.0f) * 0.5f, 30.0f, 30.0f};
}

void Stepper::step(int dir) {
    if (options.empty()) {
        return;
    }
    const usize n = options.size();
    index = (index + static_cast<usize>((dir % static_cast<int>(n)) + static_cast<int>(n))) % n;
    if (on_change) {
        on_change(index);
    }
}

void Stepper::on_update(f32 /*dt*/) {}

void Stepper::on_draw(DrawList& dl) {
    const Theme& th = theme();
    const f32 y = bounds.y + (bounds.h - text_size) * 0.5f;
    dl.text(Vec2{bounds.x, y}, label, text_size, th.text);

    auto arrow = [&](const Rect& r, const char* glyph, int side) {
        const Vec4 c = hovered_ == side ? th.button_hover : th.button;
        dl.rect(r.xywh(), c, th.radius * 0.6f);
        dl.text(r.center() - Vec2{0.0f, text_size * 0.5f}, glyph, text_size, th.text,
                TextAlign::Center);
    };
    arrow(left_arrow(), "<", -1);
    arrow(right_arrow(), ">", 1);

    const std::string value = options.empty() ? "" : options[index % options.size()];
    const Vec2 center{bounds.x + bounds.w - 90.0f, bounds.y + bounds.h * 0.5f - text_size * 0.5f};
    dl.text(center, value, text_size, th.text, TextAlign::Center);
}

bool Stepper::on_pointer_move(const Vec2& p) {
    hovered_ = left_arrow().contains(p) ? -1 : right_arrow().contains(p) ? 1 : 0;
    return false;
}

bool Stepper::on_pointer_down(const Vec2& p, int button) {
    if (button != 0) {
        return false;
    }
    if (left_arrow().contains(p)) {
        step(-1);
        return true;
    }
    if (right_arrow().contains(p)) {
        step(1);
        return true;
    }
    return false;
}

// ---- SwatchRow -----------------------------------------------------------
Rect SwatchRow::swatch_rect(usize i) const {
    const usize n = colors.empty() ? 1 : colors.size();
    const f32 total_gap = gap * static_cast<f32>(n - 1);
    const f32 cell = (bounds.w - total_gap) / static_cast<f32>(n);
    return Rect{bounds.x + static_cast<f32>(i) * (cell + gap), bounds.y, cell, bounds.h};
}

void SwatchRow::on_draw(DrawList& dl) {
    const Theme& th = theme();
    for (usize i = 0; i < colors.size(); ++i) {
        const Rect r = swatch_rect(i);
        const Vec4 col{colors[i].r, colors[i].g, colors[i].b, 1.0f};
        if (i == index) {
            // Selected: accent ring around the swatch.
            dl.rect(r.xywh(), col, th.accent, 3.0f, 7.0f);
        } else if (static_cast<int>(i) == hovered_) {
            dl.rect(r.xywh(), col, Vec4{1.0f, 1.0f, 1.0f, 0.5f}, 2.0f, 7.0f);
        } else {
            dl.rect(r.xywh(), col, 7.0f);
        }
    }
}

bool SwatchRow::on_pointer_move(const Vec2& p) {
    hovered_ = -1;
    for (usize i = 0; i < colors.size(); ++i) {
        if (swatch_rect(i).contains(p)) {
            hovered_ = static_cast<int>(i);
            break;
        }
    }
    return false;
}

bool SwatchRow::on_pointer_down(const Vec2& p, int button) {
    if (button != 0) {
        return false;
    }
    for (usize i = 0; i < colors.size(); ++i) {
        if (swatch_rect(i).contains(p)) {
            index = i;
            if (on_change) {
                on_change(i);
            }
            return true;
        }
    }
    return false;
}

// ---- TextField -----------------------------------------------------------
void TextField::on_update(f32 dt) { caret_blink_ = std::fmod(caret_blink_ + dt, 1.0f); }

void TextField::on_draw(DrawList& dl) {
    const Theme& th = theme();
    const Vec4 border = focused ? th.accent : th.panel_border;
    dl.rect(bounds.xywh(), Vec4{0.06f, 0.07f, 0.10f, 1.0f}, border, focused ? 2.0f : 1.5f,
            th.radius * 0.7f);

    const f32 pad = 12.0f;
    const f32 ty = bounds.y + (bounds.h - text_size) * 0.5f;
    const bool empty = text.empty();
    const std::string& shown = empty ? placeholder : text;
    dl.text(Vec2{bounds.x + pad, ty}, shown, text_size, empty ? th.text_muted : th.text);

    if (focused && caret_blink_ < 0.5f) {
        const f32 cx = bounds.x + pad + font_text_width(text, text_size) + 2.0f;
        dl.line(Vec2{cx, ty}, Vec2{cx, ty + text_size}, 2.0f, th.text);
    }
}

bool TextField::on_pointer_down(const Vec2& p, int button) {
    if (button == 0) {
        focused = bounds.contains(p);
        return focused;
    }
    return false;
}

bool TextField::on_text(char c) {
    if (!focused || text.size() >= max_length) {
        return false;
    }
    if (filter && !filter(c)) {
        return false;
    }
    text.push_back(c);
    if (on_change) {
        on_change(text);
    }
    return true;
}

bool TextField::on_key(KeyCode key) {
    if (focused && key == kKeyBackspace && !text.empty()) {
        text.pop_back();
        if (on_change) {
            on_change(text);
        }
        return true;
    }
    return false;
}

} // namespace alryn::ui
