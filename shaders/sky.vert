#version 450

// Vertexless fullscreen-triangle sky. The fragment shader does a screen-space
// gradient + sun glow (the iso camera pitches down, so a world-ray gradient would
// never show sky - the visible "sky" is the background band above the far terrain).
// Drawn FIRST in the main pass with depth test + write OFF, so it fills the
// background and the scene geometry draws over it.
const vec2 kVerts[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    gl_Position = vec4(kVerts[gl_VertexIndex], 1.0, 1.0); // far plane; depth test off
}
