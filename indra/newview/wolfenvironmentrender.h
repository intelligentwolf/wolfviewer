/**
 * @file wolfenvironmentrender.h
 * @brief WolfViewer: the Wolf Environment drawn natively (three-geospatial's atmosphere and clouds,
 *        Filament's camera exposure and AgX tone mapping).
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

#ifndef WOLF_ENVIRONMENT_RENDER_H
#define WOLF_ENVIRONMENT_RENDER_H

#include <functional>
#include "v3color.h"
#include "v3math.h"

class LLGLSLShader;

/**
 * [WOLF ENV TAKRAM 2026-10-12] The native Wolf sky, translated from published code (Paul: "go and actually
 * find code to do it and translate it"):
 *   atmosphere  three-geospatial (MIT) packages/atmosphere: Bruneton's precomputed scattering, computed on the
 *               GPU in Bruneton's 15-wavelength luminance mode (model.cc Init) so twilight keeps its colour;
 *               the sky dome (wolftk/skyF.glsl), the aerial perspective over the opaque frame
 *               (wolftk/compositeF.glsl), and on water and transparent surfaces (wolftk/atmosphereLibF.glsl)
 *   clouds      three-geospatial packages/clouds (wolftkclouds.cpp): Beer shadow maps, the volumetric march
 *               at 1/16 of the pixels with temporal upscaling, light shafts, haze; their shadow on the scene's
 *               sun term (softenLight / water / alpha, an adaptation of the source's post-process lighting)
 *   exposure    Filament's physically based camera (wolfexposure.cpp): an incident meter at the camera, so
 *               looking at the sky or at sand never changes it; Filament's AgX tone mapper (wolftk/filamentAgx.glsl)
 * Sources and decisions: region-terrain-work/wolf-environment-render/TRANSLATION_PROGRESS.md.
 *
 * QUALITY (WolfEnvironmentQuality, by graphics level): 0 today's EEP exactly; 1 the atmosphere; 2 + clouds
 * (takram's "medium" preset); 3 + clouds at "high".
 *
 * NOTHING CHANGES when this is off: the opt-in WolfEnvironmentNativeSky (default off: tierNow() returns 0),
 * off the Wolf grid, with no Wolf Environment, in "on top" / "off" mode, with a personal sky, or at tier 0 -
 * each hook returns before touching GL state and the EEP path runs exactly as before.
 */
class WolfEnvironmentRender
{
public:
    /** Once a frame, from LLDrawPoolWLSky::renderDeferred (not in cube snapshots): the state, the atmosphere
     *  tables (built over frames when the atmosphere changes), the light at the camera and the exposure. */
    static void frameUpdate();

    /** The quality tier in force now (0..3), after the setting, the GPU and any failure. */
    static S32  tier();
private:
    static S32  tierNow();
public:
    /** The Wolf sky replaces the EEP sky dome. */
    static bool skyActive();
    /** [WolfViewer 2026-10-13] The native sky is drawn and an EEP sky drives it (wolfeepreference.h): the frame is in the
     *  EEP's own linear units and takes the stock display chain. */
    static bool eepDriven();
    /** The native sky is drawn WITHOUT an EEP to drive it: the frame is pre-exposed luminance for Filament's AgX. */
    static bool agxDisplay();
    /** [WolfViewer 2026-10-13] VERIFICATION of the EEP reference itself (WolfEnvironmentTrace, every 5 s): the displayed frame
     *  (before the UI) read back on a 16 x 9 grid, the sky's pixels (depth at the far end) logged against
     *  WolfEepReference's stock sky and cloud for the same view direction - with the native sky off this checks the
     *  reference against the stock renderer's real output; with it on, the native frame against the reference. */
    static void verifyFrame(LLRenderTarget* displayed, LLRenderTarget* depthSource);
    /** Volumetric clouds replace the EEP clouds (and the cloud deck). */
    static bool volumetricActive();
    /** [WolfViewer 2026-10-13, round 16] THE STOCK EEP CLOUDS, MEASURED. When the volumetric clouds replace the EEP's 2D layer,
     *  the sky pass asks whether a reading of that layer is due (once a second, with measureCloudCover's own reading) and, if so,
     *  hands `draw` (LLDrawPoolWLSky's own cloud draw: the stock shader, textures and dome, blending off) to be drawn into a small
     *  target with the stock G-buffer's formats, from the same camera; the layer's opacity and colour per texel drive the
     *  native clouds' coverage, thinness and colour. */
    static bool stockCloudProbeDue();
    static void renderStockCloudProbe(const std::function<void()>& draw);

    /** Draws the Wolf sky over the whole view (the pipeline's screen triangle, each pixel's direction from the camera); false =
     *  not drawn, the caller draws the EEP sky as before. Also in reflection-probe snapshots. */
    static bool renderSkyDome();

    /** The clouds and the aerial perspective over the lit HDR frame, from the end of
     *  LLPipeline::renderDeferredLighting - BEFORE the water and the transparent geometry are drawn over it.
     *  In a reflection-probe snapshot (gCubeSnapshot) the clouds are marched for that face without temporal
     *  upscaling (WolfTk::Clouds::renderProbe), so the probes' ambient and reflections see them.
     *  With WolfEnvironmentDebugCapture on, this frame's targets are written to the logs folder (once). */
    static void postPass();
private:
    static void postPassInner();
public:
    /** The light inside the camera's zone (a cave, an interior) multiplied into the FINISHED frame,
     *  transparent geometry included (LLPipeline::renderFinalize). */
    static void zonePass();

    /** Sets the atmosphere's and the cloud shadow's uniforms and tables on a stock program that carries
     *  wolftk/atmosphereLibF.glsl (softenLight, water, alpha, material) - from LLPipeline::bindShadowMaps,
     *  which both bindDeferredShader paths call. Nothing when the program has no such unit or the sky is off. */
    static void bindSceneUniforms(LLGLSLShader& shader);

    /** [WOLF ENV TAKRAM 2026-10-12] While alive, bindSceneUniforms gives no cloud shadow: a material preview
     *  (llgltfmaterialpreviewmgr.cpp) lights its sphere in a scene of its own. */
    struct ScopedScenePreview
    {
        ScopedScenePreview() { ++sScenePreview; }
        ~ScopedScenePreview() { --sScenePreview; }
    };
    static S32 sScenePreview;

    /** The star colour for starsF.glsl wolf_star_tint: white unless the Wolf sky draws. */
    static LLColor3 starTint();
    /** [WolfViewer 2026-10-13] The EEP's star_brightness over its default (250), held to 0..4 (at least the space-star ramp aloft): x the native stars. */
    static F32 starBrightnessScale();
    /** [WolfViewer round 13] the frame's sun and moon (agent frame, unit; z up): the native sky's own bodies, which can differ
     *  from the EEP sky's (a document's own sun / moon). False before the first frame. */
    static bool bodyDirections(LLVector3& sun, LLVector3& moon);

    /** The frame's pre-exposure (Filament Exposure::exposure(EV100)): everything the Wolf sky and the scene's
     *  light write is luminance (cd/m2) x this. */
    static F32 exposure();
    /** The direct light on the scene as the deferred shaders take it: the sun's illuminance at the camera
     *  (three-geospatial getSunLightColor) / pi, pre-exposed; the clouds' shadow takes it away per pixel
     *  (wolfTkCloudShadow in the scene shaders). */
    static LLColor3 sceneSunlit();
    /** The sky's irradiance at the camera (three-geospatial SkyLightProbe) / pi, pre-exposed (wolf_amb_up). */
    static LLColor3 sceneAmblit();
    /** 0: the sky light probe's (1 + n.up) / 2 is evaluated by atmosphericsFuncs.glsl from wolf_amb_up alone. */
    static LLColor3 sceneAmbDown();
    /** The light on a horizontal surface (sun x the clouds' transmittance at the camera + sky) / pi,
     *  pre-exposed (wolf_light_h): what the water's body colour is lit by. */
    static LLColor3 sceneLightH();

    /** A fullbright surface's luminance: an LCD monitor's typical 300 cd/m2 (DESIGN.md section 8). */
    static constexpr F32 FULLBRIGHT_NITS = 300.f;
    /** An SL local light as the light shaders take it under the physical sky: luminous intensity in candela
     *  x pre-exposure (800 lm at full intensity; point I = phi / 4 pi, spot I = phi / pi, Filament). */
    static LLColor3 physicalLightColor(const LLColor3& linear_color_x_intensity, bool spot);

    /** [WOLF ENV TAKRAM 2026-10-12] The translated renderer's own programs (LLViewerShaderMgr::loadShadersDeferred);
     *  false = one failed (the native sky then stays off, the EEP draws). */
    static bool loadShaders(S32 shader_level);
    static void unloadShaders();
    /** The atmosphere's defines (WolfTk::Atmosphere::addDefines) for the sky, composite and stock programs. */
    static void addAtmosphereDefines(LLGLSLShader& sh);

    /** Free every GL object (LLPipeline::releaseGLBuffers). */
    static void releaseGL();
    /** The tone mapper's white balance under the native sky (the sun above the atmosphere, mean 1). */
    static LLColor3 whitePoint();
    /** The look before the tone curve (wolftk/filamentAgx.glsl wolfToneLook): log contrast about middle grey and
     *  saturation, Blender's "AgX - Medium High Contrast" magnitudes (1.2 / 1.0). */
    static F32 toneContrast();
    static F32 toneSaturation();
    /** The contrast's pivot: the frame's log-average as exposed (WolfExposure::Meter::tonePivot). */
    static F32 tonePivot();
    /** Free the atmosphere's tables as well (LLPipeline::destroyGL: the context goes). */
    static void destroyGL();
    /** WolfEnvironmentDebugCapture: the finished HDR frame, before the tone mapping (LLPipeline::renderFinalize). */
    static void captureFinal();
};

#endif // WOLF_ENVIRONMENT_RENDER_H
