#version 450

// Screen-space gradient sky + sun glow. A vertical gradient (deep zenith at the top
// of the frame down to a hazy horizon band where the far terrain meets it), plus a
// sun disc + halo at the sun's projected screen position - so it warms the top of
// the view at dawn/dusk and fades out at night. Colours come from the day/night
// cycle, so the background sky tracks dawn -> noon -> dusk -> night.
layout(push_constant) uniform Push {
    vec4 zenith;    // rgb = sky colour at the top of the frame
    vec4 horizon;   // rgb = hazy band where sky meets the far terrain
    vec4 sunColor;  // rgb = sun colour, w = intensity (0 night .. 1 noon)
    vec4 sunScreen; // xy = sun position in pixels, z = 1 if in front of the camera
    vec4 screen;    // xy = viewport size in pixels
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    float t = clamp(gl_FragCoord.y / max(pc.screen.y, 1.0), 0.0, 1.0); // 0 top .. 1 bottom
    // Gradient compressed toward the top, where the visible sky band sits above the terrain.
    vec3 sky = mix(pc.zenith.rgb, pc.horizon.rgb, smoothstep(0.0, 0.42, t));

    if (pc.sunScreen.z > 0.5) {
        float d = distance(gl_FragCoord.xy, pc.sunScreen.xy);
        float r = pc.screen.y;
        float disc = smoothstep(0.030 * r, 0.020 * r, d);      // crisp disc
        float halo = exp(-d / (0.17 * r)) * 0.7;               // soft halo bloom
        sky += pc.sunColor.rgb * (disc * 1.6 + halo) * pc.sunColor.w;
    }

    outColor = vec4(sky, 1.0);
}
