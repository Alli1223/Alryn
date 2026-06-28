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

// Shared atmosphere globals (set 0, binding 2). The leading lighting arrays are padded out
// (water isn't dynamically lit) so the trailing fog/screen fields land at the same std140
// offsets as in mesh.frag / the Lights UBO - see Renderer::process_lights.
layout(set = 0, binding = 2) uniform Lights {
    ivec4 count;
    vec4 _pad[216]; // spots[9] (8 vec4 each) + points[48] (3 vec4 each) = 72 + 144
    vec4 playerPeek;
    vec4 camPos;
    vec4 fogColor; // rgb = fog colour, w = density
    vec4 screen;   // xy = resolution, z = gloom
} lights;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 camPos = pc.params.yzw;
    vec3 V = normalize(camPos - vWorldPos);
    vec3 L = normalize(pc.sun.xyz);
    float intensity = pc.sun.w;
    float t = pc.params.x;

    float ndv = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - ndv, 5.0); // true Schlick term, used for the alpha (transmission)

    // --- Water body: rich blue depths fading to a vibrant shallow tint on the wave faces. ---
    vec3 deep = vec3(0.02, 0.13, 0.32);
    vec3 shallow = vec3(0.10, 0.42, 0.62);
    vec3 body = mix(deep, shallow, clamp(N.y, 0.0, 1.0));
    body *= mix(0.18, 1.06, intensity);                          // dark + steely at night
    body *= mix(vec3(0.50, 0.58, 0.80), pc.sunColor.rgb, intensity);

    // --- Planar reflection: mirror the view across the wave normal and look up the sky + sun along
    // the reflected ray. A genuine reflection (it moves with the camera + ripples) that reads a
    // procedural sky matching the land's atmosphere - so it costs nothing yet gives a real
    // reflective, mirror-like surface. Stylised reflectivity (`refl`) keeps a good chunk of sky
    // visible even from the top-down iso camera, where physical fresnel would be ~2% (= flat,
    // dead water), ramping to a full mirror at grazing.
    float refl = mix(0.36, 1.0, pow(1.0 - ndv, 3.0));
    vec3 R = reflect(-V, N);
    float up = clamp(R.y, 0.0, 1.0);
    vec3 horizonCol = mix(lights.fogColor.rgb, pc.sunColor.rgb, 0.18) * mix(0.5, 1.12, intensity);
    vec3 zenithCol = mix(horizonCol, vec3(0.20, 0.42, 0.74), 0.85) * mix(0.25, 1.2, intensity);
    vec3 sky = mix(horizonCol, zenithCol, pow(up, 0.5));
    // The reflected sun: a tight disc plus a broad warm glow, smeared into a glittering streak by
    // the rippling normals.
    float rl = max(dot(R, L), 0.0);
    sky += pc.sunColor.rgb * (pow(rl, 300.0) * 7.0 + pow(rl, 22.0) * 1.0) * intensity;

    vec3 color = mix(body, sky, refl);

    // --- Specular sun glints off the wave facets. ---
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 130.0) * intensity;
    color += pc.sunColor.rgb * spec * 1.2;

    // --- Glistening sparkle: animated high-frequency highlights, densest along the sun's
    // reflection, so the water twinkles with moving bright spots. ---
    vec2 sp = vWorldPos.xz;
    float tw = sin(dot(sp, vec2(12.9, 7.3)) + t * 3.1) *
               sin(dot(sp, vec2(-5.7, 9.1)) - t * 2.3) *
               sin(dot(sp, vec2(3.1, -4.4)) + t * 1.7);
    float glint = smoothstep(0.7, 1.0, tw) * pow(rl, 16.0) * intensity;
    color += pc.sunColor.rgb * glint * 2.0;

    // --- Foam: scrolling sine bands crest into froth on the steep wave faces, broken up by noise
    // so it reads as churning foam rather than painted stripes. ---
    vec2 fp = vWorldPos.xz * 0.5;
    float fb = sin(fp.x * 1.3 + t * 0.8) * sin(fp.y * 1.1 - t * 0.6) + 0.55 * sin((fp.x + fp.y) * 0.9 + t * 1.1);
    float crest = smoothstep(0.5, 1.0, 1.0 - N.y);              // foam rides the tilted wave faces
    float foam = smoothstep(0.7, 1.0, fb) * (0.3 + 0.7 * crest);
    foam *= 0.45 + 0.55 * hash21(floor(vWorldPos.xz * 3.0) + floor(t * 2.0));
    color = mix(color, vec3(0.92, 0.96, 1.0), foam * mix(0.25, 0.82, intensity));

    // Saturation lift so the pool reads vibrant blue (matching the world re-grade), not muddy teal.
    float wlum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(wlum), color, 1.22);

    // Fold the water into the atmosphere: haze with distance and darken toward the frame edge,
    // so it sits in the same gloom as the land instead of reading as a bright cut-out.
    float dn = length(vWorldPos - lights.camPos.xyz) * lights.fogColor.w;
    float fog = clamp(1.0 - exp(-dn * dn), 0.0, 1.0);
    color = mix(color, lights.fogColor.rgb, fog);
    if (lights.screen.x >= 1.0) { // skip when the screen size is unset (headless smoke tests)
        float vigR = length(gl_FragCoord.xy / lights.screen.xy - 0.5);
        color *= mix(1.0, smoothstep(0.86, 0.32, vigR), 0.34 + 0.18 * lights.screen.z);
    }

    // The more it reflects, the less it transmits: fairly transparent looking down (so the lily
    // pads / fish / coral read through it), near-opaque mirror at grazing.
    float alpha = mix(0.62, 0.96, refl);
    outColor = vec4(color, alpha);
}
