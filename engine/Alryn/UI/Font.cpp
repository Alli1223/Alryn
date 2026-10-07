#include <Alryn/UI/Font.h>

#include <Alryn/Core/Log.h>
#include <Alryn/Renderer/Renderer.h>

#include <stb_truetype.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <numeric>
#include <thread>

namespace alryn::embedded {
// Generated from engine/assets/fonts by cmake/EmbedFiles.cmake.
extern const unsigned char kFontBody[];
extern const std::size_t kFontBody_size;
extern const unsigned char kFontBodyBold[];
extern const std::size_t kFontBodyBold_size;
extern const unsigned char kFontDisplay[];
extern const std::size_t kFontDisplay_size;
} // namespace alryn::embedded

namespace alryn::ui {

namespace {
constexpr f32 kEmPx = 52.0f;   // the atlas's em size: cap heights land around 34 px
constexpr u32 kAtlasWidth = 1024;
constexpr int kGap = 2;        // texels between packed glyphs (no bleed under linear filtering)

struct Baked {
    std::vector<u8> sdf;
    int w = 0, h = 0, xoff = 0, yoff = 0;
};
} // namespace

FontAtlas& fonts() {
    static FontAtlas atlas;
    return atlas;
}

int FontAtlas::index_of(char c) {
    int u = static_cast<unsigned char>(c);
    if (u >= 'a' && u <= 'z') {
        u -= 'a' - 'A'; // the UI is all caps
    }
    return (u < kFirst || u > kLast) ? -1 : u - kFirst;
}

const AtlasGlyph& FontAtlas::glyph(FontFace face, char c) const {
    const int i = index_of(c);
    return faces_[static_cast<usize>(face)].glyphs[static_cast<usize>(i < 0 ? 0 : i)];
}

f32 FontAtlas::kern(FontFace face, char a, char b) const {
    const int i = index_of(a);
    const int j = index_of(b);
    const Face& f = faces_[static_cast<usize>(face)];
    if (i < 0 || j < 0 || f.kerning.empty()) {
        return 0.0f;
    }
    return f.kerning[static_cast<usize>(i * kCount + j)];
}

f32 FontAtlas::text_width(FontFace face, std::string_view text, f32 size, f32 tracking) const {
    if (text.empty()) {
        return 0.0f;
    }
    const f32 k = size / cap_height(face);
    f32 w = 0.0f;
    for (usize i = 0; i < text.size(); ++i) {
        w += glyph(face, text[i]).advance * k;
        if (i + 1 < text.size()) {
            w += kern(face, text[i], text[i + 1]) * k + tracking * size;
        }
    }
    return w;
}

bool FontAtlas::build() {
    if (tried_) {
        return ready_;
    }
    tried_ = true;

    // In FontFace order: Body, Bold, Display.
    const unsigned char* data[kFontFaceCount] = {embedded::kFontBody, embedded::kFontBodyBold,
                                                 embedded::kFontDisplay};
    std::array<stbtt_fontinfo, kFontFaceCount> info{};
    std::array<f32, kFontFaceCount> scale{};
    for (usize f = 0; f < kFontFaceCount; ++f) {
        if (stbtt_InitFont(&info[f], data[f], stbtt_GetFontOffsetForIndex(data[f], 0)) == 0) {
            ALRYN_ERROR("UI font {} failed to parse - falling back to the vector font", f);
            return false;
        }
        scale[f] = stbtt_ScaleForMappingEmToPixels(&info[f], kEmPx);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBox(&info[f], 'H', &x0, &y0, &x1, &y1);
        faces_[f].cap = std::max(1.0f, static_cast<f32>(y1) * scale[f]);
    }

    // Distance fields are the slow part (every texel against every outline curve), so bake
    // the glyphs across all cores. Each one is independent; stb_truetype only reads the fonts.
    constexpr int total = static_cast<int>(kFontFaceCount) * kCount;
    std::vector<Baked> baked(static_cast<usize>(total));
    std::atomic<int> next{0};
    auto work = [&] {
        for (int j = next++; j < total; j = next++) {
            const usize f = static_cast<usize>(j / kCount);
            const int cp = kFirst + j % kCount;
            Baked& b = baked[static_cast<usize>(j)];
            unsigned char* sdf = stbtt_GetCodepointSDF(
                &info[f], scale[f], cp, static_cast<int>(kSpread), 128, 128.0f / kSpread, &b.w,
                &b.h, &b.xoff, &b.yoff);
            if (sdf != nullptr) {
                b.sdf.assign(sdf, sdf + static_cast<std::ptrdiff_t>(b.w) * b.h);
                stbtt_FreeSDF(sdf, nullptr);
            }
        }
    };
    const u32 cores = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (u32 t = 1; t < cores; ++t) {
        pool.emplace_back(work);
    }
    work();
    for (std::thread& t : pool) {
        t.join();
    }

    // Shelf-pack the glyphs, tallest first, into a fixed-width atlas.
    std::vector<int> order(static_cast<usize>(total));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return baked[static_cast<usize>(a)].h > baked[static_cast<usize>(b)].h;
    });
    std::vector<IVec2> at(static_cast<usize>(total), IVec2{0});
    int x = kGap, y = kGap, shelf = 0;
    for (int j : order) {
        const Baked& b = baked[static_cast<usize>(j)];
        if (b.sdf.empty()) {
            continue;
        }
        if (x + b.w + kGap > static_cast<int>(kAtlasWidth)) {
            x = kGap;
            y += shelf + kGap;
            shelf = 0;
        }
        at[static_cast<usize>(j)] = IVec2{x, y};
        x += b.w + kGap;
        shelf = std::max(shelf, b.h);
    }
    width_ = kAtlasWidth;
    height_ = 1;
    while (height_ < static_cast<u32>(y + shelf + kGap)) {
        height_ *= 2;
    }
    texels_.assign(static_cast<usize>(width_) * height_, 0);

    for (int j = 0; j < total; ++j) {
        const usize f = static_cast<usize>(j / kCount);
        const int i = j % kCount;
        const int cp = kFirst + i;
        const Baked& b = baked[static_cast<usize>(j)];
        AtlasGlyph& g = faces_[f].glyphs[static_cast<usize>(i)];
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info[f], cp, &advance, &lsb);
        g.advance = static_cast<f32>(advance) * scale[f];
        if (b.sdf.empty()) {
            continue; // no ink (space)
        }
        const IVec2 p = at[static_cast<usize>(j)];
        for (int row = 0; row < b.h; ++row) {
            std::copy_n(b.sdf.begin() + static_cast<std::ptrdiff_t>(row) * b.w, b.w,
                        texels_.begin() + static_cast<std::ptrdiff_t>(p.y + row) * width_ + p.x);
        }
        g.visible = true;
        g.offset = Vec2{static_cast<f32>(b.xoff), static_cast<f32>(b.yoff)};
        g.size = Vec2{static_cast<f32>(b.w), static_cast<f32>(b.h)};
        g.uv = Vec4{static_cast<f32>(p.x) / static_cast<f32>(width_),
                    static_cast<f32>(p.y) / static_cast<f32>(height_),
                    static_cast<f32>(p.x + b.w) / static_cast<f32>(width_),
                    static_cast<f32>(p.y + b.h) / static_cast<f32>(height_)};
    }

    // Pair kerning, cached as a table (the font's own lookup walks its tables per call).
    for (usize f = 0; f < kFontFaceCount; ++f) {
        std::vector<f32>& kt = faces_[f].kerning;
        kt.assign(static_cast<usize>(kCount * kCount), 0.0f);
        for (int a = 0; a < kCount; ++a) {
            for (int b = 0; b < kCount; ++b) {
                kt[static_cast<usize>(a * kCount + b)] =
                    static_cast<f32>(stbtt_GetCodepointKernAdvance(&info[f], kFirst + a, kFirst + b)) *
                    scale[f];
            }
        }
    }
    ready_ = true;
    ALRYN_INFO("UI fonts baked: {}x{} SDF atlas", width_, height_);
    return true;
}

bool load_fonts(Renderer& renderer) {
    FontAtlas& atlas = fonts();
    if (!atlas.build()) {
        return false;
    }
    renderer.set_ui_font_atlas(atlas.texels().data(), atlas.width(), atlas.height());
    return true;
}

} // namespace alryn::ui
