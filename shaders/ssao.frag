#version 450

// Screen-space ambient occlusion, computed at half resolution from the camera depth
// prepass. For every pixel we reconstruct the view-space position + normal (from depth
// derivatives - the world is flat-shaded, so they're exact within a face), then test a
// fixed hemisphere kernel of nearby points: samples that fall BEHIND known geometry
// count as occlusion. The result darkens creases, doorways and prop-ground contacts in
// the main pass - it's what visually "seats" low-poly geometry into the world.
// The raw result is noisy per-pixel (random kernel rotation); ssao_blur.frag smooths it.

layout(location = 0) out vec4 outAO; // R8 target: r = visibility (1 = fully open)

layout(set = 0, binding = 0) uniform sampler2D depthTex; // camera depth prepass (full res)

layout(push_constant) uniform Push {
    mat4 proj;    // camera projection (view -> clip)
    mat4 invProj; // clip -> view
    vec4 params;  // x = world radius (m), y = strength, z = view-space bias, w = unused
    vec4 screen;  // xy = AO target resolution (px)
} pc;

// Fixed hemisphere kernel (+Z up), lengths ramping outward so both tight creases and
// mid-range cover register. Rotated per-pixel below to trade banding for blurable noise.
const int kKernelSize = 12;
const vec3 kKernel[12] = vec3[](
    vec3( 0.13,  0.04, 0.08), vec3(-0.09,  0.11, 0.10), vec3( 0.02, -0.16, 0.13),
    vec3( 0.18,  0.15, 0.14), vec3(-0.24, -0.09, 0.19), vec3( 0.10,  0.28, 0.22),
    vec3(-0.31,  0.19, 0.26), vec3( 0.35, -0.24, 0.29), vec3(-0.16, -0.42, 0.32),
    vec3( 0.48,  0.19, 0.34), vec3(-0.45,  0.32, 0.38), vec3( 0.28,  0.60, 0.42));

vec3 viewPos(vec2 uv) {
    float d = texture(depthTex, uv).r;
    vec4 v = pc.invProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return v.xyz / v.w;
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

void main() {
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    float d = texture(depthTex, uv).r;
    if (d >= 1.0) { // sky / beyond the far plane: fully open
        outAO = vec4(1.0);
        return;
    }
    vec3 P = viewPos(uv);
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
    if (dot(N, -P) < 0.0) {
        N = -N; // face the camera
    }

    // Random per-pixel rotation of the kernel around the normal (Gram-Schmidt basis).
    float a = hash21(gl_FragCoord.xy) * 6.2831853;
    vec3 rv = vec3(cos(a), sin(a), 0.0);
    vec3 tangent = normalize(rv - N * dot(rv, N));
    mat3 tbn = mat3(tangent, cross(N, tangent), N);

    float radius = pc.params.x;
    float occ = 0.0;
    for (int i = 0; i < kKernelSize; ++i) {
        vec3 samplePos = P + tbn * (kKernel[i] * radius);
        vec4 clip = pc.proj * vec4(samplePos, 1.0);
        if (clip.w <= 0.0) {
            continue;
        }
        vec2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) {
            continue;
        }
        float surfZ = viewPos(suv).z;
        // View space looks down -Z: known geometry IN FRONT of the sample has greater z.
        // Range fade stops distant foreground silhouettes from haloing the background.
        float fade = smoothstep(0.0, 1.0, radius / max(abs(P.z - surfZ), 1e-4));
        occ += (surfZ >= samplePos.z + pc.params.z) ? fade : 0.0;
    }
    float ao = 1.0 - pc.params.y * (occ / float(kKernelSize));
    outAO = vec4(clamp(ao, 0.0, 1.0));
}
