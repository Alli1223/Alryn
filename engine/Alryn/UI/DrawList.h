#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/UI/Font.h>
#include <Alryn/UI/VectorFont.h>

#include <string_view>

namespace alryn {
class Renderer;
}

namespace alryn::ui {

enum class TextAlign { Left, Center, Right };

// How a run of text is drawn. Lengths are px; `size` is the cap height.
struct TextStyle {
    FontFace face = FontFace::Body;
    f32 size = 16.0f;
    Vec4 color{1.0f};
    Vec4 color_bottom{0.0f};       // a > 0: a vertical gradient from `color` (top) to this
    Vec4 outline{0.0f};            // a > 0: an outline in this colour...
    f32 outline_width = 0.0f;      // ...this thick
    Vec4 shadow{0.0f};             // a > 0: a soft drop shadow in this colour
    Vec2 shadow_offset{0.0f, 2.0f};
    f32 shadow_blur = 2.0f;
    f32 tracking = 0.0f;           // extra letter spacing, in cap heights
    TextAlign align = TextAlign::Left;
};

// Thin pixel-space 2D drawing helper over the Renderer's UI primitives. All
// coordinates are in window pixels (origin top-left). Used by widgets in their
// draw() to emit rounded rectangles, gradients, shadows, glows, lines and text.
class DrawList {
public:
    explicit DrawList(Renderer& renderer) : renderer_(renderer) {}

    // Filled rounded rectangle (radius/x/y/w/h in px).
    void rect(const Vec4& xywh, const Vec4& color, f32 radius = 0.0f);
    // Rounded rectangle with a fill and a border of the given thickness.
    void rect(const Vec4& xywh, const Vec4& fill, const Vec4& border, f32 border_thickness,
              f32 radius);
    // Rounded rectangle filled with a vertical gradient (top -> bottom), optionally bordered.
    void gradient(const Vec4& xywh, const Vec4& top, const Vec4& bottom, f32 radius = 0.0f,
                  const Vec4& border = Vec4{0.0f}, f32 border_thickness = 0.0f);
    // A soft drop shadow (or, in a bright colour, a glow) under the rounded rect `xywh`:
    // `blur` px of falloff around it, shifted by `offset`.
    void shadow(const Vec4& xywh, f32 radius, f32 blur, const Vec4& color,
                const Vec2& offset = Vec2{0.0f});
    // A radial glow filling the ellipse of `xywh`: `inner` at the centre (and out to `start`, as a
    // fraction of the ellipse's radius) fading to `outer` at the rim.
    void glow(const Vec4& xywh, const Vec4& inner, const Vec4& outer = Vec4{0.0f}, f32 start = 0.0f);
    // Border-only rounded rectangle.
    void outline(const Vec4& xywh, const Vec4& border, f32 thickness, f32 radius = 0.0f);
    // Rounded-cap line.
    void line(const Vec2& a, const Vec2& b, f32 thickness, const Vec4& color);

    // Text in the theme's default style (face + soft shadow) at cap height `size` px. `pos` is
    // the top-left of the cap box (before alignment). Returns the text's pixel width.
    f32 text(const Vec2& pos, std::string_view str, f32 size, const Vec4& color,
             TextAlign align = TextAlign::Left);
    // Text in an explicit style.
    f32 text(const Vec2& pos, std::string_view str, const TextStyle& style);

    f32 text_width(std::string_view str, f32 size) const { return font_text_width(str, size); }
    static f32 text_width(std::string_view str, const TextStyle& style);

    Renderer& renderer() { return renderer_; }

private:
    void glyph_run(const Vec2& pos, std::string_view str, const TextStyle& style, const Vec2& offset,
                   const Vec4& color, const Vec4& color_bottom, const Vec4& outline,
                   f32 outline_width, f32 softness);
    Renderer& renderer_;
};

} // namespace alryn::ui
