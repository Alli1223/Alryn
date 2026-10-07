#version 450

// 2D UI fragment shader. One pipeline covers every UI primitive in the engine, each an
// anti-aliased signed-distance shape:
//   mode 0  rounded rectangle - vertical gradient fill (color -> color2), optional border,
//           and an edge softness wide enough to make soft drop shadows / glows
//   mode 1  rounded-cap capsule / line segment (slider tracks, icon strokes, vector font)
//   mode 2  radial gradient ellipse filling the rect (color at the centre -> color2 at the rim),
//           for glows, halos and vignettes
//   mode 3  a text glyph sampled from the signed-distance-field font atlas, with an optional
//           outline, softness (shadows) and a vertical gradient across the text line
layout(push_constant) uniform Push {
    vec4 rect;   // xy = top-left (px), zw = size (px)
    vec4 color;  // fill rgba (gradient start)
    vec4 params; // x = corner radius, y = edge softness, z = mode, w = border / half-thickness
    vec4 seg;    // segment: xy = p0, zw = p1 (px). glyph: atlas uv rect (u0, v0, u1, v1)
    vec4 border; // border / glyph outline rgba
    vec4 color2; // gradient end rgba
    vec4 extra;  // glyph: x = screen px per SDF unit, y = outline px, zw = gradient span (y0, h)
    vec2 screen;
} pc;

layout(set = 0, binding = 0) uniform sampler2D font_atlas;

layout(location = 0) in vec2 vPix;
layout(location = 0) out vec4 outColor;

// Signed distance to a rounded box (negative inside).
float sd_round_box(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - (half_size - radius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// Signed distance to a capsule (segment expanded by half-thickness).
float sd_segment(vec2 p, vec2 a, vec2 b, float half_thickness) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
    return length(pa - ba * h) - half_thickness;
}

void main() {
    const float soft = max(pc.params.y, 0.5);
    const int mode = int(pc.params.z + 0.5);
    vec4 col;
    float alpha;

    if (mode == 3) {
        // SDF glyph: distance to the outline in screen px (positive inside the letter).
        vec2 t = (vPix - pc.rect.xy) / max(pc.rect.zw, vec2(1e-3));
        vec2 uv = mix(pc.seg.xy, pc.seg.zw, t);
        float sd = (texture(font_atlas, uv).r - 0.5) * pc.extra.x;
        float aa = max(soft, 0.7);
        col = pc.color;
        if (pc.extra.w > 0.0) {
            col = mix(pc.color, pc.color2, clamp((vPix.y - pc.extra.z) / pc.extra.w, 0.0, 1.0));
        }
        float fill = smoothstep(-aa, aa, sd);
        float ow = pc.extra.y;
        if (ow > 0.0 && pc.border.a > 0.0) {
            // Outline: the letter grown by `ow`, in the border colour, under the fill.
            float outer = smoothstep(-aa, aa, sd + ow);
            vec4 fill_col = vec4(col.rgb, col.a);
            col = mix(pc.border, fill_col, fill);
            alpha = outer * col.a;
        } else {
            alpha = fill * col.a;
        }
    } else if (mode == 2) {
        // Radial gradient filling the rect's inscribed ellipse: `color` out to radius params.x
        // (0..1 of the ellipse), easing to `color2` at the rim.
        vec2 half_size = max(pc.rect.zw * 0.5, vec2(1e-3));
        float r = length((vPix - (pc.rect.xy + half_size)) / half_size);
        col = mix(pc.color, pc.color2, smoothstep(pc.params.x, 1.0, r));
        alpha = col.a * (1.0 - smoothstep(0.85, 1.0, r));
    } else {
        float d;
        if (mode == 0) {
            vec2 half_size = pc.rect.zw * 0.5;
            vec2 center = pc.rect.xy + half_size;
            // A soft shadow is drawn as a rect inflated by its blur; the shape inside it keeps
            // its nominal size so the falloff is centred on the true edge.
            vec2 shape_half = max(half_size - vec2(pc.params.y > 1.0 ? soft : 0.0), vec2(0.5));
            float radius = min(pc.params.x, min(shape_half.x, shape_half.y));
            d = sd_round_box(vPix - center, shape_half, radius);
            float g = clamp((vPix.y - pc.rect.y) / max(pc.rect.w, 1.0), 0.0, 1.0);
            col = mix(pc.color, pc.color2, g);

            // Optional border: blend toward the border colour near the edge.
            float bt = pc.params.w;
            if (bt > 0.0 && pc.border.a > 0.0) {
                float edge = smoothstep(-bt - 0.75, -bt + 0.75, d);
                col = mix(col, pc.border, edge);
            }
        } else {
            d = sd_segment(vPix, pc.seg.xy, pc.seg.zw, pc.params.w);
            col = pc.color;
        }
        alpha = (1.0 - smoothstep(-soft, soft, d)) * col.a;
    }

    if (alpha <= 0.0) {
        discard;
    }
    outColor = vec4(col.rgb, alpha);
}
