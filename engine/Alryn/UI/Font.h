#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <array>
#include <string_view>
#include <vector>

namespace alryn {
class Renderer;
}

namespace alryn::ui {

// The UI's typefaces, compiled into the engine (see engine/assets/fonts):
//   Body     Alegreya Sans Medium     - running text, labels, numbers
//   Bold     Alegreya Sans ExtraBold  - buttons, HUD readouts, emphasis
//   Display  Cinzel Bold              - titles and headings (Roman inscriptional capitals)
enum class FontFace : u8 { Body, Bold, Display };
inline constexpr usize kFontFaceCount = 3;

// One glyph in the atlas. Positions are atlas pixels relative to the pen on the baseline.
struct AtlasGlyph {
    Vec4 uv{0.0f};      // u0, v0, u1, v1
    Vec2 offset{0.0f};  // quad top-left from the pen (y down)
    Vec2 size{0.0f};    // quad size
    f32 advance = 0.0f; // pen advance
    bool visible = false;
};

// A signed-distance-field atlas of the printable ASCII range for every face, baked from the
// embedded TrueType fonts. SDF texels let one atlas draw crisp text at any size, plus cheap
// outlines and soft shadows (see ui.frag). Lowercase draws as uppercase, matching the UI's
// all-caps style (and the vector font it replaced).
class FontAtlas {
public:
    // Parses the fonts and bakes the atlas; later calls are no-ops. False if a font failed.
    bool build();
    bool ready() const { return ready_; }

    const AtlasGlyph& glyph(FontFace face, char c) const;
    f32 kern(FontFace face, char a, char b) const; // extra advance between a pair (atlas px)
    f32 cap_height(FontFace face) const { return faces_[static_cast<usize>(face)].cap; }
    // Atlas px from a glyph's edge to where its distance field saturates (each side).
    static constexpr f32 kSpread = 8.0f;

    // Pixel width of `text` drawn at cap height `size`, with `tracking` extra space after each
    // glyph in cap heights.
    f32 text_width(FontFace face, std::string_view text, f32 size, f32 tracking = 0.0f) const;

    u32 width() const { return width_; }
    u32 height() const { return height_; }
    const std::vector<u8>& texels() const { return texels_; }

private:
    static constexpr int kFirst = 32, kLast = 126, kCount = kLast - kFirst + 1;
    struct Face {
        std::array<AtlasGlyph, kCount> glyphs{};
        std::vector<f32> kerning; // kCount x kCount
        f32 cap = 1.0f;
    };
    static int index_of(char c);

    std::array<Face, kFontFaceCount> faces_{};
    std::vector<u8> texels_;
    u32 width_ = 0;
    u32 height_ = 0;
    bool ready_ = false;
    bool tried_ = false;
};

// The process-wide UI font atlas.
FontAtlas& fonts();

// Bakes the font atlas (if needed) and uploads it to the renderer for the UI to draw with.
// Call once after the renderer is up. Until then (or if it fails) text uses the vector font.
bool load_fonts(Renderer& renderer);

} // namespace alryn::ui
