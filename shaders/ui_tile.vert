#version 450

// Instanced flat-colour tile quad (world-map terrain raster). The general ui.vert /
// ui.frag path costs one push-constant draw call PER rect, which caps the map at a
// few thousand chunky tiles; this path carries each tile's rect + colour as
// per-instance vertex data, so tens of thousands of fine tiles render in ONE draw.
// A shared pan/zoom transform (uniform scale about a pivot, then an offset - all in
// pixels) lets a cached raster keep tracking the view between rebuilds.
layout(push_constant) uniform Push {
    vec2 screen; // viewport size in px
    vec2 pivot;  // scale pivot in px (the map panel's centre at draw time)
    vec2 offset; // post-scale translation in px (pan since the raster was built)
    float scale; // uniform scale about the pivot (zoom since the raster was built)
    float pad;
} pc;

layout(location = 0) in vec4 inRect;  // xy = top-left (px), zw = size (px) at build time
layout(location = 1) in vec4 inColor; // fill rgba

layout(location = 0) out vec4 vColor;

// Unit-square corners for a two-triangle quad (triangle list, 6 vertices).
const vec2 kCorners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main() {
    vec2 pix = inRect.xy + kCorners[gl_VertexIndex] * inRect.zw;
    pix = pc.pivot + (pix - pc.pivot) * pc.scale + pc.offset;
    vColor = inColor;
    vec2 ndc = pix / pc.screen * 2.0 - 1.0; // pixel (0,0) -> NDC (-1,-1) = top-left
    gl_Position = vec4(ndc, 0.0, 1.0);
}
