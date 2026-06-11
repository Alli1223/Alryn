#version 450

// Self-lit material for lantern glass and glowing windows: outputs the colour at
// full brightness regardless of the sun, so it reads as a light source at night.
layout(location = 0) in vec3 vWorldNormal; // unused
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vShadowCoord; // unused

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;
    vec4 tint;
    vec4 params;
    vec4 sun;
    vec4 sunColor;
} pc;

void main() {
    outColor = vec4(vColor * pc.tint.rgb, pc.tint.a);
}
