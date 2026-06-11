#version 450

// Vegetation vertex shader. Same outputs as mesh.vert (so it shares mesh.frag for
// lighting + shadows), but it bends plants: a layered-sine breeze plus a push away
// from the player. Vegetation is baked in WORLD space and drawn with model =
// identity, so inPosition.xz are world coordinates here. `inSway` is the vertex's
// height above its plant base (0 at the root, larger at the tip) so the bend grows
// toward the top. For vegetation draws the renderer packs the player position +
// wind strength into `params` (mesh.frag ignores params, so this is free).
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inSway;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;
    vec4 tint;
    vec4 params;   // x = time, y = player.x, z = player.z, w = wind strength
    vec4 sun;
    vec4 sunColor;
} pc;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec4 vShadowCoord;
layout(location = 3) out vec3 vWorldPos;

void main() {
    vec3 p = inPosition;

    if (inSway > 0.001) {
        const float t = pc.params.x;
        const vec2 wp = inPosition.xz;

        // Breeze: a couple of sines at different scales make a gusty, rolling wind.
        float phase = t * 1.6 + wp.x * 0.18 + wp.y * 0.22;
        float gust = sin(phase) * 0.6 + sin(phase * 0.47 + wp.x * 0.3) * 0.4;
        vec2 windDir = normalize(vec2(0.8, 0.55));
        vec2 windOff = windDir * gust * pc.params.w * inSway;

        // Player push: plants bend away from the player as they walk through.
        vec2 toPlant = wp - pc.params.yz;
        float d = length(toPlant);
        float influence = 1.0 - smoothstep(0.0, 1.35, d);
        vec2 pushOff = (d > 1e-3 ? toPlant / d : vec2(0.0)) * influence * 0.55 * inSway;

        p.xz += windOff + pushOff;
        // Tips dip a little as they bend over, so it doesn't look like sliding.
        p.y -= (abs(gust) * pc.params.w * 0.5 + influence * 0.4) * inSway * 0.4;
    }

    vec4 worldPos = pc.model * vec4(p, 1.0);
    gl_Position = pc.mvp * vec4(p, 1.0);
    vWorldNormal = mat3(pc.model) * inNormal;
    vColor = inColor;
    vShadowCoord = pc.lightVP * worldPos;
    vWorldPos = worldPos.xyz;
}
