/**
 * @file wolffloaterlagdetector.cpp
 * @brief WolfViewer: World > Lag Detector… (see wolffloaterlagdetector.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "wolffloaterlagdetector.h"

#include "lfsimfeaturehandler.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcachename.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llparcel.h"            // RT_NONE for Return to Owner
#include "llscrolllistctrl.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltracker.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "message.h"

#include <algorithm>

namespace
{
    // How often the two reports are asked for while the window is open. Short enough that a
    // first answer comes quickly, long enough that the region barely notices one extra pass.
    constexpr F32 POLL_SECONDS = 30.f;
    // Fractions kept per object: 10 x 30 s = the last 5 minutes (Paul, 2026-09-29: "50% of a
    // core for 5 min").
    constexpr size_t WINDOW_INTERVALS = 10;
    constexpr F32 RUNAWAY_FRACTION = 0.9f;
    constexpr F32 HEAVY_FRACTION = 0.5f;
    constexpr F32 BUSY_FRACTION = 0.1f;
    // [WOLF THROTTLE 2026-09-30] OpenSimWolf holds back a script that runs nonstop to about a
    // fifth of a core (YEngine XMRInstMain.cs HOT_SLICES / HOLD_MS: 60 ms run, 240 ms held -
    // measured 19% on VWEC Sandbox), unless someone is sitting on it. Such an object reads
    // as a flat 0.2 x its stuck scripts: at least HELD_MIN every interval, varying by no more
    // than HELD_SPREAD, for HELD_INTERVALS in a row. Normal busy scripts rise and fall.
    constexpr F32 HELD_MIN = 0.15f;
    constexpr F32 HELD_SPREAD = 0.05f;
    constexpr size_t HELD_INTERVALS = 3;
    // All scripts together using a whole core with no single culprit: the same load one runaway
    // script puts on the machine, spread over many objects.
    constexpr F32 SCRIPTS_TOTAL_FRACTION = 1.0f;
    constexpr size_t LIST_AT_MOST = 5;
    // LandStatRequest ReportType values, llfloatertopobjects.h STAT_REPORT_TOP_SCRIPTS / COLLIDERS.
    constexpr U32 REPORT_SCRIPTS = 0;
    constexpr U32 REPORT_COLLIDERS = 1;

    // Source: llfloatertopobjects.cpp onRefresh() - the same message Top Objects sends.
    void sendLandStatRequest(U32 report_type, U32 flags, S32 parcel_local_id)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
            return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_LandStatRequest);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_RequestData);
        msg->addU32Fast(_PREHASH_ReportType, report_type);
        msg->addU32Fast(_PREHASH_RequestFlags, flags);
        msg->addStringFast(_PREHASH_Filter, std::string());
        msg->addS32Fast(_PREHASH_ParcelLocalID, parcel_local_id);
        msg->sendReliable(region->getHost());
    }

    // Source: llfloatertopobjects.cpp returnObjects() - one TaskID, whole region (-1), RT_NONE.
    // OpenSim LandManagementModule.ReturnObjectsInParcel checks CanReturnObjects per object.
    void sendReturn(const LLUUID& object_id)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || object_id.isNull())
            return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ParcelReturnObjects);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ParcelData);
        msg->addS32Fast(_PREHASH_LocalID, -1); // Whole region
        msg->addS32Fast(_PREHASH_ReturnType, RT_NONE);
        msg->nextBlockFast(_PREHASH_TaskIDs);
        msg->addUUIDFast(_PREHASH_TaskID, object_id);
        msg->sendReliable(region->getHost());
    }

    // Source: llviewermenu.cpp derez_objects() - one root to Trash in one packet. OpenSim only
    // lets the owner (or a god) do this (PermissionsModule.cs CanDeleteObject).
    void sendDelete(U32 object_local_id)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || object_local_id == 0)
            return;
        LLUUID tid;
        tid.generate();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)DRD_TRASH);
        msg->addUUIDFast(_PREHASH_DestinationID, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH));
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, 1);
        msg->addU8Fast(_PREHASH_PacketNumber, 0);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object_local_id);
        msg->sendReliable(region->getHost());
    }
}

WolfFloaterLagDetector::WolfFloaterLagDetector(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterLagDetector::~WolfFloaterLagDetector()
{
}

bool WolfFloaterLagDetector::postBuild()
{
    mVerdict     = getChild<LLTextBox>("verdict_text");
    mSpeed       = getChild<LLTextBox>("speed_text");
    mList        = getChild<LLScrollListCtrl>("problems_list");
    mDetail      = getChild<LLTextBox>("detail_text");
    mShowBusy    = getChild<LLCheckBoxCtrl>("show_busy_check");
    mBeaconBtn   = getChild<LLButton>("beacon_btn");
    mTeleportBtn = getChild<LLButton>("teleport_btn");
    mReturnBtn   = getChild<LLButton>("return_btn");
    mDeleteBtn   = getChild<LLButton>("delete_btn");

    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mShowBusy->setCommitCallback([this](LLUICtrl*, const LLSD&) { rebuild(); });
    getChild<LLButton>("check_now_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mFirstPoll = true; });
    mBeaconBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBeacon(); });
    mTeleportBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTeleport(); });
    mReturnBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReturn(); });
    mDeleteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDelete(); });
    return true;
}

void WolfFloaterLagDetector::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // Every opening starts fresh: the numbers only mean something as a run of readings taken
    // while the window is open.
    resetForRegion();
}

void WolfFloaterLagDetector::resetForRegion()
{
    LLViewerRegion* region = gAgent.getRegion();
    mRegionHandle = region ? region->getHandle() : 0;
    mCanManage = gAgent.canManageEstate();
    mScripts.clear();
    mColliders.clear();
    mScriptReplies = 0;
    mFirstPoll = true;
    rebuild();
}

// Only runs while the floater is drawn: closed or minimised, nothing is asked of the region.
void WolfFloaterLagDetector::draw()
{
    LLViewerRegion* region = gAgent.getRegion();
    // Estate rights come with RegionHandshake, which can land after a teleport has already
    // switched the region - so they are re-checked every frame, not only on opening.
    if (region && (region->getHandle() != mRegionHandle || gAgent.canManageEstate() != mCanManage))
        resetForRegion();

    if (region && mCanManage && (mFirstPoll || mPollTimer.getElapsedTimeF32() >= POLL_SECONDS))
    {
        mFirstPoll = false;
        mPollTimer.reset();
        sendRequests();
    }

    // SimStats arrive every second whatever this window does; keep the speed line current.
    if (mSpeed)
    {
        LLTrace::Recording& rec = LLTrace::get_frame_recording().getLastRecording();
        const F64 dilation = rec.getLastValue(LLStatViewer::SIM_TIME_DILATION);
        const F64 sim_fps = rec.getLastValue(LLStatViewer::SIM_FPS);
        const F64 physics_fps = rec.getLastValue(LLStatViewer::SIM_PHYSICS_FPS);
        F32 script_cores = 0.f;
        for (const auto& [id, s] : mScripts)
            if (s.mSeen && !s.mFractions.empty())
                script_cores += s.mFractions.back();
        LLStringUtil::format_map_t args;
        args["[SPEED]"] = llformat("%.0f", dilation * 100.0);
        args["[SIMFPS]"] = llformat("%.0f", sim_fps);
        args["[PHYSFPS]"] = llformat("%.0f", physics_fps);
        args["[CORES]"] = mScriptReplies >= 2 ? llformat("%.2f", script_cores) : std::string("…");
        mSpeed->setText(getString("speed_line", args));
    }

    LLFloater::draw();
}

void WolfFloaterLagDetector::sendRequests()
{
    // Region owners and estate managers only (Paul 2026-09-30: "i only want region owners to
    // have it") - always the whole region, so no parcel bit and no parcel ID.
    sendLandStatRequest(REPORT_SCRIPTS, REQUEST_MARKER, 0);
    sendLandStatRequest(REPORT_COLLIDERS, REQUEST_MARKER, 0);
}

// static
bool WolfFloaterLagDetector::handleLandStatReply(LLMessageSystem* msg)
{
    U32 request_flags = 0, report_type = 0;
    msg->getU32Fast(_PREHASH_RequestData, _PREHASH_RequestFlags, request_flags);
    if (!(request_flags & REQUEST_MARKER))
        return false;
    msg->getU32Fast(_PREHASH_RequestData, _PREHASH_ReportType, report_type);

    WolfFloaterLagDetector* self = LLFloaterReg::findTypedInstance<WolfFloaterLagDetector>("wolf_lag_detector");
    if (self && self->getVisible())
        self->onReply(report_type, msg);
    return true;
}

// Source: llfloatertopobjects.cpp handleReply() for the block and field names.
void WolfFloaterLagDetector::onReply(U32 report_type, LLMessageSystem* msg)
{
    const F64 now = LLTimer::getTotalSeconds();
    const S32 blocks = msg->getNumberOfBlocksFast(_PREHASH_ReportData);
    const bool extended = msg->has("DataExtended");

    if (report_type == REPORT_COLLIDERS)
        mColliders.clear();
    else
        for (auto& [id, s] : mScripts)
            s.mSeen = false;

    for (S32 block = 0; block < blocks; ++block)
    {
        U32 local_id = 0;
        LLUUID task_id;
        F32 x = 0.f, y = 0.f, z = 0.f, score = 0.f;
        std::string name, owner, parcel;
        msg->getU32Fast(_PREHASH_ReportData, _PREHASH_TaskLocalID, local_id, block);
        msg->getUUIDFast(_PREHASH_ReportData, _PREHASH_TaskID, task_id, block);
        msg->getF32Fast(_PREHASH_ReportData, _PREHASH_LocationX, x, block);
        msg->getF32Fast(_PREHASH_ReportData, _PREHASH_LocationY, y, block);
        msg->getF32Fast(_PREHASH_ReportData, _PREHASH_LocationZ, z, block);
        msg->getF32Fast(_PREHASH_ReportData, _PREHASH_Score, score, block);
        msg->getStringFast(_PREHASH_ReportData, _PREHASH_TaskName, name, block);
        msg->getStringFast(_PREHASH_ReportData, _PREHASH_OwnerName, owner, block);
        if (extended)
            msg->getString("DataExtended", "ParcelName", parcel, block);
        // Owner names can have trailing spaces sent from server (llfloatertopobjects.cpp).
        LLStringUtil::trim(owner);
        owner = LLCacheName::buildUsername(owner);
        if (task_id.isNull())
            continue;

        if (report_type == REPORT_COLLIDERS)
        {
            Collider c;
            c.mID = task_id; c.mLocalID = local_id; c.mName = name; c.mOwner = owner;
            c.mParcel = parcel; c.mPos.set(x, y, z); c.mScore = score;
            mColliders.push_back(c);
            continue;
        }

        ScriptLoad& s = mScripts[task_id];
        const bool had = s.mLastTime > 0.0;
        // Score is the object's script time in ms since the region started. It only goes down
        // when the object was re-rezzed or its scripts recompiled - start that one over.
        if (had && score >= s.mLastScore && now > s.mLastTime)
        {
            const F32 fraction = (F32)((score - s.mLastScore) / ((now - s.mLastTime) * 1000.0));
            s.mFractions.push_back(llmax(0.f, fraction));
            while (s.mFractions.size() > WINDOW_INTERVALS)
                s.mFractions.pop_front();
        }
        else if (had)
        {
            s.mFractions.clear();
        }
        s.mLocalID = local_id; s.mName = name; s.mOwner = owner; s.mParcel = parcel;
        s.mPos.set(x, y, z); s.mLastScore = score; s.mLastTime = now; s.mSeen = true;
    }

    if (report_type == REPORT_SCRIPTS)
    {
        // Objects gone from the report (deleted, returned, or no longer running scripts).
        for (auto it = mScripts.begin(); it != mScripts.end(); )
            it = it->second.mSeen ? std::next(it) : mScripts.erase(it);
        ++mScriptReplies;
    }
    else
    {
        // BulletS returns its 25 in no order (BSScene.cs GetTopColliders sorts and discards it).
        std::sort(mColliders.begin(), mColliders.end(),
                  [](const Collider& a, const Collider& b) { return a.mScore > b.mScore; });
    }
    rebuild();
}

F32 WolfFloaterLagDetector::averageFraction(const ScriptLoad& s) const
{
    if (s.mFractions.empty())
        return 0.f;
    F32 sum = 0.f;
    for (F32 f : s.mFractions)
        sum += f;
    return sum / (F32)s.mFractions.size();
}

F32 WolfFloaterLagDetector::minFraction(const ScriptLoad& s) const
{
    return s.mFractions.empty() ? 0.f : *std::min_element(s.mFractions.begin(), s.mFractions.end());
}

std::string WolfFloaterLagDetector::percent(F32 fraction) const
{
    return llformat("%.0f%%", fraction * 100.f);
}

std::string WolfFloaterLagDetector::where(const std::string& owner, const std::string& parcel, const LLVector3& pos) const
{
    LLStringUtil::format_map_t args;
    args["[OWNER]"] = owner;
    args["[PARCEL]"] = parcel.empty() ? getString("unknown_parcel") : parcel;
    args["[POS]"] = llformat("%.0f, %.0f, %.0f", pos.mV[VX], pos.mV[VY], pos.mV[VZ]);
    return getString("where_line", args);
}

void WolfFloaterLagDetector::rebuild()
{
    if (!mList)
        return;

    const LLUUID selected = mList->getSelectedValue().asUUID();
    const S32 selected_index = mList->getFirstSelectedIndex();
    mList->deleteAllItems();
    mRows.clear();

    LLViewerRegion* region = gAgent.getRegion();
    const std::string region_name = region ? region->getName() : std::string();
    LLStringUtil::format_map_t base;
    base["[REGION]"] = region_name;

    // --- Scripts, per object -----------------------------------------------------------------
    std::vector<std::pair<LLUUID, const ScriptLoad*>> by_load;
    F32 total = 0.f;
    for (const auto& [id, s] : mScripts)
    {
        if (s.mFractions.empty())
            continue;
        by_load.emplace_back(id, &s);
        total += s.mFractions.back();
    }
    std::sort(by_load.begin(), by_load.end(), [this](const auto& a, const auto& b)
              { return averageFraction(*a.second) > averageFraction(*b.second); });

    auto add_object_row = [&](ERowKind kind, const LLUUID& id, const ScriptLoad& s, const std::string& title_key,
                              const std::string& detail_key, F32 fraction)
    {
        LLStringUtil::format_map_t args = base;
        args["[OBJECT]"] = s.mName;
        args["[PERCENT]"] = percent(fraction);
        args["[MINUTES]"] = llformat("%.0f", llmax(1.f, s.mFractions.size() * POLL_SECONDS / 60.f));
        Row row;
        row.mKind = kind; row.mObjectID = id; row.mLocalID = s.mLocalID; row.mPos = s.mPos;
        row.mObjectName = s.mName; row.mOwner = s.mOwner;
        row.mTitle = getString(title_key, args);
        row.mDetail = getString(detail_key, args) + "\n\n" + where(s.mOwner, s.mParcel, s.mPos);
        mRows.push_back(row);
    };

    auto held_back = [this](const ScriptLoad& s)
    {
        if (s.mFractions.size() < HELD_INTERVALS)
            return false;
        const F32 lo = minFraction(s);
        const F32 hi = *std::max_element(s.mFractions.begin(), s.mFractions.end());
        return lo >= HELD_MIN && lo < RUNAWAY_FRACTION && hi - lo <= HELD_SPREAD;
    };

    bool single_culprit = false;
    for (const auto& [id, s] : by_load)
    {
        if (s->mFractions.size() >= 2 && minFraction(*s) >= RUNAWAY_FRACTION)
        {
            add_object_row(ROW_RUNAWAY, id, *s, "runaway_title", "runaway_detail", averageFraction(*s));
            single_culprit = true;
        }
        else if (held_back(*s))
        {
            add_object_row(ROW_RUNAWAY, id, *s, "held_title", "held_detail", averageFraction(*s));
            single_culprit = true;
        }
    }
    for (const auto& [id, s] : by_load)
    {
        const F32 avg = averageFraction(*s);
        if (s->mFractions.size() >= 2 && avg >= HEAVY_FRACTION && minFraction(*s) < RUNAWAY_FRACTION
            && !held_back(*s))
        {
            add_object_row(ROW_HEAVY, id, *s, "heavy_title", "heavy_detail", avg);
            single_culprit = true;
        }
    }

    // --- Scripts, all together -----------------------------------------------------------------
    if (!single_culprit && mScriptReplies >= 2 && total >= SCRIPTS_TOTAL_FRACTION)
    {
        LLStringUtil::format_map_t args = base;
        args["[CORES]"] = llformat("%.1f", total);
        Row row;
        row.mKind = ROW_SCRIPTS_SUMMARY;
        row.mTitle = getString("scripts_total_title", args);
        row.mDetail = getString("scripts_total_detail", args);
        mRows.push_back(row);
        for (size_t i = 0; i < by_load.size() && i < LIST_AT_MOST; ++i)
            add_object_row(ROW_SCRIPTS_OBJECT, by_load[i].first, *by_load[i].second,
                           "scripts_object_title", "scripts_object_detail", averageFraction(*by_load[i].second));
    }

    // --- Physics -----------------------------------------------------------------------------
    // Same warning level the Lag Meter uses for the region's frame rate (llfloaterlagmeter.cpp
    // determineServer, LFSimFeatureHandler: SimulatorFPS x factor x SimulatorFPSWarnPercent).
    LLTrace::Recording& rec = LLTrace::get_frame_recording().getLastRecording();
    const F64 physics_fps = rec.getLastValue(LLStatViewer::SIM_PHYSICS_FPS);
    const F64 active_objects = rec.getLastValue(LLStatViewer::SIM_ACTIVE_OBJECTS);
    const F32 physics_warn = LFSimFeatureHandler::instance().simulatorFPSWarn();
    if (physics_fps > 0.0 && physics_fps < physics_warn)
    {
        LLStringUtil::format_map_t args = base;
        args["[PHYSFPS]"] = llformat("%.0f", physics_fps);
        args["[ACTIVE]"] = llformat("%.0f", active_objects);
        Row row;
        row.mKind = ROW_PHYSICS_SUMMARY;
        row.mTitle = getString("physics_title", args);
        row.mDetail = getString("physics_detail", args);
        mRows.push_back(row);
        size_t listed = 0;
        for (const Collider& c : mColliders)
        {
            if (c.mScore <= 0.f || listed >= LIST_AT_MOST)
                break;
            ++listed;
            LLStringUtil::format_map_t oargs = base;
            oargs["[OBJECT]"] = c.mName;
            Row orow;
            orow.mKind = ROW_PHYSICS_OBJECT; orow.mObjectID = c.mID; orow.mLocalID = c.mLocalID; orow.mPos = c.mPos;
            orow.mObjectName = c.mName; orow.mOwner = c.mOwner;
            orow.mTitle = getString("physics_object_title", oargs);
            orow.mDetail = getString("physics_object_detail", oargs) + "\n\n" + where(c.mOwner, c.mParcel, c.mPos);
            mRows.push_back(orow);
        }
    }

    const size_t problems = mRows.size();

    // --- Busy but not a problem (on request) ---------------------------------------------------
    if (mShowBusy && mShowBusy->get())
    {
        for (const auto& [id, s] : by_load)
        {
            const F32 avg = averageFraction(*s);
            if (avg >= BUSY_FRACTION && avg < HEAVY_FRACTION && !held_back(*s))
                add_object_row(ROW_BUSY, id, *s, "busy_title", "busy_detail", avg);
        }
    }

    // --- Rows into the list --------------------------------------------------------------------
    for (size_t i = 0; i < mRows.size(); ++i)
    {
        const Row& row = mRows[i];
        std::string level;
        switch (row.mKind)
        {
            case ROW_RUNAWAY:          level = getString("level_runaway"); break;
            case ROW_HEAVY:            level = getString("level_heavy"); break;
            case ROW_SCRIPTS_SUMMARY:
            case ROW_PHYSICS_SUMMARY:  level = getString("level_region"); break;
            case ROW_BUSY:             level = getString("level_busy"); break;
            default:                   level = std::string(); break;
        }
        LLSD element;
        // Summary rows have no object; index keeps every row's value unique for selection.
        element["id"] = row.mObjectID.notNull() ? LLSD(row.mObjectID) : LLSD((S32)i);
        element["columns"][0]["column"] = "level";
        element["columns"][0]["value"] = level;
        element["columns"][1]["column"] = "problem";
        element["columns"][1]["value"] = row.mTitle;
        mList->addElement(element);
    }
    if (selected.notNull())
        mList->selectByID(selected);
    else if (selected_index >= 0 && selected_index < mList->getItemCount())
        mList->selectNthItem(selected_index);

    // --- Verdict -------------------------------------------------------------------------------
    LLStringUtil::format_map_t args = base;
    args["[COUNT]"] = llformat("%d", (S32)problems);
    const F64 dilation = rec.getLastValue(LLStatViewer::SIM_TIME_DILATION);
    args["[SPEED]"] = llformat("%.0f", dilation * 100.0);
    std::string verdict;
    if (!region)
        verdict = getString("verdict_no_region", args);
    else if (!mCanManage)
        verdict = getString("verdict_not_manager", args);
    else if (mScriptReplies < 2)
        verdict = getString("verdict_measuring", args);
    else if (problems > 0)
        verdict = getString(problems == 1 ? "verdict_one_problem" : "verdict_problems", args);
    else if (dilation > 0.0 && dilation < 0.9)
        verdict = getString("verdict_slow_not_ours", args);
    else
        verdict = getString("verdict_fine", args);
    mVerdict->setText(verdict);

    updateButtons();
}

const WolfFloaterLagDetector::Row* WolfFloaterLagDetector::selectedRow() const
{
    const S32 index = mList ? mList->getFirstSelectedIndex() : -1;
    return (index >= 0 && index < (S32)mRows.size()) ? &mRows[index] : nullptr;
}

void WolfFloaterLagDetector::updateButtons()
{
    const Row* row = selectedRow();
    const bool object = row && row->mObjectID.notNull();
    mDetail->setText(row ? row->mDetail : getString("detail_none"));
    mBeaconBtn->setEnabled(object);
    mTeleportBtn->setEnabled(object);
    mReturnBtn->setEnabled(object);
    // The report carries only the owner's NAME, so ownership is read from the object itself
    // when it is loaded here; OpenSim refuses the delete anyway if it is not ours.
    LLViewerObject* vo = object ? gObjectList.findObject(row->mObjectID) : nullptr;
    mDeleteBtn->setEnabled(object && ((vo && vo->permYouOwner()) || gAgent.isGodlike()));
}

// Source: llfloatertopobjects.cpp showBeacon().
void WolfFloaterLagDetector::onBeacon()
{
    const Row* row = selectedRow();
    if (!row || row->mObjectID.isNull())
        return;
    LLTracker::trackLocation(gAgent.getPosGlobalFromAgent(row->mPos), row->mObjectName, std::string(), LLTracker::LOCATION_ITEM);
}

// Source: llfloatertopobjects.cpp onTeleportToObject().
void WolfFloaterLagDetector::onTeleport()
{
    const Row* row = selectedRow();
    if (!row || row->mObjectID.isNull())
        return;
    gAgent.teleportViaLocation(gAgent.getPosGlobalFromAgent(row->mPos));
}

void WolfFloaterLagDetector::onReturn()
{
    const Row* row = selectedRow();
    if (!row || row->mObjectID.isNull())
        return;
    const LLUUID object_id = row->mObjectID;
    LLSD args;
    args["OBJECT"] = row->mObjectName;
    args["OWNER"] = row->mOwner;
    LLNotificationsUtil::add("WolfLagDetectorReturn", args, LLSD(),
        [object_id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                sendReturn(object_id);
        });
}

void WolfFloaterLagDetector::onDelete()
{
    const Row* row = selectedRow();
    if (!row || row->mObjectID.isNull())
        return;
    const U32 local_id = row->mLocalID;
    LLSD args;
    args["OBJECT"] = row->mObjectName;
    LLNotificationsUtil::add("WolfLagDetectorDelete", args, LLSD(),
        [local_id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                sendDelete(local_id);
        });
}
