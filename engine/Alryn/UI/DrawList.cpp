#include <Alryn/UI/DrawList.h>

#include <Alryn/Renderer/Renderer.h>
#include <Alryn/UI/Theme.h>

namespace alryn::ui {

void DrawList::rect(const Vec4& xywh, const Vec4& color, f32 radius) {
    renderer_.draw_ui_rect(xywh, color, radius);
}

void DrawList::rect(const Vec4& xywh, const Vec4& fill, const Vec4& border, f32 border_thickness,
                    f32 radius) {
    renderer_.draw_ui_rect(xywh, fill, radius, border_thickness, border);
}

void DrawList::gradient(const Vec4& xywh, const Vec4& top, const Vec4& bottom, f32 radius,
                        const Vec4& border, f32 border_thickness) {
    Renderer::UIPrim p;
    p.rect = xywh;
    p.color = top;
    p.color2 = bottom;
    p.params = Vec4{radius, 1.0f, 0.0f, border_thickness};
    p.border = border;
    renderer_.draw_ui(p);
}

void DrawList::shadow(const Vec4& xywh, f32 radius, f32 blur, const Vec4& color,
                      const Vec2& offset) {
    // The quad grows by the blur on every side; ui.frag keeps the shape at its nominal size
    // inside it and fades it out across the blur.
    const f32 b = std::max(blur, 1.0f);
    Renderer::UIPrim p;
    p.rect = Vec4{xywh.x + offset.x - b, xywh.y + offset.y - b, xywh.z + b * 2.0f, xywh.w + b * 2.0f};
    p.color = color;
    p.color2 = color;
    p.params = Vec4{radius + b * 0.5f, b, 0.0f, 0.0f};
    renderer_.draw_ui(p);
}

void DrawList::glow(const Vec4& xywh, const Vec4& inner, const Vec4& outer, f32 start) {
    Renderer::UIPrim p;
    p.rect = xywh;
    p.color = inner;
    p.color2 = outer;
    p.params = Vec4{start, 1.0f, 2.0f, 0.0f};
    renderer_.draw_ui(p);
}

void DrawList::outline(const Vec4& xywh, const Vec4& border, f32 thickness, f32 radius) {
    renderer_.draw_ui_rect(xywh, Vec4{border.r, border.g, border.b, 0.0f}, radius, thickness, border);
}

void DrawList::line(const Vec2& a, const Vec2& b, f32 thickness, const Vec4& color) {
    renderer_.draw_ui_segment(a, b, thickness, color);
}

f32 DrawList::text(const Vec2& pos, std::string_view str, f32 size, const Vec4& color,
                   TextAlign align) {
    const Theme& th = theme();
    TextStyle style;
    style.face = th.font;
    style.size = size;
    style.color = color;
    style.align = align;
    if (th.text_shadow.a > 0.0f) {
        // A soft shadow scaled to the text, so small HUD labels stay legible over the world.
        style.shadow = Vec4{th.text_shadow.r, th.text_shadow.g, th.text_shadow.b,
                            th.text_shadow.a * color.a};
        style.shadow_offset = Vec2{0.0f, 1.0f + size * 0.06f};
        style.shadow_blur = 1.0f + size * 0.12f;
    }
    return text(pos, str, style);
}

f32 DrawList::text_width(std::string_view str, const TextStyle& style) {
    const FontAtlas& fa = fonts();
    if (!fa.ready()) {
        return font_text_width(str, style.size);
    }
    return fa.text_width(style.face, str, style.size, style.tracking);
}

f32 DrawList::text(const Vec2& pos, std::string_view str, const TextStyle& style) {
    const f32 width = text_width(str, style);
    Vec2 origin = pos;
    if (style.align == TextAlign::Center) {
        origin.x -= width * 0.5f;
    } else if (style.align == TextAlign::Right) {
        origin.x -= width;
    }

    if (!fonts().ready()) {
        // No font atlas (headless / tests): the monoline vector font, colour only.
        const f32 thickness = style.size * kFontStrokeRatio;
        f32 pen_x = origin.x;
        for (char c : str) {
            const Glyph& glyph = font_glyph(c);
            for (const auto& stroke : glyph.strokes) {
                for (usize i = 0; i + 1 < stroke.size(); ++i) {
                    const Vec2 p0{pen_x + stroke[i].x * style.size, origin.y + stroke[i].y * style.size};
                    const Vec2 p1{pen_x + stroke[i + 1].x * style.size,
                                  origin.y + stroke[i + 1].y * style.size};
                    line(p0, p1, thickness, style.color);
                }
            }
            pen_x += (glyph.advance + kFontTracking + style.tracking) * style.size;
        }
        return width;
    }

    // The whole shadow run first, so no glyph's shadow falls across a neighbour's fill.
    if (style.shadow.a > 0.0f) {
        glyph_run(origin, str, style, style.shadow_offset, style.shadow, Vec4{0.0f},
                  style.outline.a > 0.0f ? style.shadow : Vec4{0.0f}, style.outline_width,
                  style.shadow_blur);
    }
    glyph_run(origin, str, style, Vec2{0.0f}, style.color, style.color_bottom, style.outline,
              style.outline_width, 0.0f);
    return width;
}

void DrawList::glyph_run(const Vec2& pos, std::string_view str, const TextStyle& style,
                         const Vec2& offset, const Vec4& color, const Vec4& color_bottom,
                         const Vec4& outline, f32 outline_width, f32 softness) {
    const FontAtlas& fa = fonts();
    const f32 k = style.size / fa.cap_height(style.face); // atlas px -> screen px
    const f32 baseline = pos.y + style.size + offset.y;
    f32 pen = pos.x + offset.x;
    Renderer::UIPrim p;
    p.color = color;
    p.color2 = color_bottom.a > 0.0f ? color_bottom : color;
    p.border = outline;
    // x: screen px per unit of the stored distance (the SDF spans +-kSpread atlas px over 0..1).
    p.extra = Vec4{2.0f * FontAtlas::kSpread * k, outline.a > 0.0f ? outline_width : 0.0f,
                   pos.y + offset.y, color_bottom.a > 0.0f ? style.size : 0.0f};
    p.params = Vec4{0.0f, softness, 3.0f, 0.0f};
    for (usize i = 0; i < str.size(); ++i) {
        const AtlasGlyph& g = fa.glyph(style.face, str[i]);
        if (g.visible) {
            p.rect = Vec4{pen + g.offset.x * k, baseline + g.offset.y * k, g.size.x * k, g.size.y * k};
            p.seg = g.uv;
            renderer_.draw_ui(p);
        }
        pen += g.advance * k;
        if (i + 1 < str.size()) {
            pen += fa.kern(style.face, str[i], str[i + 1]) * k + style.tracking * style.size;
        }
    }
}

} // namespace alryn::ui
