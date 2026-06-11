#version 450

// Vertexless 2D UI quad. Emits a screen-space rectangle (the bounding box of the
// shape being drawn) from gl_VertexIndex; the fragment shader does the actual
// rounded-rect / capsule SDF so panels, buttons and the vector font all share one
// pipeline. Pixel coordinates have their origin at the top-left of the window.
layout(push_constant) uniform Push {
    vec4 rect;   // xy = top-left (px), zw = size (px) of the quad to rasterize
    vec4 color;  // fill rgba (straight alpha)
    vec4 params; // x = corner radius px, y = edge softness px, z = mode (0 rect, 1 segment), w = border/half-thickness px
    vec4 seg;    // xy = p0, zw = p1 (px) for segment/capsule mode
    vec4 border; // border rgba (rect mode, when params.w > 0)
    vec2 screen; // viewport size in px
} pc;

layout(location = 0) out vec2 vPix; // fragment position in pixels

// Unit-square corners for a two-triangle quad (triangle list, 6 vertices).
const vec2 kCorners[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main() {
    vec2 pix = pc.rect.xy + kCorners[gl_VertexIndex] * pc.rect.zw;
    vPix = pix;
    vec2 ndc = pix / pc.screen * 2.0 - 1.0; // pixel (0,0) -> NDC (-1,-1) = top-left
    gl_Position = vec4(ndc, 0.0, 1.0);
}
