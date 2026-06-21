#version 450

// Tree-canopy fragment shader: the same lit + shadowed look as mesh.frag, plus a
// "peek-through" cutout. Canopy leaves growing BETWEEN the camera and the player (i.e.
// occluding them) dissolve away through an ordered (Bayer) dither, so it looks like the
// camera is peeking through the leaves to keep the character visible. Used ONLY by the
// alpha-blended tree-foliage pipeline - tree trunks/branches and ground vegetation use
// mesh.frag and stay solid, so the peek only thins the leafy tops, not the whole scene.

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec4 vShadowCoord;
layout(location = 3) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D shadowMap;   // sun
layout(set = 0, binding = 1) uniform sampler2D lightAtlas;  // spot lights (tiled)

struct Spot {
    vec4 posRange;
    vec4 dirCosInner;
    vec4 colorCosOuter;
    vec4 atlas;
    mat4 viewProj;
};
struct Point {
    vec4 posRange;
    vec4 dirCosInner;
    vec4 colorCosOuter;
};
layout(set = 0, binding = 2) uniform Lights {
    ivec4 count;
    Spot spots[9];
    Point points[48];
    vec4 playerPeek; // xyz = player focus (world), w = tunnel radius (0 disables)
    vec4 camPos;     // xyz = camera position (world)
} lights;

layout(push_constant) uniform Push {
    mat4 mvp;
    mat4 model;
    mat4 lightVP;
    vec4 tint;
    vec4 params;
    vec4 sun;
    vec4 sunColor;
} pc;

float shadowOcclusion(vec4 coord, float ndotl) {
    vec3 p = coord.xyz / coord.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || p.z > 1.0 || p.z < 0.0) {
        return 0.0;
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
    vec2 auv = s.atlas.xy + uv * s.atlas.zw;
    float d = texture(lightAtlas, auv).r;
    return (p.z - 0.0018 > d) ? 1.0 : 0.0;
}

vec3 spotLighting(vec3 N, vec3 wpos) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < lights.count.x; ++i) {
        Spot s = lights.spots[i];
        vec3 toL = s.posRange.xyz - wpos;
        float dist = length(toL);
        if (dist > s.posRange.w) continue;
        vec3 L = toL / max(dist, 1e-4);
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        float cosA = dot(-L, s.dirCosInner.xyz);
        float cone = smoothstep(s.colorCosOuter.w, s.dirCosInner.w, cosA);
        if (cone <= 0.0) continue;
        float atten = clamp(1.0 - dist / s.posRange.w, 0.0, 1.0);
        atten *= atten;
        float occ = spotOcclusion(s, wpos);
        sum += s.colorCosOuter.rgb * (ndl * cone * atten * (1.0 - occ));
    }
    return sum;
}

vec3 pointLighting(vec3 N, vec3 wpos) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < lights.count.y; ++i) {
        Point s = lights.points[i];
        vec3 toL = s.posRange.xyz - wpos;
        float dist = length(toL);
        if (dist > s.posRange.w) continue;
        vec3 L = toL / max(dist, 1e-4);
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        float cosA = dot(-L, s.dirCosInner.xyz);
        float cone = smoothstep(s.colorCosOuter.w, s.dirCosInner.w, cosA);
        if (cone <= 0.0) continue;
        float atten = clamp(1.0 - dist / s.posRange.w, 0.0, 1.0);
        atten *= atten;
        sum += s.colorCosOuter.rgb * (ndl * cone * atten);
    }
    return sum;
}

// How strongly this fragment should dissolve to reveal the player: 1 deep inside a slim
// "tunnel" running straight from the camera to the player, fading to 0 at the tunnel's
// edge. Only geometry actually BETWEEN the camera and the player (within a small radius of
// that line) is affected - everything else is untouched.
float peekAmount() {
    float radius = lights.playerPeek.w;
    if (radius <= 0.001) return 0.0;
    vec3 C = lights.camPos.xyz;
    vec3 P = lights.playerPeek.xyz;
    vec3 cp = P - C;
    float lenCP = length(cp);
    if (lenCP <= 0.001) return 0.0;
    vec3 dir = cp / lenCP;
    float t = dot(vWorldPos - C, dir);            // distance along the camera->player ray
    if (t <= 0.4 || t >= lenCP - 0.2) return 0.0; // only what's in front of the player
    vec3 closest = C + dir * t;
    float perp = length(vWorldPos - closest);     // distance from the sight line
    float r = radius * mix(0.9, 1.7, t / lenCP);  // a broad cone, widest near the player
    return 1.0 - smoothstep(r * 0.8, r, perp);    // mostly full cull, soft only at the rim
}

// 4x4 ordered (Bayer) dither threshold in (0,1) from the pixel coordinate - stable as the
// camera moves, giving a clean stippled dissolve rather than fizzing noise.
float bayerThreshold() {
    const float m[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                  3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 p = ivec2(gl_FragCoord.xy) & 3;
    return (m[p.y * 4 + p.x] + 0.5) / 16.0;
}

void main() {
    float peek = peekAmount();
    // Dither dissolve: discard a growing fraction of pixels toward the tunnel core, so the
    // foliage melts into a soft stippled hole the camera sees the character through.
    if (peek > bayerThreshold()) {
        discard;
    }

    vec3 N = normalize(vWorldNormal);
    vec3 L = normalize(pc.sun.xyz);
    float intensity = pc.sun.w;
    vec3 sunCol = pc.sunColor.rgb;

    float ndotl = max(dot(N, L), 0.0);
    float shadow = shadowOcclusion(vShadowCoord, ndotl);
    float lit = 1.0 - pc.sunColor.w * shadow;
    float diffuse = ndotl * intensity * lit;

    vec3 ambientDay = vec3(0.42, 0.49, 0.62);
    vec3 ambientNight = vec3(0.20, 0.23, 0.33);
    vec3 ambient = mix(ambientNight, ambientDay, intensity);

    float night = 1.0 - intensity;
    float moon = max(N.y, 0.0) * 0.24 * night;

    vec3 base = vColor * pc.tint.rgb;
    vec3 illum = ambient + sunCol * diffuse + vec3(0.55, 0.65, 0.9) * moon +
                 spotLighting(N, vWorldPos) + pointLighting(N, vWorldPos);

    // A cool aqua rim on the surviving fringe of the hole, so the cut reads as the camera
    // "pushing through" the leaves rather than them simply vanishing.
    float rim = smoothstep(0.1, 0.7, peek);
    illum += vec3(0.30, 0.65, 0.85) * rim * 0.6;

    // Blended foliage also softens its alpha toward the hole (ignored by the opaque
    // vegetation pipeline, where the dither discard does the work).
    float alpha = pc.tint.a * (1.0 - peek * 0.85);
    outColor = vec4(base * illum, alpha);
}
