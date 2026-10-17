/**
 * @file wolflandsandbox.cpp
 * @brief WolfViewer: About Land > Sandbox - auto return and most prims per person for a parcel.
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

#include "wolflandsandbox.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "lldispatcher.h"
#include "llfloaterland.h"
#include "lllineeditor.h"
#include "llparcel.h"
#include "lltextbox.h"
#include "lltextvalidate.h"
#include "llviewergenericmessage.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "message.h"
#include "wolfgrid.h"

// <WolfViewer 2026-10-13> About Land > Sandbox (wolflandsandbox.h).
namespace
{
    // Source: WolfSandboxModule.cs METHOD_IN / METHOD_OUT. The sim lowers the method it receives
    // (LLClientView.cs:8890); the reply's method is matched exactly by gGenericDispatcher.
    const std::string METHOD_TO_SIM("wolfsandbox");
    const std::string MESSAGE_FROM_SIM("WolfSandbox");
    // Source: WolfSandboxModule.cs Reply - the parameters, in order.
    enum EReply { R_LOCAL_ID, R_ON, R_MAX, R_MINUTES, R_CAN_EDIT, R_MINE, R_RESULT, R_MESSAGE, R_VERB, R_COUNT };
    // A region without WolfSandboxModule never answers.
    constexpr F32 ANSWER_TIMEOUT_S = 8.f;
    // Errors: readable against the default dark panel (setStatus).
    const LLColor4 ERROR_COLOR(1.f, 0.45f, 0.40f, 1.f);

    WolfPanelLandSandbox* sInstance = nullptr;

    S32 to_s32(const std::string& s, S32 fallback)
    {
        S32 v = fallback;
        return LLStringUtil::convertToS32(s, v) ? v : fallback;
    }
}

class WolfSandboxDispatchHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice,
                    const sparam_t& strings) override
    {
        WolfPanelLandSandbox::receive(invoice, strings);
        return true;
    }
};

namespace
{
    WolfSandboxDispatchHandler sSandboxHandler;
}

WolfPanelLandSandbox::WolfPanelLandSandbox(LLSafeHandle<LLParcelSelection>& parcel)
:   LLPanel(),
    mParcel(parcel)
{
    // Source: wolfdrive.cpp WolfDrive() - registered once with gGenericDispatcher.
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sSandboxHandler);
    }
    sInstance = this;
}

WolfPanelLandSandbox::~WolfPanelLandSandbox()
{
    if (sInstance == this)
    {
        sInstance = nullptr;
    }
}

bool WolfPanelLandSandbox::postBuild()
{
    mEnabled       = getChild<LLCheckBoxCtrl>("sandbox_enabled");
    mMaxPrimsEdit  = getChild<LLLineEditor>("sandbox_max_prims");
    mMinutesEdit   = getChild<LLLineEditor>("sandbox_return_minutes");
    mMaxPrimsError = getChild<LLTextBox>("sandbox_max_prims_error");
    mMinutesError  = getChild<LLTextBox>("sandbox_return_minutes_error");
    mYours         = getChild<LLTextBox>("sandbox_yours");
    mBuildHint     = getChild<LLTextBox>("sandbox_build_hint");
    mStatus        = getChild<LLTextBox>("sandbox_status");
    mApply         = getChild<LLButton>("sandbox_apply");
    mRevert        = getChild<LLButton>("sandbox_revert");

    // Source: llfloaterland.cpp LLPanelLandObjects::postBuild - the auto return field's validator.
    mMaxPrimsEdit->setPrevalidate(LLTextValidate::validateNonNegativeS32);
    mMinutesEdit->setPrevalidate(LLTextValidate::validateNonNegativeS32);

    mEnabled->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEdited(); });
    mMaxPrimsEdit->setKeystrokeCallback([this](LLLineEditor*, void*) { onEdited(); }, nullptr);
    mMinutesEdit->setKeystrokeCallback([this](LLLineEditor*, void*) { onEdited(); }, nullptr);
    mApply->setCommitCallback([this](LLUICtrl*, const LLSD&) { onApply(); });
    mRevert->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRevert(); });

    setFieldError(mMaxPrimsError, mMaxPrimsEdit, LLStringUtil::null);
    setFieldError(mMinutesError, mMinutesEdit, LLStringUtil::null);
    refresh();
    return true;
}

// LLFloaterLand::refresh calls this on every parcel selection change and properties update.
void WolfPanelLandSandbox::refresh()
{
    if (!mEnabled)
    {
        return;
    }
    LLParcel* parcel = mParcel->getParcel();
    LLViewerRegion* region = LLViewerParcelMgr::getInstance()->getSelectionRegion();
    const LLUUID region_id = region ? region->getRegionID() : LLUUID::null;
    const S32 local_id = parcel ? parcel->getLocalID() : -1;

    if (region_id != mRegionId || local_id != mLocalId)
    {
        // Another parcel: nothing shown so far belongs to it.
        mRegionId = region_id;
        mLocalId = local_id;
        mKnown = false;
        mOn = false;
        mMaxPrims = 0;
        mMinutes = 0;
        mCanEdit = false;
        mMine = 0;
        mDirty = false;
        mGetPending = false;
        mSetPending = false;
        mUnsupported = false;
        setStatus(LLStringUtil::null, false);
        writeControls();
        if (parcel && region && isInVisibleChain())
        {
            request();
        }
    }
    updateEnabled();
}

void WolfPanelLandSandbox::onVisibilityChange(bool visible)
{
    LLPanel::onVisibilityChange(visible);
    // Counts change while the floater is closed: ask again whenever the tab is shown.
    if (visible && mEnabled && !mDirty && !mSetPending)
    {
        mUnsupported = false;
        request();
        updateEnabled();
    }
}

void WolfPanelLandSandbox::draw()
{
    // [WOLF GRID GATE 2026-10-17] a parcel read refused only because the new region's features had not arrived yet (a
    // teleport) is asked again as soon as they say Wolf, instead of showing "Wolf only" until another parcel is picked
    if (mAwaitingGrid && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mUnsupported = false;
        request();
    }
    if ((mGetPending || mSetPending) && mPendingTimer.getElapsedTimeF32() > ANSWER_TIMEOUT_S)
    {
        const bool was_set = mSetPending;
        mGetPending = false;
        mSetPending = false;
        if (was_set)
        {
            setStatus(getString("no_answer_set"), true);
        }
        else if (!mKnown)
        {
            mUnsupported = true;
            setStatus(getString("unsupported"), true);
        }
        updateEnabled();
    }
    LLPanel::draw();
}

bool WolfPanelLandSandbox::sendToRegion(const std::vector<std::string>& params)
{
    // [WOLF GRID GATE 2026-10-17] every send (get and set) only to a region that has said it is Wolf (wolfgrid.h)
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return false;
    }
    LLViewerRegion* region = LLViewerParcelMgr::getInstance()->getSelectionRegion();
    if (!region || region->getRegionID() != mRegionId)
    {
        return false;
    }
    // Source: llviewergenericmessage.cpp send_generic_message (the blocks and fields), sent to the
    // region that owns the selected parcel as llfloaterland.cpp send_other_clean_time_message does
    // (msg->sendReliable(region->getHost())): a parcel of a neighbouring region is answered there.
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessage("GenericMessage");
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_TransactionID, LLUUID::null); // not used
    msg->nextBlock("MethodData");
    msg->addString("Method", METHOD_TO_SIM);
    msg->addUUID("Invoice", LLUUID::null);
    for (const std::string& p : params)
    {
        msg->nextBlock("ParamList");
        msg->addString("Parameter", p);
    }
    msg->sendReliable(region->getHost());
    return true;
}

void WolfPanelLandSandbox::request()
{
    if (mLocalId < 0 || mRegionId.isNull())
    {
        return;
    }
    // Paul (10-09): Wolf-only features work only on Wolf Territories (wolfgrid.h).
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mUnsupported = true;
        mAwaitingGrid = true;   // [WOLF GRID GATE 2026-10-17/] draw() asks again once this region has said it is Wolf
        setStatus(getString("wolf_only"), true);
        return;
    }
    mAwaitingGrid = false;
    if (sendToRegion({ "get", llformat("%d", mLocalId) }))
    {
        mGetPending = true;
        mPendingTimer.reset();
        if (!mKnown)
        {
            setStatus(getString("asking"), false);
        }
    }
}

void WolfPanelLandSandbox::onEdited()
{
    if (mWriting)
    {
        return;
    }
    mDirty = true;
    setFieldError(mMaxPrimsError, mMaxPrimsEdit, LLStringUtil::null);
    setFieldError(mMinutesError, mMinutesEdit, LLStringUtil::null);
    updateEnabled();
}

void WolfPanelLandSandbox::onRevert()
{
    mDirty = false;
    setFieldError(mMaxPrimsError, mMaxPrimsEdit, LLStringUtil::null);
    setFieldError(mMinutesError, mMinutesEdit, LLStringUtil::null);
    writeControls();
    setStatus(LLStringUtil::null, false);
    updateEnabled();
}

void WolfPanelLandSandbox::onApply()
{
    if (!mKnown || !mCanEdit || mSetPending)
    {
        return;
    }
    const bool on = mEnabled->get();
    const std::string max_text = mMaxPrimsEdit->getText();
    const std::string min_text = mMinutesEdit->getText();
    const S32 max_prims = max_text.empty() ? 0 : to_s32(max_text, -1);
    const S32 minutes = min_text.empty() ? 0 : to_s32(min_text, -1);

    // The region checks all of this again (WolfSandboxModule.OnViewerBinary); asking here first
    // puts the reason next to the field.
    bool ok = true;
    if (on && max_prims < 1)
    {
        setFieldError(mMaxPrimsError, mMaxPrimsEdit, getString("need_max_prims"));
        ok = false;
    }
    if (on && minutes < 1)
    {
        setFieldError(mMinutesError, mMinutesEdit, getString("need_minutes"));
        ok = false;
    }
    if (!ok)
    {
        // the first field to fix takes the focus
        (on && max_prims < 1 ? mMaxPrimsEdit : mMinutesEdit)->setFocus(true);
        setStatus(getString("fix_fields"), true);
        return;
    }

    const std::vector<std::string> params = {
        "set",
        llformat("%d", mLocalId),
        on ? "1" : "0",
        llformat("%d", llmax(0, max_prims)),
        llformat("%d", llmax(0, minutes)),
    };
    if (!sendToRegion(params))
    {
        setStatus(getString("no_region"), true);
        return;
    }
    mSetPending = true;
    mPendingTimer.reset();
    setStatus(getString("saving"), false);
    updateEnabled();
}

// static
void WolfPanelLandSandbox::receive(const LLUUID& region_id, const std::vector<std::string>& strings)
{
    // [WOLF GRID GATE 2026-10-17] a "WolfSandbox" from a region that is not Wolf, or one nobody asked for, is ignored
    if (!WolfGrid::isOnWolfTerritoriesConfirmed() || !sInstance || !(sInstance->mGetPending || sInstance->mSetPending))
    {
        return;
    }
    sInstance->handleReply(region_id, strings);
}

void WolfPanelLandSandbox::handleReply(const LLUUID& region_id, const std::vector<std::string>& strings)
{
    if (strings.size() < R_COUNT)
    {
        LL_WARNS("WolfSandbox") << "unreadable answer: " << strings.size() << " parameters" << LL_ENDL;
        return;
    }
    // Only an answer about the parcel shown now.
    if (region_id != mRegionId || to_s32(strings[R_LOCAL_ID], -1) != mLocalId)
    {
        return;
    }
    const std::string& result = strings[R_RESULT];
    const std::string& message = strings[R_MESSAGE];
    const bool was_set = strings[R_VERB] == "set";
    if (was_set)
    {
        mSetPending = false;
    }
    else
    {
        mGetPending = false;
    }

    if (result == "noparcel")
    {
        mKnown = false;
        setStatus(message, true);
        writeControls();
        updateEnabled();
        return;
    }

    // Every other answer carries the parcel's current state.
    mKnown = true;
    mUnsupported = false;
    mOn = strings[R_ON] == "1";
    mMaxPrims = llmax(0, to_s32(strings[R_MAX], 0));
    mMinutes = llmax(0, to_s32(strings[R_MINUTES], 0));
    mCanEdit = strings[R_CAN_EDIT] == "1";
    mMine = llmax(0, to_s32(strings[R_MINE], 0));

    if (result == "ok")
    {
        if (was_set)
        {
            mDirty = false;
            setStatus(getString(mOn ? "saved_on" : "saved_off"), false);
            // The parcel's auto return may have changed: show it on the Objects tab too
            // (llfloaterland.cpp LLPanelLandObjects::refresh reads getCleanOtherTime()).
            if (LLParcel* parcel = mParcel->getParcel())
            {
                parcel->setCleanOtherTime(mMinutes);
            }
            LLFloaterLand::refreshAll();
        }
        else if (!mDirty)
        {
            setStatus(LLStringUtil::null, false);
        }
        if (!mDirty)
        {
            writeControls();
        }
    }
    else
    {
        // A refusal is kept on screen until the next action, with the user's edits left in place.
        setStatus(message.empty() ? getString("failed") : message, true);
        if (!mDirty)
        {
            writeControls();
        }
    }
    updateEnabled();
}

void WolfPanelLandSandbox::writeControls()
{
    if (!mEnabled)
    {
        return;
    }
    mWriting = true;
    mEnabled->set(mOn);
    mMaxPrimsEdit->setText(mKnown && mMaxPrims > 0 ? llformat("%d", mMaxPrims) : LLStringUtil::null);
    mMinutesEdit->setText(mKnown ? llformat("%d", mMinutes) : LLStringUtil::null);
    mWriting = false;
}

void WolfPanelLandSandbox::updateEnabled()
{
    if (!mEnabled)
    {
        return;
    }
    const bool editable = mKnown && mCanEdit && !mSetPending;
    mEnabled->setEnabled(editable);
    mMaxPrimsEdit->setEnabled(editable);
    mMinutesEdit->setEnabled(editable);
    mApply->setEnabled(editable && mDirty);
    mRevert->setEnabled(editable && mDirty);

    if (mKnown)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", mMine);
        if (mOn)
        {
            args["[MAX]"] = llformat("%d", mMaxPrims);
            args["[MINUTES]"] = llformat("%d", mMinutes);
            mYours->setText(getString(mCanEdit ? "yours_on_unlimited" : (mMinutes > 0 ? "yours_on" : "yours_on_no_return"), args));
        }
        else
        {
            mYours->setText(getString("yours_off"));
        }
    }
    else
    {
        mYours->setText(LLStringUtil::null);
    }

    // A sandbox nobody else may build on limits no one: say so where the switch is.
    LLParcel* parcel = mParcel->getParcel();
    const bool show_hint = mKnown && mCanEdit && mEnabled->get() && parcel && !parcel->getAllowModify();
    mBuildHint->setVisible(show_hint);
}

void WolfPanelLandSandbox::setStatus(const std::string& msg, bool error)
{
    if (!mStatus)
    {
        return;
    }
    mStatus->setText(msg);
    // The confirmation colour is wolfregionweather.cpp WolfPanelWeather::setStatus's gold (5.2:1 on
    // the default dark panel); the error red is lighter than its 0.88/0.33/0.28 (3.7:1) to reach 5.3:1.
    mStatus->setColor(error ? ERROR_COLOR : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
}

void WolfPanelLandSandbox::setFieldError(LLTextBox* box, LLLineEditor* field, const std::string& msg)
{
    if (!box || !field)
    {
        return;
    }
    box->setText(msg);
    box->setColor(ERROR_COLOR);
    box->setVisible(!msg.empty());
}
// </WolfViewer>
