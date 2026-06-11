#version 450

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vShadowCoord;
layout(location = 3) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;   // sun
layout(set = 0, binding = 1) uniform sampler2D lightAtlas;  // spot lights (tiled)

struct Spot {
    vec4 posRange;      // xyz position, w range
    vec4 dirCosInner;   // xyz spot dir, w cos(inner cone)
    vec4 colorCosOuter; // rgb colour*intensity, w cos(outer cone)
    vec4 atlas;         // xy tile offset, zw tile scale (in [0,1])
    mat4 viewProj;
};
layout(set = 0, binding = 2) uniform Lights {
    ivec4 count; // x = number of active spot lights
    Spot spots[4];
} lights;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;
    vec4 tint;
    vec4 params;   // x = time, yzw = camera position
    vec4 sun;      // xyz = normalized direction TO the sun, w = sun intensity
    vec4 sunColor; // rgb = sun colour, w = shadow strength
} pc;

// Fraction of the fragment in shadow (0 = lit, 1 = fully shadowed), 3x3 PCF.
float shadowOcclusion(vec4 coord, float ndotl) {
    vec3 p = coord.xyz / coord.w;
    // The shadow map is rendered and sampled with the same lightVP, so NDC->UV is
    // a plain *0.5+0.5 on both axes (no manual Y flip - that would mirror the
    // lookup around the map centre and make shadows drift as the focus moves).
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0 || p.z < 0.0) {
        return 0.0; // outside the light's view -> treat as lit
    }
    float bias = max(0.0025 * (1.0 - ndotl), 0.0008);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(shadowMap, uv + vec2(x, y) * texel).r;
            sum += (p.z - bias > d) ? 1.0 : 0.0;
        }
    }
    return sum / 9.0;
}

// Occlusion of a fragment from a spot light, sampling that light's atlas tile.
float spotOcclusion(Spot s, vec3 wpos) {
    vec4 lc = s.viewProj * vec4(wpos, 1.0);
    if (lc.w <= 0.0) {
        return 0.0;
    }
    vec3 p = lc.xyz / lc.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z < 0.0 || p.z > 1.0) {
        return 0.0;
    }
    vec2 auv = s.atlas.xy + uv * s.atlas.zw; // into this light's atlas tile
    float d = texture(lightAtlas, auv).r;
    return (p.z - 0.0018 > d) ? 1.0 : 0.0;
}

// Sum of all spot lights reaching this fragment (with cone + falloff + shadow).
vec3 spotLighting(vec3 N, vec3 wpos) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < lights.count.x; ++i) {
        Spot s = lights.spots[i];
        vec3 toL = s.posRange.xyz - wpos;
        float dist = length(toL);
        if (dist > s.posRange.w) {
            continue;
        }
        vec3 L = toL / max(dist, 1e-4);
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) {
            continue;
        }
        float cosA = dot(-L, s.dirCosInner.xyz);
        float cone = smoothstep(s.colorCosOuter.w, s.dirCosInner.w, cosA);
        if (cone <= 0.0) {
            continue;
        }
        float atten = clamp(1.0 - dist / s.posRange.w, 0.0, 1.0);
        atten *= atten;
        float occ = spotOcclusion(s, wpos);
        sum += s.colorCosOuter.rgb * (ndl * cone * atten * (1.0 - occ));
    }
    return sum;
}

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 L = normalize(pc.sun.xyz);
    float intensity = pc.sun.w;            // 0 at night .. 1 at noon
    vec3 sunCol = pc.sunColor.rgb;

    float ndotl = max(dot(N, L), 0.0);
    float shadow = shadowOcclusion(vShadowCoord, ndotl);
    float lit = 1.0 - pc.sunColor.w * shadow; // sunColor.w = shadow strength
    float diffuse = ndotl * intensity * lit;

    // Sky/ambient term shifts from a bright daytime blue to a dim moonlit blue.
    vec3 ambientDay = vec3(0.42, 0.49, 0.62);
    vec3 ambientNight = vec3(0.13, 0.16, 0.25);
    vec3 ambient = mix(ambientNight, ambientDay, intensity);

    // A soft cool fill from above at night so the world stays readable (moonlight).
    float night = 1.0 - intensity;
    float moon = max(N.y, 0.0) * 0.16 * night;

    vec3 base = vColor * pc.tint.rgb;
    vec3 illum = ambient + sunCol * diffuse + vec3(0.55, 0.65, 0.9) * moon +
                 spotLighting(N, vWorldPos);
    outColor = vec4(base * illum, pc.tint.a);
}
