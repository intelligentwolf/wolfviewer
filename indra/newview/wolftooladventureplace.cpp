/**
 * @file wolftooladventureplace.cpp
 * @brief WolfViewer: the Adventure Studio's in-world place tool. See wolftooladventureplace.h.
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

#include "wolftooladventureplace.h"

#include <cmath>

#include <boost/json.hpp>

#include "llagent.h"
#include "llfontgl.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llsurface.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llviewercamera.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "wolffloateradventurestudio.h"
#include "wolfgamehud.h"

namespace
{
    // Source: STUDIO_AND_CONSOLE_V1.md §6 2.1.1 "arrow keys nudge a selected spot by 0.5 m (Shift 0.1 m)";
    // §2.4 Spot "the [ / ] keys in 15 degree steps".
    const F32 NUDGE = 0.5f, NUDGE_FINE = 0.1f, TURN_STEP = 15.f;
    // A burst of nudges is written once, this long after the last key (one draft change, one autosave).
    const F64 NUDGE_WRITE_SEC = 0.25;
    // A click this close (screen pixels) to the first corner closes a polygon (§2.4 "a click on the first point").
    const F32 CLOSE_PX = 12.f;
    // A press-drag shorter than this (metres) is a click: the circle waits for its edge click.
    const F32 MIN_RADIUS = 0.5f;
    // A second click this close (metres) to the last point is the same point (a double click).
    const F32 SAME_POINT = 0.05f;
    // Source: adventure_pack_v1.schema.json areas circle radius "maximum": 512; polygon maxItems 64; paths points maxItems 64.
    const F32 MAX_RADIUS = 512.f;
    const size_t MAX_POINTS = 64;
    // The overlay: places further than this from the camera are not drawn, and a path leg has at most this many dashes.
    const F32 CULL_M = 256.f;
    const S32 MAX_DASHES = 64;
    /** Two decimals, rounded in double so the JSON carries 12.34, not 12.340000152587890625. */
    double r2d(F32 v) { return std::round((double)v * 100.0) / 100.0; }
    F32 wrap360(F32 a) { a = fmodf(a, 360.f); return a < 0.f ? a + 360.f : a; }
    std::string regionId()
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        return rgn ? rgn->getRegionID().asString() : std::string();
    }
    bool finite3(const LLVector3& v) { return std::isfinite(v.mV[VX]) && std::isfinite(v.mV[VY]) && std::isfinite(v.mV[VZ]); }
}

WolfToolAdventurePlace::WolfToolAdventurePlace() : LLTool(std::string("WolfAdventurePlace")) {}

void WolfToolAdventurePlace::attach(LLHandle<WolfAdventureStudioView> view) { mView = view; }

void WolfToolAdventurePlace::detach()
{
    cancel();
    mView = LLHandle<WolfAdventureStudioView>();
    mCacheValid = false;
}

bool WolfToolAdventurePlace::shown() const
{
    return !mView.isDead() && mView.get()->placesPageShown();
}

void WolfToolAdventurePlace::say(const std::string& what) const
{
    if (!mView.isDead()) mView.get()->placeSay(what);
}

void WolfToolAdventurePlace::begin(const std::string& kind, const std::string& id, EMode mode, const std::string& what)
{
    cancel();
    if (mView.isDead()) return;
    const LLSD& place = mView.get()->placeOf(kind, id);
    if (!place.isMap()) { say("That place is no longer in the draft."); return; }
    mKind = kind;
    mId = id;
    mMode = mode;
    mWhat = what;
    mRegionAtBegin = regionId();
    mTouched = false;
    // the facing it has now, so [ / ] turn from there
    mFacing = mode == M_SPOT ? (F32)place["facing"].asReal() : 0.f;
    if (!std::isfinite(mFacing)) mFacing = 0.f;
    // held only while placing (see the class comment)
    LLToolMgr::getInstance()->setTransientTool(this);
}

void WolfToolAdventurePlace::cancel()
{
    flushNudge();
    if (hasMouseCapture()) setMouseCapture(false);
    mMode = M_NONE;
    mPts.clear();
    mDragging = false;
    mHaveCursor = false;
    LLToolMgr* tm = LLToolMgr::getInstance();
    if (tm->getCurrentTool() == this) tm->clearTransientTool();
}

std::string WolfToolAdventurePlace::hint() const
{
    switch (mMode)
    {
    case M_SPOT: return "Click the ground, a floor or an object for " + mWhat + "; drag to turn it. [ and ] turn it, the arrow keys nudge it, Enter or Escape when done.";
    case M_CIRCLE: return "Click the centre of " + mWhat + ", then its edge (or drag outward). Escape cancels.";
    case M_POLYGON: return "Click each corner of " + mWhat + ". Enter or a click on the first corner closes it; Backspace takes the last corner back; Escape cancels.";
    case M_PATH: return "Click each point of " + mWhat + ". Enter finishes; Backspace takes the last point back; Escape cancels.";
    default: return std::string();
    }
}

void WolfToolAdventurePlace::tick()
{
    if (mPending && LLFrameTimer::getTotalSeconds() - mPendAt >= NUDGE_WRITE_SEC) flushNudge();
    // places are per region: leaving it ends the shape (STUDIO §2.4 "Places on other regions ... Teleport there")
    if (mMode != M_NONE && regionId() != mRegionAtBegin)
    {
        mPending = false;
        cancel();
        say("You left the region the place is on - placing stopped, nothing more was changed.");
    }
}

// The surface under the pointer. Source: llviewerwindow.h:424 pickImmediate(x, y, pick_transparent, ...) ->
// LLPickInfo::mPosGlobal (land or an object's surface, llviewerwindow.h:117); mousePointOnLandGlobal for
// the land alone; llviewerregion.h getPosRegionFromGlobal. Off this region nothing is placed.
bool WolfToolAdventurePlace::surface(S32 x, S32 y, LLVector3& out, bool precise) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d g;
    bool have = false;
    if (precise)
    {
        const LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false);
        if (pick.mPickType == LLPickInfo::PICK_LAND || pick.mPickType == LLPickInfo::PICK_OBJECT) { g = pick.mPosGlobal; have = true; }
    }
    if (!have && !gViewerWindow->mousePointOnLandGlobal(x, y, &g)) return false;
    const LLVector3 p = rgn->getPosRegionFromGlobal(g);
    const F32 w = rgn->getWidth();
    if (!finite3(p) || p.mV[VX] < 0.f || p.mV[VY] < 0.f || p.mV[VX] >= w || p.mV[VY] >= w) return false;
    out = p;
    return true;
}

// Source: wolfcave.cpp WolfToolCave::planeHit (llviewerwindow.h mousePointOnPlaneGlobal).
bool WolfToolAdventurePlace::plane(S32 x, S32 y, F32 z, LLVector3& out) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d g;
    if (!gViewerWindow->mousePointOnPlaneGlobal(g, x, y, rgn->getPosGlobalFromRegion(LLVector3(0.f, 0.f, z)), LLVector3::z_axis)) return false;
    out = rgn->getPosRegionFromGlobal(g);
    return finite3(out);
}

bool WolfToolAdventurePlace::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    // As WolfToolCave: with no shape begun, every click is LLToolPie's.
    if (mMode == M_NONE || (clicktype != CLICK_LEFT && clicktype != CLICK_DOUBLELEFT))
    {
        return LLToolPie::getInstance()->handleAnyMouseClick(x, y, mask, clicktype, down);
    }
    return LLTool::handleAnyMouseClick(x, y, mask, clicktype, down);
}

bool WolfToolAdventurePlace::handleScrollWheel(S32 x, S32 y, S32 clicks) { return LLToolPie::getInstance()->handleScrollWheel(x, y, clicks); }

bool WolfToolAdventurePlace::handleToolTip(S32 x, S32 y, MASK mask)
{
    return mMode == M_NONE ? LLToolPie::getInstance()->handleToolTip(x, y, mask) : false;
}

bool WolfToolAdventurePlace::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mMode == M_NONE) return false;
    LLVector3 p;
    if (!surface(x, y, p, true))
    {
        say("Click on this region's ground, a floor or an object.");
        return true;
    }
    auto add_point = [&]()
    {
        // a double click lands twice on the same point: once is enough
        if (!mPts.empty() && dist_vec(mPts.back(), p) < SAME_POINT) return;
        if (mPts.size() < MAX_POINTS) mPts.push_back(p);
        else say("That is the most points a shape may have (64): press Enter to finish.");
    };
    switch (mMode)
    {
    case M_SPOT:
        flushNudge();
        mPts.assign(1, p);
        mDragging = true;
        setMouseCapture(true);
        break;
    case M_CIRCLE:
        if (mPts.empty())
        {
            mPts.assign(1, p);
            mDragging = true;
            setMouseCapture(true);
        }
        else
        {
            // the edge click
            mPts.resize(1);
            mPts.push_back(p);
            finish();
        }
        break;
    case M_POLYGON:
    {
        if (mPts.size() >= 3)
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            LLCoordGL sp;
            if (rgn && LLViewerCamera::getInstance()->projectPosAgentToScreen(rgn->getPosAgentFromRegion(mPts[0]), sp, true))
            {
                const F32 dx = (F32)(sp.mX - x), dy = (F32)(sp.mY - y);
                if (sqrtf(dx * dx + dy * dy) <= CLOSE_PX) { finish(); return true; }
            }
        }
        add_point();
        break;
    }
    case M_PATH:
        add_point();
        break;
    default: break;
    }
    return true;
}

bool WolfToolAdventurePlace::handleHover(S32 x, S32 y, MASK mask)
{
    // The Studio closed, was minimised or left the Places page without saying so (a minimised floater
    // draws nothing, so its own tick cannot): hand the pointer back at once.
    if (!shown())
    {
        cancel();
        return LLToolPie::getInstance()->handleHover(x, y, mask);
    }
    if (mMode == M_NONE) return LLToolPie::getInstance()->handleHover(x, y, mask);
    LLVector3 p;
    mHaveCursor = false;
    if (mDragging && !mPts.empty())
    {
        if (plane(x, y, mPts[0].mV[VZ], p)) { mCursor = p; mHaveCursor = true; }
        if (mMode == M_SPOT && mHaveCursor)
        {
            const F32 dx = mCursor.mV[VX] - mPts[0].mV[VX], dy = mCursor.mV[VY] - mPts[0].mV[VY];
            // facing: 0 = east, 90 = north (adventure_pack_v1.schema.json spots facing, STORY v1 facingDeg)
            if (dx * dx + dy * dy > 0.04f) mFacing = wrap360(std::round(atan2f(dy, dx) * RAD_TO_DEG));
        }
    }
    else if (surface(x, y, p, false)) { mCursor = p; mHaveCursor = true; }
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool WolfToolAdventurePlace::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mMode == M_NONE || !mDragging) return false;
    mDragging = false;
    if (hasMouseCapture()) setMouseCapture(false);
    if (mMode == M_SPOT) { finish(); return true; }
    if (mMode == M_CIRCLE && mHaveCursor && !mPts.empty())
    {
        const F32 dx = mCursor.mV[VX] - mPts[0].mV[VX], dy = mCursor.mV[VY] - mPts[0].mV[VY];
        if (sqrtf(dx * dx + dy * dy) >= MIN_RADIUS)
        {
            mPts.resize(1);
            mPts.push_back(mCursor);
            finish();
        }
        // else: a click - the next click is the edge
    }
    return true;
}

void WolfToolAdventurePlace::onMouseCaptureLost() { mDragging = false; }

bool WolfToolAdventurePlace::handleKey(KEY key, MASK mask)
{
    if (mMode == M_NONE) return LLTool::handleKey(key, mask);
    if (key == KEY_ESCAPE || (key == KEY_RETURN && mMode == M_SPOT))
    {
        // a spot is written as it is placed and nudged, so Escape and Enter both end it as it stands
        const bool done = mMode == M_SPOT && (mTouched || mPending);
        cancel();
        say(done ? "Done." : "Cancelled - nothing was changed.");
        return true;
    }
    if (key == KEY_RETURN)
    {
        finish();
        return true;
    }
    if (key == KEY_BACKSPACE && (mMode == M_POLYGON || mMode == M_PATH))
    {
        if (!mPts.empty()) mPts.pop_back();
        return true;
    }
    if (mMode == M_SPOT)
    {
        const F32 step = (mask & MASK_SHIFT) ? NUDGE_FINE : NUDGE;
        switch (key)
        {
        case KEY_LEFT: nudgeSpot(-step, 0.f, 0.f); return true;
        case KEY_RIGHT: nudgeSpot(step, 0.f, 0.f); return true;
        case KEY_UP: nudgeSpot(0.f, step, 0.f); return true;
        case KEY_DOWN: nudgeSpot(0.f, -step, 0.f); return true;
        case '[': nudgeSpot(0.f, 0.f, -TURN_STEP); return true;
        case ']': nudgeSpot(0.f, 0.f, TURN_STEP); return true;
        default: break;
        }
    }
    return LLTool::handleKey(key, mask);
}

void WolfToolAdventurePlace::nudgeSpot(F32 dx, F32 dy, F32 dturn)
{
    if (mView.isDead()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    LLVector3 pos;
    F32 facing = 0.f;
    if (mPending) { pos = mPendPos; facing = mPendFacing; }
    else
    {
        // the spot as the draft holds it now
        const LLSD& spot = mView.get()->placeOf(mKind, mId);
        const LLSD& sp = spot["pos"];
        if (!sp.isArray() || sp.size() != 3) { say("Place the spot with a click first, then nudge it."); return; }
        pos.setVec((F32)sp[0].asReal(), (F32)sp[1].asReal(), (F32)sp[2].asReal());
        facing = (F32)spot["facing"].asReal();
        if (pos.mV[VX] < 0.f || pos.mV[VY] < 0.f || !finite3(pos)) { say("Place the spot with a click first, then nudge it."); return; }
        if (!std::isfinite(facing)) facing = 0.f;
    }
    // within the region (STUDIO §2.4: a place belongs to one region; x and y are 0 .. width)
    const F32 w = rgn->getWidth();
    LLVector3 np = pos;
    np.mV[VX] = llclamp(pos.mV[VX] + dx, 0.f, w - 0.01f);
    np.mV[VY] = llclamp(pos.mV[VY] + dy, 0.f, w - 0.01f);
    const F32 nf = wrap360(facing + dturn);
    if (np == pos && nf == facing)
    {
        say("That is the edge of the region.");
        return;
    }
    mPending = true;
    mPendPos = np;
    mPendFacing = nf;
    mPendTurned = mPendTurned || dturn != 0.f;
    mPendAt = LLFrameTimer::getTotalSeconds();
    mFacing = nf;
}

void WolfToolAdventurePlace::flushNudge()
{
    if (!mPending) return;
    mPending = false;
    if (mView.isDead()) return;
    boost::json::object o;
    o["pos"] = boost::json::array{ r2d(mPendPos.mV[VX]), r2d(mPendPos.mV[VY]), r2d(mPendPos.mV[VZ]) };
    o["facing"] = r2d(mPendFacing);
    const bool turned = mPendTurned;
    mPendTurned = false;
    mTouched = true;
    mView.get()->placed(mKind, mId, o, turned ? llformat("Facing %.0f degrees.", mPendFacing) : std::string("Moved."));
}

void WolfToolAdventurePlace::handleDeselect()
{
    if (mMode != M_NONE) cancel();
}

void WolfToolAdventurePlace::finish()
{
    if (mView.isDead()) { cancel(); return; }
    if (regionId() != mRegionAtBegin)
    {
        cancel();
        say("You left the region the place is on - nothing was changed.");
        return;
    }
    boost::json::object o;
    std::string message;
    switch (mMode)
    {
    case M_SPOT:
        if (mPts.empty()) return;
        o["pos"] = boost::json::array{ r2d(mPts[0].mV[VX]), r2d(mPts[0].mV[VY]), r2d(mPts[0].mV[VZ]) };
        o["facing"] = r2d(mFacing);
        message = llformat("Placed %s, facing %.0f degrees. Drag again, or [ ] and the arrows, to adjust; Enter when done.", mWhat.c_str(), mFacing);
        break;
    case M_CIRCLE:
    {
        if (mPts.size() < 2) return;
        const F32 dx = mPts[1].mV[VX] - mPts[0].mV[VX], dy = mPts[1].mV[VY] - mPts[0].mV[VY];
        const F32 radius = llclamp(sqrtf(dx * dx + dy * dy), 0.1f, MAX_RADIUS);
        boost::json::object c;
        c["centre"] = boost::json::array{ r2d(mPts[0].mV[VX]), r2d(mPts[0].mV[VY]) };
        c["radius"] = r2d(radius);
        boost::json::object shape;
        shape["circle"] = c;
        o["shape"] = shape;
        message = llformat("Drew %s: a circle of radius %.1f m.", mWhat.c_str(), radius);
        break;
    }
    case M_POLYGON:
    {
        if (mPts.size() < 3)
        {
            say("An area needs at least 3 corners.");
            return;
        }
        boost::json::array pts;
        for (const LLVector3& p : mPts) pts.push_back(boost::json::array{ r2d(p.mV[VX]), r2d(p.mV[VY]) });
        boost::json::object shape;
        shape["polygon"] = pts;
        o["shape"] = shape;
        message = llformat("Drew %s with %d corners.", mWhat.c_str(), (S32)mPts.size());
        break;
    }
    case M_PATH:
    {
        if (mPts.size() < 2)
        {
            say("A path needs at least 2 points.");
            return;
        }
        boost::json::array pts;
        for (const LLVector3& p : mPts) pts.push_back(boost::json::array{ r2d(p.mV[VX]), r2d(p.mV[VY]), r2d(p.mV[VZ]) });
        o["points"] = pts;
        message = llformat("Drew %s with %d points.", mWhat.c_str(), (S32)mPts.size());
        break;
    }
    default: return;
    }
    o["region"] = mRegionAtBegin;
    const std::string kind = mKind, id = mId;
    WolfAdventureStudioView* view = mView.get();
    // a spot stays under the tool for the keys (§6 2.1.1) until Enter or Escape
    mTouched = true;
    if (mMode == M_SPOT) mPts.clear();
    else cancel();
    view->placed(kind, id, o, message);
}

// ---- the overlay ----

void WolfToolAdventurePlace::renderIdle()
{
    if (!instanceExists()) return;
    WolfToolAdventurePlace* t = getInstance();
    if (LLToolMgr::getInstance()->getCurrentTool() == t) return;   // drawn as the current tool
    t->render();
}

void WolfToolAdventurePlace::drawIdle()
{
    if (!instanceExists()) return;
    WolfToolAdventurePlace* t = getInstance();
    if (LLToolMgr::getInstance()->getCurrentTool() == t) return;
    t->draw();
}

// [WOLF ADVENTURE P3 2026-10-16] The places of the draft on this region as line segments (agent metres)
// grouped per place, and their labels. Source for the shapes: wolfcave.cpp WolfToolCave::render; llsurface.h
// resolveHeightRegion for the ground under an area or a spot with z 0.
void WolfToolAdventurePlace::rebuild()
{
    mGroups.clear();
    mLabels.clear();
    mCacheValid = false;
    if (mView.isDead()) return;
    WolfAdventureStudioView* view = mView.get();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    mCacheGen = view->draftGen();
    mCacheRegion = rgn->getRegionID();
    // <WolfViewer> the cached outlines are agent-space (getPosAgentFromRegion), so key them on the
    // agent origin: a huge-region rebase (LLAgent::rebaseOrigin) moves it without changing the region.
    mCacheOrigin = gAgent.getAgentOriginGlobal();
    mCacheValid = true;
    const std::string here = mCacheRegion.asString();
    const LLSurface& land = rgn->getLand();
    const F32 w = rgn->getWidth();
    auto ground = [&](F32 x, F32 y) { return land.resolveHeightRegion(llclamp(x, 0.f, w - 0.01f), llclamp(y, 0.f, w - 0.01f)); };
    auto P = [&](F32 x, F32 y, F32 z) { return rgn->getPosAgentFromRegion(LLVector3(x, y, z)); };
    auto ok = [&](F32 x, F32 y) { return std::isfinite(x) && std::isfinite(y) && x >= 0.f && y >= 0.f && x < w && y < w; };
    auto name = [](const LLSD& v) { return v["label"].asString().empty() ? v["id"].asString() : v["label"].asString(); };
    const LLSD& places = view->draftPack()["places"];
    for (const LLSD& s : llsd::inArray(places["spots"]))
    {
        const LLSD& pos = s["pos"];
        if (s["region"].asString() != here || !pos.isArray() || pos.size() != 3) continue;
        const F32 x = (F32)pos[0].asReal(), y = (F32)pos[1].asReal();
        if (!ok(x, y)) continue;   // unplaced (-1, -1) or not on this region
        F32 z = (F32)pos[2].asReal();
        if (z == 0.f || !std::isfinite(z)) z = ground(x, y);
        Group g;
        g.mColour = 0;
        g.mCentre = P(x, y, z);
        auto seg = [&g](const LLVector3& a, const LLVector3& b) { g.mSegs.push_back(a); g.mSegs.push_back(b); };
        // a pin: a 2 m post with a diamond on top, and the facing as a 1.5 m arrow at the foot
        seg(P(x, y, z), P(x, y, z + 2.f));
        const F32 d = 0.25f;
        seg(P(x - d, y, z + 2.f), P(x, y, z + 2.f + d)); seg(P(x, y, z + 2.f + d), P(x + d, y, z + 2.f));
        seg(P(x + d, y, z + 2.f), P(x, y, z + 2.f - d)); seg(P(x, y, z + 2.f - d), P(x - d, y, z + 2.f));
        const F32 a = (F32)s["facing"].asReal() * DEG_TO_RAD;
        const F32 fx = x + cosf(a) * 1.5f, fy = y + sinf(a) * 1.5f;
        seg(P(x, y, z + 0.1f), P(fx, fy, z + 0.1f));
        seg(P(fx, fy, z + 0.1f), P(fx - cosf(a - 0.5f) * 0.4f, fy - sinf(a - 0.5f) * 0.4f, z + 0.1f));
        seg(P(fx, fy, z + 0.1f), P(fx - cosf(a + 0.5f) * 0.4f, fy - sinf(a + 0.5f) * 0.4f, z + 0.1f));
        mGroups.push_back(std::move(g));
        mLabels.push_back({ P(x, y, z + 2.6f), "Spot: " + name(s) });
    }
    for (const LLSD& ar : llsd::inArray(places["areas"]))
    {
        if (ar["region"].asString() != here) continue;
        Group g;
        g.mColour = 1;
        auto seg = [&g](const LLVector3& a, const LLVector3& b) { g.mSegs.push_back(a); g.mSegs.push_back(b); };
        const LLSD& c = ar["shape"]["circle"];
        F32 lx = -1.f, ly = -1.f;
        if (c.isMap() && c["centre"].isArray())
        {
            const F32 cx = (F32)c["centre"][0].asReal(), cy = (F32)c["centre"][1].asReal(), rr = (F32)c["radius"].asReal();
            if (!ok(cx, cy) || !(rr > 0.f) || !std::isfinite(rr)) continue;
            const S32 n = 48;
            for (S32 i = 0; i < n; ++i)
            {
                const F32 a0 = F_TWO_PI * i / n, a1 = F_TWO_PI * (i + 1) / n;
                const F32 x0 = cx + cosf(a0) * rr, y0 = cy + sinf(a0) * rr, x1 = cx + cosf(a1) * rr, y1 = cy + sinf(a1) * rr;
                seg(P(x0, y0, ground(x0, y0) + 0.3f), P(x1, y1, ground(x1, y1) + 0.3f));
            }
            lx = cx; ly = cy;
        }
        else
        {
            const LLSD& poly = ar["shape"]["polygon"];
            bool good = poly.isArray() && poly.size() >= 3;
            for (size_t i = 0; good && i < (size_t)poly.size(); ++i)
            {
                const LLSD& p0 = poly[(LLSD::Integer)i];
                const LLSD& p1 = poly[(LLSD::Integer)((i + 1) % poly.size())];
                const F32 x0 = (F32)p0[0].asReal(), y0 = (F32)p0[1].asReal(), x1 = (F32)p1[0].asReal(), y1 = (F32)p1[1].asReal();
                if (!ok(x0, y0) || !ok(x1, y1)) { good = false; break; }
                seg(P(x0, y0, ground(x0, y0) + 0.3f), P(x1, y1, ground(x1, y1) + 0.3f));
                if (i == 0) { lx = x0; ly = y0; }
            }
            if (!good) continue;
        }
        g.mCentre = P(lx, ly, ground(lx, ly));
        mGroups.push_back(std::move(g));
        mLabels.push_back({ P(lx, ly, ground(lx, ly) + 1.f), "Area: " + name(ar) });
    }
    for (const LLSD& pa : llsd::inArray(places["paths"]))
    {
        const LLSD& pts = pa["points"];
        if (pa["region"].asString() != here || !pts.isArray() || pts.size() < 2) continue;
        Group g;
        g.mColour = 2;
        auto seg = [&g](const LLVector3& a, const LLVector3& b) { g.mSegs.push_back(a); g.mSegs.push_back(b); };
        bool good = true;
        for (size_t i = 0; i + 1 < (size_t)pts.size(); ++i)
        {
            const LLSD& p0 = pts[(LLSD::Integer)i];
            const LLSD& p1 = pts[(LLSD::Integer)(i + 1)];
            const F32 x0 = (F32)p0[0].asReal(), y0 = (F32)p0[1].asReal(), x1 = (F32)p1[0].asReal(), y1 = (F32)p1[1].asReal();
            if (!ok(x0, y0) || !ok(x1, y1)) { good = false; break; }
            F32 z0 = (F32)p0[2].asReal(), z1 = (F32)p1[2].asReal();
            if (z0 == 0.f || !std::isfinite(z0)) z0 = ground(x0, y0);
            if (z1 == 0.f || !std::isfinite(z1)) z1 = ground(x1, y1);
            if (i == 0)
            {
                g.mCentre = P(x0, y0, z0);
                mLabels.push_back({ P(x0, y0, z0 + 1.2f), "Path: " + name(pa) });
            }
            // dotted: dashes about 1 m with gaps between (at most MAX_DASHES a leg), an arrow head at each leg's end
            const F32 dx = x1 - x0, dy = y1 - y0, dz = z1 - z0, len = sqrtf(dx * dx + dy * dy + dz * dz);
            const S32 dashes = llclamp((S32)(len / 2.f), 1, MAX_DASHES);
            for (S32 k = 0; k < dashes; ++k)
            {
                const F32 t0 = (2.f * k) / (2.f * dashes), t1 = (2.f * k + 1.f) / (2.f * dashes);
                seg(P(x0 + dx * t0, y0 + dy * t0, z0 + dz * t0 + 0.3f), P(x0 + dx * t1, y0 + dy * t1, z0 + dz * t1 + 0.3f));
            }
            const F32 a = atan2f(dy, dx);
            seg(P(x1, y1, z1 + 0.3f), P(x1 - cosf(a - 0.5f) * 0.8f, y1 - sinf(a - 0.5f) * 0.8f, z1 + 0.3f));
            seg(P(x1, y1, z1 + 0.3f), P(x1 - cosf(a + 0.5f) * 0.8f, y1 - sinf(a + 0.5f) * 0.8f, z1 + 0.3f));
        }
        if (good) mGroups.push_back(std::move(g));
    }
}

void WolfToolAdventurePlace::render()
{
    if (!shown()) return;
    WolfAdventureStudioView* view = mView.get();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    if (!mCacheValid || mCacheGen != view->draftGen() || mCacheRegion != rgn->getRegionID() || mCacheOrigin != gAgent.getAgentOriginGlobal()) rebuild();
    // the shape being drawn
    std::vector<LLVector3> live;
    auto seg = [&live](const LLVector3& a, const LLVector3& b) { live.push_back(a); live.push_back(b); };
    auto P = [&](F32 x, F32 y, F32 z) { return rgn->getPosAgentFromRegion(LLVector3(x, y, z)); };
    std::vector<LLVector3> pts = mPts;
    if (mHaveCursor && mMode != M_NONE && mMode != M_SPOT && !(mMode == M_CIRCLE && pts.size() >= 2)) pts.push_back(mCursor);
    if (mMode == M_CIRCLE && pts.size() >= 2)
    {
        const F32 cx = pts[0].mV[VX], cy = pts[0].mV[VY];
        const F32 rr = llmin(MAX_RADIUS, sqrtf((pts[1].mV[VX] - cx) * (pts[1].mV[VX] - cx) + (pts[1].mV[VY] - cy) * (pts[1].mV[VY] - cy)));
        for (S32 i = 0; i < 48; ++i)
        {
            const F32 a0 = F_TWO_PI * i / 48, a1 = F_TWO_PI * (i + 1) / 48;
            seg(P(cx + cosf(a0) * rr, cy + sinf(a0) * rr, pts[0].mV[VZ] + 0.4f), P(cx + cosf(a1) * rr, cy + sinf(a1) * rr, pts[0].mV[VZ] + 0.4f));
        }
    }
    else if (mMode == M_POLYGON || mMode == M_PATH)
    {
        for (size_t i = 0; i + 1 < pts.size(); ++i) seg(P(pts[i].mV[VX], pts[i].mV[VY], pts[i].mV[VZ] + 0.4f), P(pts[i + 1].mV[VX], pts[i + 1].mV[VY], pts[i + 1].mV[VZ] + 0.4f));
        for (const LLVector3& p : mPts) seg(P(p.mV[VX], p.mV[VY], p.mV[VZ]), P(p.mV[VX], p.mV[VY], p.mV[VZ] + 1.f));
    }
    else if (mMode == M_SPOT && (!mPts.empty() || mPending))
    {
        const LLVector3 p = mPts.empty() ? mPendPos : mPts[0];
        seg(P(p.mV[VX], p.mV[VY], p.mV[VZ]), P(p.mV[VX], p.mV[VY], p.mV[VZ] + 2.f));
        const F32 a = mFacing * DEG_TO_RAD;
        seg(P(p.mV[VX], p.mV[VY], p.mV[VZ] + 0.1f), P(p.mV[VX] + cosf(a) * 1.5f, p.mV[VY] + sinf(a) * 1.5f, p.mV[VZ] + 0.1f));
    }
    if (mGroups.empty() && live.empty()) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);
    // amber spots, ice areas, green paths; the shape being drawn in white (never colour alone: the 2D
    // labels name every place)
    static const F32 COLOURS[3][4] = { { 0.96f, 0.65f, 0.14f, 0.95f }, { 0.49f, 0.83f, 0.99f, 0.85f }, { 0.29f, 0.87f, 0.50f, 0.9f } };
    gGL.setLineWidth(2.f);
    for (const Group& g : mGroups)
    {
        if (g.mSegs.empty() || (dist_vec(g.mCentre, cam) > CULL_M && dist_vec(g.mSegs.front(), cam) > CULL_M)) continue;
        gGL.color4fv(COLOURS[g.mColour]);
        gGL.begin(LLRender::LINES);
        for (const LLVector3& p : g.mSegs) gGL.vertex3fv(p.mV);
        gGL.end();
    }
    if (!live.empty())
    {
        gGL.setLineWidth(3.f);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.begin(LLRender::LINES);
        for (const LLVector3& p : live) gGL.vertex3fv(p.mV);
        gGL.end();
    }
    gGL.setLineWidth(1.f);
}

// The labels (§2.4 "spots as pins with labels"): projected to the screen (llviewercamera.h:75
// projectPosAgentToScreen, scaled UI coordinates as wolfcave.cpp pickHandle uses them).
void WolfToolAdventurePlace::draw()
{
    if (!shown()) return;
    const LLFontGL* f = LLFontGL::getFontSansSerifSmall();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin();
    if (mCacheValid)
    {
        for (const Label& l : mLabels)
        {
            if (dist_vec(l.mPos, eye) > CULL_M) continue;
            LLCoordGL sp;
            if (!cam->projectPosAgentToScreen(l.mPos, sp, false)) continue;
            const F32 tw = (F32)f->getWidthF32(l.mText);
            WolfGameDraw::rect((F32)sp.mX - tw * 0.5f - 4.f, (F32)sp.mY - 2.f, (F32)sp.mX + tw * 0.5f + 4.f, (F32)sp.mY + (F32)f->getLineHeight() + 2.f,
                               LLColor4(0.f, 0.f, 0.f, 0.7f));
            f->renderUTF8(l.mText, 0, (F32)sp.mX, (F32)sp.mY, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
    }
    if (mMode != M_NONE)
    {
        // what the tool wants, at the top of the world view
        const std::string h = hint();
        const F32 tw = (F32)f->getWidthF32(h);
        const F32 cx = (F32)gViewerWindow->getWorldViewWidthScaled() * 0.5f, ty = (F32)gViewerWindow->getWorldViewHeightScaled() - 60.f;
        WolfGameDraw::rect(cx - tw * 0.5f - 8.f, ty - 4.f, cx + tw * 0.5f + 8.f, ty + (F32)f->getLineHeight() + 4.f, LLColor4(0.f, 0.f, 0.f, 0.75f));
        f->renderUTF8(h, 0, cx, ty, LLColor4::white, LLFontGL::HCENTER, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
    }
}
