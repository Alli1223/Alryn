#version 450

// 2D UI fragment shader. Renders an anti-aliased rounded rectangle (mode 0) or a
// rounded-cap capsule / line segment (mode 1, used for the vector font strokes
// and slider tracks) via signed-distance fields. One pipeline covers every UI
// primitive in the engine; the clean edges come from the SDF + screen-space AA.
layout(push_constant) uniform Push {
    vec4 rect;   // xy = top-left (px), zw = size (px)
    vec4 color;  // fill rgba
    vec4 params; // x = corner radius, y = edge softness, z = mode, w = border / half-thickness
    vec4 seg;    // xy = p0, zw = p1 (px)
    vec4 border; // border rgba
    vec2 screen;
} pc;

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
    const float aa = max(pc.params.y, 0.5);
    vec4 col = pc.color;
    float d;

    if (pc.params.z < 0.5) {
        // Rounded rectangle.
        vec2 half_size = pc.rect.zw * 0.5;
        vec2 center = pc.rect.xy + half_size;
        float radius = min(pc.params.x, min(half_size.x, half_size.y));
        d = sd_round_box(vPix - center, half_size, radius);

        // Optional border: blend toward the border colour near the edge.
        float bt = pc.params.w;
        if (bt > 0.0 && pc.border.a > 0.0) {
            float edge = smoothstep(-bt - aa, -bt + aa, d);
            col = mix(pc.color, pc.border, edge);
        }
    } else {
        // Rounded-cap segment / capsule.
        d = sd_segment(vPix, pc.seg.xy, pc.seg.zw, pc.params.w);
    }

    float alpha = (1.0 - smoothstep(-aa, aa, d)) * col.a;
    if (alpha <= 0.0) {
        discard;
    }
    outColor = vec4(col.rgb, alpha);
}
