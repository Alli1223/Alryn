#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    vec4 tint;
    vec4 params; // x = time, yzw = camera position
} pc;

void main() {
    vec3 N = normalize(vNormal);
    vec3 camPos = pc.params.yzw;
    vec3 V = normalize(camPos - vWorldPos);
    vec3 L = normalize(vec3(0.4, 1.0, 0.3));

    // Fresnel: more opaque/reflective at grazing angles.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);

    vec3 deep = vec3(0.03, 0.15, 0.26);
    vec3 shallow = vec3(0.10, 0.34, 0.40);
    vec3 color = mix(deep, shallow, clamp(N.y, 0.0, 1.0));

    // Specular glints off the wave facets + a sky-reflection lift at grazing.
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 64.0);
    color += vec3(spec) * 0.6;
    color += fresnel * 0.25;

    float alpha = mix(0.5, 0.92, fresnel);
    outColor = vec4(color, alpha);
}
