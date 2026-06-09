#version 450

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vColor;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    vec4 tint;
    vec4 params;
} pc;

void main() {
    vec3 normal = normalize(vWorldNormal);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normal, lightDir), 0.0);
    float ambient = 0.28;
    vec3 color = vColor * pc.tint.rgb * (ambient + 0.85 * diffuse);
    outColor = vec4(color, pc.tint.a);
}
