/**
 * @file wolfloadingnotice.cpp
 * @brief WolfViewer: "Loading Wolf water..." at the bottom left of the world view while the Wolf sea or sky is on its way.
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

#include "llviewerprecompiledheaders.h"

#include "wolfloadingnotice.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender2dutils.h"
#include "lltoolbarview.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "wolfenvironmentrender.h"
#include "wolfenvironmentstate.h"
#include "wolfgrid.h"
#include "wolfoceanfft.h"
#include "wolfwaterfield.h"

static LLDefaultChildRegistry::Register<WolfLoadingNotice> r_wolf_loading_notice("wolf_loading_notice");

namespace
{
    // The Wolf sea of the agent's region is still to come: what lldrawpoolwater.cpp needs before it swells - the region's
    // shore / depth fields (WolfWaterField::get is null until the first bake, and the swell is held flat until then:
    // lldrawpoolwater.cpp FIELD_FADE_S) and the FFT cascades. Nothing is "loading" where the Wolf sea would not show
    // anyway: the shore field switched off, the lite water program (LLViewerShaderMgr::wolfWaterFull), or basic water
    // from up high (lldrawpoolwater.cpp wolfUpdateBasicWater, WolfViewerWaterBasicAboveM).
    bool waterWaiting(const LLViewerRegion* region)
    {
        static LLCachedControl<bool> shore_on(gSavedSettings, "WolfViewerWaterShoreField", true);
        static LLCachedControl<bool> fft_on(gSavedSettings, "WolfViewerWaterFFT", true);
        static LLCachedControl<F32> basic_above(gSavedSettings, "WolfViewerWaterBasicAboveM", 150.f);
        if (!shore_on || !LLViewerShaderMgr::wolfWaterFull())
        {
            return false;
        }
        const F32 above = LLViewerCamera::getInstance()->getOrigin().mV[VZ] - LLEnvironment::instance().getWaterHeight();
        if ((F32)basic_above > 0.f && above >= (F32)basic_above)
        {
            return false;
        }
        if (!WolfWaterField::instance().get(region))
        {
            return true;
        }
        // The cascades only count once they have been ready in this session: a GPU that never builds them must not
        // keep the notice up after every arrival.
        static bool s_fft_ever_ready = false;
        const bool fft_ready = WolfOceanFFT::instance().isReady();
        s_fft_ever_ready = s_fft_ever_ready || fft_ready;
        return fft_on && s_fft_ever_ready && !fft_ready;
    }

    // The native sky is drawn (WolfEnvironmentRender::tierLatched) but the documents of the region underneath have not been read
    // yet, so what shows is still the EEP-only sky or the last region's (wolfenvironmentstate.cpp applyReply).
    bool skyWaiting()
    {
        if (WolfEnvironmentRender::tierLatched() <= 0)   // the world pass's latch: a UI draw must not latch the frame
        {
            return false;
        }
        const LLViewerRegion* region = LLEnvironment::wolfEnvironmentRegion();
        return region && !WolfEnvironmentState::instance().documentsReadFor(region->getRegionID().asString());
    }
}

WolfLoadingNotice::WolfLoadingNotice(const Params& p)
    : LLUICtrl(p)
{
}

void WolfLoadingNotice::draw()
{
    const LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        mRegionId.clear();
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const std::string id = region->getRegionID().asString();
    if (id != mRegionId)
    {
        mRegionId = id;
        mArrivedAt = now;
    }
    // Never on another grid's region, and never stuck: an arrival gets TIMEOUT_S at most.
    if (now - mArrivedAt > TIMEOUT_S || !WolfGrid::isOnWolfRegionConfirmed())
    {
        return;
    }
    const bool water = waterWaiting(region);
    const bool sky = skyWaiting();
    if (!water && !sky)
    {
        return;
    }
    const std::string text = water && sky ? "Loading Wolf water and sky..." : water ? "Loading Wolf water..." : "Loading Wolf sky...";

    // Bottom left of the world view, clear of the left and bottom toolbars when they hold buttons.
    LLRect mine = calcScreenRect();
    S32 left = 12, bottom = 12;
    if (gToolBarView)
    {
        LLToolBar* tb_left = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_LEFT);
        if (tb_left && tb_left->getVisible() && tb_left->hasButtons())
        {
            left = llmax(left, tb_left->calcScreenRect().mRight - mine.mLeft + 12);
        }
        LLToolBar* tb_bottom = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_BOTTOM);
        if (tb_bottom && tb_bottom->getVisible() && tb_bottom->hasButtons())
        {
            bottom = llmax(bottom, tb_bottom->calcScreenRect().mTop - mine.mBottom + 8);
        }
    }
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 pad_x = 8, pad_y = 4;
    const S32 w = font->getWidth(text) + 2 * pad_x;
    const S32 h = (S32)font->getLineHeight() + 2 * pad_y;
    LLRect panel;
    panel.setLeftTopAndSize(left, bottom + h, w, h);
    // White on 70 % black: well over 4.5:1 over any sky or sea behind it.
    gl_rect_2d(panel, LLColor4(0.f, 0.f, 0.f, 0.7f), true);
    font->renderUTF8(text, 0, (F32)(left + pad_x), (F32)(bottom + pad_y), LLColor4::white, LLFontGL::LEFT, LLFontGL::BOTTOM,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}
