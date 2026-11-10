#version 420
// [VULKAN BACKEND 2026-11-08] wolfvkscene.cpp: the full-screen triangle for the composite (Vulkan GLSL, compiled by
// glslang -V at build time into shaders/spv/wolfvk_composite.vert.spv; not a viewer GL shader).
// One triangle covering the screen from gl_VertexIndex alone (no vertex buffer): vertices (-1,-1), (3,-1), (-1,3).
// Vulkan's framebuffer y points down and the composite reads the GL frame, whose rows start at the bottom: the texture
// coordinate's y is flipped here so the GL image lands the right way up without a second copy.
layout(location = 0) out vec2 vary_uv;
void main()
{
    vec2 p = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0, (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
    vary_uv = vec2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
}
