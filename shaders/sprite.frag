#version 450

// Additive VFX sprite glow: a soft quadratic halo out to the capsule's rim plus an optional hot,
// white-shifted core, faded where it meets the opaque scene behind it (soft particles). Drawn with
// additive blending, so overlapping sprites build up into bright, blooming light.

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in float vLen;
layout(location = 2) flat in vec4 vColor;
layout(location = 3) flat in vec2 vShape; // x = core strength, y = radius (m)

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 camPos;
    vec4 camRight;
    vec4 camUp;
    vec4 params; // xy = projection terms P22/P32 (depth -> metres), zw = framebuffer size (px)
} pc;

// Scene depth without the transparent layers (the SSAO prepass) - for the soft-particle fade.
layout(set = 0, binding = 4) uniform sampler2D sceneDepth;

// Raw 0..1 depth -> metres in front of the camera (see water.frag).
float viewDist(float d) {
    return pc.params.y / (d + pc.params.x);
}

void main() {
    // Distance from the capsule's spine in radius units: 0 on the spine, 1 at the rim.
    float along = vLocal.x - clamp(vLocal.x, 0.0, vLen);
    float d = length(vec2(along, vLocal.y));
    if (d >= 1.0) {
        discard;
    }
    float f = 1.0 - d;
    float halo = f * f;                               // soft glow falling off to the rim
    float core = smoothstep(0.5, 0.95, f) * vShape.x; // a hot, tight centre
    // Soft-particle fade: dim the glow as it nears the opaque surface behind it, so a flare sitting
    // on the ground blends into it instead of showing a hard seam where it intersects.
    float scene = viewDist(texture(sceneDepth, gl_FragCoord.xy / pc.params.zw).r);
    float here = viewDist(gl_FragCoord.z);
    float soft = clamp((scene - here) / max(vShape.y, 0.1), 0.0, 1.0);
    float k = (halo + core * 1.4) * soft * vColor.a;
    vec3 col = mix(vColor.rgb, vec3(1.0), clamp(core, 0.0, 1.0) * 0.55); // the core burns white-hot
    outColor = vec4(col * k, 1.0);
}
