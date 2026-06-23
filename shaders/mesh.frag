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
// An unshadowed light: illuminates but casts no shadow (no atlas tile).
struct Point {
    vec4 posRange;
    vec4 dirCosInner;
    vec4 colorCosOuter;
};
layout(set = 0, binding = 2) uniform Lights {
    ivec4 count; // x = shadow-casting spot count, y = unshadowed point count
    Spot spots[9];
    Point points[48];
    vec4 playerPeek; // (unused here; kept so the std140 offsets match the shared UBO)
    vec4 camPos;     // xyz = camera position (world)
    vec4 fogColor;   // rgb = atmospheric fog/haze colour, w = density
    vec4 screen;     // xy = framebuffer resolution (px), z = town "gloom" 0..1
    vec4 fogVolume;  // x = road fog-bank strength 0..1, y = ground reference height (player feet)
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

// Sum of the unshadowed point lights (cone + falloff, no occlusion test). These are
// every light past the nearest few, so the whole town stays lit when zoomed out.
vec3 pointLighting(vec3 N, vec3 wpos) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < lights.count.y; ++i) {
        Point s = lights.points[i];
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
        sum += s.colorCosOuter.rgb * (ndl * cone * atten);
    }
    return sum;
}

// --- Cinematic atmosphere (shared with foliage/water) -------------------------------------
// Filmic tonemap (ACES approximation) - compresses bright lit/multi-light areas gracefully
// instead of harsh clipping, the backbone of the moodier, less-flat look.
vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
// Colour grade: push SATURATION up for a vibrant, punchy world (a little less inside the town
// "gloom"), a gentle warm/cool split-tone, and an S-curve for good contrast. This counteracts the
// flattening of the ACES tonemap so roofs, ground and foliage read rich rather than greywashed.
vec3 grade(vec3 col, float gloom) {
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = mix(vec3(lum), col, 1.28 - 0.24 * gloom);     // >1 = saturate (vibrant)
    // A stronger warm/cool split-tone: golden sunlit highlights, cool blue shadows. The warm-vs-cool
    // contrast both warms the image and reads as depth (aerial-perspective cue).
    vec3 shadowTint = vec3(0.90, 0.96, 1.13);           // cool blue shadows
    vec3 highTint = vec3(1.12, 1.03, 0.84);             // warm golden highlights
    col *= mix(shadowTint, highTint, smoothstep(0.0, 0.6, lum));
    col = mix(col, col * col * (3.0 - 2.0 * col), 0.42); // S-curve contrast (punchier)
    return col;
}
// Cheap value-noise fbm for the drifting fog wisps.
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0)), d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) { v += a * vnoise(p); p = p * 2.0 + 7.1; a *= 0.5; }
    return v;
}
// Exponential distance haze toward the atmosphere colour - the biggest depth cue - PLUS an
// optional dense, drifting, ground-hugging fog BANK (the occasional road mist). The bank keeps a
// clear bubble around the player then walls in fast, modulated by animated wisps for a volumetric
// feel and height-attenuated so it pools on the ground while tall geometry pokes out of it.
float fogFactor(vec3 wpos) {
    float dist = length(wpos - lights.camPos.xyz);
    float dn = dist * lights.fogColor.w;
    float f = 1.0 - exp(-dn * dn);

    float bank = lights.fogVolume.x;
    if (bank > 0.001) {
        vec2 drift = vec2(0.5, 0.3) * pc.params.x;       // the bank rolls over time
        float wisp = fbm(wpos.xz * 0.09 + drift * 0.18); // rolling thick/thin wisps
        float hug = clamp(1.0 - max(wpos.y - lights.fogVolume.y, 0.0) * 0.14, 0.05, 1.0);
        float pd = lights.fogColor.w * (2.4 + 5.0 * bank) * (0.4 + 1.0 * wisp) * hug;
        float fd = max(dist - 5.0, 0.0);                 // clear bubble around the camera/player
        f = max(f, (1.0 - exp(-pow(fd * pd, 2.0))) * bank);
    }
    return clamp(f, 0.0, 1.0);
}
// Soft radial vignette to pull the eye in and darken the frame edges (cinematic framing).
float vignette() {
    if (lights.screen.x < 1.0) {
        return 1.0; // screen size unset (e.g. headless smoke tests) -> no vignette
    }
    float r = length(gl_FragCoord.xy / lights.screen.xy - 0.5);
    float strength = 0.34 + 0.18 * lights.screen.z; // gloomier towns get a heavier frame
    return mix(1.0, smoothstep(0.86, 0.32, r), strength);
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

    // Hemispheric ambient: sky-tinted from above, darker/earthier from below. Kept LOW in daylight
    // so shadowed + downward faces and cast shadows fall genuinely dark (the strong key sun below
    // does the lifting) - that ambient/sun contrast is what gives form + depth instead of a flat,
    // evenly-filled look.
    vec3 skyAmb = mix(vec3(0.10, 0.13, 0.21), vec3(0.19, 0.26, 0.40), intensity);   // up (cool sky fill)
    vec3 groundAmb = mix(vec3(0.04, 0.045, 0.06), vec3(0.11, 0.085, 0.055), intensity); // down (dim earth bounce)
    float hemi = N.y * 0.5 + 0.5;
    vec3 ambient = mix(groundAmb, skyAmb, hemi);

    // A soft cool fill from above at night so the world stays readable (moonlight).
    float night = 1.0 - intensity;
    float moon = max(N.y, 0.0) * 0.24 * night;

    vec3 base = vColor * pc.tint.rgb;
    vec3 illum = ambient + sunCol * diffuse * 1.35 + vec3(0.55, 0.65, 0.9) * moon +
                 spotLighting(N, vWorldPos) + pointLighting(N, vWorldPos);

    vec3 col = base * illum;
    col = mix(col, lights.fogColor.rgb, fogFactor(vWorldPos)); // atmospheric haze
    col = acesFilm(col * 1.05);                                // exposure + filmic tonemap
    col = grade(col, lights.screen.z);                         // split-tone + contrast
    col *= vignette();
    outColor = vec4(col, pc.tint.a);
}
