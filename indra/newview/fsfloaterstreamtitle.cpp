/**
 * @file fsfloaterstreamtitle.cpp
 * @brief Class for the stream title and history floater
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2022 Ansariel Hiller @ Second Life
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterstreamtitle.h"

#include "fsscrolllistctrl.h"
#include "llaudioengine.h"
#include "llbutton.h"
#include "llfloaterreg.h"
#include "llpanel.h"
#include "llrender2dutils.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "wolfalbumart.h"

static constexpr size_t MAX_HISTORY_ENTRIES{ 10 };
static constexpr F32 SCROLL_STEP_DELAY{ 0.25f };
static constexpr F32 SCROLL_END_DELAY{ 4.f };
// <WolfViewer> the cover panel is 96px square under the title; showing it adds this much height
static constexpr S32 ART_EXTRA_HEIGHT{ 102 };
// </WolfViewer>

FSStreamTitleManager::~FSStreamTitleManager()
{
    if (mMetadataUpdateConnection.connected())
    {
        mMetadataUpdateConnection.disconnect();
    }
}

void FSStreamTitleManager::initSingleton()
{
    if (!gAudiop || !gAudiop->getStreamingAudioImpl())
    {
        return;
    }

    mMetadataUpdateConnection = gAudiop->getStreamingAudioImpl()->setMetadataUpdateCallback(std::bind(&FSStreamTitleManager::processMetadataUpdate, this, std::placeholders::_1));
    processMetadataUpdate(gAudiop->getStreamingAudioImpl()->getCurrentMetadata());
}

void FSStreamTitleManager::processMetadataUpdate(const LLSD& metadata) noexcept
{
    std::string chat{};

    if (metadata.has("ARTIST"))
    {
        chat = metadata["ARTIST"].asString();
    }
    if (metadata.has("TITLE"))
    {
        if (chat.length() > 0)
        {
            chat.append(" - ");
        }
        chat.append(metadata["TITLE"].asString());
    }

    if (chat != mCurrentStreamTitle)
    {
        mCurrentStreamTitle = std::move(chat);

        if (!mCurrentStreamTitle.empty() && (mStreamTitleHistory.empty() || mStreamTitleHistory.back() != mCurrentStreamTitle))
        {
            mStreamTitleHistory.emplace_back(mCurrentStreamTitle);

            if (mStreamTitleHistory.size() > MAX_HISTORY_ENTRIES)
            {
                mStreamTitleHistory.erase(mStreamTitleHistory.begin());
            }

            mHistoryUpdateSignal(mStreamTitleHistory);
        }

        mUpdateSignal(mCurrentStreamTitle);
    }
}


//////////////////////////////////////////////////////////////////////////
/// FSFloaterStreamTitleHistory
//////////////////////////////////////////////////////////////////////////

FSFloaterStreamTitleHistory::FSFloaterStreamTitleHistory(const LLSD& key)
    : LLFloater(key)
{
}

FSFloaterStreamTitleHistory::~FSFloaterStreamTitleHistory()
{
    if (mUpdateConnection.connected())
    {
        mUpdateConnection.disconnect();
    }
}

bool FSFloaterStreamTitleHistory::postBuild()
{
    mHistoryCtrl = findChild<FSScrollListCtrl>("history");

    FSStreamTitleManager& instance = FSStreamTitleManager::instance();
    mUpdateConnection = instance.setHistoryUpdateCallback([this](const FSStreamTitleManager::history_vec_t& history) { updateHistory(history); });
    updateHistory(instance.getStreamTitleHistory());

    return true;
}

void FSFloaterStreamTitleHistory::updateHistory(const FSStreamTitleManager::history_vec_t& history)
{
    mHistoryCtrl->clearRows();

    for (const auto& entry : history)
    {
        LLSD data;
        data["columns"][0]["name"] = "title";
        data["columns"][0]["value"] = entry;

        mHistoryCtrl->addElement(data, ADD_TOP);
    }
}

void FSFloaterStreamTitleHistory::setOwnerOrigin(LLView* owner) noexcept
{
    mOwner = owner->getHandle();
}

void FSFloaterStreamTitleHistory::draw()
{
    LLFloater::draw();

    if (mOwner.get())
    {
        static LLCachedControl<F32> max_opacity(gSavedSettings, "PickerContextOpacity", 0.4f);
        drawConeToOwner(mContextConeOpacity, max_opacity, mOwner.get());
    }
}


//////////////////////////////////////////////////////////////////////////
/// FSFloaterStreamTitle
//////////////////////////////////////////////////////////////////////////

FSFloaterStreamTitle::FSFloaterStreamTitle(const LLSD& key)
    : LLFloater(key), LLEventTimer(SCROLL_STEP_DELAY)
{
    mEventTimer.stop();
}

FSFloaterStreamTitle::~FSFloaterStreamTitle()
{
    if (mUpdateConnection.connected())
    {
        mUpdateConnection.disconnect();
    }
    if (mArtConnection.connected())
    {
        mArtConnection.disconnect();
    }
}

bool FSFloaterStreamTitle::postBuild()
{
    mTitletext = findChild<LLTextBox>("streamtitle");
    mHistoryBtn = findChild<LLButton>("btn_history");
    // <WolfViewer> album cover
    mArtPanel = findChild<LLPanel>("album_art");
    mBaseMinHeight = getMinHeight();
    mArtConnection = WolfAlbumArt::instance().setUpdateCallback([this]() { updateArt(); });
    updateArt();
    // </WolfViewer>

    FSStreamTitleManager& instance = FSStreamTitleManager::instance();
    mUpdateConnection = instance.setUpdateCallback([this](std::string_view streamtitle) { updateStreamTitle(streamtitle); });
    updateStreamTitle(instance.getCurrentStreamTitle());

    mHistoryBtn->setCommitCallback(std::bind(&FSFloaterStreamTitle::toggleHistory, this));
    mHistoryBtn->setIsToggledCallback([](LLUICtrl*, const LLSD&) { return LLFloaterReg::instanceVisible("fs_streamtitlehistory"); });


    setVisibleCallback(boost::bind(&FSFloaterStreamTitle::closeHistory, this));

    return true;
}

void FSFloaterStreamTitle::toggleHistory() noexcept
{
    LLFloater* root_floater = gFloaterView->getParentFloater(this);
    FSFloaterStreamTitleHistory* history_floater = LLFloaterReg::findTypedInstance<FSFloaterStreamTitleHistory>("fs_streamtitlehistory");

    if (!history_floater)
    {
        history_floater = LLFloaterReg::showTypedInstance<FSFloaterStreamTitleHistory>("fs_streamtitlehistory");
        if (root_floater && history_floater)
        {
            root_floater->addDependentFloater(history_floater);
            history_floater->setOwnerOrigin(root_floater);
            mHistory = history_floater->getHandle();
        }
    }
    else
    {
        closeHistory();
    }

}

void FSFloaterStreamTitle::closeHistory() noexcept
{
    if (mHistory.get())
    {
        mHistory.get()->closeFloater();
    }
}

void FSFloaterStreamTitle::updateStreamTitle(std::string_view streamtitle) noexcept
{
    if (streamtitle.empty())
    {
        mTitletext->setText(getString("NoStream"));
    }
    else
    {
        mTitletext->setText(static_cast<std::string>(streamtitle));
    }

    mCurrentTitle = mTitletext->getText();
    mCurrentDrawnTitle = mCurrentTitle;

    mTitletext->setToolTip(mCurrentTitle);

    checkTitleWidth();
}

void FSFloaterStreamTitle::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLFloater::reshape(width, height, called_from_parent);
    checkTitleWidth();
}

// <WolfViewer> album cover ---------------------------------------------------------------
void FSFloaterStreamTitle::updateArt() noexcept
{
    WolfAlbumArt& art = WolfAlbumArt::instance();
    bool show = art.isEnabled() && art.getArtTexture().notNull();
    if (mArtPanel)
    {
        mArtPanel->setToolTip(art.getArtLabel());
    }
    setArtShown(show);
}

// Grow the floater downwards to make room for the cover, and shrink it back when the
// cover goes (setting off, stream stopped, no match). Resizing keeps the top-left corner
// where the user put it; a user-resized floater keeps its extra height.
void FSFloaterStreamTitle::setArtShown(bool show) noexcept
{
    if (!mArtPanel || show == mArtShown)
    {
        return;
    }
    mArtShown = show;
    mArtPanel->setVisible(show);
    S32 min_height = mBaseMinHeight + (show ? ART_EXTRA_HEIGHT : 0);
    setResizeLimits(getMinWidth(), min_height);
    LLRect rect = getRect();
    S32 height = rect.getHeight() + (show ? ART_EXTRA_HEIGHT : -ART_EXTRA_HEIGHT);
    rect.mBottom = rect.mTop - llmax(height, min_height);
    setShape(rect);
}

void FSFloaterStreamTitle::draw()
{
    LLFloater::draw();
    if (!mArtShown || !mArtPanel || !mArtPanel->getVisible())
    {
        return;
    }
    LLPointer<LLViewerFetchedTexture> tex = WolfAlbumArt::instance().getArtTexture();
    if (tex.isNull() || !tex->hasGLTexture())
    {
        return;              // still downloading: the panel background shows as a placeholder
    }
    // Same alpha rule as LLTextureCtrl::draw so the cover fades with the floater.
    const F32 alpha = getTransparencyType() == TT_ACTIVE ? 1.0f : getCurrentTransparency();
    const LLRect& r = mArtPanel->getRect();
    tex->addTextureStats((F32)(r.getWidth() * r.getHeight()));
    gl_draw_scaled_image(r.mLeft, r.mBottom, r.getWidth(), r.getHeight(), tex, UI_VERTEX_COLOR % alpha);
}
// </WolfViewer> --------------------------------------------------------------------------

void FSFloaterStreamTitle::checkTitleWidth() noexcept
{
    if (!mTitletext)
    {
        return;
    }

    S32 text_width = mTitletext->getFont()->getWidth(mCurrentTitle);
    S32 textbox_width = mTitletext->getRect().getWidth();

    if (text_width > textbox_width)
    {
        mResetTitle = false;
        mEventTimer.start();
    }
    else
    {
        mEventTimer.stop();
        mTitletext->setText(mCurrentTitle);
        mCurrentDrawnTitle = mCurrentTitle;
    }
}

bool FSFloaterStreamTitle::tick()
{
    if (mResetTitle)
    {
        mPeriod = SCROLL_END_DELAY;
        mEventTimer.reset();
        mCurrentDrawnTitle = mCurrentTitle;
        mResetTitle = false;
    }
    else
    {
        mPeriod = SCROLL_STEP_DELAY;
        mEventTimer.reset();
        mCurrentDrawnTitle.erase(mCurrentDrawnTitle.begin());
    }

    mTitletext->setText(mCurrentDrawnTitle);

    S32 text_width = mTitletext->getFont()->getWidth(mCurrentDrawnTitle);
    S32 textbox_width = mTitletext->getRect().getWidth();

    if (textbox_width > text_width)
    {
        mPeriod = SCROLL_END_DELAY;
        mEventTimer.reset();
        mResetTitle = true;
    }

    return false;
}
