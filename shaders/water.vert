#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal; // unused
layout(location = 2) in vec3 inColor;  // unused

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model; // pure translation (water grid follows the player in xz)
    mat4 lightVP;
    vec4 tint;
    vec4 params; // x = time, yzw = camera position
    vec4 sun;      // xyz = normalized direction TO the sun, w = sun intensity
    vec4 sunColor; // rgb = sun colour, w = shadow strength
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

// Sum-of-sines surface so the waves are continuous in world space.
float waveH(vec2 p, float t) {
    return sin(p.x * 0.18 + t * 1.1) * 0.18 +
           sin(p.y * 0.23 - t * 0.9) * 0.14 +
           sin((p.x + p.y) * 0.11 + t * 0.6) * 0.10;
}

void main() {
    vec2 wxz = inPosition.xz + pc.model[3].xz;
    float t = pc.params.x;
    float h = waveH(wxz, t);

    vec3 p = vec3(inPosition.x, inPosition.y + h, inPosition.z);
    gl_Position = pc.mvp * vec4(p, 1.0);
    vWorldPos = vec3(wxz.x, pc.model[3].y + inPosition.y + h, wxz.y);

    // Analytic normal from the wave height derivatives.
    float ddx = 0.18 * 0.18 * cos(wxz.x * 0.18 + t * 1.1) +
                0.10 * 0.11 * cos((wxz.x + wxz.y) * 0.11 + t * 0.6);
    float ddz = 0.14 * 0.23 * cos(wxz.y * 0.23 - t * 0.9) +
                0.10 * 0.11 * cos((wxz.x + wxz.y) * 0.11 + t * 0.6);
    vNormal = normalize(vec3(-ddx, 1.0, -ddz));
}
