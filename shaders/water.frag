#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;
    vec4 tint;
    vec4 params;   // x = time, yzw = camera position
    vec4 sun;      // xyz = normalized direction TO the sun, w = sun intensity
    vec4 sunColor; // rgb = sun colour, w = shadow strength
} pc;

void main() {
    vec3 N = normalize(vNormal);
    vec3 camPos = pc.params.yzw;
    vec3 V = normalize(camPos - vWorldPos);
    vec3 L = normalize(pc.sun.xyz);
    float intensity = pc.sun.w;

    // Fresnel: more opaque/reflective at grazing angles.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);

    vec3 deep = vec3(0.03, 0.15, 0.26);
    vec3 shallow = vec3(0.10, 0.34, 0.40);
    vec3 color = mix(deep, shallow, clamp(N.y, 0.0, 1.0));

    // Daylight tints the water and lifts it; at night it goes dark and steely.
    color *= mix(0.18, 1.0, intensity);
    color *= mix(vec3(0.55, 0.6, 0.8), pc.sunColor.rgb, intensity);

    // Specular sun glints off the wave facets + a sky-reflection lift at grazing.
    vec3 H = normalize(L + V);
    float ndh = max(dot(N, H), 0.0);
    float spec = pow(ndh, 64.0) * intensity;
    color += pc.sunColor.rgb * spec * 0.8;
    color += fresnel * 0.25 * mix(0.2, 1.0, intensity);

    // Glistening sparkle: animated high-frequency highlights, densest along the
    // sun's reflection, so the water twinkles with moving bright spots.
    float t = pc.params.x;
    vec2 sp = vWorldPos.xz;
    float tw = sin(dot(sp, vec2(12.9, 7.3)) + t * 3.1) *
               sin(dot(sp, vec2(-5.7, 9.1)) - t * 2.3) *
               sin(dot(sp, vec2(3.1, -4.4)) + t * 1.7);
    float glint = smoothstep(0.72, 1.0, tw) * pow(ndh, 20.0) * intensity;
    color += pc.sunColor.rgb * glint * 2.2;

    float alpha = mix(0.5, 0.92, fresnel);
    outColor = vec4(color, alpha);
}
