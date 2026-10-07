#version 450

// Self-lit material for lantern glass and glowing windows: outputs the colour at
// full brightness regardless of the sun, so it reads as a light source at night.
layout(location = 0) in vec3 vWorldNormal; // unused
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vShadowCoord; // unused
layout(location = 4) in float vPave;        // shared mesh.vert output (cobble paving); always 0 here

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
    // Lift bright self-lit surfaces so lanterns / windows / crystals / fires read HOT + glowing
    // against the graded scene (a cheap stand-in for a full bloom pass).
    vec3 c = vColor * pc.tint.rgb * 1.18;
    // A no-op use of vPave (always 0: only terrain is paved) keeps it in the shader interface.
    outColor = vec4(c * (1.0 + 0.001 * vPave), pc.tint.a);
}
