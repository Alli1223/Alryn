#version 450

// Depth-only pass: render the scene from the sun's point of view into the shadow
// map. pc.mvp is set to (lightViewProj * model) by the renderer for this pass.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal; // unused
layout(location = 2) in vec3 inColor;  // unused

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}
