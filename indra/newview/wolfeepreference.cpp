/**
 * @file wolfeepreference.cpp
 * @brief The stock EEP renderer's screen values on the CPU (wolfeepreference.h).
 *
 * WolfViewer (IntelligentWolf Ltd, 2026-10-13). "[S]" = the STOCK Firestorm tree,
 * firestorm/viewercode/phoenix-firestorm-master/indra; shader paths under newview/app_settings/shaders.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfeepreference.h"

#include <algorithm>   // [2026-10-14] std::equal / std::copy (cloudStats cache)

#include "llenvironment.h"
#include "llviewercontrol.h"
#include "v3colorutil.h"

namespace
{
    // Source: [S] class1/environment/srgbF.glsl srgb_to_linear (piecewise IEC 61966-2-1, not clamped)
    F32 srgbToLinear(F32 x) { return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f); }
    LLColor3 srgbToLinear(const LLColor3& c) { return LLColor3(srgbToLinear(c.mV[0]), srgbToLinear(c.mV[1]), srgbToLinear(c.mV[2])); }
    // Source: [S] srgbF.glsl linear_to_srgb (clamped to 0..1 first)
    F32 linearToSrgb(F32 x)
    {
        x = llclamp(x, 0.f, 1.f);
        return x < 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.f / 2.4f) - 0.055f;
    }
    LLColor3 linearToSrgb(const LLColor3& c) { return LLColor3(linearToSrgb(c.mV[0]), linearToSrgb(c.mV[1]), linearToSrgb(c.mV[2])); }
    LLColor3 cexp(const LLColor3& c) { return LLColor3(expf(c.mV[0]), expf(c.mV[1]), expf(c.mV[2])); }
    LLColor3 cmax(const LLColor3& c, F32 m) { return LLColor3(llmax(c.mV[0], m), llmax(c.mV[1], m), llmax(c.mV[2], m)); }
    LLColor3 cmin(const LLColor3& c, F32 m) { return LLColor3(llmin(c.mV[0], m), llmin(c.mV[1], m), llmin(c.mV[2], m)); }
    LLColor3 cdiv(const LLColor3& a, const LLColor3& b) { return LLColor3(a.mV[0] / b.mV[0], a.mV[1] / b.mV[1], a.mV[2] / b.mV[2]); }
    LLColor3 cpow(const LLColor3& c, F32 e) { return LLColor3(powf(llmax(c.mV[0], 0.f), e), powf(llmax(c.mV[1], 0.f), e), powf(llmax(c.mV[2], 0.f), e)); }

    /** The stock renderer's light colours. Source: [S] pipeline.cpp:6351-6388 (mSunDiffuse / mMoonDiffuse:
     *  sunlight_color and moonlight_color, each scaled so its largest channel is at most 1 and clamped; both 0 while
     *  neither body is up); in classic mode these overwrite the shaders' SUNLIGHT_COLOR / MOONLIGHT_COLOR
     *  ([S] llrender.cpp:994-999 syncLightState, audit section 3.2). */
    struct Lights
    {
        LLColor3 sun, moon;
        bool sunUp = false, moonUp = false;
        LLVector3 lightnorm;   // dome frame (north, up, east): [S] llenvironment.cpp:1711-1776 getClampedLightNorm
        F32 smgf = 0.f;        // sun_moon_glow_factor: [S] llsettingssky.cpp:1318-1322
    };

    LLColor3 normalisedLight(const LLColor3& c)
    {
        LLColor3 r = c;
        const F32 mx = llmax(r.mV[0], llmax(r.mV[1], r.mV[2]));
        if (mx > 1.f) r *= 1.f / mx;
        return LLColor3(llclamp(r.mV[0], 0.f, 1.f), llclamp(r.mV[1], 0.f, 1.f), llclamp(r.mV[2], 0.f, 1.f));
    }

    Lights lights(const LLSettingsSky::ptr_t& sky, const WolfEepReference::Path& p)
    {
        Lights l;
        l.sunUp = sky->getIsSunUp();
        l.moonUp = sky->getIsMoonUp();
        // Paths A/B (classic light sync) see the normalised colours; C/D the raw ones (audit 3.2)
        l.sun = p.classic ? normalisedLight(sky->getSunlightColor()) : sky->getSunlightColor();
        l.moon = p.classic ? normalisedLight(sky->getMoonlightColor()) : sky->getMoonlightColor();
        if (!l.sunUp && !l.moonUp) { l.sun = LLColor3::black; l.moon = LLColor3::black; }
        LLVector4 ln = LLEnvironment::instance().getClampedLightNorm();
        l.lightnorm.set(ln.mV[0], ln.mV[1], ln.mV[2]);   // not renormalised, as the stock code
        l.smgf = sky->getSunMoonGlowFactor();
        return l;
    }

    /** One dome vertex's haze colour. Source: [S] class1/deferred/skyV.glsl:73-166, transcribed. `P` is the dome vertex
     *  in the dome frame (x north, y up, z east), camPosLocal = (0, 14400, 0) ([S] lldrawpoolwlsky.cpp:96-129). */
    LLColor3 skyHaze(const LLSettingsSky::ptr_t& sky, const Lights& l, const LLVector3& P)
    {
        const F32 H = 0.96f * 15000.f;   // [S] llenvironment.cpp:1186-1190 getCamHeight
        LLVector3 rel = P - LLVector3(0.f, H, 0.f) + LLVector3(0.f, 50.f, 0.f);
        const F32 max_y = sky->getMaxY();
        if (rel.mV[1] > 0.f) rel *= max_y / rel.mV[1];
        if (rel.mV[1] < 0.f) rel *= -32000.f / rel.mV[1];
        const F32 len = rel.length();
        LLVector3 n = rel; n.normalize();
        const F32 dotL = n * l.lightnorm;
        LLColor3 sunlight = l.sunUp ? l.sun : l.moon * 0.7f;
        const LLColor3 BD = sky->getBlueDensity(), BH = sky->getBlueHorizon();
        const F32 HD = sky->getHazeDensity(), HH = sky->getHazeHorizon(), DM = sky->getDensityMultiplier();
        const LLColor3 light_atten = (BD + LLColor3(HD * 0.25f, HD * 0.25f, HD * 0.25f)) * (DM * max_y);
        LLColor3 combined(llmax(fabsf(BD.mV[0]) + fabsf(HD), 1e-6f), llmax(fabsf(BD.mV[1]) + fabsf(HD), 1e-6f), llmax(fabsf(BD.mV[2]) + fabsf(HD), 1e-6f));
        const LLColor3 blue_weight = cdiv(BD, combined);
        const LLColor3 haze_weight = cdiv(LLColor3(HD, HD, HD), combined);
        const F32 off_axis = 1.f / llmax(1e-6f, llmax(0.f, n.mV[1]) + l.lightnorm.mV[1]);
        sunlight = componentMult(sunlight, cexp(light_atten * -off_axis));
        const F32 density_dist = len * DM;
        LLColor3 T = cexp(combined * -density_dist);
        const LLColor3 glow = sky->getGlow();
        F32 haze_glow = llmax(1.f - dotL, 0.001f) * glow.mV[0];
        haze_glow = powf(haze_glow, glow.mV[2]);
        haze_glow = (l.smgf < 1.f) ? 0.f : (l.smgf * (haze_glow + 0.25f));
        const LLColor3 amb = sky->getAmbientColor();
        LLColor3 color = componentMult(componentMult(BH, blue_weight), sunlight + amb)
                       + componentMult(haze_weight * HH, sunlight * haze_glow + amb);
        color = componentMult(color, LLColor3::white - T);
        const F32 CS = sky->getCloudShadow();
        const LLColor3 ambient = amb + cmax(LLColor3::white - amb, 0.f) * (CS * 0.5f);
        sunlight *= llmax(0.f, 1.f - CS);
        const LLColor3 below = componentMult(componentMult(BH, blue_weight), sunlight + ambient)
                             + componentMult(haze_weight * HH, sunlight * haze_glow + ambient);
        const LLColor3 Ts(sqrtf(T.mV[0]), sqrtf(T.mV[1]), sqrtf(T.mV[2]));
        const LLColor3 w(1.f - sqrtf(Ts.mV[0]), 1.f - sqrtf(Ts.mV[1]), 1.f - sqrtf(Ts.mV[2]));
        color += componentMult(below - color, w);
        return color;
    }

    /** The dome vertex seen in the view direction `dirWorld`. Source: the dome geometry ([S] llvowlsky.cpp:101-123, :410-430,
     *  audit 3.1): a cap of radius 15000 m seen from 14400 m above its centre, reaching 22.5 deg from its apex; a ring
     *  at zenith angle phi lies at elevation atan2(R cos phi - H, R sin phi). Solved for phi by bisection. */
    LLVector3 domeVertex(const LLVector3& dirWorld)
    {
        const F32 R = 15000.f, H = 0.96f * 15000.f;
        const F32 horiz = sqrtf(dirWorld.mV[0] * dirWorld.mV[0] + dirWorld.mV[1] * dirWorld.mV[1]);
        const F32 elev = atan2f(dirWorld.mV[2], horiz);
        F32 lo = 0.f, hi = F_PI / 8.f;   // elevation falls as phi grows
        for (S32 i = 0; i < 40; ++i)
        {
            const F32 mid = 0.5f * (lo + hi);
            const F32 e = atan2f(R * cosf(mid) - H, R * sinf(mid));
            if (e > elev) lo = mid; else hi = mid;
        }
        const F32 phi = 0.5f * (lo + hi);
        // azimuth: the dome frame's x is north, z east
        const F32 north = horiz > 1e-6f ? dirWorld.mV[1] / horiz : 1.f, east = horiz > 1e-6f ? dirWorld.mV[0] / horiz : 0.f;
        return LLVector3(R * sinf(phi) * north, R * cosf(phi), R * sinf(phi) * east);
    }

    /** calcAtmosphericVars for an eye-space position. Source: [S] class1/windlight/atmosphericsFuncs.glsl:51-131, transcribed;
     *  `posDome` is the eye position in the dome frame orientation (y up) - the stock code uses the eye-space y here
     *  (audit 5.1), which for a level view is the up axis. */
    void atmosVars(const LLSettingsSky::ptr_t& sky, const Lights& l, const LLVector3& posDome, const LLVector3& lightDirDome,
                   LLColor3& sunlit, LLColor3& amblit, LLColor3& additive, LLColor3& atten)
    {
        LLVector3 rel = posDome;
        const F32 max_y = sky->getMaxY();
        if (fabsf(rel.mV[1]) > max_y) rel *= max_y / rel.mV[1];
        LLVector3 n = rel; const F32 len = rel.length(); if (len > 0.f) n *= 1.f / len;
        LLColor3 sunlight = l.sunUp ? l.sun : l.moon;
        const LLColor3 BD = sky->getBlueDensity(), BH = sky->getBlueHorizon();
        const F32 HD = sky->getHazeDensity(), HH = sky->getHazeHorizon(), DM = sky->getDensityMultiplier();
        const LLColor3 light_atten = (BD + LLColor3(HD * 0.25f, HD * 0.25f, HD * 0.25f)) * (DM * max_y);
        const LLColor3 combined = cmax(BD + LLColor3(HD, HD, HD), 1e-6f);
        const LLColor3 blue_weight = cdiv(BD, combined), haze_weight = cdiv(LLColor3(HD, HD, HD), combined);
        const F32 above_horizon_factor = 1.f / llmax(1e-6f, l.lightnorm.mV[1]);
        sunlight = componentMult(sunlight, cexp(light_atten * -above_horizon_factor));
        const F32 density_dist = len * DM;
        atten = cexp(combined * -(density_dist * sky->getDistanceMultiplier()));
        F32 haze_glow = (n * l.lightnorm) * llmax(0.f, lightDirDome * n);
        haze_glow = llmax(1.f - haze_glow, 0.001f) * sky->getGlow().mV[0];
        haze_glow = llclamp(powf(haze_glow, sky->getGlow().mV[2]), -100000.f, 100000.f) + 0.25f;
        haze_glow *= l.smgf;
        const LLColor3 amb = sky->getAmbientColor();
        const F32 CS = sky->getCloudShadow();
        const LLColor3 tmpAmbient = amb + (LLColor3::white - amb) * (CS * 0.5f);
        const LLColor3 cs = sunlight * (1.f - CS);
        additive = componentMult(componentMult(BH, blue_weight), cs + tmpAmbient) + componentMult(haze_weight * HH, cs * haze_glow + tmpAmbient);
        sunlit = sunlight;
        amblit = cpow(tmpAmbient, 0.9f) * 0.57f;
        additive = componentMult(additive, LLColor3::white - atten);
        additive = cmin(additive, 10.f);
    }

    /** The light direction in the dome frame: the sun's, else the moon's ([S] pipeline.cpp:9443-9449 light_dir). */
    LLVector3 lightDirDome(const Lights& l)
    {
        LLVector3 d = l.sunUp ? LLEnvironment::instance().getSunDirection() : LLEnvironment::instance().getMoonDirection();
        return LLVector3(d.mV[1], d.mV[2], d.mV[0]);   // toLightNorm order (north, up, east)
    }

    /** The white legacy surface, up-facing, from the vars. Source: [S] class3/deferred/softenLightF.glsl:159-163 (x 1.35
     *  in classic), :212-236 (the legacy branch), :279-283 (x 1.1 in classic, clampHDRRange); calcAtmosphericVarsLinear
     *  ([S] atmosphericsFuncs.glsl:137-165: ambientLighting, and outside classic the linearisation + grey ambient). */
    LLColor3 litSurface(const WolfEepReference::Path& p, const Lights& l, LLColor3 sunlit, LLColor3 amblit)
    {
        const LLVector3 up(0.f, 1.f, 0.f);
        const LLVector3 ld = lightDirDome(l);
        F32 ambL = llmin(fabsf(up * ld), 1.f) * 0.5f;
        amblit *= (1.f - ambL * ambL);
        if (!p.classic)
        {
            const LLColor3 al = srgbToLinear(amblit);
            const F32 y = 0.2126f * al.mV[0] + 0.7152f * al.mV[1] + 0.0722f * al.mV[2];
            amblit = LLColor3(y, y, y);
            sunlit = srgbToLinear(sunlit);
        }
        // sky_sunlight_scale / sky_ambient_scale: RenderHDRSkySunlightScale / RenderSkyAmbientScale, 1.0 ([S] settings.xml)
        if (p.classic) sunlit *= 1.35f;
        F32 da = llclamp(up * ld, 0.f, 1.f);
        LLColor3 color;
        if (p.classic)
        {
            da = powf(da, 1.2f);
            const F32 sc = llmin(da, 1.f);
            color = srgbToLinear(amblit * 0.9f + componentMult(linearToSrgb(LLColor3(sc, sc, sc)), sunlit * 0.7f));
        }
        else
        {
            color = amblit + sunlit * llmin(da, 1.f);
        }
        color *= (p.classic ? 1.1f : 1.f);
        return LLColor3(llclamp(color.mV[0], 0.f, 11.2f), llclamp(color.mV[1], 0.f, 11.2f), llclamp(color.mV[2], 0.f, 11.2f));
    }

    /** The default cloud noise texture's value distribution (id 1dc1368f-e8fe-f02d-a08d-9d9f11c1af6b, [S] llsettingssky.cpp:145),
     *  decoded from the asset itself (512 x 512 grey, mean 0.4528, sd 0.1707): the share of texels in each tenth of 0..1
     *  (audit 6.4). Not in the source tree, so measured from the asset, not read from code. */
    const F32 NOISE_HIST[10] = { 0.007f, 0.052f, 0.133f, 0.209f, 0.234f, 0.164f, 0.107f, 0.067f, 0.024f, 0.003f };
    const F32 NOISE_MEAN = 0.4528f;
}

namespace WolfEepReference
{
    Path path(const LLSettingsSky::ptr_t& sky)
    {
        Path p;
        static LLCachedControl<bool> auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        // [S] llsettingsvo.cpp:810 classic_mode = canAutoAdjust() && !should_auto_adjust
        p.classic = sky->canAutoAdjust() && !auto_adjust;
        // [S] pipeline.cpp:7844 no_post = probe ambiance == 0
        const F32 probe = sky->getReflectionProbeAmbiance(auto_adjust);
        p.noPost = probe == 0.f;
        p.gamma = sky->getGamma();
        // [S] llsettingsvo.cpp:830-852: sky_hdr_scale = 2 sqrt(gamma) when the sky's own probe ambiance is non-zero, else 1
        p.skyHdrScale = sky->getReflectionProbeAmbiance() != 0.f ? sqrtf(p.gamma) * 2.f : 1.f;
        return p;
    }

    LLColor3 skyLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld)
    {
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        LLColor3 c = skyHaze(sky, l, domeVertex(dirWorld));
        // [S] class1/deferred/skyF.glsl:114-115: x 2, clamp 0..5 (rainbow / halo not included)
        c = LLColor3(llclamp(2.f * c.mV[0], 0.f, 5.f), llclamp(2.f * c.mV[1], 0.f, 5.f), llclamp(2.f * c.mV[2], 0.f, 5.f));
        // [S] class3/deferred/softenLightF.glsl:195-205, :279-283: srgb_to_linear x sky_hdr_scale, x 1.1 in classic, clamp
        LLColor3 lin = srgbToLinear(c) * p.skyHdrScale * (p.classic ? 1.1f : 1.f);
        return LLColor3(llclamp(lin.mV[0], 0.f, 11.2f), llclamp(lin.mV[1], 0.f, 11.2f), llclamp(lin.mV[2], 0.f, 11.2f));
    }

    LLColor3 surfaceLinear(const LLSettingsSky::ptr_t& sky)
    {
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        LLColor3 sunlit, amblit, additive, atten;
        atmosVars(sky, l, LLVector3(0.f, 0.f, -0.001f), lightDirDome(l), sunlit, amblit, additive, atten);
        return litSurface(p, l, sunlit, amblit);
    }

    void surfaceLights(const LLSettingsSky::ptr_t& sky, LLColor3& sunPerCos, LLColor3& ambient)
    {
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        LLColor3 sunlit, amblit, additive, atten;
        atmosVars(sky, l, LLVector3(0.f, 0.f, -0.001f), lightDirDome(l), sunlit, amblit, additive, atten);
        const LLColor3 full = litSurface(p, l, sunlit, amblit);
        ambient = litSurface(p, l, LLColor3::black, amblit);   // the same formula with no sun term
        const F32 nl = llmax(lightDirDome(l).mV[1], 0.05f);
        sunPerCos = cmax(full - ambient, 0.f) * (1.f / nl);
    }

    HazeParams hazeParams(const LLSettingsSky::ptr_t& sky)
    {
        // Source: [S] atmosphericsFuncs.glsl:51-131 (calcAtmosphericVars), as atmosVars above
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        HazeParams h;
        LLColor3 sunlight = l.sunUp ? l.sun : l.moon;
        const LLColor3 BD = sky->getBlueDensity(), BH = sky->getBlueHorizon();
        const F32 HD = sky->getHazeDensity(), HH = sky->getHazeHorizon(), DM = sky->getDensityMultiplier();
        const F32 max_y = sky->getMaxY();
        const LLColor3 light_atten = (BD + LLColor3(HD * 0.25f, HD * 0.25f, HD * 0.25f)) * (DM * max_y);
        h.combined = cmax(BD + LLColor3(HD, HD, HD), 1e-6f);
        h.blueW = componentMult(BH, cdiv(BD, h.combined));
        h.hazeW = cdiv(LLColor3(HD, HD, HD), h.combined) * HH;
        sunlight = componentMult(sunlight, cexp(light_atten * -(1.f / llmax(1e-6f, l.lightnorm.mV[1]))));
        const F32 CS = sky->getCloudShadow();
        const LLColor3 amb = sky->getAmbientColor();
        h.tmpAmb = amb + (LLColor3::white - amb) * (CS * 0.5f);
        h.cs = sunlight * (1.f - CS);
        h.glowX = sky->getGlow().mV[0]; h.glowZ = sky->getGlow().mV[2]; h.smgf = l.smgf;
        h.densityDistance = DM * sky->getDistanceMultiplier();
        h.maxY = max_y;
        h.skyHdrScale = p.skyHdrScale;
        h.lightnormWorld.set(l.lightnorm.mV[2], l.lightnorm.mV[0], l.lightnorm.mV[1]);   // dome (north, up, east) -> world
        const LLVector3 ld = lightDirDome(l);
        h.lightDirWorld.set(ld.mV[2], ld.mV[0], ld.mV[1]);
        return h;
    }

    LLColor3 hazedSurfaceLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld, F32 distance)
    {
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        const LLVector3 dDome(dirWorld.mV[1], dirWorld.mV[2], dirWorld.mV[0]);
        LLColor3 sunlit, amblit, additive, atten;
        atmosVars(sky, l, dDome * distance, lightDirDome(l), sunlit, amblit, additive, atten);
        const LLColor3 lit = litSurface(p, l, sunlit, amblit);
        // [S] class3/deferred/hazeF.glsl:79-100: lit x atten.r + srgb_to_linear(additive x 2) x sky_hdr_scale
        return lit * atten.mV[0] + srgbToLinear(additive * 2.f) * p.skyHdrScale;
    }

    F32 hazeTransmittance(const LLSettingsSky::ptr_t& sky, F32 distance)
    {
        const LLColor3 BD = sky->getBlueDensity();
        const F32 HD = sky->getHazeDensity();
        // [S] atmosphericsFuncs.glsl:79-87 (the red channel, as hazeF uses), a level view (|rel.y| under max_y)
        return expf(-llmax(BD.mV[0] + HD, 1e-6f) * distance * sky->getDensityMultiplier() * sky->getDistanceMultiplier());
    }

    /** The stock cloud opacity alpha1 over the noise histogram: its mean and the share where it is at least one half.
     *  Source: [S] class1/deferred/cloudsV.glsl:184 density0 = 2 (cloud_shadow - 0.25); cloudsF.glsl:59-127 (audit 6.2):
     *    k = cloud_variance (1 - cloud_scale / 4); dv = min(1, (2 dist.x + 2 dist.y + dist2.x + dist2.y) x 4) with each
     *    dist component a noise tap x k - taken at the noise's mean (NOISE_MEAN), the taps not being known per texel here;
     *    dens = density0 (1 - dv^2);
     *    a1 = (N1 - 0.5) + (N2 - 0.5) cpd2.z + dens; a1 = min(max(a1, 0) x 10 cpd1.z, 1); a1 = 1 - (1 - a1^2)^2.
     *  N1, N2 drawn from the measured noise histogram (taken independent: the second tap is the texture at 16 x the
     *  frequency). */
    void cloudStats(const LLSettingsSky::ptr_t& sky, F32& mean, F32& covered)
    {
        const F32 density0 = 2.f * (sky->getCloudShadow() - 0.25f);
        const F32 k = sky->getCloudVariance() * (1.f - sky->getCloudScale() * 0.25f);
        const F32 dv = llmin(1.f, 6.f * NOISE_MEAN * k * 4.f);
        const F32 dens = density0 * (1.f - dv * dv);
        const F32 z1 = sky->getCloudPosDensity1().mV[2], z2 = sky->getCloudPosDensity2().mV[2];
        // [WolfViewer 2026-10-14] 6400 samples, called up to twice a frame: the last answer is kept for the same five inputs
        // (cloud_shadow, cloud_variance, cloud_scale and the two densities - everything below reads)
        static F32 s_key[5] = { -1e9f, 0.f, 0.f, 0.f, 0.f }, s_mean = 0.f, s_covered = 0.f;
        const F32 key[5] = { sky->getCloudShadow(), sky->getCloudVariance(), sky->getCloudScale(), z1, z2 };
        if (std::equal(key, key + 5, s_key)) { mean = s_mean; covered = s_covered; return; }
        const S32 SUB = 8;   // sample points inside each tenth
        F32 sum = 0.f, cov = 0.f, wsum = 0.f;
        for (S32 i = 0; i < 10 * SUB; ++i)
        {
            const F32 n1 = (i + 0.5f) / (10.f * SUB), w1 = NOISE_HIST[i / SUB] / SUB;
            for (S32 j = 0; j < 10 * SUB; ++j)
            {
                const F32 n2 = (j + 0.5f) / (10.f * SUB), w2 = NOISE_HIST[j / SUB] / SUB;
                F32 a1 = (n1 - 0.5f) + (n2 - 0.5f) * z2 + dens;
                a1 = llmin(llmax(a1, 0.f) * 10.f * z1, 1.f);
                a1 = 1.f - (1.f - a1 * a1) * (1.f - a1 * a1);
                sum += a1 * w1 * w2;
                if (a1 >= 0.5f) cov += w1 * w2;
                wsum += w1 * w2;
            }
        }
        mean = wsum > 0.f ? llclamp(sum / wsum, 0.f, 1.f) : 0.f;
        covered = wsum > 0.f ? llclamp(cov / wsum, 0.f, 1.f) : 0.f;
        std::copy(key, key + 5, s_key); s_mean = mean; s_covered = covered;
    }
}

namespace WolfEepReference
{
    F32 cloudCoverage(const LLSettingsSky::ptr_t& sky)
    {
        F32 mean, covered;
        cloudStats(sky, mean, covered);
        return mean;
    }

    F32 cloudCoveredFraction(const LLSettingsSky::ptr_t& sky)
    {
        F32 mean, covered;
        cloudStats(sky, mean, covered);
        return covered;
    }

    bool cloudColourLinear(const LLSettingsSky::ptr_t& sky, const LLVector3& dirWorld, LLColor3& colour)
    {
        const Path p = path(sky);
        const Lights l = lights(sky, p);
        // the cloud vertex: [S] cloudsV.glsl:76-190 on the same dome (lldrawpoolwlsky.cpp:346 renderDome)
        const F32 H = 0.96f * 15000.f;
        LLVector3 rel = domeVertex(dirWorld) - LLVector3(0.f, H, 0.f) + LLVector3(0.f, 50.f, 0.f);
        const F32 max_y = sky->getMaxY();
        F32 altitude_blend = llclamp((rel.mV[1] + 512.f) / max_y, 0.f, 1.f);
        if (rel.mV[1] > 0.f) rel *= max_y / rel.mV[1];
        if (rel.mV[1] < 0.f) { altitude_blend = 0.f; rel *= -32000.f / rel.mV[1]; }
        const F32 len = rel.length();
        LLVector3 n = rel; n.normalize();
        const LLColor3 sunlight0 = l.sun;   // SUNLIGHT_COLOR (classic: the synced light 0, Lights)
        const LLColor3 BD = sky->getBlueDensity(), BH = sky->getBlueHorizon();
        const F32 HD = sky->getHazeDensity(), HH = sky->getHazeHorizon(), DM = sky->getDensityMultiplier();
        const LLColor3 light_atten = (BD + LLColor3(HD * 0.25f, HD * 0.25f, HD * 0.25f)) * (DM * max_y);
        const LLColor3 combined(fabsf(BD.mV[0]) + fabsf(HD), fabsf(BD.mV[1]) + fabsf(HD), fabsf(BD.mV[2]) + fabsf(HD));
        const LLColor3 blue_weight = cdiv(BD, combined), haze_weight = cdiv(LLColor3(HD, HD, HD), combined);
        const F32 off_axis = 1.f / llmax(1e-6f, llmax(0.f, n.mV[1]) + l.lightnorm.mV[1]);
        LLColor3 sunlight = componentMult(sunlight0, cexp(light_atten * -off_axis));
        LLColor3 T = cexp(combined * -(len * DM));
        const LLColor3 glow = sky->getGlow();
        F32 haze_glow = llmax(1.f - n * l.lightnorm, 0.001f) * glow.mV[0];
        haze_glow = powf(haze_glow, glow.mV[2]) * l.smgf;
        haze_glow = (l.smgf < 1.f) ? 0.f : (haze_glow + 0.25f);
        const F32 CS = sky->getCloudShadow();
        const LLColor3 amb = sky->getAmbientColor();
        const LLColor3 tmpAmbient = amb + (LLColor3::white - amb) * (CS * 0.5f);
        sunlight *= (1.f - CS);
        const LLColor3 below = componentMult(componentMult(BH, blue_weight), sunlight + tmpAmbient)
                             + componentMult(haze_weight * HH, sunlight * haze_glow + tmpAmbient);
        sunlight = componentMult(sunlight0, cexp(light_atten * -(1.f / llmax(1e-6f, l.lightnorm.mV[1] * 2.f))));
        const LLColor3 cloud_color = sky->getCloudColor();
        LLColor3 Csun = componentMult(sunlight * haze_glow, cloud_color);
        LLColor3 Camb = componentMult(tmpAmbient, cloud_color);
        const LLColor3 Ts(sqrtf(T.mV[0]), sqrtf(T.mV[1]), sqrtf(T.mV[2]));
        Csun = componentMult(Csun, Ts);
        Camb = componentMult(Camb, Ts) + componentMult(below, LLColor3::white - Ts);
        // the fragment ([S] cloudsF.glsl:83-118), over the noise histogram: the self-shadow tap (uv2 = uv1 + lightnorm.xz x
        // 0.0125) is taken at the main tap's value - 0.0125 of a texture is a small step across the noise's features
        const F32 density0 = 2.f * (CS - 0.25f);
        const F32 k = sky->getCloudVariance() * (1.f - sky->getCloudScale() * 0.25f);
        const F32 dv = llmin(1.f, 6.f * NOISE_MEAN * k * 4.f);
        const F32 dens = density0 * (1.f - dv * dv);
        const F32 z1 = sky->getCloudPosDensity1().mV[2], z2 = sky->getCloudPosDensity2().mV[2];
        const S32 SUB = 8;
        LLColor3 sum; F32 wsum = 0.f;
        for (S32 i = 0; i < 10 * SUB; ++i)
        {
            const F32 n1 = (i + 0.5f) / (10.f * SUB), w1 = NOISE_HIST[i / SUB] / SUB;
            for (S32 j = 0; j < 10 * SUB; ++j)
            {
                const F32 n2 = (j + 0.5f) / (10.f * SUB), w2 = NOISE_HIST[j / SUB] / SUB;
                F32 a1 = (n1 - 0.5f) + (n2 - 0.5f) * z2 + dens;
                a1 = llmin(llmax(a1, 0.f) * 10.f * z1, 1.f);
                a1 = 1.f - (1.f - a1 * a1) * (1.f - a1 * a1);
                a1 = llclamp(a1 * altitude_blend, 0.f, 1.f);
                if (a1 < 0.5f) continue;
                F32 a2 = llmin(llmax(n1 - 0.5f + dens, 0.f) * 2.5f * z1, 1.f);
                a2 = 1.f - a2;
                a2 = 1.f - a2 * a2;
                LLColor3 rgb = Csun * (1.f - a2) + Camb;
                rgb = LLColor3(llclamp(rgb.mV[0], 0.f, 1.f), llclamp(rgb.mV[1], 0.f, 1.f), llclamp(rgb.mV[2], 0.f, 1.f)) * 2.f;
                sum += rgb * (w1 * w2);
                wsum += w1 * w2;
            }
        }
        if (wsum <= 0.f) return false;
        colour = cloudGbufferLinear(sky, sum * (1.f / wsum));
        return true;
    }

    LLColor3 cloudGbufferLinear(const LLSettingsSky::ptr_t& sky, const LLColor3& gbuffer)
    {
        const Path p = path(sky);
        // linearised as the sky's G-buffer value is ([S] softenLightF.glsl:195-205, :279-283)
        const LLColor3 lin = srgbToLinear(gbuffer) * p.skyHdrScale * (p.classic ? 1.1f : 1.f);
        return LLColor3(llclamp(lin.mV[0], 0.f, 11.2f), llclamp(lin.mV[1], 0.f, 11.2f), llclamp(lin.mV[2], 0.f, 11.2f));
    }

    LLColor3 display(const Path& p, const LLColor3& lin)
    {
        LLColor3 c = lin;
        if (p.noPost)
        {
            // [S] postDeferredTonemap.glsl:58-62 clamp; CASF.glsl:2561-2574 linear_to_srgb, then 1 - (1 - x)^gamma
            c = linearToSrgb(c);
            for (S32 i = 0; i < 3; ++i) c.mV[i] = 1.f - powf(1.f - llclamp(c.mV[i], 0.f, 1.f), p.gamma);
            return c;
        }
        // Paths C/D: tonemapUtilF.glsl toneMapACES_Hill, mixed 0.7 with the linear value, at exposure 1 (LL's dynamic
        // exposure, which depends on the whole frame, is not reproduced here), then sRGB
        LLColor3 a(0.59719f * c.mV[0] + 0.35458f * c.mV[1] + 0.04823f * c.mV[2],
                   0.07600f * c.mV[0] + 0.90834f * c.mV[1] + 0.01566f * c.mV[2],
                   0.02840f * c.mV[0] + 0.13383f * c.mV[1] + 0.83777f * c.mV[2]);
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 v = a.mV[i];
            a.mV[i] = (v * (v + 0.0245786f) - 0.000090537f) / (v * (0.983729f * v + 0.4329510f) + 0.238081f);
        }
        LLColor3 o(1.60475f * a.mV[0] - 0.53108f * a.mV[1] - 0.07367f * a.mV[2],
                   -0.10208f * a.mV[0] + 1.10813f * a.mV[1] - 0.00605f * a.mV[2],
                   -0.00327f * a.mV[0] - 0.07276f * a.mV[1] + 1.07602f * a.mV[2]);
        for (S32 i = 0; i < 3; ++i) c.mV[i] = llclamp(0.3f * c.mV[i] + 0.7f * llclamp(o.mV[i], 0.f, 1.f), 0.f, 1.f);
        return linearToSrgb(c);
    }
}
