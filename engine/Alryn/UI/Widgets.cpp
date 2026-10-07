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
Vec4 shade(const Vec4& c, f32 k) { return Vec4{c.r * k, c.g * k, c.b * k, c.a}; }

// The style for a control's label: the theme's button face with a soft shadow.
TextStyle control_text(f32 size, const Vec4& color, TextAlign align = TextAlign::Left) {
    const Theme& th = theme();
    TextStyle s;
    s.face = th.button_font;
    s.size = size;
    s.color = color;
    s.align = align;
    s.tracking = 0.04f;
    s.shadow = with_alpha(th.text_shadow, color.a);
    s.shadow_offset = Vec2{0.0f, 1.0f + size * 0.06f};
    s.shadow_blur = 1.0f + size * 0.1f;
    return s;
}

// A small gilded boss (a rivet / stud) centred at `c`: dark rim, gold body, a bright catch-light.
void stud(DrawList& dl, const Vec2& c, f32 r, const Theme& th) {
    dl.rect(Vec4{c.x - r - 1.0f, c.y - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f},
            Vec4{0.0f, 0.0f, 0.0f, 0.55f}, r + 1.0f);
    dl.gradient(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, th.accent_hover, shade(th.accent, 0.7f), r);
    const f32 hr = r * 0.38f;
    dl.rect(Vec4{c.x - r * 0.45f - hr, c.y - r * 0.45f - hr, hr * 2.0f, hr * 2.0f},
            Vec4{1.0f, 0.96f, 0.85f, 0.75f}, hr);
}
} // namespace

// ---- Panel ---------------------------------------------------------------
void Panel::on_draw(DrawList& dl) {
    if (!fill) {
        return;
    }
    const Theme& th = theme();
    if (shadow) {
        dl.shadow(bounds.xywh(), radius, 22.0f, th.shadow, Vec2{0.0f, 8.0f});
    }
    const Vec4 bottom = color_bottom.a > 0.0f ? color_bottom
                        : color == th.panel ? th.panel_bottom
                                            : color;
    dl.gradient(bounds.xywh(), color, bottom, radius, border, 1.5f);
    // Medieval framing (procedural, no art assets): an inset gold hairline with a gilded stud and
    // bracket at each corner, so a card reads as a carved, gold-bound board rather than a flat
    // panel. Driven by the theme accent, so it adapts if a game restyles; skipped on small panels.
    constexpr f32 inset = 6.0f;
    const f32 ix = bounds.x + inset, iy = bounds.y + inset;
    const f32 iw = bounds.w - 2.0f * inset, ih = bounds.h - 2.0f * inset;
    if (!ornate || iw < 60.0f || ih < 60.0f) {
        return;
    }
    // A faint highlight along the top edge, as if lit from above.
    dl.line(Vec2{bounds.x + radius, bounds.y + 1.5f}, Vec2{bounds.x + bounds.w - radius, bounds.y + 1.5f},
            1.0f, Vec4{1.0f, 0.92f, 0.75f, 0.10f});
    dl.outline(Vec4{ix, iy, iw, ih}, with_alpha(th.accent, 0.32f), 1.0f,
               std::max(radius - inset, 0.0f));
    const Vec4 gold = with_alpha(th.accent_hover, 0.9f);
    const f32 L = std::min(26.0f, std::min(iw, ih) * 0.22f);
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
    for (const Vec2& c : {tl, tr, br, bl}) {
        stud(dl, c, 3.5f, th);
    }
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
    const Theme& th = theme();
    TextStyle s;
    s.face = face;
    s.size = size;
    s.color = color;
    s.color_bottom = color_bottom;
    s.align = align;
    s.tracking = tracking;
    if (th.text_shadow.a > 0.0f) {
        // Headings (gradient text) get a deeper, wider shadow so they stand off the backdrop.
        const bool big = color_bottom.a > 0.0f;
        s.shadow = with_alpha(th.text_shadow, color.a * (big ? 1.2f : 1.0f));
        s.shadow_offset = Vec2{0.0f, 1.0f + size * (big ? 0.08f : 0.06f)};
        s.shadow_blur = 1.0f + size * (big ? 0.16f : 0.12f);
    }
    dl.text(Vec2{x, y}, text, s);
}

// ---- Button --------------------------------------------------------------
void Button::on_update(f32 dt) {
    const f32 target = !enabled ? 0.0f : (pressed_ ? 1.0f : (hovered_ ? 0.6f : 0.0f));
    hover_anim_ = approach(hover_anim_, target, dt);
}

void Button::on_draw(DrawList& dl) {
    const Theme& th = theme();
    const f32 h = glm::clamp(hover_anim_, 0.0f, 1.0f);
    const f32 sink = pressed_ ? 1.5f : 0.0f; // pressed buttons sit down into the board
    const Vec4 r{bounds.x, bounds.y + sink, bounds.w, bounds.h};

    // A primary action is a polished gold plaque; the rest are dark lacquered wood. Both
    // brighten on hover, and a gradient + top catch-light gives them some bevel.
    const Vec4 base = primary ? th.accent : th.button;
    const Vec4 hot = primary ? th.accent_hover : th.button_hover;
    Vec4 top = glm::mix(primary ? th.accent_hover : shade(th.button_hover, 1.1f),
                        primary ? shade(th.accent_hover, 1.12f) : shade(th.button_hover, 1.35f), h);
    Vec4 bottom = glm::mix(shade(base, primary ? 0.78f : 0.82f), shade(hot, 0.9f), h);
    if (pressed_) {
        std::swap(top, bottom); // lit from below = pushed in
        top = shade(top, 0.9f);
    }
    Vec4 edge = primary ? shade(th.accent, 0.55f) : with_alpha(th.accent, 0.30f + 0.45f * h);
    if (!enabled) {
        top = with_alpha(top, 0.4f);
        bottom = with_alpha(bottom, 0.4f);
        edge = with_alpha(edge, 0.4f);
    }
    if (enabled && !pressed_) {
        dl.shadow(r, th.radius, 6.0f, with_alpha(th.shadow, 0.9f), Vec2{0.0f, 3.0f});
    }
    if (primary && enabled && h > 0.01f) {
        dl.shadow(r, th.radius, 14.0f, with_alpha(th.accent_hover, 0.35f * h)); // a warm glow
    }
    dl.gradient(r, top, bottom, th.radius, edge, 1.5f);
    dl.line(Vec2{r.x + th.radius, r.y + 2.0f}, Vec2{r.x + r.z - th.radius, r.y + 2.0f}, 1.0f,
            Vec4{1.0f, 0.95f, 0.85f, (primary ? 0.45f : 0.10f) * (enabled ? 1.0f : 0.4f)});

    const Vec4 txt = primary ? th.accent_text : glm::mix(th.text, Vec4{1.0f}, h * 0.3f);
    TextStyle s = control_text(text_size, enabled ? txt : with_alpha(txt, 0.5f), TextAlign::Center);
    if (primary) {
        s.shadow = Vec4{1.0f, 0.9f, 0.6f, 0.35f}; // embossed into the gold, not floating over it
        s.shadow_offset = Vec2{0.0f, 1.0f};
        s.shadow_blur = 0.8f;
    }
    dl.text(Vec2{r.x + r.z * 0.5f, r.y + (r.w - text_size) * 0.5f}, label, s);
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

void Button::on_activate() {
    if (enabled && on_click) {
        on_click();
    }
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
    dl.text(Vec2{bounds.x, y}, label, control_text(text_size, th.text));

    // A recessed track that fills with gold as it switches on, and a raised knob.
    const Rect sw = switch_rect();
    const Vec4 on_top = th.accent_hover, on_bottom = shade(th.accent, 0.75f);
    dl.gradient(sw.xywh(), glm::mix(shade(th.track, 0.7f), on_bottom, anim_),
                glm::mix(th.track, on_top, anim_), sw.h * 0.5f, with_alpha(th.accent, 0.35f), 1.0f);

    const f32 pad = 3.0f;
    const f32 knob_d = sw.h - pad * 2.0f;
    const f32 kx = sw.x + pad + (sw.w - knob_d - pad * 2.0f) * anim_;
    const Vec4 knob{kx, sw.y + pad, knob_d, knob_d};
    dl.shadow(knob, knob_d * 0.5f, 3.0f, th.shadow, Vec2{0.0f, 1.5f});
    dl.gradient(knob, glm::mix(th.knob, Vec4{1.0f}, 0.35f), shade(th.knob, 0.8f), knob_d * 0.5f);
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

void Toggle::on_activate() {
    value = !value;
    if (on_change) {
        on_change(value);
    }
}
void Toggle::on_nav(int /*dir*/) { on_activate(); } // left or right just flips it

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
    dl.text(Vec2{bounds.x, bounds.y}, label, control_text(text_size, th.text));

    // Value read-out, right aligned.
    char buf[32];
    if (integer) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(value)));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value));
    }
    dl.text(Vec2{bounds.x + bounds.w, bounds.y}, buf, control_text(text_size, th.accent_hover, TextAlign::Right));

    // A recessed groove, filled with gold up to the value, and a raised gilded knob.
    const Rect tr = track_rect();
    dl.gradient(tr.xywh(), shade(th.track, 0.6f), th.track, tr.h * 0.5f, with_alpha(th.accent, 0.25f), 1.0f);
    const f32 fill_w = tr.w * norm();
    if (fill_w > 1.0f) {
        dl.gradient(Vec4{tr.x, tr.y, fill_w, tr.h}, th.accent_hover, shade(th.accent, 0.75f), tr.h * 0.5f);
    }

    const f32 knob_d = 18.0f;
    const f32 kx = tr.x + fill_w - knob_d * 0.5f;
    const Vec4 knob{kx, tr.y + tr.h * 0.5f - knob_d * 0.5f, knob_d, knob_d};
    dl.shadow(knob, knob_d * 0.5f, 3.0f, th.shadow, Vec2{0.0f, 1.5f});
    dl.gradient(knob, glm::mix(th.knob, Vec4{1.0f}, 0.35f), shade(th.knob, 0.78f), knob_d * 0.5f,
                shade(th.accent, 0.6f), 1.5f);
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

void Slider::on_nav(int dir) {
    const f32 span = max_value - min_value;
    const f32 stepv = integer ? 1.0f : span * 0.05f; // 5% of the range per press
    f32 v = glm::clamp(value + static_cast<f32>(dir) * stepv, min_value, max_value);
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
    dl.text(Vec2{bounds.x, y}, label, control_text(text_size, th.text));

    // The value sits in a recessed slot between two chevron buttons.
    const Rect la = left_arrow(), ra = right_arrow();
    const Vec4 slot{la.x + la.w + 4.0f, la.y, ra.x - la.x - la.w - 8.0f, la.h};
    dl.gradient(slot, Vec4{0.0f, 0.0f, 0.0f, 0.35f}, Vec4{0.0f, 0.0f, 0.0f, 0.18f}, th.radius * 0.5f,
                with_alpha(th.accent, 0.18f), 1.0f);
    auto arrow = [&](const Rect& r, int side) {
        const bool hot = hovered_ == side;
        dl.gradient(r.xywh(), hot ? shade(th.button_hover, 1.3f) : shade(th.button_hover, 1.05f),
                    hot ? th.button_hover : th.button, th.radius * 0.6f,
                    with_alpha(th.accent, hot ? 0.7f : 0.35f), 1.0f);
        const Vec2 c = r.center();
        const f32 s = r.h * 0.17f;
        const f32 dx = static_cast<f32>(side) * s * 0.6f;
        const Vec4 col = hot ? th.accent_hover : th.text;
        dl.line(Vec2{c.x - dx, c.y - s}, Vec2{c.x + dx, c.y}, 2.2f, col);
        dl.line(Vec2{c.x + dx, c.y}, Vec2{c.x - dx, c.y + s}, 2.2f, col);
    };
    arrow(la, -1);
    arrow(ra, 1);

    // Shrink a long value to fit the slot rather than letting it run under the arrows.
    const std::string value = options.empty() ? "" : options[index % options.size()];
    TextStyle s = control_text(text_size, th.text, TextAlign::Center);
    const f32 fit = slot.z - 10.0f;
    const f32 w = DrawList::text_width(value, s);
    if (w > fit && w > 0.0f) {
        s.size *= fit / w;
    }
    dl.text(Vec2{slot.x + slot.z * 0.5f, bounds.y + (bounds.h - s.size) * 0.5f}, value, s);
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

void Stepper::on_activate() { step(1); }   // advance one option
void Stepper::on_nav(int dir) { step(dir); }

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
        // Each swatch is a little glazed tile: lighter at the top, a dark rim, a catch-light.
        const Vec4 top = glm::mix(col, Vec4{1.0f}, 0.18f);
        const Vec4 bottom = shade(col, 0.78f);
        if (i == index) {
            // Selected: a warm glow and a gold ring standing off the tile.
            dl.shadow(r.xywh(), 7.0f, 8.0f, with_alpha(th.accent_hover, 0.55f));
            dl.outline(Vec4{r.x - 3.0f, r.y - 3.0f, r.w + 6.0f, r.h + 6.0f}, th.accent_hover, 2.5f, 9.0f);
            dl.gradient(r.xywh(), top, bottom, 7.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, 1.0f);
        } else {
            const bool hot = static_cast<int>(i) == hovered_;
            dl.shadow(r.xywh(), 7.0f, 3.0f, th.shadow, Vec2{0.0f, 1.5f});
            dl.gradient(r.xywh(), top, bottom, 7.0f,
                        hot ? Vec4{1.0f, 0.95f, 0.85f, 0.7f} : Vec4{0.0f, 0.0f, 0.0f, 0.5f},
                        hot ? 2.0f : 1.0f);
        }
        dl.line(Vec2{r.x + 6.0f, r.y + 3.0f}, Vec2{r.x + r.w - 6.0f, r.y + 3.0f}, 1.5f,
                Vec4{1.0f, 1.0f, 1.0f, 0.22f});
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

void SwatchRow::on_nav(int dir) {
    if (colors.empty()) {
        return;
    }
    const int n = static_cast<int>(colors.size());
    index = static_cast<usize>(((static_cast<int>(index) + dir) % n + n) % n);
    if (on_change) {
        on_change(index);
    }
}
void SwatchRow::on_activate() { on_nav(1); } // advance one swatch

// ---- TextField -----------------------------------------------------------
void TextField::on_update(f32 dt) { caret_blink_ = std::fmod(caret_blink_ + dt, 1.0f); }

void TextField::on_draw(DrawList& dl) {
    const Theme& th = theme();
    // A recessed inset well; it rims and glows gold while it has keyboard focus.
    const Vec4 border = focused ? th.accent_hover : th.panel_border;
    if (focused) {
        dl.shadow(bounds.xywh(), th.radius * 0.7f, 8.0f, with_alpha(th.accent_hover, 0.25f));
    }
    dl.gradient(bounds.xywh(), Vec4{0.0f, 0.0f, 0.0f, 0.55f}, Vec4{0.0f, 0.0f, 0.0f, 0.30f},
                th.radius * 0.7f, border, focused ? 2.0f : 1.5f);

    const f32 pad = 14.0f;
    const f32 ty = bounds.y + (bounds.h - text_size) * 0.5f;
    const bool empty = text.empty();
    const std::string& shown = empty ? placeholder : text;
    const TextStyle s = control_text(text_size, empty ? th.text_muted : th.text);
    dl.text(Vec2{bounds.x + pad, ty}, shown, s);

    if (focused && caret_blink_ < 0.5f) {
        const f32 cx = bounds.x + pad + DrawList::text_width(text, s) + 2.0f;
        dl.line(Vec2{cx, ty - 2.0f}, Vec2{cx, ty + text_size + 2.0f}, 2.0f, th.accent_hover);
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

// A / Enter toggles keyboard focus; the value itself is still typed on the keyboard (no on-screen
// keyboard), so this just lets a pad user arm the field before typing the host IP.
void TextField::on_activate() { focused = !focused; }

} // namespace alryn::ui
