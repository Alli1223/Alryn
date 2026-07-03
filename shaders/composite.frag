#version 450

// Final composite to the swapchain: the (already tonemapped) scene, screen-blended
// with the blurred bloom and the sun-tinted god rays. Screen blend (1-(1-a)(1-b))
// brightens without clipping, so heavy bloom stays soft instead of blowing out.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D raysTex;

layout(push_constant) uniform Push {
    vec4 sun_color; // rgb = sun colour (tints the god rays)
    vec4 params;    // x = bloom strength, y = rays strength
    vec4 screen;    // xy = target resolution (px)
} pc;

void main() {
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    vec3 scene = texture(sceneTex, uv).rgb;
    vec3 bloom = clamp(texture(bloomTex, uv).rgb * pc.params.x, 0.0, 1.0);
    vec3 rays = clamp(pc.sun_color.rgb * (texture(raysTex, uv).r * pc.params.y), 0.0, 1.0);
    vec3 col = 1.0 - (1.0 - scene) * (1.0 - bloom) * (1.0 - rays);
    outColor = vec4(col, 1.0);
}
