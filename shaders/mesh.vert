#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    vec4 tint;   // rgb colour multiplier, a = alpha
    vec4 params; // x = time, yzw = camera position
} pc;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    vWorldNormal = mat3(pc.model) * inNormal;
    vColor = inColor;
}
