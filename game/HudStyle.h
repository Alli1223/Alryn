#pragma once

// The in-game HUD's shared look: a small kit of drawing helpers (legible outlined text, dark-wood
// plaques, framed gauges, key-cap hints, coins, medallions, banners and the big overlay windows) so
// every HUD element - and the map / skills / wardrobe overlays - reads as one illuminated-
// manuscript style with the menus. Everything is drawn through ui::DrawList in window pixels.

#include <Alryn/UI/UI.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace alryn::game::hud {

using ui::DrawList;
using ui::FontFace;
using ui::TextAlign;
using ui::TextStyle;

// Status colours (the theme supplies the parchment text + gold accents).
inline const Vec4 kGold{1.0f, 0.85f, 0.45f, 1.0f};
inline const Vec4 kGood{0.58f, 0.88f, 0.48f, 1.0f};
inline const Vec4 kWarn{0.99f, 0.71f, 0.33f, 1.0f};
inline const Vec4 kBad{0.97f, 0.40f, 0.32f, 1.0f};
inline const Vec4 kInk{0.07f, 0.045f, 0.025f, 1.0f};

inline Vec4 alpha(const Vec4& c, f32 a) { return Vec4{c.r, c.g, c.b, c.a * a}; }
inline Vec4 shade(const Vec4& c, f32 k) { return Vec4{c.r * k, c.g * k, c.b * k, c.a}; }

// Bold text with a dark ink outline + soft shadow: stays legible over bright grass, snow or night.
inline TextStyle style(f32 size, const Vec4& color, TextAlign align = TextAlign::Left,
                       FontFace face = FontFace::Bold) {
    TextStyle s;
    s.face = face;
    s.size = size;
    s.color = color;
    s.align = align;
    s.tracking = 0.03f;
    s.outline = alpha(kInk, 0.85f * color.a);
    s.outline_width = std::max(1.0f, size * 0.09f);
    s.shadow = Vec4{0.0f, 0.0f, 0.0f, 0.45f * color.a};
    s.shadow_offset = Vec2{0.0f, 1.0f + size * 0.08f};
    s.shadow_blur = 1.0f + size * 0.12f;
    return s;
}
inline f32 text(DrawList& d, const Vec2& pos, std::string_view s, f32 size, const Vec4& color,
                TextAlign align = TextAlign::Left, FontFace face = FontFace::Bold) {
    return d.text(pos, s, style(size, color, align, face));
}
inline f32 width(std::string_view s, f32 size, FontFace face = FontFace::Bold) {
    return DrawList::text_width(s, style(size, Vec4{1.0f}, TextAlign::Left, face));
}
// A heading in the display face with the theme's gold gradient.
inline f32 heading(DrawList& d, const Vec2& pos, std::string_view s, f32 size,
                   TextAlign align = TextAlign::Left) {
    const ui::Theme& th = ui::theme();
    TextStyle st = style(size, th.title, align, FontFace::Display);
    st.color_bottom = th.title_bottom;
    st.tracking = 0.05f;
    return d.text(pos, s, st);
}
inline f32 heading_width(std::string_view s, f32 size) {
    TextStyle st = style(size, Vec4{1.0f}, TextAlign::Left, FontFace::Display);
    st.tracking = 0.05f;
    return DrawList::text_width(s, st);
}

// Greedy word-wrap of `s` into lines no wider than `max_w` in the theme's default text style at
// cap height `size`. At most `max_lines`; the last is cut with an ellipsis if the text runs over.
inline std::vector<std::string> wrap(std::string_view s, f32 size, f32 max_w, int max_lines = 2) {
    TextStyle st;
    st.face = ui::theme().font;
    st.size = size;
    auto fits = [&](std::string_view t) { return DrawList::text_width(t, st) <= max_w; };
    std::vector<std::string> lines;
    std::string line;
    usize i = 0;
    while (i < s.size()) {
        const usize sp = s.find(' ', i);
        const std::string_view word = s.substr(i, sp == std::string_view::npos ? s.size() - i : sp - i);
        i = sp == std::string_view::npos ? s.size() : sp + 1;
        if (word.empty()) {
            continue;
        }
        const std::string candidate = line.empty() ? std::string{word} : line + " " + std::string{word};
        if (fits(candidate) || line.empty()) {
            line = candidate;
            continue;
        }
        lines.push_back(line);
        line = std::string{word};
        if (static_cast<int>(lines.size()) == max_lines) {
            line.clear();
            std::string& last = lines.back();
            while (!last.empty() && !fits(last + "...")) {
                last.pop_back();
            }
            last += "...";
            return lines;
        }
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

// A compact HUD plaque: a soft drop shadow, a dark-wood gradient and a gold hairline border.
inline void plaque(DrawList& d, const Vec4& r, f32 opacity = 0.84f, f32 radius = 9.0f) {
    const ui::Theme& th = ui::theme();
    d.shadow(r, radius, 12.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f * opacity}, Vec2{0.0f, 4.0f});
    d.gradient(r, Vec4{th.panel.r, th.panel.g, th.panel.b, opacity},
               Vec4{th.panel_bottom.r, th.panel_bottom.g, th.panel_bottom.b, opacity}, radius,
               alpha(th.accent, 0.55f), 1.25f);
    d.line(Vec2{r.x + radius, r.y + 1.5f}, Vec2{r.x + r.z - radius, r.y + 1.5f}, 1.0f,
           Vec4{1.0f, 0.92f, 0.75f, 0.08f * opacity});
}

// A gilded rivet / stud centred at `c` (window corners, medallion rims, banner ends).
inline void stud(DrawList& d, const Vec2& c, f32 r) {
    const ui::Theme& th = ui::theme();
    d.rect(Vec4{c.x - r - 1.0f, c.y - r - 1.0f, (r + 1.0f) * 2.0f, (r + 1.0f) * 2.0f},
           Vec4{0.0f, 0.0f, 0.0f, 0.55f}, r + 1.0f);
    d.gradient(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, th.accent_hover, shade(th.accent, 0.7f), r);
    const f32 hr = r * 0.38f;
    d.rect(Vec4{c.x - r * 0.45f - hr, c.y - r * 0.45f - hr, hr * 2.0f, hr * 2.0f},
           Vec4{1.0f, 0.96f, 0.85f, 0.75f}, hr);
}

// A framed gauge: a recessed groove, a gradient fill up to `frac` (with a glossy top band), a
// faint lighter "ghost" up to `ghost` (recent damage draining away), tick marks and a gold rim.
inline void bar(DrawList& d, const Vec4& r, f32 frac, const Vec4& top, const Vec4& bottom,
                int ticks = 0, f32 ghost = -1.0f) {
    const ui::Theme& th = ui::theme();
    const f32 rad = std::min(r.w * 0.5f, 6.0f);
    frac = std::clamp(frac, 0.0f, 1.0f);
    d.shadow(r, rad, 4.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 1.5f});
    d.gradient(r, Vec4{0.02f, 0.015f, 0.01f, 0.92f}, Vec4{0.08f, 0.06f, 0.04f, 0.92f}, rad);
    if (ghost > frac) {
        d.rect(Vec4{r.x, r.y, r.z * std::min(ghost, 1.0f), r.w}, Vec4{1.0f, 0.93f, 0.8f, 0.45f}, rad);
    }
    if (frac > 0.0f) {
        const Vec4 fill{r.x, r.y, std::max(r.z * frac, rad * 2.0f), r.w};
        d.gradient(fill, top, bottom, rad);
        d.rect(Vec4{fill.x + 2.0f, fill.y + 1.5f, std::max(fill.z - 4.0f, 0.0f), fill.w * 0.32f},
               Vec4{1.0f, 1.0f, 1.0f, 0.22f}, rad * 0.6f);
    }
    for (int i = 1; i < ticks; ++i) {
        const f32 tx = r.x + r.z * static_cast<f32>(i) / static_cast<f32>(ticks);
        d.line(Vec2{tx, r.y + 2.0f}, Vec2{tx, r.y + r.w - 2.0f}, 1.0f, Vec4{0.0f, 0.0f, 0.0f, 0.35f});
    }
    d.outline(r, alpha(th.accent, 0.7f), 1.25f, rad);
}

// A small parchment key-cap reading `key`, top-left at `pos`, sized for text of cap `size`.
// Returns its width.
inline f32 key_cap(DrawList& d, const Vec2& pos, std::string_view key, f32 size) {
    const f32 ks = size * 0.78f;
    const f32 h = size * 1.5f;
    const f32 w = std::max(h, width(key, ks) + size * 0.7f);
    const Vec4 r{pos.x, pos.y - size * 0.25f, w, h};
    d.rect(Vec4{r.x, r.y + 2.0f, r.z, r.w}, Vec4{0.12f, 0.08f, 0.04f, 0.9f}, size * 0.3f); // key side
    d.gradient(r, Vec4{0.97f, 0.91f, 0.76f, 1.0f}, Vec4{0.78f, 0.68f, 0.50f, 1.0f}, size * 0.3f,
               Vec4{0.25f, 0.17f, 0.08f, 1.0f}, 1.0f);
    TextStyle s = style(ks, kInk, TextAlign::Center);
    s.outline = Vec4{0.0f};
    s.shadow = Vec4{1.0f, 1.0f, 1.0f, 0.4f};
    s.shadow_offset = Vec2{0.0f, 1.0f};
    s.shadow_blur = 0.6f;
    d.text(Vec2{r.x + w * 0.5f, r.y + (h - ks) * 0.5f - 1.0f}, key, s);
    return w;
}

// Text in which every "[KEY]" token is drawn as a key-cap (e.g. "[E] RIDE THE WAGON"). Returns
// the width; rich_width measures without drawing.
inline f32 rich(DrawList* d, const Vec2& pos, std::string_view s, f32 size, const Vec4& color) {
    f32 x = pos.x;
    usize i = 0;
    while (i < s.size()) {
        const usize open = s.find('[', i);
        const usize close = open == std::string_view::npos ? open : s.find(']', open);
        const usize run_end = close == std::string_view::npos ? s.size() : open;
        if (run_end > i) {
            const std::string_view run = s.substr(i, run_end - i);
            x += d != nullptr ? text(*d, Vec2{x, pos.y}, run, size, color) : width(run, size);
        }
        if (close == std::string_view::npos) {
            break;
        }
        const std::string_view key = s.substr(open + 1, close - open - 1);
        if (d != nullptr) {
            x += key_cap(*d, Vec2{x, pos.y}, key, size);
        } else {
            x += std::max(size * 1.5f, width(key, size * 0.78f) + size * 0.7f);
        }
        x += size * 0.35f;
        i = close + 1;
        while (i < s.size() && s[i] == ' ') {
            ++i; // the cap's own gap replaces the space after it
        }
    }
    return x - pos.x;
}
inline f32 rich(DrawList& d, const Vec2& pos, std::string_view s, f32 size, const Vec4& color) {
    return rich(&d, pos, s, size, color);
}
inline f32 rich_width(std::string_view s, f32 size) { return rich(nullptr, Vec2{0.0f}, s, size, Vec4{1.0f}); }

// A gold coin of radius r centred at `c`.
inline void coin(DrawList& d, const Vec2& c, f32 r) {
    d.shadow(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, r, 3.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f},
             Vec2{0.0f, 1.5f});
    d.gradient(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, Vec4{1.0f, 0.88f, 0.48f, 1.0f},
               Vec4{0.78f, 0.52f, 0.16f, 1.0f}, r, Vec4{0.45f, 0.28f, 0.06f, 1.0f}, 1.25f);
    const f32 ir = r * 0.66f;
    d.outline(Vec4{c.x - ir, c.y - ir, ir * 2.0f, ir * 2.0f}, Vec4{0.55f, 0.35f, 0.08f, 0.7f}, 1.0f, ir);
    d.rect(Vec4{c.x - r * 0.55f, c.y - r * 0.62f, r * 0.5f, r * 0.34f}, Vec4{1.0f, 0.98f, 0.88f, 0.6f},
           r * 0.17f);
}

// A small pill-shaped status chip (coloured text on dark wood) at `pos`; returns its width.
inline f32 chip(DrawList& d, const Vec2& pos, std::string_view s, f32 size, const Vec4& color) {
    const f32 w = width(s, size) + size * 1.2f;
    const Vec4 r{pos.x, pos.y, w, size * 1.7f};
    d.gradient(r, Vec4{0.12f, 0.085f, 0.055f, 0.88f}, Vec4{0.07f, 0.05f, 0.03f, 0.88f}, r.w * 0.5f,
               alpha(color, 0.55f), 1.0f);
    text(d, Vec2{r.x + w * 0.5f, r.y + (r.w - size) * 0.5f}, s, size, color, TextAlign::Center);
    return w;
}

// A round medallion frame (minimap, compass): a dark disc in a double gold ring with studs at the
// cardinal points. `fill` is the disc colour.
inline void medallion(DrawList& d, const Vec2& c, f32 r, const Vec4& fill) {
    const ui::Theme& th = ui::theme();
    const Vec4 outer{c.x - r - 5.0f, c.y - r - 5.0f, (r + 5.0f) * 2.0f, (r + 5.0f) * 2.0f};
    d.shadow(outer, r + 5.0f, 12.0f, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, Vec2{0.0f, 4.0f});
    d.gradient(outer, th.accent_hover, shade(th.accent, 0.6f), r + 5.0f);
    d.rect(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, fill, r);
    d.outline(Vec4{c.x - r, c.y - r, r * 2.0f, r * 2.0f}, Vec4{0.1f, 0.06f, 0.02f, 0.9f}, 1.5f, r);
    const f32 sr = std::max(2.5f, r * 0.045f);
    for (const Vec2& o : {Vec2{0.0f, -1.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{-1.0f, 0.0f}}) {
        stud(d, c + o * (r + 2.5f), sr);
    }
}

// A banner across the screen: a dark ribbon fading out at both ends, gold rules above and below,
// studs, and the message in the display face. `color` tints the message (gold for news, red for
// danger, green for success).
inline void banner(DrawList& d, f32 cx, f32 y, std::string_view msg, f32 size, const Vec4& color) {
    const f32 tw = heading_width(msg, size);
    const f32 w = tw + size * 4.0f;
    const f32 h = size * 1.9f;
    const Vec4 band{cx - w * 0.5f, y, w, h};
    d.glow(Vec4{band.x - w * 0.1f, band.y - h * 0.6f, w * 1.2f, h * 2.2f}, Vec4{0.0f, 0.0f, 0.0f, 0.55f},
           Vec4{0.0f, 0.0f, 0.0f, 0.0f});
    d.gradient(band, Vec4{0.12f, 0.08f, 0.05f, 0.86f}, Vec4{0.06f, 0.04f, 0.025f, 0.86f}, h * 0.15f);
    const Vec4 rule = alpha(color, 0.85f);
    d.line(Vec2{band.x + h * 0.4f, band.y + 3.0f}, Vec2{band.x + w - h * 0.4f, band.y + 3.0f}, 1.5f, rule);
    d.line(Vec2{band.x + h * 0.4f, band.y + h - 3.0f}, Vec2{band.x + w - h * 0.4f, band.y + h - 3.0f}, 1.5f,
           rule);
    stud(d, Vec2{band.x + h * 0.4f, band.y + h * 0.5f}, size * 0.12f);
    stud(d, Vec2{band.x + w - h * 0.4f, band.y + h * 0.5f}, size * 0.12f);
    TextStyle st = style(size, glm::mix(color, Vec4{1.0f}, 0.25f), TextAlign::Center, FontFace::Display);
    st.color_bottom = shade(color, 0.8f);
    st.tracking = 0.06f;
    d.text(Vec2{cx, band.y + (h - size) * 0.5f}, msg, st);
}

// A full overlay window (map / skills / wardrobe): the world dimmed behind, a studded gold-bound
// board, the title in gold display capitals on a rule. Returns the content top (below the title).
inline f32 window(DrawList& d, f32 W, f32 H, const Vec4& panel, std::string_view title) {
    const ui::Theme& th = ui::theme();
    d.rect(Vec4{0.0f, 0.0f, W, H}, Vec4{0.02f, 0.015f, 0.01f, 0.78f});
    d.glow(Vec4{-W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f}, Vec4{0.0f}, Vec4{0.0f, 0.0f, 0.0f, 0.5f}, 0.35f);
    d.shadow(panel, 14.0f, 28.0f, Vec4{0.0f, 0.0f, 0.0f, 0.7f}, Vec2{0.0f, 10.0f});
    d.gradient(panel, th.panel, th.panel_bottom, 14.0f, th.panel_border, 2.0f);
    const Vec4 inner{panel.x + 7.0f, panel.y + 7.0f, panel.z - 14.0f, panel.w - 14.0f};
    d.outline(inner, alpha(th.accent, 0.35f), 1.0f, 9.0f);
    for (const Vec2& c : {Vec2{inner.x, inner.y}, Vec2{inner.x + inner.z, inner.y},
                          Vec2{inner.x + inner.z, inner.y + inner.w}, Vec2{inner.x, inner.y + inner.w}}) {
        stud(d, c, 4.0f);
    }
    const f32 ts = std::clamp(panel.w * 0.055f, 22.0f, 38.0f);
    const f32 ty = panel.y + 22.0f;
    heading(d, Vec2{panel.x + 34.0f, ty}, title, ts);
    const f32 ry = ty + ts + 14.0f;
    d.line(Vec2{panel.x + 30.0f, ry}, Vec2{panel.x + panel.z - 30.0f, ry}, 1.25f, alpha(th.accent, 0.5f));
    return ry + 14.0f;
}

} // namespace alryn::game::hud
