#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;   // model-view-projection
    mat4 model; // for transforming normals to world space
} pc;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    vWorldNormal = mat3(pc.model) * inNormal;
    vColor = inColor;
}
