#version 450

// Screen-space god rays: march each pixel toward the sun's screen position through
// the depth prepass, accumulating where the march crosses SKY (depth = far). Geometry
// between the pixel and the sun (a town wall, a tree line) blocks the march, so shafts
// stream through gaps the way low sun does. Strength is faded in by the CPU only when
// the sun sits low (dawn/dusk golden hour); the result is tinted at composite time.

layout(location = 0) out vec4 outRays; // R8: shaft intensity

layout(set = 0, binding = 0) uniform sampler2D depthTex; // camera depth prepass (full res)

layout(push_constant) uniform Push {
    vec4 sun;    // xy = sun position in UV space, z = 1 if in front of the camera
    vec4 params; // x = strength 0..1, y = per-step decay
    vec4 screen; // xy = target resolution (px)
} pc;

void main() {
    if (pc.sun.z < 0.5 || pc.params.x <= 0.001) {
        outRays = vec4(0.0);
        return;
    }
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    const int kSteps = 36;
    vec2 delta = (pc.sun.xy - uv) / float(kSteps);
    vec2 p = uv;
    float acc = 0.0;
    float w = 1.0;
    float total = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        p += delta;
        vec2 cp = clamp(p, vec2(0.0), vec2(1.0));
        float sky = texture(depthTex, cp).r > 0.9999 ? 1.0 : 0.0; // far plane = open sky
        acc += sky * w;
        total += w;
        w *= pc.params.y;
    }
    // Fade with distance from the sun so shafts hug the light instead of washing the
    // whole frame.
    float falloff = exp(-distance(uv, pc.sun.xy) * 2.2);
    outRays = vec4(pc.params.x * (acc / total) * falloff);
}
