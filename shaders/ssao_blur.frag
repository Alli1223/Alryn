#version 450

// 4x4 box blur over the raw SSAO target: averages away the per-pixel noise from the
// randomly-rotated kernel in ssao.frag, leaving soft, stable contact shadows.

layout(location = 0) out vec4 outAO;

layout(set = 0, binding = 0) uniform sampler2D aoTex;

layout(push_constant) uniform Push {
    vec4 screen; // xy = target resolution (px)
} pc;

void main() {
    vec2 texel = 1.0 / pc.screen.xy;
    vec2 uv = gl_FragCoord.xy * texel;
    float sum = 0.0;
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            sum += texture(aoTex, uv + (vec2(x, y) - 0.5) * texel).r;
        }
    }
    outAO = vec4(sum / 16.0);
}
