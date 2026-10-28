/**
 * @file lldrawpoolwater.h
 * @brief LLDrawPoolWater class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLDRAWPOOLWATER_H
#define LL_LLDRAWPOOLWATER_H

#include "lldrawpool.h"
#include "wolfseastate.h"   // <WolfViewer 2026-09-06>


class LLFace;
class LLHeavenBody;
class LLWaterSurface;
class LLGLSLShader;

class LLDrawPoolWater final: public LLFacePool
{
protected:
    LLPointer<LLViewerTexture> mWaterImagep[2];
    LLPointer<LLViewerTexture> mWaterNormp[2];

    LLPointer<LLViewerTexture> mOpaqueWaterImagep;

public:
    static bool sSkipScreenCopy;
    static bool sNeedsReflectionUpdate;
    static bool sNeedsDistortionUpdate;
    static F32 sWaterFogEnd;

    enum
    {
        VERTEX_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_TEXCOORD0 |
                            LLVertexBuffer::MAP_TEXCOORD1   // <WolfViewer> terrain-LOD lift, conforming meshes only
    };

    virtual U32 getVertexDataMask() override { return VERTEX_DATA_MASK; }

    LLDrawPoolWater();
    /*virtual*/ ~LLDrawPoolWater();

    S32 getNumPostDeferredPasses() override;
    void beginPostDeferredPass(S32 pass) override;
    void renderPostDeferred(S32 pass) override;

    void prerender() override;

    LLViewerTexture *getDebugTexture() override;
    LLColor3 getDebugColor() const; // For AGP debug display

    void setTransparentTextures(const LLUUID& transparentTextureId, const LLUUID& nextTransparentTextureId);
    void setOpaqueTexture(const LLUUID& opaqueTextureId);
    void setNormalMaps(const LLUUID& normalMapId, const LLUUID& nextNormalMapId);

    void pushWaterPlanes(int pass);

    // <WolfViewer> The sea state this pool last drew with — the boat rocker
    // (wolfboatrock.cpp) samples the same swell on the CPU so hulls ride the surface that
    // is actually on screen.
    const WolfSeaState& getSeaState() const { return mSeaState; }
    // [SURF 2026-09-07] this frame's surf train parameters (the boat rocker mirrors them)
    F32 getSurfHeight() const { return mSurfHeight; }
    F32 getSurfSetInterval() const { return mSurfSetInterval; }
    F32 getSurfLength() const { return mSurfLength; }
    // <WolfViewer 2026-10-07> true while the camera is high enough to draw plain water
    // (no swell, foam or surf); see wolfUpdateBasicWater.
    bool isBasicWater() const { return mBasicWater; }
    // </WolfViewer>
    // [SMOOTH SEA 2026-10-28] The wave inputs EASED from the old weather to the new (wolfUpdateSea), and the phases
    // ACCUMULATED from them, so a weather change never re-phases the sea. All in the shader's `time` units
    // (LLFrameTimer::getElapsedSeconds() * 0.5). The boat rocker (wolfboatrock.cpp) rides the same sea through these.
    struct SeaClocks
    {
        F64 mSwell = 0.0;     // integral of sqrt(waveFrequency + 0.001) * waveSpeed  (waterV.glsl wolfSwellPhaseT)
        F64 mBreaker = 0.0;   // integral of 4.5 * the breaker speed scale, wrapped to 2 pi (waterV.glsl breakers)
        F64 mAutoSurf = 0.0;  // integral of the automatic surf's omega0, wrapped to 2 pi (waterV.glsl autoS)
    };
    // The clocks at shader time t (the last update carried on at the rates it was made with).
    SeaClocks seaClocksAt(F64 t) const;
    const LLVector2& getEasedWaveDir1() const { return mSeaDir1; }
    // True while the sea snaps instead of easing: a teleport arrival (LLEnvironment::wolfTeleportArrival) and the
    // first WOLF_SEA_LOGIN_SNAP_S in-world after login - no fade from the last place or the default water.
    static bool wolfSeaArrival();

protected:
    void renderOpaqueLegacyWater();

    // <FS:Zi> Render speedup for water parameters
    void onRenderWaterMipNormalChanged();

    bool mRenderWaterMipNormal;
    // </FS:Zi>
    // <WolfViewer 2026-09-06> the wave field this frame draws with (wolfseastate.h), set in
    // renderPostDeferred from the region's EEP water (or the manual settings) and read per
    // face by pushWaterPlanes.
    WolfSeaState mSeaState;
    // [SURF 2026-09-07 phase 2] this frame's surf parameters, for the curl (wolfsurfcurl.cpp)
    F32 mSurfHeight = 0.f;
    F32 mSurfSetInterval = 90.f;
    F32 mSurfLength = 36.f;
    // <WolfViewer 2026-10-07> basic water from up high
    void wolfUpdateBasicWater(F32 camera_above_water);
    bool mBasicWater = false;
    // </WolfViewer>
    // [SMOOTH SEA 2026-10-28] see seaClocksAt
    void wolfUpdateSea(const LLSettingsWater::ptr_t& pwater);
    bool      mSeaInit = false;
    F64       mSeaLastT = 0.0;            // shader time of the last update
    F64       mSeaIndexA = 1.0, mSeaIndexB = 1.0;   // the sea state index through the two easing stages
    F64       mDir1AngA = 0.0, mDir1AngB = 0.0, mDir1LenA = 0.0, mDir1LenB = 0.0;
    F64       mDir2AngA = 0.0, mDir2AngB = 0.0, mDir2LenA = 0.0, mDir2LenB = 0.0;
    LLVector2 mSeaDir1, mSeaDir2;         // the eased EEP wave1 / wave2 directions (direction AND scroll speed)
    SeaClocks mSeaClocks;
    SeaClocks mSeaRates;                  // per unit of shader time, as last computed
    F64       mScroll1X = 0.0, mScroll1Y = 0.0, mScroll2X = 0.0, mScroll2Y = 0.0;   // integral of waveDir1 / waveDir2
};

void cgErrorCallback();

#endif // LL_LLDRAWPOOLWATER_H
