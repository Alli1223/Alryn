#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>

#include <string_view>
#include <vector>

namespace alryn::ui {

// A from-scratch monoline vector font: each glyph is a set of polyline strokes
// defined on a cap-height grid (x right, y down, cap box y in [0,1]). Strokes are
// drawn as rounded-cap capsules so the result is a clean geometric sans with no
// font assets. Lowercase maps to uppercase. Covers A-Z, 0-9 and common
// punctuation - enough for menus and IP-address entry.
struct Glyph {
    std::vector<std::vector<Vec2>> strokes; // polylines in cap-height units
    f32 advance = 0.62f;                    // horizontal advance (cap-height units)
};

// The glyph for a character (a blank glyph for unknown characters).
const Glyph& font_glyph(char c);

// Stroke thickness as a fraction of the font (cap-height) size.
constexpr f32 kFontStrokeRatio = 0.085f;
// Extra space added after each glyph's advance, in cap-height units.
constexpr f32 kFontTracking = 0.16f;

// Pixel width of a string rendered at the given cap-height size.
f32 font_text_width(std::string_view text, f32 size);

} // namespace alryn::ui
