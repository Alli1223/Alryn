#version 450

// Bloom bright-pass: keep only the pixels that should glow (lantern glass, lit
// windows, spell VFX, the sun's sky glow), with a soft knee so the cutoff doesn't
// shimmer. Runs at half resolution into the first bloom ping-pong target.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;

layout(push_constant) uniform Push {
    vec4 params; // x = knee start (luminance), y = knee end
    vec4 screen; // xy = target resolution (px)
} pc;

void main() {
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    vec3 c = texture(sceneTex, uv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = smoothstep(pc.params.x, pc.params.y, lum);
    outColor = vec4(c * k, 1.0);
}
