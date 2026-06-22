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

void main() {
    vec3 N = normalize(vNormal);
    vec3 camPos = pc.params.yzw;
    vec3 V = normalize(camPos - vWorldPos);
    vec3 L = normalize(pc.sun.xyz);
    float intensity = pc.sun.w;

    // Fresnel: more opaque/reflective at grazing angles.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);

    vec3 deep = vec3(0.05, 0.26, 0.55);    // rich blue depths
    vec3 shallow = vec3(0.18, 0.52, 0.74); // vibrant shallow blue
    vec3 color = mix(deep, shallow, clamp(N.y, 0.0, 1.0));

    // Daylight tints the water and lifts it; at night it goes dark and steely.
    color *= mix(0.22, 1.08, intensity);
    color *= mix(vec3(0.55, 0.6, 0.8), pc.sunColor.rgb, intensity);

    // Specular sun glints off the wave facets + a sky-reflection lift at grazing.
    vec3 H = normalize(L + V);
    float ndh = max(dot(N, H), 0.0);
    float spec = pow(ndh, 64.0) * intensity;
    color += pc.sunColor.rgb * spec * 0.8;
    color += fresnel * 0.25 * mix(0.2, 1.0, intensity);

    // Glistening sparkle: animated high-frequency highlights, densest along the
    // sun's reflection, so the water twinkles with moving bright spots.
    float t = pc.params.x;
    vec2 sp = vWorldPos.xz;
    float tw = sin(dot(sp, vec2(12.9, 7.3)) + t * 3.1) *
               sin(dot(sp, vec2(-5.7, 9.1)) - t * 2.3) *
               sin(dot(sp, vec2(3.1, -4.4)) + t * 1.7);
    float glint = smoothstep(0.72, 1.0, tw) * pow(ndh, 20.0) * intensity;
    color += pc.sunColor.rgb * glint * 2.2;

    // Saturation lift so the pool reads vibrant blue (matching the world re-grade), not muddy teal.
    float wlum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(wlum), color, 1.28);

    // Fold the water into the atmosphere: haze with distance and darken toward the frame edge,
    // so it sits in the same gloom as the land instead of reading as a bright cut-out.
    float dn = length(vWorldPos - lights.camPos.xyz) * lights.fogColor.w;
    float fog = clamp(1.0 - exp(-dn * dn), 0.0, 1.0);
    color = mix(color, lights.fogColor.rgb, fog);
    if (lights.screen.x >= 1.0) { // skip when the screen size is unset (headless smoke tests)
        float vigR = length(gl_FragCoord.xy / lights.screen.xy - 0.5);
        color *= mix(1.0, smoothstep(0.86, 0.32, vigR), 0.34 + 0.18 * lights.screen.z);
    }

    float alpha = mix(0.5, 0.92, fresnel);
    outColor = vec4(color, alpha);
}
