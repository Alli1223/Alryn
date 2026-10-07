#version 450

// Additive VFX sprite (particles, flares, beams), one quad per instance. A sprite is a capsule from
// `a` to `b` with radius r: when a == b it collapses to a round camera-facing billboard; otherwise
// the quad stretches along the segment and turns about that axis to face the camera (an axial
// billboard - beams and motion-streaked sparks). sprite.frag shapes the glow from the local
// coordinates emitted here.

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 camPos;   // xyz = camera position (world)
    vec4 camRight; // xyz = camera right axis (world)
    vec4 camUp;    // xyz = camera up axis (world)
    vec4 params;   // xy = projection terms P22/P32 (depth -> metres), zw = framebuffer size (px)
} pc;

layout(location = 0) in vec4 inA;     // xyz = start, w = radius (m)
layout(location = 1) in vec4 inB;     // xyz = end, w = hot-core strength 0..1
layout(location = 2) in vec4 inColor; // rgb = colour, a = intensity

layout(location = 0) out vec2 vLocal;      // x along the spine, y across it - both in radius units
layout(location = 1) flat out float vLen;  // spine length in radius units (0 = a round sprite)
layout(location = 2) flat out vec4 vColor;
layout(location = 3) flat out vec2 vShape; // x = core strength, y = radius (m)

// Two-triangle quad: x picks the start (0) or end (1) cap, y the side (-1 / +1).
const vec2 kCorners[6] = vec2[6](
    vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));

void main() {
    float r = max(inA.w, 1e-4);
    vec3 a = inA.xyz;
    vec3 b = inB.xyz;
    vec3 axis = pc.camUp.xyz;
    vec3 side = pc.camRight.xyz;
    float len = length(b - a);
    if (len > r * 0.01) {
        vec3 dir = (b - a) / len;
        vec3 toCam = normalize(pc.camPos.xyz - (a + b) * 0.5);
        vec3 s = cross(dir, toCam);
        float sl = length(s);
        if (sl > 1e-3) {
            axis = dir;
            side = s / sl;
        } else {
            // Pointing straight down the view: draw it as a round glow at its midpoint.
            a = (a + b) * 0.5;
            b = a;
            len = 0.0;
        }
    } else {
        b = a;
        len = 0.0;
    }
    vec2 c = kCorners[gl_VertexIndex];
    // The round caps reach one radius past each end of the spine.
    vec3 end = c.x < 0.5 ? a - axis * r : b + axis * r;
    vec3 world = end + side * (c.y * r);
    float spine = len / r;
    vLocal = vec2(c.x < 0.5 ? -1.0 : spine + 1.0, c.y);
    vLen = spine;
    vColor = inColor;
    vShape = vec2(inB.w, r);
    gl_Position = pc.viewProj * vec4(world, 1.0);
}
