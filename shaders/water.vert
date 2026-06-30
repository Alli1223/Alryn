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

// Directional sum-of-sines: a couple of broad rolling swells plus finer cross-chop, so the
// surface has detail at several scales (richer normals => crisper reflections + glints). Each
// wave is (kx, kz, amplitude, speed); the height and its analytic xz gradient share the loop so
// the normal exactly matches the displaced surface.
const vec4 kWaveDir[5] = vec4[5](
    vec4( 0.16,  0.05, 0.165, 1.05),
    vec4( 0.04,  0.21, 0.130, 0.90),
    vec4( 0.11,  0.11, 0.100, 0.62),
    vec4( 0.31, -0.19, 0.045, 1.75),
    vec4(-0.23,  0.27, 0.035, 1.95)
);

void main() {
    vec2 wxz = inPosition.xz + pc.model[3].xz;
    float t = pc.params.x;

    float h = 0.0;
    float ddx = 0.0;
    float ddz = 0.0;
    for (int i = 0; i < 5; ++i) {
        vec2 k = kWaveDir[i].xy;
        float a = kWaveDir[i].z;
        float w = kWaveDir[i].w;
        float ph = k.x * wxz.x + k.y * wxz.y + w * t;
        h   += a * sin(ph);
        ddx += a * k.x * cos(ph);
        ddz += a * k.y * cos(ph);
    }

    vec3 p = vec3(inPosition.x, inPosition.y + h, inPosition.z);
    gl_Position = pc.mvp * vec4(p, 1.0);
    vWorldPos = vec3(wxz.x, pc.model[3].y + inPosition.y + h, wxz.y);
    vNormal = normalize(vec3(-ddx, 1.0, -ddz));
}
