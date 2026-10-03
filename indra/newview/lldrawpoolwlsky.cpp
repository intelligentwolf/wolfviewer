/**
 * @file lldrawpoolwlsky.cpp
 * @brief LLDrawPoolWLSky class implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lldrawpoolwlsky.h"
#include "wolfweather.h"   // <WolfViewer 2026-09-18/> auroraAmount()

#include "llerror.h"
#include "llface.h"
#include "llimage.h"
#include "llrender.h"
#include "llenvironment.h"
#include "llglslshader.h"
#include "llgl.h"

#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewercamera.h"
#include "pipeline.h"
#include "llsky.h"
#include "llvowlsky.h"
#include "llsettingsvo.h"
#include "llviewercontrol.h"
#include "lltimer.h"
#include "llviewertexturelist.h"
#include "wolfregionweather.h"   // <WolfViewer 2026-10-03/> autoEnvSky() for the moon phase

extern bool gCubeSnapshot;

static LLStaticHashedString sCamPosLocal("camPosLocal");
static LLStaticHashedString sCustomAlpha("custom_alpha");
// <WolfViewer 2026-10-03> moonF.glsl wolf_moon_phase (Jimmy Olsen's Automatic Environment).
static LLStaticHashedString sWolfMoonPhase("wolf_moon_phase");
static LLStaticHashedString sWolfMoonNorth("wolf_moon_north");

static LLGLSLShader* cloud_shader = NULL;
static LLGLSLShader* sky_shader   = NULL;
static LLGLSLShader* sun_shader   = NULL;
static LLGLSLShader* moon_shader  = NULL;

static float sStarTime;

LLDrawPoolWLSky::LLDrawPoolWLSky(void) :
    LLDrawPool(POOL_WL_SKY)
{
}

LLDrawPoolWLSky::~LLDrawPoolWLSky()
{
}

LLViewerTexture *LLDrawPoolWLSky::getDebugTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::beginDeferredPass(S32 pass)
{
    sky_shader = &gDeferredWLSkyProgram;
    cloud_shader = &gDeferredWLCloudProgram;

    sun_shader = &gDeferredWLSunProgram;

    moon_shader = &gDeferredWLMoonProgram;
}

void LLDrawPoolWLSky::endDeferredPass(S32 pass)
{
    sky_shader   = nullptr;
    cloud_shader = nullptr;
    sun_shader   = nullptr;
    moon_shader  = nullptr;

    // clear the depth buffer so haze shaders can use unwritten depth as a mask
    glClear(GL_DEPTH_BUFFER_BIT);
}

void LLDrawPoolWLSky::renderDome(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader * shader) const
{
    llassert_always(NULL != shader);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    //chop off translation
    if (LLPipeline::sReflectionRender && camPosLocal.mV[2] > 256.f)
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], 256.f-camPosLocal.mV[2]*0.5f);
    }
    else
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    }


    // the windlight sky dome works most conveniently in a coordinate system
    // where Y is up, so permute our basis vectors accordingly.
    gGL.rotatef(120.f, 1.f / F_SQRT3, 1.f / F_SQRT3, 1.f / F_SQRT3);

    gGL.scalef(0.333f, 0.333f, 0.333f);

    gGL.translatef(0.f,-camHeightLocal, 0.f);

    // Draw WL Sky
    shader->uniform3f(sCamPosLocal, 0.f, camHeightLocal, 0.f);

    gSky.mVOWLSkyp->drawDome();

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

extern LLPointer<LLImageGL> gEXRImage;

static bool use_hdri_sky()
{
    static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
    static LLCachedControl<bool> irradiance_only(gSavedSettings, "RenderHDRIIrradianceOnly", false);

    return gCubeSnapshot && (!irradiance_only || !gPipeline.mReflectionMapManager.isRadiancePass()) ? gEXRImage.notNull() : // always use HDRI for reflection probes when available
        gEXRImage.notNull() ? hdri_split > 0.f : // fallback to EEP sky when split screen is zero
        false; // no HDRI available, always use EEP sky

}

void LLDrawPoolWLSky::renderSkyHazeDeferred(const LLVector3& camPosLocal, F32 camHeightLocal) const
{
    if (!gSky.mVOSkyp)
    {
        return;
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY))
    {
        if (use_hdri_sky())
        {
            sky_shader = &gEnvironmentMapProgram;
            sky_shader->bind();
            S32 idx = sky_shader->enableTexture(LLShaderMgr::ENVIRONMENT_MAP);
            if (idx > -1)
            {
                gGL.getTexUnit(idx)->bind(gEXRImage);
            }

            static LLCachedControl<F32> hdri_exposure(gSavedSettings, "RenderHDRIExposure", 0.0f);
            static LLCachedControl<F32> hdri_rotation(gSavedSettings, "RenderHDRIRotation", 0.f);
            static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
            static LLStaticHashedString hdri_split_screen("hdri_split_screen");

            LLMatrix3 rot;
            rot.setRot(0.f, hdri_rotation*DEG_TO_RAD, 0.f);

            sky_shader->uniform1f(LLShaderMgr::SKY_HDR_SCALE, powf(2.f, hdri_exposure));
            sky_shader->uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, GL_FALSE, (F32*) rot.mMatrix);
            sky_shader->uniform1f(hdri_split_screen, gCubeSnapshot ? 1.f : hdri_split);
        }
        else
        {
            sky_shader->bind();
        }

        LLGLSPipelineDepthTestSkyBox sky(true, true);

        sky_shader->uniform1i(LLShaderMgr::CUBE_SNAPSHOT, gCubeSnapshot ? 1 : 0);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLViewerTexture* rainbow_tex = gSky.mVOSkyp->getRainbowTex();
        LLViewerTexture* halo_tex  = gSky.mVOSkyp->getHaloTex();

        sky_shader->bindTexture(LLShaderMgr::RAINBOW_MAP, rainbow_tex);
        sky_shader->bindTexture(LLShaderMgr::HALO_MAP,  halo_tex);

        // <WolfViewer 2026-09-18> the northern lights (skyF.glsl wolfAurora). Source:
        // render_manager.js _applyWeatherAurora. 0 skips the march entirely.
        static LLStaticHashedString s_wolf_aurora("wolf_aurora");
        static LLStaticHashedString s_wolf_aurora_time("wolf_aurora_time");
        sky_shader->uniform1f(s_wolf_aurora, gCubeSnapshot ? 0.f : WolfWeather::auroraAmount());
        sky_shader->uniform1f(s_wolf_aurora_time, fmodf(gFrameTimeSeconds, 100000.f));
        static LLStaticHashedString s_wolf_aurora_color("wolf_aurora_color");
        sky_shader->uniform1i(s_wolf_aurora_color, WolfWeather::auroraColorMode());
        // </WolfViewer>

        F32 moisture_level  = (float)psky->getSkyMoistureLevel();
        F32 droplet_radius  = (float)psky->getSkyDropletRadius();
        F32 ice_level       = (float)psky->getSkyIceLevel();

        // hobble halos and rainbows when there's no light source to generate them
        if (!psky->getIsSunUp() && !psky->getIsMoonUp())
        {
            moisture_level = 0.0f;
            ice_level      = 0.0f;
        }

        sky_shader->uniform1f(LLShaderMgr::MOISTURE_LEVEL, moisture_level);
        sky_shader->uniform1f(LLShaderMgr::DROPLET_RADIUS, droplet_radius);
        sky_shader->uniform1f(LLShaderMgr::ICE_LEVEL, ice_level);

        sky_shader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        sky_shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, psky->getIsSunUp() ? 1 : 0);

        /// Render the skydome
        renderDome(origin, camHeightLocal, sky_shader);

        sky_shader->unbind();
    }
}

void LLDrawPoolWLSky::renderStarsDeferred(const LLVector3& camPosLocal) const
{
    if (!gSky.mVOSkyp || use_hdri_sky())
    {
        return;
    }

    LLGLSPipelineBlendSkyBox gls_sky(true, false);

    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    F32 star_alpha = LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f;

    // If start_brightness is not set, exit
    if(star_alpha < 0.001f)
    {
        // <WolfViewer 2026-10-03> no stars, so no constellation names either (Show Astronomy)
        if (gSky.mVOWLSkyp) gSky.mVOWLSkyp->hideConstellationNames();
        LL_DEBUGS("SKY") << "star_brightness below threshold." << LL_ENDL;
        return;
    }

    gDeferredStarProgram.bind();

    LLViewerTexture* tex_a = gSky.mVOSkyp->getBloomTex();
    LLViewerTexture* tex_b = gSky.mVOSkyp->getBloomTexNext();

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();

    if (tex_a && (!tex_b || (tex_a == tex_b)))
    {
        // Bind current and next sun textures
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b && !tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_b);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b != tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->bind(tex_b);
    }

    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    // <WolfViewer 2026-10-03> The real star map is placed for the real time already
    // (LLVOWLSky::updateRealStars); only the stock random field gets the slow decorative spin.
    // drawingRealStars() reports the previous frame's choice, which is the same choice.
    if (!gSky.mVOWLSkyp->drawingRealStars())
    {
        gGL.rotatef(gFrameTimeSeconds*0.01f, 0.f, 0.f, 1.f);
    }
    gDeferredStarProgram.uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

    if (LLPipeline::sReflectionRender)
    {
        star_alpha = 1.0f;
    }
    gDeferredStarProgram.uniform1f(sCustomAlpha, star_alpha);

    sStarTime = (F32)LLFrameTimer::getElapsedSeconds() * 0.5f;

    gDeferredStarProgram.uniform1f(LLShaderMgr::WATER_TIME, sStarTime);

    gSky.mVOWLSkyp->drawStars();

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

    gDeferredStarProgram.unbind();

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderSkyCloudsDeferred(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader* cloudshader) const
{
    if (use_hdri_sky())
    {
        return;
    }

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) && gSky.mVOSkyp && gSky.mVOSkyp->getCloudNoiseTex())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLGLSPipelineBlendSkyBox pipeline(true, true);

        cloudshader->bind();

        LLPointer<LLViewerTexture> cloud_noise      = gSky.mVOSkyp->getCloudNoiseTex();
        LLPointer<LLViewerTexture> cloud_noise_next = gSky.mVOSkyp->getCloudNoiseTexNext();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        F32 cloud_variance = psky ? (F32)psky->getCloudVariance() : 0.0f;
        F32 blend_factor   = psky ? (F32)psky->getBlendFactor() : 0.0f;

        if (psky->getCloudScrollRate().isExactlyZero())
        {
            blend_factor = 0.f;
        }

        // if we even have sun disc textures to work with...
        if (cloud_noise || cloud_noise_next)
        {
            if (cloud_noise && (!cloud_noise_next || (cloud_noise == cloud_noise_next)))
            {
                // Bind current and next sun textures
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next && !cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise_next, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next != cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, cloud_noise_next, LLTexUnit::TT_TEXTURE);
            }
        }

        cloudshader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
        cloudshader->uniform1f(LLShaderMgr::CLOUD_VARIANCE, cloud_variance);
        cloudshader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        /// Render the skydome
        renderDome(camPosLocal, camHeightLocal, cloudshader);

        cloudshader->unbind();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
    }
}

void LLDrawPoolWLSky::renderHeavenlyBodies()
{
    if (!gSky.mVOSkyp || use_hdri_sky()) return;

    LLGLSPipelineBlendSkyBox gls_skybox(true, true); // SL-14113 we need moon to write to depth to clip stars behind

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();
    gGL.pushMatrix();
    gGL.translatef(origin.mV[0], origin.mV[1], origin.mV[2]);

    LLFace * face = gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN];

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();
    bool can_use_vertex_shaders = gPipeline.shadersLoaded();
    bool can_use_windlight_shaders = gPipeline.canUseWindLightShaders();


    if (gSky.mVOSkyp->getSun().getDraw() && face && face->getGeomCount())
    {
        LLPointer<LLViewerTexture> tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLPointer<LLViewerTexture> tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        // if we even have sun disc textures to work with...
        if (tex_a || tex_b)
        {
            // if and only if we have a texture defined, render the sun disc
            if (can_use_vertex_shaders && can_use_windlight_shaders)
            {
                sun_shader->bind();

                if (tex_a && (!tex_b || (tex_a == tex_b)))
                {
                    // Bind current and next sun textures
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b && !tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b != tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    sun_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                }

                LLColor4 color(gSky.mVOSkyp->getSun().getInterpColor());

                sun_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
                sun_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

                face->renderIndexed();

                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

                sun_shader->unbind();
            }
        }
    }

    face = gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON];

    if (gSky.mVOSkyp->getMoon().getDraw() && face && face->getTexture(LLRender::DIFFUSE_MAP) && face->getGeomCount() && moon_shader)
    {
        LLViewerTexture* tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLViewerTexture* tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        LLColor4 color(gSky.mVOSkyp->getMoon().getInterpColor());

        if (can_use_vertex_shaders && can_use_windlight_shaders && (tex_a || tex_b))
        {
            moon_shader->bind();

            if (tex_a && (!tex_b || (tex_a == tex_b)))
            {
                // Bind current and next sun textures
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b && !tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b != tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //moon_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
            }

            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

            F32 moon_brightness = (float)psky->getMoonBrightness();

            moon_shader->uniform1f(LLShaderMgr::MOON_BRIGHTNESS, moon_brightness);
            moon_shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, gSky.mVOSkyp->getMoon().getColor().mV);
            moon_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
            //moon_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
            moon_shader->uniform3fv(LLShaderMgr::DEFERRED_MOON_DIR, 1, psky->getMoonDirection().mV); // shader: moon_dir

            // <WolfViewer 2026-10-03> The real moon, on regions following a real place (the
            // default sky keeps the moon close to the sun, which would make it nearly new).
            // The disc's axes are rebuilt exactly as LLVOSky::updateHeavenlyBodyGeometry builds
            // the quad (llvosky.cpp): right = to_dir x Z, up = right x to_dir, with its zenith
            // fallback; there texture u runs along right and v along up (TEX00..TEX11 corners).
            // The sun direction in that frame, with z towards the viewer (-moon direction), is
            // what moonF.glsl lights the sphere by. The sun direction is the sky's own
            // (LLSettingsSky::getSunDirection), the same one the Jimmy's Weather tab reports.
            {
                LLVector4 phase(0.f, 0.f, 1.f, 0.f);
                LLVector4 north(0.f, 1.f, 0.f, 0.f);
                static LLPointer<LLViewerFetchedTexture> sMoonMap;
                if (WolfRegionWeather::instance().autoEnvRealSky())
                {
                    if (sMoonMap.isNull())
                    {
                        // NASA SVS CGI Moon Kit (svs.gsfc.nasa.gov/4720), LRO colour, 2048 x 1024.
                        sMoonMap = LLViewerTextureManager::getFetchedTextureFromFile("world/wolf_moon_lroc.png",
                                        FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI);
                    }
                    const LLQuaternion rot = gSky.mVOSkyp->getMoon().getRotation();
                    const LLVector3 to_dir = LLVector3::x_axis * rot;
                    LLVector3 sun_dir = psky->getSunDirection();
                    sun_dir.normalize();
                    LLVector3 hb_right = to_dir % LLVector3::z_axis;
                    LLVector3 hb_up = hb_right % to_dir;
                    if ((to_dir * LLVector3::z_axis) > 0.99f)
                    {
                        hb_right = LLVector3::y_axis_neg * rot;
                        hb_up = LLVector3::z_axis * rot;
                    }
                    hb_right.normalize();
                    hb_up.normalize();
                    phase.set(sun_dir * hb_right, sun_dir * hb_up, -(sun_dir * to_dir), 1.f);

                    // Lunar north ~ the ecliptic north pole (RA 270, Dec 66.56; the Moon's axis
                    // is 1.5 deg from it), placed in this sky as LLVOWLSky::updateRealStars places
                    // a star: LST from GMST (Meeus 12.4) + longitude, then Meeus 13.5/13.6.
                    const F64 lat = WolfRegionWeather::instance().autoEnvLat() * DEG_TO_RAD;
                    const F64 lon = WolfRegionWeather::instance().autoEnvLon();
                    const F64 jd = WolfRegionWeather::instance().autoEnvSkyTime() / 86400.0 + 2440587.5;
                    const F64 t = (jd - 2451545.0) / 36525.0;
                    const F64 gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
                    const F64 h = fmod(fmod(gmst + lon, 360.0) + 360.0, 360.0) * DEG_TO_RAD - 270.0 * DEG_TO_RAD;
                    const F64 dec = 66.56 * DEG_TO_RAD;
                    const LLVector3 pole((F32)(-cos(dec) * sin(h)),
                                         (F32)(sin(dec) * cos(lat) - cos(dec) * cos(h) * sin(lat)),
                                         (F32)(sin(dec) * sin(lat) + cos(dec) * cos(h) * cos(lat)));
                    LLVector2 nd(pole * hb_right, pole * hb_up);
                    if (nd.length() < 1e-3f) nd.set(0.f, 1.f);
                    nd.normalize();
                    // Earthshine fades in as the sky darkens (sun from 0 to 10 degrees below).
                    const F32 night = llclamp(-sun_dir.mV[VZ] / 0.17f, 0.f, 1.f);
                    north.set(nd.mV[VX], nd.mV[VY], night, 0.f);
                    moon_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, sMoonMap, LLTexUnit::TT_TEXTURE);
                }
                moon_shader->uniform4f(sWolfMoonPhase, phase.mV[0], phase.mV[1], phase.mV[2], phase.mV[3]);
                moon_shader->uniform4f(sWolfMoonNorth, north.mV[0], north.mV[1], north.mV[2], north.mV[3]);
            }

            face->renderIndexed();

            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

            moon_shader->unbind();
        }
    }

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_WL_SKY);
    if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY) || gSky.mVOSkyp.isNull())
    {
        return;
    }

    // TODO: remove gSky.mVOSkyp and fold sun/moon into LLVOWLSky
    gSky.mVOSkyp->updateGeometry(gSky.mVOSkyp->mDrawable);

    const F32 camHeightLocal = LLEnvironment::instance().getCamHeight();

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders())
    {
        renderSkyHazeDeferred(origin, camHeightLocal);
        renderHeavenlyBodies();
        if (!gCubeSnapshot)
        {
            renderStarsDeferred(origin);
        }

        if (!gCubeSnapshot || gPipeline.mReflectionMapManager.isRadiancePass()) // don't draw clouds in irradiance maps to avoid popping
        {
            renderSkyCloudsDeferred(origin, camHeightLocal, cloud_shader);
        }

        // <WolfViewer 2026-10-03> World > Show Astronomy — in FRONT of the clouds (Paul: "it
        // needs to go in front of the clouds"), still depth-tested against the scene.
        if (!gCubeSnapshot)
        {
            renderConstellationsDeferred(origin);
        }
    }
}

// <WolfViewer 2026-10-03> The constellation figures, after the clouds. The star program draws them
// (starsF.glsl's flagged-line path: the vertex colour, scaled by smoothstep(0, 0.25, custom_alpha));
// custom_alpha 1 here = full bright white whenever the star pass has drawn the night sky at all.
void LLDrawPoolWLSky::renderConstellationsDeferred(const LLVector3& camPosLocal) const
{
    if (!gSky.mVOSkyp || !gSky.mVOWLSkyp || use_hdri_sky()) return;
    if (LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f < 0.001f) return;   // day: no night sky

    LLGLSPipelineBlendSkyBox gls_sky(true, false);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gDeferredStarProgram.bind();
    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    gDeferredStarProgram.uniform1f(sCustomAlpha, 1.0f);
    gDeferredStarProgram.uniform1f(LLShaderMgr::BLEND_FACTOR, 0.f);
    gSky.mVOWLSkyp->drawConstellations();
    gGL.popMatrix();
    gDeferredStarProgram.unbind();
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
}



LLViewerTexture* LLDrawPoolWLSky::getTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::resetDrawOrders()
{
}

//static
void LLDrawPoolWLSky::cleanupGL()
{
}

//static
void LLDrawPoolWLSky::restoreGL()
{
}
