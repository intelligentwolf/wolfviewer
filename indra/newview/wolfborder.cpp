/**
 * @file wolfborder.cpp
 * @brief WolfViewer: border warnings and the hop across a refusing region. See wolfborder.h.
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

#include "wolfborder.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "fscommon.h"
#include "llagent.h"
#include "llcallbacklist.h"
#include "lldispatcher.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llregionflags.h"
#include "llrender2dutils.h"
#include "lltooltip.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llwindow.h"
#include "llworld.h"
#include "llworldmap.h"
#include "wolfgrid.h"
#include "wolfspeech.h"
#include "xform.h"

static LLDefaultChildRegistry::Register<WolfBorderBanner> r_wolf_border_banner("wolf_border_banner");

namespace
{
    // Source: WolfBorderModule.cs METHOD_IN / MESSAGE_OUT
    const std::string METHOD_TO_SIM("wolfborder");
    const std::string MESSAGE_FROM_SIM("WolfBorder");

    // Source: WolfBorderModule.cs LookMinM / LookSeconds - "ahead" is within this, or this many seconds of travel
    const F32 LOOK_MIN_M = 256.f;
    const F32 LOOK_SECONDS = 20.f;
    // The region allows a bucket of 4 looks, one back every 2 s (WolfBorderModule.cs LOOK_BUCKET / LOOK_REFILL).
    const F64 LOOK_EVERY_S = 2.0;
    const F64 LOOK_FRESH_S = 5.0;
    // The hop is asked for this close to the border: within the region's HopTriggerM (48 m) / HopTriggerSeconds (4 s),
    // late enough that the way ahead is certain, early enough that the region answers before the edge.
    const F32 HOP_AT_M = 12.f;
    const F32 HOP_AT_SECONDS = 1.5f;
    // One hop per 20 s per object (Scene.InTeleportTargetsCoolDown(..., 20000) in TeleportObject); the bucket there
    // is 2, one back every 20 s (WolfBorderModule.cs HOP_BUCKET / HOP_REFILL).
    const F64 HOP_EVERY_S = 6.0;
    const F64 HELLO_RETRY_S = 30.0;
    // Off the module: a refusing region is remembered this long (wolfflight.cpp BLOCKED_KEEP_SECS, the flight deck's).
    const F64 REFUSED_KEEP_S = 600.0;
    // Past the far edge of a refusing region, where a walker's teleport offer lands.
    const F64 TELEPORT_PAST_M = 8.0;
    // The same warning is said (chat, voice) at most once in this long, however often the banner comes and goes.
    const F64 ANNOUNCE_AGAIN_S = 60.0;
    // A warning (held at the border, not allowed in) stays while the vehicle or person is still at the edge, up to this.
    const F64 WARNING_HOLD_MAX_S = 60.0;
    const F64 WARNING_EDGE_M = 20.0;

    // How far the region can put a walker's teleport target ("tx/ty/tz") from them: WolfBorderModule.cs Survey
    // (:804-880) finds the border within the look's reach, max(LookMinM, speed * LookSeconds) (:822-825), steps StepM
    // past it (:836), goes on at most BorderHopMaxDistance (:865, the setting clamped to 16000 m at most, :159), and
    // puts the arrival ArriveInM inside the far region (:868).
    const F64 HOP_MAX_DISTANCE_CAP_M = 16000.0;   // WolfBorderModule.cs:159 Math.Clamp(..., 256f, 16000f)
    const F64 STEP_M = 0.5;                       // WolfBorderModule.cs:110 StepM
    const F64 ARRIVE_IN_M = 6.0;                  // WolfBorderModule.cs:109 ArriveInM

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    // Source: wolfdrive.cpp seatRoot / wolfflight.cpp vehicleRoot
    LLViewerObject* seatRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            return nullptr;
        }
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent())
        {
            p = (LLViewerObject*)p->getParent();
        }
        return p;
    }

    /** Distance along dir (unit, horizontal) from p to where it leaves [x0,x1) x [y0,y1); +inf when it never does.
     *  Source: WolfBorderModule.cs ExitDistance. */
    F64 exitDistance(const LLVector3d& p, const LLVector3d& dir, F64 x0, F64 y0, F64 x1, F64 y1)
    {
        F64 t = std::numeric_limits<F64>::infinity();
        if (dir.mdV[VX] > 1e-6) t = std::min(t, (x1 - p.mdV[VX]) / dir.mdV[VX]);
        else if (dir.mdV[VX] < -1e-6) t = std::min(t, (x0 - p.mdV[VX]) / dir.mdV[VX]);
        if (dir.mdV[VY] > 1e-6) t = std::min(t, (y1 - p.mdV[VY]) / dir.mdV[VY]);
        else if (dir.mdV[VY] < -1e-6) t = std::min(t, (y0 - p.mdV[VY]) / dir.mdV[VY]);
        return std::isfinite(t) ? std::max(0.0, t) : t;
    }

    std::string capitalised(std::string s)
    {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        {
            s[0] = (char)(s[0] - 'a' + 'A');
        }
        if (!s.empty() && s.back() != '.')
        {
            s += ".";
        }
        return s;
    }

    // A distance as the banner shows it: whole metres under 1 km, else tenths of a km (1000000 + tenths); -1 none.
    S32 distanceUnits(F32 m)
    {
        if (m < 0.f) return -1;
        if (m >= 1000.f) return 1000000 + (S32)std::lround(m / 100.f);
        return (S32)std::lround(m);
    }

    std::string distanceText(S32 units)
    {
        if (units < 0) return std::string();
        if (units >= 1000000) return llformat("%d.%d km", (units - 1000000) / 10, (units - 1000000) % 10);
        return llformat("%d m", units);
    }

    /** The vehicle (or us) and its horizontal speed, as idle() reads them. */
    LLVector3d travellerPos(F64& speed)
    {
        LLViewerObject* root = seatRoot();
        const LLVector3 v = root ? root->getVelocity() : gAgentAvatarp->getVelocity();
        speed = LLVector3d(v.mV[VX], v.mV[VY], 0.0).magVec();
        return root ? root->getPositionGlobal() : gAgent.getPositionGlobal();
    }

    /** A teleport target from the wire is only used when the region could have meant it: inside what a region handle
     *  holds (teleportViaLocation -> to_region_handle casts global metres to U32, llregionhandle.h:65-73), inside
     *  OpenSim's height range (xform.h OS_MIN_OBJECT_Z / OS_MAX_OBJECT_Z), and no farther from the traveller than
     *  WolfBorderModule.cs can put it (see HOP_MAX_DISTANCE_CAP_M), plus what they cover while a look is fresh. */
    bool plausibleTarget(const LLVector3d& target, const LLVector3d& from, F64 speed)
    {
        for (S32 i : { VX, VY })
        {
            if (!(target.mdV[i] > 0.0 && target.mdV[i] < 4294967296.0)) return false;
        }
        if (!(target.mdV[VZ] >= OS_MIN_OBJECT_Z && target.mdV[VZ] <= OS_MAX_OBJECT_Z)) return false;
        const F64 reach = std::max((F64)LOOK_MIN_M, speed * LOOK_SECONDS);
        const F64 most = reach + STEP_M + HOP_MAX_DISTANCE_CAP_M + ARRIVE_IN_M + speed * LOOK_FRESH_S;
        const F64 dx = target.mdV[VX] - from.mdV[VX], dy = target.mdV[VY] - from.mdV[VY];
        return dx * dx + dy * dy <= most * most;
    }

    bool sameWords(const WolfBorder::Banner& a, const WolfBorder::Banner& b)
    {
        return a.mLevel == b.mLevel && a.mOfferTeleport == b.mOfferTeleport && a.mTitle == b.mTitle && a.mReason == b.mReason
            && a.mOutcome == b.mOutcome && a.mTeleportLabel == b.mTeleportLabel;
    }
}

/** Source: wolfdrive.cpp WolfDashboardPushHandler - registered with gGenericDispatcher. */
class WolfBorderPushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t& strings) override
    {
        WolfBorder::instance().receive(invoice, strings);
        return true;
    }
};

namespace
{
    WolfBorderPushHandler sBorderHandler;
}

WolfBorder::WolfBorder()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sBorderHandler);
    }
    gIdleCallbacks.addFunction(&WolfBorder::onIdle, nullptr);
}

WolfBorder::~WolfBorder()
{
    gIdleCallbacks.deleteFunction(&WolfBorder::onIdle, nullptr);
}

// static
void WolfBorder::onIdle(void*)
{
    if (WolfBorder::instanceExists())
    {
        WolfBorder::instance().idle();
    }
}

void WolfBorder::sendLook(bool force)
{
    const F64 t = now();
    if (!force && t - mLookSentAt < LOOK_EVERY_S)
    {
        return;
    }
    mLookSentAt = t;
    send_generic_message(METHOD_TO_SIM, { "look" });
}

void WolfBorder::sendHop(const std::string& why)
{
    static LLCachedControl<bool> hop_on(gSavedSettings, "WolfBorderHop", true);
    const F64 t = now();
    if (!hop_on || t - mHopSentAt < HOP_EVERY_S)
    {
        return;
    }
    mHopSentAt = t;
    LL_INFOS("WolfBorder") << "asking the region to carry the vehicle across " << mLook.mAhead << " (" << why << ")" << LL_ENDL;
    send_generic_message(METHOD_TO_SIM, { "hop" });
}

void WolfBorder::receive(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // Only from a Wolf Territories region that has said so itself (WolfGrid::isOnWolfTerritoriesConfirmed,
    // wolfgrid.h - the gate idle() sends "hello" behind): on any other grid "WolfBorder" is not ours, and a foreign
    // region could raise banners or put a teleport target on offer.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || invoice != region->getRegionID())
    {
        return;   // only the region we are in (a child region's message is not about us)
    }
    std::string ev, state, plan, ahead, why, to;
    F64 rx = 0, ry = 0, rw = 0, rh = 0, tx = 0, ty = 0, tz = 0;
    bool has_t = false;
    for (const std::string& kv : strings)
    {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        // never trust the wire: text is shown as text, numbers are parsed and checked
        auto num = [&v](F64& out) { try { size_t n = 0; F64 d = std::stod(v, &n); if (n == v.size() && std::isfinite(d)) out = d; } catch (...) {} };
        if (k == "event") ev = v;
        else if (k == "state") state = v;
        else if (k == "plan") plan = v;
        else if (k == "ahead") ahead = utf8str_truncate(v, 100);
        else if (k == "why") why = utf8str_truncate(v, 200);
        else if (k == "to") to = utf8str_truncate(v, 100);
        else if (k == "rx") num(rx);
        else if (k == "ry") num(ry);
        else if (k == "rw") num(rw);
        else if (k == "rh") num(rh);
        else if (k == "tx") { num(tx); has_t = true; }
        else if (k == "ty") num(ty);
        else if (k == "tz") num(tz);
    }
    const F64 t = now();
    if (ev == "hello")
    {
        mModuleRegion = invoice;
        return;
    }
    mModuleRegion = invoice;   // anything from it says it has the module
    if (ev == "look")
    {
        mLook = Look();
        mLook.mAt = t;
        mLook.mState = state;
        mLook.mPlan = plan;
        mLook.mAhead = ahead;
        mLook.mWhy = why;
        mLook.mTo = to;
        mLook.mRX = rx; mLook.mRY = ry; mLook.mRW = rw; mLook.mRH = rh;
        mLook.mTarget.setVec(tx, ty, tz);
        mLook.mHasTarget = false;
        if (has_t && isAgentAvatarValid())
        {
            F64 speed = 0.0;
            const LLVector3d from = travellerPos(speed);
            mLook.mHasTarget = plausibleTarget(mLook.mTarget, from, speed);
        }
        return;
    }
    static LLCachedControl<bool> hop_on(gSavedSettings, "WolfBorderHop", true);
    Banner b;
    if (ev == "held")
    {
        b.mLevel = L_WARNING;
        b.mTitle = "STOPPED AT THE BORDER: " + ahead;
        b.mReason = capitalised(why);
        const bool hop = hop_on && mLook.mPlan == "hop" && mLook.mAhead == ahead && t - mLook.mAt < LOOK_FRESH_S * 2;
        b.mOutcome = hop ? "Asking the region to carry you across to " + mLook.mTo + "..." : "Your vehicle is held here. Turn back, or find another way round.";
        setEvent(b, 8.0);
        if (hop)
        {
            sendHop("held at the border");
        }
    }
    else if (ev == "hop")
    {
        b.mLevel = L_INFO;
        b.mTitle = "CARRIED ACROSS: " + ahead;
        b.mReason = capitalised(why);
        b.mOutcome = "Coming out in " + to + ".";
        setEvent(b, 8.0);
    }
    else if (ev == "hopfail")
    {
        b.mLevel = L_CAUTION;
        b.mTitle = ahead.empty() ? std::string("CANNOT CARRY YOU ACROSS") : "CANNOT CARRY YOU ACROSS: " + ahead;
        b.mReason = capitalised(why);
        b.mOutcome = "Your vehicle will stop at the border.";
        setEvent(b, 6.0);
    }
}

void WolfBorder::setEvent(const Banner& b, F64 seconds)
{
    mEventBanner = b;
    mEventKey = b.mTitle + "|" + b.mReason;
    mEventSince = now();
    mEventUntil = mEventSince + seconds;
}

bool WolfBorder::rebuild(S32 kind, F64 a, F64 b, bool hop, bool seated)
{
    if (mBuilt.mKind == kind && mBuilt.mA == a && mBuilt.mB == b && mBuilt.mHop == hop && mBuilt.mSeated == seated)
    {
        return false;
    }
    mBuilt.mKind = kind;
    mBuilt.mA = a;
    mBuilt.mB = b;
    mBuilt.mHop = hop;
    mBuilt.mSeated = seated;
    mBuilt.mBanner = Banner();
    mBuilt.mKey.clear();
    return true;
}

void WolfBorder::onAlert(const std::string& message)
{
    // Source: wolfflight.cpp onCrossingRefused (the flight deck's refusal texts) and what OpenSim sends a walker
    // whose crossing is refused: EntityTransferModule.CrossAsync -> SendAlertMessage(failureReason), the reason
    // from GetDestination ("Access Denied or Temporary not possible", "Access Denied") or QueryAccess - Scene.cs
    // AuthorizeUser ("Denied access to region X: ...", "Denied access to private region X: ...") and
    // CheckLandPositionAccess ("The region is full", "You dont have access to the region parcels", "You are banned
    // from the parcel", "The parcel is restricted", "Region Crossing not allowed").
    std::string text = (!message.empty() && message[0] == '/') ? message.substr(1) : message;
    static const char* const EXACT[] = {
        "Access Denied or Temporary not possible", "Access Denied", "The region is full",
        "You dont have access to the region parcels", "You are banned from the parcel", "The parcel is restricted",
        "Region Crossing not allowed" };
    bool refusal = text.rfind("Denied access to region ", 0) == 0 || text.rfind("Denied access to private region ", 0) == 0;
    for (const char* e : EXACT)
    {
        refusal |= text == e;
    }
    // a teleport's own failure says "Access Denied" too: only a crossing's refusal is about the region ahead
    if (!refusal || !isAgentAvatarValid() || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        return;
    }
    LLViewerRegion* here = gAgent.getRegion();
    if (!here)
    {
        return;
    }
    // The region refusing: the one just ahead (the crossing that was tried), as the flight deck finds it.
    LLViewerObject* root = seatRoot();
    LLVector3d pos = root ? root->getPositionGlobal() : gAgent.getPositionGlobal();
    LLVector3 v = root ? root->getVelocity() : gAgentAvatarp->getVelocity();
    LLVector3d dir(v.mV[VX], v.mV[VY], 0.0);
    if (dir.magVec() < 0.5)
    {
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        dir.setVec(at.mV[VX], at.mV[VY], 0.0);
    }
    if (dir.magVec() < 1e-3)
    {
        return;
    }
    dir.normVec();
    const LLVector3d o = here->getOriginGlobal();
    const F64 w = here->getWidth();
    const F64 to_edge = exitDistance(pos, dir, o.mdV[VX], o.mdV[VY], o.mdV[VX] + w, o.mdV[VY] + w);
    if (!std::isfinite(to_edge) || to_edge > 48.0)
    {
        return;
    }
    LLVector3d probe = pos + dir * (to_edge + 2.0);
    LLVector3d lo, hi;
    std::string name;
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosGlobal(probe);
    if (r && r != here)
    {
        lo = r->getOriginGlobal();
        hi = lo + LLVector3d(r->getWidth(), r->getWidth(), 0.0);
        name = r->getName();
    }
    else if (LLSimInfo* sim = LLWorldMap::getInstance()->simInfoFromPosGlobal(probe))
    {
        lo = sim->getGlobalOrigin();
        hi = lo + LLVector3d(sim->getSizeX(), sim->getSizeY(), 0.0);
        name = sim->getName();
    }
    else
    {
        return;
    }
    const F64 t = now();
    bool known = false;
    for (Refused& f : mRefused)
    {
        if (f.mMin == lo && f.mMax == hi)
        {
            f.mUntil = t + REFUSED_KEEP_S;
            known = true;
        }
    }
    if (!known)
    {
        mRefused.push_back({ lo, hi, name, t + REFUSED_KEEP_S });
    }

    // What happens now: a walker is offered an ordinary teleport to the far side (the destination checks it as any
    // teleport); a vehicle stopped (CrossAsyncCompleted, stock).
    Banner b;
    b.mLevel = L_WARNING;
    b.mTitle = "NOT ALLOWED IN: " + name;
    b.mReason = capitalised(text);
    if (root)
    {
        b.mOutcome = "Your vehicle stopped at the border. Turn back, or find another way round.";
        mHasOffer = false;
    }
    else
    {
        const F64 across = exitDistance(probe, dir, lo.mdV[VX], lo.mdV[VY], hi.mdV[VX], hi.mdV[VY]);
        if (std::isfinite(across))
        {
            mTeleportOffer = probe + dir * (across + TELEPORT_PAST_M);
            mTeleportOffer.mdV[VZ] = pos.mdV[VZ];
            mHasOffer = true;
            mOfferFromLook = false;
            std::string far_name;
            if (LLSimInfo* far_sim = LLWorldMap::getInstance()->simInfoFromPosGlobal(mTeleportOffer))
            {
                far_name = far_sim->getName();
            }
            b.mOfferTeleport = true;
            b.mTeleportLabel = far_name.empty() ? std::string("Teleport to the far side") : "Teleport across to " + far_name;
            b.mOutcome = "You can't walk in. You can teleport to the far side instead.";
        }
        else
        {
            b.mOutcome = "You can't walk in.";
        }
    }
    setEvent(b, 12.0);
}

void WolfBorder::teleportAcross()
{
    if (!mHasOffer)
    {
        return;
    }
    // the region's target was checked when the banner was built; the walker may have moved since (a look is used for
    // LOOK_FRESH_S), so again from where they are now
    if (mOfferFromLook && isAgentAvatarValid())
    {
        F64 speed = 0.0;
        const LLVector3d from = travellerPos(speed);
        if (!plausibleTarget(mTeleportOffer, from, speed))
        {
            LL_WARNS("WolfBorder") << "teleport target " << mTeleportOffer << " is out of range from here, not used" << LL_ENDL;
            FSCommon::report_to_nearby_chat("Not teleported: the far side the region gave is too far from where you are now.");
            mHasOffer = false;
            return;
        }
    }
    LL_INFOS("WolfBorder") << "teleport across the refusing region to " << mTeleportOffer << LL_ENDL;
    mHasOffer = false;
    mEventUntil = 0.0;
    // an ordinary teleport request: the destination checks access as for any teleport
    gAgent.teleportViaLocation(mTeleportOffer);
}

void WolfBorder::clearBanner()
{
    mBanner = Banner();
}

void WolfBorder::announce(const Banner& b)
{
    // Said once per new warning: to nearby chat (where it can be read again, copied, and read by a screen reader
    // that follows chat), and spoken when the Wolf voice is on (wolfspeech.cpp towerOn - Wolf World > Tower Voice).
    std::string line = b.mTitle;
    if (!b.mReason.empty()) line += " - " + b.mReason;
    if (!b.mOutcome.empty()) line += " " + b.mOutcome;
    FSCommon::report_to_nearby_chat(line);
    if (WolfGrid::isOnWolfTerritories() && WolfSpeech::towerOn() && WolfSpeech::instanceExists())
    {
        std::string spoken = b.mTitle;
        LLStringUtil::replaceString(spoken, ": ", ", ");
        WolfSpeech::instance().announce(spoken + ". " + b.mOutcome);
    }
}

void WolfBorder::setBanner(const Banner& b, const std::string& announceKey, F32 dist)
{
    // idle() calls this every frame: the words are copied only when they change
    if (!sameWords(mBanner, b))
    {
        mBanner = b;
    }
    mBanner.mDist = dist;
    if (b.mLevel != L_NONE && !announceKey.empty() && announceKey != mAnnounced)
    {
        mAnnounced = announceKey;
        const F64 t = now();
        auto it = mAnnouncedAt.find(announceKey);
        if (it == mAnnouncedAt.end() || t - it->second > ANNOUNCE_AGAIN_S)
        {
            if (mAnnouncedAt.size() > 200)
            {
                mAnnouncedAt.clear();
            }
            mAnnouncedAt[announceKey] = t;
            announce(b);
        }
    }
    if (b.mLevel == L_NONE && !mAnnounced.empty())
    {
        mAnnounced.clear();
    }
}

void WolfBorder::idle()
{
    static LLCachedControl<bool> warnings_on(gSavedSettings, "WolfBorderWarnings", true);
    static LLCachedControl<bool> hop_on(gSavedSettings, "WolfBorderHop", true);
    LLViewerRegion* region = gAgent.getRegion();
    // the hop works with the warnings off too (Wolf World > Carry My Vehicle Across ... is its own switch): then the
    // looks still go out, but nothing is shown or said
    const bool show = warnings_on;
    if ((!show && !(hop_on && seatRoot())) || !isAgentAvatarValid() || !region)
    {
        clearBanner();
        mAnnounced.clear();
        return;
    }
    const F64 t = now();
    const LLUUID region_id = region->getRegionID();
    if (region_id != mLastRegion)
    {
        // a crossing or teleport: the old region's answers and events are about somewhere else now
        mLastRegion = region_id;
        mLook = Look();
        mEventUntil = 0.0;
        mHasOffer = false;
    }

    // Wolf Territories: tell the region this viewer shows border warnings (WolfBorderModule.cs sends "WolfBorder" only
    // to viewers that said so). Again after a while if it never answered (a region without the module stays silent).
    const bool wolf = WolfGrid::isOnWolfTerritoriesConfirmed();
    if (wolf && (mHelloRegion != region_id || (mModuleRegion != region_id && t - mHelloAt > HELLO_RETRY_S)))
    {
        mHelloRegion = region_id;
        mHelloAt = t;
        send_generic_message(METHOD_TO_SIM, { "hello" });
    }
    const bool module = wolf && mModuleRegion == region_id;

    // Where we are going: the vehicle (or us), along the horizontal velocity; standing still, where the camera looks.
    LLViewerObject* root = seatRoot();
    const LLVector3d pos = root ? root->getPositionGlobal() : gAgent.getPositionGlobal();
    const LLVector3 v = root ? root->getVelocity() : gAgentAvatarp->getVelocity();
    LLVector3d dir(v.mV[VX], v.mV[VY], 0.0);
    F64 speed = dir.magVec();
    if (speed < 0.5)
    {
        const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
        dir.setVec(at.mV[VX], at.mV[VY], 0.0);
        speed = 0.0;
    }
    const bool has_dir = dir.magVec() > 1e-3;
    if (has_dir)
    {
        dir.normVec();
    }
    const LLVector3d o = region->getOriginGlobal();
    const F64 w = region->getWidth();
    const bool inside = pos.mdV[VX] >= o.mdV[VX] && pos.mdV[VY] >= o.mdV[VY] && pos.mdV[VX] < o.mdV[VX] + w && pos.mdV[VY] < o.mdV[VY] + w;
    // not inside this region's square (a crossing under way): no border ahead of us here, so nothing to look at
    const F64 to_edge = (!has_dir || !inside) ? std::numeric_limits<F64>::infinity()
                      : exitDistance(pos, dir, o.mdV[VX], o.mdV[VY], o.mdV[VX] + w, o.mdV[VY] + w);
    const F64 reach = std::max((F64)LOOK_MIN_M, speed * LOOK_SECONDS);
    const bool approaching = std::isfinite(to_edge) && to_edge <= reach;
    // moving, or standing at the edge (held there, looking out)
    const bool interested = approaching && (speed >= 0.5 || to_edge < 30.0);

    if (mEventBanner.mLevel == L_WARNING && t >= mEventUntil && t < mEventSince + WARNING_HOLD_MAX_S
        && std::isfinite(to_edge) && to_edge < WARNING_EDGE_M && speed < 1.0)
    {
        mEventUntil = t + 0.5;   // still stopped at the border: the warning stays until they move off
    }
    if (t < mEventUntil)
    {
        if (show)
        {
            setBanner(mEventBanner, mEventKey, mEventBanner.mDist);
        }
        else
        {
            clearBanner();
        }
        return;
    }
    mHasOffer = mHasOffer && mBanner.mOfferTeleport;

    // What is shown comes from mBuilt, made again only when what it says changes (rebuild); the distance is passed
    // apart, so a frame on the way to the border builds no strings.
    const Built* shown = nullptr;
    if (module)
    {
        if (interested)
        {
            sendLook(false);
        }
        const bool fresh = t - mLook.mAt < LOOK_FRESH_S;
        if (interested && fresh && !mLook.mAhead.empty() && mLook.mState != "ok" && mLook.mState != "clear" && !mLook.mState.empty())
        {
            // a new look (its time), the hop switch, or sitting down / standing up changes the words
            if (rebuild(1, mLook.mAt, 0.0, hop_on, root != nullptr))
            {
                Banner& b = mBuilt.mBanner;
                b.mLevel = L_CAUTION;
                b.mTitle = "AHEAD: " + mLook.mAhead;
                b.mReason = capitalised(mLook.mWhy);
                const std::string& to = mLook.mTo;
                if (mLook.mPlan == "hop" && hop_on)
                {
                    b.mOutcome = "You'll be carried across to " + to + ".";
                }
                else if (mLook.mPlan == "over")
                {
                    b.mOutcome = to.empty() ? std::string("You'll be carried over it.") : "You'll be carried over it to " + to + ".";
                }
                else if (mLook.mPlan == "walk")
                {
                    b.mOutcome = "You can't walk in.";
                    // the target was checked when it came (receive); checked again here, where we are now
                    if (mLook.mHasTarget && !to.empty() && plausibleTarget(mLook.mTarget, pos, speed))
                    {
                        b.mOutcome += " You can teleport across to " + to + " instead.";
                        b.mOfferTeleport = true;
                        b.mTeleportLabel = "Teleport across to " + to;
                    }
                }
                else
                {
                    b.mOutcome = root ? "Your vehicle will stop at the border." : "You can't go in.";
                }
                mBuilt.mKey = b.mTitle + "|" + b.mReason + "|" + mLook.mPlan;
            }
            shown = &mBuilt;
            // at the border: ask the region for the hop (it decides, checks everyone, and moves the vehicle)
            if (mLook.mPlan == "hop" && hop_on && root && to_edge <= std::max((F64)HOP_AT_M, speed * HOP_AT_SECONDS))
            {
                sendHop("at the border");
            }
            if (mBuilt.mBanner.mOfferTeleport)
            {
                mTeleportOffer = mLook.mTarget;
                mHasOffer = true;
                mOfferFromLook = true;
            }
        }
    }
    else if (interested)
    {
        // Off the module: what the viewer itself knows of the region ahead.
        const LLVector3d probe = pos + dir * (to_edge + 2.0);
        LLViewerRegion* ahead = LLWorld::getInstance()->getRegionFromPosGlobal(probe);
        const Refused* refused = nullptr;
        mRefused.erase(std::remove_if(mRefused.begin(), mRefused.end(), [t](const Refused& f) { return f.mUntil <= t; }), mRefused.end());
        for (const Refused& f : mRefused)
        {
            if (probe.mdV[VX] >= f.mMin.mdV[VX] && probe.mdV[VX] < f.mMax.mdV[VX] && probe.mdV[VY] >= f.mMin.mdV[VY] && probe.mdV[VY] < f.mMax.mdV[VY])
            {
                refused = &f;
            }
        }
        if (refused)
        {
            if (rebuild(2, refused->mMin.mdV[VX], refused->mMin.mdV[VY], false, root != nullptr))
            {
                Banner& b = mBuilt.mBanner;
                b.mLevel = L_CAUTION;
                b.mTitle = "AHEAD: " + refused->mName;
                b.mReason = "It refused you a few minutes ago.";
                b.mOutcome = root ? "Your vehicle will stop at the border." : "You can't walk in.";
                mBuilt.mKey = b.mTitle + "|refused";
            }
            shown = &mBuilt;
        }
        else if (root && ahead && ahead != region && ahead->getRegionFlag(REGION_FLAGS_SKIP_SCRIPTS))
        {
            // Source: llviewerregion.cpp unpackRegionHandshake -> setRegionFlags: every connected region's flags,
            // a neighbour's too (OpenSim sends RegionHandshake on child circuits; LLClientView.cs SkipScripts when
            // RegionSettings.DisableScripts).
            const LLVector3d ahead_origin = ahead->getOriginGlobal();
            if (rebuild(3, ahead_origin.mdV[VX], ahead_origin.mdV[VY], false, true))
            {
                Banner& b = mBuilt.mBanner;
                b.mLevel = L_CAUTION;
                b.mTitle = "AHEAD: " + ahead->getName();
                b.mReason = "Scripts are switched off in that region.";
                b.mOutcome = "Your vehicle would stop working there.";
                mBuilt.mKey = b.mTitle + "|scripts";
            }
            shown = &mBuilt;
        }

        // A vehicle that drove up to the edge and stopped dead there: the region ahead did not take it (stock OpenSim
        // says nothing to a seated avatar: SceneObjectGroup.CrossAsyncCompleted only puts the vehicle back).
        if (root && ahead && ahead != region)
        {
            if (speed >= 2.0 && to_edge < 60.0)
            {
                mWasApproaching = true;
            }
            if (mWasApproaching && to_edge < 3.0 && speed < 0.5)
            {
                if (mStoppedSince == 0.0)
                {
                    mStoppedSince = t;
                }
                else if (t - mStoppedSince > 1.0)
                {
                    Banner s;
                    s.mLevel = L_WARNING;
                    s.mTitle = "STOPPED AT THE BORDER: " + ahead->getName();
                    s.mReason = "The region did not take your vehicle.";
                    s.mOutcome = "Turn back, or find another way round.";
                    setEvent(s, 8.0);
                    mWasApproaching = false;
                    mStoppedSince = 0.0;
                }
            }
            else
            {
                mStoppedSince = 0.0;
            }
        }
    }
    if (!root || !approaching)
    {
        mWasApproaching = false;
        mStoppedSince = 0.0;
    }
    if (!shown || !shown->mBanner.mOfferTeleport)
    {
        mHasOffer = false;
    }
    if (show)
    {
        static const Banner NONE;
        static const std::string NO_KEY;
        if (shown)
        {
            setBanner(shown->mBanner, shown->mKey, (F32)to_edge);
        }
        else
        {
            setBanner(NONE, NO_KEY, -1.f);
        }
    }
    else
    {
        clearBanner();
        mHasOffer = false;
    }
}

// ══════════════════════════════════════════════════════════════════════════════════════════════
// the banner
// ══════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // The flight deck's / dashboard's alert colours (wolfdashboard.cpp ISO 2575: red = act now, amber = caution), and
    // a neutral teal for "done". Text is near-white on a near-black panel: about 17:1.
    const LLColor4 C_PANEL(0.06f, 0.07f, 0.09f, 0.90f);
    const LLColor4 C_TEXT(0.96f, 0.96f, 0.96f, 1.f);
    const LLColor4 C_SUB(0.82f, 0.84f, 0.87f, 1.f);
    const LLColor4 C_AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 C_RED(1.00f, 0.25f, 0.20f, 1.f);
    const LLColor4 C_TEAL(0.25f, 0.85f, 0.90f, 1.f);
    const LLColor4 C_BUTTON(0.16f, 0.36f, 0.62f, 1.f);
    const LLColor4 C_BUTTON_HI(0.22f, 0.46f, 0.76f, 1.f);

    const LLColor4& levelColour(WolfBorder::ELevel l)
    {
        return l == WolfBorder::L_WARNING ? C_RED : l == WolfBorder::L_CAUTION ? C_AMBER : C_TEAL;
    }
    const char* levelWord(WolfBorder::ELevel l)
    {
        return l == WolfBorder::L_WARNING ? "WARNING" : l == WolfBorder::L_CAUTION ? "CAUTION" : "INFO";
    }
}

WolfBorderBanner::WolfBorderBanner(const Params& p)
    : LLUICtrl(p)
{
    WolfBorder::getInstance();   // the warnings run from here on (gIdleCallbacks), drawn or not
}

void WolfBorderBanner::draw()
{
    mPanel = LLRect();
    mButton = LLRect();
    if (!WolfBorder::instanceExists())
    {
        return;
    }
    const WolfBorder::Banner& b = WolfBorder::instance().banner();
    if (b.mLevel == WolfBorder::L_NONE)
    {
        return;
    }
    const LLFontGL* big = LLFontGL::getFontSansSerifHuge();
    const LLFontGL* med = LLFontGL::getFontSansSerifBig();
    const S32 view_w = getRect().getWidth(), view_h = getRect().getHeight();
    const S32 pad = 16, bar = 10, gap = 8;
    const S32 width = llclamp((S32)(view_w * 0.62f), 360, 760);
    const S32 line_big = (S32)big->getLineHeight(), line_med = (S32)med->getLineHeight();
    const bool button = b.mOfferTeleport;
    const S32 button_h = 44;   // a 44 px target (Fitts / WCAG 2.5.5)
    S32 height = pad + line_big + gap + line_med + (b.mOutcome.empty() ? 0 : gap + line_med) + pad + (button ? button_h + gap : 0);
    const S32 left = (view_w - width) / 2;
    const S32 top = view_h - 72;   // under the top toolbar and the navigation bar
    mPanel.setLeftTopAndSize(left, top, width, height);

    gl_rect_2d(mPanel, C_PANEL, true);
    LLRect accent(left, top, left + bar, top - height);
    gl_rect_2d(accent, levelColour(b.mLevel), true);

    const S32 tx = left + bar + pad;
    const S32 text_w = width - bar - 2 * pad;
    S32 y = top - pad;
    // the level as a word, coloured, then the title in white: never colour alone (WCAG 1.4.1)
    F32 word_right = (F32)tx;
    big->renderUTF8(levelWord(b.mLevel), 0, (F32)tx, (F32)y, levelColour(b.mLevel), LLFontGL::LEFT, LLFontGL::TOP,
                    LLFontGL::BOLD, LLFontGL::NO_SHADOW, S32_MAX, text_w, &word_right);
    // the distance's text is made again only when what it shows changes (a metre, or a tenth of a km)
    const S32 units = distanceUnits(b.mDist);
    if (units != mDistUnits)
    {
        mDistUnits = units;
        mDistText = distanceText(units);
    }
    const std::string& dist = mDistText;
    S32 dist_w = 0;
    if (!dist.empty())
    {
        dist_w = big->getWidth(dist) + pad;
        big->renderUTF8(dist, 0, (F32)(left + width - pad), (F32)y, C_TEXT, LLFontGL::RIGHT, LLFontGL::TOP,
                        LLFontGL::BOLD, LLFontGL::NO_SHADOW);
    }
    const F32 title_x = word_right + 12.f;
    big->renderUTF8(b.mTitle, 0, title_x, (F32)y, C_TEXT, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::BOLD, LLFontGL::NO_SHADOW,
                    S32_MAX, std::max(0, (S32)(left + width - pad - dist_w - title_x)), nullptr, true);
    y -= line_big + gap;
    med->renderUTF8(b.mReason, 0, (F32)tx, (F32)y, C_SUB, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                    S32_MAX, text_w, nullptr, true);
    if (!b.mOutcome.empty())
    {
        y -= line_med + gap;
        med->renderUTF8(b.mOutcome, 0, (F32)tx, (F32)y, C_TEXT, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::BOLD, LLFontGL::NO_SHADOW,
                        S32_MAX, text_w, nullptr, true);
    }
    if (button)
    {
        y -= line_med + gap + pad / 2;
        const S32 bw = std::min(text_w, med->getWidth(b.mTeleportLabel) + 2 * pad);
        mButton.setLeftTopAndSize(tx, y, bw, button_h);
        S32 mx = 0, my = 0;
        LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
        gl_rect_2d(mButton, mButton.pointInRect(mx, my) ? C_BUTTON_HI : C_BUTTON, true);
        med->renderUTF8(b.mTeleportLabel, 0, (F32)mButton.getCenterX(), (F32)mButton.getCenterY(), C_TEXT,
                        LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::BOLD, LLFontGL::NO_SHADOW);
    }
}

bool WolfBorderBanner::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mButton.notEmpty() && mButton.pointInRect(x, y) && WolfBorder::instanceExists())
    {
        WolfBorder::instance().teleportAcross();
        return true;
    }
    return false;   // the world under the panel keeps its clicks
}

bool WolfBorderBanner::handleHover(S32 x, S32 y, MASK mask)
{
    if (mButton.notEmpty() && mButton.pointInRect(x, y))
    {
        getWindow()->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return false;
}

bool WolfBorderBanner::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mButton.notEmpty() && mButton.pointInRect(x, y))
    {
        LLToolTipMgr::instance().show("An ordinary teleport: the region you land in checks it like any other.");
        return true;
    }
    return false;
}
