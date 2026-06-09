#version 450

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vColor;

layout(location = 0) out vec4 outColor;

// Simple stylized lighting: one directional key light + ambient fill. Because the
// mesher emits per-face (flat) normals, each facet is shaded uniformly, giving the
// low-poly look.
void main() {
    vec3 normal = normalize(vWorldNormal);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normal, lightDir), 0.0);
    float ambient = 0.28;
    vec3 color = vColor * (ambient + 0.85 * diffuse);
    outColor = vec4(color, 1.0);
}
