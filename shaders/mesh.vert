#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;  // world -> light clip space (for shadow lookup)
    vec4 tint;     // rgb colour multiplier, a = alpha
    vec4 params;   // x = time, yzw = camera position
    vec4 sun;      // xyz = normalized direction TO the sun, w = sun intensity
    vec4 sunColor; // rgb = sun colour, w = shadow strength
} pc;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec4 vShadowCoord;
layout(location = 3) out vec3 vWorldPos;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    vWorldNormal = mat3(pc.model) * inNormal;
    vColor = inColor;
    vShadowCoord = pc.lightVP * worldPos;
    vWorldPos = worldPos.xyz;
}
