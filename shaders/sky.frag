#version 450

// Screen-space sky: a vertical gradient (deep zenith at the top of the frame down to a
// hazy horizon band where the far terrain meets it), plus - all tracking the day/night
// cycle - a sun disc + halo, a crescent moon opposite the sun, a twinkling starfield
// that fades in with darkness, and two layers of drifting clouds whose coverage follows
// the same cloud-cover value that drives the ground's cloud shadows (so sky and ground
// agree, and a storm reads as a dark grey deck overhead).
layout(push_constant) uniform Push {
    vec4 zenith;     // rgb = sky colour at the top of the frame
    vec4 horizon;    // rgb = hazy band where sky meets the far terrain
    vec4 sunColor;   // rgb = sun colour, w = intensity (0 night .. 1 noon)
    vec4 sunScreen;  // xy = sun position in pixels, z = 1 if in front of the camera
    vec4 moonScreen; // xy = moon position in pixels, z = 1 if in front of the camera
    vec4 params;     // x = time (s), y = cloud cover 0..1, z = night amount 0..1
    vec4 screen;     // xy = viewport size in pixels
} pc;

layout(location = 0) out vec4 outColor;

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

void main() {
    float t = clamp(gl_FragCoord.y / max(pc.screen.y, 1.0), 0.0, 1.0); // 0 top .. 1 bottom
    // Gradient compressed toward the top, where the visible sky band sits above the terrain.
    vec3 sky = mix(pc.zenith.rgb, pc.horizon.rgb, smoothstep(0.0, 0.42, t));
    float night = pc.params.z;

    // Starfield: one candidate star per screen cell, fading in with darkness and out
    // toward the hazy horizon; each twinkles to its own rhythm.
    if (night > 0.2) {
        vec2 g = gl_FragCoord.xy / 14.0;
        vec2 cell = floor(g);
        float h = hash21(cell);
        if (h > 0.82) {
            vec2 sp = (vec2(hash21(cell + 1.3), hash21(cell + 2.7)) - 0.5) * 0.6;
            float d = length(fract(g) - 0.5 - sp);
            float twinkle = 0.75 + 0.25 * sin(pc.params.x * (1.5 + 3.0 * h) + h * 40.0);
            float star = smoothstep(0.10, 0.02, d) * twinkle;
            float band = 1.0 - smoothstep(0.0, 0.45, t);
            sky += vec3(0.9, 0.95, 1.0) * star * (night - 0.2) * 1.25 * band;
        }
    }

    // Crescent moon (opposite the sun): a cool disc with a dark offset disc biting the
    // crescent out, plus a faint halo.
    if (pc.moonScreen.z > 0.5 && night > 0.05) {
        float r = pc.screen.y;
        float d = distance(gl_FragCoord.xy, pc.moonScreen.xy);
        float disc = smoothstep(0.022 * r, 0.016 * r, d);
        float bite = smoothstep(0.026 * r, 0.017 * r,
                                distance(gl_FragCoord.xy, pc.moonScreen.xy + vec2(0.011, -0.005) * r));
        float halo = exp(-d / (0.10 * r)) * 0.18;
        sky += vec3(0.82, 0.87, 0.95) * (max(disc - bite * 0.85, 0.0) * 1.1 + halo) * night;
    }

    // Sun disc + halo (dawn/dusk it sits near the top of the frame and warms it).
    if (pc.sunScreen.z > 0.5) {
        float d = distance(gl_FragCoord.xy, pc.sunScreen.xy);
        float r = pc.screen.y;
        float disc = smoothstep(0.030 * r, 0.020 * r, d);      // crisp disc
        float halo = exp(-d / (0.17 * r)) * 0.7;               // soft halo bloom
        sky += pc.sunColor.rgb * (disc * 1.6 + halo) * pc.sunColor.w;
    }

    // Drifting clouds, drawn LAST so a heavy deck veils the sun/moon/stars. Two fbm
    // layers scroll at different speeds; coverage slides the threshold (same value that
    // mottles the ground with cloud shadows) and a storm deck greys + darkens.
    float cover = pc.params.y;
    if (cover > 0.02) {
        vec2 suv = gl_FragCoord.xy / pc.screen.y;
        float n = 0.65 * fbm(suv * vec2(3.0, 6.0) + vec2(pc.params.x * 0.008, 0.0)) +
                  0.35 * fbm(suv * vec2(6.0, 12.0) - vec2(pc.params.x * 0.014, 0.0));
        float edge = mix(0.68, 0.34, cover);
        float cl = smoothstep(edge, edge + 0.25, n);
        float band = 1.0 - smoothstep(0.05, 0.5, t); // clouds live in the sky band
        vec3 cloudCol = mix(pc.horizon.rgb * 1.06, pc.zenith.rgb * 0.55 + vec3(0.18), 0.5);
        cloudCol = mix(cloudCol, vec3(dot(cloudCol, vec3(0.333))) * 0.75,
                       smoothstep(0.6, 1.0, cover)); // storm decks grey out
        cloudCol *= 1.0 - 0.65 * night;              // and go dark at night
        sky = mix(sky, cloudCol, cl * band * 0.85);
    }

    outColor = vec4(sky, 1.0);
}
