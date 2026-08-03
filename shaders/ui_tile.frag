#version 450

// Flat tile colour, no SDF or edge AA: raster tiles abut (and overlap by a pixel),
// so hard edges are exactly what the terrain raster wants.
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vColor;
}
