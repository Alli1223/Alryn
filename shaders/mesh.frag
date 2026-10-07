#version 450

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vShadowCoord;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in float vPave; // 0..1 cobblestone paving weight (terrain in towns)

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;   // sun
layout(set = 0, binding = 1) uniform sampler2D lightAtlas;  // spot lights (tiled)
layout(set = 0, binding = 3) uniform sampler2D ssaoMap;     // screen-space AO (1 = open)

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
    vec4 playerPeek; // xyz = player position (world); used as the fog reference (zoom-independent)
    vec4 camPos;     // xyz = camera position (world)
    vec4 fogColor;   // rgb = atmospheric fog/haze colour, w = density
    vec4 screen;     // xy = framebuffer resolution (px), z = town "gloom" 0..1
    vec4 fogVolume;  // x = road fog-bank 0..1, y = ground ref height, z = cloud cover 0..1, w = wind
    vec4 extra;      // xy = projection depth terms (water), z = ground wetness 0..1
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

// Stable per-pixel random used to rotate the shadow taps (distinct from hash21 below,
// which feeds the fbm noise; GLSL wants declaration-before-use in file order).
float shadowJitter(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Fraction of the fragment in shadow (0 = lit, 1 = fully shadowed): an 8-tap Poisson
// disk, rotated per pixel - a softer, wider penumbra than a box PCF at the same cost,
// with the rotation turning banding into gentle (visually blurred) noise.
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
    const vec2 kPoisson[8] = vec2[](
        vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457),
        vec2(-0.203, 0.621), vec2(0.962, -0.195), vec2(0.473, -0.480),
        vec2(0.519, 0.767), vec2(0.185, -0.893));
    float a = shadowJitter(gl_FragCoord.xy) * 6.2831853;
    float ca = cos(a), sa = sin(a);
    mat2 rot = mat2(ca, sa, -sa, ca);
    float sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        float d = texture(shadowMap, uv + rot * kPoisson[i] * texel * 2.2).r;
        sum += (p.z - bias > d) ? 1.0 : 0.0;
    }
    return sum / 8.0;
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
    vec3 shadowTint = vec3(0.94, 0.97, 1.09);           // gently cool shadows (not steely)
    vec3 highTint = vec3(1.15, 1.04, 0.82);             // warm golden highlights
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
    // Distance haze is measured from the PLAYER, not the camera: with the iso follow-cam, zooming
    // out moves the eye far back, and a camera-relative fog would then haze the whole scene around
    // the player (washing out the wagon lamp + other lights). Player-relative fog keeps the near
    // scene clear + the lights at a constant intensity whether the camera is close or zoomed out,
    // while distant terrain (far from the player too) still fades for aerial perspective.
    float dist = length(wpos - lights.playerPeek.xyz);
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
// Drifting cloud shadows: a slow-scrolling fbm "cloud deck" (~120m up) projected along the
// sun direction onto the world modulates the sun's diffuse term, so soft shadow patches roam
// the ground and break up big, uniformly-lit midday areas (the main flat-noon fix).
// fogVolume.z = cloud cover 0..1 (storms overcast), fogVolume.w = wind strength (drift speed).
float cloudShadow(vec3 wpos) {
    float cover = lights.fogVolume.z;
    if (cover <= 0.001) {
        return 1.0;
    }
    // Where a ray from this point toward the sun pierces the cloud deck (parallax with height).
    vec2 cp = wpos.xz + pc.sun.xz * ((120.0 - wpos.y) / max(pc.sun.y, 0.2));
    vec2 drift = vec2(1.0, 0.6) * pc.params.x * (0.5 + 2.2 * lights.fogVolume.w);
    float n = fbm(cp * 0.011 + drift * 0.012);
    // More cover slides the threshold down, so more of the noise field reads as cloud.
    float edge = mix(0.72, 0.30, cover);
    float cloud = smoothstep(edge, edge + 0.22, n);
    return 1.0 - cloud * (0.32 + 0.26 * cover);
}
// --- Procedural cobblestones ------------------------------------------------------------------
// Town streets + plazas are paved with rounded setts laid over the packed-earth street colour: a
// jittered Voronoi tiling (each cell one stone), so the pattern needs no UVs and runs seamlessly over
// the terrain whatever way a street turns. Each stone gets its own tone, a domed normal (so the low
// sun picks out every cobble), and earth/moss in the joints; where the paving thins out at its edge
// whole stones drop out at random, so the cobbles fray into the dirt rather than ending on a line.
vec2 hash22(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}
struct Cobble {
    vec2 id;     // the stone's cell (for per-stone variation)
    vec2 offset; // fragment position relative to the stone centre (cells)
    float edge;  // distance to the nearest joint (cells; 0 at the joint)
};
Cobble cobbleAt(vec2 p) {
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    // Pass 1: the nearest stone centre.
    float d1 = 8.0;
    vec2 best = vec2(0.0), bestR = vec2(0.0);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(float(i), float(j));
            vec2 r = g + 0.12 + 0.76 * hash22(ip + g) - fp;
            float d = dot(r, r);
            if (d < d1) {
                d1 = d;
                best = ip + g;
                bestR = r;
            }
        }
    }
    // Pass 2: the true distance to the nearest Voronoi edge (the joint).
    float edge = 8.0;
    vec2 bg = best - ip;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 g = bg + vec2(float(i), float(j));
            vec2 r = g + 0.12 + 0.76 * hash22(ip + g) - fp;
            vec2 dr = r - bestR;
            if (dot(dr, dr) > 1e-5) {
                edge = min(edge, dot(0.5 * (bestR + r), normalize(dr)));
            }
        }
    }
    Cobble c;
    c.id = best;
    c.offset = -bestR;
    c.edge = edge;
    return c;
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

    // Cobblestones: decide the stone/joint mix + the stone's own colour and domed normal up front, so
    // the lighting below picks the relief up for free.
    float stone = 0.0;     // 0 = no paving here (or a joint), 1 = the top of a stone
    float joint = 0.0;     // 1 inside a joint between stones
    vec3 stoneCol = vec3(0.0);
    // (The stone-space coordinate + its screen derivative are taken OUTSIDE the branch below:
    // derivatives are undefined in non-uniform control flow, and the paving edge is exactly that.)
    // A gentle drift in stone size across a town keeps the setts from reading as one stamp.
    float cobbleSize = mix(0.30, 0.40, vnoise(vWorldPos.xz * 0.05));
    vec2 sp = vWorldPos.xz / cobbleSize;
    float px = length(fwidth(sp));
    if (vPave > 0.02) {
        Cobble cb = cobbleAt(sp);
        float h = hash22(cb.id * 1.37 + 4.1).x;
        float h2 = hash22(cb.id * 2.11 + 9.7).y;
        // Fade the pattern out where a stone shrinks to a couple of pixels (no shimmer at a distance).
        float detail = 1.0 - smoothstep(0.18, 0.55, px);
        // Fraying edge: each stone appears once the paving weight passes its own threshold.
        float present = smoothstep(h * 0.55 + 0.2, h * 0.55 + 0.32, vPave);
        float gap = 0.075 + 0.05 * h2;                         // joint half-width (cells)
        float aa = max(px * 0.75, 0.01);
        float top = smoothstep(gap - aa, gap + aa, cb.edge);   // 0 in the joint .. 1 on the stone
        stone = present * mix(1.0, top, detail);
        joint = present * (1.0 - top) * detail;
        // Each stone's tone: warm-grey granite with a few darker / sandier / bluish ones mixed in.
        // (Kept well below the blow-out range: the ACES + warm grade turns bright albedo cream.)
        vec3 granite = vec3(0.355, 0.345, 0.335);
        vec3 tones = mix(vec3(0.80, 0.80, 0.80), vec3(1.12, 1.07, 1.0), h);
        if (h2 > 0.82) tones *= vec3(0.84, 0.88, 0.96);      // the odd cool slate
        if (h2 < 0.12) tones *= vec3(1.08, 0.98, 0.82);      // the odd sandy one
        float grain = 0.92 + 0.16 * vnoise(vWorldPos.xz * 9.0 + cb.id); // speckled granite
        stoneCol = granite * tones * grain;
        // Worn smooth on top, darker toward the joint (mud + shade).
        stoneCol *= mix(0.78, 1.0, smoothstep(0.0, 0.35, cb.edge));
        // A domed normal: tilt away from the stone centre, more toward its rim.
        float rim = clamp(length(cb.offset) * 1.6, 0.0, 1.0);
        vec3 tilt = vec3(cb.offset.x, 0.0, cb.offset.y) * (0.55 + 0.6 * rim) * detail * stone;
        N = normalize(N + tilt);
    }
    vec3 sunCol = pc.sunColor.rgb;

    float ndotl = max(dot(N, L), 0.0);
    float shadow = shadowOcclusion(vShadowCoord, ndotl);
    float lit = 1.0 - pc.sunColor.w * shadow; // sunColor.w = shadow strength
    float diffuse = ndotl * intensity * lit * cloudShadow(vWorldPos);

    // Hemispheric ambient: sky-tinted from above, darker/earthier from below. Kept LOW in daylight
    // so shadowed + downward faces and cast shadows fall genuinely dark (the strong key sun below
    // does the lifting) - that ambient/sun contrast is what gives form + depth instead of a flat,
    // evenly-filled look.
    vec3 skyAmb = mix(vec3(0.10, 0.13, 0.21), vec3(0.26, 0.30, 0.40), intensity);   // up (soft warm-blue sky fill)
    vec3 groundAmb = mix(vec3(0.04, 0.045, 0.06), vec3(0.17, 0.125, 0.075), intensity); // down (warm earth bounce)
    float hemi = N.y * 0.5 + 0.5;
    vec3 ambient = mix(groundAmb, skyAmb, hemi);

    // A soft cool fill from above at night so the world stays readable (moonlight).
    float night = 1.0 - intensity;
    float moon = max(N.y, 0.0) * 0.24 * night;

    // Screen-space AO: fully scales the ambient (occluded creases lose their fill light)
    // and partially scales the sun (a corner under the eaves still darkens at noon).
    float ssao = lights.screen.x > 1.0
                     ? texture(ssaoMap, gl_FragCoord.xy / lights.screen.xy).r
                     : 1.0; // screen size unset (headless tests) -> AO off

    vec3 base = vColor * pc.tint.rgb;
    if (vPave > 0.02) {
        // Joints: dark packed earth (only a hint of whatever ground lies under the paving), with moss
        // creeping into them in the odd damp patch; stone tops: the stone's own colour.
        vec3 jointCol = mix(vec3(0.20, 0.165, 0.125), vColor * 0.5, 0.15);
        float moss = smoothstep(0.62, 0.8, vnoise(vWorldPos.xz * 0.45));
        jointCol = mix(jointCol, vec3(0.16, 0.22, 0.11), 0.6 * moss);
        base = mix(base, jointCol, joint);
        base = mix(base, stoneCol * pc.tint.rgb, stone);
    }
    // Rain-soaked world (extra.z): upward faces darken + cool while wet, like real
    // drenched earth and stone. Puddle sheen is layered on after lighting, below.
    float wet = lights.extra.z;
    float soak = wet * smoothstep(0.55, 0.9, N.y);
    base *= mix(vec3(1.0), vec3(0.60, 0.63, 0.68), soak * 0.75);

    vec3 illum = ambient * ssao + sunCol * diffuse * mix(1.0, ssao, 0.35) * 1.35 +
                 vec3(0.55, 0.65, 0.9) * moon +
                 spotLighting(N, vWorldPos) + pointLighting(N, vWorldPos);

    vec3 col = base * illum;

    // Puddles: a slow noise mask collects on near-flat ground, reflecting a soft
    // sky tint (stronger at glancing view angles) with a tight sun glint - the
    // world visibly SHINES after rain instead of just darkening.
    if (wet > 0.01) {
        float pud = smoothstep(0.60, 0.72, fbm(vWorldPos.xz * 0.35)) *
                    smoothstep(0.93, 0.995, N.y) * wet;
        // Rain pools in the joints between the cobbles first, and wet stone tops glisten.
        pud = max(pud, joint * wet * 0.85);
        pud = max(pud, stone * wet * 0.25);
        if (pud > 0.001) {
            vec3 V = normalize(lights.camPos.xyz - vWorldPos);
            float fres = pow(1.0 - max(dot(N, V), 0.0), 2.0);
            vec3 skyTint = mix(vec3(0.35, 0.42, 0.55), lights.fogColor.rgb * 1.4, 0.5);
            float glintSun = pow(max(dot(N, normalize(L + V)), 0.0), 90.0) * intensity;
            vec3 puddleCol = skyTint * (0.45 + 0.55 * intensity) + sunCol * glintSun * 2.0;
            col = mix(col, puddleCol, pud * (0.35 + 0.45 * fres));
        }
    }
    col = mix(col, lights.fogColor.rgb, fogFactor(vWorldPos)); // atmospheric haze
    col = acesFilm(col * 1.05);                                // exposure + filmic tonemap
    col = grade(col, lights.screen.z);                         // split-tone + contrast
    col *= vignette();
    outColor = vec4(col, pc.tint.a);
}
