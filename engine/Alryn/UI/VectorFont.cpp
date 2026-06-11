#include <Alryn/UI/VectorFont.h>

#include <cctype>
#include <unordered_map>

namespace alryn::ui {

namespace {

using Stroke = std::vector<Vec2>;
using Strokes = std::vector<Stroke>;

// Smooths a polyline into many short segments so curved letters read as crisp
// curves rather than faceted polygons. Uses corner-preserving Catmull-Rom: gentle
// bends (the bowl of an O/C/S) are interpolated, but sharp corners (the apex of an
// A, the corners of an E) keep a one-sided tangent so they stay perfectly sharp.
// Closed loops (O, 0, 8) wrap their tangents so the seam is smooth too.
Stroke smooth_stroke(const Stroke& p) {
    const usize n = p.size();
    if (n < 3) {
        return p; // straight lines, dots and dashes need no smoothing
    }
    const bool closed = glm::length(p.front() - p.back()) < 1e-4f;
    Stroke pts = p;
    if (closed) {
        pts.pop_back(); // drop the duplicated closing point; we wrap instead
    }
    const long m = static_cast<long>(pts.size());

    auto at = [&](long i) -> Vec2 {
        if (closed) {
            return pts[static_cast<usize>((i % m + m) % m)];
        }
        return pts[static_cast<usize>(glm::clamp<long>(i, 0, m - 1))];
    };
    auto is_corner = [&](long i) -> bool {
        if (!closed && (i == 0 || i == m - 1)) {
            return true; // open-stroke endpoints: straight in/out
        }
        const Vec2 a = at(i) - at(i - 1);
        const Vec2 b = at(i + 1) - at(i);
        const f32 la = glm::length(a);
        const f32 lb = glm::length(b);
        if (la < 1e-5f || lb < 1e-5f) {
            return true;
        }
        return glm::dot(a / la, b / lb) < 0.35f; // turn sharper than ~70 deg
    };

    constexpr int kSub = 6;
    Stroke out;
    const long seg_count = closed ? m : m - 1;
    for (long s = 0; s < seg_count; ++s) {
        const Vec2 p1 = at(s);
        const Vec2 p2 = at(s + 1);
        // A segment between two corners is straight - emit it as a single point so
        // it stays one clean capsule (subdividing straight runs into overlapping
        // capsules is what caused edge grain). Only curved segments are tessellated.
        if (is_corner(s) && is_corner(s + 1)) {
            out.push_back(p1);
            continue;
        }
        const Vec2 m1 = is_corner(s) ? (p2 - p1) : 0.5f * (at(s + 1) - at(s - 1));
        const Vec2 m2 = is_corner(s + 1) ? (p2 - p1) : 0.5f * (at(s + 2) - at(s));
        for (int k = 0; k < kSub; ++k) {
            const f32 t = static_cast<f32>(k) / static_cast<f32>(kSub);
            const f32 t2 = t * t;
            const f32 t3 = t2 * t;
            out.push_back((2.0f * t3 - 3.0f * t2 + 1.0f) * p1 + (t3 - 2.0f * t2 + t) * m1 +
                          (-2.0f * t3 + 3.0f * t2) * p2 + (t3 - t2) * m2);
        }
    }
    out.push_back(at(seg_count));
    return out;
}

// Build the glyph table once. Coordinates are in cap-height units: x grows
// right, y grows down, the cap box spans y in [0,1] and x roughly [0,0.62].
const std::unordered_map<char, Glyph>& table() {
    static const std::unordered_map<char, Glyph> glyphs = [] {
        std::unordered_map<char, Glyph> g;
        auto add = [&](char c, Strokes s, f32 advance = 0.62f) {
            for (Stroke& stroke : s) {
                stroke = smooth_stroke(stroke);
            }
            g.emplace(c, Glyph{std::move(s), advance});
        };

        add(' ', {}, 0.42f);

        // ---- Letters ----
        add('A', {{{0.0f, 1.0f}, {0.31f, 0.0f}, {0.62f, 1.0f}}, {{0.13f, 0.62f}, {0.49f, 0.62f}}});
        add('B', {{{0.0f, 0.0f}, {0.0f, 1.0f}},
                  {{0.0f, 0.0f}, {0.42f, 0.0f}, {0.56f, 0.14f}, {0.42f, 0.5f}, {0.0f, 0.5f}},
                  {{0.0f, 0.5f}, {0.48f, 0.5f}, {0.62f, 0.66f}, {0.46f, 1.0f}, {0.0f, 1.0f}}});
        add('C', {{{0.62f, 0.18f}, {0.42f, 0.0f}, {0.16f, 0.06f}, {0.0f, 0.5f}, {0.16f, 0.94f},
                   {0.42f, 1.0f}, {0.62f, 0.82f}}});
        add('D', {{{0.0f, 0.0f}, {0.0f, 1.0f}},
                  {{0.0f, 0.0f}, {0.36f, 0.0f}, {0.6f, 0.28f}, {0.6f, 0.72f}, {0.36f, 1.0f},
                   {0.0f, 1.0f}}});
        add('E', {{{0.62f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}, {0.6f, 1.0f}}, {{0.0f, 0.5f}, {0.46f, 0.5f}}});
        add('F', {{{0.62f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}}, {{0.0f, 0.5f}, {0.44f, 0.5f}}});
        add('G', {{{0.62f, 0.18f}, {0.42f, 0.0f}, {0.16f, 0.06f}, {0.0f, 0.5f}, {0.16f, 0.94f},
                   {0.42f, 1.0f}, {0.62f, 0.84f}, {0.62f, 0.58f}, {0.36f, 0.58f}}});
        add('H', {{{0.0f, 0.0f}, {0.0f, 1.0f}}, {{0.62f, 0.0f}, {0.62f, 1.0f}}, {{0.0f, 0.5f}, {0.62f, 0.5f}}});
        add('I', {{{0.31f, 0.0f}, {0.31f, 1.0f}}, {{0.12f, 0.0f}, {0.5f, 0.0f}}, {{0.12f, 1.0f}, {0.5f, 1.0f}}},
            0.34f);
        add('J', {{{0.52f, 0.0f}, {0.52f, 0.78f}, {0.38f, 0.98f}, {0.16f, 1.0f}, {0.02f, 0.82f},
                   {0.0f, 0.7f}}});
        add('K', {{{0.0f, 0.0f}, {0.0f, 1.0f}}, {{0.58f, 0.0f}, {0.0f, 0.56f}}, {{0.16f, 0.42f}, {0.62f, 1.0f}}});
        add('L', {{{0.0f, 0.0f}, {0.0f, 1.0f}, {0.58f, 1.0f}}});
        add('M', {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.31f, 0.55f}, {0.62f, 0.0f}, {0.62f, 1.0f}}}, 0.7f);
        add('N', {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.62f, 1.0f}, {0.62f, 0.0f}}});
        add('O', {{{0.31f, 0.0f}, {0.55f, 0.12f}, {0.62f, 0.5f}, {0.55f, 0.88f}, {0.31f, 1.0f},
                   {0.07f, 0.88f}, {0.0f, 0.5f}, {0.07f, 0.12f}, {0.31f, 0.0f}}});
        add('P', {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.42f, 0.0f}, {0.58f, 0.18f}, {0.42f, 0.52f}, {0.0f, 0.52f}}});
        add('Q', {{{0.31f, 0.0f}, {0.55f, 0.12f}, {0.62f, 0.5f}, {0.55f, 0.88f}, {0.31f, 1.0f},
                   {0.07f, 0.88f}, {0.0f, 0.5f}, {0.07f, 0.12f}, {0.31f, 0.0f}},
                  {{0.4f, 0.72f}, {0.64f, 1.04f}}});
        add('R', {{{0.0f, 1.0f}, {0.0f, 0.0f}, {0.42f, 0.0f}, {0.58f, 0.18f}, {0.42f, 0.52f}, {0.0f, 0.52f}},
                  {{0.3f, 0.52f}, {0.62f, 1.0f}}});
        add('S', {{{0.6f, 0.16f}, {0.42f, 0.0f}, {0.16f, 0.02f}, {0.04f, 0.2f}, {0.18f, 0.42f},
                   {0.46f, 0.54f}, {0.58f, 0.72f}, {0.46f, 0.98f}, {0.18f, 1.0f}, {0.02f, 0.84f}}});
        add('T', {{{0.0f, 0.0f}, {0.62f, 0.0f}}, {{0.31f, 0.0f}, {0.31f, 1.0f}}});
        add('U', {{{0.0f, 0.0f}, {0.0f, 0.74f}, {0.16f, 0.96f}, {0.31f, 1.0f}, {0.46f, 0.96f},
                   {0.62f, 0.74f}, {0.62f, 0.0f}}});
        add('V', {{{0.0f, 0.0f}, {0.31f, 1.0f}, {0.62f, 0.0f}}});
        add('W', {{{0.0f, 0.0f}, {0.16f, 1.0f}, {0.31f, 0.42f}, {0.46f, 1.0f}, {0.62f, 0.0f}}}, 0.7f);
        add('X', {{{0.0f, 0.0f}, {0.62f, 1.0f}}, {{0.62f, 0.0f}, {0.0f, 1.0f}}});
        add('Y', {{{0.0f, 0.0f}, {0.31f, 0.5f}, {0.62f, 0.0f}}, {{0.31f, 0.5f}, {0.31f, 1.0f}}});
        add('Z', {{{0.0f, 0.0f}, {0.62f, 0.0f}, {0.0f, 1.0f}, {0.62f, 1.0f}}});

        // ---- Digits ----
        add('0', {{{0.31f, 0.0f}, {0.55f, 0.12f}, {0.62f, 0.5f}, {0.55f, 0.88f}, {0.31f, 1.0f},
                   {0.07f, 0.88f}, {0.0f, 0.5f}, {0.07f, 0.12f}, {0.31f, 0.0f}}});
        add('1', {{{0.12f, 0.2f}, {0.34f, 0.0f}, {0.34f, 1.0f}}, {{0.1f, 1.0f}, {0.56f, 1.0f}}}, 0.5f);
        add('2', {{{0.04f, 0.2f}, {0.22f, 0.0f}, {0.46f, 0.02f}, {0.6f, 0.22f}, {0.5f, 0.46f},
                   {0.04f, 1.0f}, {0.62f, 1.0f}}});
        add('3', {{{0.05f, 0.14f}, {0.32f, 0.0f}, {0.56f, 0.14f}, {0.48f, 0.42f}, {0.26f, 0.5f}},
                  {{0.26f, 0.5f}, {0.56f, 0.62f}, {0.58f, 0.86f}, {0.32f, 1.0f}, {0.06f, 0.88f}}});
        add('4', {{{0.46f, 0.0f}, {0.02f, 0.66f}, {0.62f, 0.66f}}, {{0.46f, 0.0f}, {0.46f, 1.0f}}});
        add('5', {{{0.56f, 0.02f}, {0.1f, 0.02f}, {0.06f, 0.44f}, {0.34f, 0.38f}, {0.56f, 0.52f},
                   {0.58f, 0.78f}, {0.4f, 0.98f}, {0.12f, 0.98f}, {0.02f, 0.82f}}});
        add('6', {{{0.54f, 0.1f}, {0.3f, 0.0f}, {0.1f, 0.2f}, {0.04f, 0.6f}, {0.18f, 0.98f},
                   {0.42f, 0.98f}, {0.58f, 0.78f}, {0.5f, 0.54f}, {0.26f, 0.48f}, {0.06f, 0.62f}}});
        add('7', {{{0.0f, 0.0f}, {0.62f, 0.0f}, {0.26f, 1.0f}}});
        add('8', {{{0.31f, 0.48f}, {0.5f, 0.32f}, {0.5f, 0.12f}, {0.31f, 0.0f}, {0.12f, 0.12f},
                   {0.12f, 0.32f}, {0.31f, 0.48f}},
                  {{0.31f, 0.48f}, {0.56f, 0.64f}, {0.56f, 0.86f}, {0.31f, 1.0f}, {0.06f, 0.86f},
                   {0.06f, 0.64f}, {0.31f, 0.48f}}});
        add('9', {{{0.08f, 0.9f}, {0.32f, 1.0f}, {0.52f, 0.8f}, {0.58f, 0.4f}, {0.44f, 0.02f},
                   {0.2f, 0.02f}, {0.04f, 0.22f}, {0.12f, 0.46f}, {0.36f, 0.52f}, {0.56f, 0.38f}}});

        // ---- Punctuation ----
        add('.', {{{0.28f, 0.94f}, {0.34f, 0.94f}}}, 0.3f);
        add(',', {{{0.32f, 0.88f}, {0.32f, 0.98f}, {0.22f, 1.08f}}}, 0.3f);
        add(':', {{{0.3f, 0.4f}, {0.36f, 0.4f}}, {{0.3f, 0.86f}, {0.36f, 0.86f}}}, 0.3f);
        add('-', {{{0.12f, 0.5f}, {0.5f, 0.5f}}}, 0.6f);
        add('_', {{{0.0f, 1.04f}, {0.62f, 1.04f}}});
        add('/', {{{0.0f, 1.0f}, {0.6f, 0.0f}}}, 0.5f);
        add('!', {{{0.31f, 0.0f}, {0.31f, 0.66f}}, {{0.31f, 0.9f}, {0.31f, 0.96f}}}, 0.3f);
        add('?', {{{0.06f, 0.2f}, {0.22f, 0.02f}, {0.46f, 0.06f}, {0.56f, 0.26f}, {0.42f, 0.46f},
                   {0.31f, 0.56f}, {0.31f, 0.66f}},
                  {{0.31f, 0.9f}, {0.31f, 0.96f}}});
        add('(', {{{0.42f, 0.0f}, {0.18f, 0.3f}, {0.18f, 0.7f}, {0.42f, 1.0f}}}, 0.4f);
        add(')', {{{0.2f, 0.0f}, {0.44f, 0.3f}, {0.44f, 0.7f}, {0.2f, 1.0f}}}, 0.4f);
        add('+', {{{0.31f, 0.24f}, {0.31f, 0.76f}}, {{0.08f, 0.5f}, {0.54f, 0.5f}}});
        add('<', {{{0.5f, 0.12f}, {0.12f, 0.5f}, {0.5f, 0.88f}}}, 0.6f);
        add('>', {{{0.12f, 0.12f}, {0.5f, 0.5f}, {0.12f, 0.88f}}}, 0.6f);
        add('%', {{{0.56f, 0.04f}, {0.06f, 0.96f}},
                  {{0.1f, 0.1f}, {0.24f, 0.1f}, {0.24f, 0.28f}, {0.1f, 0.28f}, {0.1f, 0.1f}},
                  {{0.38f, 0.72f}, {0.52f, 0.72f}, {0.52f, 0.9f}, {0.38f, 0.9f}, {0.38f, 0.72f}}});

        return g;
    }();
    return glyphs;
}

} // namespace

const Glyph& font_glyph(char c) {
    static const Glyph blank{};
    const char uc = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto& t = table();
    const auto it = t.find(uc);
    return it != t.end() ? it->second : blank;
}

f32 font_text_width(std::string_view text, f32 size) {
    f32 w = 0.0f;
    for (char c : text) {
        w += (font_glyph(c).advance + kFontTracking) * size;
    }
    if (!text.empty()) {
        w -= kFontTracking * size; // no trailing gap
    }
    return w;
}

} // namespace alryn::ui
