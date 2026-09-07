/**
 * @file wolfsurfcurl.h
 * @brief WolfViewer: the barrel — curl ribbons along the surf train's break line.
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

#ifndef WOLF_SURFCURL_H
#define WOLF_SURFCURL_H

#include "llsingleton.h"
#include "llvertexbuffer.h"
#include "llpointer.h"
#include "llsettingswater.h"
#include "v3math.h"
#include "v3color.h"
#include <vector>

class LLViewerTexture;

// Source: wolfstorm/js/world/surf_curl.js SurfCurl — the same contour, ribbons and shader.
//
// The sea is one sheet of vertices: it can rear up and lean (waterV.glsl [SURF]) but never
// pass over itself, so the curl of a breaking wave — the lip thrown forward and the tube
// behind it — is a SEPARATE piece of geometry. The break line is the contour of the region's
// depth field at 0.6 x the surf height (where the sea's surf train is fully breaking), inside
// the SURF cells; it is extracted on the CPU (marching squares over WolfWaterField's depth
// bake), resampled every RIBBON_STEP_M and built as ribbons of PROFILE_STEPS across. The
// vertex shader runs the sea's own phase, opens the curl as the crest passes, peels it along
// the line and fades it to the sea's whitewater.
class WolfSurfCurl : public LLSingleton<WolfSurfCurl>
{
    LLSINGLETON(WolfSurfCurl);
    ~WolfSurfCurl();

public:
    static constexpr F32 RIBBON_STEP_M = 4.f;
    static constexpr S32 PROFILE_STEPS = 14;
    static constexpr F32 MIN_RIBBON_M = 24.f;
    static constexpr S32 MAX_ALONG_POINTS = 4000;   // x PROFILE_STEPS < 65536 (U16 indices)
    static constexpr F32 CURL_TRAVEL = 0.45f;       // fraction of a wavelength the curl lives

    /** The depth at which the sea's surf train is fully breaking (surf_curl.js breakDepth). */
    static F32 breakDepth(F32 surf_h);

    /**
     * Draw this frame's curl for the agent region (LLDrawPoolWater::renderPostDeferred, after
     * the sea). Rebuilds the ribbons when the field, the region or the surf height changed.
     */
    void render(F32 surf_h, F32 surf_set, F32 surf_len, F32 phase_time,
                const LLVector3& light_dir, const LLColor3& light_diffuse,
                const LLSettingsWater::ptr_t& pwater, LLViewerTexture* normal_map);
    /** Drop the geometry (region change, GL reset). */
    void release();

private:
    struct Pt { F32 x, y, dx, dy, dist; };
    bool rebuild(F32 surf_h);
    void appendRibbon(const std::vector<Pt>& run, F32 s0);

    LLPointer<LLVertexBuffer> mVB;
    U32  mIndexCount = 0;
    U32  mVertexCount = 0;
    F64  mBuiltAt = -1.0;       // WolfWaterField::Field::mBakedAt the ribbons were built from
    U64  mBuiltHandle = 0;
    F32  mBuiltH = 0.f;
    F64  mNextRebuild = 0.0;
    S32  mRibbons = 0;
    S32  mPoints = 0;
    // build scratch
    std::vector<F32> mPos, mUV, mDir, mDist;
    std::vector<U16> mIdx;
};

#endif // WOLF_SURFCURL_H
