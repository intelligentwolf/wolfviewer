/**
 * @file wolftk/fogSigmaF.glsl
 * @brief The native fog's mean extinction along each opaque pixel's ray, for the surfaces drawn after the composite.
 *
 * WolfViewer (IntelligentWolf Ltd, 2026-10-29, ticket 349: Jimmy Olsen at Wolf Territories Home in heavy native fog,
 * "some objects are almost like highlighted and others are hidden from fog"). The clouds' overlay (wolftk/cloudsF.glsl:
 * the clouds, the fog genus and the haze between the camera and the opaque scene, getRayDistanceToScene) is laid over the
 * opaque frame by wolftk/compositeF.glsl (out = radiance x (1 - overlay.a) + overlay.rgb) BEFORE the transparent
 * surfaces, the water and the particles are drawn (pipeline.cpp), so none of those got that fog. They take it from here
 * (wolftk/atmosphereLibF.glsl wolfTkNativeFog): this pass writes, per pixel whose depth is a surface, the overlay's
 * opacity a over the ray's length D as the mean extinction of a uniform medium, sigma = -ln(1 - a) / D (Beer-Lambert:
 * a = 1 - exp(-sigma D)), so a surface at distance d <= D on that ray gets 1 - exp(-sigma d), and exactly the opaque
 * pixel's own fog at d = D. Sky pixels (no surface) write weight 0; the target's mipmaps then average sigma x weight and
 * weight, whose ratio is the mean extinction of the surfaces round a pixel (wolfTkNativeFog's fallback over the sky).
 *
 * Drawn by WolfEnvironmentRender::postPassInner into a W x H GL_RG16F target with mipmaps (TMG_AUTO).
 */

out vec4 frag_color;
in vec2 vary_fragcoord;   // deferred/postDeferredNoTCV.glsl: the pixel's screen coordinate, 0..1

uniform sampler2D overlayBuffer;   // the clouds' resolved overlay (premultiplied: rgb the light in front, a the share hidden)
uniform sampler2D depthBuffer;     // LL's deferred depth (mRT->deferredScreen, as compositeF.glsl reads it)
uniform mat4 wolfInvProj;          // the inverse of the scene's projection (gGLProjection)

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depthBuffer, px, 0).r;
    // compositeF.glsl mainImage: depth >= 1 - 1e-8 is the sky (no surface; the overlay there has no scene distance)
    if (depth >= 1.0 - 1e-8)
    {
        frag_color = vec4(0.0);
        return;
    }
    // deferredUtil.glsl getPositionWithDepth: NDC (2 uv - 1, 2 depth - 1) through the inverse projection
    vec4 v = wolfInvProj * vec4(vary_fragcoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    v.xyz /= v.w;
    float D = max(length(v.xyz), 1e-3);
    // a below 1: 1 - 1e-4 holds sigma x D to ln(1e4) = 9.2 (the fp16 target holds up to 65504)
    float a = clamp(texelFetch(overlayBuffer, px, 0).a, 0.0, 0.9999);
    float sigma = -log(1.0 - a) / D;
    if (!(sigma == sigma) || sigma > 65000.0) sigma = 0.0;   // never a NaN or an infinity in the mipmaps
    frag_color = vec4(sigma, 1.0, 0.0, 0.0);
}
