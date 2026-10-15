/**
 * @file wolfenvironmentrender.cpp
 * @brief WolfViewer: the Wolf Environment drawn natively. See wolfenvironmentrender.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

// [WOLF ENV TAKRAM 2026-10-12] The renderer itself is translated code (wolftkatmosphere.cpp, wolftkclouds.cpp,
// wolfexposure.cpp, shaders/class1/wolftk/); this file only drives it from the Wolf Environment document and
// fits it into LL's deferred pipeline. Every value it adds cites where it comes from;
// region-terrain-work/wolf-environment-render/TRANSLATION_PROGRESS.md lists the sources and Paul's decisions.

#include "llviewerprecompiledheaders.h"

#include "wolfenvironmentrender.h"

#include <algorithm>
#include <sstream>
#include <cmath>
#include <ctime>
#include <map>
#include <vector>

#include "llagent.h"
#include "lldate.h"
#include "lldir.h"
#include "llenvironment.h"
#include "wolfeepreference.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimage.h"
#include "llimagepng.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexturelist.h"
#include "llvosky.h"
#include "pipeline.h"
#include "wolfaltitudesky.h"
#include "wolfaurora.h"
#include "wolfcloudgenus.h"
#include "wolfenvironmentstate.h"
#include "wolfexposure.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"
#include "wolftkatmosphere.h"
#include "wolftkclouds.h"
#include "wolftkgl.h"
#include "wolfweather.h"
#include "llrand.h"

extern bool gCubeSnapshot;
extern bool gSnapshot;   // llviewerdisplay.cpp:141

namespace
{
    // ── WolfViewer's own bodies (kept by Paul's decisions D5 / D12) ─────────────────────────────────
    // The Moon's mean angular diameter, 31.1 arcmin (0.5181 deg; Hillaire 2016 4.2 gives 0.488-0.568).
    const F64 WOLF_MOON_RADIUS = 0.5181 * 0.5 * DEG_TO_RAD;
    const F64 FOOTCANDLE_LUX = 10.7639;          // 1 fc = 10.7639 lux (definition)
    // Decision D13. Source: NPS Night Skies, "Night Sky Quality Metrics" (nps.gov/subjects/nightskies/
    // NSQmetrics.html): "22.0 mag/arcsec2 or 172 ucd/m2 is generally considered to represent the darkest part of
    // pristine skies" (zenith luminance).
    const F32 NIGHT_SKY_ZENITH_CD = 172.0e-6f;
    // The disc radius drawn for the extra suns: three-geospatial AtmosphereParameters.ts:102 sunAngularRadius
    // = 0.004675 (the first sun's disc is the atmosphere's own, sky.glsl:48-59).
    const F64 WOLF_SUN_RADIUS = 0.004675;

    struct Gpu
    {
        bool failed = false;          // a program or a target failed: tier 0 for the session
        bool warned = false;
        S32  tierFrame = -1;          // tier() is worked out once a frame
        S32  tierValue = 0;
        U32  frame = 0;
        F64  lastTime = 0.0;
        LLRenderTarget layer;         // the composite, full resolution (the frame is its input)
        LLRenderTarget probeLayer;    // the same for reflection-probe faces (their own size: no reallocation per face)
        LLRenderTarget camShadow;     // 1 x 1: the clouds' transmittance at the camera (the light meter)
        GLuint camPbo[2] = {};        // camShadow read back through two pixel-pack buffers, a frame apart (no stall)
        bool   camPboPending[2] = {};
        U32    camPboIndex = 0;
        LLRenderTarget logLum;        // 64 x 64, mipmapped: the frame's log luminance (wolftk/logLumF.glsl), its 1 x 1 mip the mean
        GLuint lumPbo[2] = {};        // the mean read back a frame apart, as camShadow
        bool   lumPboPending[2] = {};
        F32    lumPboExposure[2] = { 1.f, 1.f };   // the pre-exposure of the frame each reading was taken from
        U32    lumPboIndex = 0;
        U32    updateFrame = U32_MAX; // the frame frameUpdate last ran in (it runs once a frame)
        bool   active = false;        // skyActive(), worked out once a frame with tier()
        bool   captureFinalPending = false;   // WolfEnvironmentDebugCapture: postPass captured, the final frame to come
        size_t atmosHash = 0;         // the atmosphere being built or in force
        bool atmosRequested = false;
        // GPU timings (GL_TIMESTAMP, glQueryCounter): [section][begin/end], three frames deep
        GLuint q[3][4][2] = {};
        bool   qPending[3][4] = {};
        F64    ms[4] = {};
        U32    msFrames[4] = {};
        F64    lastLog = 0.0;
    };
    Gpu sGpu;

    WolfTk::Atmosphere* sAtmos = nullptr;
    WolfTk::Clouds* sClouds = nullptr;
    WolfExposure::Meter sMeter;
    // [WolfViewer step 3] the cloud genera in force, smoothed (wolfcloudgenus.h approach): the target is mapped every
    // frame from the document's weather, the numbers follow it over GENUS_TAU seconds (section 10 A23)
    WolfCloudGenus::Result sGenus;
    bool sGenusValid = false;
    constexpr F32 GENUS_TAU = 20.f;

    /** The frame's geometry and light (absolute photometric values; x exposure when sent to shaders). */
    struct Frame
    {
        glm::dmat4 worldToECEF = glm::dmat4(1.0);   // LL agent metres -> "ECEF" metres (see placeOnPlanet)
        glm::dmat4 ecefToWorld = glm::dmat4(1.0);
        LLVector3d cameraECEF;                       // metres
        F64      bottomRadius = 6360000.0;           // metres
        F32      cameraHeight = 0.f;                 // above the sea, metres
        LLVector3 sunWorld = LLVector3::z_axis, moonWorld = -LLVector3::z_axis;
        LLVector3 sunECEF = LLVector3::z_axis;
        bool     sunUp = true;
        LLColor3 sunNormal;                          // the sun's illuminance at the camera, perpendicular (lux)
        LLColor3 sunHorizontal, skyHorizontal;       // on a horizontal surface (lux)
        F32      moonTOA = 0.f;                      // Krisciunas & Schaefer, lux
        LLColor3 moonNormal;                         // at the camera, perpendicular
        LLVector3 moonECEF = -LLVector3::z_axis;
        // [WolfViewer round 8] the clouds' light: the sun, or the moon once the sun is 6 deg under (civil twilight's end,
        // the AMS Glossary definition) while the moon gives any sky (moonSkyScale > 0); the clouds' shadow map is cast
        // along it (wolftkclouds.cpp CloudsFrame sun_dir_ecef), so the light under the clouds is read along it too
        LLVector3 cloudLightECEF = LLVector3::z_axis;
        bool     moonLightsClouds = false;
        F32      moonSkyScale = 0.f;                 // the moon's illuminance above the atmosphere / the sun's (sun down)
        F32      moonPhaseAngle = 0.f;
        F32      cloudTransmittanceAtCamera = 1.f;   // last frame's (camShadow): the sun's direct beam
        F32      cloudDiffuseT = 1.f;                // the clouds' mean two-stream diffuse transmittance over the sky
        F32      cloudDiffusedBeam = 0.f;            // the mean share of the sun's beam they pass on as diffuse light
        LLColor3 skyUnderCloud = LLColor3::white;    // skyHorizontal under the clouds / the clear sky's (the clouds' haze light)
        F32      meterLux = 0.f;
        F32      frameLogAvg = -1.f;                 // last frame's log-average luminance, cd/m2 (wolftk/logLumF.glsl); < 0 none
        F32      skyLuminanceShare = 1.f;            // the share of last frame's light that was sky (the meter's diffuse bound)
        F32      exposure = 1.f;
        F32      cover = 0.f;
        F32      hazeScale = 1.f;
        F32      weatherRepeatScale = 1.f;          // [WolfViewer round 9] the EEP's cloud_scale over its default
        LLColor3 sunTint = LLColor3::white;         // [WolfViewer 2026-10-13] x the direct light: LL's own light's hue (hueSwap)
        LLColor3 ambTint = LLColor3::white;         // [WolfViewer 2026-10-13] x the sky's light: LL's own ambient hue (hueSwap)
        bool     llLights = false;                  // [WolfViewer 2026-10-13] the scene's sun and sky light are the stock EEP's ..
        LLColor3 llSunlit, llAmblit;                // .. (WolfEepReference::surfaceLights), in the frame's (the EEP's) units
        bool     valid = false;
    };
    // sNext is written while a frame is drawn (frameUpdate in the sky pass, the camera's cloud shadow after the
    // composite); sFrame is what every pass of a frame reads - sNext as it stood when the frame began (copied in
    // WolfEnvironmentRender::tier), so the scene's lights, the dome, the composite and the tone mapping of one
    // frame all use one exposure and one sun
    Frame sNext;
    Frame sFrame;

    // [WolfViewer 2026-10-13] the EEP's cloud cover, closed-loop (measureCloudCover): the takram coverage that makes the clouds
    // over the sky cover the share the EEP's clouds cover in the stock renderer, as measured; its last measurement for the log
    bool sCoverCtlValid = false;
    F32  sCoverScale = 3.5f;   // takram coverage / the stock share (measureCloudCover learns it; Iceland 10-13: 0.36 - 0.40 for 0.108)
    F32  sCoverMeasured = -1.f;
    S32  sCoverSamples = 0;
    // .. and the clouds' colour, closed-loop the same way (measureCloudCover): the gain on the clouds' light (wolfEepCloudTint)
    // that brings their measured colour to the stock clouds'; the last means for the log
    bool sCloudGainValid = false;   // the per-direction cloud gains (measureCloudCover) carry the clouds' colour
    // [WolfViewer 2026-10-13, round 16] the stock EEP cloud layer, drawn unseen into a small target from the same camera
    // (renderStockCloudProbe) and read back with measureCloudCover's own reading; what it shows drives the native clouds
    bool sStockDue = false;            // measureCloudCover started a reading: draw the stock layer for it this frame
    S32  sStockW = 0, sStockH = 0;     // the reading's size (the march target's), the probe's too
    U32  sStockSeqWanted = 0, sStockSeqRead = 0;   // which native reading the probe's read-back belongs to
    LLRenderTarget sStockRT;
    bool sStockRTEmissive = false;
    GLuint sStockPbo = 0;
    size_t sStockPboBytes = 0;
    bool sStockPending = false;
    S32  sStockPW = 0, sStockPH = 0;
    bool sStockValid = false;          // the coverage and the thinning below come from the stock layer
    F32  sCoverAbs = -1.f;             // the takram coverage, learned from the stock layer
    // [WolfViewer 2026-10-14] Paul at Lampert Highlands: "the cloud changing is way too fast". The coverage (and thinning) loop
    // stepped up to 5 % every reading toward a measurement that moves with the view and the drifting clouds, so it never settled
    // (cover 0.42 -> 0.14 -> 0.20 -> 0.16 in a minute), and every step re-formed the cloud shapes. Now it settles once, then
    // LOCKS: 3 readings in a row within 15 % of the stock share lock it; only a change of the EEP's own cloud share (> 15 %), a
    // teleport or a sky switch unlocks it.
    bool sCoverLocked = false;
    S32  sCoverSettled = 0;
    F32  sCoverLockTarget = -1.f;
    F32  sThin = 0.f, sThinTarget = 0.f;   // the clouds' thinning (cloudsF.glsl wolfEepCloudThin)
    F32  sStockShare = -1.f, sNativeShare = -1.f, sStockOpacity = -1.f, sNativeOpacity = -1.f;   // the last reading, for the log
    LLColor3 sCloudMeasured, sCloudTarget;

    /** bindSceneUniforms' record of what a program was last set up with (its uniforms persist in the program). */
    struct SceneUniformCache { U32 frame = U32_MAX; F32 mv[16] = {}; F32 pj[16] = {}; bool on = false; GLuint shadowTex = 0, tables = 0; };
    std::map<GLuint, SceneUniformCache> sSceneUniformCache;

    enum Section { SEC_LUTS = 0, SEC_SKY = 1, SEC_CLOUDS = 2, SEC_POST = 3 };
    const char* SECTION_NAMES[4] = { "atmosphere", "sky", "clouds", "post" };

    F32 luminance(const LLColor3& c) { return 0.2126f * c.mV[0] + 0.7152f * c.mV[1] + 0.0722f * c.mV[2]; }

    bool timersAvailable()
    {
        // glQueryCounter is OpenGL 3.3 core. On Windows llgl.cpp loads it as a pointer (llgl.cpp:228,
        // 1928, inside #if LL_WINDOWS): fail closed when the driver did not give one.
#if LL_WINDOWS
        if (glQueryCounter == nullptr || glGetQueryObjectui64v == nullptr) return false;
#endif
        return gGLManager.mGLVersion >= 3.29f;
    }

    F64 skyNow()
    {
        const F64 wolf = WolfEnvironmentState::instance().timeAhead();
        const F64 ahead = wolf > 0.0 ? wolf : WolfRegionWeather::instance().autoEnvTimeOffset();
        return LLDate::now().secondsSinceEpoch() + ahead;
    }

    void timerStamp(Section s, S32 which)
    {
        if (!timersAvailable() || gCubeSnapshot) return;
        GLuint& id = sGpu.q[sGpu.frame % 3][s][which];
        if (!id) glGenQueries(1, &id);
        glQueryCounter(id, GL_TIMESTAMP);
        if (which == 1) sGpu.qPending[sGpu.frame % 3][s] = true;
    }

    void timerCollect()
    {
        if (!timersAvailable()) return;
        const U32 slot = (sGpu.frame + 1) % 3;
        for (S32 s = 0; s < 4; ++s)
        {
            if (!sGpu.qPending[slot][s]) continue;
            GLuint64 ready = 0;
            glGetQueryObjectui64v(sGpu.q[slot][s][1], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready) continue;
            GLuint64 a = 0, b = 0;
            glGetQueryObjectui64v(sGpu.q[slot][s][0], GL_QUERY_RESULT, &a);
            glGetQueryObjectui64v(sGpu.q[slot][s][1], GL_QUERY_RESULT, &b);
            if (b > a) { sGpu.ms[s] += (b - a) / 1.0e6; sGpu.msFrames[s]++; }
            sGpu.qPending[slot][s] = false;
        }
    }

    /** "#rrggbb" colours are display (sRGB) values; the frame is linear. */
    LLColor3 lin(const LLColor3& c) { return linearColor3(c); }

    /** Source: WolfSim WolfEnvironment.cs BodyAt - fixed, or an orbit from the Unix epoch, as bearing (from
     *  north, clockwise) and height. Returned as a direction, x east, y north, z up. */
    LLVector3 bodyDir(const WolfEnvBody& b, F64 unix_seconds)
    {
        F64 az, alt;
        if (b.mMode == WolfEnvBody::FIXED)
        {
            az = b.mAzimuth; alt = b.mElevation;
        }
        else
        {
            const F64 hours = unix_seconds / 3600.0;
            const F64 angle = fmod(b.mPhase + 360.0 * hours / llmax(0.5, (F64)b.mPeriodHours), 360.0);
            const F64 th = angle * DEG_TO_RAD, tilt = b.mTilt * DEG_TO_RAD;
            const F64 east = cos(th), up = sin(th) * cos(tilt), north = -sin(th) * sin(tilt);
            alt = asin(llclamp(up, -1.0, 1.0)) * RAD_TO_DEG;
            az = atan2(east, north) * RAD_TO_DEG;
        }
        const F64 a = az * DEG_TO_RAD, e = alt * DEG_TO_RAD;
        return LLVector3((F32)(sin(a) * cos(e)), (F32)(cos(a) * cos(e)), (F32)sin(e));
    }

    void fail(const char* why)
    {
        if (!sGpu.warned)
        {
            LL_WARNS("WolfEnv") << "Wolf Environment rendering off for this session: " << why << " - the EEP sky is drawn instead" << LL_ENDL;
            sGpu.warned = true;
        }
        sGpu.failed = true;
    }

    bool programsReady()
    {
        return sAtmos && sAtmos->programsReady() && sClouds && sClouds->shadersReady()
            && gWolfEnvSkyProgram.isComplete() && gWolfEnvCompositeProgram[0].isComplete() && gWolfEnvCompositeProgram[1].isComplete()
            && gWolfEnvCameraShadowProgram.isComplete() && gWolfEnvLightProgram.isComplete();
    }

    /** The shared environment is on screen - not a personal sky, a script-pushed one or the EEP editor's - or one
     *  of the previews of it (the Wolf Environment draft; the auto environment's time preview, which is the
     *  region's own cycle in ENV_LOCAL: WolfWeather::usingSharedEnvironment counts it as shared too). */
    bool sharedSky()
    {
        if (WolfEnvironmentState::instance().previewing()) return true;
        LLEnvironment& env = LLEnvironment::instance();
        for (LLEnvironment::EnvSelection_t e : { LLEnvironment::ENV_EDIT, LLEnvironment::ENV_PUSH })
        {
            if (env.getEnvironmentDay(e) || env.getEnvironmentFixedSky(e)) return false;
        }
        if (WolfRegionWeather::instance().autoEnvPreviewOn()) return true;
        return !env.getEnvironmentDay(LLEnvironment::ENV_LOCAL) && !env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
    }

    // ── the moon (Paul's decision D5: WolfViewer's moon, converted to the new units) ────────────────────────
    LLPointer<LLViewerFetchedTexture> sMoonMap;
    F32 sMoonMeanAlbedo = -1.f;
    F32 moonMeanAlbedo()
    {
        if (sMoonMeanAlbedo >= 0.f) return sMoonMeanAlbedo;
        sMoonMeanAlbedo = 0.12f;   // until the file is read: the Moon's Bond albedo
        const std::string path = gDirUtilp->findSkinnedFilename("textures", "world/wolf_moon_lroc.png");
        LLPointer<LLImagePNG> png = new LLImagePNG();
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!path.empty() && png->load(path) && png->decode(raw, 0.f) && raw->getData())
        {
            LLImageDataSharedLock lock(raw);
            const U8* d = raw->getData();
            const S32 c = raw->getComponents();
            const size_t n = (size_t)raw->getWidth() * raw->getHeight();
            F64 sum = 0.0;
            for (size_t i = 0; i < n; ++i) for (S32 k = 0; k < llmin(c, 3); ++k) sum += d[i * c + k];
            if (n) sMoonMeanAlbedo = (F32)(sum / (n * llmin(c, 3) * 255.0));
        }
        return sMoonMeanAlbedo;
    }

    /** The mean of Lommel-Seeliger mu0 / (mu0 + mu) over the visible disc at this sun / moon geometry. */
    F32 lommelSeeligerMean(const LLVector3& moon, const LLVector3& sun)
    {
        LLVector3 north(0.f, 0.f, 1.f);
        north -= moon * (north * moon);
        if (north.normalize() < 1e-4f) north = LLVector3::y_axis;
        const LLVector3 east = north % (-moon);
        F64 s = 0.0; S32 n = 0;
        for (S32 j = 0; j < 24; ++j)
            for (S32 i = 0; i < 24; ++i)
            {
                const F32 x = -0.97f + 1.94f * i / 23.f, y = -0.97f + 1.94f * j / 23.f;
                const F32 r2 = x * x + y * y;
                if (r2 >= 1.f) continue;
                const LLVector3 nn = east * x + north * y + (-moon) * sqrtf(1.f - r2);
                const F32 mu0 = llmax(nn * sun, 0.f), mu = llmax(nn * (-moon), 0.f);
                s += mu0 / (mu0 + mu + 1e-4f); ++n;
            }
        return n ? (F32)(s / n) : 0.f;
    }

    /** Lunar north, world. Source: lldrawpoolwlsky.cpp renderHeavenlyBodies (the ecliptic north pole, RA 270,
     *  Dec 66.56, placed as LLVOWLSky::updateRealStars places a star: Meeus 12.4 GMST, 13.5/13.6). */
    LLVector3 lunarNorthAt(F64 seconds)
    {
        const F64 lat = WolfRegionWeather::instance().autoEnvLat() * DEG_TO_RAD;
        const F64 lon = WolfRegionWeather::instance().autoEnvLon();
        const F64 jd = seconds / 86400.0 + 2440587.5;
        const F64 t = (jd - 2451545.0) / 36525.0;
        const F64 gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
        const F64 h = fmod(fmod(gmst + lon, 360.0) + 360.0, 360.0) * DEG_TO_RAD - 270.0 * DEG_TO_RAD;
        const F64 dec = 66.56 * DEG_TO_RAD;
        return LLVector3((F32)(-cos(dec) * sin(h)), (F32)(sin(dec) * cos(lat) - cos(dec) * cos(h) * sin(lat)),
                         (F32)(sin(dec) * sin(lat) + cos(dec) * cos(h) * cos(lat)));
    }
    LLVector3 lunarNorth() { return lunarNorthAt(skyNow()); }

    /** [WolfViewer 2026-10-13] The real planets for Earth mode (Paul: "the planets should only be visible if they are visible
     *  in the night sky at the time"). Source: JPL SSD "Approximate Positions of the Planets" (E. M. Standish), Table 1
     *  (Keplerian elements and rates, J2000 ecliptic, valid 1800-2050) and its algorithm: M = L - varpi; Kepler's equation by
     *  Newton's method; x' = a(cos E - e), y' = a sqrt(1 - e^2) sin E; rotated by omega = varpi - Omega, I, Omega to the J2000
     *  ecliptic; to the equator by the J2000 obliquity 23.43928 deg; geocentric = planet - EM barycentre. Placed as lunarNorth
     *  places a direction (Meeus 12.4 GMST, 13.5/13.6). Brightness: m = H + 5 log10(r_sun r_earth) (Astronomical Almanac H via
     *  Wikipedia "Absolute magnitude"; the phase term is left out, so a crescent Venus/Mercury reads a little bright), and
     *  E = 10^(-(m + 14.18) / 2.5) lux (Wikipedia "Apparent magnitude"). Out: world direction + illuminance (lux), above
     *  the horizon only. */
    struct RealPlanet { LLVector3 dir; F32 lux; };
    std::vector<RealPlanet> realPlanets(F64 unix_seconds, F64 lat_deg, F64 lon_deg)
    {
        // a, e, I, L, varpi, Omega and their rates per Julian century (JPL Table 1)
        static const F64 EL[8][12] = {
            { 0.38709927, 0.00000037, 0.20563593, 0.00001906, 7.00497902, -0.00594749, 252.25032350, 149472.67411175, 77.45779628, 0.16047689, 48.33076593, -0.12534081 },
            { 0.72333566, 0.00000390, 0.00677672, -0.00004107, 3.39467605, -0.00078890, 181.97909950, 58517.81538729, 131.60246718, 0.00268329, 76.67984255, -0.27769418 },
            { 1.00000261, 0.00000562, 0.01671123, -0.00004392, -0.00001531, -0.01294668, 100.46457166, 35999.37244981, 102.93768193, 0.32327364, 0.0, 0.0 },
            { 1.52371034, 0.00001847, 0.09339410, 0.00007882, 1.84969142, -0.00813131, -4.55343205, 19140.30268499, -23.94362959, 0.44441088, 49.55953891, -0.29257343 },
            { 5.20288700, -0.00011607, 0.04838624, -0.00013253, 1.30439695, -0.00183714, 34.39644051, 3034.74612775, 14.72847983, 0.21252668, 100.47390909, 0.20469106 },
            { 9.53667594, -0.00125060, 0.05386179, -0.00050991, 2.48599187, 0.00193609, 49.95424423, 1222.49362201, 92.59887831, -0.41897216, 113.66242448, -0.28867794 },
            { 19.18916464, -0.00196176, 0.04725744, -0.00004397, 0.77263783, -0.00242939, 313.23810451, 428.48202785, 170.95427630, 0.40805281, 74.01692503, 0.04240589 },
            { 30.06992276, 0.00026291, 0.00859048, 0.00005105, 1.77004347, 0.00035372, -55.12002969, 218.45945325, 44.96476227, -0.32241464, 131.78422574, -0.00508664 } };
        static const F64 H[8] = { -0.4, -4.4, 0.0, -1.5, -9.4, -9.7, -7.2, -6.9 };   // Earth (index 2) is the observer
        const F64 D = DEG_TO_RAD;
        const F64 jd = unix_seconds / 86400.0 + 2440587.5;
        const F64 T = (jd - 2451545.0) / 36525.0;
        auto helio = [&](S32 p) -> glm::dvec3
        {
            const F64* k = EL[p];
            const F64 a = k[0] + k[1] * T, e = k[2] + k[3] * T, I = (k[4] + k[5] * T) * D;
            const F64 L = k[6] + k[7] * T, vp = k[8] + k[9] * T, Om = (k[10] + k[11] * T) * D;
            const F64 w = vp * D - Om;
            const F64 M = fmod(fmod(L - vp + 180.0, 360.0) + 360.0, 360.0) - 180.0;   // -180..180 (deg)
            F64 E = M * D + e * sin(M * D);
            for (S32 i = 0; i < 12; ++i) { const F64 dE = (M * D - (E - e * sin(E))) / (1.0 - e * cos(E)); E += dE; if (fabs(dE) < 1e-12) break; }
            const F64 xp = a * (cos(E) - e), yp = a * sqrt(1.0 - e * e) * sin(E);
            const F64 cw = cos(w), sw = sin(w), cO = cos(Om), sO = sin(Om), cI = cos(I), sI = sin(I);
            return glm::dvec3((cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
                              (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
                              (sw * sI) * xp + (cw * sI) * yp);
        };
        const glm::dvec3 earth = helio(2);
        const F64 eps = 23.43928 * D;
        const F64 gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0;
        const F64 lat = lat_deg * D;
        std::vector<RealPlanet> out;
        for (S32 p = 0; p < 8; ++p)
        {
            if (p == 2) continue;
            const glm::dvec3 hp = helio(p);
            const glm::dvec3 g = hp - earth;
            const glm::dvec3 q(g.x, cos(eps) * g.y - sin(eps) * g.z, sin(eps) * g.y + cos(eps) * g.z);
            const F64 dist = glm::length(q);
            const F64 ra = atan2(q.y, q.x) / D, dec = asin(q.z / dist);
            const F64 h = fmod(fmod(gmst + lon_deg - ra, 360.0) + 360.0, 360.0) * D;   // hour angle, as lunarNorth
            const LLVector3 dir((F32)(-cos(dec) * sin(h)), (F32)(sin(dec) * cos(lat) - cos(dec) * cos(h) * sin(lat)),
                                (F32)(sin(dec) * sin(lat) + cos(dec) * cos(h) * cos(lat)));
            if (dir.mV[VZ] <= 0.f) continue;   // below the horizon: not in the sky now
            const F64 m = H[p] + 5.0 * log10(glm::length(hp) * dist);
            out.push_back({ dir, (F32)pow(10.0, -(m + 14.18) / 2.5) });
        }
        return out;
    }

    /** Coverage as the auto environment counts it. Source: WolfAutoEnvironmentModule.cs Coverage. */
    /** [WolfViewer round 11] Cox & Munk 1954 total mean square slope sigma^2 = 0.003 + 5.12e-3 U (Ocean Optics Web Book, eq. 4;
     *  wolftk/seaGlint.glsl wolfCoxMunkMss): U the document's wind (MET windSpeed, wolfenvironmentstate.cpp:225 - 10 m, where
     *  Cox-Munk's is at 12.5 m; the difference is not corrected), the same wind the ocean's waves use (wolfoceanfft.cpp). */
    F32 seaMeanSquareSlope()
    {
        const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
        return 0.003f + 5.12e-3f * llmax(d.mWindSpeed, 0.f);
    }

    /** Source: three-geospatial packages/clouds/src/shaders/clouds.glsl:15-42 getCubeSphereUv (the same mapping the clouds'
     *  weather texture is on, wolftk/cloudsCommon.glsl), for a direction from the planet's centre (ECEF). */
    LLVector2 cubeSphereUv(const LLVector3d& position)
    {
        LLVector3d n = position; n.normVec();
        const F64 fx = fabs(n.mdV[0]), fy = fabs(n.mdV[1]), fz = fabs(n.mdV[2]);
        const F64 mx = llmax(fx, llmax(fy, fz));
        const F64 cx = n.mdV[0] / mx, cy = n.mdV[1] / mx, cz = n.mdV[2] / mx;
        F64 m0, m1;
        if (fy > fx && fy > fz) { if (cy > 0.0) { m0 = -n.mdV[0]; m1 = n.mdV[2]; } else { m0 = n.mdV[0]; m1 = n.mdV[2]; } }
        else if (fx > fy && fx > fz) { if (cx > 0.0) { m0 = n.mdV[1]; m1 = n.mdV[2]; } else { m0 = -n.mdV[1]; m1 = n.mdV[2]; } }
        else { if (cz > 0.0) { m0 = n.mdV[0]; m1 = n.mdV[1]; } else { m0 = n.mdV[0]; m1 = -n.mdV[1]; } }
        const F64 a = m0 * m0, b = m1 * m1;
        const F64 q = -2.0 * a + 2.0 * b - 3.0, q2 = q * q;
        const F64 ux = sqrt(llmax(1.5 + a - b - 0.5 * sqrt(llmax(-24.0 * a + q2, 0.0)), 0.0)) * (m0 > 0.0 ? 1.0 : -1.0);
        const F64 uy = sqrt(6.0 / (3.0 - ux * ux)) * m1;
        return LLVector2((F32)(ux * 0.5 + 0.5), (F32)(uy * 0.5 + 0.5));
    }

    F32 weatherCover(const WolfEnvDoc& d)
    {
        if (!d.mHaveWeather) return 0.3f;
        if (d.mWeatherLow + d.mWeatherMid + d.mWeatherHigh <= 0.f) return d.mWeatherCloud;
        return llclamp(llmax(d.mWeatherLow, llmax(0.8f * d.mWeatherMid, 0.35f * d.mWeatherHigh)), 0.f, 1.f);
    }

    /** What the camera's zones change: each zone's override blended in by its strength where the camera is
     *  (wolfenvironmentstate.cpp WolfEnvZone::strengthAt), in list order. */
    struct ZoneMix
    {
        F32 haze = 0.f;
        LLColor3 lightMul = LLColor3::white;
        bool hasClouds = false; F32 clouds = 0.f;
    };

    ZoneMix zonesAtCamera(const WolfEnvDoc& d)
    {
        ZoneMix z;
        z.haze = d.mHaze;
        z.hasClouds = d.mHasClouds; z.clouds = d.mClouds;
        const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
        for (const WolfEnvZone& zn : d.mZones)
        {
            const F32 w = zn.strengthAt(cam);
            if (w <= 0.f) continue;
            if (zn.mHasHaze) z.haze = lerp(z.haze, zn.mHaze, w);
            if (zn.mHasClouds) { z.clouds = z.hasClouds ? lerp(z.clouds, zn.mClouds, w) : zn.mClouds; z.hasClouds = true; }
            if (zn.mHasLight || zn.mHasLightStrength || zn.mHasMood)
            {
                LLColor3 m = zn.mHasLight ? lin(zn.mLight) : LLColor3::white;
                const F32 top = llmax(m.mV[0], llmax(m.mV[1], m.mV[2]));
                if (top > 0.f) m = m * (1.f / top);
                m = m * (zn.mHasLightStrength ? zn.mLightStrength : 1.f);
                if (zn.mHasMood) m = componentMult(m, lerp(LLColor3::white, lin(zn.mTint), zn.mTintStrength * 0.5f));
                z.lightMul = componentMult(z.lightMul, lerp(LLColor3::white, m, w));
            }
        }
        return z;
    }

    /**
     * Places the camera on the planet: three-geospatial works in ECEF metres on a globe (the camera's
     * "worldToECEFMatrix", AtmosphereMaterialBase.ts:81-82); the grid is flat. The camera's global grid position
     * becomes a longitude / latitude on a sphere of the atmosphere's bottom radius (x east -> longitude,
     * y north -> latitude, an equirectangular map, so the clouds' weather texture - cube-sphere UVs of the
     * position, clouds.glsl:15-46 - stays fixed to the ground across region crossings), the camera at its height
     * above the sea there, and the agent frame's east / north / up turned onto that point's local east /
     * north / up. A rigid transform, rebuilt every frame; directions (the sun) rotate with it. With a sphere,
     * three-geospatial's altitude correction is 0 (getAltitudeCorrectionOffset.ts:7-21: the osculating sphere's
     * centre is the planet's).
     */
    void placeOnPlanet(F64 bottom_radius_m)
    {
        const LLVector3 camLocal = LLViewerCamera::getInstance()->getOrigin();
        const LLVector3d camGlobal = gAgent.getPosGlobalFromAgent(camLocal);
        F32 sea = 0.f;
        if (LLViewerRegion* r = gAgent.getRegion()) sea = r->getWaterHeight();
        const F64 R = bottom_radius_m;
        // The grid's origin sits where three-geospatial's own clouds story places its camera: longitude 30 deg
        // (storybook/src/clouds/Clouds-Basic.tsx:28-31), latitude 35 deg (storybook/src/helpers/useLocationControls.tsx:17,
        // the default it leaves). Not at a cube-sphere face's centre: there getCubeSphereUv (clouds.glsl:15-42) takes the
        // square root of a tiny difference of large numbers and, in 32-bit floats, the weather UV moves in steps - the
        // straight seams the clouds harness showed with the camera at (0, 0, R).
        const F64 lon0 = 30.0 * DEG_TO_RAD, lat0 = 35.0 * DEG_TO_RAD;
        // the latitude kept short of the pole, where east / north turn over (a grid would have to reach 6,000 km
        // north of its origin for that); past it the camera's latitude stays, its longitude still follows
        const F64 lon = lon0 + camGlobal.mdV[VX] / R;
        const F64 lat = llclamp(lat0 + camGlobal.mdV[VY] / R, -85.0 * DEG_TO_RAD, 85.0 * DEG_TO_RAD);
        const glm::dvec3 E(-sin(lon), cos(lon), 0.0);
        const glm::dvec3 N(-sin(lat) * cos(lon), -sin(lat) * sin(lon), cos(lat));
        const glm::dvec3 U(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
        // at least 1 m above the bottom of the atmosphere: Bruneton's functions take r >= bottom_radius
        // (the asserts of common.glsl, e.g. GetTransmittanceTextureUvFromRMu)
        const F64 h = llmax((F64)(camLocal.mV[VZ] - sea), 1.0);
        const glm::dvec3 C = U * (R + h);
        const glm::dvec3 cam((F64)camLocal.mV[VX], (F64)camLocal.mV[VY], (F64)camLocal.mV[VZ]);
        glm::dmat4 m(1.0);
        m[0] = glm::dvec4(E, 0.0);
        m[1] = glm::dvec4(N, 0.0);
        m[2] = glm::dvec4(U, 0.0);
        m[3] = glm::dvec4(C - E * cam.x - N * cam.y - U * cam.z, 1.0);
        sNext.worldToECEF = m;
        sNext.ecefToWorld = glm::inverse(m);
        sNext.cameraECEF.setVec(C.x, C.y, C.z);
        sNext.bottomRadius = R;
        sNext.cameraHeight = (F32)h;
    }

    LLVector3 toECEF(const Frame& F, const LLVector3& dirWorld)
    {
        const glm::dvec4 d = F.worldToECEF * glm::dvec4(dirWorld.mV[0], dirWorld.mV[1], dirWorld.mV[2], 0.0);
        LLVector3 v((F32)d.x, (F32)d.y, (F32)d.z);
        v.normalize();
        return v;
    }

    LLVector3 cameraECEFKm(const Frame& F)
    {
        // Source: constants.ts:21 METER_TO_LENGTH_UNIT = 1 / 1000 (km fit a float: 6.4e3 with 1e-3 resolution)
        return LLVector3(F.cameraECEF * 1.0e-3);
    }

    /**
     * [WolfViewer 2026-10-13] A colour's hue: scaled to unit luminance, each channel held to 3 x the luminance (then the
     * luminance made 1 again) so a saturated colour cannot throw one channel's light to infinity; black gives white.
     */
    LLColor3 hueOf(const LLColor3& c)
    {
        LLColor3 r(llmax(c.mV[0], 0.f), llmax(c.mV[1], 0.f), llmax(c.mV[2], 0.f));
        F32 y = luminance(r);
        if (y <= 1.0e-6f) return LLColor3::white;
        r *= 1.f / y;
        for (S32 i = 0; i < 3; ++i) r.mV[i] = llmin(r.mV[i], 3.f);
        y = luminance(r);
        return y > 1.0e-6f ? r * (1.f / y) : LLColor3::white;
    }

    /** [WolfViewer 2026-10-13] Per channel hue(want) / hue(have): x a light of hue `have` gives hue `want`, its luminance kept. */
    LLColor3 hueSwap(const LLColor3& want, const LLColor3& have)
    {
        const LLColor3 w = hueOf(want), h = hueOf(have);
        return LLColor3(w.mV[0] / llmax(h.mV[0], 1.0e-4f), w.mV[1] / llmax(h.mV[1], 1.0e-4f), w.mV[2] / llmax(h.mV[2], 1.0e-4f));
    }

    /** Per channel b / a (0 where a is 0): a transmittance from two illuminances. */
    LLColor3 ratio(const LLColor3& b, const LLColor3& a)
    {
        return LLColor3(a.mV[0] > 0.f ? b.mV[0] / a.mV[0] : 0.f, a.mV[1] > 0.f ? b.mV[1] / a.mV[1] : 0.f, a.mV[2] > 0.f ? b.mV[2] / a.mV[2] : 0.f);
    }

    /**
     * The frame's light at the camera and the exposure (absolute values):
     *   sun        three-geospatial getSunLightColor (getSunLightColor.ts:48-103): transmittance to the sun x the
     *              solar irradiance x the sun's spectral-radiance-to-luminance factor (WolfTk::Atmosphere)
     *   sky        SkyLightProbe (SkyLightProbe.ts:85-129): the irradiance table at the camera
     *   moon       (decision D5) Krisciunas & Schaefer 1991 eq. 8-9 from the phase angle, through the same
     *              transmittance; it lights the scene when the sun is down (LL's light direction is then the moon)
     *   meter      Filament's incident meter (WolfExposure::Meter) on the horizontal illuminance: sun (or moon)
     *              x the clouds' transmittance at the camera (last frame's shadow-map sample) + sky + the
     *              natural night sky's pi L (an isotropic sky of luminance L gives E = pi L on a horizontal plane)
     */
    void updateLight(const WolfEnvDoc& d, F32 dt)
    {
        // [WolfViewer 2026-10-13] a teleport is a cut: the clouds' cover and genus easing (GENUS_TAU, 20 s) is for weather
        // changing in place, but after a teleport it showed the previous place's clouds melting away (Paul: "lots of weird
        // white clouds that gradually disappear"). A camera jump of more than 300 m in one update (a region is 256 m; no
        // flight covers that in a frame) restarts the cover and the genera at the new place's values. Crossings still blend.
        static bool s_cut = false;
        {
            static LLVector3d s_lastCam; static bool s_haveCam = false;
            const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
            s_cut = s_haveCam && (cam - s_lastCam).magVec() > 300.0;
            s_lastCam = cam; s_haveCam = true;
            if (s_cut) sGenusValid = false;
        }
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
        const F64 now = skyNow();
        LLVector3 sun = LLVector3::z_axis;
        // [WolfViewer 2026-10-13] Paul: "on jimmy's eep the sun is in a different place". The EEP is the basis: its sun and
        // moon (LLSettingsSky::getSunDirection / getMoonDirection - where LL draws and lights them) are the native sky's
        // whenever there is an EEP sky; the document's own body motion only without one.
        if (psky)
        {
            sun = psky->getSunDirection();
        }
        else if (!d.mSuns.empty())
        {
            sun = bodyDir(d.mSuns[0], now);
        }
        sun.normalize();
        LLVector3 moon = psky ? psky->getMoonDirection() : -LLVector3::z_axis;
        if (!psky && !d.mMoons.empty()) moon = bodyDir(d.mMoons[0], now);
        moon.normalize();
        sNext.sunWorld = sun; sNext.moonWorld = moon;
        sNext.sunECEF = toECEF(sNext, sun);
        sNext.sunUp = sun.mV[VZ] > 0.f;

        const LLVector3 camKm = cameraECEFKm(sNext);
        LLColor3 sunH, skyH;
        sAtmos->horizontalIlluminance(camKm, sNext.sunECEF, &sunH, &skyH);
        sNext.sunNormal = d.mSuns.empty() ? LLColor3::black : sAtmos->sunIlluminance(camKm, sNext.sunECEF);
        sNext.sunHorizontal = d.mSuns.empty() ? LLColor3::black : sunH;
        sNext.skyHorizontal = skyH;
        if (!d.mSuns.empty())
        {
            const WolfEnvBody& b = d.mSuns[0];
            const LLColor3 tint = (b.mHasColor ? lin(b.mColor) : LLColor3::white) * b.mBrightness;
            sNext.sunNormal = componentMult(sNext.sunNormal, tint);
            sNext.sunHorizontal = componentMult(sNext.sunHorizontal, tint);
        }

        // under the clouds (wolftk/atmosphereLibF.glsl wolfTkCloudSkyLight, last frame's reading): the clear sky's
        // light passes their diffuse transmittance, and the share of the sun's beam they scatter instead of
        // stopping reaches the ground as sky light too (Stephens AT622 notes section 15, eq. 15.20b). The shadow map
        // is the sun's while it is up; the moon's once it lights the clouds (below: the moon's sky light under them the
        // same way); between (the sun 0..6 deg under) the twilight sky is left as the clear sky gives it.
        sNext.skyUnderCloud = LLColor3::white;
        if (sNext.sunUp)
        {
            const LLColor3 clear = sNext.skyHorizontal;
            sNext.skyHorizontal = sNext.skyHorizontal * sNext.cloudDiffuseT + sNext.sunHorizontal * sNext.cloudDiffusedBeam;
            // the ratio the clouds' haze is lit by (cloudsF.glsl approximateHaze takes Bruneton's clear-sky irradiance):
            // 1 with no clouds (T 1, beam 0); a channel the clear sky gives nothing in (the sun on the horizon) stays 1
            for (S32 i = 0; i < 3; ++i)
            {
                sNext.skyUnderCloud.mV[i] = clear.mV[i] > 0.f ? sNext.skyHorizontal.mV[i] / clear.mV[i] : 1.f;
            }
        }

        // Krisciunas & Schaefer 1991 eq. 8-9 (I* in foot-candles, outside the atmosphere)
        const F32 elong = acosf(llclamp(sun * moon, -1.f, 1.f)) * RAD_TO_DEG;
        const F32 alpha = 180.f - elong;
        sNext.moonPhaseAngle = alpha;
        const F64 m = -12.73 + 0.026 * fabs(alpha) + 4.0e-9 * pow(alpha, 4.0);
        F32 moonLux = (F32)(pow(10.0, -0.4 * (m + 16.57)) * FOOTCANDLE_LUX);
        if (d.mMoons.empty()) moonLux = 0.f;
        else
        {
            const WolfEnvBody& b = d.mMoons[0];
            moonLux *= b.mBrightness;
            if (b.mHasLit) moonLux = (F32)(pow(10.0, -0.4 * (-12.73 + 16.57)) * FOOTCANDLE_LUX) * b.mLit * b.mBrightness;
        }
        sNext.moonTOA = moonLux;
        // the moon's light through the same atmosphere: the transmittance toward the moon's direction (the
        // illuminance getSunLightColor gives there / the sun's above the atmosphere)
        const LLColor3 tMoon = ratio(sAtmos->sunIlluminance(camKm, toECEF(sNext, moon)), sAtmos->sunIlluminanceAboveAtmosphere());
        sNext.moonNormal = tMoon * moonLux;
        // the moonlit air (skyF.glsl wolfSkyWithBodies: the tables in the moon's direction x moon / sun above the
        // atmosphere) and the sky light it gives, added to the sky's while the sun is down
        sNext.moonECEF = toECEF(sNext, moon);
        const F32 sunTop = luminance(sAtmos->sunIlluminanceAboveAtmosphere());
        sNext.moonSkyScale = (!sNext.sunUp && sunTop > 0.f) ? moonLux / sunTop : 0.f;
        // [WolfViewer round 8] the clouds' light (Frame::cloudLightECEF): the moon once the sun is 6 deg under, sin(6 deg)
        sNext.moonLightsClouds = sun.mV[VZ] < -0.1045f && sNext.moonSkyScale > 0.f;
        sNext.cloudLightECEF = sNext.moonLightsClouds ? sNext.moonECEF : sNext.sunECEF;
        if (sNext.moonSkyScale > 0.f)
        {
            LLColor3 moonSunPart, moonSkyPart;
            sAtmos->horizontalIlluminance(camKm, sNext.moonECEF, &moonSunPart, &moonSkyPart);
            LLColor3 moonSky = moonSkyPart * sNext.moonSkyScale;
            if (sNext.moonLightsClouds)
            {
                // the shadow map is the moon's: its sky light under the clouds as the sun's by day (above), and the ratio
                // the clouds' haze is lit by
                const LLColor3 clear = moonSky;
                moonSky = moonSky * sNext.cloudDiffuseT + moonSunPart * sNext.moonSkyScale * sNext.cloudDiffusedBeam;
                for (S32 i = 0; i < 3; ++i)
                {
                    sNext.skyUnderCloud.mV[i] = clear.mV[i] > 0.f ? moonSky.mV[i] / clear.mV[i] : 1.f;
                }
            }
            sNext.skyHorizontal += moonSky;
        }

        // [WolfViewer round 9] the natural night sky's background (NIGHT_SKY_ZENITH_CD: airglow + zodiacal light + integrated
        // starlight, Leinert et al. 1998 A&AS 127:1 - a dark rural sky 21.6-22.0 mag/arcsec2 = 1.7-2.5e-4 cd/m2; the dome
        // draws it, skyF.glsl uNightSky) lights the ground too: an isotropic sky of luminance L gives E = pi L on a horizontal
        // plane, so it joins the sky light the scene is lit by - a moonless night's ground was black (Paul, 03:35Z)
        sNext.skyHorizontal += LLColor3(F_PI * NIGHT_SKY_ZENITH_CD, F_PI * NIGHT_SKY_ZENITH_CD, F_PI * NIGHT_SKY_ZENITH_CD);

        // the meter (the night sky's pi L is in skyHorizontal now)
        const LLColor3 direct = sNext.sunUp ? sNext.sunHorizontal : sNext.moonNormal * llmax(moon.mV[VZ], 0.f);
        sNext.meterLux = luminance(direct) * sNext.cloudTransmittanceAtCamera + luminance(sNext.skyHorizontal);
        // Filament.md.html "Exposure compensation": the viewer's own RenderExposure as EC (1 = none); the
        // same 0.5..4 range LLPipeline::tonemap clamps it to
        static LLCachedControl<F32> render_exposure(gSavedSettings, "RenderExposure", 1.f);
        const F32 ec = log2f(llclamp((F32)render_exposure, 0.5f, 4.f));
        // last frame's log-average luminance (postPassInner, wolftk/logLumF.glsl): the reflected reading the target
        // EV100 is taken from (wolfexposure.h); none on the first frame or without the program
        // [WolfViewer round 9] a cut (an environment preset switched by hand, a sky swapped with no transition) is no
        // scene to adapt through: the metered light moving more than CUT_EV in one frame snaps the meter (wolfexposure.h)
        {
            static F32 s_lastEv = 0.f; static bool s_haveLast = false;
            const F32 evNow = WolfExposure::ev100FromIlluminance(llmax(sNext.meterLux, 1.0e-12f));
            if (s_haveLast && fabsf(evNow - s_lastEv) > WolfExposure::CUT_EV) sMeter.cut();
            s_lastEv = evNow; s_haveLast = true;
        }
        // [WolfViewer round 9] the night floor follows the moon's direct light at the camera (wolfexposure.h nightFloorEv100)
        const F32 moonDirect = sNext.sunUp ? 0.f : luminance(sNext.moonNormal) * llmax(moon.mV[VZ], 0.f);
        sNext.exposure = sMeter.update(sNext.meterLux, dt, ec, sNext.frameLogAvg, sNext.skyLuminanceShare, moonDirect);

        // the clouds' coverage (decision D9: one coverage, three-geospatial's): the auto environment's rule
        // (WolfAutoEnvironmentModule.cs Coverage), or the document's / a zone's own cover
        const ZoneMix z = zonesAtCamera(d);
        // [WolfViewer round 9] Paul: "cloud of the weather - that should depend on the eep settings": the EEP sky the server
        // bakes is the basis. Its cloud_shadow IS the weather's coverage (WolfAutoEnvironmentModule.cs OverlayWeather writes
        // Coverage() into it; LLSettingsSky::getCloudShadow, llsettingssky.h SETTING_CLOUD_SHADOW), so the native clouds take
        // it from there (a zone's own cloud setting still overrides at the camera); the MET fields below only arrange the
        // genera within it. The viewer's own rule (weatherCover) is kept for a document with no EEP sky at hand.
        {
            // [WolfViewer 2026-10-13] Paul: "we still have no clouds - jimmy has amazing clouds". cloud_shadow is not LL's cloud
            // fraction; the share of the sky the EEP's clouds actually cover is the stock cloud opacity's mean over the cloud
            // noise texture (WolfEepReference::cloudCoverage: [S] cloudsV.glsl:184, cloudsF.glsl:59-127 transcribed, the noise's
            // measured histogram). That share is the native coverage.
            // ...driven by measurement: measureCloudCover adjusts the coverage until the clouds measured over the sky cover the
            // stock share (WolfEepReference::cloudCoveredFraction); before its first measurement, that share itself
            // [round 16] once the stock layer itself has been measured (renderStockCloudProbe), the coverage learned from it
            // [WolfViewer 2026-10-14] audit S9: an EEP with no clouds at all means no clouds, at once - the learned coverage
            // (measureCloudCover) only moves 5 % a reading and is held at 0.02 or more, so a clear region reached by a crossing
            // kept a thin veil for a minute and never cleared
            const F32 eepShareNow = psky ? WolfEepReference::cloudCoveredFraction(psky) : 0.f;
            const F32 eepCover = (!psky || eepShareNow <= 0.f) ? 0.f : (sStockValid && sCoverAbs >= 0.f) ? sCoverAbs
                                 : llclamp(eepShareNow * sCoverScale, 0.f, 1.f);
            const F32 coverTarget = llclamp(z.hasClouds ? z.clouds : (psky ? eepCover : weatherCover(d)), 0.f, 1.f);
            // [WolfViewer round 17] the EEP's cloud_shadow steps (a region's 120 s sky transition, a day cycle's keyframes, a
            // crossing): the clouds' coverage follows over GENUS_TAU (20 s, as the genus layers do) instead of jumping -
            // Paul's log showed cover 1.00 -> 0.37 between minute lines while "the clouds are moving very very fast"
            static F32 s_cover = -1.f;
            // [WolfViewer 2026-10-13] the measured EEP cover (measureCloudCover) is a closed loop with its own pace: taken as it is
            // (eased over 5 s: the measured loop's own readings must never show as the clouds growing and shrinking)
            const bool closedLoop = psky && sCoverCtlValid && !z.hasClouds;
            const F32 tau = closedLoop ? 30.f : GENUS_TAU;   // [2026-10-14] 30 s (was 5): a settle or re-settle shows as slow change, never as shapes re-forming
            s_cover = (s_cover < 0.f || dt <= 0.f || s_cut) ? coverTarget : s_cover + (coverTarget - s_cover) * (1.f - expf(-dt / tau));
            sNext.cover = s_cover;
        }
        // [WolfViewer step 3] the cloud genera from the weather (wolfcloudgenus.cpp, BEAUTY_RESEARCH.md section 9.4): the
        // three fractions read as three layers (+ a fog / pannus slab), each a genus with its base, extinction and
        // phase. A zone's own cloud setting, or no weather, keeps takram's default layers under the one coverage.
        if (sClouds && d.mHaveWeather && !z.hasClouds)
        {
            WolfCloudGenus::Weather w;
            w.low = d.mWeatherLow; w.medium = d.mWeatherMid; w.high = d.mWeatherHigh; w.total = d.mWeatherCloud;
            w.fog = d.mWeatherFog; w.precip = d.mPrecipitation; w.thunderPct = d.mThunderChance; w.symbol = d.mWeatherSymbol;
            w.wind = d.mWindSpeed; w.windFrom = d.mWindFrom; w.T = d.mTemperature; w.RH = d.mHumidity; w.haveTemperature = d.mHaveTemperature;
            w.sunUp = sNext.sunUp;
            // [WolfViewer 2026-10-14] THE EEP IS THE BASIS for how much cloud there is. Paul at Iceland: Jimmy's EEP was total rain
            // overcast (its stock layer veiled 99.95 % of the sky at opacity 0.94) while the forecast fractions gave only cirrus
            // (Ci 0.73, Ac 0.01), and no coverage can make cirrus an overcast: the native sky came out clear and bright and the
            // scene lost the overcast's soft light ("Jimmy's standard EEP looks incredible"). The EEP's cloud share
            // (WolfEepReference::cloudCoveredFraction, the stock layer's opaque share) now raises the forecast's total, the missing
            // part going to the low band - what a grey EEP sheet is - so the genus rules choose the overcast's own clouds
            // (rain under > 86 %: rule 7 Ns + pannus; dry: St / Sc). A forecast with more cloud than the EEP keeps its total and its
            // middle and high bands; its low band is never less than the EEP's share (below).
            if (psky && WolfEnvironmentRender::eepDriven())
            {
                const F32 eepShare = llclamp(WolfEepReference::cloudCoveredFraction(psky), 0.f, 1.f);
                if (eepShare > w.total) w.total = eepShare;
                // [WolfViewer 2026-10-14] the EEP's clouds are one layer of low cloud (LL draws them as a single cloud dome), so
                // their share is a floor on the LOW band, whatever the other bands hold. Before, only the part missing from the
                // forecast's total went low; when the forecast's total was larger (Paul 10-14 at dusk: Cu 0.047, Ac 0.109, the
                // stock layer veiling 44 % of the sky) the EEP's cover was shared out in the forecast's proportions below and
                // landed on the thin Ac layer at 3 km - flat, identical pancakes at one height.
                w.low = llmax(w.low, eepShare);
                w.eepLow = eepShare;
            }
            // the local solar hour from the sun's azimuth: the sun transits the meridian at 12 h and moves 15 deg/h
            // (the hour angle); a southern transit is assumed (the Wolf grid's places), so east = morning
            const F32 az = atan2f(sun.mV[VX], sun.mV[VY]) * RAD_TO_DEG;   // from north, clockwise (x east, y north)
            w.hour = 12.f + (fmodf(az - 180.f + 540.f, 360.f) - 180.f) / 15.f;
            // [WolfViewer 2026-10-14] Paul: "where we are timed to". A region on a real place's sky shows that place at the moment
            // autoEnvSkyTime (grid wall clock + skyShift, auto_env_lib.php ae_timezone_fields): its local mean solar hour is that
            // UTC time + longitude / 15 (15 deg of longitude per hour), exact for any hemisphere, where the azimuth rule above
            // assumes the sun crosses the meridian in the south. Regions on their own day cycle keep the azimuth rule.
            WolfRegionWeather& rw = WolfRegionWeather::instance();
            if (rw.autoEnvRealSky())
            {
                F64 h = fmod(rw.autoEnvSkyTime() / 3600.0 + rw.autoEnvLon() / 15.0, 24.0);
                if (h < 0.0) h += 24.0;
                w.hour = (F32)h;
                // [WolfViewer 2026-10-14] audit B1: Weather::summer was never set (always true). The warm half-year of the place:
                // the sun on the place's side of the equator (declination with the latitude's sign), or the tropics (|lat| under
                // the obliquity, 23.439 deg), where it is always warm. Declination from the U.S. Naval Observatory's "Approximate
                // Solar Coordinates": n = JD - 2451545, L = 280.459 + 0.98564736 n, g = 357.529 + 0.98560028 n,
                // lambda = L + 1.915 sin g + 0.020 sin 2g, sin(delta) = sin(23.439) sin(lambda); JD = unix / 86400 + 2440587.5.
                const F64 n = rw.autoEnvSkyTime() / 86400.0 + 2440587.5 - 2451545.0;
                const F64 L = 280.459 + 0.98564736 * n, g = (357.529 + 0.98560028 * n) * DEG_TO_RAD;
                const F64 lambda = (L + 1.915 * sin(g) + 0.020 * sin(2.0 * g)) * DEG_TO_RAD;
                const F64 sinDelta = sin(23.439 * DEG_TO_RAD) * sin(lambda);
                const F64 lat = rw.autoEnvLat();
                w.summer = fabs(lat) < 23.439 || lat * sinDelta > 0.0;
            }
            // the high band's trend over the next hours (rule 9: cirrus invading before a warm front), from the forecast rows
            if (d.mHourly.size() >= 2)
            {
                const F64 t = (F64)time(nullptr);
                for (size_t i = 0; i + 1 < d.mHourly.size(); ++i)
                {
                    const WolfEnvDoc::HourlyRow& a = d.mHourly[i]; const WolfEnvDoc::HourlyRow& b = d.mHourly[i + 1];
                    if (t >= a.t && t < b.t + 3600.0) { const size_t j = llmin(i + 3, d.mHourly.size() - 1); w.highTrend = (d.mHourly[j].high - a.high) / (F32)llmax(j - i, (size_t)1); break; }
                }
            }
            LLViewerRegion* rgn = gAgent.getRegion();
            w.land = rgn ? (rgn->getLandHeightRegion(LLViewerCamera::getInstance()->getOrigin()) > rgn->getWaterHeight()) : true;
            const WolfCloudGenus::Result target = WolfCloudGenus::map(w);
            if (!sGenusValid) { sGenus = target; sGenusValid = true; }
            else WolfCloudGenus::approach(sGenus, target, dt, GENUS_TAU);
            // [WolfViewer round 9] the EEP is the basis: the genera are arranged WITHIN the EEP's coverage (sNext.cover above,
            // cloud_shadow) and never contradict it - the three cloud layers' coverages are scaled so the largest shows the
            // EEP's, keeping their proportions; the fog / pannus slab (channel a) is not cloud cover in the EEP's sense (the
            // server carries fog as haze, WolfEnvironment.cs:1027-1031 ApplyWeather) and keeps the rule's value
            WolfCloudGenus::Result shown = sGenus;
            // [WolfViewer 2026-10-14] audit S3: the scale is taken from the weather's own genera (target), not the eased ones, so a
            // layer fading out keeps fading (scaled by the eased layers' own largest, it was put back to the full cover), and a
            // share under 1e-3 counts as no cloud (no divide by a vanishing number)
            F32 metMax = 0.f;
            for (S32 i = 0; i < 3; ++i) if (target.layer[i].height > 0.f) metMax = llmax(metMax, target.layer[i].coverage);
            if (metMax > 1e-3f)
            {
                const F32 k = sNext.cover / metMax;
                for (S32 i = 0; i < 3; ++i)
                {
                    const F32 c = shown.layer[i].coverage;
                    shown.layer[i].coverage = c > 1e-3f ? llclamp(c * k, 0.f, 1.f) : 0.f;
                }
                WolfCloudGenus::toCloudLayers(shown, sClouds->layers());
            }
            else
            {
                // [WolfViewer 2026-10-13] the weather gives no cloud genus (all three layers' coverage 0 - a clear forecast,
                // night cumulus) while the EEP has clouds: there is nothing to scale, and the empty layers hid the EEP's clouds
                // entirely (Paul: "we still have no clouds"). takram's default layers (CloudLayers.ts:31-68) carry the EEP's
                // coverage instead, as they do with no weather at all (below).
                sClouds->layers() = WolfTk::CloudLayers::defaults();
            }
        }
        else if (sClouds)
        {
            sClouds->layers() = WolfTk::CloudLayers::defaults();
            sGenusValid = false;
        }
        // [WolfViewer 2026-10-13] Paul: "cloud levels - lindens eep should control the new environment". LL's cloud density
        // (cloud_pos_density1.z, default 1.0, llsettingssky.cpp:902) is an opacity GAIN that saturates: cloudsF.glsl:92 alpha =
        // min(max(noise + cloudDensity, 0) x 10 x density1.z, 1) - its clouds are solid wherever the noise clears 0.1 / z, so any
        // density from about 0.1 up shows the same clouds with harder or softer edges, and only 0 clears them. Mapping it onto
        // the extinction made a "wispy" 0.25 four times thinner and the clouds all but vanished (Paul: "where are the clouds?").
        // So it only fades the clouds out below that knee: x min(10 density1.z, 1). The detail (cloud_pos_density2.z, default
        // 1.0, :903; LL cloudsF.glsl:91 the small noise's amplitude) scales the detail erosion, held to 0..1.5.
        if (sClouds && psky)
        {
            const F32 dens = llclamp(10.f * psky->getCloudPosDensity1().mV[2], 0.f, 1.f);
            const F32 detail = llclamp(psky->getCloudPosDensity2().mV[2], 0.f, 1.5f);
            for (WolfTk::CloudLayer& l : sClouds->layers().layer)
            {
                l.densityScale *= dens;
                l.shapeDetailAmount *= detail;
            }
        }
        // [WolfViewer round 9] the haze from the EEP sky the server bakes (Paul: the EEP is the basis). The document's haze
        // and the weather's fog are already in it - WolfSim WolfEnvironment.cs:1066-1069 ApplyDoc multiplies the air's
        // distance_multiplier by (1 + 3 haze), :1027-1031 ApplyWeather by (1 + 4 fog) - so they are read back from the sky's
        // own fields, not applied a second time: LL's haze optical depth is haze_density x density_multiplier x
        // distance_multiplier x path (windlight/atmosphericsFuncs.glsl:77-95: light_atten = (blue_density + haze_density /
        // 4) density_multiplier max_y; combined_haze = exp(-(blue_density + haze_density) x path x density_multiplier x
        // distance_multiplier)), and the EEP defaults (llsettingssky.cpp:1227-1230: haze_density 0.7, density_multiplier
        // 0.0001, distance_multiplier 0.8) are three-geospatial's default haze (CloudsMaterial.ts:536 hazeDensityScale 3e-5),
        // so each field scales the native haze by its ratio to its default. A zone's own haze at the camera (a viewer-side
        // override the server does not see) multiplies on top as before, (1 + 3 x) with the server's meaning; the document's
        // own haze is in the EEP already, so only the zones' excess over it counts. Without an EEP sky: the old rule.
        const F32 zoneExtra = llclamp(z.haze - d.mHaze, 0.f, 1.f);
        if (psky)
        {
            // [WolfViewer 2026-10-13] Paul: "there is a grey bar on the horizon". density_multiplier and distance_multiplier
            // scale LL's WHOLE atmosphere (blue_density too, atmosphericsFuncs.glsl:77-95), which Bruneton already models
            // physically; multiplying them in drove the haze up to 100 x and drew a flat grey band over the sea. Only the
            // aerosol term, haze_density over its default (llsettingssky.cpp:1227 0.7), scales the native haze, held to
            // 0.25 .. 4 x three-geospatial's clear-day haze (CloudsMaterial.ts:536 hazeDensityScale 3e-5).
            // [WolfViewer 2026-10-13, corrected] LL's own haze amount is the product (atmosphericsFuncs.glsl:77-95 combined_haze =
            // exp(-(blue_density + haze_density) x path x density_multiplier x distance_multiplier)); haze_density alone misread
            // EEPs whose multipliers are not the defaults (Tromso: 2.86 x, a grey band over the town). Held to 0.25 .. 4 x the clear
            // day, which still stops the 100 x case behind the earlier grey horizon bar.
            const F32 eepHaze = (psky->getHazeDensity() / 0.7f) * (psky->getDensityMultiplier() / 0.0001f) * (psky->getDistanceMultiplier() / 0.8f);
            sNext.hazeScale = llclamp(eepHaze, 0.25f, 4.f) * (1.f + 3.f * zoneExtra);
        }
        else
        {
            const F32 fog = (d.mMode == WolfEnvDoc::WOLF && d.mHaveWeather) ? d.mWeatherFog : 0.f;
            sNext.hazeScale = (1.f + 3.f * llclamp(z.haze, 0.f, 1.f)) * (1.f + 4.f * llclamp(fog, 0.f, 1.f));
        }
        // [WolfViewer round 9] the clouds' size from the EEP's cloud_scale (llsettingssky.cpp:904 default 0.4199): the weather
        // texture repeats 100 times at the default (CloudsEffect.ts:180-186), fewer for bigger clouds
        sNext.weatherRepeatScale = psky ? 0.4199f / llmax(psky->getCloudScale(), 0.01f) : 1.f;
        // [WolfViewer 2026-10-13] Paul: the EEPs "must be reflected in the native" sky; then at Iceland, "the colours are all wrong".
        // An EEP's colours hold its time of day already (a day cycle's dawn frame has a dawn sunlight and a dawn horizon), so
        // multiplying them onto the physical light - which colours its own dawn - coloured every dawn twice. The native sky now
        // takes the colours LL itself would light and fog the scene with, whole, and keeps only its own brightness:
        //   the direct light  LL's sun light (LLSettingsSky::getSunDiffuse: sunlight_color through LL's air, llsettingssky.cpp
        //                     :1706-1737), the moon's when the moon lights the scene (getMoonDiffuse, :1747-1756)
        //   the sky's light   LL's ambient (getSunAmbient: ambient lifted by cloud_shadow, :1733-1738)
        //   the sky           measured against the stock sky and driven to it per direction (probeSky, wolfEepGain)
        //   the clouds        cloud_color x LL's sun + ambient (LL's clouds, cloudsV.glsl:174-175) over the physical light on them
        //   haze_horizon      the air's sunlit haze amount (the Mie in-scatter, brunetonRuntime.glsl wolfEepMieScale)
        // LL's light colours are display-referred (atmosphericsFuncs.glsl:183-185 srgb_to_linear in classic mode): lin() first.
        if (psky)
        {
            const LLColor3 llSun = lin(sNext.sunUp ? psky->getSunDiffuse() : psky->getMoonDiffuse());
            const LLColor3 llAmb = lin(LLColor3(psky->getSunAmbient()));
            const LLColor3 physDirect = sNext.sunUp ? sNext.sunNormal : sNext.moonNormal;
            sNext.sunTint = hueSwap(llSun, physDirect);
            sNext.ambTint = hueSwap(llAmb, sNext.skyHorizontal);
            // the clouds' brightness too: cloud_color's luminance over LL's default grey (llsettingssky.cpp:901 0.4099) - the
            // server darkens it for rain, snow and storms (WolfAutoEnvironmentModule.cs:849-853, WolfEnvironment.cs:1017-1021)
            const F32 cloudGain = llclamp(luminance(lin(psky->getCloudColor())) / luminance(lin(LLColor3(0.4099f, 0.4099f, 0.4099f))), 0.25f, 2.f);
            // the clouds' colour is the measured per-direction gains' (measureCloudCover). [WolfViewer 2026-10-13, round 15] not a
            // hue swap against an assumed light: the clouds are lit by more than the sun + the sky at the camera (the twilight
            // glow round the horizon), and at dusk that swap multiplied their red ten-fold - 135,57,34 against the EEP's 56,53,56
            sAtmos->setEepColours(sCloudGainValid ? LLColor3::white : LLColor3::white * cloudGain,
                                  // haze_horizon over its default (llsettingssky.cpp:1229 0.19), held to 0..4 like the haze amount
                                  llclamp(psky->getHazeHorizon() / 0.19f, 0.f, 4.f));
        }
        else
        {
            sNext.sunTint = sNext.ambTint = LLColor3::white;
            sAtmos->setEepColours(LLColor3::white, 1.f);
        }
        // [WolfViewer 2026-10-13] DRIVEN BY THE EEP (Paul: "AUDIT the standard EEP -> use it to DRIVE the physical sky -> verify
        // it is correct"). The frame is put in the EEP's own linear units, so the stock display chain (pipeline.cpp, agxDisplay)
        // shows it as the EEP would: the exposure is the one that brings a white horizontal surface lit by the native sun and
        // sky to the value the stock renderer gives it (WolfEepReference::surfaceLinear, its formulas transcribed from the
        // stock shaders). The native white surface is what the native lighting computes for it - amblit + min(N.L, shadow)
        // sunlit (softenLightF.glsl legacy branch with wolf_phys; atmosphericsFuncs.glsl wolf_sunlit / wolf_amb_up =
        // sceneSunlit / sceneAmblit) - with the cloud field's MEAN sun transmittance (1 - cover, as wolfTkFarCloudT) rather than
        // the camera's own shadow, so a cloud's shadow passing over still darkens the ground instead of being metered away.
        // The stock renderer does not dim its sun under clouds at all (atmosphericsFuncs.glsl:124, audit 6.3).
        // [WolfViewer 2026-10-13] and the scene's own light is the stock EEP's, split into sun and ambient the way the stock
        // shading splits it (WolfEepReference::surfaceLights): its classic shading gives a slope turned from the sun far more
        // ambient than the physical sky light does (Paul's Iceland mountains: "looks crap", dark on the shaded side)
        sNext.llLights = psky != nullptr;
        if (psky) WolfEepReference::surfaceLights(psky, sNext.llSunlit, sNext.llAmblit);
        if (psky)
        {
            const LLColor3 direct = sNext.sunUp ? sNext.sunHorizontal : sNext.moonNormal * llmax(moon.mV[VZ], 0.f);
            const LLColor3 nativeWhite = (componentMult(direct, sNext.sunTint) * llclamp(1.f - sNext.cover, 0.f, 1.f)
                                          + componentMult(sNext.skyHorizontal, sNext.ambTint)) * (1.f / F_PI);
            const F32 target = luminance(WolfEepReference::surfaceLinear(psky));
            const F32 have = luminance(nativeWhite);
            static F32 s_logExp = 0.f; static bool s_haveExp = false;
            if (have > 0.f && target > 0.f)
            {
                const F32 want = log2f(target / have);
                // the eye's adaptation: half a second (a teleport or a sky switched by hand is a cut: at once)
                s_logExp = (!s_haveExp || s_cut || dt <= 0.f) ? want : s_logExp + (want - s_logExp) * (1.f - expf(-dt / 0.5f));
                s_haveExp = true;
                sNext.exposure = exp2f(s_logExp);
            }
        }
        sNext.valid = true;
    }

    /** The clouds' frame description (wolftkclouds.h CloudsFrame). */
    WolfTk::CloudsFrame cloudsFrame(const WolfEnvDoc& d, U32 w, U32 h, bool probe)
    {
        WolfTk::CloudsFrame f;
        f.view = glm::make_mat4(gGLModelView);
        f.proj = glm::make_mat4(gGLProjection);
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        f.near_clip = cam->getNear();
        f.far_clip = cam->getFar();
        f.width = w; f.height = h;
        f.world_to_ecef = sFrame.worldToECEF;
        // [WolfViewer round 8] the light on the clouds: the sun until it is 6 deg under (civil twilight's end, the AMS Glossary
        // definition of civil twilight), then the moon with its illuminance over the sun's above the atmosphere (Krisciunas &
        // Schaefer 1991, sFrame.moonSkyScale, the same scale the sky dome's moonlit air uses); the other body's sky irradiance
        // comes along (the twilight sky under the moon's light, the moon's sky by day) so nothing steps at the switch - the
        // sun's direct light on the clouds is already 0 there (its rays graze 35 km up at 6 deg under, Bruneton's transmittance)
        const bool moonLights = sFrame.moonLightsClouds;   // updateLight: the sun 6 deg under (sin = 0.1045) and the moon giving sky
        f.sun_dir_ecef = sFrame.cloudLightECEF;
        f.light_scale = moonLights ? sFrame.moonSkyScale : 1.f;
        f.other_dir_ecef = moonLights ? sFrame.sunECEF : sFrame.moonECEF;
        f.other_scale = moonLights ? 1.f : sFrame.moonSkyScale;
        f.camera_world = cam->getOrigin();
        const F64 t = LLFrameTimer::getElapsedSeconds();
        static F64 s_last = 0.0;
        f.dt = probe || s_last <= 0.0 ? 0.f : (F32)llclamp(t - s_last, 0.0, 0.5);
        if (!probe) s_last = t;
        f.time = t;
        f.quality = WolfEnvironmentRender::tier() >= 3 ? 2 : 1;   // decision D8: tier 2 "medium", tier 3 "high"
        f.coverage = sFrame.cover;
        // the wind moves the weather texture (CloudsEffect.ts:349-352 localWeatherOffset += velocity x dt): the
        // texture is on the globe's cube-sphere UVs (clouds.glsl:15-46), which run 0..1 across a face spanning a
        // quarter of a great circle, R pi / 2 (near the face's middle, where the camera's region is)
        // [WolfViewer 2026-10-13] Paul: "lindens eep should control the new environment" - the clouds' wind and turbulence.
        //   wind   A region with the Wolf weather keeps MET's wind: its EEP's cloud_scroll_rate is a fixed 0.05 north pace the
        //          server writes for every weather (WolfAutoEnvironmentModule.cs:866), not the wind. Any other EEP's scroll IS
        //          its clouds' motion, turned into metres per second from LL's own dome: llvowlsky.cpp:747 gives the dome
        //          uv = (1 - z) / 2 over the unit dome (one uv across 2 x dome_radius), cloudsV.glsl:85-87 divides it by
        //          cloud_scale (one cloud texture = 2 R cloud_scale metres), and llenvironment.cpp:1931 / llsettingsvo.cpp:806 add
        //          rate / 100 of a texture per second (x east, y north: WolfAutoEnvironmentModule.cs:857-861 traces the signs).
        //          [2026-10-14] now matched by angular speed at the native layer's height (below); LL's default (0.2, 0.01) at
        //          scale 0.4199 crosses the zenith at 0.0017 rad/s, 1.7 m/s for a layer 1 km up. Held to 30 m/s (a strong upper-level wind;
        //          legacy WindLight values of several units would otherwise be hundreds of m/s - the "MAD FAST" clouds).
        //   turbulence  NOT from the EEP's cloud_variance: LL's variance shifts its flat cloud texture by a share of that texture
        //          (cloudsF.glsl:76-80); taken as metres on its dome it came to 1.5 km of turbulence, 4 x takram's 350, and shredded
        //          the clouds (Paul: "where are the clouds?"). The volumetric clouds keep takram's 350 m (CloudsMaterial.ts).
        F32 windSpeed = d.mWindSpeed, windFrom = d.mWindFrom;
        if (LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky())
        {
            const F32 scale = llmax(psky->getCloudScale(), 0.01f);
            if (!d.mHaveWeather)
            {
                // [WolfViewer 2026-10-14] Paul at Lampert Highlands: "the clouds are mutating too fast ... a whole series of cloud
                // patterns". The speed above treated LL's dome radius (15 km) as the clouds' height, but the native clouds are a few
                // km up or less, and what the eye sees is how fast they cross the sky: the same m/s sweeps a 1 km layer 15 x faster
                // than a 15 km one. Now the native clouds cross the zenith at the stock layer's own angular speed:
                //   llvowlsky.cpp:752 uv = (1 - z0) / 2 with z0 = sin(phi) sin(theta), so at the zenith uv changes 1/2 per radian;
                //   cloudsV.glsl:85-87 divides it by cloud_scale (2 scale radians per texture); llenvironment.cpp:1931 and
                //   cloudsF.glsl:80 add rate / 100 texture per second. Zenith angular speed = 2 scale |rate| / 100 rad/s.
                // A layer at height h shows that at v = omega h, h = the middle of the layer that carries the most cloud (the lowest
                // on a tie: low clouds are the ones the eye follows). The wind also drives the shapes' change (wolftkclouds.cpp
                // cloudEvolution), so this slows both.
                F32 layerMid = 0.f, bestWeight = -1.f;
                for (const WolfTk::CloudLayer& l : sClouds->layers().layer)
                {
                    if (l.height <= 0.f) continue;
                    const F32 w = l.coverage < 0.f ? 1.f : l.coverage;
                    const F32 mid = l.altitude + 0.5f * l.height;
                    if (w > bestWeight + 1e-4f || (fabsf(w - bestWeight) <= 1e-4f && mid < layerMid)) { bestWeight = w; layerMid = mid; }
                }
                if (layerMid <= 0.f) layerMid = 2000.f;
                const LLVector2 rate = psky->getCloudScrollRate();
                const F32 radPerSecPerRate = 2.f * scale / 100.f;
                const F32 east = rate.mV[VX] * radPerSecPerRate * layerMid, north = rate.mV[VY] * radPerSecPerRate * layerMid;
                windSpeed = llmin(sqrtf(east * east + north * north), 30.f);
                windFrom = windSpeed > 0.f ? fmodf(atan2f(east, north) * RAD_TO_DEG + 180.f + 360.f, 360.f) : d.mWindFrom;
            }
        }
        const F32 toward = (windFrom + 180.f) * DEG_TO_RAD;
        const F32 uvPerMetre = (F32)(1.0 / (sFrame.bottomRadius * F_PI * 0.5));
        f.haze_density_scale = 3.0e-5f * sFrame.hazeScale;   // Source: CloudsMaterial.ts:536 hazeDensityScale 3e-5
        // [WolfViewer 2026-10-13] THE EEP DRIVES THE HAZE. The stock haze pass dims a surface d metres away by
        // exp(-(blue_density.r + haze_density) d density_multiplier distance_multiplier) ([S] atmosphericsFuncs.glsl:79-87,
        // hazeF.glsl:79-83 atten.r), the same density everywhere around the camera up to max_y above it (:57 the ray cut at
        // |y| = max_y). The native haze (cloudsF.glsl approximateHaze) is density x exp(-height x exponent); so its density AT
        // THE CAMERA is set to that extinction and its scale height to max_y (the same vertical optical depth), with no cloud
        // cover gate. The EEP's own value replaces the physical-haze rule above whenever an EEP drives the sky.
        if (WolfEnvironmentRender::eepDriven())
        {
            if (LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky())
            {
                const F32 sigma = llmax(psky->getBlueDensity().mV[0] + psky->getHazeDensity(), 0.f)
                                * psky->getDensityMultiplier() * psky->getDistanceMultiplier();
                const F32 maxY = llmax(psky->getMaxY(), 1.f);
                f.haze_exponent = 1.f / maxY;
                f.haze_density_scale = sigma * expf(llmin(sFrame.cameraHeight / maxY, 20.f));
                f.haze_always = sigma > 0.f;
                // [WolfViewer 2026-10-13] and the haze itself is the stock haze pass's (cloudsF.glsl wolfLLHaze): Paul, "Jimmy's
                // EEP has like a haze, ours is very bright" - takram's haze only took the EEP's density, its light was physical
                const WolfEepReference::HazeParams hp = WolfEepReference::hazeParams(psky);
                f.ll_haze = true;
                f.ll_blue_w = hp.blueW; f.ll_haze_w = hp.hazeW; f.ll_cs = hp.cs; f.ll_tmp_amb = hp.tmpAmb; f.ll_combined = hp.combined;
                f.ll_glow_x = hp.glowX; f.ll_glow_z = hp.glowZ; f.ll_smgf = hp.smgf;
                f.ll_density_distance = hp.densityDistance; f.ll_max_y = hp.maxY; f.ll_hdr = hp.skyHdrScale;
                // world -> ECEF as a rotation only (the light norm is not unit length in the stock code and must stay so)
                auto rot = [&](const LLVector3& v) {
                    const glm::dvec4 e = sFrame.worldToECEF * glm::dvec4(v.mV[0], v.mV[1], v.mV[2], 0.0);
                    return LLVector3((F32)e.x, (F32)e.y, (F32)e.z); };
                f.ll_lightnorm_ecef = rot(hp.lightnormWorld);
                f.ll_light_dir_ecef = rot(hp.lightDirWorld);
            }
        }
        // [WolfViewer 2026-10-13] the EEP's cloud_scale, HELD. Source: cloudsCommon.glsl:127 sampleWeather samples
        // wuv * localWeatherRepeat + offset, so the repeat scales the whole weather field about globe uv 0, ~0.5 face
        // (5,000 km) from the camera: a day cycle easing cloud_scale moved the clouds overhead by uv x d(repeat)/repeat.
        // Paul's trace (10-13): repeat 0.5437 -> 0.5435 over 3 s = 1.3e-4 /s x 0.5 x 1e7 m = ~650 m/s ("clouds flying
        // across the screen"), six times that on a region with a 4-hour day (Tropical Illusions). The repeat is taken
        // when the sky starts and kept; only a change of more than 2x (a different sky preset) is followed, as one cut.
        {
            static F32 s_repeat = 0.f;
            const F32 want = sFrame.weatherRepeatScale;
            if (s_repeat <= 0.f || want > 2.f * s_repeat || want < 0.5f * s_repeat) s_repeat = want;
            f.weather_repeat_scale = s_repeat;
        }
        f.weather_rotation_deg = sGenusValid ? sGenus.rotationDeg : 0.f;   // [WolfViewer step 3] cloud streets / cirrus along the wind
        f.weather_stretch = sGenusValid ? sGenus.stretch : 1.f;
        for (S32 i = 0; i < 4; ++i) f.weather_layer_stretched[i] = sGenusValid ? sGenus.layerStretch[i] : 0.f;   // [2026-10-14] only those layers
        // [WolfViewer 2026-10-14] convective layers get a top per column (cloudsCommon.glsl wolfLayerTower; audit B2): the genus's
        // own value (WolfCloudGenus::towerShare), or a document's cloud style "towering" on takram's low layer, as Cu mediocris
        for (S32 i = 0; i < 4; ++i) f.weather_layer_tower[i] = sGenusValid ? WolfCloudGenus::towerShare(sGenus.layer[i]) : 0.f;
        if (d.mCloudStyle == "towering" && f.weather_layer_tower[0] <= 0.f)
            f.weather_layer_tower[0] = WolfCloudGenus::towerShareOf("Cu_mediocris");
        // [WolfViewer round 13] the offset is added AFTER the repeat and the turn (cloudsCommon.glsl sampleWeather:
        // textureLod(localWeatherTexture, (S R (uv - pivot) + pivot) x repeat + offset)), so a wind of w globe-uv per second
        // moves the clouds at w only as offset velocity = repeat x S R x w - before, w alone: the clouds drifted at 1/100 of
        // the wind, and only the turn about the face centre moved them (fast).
        {
            const F32 wx = sinf(toward) * windSpeed * uvPerMetre, wy = cosf(toward) * windSpeed * uvPerMetre;
            const F32 a = f.weather_rotation_deg * DEG_TO_RAD, ca = cosf(a), sa = sinf(a), inv = 1.f / llmax(f.weather_stretch, 1e-3f);
            const F32 rep = 100.f * f.weather_repeat_scale;   // wolftkclouds.cpp localWeatherRepeat 100 (CloudsEffect.ts:180-186)
            // a feature at texture coordinate t sits where repeat x M (uv - pivot) + offset = t: offset growing by delta moves
            // it by -M^-1 delta / repeat in uv, so the clouds go WITH the wind for delta = -repeat M w dt
            f.local_weather_velocity.set(-rep * inv * (ca * wx - sa * wy), -rep * (sa * wx + ca * wy));
            // [WolfViewer 2026-10-14] the unstretched layers' lookup (R without S, cloudsCommon.glsl sampleWeather) has its own
            // offset, -repeat R w dt, so a change of the stretch never moves them
            f.local_weather_velocity_turned.set(-rep * (ca * wx - sa * wy), -rep * (sa * wx + ca * wy));
            f.wind_speed = windSpeed;   // [WolfViewer round 17] the shapes' drift
        }
        // [WolfViewer 2026-10-13] WolfEnvironmentTrace: every input that moves the clouds, four times a second, so a report
        // of "clouds moving very fast" can be traced to the value that changes (Paul's video 09:53: clouds re-form in place
        // within half a second). Source of each value: this function (velocity, rotation, stretch, repeat, pivot), updateLight
        // (sFrame.cover, wolfenvironmentrender.cpp:613-619), WolfCloudGenus layers (sGenus).
        {
            static LLCachedControl<bool> trace(gSavedSettings, "WolfEnvironmentTrace", false);
            static F64 s_nextTrace = 0.0;
            if (trace && !probe && t >= s_nextTrace)
            {
                s_nextTrace = t + 0.25;
                std::ostringstream o;
                o << "t " << t << " dt " << f.dt << " cover " << sFrame.cover << " wind " << windSpeed << " from " << windFrom << " turb " << f.turbulence_displacement << " dens0 " << (sClouds ? sClouds->layers().layer[0].densityScale : 0.f)
                  << " vel " << f.local_weather_velocity.mV[0] << "," << f.local_weather_velocity.mV[1]
                  << " rot " << f.weather_rotation_deg << " stretch " << f.weather_stretch
                  << " lstretch " << f.weather_layer_stretched[0] << "," << f.weather_layer_stretched[1] << "," << f.weather_layer_stretched[2] << "," << f.weather_layer_stretched[3]
                  << " repeat " << f.weather_repeat_scale
                  << " pivot " << f.weather_pivot.mV[0] << "," << f.weather_pivot.mV[1] << " genus " << (sGenusValid ? 1 : 0);
                for (S32 i = 0; i < 4; ++i)
                    o << " L" << i << " " << sGenus.layer[i].genus << " alt " << sGenus.layer[i].altitude << " h " << sGenus.layer[i].height
                      << " cov " << sGenus.layer[i].coverage;
                LL_INFOS("WolfEnvTrace") << o.str() << LL_ENDL;
            }
        }
        // [WolfViewer round 13] the pivot of the turn and stretch: the camera's globe uv, kept while the camera stays within
        // 50 km of it (a wind turn of 10 deg an hour moves clouds 50 km away at 0.15 m/s), re-anchored past that (one jump of
        // the field: rare, and a teleport is a cut anyway)
        {
            static LLVector3d s_pivotECEF; static bool s_have = false;
            const LLVector3d camE = sFrame.cameraECEF;
            if (!s_have || (camE - s_pivotECEF).magVec() > 50000.0) { s_pivotECEF = camE; s_have = true; }
            f.weather_pivot = cubeSphereUv(s_pivotECEF);
        }
        f.sky_under_cloud.set(sFrame.skyUnderCloud.mV);       // the haze's sky light under the clouds (updateLight)
        f.depth_tex = gPipeline.mRT->deferredScreen.getDepth();
        f.pre_exposure = sFrame.exposure;
        f.reset_history = gSnapshot;
        return f;
    }

    void captureTexture(const std::string& name, U32 tex, U32 w, U32 h, bool alpha_png, GLenum target = GL_TEXTURE_2D, S32 layer = 0)
    {
        if (!tex || !w || !h || w > 8192 || h > 8192) return;
        std::vector<F32> px;
        if (target == GL_TEXTURE_2D)
        {
            px.assign((size_t)w * h * 4, 0.f);
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, tex);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, px.data());
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        }
        else
        {
            // a 3D texture or an array: read it whole, keep `layer`
            GLint depth = 1;
            gGL.getTexUnit(0)->activate();
            glBindTexture(target, tex);
            glGetTexLevelParameteriv(target, 0, GL_TEXTURE_DEPTH, &depth);
            depth = llmax(depth, 1);
            std::vector<F32> all((size_t)w * h * depth * 4, 0.f);
            glGetTexImage(target, 0, GL_RGBA, GL_FLOAT, all.data());
            glBindTexture(target, 0);
            const S32 l = llclamp(layer, 0, depth - 1);
            px.assign(all.begin() + (size_t)l * w * h * 4, all.begin() + (size_t)(l + 1) * w * h * 4);
        }
        std::string line = llformat("%s %ux%u:", name.c_str(), w, h);
        for (S32 c = 0; c < 4; ++c)
        {
            F64 mn = 1e30, mx = -1e30, sum = 0.0;
            size_t nan = 0, inf = 0, fin = 0;
            for (size_t i = c; i < px.size(); i += 4)
            {
                const F32 v = px[i];
                if (std::isnan(v)) { ++nan; continue; }
                if (std::isinf(v)) { ++inf; continue; }
                mn = llmin(mn, (F64)v); mx = llmax(mx, (F64)v); sum += v; ++fin;
            }
            line += llformat(" %c[min %.4g max %.4g mean %.4g nan %zu inf %zu]", "rgba"[c], fin ? mn : 0.0, fin ? mx : 0.0,
                             fin ? sum / fin : 0.0, nan, inf);
        }
        LL_INFOS("WolfEnvDebug") << line << LL_ENDL;
        auto write = [&](const std::string& file, bool alpha)
        {
            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)w, (U16)h, 3);
            {
                LLImageDataLock lock(raw);
                U8* out = raw->getData();
                if (!out) return;
                for (size_t i = 0; i < (size_t)w * h; ++i)
                {
                    for (S32 c = 0; c < 3; ++c)
                    {
                        F32 v = px[i * 4 + (alpha ? 3 : c)];
                        if (!std::isfinite(v)) v = std::isnan(v) ? 0.f : 50.f;
                        v = llmax(v, 0.f);
                        out[i * 3 + c] = (U8)llclamp(powf(v / (1.f + v), 1.f / 2.2f) * 255.f + 0.5f, 0.f, 255.f);
                    }
                }
            }
            LLPointer<LLImagePNG> png = new LLImagePNG();
            if (png->encode(raw, 0.f)) png->save(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, file));
        };
        write("wolfenv_" + name + ".png", false);
        if (alpha_png) write("wolfenv_" + name + "_alpha.png", true);
    }

    void captureTarget(const std::string& name, LLRenderTarget& rt, bool alpha_png)
    {
        if (!rt.isComplete()) { LL_INFOS("WolfEnvDebug") << name << ": not allocated" << LL_ENDL; return; }
        for (U32 i = 0; i < rt.getNumTextures(); ++i)
        {
            captureTexture(i ? llformat("%s_%u", name.c_str(), i) : name, rt.getTexture(i), rt.getWidth(), rt.getHeight(), alpha_png);
        }
    }

    void logTimings()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - sGpu.lastLog < 60.0) return;
        sGpu.lastLog = now;
        std::string line = llformat("tier %d, sky %s, meter %.4g lux, EV100 %.2f (target %.2f), exposure %.4g, cover %.2f |",
                                    WolfEnvironmentRender::tier(), WolfEnvironmentRender::skyActive() ? "on" : "off",
                                    sFrame.meterLux, sMeter.ev100(), sMeter.targetEv100(), sFrame.exposure, sFrame.cover);
        for (S32 s = 0; s < 4; ++s)
        {
            line += llformat(" %s %.2f ms", SECTION_NAMES[s], sGpu.msFrames[s] ? sGpu.ms[s] / sGpu.msFrames[s] : 0.0);
            sGpu.ms[s] = 0.0;
            sGpu.msFrames[s] = 0;
        }
        if (!timersAvailable()) line += " (no GPU timer queries on this driver)";
        // [WolfViewer round 15] the frame's bodies and readings, so a wrong exposure can be traced from the log alone: the
        // native sun / moon elevation, the EEP sky's, the reflected reading and the sky's share of it, the scene's light
        {
            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
            const F32 eepSun = psky ? asinf(llclamp(psky->getSunDirection().mV[VZ], -1.f, 1.f)) * RAD_TO_DEG : -999.f;
            const LLColor3 sl = WolfEnvironmentRender::sceneSunlit(), am = WolfEnvironmentRender::sceneAmblit();
            line += llformat(" | sun %.1f deg (EEP %.1f), moon %.1f deg, frame log-avg %.4g cd/m2, sky share %.2f, sunlit %.3g amblit %.3g (pre-exposed)",
                             asinf(llclamp(sFrame.sunWorld.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG, eepSun, asinf(llclamp(sFrame.moonWorld.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG,
                             sFrame.frameLogAvg, sFrame.skyLuminanceShare, luminance(sl), luminance(am));
        }
        LL_INFOS("WolfEnvGPU") << line << LL_ENDL;
    }

    /** The camera's world position as the matrices in use place it: the translation of the inverse scene modelview
     *  (three-geospatial's cameraPosition is camera.getWorldPosition, AtmosphereMaterialBase.ts:174-176 - the same
     *  point). From the matrices rather than LLViewerCamera, so a reflection-probe face or a material preview, which
     *  draw with their own modelview, get their own camera. */
    LLVector3 cameraWorldFromView()
    {
        const glm::mat4 iview = glm::inverse(glm::make_mat4(gGLModelView));
        return LLVector3(iview[3].x, iview[3].y, iview[3].z);
    }

    /** The camera and frame uniforms every translated program shares (the takram names). `own`: one of the native
     *  sky's own programs, whose noise clock is takram's `time`; a stock program with the atmosphereLibF unit has a
     *  `time` of its own (waterF.glsl's waves) and reads the clock as wolfTkTime. */
    void setFrameUniforms(LLGLSLShader& sh, bool own)
    {
        static LLStaticHashedString s_cp("cameraPosition"), s_w2e("worldToECEFMatrix"), s_e2w("ecefToWorldMatrix"), s_ac("altitudeCorrection");
        static LLStaticHashedString s_sd("sunDirection"), s_csr("cosSunAngularRadius"), s_br("bottomRadius"), s_time("time");
        const LLVector3 cam = cameraWorldFromView();
        sh.uniform3f(s_cp, cam.mV[0], cam.mV[1], cam.mV[2]);
        const glm::mat4 w2e(sFrame.worldToECEF), e2w(sFrame.ecefToWorld);
        sh.uniformMatrix4fv(s_w2e, 1, GL_FALSE, glm::value_ptr(w2e));
        sh.uniformMatrix4fv(s_e2w, 1, GL_FALSE, glm::value_ptr(e2w));
        sh.uniform3f(s_ac, 0.f, 0.f, 0.f);
        sh.uniform3fv(s_sd, 1, sFrame.sunECEF.mV);
        // Source: AtmosphereMaterialBase.ts:277-280 cosSunAngularRadius = cos(sun_angular_radius)
        sh.uniform1f(s_csr, cosf(sAtmos->sunAngularRadius()));
        sh.uniform1f(s_br, (F32)sFrame.bottomRadius);   // metres, as uniforms.ts:209 / AerialPerspectiveEffect.ts:250
        // seconds for the interleaved-gradient-noise seed (takram generators.ts:20-25), kept small for precision
        const F32 clock = (F32)fmod(LLFrameTimer::getElapsedSeconds(), 1000.0);
        static LLStaticHashedString s_wtime("wolfTkTime");   // atmosphereLibF.glsl
        if (own) sh.uniform1f(s_time, clock);
        sh.uniform1f(s_wtime, clock);
    }

    /** A 1 x 1 zero table for a sampler with nothing to read (the light shafts' length without clouds: 0 km). */
    GLuint sZeroTex = 0;
    GLuint zeroTex()
    {
        if (sZeroTex) return sZeroTex;
        const F32 zero[4] = { 0.f, 0.f, 0.f, 0.f };
        glGenTextures(1, &sZeroTex);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sZeroTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 1, 1, 0, GL_RGBA, GL_FLOAT, zero);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return sZeroTex;
    }

    bool allocate(LLRenderTarget& rt, U32 w, U32 h, U32 fmt = GL_RGBA16F)
    {
        if (rt.isComplete() && rt.getWidth() == w && rt.getHeight() == h) return true;
        rt.release();
        return rt.allocate(w, h, fmt, false);
    }

    // [WolfViewer 2026-10-14] Paul: "add shooting stars back and meteors because they were there some time ago just one or
    // two in the night sky occasionally" (WolfStorm drew them until January 2026), then: "one or two every 6 hours rarely
    // appearing". How often is Paul's - one every one to six hours of night, not a real sporadic rate - the rest is what a
    // meteor looks like: a streak a few to twenty degrees long
    // lasting under a second, now and then a brighter, slower fireball. Only at night (the sun 10 degrees or more below
    // the horizon), and only well up the sky. skyF.glsl wolfMeteors draws them.
    struct Meteor
    {
        LLVector3 start, axis;   // world (x east, y north, z up): where it starts, the axis it turns about
        F32 arc = 0.f;           // the path's length, radians
        F64 born = 0.0;
        F32 life = 0.f;          // seconds
        F32 peak = 0.f;          // the head's brightness, the frame's linear units
        F32 tail = 0.35f;        // the tail's share of the path
    };
    Meteor sMeteors[2];
    F64 sNextMeteor = 0.0;

    void updateMeteors(F64 now, const LLVector3& sunWorld)
    {
        const bool night = sunWorld.mV[VZ] < -sinf(10.f * DEG_TO_RAD);
        for (Meteor& m : sMeteors) if (m.peak > 0.f && now - m.born >= m.life) m.peak = 0.f;
        if (!night) { sNextMeteor = 0.0; return; }
        if (sNextMeteor <= 0.0) sNextMeteor = now + 3600.0 + ll_frand(18000.f);   // dark (or a teleport): one to six hours on
        if (now < sNextMeteor) return;
        sNextMeteor = now + 3600.0 + ll_frand(18000.f);
        Meteor* slot = nullptr;
        for (Meteor& m : sMeteors) if (m.peak <= 0.f) { slot = &m; break; }
        if (!slot) return;
        Meteor m;
        const F32 el = (25.f + ll_frand(50.f)) * DEG_TO_RAD, az = ll_frand(F_TWO_PI);
        m.start.set(cosf(el) * sinf(az), cosf(el) * cosf(az), sinf(el));
        // a direction along the sky at the start: its two tangents (along the azimuth, and start x that one, along the
        // elevation) mixed at a random angle
        LLVector3 east(cosf(az), -sinf(az), 0.f);
        LLVector3 alongEl = m.start % east;
        alongEl.normalize();
        const F32 turn = ll_frand(F_TWO_PI);
        LLVector3 travel = east * cosf(turn) + alongEl * sinf(turn);
        const bool fireball = ll_frand() < 0.125f;
        m.arc = (fireball ? 25.f + ll_frand(15.f) : 6.f + ll_frand(14.f)) * DEG_TO_RAD;
        m.life = fireball ? 1.5f + ll_frand(1.f) : 0.4f + ll_frand(0.6f);
        m.peak = fireball ? 2.5f : 0.7f + ll_frand(0.7f);
        m.tail = fireball ? 0.5f : 0.35f;
        // keep the whole path 10 degrees or more above the horizon: the other way if it would end lower
        const LLVector3 endA = m.start * cosf(m.arc) + travel * sinf(m.arc);
        if (endA.mV[VZ] < sinf(10.f * DEG_TO_RAD)) travel = -travel;
        m.axis = m.start % travel;   // turning start about it moves it toward travel (skyF.glsl wolfMeteors)
        m.axis.normalize();
        m.born = now;
        *slot = m;
    }
}

// static
S32 WolfEnvironmentRender::sScenePreview = 0;

// static
S32 WolfEnvironmentRender::tier()
{
    if (sGpu.tierFrame == (S32)LLFrameTimer::getFrameCount()) return sGpu.tierValue;
    sGpu.tierFrame = (S32)LLFrameTimer::getFrameCount();
    sGpu.tierValue = tierNow();
    // the frame's state, latched with the tier: everything this frame draws reads the same light and the same
    // answer to skyActive() (frameUpdate, mid-frame, prepares the next one in sNext)
    sFrame = sNext;
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    sGpu.active = sGpu.tierValue >= 1 && d.mValid && d.mMode == WolfEnvDoc::WOLF && sharedSky() && sAtmos && sAtmos->ready() && sFrame.valid;
    return sGpu.tierValue;
}

// static
S32 WolfEnvironmentRender::tierNow()
{
    static LLCachedControl<S32> quality(gSavedSettings, "WolfEnvironmentQuality", -1);
    static LLCachedControl<U32> level(gSavedSettings, "RenderQualityPerformance", 1);
    // [2026-10-12] Paul: not rolled out "until i cry because its so beautiful": an opt-in test mode. Off,
    // every native path is inert (tier 0: skyActive() / volumetricActive() false, wolf_phys = 0 in every
    // shader) and the EEP draws exactly as before.
    static LLCachedControl<bool> native_sky(gSavedSettings, "WolfEnvironmentNativeSky", false);
    if (!native_sky) return 0;
    if (sGpu.failed || !WolfGrid::isOnWolfTerritories()) return 0;
    if (gGLManager.mGLVersion < 3.29f) return 0;
    if (!programsReady()) return 0;
    // the native sky's pipeline (HDR and its targets) exists only with mRT->deferredLight
    if (!gPipeline.mRT || !gPipeline.mRT->deferredLight.isComplete()) return 0;
    // the frame is pre-exposed HDR end to end: the tone mapping it needs runs only with HDR on
    // (LLPipeline::renderFinalize: hdr = GL > 4.05 && RenderHDREnabled)
    static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
    if (!(gGLManager.mGLVersion > 4.05f && has_hdr())) return 0;
    // -1: follow the graphics level as the feature tables do (llfeaturemanager.cpp:192-198 Low, LowMid, Mid,
    // MidHigh, High, HighUltra, Ultra = 0..6 -> 0, 1, 1, 2, 2, 3, 3)
    static const S32 BY_LEVEL[7] = { 0, 1, 1, 2, 2, 3, 3 };
    return (S32)quality < 0 ? BY_LEVEL[llmin((U32)level, 6u)] : llclamp((S32)quality, 0, 3);
}

// static
bool WolfEnvironmentRender::skyActive()
{
    if (tier() < 1) return false;   // tier() latches the frame and works sGpu.active out with it
    return sGpu.active;
}

// static
bool WolfEnvironmentRender::volumetricActive()
{
    // the physical sky always replaces the EEP 2D clouds (they are in EEP's units); at tier 1 there are none
    return skyActive();
}

// static
void WolfEnvironmentRender::verifyFrame(LLRenderTarget* displayed, LLRenderTarget* depthSource)
{
    static LLCachedControl<bool> trace(gSavedSettings, "WolfEnvironmentTrace", false);
    if (!trace || gCubeSnapshot || !displayed || !depthSource) return;
    static F64 s_next = 0.0;
    const F64 t = LLFrameTimer::getElapsedSeconds();
    if (t < s_next) return;
    s_next = t + 15.0;   // a full-frame read waits for the GPU: trace only, and seldom
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    if (!psky) return;
    const S32 W = (S32)displayed->getWidth(), H = (S32)displayed->getHeight();
    if (W <= 0 || H <= 0 || (S32)depthSource->getWidth() != W || (S32)depthSource->getHeight() != H || !depthSource->getDepth()) return;
    // only the 16 x 9 sample pixels are read (single-pixel reads from the two framebuffers), not the whole frame: a full read
    // stalled the frame visibly every time (Paul: "flashing and jerking")
    if (!displayed->getFBO() || !depthSource->getFBO()) return;
    GLint prevRead = 0, prevBuf = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_READ_BUFFER, &prevBuf);
    F32 sampCol[16 * 9][4] = {}, sampDep[16 * 9] = {};
    for (S32 gy = 0; gy < 9; ++gy)
    {
        for (S32 gx = 0; gx < 16; ++gx)
        {
            const S32 x = (S32)((gx + 0.5f) / 16.f * W), y = (S32)((gy + 0.5f) / 9.f * H);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, displayed->getFBO());
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, sampCol[gy * 16 + gx]);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, depthSource->getFBO());
            glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &sampDep[gy * 16 + gx]);
        }
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glReadBuffer(prevBuf);

    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 at = cam->getAtAxis(), left = cam->getLeftAxis(), up = cam->getUpAxis();
    const F32 ty = tanf(cam->getView() * 0.5f), tx = ty * cam->getAspect();
    LLVector3 sun = LLEnvironment::instance().getSunDirection();
    const F32 sunAz = atan2f(sun.mV[VX], sun.mV[VY]);
    const WolfEepReference::Path path = WolfEepReference::path(psky);
    auto d8 = [](const LLColor3& c) { return llformat("%3d,%3d,%3d", (S32)llround(c.mV[0] * 255.f), (S32)llround(c.mV[1] * 255.f), (S32)llround(c.mV[2] * 255.f)); };
    std::ostringstream o;
    o << "SCREEN verify (" << (skyActive() ? "native sky" : "STOCK EEP renderer") << "; the displayed frame's sky pixels, 0-255, against the EEP reference):";
    S32 rows = 0;
    for (S32 gy = 8; gy >= 0 && rows < 40; --gy)
    {
        for (S32 gx = 0; gx < 16 && rows < 40; ++gx)
        {
            const S32 x = (S32)((gx + 0.5f) / 16.f * W), y = (S32)((gy + 0.5f) / 9.f * H);
            const S32 i = gy * 16 + gx;
            if (sampDep[i] < 0.99999f) continue;   // something in front of the sky
            const F32 ndcX = 2.f * (x + 0.5f) / W - 1.f, ndcY = 2.f * (y + 0.5f) / H - 1.f;
            LLVector3 dir = at + up * (ty * ndcY) - left * (tx * ndcX);
            dir.normalize();
            if (dir.mV[VZ] < 0.f) continue;
            const LLColor3 shown(sampCol[i][0], sampCol[i][1], sampCol[i][2]);
            const F32 el = asinf(llclamp(dir.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
            F32 daz = fabsf(atan2f(dir.mV[VX], dir.mV[VY]) - sunAz) * RAD_TO_DEG;
            if (daz > 180.f) daz = 360.f - daz;
            LLColor3 cloud;
            const bool haveCloud = WolfEepReference::cloudColourLinear(psky, dir, cloud);
            o << "\n  " << llformat("%4.1f deg up, %5.1f deg from the sun", el, daz) << "  screen " << d8(shown)
              << "  EEP sky " << d8(WolfEepReference::display(path, WolfEepReference::skyLinear(psky, dir)))
              << (haveCloud ? "  EEP cloud " + d8(WolfEepReference::display(path, cloud)) : std::string("  (no EEP cloud)"));
            ++rows;
        }
    }
    if (rows == 0) o << " no sky in view";
    LL_INFOS("WolfEnvTrace") << o.str() << LL_ENDL;
}

// static
bool WolfEnvironmentRender::eepDriven() { return skyActive() && LLEnvironment::instance().getCurrentSky() != nullptr; }

// static
bool WolfEnvironmentRender::agxDisplay() { return skyActive() && !eepDriven(); }

// static
F32 WolfEnvironmentRender::exposure() { return sFrame.exposure; }

// static
LLColor3 WolfEnvironmentRender::whitePoint() { return sAtmos ? sAtmos->whitePoint() : LLColor3::white; }

// static
LLColor3 WolfEnvironmentRender::sceneSunlit()
{
    // the direct light in full (the clouds' shadow takes it away per pixel, wolfTkCloudShadow); Lambert: L =
    // albedo / pi x E (deferredUtil.glsl multiplies by pi under wolf_phys, pbrPunctual carries the 1 / pi)
    if (sFrame.llLights) return sFrame.llSunlit;   // [WolfViewer 2026-10-13] the stock EEP's sun, per unit N.L (updateLight)
    const LLColor3 E = sFrame.sunUp ? sFrame.sunNormal : sFrame.moonNormal;
    return componentMult(E, sFrame.sunTint) * (sFrame.exposure / F_PI);   // the EEP's sunlight_color hue (updateLight)
}

// static
LLColor3 WolfEnvironmentRender::sceneAmblit()
{
    // Source: SkyLightProbe.ts:85-129: the irradiance at the camera, as the SH of (1 + n.up) / 2 x it
    // (atmosphericsFuncs.glsl mixes 0 -> this by n.up x 0.5 + 0.5: the same function)
    if (sFrame.llLights) return sFrame.llAmblit;   // [WolfViewer 2026-10-13] the stock EEP's ambient (updateLight)
    return componentMult(sFrame.skyHorizontal, sFrame.ambTint) * (sFrame.exposure / F_PI);   // the EEP's ambient hue (updateLight)
}

// static
LLColor3 WolfEnvironmentRender::sceneAmbDown() { return LLColor3::black; }

// static
LLColor3 WolfEnvironmentRender::sceneLightH()
{
    const LLColor3 direct = sFrame.sunUp ? sFrame.sunHorizontal : sFrame.moonNormal * llmax(sFrame.moonWorld.mV[VZ], 0.f);
    return (componentMult(direct, sFrame.sunTint) * sFrame.cloudTransmittanceAtCamera + componentMult(sFrame.skyHorizontal, sFrame.ambTint))
           * (sFrame.exposure / F_PI);
}

// static
LLColor3 WolfEnvironmentRender::physicalLightColor(const LLColor3& c, bool spot)
{
    // Filament "Physically based light units": point I = phi / (4 pi), spot (absorber) I = phi / pi
    // [WolfViewer 2026-10-13] LL lights are set by builders for LL's look (intensity, radius, falloff), not as photometric
    // bulbs: as 800 lm x the physical exposure they were ~10^4 x a starlit night and burned every surface white (Paul,
    // Sandbox at night). They are display-referred like full-bright: LL's own colour and LL's own falloff (the shaders'
    // legacy attenuation), added to the pre-exposed frame at the brightness LL gives them, day and night.
    (void)spot;
    return c;
}

// static
bool WolfEnvironmentRender::bodyDirections(LLVector3& sun, LLVector3& moon)
{
    if (!sFrame.valid) return false;
    sun = sFrame.sunWorld; moon = sFrame.moonWorld;
    return true;
}

// static
F32 WolfEnvironmentRender::starBrightnessScale()
{
    // [WolfViewer 2026-10-13] the EEP's star_brightness over LL's default (llsettingssky.cpp:918 250): a default EEP keeps the
    // physical stars as they are, 0 hides them (as LL's renderStarsDeferred returns under 0.001), 500 doubles them; held to 4 x.
    // Above about 18 km the stars come out whatever the EEP says (WolfAltitudeSky::spaceStarAlpha, as LL's star pass takes it).
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    const F32 eep = psky ? psky->getStarBrightness() / 250.f : 1.f;
    return llclamp(llmax(eep, WolfAltitudeSky::spaceStarAlpha()), 0.f, 4.f);
}

// static
LLColor3 WolfEnvironmentRender::starTint()
{
    if (!skyActive()) return LLColor3::white;
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    LLColor3 c = LLColor3::white;
    if (d.mHasStarColor)
    {
        c = lin(d.mStarColor);
        const F32 top = llmax(c.mV[0], llmax(c.mV[1], c.mV[2]));
        c = top > 0.f ? c * (1.f / top) : LLColor3::white;
    }
    // the catalogue stars' vertex brightness (llvowlsky.cpp updateRealStars) is relative to a first-magnitude
    // star; one such star gives E = 10^(0.4 (-m - 19)) W/m2 (Jensen 2001 eq. 6) x 683 lm/W over its sprite of
    // ~2 sprite radii^2 / dome radius^2 sr (sc = 10 + 22 intensity m at the dome radius, llvowlsky.cpp:395): its
    // luminance x pre-exposure, seen through the atmosphere at the zenith (the sprites carry no per-star
    // extinction) - the transmittance from three-geospatial's table
    const F32 E1 = powf(10.f, 0.4f * (-1.f - 19.f)) * 683.f;
    const F32 R = LLEnvironment::instance().getCurrentSky() ? LLEnvironment::instance().getCurrentSky()->getDomeRadius() : 15000.f;
    const F32 omega = (32.f * 32.f) / (R * R);
    const LLColor3 tz = ratio(sAtmos->sunIlluminance(cameraECEFKm(sFrame), toECEF(sFrame, LLVector3::z_axis)), sAtmos->sunIlluminanceAboveAtmosphere());
    return componentMult(c, tz) * (E1 / omega * sFrame.exposure * starBrightnessScale());
}

// static
void WolfEnvironmentRender::frameUpdate()
{
    if (gCubeSnapshot) return;
    // once a frame (LLDrawPoolWLSky::renderDeferred calls it; a second call in one frame would step the table
    // build and the meter twice)
    if (sGpu.updateFrame == LLFrameTimer::getFrameCount()) return;
    sGpu.updateFrame = LLFrameTimer::getFrameCount();
    if (tier() > 0) WolfEnvironmentState::instance().idle();   // nothing is read at tier 0 (the Low preset)
    ++sGpu.frame;
    timerCollect();
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    const bool wanted = tier() >= 1 && d.mValid && d.mMode == WolfEnvDoc::WOLF && sharedSky();
    if (!wanted)
    {
        sMeter.reset();   // a new start when the Wolf sky comes back
        sGenusValid = false;
        sNext.valid = false;
        sNext.cloudTransmittanceAtCamera = 1.f;   // no old reading carried into the next start
        sNext.cloudDiffuseT = 1.f;
        sNext.cloudDiffusedBeam = 0.f;
        sGpu.camPboPending[0] = sGpu.camPboPending[1] = false;
        if (tier() > 0) logTimings();
        return;
    }
    timerStamp(SEC_LUTS, 0);
    // the atmosphere: built when the document's changes, over several frames, the previous one drawn meanwhile
    // (WolfTk::Atmosphere double-buffers its tables). Bruneton's 15-wavelength luminance mode (model.cc Init with
    // num_precomputed_wavelengths 15, as demo.cc InitModel uses for use_luminance == PRECOMPUTED) - Paul's
    // decision, so the twilight keeps its colour.
    const WolfTk::AtmosphereSpectrum spec = WolfTk::AtmosphereSpectrum::fromDoc(d);
    const size_t h = WolfTk::hashOf(spec);
    if (!sGpu.atmosRequested || h != sGpu.atmosHash)
    {
        // recorded only once a build has started: asked again each frame until the programs are loaded
        if (sAtmos->beginPrecompute(spec, 15))
        {
            sGpu.atmosHash = h;
            sGpu.atmosRequested = true;
        }
    }
    if (sAtmos->building())
    {
        // [WolfViewer 2026-10-13] Paul: "it still takes a lot of time to move to our native generation from the default eep".
        // One step a frame took ~350 frames - 20 s at the 15-20 fps of a login. Steps now run against a time budget: 25 ms a
        // frame for the first build (nothing native is on screen yet), 4 ms for a rebuild (the old tables stay in use, so a
        // rebuild must not cost the flight its frame rate).
        const F64 budget = sAtmos->ready() ? 0.004 : 0.025;
        LLTimer stepTimer;
        do
        {
            sAtmos->precomputeStep();
        } while (sAtmos->building() && !sAtmos->failed() && stepTimer.getElapsedTimeF64() < budget);
        if (sAtmos->failed()) fail("the atmosphere tables could not be built");
    }
    const F64 t = LLFrameTimer::getElapsedSeconds();
    const F32 dt = sGpu.lastTime > 0.0 ? (F32)llclamp(t - sGpu.lastTime, 0.0, 1.0) : 0.f;
    // a new region (teleport or crossing): the light there is a new reading, taken at once, as a light meter
    // pointed into new light reads it - not adapted toward over seconds from the old one
    static const LLViewerRegion* s_region = nullptr;
    if (gAgent.getRegion() != s_region)
    {
        s_region = gAgent.getRegion();
        sMeter.reset();
    }
    if (sAtmos->ready() && !sGpu.failed)
    {
        placeOnPlanet((F64)sAtmos->bottomRadiusKm() * 1000.0);
        updateLight(d, dt);   // sNext: drawn from the next frame on (sFrame, latched in tier())
    }
    sGpu.lastTime = t;
    timerStamp(SEC_LUTS, 1);
    logTimings();
}

// static
namespace
{
    // [WolfViewer 2026-10-13] THE EEP DRIVES THE SKY: the sky's gains toward the stock EEP sky (brunetonRuntime.glsl wolfEepGain),
    // and the last probe's numbers for the verification log (WolfEnvironmentTrace)
    constexpr S32 NPROBE = WolfTk::Atmosphere::EEP_GAINS;   // 6 heights x 6 azimuths (brunetonRuntime.glsl wolfEepGainCore)
    LLColor3 sGain[NPROBE];   // set white before first use (probeSky)
    bool sGainValid = false;
    LLVector3 sProbeDir[NPROBE];
    LLColor3 sProbeRaw[NPROBE], sProbeTarget[NPROBE];
    const F32 PROBE_ELEV_DEG[6] = { 90.f, 40.f, 20.f, 10.f, 5.f, 2.f };   // brunetonRuntime.glsl wolfEepGainCore E[]
    const F32 PROBE_AZ_DEG[6] = { 3.f, 10.f, 25.f, 50.f, 90.f, 180.f };    // .. A[], from the sun's azimuth

    /**
     * Renders the native dome (the program `sh`, every uniform already set for this frame) at 36 directions with the
     * gains off, reads them back, and sets each gain to the stock sky's value there (WolfEepReference::skyLinear) over the
     * native one, per channel - the frame is in the EEP's linear units (updateLight's exposure), so the two compare
     * directly. The grid is dense round the sun (3 deg off it at the nearest, missing its disc). Four times a second; the
     * gains ease over half a second in log space (a cut - teleport, a sky switched by hand - takes them at once).
     */
    void probeSky(LLGLSLShader& sh)
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
        if (!psky || !WolfEnvironmentRender::eepDriven())
        {
            // no EEP to drive the sky: the physical sky as it is
            if (sGainValid) { sGainValid = false; sAtmos->setEepGains(sGain, false); }
            return;
        }
        if (gCubeSnapshot || !gPipeline.mScreenTriangleVB) return;
        static F64 s_next = 0.0, s_last = 0.0;
        const F64 t = LLFrameTimer::getElapsedSeconds();
        if (t < s_next) return;
        const F32 dt = s_last > 0.0 ? (F32)llmin(t - s_last, 2.0) : 0.f;
        s_next = t + 0.5; s_last = t;

        static LLRenderTarget s_rt;
        if (!s_rt.isComplete() && !s_rt.allocate(NPROBE, 1, GL_RGBA16F, false)) return;
        static bool s_whiteInit = false;
        if (!s_whiteInit) { for (S32 i = 0; i < NPROBE; ++i) sGain[i] = LLColor3::white; s_whiteInit = true; }

        // the 36 directions (world: x east, y north, z up)
        const LLVector3 sun = sFrame.sunWorld;
        const F32 sunAz = (fabsf(sun.mV[VX]) + fabsf(sun.mV[VY]) > 1e-4f) ? atan2f(sun.mV[VX], sun.mV[VY]) : 0.f;   // from north, clockwise
        for (S32 r = 0; r < 6; ++r)
        {
            const F32 el = PROBE_ELEV_DEG[r] * DEG_TO_RAD;
            for (S32 c = 0; c < 6; ++c)
            {
                const F32 az = sunAz + PROBE_AZ_DEG[c] * DEG_TO_RAD;
                sProbeDir[r * 6 + c].set(cosf(el) * sinf(az), cosf(el) * cosf(az), sinf(el));
            }
        }

        static LLStaticHashedString s_probe("uProbe"), s_pdir("uProbeDir"), s_dgain("wolfEepDomeGain");
        // [WolfViewer 2026-10-13] Paul: "every time I move the cloud jerks" - a glReadPixels into client memory waits for the
        // GPU to finish the frame so far. The working probe is read into a pixel-pack buffer instead and mapped at the NEXT
        // probe, half a second later, when the copy is long done: no wait. (The verification render below, trace only, still
        // reads at once.)
        static GLuint s_pbo = 0;
        static bool s_pending = false;
        static LLVector3 s_pendDir[NPROBE];
        if (!s_pbo)
        {
            glGenBuffers(1, &s_pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, NPROBE * 4 * sizeof(F32), nullptr, GL_STREAM_READ);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        auto render = [&](bool gainsOn, F32* out)   // out == nullptr: into the pack buffer
        {
            sh.uniform1f(s_probe, 1.f);
            sh.uniform3fv(s_pdir, NPROBE, sProbeDir[0].mV);
            sh.uniform1f(s_dgain, gainsOn ? 1.f : 0.f);   // the dome's gain is on its whole colour (skyF.glsl wolfEepDomeGain)
            GLint vp[4] = { 0, 0, 0, 0 };
            glGetIntegerv(GL_VIEWPORT, vp);   // flush() rebinds the frame's target at its full size: a snapshot tile's viewport is kept
            s_rt.bindTarget();
            {
                LLGLDepthTest depth(GL_FALSE, GL_FALSE);
                LLGLDisable blend(GL_BLEND);
                gPipeline.mScreenTriangleVB->setBuffer();
                gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                if (out)
                {
                    glReadPixels(0, 0, NPROBE, 1, GL_RGBA, GL_FLOAT, out);
                }
                else
                {
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo);
                    glReadPixels(0, 0, NPROBE, 1, GL_RGBA, GL_FLOAT, nullptr);
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                }
            }
            s_rt.flush();
            glViewport(vp[0], vp[1], vp[2], vp[3]);
            sh.uniform1f(s_probe, 0.f);
            sh.uniform1f(s_dgain, sGainValid ? 1.f : 0.f);
        };
        // the reading started at the last probe (its directions kept with it), then the next one started
        F32 px[NPROBE * 4] = {};
        bool haveReading = false;
        if (s_pending)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo);
            if (const void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, NPROBE * 4 * sizeof(F32), GL_MAP_READ_BIT))
            {
                memcpy(px, mapped, sizeof(px));
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                haveReading = true;
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            s_pending = false;
        }
        LLVector3 readDir[NPROBE];
        for (S32 i = 0; i < NPROBE; ++i) readDir[i] = s_pendDir[i];
        render(false, nullptr);
        for (S32 i = 0; i < NPROBE; ++i) s_pendDir[i] = sProbeDir[i];
        s_pending = true;

        // VERIFICATION (WolfEnvironmentTrace, every 15 s): the same 36 directions rendered WITH this frame's gains, through
        // the stock display chain, against the stock sky's - what the screen shows, measured, not computed
        static LLCachedControl<bool> trace(gSavedSettings, "WolfEnvironmentTrace", false);
        static F64 s_nextVerify = 0.0;
        if (trace && sGainValid && t >= s_nextVerify)
        {
            s_nextVerify = t + 15.0;
            F32 pv[NPROBE * 4] = {};
            render(true, pv);
            const WolfEepReference::Path path = WolfEepReference::path(psky);
            auto d8 = [](const LLColor3& c) { return llformat("%3d,%3d,%3d", (S32)llround(c.mV[0] * 255.f), (S32)llround(c.mV[1] * 255.f), (S32)llround(c.mV[2] * 255.f)); };
            std::ostringstream o;
            o << "EEP verify (display 0-255; " << (path.classic ? "classic" : "pbr") << (path.noPost ? ", no post" : ", tonemapped")
              << ", gamma " << path.gamma << "):";
            for (S32 i = 0; i < NPROBE; ++i)
            {
                const LLColor3 measured(pv[i * 4 + 0], pv[i * 4 + 1], pv[i * 4 + 2]);
                o << "\n  sky " << llformat("%2d deg up, %3d deg from the sun", (S32)PROBE_ELEV_DEG[i / 6], (S32)PROBE_AZ_DEG[i % 6])
                  << "  EEP " << d8(WolfEepReference::display(path, sProbeTarget[i]))
                  << "  native " << d8(WolfEepReference::display(path, measured))
                  << "  (physical before gain " << d8(WolfEepReference::display(path, sProbeRaw[i])) << ")";
            }
            const LLColor3 eepWhite = WolfEepReference::surfaceLinear(psky);
            const LLColor3 natWhite = WolfEnvironmentRender::sceneAmblit()
                + WolfEnvironmentRender::sceneSunlit() * llmax(sFrame.sunUp ? sFrame.sunWorld.mV[VZ] : sFrame.moonWorld.mV[VZ], 0.f)
                  * llclamp(1.f - sFrame.cover, 0.f, 1.f);
            o << "\n  white ground   EEP " << d8(WolfEepReference::display(path, eepWhite)) << "  native " << d8(WolfEepReference::display(path, natWhite))
              << "  (the scene's sun and ambient are SET from the EEP's, surfaceLights; the clouds' shadow comes on top)";
            o << "\n  clouds         EEP covers " << WolfEepReference::cloudCoveredFraction(psky) << " of the sky (mean opacity "
              << WolfEepReference::cloudCoverage(psky) << ")  native measured " << sCoverMeasured << " over " << sCoverSamples
              << " texels above 20 deg (takram coverage " << sFrame.cover << ")";
            if (sStockValid)
                o << "\n  clouds (stock layer drawn, same view)  veils (opacity >= 0.2) " << sStockShare << " of the sky, mean opacity there "
                  << sStockOpacity << "  native veils " << sNativeShare << ", marched opacity " << sNativeOpacity << " thinned x "
                  << (1.f - sThin) << "  (learned coverage " << sCoverAbs << ")";
            o << "\n  cloud colour   EEP " << d8(WolfEepReference::display(path, sCloudTarget)) << "  native measured "
              << d8(WolfEepReference::display(path, sCloudMeasured)) << "  (mean over the covered texels, per-direction gains "
              << (sCloudGainValid ? "on" : "off") << ")";
            o << "\n  haze 1 km      EEP transmittance " << WolfEepReference::hazeTransmittance(psky, 1000.f)
              << "  (native: its haze density at the camera SET to the same extinction, cloudsFrame - not measured here)";
            o << "\n  haze 1 km      EEP surface " << d8(WolfEepReference::display(path, WolfEepReference::hazedSurfaceLinear(psky, LLVector3(0.f, 1.f, 0.f), 1000.f)))
              << "  5 km " << d8(WolfEepReference::display(path, WolfEepReference::hazedSurfaceLinear(psky, LLVector3(0.f, 1.f, 0.f), 5000.f)));
            LL_INFOS("WolfEnvTrace") << o.str() << LL_ENDL;
        }

        if (!haveReading) return;
        static LLVector3d s_lastCam; static bool s_haveCam = false;
        const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
        const bool cut = !sGainValid || dt <= 0.f || (s_haveCam && (cam - s_lastCam).magVec() > 300.0);
        s_lastCam = cam; s_haveCam = true;
        const F32 k = cut ? 1.f : 1.f - expf(-dt / 0.5f);
        for (S32 i = 0; i < NPROBE; ++i)
        {
            sProbeRaw[i].set(px[i * 4 + 0], px[i * 4 + 1], px[i * 4 + 2]);
            sProbeTarget[i] = WolfEepReference::skyLinear(psky, readDir[i]);   // the directions that reading looked along
            for (S32 c = 0; c < 3; ++c)
            {
                const F32 raw = sProbeRaw[i].mV[c], want = sProbeTarget[i].mV[c];
                if (!(raw > 1e-7f) || !(want >= 0.f) || !llfinite(raw)) continue;   // a probe that saw nothing keeps its gain
                const F32 g = llclamp(llmax(want, 1e-7f) / raw, 1.f / 1024.f, 1024.f);
                const F32 lg = log2f(llmax(sGain[i].mV[c], 1e-9f));
                sGain[i].mV[c] = exp2f(lg + (log2f(g) - lg) * k);
            }
        }
        sGainValid = true;
        sAtmos->setEepGains(sGain, true);
    }
}

namespace
{
    /**
     * [WolfViewer 2026-10-13] THE EEP DRIVES THE CLOUD COVER, measured. takram's coverage is a threshold on its weather texture
     * eroded by its shape noise, not a share of the sky, so the coverage that shows the EEP's clouds is found by measuring:
     * every two seconds this frame's low-resolution march target is read back (.a = the clouds' own opacity over the sky,
     * cloudsF.glsl), the texels looking 20 deg or more above the horizon at open sky are counted, and the share with opacity
     * >= 0.5 is compared with the stock share (WolfEepReference::cloudCoveredFraction: alpha1 >= 0.5). The coverage is the stock
     * share x a learned scale; each reading with 2000 texels or more moves the scale by at most 1 %, and the cover itself eases
     * over 5 s (updateLight). The clouds' colour, per direction, is learned the same way (below).
     */
    void measureCloudCover(const WolfTk::CloudsFrame& f)
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
        if (!psky || !WolfEnvironmentRender::eepDriven())
        {
            sCoverCtlValid = false;
            if (sCloudGainValid) { sCloudGainValid = false; LLColor3 white[NPROBE]; for (S32 i = 0; i < NPROBE; ++i) white[i] = LLColor3::white; sAtmos->setEepCloudGains(white, false); }
            if (sStockValid || sThin != 0.f) { sStockValid = false; sCoverAbs = -1.f; sThin = sThinTarget = 0.f; sAtmos->setEepCloudThin(0.f); }
            sCoverLocked = false; sCoverSettled = 0; sCoverLockTarget = -1.f;
            return;
        }
        static F64 s_next = 0.0;
        static LLVector3d s_lastCam; static bool s_haveCam = false;
        // [WolfViewer 2026-10-13] Paul: "every time I move the cloud jerks and changes": the reads go to pixel-pack buffers and
        // are taken two seconds later (no wait for the GPU), and the stock cloud colour is computed once per grid node.
        // [WolfViewer 2026-10-13, round 12] Paul: "its flashing and jerking". The readings swing with the view (the cloud field is
        // patchy: 0.22 one second, 0.005 the next) and a loop that stepped the coverage and the colour on each reading made the
        // clouds grow, shrink and change colour every second. Now the coverage is the stock share x a scale that the readings
        // only LEARN, at most 1 % a reading; the colour gains the readings set are targets the applied gains glide to over
        // 3 s, per frame; nodes out of view keep their gains.
        static LLColor3 s_grid[NPROBE], s_gridTarget[NPROBE];
        static F64 s_lastEase = 0.0;
        const LLVector3d cam = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
        if (s_haveCam && (cam - s_lastCam).magVec() > 300.0)
        {
            // a teleport: start again, the clouds' gains off until the new place is measured
            sCoverCtlValid = false;
            if (sCloudGainValid) { sCloudGainValid = false; LLColor3 white[NPROBE]; for (S32 i = 0; i < NPROBE; ++i) white[i] = LLColor3::white; sAtmos->setEepCloudGains(white, false); }
            if (sStockValid || sThin != 0.f) { sStockValid = false; sCoverAbs = -1.f; sThin = sThinTarget = 0.f; sAtmos->setEepCloudThin(0.f); }
            sCoverLocked = false; sCoverSettled = 0; sCoverLockTarget = -1.f;
        }
        s_lastCam = cam; s_haveCam = true;
        const F32 target = WolfEepReference::cloudCoveredFraction(psky);
        // takram's coverage is a threshold, so it runs well above the stock share: the share x the learned scale (updateLight)
        sCoverCtlValid = true;
        const F64 t = LLFrameTimer::getElapsedSeconds();
        // the colour gains glide toward their targets every frame (3 s)
        if (sCloudGainValid)
        {
            const F32 dte = s_lastEase > 0.0 ? (F32)llmin(t - s_lastEase, 0.5) : 0.f;
            const F32 k = 1.f - expf(-dte / 1.5f);
            for (S32 i = 0; i < NPROBE; ++i) s_grid[i] += (s_gridTarget[i] - s_grid[i]) * k;
            sAtmos->setEepCloudGains(s_grid, true);
        }
        // [round 16] the thinning glides the same way (3 s)
        if (sStockValid)
        {
            const F32 dte = s_lastEase > 0.0 ? (F32)llmin(t - s_lastEase, 0.5) : 0.f;
            sThin += (sThinTarget - sThin) * (1.f - expf(-dte / 1.5f));
            sAtmos->setEepCloudThin(sThin);
        }
        s_lastEase = t;
        if (t < s_next) return;
        s_next = t + 1.0;

        S32 w = 0, h = 0;
        const GLuint tex = sClouds->marchDepthVelocityTex(w, h);
        if (!tex || w <= 0 || h <= 0) return;
        static GLuint s_pbo[2] = { 0, 0 };
        static size_t s_pboBytes = 0;
        static bool s_pending = false;
        static S32 s_pw = 0, s_ph = 0;
        static U32 s_seq = 0;   // the reading in flight (the stock probe's read-back is matched to it)
        static glm::mat4 s_pinv(1.f);
        const size_t bytes = (size_t)w * h * 4 * sizeof(F32);
        if (!s_pbo[0]) glGenBuffers(2, s_pbo);
        if (bytes != s_pboBytes)
        {
            for (S32 i = 0; i < 2; ++i)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[i]);
                glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            s_pboBytes = bytes;
            s_pending = false;
        }
        // take the reading started a second ago
        std::vector<F32> buf, col;
        S32 rw = 0, rh = 0;
        glm::mat4 inv(1.f);
        if (s_pending && (size_t)s_pw * s_ph * 4 * sizeof(F32) == s_pboBytes)
        {
            buf.resize((size_t)s_pw * s_ph * 4); col.resize(buf.size());
            bool ok = true;
            for (S32 i = 0; i < 2; ++i)
            {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[i]);
                const void* m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, s_pboBytes, GL_MAP_READ_BIT);
                if (m) { memcpy(i == 0 ? buf.data() : col.data(), m, s_pboBytes); glUnmapBuffer(GL_PIXEL_PACK_BUFFER); } else ok = false;
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            if (ok) { rw = s_pw; rh = s_ph; inv = s_pinv; }
        }
        // [round 16] the stock layer drawn for that reading (renderStockCloudProbe), same size, same camera a frame later
        std::vector<F32> stock;
        if (rw > 0 && sStockPending && sStockSeqRead == s_seq && sStockPW == rw && sStockPH == rh && sStockPbo
            && sStockPboBytes == (size_t)rw * rh * 4 * sizeof(F32))
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sStockPbo);
            const void* m = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sStockPboBytes, GL_MAP_READ_BIT);
            if (m) { stock.resize((size_t)rw * rh * 4); memcpy(stock.data(), m, sStockPboBytes); glUnmapBuffer(GL_PIXEL_PACK_BUFFER); }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        sStockPending = false;
        const bool haveStock = !stock.empty();
        // start the next
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[0]);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s_pbo[1]);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sClouds->marchColorTex());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        s_pending = true; s_pw = w; s_ph = h;
        // ... and have the sky pass draw the stock layer for it (renderStockCloudProbe)
        ++s_seq; sStockSeqWanted = s_seq; sStockDue = true; sStockW = w; sStockH = h;
        s_pinv = glm::inverse(f.proj * f.view);   // clip -> the LL agent frame (z up), for that reading
        if (rw <= 0) return;

        // [WolfViewer 2026-10-13] the clouds' colour per direction, on the sky's 6 x 6 grid (heights 90 40 20 10 5 2 deg x azimuths
        // 3 10 25 50 90 180 deg from the sun): each covered texel 2 deg or more up counts to its nearest node; the stock cloud
        // colour is taken once per node, at the node's own direction. (Coverage still counts only 20 deg and up.)
        static const F32 GE[6] = { 90.f, 40.f, 20.f, 10.f, 5.f, 2.f }, GA[6] = { 3.f, 10.f, 25.f, 50.f, 90.f, 180.f };
        const LLVector3 sun = sFrame.sunWorld;
        const F32 sunAz = atan2f(sun.mV[VX], sun.mV[VY]);
        LLColor3 nodeNative[NPROBE], nodeTarget[NPROBE]; S32 nodeCount[NPROBE] = {}; S8 nodeState[NPROBE] = {};
        LLColor3 nSum, tSum; S32 nCol = 0;
        const F32 sin20 = sinf(20.f * DEG_TO_RAD), sin2 = sinf(2.f * DEG_TO_RAD);
        S32 n = 0, covered = 0;
        // [WolfViewer 2026-10-13, round 16] Paul: "big gap in the clouds ... EEP is very different". The stock layer measured on the
        // screen (SCREEN verify, Iceland night, 13 readings) is a VEIL: opacity 0.2 - 0.6 over half the sky and more, never
        // solid; the native clouds were solid clumps with open sky between. The CPU reference cannot predict that spread (the
        // noise texture's filtering on the dome), so the stock layer itself is drawn for each reading (renderStockCloudProbe)
        // and, over the same texels: the share of the sky its clouds veil (opacity >= VEIL) is the share the native clouds must
        // veil (the coverage), its mean opacity there is theirs (the thinning), and its colour per node is their colour target.
        const F32 VEIL = 0.2f;
        const F32 keep = 1.f - sThin;   // the native opacity on screen = the marched one x keep (cloudsF.glsl wolfEepCloudThin)
        S32 sVeil = 0, nVeil = 0; F64 sOpSum = 0.0, nOpSum = 0.0; S32 nOpN = 0;
        LLColor3 stockNode[NPROBE]; S32 stockNodeN[NPROBE] = {};
        for (S32 y = 0; y < rh; y += 2)
        {
            for (S32 x = 0; x < rw; x += 2)
            {
                const size_t q = ((size_t)y * rw + x) * 4;
                const F32 a = buf[q + 3];
                if (!(a >= 0.f)) continue;   // a scene surface (-distance), or not a number
                const glm::vec2 ndc(2.f * (x + 0.5f) / rw - 1.f, 2.f * (y + 0.5f) / rh - 1.f);
                glm::vec4 pn = inv * glm::vec4(ndc, -1.f, 1.f), pf = inv * glm::vec4(ndc, 1.f, 1.f);
                const glm::vec3 dir = glm::normalize(glm::vec3(pf) / pf.w - glm::vec3(pn) / pn.w);
                if (dir.z < sin2) continue;
                const F32 sa = haveStock && llfinite(stock[q + 3]) ? llclamp(stock[q + 3], 0.f, 1.f) : 0.f;   // the stock alpha1
                if (dir.z >= sin20)
                {
                    ++n; if (a >= 0.5f) ++covered;
                    if (haveStock)
                    {
                        if (sa >= VEIL) { ++sVeil; sOpSum += sa; }
                        if (a * keep >= VEIL) ++nVeil;
                        if (a >= VEIL) { nOpSum += a; ++nOpN; }
                    }
                }
                const bool nativeCloud = a >= 0.5f && col[q + 3] > 0.05f && llfinite(col[q]);
                const bool stockCloud = haveStock && sa >= VEIL && llfinite(stock[q]);
                if (!nativeCloud && !stockCloud) continue;
                const F32 el = asinf(llclamp(dir.z, -1.f, 1.f)) * RAD_TO_DEG;
                F32 daz = fabsf(atan2f(dir.x, dir.y) - sunAz) * RAD_TO_DEG;
                if (daz > 180.f) daz = 360.f - daz;
                S32 bi = 0, bj = 0;
                for (S32 k = 1; k < 6; ++k) { if (fabsf(el - GE[k]) < fabsf(el - GE[bi])) bi = k; if (fabsf(daz - GA[k]) < fabsf(daz - GA[bj])) bj = k; }
                const S32 node = bi * 6 + bj;
                if (stockCloud)
                {
                    // the stock fragment's colour as its G-buffer holds it (the probe has the same formats), linearised
                    stockNode[node] += WolfEepReference::cloudGbufferLinear(psky, LLColor3(stock[q], stock[q + 1], stock[q + 2]));
                    ++stockNodeN[node];
                }
                if (!nativeCloud) continue;
                if (haveStock)
                {
                    // the target comes from the stock layer after the loop
                    nodeState[node] = 1;
                    const LLColor3 own = LLColor3(col[q], col[q + 1], col[q + 2]) * (1.f / col[q + 3]);
                    nodeNative[node] += own; ++nodeCount[node];
                    continue;
                }
                if (nodeState[node] == 0)
                {
                    const F32 ce = GE[bi] * DEG_TO_RAD, ca = sunAz + GA[bj] * DEG_TO_RAD;
                    nodeState[node] = WolfEepReference::cloudColourLinear(psky, LLVector3(cosf(ce) * sinf(ca), cosf(ce) * cosf(ca), sinf(ce)), nodeTarget[node]) ? 1 : -1;
                }
                if (nodeState[node] < 0) continue;
                const LLColor3 own = LLColor3(col[q], col[q + 1], col[q + 2]) * (1.f / col[q + 3]);
                nodeNative[node] += own; ++nodeCount[node];
                nSum += own; tSum += nodeTarget[node]; ++nCol;
            }
        }
        if (haveStock)
        {
            // each node's target is the stock layer's own colour there (8 texels or more); a node with native clouds but too few
            // stock texels is not corrected this reading
            for (S32 i = 0; i < NPROBE; ++i)
            {
                if (nodeCount[i] <= 0) continue;
                if (stockNodeN[i] < 8) { nodeState[i] = -1; continue; }
                nodeTarget[i] = stockNode[i] * (1.f / stockNodeN[i]);
                nSum += nodeNative[i]; tSum += nodeTarget[i] * (F32)nodeCount[i]; nCol += nodeCount[i];
            }
        }
        sCoverSamples = n;
        if (n < 200) return;   // too little sky above 20 deg in view: hold
        sCoverMeasured = (F32)covered / (F32)n;
        if (haveStock)
        {
            // [round 16] the coverage: the native veiled share moved toward the stock's, at most 5 % a reading (the target is the
            // same texels' stock layer, so it does not swing with the view as the reference share did); the thinning: the stock
            // mean opacity over its veil against the native one, before the thinning
            sStockShare = (F32)sVeil / (F32)n; sNativeShare = (F32)nVeil / (F32)n;
            sStockOpacity = sVeil > 0 ? (F32)(sOpSum / sVeil) : 0.f;
            sNativeOpacity = nOpN > 0 ? (F32)(nOpSum / nOpN) : -1.f;
            if (sCoverAbs < 0.f) sCoverAbs = llclamp(target * sCoverScale, 0.02f, 1.f);   // from the cover in use
            // the EEP's own cloud share moved (a sky transition, a day-cycle keyframe): settle again
            if (sCoverLocked && sCoverLockTarget > 0.f && fabsf(target - sCoverLockTarget) > 0.15f * llmax(sCoverLockTarget, 0.05f))
            {
                sCoverLocked = false; sCoverSettled = 0;
                LL_INFOS("WolfEnv") << "clouds: EEP cloud share " << sCoverLockTarget << " -> " << target << ", coverage settling again" << LL_ENDL;
            }
            const F32 err = (sStockShare - sNativeShare) / llmax(sStockShare, 0.05f);
            if (!sCoverLocked)
            {
                sCoverAbs = llclamp(sCoverAbs * (1.f + llclamp(0.3f * err, -0.05f, 0.05f)), 0.02f, 1.f);
                if (nOpN >= 50 && sVeil >= 50) sThinTarget = llclamp(1.f - sStockOpacity / sNativeOpacity, 0.f, 0.8f);
                else if (sVeil == 0) sThinTarget = 0.f;
                sCoverSettled = fabsf(err) < 0.15f ? sCoverSettled + 1 : 0;
                if (sCoverSettled >= 3)
                {
                    sCoverLocked = true; sCoverLockTarget = target;
                    LL_INFOS("WolfEnv") << "clouds: coverage settled at " << sCoverAbs << " (thinning " << sThinTarget << "), locked" << LL_ENDL;
                }
            }
            sStockValid = true;
        }
        // a proportional step each reading: 0.3 x the error, held to 0.03 a step so the clouds grow and shrink gently; at least
        // 0.03 while none show at all (takram draws nothing below its weather threshold, so a measured 0 says little)
        // learn the scale: at most 1 % a reading, and only from a reading with plenty of sky
        if (n >= 2000 && target > 0.01f)
        {
            const F32 err = (target - sCoverMeasured) / target;
            sCoverScale = llclamp(sCoverScale * (1.f + llclamp(0.05f * err, -0.01f, 0.01f)), 1.f, 10.f);
        }
        // the clouds' colour: each node seen (8 texels or more) moves its gain halfway (in log) to the ratio of the stock
        // colour to the measured one, per channel, held to 0.25 .. 4 x; nodes not in view take the mean of those that were
        if (!sCloudGainValid) { for (S32 i = 0; i < NPROBE; ++i) s_grid[i] = s_gridTarget[i] = LLColor3::white; }
        LLColor3 seenSum; S32 seen = 0;
        for (S32 i = 0; i < NPROBE; ++i)
        {
            if (nodeCount[i] < 8 || nodeState[i] <= 0) continue;
            const LLColor3 meas = nodeNative[i] * (1.f / nodeCount[i]);
            for (S32 c = 0; c < 3; ++c)
            {
                if (!(meas.mV[c] > 1e-6f) || !(nodeTarget[i].mV[c] > 0.f)) continue;
                // the first reading is taken whole; after it, at most 4 x either way a reading
                const F32 r = llclamp(nodeTarget[i].mV[c] / meas.mV[c], sCloudGainValid ? 1.f / 4.f : 1.f / 8.f, 4.f);
                // held to 1/8 .. 4 x: physical clouds toward a low sun are 4 - 5 x brighter than the stock flat clouds (Iceland
                // 10-13: measured 242,199,166 against 171,152,146) and a 1/4 floor left them there; 1/32 blacked them out at
                // night (round 16). The ratio is against the
                // gain in use when the reading was taken (s_grid), so the target is the gain that reading asks for.
                s_gridTarget[i].mV[c] = llclamp(s_grid[i].mV[c] * r, 1.f / 8.f, 4.f);
            }
            seenSum += s_gridTarget[i]; ++seen;
        }
        // [WolfViewer 2026-10-13, round 16] Paul: "big gap in the clouds". Toward the moon the physical clouds glow and their node
        // was darkened to 1/32 while the next node barely changed: near-black clouds on a night sky beside bright ones - a
        // hole. The targets are now smoothed with their neighbours (half each), kept within 4 x of the grid's mean, and never
        // darker than 1/8: the clouds keep their light and shape across the sky, the EEP's colour is matched on average.
        if (seen > 0)
        {
            LLColor3 mean; for (S32 i = 0; i < NPROBE; ++i) mean += s_gridTarget[i];
            mean *= 1.f / NPROBE;
            LLColor3 smoothed[NPROBE];
            for (S32 r = 0; r < 6; ++r)
            {
                for (S32 cc = 0; cc < 6; ++cc)
                {
                    LLColor3 nsum; S32 nn = 0;
                    if (r > 0) { nsum += s_gridTarget[(r - 1) * 6 + cc]; ++nn; }
                    if (r < 5) { nsum += s_gridTarget[(r + 1) * 6 + cc]; ++nn; }
                    if (cc > 0) { nsum += s_gridTarget[r * 6 + cc - 1]; ++nn; }
                    if (cc < 5) { nsum += s_gridTarget[r * 6 + cc + 1]; ++nn; }
                    smoothed[r * 6 + cc] = s_gridTarget[r * 6 + cc] * 0.5f + nsum * (0.5f / nn);
                }
            }
            for (S32 i = 0; i < NPROBE; ++i)
                for (S32 c = 0; c < 3; ++c)
                    s_gridTarget[i].mV[c] = llclamp(llclamp(smoothed[i].mV[c], mean.mV[c] * 0.25f, mean.mV[c] * 4.f), 1.f / 8.f, 4.f);
        }
        if (seen > 0 && !sCloudGainValid)
        {
            // the first reading: the nodes not in view start from the mean of those that were, and the gains take it at once
            // (no glide from the physical colour)
            const LLColor3 mean = seenSum * (1.f / seen);
            for (S32 i = 0; i < NPROBE; ++i) if (nodeCount[i] < 8) s_gridTarget[i] = mean;
            for (S32 i = 0; i < NPROBE; ++i) s_grid[i] = s_gridTarget[i];
            sCloudGainValid = true;
            sAtmos->setEepCloudGains(s_grid, true);
        }
        if (nCol > 0) { sCloudMeasured = nSum * (1.f / nCol); sCloudTarget = tSum * (1.f / nCol); }
    }
}

bool WolfEnvironmentRender::stockCloudProbeDue()
{
    return sStockDue && eepDriven() && sStockW > 0 && sStockH > 0;
}

void WolfEnvironmentRender::renderStockCloudProbe(const std::function<void()>& draw)
{
    sStockDue = false;
    const S32 w = sStockW, h = sStockH;
    if (w <= 0 || h <= 0) return;
    // the stock G-buffer's formats (pipeline.cpp allocateScreenBuffer / addDeferredAttachments): the colour target RGBA8, so the
    // layer's colour clamps as it does there; with the emissive buffer the cloud shader writes to attachment 3 (cloudsF.glsl
    // HAS_EMISSIVE), RGB16F there - RGBA16F here to keep the opacity
    static LLCachedControl<bool> has_emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
    const bool em = has_emissive();
    if (!sStockRT.isComplete() || (S32)sStockRT.getWidth() != w || (S32)sStockRT.getHeight() != h || em != sStockRTEmissive)
    {
        sStockRT.release();
        bool ok = sStockRT.allocate(w, h, GL_RGBA) && sStockRT.addColorAttachment(GL_RGBA) && sStockRT.addColorAttachment(GL_RGBA);
        if (ok && em) ok = sStockRT.addColorAttachment(GL_RGBA16F);
        if (!ok) { sStockRT.release(); return; }
        sStockRTEmissive = em;
    }
    const size_t bytes = (size_t)w * h * 4 * sizeof(F32);
    if (!sStockPbo) glGenBuffers(1, &sStockPbo);
    if (bytes != sStockPboBytes)
    {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, sStockPbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        sStockPboBytes = bytes;
    }
    sStockRT.bindTarget();
    {
        LLGLDisable no_scissor(GL_SCISSOR_TEST);
        GLfloat clear[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        sStockRT.clear(GL_COLOR_BUFFER_BIT);
        glClearColor(clear[0], clear[1], clear[2], clear[3]);
    }
    draw();
    gGL.flush();
    glReadBuffer(GL_COLOR_ATTACHMENT0 + (em ? 3 : 0));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sStockPbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    sStockRT.flush();
    sStockPending = true; sStockSeqRead = sStockSeqWanted; sStockPW = w; sStockPH = h;
}

// [WolfViewer 2026-10-14] the magenta sky: the dome's draw fails with GL_INVALID_OPERATION (0x502) while the program validates
// and every logged state is right. The driver says why in words through GL debug output; while WolfEnvironmentTrace is on,
// one sky pass every 10 s runs with it switched on (synchronous, so each message comes from the call that caused it) and
// every message is logged here. Linux LL installs no callback of its own (llrender.cpp init: Windows + gDebugGL only).
// [w66 CI 2026-10-14] GL debug output is compiled out on macOS (no glDebugMessageCallback there: the macos64 link
// failed) and gated on gGLManager.mHasDebugOutput elsewhere (llgl.cpp:1472 sets it for GL >= 4.3; Windows loads the
// pointers at llgl.cpp:2173-2175, Linux links the real functions, so testing their address is always true).
#if !LL_DARWIN
static void APIENTRY wolfSkyGLDebug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void*)
{
    LL_WARNS("WolfEnv") << "sky pass GL message: source 0x" << std::hex << source << " type 0x" << type << " id 0x" << id
                        << " severity 0x" << severity << std::dec << ": " << (message ? std::string(message, length > 0 ? (size_t)length : strlen(message)) : std::string())
                        << LL_ENDL;
}
#endif

bool WolfEnvironmentRender::renderSkyDome()
{
    if (!skyActive()) return false;
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    LLGLSLShader& sh = gWolfEnvSkyProgram;
    // the driver's own messages for this whole pass (wolfSkyGLDebug), trace only, once every 10 s, the main view only
    static LLCachedControl<bool> glMsgTrace(gSavedSettings, "WolfEnvironmentTrace", false);
    static F64 s_glMsgNext = 0.0;
#if LL_DARWIN
    const bool glMsg = false;
    (void)glMsgTrace; (void)s_glMsgNext;
#else
    const bool glMsg = glMsgTrace && !gCubeSnapshot && gGLManager.mHasDebugOutput && LLFrameTimer::getElapsedSeconds() >= s_glMsgNext;
#endif
#if !LL_DARWIN
    if (glMsg)
    {
        s_glMsgNext = LLFrameTimer::getElapsedSeconds() + 10.0;
        while (glGetError() != GL_NO_ERROR) {}
        glDebugMessageCallback((GLDEBUGPROC)wolfSkyGLDebug, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        LL_INFOS("WolfEnv") << "sky pass: GL debug output on for this pass" << LL_ENDL;
    }
#endif
    timerStamp(SEC_SKY, 0);
    sh.bind();
    // [WolfViewer 2026-10-14] THE MAGENTA SKY. LL gives a texture unit only to its reserved sampler names (llglslshader.cpp
    // mapUniform); this program's own samplers start on unit 0 until set. wolfMoonMap and wolfSunMap were set only while the
    // moon / an EEP sun texture was drawn, so with the moon down wolfMoonMap (sampler2D) stayed on unit 0 - the unit LL gave
    // higher_order_scattering_texture (sampler3D). Once the atmosphere tables were built and bound there, NVIDIA refused every
    // draw: "GL_INVALID_OPERATION ... State(s) are invalid: program texture usage" (Paul's log 07:15:06Z, the sampler list
    // showed both on unit 0); the dome drew nothing, LL's magenta G-buffer clear showed, and the water reflected it. Both
    // samplers now sit on their own named units every frame, drawn or not (shadowLengthBuffer is set below, NAMED_UNIT_BASE).
    {
        static LLStaticHashedString s_moonMap("wolfMoonMap"), s_sunMap("wolfSunMap");
        const S32 moonLoc = sh.getUniformLocation(s_moonMap), sunLoc = sh.getUniformLocation(s_sunMap);
        if (moonLoc >= 0) glUniform1i(moonLoc, WolfTk::NAMED_UNIT_BASE + 1);
        if (sunLoc >= 0) glUniform1i(sunLoc, WolfTk::NAMED_UNIT_BASE + 2);
    }
    // Source: LLDrawPoolWLSky::renderSkyHazeDeferred - the same depth state for the dome.
    LLGLSPipelineDepthTestSkyBox sky(true, true);
    const F32 e = sFrame.exposure;
    sAtmos->setRuntimeUniforms(sh, e);
    setFrameUniforms(sh, true);
    {
        // [WolfViewer 2026-10-13] the dome takes the EEP gains on its whole colour (skyF.glsl wolfEepDomeGain), so the runtime's
        // own gain on the air's in-scatter is off for this program - the probe measures exactly what is corrected
        static LLStaticHashedString s_gon("wolfEepGainOn"), s_dgain("wolfEepDomeGain");
        sh.uniform1f(s_gon, 0.f);
        sh.uniform1f(s_dgain, sGainValid ? 1.f : 0.f);
    }

    // last frame's light shafts (sky.frag HAS_SHADOW_LENGTH: the program is built with it; no clouds = a zero
    // table)
    static LLStaticHashedString s_res("resolution"), s_vpo("viewportOrigin");
    const bool shafts = !gCubeSnapshot && tier() >= 2 && sFrame.cover > 0.f && sClouds->shadowLengthTex() && gPipeline.mRT;
    const S32 shaftUnit = WolfTk::NAMED_UNIT_BASE;
    WolfTk::bindNamed(sh, "shadowLengthBuffer", shaftUnit, GL_TEXTURE_2D, shafts ? sClouds->shadowLengthTex() : zeroTex());
    // the shafts were rendered over the whole viewport the frame draws into: the dome's screen uv is its
    // fragment's place in that viewport (a tiled snapshot's viewport is not the window's)
    GLint vp[4] = { 0, 0, 1, 1 };
    glGetIntegerv(GL_VIEWPORT, vp);
    sh.uniform2f(s_vpo, (F32)vp[0], (F32)vp[1]);
    sh.uniform2f(s_res, (F32)llmax(vp[2], 1), (F32)llmax(vp[3], 1));

    static LLStaticHashedString s_md("uMoonDir"), s_ms("uMoonSin"), s_ml("uMoonLum"), s_msd("uMoonSunDir"), s_mn("uMoonNorth"), s_mnorm("uMoonNorm"), s_mon("uMoonOn");
    static LLStaticHashedString s_bc("uBodyCount"), s_bd("uBodyDir"), s_bcol("uBodyColor"), s_bs("uBodySin"), s_bt("uBodyType"), s_br("uBodyRing"), s_lit("uLitDir");
    static LLStaticHashedString s_neb("uNebula"), s_nebc("uNebulaColor"), s_exp("uExposure"), s_night("uNightSky");
    static LLStaticHashedString s_sea("wolf_sea_height");

    // the moon's disc (decision D5): the K&S illuminance over its solid angle; the shading normalised by the
    // disc's mean Lommel-Seeliger factor and the LRO map's mean albedo; its transmittance in the shader
    // sizes and the map's albedo kept off 0 (a document's size 0, an all-black map): they divide below
    // [WolfViewer 2026-10-13] Paul (Europa, "a big Jupiter in the sky but I don't see it"): an EEP sky with its own moon
    // texture (often a planet) is drawn as EEP draws its moon - that texture, at the EEP's own moon direction and moon_scale,
    // the whole disc lit (LL's moon pass draws the texture flat, no phase) - still seen through the physical atmosphere.
    // Source: llsettingssky.h getMoonTextureId / GetDefaultMoonTextureId / getMoonDirection / getMoonScale. The default moon
    // stays the real moon (NASA map, the document's size, the real sun's phase).
    if (sMoonMap.isNull())
    {
        // NASA SVS CGI Moon Kit (svs.gsfc.nasa.gov/4720), LRO colour, 2048 x 1024 (lldrawpoolwlsky.cpp's)
        sMoonMap = LLViewerTextureManager::getFetchedTextureFromFile("world/wolf_moon_lroc.png", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
    }
    LLPointer<LLViewerFetchedTexture> moonMap = sMoonMap;
    bool eepMoon = false;
    LLVector3 moonDir = sFrame.moonWorld, moonSunDir = sFrame.sunWorld;
    F32 moonScale = d.mMoons.empty() ? 1.f : llmax(d.mMoons[0].mSize, 1e-3f);
    if (LLSettingsSky::ptr_t eep = LLEnvironment::instance().getCurrentSky())
    {
        const LLUUID mid = eep->getMoonTextureId();
        if (mid.notNull() && mid != LLSettingsSky::GetDefaultMoonTextureId())
        {
            LLViewerFetchedTexture* t = LLViewerTextureManager::getFetchedTexture(mid, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
            if (t && t->hasGLTexture())
            {
                moonMap = t;
                eepMoon = true;
                moonDir = eep->getMoonDirection(); moonDir.normalize();
                moonSunDir = -moonDir;   // the sun behind the viewer: the whole disc lit, as LL draws the texture
                moonScale = llmax(eep->getMoonScale(), 1e-3f);
            }
        }
    }
    // the moon's disc (decision D5): the K&S illuminance over its solid angle; the shading normalised by the
    // disc's mean Lommel-Seeliger factor and the map's mean albedo; its transmittance in the shader
    const F32 moonOmega = (F32)(2.0 * F_PI * (1.0 - cos(WOLF_MOON_RADIUS * moonScale)));
    const LLColor3 moonTint = d.mMoons.empty() ? LLColor3::black : (d.mMoons[0].mHasColor ? lin(d.mMoons[0].mColor) : LLColor3::white);
    const LLColor3 moonLum = moonTint * (sFrame.moonTOA * e / moonOmega);
    const F32 lsMean = lommelSeeligerMean(moonDir, moonSunDir);
    const bool moonOn = (eepMoon || !d.mMoons.empty()) && moonMap.notNull() && moonMap->hasGLTexture() && lsMean > 1e-4f && moonDir.mV[VZ] > -0.2f;
    S32 moonUnit = -1;
    if (moonOn)
    {
        const S32 loc = sh.getUniformLocation(LLStaticHashedString("wolfMoonMap"));
        if (loc >= 0) { moonUnit = WolfTk::NAMED_UNIT_BASE + 1; gGL.getTexUnit(moonUnit)->bind(moonMap); glUniform1i(loc, moonUnit); }
    }
    const LLVector3 north = (!eepMoon && WolfRegionWeather::instance().autoEnvRealSky()) ? lunarNorth() : LLVector3::z_axis;
    sh.uniform3fv(s_md, 1, moonDir.mV);
    sh.uniform1f(s_ms, (F32)sin(llmin(WOLF_MOON_RADIUS * moonScale, 1.2)));
    sh.uniform3fv(s_ml, 1, (eepMoon ? LLColor3::white * (sFrame.moonTOA * e / moonOmega) : moonLum).mV);
    sh.uniform3fv(s_msd, 1, moonSunDir.mV);
    sh.uniform3fv(s_mn, 1, north.mV);
    sh.uniform1f(s_mnorm, moonOn ? 1.f / (lsMean * llmax(moonMeanAlbedo(), 1e-3f)) : 0.f);

    // [WolfViewer 2026-10-13, round 16] while the EEP drives the sky the moon is LL's moon pass (skyF.glsl wolfEepMoon), the
    // physical moon disc off. Every value as lldrawpoolwlsky.cpp renderHeavenlyBodies / llvosky.cpp updateHeavenlyBodyGeometry
    // set them for moonF.glsl.
    bool eepMoonOn = false;
    {
        static LLStaticHashedString s_eon("uEepMoonOn"), s_ereal("uEepMoonReal"), s_eto("uEepMoonTo"), s_eright("uEepMoonRight"),
            s_eup("uEepMoonUp"), s_ehalf("uEepMoonHalf"), s_ephase("uEepMoonPhase"), s_enorth("uEepMoonNorth"), s_efade("uEepMoonFade"),
            s_ebright("uEepMoonBright"), s_escale("uEepSkyScale"), s_eclamp("uEepGbufClamp");
        LLSettingsSky::ptr_t eep = eepDriven() ? LLEnvironment::instance().getCurrentSky() : LLSettingsSky::ptr_t();
        // drawn when the moon is up (llvosky.cpp:1077 getIsMoonUp: llsettingssky.cpp getIsMoonUp, z >= 0)
        // ... and the moon face has its texture (lldrawpoolwlsky.cpp renderHeavenlyBodies: face->getTexture(DIFFUSE_MAP))
        if (eep && gSky.mVOSkyp.notNull() && eep->getIsMoonUp() && gSky.mVOSkyp->getMoonTex())
        {
            const bool real = WolfRegionWeather::instance().autoEnvRealSky();
            LLViewerTexture* tex = real ? (LLViewerTexture*)sMoonMap.get() : gSky.mVOSkyp->getMoonTex();
            if (tex && tex->hasGLTexture())
            {
                // Source: llvosky.cpp updateHeavenlyBodyGeometry - the quad's axes, its zenith fallback, its size
                const LLQuaternion rot = gSky.mVOSkyp->getMoon().getRotation();
                const LLVector3 to_dir = LLVector3::x_axis * rot;
                LLVector3 hb_right = to_dir % LLVector3::z_axis;
                LLVector3 hb_up = hb_right % to_dir;
                if ((to_dir * LLVector3::z_axis) > 0.99f)
                {
                    hb_right = LLVector3::y_axis_neg * rot;
                    hb_up = LLVector3::z_axis * rot;
                }
                hb_right.normalize();
                hb_up.normalize();
                const F32 enlarge = 1.f - to_dir.mV[VZ];
                // llsettingsvo.cpp:688 gSky.setMoonScale(getMoonScale()); llvosky.h HEAVENLY_BODY_FACTOR 0.1, llvosky.cpp
                // MOON_DISK_RADIUS = SUN_DISK_RADIUS (0.5) x 0.9
                const F32 half = eep->getMoonScale() * 0.1f * (0.5f * 0.9f);
                const LLVector2 halfRU((1.f + enlarge * 0.3f) * half, (1.f + enlarge * 0.2f) * half);
                // Source: lldrawpoolwlsky.cpp renderHeavenlyBodies (WolfViewer real moon): the sun in the quad's frame, lunar
                // north (the stock pass's pole at WolfRegionWeather::autoEnvSkyTime), the night factor
                LLVector3 sun_dir = eep->getSunDirection();
                sun_dir.normalize();
                const LLVector3 phase(sun_dir * hb_right, sun_dir * hb_up, -(sun_dir * to_dir));
                LLVector2 nd(0.f, 1.f);
                if (real)
                {
                    const LLVector3 pole = lunarNorthAt(WolfRegionWeather::instance().autoEnvSkyTime());
                    nd.set(pole * hb_right, pole * hb_up);
                    if (nd.length() < 1e-3f) nd.set(0.f, 1.f);
                    nd.normalize();
                }
                const F32 night = llclamp(-sun_dir.mV[VZ] / 0.17f, 0.f, 1.f);
                // moonF.glsl: the pre-EEP fade near the horizon, from moon_dir (the sky's moon direction)
                const LLVector3 md = eep->getMoonDirection();
                const F32 fade = md.mV[VZ] > 0.f ? llclamp(md.mV[VZ] * md.mV[VZ] * 4.f, 0.f, 1.f) : 1.f;
                const WolfEepReference::Path path = WolfEepReference::path(eep);
                static LLCachedControl<bool> has_emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
                const S32 loc = sh.getUniformLocation(LLStaticHashedString("wolfMoonMap"));
                if (loc >= 0)
                {
                    moonUnit = WolfTk::NAMED_UNIT_BASE + 1;
                    gGL.getTexUnit(moonUnit)->bind(tex);
                    glUniform1i(loc, moonUnit);
                    eepMoonOn = true;
                    sh.uniform1f(s_ereal, real ? 1.f : 0.f);
                    sh.uniform3fv(s_eto, 1, to_dir.mV);
                    sh.uniform3fv(s_eright, 1, hb_right.mV);
                    sh.uniform3fv(s_eup, 1, hb_up.mV);
                    sh.uniform2f(s_ehalf, halfRU.mV[0], halfRU.mV[1]);
                    sh.uniform3fv(s_ephase, 1, phase.mV);
                    sh.uniform3f(s_enorth, nd.mV[0], nd.mV[1], real ? night : 0.f);
                    sh.uniform1f(s_efade, fade);
                    sh.uniform1f(s_ebright, eep->getMoonBrightness());
                    sh.uniform1f(s_escale, path.skyHdrScale * (path.classic ? 1.1f : 1.f));
                    sh.uniform1f(s_eclamp, has_emissive() ? 0.f : 1.f);
                }
            }
        }
        sh.uniform1f(s_eon, eepMoonOn ? 1.f : 0.f);
    }
    sh.uniform1f(s_mon, (moonOn && !eepDriven()) ? 1.f : 0.f);

    // [WolfViewer 2026-10-13] the EEP's own sun (skyF.glsl uSunScaleD / wolfSunMap): its sun_scale sizes the disc, and a sun
    // texture of its own (not LL's default, the null id - llsettingssky.cpp:1767 GetDefaultSunTextureId - nor its blank sun,
    // :1773) is drawn across it. Source: llsettingssky.h getSunScale / getSunTextureId, llvosky.cpp:877-881 setSunTextures.
    S32 sunUnit = -1;
    {
        static LLStaticHashedString s_ssd("uSunScaleD"), s_smo("uSunMapOn");
        F32 sunScale = 1.f;
        LLViewerFetchedTexture* sunTex = nullptr;
        if (LLSettingsSky::ptr_t eep = LLEnvironment::instance().getCurrentSky())
        {
            const LLUUID sid = eep->getSunTextureId();
            // LL draws a sun disc only when the sky has a sun texture (lldrawpoolwlsky.cpp:433-436 "if and only if we have a
            // texture defined, render the sun disc"), so sun_scale only sizes anything then: Jimmy's day looks carry sun_scale
            // 20 with no sun texture (WolfAutoEnvironmentModule.cs:727) and show no disc in LL - here they keep the physical one
            if (sid.notNull()) sunScale = llclamp(eep->getSunScale(), 0.25f, 20.f);
            if (sid.notNull() && sid != LLSettingsSky::GetBlankSunTextureId())
            {
                LLViewerFetchedTexture* t = LLViewerTextureManager::getFetchedTexture(sid, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
                if (t && t->hasGLTexture()) sunTex = t;
            }
        }
        sh.uniform1f(s_ssd, sunScale - 1.f);
        const S32 loc = sh.getUniformLocation(LLStaticHashedString("wolfSunMap"));
        const bool sunMapOn = sunTex && loc >= 0;
        if (sunMapOn)
        {
            sunUnit = WolfTk::NAMED_UNIT_BASE + 2;
            gGL.getTexUnit(sunUnit)->bind(sunTex);
            glUniform1i(loc, sunUnit);
        }
        sh.uniform1f(s_smo, sunMapOn ? 1.f : 0.f);
    }

    // the extra bodies (decision D12): suns 2..4 at the first sun's disc luminance x their brightness; moons and
    // planets Lambert spheres of their colour as albedo under the first sun (E / pi); in the frame's units
    const F64 now = skyNow();
    F32 dirs[36] = {}, cols[36] = {}, sins[12] = {}, types[12] = {}, rings[36] = {};
    S32 n = 0;
    auto add = [&](const LLVector3& dir, const LLColor3& col, F32 radius, F32 type, const LLColor3& ring)
    {
        if (n >= 12) return;
        for (S32 k = 0; k < 3; ++k) { dirs[n * 3 + k] = dir.mV[k]; cols[n * 3 + k] = col.mV[k]; rings[n * 3 + k] = ring.mV[k]; }
        sins[n] = sinf(llclamp(radius, 1e-5f, 0.6f));
        types[n] = type;
        ++n;
    };
    // the sun above the atmosphere (illuminance, lux) and a disc's luminance L = E / omega
    const LLColor3 sunTop = sAtmos->sunIlluminanceAboveAtmosphere();
    const F32 sunOmega = (F32)(2.0 * F_PI * (1.0 - cos(WOLF_SUN_RADIUS)));
    const LLColor3 sunIrrTOA = sunTop * (e / F_PI);
    for (size_t i = 1; i < d.mSuns.size(); ++i)
    {
        const WolfEnvBody& b = d.mSuns[i];
        const LLColor3 c = (b.mHasColor ? lin(b.mColor) : LLColor3::white) * b.mBrightness;
        const F32 size = llmax(b.mSize, 1e-3f);
        add(bodyDir(b, now), componentMult(sunTop, c) * (e / (sunOmega * size * size)), (F32)WOLF_SUN_RADIUS * size, 0.f, LLColor3::black);
    }
    for (size_t i = 1; i < d.mMoons.size(); ++i)
    {
        const WolfEnvBody& b = d.mMoons[i];
        const LLColor3 alb = b.mHasColor ? lin(b.mColor) * 0.12f : LLColor3(0.12f, 0.12f, 0.12f);   // the Moon's Bond albedo
        add(bodyDir(b, now), componentMult(sunIrrTOA, alb) * b.mBrightness, (F32)WOLF_MOON_RADIUS * b.mSize, 1.f, LLColor3::black);
    }
    for (const WolfEnvBody& b : d.mPlanets)
    {
        add(bodyDir(b, now), componentMult(sunIrrTOA, lin(b.mColor)), (F32)WOLF_MOON_RADIUS * b.mSize, b.mRings ? 3.f : 2.f, componentMult(sunIrrTOA, lin(b.mRingColor)));
    }
    // [WolfViewer 2026-10-13] Earth mode: the real planets where they are now, above the horizon only (realPlanets). A planet is
    // a point; it is drawn as a sun-type disc of 0.0004 rad (a pixel or two) with L = E / omega, so its brightness against the
    // sky is its magnitude's and the day sky outshines it as it does the stars.
    if (WolfRegionWeather::instance().autoEnvRealSky())
    {
        const F32 PLANET_R = 0.0004f;
        const F32 omega = (F32)(2.0 * F_PI * (1.0 - cos((F64)PLANET_R)));
        for (const RealPlanet& p : realPlanets(now, WolfRegionWeather::instance().autoEnvLat(), WolfRegionWeather::instance().autoEnvLon()))
            add(p.dir, LLColor3::white * (p.lux * e / omega), PLANET_R, 0.f, LLColor3::black);
    }
    sh.uniform1i(s_bc, n);
    sh.uniform3fv(s_bd, 12, dirs);
    sh.uniform3fv(s_bcol, 12, cols);
    sh.uniform1fv(s_bs, 12, sins);
    sh.uniform1fv(s_bt, 12, types);
    sh.uniform3fv(s_br, 12, rings);
    sh.uniform3fv(s_lit, 1, sFrame.sunWorld.mV);
    // a nebula: an emission nebula's surface brightness, 3e-4 cd/m2 (21-22 mag/arcsec2, NPS's scale) at the
    // document's full strength
    sh.uniform1f(s_neb, d.mNebula * 3.0e-4f * e);
    const LLColor3 nc = lin(d.mNebulaColor);
    sh.uniform3fv(s_nebc, 1, nc.mV);
    sh.uniform1f(s_night, NIGHT_SKY_ZENITH_CD * e);
    // a probe face draws no sun disc (LL lights with the sun itself; in a probe it would be ambient a second time);
    // the sea mirrors the default reflection probe, as the world's water does - in a probe face too: a face is
    // rendered into LLReflectionMapManager::mRenderTarget with the water in its render-type mask
    // (llreflectionmapmanager.cpp:796-810 updateProbeFace) and copied into the cube array afterwards (:856-911), so
    // the water, and this sea, read the probe's previous content there, never the face being drawn. Without this the
    // probe's lower half mirrored the clear sky under an overcast, and the far water (LL's parallax-corrected lookup
    // lands below the probe's horizon) reflected that clear sky: brighter than the sky's ocean beyond the planes.
    static LLStaticHashedString s_disc("uDrawSunDisc"), s_seap("uSeaProbe");
    sh.uniform1f(s_disc, gCubeSnapshot ? 0.f : 1.f);
    const bool seaProbe = LLPipeline::sReflectionProbesEnabled && gPipeline.mReflectionMapManager.defaultProbeReady();
    if (seaProbe) gPipeline.bindReflectionProbes(sh);
    sh.uniform1f(s_seap, seaProbe ? 1.f : 0.f);
    // the sea's surface as waterF.glsl shades the world's water at its far clip (skyF.glsl wolfFarSeaSurface): the EEP
    // water's fresnel, as LLDrawPoolWater::renderPostDeferred gives the water (lldrawpoolwater.cpp WATER_FRESNEL_SCALE /
    // _OFFSET from the current water settings)
    {
        static LLStaticHashedString s_fs("fresnelScale"), s_fo("fresnelOffset");
        static LLStaticHashedString s_seaMss("wolfSeaMss");   // [WolfViewer round 11] the open sea's glint (skyF.glsl, seaGlint.glsl)
        sh.uniform1f(s_seaMss, seaMeanSquareSlope());
        const LLSettingsWater::ptr_t water = LLEnvironment::instance().getCurrentWater();
        sh.uniform1f(s_fs, water ? water->getFresnelScale() : 0.4f);
        sh.uniform1f(s_fo, water ? water->getFresnelOffset() : 0.5f);
    }
    static LLStaticHashedString s_mde("uMoonDirECEF"), s_mss("uMoonSkyScale");
    sh.uniform3fv(s_mde, 1, sFrame.moonECEF.mV);
    sh.uniform1f(s_mss, sFrame.moonSkyScale);
    {
        // [WolfViewer 2026-10-14] the shooting stars (updateMeteors, skyF.glsl wolfMeteors); none in a reflection probe's faces
        static LLStaticHashedString s_ma("uMeteorA"), s_mb("uMeteorB"), s_mc("uMeteorC");
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (!gCubeSnapshot) updateMeteors(now, sFrame.sunWorld);
        F32 a[8] = {}, b[8] = {}, c[8] = {};
        for (S32 i = 0; i < 2; ++i)
        {
            const Meteor& m = sMeteors[i];
            if (gCubeSnapshot || m.peak <= 0.f) continue;
            a[i * 4 + 0] = m.start.mV[VX]; a[i * 4 + 1] = m.start.mV[VY]; a[i * 4 + 2] = m.start.mV[VZ]; a[i * 4 + 3] = m.arc;
            b[i * 4 + 0] = m.axis.mV[VX];  b[i * 4 + 1] = m.axis.mV[VY];  b[i * 4 + 2] = m.axis.mV[VZ];
            b[i * 4 + 3] = (F32)((now - m.born) / m.life);
            c[i * 4 + 0] = m.peak; c[i * 4 + 1] = m.tail;
        }
        sh.uniform4fv(s_ma, 2, a);
        sh.uniform4fv(s_mb, 2, b);
        sh.uniform4fv(s_mc, 2, c);
    }
    WolfAurora::bindUniforms(&sh);
    if (eepDriven() && !WolfAurora::forced())   // [2026-10-15] WolfForceAurora: shown under an EEP sky too
    {
        // [WolfViewer 2026-10-13] an EEP sky has no aurora - the stock renderer draws none - and the dome is measured against
        // the stock sky only at its 36 probes, so an aurora between them showed as a blue-green band the EEP does not have
        // (Iceland at dusk: 30,53,79 on screen against the EEP's 39,42,52)
        static LLStaticHashedString s_aur("wolf_aurora");
        sh.uniform1f(s_aur, 0.f);
    }
    sh.uniform1f(s_exp, e);
    sh.uniform1f(s_sea, WolfAltitudeSky::horizonSeaHeight());

    // [WolfViewer 2026-10-14] WolfEnvironmentTrace: the samples the dome's draw passes (an occlusion query, read a frame later
    // without waiting) and the GL state and depth it draws under, one line every 10 s in the main view - how the magenta sky
    // (the dome beyond the far plane, wolfEnvSkyV.glsl) was found; a dome that draws nothing shows here as 0 samples.
    static LLCachedControl<bool> domeTrace(gSavedSettings, "WolfEnvironmentTrace", false);
    static GLuint s_domeQuery = 0;
    static bool s_domeQueryPending = false;
    static F64 s_domeNext = 0.0;
    const F64 domeNow = LLFrameTimer::getElapsedSeconds();
    if (!s_domeQuery) glGenQueries(1, &s_domeQuery);
    if (s_domeQueryPending)
    {
        GLuint ready = 0;
        glGetQueryObjectuiv(s_domeQuery, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready)
        {
            GLuint samples = 0;
            glGetQueryObjectuiv(s_domeQuery, GL_QUERY_RESULT, &samples);
            s_domeQueryPending = false;
            LL_INFOS("WolfEnv") << "sky dome drew " << samples << " samples" << LL_ENDL;
        }
    }
    const bool domeMeasure = domeTrace && !gCubeSnapshot && !s_domeQueryPending && domeNow >= s_domeNext;
    if (domeMeasure)
    {
        s_domeNext = domeNow + 10.0;
        GLint fbo = 0, depthFunc = 0, vp[4] = {}, drawBuf0 = 0, drawBuf1 = 0;
        GLboolean mask[4] = {}, depthMask = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_DRAW_BUFFER0, &drawBuf0);
        glGetIntegerv(GL_DRAW_BUFFER1, &drawBuf1);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        const U32 gfbo = gPipeline.mRT ? gPipeline.mRT->deferredScreen.getFBO() : 0;
        LL_INFOS("WolfEnv") << "sky dome state: fbo " << fbo << " (G-buffer " << gfbo << "), viewport " << vp[0] << "," << vp[1] << " "
                            << vp[2] << "x" << vp[3] << ", colour mask " << (int)mask[0] << (int)mask[1] << (int)mask[2] << (int)mask[3]
                            << ", draw buffers 0x" << std::hex << drawBuf0 << " 0x" << drawBuf1 << std::dec
                            << ", depth test " << (int)glIsEnabled(GL_DEPTH_TEST) << " func 0x" << std::hex << depthFunc << std::dec
                            << " write " << (int)depthMask << ", cull " << (int)glIsEnabled(GL_CULL_FACE) << ", stencil "
                            << (int)glIsEnabled(GL_STENCIL_TEST) << ", scissor " << (int)glIsEnabled(GL_SCISSOR_TEST)
                            << ", blend " << (int)glIsEnabled(GL_BLEND) << ", depth clamp " << (int)glIsEnabled(GL_DEPTH_CLAMP) << ", far clip " << LLViewerCamera::getInstance()->getFar()
                            << ", dome radius " << (LLEnvironment::instance().getCurrentSky() ? LLEnvironment::instance().getCurrentSky()->getDomeRadius() : -1.f)
                            << LL_ENDL;
        // the depth already in the G-buffer at a sky pixel (top centre) and the projection's own near / far (OpenGL
        // perspective: P[10] = -(f + n) / (f - n), P[14] = -2 f n / (f - n)), and where the dome straight ahead lands
        GLfloat d = -1.f;
        glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] - 4, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
        const F32 p10 = gGLProjection[10], p14 = gGLProjection[14];
        const F32 pn = p14 / (p10 - 1.f), pf = p14 / (p10 + 1.f);
        LL_INFOS("WolfEnv") << "sky dome depth: buffer " << d << " at the top centre; projection near " << pn << " far " << pf
                            << " (P00 " << gGLProjection[0] << " P11 " << gGLProjection[5] << "), the dome at "
                            << (LLEnvironment::instance().getCurrentSky() ? LLEnvironment::instance().getCurrentSky()->getDomeRadius() * 0.333f : -1.f)
                            << " m" << LL_ENDL;
        glBeginQuery(GL_SAMPLES_PASSED, s_domeQuery);
    }
    {
        // [WolfViewer 2026-10-14] the sky as the pipeline's screen triangle (wolfEnvSkyV.glsl: why not LL's dome mesh), each
        // pixel's direction from the camera: the inverse of the projection x the view's rotation (no translation - a
        // direction), clip -> world (x east, y north, z up). gGLModelView / gGLProjection are the matrices this pass draws
        // with (a reflection probe's face, a snapshot tile's included).
        static LLStaticHashedString s_s2w("uScreenToWorldDir");
        glm::mat4 rot = glm::make_mat4(gGLModelView);
        rot[3] = glm::vec4(0.f, 0.f, 0.f, 1.f);
        const glm::mat4 s2w = glm::inverse(glm::make_mat4(gGLProjection) * rot);
        sh.uniformMatrix4fv(s_s2w, 1, GL_FALSE, glm::value_ptr(s2w));
        // [WolfViewer 2026-10-14] the magenta sky (Paul, 8 hours): the dome's draw passes 0 samples from ~20 s after login
        // (and after every shader reload) with every logged GL state right, while LL's own screen-triangle passes keep
        // working. Two things LL caches rather than reads back are made true here before the draw, and a mismatch is
        // logged: the program (LLGLSLShader::bind skips glUseProgram when its cache says this program is current) and the
        // screen triangle's attribute pointers (LLVertexBuffer::setBuffer skips setupVertexBuffer when its cache says this
        // buffer is bound). Source: llglslshader.cpp bind() sCurBoundShader; llvertexbuffer.cpp setBuffer() sGLRenderBuffer.
        {
            GLint cur = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
            if ((GLuint)cur != sh.mProgramObject)
            {
                static F64 s_warnNext = 0.0;
                if (domeNow >= s_warnNext)
                {
                    s_warnNext = domeNow + 1.0;
                    LL_WARNS("WolfEnv") << "sky dome: GL had program " << cur << " bound, not the dome's " << sh.mProgramObject
                                        << " (LL cache " << LLGLSLShader::sCurBoundShader << "); rebound" << LL_ENDL;
                }
                glUseProgram(sh.mProgramObject);
            }
        }
        LLVertexBuffer::unbind();   // the attribute pointers are set again for this buffer, whatever LL's cache says
        gPipeline.mScreenTriangleVB->setBuffer();
        if (domeMeasure)
        {
            // drain the error queue so the error read after the draw is the draw's own
            S32 n = 0;
            for (GLenum e = glGetError(); e != GL_NO_ERROR && n < 8; e = glGetError(), ++n)
            {
                LL_INFOS("WolfEnv") << "sky dome: GL error 0x" << std::hex << e << std::dec << " pending from earlier in the frame" << LL_ENDL;
            }
        }
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        if (glMsg)
        {
            // every sampler the program uses, the unit it reads and what is bound there for its own target: the driver's
            // "program texture usage" error names no sampler, this list shows which two share a unit
            GLint n = 0, prevActive = 0;
            glGetProgramiv(sh.mProgramObject, GL_ACTIVE_UNIFORMS, &n);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
            std::ostringstream o;
            o << "sky pass samplers (name type unit -> bound texture):";
            for (GLint i = 0; i < n; ++i)
            {
                char name[256] = {}; GLsizei len = 0; GLint size = 0; GLenum type = 0;
                glGetActiveUniform(sh.mProgramObject, i, sizeof(name), &len, &size, &type, name);
                GLenum binding = 0; const char* tn = nullptr;
                switch (type)
                {
                case GL_SAMPLER_2D: binding = GL_TEXTURE_BINDING_2D; tn = "2D"; break;
                case GL_SAMPLER_3D: binding = GL_TEXTURE_BINDING_3D; tn = "3D"; break;
                case GL_SAMPLER_CUBE: binding = GL_TEXTURE_BINDING_CUBE_MAP; tn = "CUBE"; break;
                case GL_SAMPLER_2D_ARRAY: binding = GL_TEXTURE_BINDING_2D_ARRAY; tn = "2DARRAY"; break;
                case GL_SAMPLER_CUBE_MAP_ARRAY: binding = GL_TEXTURE_BINDING_CUBE_MAP_ARRAY; tn = "CUBEARRAY"; break;
                case GL_SAMPLER_2D_SHADOW: binding = GL_TEXTURE_BINDING_2D; tn = "2DSHADOW"; break;
                case GL_SAMPLER_2D_ARRAY_SHADOW: binding = GL_TEXTURE_BINDING_2D_ARRAY; tn = "2DARRAYSHADOW"; break;
                case GL_SAMPLER_2D_RECT: binding = GL_TEXTURE_BINDING_RECTANGLE; tn = "RECT"; break;
                default: break;
                }
                if (!tn) continue;
                const GLint loc = glGetUniformLocation(sh.mProgramObject, name);
                GLint unit = -1;
                if (loc >= 0) glGetUniformiv(sh.mProgramObject, loc, &unit);
                GLint tex = -1;
                if (unit >= 0 && unit < 32)
                {
                    glActiveTexture(GL_TEXTURE0 + unit);
                    glGetIntegerv(binding, &tex);
                }
                o << "\n  " << name << " " << tn << " unit " << unit << " -> " << tex;
            }
            glActiveTexture(prevActive);
            LL_INFOS("WolfEnv") << o.str() << LL_ENDL;
        }
        if (domeMeasure)
        {
            const GLenum err = glGetError();
            // what the draw ran under: the program's validation, attribute 0 (position) and its buffer, the states the first
            // state line left out, and the G-buffer's colour at the top centre right after the draw (magenta = LL's clear
            // colour, llviewerdisplay.cpp; the sky not written)
            glValidateProgram(sh.mProgramObject);
            GLint valid = 0, logLen = 0;
            glGetProgramiv(sh.mProgramObject, GL_VALIDATE_STATUS, &valid);
            glGetProgramiv(sh.mProgramObject, GL_INFO_LOG_LENGTH, &logLen);
            std::string vlog;
            if (logLen > 1)
            {
                std::vector<char> buf(logLen + 1, 0);
                glGetProgramInfoLog(sh.mProgramObject, logLen, nullptr, buf.data());
                vlog = buf.data();
                while (!vlog.empty() && (vlog.back() == '\n' || vlog.back() == '\r' || vlog.back() == ' ')) vlog.pop_back();
            }
            GLint aEnabled = 0, aBuffer = 0, aSize = 0, aStride = 0, aType = 0, arrayBuf = 0, curProg = 0, polyFill = 0, drawBuf2 = 0, drawBuf3 = 0, readFbo = 0;
            GLfloat depthRange[2] = { -1.f, -1.f }, polyFactor = 0.f, polyUnits = 0.f;
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &aEnabled);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &aBuffer);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &aSize);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &aStride);
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &aType);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
            glGetIntegerv(GL_CURRENT_PROGRAM, &curProg);
            glGetIntegerv(GL_DRAW_BUFFER2, &drawBuf2);
            glGetIntegerv(GL_DRAW_BUFFER3, &drawBuf3);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
            glGetFloatv(GL_DEPTH_RANGE, depthRange);
            polyFill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
            glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &polyFactor);
            glGetFloatv(GL_POLYGON_OFFSET_UNITS, &polyUnits);
            GLint vp[4] = { 0, 0, 1, 1 };
            glGetIntegerv(GL_VIEWPORT, vp);
            GLfloat px[4] = { -1.f, -1.f, -1.f, -1.f };
            GLint prevReadBuf = 0;
            glGetIntegerv(GL_READ_BUFFER, &prevReadBuf);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] - 4, 1, 1, GL_RGBA, GL_FLOAT, px);
            glReadBuffer(prevReadBuf);
            const GLenum err2 = glGetError();
            LL_INFOS("WolfEnv") << "sky dome draw: GL error after draw 0x" << std::hex << err << std::dec
                                << ", program " << sh.mProgramObject << " current " << curProg << " (LL cache " << LLGLSLShader::sCurBoundShader << ")"
                                << ", validate " << valid << (vlog.empty() ? "" : " [" + vlog + "]")
                                << ", attrib 0 enabled " << aEnabled << " buffer " << aBuffer << " size " << aSize << " stride " << aStride << " type 0x" << std::hex << aType << std::dec
                                << ", array buffer " << arrayBuf << " (LL cache " << LLVertexBuffer::sGLRenderBuffer << ")"
                                << ", attribute mask 0x" << std::hex << sh.mAttributeMask << std::dec
                                << ", depth range " << depthRange[0] << ".." << depthRange[1]
                                << ", polygon offset " << polyFill << " (" << polyFactor << ", " << polyUnits << ")"
                                << ", draw buffers 2/3 0x" << std::hex << drawBuf2 << " 0x" << drawBuf3 << std::dec
                                << ", read fbo " << readFbo
                                << ", colour at the top centre after the draw " << px[0] << "," << px[1] << "," << px[2] << "," << px[3]
                                << " (GL error after the read 0x" << std::hex << err2 << std::dec << ")" << LL_ENDL;
        }
    }
    if (domeMeasure)
    {
        glEndQuery(GL_SAMPLES_PASSED);
        s_domeQueryPending = true;
    }
    probeSky(sh);   // [WolfViewer 2026-10-13] the EEP drives the sky: measure this dome against the stock one
    if (seaProbe) gPipeline.unbindReflectionProbes(sh);
    if (moonUnit >= 0) gGL.getTexUnit(moonUnit)->unbind(LLTexUnit::TT_TEXTURE);
    if (sunUnit >= 0) gGL.getTexUnit(sunUnit)->unbind(LLTexUnit::TT_TEXTURE);
    WolfTk::unbindUnit(shaftUnit, GL_TEXTURE_2D);
    sh.unbind();
    timerStamp(SEC_SKY, 1);
#if !LL_DARWIN
    if (glMsg)
    {
        glDisable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDisable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(nullptr, nullptr);
        LL_INFOS("WolfEnv") << "sky pass: GL debug output off" << LL_ENDL;
    }
#endif
    return true;
}

// static
void WolfEnvironmentRender::postPass()
{
    static LLCachedControl<bool> capture(gSavedSettings, "WolfEnvironmentDebugCapture", false);
    LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
    const bool cap = capture && !gCubeSnapshot && rt;
    if (cap)
    {
        LL_INFOS("WolfEnvDebug") << "capture: tier " << tier() << ", sky " << (skyActive() ? "on" : "off") << ", atmosphere ready "
                                 << (sAtmos && sAtmos->ready()) << ", failed " << sGpu.failed << ", camera height " << sFrame.cameraHeight << " m" << LL_ENDL;
        LL_INFOS("WolfEnvDebug") << "light: sun world " << sFrame.sunWorld << " up " << sFrame.sunUp << " normal " << sFrame.sunNormal
                                 << " lux, horizontal " << sFrame.sunHorizontal << ", sky " << sFrame.skyHorizontal << " lux; moon " << sFrame.moonWorld
                                 << " phase angle " << sFrame.moonPhaseAngle << " TOA " << sFrame.moonTOA << " lux, at camera " << sFrame.moonNormal << LL_ENDL;
        LL_INFOS("WolfEnvDebug") << "meter: " << sFrame.meterLux << " lux (cloud transmittance at the camera " << sFrame.cloudTransmittanceAtCamera
                                 << "), EV100 target " << sMeter.targetEv100() << " adapted " << sMeter.ev100() << ", exposure " << sFrame.exposure
                                 << "; cover " << sFrame.cover << " haze x" << sFrame.hazeScale << "; scene sunlit " << sceneSunlit()
                                 << " amblit " << sceneAmblit() << " (pre-exposed)" << LL_ENDL;
        captureTarget("screen_before", rt->screen, true);
        captureTarget("gbuffer", rt->deferredScreen, true);
    }
    postPassInner();
    if (cap)
    {
        captureTarget("screen_after", rt->screen, true);
        captureTarget("composite", sGpu.layer, true);
        if (sAtmos && sAtmos->ready())
        {
            captureTexture("table_transmittance", sAtmos->transmittanceTexture(), WolfTk::TRANSMITTANCE_TEXTURE_WIDTH, WolfTk::TRANSMITTANCE_TEXTURE_HEIGHT, false);
            captureTexture("table_irradiance", sAtmos->irradianceTexture(), WolfTk::IRRADIANCE_TEXTURE_WIDTH, WolfTk::IRRADIANCE_TEXTURE_HEIGHT, false);
            captureTexture("table_scattering_layer16", sAtmos->scatteringTexture(), WolfTk::SCATTERING_TEXTURE_WIDTH, WolfTk::SCATTERING_TEXTURE_HEIGHT, false, GL_TEXTURE_3D, 16);
        }
        if (sClouds && sClouds->colorTex())
        {
            captureTexture("clouds", sClouds->colorTex(), rt->screen.getWidth(), rt->screen.getHeight(), true);
            if (sClouds->shadowLengthTex()) captureTexture("clouds_shadow_length", sClouds->shadowLengthTex(), rt->screen.getWidth(), rt->screen.getHeight(), false);
            const LLVector2 ms = sClouds->shadowMapSize();
            for (S32 c = 0; c < sClouds->cascadeCount(); ++c)
            {
                captureTexture(llformat("clouds_bsm_%d", c), sClouds->shadowMapArray(), (U32)ms.mV[0], (U32)ms.mV[1], false, GL_TEXTURE_2D_ARRAY, c);
            }
        }
        captureTarget("exposure_map_last_frame", gPipeline.mExposureMap, false);
        LL_INFOS("WolfEnvDebug") << "capture written to " << gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "wolfenv_*.png") << LL_ENDL;
        // one frame; the finished frame (water and transparent geometry drawn over the composite) is captured in
        // captureFinal, before the tone mapping, which then turns the setting off
        sGpu.captureFinalPending = true;
    }
}

// static
void WolfEnvironmentRender::postPassInner()
{
    if (!skyActive()) return;
    LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
    if (!rt || !rt->screen.isComplete()) return;
    // under water the air's aerial perspective and the clouds are not what the camera sees through: LL's own
    // underwater fog is the medium (the scene shaders skip it too, wolfTkUnderwater); the meter keeps last reading
    if (LLViewerCamera::getInstance()->cameraUnderWater()) return;
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    const U32 W = rt->screen.getWidth(), H = rt->screen.getHeight();
    // [WolfViewer 2026-10-13] with an EEP driving the sky the pass also carries the EEP's haze, so it runs at any cover
    const bool clouds = tier() >= 2 && (sFrame.cover > 0.f || eepDriven());

    LLGLDepthTest depthOff(GL_FALSE, GL_FALSE);
    LLGLDisable cullOff(GL_CULL_FACE);
    LLGLDisable blendOff(GL_BLEND);
    timerStamp(SEC_POST, 0);

    // ── the clouds: shadow maps, the march at 1/16 of the pixels, the temporal resolves (three-geospatial
    // CloudsEffect.update, CloudsEffect.ts:454-488); in a probe face without temporal upscaling ──
    bool haveClouds = false;
    if (clouds)
    {
        timerStamp(SEC_CLOUDS, 0);
        const WolfTk::CloudsFrame f = cloudsFrame(d, W, H, gCubeSnapshot);
        haveClouds = gCubeSnapshot ? sClouds->renderProbe(*sAtmos, f) : sClouds->render(*sAtmos, f);
        if (haveClouds && !gCubeSnapshot) measureCloudCover(f);   // [WolfViewer 2026-10-13] the EEP's cover, measured
        timerStamp(SEC_CLOUDS, 1);
    }
    const GLuint overlay = haveClouds ? (gCubeSnapshot ? sClouds->probeColorTex() : sClouds->colorTex()) : 0;
    const GLuint shafts = haveClouds ? (gCubeSnapshot ? sClouds->probeShadowLengthTex() : sClouds->shadowLengthTex()) : 0;

    // ── the aerial perspective over the opaque frame, the clouds laid over it (three-geospatial
    // AerialPerspectiveEffect, light-source lighting: TRANSMITTANCE + INSCATTER, overlay, shadowLength) ──
    // The frame is the effect's input and its output, so the result goes to its own target and is copied back
    // (colour only: the frame's ALPHA is the glow mask, glowExtractF.glsl).
    LLRenderTarget& layer = gCubeSnapshot ? sGpu.probeLayer : sGpu.layer;
    if (!allocate(layer, W, H))
    {
        fail("the composite target could not be allocated");
        timerStamp(SEC_POST, 1);
        return;
    }
    LLGLSLShader& sh = gWolfEnvCompositeProgram[overlay ? 1 : 0];
    layer.bindTarget();
    sh.bind();
    sAtmos->setRuntimeUniforms(sh, sFrame.exposure);
    setFrameUniforms(sh, true);
    {
        // [WolfViewer 2026-10-13] the overlay carries the stock EEP haze over the opaque scene (cloudsF.glsl wolfLLHaze): no
        // physical aerial perspective there as well
        static LLStaticHashedString s_noAerial("wolfEepNoAerial");
        sh.uniform1f(s_noAerial, (overlay && eepDriven()) ? 1.f : 0.f);
    }
    {
        const glm::mat4 view = glm::make_mat4(gGLModelView), proj = glm::make_mat4(gGLProjection);
        const glm::mat4 iview = glm::inverse(view), iproj = glm::inverse(proj);
        static LLStaticHashedString s_pm("projectionMatrix"), s_vm("viewMatrix"), s_ipm("inverseProjectionMatrix"), s_ivm("inverseViewMatrix");
        static LLStaticHashedString s_near("cameraNear"), s_far("cameraFar"), s_res("resolution"), s_geca("geometricErrorCorrectionAmount"), s_alb("albedoScale");
        sh.uniformMatrix4fv(s_pm, 1, GL_FALSE, glm::value_ptr(proj));
        sh.uniformMatrix4fv(s_vm, 1, GL_FALSE, glm::value_ptr(view));
        sh.uniformMatrix4fv(s_ipm, 1, GL_FALSE, glm::value_ptr(iproj));
        sh.uniformMatrix4fv(s_ivm, 1, GL_FALSE, glm::value_ptr(iview));
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        sh.uniform1f(s_near, cam->getNear());
        sh.uniform1f(s_far, cam->getFar());
        sh.uniform2f(s_res, (F32)W, (F32)H);
        sh.uniform1f(s_geca, 0.f);   // CORRECT_GEOMETRIC_ERROR is off: a sphere has no geometric error to correct
        sh.uniform1f(s_alb, 1.f);    // Source: AerialPerspectiveEffect.ts:161 albedoScale 1
    }
    const S32 u = WolfTk::NAMED_UNIT_BASE;
    WolfTk::bindNamed(sh, "inputBuffer", u, GL_TEXTURE_2D, rt->screen.getTexture());
    WolfTk::bindNamed(sh, "depthBuffer", u + 1, GL_TEXTURE_2D, rt->deferredScreen.getDepth());
    if (overlay)
    {
        WolfTk::bindNamed(sh, "overlayBuffer", u + 2, GL_TEXTURE_2D, overlay);
        WolfTk::bindNamed(sh, "shadowLengthBuffer", u + 3, GL_TEXTURE_2D, shafts ? shafts : zeroTex());
    }
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    for (S32 k = 0; k < 4; ++k) WolfTk::unbindUnit(u + k, GL_TEXTURE_2D);
    sh.unbind();
    layer.flush();

    rt->screen.bindTarget();
    {
        gGL.setColorMask(true, false);   // colour only: alpha is the glow mask
        gCopyProgram.bind();
        gGL.getTexUnit(0)->bind(&layer);
        gPipeline.mScreenTriangleVB->setBuffer();
        gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gCopyProgram.unbind();
        gGL.setColorMask(true, true);
    }
    rt->screen.flush();

    // ── the light meter's cloud shadow: the clouds' transmittance at the camera, from the same shadow map the
    // scene's sun term uses (wolfTkCloudShadow, wolftk/atmosphereLibF.glsl), read back once a frame (1 x 1) ──
    if (!gCubeSnapshot)
    {
        if (haveClouds && allocate(sGpu.camShadow, 1, 1, GL_RGBA32F))
        {
            // last frame's reading first (its copy has had a frame to land), then this frame's copy queued: the
            // CPU never waits for the GPU here
            if (!sGpu.camPbo[0]) glGenBuffers(2, sGpu.camPbo);
            const U32 cur = sGpu.camPboIndex, prev = 1 - cur;
            if (sGpu.camPboPending[prev])
            {
                F32 px[4] = { 1.f, 1.f, 0.f, 1.f };   // (direct T, diffuse T, diffused beam): clear sky
                glBindBuffer(GL_PIXEL_PACK_BUFFER, sGpu.camPbo[prev]);
                glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(px), px);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                sGpu.camPboPending[prev] = false;
                if (std::isfinite(px[0]) && std::isfinite(px[1]) && std::isfinite(px[2]))
                {
                    sNext.cloudTransmittanceAtCamera = llclamp(px[0], 0.f, 1.f);
                    sNext.cloudDiffuseT = llclamp(px[1], 0.f, 1.f);
                    sNext.cloudDiffusedBeam = llclamp(px[2], 0.f, 1.f);
                }
            }
            sGpu.camShadow.bindTarget();
            LLGLSLShader& cs = gWolfEnvCameraShadowProgram;
            cs.bind();
            bindSceneUniforms(cs);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            cs.unbind();
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sGpu.camPbo[cur]);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(F32), nullptr, GL_STREAM_READ);
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            sGpu.camPboPending[cur] = true;
            sGpu.camPboIndex = prev;
            sGpu.camShadow.flush();
        }
        else
        {
            sNext.cloudTransmittanceAtCamera = 1.f;
            sNext.cloudDiffuseT = 1.f;
            sNext.cloudDiffusedBeam = 0.f;
            sGpu.camPboPending[0] = sGpu.camPboPending[1] = false;
        }

        // ── the light meter's reflected reading: the composited frame's log luminance (Reinhard 2002 eq. 1) over a
        // 64 x 64 target whose last mip is the mean, read back a frame later as the cloud shadow is; the reader
        // divides by the pre-exposure of the frame it came from (wolfexposure.h) ──
        if (gWolfEnvLogLumProgram.isComplete() && sGpu.logLum.isComplete() == false)
        {
            sGpu.logLum.allocate(64, 64, GL_RGBA16F, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_AUTO);
        }
        if (gWolfEnvLogLumProgram.isComplete() && sGpu.logLum.isComplete())
        {
            if (!sGpu.lumPbo[0]) glGenBuffers(2, sGpu.lumPbo);
            const U32 cur = sGpu.lumPboIndex, prev = 1 - cur;
            if (sGpu.lumPboPending[prev])
            {
                F32 v[4] = { 0.f, 0.f, 0.f, 0.f };   // (mean log(delta + L), mean L x sky, mean L, 1)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, sGpu.lumPbo[prev]);
                glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, sizeof(v), v);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                sGpu.lumPboPending[prev] = false;
                const F32 e = sGpu.lumPboExposure[prev];
                sNext.frameLogAvg = (std::isfinite(v[0]) && e > 0.f) ? expf(v[0]) / e : -1.f;
                sNext.skyLuminanceShare = (std::isfinite(v[1]) && std::isfinite(v[2]) && v[2] > 0.f) ? llclamp(v[1] / v[2], 0.f, 1.f) : 1.f;
            }
            sGpu.logLum.bindTarget();
            LLGLSLShader& ll = gWolfEnvLogLumProgram;
            ll.bind();
            WolfTk::bindNamed(ll, "frameTex", WolfTk::NAMED_UNIT_BASE, GL_TEXTURE_2D, rt->screen.getTexture());
            // LL's G-buffer normal + flags: deferredScreen texture 2 (LLPipeline::bindDeferredShader DEFERRED_NORMAL)
            WolfTk::bindNamed(ll, "normalMap", WolfTk::NAMED_UNIT_BASE + 1, GL_TEXTURE_2D, rt->deferredScreen.getTexture(2));
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            WolfTk::unbindUnit(WolfTk::NAMED_UNIT_BASE, GL_TEXTURE_2D);
            WolfTk::unbindUnit(WolfTk::NAMED_UNIT_BASE + 1, GL_TEXTURE_2D);
            ll.unbind();
            sGpu.logLum.flush();   // TMG_AUTO: the mipmaps are generated here (llrendertarget.cpp:511-515)
            // the 1 x 1 mip (level 6 of 64) through a pixel-pack buffer: no wait for the GPU
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, sGpu.logLum.getTexture());
            glBindBuffer(GL_PIXEL_PACK_BUFFER, sGpu.lumPbo[cur]);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4 * sizeof(F32), nullptr, GL_STREAM_READ);
            glGetTexImage(GL_TEXTURE_2D, 6, GL_RGBA, GL_FLOAT, nullptr);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            sGpu.lumPboPending[cur] = true;
            sGpu.lumPboExposure[cur] = sFrame.exposure;
            sGpu.lumPboIndex = prev;
        }
        else
        {
            sNext.frameLogAvg = -1.f;
            sNext.skyLuminanceShare = 1.f;
            sGpu.lumPboPending[0] = sGpu.lumPboPending[1] = false;
        }
    }
    timerStamp(SEC_POST, 1);
}

// static
F32 WolfEnvironmentRender::toneContrast()
{
    // Blender's "AgX - Medium High Contrast" look magnitude (config.ocio: contrast 1.2, saturation 1.0), applied as
    // Hable's log contrast before the curve (wolftk/filamentAgx.glsl wolfToneLook; round 6, BEAUTY_RESEARCH.md 4.5 item 3)
    return 1.2f;
}

// static
F32 WolfEnvironmentRender::toneSaturation()
{
    return 1.0f;
}

// static
F32 WolfEnvironmentRender::tonePivot()
{
    return sMeter.tonePivot();
}

// static
void WolfEnvironmentRender::captureFinal()
{
    if (!sGpu.captureFinalPending) return;
    sGpu.captureFinalPending = false;
    LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
    if (rt)
    {
        // the HDR frame with the water and the alpha pass over the composite (pre-exposed; the tone mapping follows)
        captureTarget("screen_final", rt->screen, true);
        LL_INFOS("WolfEnvDebug") << "final frame captured: far clip " << LLViewerCamera::getInstance()->getFar() << " m, camera height above the sea "
                                 << sFrame.cameraHeight << " m, sea height for the dome " << WolfAltitudeSky::horizonSeaHeight() << LL_ENDL;
    }
    gSavedSettings.setBOOL("WolfEnvironmentDebugCapture", false);
}

// static
void WolfEnvironmentRender::bindSceneUniforms(LLGLSLShader& shader)
{
    static LLStaticHashedString s_e2w("wolfTkEyeToWorld"), s_on("wolfTkShadowOn"), s_uw("wolfTkUnderwater");
    if (shader.getUniformLocation(s_e2w) < 0) return;   // no wolftk/atmosphereLibF.glsl unit in this program
    if (!skyActive()) return;
    const bool on = sScenePreview == 0 && tier() >= 2 && sFrame.cover > 0.f && sClouds->shadowMapArray() != 0;
    // the uniforms persist in the program: a program already set up this frame for these matrices (the alpha
    // pass binds the same few programs many times) only gets its textures bound again (other draws use the units)
    // the key: the frame, the matrices, the clouds' shadow map in use (Clouds::render swaps it and refits its cascades
    // mid-frame) and the atmosphere's tables (a build can finish mid-frame)
    if (sSceneUniformCache.size() > 256) sSceneUniformCache.clear();   // programs come and go with shader reloads
    SceneUniformCache& c = sSceneUniformCache[shader.mProgramObject];
    const U32 frameNo = LLFrameTimer::getFrameCount();
    const GLuint shadowTex = on ? sClouds->shadowMapArray() : 0;
    const GLuint tables = sAtmos->scatteringTexture();
    if (c.frame == frameNo && c.on == on && c.shadowTex == shadowTex && c.tables == tables
        && !memcmp(c.mv, gGLModelView, sizeof(c.mv)) && !memcmp(c.pj, gGLProjection, sizeof(c.pj)))
    {
        sAtmos->setRuntimeUniforms(shader, sFrame.exposure, false);
        if (on) WolfTk::bindReserved(shader, LLShaderMgr::WOLF_TK_SHADOW_BUFFER, GL_TEXTURE_2D_ARRAY, sClouds->shadowMapArray());
        return;
    }
    c.frame = frameNo; c.on = on; c.shadowTex = shadowTex; c.tables = tables;
    memcpy(c.mv, gGLModelView, sizeof(c.mv));
    memcpy(c.pj, gGLProjection, sizeof(c.pj));
    sAtmos->setRuntimeUniforms(shader, sFrame.exposure);
    setFrameUniforms(shader, false);
    // [WolfViewer round 8] the direction the clouds' shadow map is cast along (wolfTkCloudSkyLight's slant -> vertical)
    static LLStaticHashedString s_cld("wolfTkCloudLightDirection");
    shader.uniform3fv(s_cld, 1, sFrame.cloudLightECEF.mV);
    // [WolfViewer round 11] the sea's Cox-Munk slopes from the wind (waterF.glsl wolfSeaGlint, wolftk/seaGlint.glsl)
    static LLStaticHashedString s_mss("wolfSeaMss");
    shader.uniform1f(s_mss, seaMeanSquareSlope());
    // [WolfViewer 2026-10-13] beyond the clouds' shadow map the ground gets the cloud field's mean direct-beam
    // transmittance instead of full sun (wolftk/atmosphereLibF.glsl wolfTkCloudShadow): for opaque clouds the chance of a
    // clear line to the sun is the clear-sky fraction, 1 - cloud fraction (WMO cloud fraction = the sky's covered part)
    static LLStaticHashedString s_fart("wolfTkFarCloudT");
    shader.uniform1f(s_fart, llclamp(1.f - sFrame.cover, 0.f, 1.f));
    shader.uniform1f(s_uw, LLViewerCamera::getInstance()->cameraUnderWater() ? 1.f : 0.f);
    // LL's eye space (the scene shaders' positions) -> world: the inverse of the scene's modelview
    const glm::mat4 view = glm::make_mat4(gGLModelView), proj = glm::make_mat4(gGLProjection);
    const glm::mat4 iview = glm::inverse(view);
    shader.uniformMatrix4fv(s_e2w, 1, GL_FALSE, glm::value_ptr(iview));
    static LLStaticHashedString s_pm("projectionMatrix"), s_vm("viewMatrix"), s_near("cameraNear"), s_res("resolution");
    shader.uniformMatrix4fv(s_pm, 1, GL_FALSE, glm::value_ptr(proj));
    shader.uniformMatrix4fv(s_vm, 1, GL_FALSE, glm::value_ptr(view));
    shader.uniform1f(s_near, LLViewerCamera::getInstance()->getNear());
    if (gPipeline.mRT) shader.uniform2f(s_res, (F32)gPipeline.mRT->screen.getWidth(), (F32)gPipeline.mRT->screen.getHeight());
    // the clouds' Beer shadow map (aerialPerspectiveEffect.frag HAS_SHADOW uniforms, CloudsEffect.ts:421-431); none in
    // a material preview (LLGLTFPreviewTexture renders a sphere in its own little scene, not under the world's clouds)
    shader.uniform1f(s_on, on ? 1.f : 0.f);
    if (on)
    {
        static LLStaticHashedString s_si("shadowIntervals"), s_sm("shadowMatrices"), s_ism("inverseShadowMatrices"), s_sf("shadowFar"), s_sth("shadowTopHeight"), s_sr("shadowRadius");
        const S32 n = sClouds->cascadeCount();
        shader.uniform2fv(s_si, n, sClouds->shadowIntervals());
        shader.uniformMatrix4fv(s_sm, n, GL_FALSE, sClouds->shadowMatrices());
        shader.uniformMatrix4fv(s_ism, n, GL_FALSE, sClouds->inverseShadowMatrices());
        shader.uniform1f(s_sf, sClouds->shadowFar());
        shader.uniform1f(s_sth, sClouds->shadowTopHeight());
        shader.uniform1f(s_sr, 3.f);   // Source: AerialPerspectiveEffect.ts:271 shadowRadius 3
        static LLStaticHashedString s_cbh("wolfTkCloudBaseHeight");
        shader.uniform1f(s_cbh, sClouds->shadowBottomHeight());
        WolfTk::bindReserved(shader, LLShaderMgr::WOLF_TK_SHADOW_BUFFER, GL_TEXTURE_2D_ARRAY, sClouds->shadowMapArray());
    }
}

// static
void WolfEnvironmentRender::zonePass()
{
    if (gCubeSnapshot || tier() < 1) return;
    const WolfEnvDoc& d = WolfEnvironmentState::instance().active();
    if (!d.mValid || d.mMode == WolfEnvDoc::OFF || !sharedSky()) return;
    LLPipeline::RenderTargetPack* rt = gPipeline.mRT;
    if (!rt || !rt->screen.isComplete()) return;
    const ZoneMix z = zonesAtCamera(d);
    if (dist_vec_squared(LLVector3(z.lightMul.mV), LLVector3(1.f, 1.f, 1.f)) <= 1e-6f) return;
    LLGLDepthTest depthOff(GL_FALSE, GL_FALSE);
    rt->screen.bindTarget();
    LLGLEnable blend(GL_BLEND);
    gGL.blendFunc(LLRender::BF_DEST_COLOR, LLRender::BF_ZERO, LLRender::BF_ZERO, LLRender::BF_ONE);   // alpha (the glow mask) kept
    LLGLSLShader& sh = gWolfEnvLightProgram;
    sh.bind();
    static LLStaticHashedString s_lm("uLightMul");
    sh.uniform3f(s_lm, z.lightMul.mV[0], z.lightMul.mV[1], z.lightMul.mV[2]);
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    sh.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    rt->screen.flush();
}

// static
bool WolfEnvironmentRender::loadShaders(S32 shader_level)
{
    if (!sAtmos) sAtmos = new WolfTk::Atmosphere();
    if (!sClouds) sClouds = new WolfTk::Clouds();
    const bool a = sAtmos->loadShaders(shader_level);
    const bool c = sClouds->loadShaders(shader_level);
    return a && c;
}

// static
void WolfEnvironmentRender::unloadShaders()
{
    // a table build in flight is cancelled with the programs (WolfTk::Atmosphere::unloadShaders): asked for again
    // once they are back; finished tables stay and are kept drawing
    if (sAtmos && sAtmos->building()) sGpu.atmosRequested = false;
    sSceneUniformCache.clear();   // the program ids are about to go (and may be reused)
    if (sAtmos) sAtmos->unloadShaders();
    if (sClouds) sClouds->unloadShaders();
}

// static
void WolfEnvironmentRender::addAtmosphereDefines(LLGLSLShader& sh)
{
    WolfTk::Atmosphere::addDefines(sh);
}

// static
void WolfEnvironmentRender::releaseGL()
{
    // GL buffers are being rebuilt (a graphics setting changed, LLPipeline::releaseGLBuffers): the screen-sized
    // targets go; the atmosphere's tables do not depend on the screen and are kept (they take ~350 frames to
    // build, the EEP sky would show meanwhile) - destroyGL frees them with the context
    sGpu.failed = false;   // a failure is worth one more try
    sGpu.warned = false;
    sGpu.layer.release();
    sGpu.probeLayer.release();
    sGpu.camShadow.release();
    sStockRT.release();   // [round 16] the stock cloud probe
    if (sStockPbo) { glDeleteBuffers(1, &sStockPbo); sStockPbo = 0; sStockPboBytes = 0; }
    sStockPending = false;
    if (sGpu.camPbo[0]) { glDeleteBuffers(2, sGpu.camPbo); sGpu.camPbo[0] = sGpu.camPbo[1] = 0; }
    sGpu.camPboPending[0] = sGpu.camPboPending[1] = false;
    sGpu.logLum.release();
    if (sGpu.lumPbo[0]) { glDeleteBuffers(2, sGpu.lumPbo); sGpu.lumPbo[0] = sGpu.lumPbo[1] = 0; }
    sGpu.lumPboPending[0] = sGpu.lumPboPending[1] = false;
    sNext.frameLogAvg = -1.f;
    if (sZeroTex) { glDeleteTextures(1, &sZeroTex); sZeroTex = 0; }
    if (sClouds) sClouds->release();
    sMeter.reset();
    for (auto& f : sGpu.q)
        for (auto& s : f)
            for (GLuint& id : s)
                if (id) { glDeleteQueries(1, &id); id = 0; }
    for (auto& f : sGpu.qPending) for (bool& p : f) p = false;
}

// static
void WolfEnvironmentRender::destroyGL()
{
    // the GL context goes (LLPipeline::destroyGL): everything, the atmosphere's tables too
    releaseGL();
    if (sAtmos) sAtmos->release();
    sGpu.atmosRequested = false;
    sNext.valid = false;
    sFrame.valid = false;
    sGpu.active = false;
}
