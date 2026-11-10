#version 420
// [VULKAN BACKEND 2026-11-08] wolfvkscene.cpp: composites the three layers into the swapchain image.
//   world: the finished OpenGL world (renderFinalize's output, gamma already applied by the viewer), opaque
//   scene: what Vulkan drew (the fullbright batches) in linear light with coverage in alpha
//   ui:    the HUD and the 2D interface drawn by OpenGL onto a cleared (0,0,0,0) buffer with
//          glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) (LLRender::setSceneBlendType BT_ALPHA), which leaves
//          premultiplied colour and coverage alpha: out = ui.rgb + dst * (1 - ui.a)
// The scene layer's linear colour is encoded with the viewer's own linear_to_srgb (environment/srgbF.glsl) before it
// goes over the world; the viewer's exposure and tone map are not applied to it (declared in docs/VULKAN_BACKEND.md).
layout(location = 0) in vec2 vary_uv;
layout(location = 0) out vec4 frag_color;
layout(set = 1, binding = 0) uniform sampler2D worldMap;
layout(set = 1, binding = 1) uniform sampler2D sceneMap;
layout(set = 1, binding = 2) uniform sampler2D uiMap;
layout(set = 1, binding = 3) uniform sampler2D coverageMap;   // [COVERAGE 2026-11-08] where the Vulkan pass drew (wolf_coverage)
layout(std140, set = 0, binding = 1) uniform WolfUniformsF
{
    vec4 wolf_tint;        // rgb multiplied into the scene layer when debug tint is on, a = 1 tint on / 0 off
    vec4 wolf_flags;       // x: 1 = the swapchain is an sRGB format (write linear), y: 1 = show the Vulkan scene only
};
// environment/srgbF.glsl linear_to_srgb, repeated here (this file is not stitched with the viewer's units)
vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0), vec3(1));
    bvec3 lt = lessThan(cl, vec3(0.0031308));
    vec3 low = cl * 12.92;
    vec3 high = 1.055 * pow(cl, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lt);
}
vec3 srgb_to_linear(vec3 cs)
{
    bvec3 lt = lessThan(cs, vec3(0.04045));
    vec3 low = cs / 12.92;
    vec3 high = pow((cs + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lt);
}
void main()
{
    vec4 world = texture(worldMap, vary_uv);
    vec4 scene = texture(sceneMap, vary_uv);
    vec4 ui = texture(uiMap, vary_uv);
    float coverage = texture(coverageMap, vary_uv).r;
    vec3 scene_rgb = linear_to_srgb(scene.rgb);
    vec3 base = (wolf_flags.y > 0.5) ? vec3(0.08, 0.08, 0.10) : world.rgb;
    // the Vulkan layer over the OpenGL picture wherever the Vulkan pass drew: GL's own pass overwrites (blend off,
    // alpha not written), so coverage, not the shader's alpha, decides
    vec3 colour = mix(base, scene_rgb, coverage);
    if (wolf_tint.a > 0.5 && coverage > 0.0)
    {
        // debug tint: every pixel the Vulkan pass wrote at all (any coverage) shows the tint colour plainly, so the
        // Vulkan-drawn part cannot be missed even where it is dark or thin
        colour = mix(colour, wolf_tint.rgb, 0.7);
    }
    colour = ui.rgb + colour * (1.0 - ui.a);
    if (wolf_flags.x > 0.5)
    {
        colour = srgb_to_linear(colour);   // an _SRGB swapchain re-encodes on store
    }
    frag_color = vec4(colour, 1.0);
}
