#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/UI/VectorFont.h>

#include <string_view>

namespace alryn {
class Renderer;
}

namespace alryn::ui {

enum class TextAlign { Left, Center, Right };

// Thin pixel-space 2D drawing helper over the Renderer's UI primitives. All
// coordinates are in window pixels (origin top-left). Used by widgets in their
// draw() to emit rounded rectangles, lines and vector-font text.
class DrawList {
public:
    explicit DrawList(Renderer& renderer) : renderer_(renderer) {}

    // Filled rounded rectangle (radius/x/y/w/h in px).
    void rect(const Vec4& xywh, const Vec4& color, f32 radius = 0.0f);
    // Rounded rectangle with a fill and a border of the given thickness.
    void rect(const Vec4& xywh, const Vec4& fill, const Vec4& border, f32 border_thickness,
              f32 radius);
    // Border-only rounded rectangle.
    void outline(const Vec4& xywh, const Vec4& border, f32 thickness, f32 radius = 0.0f);
    // Rounded-cap line.
    void line(const Vec2& a, const Vec2& b, f32 thickness, const Vec4& color);

    // Vector-font text. `size` is the cap height in px; `pos` is the top-left of
    // the cap box (before alignment). Returns the text's pixel width.
    f32 text(const Vec2& pos, std::string_view str, f32 size, const Vec4& color,
             TextAlign align = TextAlign::Left);

    f32 text_width(std::string_view str, f32 size) const { return font_text_width(str, size); }

private:
    Renderer& renderer_;
};

} // namespace alryn::ui
