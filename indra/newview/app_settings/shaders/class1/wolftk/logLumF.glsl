/**
 * @file wolftk/logLumF.glsl
 * @brief The frame's log luminance, for the light meter's reflected reading (WolfExposure).
 *
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13). Drawn over a small target (WolfEnvironmentRender::postPassInner,
 * 64 x 64) whose mipmaps average it to one texel: the mean of log(delta + L) over the frame, i.e. Reinhard,
 * Stark, Shirley & Ferwerda 2002, "Photographic Tone Reproduction for Digital Images", eq. 1, the log-average
 * luminance L_w = exp(1/N sum log(delta + L_w(x, y))), "delta is a small value to avoid the singularity that occurs
 * if black pixels are present". The frame is pre-exposed (WolfEnvironmentRender::exposure); the reader divides by
 * that exposure to get cd/m2 (Filament.md.html "Exposure value and luminance": a 1-pixel downsample "can easily be
 * affected by extreme values" - the sun's disc - which the log mean is not).
 */

out vec4 frag_color;
in vec2 vary_fragcoord;
uniform sampler2D frameTex;
uniform sampler2D normalMap;   // the G-buffer's normal + flags (LL's luminanceF.glsl reads the same for its sky test)

void main()
{
    vec3 c = texture(frameTex, vary_fragcoord).rgb;
    float L = dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));   // Rec.709 luma, as LL's luminanceF.glsl
    // delta 1e-4 in pre-exposed units (Reinhard's "small value"); NaN or inf texels count as black
    if (!(L == L) || L > 1e30) L = 0.0;
    // the sky (and HDRI) texels: luminanceF.glsl's GBUFFER_FLAG_SKIP_ATMOS / HAS_HDRI test - the share of the frame's
    // light that is sky is the meter's diffuse bound (wolfexposure.h); the mipmaps average g and b to their means
    vec4 norm = texture(normalMap, vary_fragcoord);
    float sky = (GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_SKIP_ATMOS) || GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_HAS_HDRI)) ? 1.0 : 0.0;
    frag_color = vec4(log(1e-4 + L), L * sky, L, 1.0);
}
