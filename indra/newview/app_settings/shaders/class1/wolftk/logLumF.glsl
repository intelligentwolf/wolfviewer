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
#wolf_include "wolftk/nightFloor.glsl"   // [WolfViewer 2026-11-02 W92] the clouds' night floor, taken back out below
// [WolfViewer 2026-11-02 W92] the night floor (nightFloor.glsl: the sky dome's in skyF.glsl, the clouds' in compositeF.glsl) is what
// the screen shows, not light: the meter reads the frame without it, so the exposure - and with it the ground, the lit windows and
// the lamps - keeps its real value. The composite is dome' x (1 - a) + overlay', each lifted. The clouds' lift (overlay' - overlay)
// is recomputed here from the same overlay and direction and subtracted exactly. The dome's lift cannot be (its colour before the
// lift is not kept), so a sky texel whose dome part reads at or under its floor (2 % for rounding) counts as black there: exact
// for a dark night sky, and only a narrow band of real sky just above the floor - by night - is read darker than it was.
uniform sampler2D overlayBuffer;   // the composite's overlay (postPassInner), when there is one
uniform float wolfLogLumOverlay;   // 1: overlayBuffer is bound
uniform mat4 wolfLogLumInvProj;    // the inverse projection and view, as the composite builds its rays (compositeV.glsl getCameraRay)
uniform mat4 wolfLogLumInvView;

void main()
{
    vec3 c = texture(frameTex, vary_fragcoord).rgb;
    vec4 norm = texture(normalMap, vary_fragcoord);
    float sky = (GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_SKIP_ATMOS) || GET_GBUFFER_FLAG(norm.w, GBUFFER_FLAG_HAS_HDRI)) ? 1.0 : 0.0;
    if (wolfNightFloorOn > 0.5)
    {
        // the texel's direction as compositeV.glsl getCameraRay unprojects it (ndc z -1 and -0.9: clear of a far plane at infinity)
        vec2 ndc = vary_fragcoord * 2.0 - 1.0;
        vec4 pn = wolfLogLumInvProj * vec4(ndc, -1.0, 1.0), pf = wolfLogLumInvProj * vec4(ndc, -0.9, 1.0);
        if (abs(pn.w) > 1e-8 && abs(pf.w) > 1e-8)
        {
            vec3 dir = normalize((wolfLogLumInvView * vec4(pf.xyz / pf.w - pn.xyz / pn.w, 0.0)).xyz);
            vec4 o = wolfLogLumOverlay > 0.5 ? texture(overlayBuffer, vary_fragcoord) : vec4(0.0);
            vec3 oLift = wolfNightFloorLiftPremultiplied(o, wolfNightFloorCloud(dir.z));
            c = max(c - (oLift - o.rgb), vec3(0.0));
            if (sky > 0.5)
            {
                vec3 skyPart = wolfNightFloorSky(dir.z) * (1.0 - clamp(o.a, 0.0, 1.0));
                if (wolfNightFloorY(max(c - o.rgb, vec3(0.0))) <= wolfNightFloorY(skyPart) * 1.02) c = max(c - skyPart, vec3(0.0));
            }
        }
    }
    float L = dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));   // Rec.709 luma, as LL's luminanceF.glsl
    // delta 1e-4 in pre-exposed units (Reinhard's "small value"); NaN or inf texels count as black
    if (!(L == L) || L > 1e30) L = 0.0;
    // the sky (and HDRI) texels: luminanceF.glsl's GBUFFER_FLAG_SKIP_ATMOS / HAS_HDRI test - the share of the frame's
    // light that is sky is the meter's diffuse bound (wolfexposure.h); the mipmaps average g and b to their means
    frag_color = vec4(log(1e-4 + L), L * sky, L, 1.0);
}
