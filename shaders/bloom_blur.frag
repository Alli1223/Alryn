#version 450

// 4x4 RGB box blur for the bloom chain - run twice (ping-pong) at half resolution
// for a wide, soft halo at trivial cost.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec4 screen; // xy = target resolution (px)
} pc;

void main() {
    vec2 texel = 1.0 / pc.screen.xy;
    vec2 uv = gl_FragCoord.xy * texel;
    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            sum += texture(srcTex, uv + (vec2(x, y) - 0.5) * texel).rgb;
        }
    }
    outColor = vec4(sum / 16.0, 1.0);
}
