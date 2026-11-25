/**
 * @file wolftraincab.cpp
 * @brief WolfViewer: the Train Cab. See wolftraincab.h and region-terrain-work/wolf-trains-drivable-2026-11-13/DESIGN.md.
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

#include "wolftraincab.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>

#include "llagent.h"
#include "lldispatcher.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llstartup.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "wolfdrive.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfhelm.h"
#include "wolfhudkit.h"

static LLDefaultChildRegistry::Register<WolfTrainCabView> r_wolf_train_cab("wolf_train_cab");

namespace
{
    // Source: wolf-rail src/Cab.cs MESSAGE_OUT.
    const std::string MESSAGE_FROM_SIM("WolfTrainCab");
    // The region sends at least once a second while someone drives (Cab.cs FIRST_EVERY_MS, then every plan): a cab
    // that has heard nothing for this long has been let go (the engine left the region, the plugin was taken out).
    const F64 SILENT_CLOSE_S = 6.0;
    // [client review 13 Nov, W3] a cab closed by that silence and opened again for the same seat and train this soon keeps
    // its levers (a hand-over at a border that took longer than SILENT_CLOSE_S). The region gives the driver
    // EngineDriver.CROSS_GIVE_UP_MS (30 s) to be seated after a crossing; the levers are kept as long.
    const F64 REOPEN_KEEP_S = 30.0;
    // Easy driving's three notches. The plugin takes power as the share of the line speed it keeps
    // (EngineDriver.DrivenTarget: line = power x LineSpeed; any brake stops it). SLOW is a third of the line speed:
    // the Wolf Grid Railroad's Castle runs 11 m/s (wolf-rail README "the template's 3 m at 11 m/s"), so SLOW is about
    // 3.7 m/s there.
    const F32 NOTCH_POWER[WolfTrain::NOTCH_COUNT] = { 0.f, 1.f / 3.f, 1.f };
    // Driver's mode: one key press moves a lever a tenth (Up: brake off first, then power on; Down the other way).
    const F32 KEY_STEP = 0.1f;
    // [wolf-rail 1.2] AUTO's press on the wire: CRUISE held this long, so at least two of the region's plans see it
    // (EngineDriver.TICK 0.25 s; the "in" message goes out within wolfdrive.cpp SEND_MIN_GAP 0.1 s of a change). The region
    // switches on the bit coming on (EngineDriver.ReadDriver), so a longer hold is still one press.
    const F64 AUTO_PULSE_S = 0.6;
    // A choice the driver made here (AUTO pressed, a lever taken) wins over the region's mode for this long: its answer to
    // that press takes a plan and the way back, and the cab messages sent meanwhile still carry the old mode (four a second).
    const F64 AUTO_GRACE_S = 1.5;
    // [wolf-rail 1.2] the points a driver may change from the cab lie this close ahead (wolf-rail RailEngine.DRIVER_POINTS_M)
    const F32 DRIVER_POINTS_M = 200.f;
    // a points or signal name: one word of at most 32 characters (wolf-rail GuideSet.Markers)
    const size_t MAX_MARK_NAME = 32;
    // 1 m/s = 3.6 km/h; 1 mile = 1609.344 m exactly (the international yard and pound agreement, 1959), so 1 m/s =
    // 3600 / 1609.344 mph.
    const F32 MS_TO_KMH = 3.6f, MS_TO_MPH = 3600.f / 1609.344f;
    // [wolf-rail 1.3, stage V] after CHANGE ENDS the driver has this long to sit in the cab at the other end
    // (Source: wolf-rail Cab.cs CAB_WALK_MS 120_000; RailEngine.cs CommitEnds PendingUntilMs = now + Cab.CAB_WALK_MS)
    const F64 CAB_WALK_S = 120.0;
    // [wolf-rail 1.3.7] after a change of ends the lamp is drawn large this long: the region keeps its "You are now driving
    // from the other end ..." / "You have changed ends ..." note on the cab for 15 s (wolf-rail 1.3.7 protocol, 14 Nov)
    const F64 LEAD_FLASH_S = 15.0;
    // the starts of those two notes (the second also begins the 1.3 notes, RailEngine.cs:930-932)
    bool isEndsChangedNote(const std::string& note)
    {
        static const char* const P[] = { "You are now driving from the other end", "You have changed ends" };
        for (const char* p : P)
        {
            if (note.compare(0, strlen(p), p) == 0) return true;
        }
        return false;
    }

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // [stage V] The region's answers that refuse a SHUNT press, each the start of the note it sets. Source: wolf-rail
    // RailEngine.cs:2010 "The train cannot change ends: "; RailCouple.cs:322 / :332 "You may not couple / uncouple this
    // train: ", :324 "Put the reverser to forward or reverse ...", :326 / :334 / :335 "The train cannot couple / uncouple",
    // :130 "The train did not couple: ", :497 "The train was not ". These are kept on screen until the driver closes them.
    // [wolf-rail 1.3.7] a refused TURNTABLE press says "The train cannot be turned: ..." - already "The train cannot ".
    bool isRefusal(const std::string& note)
    {
        static const char* const P[] = { "The train cannot ", "You may not ", "The train did not ", "The train was not ",
                                         "Put the reverser to forward or reverse" };
        // [review W1] a second COUPLE press cancels the move: "The train did not couple: the driver cancelled it."
        // (RailCouple.cs:321 EndCouple("error: the driver cancelled it") -> :130) - the driver's own choice, no refusal
        if (note.find(": the driver cancelled it") != std::string::npos) return false;
        for (const char* p : P)
        {
            if (note.compare(0, strlen(p), p) == 0) return true;
        }
        return false;
    }

    // The value of `key=` among the parameters, or empty.
    std::string valueOf(const std::vector<std::string>& strings, const std::string& key)
    {
        for (const std::string& s : strings)
        {
            if (s.size() <= 254 && s.size() > key.size() && s.compare(0, key.size(), key) == 0 && s[key.size()] == '=') return s.substr(key.size() + 1);
        }
        return std::string();
    }

    // Source: wolfdrive.cpp seatRoot - the root of what the avatar sits on.
    LLViewerObject* seatRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting()) return nullptr;
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent()) p = (LLViewerObject*)p->getParent();
        return p;
    }

    // At most `max_bytes` of UTF-8, never cutting a character in two (review 13 Nov, W12): back off continuation bytes.
    std::string utf8Cut(const std::string& v, size_t max_bytes)
    {
        if (v.size() <= max_bytes) return v;
        size_t n = max_bytes;
        while (n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80) --n;
        return v.substr(0, n);
    }

    bool parseF(const std::string& v, F32& out)
    {
        if (v.empty() || v.size() > 16) return false;
        for (char c : v) if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) return false;   // plain decimals only
        char* end = nullptr;
        const double d = strtod(v.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(d)) return false;
        out = (F32)d;
        return true;
    }

    // Source: wolfdrive.cpp WolfDashboardPushHandler - registered with gGenericDispatcher; WolfTrain checks the grid,
    // the region and the seat.
    class WolfTrainCabHandler : public LLDispatchHandler
    {
    public:
        bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
        {
            WolfTrain::instance().receive(invoice, strings);
            return true;
        }
    };
    WolfTrainCabHandler sCabHandler;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the link
// ════════════════════════════════════════════════════════════════════════════════════════

WolfTrain::WolfTrain()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sCabHandler);
    }
}

WolfTrain::~WolfTrain()
{
}

// static
bool WolfTrain::onWolf()
{
    return WolfGrid::isOnWolfTerritories();
}

void WolfTrain::receive(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // Wolf Territories only, from the region the agent is in, while sitting on something (the dashboard's rules,
    // wolfdrive.cpp receiveDashboard: confirmed, not the lenient onWolf).
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != invoice) return;
    LLViewerObject* root = seatRoot();
    if (strings.size() > 32) return;
    if (!root)
    {
        // [client review 13 Nov, W1] no seat (stood up, or a moment mid-crossing): only "state=off" is read - the region
        // lets a driver who stood up go at once (wolf-rail RailEngine.LetDriverGo), and the cab now waits for it
        for (const std::string& s : strings)
        {
            if (s == "state=off")
            {
                // [stage V] the state=off of CHANGE ENDS carries walk= and its note (SceneRailWorld.cs CabOff :242-245)
                const std::string walk = utf8Cut(valueOf(strings, "walk"), 254);
                if (mOpen && !walk.empty()) beginWalk(mCab.mTrain, utf8Cut(valueOf(strings, "note"), 254), walk);
                else if (mOpen) close();
                return;
            }
        }
        return;
    }

    Cab c = mOpen && root->getID() == mSeat ? mCab : Cab();
    // [wolf-rail 1.3.7] tt= says a turntable only while the engine stands at one; every state=on message is whole
    // (RailEngine.cs SendCab :1207-1226 builds every key each time), so a message without it is no turntable
    c.mTt.clear();
    bool on = false, off = false;
    for (const std::string& s : strings)
    {
        if (s.size() > 254) continue;   // never trust the wire: one parameter is at most 254 bytes (Cab.cs MAX_PARAM_BYTES)
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        F32 n = 0.f;
        if (k == "state") { on = v == "on"; off = v == "off"; }
        else if (k == "train") c.mTrain = utf8Cut(v, 254);
        else if (k == "kind") { if (v.empty() || v == "steam" || v == "diesel" || v == "electric" || v == "highspeed" || v == "shunter" || v == "tram") c.mKind = v; }
        else if (k == "mode") { if (v == "driver" || v == "easy" || v == "auto" || v == "script" || v == "timetable") c.mMode = v; }
        else if (k == "stop") { if (v.empty() || v == "station" || v == "stop") c.mStop = v; }
        else if (k == "region") c.mRegion = utf8Cut(v, 254);
        else if (k == "note") c.mNote = utf8Cut(v, 254);
        else if (k == "sig")
        {
            // [wolf-rail 1.2] "aspect|metres", or empty: no signal on the line laid
            c.mSig.clear(); c.mSigM = -1.f;
            const size_t bar = v.find('|');
            F32 m = 0.f;
            if (bar != std::string::npos && parseF(v.substr(bar + 1), m))
            {
                const std::string a = v.substr(0, bar);
                if (a == "red" || a == "yellow" || a == "green") { c.mSig = a; c.mSigM = llclamp(m, 0.f, 100000.f); }
            }
        }
        else if (k == "pts")
        {
            // [wolf-rail 1.2] "name|setting|metres|driver", or empty: no points on the line laid
            c.mPts.clear(); c.mPtsSet.clear(); c.mPtsM = -1.f; c.mPtsDriver = false;
            std::vector<std::string> f;
            size_t from = 0;
            while (f.size() < 5)
            {
                const size_t bar = v.find('|', from);
                f.push_back(v.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            F32 m = 0.f;
            if (f.size() == 4 && !f[0].empty() && utf8str_to_wstring(f[0]).size() <= MAX_MARK_NAME && (f[1] == "left" || f[1] == "right" || f[1] == "auto")
                && parseF(f[2], m) && (f[3] == "0" || f[3] == "1"))
            {
                c.mPts = f[0]; c.mPtsSet = f[1]; c.mPtsM = llclamp(m, 0.f, 100000.f); c.mPtsDriver = f[3] == "1";
            }
        }
        else if (k == "couple")
        {
            // [wolf-rail 1.3 stage 3] "ahead|metres|name" or "behind|metres|name", or empty: no stock of the owner's on the
            // line laid (RailEngine.cs CoupleKey: side + "|" + metres "0.0" + "|" + the train's name, cut by Cab.Fit). The
            // name is the rest after the second bar (a train name may hold a bar itself).
            c.mCoupleSide.clear(); c.mCoupleName.clear(); c.mCoupleM = -1.f;
            const size_t b1 = v.find('|');
            const size_t b2 = b1 == std::string::npos ? std::string::npos : v.find('|', b1 + 1);
            F32 m = 0.f;
            if (b2 != std::string::npos && parseF(v.substr(b1 + 1, b2 - b1 - 1), m))
            {
                const std::string side = v.substr(0, b1);
                if (side == "ahead" || side == "behind") { c.mCoupleSide = side; c.mCoupleM = llclamp(m, 0.f, 100000.f); c.mCoupleName = v.substr(b2 + 1); }
            }
        }
        else if (k == "tt")
        {
            // [wolf-rail 1.3.7] the turntable's name: one word of 1-32 characters, no space and no '|' (the names a region
            // keeps, Turntable.cs:841 ReadDecks); anything else is no turntable
            const size_t len = utf8str_to_wstring(v).size();
            c.mTt = len >= 1 && len <= MAX_MARK_NAME && v.find(' ') == std::string::npos && v.find('|') == std::string::npos ? v : std::string();
        }
        else if (!parseF(v, n)) continue;
        // [wolf-rail 1.3 stage 1 / 3] RailEngine.cs SendCab :1215-1218: "0" / "1", and the vehicle count
        else if (k == "lead")    { c.mHasLead = true; c.mLead = n >= 0.5f; }
        else if (k == "ends")    c.mEnds = n >= 0.5f;
        else if (k == "veh")     c.mVeh = (S32)llround(llclamp(n, 0.f, 100000.f));
        else if (k == "shunt")   c.mShunt = n >= 0.5f;
        else if (k == "speed")   c.mSpeed = llclamp(n, 0.f, 100.f);
        else if (k == "line")    c.mLine = llclamp(n, 0.f, 100.f);
        else if (k == "power")   c.mPower = llclamp(n, 0.f, 1.f);
        else if (k == "brake")   c.mBrake = llclamp(n, 0.f, 1.f);
        else if (k == "rev")     c.mRev = llclamp((S32)llround(n), -1, 1);
        else if (k == "stopm")   c.mStopM = llclamp(n, -1.f, 100000.f);
        else if (k == "ahead")   c.mAhead = llclamp(n, 0.f, 100000.f);
        else if (k == "protect") c.mProtect = n >= 0.5f;
        else if (k == "back")    c.mBack = n >= 0.5f;   // [wolf-rail 1.2]
    }
    if (off)
    {
        if (mOpen && root->getID() == mSeat)
        {
            // [stage V] CHANGE ENDS: "Walk to <vehicle> and sit in its cab to drive on." with walk=<vehicle>
            // (RailEngine.cs CommitEnds :907-909, SceneRailWorld.cs CabOff); without walk= it is the cab let go
            const std::string walk = utf8Cut(valueOf(strings, "walk"), 254);
            if (!walk.empty()) beginWalk(c.mTrain, c.mNote, walk);
            else close();
        }
        return;
    }
    if (!on) return;
    const bool fresh = !mOpen || root->getID() != mSeat;
    // [stage V] a refusal of a SHUNT press: kept for the driver to read when the region first says it (the region's note
    // stays 4 s and is sent again with every message meanwhile: only a new text is a new refusal)
    const std::string before = fresh ? std::string() : mCab.mNote;
    // [review W2] after a press, the same refusal said again is the answer to that press: once the press is over on the
    // wire (mRearmAt, its AUTO_PULSE_S; the region reads a press every 0.25 s plan, EngineDriver.TICK) it is raised again
    const bool again = mRearmAt >= 0.0 && now() >= mRearmAt;
    const bool refused = !c.mNote.empty() && (c.mNote != before || again) && isRefusal(c.mNote);
    if (again || c.mNote != before) mRearmAt = -1.0;
    // [wolf-rail 1.3.7] a change of ends: its note new, or lead= turned round while this cab is open
    const bool leadTurned = !fresh && c.mHasLead && mCab.mHasLead && c.mLead != mCab.mLead;
    const bool endsNote = c.mNote != before && isEndsChangedNote(c.mNote);
    mCab = c;
    mSeat = root->getID();
    mLastAt = now();
    if (fresh)
    {
        mOpen = true;
        mHidden = false;
        mYielded = false;
        mByHand = false;
        mHornHeld = false;
        // [stage V] after CHANGE ENDS: the same train's cab at the other end (another seat) within CAB_WALK_S keeps them too
        // (DESIGN-terminus.md 5.4); the region still asks for the power to be let go once (RailEngine.cs PendingCheck NeedRelease)
        const bool keep = mKept.at >= 0.0 && mKept.train == c.mTrain && now() >= mKept.at
            && (mKept.walk ? now() - mKept.at <= CAB_WALK_S : (mKept.seat == mSeat && now() - mKept.at <= REOPEN_KEEP_S));
        mAlert.clear();
        dismissWalk();
        mEndsPulseUntil = mCouplePulseUntil = mUncouplePulseUntil = -1.0;
        mTtPulseUntil = -1.0;   // [wolf-rail 1.3.7]
        // [wolf-rail 1.3.7] a cab opened within the walk after CHANGE ENDS: the driver has just changed ends
        mLeadFlashUntil = keep && mKept.walk ? now() + LEAD_FLASH_S : -1.0;
        if (keep)
        {
            // the same train after a long hand-over (W3): its levers as the driver left them
            mNotch = mKept.notch;
            mPower = mKept.power;
            mBrake = mKept.brake;
            mRev = mKept.rev;
            mEasyBack = mKept.easyBack;
        }
        else
        {
            // A new cab starts on the brake, going forward, in the driver's chosen mode (WolfTrainCabEasy).
            mNotch = NOTCH_STOP;
            mPower = 0.f;
            mBrake = 1.f;
            mRev = 1;
            mEasyBack = false;
        }
        mKept = Levers();
        mAutoWant = false;
        mAutoAskUntil = -1.0;
        claimBottom();
    }
    if (refused) mAlert = c.mNote;
    if (leadTurned || endsNote) mLeadFlashUntil = now() + LEAD_FLASH_S;   // [wolf-rail 1.3.7]
    // [wolf-rail 1.2] the region's word on Auto (its own, or a script's train.control [train, "auto"]).
    // [client review 13 Nov, W3] An AUTO press waiting for its answer (autoAsking) takes the first mode=auto as the yes; a
    // mode that is not "auto" meanwhile may still be a message sent before the region read the press, so it waits until
    // AUTO_GRACE_S is over - and then the levers are simply the driver's as they were (the press never changed them).
    // After the driver let Auto go here (an off press, a lever: mAutoChosenAt) the region's old "auto" is not believed for
    // AUTO_GRACE_S either.
    if (c.mMode == "auto")
    {
        if (fresh || autoAsking() || now() - mAutoChosenAt > AUTO_GRACE_S)
        {
            if (!mAutoWant) autoLetGo();
            mAutoWant = true;
            mAutoAskUntil = -1.0;
        }
    }
    else if (!autoAsking() && (fresh || now() - mAutoChosenAt > AUTO_GRACE_S))
    {
        mAutoWant = false;
    }
}

void WolfTrain::close()
{
    mKept = Levers();   // only the silence rule keeps the levers (idle)
    mOpen = false;
    mHidden = false;
    mYielded = false;
    mSeat.setNull();
    mCab = Cab();
    mHornHeld = false;
    mAutoWant = false;
    mAutoAskUntil = -1.0;
    mAutoPulseUntil = -1.0;
    mPointsPulseUntil = -1.0;
    mEndsPulseUntil = mCouplePulseUntil = mUncouplePulseUntil = -1.0;   // [stage V]
    mTtPulseUntil = -1.0;   // [wolf-rail 1.3.7]
    mLeadFlashUntil = -1.0;
    mAlert.clear();
    mRearmAt = -1.0;
    dismissWalk();
}

// The levers as they are now, for a cab of the same train that opens soon after (the silence rule, or [stage V] the walk
// after CHANGE ENDS). Taken before close(), which forgets them; put back after it.
void WolfTrain::keepLevers(bool walk)
{
    mKept.seat = mSeat; mKept.train = mCab.mTrain; mKept.at = now(); mKept.walk = walk;
    mKept.notch = mNotch; mKept.power = mPower; mKept.brake = mBrake; mKept.rev = mRev; mKept.easyBack = mEasyBack;
}

// [stage V] CHANGE ENDS done: the cab closes; the region's note ("Walk to <vehicle> and sit in its cab to drive on.") stays
// on screen while the driver walks, and the levers wait for the cab at the other end (DESIGN-terminus.md 5.1 step 3, 5.4).
void WolfTrain::beginWalk(const std::string& train, const std::string& note, const std::string& walk)
{
    keepLevers(true);
    if (!train.empty()) mKept.train = train;   // the state=off names the train (SceneRailWorld.cs CabOff "train=")
    const Levers kept = mKept;
    close();
    mKept = kept;
    // the note is the region's own words; were it ever missing, the same sentence from walk= (RailEngine.cs:908)
    mWalkNote = note.empty() ? "Walk to " + walk + " and sit in its cab to drive on." : note;
    mWalkUntil = now() + CAB_WALK_S;
}

bool WolfTrain::leadFlash() const
{
    return driving() && mCab.mHasLead && now() < mLeadFlashUntil;
}

bool WolfTrain::walkPending() const
{
    return !mWalkNote.empty() && now() < mWalkUntil && !mOpen;
}

void WolfTrain::claimBottom()
{
    // Source: wolfhelm.cpp WolfHelm::claimBottom - one bottom panel at a time: the dashboard closes, the flight /
    // sailing deck is hidden (its mode keeps running), the Captain's Helm closes.
    if (WolfDrive::instance().dashboardOn()) WolfDrive::instance().requestDashboard(false);
    WolfFlight& wf = WolfFlight::instance();
    if (wf.active() && !wf.deckHidden()) wf.toggleDeck(wf.sailing());
    if (WolfHelm::instance().hudShowing()) WolfHelm::instance().showHud(false);
}

bool WolfTrain::driving() const
{
    if (!mOpen || !WolfGrid::isOnWolfTerritoriesConfirmed()) return false;
    LLViewerObject* root = seatRoot();
    return root && root->getID() == mSeat;
}

bool WolfTrain::shown() const
{
    if (!onWolf()) return false;
    return (driving() && !mHidden) || mByHand;
}

void WolfTrain::toggleShown()
{
    if (!onWolf()) return;
    if (driving())
    {
        mHidden = !mHidden;
        mYielded = false;   // the driver's own choice now
        if (!mHidden) claimBottom();
        return;
    }
    mByHand = !mByHand;
    if (mByHand) claimBottom();
}

void WolfTrain::idle()
{
    if (LLStartUp::getStartupState() < STATE_STARTED) return;
    if (!onWolf())
    {
        // [WOLF ONLY] left Wolf: the cab goes, nothing more is read or sent
        if (mOpen) close();
        mByHand = false;
        dismissWalk();   // [stage V]
        return;
    }
    if (mWalkUntil >= 0.0 && now() >= mWalkUntil) dismissWalk();   // [stage V] the region's CAB_WALK_MS is over
    if (mOpen)
    {
        LLViewerObject* root = seatRoot();
        // A border crossed with the engine is NOT a reason to close (Paul 13 Nov: "trains should run across regions"): the
        // next region takes the driver over and its cab follows with the levers as they are; if no region drives the
        // train there, nothing more comes and the silence rule closes it (re-review S5: a child agent hears no state=off).
        // [client review 13 Nov, W1] mid-crossing the avatar's seat is not known for a moment (no root): that alone does
        // not close the cab - only sitting on something else does at once; with no seat it waits for the silence rule (a
        // driver who really stood up is told state=off by the region: wolf-rail RailEngine.LetDriverGo)
        if (root && root->getID() != mSeat)
        {
            close();   // sat on something else
            return;
        }
        if (now() - mLastAt > SILENT_CLOSE_S)
        {
            // the region stopped saying (the region stops the train itself): keep the levers in case the same train's
            // cab comes back from the next region a moment later (W3)
            keepLevers(false);
            const Levers kept = mKept;
            close();
            mKept = kept;
            return;
        }
    }
    // One bottom panel: the dashboard, the deck or the helm coming on takes the bottom from the cab; when it goes again,
    // a cab that only gave way comes back (review 13 Nov, W5).
    const WolfFlight& wf = WolfFlight::instance();
    const bool other = WolfDrive::instance().dashboardOn() || (wf.active() && !wf.deckHidden()) || WolfHelm::instance().hudShowing();
    if (shown() && other)
    {
        if (driving()) { mHidden = true; mYielded = true; }
        mByHand = false;
    }
    else if (!other && driving() && mHidden && mYielded)
    {
        mHidden = false;
        mYielded = false;
    }
}

bool WolfTrain::easy() const
{
    return gSavedSettings.getBOOL("WolfTrainCabEasy");
}

void WolfTrain::setEasy(bool on)
{
    takeBack();   // [wolf-rail 1.2] the new mode's levers start on the brake: that takes the train back from Auto
    gSavedSettings.setBOOL("WolfTrainCabEasy", on);
    // Changing mode never lets the train run on by itself: both start on the brake (Easy driving going forward).
    mNotch = NOTCH_STOP;
    mPower = 0.f;
    mBrake = 1.f;
    mEasyBack = false;
}

void WolfTrain::setNotch(S32 n)
{
    takeBack();
    mNotch = llclamp(n, 0, (S32)NOTCH_COUNT - 1);
}

void WolfTrain::setPower(F32 v)
{
    takeBack();
    mPower = llclamp(v, 0.f, 1.f);
}

void WolfTrain::setBrake(F32 v)
{
    takeBack();
    mBrake = llclamp(v, 0.f, 1.f);
}

// [wolf-rail 1.2] A lever taken while the train drives itself: the cab sends the lever as it now is, and the region takes
// Auto off on seeing it move (EngineDriver.ReadDriver).
void WolfTrain::takeBack()
{
    if (!mAutoWant && !autoAsking()) return;
    mAutoWant = false;
    mAutoAskUntil = -1.0;   // a lever taken while the press waits for its answer: the driver has the train
    mAutoChosenAt = now();
}

bool WolfTrain::autoAsking() const
{
    return now() < mAutoAskUntil;
}

// [wolf-rail 1.2] The region has switched Auto on (mode=auto): the levers to their let-go position, as they are then sent
// (sendPower / sendBrake). The region takes Auto back when power or brake move from where they were when it came on, and
// not while both are let go (EngineDriver.ReadDriver: `Auto && levers && moved`), so the levers sent as let go never take
// it back. Easy driving's lever has no let-go notch: it goes to STOP, which is not sent while Auto is on, so Auto ending
// for any other reason (the end of a run, a script) leaves the train on the brake, never at GO.
void WolfTrain::autoLetGo()
{
    mPower = 0.f;
    mBrake = 0.f;
    mNotch = NOTCH_STOP;
}

void WolfTrain::pressAuto()
{
    if (!driving()) return;
    if (autoAsking()) return;   // one press is on its way: a second is not another switch
    mAutoPulseUntil = now() + AUTO_PULSE_S;
    if (mAutoWant)
    {
        // off again: the levers are already let go (autoLetGo); in the driver's mode the train keeps its speed and the
        // driver takes it from there, in Easy driving the lever is at STOP and the train brakes to a stand
        mAutoWant = false;
        mAutoChosenAt = now();
        return;
    }
    // on: [client review 13 Nov, W3] the levers stay exactly as the driver has them until the region says mode=auto
    // (receive); the region switches Auto on only with neither lever held, and otherwise says so in the note ("Let both
    // levers go, then press AUTO.").
    mAutoAskUntil = now() + AUTO_GRACE_S;
}

bool WolfTrain::canChangePoints() const
{
    return driving() && !mCab.mPts.empty() && mCab.mPtsDriver && mCab.mPtsM >= 0.f && mCab.mPtsM <= DRIVER_POINTS_M && !mCab.mBack;
}

void WolfTrain::pressPoints()
{
    // [Paul 13 Nov, D-b] one press, no question: STAGE held as AUTO's is (wolf-rail Cab.cs BTN_POINTS 512; the region changes
    // the next points a driver may set on the bit coming on, EngineDriver.ReadDriver / RailEngine "toggle"), and puts the new
    // setting in the cab's note ("Points P1 set left.")
    if (!canChangePoints() || now() < mPointsPulseUntil) return;
    mPointsPulseUntil = now() + AUTO_PULSE_S;
}

// [wolf-rail 1.3, stage V] DESIGN-terminus.md 5.4: each SHUNT button only when it can act.
bool WolfTrain::canChangeEnds() const
{
    return driving() && mCab.mEnds;
}

bool WolfTrain::canCouple() const
{
    return driving() && mCab.mShunt && !mCab.mCoupleSide.empty();
}

bool WolfTrain::canUncouple() const
{
    return driving() && mCab.mShunt && mCab.mVeh > 0;
}

// [wolf-rail 1.3.7] only while the region names the turntable whose deck-edge stop the engine stands at (tt=)
bool WolfTrain::canTurntable() const
{
    return driving() && !mCab.mTt.empty();
}

bool WolfTrain::coupleWrongWay() const
{
    // RailCouple.cs CabCouple: the record's way = the reverser x TrainRecord.CabSign; CoupleSeen's "ahead" is the record's
    // way x CabSign > 0, so "ahead" is reached with the reverser at FORWARD and "behind" with it at REVERSE (gear R).
    // What this cab sends as its reverser is sendGear (Easy driving: FORWARD 1, BACK R; Auto: forward).
    if (mCab.mCoupleSide.empty()) return false;
    const S32 g = sendGear();
    return mCab.mCoupleSide == "ahead" ? g <= 0 : g >= 0;
}

// Each one press as POINTS is (EngineDriver.cs ReadDriver :1845-1855: the bit coming on), held AUTO_PULSE_S so a plan sees it.
void WolfTrain::pressEnds()
{
    if (!canChangeEnds() || now() < mEndsPulseUntil) return;
    mAlert.clear();
    mEndsPulseUntil = now() + AUTO_PULSE_S;
    mRearmAt = mEndsPulseUntil;
}

void WolfTrain::pressCouple()
{
    // (pressed again while it couples: the region cancels the move, RailCouple.cs CabCouple; couple= is still sent then)
    if (!canCouple() || now() < mCouplePulseUntil) return;
    mAlert.clear();
    mCouplePulseUntil = now() + AUTO_PULSE_S;
    mRearmAt = mCouplePulseUntil;
}

void WolfTrain::pressUncouple()
{
    if (!canUncouple() || now() < mUncouplePulseUntil) return;
    mAlert.clear();
    mUncouplePulseUntil = now() + AUTO_PULSE_S;
    mRearmAt = mUncouplePulseUntil;
}

// [wolf-rail 1.3.7] the region turns the engine round on the turntable in tt= on BOOST coming on (Cab.cs:79 "One press (the
// bit coming on), as CHANGE ENDS"); a refusal ("The train cannot be turned: ...") is raised again after this press as the others'
void WolfTrain::pressTurntable()
{
    if (!canTurntable() || now() < mTtPulseUntil) return;
    mAlert.clear();
    mTtPulseUntil = now() + AUTO_PULSE_S;
    mRearmAt = mTtPulseUntil;
}

void WolfTrain::setEasyBack(bool back)
{
    // [wolf-rail 1.2, D-c] chosen at any time; the region changes the way the train goes only from a stand and otherwise
    // says "Stop the train before you change direction." in the note. Not while Auto drives (Auto runs forward: the
    // choice is under the AUTO card and is not sent then).
    if (mAutoWant) return;
    mEasyBack = back;
}

void WolfTrain::setReverser(S32 r)
{
    mRev = llclamp(r, -1, 1);
}

bool WolfTrain::mph() const
{
    return gSavedSettings.getBOOL("WolfTrainCabMph");
}

void WolfTrain::setMph(bool on)
{
    gSavedSettings.setBOOL("WolfTrainCabMph", on);
}

// [wolf-rail 1.2] Both levers let go on the wire: in Auto, and - in Easy driving only - for an AUTO press waiting for its
// answer. Easy driving's lever has no let-go notch (STOP is the brake, SLOW and GO are power), so without this the region
// could never switch Auto on from it ("Let both levers go"); in Easy driving no power is a stop, never a run on (wolf-rail
// EngineDriver.DrivenTarget: the target is power x the line speed, or the slow backing share), so the train only slows
// meanwhile. The notch itself is not changed and is sent again if the region does not switch Auto on. In the driver's mode
// the levers are sent exactly as they are until the region says mode=auto (client review 13 Nov, W3: no brake released while
// the region has not agreed, e.g. while backing).
static bool letGoOnWire(const WolfTrain& t) { return t.autoOn() || (t.easy() && t.autoAsking()); }

F32 WolfTrain::sendPower() const
{
    if (letGoOnWire(*this)) return 0.f;
    return easy() ? NOTCH_POWER[mNotch] : mPower;
}

F32 WolfTrain::sendBrake() const
{
    if (letGoOnWire(*this)) return 0.f;
    return easy() ? (mNotch == NOTCH_STOP ? 1.f : 0.f) : mBrake;
}

S32 WolfTrain::sendGear() const
{
    // Source: wolfdrive.h:74 GEAR_R -1, GEAR_N 0, 1 the first forward gear; the plugin reads -1 R, 0 N, >= 1 forward
    // (wolf-rail Cab.cs DriveControls.Reverser). Auto runs forward (EngineDriver.Direction).
    if (mAutoWant) return 1;
    // [wolf-rail 1.2, D-c] Easy driving's BACK: the very gear the driver's mode sends for REVERSE (below), WolfDrive::GEAR_R
    if (easy()) return mEasyBack ? WolfDrive::GEAR_R : 1;
    return mRev > 0 ? 1 : mRev < 0 ? WolfDrive::GEAR_R : WolfDrive::GEAR_N;
}

U32 WolfTrain::sendButtons() const
{
    U32 b = 0;
    if (mHornHeld) b |= WolfDrive::BTN_HORN;
    if (easy()) b |= WolfDrive::BTN_ASSIST;   // the plugin's Easy driving (Cab.cs BTN_ASSIST, held while it is on)
    if (now() < mAutoPulseUntil) b |= WolfDrive::BTN_CRUISE;   // [wolf-rail 1.2] AUTO pressed (Cab.cs BTN_AUTO 256)
    if (now() < mPointsPulseUntil) b |= WolfDrive::BTN_STAGE;  // [wolf-rail 1.2] POINTS pressed (Cab.cs BTN_POINTS 512)
    // [wolf-rail 1.3, stage V] wolfdrive.h BTN_ENGINE 16384 / BTN_ANCHOR 2048 / BTN_LANDING_GEAR 8192 = wolf-rail Cab.cs
    // BTN_ENDS / BTN_COUPLE / BTN_UNCOUPLE (IWolfDriveModule.cs:29-30), all inside wolfdrive.h:85 BUTTON_MASK 0x7FFFF
    if (now() < mEndsPulseUntil) b |= WolfDrive::BTN_ENGINE;
    if (now() < mCouplePulseUntil) b |= WolfDrive::BTN_ANCHOR;
    if (now() < mUncouplePulseUntil) b |= WolfDrive::BTN_LANDING_GEAR;
    // [wolf-rail 1.3.7] wolfdrive.h:82 BTN_BOOST 1024 = wolf-rail Cab.cs:82 BTN_TURNTABLE, inside BUTTON_MASK 0x7FFFF (wolfdrive.h:85)
    if (now() < mTtPulseUntil) b |= WolfDrive::BTN_BOOST;
    return b;
}

// static
bool WolfTrain::isCabKey(KEY key)
{
    return key == KEY_UP || key == 'W' || key == KEY_DOWN || key == 'S' || key == KEY_F5 || key == KEY_F2 || key == KEY_F3 || key == KEY_F4
        || key == KEY_F8 || key == KEY_F9    // [wolf-rail 1.2] AUTO, POINTS
        || key == KEY_F6 || key == KEY_F7    // [wolf-rail 1.3, stage V] CHANGE ENDS, COUPLE
        || key == KEY_ESCAPE;                // [stage V, review W4] closes the refusal strip (watched only: Escape still does its own job)
}

void WolfTrain::noteShiftKey(KEY key, bool key_down, bool repeat)
{
    if (!key_down || repeat || !driving()) return;
    if (key == KEY_F7) pressUncouple();   // [stage V] Shift+F7 UNCOUPLE (DESIGN-terminus.md 5.4)
}

void WolfTrain::noteKey(KEY key, bool key_down, bool repeat)
{
    if (!key_down || !driving()) return;
    if (key == KEY_ESCAPE) { if (!repeat) dismissAlert(); return; }   // [stage V, review W4] the keyboard's OK
    if (key == KEY_F5 || key == KEY_F2 || key == KEY_F3 || key == KEY_F4 || key == KEY_F8 || key == KEY_F9 || key == KEY_F6 || key == KEY_F7)
    {
        if (repeat) return;   // one press, one step
        if (key == KEY_F6) pressEnds();          // [stage V]
        else if (key == KEY_F7) pressCouple();   // [stage V]
        else if (key == KEY_F8) pressAuto();
        else if (key == KEY_F9) pressPoints();   // one press (D-b)
        else if (key == KEY_F5 && easy()) setEasyBack(!mEasyBack);   // [D-c] Easy driving: forward <-> back
        else if (key == KEY_F5) setReverser(mRev > 0 ? 0 : mRev == 0 ? -1 : 1);   // forward -> neutral -> reverse -> forward
        else if (key == KEY_F2) setEasy(!easy());
        else if (key == KEY_F3) setMph(!mph());
        else toggleShown();
        return;
    }
    const bool up = key == KEY_UP || key == 'W';
    const bool down = key == KEY_DOWN || key == 'S';
    if (!up && !down) return;
    if (easy())
    {
        if (!repeat) setNotch(mNotch + (up ? 1 : -1));
        return;
    }
    // whole tenths, so ten presses always come back to exactly 0 (review 13 Nov, W6: no float drift)
    auto step = [](F32 v, F32 d) { return (F32)llround((v + d) * 10.f) / 10.f; };
    if (up)
    {
        if (mBrake > 0.f) setBrake(step(mBrake, -KEY_STEP));
        else setPower(step(mPower, KEY_STEP));
    }
    else
    {
        if (mPower > 0.f) setPower(step(mPower, -KEY_STEP));
        else setBrake(step(mBrake, KEY_STEP));
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the HUD
// ════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    const F32 PI_F = 3.14159265f;
    const LLColor4 C_RED(1.00f, 0.13f, 0.10f, 1.f);      // ISO 2575 status colours, as the dashboard's
    const LLColor4 C_AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 C_GREEN(0.20f, 0.95f, 0.30f, 1.f);
    const LLColor4 C_WHITE(0.96f, 0.96f, 0.96f, 1.f);
    // [WOLF HUD LOOKS 2026-11-25] lighter, so words in it keep 4.5:1 on every cab's lightest face (the tram's 0.36,0.21,0.10:
    // 5.3:1; it was 4.2:1); C_DIM stays for marks that are off, never for words that say something
    const LLColor4 C_GREY(0.74f, 0.72f, 0.68f, 1.f);
    const LLColor4 C_DIM(0.36f, 0.33f, 0.30f, 1.f);

    /** Each kind's cab (design): a steam footplate in brass and Brunswick green, a diesel desk in charcoal and amber,
     *  an electric or high-speed desk in navy and cyan, a shunter in yellow and black, a tram in varnished wood. */
    struct Theme { LLColor4 hi, lo, inset, accent, accentLo, label; const char* power; const char* horn; const char* name; };
    Theme themeFor(const std::string& kind)
    {
        if (kind == "diesel")    return { LLColor4(0.17f,0.17f,0.18f,1.f), LLColor4(0.05f,0.05f,0.06f,1.f), LLColor4(0.02f,0.02f,0.03f,1.f), LLColor4(1.f,0.70f,0.15f,1.f), LLColor4(0.45f,0.28f,0.04f,1.f), C_WHITE, "POWER", "HORN", "DIESEL" };
        if (kind == "electric")  return { LLColor4(0.08f,0.13f,0.24f,1.f), LLColor4(0.02f,0.04f,0.09f,1.f), LLColor4(0.01f,0.02f,0.05f,1.f), LLColor4(0.30f,0.85f,1.f,1.f), LLColor4(0.06f,0.30f,0.45f,1.f), C_WHITE, "POWER", "HORN", "ELECTRIC" };
        if (kind == "highspeed") return { LLColor4(0.10f,0.12f,0.20f,1.f), LLColor4(0.02f,0.03f,0.07f,1.f), LLColor4(0.01f,0.01f,0.04f,1.f), LLColor4(0.45f,0.95f,0.95f,1.f), LLColor4(0.08f,0.35f,0.40f,1.f), C_WHITE, "POWER", "HORN", "HIGH-SPEED" };
        if (kind == "shunter")   return { LLColor4(0.20f,0.19f,0.15f,1.f), LLColor4(0.07f,0.07f,0.05f,1.f), LLColor4(0.03f,0.03f,0.02f,1.f), LLColor4(1.f,0.82f,0.10f,1.f), LLColor4(0.45f,0.36f,0.02f,1.f), C_WHITE, "POWER", "HORN", "SHUNTER" };
        if (kind == "tram")      return { LLColor4(0.36f,0.21f,0.10f,1.f), LLColor4(0.15f,0.08f,0.03f,1.f), LLColor4(0.08f,0.04f,0.02f,1.f), LLColor4(0.95f,0.88f,0.68f,1.f), LLColor4(0.45f,0.35f,0.18f,1.f), C_WHITE, "CONTROLLER", "GONG", "TRAM" };
        // steam, and a train that does not say what it is
        return { LLColor4(0.06f,0.20f,0.13f,1.f), LLColor4(0.02f,0.07f,0.04f,1.f), LLColor4(0.01f,0.04f,0.02f,1.f), LLColor4(0.86f,0.69f,0.34f,1.f), LLColor4(0.40f,0.28f,0.11f,1.f), C_WHITE, "REGULATOR", "WHISTLE", kind == "steam" ? "STEAM" : "TRAIN" };
    }

    // ---- primitives, view-local, untextured (after wolfhelmhud.cpp's, itself after wolfdashboard.cpp's) ----
    void noTex() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }
    void vtx(F32 x, F32 y, const LLColor4& c) { gGL.color4fv(c.mV); gGL.vertex2f(x, y); }
    void rectF(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(r, b, c); vtx(r, t, c);
        vtx(l, b, c); vtx(r, t, c); vtx(l, t, c);
        gGL.end();
    }
    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bot)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bot); vtx(r, b, bot); vtx(r, t, top);
        vtx(l, b, bot); vtx(r, t, top); vtx(l, t, top);
        gGL.end();
    }
    void quadF(F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3, F32 x4, F32 y4, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(x1, y1, c); vtx(x2, y2, c); vtx(x3, y3, c);
        vtx(x1, y1, c); vtx(x3, y3, c); vtx(x4, y4, c);
        gGL.end();
    }
    void lineW(F32 x1, F32 y1, F32 x2, F32 y2, F32 w, const LLColor4& c)
    {
        const F32 dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
        if (len <= 0.0001f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        quadF(x1 + nx, y1 + ny, x1 - nx, y1 - ny, x2 - nx, y2 - ny, x2 + nx, y2 + ny, c);
    }
    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c); rectF(l, t - w, r, t, c); rectF(l, b, l + w, t, c); rectF(r - w, b, r, t, c);
    }
    void circleF(F32 cx, F32 cy, F32 rad, const LLColor4& c, S32 seg = 40)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        for (S32 i = 0; i < seg; ++i)
        {
            const F32 a0 = 2.f * PI_F * (F32)i / (F32)seg, a1 = 2.f * PI_F * (F32)(i + 1) / (F32)seg;
            vtx(cx, cy, c); vtx(cx + rad * cosf(a0), cy + rad * sinf(a0), c); vtx(cx + rad * cosf(a1), cy + rad * sinf(a1), c);
        }
        gGL.end();
    }
    // a point `deg` clockwise from 12 o'clock (y up)
    void pt(F32 cx, F32 cy, F32 r, F32 deg, F32& x, F32& y)
    {
        const F32 a = deg * DEG_TO_RAD;
        x = cx + r * sinf(a);
        y = cy + r * cosf(a);
    }
    void clockArc(F32 cx, F32 cy, F32 r, F32 w, F32 d0, F32 d1, const LLColor4& c, S32 seg = 48)
    {
        for (S32 i = 0; i < seg; ++i)
        {
            F32 x0, y0, x1, y1, x2, y2, x3, y3;
            const F32 a = d0 + (d1 - d0) * (F32)i / (F32)seg, b = d0 + (d1 - d0) * (F32)(i + 1) / (F32)seg;
            pt(cx, cy, r - w * 0.5f, a, x0, y0); pt(cx, cy, r + w * 0.5f, a, x1, y1);
            pt(cx, cy, r + w * 0.5f, b, x2, y2); pt(cx, cy, r - w * 0.5f, b, x3, y3);
            quadF(x0, y0, x1, y1, x2, y2, x3, y3, c);
        }
    }

    // Source: wolfhelmhud.cpp face() - the B612 face (fonts.xml Avionics0..11) at the largest size whose line fits px.
    const LLFontGL* face(const char* name, U8 style, F32 px)
    {
        static std::map<std::string, std::vector<const LLFontGL*>> cache;
        const std::string key = std::string(name) + (style ? "B" : "R");
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                if (const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, llformat("Avionics%d", i), style))) v.push_back(f);
            }
            if (v.empty()) v.push_back(LLFontGL::getFontSansSerif());
            it = cache.emplace(key, v).first;
        }
        const LLFontGL* best = it->second.front();
        for (const LLFontGL* f : it->second) if ((F32)f->getLineHeight() <= px) best = f;
        return best;
    }

    // THE LAYOUT, after wolfhelmhud.cpp: a 1600 x 304 panel (y up from its bottom here) scaled by u, centred.
    struct Geo { F32 ox = 0.f, oy = 0.f, u = 1.f; } g;
    F32 X(F32 x) { return g.ox + x * g.u; }
    F32 Y(F32 y) { return g.oy + y * g.u; }
    F32 S(F32 v) { return v * g.u; }
    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        WolfHudScale::text(face("B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(px) * 1.25f)), s, x, y, c, h, LLFontGL::VCENTER);
    }

    // [HUD KIT 2026-11-25] Paul's Gunmetal Console look (wolfhudkit.h): the band a machined gunmetal plate, the panels carbon
    // or dark glass in a chrome (brass for steam and trams) frame, every button a backlit key in its well (lit while chosen or
    // on, pressed while held), the dial an instrument behind glass in its bezel, the lamps lit lenses. Same layout, words,
    // hit ids and actions as before (scripts/wolf/hudkit/inventory/traincab.json). Until the sheet has loaded, the old flat
    // fills. Words on a key cap (its lightest middle as rendered, 76, 75, 78; WCAG 2): C_KEYTEXT 7.9:1, C_KEYDIM 5.6:1,
    // C_KEYLIT 5.3:1 - the colours.xml WolfHudKit* ones. The status colours (red, amber, green) light lamps and lenses beside
    // the words; the words themselves stay light, as red on a key cap would be 2.3:1.
    const LLColor4 C_KEYTEXT(0.949f, 0.957f, 0.969f, 1.f);
    const LLColor4 C_KEYDIM(0.80f, 0.82f, 0.85f, 1.f);
    const LLColor4 C_KEYLIT(1.f, 0.753f, 0.38f, 1.f);
    bool brassCab(const std::string& kind) { return kind.empty() || kind == "steam" || kind == "tram"; }   // themeFor's footplate and tram
    // a key, lit or held; `flat` (and `edge`, when not clear) the old look while the sheet loads
    void key(F32 l, F32 b, F32 r, F32 t, bool lit, bool down, const LLColor4& flat, const LLColor4& edge = LLColor4(0.f, 0.f, 0.f, 0.f))
    {
        // the key's 30 sheet pixels of bevel at 12.6 units (S(0.42)), whatever its size
        if (WolfHudKit::key(l, b, r, t, down ? WolfHudKit::CAP_DOWN : lit ? WolfHudKit::CAP_LIT : WolfHudKit::CAP_UP, S(0.42f))) return;
        rectF(l, b, r, t, flat);
        if (edge.mV[VALPHA] > 0.f) frameW(l, b, r, t, S(2.f), edge);
    }
    // a panel: carbon (the controls) or dark glass (the signal, the line ahead) in its frame
    void panel(F32 l, F32 b, F32 r, F32 t, bool glass, bool brass, const LLColor4& flat, const LLColor4& edge)
    {
        if (!WolfHudKit::part(glass ? WolfHudKitMap::P_SCREEN : WolfHudKitMap::P_INLAY, l, b, r, t, LLColor4::white, S(0.5f)))
        {
            rectF(l, b, r, t, flat);
            frameW(l, b, r, t, S(2.f), edge);
            return;
        }
        WolfHudKit::part(brass ? WolfHudKitMap::P_FRAME_BRASS : WolfHudKitMap::P_FRAME_CHROME, l - S(3.f), b - S(3.f), r + S(3.f), t + S(3.f),
                         LLColor4::white, S(0.3f));
    }
    // a lamp: the colour behind its lens, a glow round it while lit
    void lamp(F32 cx, F32 cy, F32 r, const LLColor4& on, const LLColor4& off, bool lit)
    {
        if (lit) WolfHudKit::glow(cx, cy, r * 2.6f, LLColor4(on.mV[VRED], on.mV[VGREEN], on.mV[VBLUE], 0.7f));
        circleF(cx, cy, r, lit ? on : off);
        // the lens sprite's disc is 46 of its 48 px half-width (parts.py lens: W / 2 - 2)
        const F32 lr = r * 48.f / 46.f;
        WolfHudKit::part(WolfHudKitMap::P_LENS, cx - lr, cy - lr, cx + lr, cy + lr);
    }

    std::string fitWMeasure(const std::string& str, F32 px, bool bold, F32 maxw);
    // Text cut to `maxw` picture units on a character boundary, with "..." (review 13 Nov, W12): measured with the very
    // face T() draws it with.
    std::string fitW(const std::string& str, F32 px, bool bold, F32 maxw)
    {
        // cached by (text, size, weight, width): the same few strings every frame are measured once (review W6)
        static std::map<std::string, std::string> cache;
        const std::string key = llformat("%d|%d|%d|", (S32)llround(S(px) * 100.f), bold ? 1 : 0, (S32)llround(maxw)) + str;
        auto hit = cache.find(key);
        if (hit != cache.end()) return hit->second;
        if (cache.size() > 256) cache.clear();
        return cache[key] = fitWMeasure(str, px, bold, maxw);
    }

    std::string fitWMeasure(const std::string& str, F32 px, bool bold, F32 maxw)
    {
        const LLFontGL* f = face("B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(px) * 1.25f));
        if (!f || f->getWidthF32(utf8str_to_wstring(str).c_str()) <= maxw) return str;
        LLWString w = utf8str_to_wstring(str);
        const LLWString dots = utf8str_to_wstring("...");
        while (!w.empty() && f->getWidthF32((w + dots).c_str()) > maxw) w.pop_back();
        return wstring_to_utf8str(w + dots);
    }

    // [wolf-rail 1.3.7] Text broken into at most `max_lines` lines of whole words, each at most `maxw` picture units, measured
    // with the very face T() draws it with (as fitW). What does not fit on the last line is cut there with "..." (fitW); a
    // single word longer than a line is cut the same way. Cached as fitW is (the same note is drawn every frame).
    std::vector<std::string> wrapW(const std::string& str, F32 px, bool bold, F32 maxw, size_t max_lines)
    {
        static std::map<std::string, std::vector<std::string>> cache;
        const std::string key = llformat("%d|%d|%d|%d|", (S32)llround(S(px) * 100.f), bold ? 1 : 0, (S32)llround(maxw), (S32)max_lines) + str;
        auto hit = cache.find(key);
        if (hit != cache.end()) return hit->second;
        if (cache.size() > 64) cache.clear();
        std::vector<std::string> lines;
        const LLFontGL* f = face("B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(px) * 1.25f));
        std::vector<LLWString> words;
        {
            const LLWString all = utf8str_to_wstring(str);
            LLWString cur;
            for (llwchar ch : all)
            {
                if (ch == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            if (!cur.empty()) words.push_back(cur);
        }
        const LLWString space = utf8str_to_wstring(" ");
        LLWString line;
        for (size_t i = 0; i < words.size() && f && max_lines > 0; ++i)
        {
            if (lines.size() + 1 == max_lines)
            {
                // the last line there is room for: everything left, cut to fit
                LLWString rest = line;
                for (size_t j = i; j < words.size(); ++j) rest += (rest.empty() ? LLWString() : space) + words[j];
                lines.push_back(fitW(wstring_to_utf8str(rest), px, bold, maxw));
                line.clear();
                break;
            }
            if (line.empty()) { line = words[i]; continue; }
            const LLWString trial = line + space + words[i];
            if (f->getWidthF32(trial.c_str()) <= maxw) { line = trial; continue; }
            lines.push_back(fitW(wstring_to_utf8str(line), px, bold, maxw));
            line = words[i];
        }
        if (!line.empty()) lines.push_back(fitW(wstring_to_utf8str(line), px, bold, maxw));
        if (!f && !str.empty() && max_lines > 0) lines.push_back(str);
        return cache[key] = lines;
    }

    std::string distance(F32 m)
    {
        if (m >= 1000.f) return llformat("%.1f km", m / 1000.f);
        return llformat("%d m", (S32)llround(m));
    }

    // [wolf-rail 1.2] a points setting in words, as the region's own note says it ("Points P1 set left.": RailEngine
    // SettingName left / right / auto; auto = the points as they were laid, Guides.cs NextGuide)
    std::string settingWords(const std::string& s)
    {
        return s == "left" ? "Set left" : s == "right" ? "Set right" : "Set auto";
    }

    // The guide (Paul 2026-11-11, paul-games-must-look-fun: cards with Back / Next and dots, not "page 1 of 9").
    struct Card { const char* title; const char* l1; const char* l2; const char* l3; const char* l4 = nullptr; };   // [1.3.7] l4
    const Card CARDS[] = {
        { "Welcome to the cab", "You are driving this train. Take your time - there is no clock and no score.",
          "The line looks after itself: the train always stops before the end of its line.", "Stand up to leave the cab; the train stops when you do." },
        { "Easy driving", "One lever: STOP, SLOW and GO. Click a notch, or press Up and Down. It stops at stations by itself.",
          "GO runs at the line's speed, SLOW at a third of it, STOP brakes to a stand.",
          "FORWARD or BACK (F5) sets the way it goes. Backing is always slow; stop the train first." },
        { "Driver's mode", "The REGULATOR (or POWER) lever pulls, the BRAKE lever slows the train. Drag them, or press Up / Down.",
          "With neither, the train keeps its speed. The reverser: FORWARD, NEUTRAL, or REVERSE to back it from a stand.", "Stations are yours to stop at. The red lamp lights when the line makes the train brake." },
        { "The horn and the dial", "WHISTLE (or HORN) sounds the engine's own - everyone near hears it. The horn key works too.",
          "The dial shows the speed and, in red, the fastest this line allows. Click it for mph or km/h.", "THE LINE AHEAD shows the next station and how much track is in front of you." },
        // [wolf-rail 1.2] Auto (Paul 13 Nov: "trains should have an auto mode as well btw so they can just run")
        { "Auto", "Press AUTO and the train drives itself: the line's speed, the stations and the signals.",
          "Take any lever, or press AUTO again, and you are driving once more.", "Stand up in Auto and the train goes on to the next station and stops there." },
        // [wolf-rail 1.2] signals, points and backing
        { "Signals and points", "SIGNAL shows the next signal: STOP (red) - the train waits; CAUTION (yellow) - the next is red; CLEAR (green).",
          "POINTS shows the next points and how they are set. Where drivers may change them, one press of POINTS changes them.",
          "To back out of a platform: stop, choose BACK (or REVERSE in driver's mode), then go. It stops before the buffers." },
        // [wolf-rail 1.3, stage V] DESIGN-terminus.md 5.4 "a 'Changing ends' page"; the walk (Cab.cs CAB_WALK_MS 120 s), COUPLE's
        // way (RailCouple.cs CabCouple) and UNCOUPLE's part (the cab's own unit) as the region does them
        // [wolf-rail 1.3.7] Paul 14 Nov ("when i did change ends the locomotive stayed facing the same way?"): the second line
        { "Changing ends", "Stopped at the end of the line? Press CHANGE ENDS (F6), then walk to the cab at the other end and sit in it.",
          "Changing ends doesn't turn the engine round - use a turntable for that.",
          "COUPLE (F7) shows when your own train stands on the line: set FORWARD or REVERSE towards it, then press. Again stops it.",
          "UNCOUPLE (Shift+F7) leaves the rest standing behind your cab's part. Escape closes a red message." },
        // [wolf-rail 1.3.7] Paul 14 Nov (a shunter stopped at the end of a shed road): the end-of-line note (Cab.cs:85) and
        // the TURNTABLE button (tt=, Cab.cs:82), button only
        { "End of the line and turntables", "Line ended? The note above the cab says so. Press BACK (or REVERSE), or CHANGE ENDS, to drive out.",
          "At a turntable's edge a TURNTABLE button shows, with the turntable's name. Press it once.",
          "The turntable turns your engine round with you in the cab. Then take the power to drive off." },
        { "Driving from the keyboard", "Up and Down move the lever (in driver's mode: the brake off first, then the power).",
          "F5: FORWARD or BACK in Easy driving; FORWARD, NEUTRAL, REVERSE in driver's mode. F2 Easy or driver's mode.",
          "F3 mph or km/h, F4 hides or shows the cab, F8 AUTO, F9 POINTS. Your horn key (H unless changed) sounds the horn." },
        // [client review 13 Nov, W7] a gesture's own key is not taken from it: LLGestureMgr::triggerGesture(key, mask)
        // (llgesturemgr.cpp:761) plays it from the same key press
        { "Your gestures", "A gesture you have put on F2 to F9 (or Shift+F7) still plays while you drive, as well as the cab's key.",
          "If you would rather it did not, give the gesture another key in the Gestures window.", "Standing up gives every key back as it was." },
    };
    const S32 CARD_COUNT = (S32)(sizeof(CARDS) / sizeof(CARDS[0]));
}

WolfTrainCabView* WolfTrainCabView::sView = nullptr;

WolfTrainCabView::WolfTrainCabView(const Params& p)
:   LLUICtrl(p)
{
    sView = this;
}

WolfTrainCabView::~WolfTrainCabView()
{
    if (sView == this) sView = nullptr;
}

// static
const char* WolfTrainCabView::hitName(EHit id)
{
    // Source: wolftraincab.h EHit, every member
    switch (id)
    {
    case H_NONE: return "H_NONE";
    case H_NOTCH_STOP: return "H_NOTCH_STOP";
    case H_NOTCH_SLOW: return "H_NOTCH_SLOW";
    case H_NOTCH_GO: return "H_NOTCH_GO";
    case H_POWER: return "H_POWER";
    case H_BRAKE: return "H_BRAKE";
    case H_REV_FWD: return "H_REV_FWD";
    case H_REV_N: return "H_REV_N";
    case H_REV_REV: return "H_REV_REV";
    case H_HORN: return "H_HORN";
    case H_MODE: return "H_MODE";
    case H_UNITS: return "H_UNITS";
    case H_HELP: return "H_HELP";
    case H_HELP_BACK: return "H_HELP_BACK";
    case H_HELP_NEXT: return "H_HELP_NEXT";
    case H_HELP_CLOSE: return "H_HELP_CLOSE";
    case H_CLOSE: return "H_CLOSE";
    case H_SHOW_TAB: return "H_SHOW_TAB";
    case H_AUTO: return "H_AUTO";
    case H_POINTS: return "H_POINTS";
    case H_EASY_FWD: return "H_EASY_FWD";
    case H_EASY_BACK: return "H_EASY_BACK";
    case H_ENDS: return "H_ENDS";
    case H_COUPLE: return "H_COUPLE";
    case H_UNCOUPLE: return "H_UNCOUPLE";
    case H_ALERT_OK: return "H_ALERT_OK";
    case H_WALK: return "H_WALK";
    case H_TURNTABLE: return "H_TURNTABLE";
    }
    return "H_UNKNOWN";
}

// static
void WolfTrainCabView::logHits()
{
    if (!sView) { LL_INFOS("WolfHudKit") << "hits traincab: no cab view" << LL_ENDL; return; }
    // Source: wolfstarfolddeck.cpp WolfStarfoldDeck::logHits - view-local UI units to window GL pixels (localPointToScreen, then
    // the display scale, llviewerwindow.cpp handleMouseDown). The cab's hits are in picture units: x mHitScale (hitAt).
    const LLVector2 scale = gViewerWindow ? gViewerWindow->getDisplayScale() : LLVector2(1.f, 1.f);
    const F32 k = sView->mHitScale;
    LL_INFOS("WolfHudKit") << "hits traincab: " << sView->mHits.size() << " (display scale " << scale.mV[VX] << ", zoom " << k << ")" << LL_ENDL;
    for (const Hit& h : sView->mHits)
    {
        S32 sx = 0, sy = 0;
        sView->localPointToScreen(ll_round((h.mL + h.mR) * 0.5f * k), ll_round((h.mB + h.mT) * 0.5f * k), &sx, &sy);
        LL_INFOS("WolfHudKit") << "WolfHudKit hit traincab " << hitName(h.mId) << " gl " << ll_round((F32)sx * scale.mV[VX]) << ","
                               << ll_round((F32)sy * scale.mV[VY]) << " size " << ll_round((h.mR - h.mL) * k * scale.mV[VX]) << ","
                               << ll_round((h.mT - h.mB) * k * scale.mV[VY]) << " tip " << h.mTip << LL_ENDL;
    }
}

void WolfTrainCabView::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ llmin(l, r), llmin(b, t), llmax(l, r), llmax(b, t), id, tip });
}

const WolfTrainCabView::Hit* WolfTrainCabView::hitAt(S32 x, S32 y) const
{
    const F32 px = (F32)x / mHitScale, py = (F32)y / mHitScale;
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (px >= it->mL && px <= it->mR && py >= it->mB && py <= it->mT) return &*it;
    }
    return nullptr;
}

bool WolfTrainCabView::onPanel(S32 x, S32 y) const
{
    if (!mDrawn) return false;
    if (hitAt(x, y)) return true;
    // the guide card (above the panel) and the tab are ours too: a click on them never reaches the world (review W11)
    if (mShowHelp && (F32)x >= mHelpL && (F32)x <= mHelpR && (F32)y >= mHelpB && (F32)y <= mHelpT) return true;
    if (mShowAlert && (F32)x >= mAlertL && (F32)x <= mAlertR && (F32)y >= mAlertB && (F32)y <= mAlertT) return true;   // [stage V]
    return (F32)y >= mPanelB && (F32)y <= mPanelT;
}

void WolfTrainCabView::draw()
{
    WolfTrain& tr = WolfTrain::instance();
    mHits.clear();
    mDrawn = false;
    mShowAlert = false;   // [stage V] set again below while the refusal strip is drawn
    if (!tr.shown())
    {
        mShowHelp = false;
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        if (mPressed == H_HORN) tr.setHorn(false);
        mPressed = H_NONE;
        mResize.hide();
        const bool walk = !tr.hiddenByDriver() && tr.walkPending();
        if (tr.hiddenByDriver() || walk)
        {
            // still driving with the cab hidden (review suggestion 16): a small tab at the bottom centre brings it back;
            // [stage V] or, after CHANGE ENDS, the region's word on where to walk, in the same place until the cab at the
            // other end opens (or the region's 120 s are over, or the driver closes it)
            const F32 w = (F32)getRect().getWidth();
            // above the bottom toolbar, as the cab itself sits (review W5; the same measure as draw() below)
            F32 tb = 0.f;
            if (gToolBarView)
            {
                if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
                {
                    if (bp->isInVisibleChain())
                    {
                        const LLRect sr = bp->calcScreenRect();
                        S32 lx = 0, ly = 0;
                        screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                        tb = llclamp((F32)ly, 0.f, (F32)getRect().getHeight() * 0.4f);
                    }
                }
            }
            LLGLSUIDefault gls_ui;
            mHitScale = 1.f;
            g.u = 1.f; g.ox = 0.f; g.oy = 0.f;
            if (walk)
            {
                const F32 half = llmin(380.f, w * 0.5f - 8.f);
                mTabL = w * 0.5f - half; mTabR = w * 0.5f + half; mTabB = tb + 4.f; mTabT = tb + 58.f;
                key(mTabL, mTabB, mTabR, mTabT, false, mPressed == H_WALK, LLColor4(0.02f, 0.07f, 0.04f, 0.94f), LLColor4(0.86f, 0.69f, 0.34f, 1.f));
                T((mTabL + mTabR) * 0.5f, mTabT - 18.f, fitW(tr.walkNote(), 12.f, true, 2.f * half - 24.f), 12.f, C_KEYTEXT, true);
                T((mTabL + mTabR) * 0.5f, mTabB + 15.f, "You have two minutes to sit down there. Click to close this message.", 10.f, C_KEYLIT);
                addHit(mTabL, mTabB, mTabR, mTabT, H_WALK, tr.walkNote() + " - click to close this message");
            }
            else
            {
                // [HUD KIT 2026-11-25] 44 high (was 30): a target of 44 px or more
                mTabL = w * 0.5f - 100.f; mTabR = w * 0.5f + 100.f; mTabB = tb + 4.f; mTabT = tb + 48.f;
                key(mTabL, mTabB, mTabR, mTabT, false, mPressed == H_SHOW_TAB, LLColor4(0.02f, 0.07f, 0.04f, 0.9f), LLColor4(0.86f, 0.69f, 0.34f, 1.f));
                T((mTabL + mTabR) * 0.5f, (mTabB + mTabT) * 0.5f, "DRIVING - SHOW THE CAB", 10.f, C_KEYLIT, true);
                addHit(mTabL, mTabB, mTabR, mTabT, H_SHOW_TAB, "You are still driving: show the Train Cab (F4)");
            }
            mDrawn = true;
            mPanelB = mPanelT = -1.f;
        }
        return;
    }
    if (!gSavedSettings.getBOOL("WolfTrainCabHelpSeen") && tr.driving())
    {
        gSavedSettings.setBOOL("WolfTrainCabHelpSeen", true);
        mShowHelp = true;   // the guide opens by itself the first time (as the dashboards')
        mHelpPage = 0;
    }

    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    // Source: wolfhelmhud.cpp draw - sit on whatever the bottom toolbar leaves.
    F32 screen_bottom = 0.f;
    if (gToolBarView)
    {
        if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
        {
            if (bp->isInVisibleChain())
            {
                const LLRect sr = bp->calcScreenRect();
                S32 lx = 0, ly = 0;
                screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                screen_bottom = llclamp((F32)ly, 0.f, Hv * 0.4f);
            }
        }
    }
    // [HUD KIT 2026-11-25] Paul: every press target 44 px or more. The smallest are 44 units (X, ?, the guide's Back / Next,
    // the refusal's OK; the rest are larger): a unit is u x zoom x the UI size in window pixels, so the cab is never drawn
    // smaller than 1 / (u x UI size). u at full size is this frame's layout below at zoom 1 (a narrower picture only makes u
    // smaller, so this is the most it can shrink).
    {
        const F32 ui_x = llmax(0.1f, LLUI::getScaleFactor().mV[VX]);
        const F32 want1 = llclamp(Hv * llmax(0.1f, LLUI::getScaleFactor().mV[VY]) * 0.30f, 170.f, 380.f);
        const F32 u1 = llmin(llmin(want1, llmax(60.f, (Hv - screen_bottom) * 0.55f)) / 304.f, screen_w / 1600.f);
        mResize.setFloor(1.f / llmax(0.01f, u1 * ui_x));
    }
    const F32 zoom = mResize.scale();
    const F32 W = screen_w / zoom;
    const F32 bottom = screen_bottom / zoom;
    // Source: wolfhelmhud.cpp draw - the panel's 304 units are 30% of the window (170..380 px) times the UI scale, never
    // more than 55% of what the toolbar leaves; never wider than the window.
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    const F32 want_h = llclamp(Hv * ui * 0.30f, 170.f, 380.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.55f);
    g.u = llmin(llmin(want_h, max_h) / 304.f, W / 1600.f);
    g.ox = W * 0.5f - 800.f * g.u;
    g.oy = bottom;
    const F32 top = Y(304.f);
    mPanelB = screen_bottom;
    mPanelT = top * zoom;
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mDrawn = true;

    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;
    zoomed.emplace(zoom);
    const WolfTrain::Cab& c = tr.cab();
    const Theme th = themeFor(c.mKind);
    const bool driving = tr.driving();
    const bool easy = tr.easy();

    // the band: the cab's own colours, a rail along the top
    const bool brass = brassCab(c.mKind);
    if (!WolfHudKit::part(WolfHudKitMap::P_PLATE, 0.f, bottom, W, top, LLColor4::white, S(0.6f))) rectG(0.f, bottom, W, top, th.hi, th.lo);
    rectF(0.f, top - S(4.f), W, top, th.accentLo);
    rectF(0.f, top - S(2.f), W, top - S(1.f), th.accent);

    // ── the badge: the line's emblem, the train and its kind ─────────────────────────────────────
    T(X(24.f), Y(284.f), "WOLF GRID RAILROAD", 11.f, th.accent, true, LLFontGL::LEFT);
    T(X(24.f), Y(262.f), fitW(driving ? (c.mTrain.empty() ? std::string("Your train") : c.mTrain) : std::string("Train Cab"), 15.f, true, S(470.f)), 15.f, C_WHITE, true, LLFontGL::LEFT);
    // [wolf-rail 1.3, stage V] veh= is the vehicles behind the head (RailEngine.cs SendCab: TrainRecord.Vehicles.Count, the
    // head not counted - RailCouple.cs counts an uncoupled part "Vehicles.Count + 1 objects"): the train is veh + 1
    const std::string vehicles = !driving || c.mVeh < 0 ? std::string() : c.mVeh == 0 ? std::string("  -  1 vehicle") : llformat("  -  %d vehicles", c.mVeh + 1);
    T(X(24.f), Y(242.f), fitW(std::string(th.name) + vehicles + (c.mRegion.empty() ? "" : "  -  " + c.mRegion), 10.f, false, S(470.f)), 10.f, C_GREY, false, LLFontGL::LEFT);
    // close (hide): the cab stays yours; Wolf World > Train Cab brings it back
    // [HUD KIT 2026-11-25] X and ? 44 x 44 (were 32 x 28): targets of 44 px or more at the cab's full size
    key(X(1548.f), Y(254.f), X(1592.f), Y(298.f), false, mPressed == H_CLOSE, th.inset);
    T(X(1570.f), Y(276.f), "X", 13.f, C_KEYTEXT, true);
    addHit(X(1548.f), Y(254.f), X(1592.f), Y(298.f), H_CLOSE, "Hide the Train Cab - F4 (you keep driving; F4 or Wolf World > Vehicles & Travel > Train Cab brings it back)");
    key(X(1500.f), Y(254.f), X(1544.f), Y(298.f), mShowHelp, mPressed == H_HELP, th.inset);
    T(X(1522.f), Y(276.f), "?", 14.f, mShowHelp ? C_KEYLIT : C_KEYTEXT, true);
    addHit(X(1500.f), Y(254.f), X(1544.f), Y(298.f), H_HELP, "How to drive");

    if (!driving)
    {
        T(X(800.f), Y(160.f), "Sit in the cab of a Wolf Rail engine to drive it.", 16.f, C_WHITE, true);
        T(X(800.f), Y(126.f), "Paddington Station's driving loop, where anyone may drive, is being built. Press ? to read how driving works.", 11.f, C_GREY);
        if (mShowHelp) drawHelp(top, false, W);
        zoomed.reset();
        mResize.draw();
        return;
    }

    // ── left: the controls ────────────────────────────────────────────────────────────────────────
    panel(X(16.f), Y(14.f), X(520.f), Y(226.f), false, brass, th.inset, th.accentLo);
    mLeverB = Y(36.f);
    mLeverT = Y(196.f);
    if (easy)
    {
        // one lever in a gate of three notches
        const F32 sx = X(110.f);
        if (!WolfHudKit::part(WolfHudKitMap::P_SLOT, sx - S(12.f), Y(28.f), sx + S(12.f), Y(204.f), LLColor4::white, S(0.4f)))
            rectF(sx - S(10.f), Y(36.f), sx + S(10.f), Y(196.f), LLColor4(0.f, 0.f, 0.f, 0.6f));
        static const char* NAMES[WolfTrain::NOTCH_COUNT] = { "STOP", "SLOW", "GO" };
        static const EHit HITS[WolfTrain::NOTCH_COUNT] = { H_NOTCH_STOP, H_NOTCH_SLOW, H_NOTCH_GO };
        static const char* TIPS[WolfTrain::NOTCH_COUNT] = { "Stop: brake to a stand", "Slow: a third of the line's speed", "Go: the line's speed" };
        // [D-c] set to BACK: the region backs the train at a slow speed share (lever x a slow maximum)
        static const char* TIPS_BACK[WolfTrain::NOTCH_COUNT] = { "Stop: brake to a stand", "Slow: back very slowly", "Go: back slowly" };
        const bool back = tr.easyBack();
        for (S32 n = 0; n < WolfTrain::NOTCH_COUNT; ++n)
        {
            const F32 ny = Y(36.f + 80.f * (F32)n);
            const bool sel = tr.notch() == n;
            const LLColor4 col = n == WolfTrain::NOTCH_STOP ? C_RED : n == WolfTrain::NOTCH_SLOW ? C_AMBER : C_GREEN;
            const LLColor4 dark(col.mV[0] * 0.25f, col.mV[1] * 0.25f, col.mV[2] * 0.25f, 1.f);
            if (WolfHudKit::ready())
            {
                // the notch a key, lit when chosen, with its colour in a lamp beside the word (the word says it too)
                key(sx + S(24.f), ny - S(20.f), X(330.f), ny + S(20.f), sel, mPressed == HITS[n], dark);
                lamp(sx + S(48.f), ny, S(7.f), col, dark, sel);
                T(X(240.f), ny, NAMES[n], 18.f, sel ? C_KEYLIT : C_KEYTEXT, true);
            }
            else
            {
                rectF(sx + S(24.f), ny - S(18.f), X(330.f), ny + S(18.f), sel ? col : dark);
                T(X(232.f), ny, NAMES[n], 18.f, sel ? LLColor4(0.f, 0.f, 0.f, 1.f) : col, true);
            }
            addHit(sx - S(30.f), ny - S(24.f), X(330.f), ny + S(24.f), HITS[n], back ? TIPS_BACK[n] : TIPS[n]);
        }
        const F32 ky = Y(36.f + 80.f * (F32)tr.notch());
        lineW(sx, Y(36.f), sx, ky, S(8.f), C_GREY);
        if (!WolfHudKit::part(WolfHudKitMap::P_HUB, sx - S(19.f), ky - S(19.f), sx + S(19.f), ky + S(19.f)))
        {
            circleF(sx, ky, S(18.f), th.accent);
            circleF(sx - S(5.f), ky + S(5.f), S(6.f), LLColor4(1.f, 1.f, 1.f, 0.35f));
        }
        T(X(428.f), Y(208.f), "EASY DRIVING", 11.f, th.accent, true);
        // [wolf-rail 1.2, D-c] the way it goes: FORWARD or BACK (slow only), in words, large; F5 too. back=1 from the region
        // (it is backing) reads BACKING on the button.
        struct Dir { F32 b; bool isBack; EHit id; const char* tip; };
        const Dir D[2] = {
            { 128.f, false, H_EASY_FWD, "Forward (F5: forward or back)" },
            { 62.f, true, H_EASY_BACK, "Back, slowly - to back out of a platform. Stop the train first (F5: forward or back)" },
        };
        for (const Dir& d : D)
        {
            const F32 b = Y(d.b), t = Y(d.b + 56.f);
            const bool sel = back == d.isBack;
            key(X(346.f), b, X(510.f), t, sel, mPressed == d.id, sel ? th.accent : LLColor4(0.f, 0.f, 0.f, 0.45f), th.accent);
            const char* word = !d.isBack ? "FORWARD" : (sel && c.mBack ? "BACKING" : "BACK");
            T(X(428.f), (b + t) * 0.5f, word, 15.f, !WolfHudKit::ready() ? (sel ? LLColor4(0.f, 0.f, 0.f, 1.f) : C_WHITE) : sel ? C_KEYLIT : C_KEYTEXT, true);
            addHit(X(346.f), b, X(510.f), t, d.id, d.tip);
        }
        T(X(428.f), Y(42.f), "Up / Down: the lever", 9.f, C_GREY);
        T(X(428.f), Y(26.f), "F5: forward or back", 9.f, C_GREY);
    }
    else
    {
        // two levers: power (regulator) and brake, dragged up and down, and the reverser
        struct Lever { F32 x; F32 v; EHit id; const char* label; LLColor4 col; const char* tip; };
        const Lever L[2] = {
            { 90.f, tr.power(), H_POWER, th.power, C_GREEN, "Drag up to pull harder (Up / Down keys too)" },
            { 210.f, tr.brake(), H_BRAKE, "BRAKE", C_RED, "Drag up to brake harder (Up / Down keys too)" },
        };
        for (const Lever& l : L)
        {
            const F32 lx = X(l.x);
            if (!WolfHudKit::part(WolfHudKitMap::P_SLOT, lx - S(14.f), Y(28.f), lx + S(14.f), Y(204.f), LLColor4::white, S(0.4f)))
                rectF(lx - S(12.f), Y(36.f), lx + S(12.f), Y(196.f), LLColor4(0.f, 0.f, 0.f, 0.6f));
            rectF(lx - S(8.f), Y(36.f), lx + S(8.f), Y(36.f + 160.f * l.v), LLColor4(l.col.mV[0], l.col.mV[1], l.col.mV[2], 0.55f));
            for (S32 k = 0; k <= 10; ++k) rectF(lx + S(14.f), Y(36.f + 16.f * (F32)k) - S(0.6f), lx + S(22.f), Y(36.f + 16.f * (F32)k) + S(0.6f), C_DIM);
            const F32 ky = Y(36.f + 160.f * l.v);
            // the handle: a turned brass (steam, tram) or chrome bar
            if (!WolfHudKit::part(brass ? WolfHudKitMap::P_KNOB_BRASS : WolfHudKitMap::P_KNOB_CHROME, lx - S(28.f), ky - S(13.f), lx + S(28.f), ky + S(13.f),
                                  LLColor4::white, S(0.33f)))
            {
                rectF(lx - S(26.f), ky - S(9.f), lx + S(26.f), ky + S(9.f), th.accent);
                rectF(lx - S(26.f), ky + S(3.f), lx + S(26.f), ky + S(9.f), LLColor4(1.f, 1.f, 1.f, 0.25f));
            }
            T(lx, Y(212.f), l.label, 10.f, C_WHITE, true);
            T(lx, Y(22.f), llformat("%d%%", (S32)llround(l.v * 100.f)), 10.f, C_GREY);
            addHit(lx - S(40.f), Y(28.f), lx + S(40.f), Y(204.f), l.id, l.tip);
        }
        // the reverser
        static const char* RN[3] = { "FORWARD", "NEUTRAL", "REVERSE" };
        static const EHit RH[3] = { H_REV_FWD, H_REV_N, H_REV_REV };
        static const S32 RV[3] = { 1, 0, -1 };
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 b = Y(150.f - 54.f * (F32)i), t = b + S(42.f);
            const bool sel = tr.reverser() == RV[i];
            key(X(300.f), b, X(500.f), t, sel, mPressed == RH[i], sel ? th.accent : LLColor4(0.f, 0.f, 0.f, 0.45f));
            T(X(400.f), (b + t) * 0.5f, RN[i], 12.f, !WolfHudKit::ready() ? (sel ? LLColor4(0.f, 0.f, 0.f, 1.f) : C_WHITE) : sel ? C_KEYLIT : C_KEYTEXT, true);
            // [HUD KIT 2026-11-25] the press target 48 high (the key 42, rows 54 apart): 44 px or more at full size
            addHit(X(300.f), b - S(3.f), X(500.f), t + S(3.f), RH[i], i == 2 ? std::string("Reverse: with the train standing, power backs it along its line (F5)")
                                                          : std::string(i == 0 ? "Forward (F5: forward, neutral, reverse in turn)" : "Neutral: no power reaches the wheels (F5)"));
        }
        T(X(400.f), Y(212.f), "REVERSER", 10.f, C_WHITE, true);
    }
    const bool autoOn = tr.autoOn();
    if (autoOn)
    {
        // [wolf-rail 1.2] the train drives itself: the levers are let go, and the words say so over them
        rectF(X(18.f), Y(16.f), X(518.f), Y(224.f), LLColor4(th.inset.mV[0], th.inset.mV[1], th.inset.mV[2], 0.86f));
        T(X(268.f), Y(150.f), "AUTO", 22.f, th.accent, true);
        T(X(268.f), Y(116.f), "Auto - the train is driving itself", 13.f, C_WHITE, true);
        T(X(268.f), Y(86.f), "Take a lever, or press AUTO, to drive yourself.", 10.f, C_GREY);
    }
    // the AUTO button (F8), between the controls and the dial
    const bool kit = WolfHudKit::ready();
    key(X(536.f), Y(130.f), X(680.f), Y(214.f), autoOn, mPressed == H_AUTO, autoOn ? th.accent : LLColor4(0.f, 0.f, 0.f, 0.45f), th.accent);
    T(X(608.f), Y(182.f), "AUTO", 18.f, kit ? (autoOn ? C_KEYLIT : C_KEYTEXT) : autoOn ? LLColor4(0.f, 0.f, 0.f, 1.f) : th.accent, true);
    // (the press on its way, the region's answer not heard yet: "ONE MOMENT"; its note says why when it does not agree)
    T(X(608.f), Y(152.f), autoOn ? "ON" : tr.autoAsking() ? "ONE MOMENT" : "OFF", 11.f, kit ? (autoOn ? C_KEYLIT : C_KEYDIM) : autoOn ? LLColor4(0.f, 0.f, 0.f, 1.f) : C_GREY, true);
    addHit(X(536.f), Y(130.f), X(680.f), Y(214.f), H_AUTO, autoOn ? std::string("Drive yourself again (F8)")
           : std::string("Auto: the train drives itself - the line's speed, stations and signals") + (easy ? " (F8)" : ". Let both levers go first (F8)"));

    // [wolf-rail 1.2] the POINTS button (F9): the next points ahead, their setting in words and - when drivers may set them -
    // one press changes them (Paul 13 Nov, D-b); the region's note then says how they are set
    const bool canPts = tr.canChangePoints();
    key(X(536.f), Y(14.f), X(680.f), Y(118.f), false, canPts && mPressed == H_POINTS, canPts ? LLColor4(0.f, 0.f, 0.f, 0.55f) : LLColor4(0.f, 0.f, 0.f, 0.3f),
        canPts ? th.accent : th.accentLo);
    const LLColor4 ptsDim = kit ? C_KEYDIM : C_GREY, ptsText = kit ? C_KEYTEXT : C_WHITE, ptsLit = kit ? C_KEYLIT : th.accent;
    T(X(608.f), Y(102.f), "POINTS", 14.f, canPts ? ptsLit : ptsDim, true);
    if (c.mPts.empty())
    {
        // the same words as the signal's (and WolfStorm's): "None in sight"
        T(X(608.f), Y(72.f), "None", 11.f, ptsDim, true);
        T(X(608.f), Y(54.f), "in sight", 10.f, ptsDim);
    }
    else
    {
        T(X(608.f), Y(82.f), fitW(c.mPts, 11.f, true, S(136.f)), 11.f, ptsText, true);
        T(X(608.f), Y(64.f), fitW(settingWords(c.mPtsSet), 10.f, true, S(136.f)), 10.f, ptsText, true);
        T(X(608.f), Y(46.f), "in " + distance(c.mPtsM), 9.f, ptsDim);
        T(X(608.f), Y(27.f), canPts ? "Press to change" : c.mPtsDriver ? "Too far to change" : "Set by the line", 9.f, canPts ? ptsLit : ptsDim, true);
    }
    addHit(X(536.f), Y(14.f), X(680.f), Y(118.f), H_POINTS, canPts ? std::string("Change the points ahead - one press (F9)")
           : c.mPts.empty() ? std::string("No points ahead on the line")
           : c.mPtsDriver ? "These points can be changed from " + distance(DRIVER_POINTS_M) + " away"
           : std::string("These points are set by the line's owner, not from the cab"));

    // ── centre: the speed dial ────────────────────────────────────────────────────────────────────
    const bool mph = tr.mph();
    const F32 k = mph ? MS_TO_MPH : MS_TO_KMH;
    const F32 line_u = c.mLine * k;
    const F32 full = llmax(40.f, ceilf((line_u * 1.25f) / 20.f) * 20.f);   // the dial's full scale: a round figure past the line speed
    // [HUD KIT 2026-11-25] an instrument: its face, the marks and the needle, then the glass and the bezel over them. The bezel's
    // ring reaches 1.21 x the face (wolfhudkit.cpp gaugeBezel), so the face is 98 to end where the old rings did (118)
    const F32 cx = X(800.f), cy = Y(124.f), r = S(kit ? 98.f : 108.f);
    if (!WolfHudKit::gaugeFace(cx, cy, r, false))
    {
        circleF(cx, cy, r + S(10.f), th.accentLo);
        circleF(cx, cy, r + S(6.f), th.accent);
        circleF(cx, cy, r, LLColor4(0.03f, 0.03f, 0.03f, 1.f));
    }
    const F32 d0 = -135.f, d1 = 135.f;   // 7:30 to 4:30
    auto deg = [&](F32 v) { return d0 + (d1 - d0) * llclamp(v / full, 0.f, 1.f); };
    for (F32 v = 0.f; v <= full + 0.01f; v += 10.f)
    {
        F32 x0, y0, x1, y1;
        const bool major = fmodf(v, 20.f) < 0.01f;
        pt(cx, cy, r - S(major ? 16.f : 9.f), deg(v), x0, y0);
        pt(cx, cy, r - S(2.f), deg(v), x1, y1);
        lineW(x0, y0, x1, y1, S(major ? 2.5f : 1.5f), C_WHITE);
        if (major)
        {
            F32 lx, ly;
            pt(cx, cy, r - S(32.f), deg(v), lx, ly);
            T(lx, ly, llformat("%d", (S32)v), 10.f, C_GREY);
        }
    }
    if (c.mLine > 0.f) clockArc(cx, cy, r - S(5.f), S(6.f), deg(line_u), d1, C_RED, 24);   // past the line speed
    F32 nx, ny;
    pt(cx, cy, r - S(14.f), deg(c.mSpeed * k), nx, ny);
    lineW(cx, cy, nx, ny, S(4.f), C_RED);
    if (!WolfHudKit::part(WolfHudKitMap::P_HUB, cx - S(11.f), cy - S(11.f), cx + S(11.f), cy + S(11.f))) circleF(cx, cy, S(10.f), th.accent);
    T(cx, cy - S(44.f), llformat("%d", (S32)llround(c.mSpeed * k)), 24.f, C_WHITE, true);
    T(cx, cy - S(70.f), mph ? "mph" : "km/h", 10.f, C_GREY);
    WolfHudKit::gaugeBezel(cx, cy, r, brass);
    addHit(cx - S(108.f), cy - S(108.f), cx + S(108.f), cy + S(108.f), H_UNITS, mph ? "Click for km/h (F3)" : "Click for mph (F3)");
    T(cx, Y(266.f), autoOn ? "AUTO" : c.mBack ? "BACKING" : c.mMode == "easy" ? "EASY DRIVING" : "YOU ARE DRIVING", 11.f, th.accent, true);

    // ── [wolf-rail 1.3, stage V] SHUNT: CHANGE ENDS, COUPLE, UNCOUPLE along the top band, each only when it can act
    //    (DESIGN-terminus.md 5.4); fixed places, so a button that comes and goes never moves the others ─────────────────
    {
        struct Shunt { F32 l; EHit id; bool can; const char* word; std::string sub; std::string tip; bool dim; };
        std::string coupleSub, coupleTip;
        const bool wrong = tr.coupleWrongWay();
        if (!c.mCoupleSide.empty())
        {
            const bool ahead = c.mCoupleSide == "ahead";
            // the reverser's own words for the way (FORWARD / REVERSE; Easy driving's FORWARD / BACK)
            const char* way = ahead ? "FORWARD" : (easy ? "BACK" : "REVERSE");
            coupleSub = wrong ? std::string("Set ") + way + " first" : "Stock " + distance(c.mCoupleM) + (ahead ? " ahead" : " behind");
            coupleTip = "Couple to " + (c.mCoupleName.empty() ? std::string("the stock") : "\"" + c.mCoupleName + "\"") + ", "
                + distance(c.mCoupleM) + (ahead ? " ahead" : " behind") + ". Set " + way + " towards it, then press COUPLE (F7); press again to stop";
        }
        // [wolf-rail 1.3.7] TURNTABLE (tt=) under its turntable's name: the turntable turns the engine round, the driver seated
        const std::string ttTip = "Turn your engine round on the turntable " + c.mTt
            + ": you stay in the cab and go round with it. One press (button only - no key)";
        // four fixed places of 140 between the badge's band end and the ? button: [HUD KIT 2026-11-25] from X 920, 144 apart
        // (920 + 3 x 144 + 140 = 1492), the ? button 44 wide from X 1500 (it was 930, 146 apart, to 1508 by a 32-wide ? at 1520).
        // The longest word, CHANGE ENDS at 14, is about 5.6 line heights wide in B612 (fonts/B612-Bold.ttf advances; a
        // line height of at most S(14) x 1.25 = 17.5 at u = 1): about 99 of the 130 it is fitted to.
        const Shunt SH[4] = {
            { 920.f, H_ENDS, tr.canChangeEnds(), "CHANGE ENDS", "F6", "Change ends: then walk to the cab at the other end and sit in it (F6)", false },
            { 1064.f, H_COUPLE, tr.canCouple(), "COUPLE", coupleSub, coupleTip, wrong },
            { 1208.f, H_UNCOUPLE, tr.canUncouple(), "UNCOUPLE", "Shift+F7", "Uncouple: your cab's part leaves the rest standing on its own (Shift+F7)", false },
            { 1352.f, H_TURNTABLE, tr.canTurntable(), "TURNTABLE", c.mTt, ttTip, false },
        };
        for (const Shunt& s : SH)
        {
            if (!s.can) continue;
            const F32 l = X(s.l), r = X(s.l + 140.f), b = Y(232.f), t = Y(296.f);
            key(l, b, r, t, false, mPressed == s.id, s.dim ? LLColor4(0.f, 0.f, 0.f, 0.45f) : LLColor4(0.f, 0.f, 0.f, 0.6f), s.dim ? th.accentLo : th.accent);
            T((l + r) * 0.5f, Y(274.f), fitW(s.word, 14.f, true, S(130.f)), 14.f, kit ? C_KEYTEXT : th.accent, true);
            T((l + r) * 0.5f, Y(248.f), fitW(s.sub, 9.f, true, S(130.f)), 9.f, s.dim ? (kit ? C_KEYLIT : C_AMBER) : (kit ? C_KEYDIM : C_GREY), true);
            addHit(l, b, r, t, s.id, s.tip);
        }
    }

    // ── [wolf-rail 1.2] the signal ahead: its lamp, its word (never the colour alone) and how far ──────────────────────
    panel(X(922.f), Y(14.f), X(1040.f), Y(226.f), true, brass, th.inset, th.accentLo);
    T(X(981.f), Y(208.f), "SIGNAL", 10.f, th.accent, true);
    const bool sig = !c.mSig.empty() && !c.mBack;
    const LLColor4 sigCol = !sig ? C_DIM : c.mSig == "red" ? C_RED : c.mSig == "yellow" ? C_AMBER : C_GREEN;
    circleF(X(981.f), Y(152.f), S(30.f), LLColor4(0.f, 0.f, 0.f, 0.8f));
    lamp(X(981.f), Y(152.f), S(24.f), sigCol, LLColor4(0.12f, 0.12f, 0.12f, 1.f), sig);
    if (sig)
    {
        T(X(981.f), Y(104.f), c.mSig == "red" ? "STOP" : c.mSig == "yellow" ? "CAUTION" : "CLEAR", 13.f, sigCol, true);
        T(X(981.f), Y(80.f), "in " + distance(c.mSigM), 10.f, C_GREY);
        T(X(981.f), Y(52.f), c.mSig == "red" ? "The train waits" : c.mSig == "yellow" ? "Next one red" : "Line clear", 9.f, C_GREY);
    }
    else
    {
        T(X(981.f), Y(104.f), "None", 12.f, C_GREY, true);
        T(X(981.f), Y(80.f), "in sight", 10.f, C_GREY);
    }

    // ── right: the line ahead, the lamp, the horn ─────────────────────────────────────────────────
    panel(X(1050.f), Y(14.f), X(1592.f), Y(226.f), true, brass, th.inset, th.accentLo);
    T(X(1070.f), Y(208.f), c.mBack ? "THE LINE BEHIND" : "THE LINE AHEAD", 10.f, th.accent, true, LLFontGL::LEFT);
    if (c.mHasLead)
    {
        // [wolf-rail 1.3, stage V] the PROPELLING lamp (DESIGN-terminus.md 5.4): lead=0 - this cab is not at the end that
        // leads, looking the way the train goes (RailEngine.cs Lead), so the train is pushed; lead=1 - this cab leads
        if (tr.leadFlash())
        {
            // [wolf-rail 1.3.7] just after a change of ends: the same words, larger, black on a filled box - amber PROPELLING,
            // green YOUR CAB LEADS (the words say it, never the colour alone). Within the LINE AHEAD panel's top row
            // (X 1388-1588, Y 195-221: under its top frame at Y 224, over the station line, 14 at Y 180). Contrast (WCAG 2):
            // black text on C_AMBER 10.9:1, on C_GREEN 14.0:1; the box against the darkest themes' insets (themeFor) at least
            // 10.1:1 (amber) and 13.0:1 (green). Fixed colours: no skin changes them.
            const LLColor4 fill = c.mLead ? C_GREEN : C_AMBER;
            const LLColor4 ink(0.f, 0.f, 0.f, 1.f);
            rectF(X(1388.f), Y(195.f), X(1588.f), Y(221.f), fill);
            circleF(X(1401.f), Y(208.f), S(8.f), ink);
            circleF(X(1401.f), Y(208.f), S(5.f), fill);
            T(X(1414.f), Y(208.f), fitW(c.mLead ? "YOUR CAB LEADS" : "PROPELLING", 12.f, true, S(170.f)), 12.f, ink, true, LLFontGL::LEFT);
        }
        else
        {
            circleF(X(1400.f), Y(208.f), S(7.f), c.mLead ? LLColor4(0.25f, 0.2f, 0.05f, 1.f) : C_AMBER);
            T(X(1414.f), Y(208.f), c.mLead ? "YOUR CAB LEADS" : "PROPELLING", 10.f, c.mLead ? C_GREY : C_AMBER, true, LLFontGL::LEFT);
        }
        addHit(X(1390.f), Y(198.f), X(1585.f), Y(218.f), H_NONE, c.mLead ? "You are in the cab at the front of the train"
                                                                         : "Propelling: your cab is not at the front, so the train is pushed from behind you");
    }
    std::string stop_line = "No station in sight";
    if (c.mBack) stop_line = "Backing";   // [wolf-rail 1.2] ("ahead" is the line behind while it backs)
    else if (c.mStopM >= 0.f) stop_line = (c.mStop == "stop" ? "End of the run in " : "Station in ") + distance(c.mStopM);
    T(X(1070.f), Y(180.f), stop_line, 14.f, C_WHITE, true, LLFontGL::LEFT);
    T(X(1070.f), Y(152.f), (c.mBack ? "Track behind you: " : "Track in front of you: ") + distance(c.mAhead), 11.f, C_GREY, false, LLFontGL::LEFT);
    // the track strip: the engine on the left, the stop and the end of the line it can see
    const F32 sl = X(1070.f), sr = X(1380.f), sy = Y(124.f);
    lineW(sl, sy, sr, sy, S(3.f), C_GREY);
    const F32 span = llmax(100.f, c.mAhead);
    if (c.mStopM >= 0.f && c.mStopM <= span)
    {
        const F32 px = sl + (sr - sl) * (c.mStopM / span);
        rectF(px - S(3.f), sy - S(10.f), px + S(3.f), sy + S(10.f), c.mStop == "stop" ? C_RED : C_GREEN);
    }
    rectF(sr - S(3.f), sy - S(12.f), sr + S(3.f), sy + S(12.f), C_RED);   // where the line it sees ends
    rectF(sl - S(8.f), sy - S(6.f), sl + S(8.f), sy + S(6.f), th.accent);  // the engine
    // the protection lamp: lit while the line makes the train brake
    lamp(X(1420.f), Y(124.f), S(14.f), C_RED, LLColor4(0.25f, 0.05f, 0.04f, 1.f), c.mProtect);
    T(X(1500.f), Y(132.f), "LINE", 9.f, c.mProtect ? C_RED : C_GREY, true);
    T(X(1500.f), Y(116.f), "BRAKING", 9.f, c.mProtect ? C_RED : C_GREY, true);
    // [wolf-rail 1.3.7] the region's note (where to go at the end of the line, the turntable's progress, a points setting...):
    // drawn below, over the panel, so it cannot be missed. (A refusal is shown once, in the red strip that stays until OK.)
    // the horn (held while pressed) and the mode switch
    const bool horn = tr.horn();
    key(X(1070.f), Y(26.f), X(1300.f), Y(74.f), horn, mPressed == H_HORN, horn ? th.accent : th.accentLo);
    T(X(1185.f), Y(50.f), th.horn, 16.f, kit ? (horn ? C_KEYLIT : C_KEYTEXT) : horn ? LLColor4(0.f, 0.f, 0.f, 1.f) : C_WHITE, true);
    addHit(X(1070.f), Y(26.f), X(1300.f), Y(74.f), H_HORN, std::string("Sound the ") + (th.horn == std::string("WHISTLE") ? "whistle" : th.horn == std::string("GONG") ? "gong" : "horn") + " (hold)");
    key(X(1320.f), Y(26.f), X(1580.f), Y(74.f), false, mPressed == H_MODE, LLColor4(0.f, 0.f, 0.f, 0.45f));
    T(X(1450.f), Y(50.f), easy ? "SWITCH TO DRIVER'S MODE" : "SWITCH TO EASY DRIVING", 10.f, kit ? C_KEYTEXT : th.accent, true);
    addHit(X(1320.f), Y(26.f), X(1580.f), Y(74.f), H_MODE, easy ? "Switch to the driver's own levers - F2 (the train stops first)" : "Switch to the one-lever Easy driving - F2 (the train stops first)");

    // what is drawn over the panel stacks upwards from its top: the note box, the refusal strip, the guide card
    F32 above = top;
    bool stacked = false;

    // [wolf-rail 1.3.7] THE NOTE: Paul 14 Nov - a stop guide halted his shunter at the end of a shed road and he did not know
    // how to get out. 1.3.7 says how in the note, which was one small amber line cut at 510 units. Now: whole words on up to
    // three lines at 15 (was 11), white on an opaque near-black box with a 3-unit amber border, just above the panel over the
    // SIGNAL and LINE AHEAD panels (X 922-1592). Inside the panel there is no room for it: between the horn / mode buttons
    // (top Y 74) and the track strip and LINE BRAKING lamp (bottom Y 110) are 36 units, one line. Contrast (WCAG 2 relative
    // luminance, sRGB): C_WHITE 0.96 (L 0.911) on (0.06, 0.05, 0.03) (L 0.0040) is 17.8:1; the C_AMBER border on it 10.1:1.
    // The colours are this file's own constants (no skin or colour table reads them): every viewer skin and theme is the same.
    if (!c.mNote.empty() && c.mNote != tr.alert())
    {
        const F32 l = X(922.f), r = X(1592.f);
        const std::vector<std::string> lines = wrapW(c.mNote, 15.f, true, S(630.f), 3);
        const F32 step = S(22.f);
        const F32 b = above + S(8.f), t = b + step * (F32)lines.size() + S(20.f);
        rectF(l, b, r, t, LLColor4(0.06f, 0.05f, 0.03f, 1.f));
        frameW(l, b, r, t, S(3.f), C_AMBER);
        for (size_t i = 0; i < lines.size(); ++i)
        {
            T(l + S(20.f), t - S(21.f) - step * (F32)i, lines[i], 15.f, C_WHITE, true, LLFontGL::LEFT);
        }
        addHit(l, b, r, t, H_NONE, c.mNote);   // the whole note as a tooltip (a very long one is cut at the third line)
        above = t;
        stacked = true;
    }

    // [wolf-rail 1.3, stage V] a refusal of CHANGE ENDS, COUPLE, UNCOUPLE or [1.3.7] TURNTABLE: in a red strip above the panel
    // (and the note), in the region's own words, until the driver presses OK (the region's note is gone after 4 s:
    // EngineDriver.cs Note)
    mShowAlert = !tr.alert().empty();
    if (mShowAlert)
    {
        // [HUD KIT 2026-11-25] 64 high (was 56): its OK key 44 high
        const F32 l = W * 0.5f - S(560.f), r = W * 0.5f + S(560.f), b = above + S(8.f), t = b + S(64.f);
        mAlertL = l * mHitScale; mAlertR = r * mHitScale; mAlertB = b * mHitScale; mAlertT = t * mHitScale;
        rectF(l, b, r, t, LLColor4(0.30f, 0.02f, 0.02f, 0.96f));
        frameW(l, b, r, t, S(2.f), C_RED);
        T(l + S(20.f), (b + t) * 0.5f, fitW(tr.alert(), 12.f, true, S(950.f)), 12.f, C_WHITE, true, LLFontGL::LEFT);
        key(r - S(110.f), b + S(10.f), r - S(14.f), t - S(10.f), false, mPressed == H_ALERT_OK, C_WHITE);
        T(r - S(62.f), (b + t) * 0.5f, "OK", 13.f, WolfHudKit::ready() ? C_KEYTEXT : LLColor4(0.f, 0.f, 0.f, 1.f), true);
        addHit(r - S(110.f), b + S(10.f), r - S(14.f), t - S(10.f), H_ALERT_OK, "Close this message (Escape)");
        addHit(l, b, r - S(112.f), t, H_NONE, tr.alert());   // the whole text as a tooltip (it may be cut to fit)
        above = t;
        stacked = true;
    }

    if (mShowHelp) drawHelp(above, stacked, W);
    zoomed.reset();
    mResize.draw();
}

void WolfTrainCabView::drawHelp(F32 above, bool stacked, F32 W)
{
    mHelpPage = llclamp(mHelpPage, 0, CARD_COUNT - 1);
    const Card& cd = CARDS[mHelpPage];
    // (above the refusal strip while one is shown, [stage V], and [1.3.7] the note box: 8 over what is stacked, 16 over the panel)
    const F32 l = W * 0.5f - S(420.f), r = W * 0.5f + S(420.f), b = above + S(stacked ? 8.f : 16.f), t = b + S(230.f);
    mHelpL = l * mHitScale; mHelpR = r * mHitScale; mHelpB = b * mHitScale; mHelpT = t * mHitScale;
    rectF(l, b, r, t, LLColor4(0.02f, 0.03f, 0.02f, 0.94f));
    frameW(l, b, r, t, S(2.f), LLColor4(0.86f, 0.69f, 0.34f, 1.f));
    const F32 cx = (l + r) * 0.5f;
    T(cx, t - S(30.f), cd.title, 18.f, LLColor4(0.86f, 0.69f, 0.34f, 1.f), true);
    T(cx, t - S(74.f), cd.l1, 12.f, C_WHITE);
    T(cx, t - S(102.f), cd.l2, 12.f, C_WHITE);
    T(cx, t - S(130.f), cd.l3, 12.f, C_WHITE);
    // [1.3.7] a card's fourth line (only "Changing ends" has one), between the third (t - 130) and the dots (b + 62 = t - 168)
    if (cd.l4) T(cx, t - S(154.f), cd.l4, 12.f, C_WHITE);
    for (S32 i = 0; i < CARD_COUNT; ++i)   // the dots
    {
        circleF(cx + S(22.f) * ((F32)i - (F32)(CARD_COUNT - 1) * 0.5f), b + S(62.f), S(5.f), i == mHelpPage ? LLColor4(0.86f, 0.69f, 0.34f, 1.f) : C_DIM);
    }
    if (mHelpPage > 0)
    {
        // [HUD KIT 2026-11-25] Back and Next keys 44 high (were 32), the dots above them
        key(l + S(20.f), b + S(8.f), l + S(150.f), b + S(52.f), false, mPressed == H_HELP_BACK, LLColor4(0.f, 0.f, 0.f, 0.6f));
        T(l + S(85.f), b + S(30.f), "< Back", 12.f, C_KEYTEXT, true);
        addHit(l + S(20.f), b + S(8.f), l + S(150.f), b + S(52.f), H_HELP_BACK, "");
    }
    const bool last = mHelpPage >= CARD_COUNT - 1;
    key(r - S(150.f), b + S(8.f), r - S(20.f), b + S(52.f), true, mPressed == H_HELP_NEXT || mPressed == H_HELP_CLOSE, LLColor4(0.86f, 0.69f, 0.34f, 1.f));
    T(r - S(85.f), b + S(30.f), last ? "Got it" : "Next >", 12.f, WolfHudKit::ready() ? C_KEYLIT : LLColor4(0.f, 0.f, 0.f, 1.f), true);
    addHit(r - S(150.f), b + S(8.f), r - S(20.f), b + S(52.f), last ? H_HELP_CLOSE : H_HELP_NEXT, "");
}

void WolfTrainCabView::press(EHit id, S32 y)
{
    WolfTrain& tr = WolfTrain::instance();
    switch (id)
    {
    case H_NOTCH_STOP: tr.setNotch(WolfTrain::NOTCH_STOP); break;
    case H_NOTCH_SLOW: tr.setNotch(WolfTrain::NOTCH_SLOW); break;
    case H_NOTCH_GO:   tr.setNotch(WolfTrain::NOTCH_GO); break;
    case H_POWER:
    case H_BRAKE:      dragLever(y); break;
    case H_REV_FWD:    tr.setReverser(1); break;
    case H_REV_N:      tr.setReverser(0); break;
    case H_REV_REV:    tr.setReverser(-1); break;
    case H_HORN:       tr.setHorn(true); break;
    case H_AUTO:       tr.pressAuto(); break;
    case H_POINTS:     tr.pressPoints(); break;
    case H_EASY_FWD:   tr.setEasyBack(false); break;
    case H_EASY_BACK:  tr.setEasyBack(true); break;
    case H_MODE:       tr.setEasy(!tr.easy()); break;
    case H_UNITS:      tr.setMph(!tr.mph()); break;
    case H_HELP:       mShowHelp = !mShowHelp; mHelpPage = 0; break;
    case H_HELP_BACK:  mHelpPage = llmax(0, mHelpPage - 1); break;
    case H_HELP_NEXT:  mHelpPage = llmin(CARD_COUNT - 1, mHelpPage + 1); break;
    case H_HELP_CLOSE: mShowHelp = false; break;
    case H_CLOSE:      mShowHelp = false; tr.toggleShown(); break;
    case H_SHOW_TAB:   tr.toggleShown(); break;
    case H_ENDS:       tr.pressEnds(); break;       // [wolf-rail 1.3, stage V]
    case H_COUPLE:     tr.pressCouple(); break;
    case H_UNCOUPLE:   tr.pressUncouple(); break;
    case H_TURNTABLE:  tr.pressTurntable(); break;   // [wolf-rail 1.3.7]
    case H_ALERT_OK:   tr.dismissAlert(); break;
    case H_WALK:       tr.dismissWalk(); break;
    default: break;
    }
}

void WolfTrainCabView::dragLever(S32 y)
{
    // the view point to the picture, then the slot's 0..1 (Y(36) .. Y(196) when drawn)
    const F32 py = (F32)y / mHitScale;
    const F32 v = llclamp((py - mLeverB) / llmax(1.f, mLeverT - mLeverB), 0.f, 1.f);
    WolfTrain& tr = WolfTrain::instance();
    if (mPressed == H_POWER) tr.setPower(v);
    else if (mPressed == H_BRAKE) tr.setBrake(v);
}

bool WolfTrainCabView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (!onPanel(x, y)) return false;
    const Hit* hit = hitAt(x, y);
    if (!hit) return true;   // the panel itself: not a click on the world behind it
    mPressed = hit->mId;
    gFocusMgr.setMouseCapture(this);
    press(hit->mId, y);
    return true;
}

bool WolfTrainCabView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE || mResize.dragging() || mResize.onGrip(x, y);
    mResize.end();
    if (mPressed == H_HORN) WolfTrain::instance().setHorn(false);
    if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
    mPressed = H_NONE;
    return was || onPanel(x, y);
}

bool WolfTrainCabView::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && (mPressed == H_POWER || mPressed == H_BRAKE))
    {
        dragLever(y);
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    if (hasMouseCapture() && mResize.dragging())
    {
        mResize.drag(y);
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && mDrawn && mResize.onGrip(x, y))
    {
        mResize.hovered();
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && !onPanel(x, y)) return false;
    // (a hit that is only a tooltip - [stage V] the PROPELLING lamp, the refusal's text - is not a button)
    const Hit* h = hitAt(x, y);
    getWindow()->setCursor(h && h->mId != H_NONE ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfTrainCabView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfTrainCabView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();
        return true;
    }
    return onPanel(x, y) ? handleMouseDown(x, y, mask) : false;
}

bool WolfTrainCabView::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());
        return true;
    }
    if (!onPanel(x, y)) return false;
    if (const Hit* hit = hitAt(x, y))
    {
        if (!hit->mTip.empty()) LLToolTipMgr::instance().show(hit->mTip);
    }
    return true;
}

void WolfTrainCabView::onMouseCaptureLost()
{
    mResize.end();
    if (mPressed == H_HORN) WolfTrain::instance().setHorn(false);
    mPressed = H_NONE;
    LLUICtrl::onMouseCaptureLost();
}
