#include <Alryn/UI/DrawList.h>

#include <Alryn/Renderer/Renderer.h>

namespace alryn::ui {

void DrawList::rect(const Vec4& xywh, const Vec4& color, f32 radius) {
    renderer_.draw_ui_rect(xywh, color, radius);
}

void DrawList::rect(const Vec4& xywh, const Vec4& fill, const Vec4& border, f32 border_thickness,
                    f32 radius) {
    renderer_.draw_ui_rect(xywh, fill, radius, border_thickness, border);
}

void DrawList::outline(const Vec4& xywh, const Vec4& border, f32 thickness, f32 radius) {
    renderer_.draw_ui_rect(xywh, Vec4{border.r, border.g, border.b, 0.0f}, radius, thickness, border);
}

void DrawList::line(const Vec2& a, const Vec2& b, f32 thickness, const Vec4& color) {
    renderer_.draw_ui_segment(a, b, thickness, color);
}

f32 DrawList::text(const Vec2& pos, std::string_view str, f32 size, const Vec4& color,
                   TextAlign align) {
    const f32 width = font_text_width(str, size);
    const f32 thickness = size * kFontStrokeRatio;

    f32 pen_x = pos.x;
    if (align == TextAlign::Center) {
        pen_x -= width * 0.5f;
    } else if (align == TextAlign::Right) {
        pen_x -= width;
    }

    for (char c : str) {
        const Glyph& glyph = font_glyph(c);
        for (const auto& stroke : glyph.strokes) {
            for (usize i = 0; i + 1 < stroke.size(); ++i) {
                const Vec2 p0{pen_x + stroke[i].x * size, pos.y + stroke[i].y * size};
                const Vec2 p1{pen_x + stroke[i + 1].x * size, pos.y + stroke[i + 1].y * size};
                line(p0, p1, thickness, color);
            }
        }
        pen_x += (glyph.advance + kFontTracking) * size;
    }
    return width;
}

} // namespace alryn::ui
