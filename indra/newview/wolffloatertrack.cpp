/**
 * @file wolffloatertrack.cpp
 * @brief WolfViewer: Lay Railway Track. Design: wolffloatertrack.h; track-tool-2026-11-14/DESIGN.md.
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

#include "wolffloatertrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <boost/json.hpp>

#include "fscommon.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llbase64.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcoros.h"
#include "lldate.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llfloatertools.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llprogressbar.h"
#include "llradiogroup.h"
#include "llrender.h"
#include "llscrollcontainer.h"   // [T961 2026-11-21/] track_scroll
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "llsdjson.h"
#include "llselectmgr.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"   // pack_permissions_slam
#include "lltoolmgr.h"
#include "lluicolortable.h"
#include "llviewercamera.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvolume.h"
#include "llworld.h"
#include "message.h"
#include "wolfcave.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"
#include "wolfadaptivedrawdistance.h"   // [TRACK PICK 2026-11-18/] userDrawDistance

namespace
{
    // DESIGN.md 10: the WolfViewer endpoint (the Lifts / Building Generator host)
    const char* TRACK_URL = "https://wolfstorm.app/php/track_tool.php";
    constexpr F32 TIMEOUT_S = 30.f;            // CHOSEN: a plan answers in < 2 s (DESIGN.md 10); lay / remove are quick DB calls
    constexpr F64 PLAN_WAIT_S = 0.6;           // wolffloaterlifts.cpp:76 PLAN_WAIT_S (DESIGN.md 1.2)
    constexpr F32 STATUS_EVERY_S = 2.f;        // the brief: "poll status every 2 s ONLY while a job is queued/running"
    constexpr S32 LOOK_S = 60;                 // DESIGN.md 10: wait up to 60 s for the pieces
    constexpr F32 MATCH_M = 0.05f;             // DESIGN.md 10: roots within 0.05 m
    constexpr F32 NAME_WAIT_S = 20.f;          // MODELLED: a look matches by name this long, then also by place (matchPieces)
    // wolfcave.cpp:941 rezExact waits 20 s for an item in a folder the viewer knows; here the folder itself is new (the worker makes
    // "Railway <code> - <name>" under Objects through Robust: deliver.py:76-78), so two fetches in turn - Objects, then the new
    // folder - each re-asked at most every FOLDER_REASK_S: 30 s (CHOSEN) leaves room for both
    constexpr S32 FETCH_TRIES = 30;
    constexpr S32 FOLDER_REASK_S = 5;          // CHOSEN: a folder not seen yet is asked for again this often (wolffloaterlifts.cpp:996-1006 every 5 s)
    // DESIGN.md 10 "Removal by the server": the window polls the job's status every 2-3 s until done or failed
    constexpr F32 REMOVE_POLL_S = 2.5f;
    constexpr F64 REMOVE_JOB_MAX_S = 600.0;    // CHOSEN (review of ec46b58654): a removal job is waited for 10 minutes at most
    constexpr S32 REMOVE_POLL_FAILS = 5;       // CHOSEN: as statusCoro, five unanswered asks in a row and the window stops waiting
    // DESIGN.md 10: the confirmation for pieces the region deleted (permanently, not to the Trash; the item stays in Objects)
    const char* SERVER_GONE_WORDS = "Its pieces are taken away; your railway stays in your inventory (Objects) to lay again.";
    constexpr S32 GONE_WAIT_S = 30;            // CHOSEN: after a delete, the pieces are waited for this long to go
    // DESIGN.md 5.1 / 10: the ground box is the drawing's box plus 60 m all round, 4 m apart, 8 m when that is over 256 a side
    constexpr F64 GROUND_MARGIN_M = 60.0;
    constexpr S32 GROUND_MAX = 256;            // tracktool/plan.py:20 GRID_MAX
    constexpr U16 GROUND_UNKNOWN = 0xFFFF;     // DESIGN.md 10: 0xFFFF = not known
    constexpr size_t OTHERS_MAX = 2000;        // tracktool/plan.py:19 MAX_OTHERS
    // tracktool/plan.py:14-18 the planner's limits (a request over them is refused as bad)
    constexpr size_t MAX_REGIONS = 4, MAX_LINES = 12, MAX_CLICKS = 40, MAX_STATIONS = 8, MAX_SIGNALS = 40;
    constexpr size_t MAX_TURNTABLES = 4;       // DESIGN.md 10 REQ turntables: at most 4 (tracktool/plan.py turntables > 4: refused)
    constexpr F64 TT_PIT_EXTRA_M = 0.3;        // DESIGN.md 10 / tracktool/layout.py:47 TT_PIT_EXTRA: the pit ring at size / 2 + 0.3
    constexpr S32 TT_SPARE_M = 1;              // DESIGN.md 10: the longest train a deck turns is the deck less 1 m (Turntable.cs TT_SPARE_M)
    constexpr size_t STATION_NAME_MAX = 24;            // tracktool/plan.py:93 / layout.py:39 NAME_OK: 24 characters
    // DESIGN.md 4.1: a guide is a 0.5 x 0.5 x 0.2 box (build_loops.py:61); DESIGN.md 5.1: +-0.01
    const LLVector3 GUIDE_SIZE(0.5f, 0.5f, 0.2f);
    constexpr F32 GUIDE_TOL = 0.01f;
    constexpr F64 SAME_CLICK_M = 1.0;          // wolffloaterlifts.cpp:75: the second press of a double-click
    constexpr F64 LOOP_CLOSE_M = 4.0;          // CHOSEN: under the planner's 5 m between clicks (tracktool/geom.py:19 MIN_CLICK_GAP)
    constexpr F64 ON_LINE_M = 8.0;             // CHOSEN: a click this near a line is "on" it; it is put on the line itself
    constexpr F32 LINE_PICK_PX = 12.f;         // [15 Nov] WolfStorm floater_wolf_track_tool.js LINE_PICK_PX: a click this near the
                                               // plan's line on the screen (drawn at its rail level) is ON the line
    constexpr F64 JOIN_M = 6.0;                // tracktool/layout.py:108 nearest_line within=6.0: a branch's last click joins there
    constexpr F64 HALF_GAUGE_M = 1.435 * 0.5;  // the brief: two rails 1.435 m apart (standard gauge)
    constexpr F64 FORMATION_M = 0.75;          // tracktool/profile.py:15 FORMATION_DOWN: formation under the rail head
    constexpr F64 PLATFORM_EDGE_M = 1.6;       // tracktool/layout.py:35 PLATFORM_EDGE (1.4 + 0.2)
    constexpr F64 ISLAND_W_M = 6.0;            // tracktool/layout.py:36 ISLAND_W
    constexpr F64 PLATFORM_DEPTH_M = 3.0;      // CHOSEN: how deep a side platform's outline is drawn (the preview only)
    constexpr F32 TOP_VIEW_M = 120.f;          // wolffloaterlifts.cpp:85
    constexpr F32 TOP_VIEW_TILT = 0.35f;       // wolffloaterlifts.cpp:86
    constexpr F32 LABEL_FAR_M = 400.f;         // CHOSEN: names further from the camera than this are not written

    // the preview's colours (DESIGN.md 1.2: ground green, embankment brown, viaduct brick red, over water blue)
    const LLColor4 COL_GROUND(0.35f, 0.85f, 0.35f, 1.f);
    const LLColor4 COL_BANK(0.70f, 0.48f, 0.25f, 1.f);
    const LLColor4 COL_VIADUCT(0.80f, 0.28f, 0.20f, 1.f);
    const LLColor4 COL_WATER(0.30f, 0.60f, 1.f, 1.f);
    const LLColor4 COL_UNKNOWN(1.f, 1.f, 1.f, 0.8f);
    const LLColor4 COL_STATION(1.f, 0.85f, 0.35f, 1.f);
    const LLColor4 COL_POINTS(0.95f, 0.55f, 1.f, 1.f);
    const LLColor4 COL_SIGNAL(1.f, 0.35f, 0.3f, 1.f);
    const LLColor4 COL_PROBLEM(1.f, 0.2f, 0.15f, 1.f);
    const LLColor4 COL_SHOWN(0.45f, 0.9f, 1.f, 0.9f);
    const LLColor4 COL_DRAWN(1.f, 1.f, 1.f, 0.85f);
    const LLColor4 COL_TURNTABLE(1.f, 0.65f, 0.25f, 1.f);
    // [TRACK TOOL TUNNELS 2026-11-15] DESIGN.md 3.4 / 10: a stretch through the hill (PLAN structure "tunnel"), and a tunnel
    // being marked
    const LLColor4 COL_TUNNEL(0.55f, 0.55f, 0.65f, 1.f);
    const LLColor4 COL_TUNNEL_MARK(0.82f, 0.63f, 1.f, 1.f);
    constexpr size_t MAX_TUNNELS = 8;          // tracktool/tunnels.py:63 MAX_TUNNELS (plan.py parse refuses more)
    constexpr F64 TUNNEL_MIN_M = 20.0;         // tracktool/tunnels.py:56 MIN_LEN
    // [TRACK TOOL CUTTINGS 2026-11-17] DESIGN.md 3.6: the ground lowered along the line where the resident chooses it
    const LLColor4 COL_CUTTING(0.45f, 0.29f, 0.12f, 1.f);       // CHOSEN: earth brown, darker than COL_BANK so the two differ
    const LLColor4 COL_CUTTING_MARK(0.95f, 0.78f, 0.52f, 1.f);  // CHOSEN: a waiting cutting-only hill's posts, light earth
    constexpr size_t MAX_CUTTINGS = 8;         // DESIGN.md 3.6 contract: REQ cuttings "at most 8 ... like tunnels"
    constexpr F64 CUTTING_MIN_M = 20.0;        // DESIGN.md 3.6 "The levels": a cutting is fitted as a tunnel, at least MIN_LEN 20 m
    // DESIGN.md 3.6 "The cross-section" (formation f = rail - 0.75; the side starts 5.0 m out; 1:2 to CUT_ROCK_AT 4 m deep, else
    // 1:1.5; a wall face 5.2 m out where deeper than CUT_WALL_AT 8 m): where the preview draws the tops of a cutting's sides.
    // The preview only - the planner's own section also turns slopes into walls at land the resident may not dig, which the
    // window does not know, and reads each side's own ground (the window uses the ground under the line)
    constexpr F64 CUT_SIDE_FROM_M = 5.0, CUT_ROCK_AT_M = 4.0, CUT_WALL_AT_M = 8.0, CUT_WALL_FACE_M = 5.2;
    constexpr F64 CUT_SLOPE_SOIL = 2.0, CUT_SLOPE_ROCK = 1.5;    // metres out per metre up
    constexpr F64 CUT_DASH_M = 2.0;            // CHOSEN: the tops are drawn 2 m on, 2 m off
    constexpr F32 GROUND_POLL_S = 2.5f;        // the brief / DESIGN.md 10: a job's status every 2-3 s
    constexpr F64 GROUND_JOB_MAX_S = 1200.0;   // CHOSEN: a dig or fill is waited for 20 minutes at most (it may queue behind others)

    bool onWolf() { return WolfGrid::isOnWolfTerritories(); }

    /** The land height at grid (x, y) when this viewer has it, else false. Source: llworld.cpp:973-981 resolveLandHeightGlobal
     *  (the region holding the point; 0 when none) and llsurface.cpp:1144-1200 resolveHeightRegion, which reads the four grid
     *  corners round the point through getZ (llsurface.cpp:676-691): a corner in a patch never made, or made but never sent its
     *  heights (llsurfacepatch.h:94 getHasReceivedData), reads 0. So the point is known only when every patch those corners
     *  are in has had its data. */
    bool landAt(F64 gx, F64 gy, F32& z)
    {
        const LLVector3d g(gx, gy, 0.0);
        LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosGlobal(g);
        if (!r) return false;
        const LLSurface& s = r->getLand();
        const S32 n = s.getPatchesPerEdge();
        const F32 mpg = s.getMetersPerGrid();
        const F32 patch_m = mpg * (F32)s.getGridsPerPatchEdge();
        if (n <= 0 || patch_m <= 0.f) return false;
        const LLVector3 p = r->getPosRegionFromGlobal(g);
        for (S32 dj = 0; dj < 2; ++dj)
        {
            for (S32 di = 0; di < 2; ++di)
            {
                const S32 i = llclamp((S32)floorf((p.mV[VX] + (F32)di * mpg) / patch_m), 0, n - 1);
                const S32 j = llclamp((S32)floorf((p.mV[VY] + (F32)dj * mpg) / patch_m), 0, n - 1);
                const LLSurfacePatch* patch = s.findPatch(i, j);
                if (!patch || !patch->getHasReceivedData()) return false;
            }
        }
        z = s.resolveHeightRegion(p.mV[VX], p.mV[VY]);
        return std::isfinite(z);
    }

    /** The water height of the region holding grid (x, y), or false. */
    bool waterAt(F64 gx, F64 gy, F32& w)
    {
        LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosGlobal(LLVector3d(gx, gy, 0.0));
        if (!r) return false;
        w = r->getWaterHeight();
        return true;
    }

    /** A box prim: a square profile along a line (or flexible) path. Source: llpanelobject.cpp:727-746 (MI_BOX). */
    bool isBoxPrim(LLViewerObject* o)
    {
        if (!o || o->getPCode() != LL_PCODE_VOLUME || !o->getVolume()) return false;
        const LLVolumeParams& vp = o->getVolume()->getParams();
        const U8 path = vp.getPathParams().getCurveType();
        const U8 profile = vp.getProfileParams().getCurveType() & LL_PCODE_PROFILE_MASK;
        return (path == LL_PCODE_PATH_LINE || path == LL_PCODE_PATH_FLEXIBLE) && profile == LL_PCODE_PROFILE_SQUARE;
    }

    /** RezObject of `item` at `at` (region metres) in the agent's region. Source: wolffloaterlifts.cpp:336-360 rezAt (the same
     *  message, field for field: BypassRaycast 1, RayStart = RayEnd, RayEndIsIntersection true, RezSelected false, RemoveItem
     *  false, the agent's rez group; DESIGN.md 10 "What the window does with a JOB result"). */
    void rezAt(LLViewerInventoryItem* item, const LLVector3& at)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || !item) return;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
        msg->addVector3Fast(_PREHASH_RayStart, at);
        msg->addVector3Fast(_PREHASH_RayEnd, at);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, true);
        msg->addBOOLFast(_PREHASH_RezSelected, false);
        msg->addBOOLFast(_PREHASH_RemoveItem, false);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(rgn->getHost());
        FSCommon::sObjectAddMsg = 0;   // lltooldraganddrop.cpp:2030-2032: no build defaults for it
    }

    /** Delete `objs` to the Trash exactly as the viewer's Delete does: DeRezObject, Destination DRD_TRASH (llselectmgr.cpp
     *  :4447-4455 confirmDelete), the header of packDeRezHeader (:5835-5852: AgentData; AgentBlock GroupID, Destination,
     *  DestinationID = the Trash, a new TransactionID, PacketCount 1, PacketNumber 1), one ObjectLocalID block per root
     *  (:5882-5886 packObjectLocalID), a new message per region and when it is full or has MAX_OBJECTS_PER_PACKET roots
     *  (:126, :6095-6098 sendListToRegions). */
    void deRezToTrash(const std::vector<LLViewerObject*>& objs)
    {
        constexpr S32 MAX_OBJECTS_PER_PACKET = 254;   // llselectmgr.cpp:126
        const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        LLMessageSystem* msg = gMessageSystem;
        LLViewerRegion* open_region = nullptr;
        S32 in_packet = 0;
        auto start = [&](LLViewerRegion* r)
        {
            msg->newMessageFast(_PREHASH_DeRezObject);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_AgentBlock);
            msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
            msg->addU8Fast(_PREHASH_Destination, (U8)DRD_TRASH);
            msg->addUUIDFast(_PREHASH_DestinationID, trash);
            LLUUID tid;
            tid.generate();
            msg->addUUIDFast(_PREHASH_TransactionID, tid);
            const U8 PACKET = 1;
            msg->addU8Fast(_PREHASH_PacketCount, PACKET);
            msg->addU8Fast(_PREHASH_PacketNumber, PACKET);
            open_region = r;
            in_packet = 0;
        };
        for (LLViewerObject* o : objs)
        {
            if (!o || o->isDead() || !o->getRegion()) continue;
            if (open_region && (o->getRegion() != open_region || msg->isSendFull(nullptr) || in_packet >= MAX_OBJECTS_PER_PACKET))
            {
                msg->sendReliable(open_region->getHost());
                open_region = nullptr;
            }
            if (!open_region) start(o->getRegion());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
            ++in_packet;
        }
        if (open_region) msg->sendReliable(open_region->getHost());
    }

    // Source: wolffloaterbuildinggen.cpp:77-95 yardError - the service's own words first, else a plain line per status
    std::string serviceError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The railway service refused it without saying why.";
            case 400: return "The railway service could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "The grid did not accept your session - log out and back in, then try again.";
            case 404: return "The railway service is not open on this grid yet.";
            case 413: return "Your drawing is too large to send - lay it as two railways.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 500: return "The railway service hit an error - try again in a moment.";
            case 503: return "The railway service is not answering - try again in a moment.";
            default:
                return transport.empty() ? llformat("The railway service answered HTTP %d.", status)
                                         : "Could not reach the railway service: " + transport;
        }
    }

    // [x, y] - appended, never LLSD::with(index): ImplArray::insert resizes to index + 1 and then inserts, so with(0, x)
    // .with(1, y) is [x, y, null] (llcommon/llsd.cpp:680-691 ImplArray::insert), which the planner refuses (tracktool/plan.py _pt: len(v) != 2)
    LLSD xy(F64 x, F64 y)
    {
        LLSD a = LLSD::emptyArray();
        a.append(x);
        a.append(y);
        return a;
    }

    std::string plural(S32 n, const char* one, const char* many) { return llformat("%d %s", n, n == 1 ? one : many); }

    std::string stateWords(const std::string& s)
    {
        // DESIGN.md 10 LAYOUT state
        if (s == "building") return "Being made";
        if (s == "built") return "Made, not laid yet";
        if (s == "partly_laid") return "Partly laid";
        if (s == "laid") return "Laid";
        if (s == "partly_removed") return "Partly taken away";
        if (s == "removed") return "Taken away";
        if (s == "failed") return "Could not be made";
        return s;
    }

    std::string sideWords(const std::string& s)
    {
        if (s == "left") return "on the left";
        if (s == "right") return "on the right";
        if (s == "both") return "both sides";
        if (s == "island") return "island";
        return s;
    }

    std::string styleWords(const std::string& s)
    {
        if (s == "halt") return "country halt";
        if (s == "town") return "town station";
        if (s == "modern") return "modern";
        return s;
    }

    /** A station name the planner takes: tracktool/layout.py:39 NAME_OK ^[A-Za-z0-9][A-Za-z0-9 '\-]{0,23}$, never "station". */
    bool nameOk(const std::string& n)
    {
        if (n.empty() || n.size() > STATION_NAME_MAX || !isalnum((unsigned char)n[0])) return false;
        for (char c : n)
        {
            if (!(isalnum((unsigned char)c) || c == ' ' || c == '\'' || c == '-')) return false;
        }
        std::string lower = n;
        LLStringUtil::toLower(lower);
        return lower.find("station") == std::string::npos;
    }

    /** The points of an LLSD [[x, y(, z)], ...] list; false if any is not that. */
    bool readXY(const LLSD& v, F64& x, F64& y)
    {
        if (!v.isArray() || v.size() < 2) return false;
        x = v[0].asReal();
        y = v[1].asReal();
        return std::isfinite(x) && std::isfinite(y);
    }

    LLVector3 readV3(const LLSD& v)
    {
        if (!v.isArray() || v.size() < 3) return LLVector3::zero;
        return LLVector3((F32)v[0].asReal(), (F32)v[1].asReal(), (F32)v[2].asReal());
    }

    /** `v` when it is an array, else an empty one (a reply never makes the loops below change their LLSD). */
    const LLSD& arr(const LLSD& v)
    {
        static const LLSD empty = LLSD::emptyArray();
        return v.isArray() ? v : empty;
    }

    std::string dateWords(const LLSD& v)
    {
        if (v.isString()) return v.asString().substr(0, 10);
        if (v.isInteger() || v.isReal()) return LLDate(v.asReal()).asString().substr(0, 10);
        return std::string();
    }
}

// ---- WolfTrackTool ----------------------------------------------------------------------------------------------------------

void WolfTrackTool::onAlert(const std::string& message)
{
    // InventoryAccessModule.cs:1192-1195 "Can't rez object '{0}' at <...> on parcel '{4}' in region {5}."; sent as a plain
    // AlertMessage (LLClientView.cs:3120-3126 SendAlertMessage), which reaches process_alert_core
    static const std::string REFUSED("Can't rez object '");
    if (message.compare(0, REFUSED.size(), REFUSED) != 0) return;
    if (WolfFloaterTrack* f = LLFloaterReg::findTypedInstance<WolfFloaterTrack>("wolf_track")) f->onRezAlert(message);
}

// ---- the window ---------------------------------------------------------------------------------------------------------------

WolfFloaterTrack::WolfFloaterTrack(const LLSD& key) : LLFloater(key) {}

WolfFloaterTrack::~WolfFloaterTrack()
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
}

bool WolfFloaterTrack::postBuild()
{
    mHint = getChild<LLTextBox>("track_hint");
    mMode = getChild<LLRadioGroup>("track_mode");
    mUndoBtn = getChild<LLButton>("track_undo");
    mFinishBtn = getChild<LLButton>("track_finish");
    mStartOverBtn = getChild<LLButton>("track_start_over");
    mTopViewBtn = getChild<LLButton>("track_top_view");
    mAskText = getChild<LLTextBox>("track_ask_text");
    mAskYes = getChild<LLButton>("track_ask_yes");
    mAskNo = getChild<LLButton>("track_ask_no");
    mSide = getChild<LLComboBox>("station_side");
    mStyle = getChild<LLComboBox>("station_style");
    mName = getChild<LLLineEditor>("station_name");
    mTTPanel = getChild<LLPanel>("turntable_panel");
    mTTSize = mTTPanel->getChild<LLComboBox>("tt_size");
    mTTRoads = mTTPanel->getChild<LLSpinCtrl>("tt_roads");
    mTTSpread = mTTPanel->getChild<LLSpinCtrl>("tt_spread");
    mTTShed = mTTPanel->getChild<LLCheckBoxCtrl>("tt_shed");
    mTTTurns = mTTPanel->getChild<LLTextBox>("tt_turns");
    mSugPanel = getChild<LLPanel>("tunnel_choice_panel");
    mSugText = mSugPanel->getChild<LLTextBox>("tunnel_choice_text");
    mSugTunnelText = mSugPanel->getChild<LLTextBox>("tunnel_choice_tunnel_text");
    mSugClimbText = mSugPanel->getChild<LLTextBox>("tunnel_choice_climb_text");
    mSugTunnelBtn = mSugPanel->getChild<LLButton>("tunnel_choice_tunnel");
    mSugClimbBtn = mSugPanel->getChild<LLButton>("tunnel_choice_climb");
    mSugCutText = mSugPanel->getChild<LLTextBox>("tunnel_choice_cutting_text");
    mSugCutBtn = mSugPanel->getChild<LLButton>("tunnel_choice_cutting");
    mSugRowRect[0][0] = mSugTunnelBtn->getRect();
    mSugRowRect[0][1] = mSugTunnelText->getRect();
    mSugRowRect[1][0] = mSugCutBtn->getRect();
    mSugRowRect[1][1] = mSugCutText->getRect();
    mSugRowRect[2][0] = mSugClimbBtn->getRect();
    mSugRowRect[2][1] = mSugClimbText->getRect();
    mGroundBtn = getChild<LLButton>("track_ground_again");
    mItemsList = getChild<LLScrollListCtrl>("track_items");
    mTakeOutBtn = getChild<LLButton>("track_take_out");
    mSummary = getChild<LLTextBox>("track_summary");
    mProblems = getChild<LLTextBox>("track_problems");
    mLayBtn = getChild<LLButton>("track_lay");
    mProgress = getChild<LLProgressBar>("track_progress");
    mNextPartBtn = getChild<LLButton>("track_next_part");
    mLookAgainBtn = getChild<LLButton>("track_look_again");
    mTakeAwayBtn = getChild<LLButton>("track_take_away");
    mStatus = getChild<LLTextBox>("track_status");
    mMineCheck = getChild<LLCheckBoxCtrl>("track_mine_check");
    mMinePanel = getChild<LLPanel>("track_mine_panel");
    // [T961 2026-11-21] the scrolled content (floater_wolf_track.xml track_scroll / track_content), measured before the first
    // relayout (below). Open, the content reaches the Your railways panel's foot (it follows the content's top, so this
    // distance never changes) and 5 px under it: W108 opened to 820 with the foot 815 below the window's top (rig, 21 Nov).
    mScroll = getChild<LLScrollContainer>("track_scroll");
    mContent = getChild<LLPanel>("track_content");
    mHeaderH = getRect().getHeight() - mScroll->getRect().getHeight();
    mContentW = mContent->getRect().getWidth();
    mClosedH = mContent->getRect().getHeight();
    mOpenH = mClosedH - mMinePanel->getRect().mBottom + 5;
    mMineList = mMinePanel->getChild<LLScrollListCtrl>("track_mine_list");
    mShowBtn = mMinePanel->getChild<LLButton>("track_show");
    mChangeBtn = mMinePanel->getChild<LLButton>("track_change");
    mLayPartBtn = mMinePanel->getChild<LLButton>("track_lay_part");
    mRemoveBtn = mMinePanel->getChild<LLButton>("track_remove");

    mMode->setSelectedIndex(MODE_LINE);
    mSide->setValue("left");
    mStyle->setValue("town");
    mMode->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMode(); });
    mUndoBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { undoPoint(); });
    mFinishBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { finishLine(); });
    mStartOverBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        ask("Clear the whole drawing and start again?", "Clear it", [this]() { startOver(); });
    });
    mTopViewBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { lookFromAbove(); });
    mAskYes->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        std::function<void()> f = mOnYes;
        endAsk();
        if (f) f();
    });
    mAskNo->setClickedCallback([this](LLUICtrl*, const LLSD&) { endAsk(); setStatus(std::string(), false); });
    mSide->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStationControls(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStationControls(); });
    mName->setKeystrokeCallback([this](LLLineEditor*, void*) { onStationControls(); }, nullptr);
    mName->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStationControls(); });
    mItemsList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPickItem(); });
    // a new turntable's choices until the resident changes them (DESIGN.md 10: spread default 20)
    const Turntable tt_defaults;
    mTTSize->setValue(llformat("%d", tt_defaults.mSize));
    mTTRoads->setValue(tt_defaults.mRoads);
    mTTSpread->setValue(tt_defaults.mSpread);
    mTTShed->set(tt_defaults.mShed);
    mTTSize->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTurntableControls(); });
    mTTRoads->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTurntableControls(); });
    mTTSpread->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTurntableControls(); });
    mTTShed->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTurntableControls(); });
    mTakeOutBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onTakeOut(); });
    mSugTunnelBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSuggestion(SUG_TUNNEL); });
    mSugCutBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSuggestion(SUG_CUTTING); });
    mSugClimbBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSuggestion(SUG_CLIMB); });
    mGroundBtn->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD r = mGroundRetry;
        if (r.isMap()) groundAgain(r["action"].asString(), r["layout"], r["region"].asUUID());
    });
    mMineList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPickMine(); });
    mLayBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLay(); });
    mNextPartBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { layHere(); });
    mLookAgainBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { lookAgain(); });
    mTakeAwayBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { takeAwayPlaced(); });
    mMineCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMineCheck(); });
    mShowBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onShow(); });
    mChangeBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onChange(); });
    mLayPartBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLayPart(); });
    mRemoveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });
    mProgress->setVisible(false);
    mMineCheck->set(false);   // DESIGN.md 1.5: "a closed section of the window"
    relayout();
    fillItemList();
    return true;
}

void WolfFloaterTrack::onOpen(const LLSD& key)
{
    if (!onWolf())
    {
        // paul-wolf-features-wolf-grid-only: the menu item is hidden off Wolf; a window opened another way closes
        closeFloater();
        return;
    }
    // one pen: the Tunnels, Terraform and Lifts windows draw with it too (wolffloaterlifts.cpp:445-453 the same)
    for (const char* other : { "wolf_tunnels", "wolf_terraform", "wolf_lifts" })
    {
        if (LLFloaterReg::instanceVisible(other))
        {
            LLFloaterReg::hideInstance(other);
            setStatus("The Tunnels / Terraform / Lifts window was closed: it draws with the same pen.", false);
        }
    }
    mStartPending = true;
    if (LLViewerRegion* rgn = gAgent.getRegion()) mMineRegion = rgn->getRegionID();
    refreshMine();                                   // DESIGN.md 10 "open": the access and the layouts here
    if (mLaying && mJobId.isDefined()) startStatusPoll();   // a job that ran while the window was closed
    if (mGroundJob.isDefined() && !mGroundPolling)
    {
        // [CUTTINGS] a dig or fill followed when the window was closed: followed again (the closed window's coroutine stopped)
        const LLSD job = mGroundJob, layout = mGroundLayout;
        const std::string kind = mGroundKind, lead = mGroundLead;
        const LLUUID region = mGroundRegion;
        startGroundJob(job, kind, layout, region, lead);
    }
    if (usableLines() > 0) changed();
    updateControls();
}

void WolfFloaterTrack::onClose(bool app_quitting)
{
    if (WolfToolTunnel::instanceExists() && WolfToolTunnel::getInstance()->hostIs(this)) WolfToolTunnel::getInstance()->detach();
    mPenWanted = false;
    ++mGen;   // every coroutine of this window stops at its next wake-up (wolffloaterlifts.cpp mBoxGen, the same)
    mPolling = false;
    mGroundPolling = false;   // [CUTTINGS] its coroutine stops too (mGen); onOpen follows the job again
    mPlanInFlight = false;
    if (app_quitting) return;
    if (mRezzing)
    {
        // the rez may have happened: the pieces are looked for again on the next open, never rezzed a second time
        mRezzing = false;
        mPartialShown = mPartial.mItem.notNull();   // set the moment the rez was sent (rezCoro)
        setStatus(mPartialShown ? "The window was closed while your railway was being placed. Press Look again to find its pieces."
                                : "The window was closed before your railway was placed. Press Lay the next part to place it.", true);
    }
    if (mRemoving)
    {
        mRemoving = false;
        setStatus("The window was closed while pieces were being taken away. Open Your railways and press Remove again "
                  "to finish.", true);
    }
    endAsk();
    giveCameraBack();
}

// ---- the question in the window ---------------------------------------------------------------------------------------------

void WolfFloaterTrack::ask(const std::string& question, const std::string& yes, std::function<void()> on_yes)
{
    mAsking = true;
    mOnYes = std::move(on_yes);
    mAskText->setText(question);
    mAskYes->setLabel(yes);
    mAskText->setVisible(true);
    mAskYes->setVisible(true);
    mAskNo->setVisible(true);
    updateControls();
}

void WolfFloaterTrack::endAsk()
{
    mAsking = false;
    mOnYes = nullptr;
    if (mAskText) mAskText->setVisible(false);
    if (mAskYes) mAskYes->setVisible(false);
    if (mAskNo) mAskNo->setVisible(false);
}

// ---- coordinates --------------------------------------------------------------------------------------------------------------

WolfFloaterTrack::GP WolfFloaterTrack::toGrid(F32 x, F32 y) const
{
    GP p;
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        const LLVector3d g = rgn->getPosGlobalFromRegion(LLVector3(x, y, 0.f));
        p.x = g.mdV[VX];
        p.y = g.mdV[VY];
    }
    return p;
}

LLVector2 WolfFloaterTrack::toRegion(const GP& p) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return LLVector2();
    const LLVector3 r = rgn->getPosRegionFromGlobal(LLVector3d(p.x, p.y, 0.0));
    return LLVector2(r.mV[VX], r.mV[VY]);
}

// The pen picks the land under the pointer in any region this viewer has (not only the agent's), so a line can run on over a
// border. Source: wolffloatertunnels.cpp:4168-4181 WolfToolTunnel::surface (llviewerwindow.cpp mousePointOnLandGlobal), without
// its "inside the agent's region" test; the result is in the agent's region's metres, as the pen's clicks are.
bool WolfFloaterTrack::pickSurface(S32 x, S32 y, LLVector3& out) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d g;
    if (!gViewerWindow->mousePointOnLandGlobal(x, y, &g)) return false;
    if (!LLWorld::getInstance()->getRegionFromPosGlobal(g)) return false;
    const LLVector3 p = rgn->getPosRegionFromGlobal(g);
    if (!std::isfinite(p.mV[VX]) || !std::isfinite(p.mV[VY]) || !std::isfinite(p.mV[VZ])) return false;
    out = p;
    return true;
}

// the pen's marks stand on the land of whichever region they are in (llworld.cpp:973-981 resolveLandHeightGlobal)
bool WolfFloaterTrack::surfaceZ(F32 x, F32 y, F32& z) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    z = LLWorld::getInstance()->resolveLandHeightGlobal(rgn->getPosGlobalFromRegion(LLVector3(x, y, 0.f)));
    return true;
}

F32 WolfFloaterTrack::planeHeight() const
{
    // customSurface() is true, so the pen never uses the underground plane (wolffloatertunnels.cpp:4221); kept as the Lifts'
    LLViewerRegion* rgn = gAgent.getRegion();
    return rgn ? rgn->getPosRegionFromAgent(gAgent.getPositionAgent()).mV[VZ] - 1.f : 0.f;
}

// ---- the drawing ----------------------------------------------------------------------------------------------------------------

bool WolfFloaterTrack::routeEditing() const
{
    return getVisible() && !isMinimized() && onWolf() && !busy();
}

bool WolfFloaterTrack::drawingEmpty() const
{
    return mLines.empty() && mStations.empty() && mSignals.empty() && mTurntables.empty();
}

S32 WolfFloaterTrack::usableLines() const
{
    S32 n = 0;
    for (const Line& l : mLines)
    {
        if (l.mBranch ? !l.mClicks.empty() : l.mClicks.size() >= 2) ++n;
    }
    return n;
}

bool WolfFloaterTrack::onALine(const GP& p, F64 within, GP& snapped, S32 exclude_line) const
{
    F64 best = within;
    bool found = false;
    auto seg = [&](F64 ax, F64 ay, F64 bx, F64 by)
    {
        const F64 vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
        const F64 u = l2 > 1e-9 ? llclamp(((p.x - ax) * vx + (p.y - ay) * vy) / l2, 0.0, 1.0) : 0.0;
        const F64 qx = ax + vx * u, qy = ay + vy * u;
        const F64 d = sqrt((qx - p.x) * (qx - p.x) + (qy - p.y) * (qy - p.y));
        if (d <= best) { best = d; snapped.x = qx; snapped.y = qy; found = true; }
    };
    // the plan's own line first: its curves are not where the clicks' corners are (tracktool/geom.py fits each corner a curve
    // of up to 1000 m radius, so the track can be tens of metres inside the corner) and the planner measures from it
    // (tracktool/layout.py:108-116 nearest_line projects onto ln.XY). Its lines are "L1", "L2"... in the order of the request's
    // lines (tracktool/plan.py:119 lid = "L%d" % (i + 1)), and the request leaves out lines with too few clicks (buildRequest).
    std::string exclude_id;
    if (exclude_line >= 0 && (size_t)exclude_line < mLines.size())
    {
        auto usable = [](const Line& l) { return l.mBranch ? !l.mClicks.empty() : l.mClicks.size() >= 2; };
        if (usable(mLines[exclude_line]))
        {
            S32 n = 0;
            for (S32 i = 0; i < exclude_line; ++i) n += usable(mLines[i]) ? 1 : 0;
            exclude_id = llformat("L%d", n + 1);
        }
    }
    // a plan of an older drawing (the drawing changed since it was asked for) is not used: its line numbers may have moved
    const bool plan_current = !mPlanDirty && !mPlanInFlight;
    for (size_t li = 0; plan_current && li < mPlan.mLines.size(); ++li)
    {
        if (!exclude_id.empty() && mPlan.mLines[li].mId == exclude_id) continue;
        const std::vector<LLVector3d>& P = mPlan.mLines[li].mP;
        for (size_t k = 1; k < P.size(); ++k) seg(P[k - 1].mdV[VX], P[k - 1].mdV[VY], P[k].mdV[VX], P[k].mdV[VY]);
    }
    if (found) return true;
    // no plan yet (or it is not near): the drawn lines
    for (size_t li = 0; li < mLines.size(); ++li)
    {
        if ((S32)li == exclude_line) continue;
        const Line& l = mLines[li];
        std::vector<GP> pts;
        if (l.mBranch) pts.push_back(l.mStart);
        pts.insert(pts.end(), l.mClicks.begin(), l.mClicks.end());
        for (size_t k = 1; k < pts.size(); ++k) seg(pts[k - 1].x, pts[k - 1].y, pts[k].x, pts[k].y);
        if (l.mClosed && pts.size() >= 3) seg(pts.back().x, pts.back().y, pts.front().x, pts.front().y);
    }
    return found;
}

bool WolfFloaterTrack::lineEnd(const GP& p, F64 within, GP& end) const
{
    // tracktool/layout.py add_turntable: a main line's or branch's end that is not a loop (its first or last click; a
    // branch's first end is its points, so only its last); the planner then checks that the end has a buffer stop
    F64 best = within;
    bool found = false;
    auto consider = [&](const GP& e)
    {
        const F64 d = sqrt((e.x - p.x) * (e.x - p.x) + (e.y - p.y) * (e.y - p.y));
        if (d <= best) { best = d; end = e; found = true; }
    };
    for (const Line& l : mLines)
    {
        if (l.mClosed || l.mClicks.empty()) continue;
        if (!l.mBranch) consider(l.mClicks.front());
        if (!l.mJoins) consider(l.mClicks.back());   // a branch that joins a line has no end there
    }
    // the end is taken exactly, so "at" is well within the planner's 6 m of it (tracktool/layout.py add_turntable d <= 6.0)
    return found;
}

void WolfFloaterTrack::onTurntableControls()
{
    if (busy() || mEditTurntable < 0 || (size_t)mEditTurntable >= mTurntables.size()) return;
    Turntable& t = mTurntables[mEditTurntable];
    const S32 size = mTTSize->getValue().asInteger(), roads = mTTRoads->getValue().asInteger(), spread = mTTSpread->getValue().asInteger();
    const bool shed = mTTShed->get();
    if (size == t.mSize && roads == t.mRoads && spread == t.mSpread && shed == t.mShed) return;
    t.mSize = size;
    t.mRoads = roads;
    t.mSpread = spread;
    t.mShed = shed;
    t.mStamp = nextStamp();
    setStatus(std::string(), false);
    changed();
}

void WolfFloaterTrack::clickAt(F32 x, F32 y)
{
    if (busy() || !gAgent.getRegion()) return;
    endAsk();
    GP g = toGrid(x, y);
    auto dist = [](const GP& a, const GP& b) { return sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); };
    const S32 mode = mMode->getSelectedIndex();
    {
        // [15 Nov] a station, signal, tunnel or a branch's start goes ON a line: a click on the plan's line where it stands
        // up on a bank or viaduct (drawn at its rail level, so on the screen it is not over the land under it) is on the
        // line (Paul: "i couldnt really make tunnels work"). WolfStorm: floater_wolf_track_tool.js _onDown / _planLineAt.
        const bool new_branch = mode == MODE_BRANCH && !(mCurLine >= 0 && (size_t)mCurLine < mLines.size() && mLines[mCurLine].mBranch);
        GP on, picked;
        if ((mode == MODE_STATION || mode == MODE_SIGNAL || mode == MODE_TUNNEL || mode == MODE_CUTTING || new_branch) && !onALine(g, ON_LINE_M, on)
            && planLineAtMouse(picked))
        {
            g = picked;
        }
    }
    // [TRACK STEPS 2026-11-18] a station, signal, turntable, tunnel, cutting or a branch's start goes on a line already drawn:
    // with none drawn yet, say so plainly (the click was otherwise answered "Click right on a line")
    if (mode != MODE_LINE && usableLines() == 0 && !(mode == MODE_BRANCH && mCurLine >= 0))
    {
        setStatus("There is no track here yet. First draw your line: choose Line (step 1) and click on the ground. " + needsLineWords(mode), true);
        return;
    }
    if (mode == MODE_LINE || mode == MODE_BRANCH)
    {
        if (mCurLine >= 0 && (size_t)mCurLine < mLines.size() && mLines[mCurLine].mBranch != (mode == MODE_BRANCH)) mCurLine = -1;
        if (mCurLine < 0)
        {
            if (mLines.size() >= MAX_LINES)
            {
                setStatus(llformat("That is the most lines one railway can have (%d). Lay this one, then draw more as a second railway.", (S32)MAX_LINES), true);
                return;
            }
            Line l;
            l.mBranch = mode == MODE_BRANCH;
            if (l.mBranch)
            {
                GP on;
                if (!onALine(g, ON_LINE_M, on))
                {
                    setStatus("Click right on a line where the branch should leave it.", true);
                    return;
                }
                l.mStart = on;
                setStatus("Now click the branch's bends and its end. End it on a line to join that line.", false);
            }
            else
            {
                l.mClicks.push_back(g);
                setStatus("Now click each bend, and where the line ends. Click the first point again to make a loop.", false);
            }
            l.mStamp = nextStamp();
            mLines.push_back(l);
            mCurLine = (S32)mLines.size() - 1;
            changed();
            return;
        }
        Line& l = mLines[mCurLine];
        const GP& last = l.mClicks.empty() ? l.mStart : l.mClicks.back();
        if (dist(last, g) < SAME_CLICK_M) return;   // the second press of a double-click
        if (l.mClicks.size() >= MAX_CLICKS)
        {
            setStatus(llformat("That is the most points one line can have (%d) - drag a point to move it, or press Finish this line.", (S32)MAX_CLICKS), true);
            return;
        }
        if (!l.mBranch && l.mClicks.size() >= 3 && dist(l.mClicks.front(), g) <= LOOP_CLOSE_M)
        {
            l.mClosed = true;
            l.mStamp = nextStamp();
            mCurLine = -1;
            setStatus("The line is closed into a loop. Click somewhere else to start another line, or add a station.", false);
            changed();
            return;
        }
        GP at = g;
        bool joins = false;
        if (l.mBranch && !l.mClicks.empty())
        {
            // tracktool/layout.py:137-151: with at least two clicks, a last click within 6 m of a line joins that line there
            GP on;
            if (onALine(g, JOIN_M, on, mCurLine)) { at = on; joins = true; }
        }
        l.mClicks.push_back(at);
        l.mStamp = nextStamp();
        if (joins)
        {
            l.mJoins = true;
            mCurLine = -1;
            setStatus("The branch joins the line there.", false);
        }
        else
        {
            setStatus(std::string(), false);
        }
        changed();
        return;
    }
    if (mode == MODE_TUNNEL)
    {
        // [TUNNELS] DESIGN.md 3.4 "By hand": its start and its end on one line, as a platform's
        GP on;
        if (!onALine(g, ON_LINE_M, on))
        {
            setStatus("Click right on a line - a tunnel takes the line through the hill.", true);
            return;
        }
        if (mCurTunnel >= 0 && (size_t)mCurTunnel < mTunnels.size())
        {
            Tunnel& t = mTunnels[mCurTunnel];
            if (dist(t.mA, on) < TUNNEL_MIN_M)
            {
                setStatus(llformat("A tunnel is at least %d m long: click its end further away, beyond the hill.", (S32)TUNNEL_MIN_M), true);
                return;
            }
            // [CUTTINGS] DESIGN.md 3.6 contract: a cutting may not overlap a tunnel (nor a tunnel a cutting)
            Tunnel whole = t;
            whole.mB = on;
            whole.mHasB = true;
            const std::string clash = overlapWords(whole, false);
            if (!clash.empty()) { setStatus(clash, true); return; }
            t.mB = on;
            t.mHasB = true;
            t.mStamp = nextStamp();
            mCurTunnel = -1;
            setStatus("The tunnel is marked. Its ends are fitted to the hill when the plan comes back.", false);
            changed();
            return;
        }
        if (mTunnels.size() >= MAX_TUNNELS)
        {
            setStatus(llformat("That is the most tunnels one railway can have (%d).", (S32)MAX_TUNNELS), true);
            return;
        }
        Tunnel t;
        t.mA = on;
        t.mStamp = nextStamp();
        mTunnels.push_back(t);
        mCurTunnel = (S32)mTunnels.size() - 1;
        setStatus("Now click on the same line where the tunnel ends, on the far side of the hill.", false);
        changed();
        return;
    }
    if (mode == MODE_CUTTING)
    {
        // [TRACK TOOL CUTTINGS 2026-11-17] DESIGN.md 3.6 "Window": click where it starts and where it ends on a line, as a tunnel
        GP on;
        if (!onALine(g, ON_LINE_M, on))
        {
            setStatus("Click right on a line - a cutting digs the ground down along the line.", true);
            return;
        }
        if (mCurCutting >= 0 && (size_t)mCurCutting < mCuttings.size())
        {
            Cutting& c = mCuttings[mCurCutting];
            if (dist(c.mA, on) < CUTTING_MIN_M)
            {
                setStatus(llformat("A cutting is at least %d m long: click its end further along the line.", (S32)CUTTING_MIN_M), true);
                return;
            }
            Cutting whole = c;
            whole.mB = on;
            whole.mHasB = true;
            const std::string clash = overlapWords(whole, true);
            if (!clash.empty()) { setStatus(clash, true); return; }
            c.mB = on;
            c.mHasB = true;
            c.mStamp = nextStamp();
            const size_t at = (size_t)mCurCutting;
            mCurCutting = -1;
            setStatus(mergeCuttings(at)
                ? std::string("The cutting overlaps one you marked before, so they were joined into one cutting.")
                : std::string("The cutting is marked. The ground there is dug down to the rails when your railway is laid."), false);
            changed();
            return;
        }
        if (mCuttings.size() >= MAX_CUTTINGS)
        {
            setStatus(llformat("That is the most cuttings one railway can have (%d).", (S32)MAX_CUTTINGS), true);
            return;
        }
        Cutting c;
        c.mA = on;
        c.mB = on;   // a stretch of nothing yet: only its start is tested against the tunnels
        c.mHasB = true;
        const std::string clash = overlapWords(c, true);
        if (!clash.empty()) { setStatus(clash, true); return; }
        c.mHasB = false;
        c.mStamp = nextStamp();
        mCuttings.push_back(c);
        mCurCutting = (S32)mCuttings.size() - 1;
        setStatus("Now click on the same line where the cutting ends.", false);
        changed();
        return;
    }
    if (mode == MODE_TURNTABLE)
    {
        if (mTurntables.size() >= MAX_TURNTABLES)
        {
            setStatus(llformat("That is the most turntables one railway can have (%d).", (S32)MAX_TURNTABLES), true);
            return;
        }
        GP end;
        if (!lineEnd(g, ON_LINE_M, end))
        {
            setStatus("Click right on the end of a line - the turntable goes where a line stops.", true);
            return;
        }
        for (const Turntable& t : mTurntables)
        {
            if (dist(t.mAt, end) < SAME_CLICK_M) { setStatus("There is a turntable at this end already.", true); return; }
        }
        Turntable t;
        t.mAt = end;
        t.mSize = mTTSize->getValue().asInteger();
        t.mRoads = mTTRoads->getValue().asInteger();
        t.mSpread = mTTSpread->getValue().asInteger();
        t.mShed = mTTShed->get();
        t.mStamp = nextStamp();
        mTurntables.push_back(t);
        mEditTurntable = (S32)mTurntables.size() - 1;
        mEditStation = -1;
        setStatus("A turntable was put at the end of the line. Choose its deck, roads and shed in the window; the stall roads fan "
                  "out beyond the end of the line.", false);
        changed();
        return;
    }
    GP on;
    if (!onALine(g, ON_LINE_M, on))
    {
        setStatus(mode == MODE_STATION ? "Click right on a line - a platform goes beside the track."
                                       : "Click right on a line to put a signal there.", true);
        return;
    }
    if (mode == MODE_STATION)
    {
        if (mCurStation >= 0 && (size_t)mCurStation < mStations.size())
        {
            Station& s = mStations[mCurStation];
            if (dist(s.mA, on) < SAME_CLICK_M) return;
            s.mB = on;
            s.mHasB = true;
            s.mStamp = nextStamp();
            mCurStation = -1;
            setStatus(nameOk(s.mName) ? std::string("The platform is drawn.")
                                      : std::string("The platform is drawn. Now type the station's name in the window (for example Badger Hill)."), false);
            if (!nameOk(s.mName)) mName->setFocus(true);
            changed();
            return;
        }
        if (mStations.size() >= MAX_STATIONS)
        {
            setStatus(llformat("That is the most stations one railway can have (%d).", (S32)MAX_STATIONS), true);
            return;
        }
        Station s;
        s.mA = on;
        s.mSide = mSide->getValue().asString();
        s.mStyle = mStyle->getValue().asString();
        s.mName.clear();
        s.mStamp = nextStamp();
        mStations.push_back(s);
        mCurStation = (S32)mStations.size() - 1;
        mEditStation = mCurStation;
        mEditTurntable = -1;
        mName->setText(LLStringUtil::null);
        setStatus("Now click on the line where the platform ends.", false);
        changed();
        return;
    }
    if (mSignals.size() >= MAX_SIGNALS)
    {
        setStatus(llformat("That is the most signals you can add (%d). More are put in by themselves where they are needed.", (S32)MAX_SIGNALS), true);
        return;
    }
    Signal s;
    s.mAt = on;
    s.mStamp = nextStamp();
    mSignals.push_back(s);
    setStatus("A signal was put there.", false);
    changed();
}

void WolfFloaterTrack::moveClick(S32 k, F32 x, F32 y, bool done)
{
    (void)done;
    if (busy() || k < 0 || (size_t)k >= mFlatRefs.size()) return;
    const Ref& r = mFlatRefs[k];
    GP g = toGrid(x, y);
    if (r.mKind == REF_LINE_CLICK && (r.mPoint == 0 || r.mPoint + 1 == mLines[r.mItem].mClicks.size()))
    {
        // a turntable at this line end goes with it
        const GP was = mLines[r.mItem].mClicks[r.mPoint];
        for (Turntable& t : mTurntables)
        {
            if (fabs(t.mAt.x - was.x) < 1e-6 && fabs(t.mAt.y - was.y) < 1e-6) t.mAt = g;
        }
    }
    if (r.mKind == REF_LINE_START || r.mKind == REF_STATION_A || r.mKind == REF_STATION_B || r.mKind == REF_SIGNAL
        || r.mKind == REF_TUNNEL_A || r.mKind == REF_TUNNEL_B || r.mKind == REF_CUTTING_A || r.mKind == REF_CUTTING_B)
    {
        // these stay on a line: a drag puts them on the nearest one (the planner refuses them off a line anyway)
        GP on;
        if (onALine(g, ON_LINE_M, on, r.mKind == REF_LINE_START ? (S32)r.mItem : -1)) g = on;
    }
    switch (r.mKind)
    {
        case REF_LINE_START: mLines[r.mItem].mStart = g; break;
        case REF_LINE_CLICK:
        {
            Line& l = mLines[r.mItem];
            l.mClicks[r.mPoint] = g;
            // a branch's last click joins a line when it is on one (tracktool/layout.py:137-151): said again for where it is now
            GP on;
            if (l.mBranch && r.mPoint + 1 == l.mClicks.size()) l.mJoins = l.mClicks.size() >= 2 && onALine(g, JOIN_M, on, (S32)r.mItem);
            break;
        }
        case REF_STATION_A: mStations[r.mItem].mA = g; break;
        case REF_STATION_B: mStations[r.mItem].mB = g; break;
        case REF_SIGNAL: mSignals[r.mItem].mAt = g; break;
        case REF_TUNNEL_A: mTunnels[r.mItem].mA = g; break;
        case REF_TUNNEL_B: mTunnels[r.mItem].mB = g; break;
        // [CUTTINGS] a dragged end that makes it overlap a tunnel is refused by the window when the plan is asked for
        // (buildRequest: the sentence in the window), not mid-drag
        case REF_CUTTING_A: mCuttings[r.mItem].mA = g; break;
        case REF_CUTTING_B: mCuttings[r.mItem].mB = g; break;
    }
    changed();
}

void WolfFloaterTrack::undoPoint()
{
    if (busy()) return;
    endAsk();
    // the most recently changed thing loses its last point (wolffloaterlifts.cpp undoPoint, for several lines)
    U32 best = 0;
    S32 kind = -1;
    size_t at = 0;
    for (size_t i = 0; i < mLines.size(); ++i) if (mLines[i].mStamp > best) { best = mLines[i].mStamp; kind = 0; at = i; }
    for (size_t i = 0; i < mStations.size(); ++i) if (mStations[i].mStamp > best) { best = mStations[i].mStamp; kind = 1; at = i; }
    for (size_t i = 0; i < mSignals.size(); ++i) if (mSignals[i].mStamp > best) { best = mSignals[i].mStamp; kind = 2; at = i; }
    for (size_t i = 0; i < mTurntables.size(); ++i) if (mTurntables[i].mStamp > best) { best = mTurntables[i].mStamp; kind = 3; at = i; }
    for (size_t i = 0; i < mTunnels.size(); ++i) if (mTunnels[i].mStamp > best) { best = mTunnels[i].mStamp; kind = 4; at = i; }
    for (size_t i = 0; i < mCuttings.size(); ++i) if (mCuttings[i].mStamp > best) { best = mCuttings[i].mStamp; kind = 5; at = i; }
    if (kind < 0)
    {
        setStatus("There is nothing to take back - click on the ground to draw a line.", false);
        return;
    }
    if (kind == 0)
    {
        Line& l = mLines[at];
        if (l.mClosed)
        {
            l.mClosed = false;
            mCurLine = (S32)at;
            setStatus("The loop was opened again.", false);
        }
        else if (!l.mClicks.empty() && !(l.mClicks.size() == 1 && !l.mBranch))
        {
            l.mClicks.pop_back();
            l.mJoins = false;
            mCurLine = (S32)at;
            setStatus("The last point was taken back.", false);
        }
        else
        {
            const bool was_branch = l.mBranch;
            mLines.erase(mLines.begin() + (std::ptrdiff_t)at);
            mCurLine = -1;
            setStatus(was_branch ? "The branch was taken out." : "The line was taken out.", false);
        }
    }
    else if (kind == 1)
    {
        Station& s = mStations[at];
        if (s.mHasB)
        {
            s.mHasB = false;
            mCurStation = (S32)at;
            setStatus("The platform's end was taken back - click where it ends.", false);
        }
        else
        {
            mStations.erase(mStations.begin() + (std::ptrdiff_t)at);
            mCurStation = -1;
            mEditStation = -1;
            setStatus("The station was taken out.", false);
        }
    }
    else if (kind == 2)
    {
        mSignals.erase(mSignals.begin() + (std::ptrdiff_t)at);
        setStatus("The signal was taken out.", false);
    }
    else if (kind == 4)
    {
        Tunnel& t = mTunnels[at];
        if (t.mHasB)
        {
            t.mHasB = false;
            mCurTunnel = (S32)at;
            setStatus("The tunnel's end was taken back - click where it ends.", false);
        }
        else
        {
            mTunnels.erase(mTunnels.begin() + (std::ptrdiff_t)at);
            mCurTunnel = -1;
            setStatus("The tunnel was taken out: the line climbs over the hill again.", false);
        }
    }
    else if (kind == 5)
    {
        Cutting& c = mCuttings[at];
        if (c.mHasB)
        {
            c.mHasB = false;
            mCurCutting = (S32)at;
            setStatus("The cutting's end was taken back - click where it ends.", false);
        }
        else
        {
            mCuttings.erase(mCuttings.begin() + (std::ptrdiff_t)at);
            mCurCutting = -1;
            setStatus("The cutting was taken out: the line climbs over the hill again.", false);
        }
    }
    else
    {
        mTurntables.erase(mTurntables.begin() + (std::ptrdiff_t)at);
        mEditTurntable = -1;
        setStatus("The turntable was taken out.", false);
    }
    changed();
}

void WolfFloaterTrack::removeClick(S32 k)
{
    if (busy() || k < 0 || (size_t)k >= mFlatRefs.size()) return;
    const Ref r = mFlatRefs[k];
    switch (r.mKind)
    {
        case REF_LINE_START:
            mLines.erase(mLines.begin() + (std::ptrdiff_t)r.mItem);
            mCurLine = -1;
            setStatus("The branch was taken out.", false);
            break;
        case REF_LINE_CLICK:
        {
            Line& l = mLines[r.mItem];
            const size_t keep = l.mBranch ? 1 : 2;
            if (l.mClicks.size() <= keep)
            {
                setStatus("A line needs its ends - drag them instead, or pick the line in the window and press Take out.", true);
                return;
            }
            if (r.mPoint + 1 == l.mClicks.size()) l.mJoins = false;
            l.mClicks.erase(l.mClicks.begin() + (std::ptrdiff_t)r.mPoint);
            if (l.mClosed && l.mClicks.size() < 3) l.mClosed = false;
            setStatus("That point was taken out.", false);
            break;
        }
        case REF_STATION_A:
        case REF_STATION_B:
            mStations.erase(mStations.begin() + (std::ptrdiff_t)r.mItem);
            mCurStation = -1;
            mEditStation = -1;
            setStatus("The station was taken out.", false);
            break;
        case REF_SIGNAL:
            mSignals.erase(mSignals.begin() + (std::ptrdiff_t)r.mItem);
            setStatus("The signal was taken out.", false);
            break;
        case REF_TUNNEL_A:
        case REF_TUNNEL_B:
            mTunnels.erase(mTunnels.begin() + (std::ptrdiff_t)r.mItem);
            mCurTunnel = -1;
            setStatus("The tunnel was taken out: the line climbs over the hill again.", false);
            break;
        case REF_CUTTING_A:
        case REF_CUTTING_B:
            mCuttings.erase(mCuttings.begin() + (std::ptrdiff_t)r.mItem);
            mCurCutting = -1;
            setStatus("The cutting was taken out: the line climbs over the hill again.", false);
            break;
    }
    changed();
}

// Source: wolffloaterlifts.cpp:553-574 insertClickNear (the piece of a line nearest the click gets the point), over every line.
void WolfFloaterTrack::insertClickNear(F32 x, F32 y)
{
    if (busy()) return;
    const GP q = toGrid(x, y);
    F64 best_d = 6.0;   // wolffloaterlifts.cpp:569 the same 6 m
    S32 best_line = -1;
    size_t best_at = 0;
    for (size_t li = 0; li < mLines.size(); ++li)
    {
        const Line& l = mLines[li];
        std::vector<GP> pts;
        if (l.mBranch) pts.push_back(l.mStart);
        pts.insert(pts.end(), l.mClicks.begin(), l.mClicks.end());
        const size_t off = l.mBranch ? 1 : 0;
        const size_t n = pts.size();
        const size_t segs = l.mClosed ? n : (n ? n - 1 : 0);
        for (size_t i = 0; i < segs; ++i)
        {
            const GP& a = pts[i];
            const GP& b = pts[(i + 1) % n];
            const F64 vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy;
            const F64 u = l2 > 1e-9 ? llclamp(((q.x - a.x) * vx + (q.y - a.y) * vy) / l2, 0.0, 1.0) : 0.0;
            const F64 d = sqrt((a.x + vx * u - q.x) * (a.x + vx * u - q.x) + (a.y + vy * u - q.y) * (a.y + vy * u - q.y));
            if (d < best_d) { best_d = d; best_line = (S32)li; best_at = i + 1 - off; }
        }
    }
    if (best_line < 0) { setStatus("Ctrl+click on a line to put a bend there.", true); return; }
    Line& l = mLines[best_line];
    if (l.mClicks.size() >= MAX_CLICKS) { setStatus("That is the most bends one line can have.", true); return; }
    l.mClicks.insert(l.mClicks.begin() + (std::ptrdiff_t)llmin(best_at, l.mClicks.size()), q);
    setStatus("A bend was put in - drag it where you want it.", false);
    changed();
}

void WolfFloaterTrack::finishLine()
{
    if (busy()) return;
    if (mCurLine < 0)
    {
        setStatus("Your next click starts a new line.", false);
        return;
    }
    mCurLine = -1;
    setStatus("The line is finished. Your next click starts a new line.", false);
    updateControls();
}

void WolfFloaterTrack::startOver()
{
    if (busy()) return;
    mLines.clear();
    mStations.clear();
    mSignals.clear();
    mTurntables.clear();
    mTunnels.clear();
    mCurTunnel = -1;
    mCuttings.clear();
    mCurCutting = -1;
    mCurLine = mCurStation = mEditStation = mEditTurntable = -1;
    mReplaces = LLSD();
    mReplaceLaid.clear();
    mReplaceName.clear();
    mPlan.clear();
    mPlannedReq = LLSD();
    setStatus("The drawing was cleared. Click on the ground where your line starts.", false);
    changed();
}

void WolfFloaterTrack::onMode()
{
    mCurLine = -1;
    if (mCurStation >= 0 && (size_t)mCurStation < mStations.size() && !mStations[mCurStation].mHasB)
    {
        mStations.erase(mStations.begin() + mCurStation);   // a platform without its end is not kept
        mEditStation = -1;
        changed();
    }
    mCurStation = -1;
    if (mCurTunnel >= 0 && (size_t)mCurTunnel < mTunnels.size() && !mTunnels[mCurTunnel].mHasB)
    {
        mTunnels.erase(mTunnels.begin() + mCurTunnel);   // a tunnel without its end is not kept
        changed();
    }
    mCurTunnel = -1;
    if (mCurCutting >= 0 && (size_t)mCurCutting < mCuttings.size() && !mCuttings[mCurCutting].mHasB)
    {
        mCuttings.erase(mCuttings.begin() + mCurCutting);   // [CUTTINGS] a cutting without its end is not kept
        changed();
    }
    mCurCutting = -1;
    setStatus(std::string(), false);
    updateControls();
}

void WolfFloaterTrack::onStationControls()
{
    if (busy() || mEditStation < 0 || (size_t)mEditStation >= mStations.size()) return;
    Station& s = mStations[mEditStation];
    const std::string side = mSide->getValue().asString(), style = mStyle->getValue().asString();
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    if (side == s.mSide && style == s.mStyle && name == s.mName) return;
    s.mSide = side;
    s.mStyle = style;
    s.mName = name;
    if (!name.empty() && !nameOk(name))
    {
        setStatus("A station's name may have letters, digits, spaces, apostrophes and hyphens, up to 24 of them, and not the word "
                  "\"station\" itself.", true);
    }
    else
    {
        setStatus(std::string(), false);
    }
    changed();
}

void WolfFloaterTrack::onPickItem()
{
    const LLScrollListItem* it = mItemsList->getFirstSelected();
    if (!it) return;
    const std::string v = it->getValue().asString();
    if (v.size() > 2 && (v[0] == 'L' || v[0] == 'G'))
    {
        mEditTurntable = -1;
        mEditStation = -1;
    }
    if (v.size() > 2 && v[0] == 'T')
    {
        const S32 i = atoi(v.c_str() + 2);
        if (i >= 0 && (size_t)i < mTurntables.size())
        {
            mEditTurntable = i;
            mEditStation = -1;
            mTTSize->setValue(llformat("%d", mTurntables[i].mSize));
            mTTRoads->setValue(mTurntables[i].mRoads);
            mTTSpread->setValue(mTurntables[i].mSpread);
            mTTShed->set(mTurntables[i].mShed);
        }
    }
    if (v.size() > 2 && v[0] == 'S')
    {
        const S32 i = atoi(v.c_str() + 2);
        if (i >= 0 && (size_t)i < mStations.size())
        {
            mEditTurntable = -1;
            mEditStation = i;
            mSide->setValue(mStations[i].mSide);
            mStyle->setValue(mStations[i].mStyle);
            mName->setText(mStations[i].mName);
        }
    }
    updateControls();
}

void WolfFloaterTrack::onTakeOut()
{
    if (busy()) return;
    const LLScrollListItem* it = mItemsList->getFirstSelected();
    if (!it) { setStatus("Pick a line, branch, station, signal, turntable, tunnel or cutting in the list first.", true); return; }
    const std::string v = it->getValue().asString();
    if (v.size() < 3) return;
    const size_t i = (size_t)atoi(v.c_str() + 2);
    if (v[0] == 'L' && i < mLines.size())
    {
        setStatus(mLines[i].mBranch ? "The branch was taken out." : "The line was taken out.", false);
        mLines.erase(mLines.begin() + (std::ptrdiff_t)i);
        mCurLine = -1;
    }
    else if (v[0] == 'S' && i < mStations.size())
    {
        mStations.erase(mStations.begin() + (std::ptrdiff_t)i);
        mCurStation = mEditStation = -1;
        setStatus("The station was taken out.", false);
    }
    else if (v[0] == 'G' && i < mSignals.size())
    {
        mSignals.erase(mSignals.begin() + (std::ptrdiff_t)i);
        setStatus("The signal was taken out.", false);
    }
    else if (v[0] == 'T' && i < mTurntables.size())
    {
        mTurntables.erase(mTurntables.begin() + (std::ptrdiff_t)i);
        mEditTurntable = -1;
        setStatus("The turntable was taken out.", false);
    }
    else if (v[0] == 'N' && i < mTunnels.size())
    {
        mTunnels.erase(mTunnels.begin() + (std::ptrdiff_t)i);
        mCurTunnel = -1;
        setStatus("The tunnel was taken out: the line climbs over the hill again.", false);
    }
    else if (v[0] == 'C' && i < mCuttings.size())
    {
        mCuttings.erase(mCuttings.begin() + (std::ptrdiff_t)i);
        mCurCutting = -1;
        setStatus("The cutting was taken out: the line climbs over the hill again.", false);
    }
    changed();
}

void WolfFloaterTrack::fillItemList()
{
    if (!mItemsList) return;
    // the picked row stays picked (this runs after every change, a drag or a letter typed in the name too)
    const LLScrollListItem* was = mItemsList->getFirstSelected();
    const LLSD picked = was ? was->getValue() : LLSD();
    mItemsList->deleteAllItems();
    auto row = [&](const std::string& value, const std::string& text)
    {
        LLSD e;
        e["value"] = value;
        e["columns"][0]["column"] = "what";
        e["columns"][0]["value"] = text;
        mItemsList->addElement(e);
    };
    S32 nl = 0, nb = 0;
    for (size_t i = 0; i < mLines.size(); ++i)
    {
        const Line& l = mLines[i];
        std::string t;
        if (l.mBranch)
        {
            t = llformat("Branch %d - ", ++nb) + plural((S32)l.mClicks.size(), "point", "points");
            if (l.mJoins) t += ", joins a line";
            else if (l.mClicks.empty()) t += " (click its bends and end)";
        }
        else
        {
            t = llformat("Line %d - ", ++nl) + plural((S32)l.mClicks.size(), "point", "points");
            if (l.mClosed) t += ", a loop";
            else if (l.mClicks.size() < 2) t += " (click where it goes next)";
        }
        row(llformat("L:%d", (S32)i), t);
    }
    for (size_t i = 0; i < mStations.size(); ++i)
    {
        const Station& s = mStations[i];
        std::string t = "Station " + (s.mName.empty() ? std::string("(no name yet)") : s.mName) + " - " + styleWords(s.mStyle) + ", "
                      + sideWords(s.mSide);
        if (!s.mHasB) t += " (click where the platform ends)";
        row(llformat("S:%d", (S32)i), t);
    }
    for (size_t i = 0; i < mSignals.size(); ++i) row(llformat("G:%d", (S32)i), llformat("Signal %d", (S32)i + 1));
    for (size_t i = 0; i < mTurntables.size(); ++i)
    {
        const Turntable& t = mTurntables[i];
        row(llformat("T:%d", (S32)i), llformat("Turntable %d - %d m deck, ", (S32)i + 1, t.mSize) + plural(t.mRoads, "stall road", "stall roads")
            + llformat(" %d degrees apart", t.mSpread) + (t.mShed ? ", engine shed" : ""));
    }
    for (size_t i = 0; i < mTunnels.size(); ++i)
    {
        // as fitted to the hill by the last plan (PLAN layout.tunnels, in the order drawn), else as marked
        const Tunnel& t = mTunnels[i];
        std::string w = llformat("Tunnel %d - ", (S32)i + 1);
        const LLSD& fitted = mPlan.mTunnels;
        // [PAUL FMV 2026-11-18] track-tool DESIGN.md 3.8: PLAN layout.tunnels[].marks names the marks each tunnel was made
        // from, so marks the plan joined (overlapping, or under 24 m apart) still show the tunnel they became
        S32 in_tunnel = -1, first_mark = -1;
        if (fitted.isArray() && !mPlanDirty)
        {
            for (S32 k = 0; k < fitted.size() && in_tunnel < 0; ++k)
            {
                const LLSD& m = fitted[k]["marks"];
                for (S32 q = 0; m.isArray() && q < m.size(); ++q)
                    if (m[q].asInteger() == (S32)i) { in_tunnel = k; first_mark = m[0].asInteger(); }
            }
        }
        if (!t.mHasB) w += "(click where it ends)";
        else if (in_tunnel >= 0 && first_mark != (S32)i)
            w += llformat("joined into tunnel %d (the two were too close to be separate tunnels)", first_mark + 1);
        else if (in_tunnel >= 0)
        {
            const LLSD& f = fitted[in_tunnel];
            w += llformat("%d m through the hill, climbs at most ", f["length_m"].asInteger()) + f["grade"].asString();
            if (f["marks"].size() > 1) w += llformat(" (%d of your marks joined into one)", (S32)f["marks"].size());
        }
        else if (fitted.isArray() && (size_t)fitted.size() == mTunnels.size() && !mPlanDirty && !fitted[(S32)i].has("marks"))
            w += llformat("%d m through the hill, climbs at most ", fitted[(S32)i]["length_m"].asInteger()) + fitted[(S32)i]["grade"].asString();
        else w += llformat("%d m as marked", (S32)ll_round((F32)sqrt((t.mB.x - t.mA.x) * (t.mB.x - t.mA.x) + (t.mB.y - t.mA.y) * (t.mB.y - t.mA.y))));
        row(llformat("N:%d", (S32)i), w);
    }
    for (size_t i = 0; i < mCuttings.size(); ++i)
    {
        // [CUTTINGS] as fitted to the hill by the last plan (PLAN layout.cuttings: length_m, deepest_m, walls_m - DESIGN.md 3.6
        // contract), else as marked. The plan joins marks that overlap, so its list is only read when it has one per mark
        const Cutting& c = mCuttings[i];
        std::string w = llformat("Cutting %d - ", (S32)i + 1);
        const LLSD& fitted = mPlan.mCuttings;
        if (!c.mHasB) w += "(click where it ends)";
        else if (fitted.isArray() && (size_t)fitted.size() == mCuttings.size() && !mPlanDirty)
        {
            const LLSD& f = fitted[(S32)i];
            w += llformat("%d m long, up to %d m deep", f["length_m"].asInteger(), ll_round((F32)f["deepest_m"].asReal()));
            const S32 walls = ll_round((F32)f["walls_m"].asReal());
            if (walls > 0) w += llformat(", %d m of stone wall", walls);
        }
        else w += llformat("%d m as marked", (S32)ll_round((F32)sqrt((c.mB.x - c.mA.x) * (c.mB.x - c.mA.x) + (c.mB.y - c.mA.y) * (c.mB.y - c.mA.y))));
        row(llformat("C:%d", (S32)i), w);
    }
    if (drawingEmpty()) row(std::string(), "Nothing yet - click on the ground to start a line.");
    if (picked.isDefined() && !picked.asString().empty()) mItemsList->selectByValue(picked);
}

void WolfFloaterTrack::rebuildFlat()
{
    mFlat.clear();
    mFlatRefs.clear();
    LLViewerRegion* rgn = gAgent.getRegion();
    mFlatRegion = rgn ? rgn->getHandle() : 0;
    // a turntable has no marker of its own: it stands on a line's end, whose marker is its handle (a drag of the end takes
    // the turntable with it: moveClick), and it is changed or taken out from the list
    for (size_t i = 0; i < mLines.size(); ++i)
    {
        const Line& l = mLines[i];
        if (l.mBranch) { mFlat.push_back(toRegion(l.mStart)); mFlatRefs.push_back({ REF_LINE_START, i, 0 }); }
        for (size_t k = 0; k < l.mClicks.size(); ++k) { mFlat.push_back(toRegion(l.mClicks[k])); mFlatRefs.push_back({ REF_LINE_CLICK, i, k }); }
    }
    for (size_t i = 0; i < mStations.size(); ++i)
    {
        mFlat.push_back(toRegion(mStations[i].mA));
        mFlatRefs.push_back({ REF_STATION_A, i, 0 });
        if (mStations[i].mHasB) { mFlat.push_back(toRegion(mStations[i].mB)); mFlatRefs.push_back({ REF_STATION_B, i, 0 }); }
    }
    for (size_t i = 0; i < mSignals.size(); ++i) { mFlat.push_back(toRegion(mSignals[i].mAt)); mFlatRefs.push_back({ REF_SIGNAL, i, 0 }); }
    for (size_t i = 0; i < mTunnels.size(); ++i)
    {
        mFlat.push_back(toRegion(mTunnels[i].mA));
        mFlatRefs.push_back({ REF_TUNNEL_A, i, 0 });
        if (mTunnels[i].mHasB) { mFlat.push_back(toRegion(mTunnels[i].mB)); mFlatRefs.push_back({ REF_TUNNEL_B, i, 0 }); }
    }
    for (size_t i = 0; i < mCuttings.size(); ++i)
    {
        mFlat.push_back(toRegion(mCuttings[i].mA));
        mFlatRefs.push_back({ REF_CUTTING_A, i, 0 });
        if (mCuttings[i].mHasB) { mFlat.push_back(toRegion(mCuttings[i].mB)); mFlatRefs.push_back({ REF_CUTTING_B, i, 0 }); }
    }
}

void WolfFloaterTrack::changed()
{
    mCurLine = mCurLine >= (S32)mLines.size() ? -1 : mCurLine;
    mCurStation = mCurStation >= (S32)mStations.size() ? -1 : mCurStation;
    mEditStation = mEditStation >= (S32)mStations.size() ? -1 : mEditStation;
    mCurTunnel = mCurTunnel >= (S32)mTunnels.size() ? -1 : mCurTunnel;
    mCurCutting = mCurCutting >= (S32)mCuttings.size() ? -1 : mCurCutting;
    // a turntable stands on a line's end (tracktool/layout.py add_turntable: within 6 m): one whose end was taken away goes
    // with it (the planner would refuse it), and one loaded within 6 m of an end (Change) is put on the end exactly
    size_t dropped = 0;
    for (size_t i = mTurntables.size(); i-- > 0;)
    {
        GP end;
        if (lineEnd(mTurntables[i].mAt, 6.0, end)) { mTurntables[i].mAt = end; continue; }
        mTurntables.erase(mTurntables.begin() + (std::ptrdiff_t)i);
        if (mEditTurntable == (S32)i) mEditTurntable = -1;
        else if (mEditTurntable > (S32)i) --mEditTurntable;
        ++dropped;
    }
    if (dropped > 0 && mStatus)
    {
        const std::string said = mStatus->getText();
        setStatus(said + (said.empty() ? "" : " ") + (dropped == 1 ? std::string("The turntable at that end was taken out too.")
                                                                  : llformat("The %d turntables at those ends were taken out too.", (S32)dropped)), false);
    }
    mEditTurntable = mEditTurntable >= (S32)mTurntables.size() ? -1 : mEditTurntable;
    mPlanDirty = true;
    ++mPlanSerial;
    mChangedAt = LLFrameTimer::getElapsedSeconds();
    if (usableLines() == 0)
    {
        mPlan.clear();
        mPlannedReq = LLSD();
        mPlanDirty = false;
    }
    rebuildFlat();
    fillItemList();
    updateControls();
}

std::string WolfFloaterTrack::markerLabel(size_t k) const
{
    if (k >= mFlatRefs.size()) return std::string();
    const Ref& r = mFlatRefs[k];
    switch (r.mKind)
    {
        case REF_LINE_START: return "Branch";
        case REF_LINE_CLICK:
        {
            // [TRACK STEPS 2026-11-18] each point shows its number: the plan names points by them ("points 2 and 3 were
            // merged", "Moving point 4 about 20 m to the north would fix it" - tracktool/geom.py repair / _advice number a line's
            // clicks from 1, and a branch's clicks after the one on the line from 1)
            const Line& l = mLines[r.mItem];
            const S32 num = (S32)r.mPoint + 1;
            if (!l.mBranch && r.mPoint == 0) return l.mClosed ? std::string("1 Loop") : std::string("1 Start");
            if (r.mPoint + 1 == l.mClicks.size() && (S32)r.mItem != mCurLine && !l.mClosed)
                return l.mJoins ? std::string("Joins") : llformat("%d End", num);
            return llformat("%d", num);
        }
        case REF_STATION_A:
            return mStations[r.mItem].mName.empty() ? std::string("Platform") : mStations[r.mItem].mName;
        case REF_STATION_B: return "Platform end";
        case REF_SIGNAL: return "Signal";
        case REF_TUNNEL_A: return llformat("Tunnel %d", (S32)r.mItem + 1);
        case REF_TUNNEL_B: return "Tunnel end";
        case REF_CUTTING_A: return llformat("Cutting %d", (S32)r.mItem + 1);
        case REF_CUTTING_B: return "Cutting end";
    }
    return std::string();
}

std::string WolfFloaterTrack::noGroundText(bool underground) const
{
    (void)underground;
    return "Point at the ground - the railway is drawn on the land your viewer can see.";
}

std::string WolfFloaterTrack::escapeText() const
{
    return "You are drawing a railway. Backspace takes the last point back; closing the window stops.";
}

std::string WolfFloaterTrack::needsLineWords(S32 mode) const
{
    // [TRACK STEPS 2026-11-18] what goes on the line, for the "draw the track first" sentences (routeHint, clickAt)
    switch (mode)
    {
        case MODE_BRANCH: return "A branch leaves a line you have drawn.";
        case MODE_STATION: return "Then put the station beside it.";
        case MODE_SIGNAL: return "Then put signals on it.";
        case MODE_TURNTABLE: return "Then put the turntable at its end.";
        case MODE_TUNNEL: return "Then mark the tunnel on it.";
        case MODE_CUTTING: return "Then mark the cutting on it.";
    }
    return std::string();
}

std::string WolfFloaterTrack::routeHint() const
{
    if (mLaying) return "Your railway is being made - the window shows how far it has got.";
    if (mRezzing) return "Placing your railway on the land...";
    if (mRemoving) return "Taking the railway's pieces away...";
    const S32 mode = mMode ? mMode->getSelectedIndex() : MODE_LINE;
    // [TRACK STEPS 2026-11-18] Paul: "its not obvious that you have to lay the track first THEN do the tunnels and cuttings" -
    // step 2's pens (and a branch, which leaves a line) need a line drawn first: the hint says so before a click is wasted
    if (mode != MODE_LINE && usableLines() == 0 && !(mode == MODE_BRANCH && mCurLine >= 0))
        return "Step 1 first: choose Line and draw your track. " + needsLineWords(mode);
    if (mode == MODE_BRANCH)
    {
        if (mCurLine >= 0 && mLines[mCurLine].mBranch)
            return "Click the branch's bends and its end. End it on a line to join that line.";
        return "Click on a line where the branch should leave it.";
    }
    if (mode == MODE_STATION)
    {
        return mCurStation >= 0 ? "Click on the line where the platform ENDS." : "Click on a line where the platform STARTS.";
    }
    if (mode == MODE_SIGNAL) return "Click on a line to put a signal there.";
    if (mode == MODE_TURNTABLE) return "Click on the END of a line to put a turntable there.";
    if (mode == MODE_TUNNEL)
    {
        if (!mMayTunnel && !mNoTunnelReason.empty()) return mNoTunnelReason;
        return mCurTunnel >= 0 ? "Click on the same line where the tunnel ENDS, beyond the hill."
                               : "Click on a line before the hill where the tunnel STARTS.";
    }
    if (mode == MODE_CUTTING)
    {
        // [CUTTINGS] DESIGN.md 3.6 "open": access.cut / access.cut_reason
        if (!mMayCut && !mNoCutReason.empty()) return mNoCutReason;
        return mCurCutting >= 0 ? "Click on the same line where the cutting ENDS."
                                : "Click on a line where the cutting STARTS, before the rising ground.";
    }
    if (mCurLine < 0)
    {
        if (mLines.empty()) return "Step 1: click on the ground where your line STARTS.";
        if (usableLines() > 0 && !mPlanDirty && !mPlanInFlight && mPlan.mOk)
            return "Next: add a station, tunnel or cutting (step 2), or press Lay track (step 3). A click on the ground starts another line.";
        return "Click on the ground to start another line.";
    }
    const Line& l = mLines[mCurLine];
    if (l.mClicks.size() < 2) return "Step 1: click at each bend, then where the line ends. Too far to click? Alt + drag moves the camera.";
    if (mPlanDirty || mPlanInFlight) return "Working out your track...";
    if (!mPlan.mOk) return "This railway cannot be laid yet - the window says why. Drag a point to move it.";
    return "Keep clicking to make the line longer, then add a station, tunnel or cutting (step 2) or press Lay track (step 3).";
}

// ---- the request (DESIGN.md 10 REQ) ----------------------------------------------------------------------------------------------

bool WolfFloaterTrack::buildRequest(LLSD& req, std::string& why) const
{
    why.clear();
    // the drawing's box: every point that goes into the request
    F64 x1 = 1e300, y1 = 1e300, x2 = -1e300, y2 = -1e300;
    std::vector<GP> pts;
    auto grow = [&](const GP& p) { x1 = llmin(x1, p.x); y1 = llmin(y1, p.y); x2 = llmax(x2, p.x); y2 = llmax(y2, p.y); pts.push_back(p); };
    LLSD lines = LLSD::emptyArray();
    for (const Line& l : mLines)
    {
        if (l.mBranch ? l.mClicks.empty() : l.mClicks.size() < 2) continue;
        LLSD e;
        e["kind"] = l.mBranch ? "branch" : "main";
        LLSD c = LLSD::emptyArray();
        for (const GP& p : l.mClicks) { c.append(xy(p.x, p.y)); grow(p); }
        e["clicks"] = c;
        if (l.mBranch) { e["start"] = xy(l.mStart.x, l.mStart.y); grow(l.mStart); }
        else e["closed"] = l.mClosed;
        lines.append(e);
    }
    if (lines.size() == 0) return false;
    LLSD stations = LLSD::emptyArray();
    for (const Station& s : mStations)
    {
        if (!s.mHasB) continue;
        LLSD e;
        e["a"] = xy(s.mA.x, s.mA.y);
        e["b"] = xy(s.mB.x, s.mB.y);
        e["side"] = s.mSide;
        e["style"] = s.mStyle;
        e["name"] = s.mName.substr(0, STATION_NAME_MAX);
        stations.append(e);
        grow(s.mA);
        grow(s.mB);
    }
    LLSD signals = LLSD::emptyArray();
    for (const Signal& s : mSignals) { signals.append(xy(s.mAt.x, s.mAt.y)); grow(s.mAt); }
    // DESIGN.md 10 REQ turntables: [{at, size 15|20|24, roads 1..10, spread degrees, shed}]
    LLSD turntables = LLSD::emptyArray();
    for (const Turntable& t : mTurntables)
    {
        LLSD e;
        e["at"] = xy(t.mAt.x, t.mAt.y);
        e["size"] = t.mSize;
        e["roads"] = t.mRoads;
        e["spread"] = t.mSpread;
        e["shed"] = t.mShed;
        turntables.append(e);
        grow(t.mAt);
    }

    // [TUNNELS] DESIGN.md 10 REQ tunnels: [{a, b}] (both ends on one line)
    LLSD tunnels = LLSD::emptyArray();
    for (const Tunnel& t : mTunnels)
    {
        if (!t.mHasB) continue;
        tunnels.append(LLSD().with("a", xy(t.mA.x, t.mA.y)).with("b", xy(t.mB.x, t.mB.y)));
        grow(t.mA);
        grow(t.mB);
    }
    // [CUTTINGS] DESIGN.md 3.6 contract: REQ cuttings [{a, b}] (both ends on one line, at most 8; a cutting may not overlap a
    // tunnel - a dragged end can make one: said here, in the window, before anything is sent)
    LLSD cuttings = LLSD::emptyArray();
    for (size_t i = 0; i < mCuttings.size(); ++i)
    {
        const Cutting& c = mCuttings[i];
        if (!c.mHasB) continue;
        const std::string clash = overlapWords(c, true);
        if (!clash.empty()) { why = llformat("Cutting %d: ", (S32)i + 1) + clash; return false; }
        cuttings.append(LLSD().with("a", xy(c.mA.x, c.mA.y)).with("b", xy(c.mB.x, c.mB.y)));
        grow(c.mA);
        grow(c.mB);
    }

    // the ground box (DESIGN.md 10 "Ground box"; the planner's own test requests: test/fixtures.py request())
    F64 step = 4.0;
    F64 gx0 = 0.0, gy0 = 0.0;
    S32 nx = 0, ny = 0;
    for (S32 pass = 0; pass < 2; ++pass)
    {
        gx0 = floor((x1 - GROUND_MARGIN_M) / step) * step;
        gy0 = floor((y1 - GROUND_MARGIN_M) / step) * step;
        nx = (S32)ceil((x2 + GROUND_MARGIN_M - gx0) / step) + 1;
        ny = (S32)ceil((y2 + GROUND_MARGIN_M - gy0) / step) + 1;
        if (nx <= GROUND_MAX && ny <= GROUND_MAX) break;
        step = 8.0;
    }
    if (nx > GROUND_MAX || ny > GROUND_MAX)
    {
        why = "This railway is too big to plan in one go (more than 2 km across). Lay it as two layouts: draw and lay the first "
              "part, then draw the rest from its end.";
        return false;
    }
    const F64 gx1 = gx0 + (nx - 1) * step, gy1 = gy0 + (ny - 1) * step;

    // the regions the box touches: the ones a point is in first, then the nearest; at most MAX_REGIONS
    struct Cand { LLViewerRegion* r; bool has_point; F64 d; };
    std::vector<Cand> cands;
    const F64 cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5;
    for (LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
    {
        if (!r) continue;
        const LLVector3d o = r->getOriginGlobal();
        const F64 w = r->getWidth();
        if (o.mdV[VX] > gx1 || o.mdV[VX] + w < gx0 || o.mdV[VY] > gy1 || o.mdV[VY] + w < gy0) continue;
        bool has = false;
        for (const GP& p : pts)
        {
            if (p.x >= o.mdV[VX] && p.x < o.mdV[VX] + w && p.y >= o.mdV[VY] && p.y < o.mdV[VY] + w) { has = true; break; }
        }
        const F64 dx = o.mdV[VX] + w * 0.5 - cx, dy = o.mdV[VY] + w * 0.5 - cy;
        cands.push_back({ r, has, dx * dx + dy * dy });
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.has_point != b.has_point ? a.has_point : a.d < b.d; });
    size_t with_points = 0;
    for (const Cand& c : cands) if (c.has_point) ++with_points;
    if (with_points > MAX_REGIONS)
    {
        why = llformat("This railway runs through more than %d regions. Lay it as two layouts: draw and lay the first part, then "
                       "draw the rest from its end.", (S32)MAX_REGIONS);
        return false;
    }
    LLSD regions = LLSD::emptyArray();
    for (size_t i = 0; i < cands.size() && i < MAX_REGIONS; ++i)
    {
        LLViewerRegion* r = cands[i].r;
        const LLVector3d o = r->getOriginGlobal();
        LLSD e;
        e["name"] = r->getName();
        e["uuid"] = r->getRegionID().asString();
        e["x0"] = o.mdV[VX];
        e["y0"] = o.mdV[VY];
        e["w"] = (F64)r->getWidth();
        e["h"] = (F64)r->getWidth();   // the viewer's regions are square (llviewerregion.h:263 getWidth)
        e["water"] = (F64)r->getWaterHeight();
        regions.append(e);
    }
    if (regions.size() == 0)
    {
        why = "Your viewer does not have the region under this drawing yet. Fly closer to it and try again.";
        return false;
    }

    // the ground: uint16 little-endian centimetres above the lowest known height, row-major (y rows, x columns)
    std::vector<F32> z((size_t)nx * (size_t)ny, 0.f);
    std::vector<U8> known((size_t)nx * (size_t)ny, 0);
    F32 base = 1e30f;
    for (S32 j = 0; j < ny; ++j)
    {
        for (S32 i = 0; i < nx; ++i)
        {
            F32 h;
            const size_t k = (size_t)j * (size_t)nx + (size_t)i;
            if (landAt(gx0 + i * step, gy0 + j * step, h)) { z[k] = h; known[k] = 1; base = llmin(base, h); }
        }
    }
    if (base > 1e29f) base = 0.f;
    base = floorf(base);   // test/fixtures.py ground_b64: base = floor(min)
    std::vector<U8> raw((size_t)nx * (size_t)ny * 2);
    for (size_t k = 0; k < z.size(); ++k)
    {
        U16 v = GROUND_UNKNOWN;
        if (known[k]) v = (U16)llclamp(ll_round((z[k] - base) * 100.f), 0, 65534);   // test/fixtures.py: clip(cm, 0, 65534)
        raw[2 * k] = (U8)(v & 0xFF);
        raw[2 * k + 1] = (U8)(v >> 8);
    }
    LLSD ground;
    ground["x0"] = gx0;
    ground["y0"] = gy0;
    ground["step"] = step;
    ground["nx"] = nx;
    ground["ny"] = ny;
    ground["base"] = (F64)base;
    ground["data"] = LLBase64::encode(raw.data(), raw.size());

    // guide-shaped objects within the box (DESIGN.md 5.1), the ones of a railway being replaced left out
    const std::set<LLUUID> mine(mReplaceLaid.begin(), mReplaceLaid.end());
    LLSD others = LLSD::emptyArray();
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n && others.size() < OTHERS_MAX; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || !o->isRoot() || o->isAttachment() || o->isAvatar() || !isBoxPrim(o)) continue;
        const LLVector3 sc = o->getScale();
        if (fabsf(sc.mV[VX] - GUIDE_SIZE.mV[VX]) > GUIDE_TOL || fabsf(sc.mV[VY] - GUIDE_SIZE.mV[VY]) > GUIDE_TOL
            || fabsf(sc.mV[VZ] - GUIDE_SIZE.mV[VZ]) > GUIDE_TOL) continue;
        const LLVector3d g = o->getPositionGlobal();
        if (g.mdV[VX] < gx0 || g.mdV[VX] > gx1 || g.mdV[VY] < gy0 || g.mdV[VY] > gy1) continue;
        if (mine.count(o->getID())) continue;
        const LLVector3 fx = LLVector3::x_axis * o->getRotationRegion();   // its +X (Guides.cs:29 Fwd)
        LLSD e = LLSD::emptyArray();
        e.append(g.mdV[VX]);
        e.append(g.mdV[VY]);
        e.append(g.mdV[VZ]);
        e.append((F64)atan2f(fx.mV[VY], fx.mV[VX]));
        others.append(e);
    }

    req = LLSD::emptyMap();
    req["code"] = "NEW";   // DESIGN.md 10: the window always sends "NEW"; the server gives the real code at lay
    req["regions"] = regions;
    req["ground"] = ground;
    req["lines"] = lines;
    req["stations"] = stations;
    req["signals"] = signals;
    req["turntables"] = turntables;
    req["tunnels"] = tunnels;
    req["cuttings"] = cuttings;
    req["others"] = others;
    return true;
}

void WolfFloaterTrack::sendPlan()
{
    mPlanDirty = false;
    if (busy()) return;
    LLSD req;
    std::string why;
    if (!buildRequest(req, why))
    {
        mPlan.clear();
        mPlannedReq = LLSD();
        if (!why.empty()) { mPlan.mRefused = true; mPlan.mProblemText = why; }
        updateControls();
        return;
    }
    mPlanInFlight = true;
    const U32 serial = mPlanSerial;
    LLSD body = LLSD().with("request", req);
    // [CUTTINGS] DESIGN.md 3.6 contract: "plan also takes layout: id (the layout being changed): the preview is then planned on
    // the ground before that layout's cuttings" - a Change's old layout (lay sends the same id as "layout")
    if (mReplaces.isDefined() && !(mReplaces.isString() && mReplaces.asString().empty())) body["layout"] = mReplaces;
    post("plan", body, [serial, req](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        f->mPlanInFlight = false;
        if (serial != f->mPlanSerial) return;   // the drawing changed meanwhile: draw() asks again
        if (!ok)
        {
            f->mPlan.clear();
            f->mPlannedReq = LLSD();
            f->setStatus("Your track could not be worked out: " + error, true);
            f->updateControls();
            return;
        }
        Plan p;
        f->applyPlan(reply["plan"], p);
        f->mPlan = p;
        f->mPlannedReq = req;
        f->updateControls();
    });
}

void WolfFloaterTrack::applyPlan(const LLSD& plan, Plan& out) const
{
    out.clear();
    if (!plan.isMap()) { out.mRefused = true; out.mProblemText = "The railway service sent back no plan."; return; }
    out.mOk = plan["ok"].asBoolean();
    const LLSD& layout = plan["layout"];

    auto nearestZ = [&](F64 x, F64 y, F64& z) -> bool
    {
        F64 best = 1e300;
        bool found = false;
        for (const PlanLine& l : out.mLines)
        {
            for (const LLVector3d& p : l.mP)
            {
                const F64 d = (p.mdV[VX] - x) * (p.mdV[VX] - x) + (p.mdV[VY] - y) * (p.mdV[VY] - y);
                if (d < best) { best = d; z = p.mdV[VZ]; found = true; }
            }
        }
        return found;
    };
    auto groundZ = [](F64 x, F64 y) -> F64
    {
        F32 h;
        return landAt(x, y, h) ? (F64)h : (F64)LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(x, y, 0.0));
    };

    // the lines: rails, sleepers' ends, the structure's legs
    for (const LLSD& l : llsd::inArray(arr(layout["lines"])))
    {
        PlanLine pl;
        pl.mId = l["id"].asString();
        pl.mClosed = l["closed"].asBoolean();
        const LLSD& P = l["P"];
        const LLSD& Z = l["Z"];
        const bool have_z = Z.isArray() && Z.size() == P.size();
        F64 s = 0.0;
        for (LLSD::Integer k = 0; k < (LLSD::Integer)P.size(); ++k)
        {
            F64 x, y;
            if (!readXY(P[k], x, y)) continue;
            const F64 zz = have_z ? Z[k].asReal() : groundZ(x, y) + 0.2;
            if (!pl.mP.empty()) s += sqrt((x - pl.mP.back().mdV[VX]) * (x - pl.mP.back().mdV[VX]) + (y - pl.mP.back().mdV[VY]) * (y - pl.mP.back().mdV[VY]));
            pl.mP.emplace_back(x, y, zz);
            pl.mS.push_back(s);
        }
        const LLSD& structure = l["structure"];
        for (size_t k = 0; k + 1 < pl.mP.size(); ++k)
        {
            const LLVector3d& a = pl.mP[k];
            const LLVector3d& b = pl.mP[k + 1];
            // DESIGN.md 10: structure is per s2 sample (every 2 m)
            const F64 sm = (pl.mS[k] + pl.mS[k + 1]) * 0.5;
            LLColor4 col = COL_UNKNOWN;
            U8 kind = 255;
            if (have_z && structure.isArray() && structure.size() > 0)
            {
                const LLSD::Integer idx = llclamp((LLSD::Integer)(sm / 2.0), 0, (LLSD::Integer)structure.size() - 1);
                const std::string st = structure[idx].asString();
                if (st == "ground") { col = COL_GROUND; kind = 0; }
                else if (st == "tunnel") { col = COL_TUNNEL; kind = 4; }
                else if (st == "cutting") { col = COL_CUTTING; kind = 5; }   // [CUTTINGS] DESIGN.md 3.6 contract
                else if (st == "bank") { col = COL_BANK; kind = 1; }
                else if (st == "viaduct")
                {
                    col = COL_VIADUCT;
                    kind = 2;
                    F32 w, h;
                    if (waterAt(a.mdV[VX], a.mdV[VY], w) && landAt(a.mdV[VX], a.mdV[VY], h) && h < w) { col = COL_WATER; kind = 3; }
                }
            }
            const F64 dx = b.mdV[VX] - a.mdV[VX], dy = b.mdV[VY] - a.mdV[VY], len = sqrt(dx * dx + dy * dy);
            if (len < 1e-6) continue;
            const F64 nx = -dy / len, ny = dx / len;
            for (F64 side : { -HALF_GAUGE_M, HALF_GAUGE_M })
            {
                out.mRails.v.emplace_back(a.mdV[VX] + nx * side, a.mdV[VY] + ny * side, a.mdV[VZ]);
                out.mRails.v.emplace_back(b.mdV[VX] + nx * side, b.mdV[VY] + ny * side, b.mdV[VZ]);
                out.mRails.c.push_back(col);
            }
            // a sleeper every 2 m (1.2 m each side of the middle: the kit's sleeper is 2.42 m long, tracktool/layout.py:27)
            if (k % 2 == 0)
            {
                out.mThin.v.emplace_back(a.mdV[VX] - nx * 1.2, a.mdV[VY] - ny * 1.2, a.mdV[VZ] - 0.05);
                out.mThin.v.emplace_back(a.mdV[VX] + nx * 1.2, a.mdV[VY] + ny * 1.2, a.mdV[VZ] - 0.05);
                out.mThin.c.push_back(col * 0.8f);
            }
            // a bank's or a viaduct's leg every 8 m: from the formation down to the land
            if ((kind == 1 || kind == 2 || kind == 3) && k % 8 == 0)
            {
                out.mThin.v.emplace_back(a.mdV[VX], a.mdV[VY], a.mdV[VZ] - FORMATION_M);
                out.mThin.v.emplace_back(a.mdV[VX], a.mdV[VY], groundZ(a.mdV[VX], a.mdV[VY]));
                out.mThin.c.push_back(col);
            }
        }
        out.mLines.push_back(pl);
    }
    auto lineById = [&](const std::string& id) -> const PlanLine*
    {
        for (const PlanLine& l : out.mLines) if (l.mId == id) return &l;
        return nullptr;
    };
    // a place s metres along line l, its heading (x, y) and its left (-y, x)
    auto along = [](const PlanLine& l, F64 s, LLVector3d& at, F64& hx, F64& hy) -> bool
    {
        if (l.mP.size() < 2) return false;
        size_t k = 0;
        while (k + 2 < l.mP.size() && l.mS[k + 1] < s) ++k;
        const LLVector3d& a = l.mP[k];
        const LLVector3d& b = l.mP[k + 1];
        const F64 seg = llmax(1e-6, l.mS[k + 1] - l.mS[k]);
        const F64 u = llclamp((s - l.mS[k]) / seg, 0.0, 1.0);
        at = a + (b - a) * u;
        const F64 dx = b.mdV[VX] - a.mdV[VX], dy = b.mdV[VY] - a.mdV[VY], len = llmax(1e-6, sqrt(dx * dx + dy * dy));
        hx = dx / len;
        hy = dy / len;
        return true;
    };
    auto outline = [&](const std::vector<LLVector3d>& ring, const LLColor4& col)
    {
        for (size_t k = 0; k + 1 < ring.size(); ++k)
        {
            out.mOutline.v.push_back(ring[k]);
            out.mOutline.v.push_back(ring[k + 1]);
            out.mOutline.c.push_back(col);
        }
    };

    // [CUTTINGS] DESIGN.md 3.6 "Window": the tops of each cutting's sides, dashed, from layout.cuttings and the line - where the
    // section (CUT_* above) meets the ground this viewer has under the line; nothing where the rail is not below the ground
    for (const LLSD& c : llsd::inArray(arr(layout["cuttings"])))
    {
        const PlanLine* l = lineById(c["line"].asString());
        if (!l) continue;
        const F64 s0 = c["s0"].asReal(), s1 = c["s1"].asReal();
        LLVector3d prev[2];
        bool prev_ok = false;
        S32 n = 0;
        for (F64 s = s0; s <= s1 + 1e-6; s += CUT_DASH_M, ++n)
        {
            LLVector3d p;
            F64 hx, hy;
            bool ok = along(*l, llmin(s, s1), p, hx, hy);
            const F64 depth = ok ? groundZ(p.mdV[VX], p.mdV[VY]) - (p.mdV[VZ] - FORMATION_M) : 0.0;
            ok = ok && depth > 0.0;
            LLVector3d cur[2];
            if (ok)
            {
                const F64 w = depth > CUT_WALL_AT_M ? CUT_WALL_FACE_M
                                                    : CUT_SIDE_FROM_M + depth * (depth > CUT_ROCK_AT_M ? CUT_SLOPE_ROCK : CUT_SLOPE_SOIL);
                for (S32 k = 0; k < 2; ++k)
                {
                    const F64 sgn = k == 0 ? 1.0 : -1.0;   // left (-hy, hx), then right
                    const F64 x = p.mdV[VX] - hy * w * sgn, y = p.mdV[VY] + hx * w * sgn;
                    cur[k] = LLVector3d(x, y, groundZ(x, y) + 0.15);
                }
            }
            if (ok && prev_ok && n % 2 == 1)   // every other 2 m: dashed
            {
                for (S32 k = 0; k < 2; ++k)
                {
                    out.mCutEdges.v.push_back(prev[k]);
                    out.mCutEdges.v.push_back(cur[k]);
                    out.mCutEdges.c.push_back(COL_CUTTING);
                }
            }
            prev[0] = cur[0];
            prev[1] = cur[1];
            prev_ok = ok;
        }
    }

    // stations: platform outlines beside the line (DESIGN.md 4.4; offsets tracktool/layout.py:35-36)
    for (const LLSD& st : llsd::inArray(arr(layout["stations"])))
    {
        const PlanLine* l = lineById(st["line"].asString());
        if (!l) continue;
        const F64 s0 = st["s0"].asReal(), s1 = st["s1"].asReal();
        const std::string side = st["side"].asString();
        std::vector<std::pair<F64, F64>> bands;   // left offsets (negative = right) from the line's middle
        if (side == "left" || side == "both") bands.emplace_back(PLATFORM_EDGE_M, PLATFORM_EDGE_M + PLATFORM_DEPTH_M);
        if (side == "right" || side == "both") bands.emplace_back(-PLATFORM_EDGE_M - PLATFORM_DEPTH_M, -PLATFORM_EDGE_M);
        if (side == "island")
        {
            // DESIGN.md 10 PLAN stations: `loop` is the island's passing loop line. The platform's middle is half way between the
            // line and the loop at every sample along it (the loop's nearest point), ISLAND_W wide (tracktool/layout.py:36).
            const PlanLine* loop = lineById(st["loop"].asString());
            std::vector<LLVector3d> inner, outer;
            for (F64 s = s0; s <= s1 + 1e-6; s += llmax(1.0, (s1 - s0) / 40.0))
            {
                LLVector3d p;
                F64 hx, hy;
                if (!along(*l, llmin(s, s1), p, hx, hy)) continue;
                F64 off = 0.0;   // no loop in the reply: centred on the line
                if (loop)
                {
                    F64 best = 1e300;
                    for (const LLVector3d& q : loop->mP)
                    {
                        const F64 dx = q.mdV[VX] - p.mdV[VX], dy = q.mdV[VY] - p.mdV[VY], d = dx * dx + dy * dy;
                        if (d < best) { best = d; off = -hy * dx + hx * dy; }
                    }
                }
                const F64 c = off * 0.5;
                inner.emplace_back(p.mdV[VX] - hy * (c - ISLAND_W_M * 0.5), p.mdV[VY] + hx * (c - ISLAND_W_M * 0.5), p.mdV[VZ] + 0.1);
                outer.emplace_back(p.mdV[VX] - hy * (c + ISLAND_W_M * 0.5), p.mdV[VY] + hx * (c + ISLAND_W_M * 0.5), p.mdV[VZ] + 0.1);
            }
            if (inner.size() >= 2)
            {
                std::vector<LLVector3d> ring = inner;
                ring.insert(ring.end(), outer.rbegin(), outer.rend());
                ring.push_back(inner.front());
                outline(ring, COL_STATION);
            }
        }
        for (const auto& b : bands)
        {
            std::vector<LLVector3d> inner, outer;
            for (F64 s = s0; s <= s1 + 1e-6; s += llmax(1.0, (s1 - s0) / 40.0))
            {
                LLVector3d p;
                F64 hx, hy;
                if (!along(*l, llmin(s, s1), p, hx, hy)) continue;
                inner.emplace_back(p.mdV[VX] - hy * b.first, p.mdV[VY] + hx * b.first, p.mdV[VZ] + 0.1);
                outer.emplace_back(p.mdV[VX] - hy * b.second, p.mdV[VY] + hx * b.second, p.mdV[VZ] + 0.1);
            }
            if (inner.size() < 2) continue;
            std::vector<LLVector3d> ring = inner;
            ring.insert(ring.end(), outer.rbegin(), outer.rend());
            ring.push_back(inner.front());
            outline(ring, COL_STATION);
        }
        LLVector3d mid;
        F64 hx, hy;
        if (along(*l, (s0 + s1) * 0.5, mid, hx, hy))
        {
            out.mLabels.push_back({ LLVector3d(mid.mdV[VX], mid.mdV[VY], mid.mdV[VZ] + 3.0), st["name"].asString(), COL_STATION });
        }
    }
    // points: a ring at the toe and the name
    for (const LLSD& p : llsd::inArray(arr(layout["points"])))
    {
        F64 x, y, z;
        if (!readXY(p["at"], x, y)) continue;
        if (!nearestZ(x, y, z)) z = groundZ(x, y);
        std::vector<LLVector3d> ring;
        for (S32 i = 0; i <= 16; ++i)
        {
            const F64 a = (F64)i / 16.0 * F_TWO_PI;
            ring.emplace_back(x + cos(a) * 2.5, y + sin(a) * 2.5, z + 0.1);
        }
        outline(ring, COL_POINTS);
        out.mOutline.v.emplace_back(x, y, z);
        out.mOutline.v.emplace_back(x, y, z + 4.0);
        out.mOutline.c.push_back(COL_POINTS);
        out.mLabels.push_back({ LLVector3d(x, y, z + 4.0), "Points " + p["name"].asString(), COL_POINTS });
    }
    // turntables (DESIGN.md 10 PLAN layout.turntables): the deck circle (diameter = size) and the pit ring (size / 2 + 0.3);
    // the stall roads are ordinary lines of kind "stall", drawn above with the rest
    for (const LLSD& t : llsd::inArray(arr(layout["turntables"])))
    {
        F64 x, y, z;
        if (!readXY(t["at"], x, y)) continue;
        if (!nearestZ(x, y, z)) z = groundZ(x, y);
        const F64 r_deck = t["size"].asReal() * 0.5;
        for (F64 r : { r_deck, r_deck + TT_PIT_EXTRA_M })
        {
            std::vector<LLVector3d> ring;
            for (S32 i = 0; i <= 48; ++i)
            {
                const F64 a = (F64)i / 48.0 * F_TWO_PI;
                ring.emplace_back(x + cos(a) * r, y + sin(a) * r, z + 0.05);
            }
            outline(ring, COL_TURNTABLE);
        }
        // the deck's own length across it, so it reads as a turning bridge (along the first road's compass bearing)
        const LLSD& roads = t["roads"];
        F64 bearing = 0.0;
        if (roads.isArray() && roads.size() > 0) bearing = roads[0]["compass"].asReal() * DEG_TO_RAD;   // clockwise from north
        const F64 dx = sin(bearing) * r_deck, dy = cos(bearing) * r_deck;
        out.mOutline.v.emplace_back(x - dx, y - dy, z + 0.05);
        out.mOutline.v.emplace_back(x + dx, y + dy, z + 0.05);
        out.mOutline.c.push_back(COL_TURNTABLE);
        out.mLabels.push_back({ LLVector3d(x, y, z + 3.0), "Turntable " + t["name"].asString(), COL_TURNTABLE });
    }
    // buffer stops: a bar across the line's end and two posts
    for (const LLSD& b : llsd::inArray(arr(layout["buffers"])))
    {
        const PlanLine* l = lineById(b["line"].asString());
        LLVector3d at;
        F64 hx, hy;
        if (!l || !along(*l, b["s"].asReal(), at, hx, hy)) continue;
        const LLVector3d left(-hy * 1.5, hx * 1.5, 0.0);
        out.mOutline.v.push_back(at - left + LLVector3d(0, 0, 0.8));
        out.mOutline.v.push_back(at + left + LLVector3d(0, 0, 0.8));
        out.mOutline.c.push_back(COL_SIGNAL);
        for (F64 sgn : { -1.0, 1.0 })
        {
            out.mOutline.v.push_back(at + left * sgn);
            out.mOutline.v.push_back(at + left * sgn + LLVector3d(0, 0, 1.2));
            out.mOutline.c.push_back(COL_SIGNAL);
        }
    }
    // signals: a post on the driver's left of each "signal <name>" guide (Guides.cs:88; DESIGN.md 4.5 "on the driver's left")
    static const std::string SIGNAL("signal ");
    for (const LLSD& g : llsd::inArray(arr(layout["guides"])))
    {
        const std::string desc = g["desc"].asString();
        if (desc.compare(0, SIGNAL.size(), SIGNAL) != 0) continue;
        const F64 yaw = g["yaw"].asReal();
        const F64 x = g["x"].asReal() - sin(yaw) * 2.2, y = g["y"].asReal() + cos(yaw) * 2.2, z = g["z"].asReal();
        out.mOutline.v.emplace_back(x, y, z - 0.5);
        out.mOutline.v.emplace_back(x, y, z + 4.0);
        out.mOutline.c.push_back(COL_SIGNAL);
        out.mOutline.v.emplace_back(x - 0.3, y, z + 4.0);
        out.mOutline.v.emplace_back(x + 0.3, y, z + 4.0);
        out.mOutline.c.push_back(COL_SIGNAL);
    }

    // the summary sentence (DESIGN.md 1.2: "820 m of track, 2 curves (tightest 180 m), 1 station, 1 set of points, rises 6 m.")
    const LLSD& sm = plan["summary"];
    if (sm.isMap() && !out.mLines.empty())
    {
        std::string t = llformat("%d m of track", sm["length_m"].asInteger());
        const S32 curves = sm["curves"].asInteger();
        if (curves > 0)
        {
            t += ", " + plural(curves, "curve", "curves");
            if (sm["tightest_m"].isDefined()) t += llformat(" (tightest %d m)", sm["tightest_m"].asInteger());
        }
        if (sm["stations"].asInteger() > 0) t += ", " + plural(sm["stations"].asInteger(), "station", "stations");
        if (sm["points"].asInteger() > 0) t += ", " + plural(sm["points"].asInteger(), "set of points", "sets of points");
        if (sm["tunnels"].asInteger() > 0)
            t += ", " + plural(sm["tunnels"].asInteger(), "tunnel", "tunnels") + llformat(" (%d m)", sm["tunnel_m"].asInteger());
        // [CUTTINGS] DESIGN.md 3.6 contract: summary cuttings, cutting_m
        if (sm["cuttings"].asInteger() > 0)
            t += ", " + plural(sm["cuttings"].asInteger(), "cutting", "cuttings") + llformat(" (%d m)", ll_round((F32)sm["cutting_m"].asReal()));
        const S32 rises = ll_round((F32)sm["rises_m"].asReal());
        if (rises > 0) t += llformat(", rises %d m", rises);
        out.mSummary = t + ".";
    }

    // the problems: refused first, then warnings; each a red mark at its place
    std::vector<std::string> refused, warned;
    for (const LLSD& p : llsd::inArray(arr(plan["problems"])))
    {
        const bool r = p["level"].asString() == "refused";
        const std::string text = p["text"].asString();
        (r ? refused : warned).push_back(text);
        F64 x, y;
        if (readXY(p["at"], x, y))
        {
            out.mProblems.push_back({ LLVector3d(x, y, groundZ(x, y)), text, r ? COL_PROBLEM : COL_STATION });
        }
    }
    out.mRefused = !refused.empty() || !out.mOk;
    std::string text;
    size_t shown = 0;
    const size_t total = refused.size() + warned.size();
    auto add = [&](const std::vector<std::string>& v, const char* lead)
    {
        for (const std::string& s : v)
        {
            if (shown >= 3) return;
            if (!text.empty()) text += "\n";
            text += lead + s;
            ++shown;
        }
    };
    add(refused, "Cannot lay: ");
    add(warned, "Note: ");
    if (total > shown) text += llformat("\n...and %d more (the red marks in the world).", (S32)(total - shown));
    if (!out.mOk && refused.empty()) text = "This railway cannot be laid yet.";
    out.mProblemText = text;
    out.mTunnels = arr(layout["tunnels"]);
    out.mSuggestions = arr(plan["suggestions"]);
    out.mCuttings = arr(layout["cuttings"]);            // [CUTTINGS] DESIGN.md 3.6 contract
    out.mCutSuggestions = arr(plan["cut_suggestions"]);
}

// ---- the preview in the world ---------------------------------------------------------------------------------------------

// Source: wolffloaterlifts.cpp:633-666 renderRouteExtras (gGL lines in the agent frame, depth test off so the plan shows over
// the land), called by the pen's render (wolffloatertunnels.cpp:4546-4549) with the texture unbound and alpha blending on.
void WolfFloaterTrack::renderRouteExtras(LLViewerRegion* rgn)
{
    if (!rgn || !getVisible() || isMinimized()) return;
    auto A = [&](const LLVector3d& g) { return rgn->getPosAgentFromRegion(rgn->getPosRegionFromGlobal(g)); };
    auto strokes = [&](const Strokes& s, const LLColor4* over)
    {
        if (s.v.size() < 2) return;
        gGL.begin(LLRender::LINES);
        for (size_t i = 0; 2 * i + 1 < s.v.size(); ++i)
        {
            gGL.color4fv((over ? *over : (i < s.c.size() ? s.c[i] : COL_UNKNOWN)).mV);
            gGL.vertex3fv(A(s.v[2 * i]).mV);
            gGL.vertex3fv(A(s.v[2 * i + 1]).mV);
        }
        gGL.end();
    };
    LLGLDepthTest depth(GL_FALSE);
    // the drawn lines, on the land between the clicks (a few points along each piece so it follows the hill:
    // wolffloatertunnels.cpp:4418-4428 routeMarks, the same 4 m)
    {
        Strokes drawn;
        auto piece = [&](const GP& a, const GP& b)
        {
            const F64 len = sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            const S32 parts = llclamp((S32)(len / 4.0), 1, 256);
            for (S32 i = 0; i < parts; ++i)
            {
                const F64 u0 = (F64)i / parts, u1 = (F64)(i + 1) / parts;
                const F64 ax = a.x + (b.x - a.x) * u0, ay = a.y + (b.y - a.y) * u0, bx = a.x + (b.x - a.x) * u1, by = a.y + (b.y - a.y) * u1;
                drawn.v.emplace_back(ax, ay, LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(ax, ay, 0.0)) + 0.15);
                drawn.v.emplace_back(bx, by, LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(bx, by, 0.0)) + 0.15);
                drawn.c.push_back(COL_DRAWN);
            }
        };
        for (const Line& l : mLines)
        {
            std::vector<GP> pts;
            if (l.mBranch) pts.push_back(l.mStart);
            pts.insert(pts.end(), l.mClicks.begin(), l.mClicks.end());
            for (size_t k = 1; k < pts.size(); ++k) piece(pts[k - 1], pts[k]);
            if (l.mClosed && pts.size() >= 3) piece(pts.back(), pts.front());
        }
        for (const Station& s : mStations) if (s.mHasB) piece(s.mA, s.mB);
        gGL.setLineWidth(1.5f);
        strokes(drawn, nullptr);
    }
    // a railway shown from Your railways
    gGL.setLineWidth(2.f);
    strokes(mShown.mRails, &COL_SHOWN);
    strokes(mShown.mOutline, &COL_SHOWN);
    // the plan
    gGL.setLineWidth(1.f);
    strokes(mPlan.mThin, nullptr);
    gGL.setLineWidth(3.f);
    strokes(mPlan.mRails, nullptr);
    gGL.setLineWidth(2.f);
    strokes(mPlan.mOutline, nullptr);
    strokes(mPlan.mCutEdges, nullptr);   // [CUTTINGS] the tops of a cutting's sides, dashed
    // every problem: a post from the land and a ring round its foot
    Strokes red;
    for (const PlanMark& m : mPlan.mProblems)
    {
        red.v.push_back(m.mAt);
        red.v.emplace_back(m.mAt.mdV[VX], m.mAt.mdV[VY], m.mAt.mdV[VZ] + 8.0);
        red.c.push_back(m.mColour);
        for (S32 i = 0; i < 16; ++i)
        {
            const F64 a0 = (F64)i / 16.0 * F_TWO_PI, a1 = (F64)(i + 1) / 16.0 * F_TWO_PI;
            red.v.emplace_back(m.mAt.mdV[VX] + cos(a0) * 3.0, m.mAt.mdV[VY] + sin(a0) * 3.0, m.mAt.mdV[VZ] + 0.2);
            red.v.emplace_back(m.mAt.mdV[VX] + cos(a1) * 3.0, m.mAt.mdV[VY] + sin(a1) * 3.0, m.mAt.mdV[VZ] + 0.2);
            red.c.push_back(m.mColour);
        }
    }
    // [15 Nov] the hill whose choice is waiting (WolfStorm _draw3d COL_SUG): posts at its two ends and its middle, purple, so
    // the choice shows where it is, not only in the window (Paul: "i couldnt really make tunnels work")
    const LLSD sug = pendingSuggestion();
    if (sug.isMap())
    {
        // [CUTTINGS] a hill whose only choice is a cutting: "Cutting?" posts in light earth (DESIGN.md 3.6 "Window")
        const LLColor4& mark = sug["cut_only"].asBoolean() ? COL_CUTTING_MARK : COL_TUNNEL_MARK;
        auto post = [&](const LLSD& p, F64 h, F64 r)
        {
            F64 x, y;
            if (!readXY(p, x, y)) return;
            F32 g32;
            const F64 z = landAt(x, y, g32) ? (F64)g32 : (F64)LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(x, y, 0.0));
            red.v.emplace_back(x, y, z);
            red.v.emplace_back(x, y, z + h);
            red.c.push_back(mark);
            for (S32 i = 0; i < 16; ++i)
            {
                const F64 a0 = (F64)i / 16.0 * F_TWO_PI, a1 = (F64)(i + 1) / 16.0 * F_TWO_PI;
                red.v.emplace_back(x + cos(a0) * r, y + sin(a0) * r, z + 0.2);
                red.v.emplace_back(x + cos(a1) * r, y + sin(a1) * r, z + 0.2);
                red.c.push_back(mark);
            }
        };
        post(sug["a"], 8.0, 2.5);
        post(sug["b"], 8.0, 2.5);
        post(sug["at"], 14.0, 4.0);
    }
    gGL.setLineWidth(4.f);
    strokes(red, nullptr);
    gGL.setLineWidth(1.f);
}

// The names and the problems' words beside the preview (wolffloatertunnels.cpp:4684-4695: projectPosAgentToScreen, then the
// sans-serif font with a drop shadow, as the pen's own marker names).
void WolfFloaterTrack::drawRouteExtras(LLViewerRegion* rgn)
{
    if (!rgn || !getVisible() || isMinimized()) return;
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 eye = cam->getOrigin();
    auto label = [&](const PlanMark& m, bool always)
    {
        const LLVector3 a = rgn->getPosAgentFromRegion(rgn->getPosRegionFromGlobal(m.mAt));
        if (!always && dist_vec(a, eye) > LABEL_FAR_M) return;
        LLCoordGL sp;
        if (!cam->projectPosAgentToScreen(a, sp, false)) return;
        std::string t = m.mText;
        if (t.size() > 90) t = t.substr(0, 87) + "...";
        font->renderUTF8(t, 0, (F32)sp.mX + 8.f, (F32)sp.mY + 4.f, m.mColour, LLFontGL::LEFT, LLFontGL::BOTTOM, LLFontGL::BOLD,
                         LLFontGL::DROP_SHADOW);
    };
    for (const PlanMark& m : mShown.mLabels) label(PlanMark{ m.mAt, m.mText, COL_SHOWN }, false);
    for (const PlanMark& m : mPlan.mLabels) label(m, false);
    for (const PlanMark& m : mPlan.mProblems)
    {
        PlanMark up = m;
        up.mAt.mdV[VZ] += 8.0;
        label(up, true);
    }
    const LLSD sug = pendingSuggestion();                // [15 Nov] the waiting hill choice's post (renderRouteExtras)
    F64 sx, sy;
    if (sug.isMap() && readXY(sug["at"], sx, sy))
    {
        F32 g32;
        const F64 z = landAt(sx, sy, g32) ? (F64)g32 : (F64)LLWorld::getInstance()->resolveLandHeightGlobal(LLVector3d(sx, sy, 0.0));
        // [CUTTINGS] a hill whose only choice is a cutting: "Cutting?" (DESIGN.md 3.6 "Window")
        if (sug["cut_only"].asBoolean())
            label(PlanMark{ LLVector3d(sx, sy, z + 14.0), "Cutting? A hill is in the way - choose in the window", COL_CUTTING_MARK }, true);
        else
            label(PlanMark{ LLVector3d(sx, sy, z + 14.0), "Tunnel? A hill is in the way - choose in the window", COL_TUNNEL_MARK }, true);
    }
}

// ---- transport ------------------------------------------------------------------------------------------------------------------

// static
bool WolfFloaterTrack::callNow(const std::string& action, LLSD body, LLSD& reply, std::string& error)
{
    // Source: wolffloaterbuildinggen.cpp:919-937 requestCoro - WolfCaves::serviceRequest (POST JSON with X-Wolf-Agent /
    // X-Wolf-Session, no automatic retry: wolfcave.cpp:141-169), the action in the body; every reply {"success": ...} with the
    // resident's words in "error" (DESIGN.md 10)
    body["action"] = action;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(TRACK_URL, &text, TIMEOUT_S);
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    error = ok ? std::string() : serviceError(status, reply, transport);
    if (!ok) LL_INFOS("WolfTrack") << action << ": " << status << " " << error << LL_ENDL;
    return ok;
}

void WolfFloaterTrack::post(const std::string& action, LLSD body, reply_fn_t then)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfTrack", [h, action, body, then]() { postCoro(h, action, body, then); });
}

// static
void WolfFloaterTrack::postCoro(LLHandle<LLFloater> h, std::string action, LLSD body, reply_fn_t then)
{
    LLSD reply;
    std::string error;
    const bool ok = callNow(action, body, reply, error);
    // the window is looked up again through its handle (wolffloaterbuildinggen.cpp:933-936): gone meanwhile, the reply is dropped
    if (WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get()))
    {
        if (then) then(f, ok, reply, error);
    }
}

// ---- laying -------------------------------------------------------------------------------------------------------------------

// static
std::vector<WolfFloaterTrack::Item> WolfFloaterTrack::readItems(const LLSD& items)
{
    // DESIGN.md 10 ITEM {region, region_name, item, folder, ray_end (region metres), objects [{name, root}], laid}
    std::vector<Item> out;
    for (const LLSD& e : llsd::inArray(arr(items)))
    {
        Item it;
        it.mRegion = e["region"].asUUID();
        it.mRegionName = e["region_name"].asString();
        it.mItem = e["item"].asUUID();
        it.mFolder = e["folder"].asUUID();
        it.mRayEnd = readV3(e["ray_end"]);
        it.mLaid = e["laid"].asBoolean();
        for (const LLSD& o : llsd::inArray(arr(e["objects"]))) it.mObjects.push_back({ o["name"].asString(), readV3(o["root"]) });
        it.mTunnels = arr(e["tunnels"]);
        if (it.mRegion.notNull() && it.mItem.notNull()) out.push_back(it);
    }
    return out;
}

void WolfFloaterTrack::onLay()
{
    if (busy() || !mPlan.mOk || mPlanDirty || mPlanInFlight || !mPlannedReq.isMap() || !onWolf()) return;
    endAsk();
    // the last lay of this drawing still has parts to put down: the drawing has to change first (else the same railway is
    // made twice), and when some of it is laid already the resident says that this is to be another railway
    bool unlaid = false, laid = false;
    for (const Item& it : mItems) { unlaid = unlaid || !it.mLaid; laid = laid || it.mLaid; }
    if (mItemsFromDrawing && unlaid && mLaySerial == mPlanSerial) return;
    if (mItemsFromDrawing && unlaid && laid && !mLayAnyway)
    {
        ask("Part of your last railway is laid already. Lay this drawing as another railway?", "Lay it", [this]()
        {
            mLayAnyway = true;
            onLay();
        });
        return;
    }
    mLayAnyway = false;
    // DESIGN.md 10: the name is the first station's, else "Railway"
    std::string name = "Railway";
    for (const Station& s : mStations)
    {
        if (s.mHasB && nameOk(s.mName)) { name = s.mName; break; }
    }
    LLSD body;
    body["request"] = mPlannedReq;
    body["layout"] = mReplaces;   // undefined -> null: a new railway
    body["name"] = name;
    mLaying = true;
    mPartialShown = false;
    mLaySerial = mPlanSerial;
    mProgress->setValue(0.f);
    setStatus("Laying your track: sending your drawing...", false);
    LL_INFOS("WolfTrack") << "lay '" << name << "'" << (mReplaces.isDefined() ? " (replacing a railway)" : "") << LL_ENDL;
    post("lay", body, [](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok)
        {
            f->mLaying = false;
            // DESIGN.md 10: "lay" plans again first and refuses with the plan's first refusal (bad_request + that sentence):
            // it is shown as a plan problem, and the drawing must change before Lay track is offered again
            if (reply.isMap() && reply["code"].asString() == "bad_request")
            {
                f->mPlan.mOk = false;
                f->mPlan.mRefused = true;
                f->mPlan.mProblemText = "Cannot lay: " + error;
            }
            f->setStatus("Your railway was not laid: " + error, true);
            f->updateControls();
            return;
        }
        f->mJobId = reply["job"]["id"];
        f->onJob(reply["job"]);
        if (f->mLaying) f->startStatusPoll();
    });
    updateControls();
}

void WolfFloaterTrack::startStatusPoll()
{
    if (mPolling || !mLaying || !mJobId.isDefined()) return;
    mPolling = true;
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    LLCoros::instance().launch("WolfTrackStatus", [h, gen]() { statusCoro(h, gen); });
}

// static
void WolfFloaterTrack::statusCoro(LLHandle<LLFloater> h, U32 gen)
{
    auto self = [&h, gen]() -> WolfFloaterTrack*
    {
        WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
        return f && f->mGen == gen ? f : nullptr;
    };
    // only while a job is queued or running and the window is open (paul-think-of-scale: no polling while idle)
    S32 failures = 0;
    constexpr S32 MAX_FAILURES = 5;   // CHOSEN: five answers in a row that are not the job's (10 s): the window stops waiting
    while (true)
    {
        llcoro::suspendUntilTimeout(STATUS_EVERY_S);
        WolfFloaterTrack* f = self();
        if (!f) return;
        if (!f->mLaying || !f->getVisible()) { f->mPolling = false; return; }
        const LLSD job = f->mJobId;
        LLSD reply;
        std::string error;
        const bool ok = callNow("status", LLSD().with("job", job), reply, error);
        f = self();
        if (!f) return;
        if (!ok)
        {
            if (++failures >= MAX_FAILURES)
            {
                f->mLaying = false;
                f->mPolling = false;
                f->mJobId = LLSD();
                f->setStatus("This window has stopped waiting for your railway: " + error + " It may still be made - open Your "
                             "railways to see, and lay it from there.", true);
                f->updateControls();
                return;
            }
            f->setStatus("The railway service did not answer about your railway (it is still being made): " + error, true);
            continue;
        }
        failures = 0;
        f->onJob(reply["job"]);
        if (!f->mLaying) { f->mPolling = false; return; }
    }
}

void WolfFloaterTrack::onJob(const LLSD& job)
{
    const std::string state = job["state"].asString();
    if (state == "queued" || state == "running")
    {
        mLaying = true;
        const std::string progress = job["progress"].asString();
        setStatus("Laying your track: " + (progress.empty() ? std::string("waiting for its turn") : progress), false);
        // DESIGN.md 1.4: "making the pieces (1 of 3)": the bar follows the step
        const size_t open = progress.rfind('(');
        S32 a = 0, b = 0;
        if (open != std::string::npos && sscanf(progress.c_str() + open, "(%d of %d)", &a, &b) == 2 && b > 0)
        {
            mProgress->setValue(100.f * (F32)llclamp(a - 1, 0, b) / (F32)b);
        }
        updateControls();
        return;
    }
    mLaying = false;
    if (state == "done")
    {
        const LLSD& result = job["result"];
        mItems = readItems(result["items"]);
        mItemsFromDrawing = true;
        mLayLayout = result["layout"];
        for (const Item& it : mItems) if (!it.mRegionName.empty()) mRegionNames[it.mRegion] = it.mRegionName;
        mProgress->setValue(100.f * 2.f / 3.f);
        LL_INFOS("WolfTrack") << "job done: " << mItems.size() << " part(s), layout " << mLayLayout << LL_ENDL;
        if (mItems.empty())
        {
            setStatus("Your railway was made, but the service sent back nothing to put on the land. Please report it.", true);
        }
        else
        {
            layHere();
        }
        updateControls();
        return;
    }
    const std::string error = job["error"].asString();
    setStatus("Your railway could not be made: " + (error.empty() ? std::string("the railway service did not say why.") : error), true);
    updateControls();
}

std::string WolfFloaterTrack::regionName(const LLUUID& id) const
{
    auto it = mRegionNames.find(id);
    if (it != mRegionNames.end() && !it->second.empty()) return it->second;
    if (LLViewerRegion* r = LLWorld::getInstance()->getRegionFromID(id)) return r->getName();
    return "the next region";
}

void WolfFloaterTrack::partsMessage(const std::string& lead)
{
    const size_t n = mItems.size();
    std::string t = lead;
    for (size_t i = 0; i < n; ++i)
    {
        if (mItems[i].mLaid) continue;
        if (!t.empty()) t += " ";
        t += llformat("Part %d of %d is for ", (S32)i + 1, (S32)n) + regionName(mItems[i].mRegion)
           + ": walk or fly into it and press Lay the next part. Lay the part in " + regionName(mItems[i].mRegion)
           + " before you run trains over the border.";
    }
    setStatus(t, false);
}

void WolfFloaterTrack::layHere()
{
    if (busy()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    for (const Item& it : mItems)
    {
        if (it.mLaid || it.mRegion != rgn->getRegionID()) continue;
        mRezzing = true;
        mPartialShown = false;
        mPartial = Item();   // rezCoro sets it when the rez is sent
        mMatched.clear();
        LLHandle<LLFloater> h = getHandle();
        const U32 gen = mGen;
        const LLSD layout = mLayLayout;
        const Item item = it;
        LLCoros::instance().launch("WolfTrackRez", [h, gen, item, layout]() { rezCoro(h, gen, item, layout, true); });
        updateControls();
        return;
    }
    partsMessage(std::string());
}

void WolfFloaterTrack::onRezAlert(const std::string& message)
{
    if (!mRezzing) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != mRezRegion) return;
    mRezAlert = message;
    LL_INFOS("WolfTrack") << "the region refused the rez: " << message << LL_ENDL;
}

std::set<LLUUID> WolfFloaterTrack::laidByOthers() const
{
    // the railway being replaced, and every other railway of this resident here (DESIGN.md 10 "open" LAYOUT laid)
    std::set<LLUUID> out(mReplaceLaid.begin(), mReplaceLaid.end());
    for (const LLSD& l : llsd::inArray(arr(mMine)))
    {
        if (mLayLayout.isDefined() && l["id"].asString() == mLayLayout.asString()) continue;
        const LLSD& laid = l["laid"];
        if (!laid.isMap()) continue;
        for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
            for (const LLSD& u : llsd::inArray(arr(it->second))) out.insert(u.asUUID());
    }
    return out;
}

size_t WolfFloaterTrack::piecesNear(const Item& item) const
{
    // names are not known yet at this point (they are asked for one by one: wolfobjectprops.h), so any object of this agent's
    // whose root is within MATCH_M of one of the part's roots counts
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != item.mRegion) return 0;
    std::map<std::pair<S32, S32>, std::vector<size_t>> cells;
    for (size_t i = 0; i < item.mObjects.size(); ++i)
    {
        const LLVector3& r = item.mObjects[i].mRoot;
        cells[{ (S32)floorf(r.mV[VX]), (S32)floorf(r.mV[VY]) }].push_back(i);
    }
    size_t n_near = 0;
    for (S32 n = 0, count = gObjectList.getNumObjects(); n < count; ++n)
    {
        LLViewerObject* o = gObjectList.getObject(n);
        if (!o || o->isDead() || !o->isRoot() || o->getRegion() != rgn || !o->permYouOwner() || mNotOurs.count(o->getID())) continue;
        const LLVector3 p = o->getPositionRegion();
        const S32 cx = (S32)floorf(p.mV[VX]), cy = (S32)floorf(p.mV[VY]);
        bool hit = false;
        for (S32 dy = -1; dy <= 1 && !hit; ++dy)
            for (S32 dx = -1; dx <= 1 && !hit; ++dx)
            {
                auto c = cells.find({ cx + dx, cy + dy });
                if (c == cells.end()) continue;
                for (size_t i : c->second) if (dist_vec(item.mObjects[i].mRoot, p) <= MATCH_M) { hit = true; break; }
            }
        if (hit) ++n_near;
    }
    return n_near;
}

// by_position: also take a piece whose name has not come (WolfObjectProps asks at most MAX_TRIES times and then treats
// silence as gone - live 14 Nov 14:45Z: after a 159-piece rez beside the 160 pieces of the railway it replaced, 112 names
// never came and the window stopped at "96 of 159"). Every object here is new since the rez (mBefore) and yours, so a root
// within MATCH_M of an expected root is that piece; two expected pieces that share a root share a name (a guide pair).
size_t WolfFloaterTrack::matchPieces(const Item& item, std::vector<std::pair<std::string, LLUUID>>& matched, bool by_position) const
{
    matched.clear();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != item.mRegion) return 0;
    // the expected roots by 1 m cell, so each object in view is checked against the few near it
    std::map<std::pair<S32, S32>, std::vector<size_t>> cells;
    for (size_t i = 0; i < item.mObjects.size(); ++i)
    {
        const LLVector3& r = item.mObjects[i].mRoot;
        cells[{ (S32)floorf(r.mV[VX]), (S32)floorf(r.mV[VY]) }].push_back(i);
    }
    std::vector<bool> taken(item.mObjects.size(), false);
    WolfObjectProps& props = WolfObjectProps::instance();
    for (S32 n = 0, count = gObjectList.getNumObjects(); n < count; ++n)
    {
        LLViewerObject* o = gObjectList.getObject(n);
        if (!o || o->isDead() || !o->isRoot() || o->getRegion() != rgn || !o->permYouOwner()) continue;
        if (mBefore.count(o->getID()) || mNotOurs.count(o->getID())) continue;
        const LLVector3 p = o->getPositionRegion();
        const S32 cx = (S32)floorf(p.mV[VX]), cy = (S32)floorf(p.mV[VY]);
        bool near_any = false;
        const WolfObjectProps::Props* known = nullptr;
        for (S32 dy = -1; dy <= 1; ++dy)
        {
            for (S32 dx = -1; dx <= 1; ++dx)
            {
                auto c = cells.find({ cx + dx, cy + dy });
                if (c == cells.end()) continue;
                for (size_t i : c->second)
                {
                    if (taken[i] || dist_vec(item.mObjects[i].mRoot, p) > MATCH_M) continue;
                    near_any = true;
                    // the name is not in ObjectUpdate (wolfobjectprops.h): it is asked for once, through the shared budget
                    if (!known) known = props.get(o->getID());
                    if (!known && !by_position) break;
                    if (known && known->mName != item.mObjects[i].mName) continue;
                    // a multiset: two guides of one place (one each way) share root and name; each takes one object
                    taken[i] = true;
                    matched.emplace_back(item.mObjects[i].mName, o->getID());
                    near_any = false;
                    dx = dy = 2;   // this object is used: on to the next
                    break;
                }
            }
        }
        if (near_any && !known) props.want(o);
    }
    return matched.size();
}

// static
void WolfFloaterTrack::rezCoro(LLHandle<LLFloater> h, U32 gen, Item item, LLSD layout, bool rez)
{
    auto self = [&h, gen]() -> WolfFloaterTrack*
    {
        WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
        return f && f->mGen == gen ? f : nullptr;
    };
    WolfFloaterTrack* f = self();
    if (!f) return;
    auto fail = [&](const std::string& why)
    {
        f->mRezzing = false;
        f->mProgress->setVisible(false);
        f->setStatus(why, true);
        f->updateControls();
    };
    const std::string where = f->regionName(item.mRegion);
    if (rez)
    {
        f->setStatus("Laying your track: placing it on your land (3 of 3)...", false);
        // Source: wolfcave.cpp:938-954 WolfCaves::rezExact - the service made the item through Robust (no
        // BulkUpdateInventory from the region), so its folder is fetched until the item shows, finished.
        // [live test 14 Nov 13:14Z] The item's folder is NEW (deliver.py:76-78 makes it under Objects through Robust), and a
        // folder the inventory model has never seen cannot be fetched: fetchDescendentsOf returns at once with "Asked to fetch
        // descendents of non-existent folder" (llinventorymodel.cpp:2500-2512) and getCategory is null. So, as the Lifts window
        // waits for a kit folder the grid has just made (wolffloaterlifts.cpp:991-1006), its parent - Objects
        // (findCategoryUUIDForType FT_OBJECT, wolffloaterlifts.cpp:162) - is asked for again until the new folder appears:
        // version set unknown, then LLViewerInventoryCategory::fetch (llviewerinventory.cpp:741-762: it asks only when the
        // version is unknown and its timer has run out, so the timer is set to FOLDER_REASK_S; it starts
        // LLInventoryModelBackgroundFetch::start(id, false), llinventorymodelbackgroundfetch.cpp:287-298, the folder and its
        // direct contents - the new folder comes with them). Then the new folder is fetched the same way until the item shows.
        LLViewerInventoryItem* inv = nullptr;
        const LLUUID objects = gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT);
        if (!gInventory.getCategory(item.mFolder) && (objects.isNull() || !gInventory.getCategory(objects)))
        {
            // nothing to ask for: the railway's folder is new and the inventory has no Objects folder loaded to find it in
            LL_WARNS("WolfTrack") << "the railway's folder " << item.mFolder << " is not known and Objects (" << objects
                                  << ") is not in the inventory model: cannot fetch it" << LL_ENDL;
            fail("Your railway was made and is in your inventory (Objects), but this viewer has not loaded your Objects folder yet. "
                 "Open your inventory, wait for it to finish loading, then press Lay the next part.");
            return;
        }
        bool folder_logged = false;
        for (S32 i = 0; i < FETCH_TRIES && !inv; ++i)
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(item.mFolder);
            LLViewerInventoryCategory* parent = cat ? nullptr : gInventory.getCategory(objects);
            if (!cat && parent && i % FOLDER_REASK_S == 0)
            {
                parent->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
                parent->fetch(FOLDER_REASK_S);
            }
            if (cat)
            {
                if (!folder_logged) { LL_INFOS("WolfTrack") << "the railway's folder " << item.mFolder << " is known after " << i << " s" << LL_ENDL; folder_logged = true; }
                if (i % FOLDER_REASK_S == 0) cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
                cat->fetch(FOLDER_REASK_S);
            }
            llcoro::suspendUntilTimeout(1.f);
            if (!self()) return;
            inv = gInventory.getItem(item.mItem);
            if (inv && !inv->isFinished()) inv = nullptr;
        }
        f = self();
        if (!f) return;
        if (!inv)
        {
            fail("Your railway was made and is in your inventory (Objects), but this viewer could not see it yet. Press Lay the "
                 "next part to try again.");
            return;
        }
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != item.mRegion)
        {
            fail("This part is for " + where + ", and you are not there now. Go there and press Lay the next part.");
            return;
        }
        // never a second copy: pieces of this part that already stand (an earlier rez whose window was closed, or a viewer
        // restarted since) are looked for first - any one there and the part is only looked for, not rezzed again. Pieces of
        // the resident's other railways here (Your railways' laid lists) and of the one being replaced never count.
        f->mNotOurs = f->laidByOthers();
        f->mBefore.clear();
        const size_t standing = f->piecesNear(item);
        if (standing > 0)
        {
            LL_INFOS("WolfTrack") << standing << " objects of this agent already stand at this part's roots: looking, not rezzing" << LL_ENDL;
            f->setStatus("Some pieces of this part are on the land already - looking for the rest.", false);
        }
        else
        {
            for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
            {
                LLViewerObject* o = gObjectList.getObject(i);
                if (o && !o->isDead() && o->isRoot()) f->mBefore.insert(o->getID());
            }
            f->mRezAlert.clear();
            f->mRezRegion = item.mRegion;
            LL_INFOS("WolfTrack") << "rez " << item.mItem << " in " << where << " at " << item.mRayEnd << " (" << item.mObjects.size()
                                  << " pieces)" << LL_ENDL;
            rezAt(inv, item.mRayEnd);
        }
    }
    f->mPartial = item;
    // look for the pieces as they stream in: up to LOOK_S, every half second
    std::vector<std::pair<std::string, LLUUID>> matched;
    const size_t want = item.mObjects.size();
    for (S32 t = 0; t < LOOK_S * 2; ++t)
    {
        llcoro::suspendUntilTimeout(0.5f);
        f = self();
        if (!f) return;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn || rgn->getRegionID() != item.mRegion)
        {
            f->mPartialShown = true;
            fail("You left " + where + " before all the pieces of your railway were found. Go back there and press Look again.");
            return;
        }
        // names first; after NAME_WAIT_S of this look, a piece whose name never came is taken by its place alone
        const size_t got = f->matchPieces(item, matched, t >= (S32)(NAME_WAIT_S * 2.f));
        if (got >= want) break;
        if (!f->mRezAlert.empty() && got == 0)
        {
            // DESIGN.md 5.3: one refusal and nothing is placed (InventoryAccessModule.cs:1176-1198); the item stays in Objects
            const std::string said = f->mRezAlert;
            f->mRezAlert.clear();
            fail("Your track crosses land where you cannot build. Draw it on your own land, or ask the land's owner.\n"
                 "The region said: " + said);
            return;
        }
        f->mProgress->setVisible(true);
        f->mProgress->setValue(66.7f + 33.3f * (F32)got / (F32)llmax((size_t)1, want));
        f->setStatus(llformat("Laying your track: placing it on your land (3 of 3) - %d of %d pieces in place.", (S32)got, (S32)want), false);
    }
    if (matched.size() < want)
    {
        f->mMatched = matched;
        f->mPartialShown = true;
        // DESIGN.md 5.3: OpenSim can refuse the whole rez with no alert at all (the first piece's parcel, or a prim limit
        // counting every piece against it: InventoryAccessModule.cs:1017-1029)
        fail(matched.empty()
            ? std::string("Nothing was placed. The region did not let your railway in - check that you may build on this land and "
                          "that the parcel has room for more objects. If you think it did place it, press Look again (a slow "
                          "connection can take longer).")
            : llformat("Only %d of %d pieces appeared. Press Look again to keep looking (a slow connection can take longer), or "
                       "Take away what was placed.", (S32)matched.size(), (S32)want));
        return;
    }
    // every piece is here: the record is told exactly which objects they are (DESIGN.md 10 "saved")
    LLSD objects = LLSD::emptyArray();
    for (const auto& m : matched) objects.append(LLSD().with("name", m.first).with("uuid", m.second.asString()));
    LLSD body;
    body["layout"] = layout;
    body["region"] = item.mRegion.asString();
    body["objects"] = objects;
    f->mMatched = matched;
    f->setStatus("Laying your track: saving which pieces are yours...", false);
    LLSD reply;
    std::string error;
    const bool ok = callNow("saved", body, reply, error);
    f = self();
    if (!f) return;
    if (!ok)
    {
        f->mPartialShown = true;
        if (reply.isMap() && reply["code"].asString() == "already_laid")
        {
            // DESIGN.md 10: this part is laid already (by an earlier window): it is never rezzed or saved twice
            f->mPartialShown = false;
            for (Item& it : f->mItems) if (it.mRegion == item.mRegion && it.mItem == item.mItem) it.mLaid = true;
            fail("This part of your railway is laid already - it is there.");
            return;
        }
        if (reply.isMap() && reply["code"].asString() == "incomplete")
        {
            const S32 missing = (S32)arr(reply["missing"]).size();
            fail(llformat("Only %d of %d pieces were found. Press Look again, or Take away what was placed.", (S32)want - missing, (S32)want));
            return;
        }
        fail("The region let your railway in, but its pieces could not be saved as yours: " + error + " Press Look again to try "
             "saving them again.");
        return;
    }
    // [TUNNELS] DESIGN.md 3.4 / 10: the part's tunnels opened (the region's boxes) and recorded (the Tunnel tool's record)
    std::string tunnel_words;
    if (item.mTunnels.size() > 0)
    {
        const auto uuid_of = [&matched](const std::string& name) -> LLUUID
        {
            for (const auto& m : matched) if (m.first == name) return m.second;
            return LLUUID::null;
        };
        const std::string why = finishTunnels(layout, item, uuid_of, [&](const std::string& t) { if (WolfFloaterTrack* g = self()) g->setStatus(t, false); });
        f = self();
        if (!f) return;
        tunnel_words = why.empty() ? " " + plural((S32)item.mTunnels.size(), "tunnel was", "tunnels were") + " opened through the hill."
                                   : " " + why + " Your railway is laid; the tunnel stays closed until it is opened: pick the railway in "
                                     "Your railways and press Open the tunnel.";
    }
    f->mRezzing = false;
    f->mPartialShown = false;
    for (Item& it : f->mItems) if (it.mRegion == item.mRegion && it.mItem == item.mItem) it.mLaid = true;
    LL_INFOS("WolfTrack") << "saved " << matched.size() << " pieces in " << where << LL_ENDL;
    f->mLaidThisSession.insert(layout.asString());   // every piece of it is in view this session: Remove starts nearby
    f->afterSaved(reply["layout"]);
    if (!tunnel_words.empty()) f->setStatus(f->mStatus->getText() + tunnel_words, tunnel_words.find("stays closed") != std::string::npos);
    // [CUTTINGS] DESIGN.md 3.6 contract: "saved answers also cut_job: JOB | null (the dig, queued at once for that region's
    // cuttings)" - followed in the window until the cutting is dug (an error said above stays in front of it)
    const LLSD cut_job = reply["cut_job"];
    if (cut_job.isMap()) f->startGroundJob(cut_job, "cut", layout, item.mRegion, f->mStatusError ? f->mStatus->getText() : std::string());
}

void WolfFloaterTrack::lookAgain()
{
    if (busy() || mPartial.mItem.isNull()) return;
    mRezzing = true;
    mPartialShown = false;
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    const Item item = mPartial;
    const LLSD layout = mLayLayout;
    LLCoros::instance().launch("WolfTrackLook", [h, gen, item, layout]() { rezCoro(h, gen, item, layout, false); });
    updateControls();
}

void WolfFloaterTrack::takeAwayPlaced()
{
    if (busy() || mPartial.mItem.isNull()) return;
    std::vector<LLUUID> ids;
    for (const auto& m : mMatched) ids.push_back(m.second);
    if (ids.empty())
    {
        mPartialShown = false;
        setStatus("No pieces appeared, so there is nothing to take away. Your railway is in your inventory (Objects).", false);
        updateControls();
        return;
    }
    takeAwayPart(mPartial.mRegion, ids);
}

void WolfFloaterTrack::afterSaved(const LLSD& layout)
{
    // DESIGN.md 10 LAYOUT: laid {region uuid: [uuid]} - every part already laid (also by an earlier window)
    if (layout.isMap() && layout["laid"].isMap())
    {
        for (Item& it : mItems) if (layout["laid"].has(it.mRegion.asString())) it.mLaid = true;
    }
    mProgress->setVisible(false);
    bool all = true;
    for (const Item& it : mItems) all = all && it.mLaid;
    const std::string state = layout["state"].asString();
    if ((all || state == "laid") && !mItemsFromDrawing)
    {
        setStatus("That railway is laid in full now.", false);
    }
    else if (all || state == "laid")
    {
        mItemsFromDrawing = false;
        // the switch-on note goes with every word about this lay (the old railway's removal says more later)
        mLaidNote = mTurntables.empty() ? std::string()
            : std::string(" The turntable switches itself on when it is laid. If the region cannot switch it on yet, its deck tells you "
                          "why - touch the deck and choose Switch on to try again.");
        mLines.clear();
        mStations.clear();
        mSignals.clear();
        mTurntables.clear();
        mTunnels.clear();
        mCurTunnel = -1;
        mCuttings.clear();
        mCurCutting = -1;
        mCurLine = mCurStation = mEditStation = mEditTurntable = -1;
        mPlan.clear();
        mPlannedReq = LLSD();
        const LLSD replaces = layout["replaces"];
        mReplaces = LLSD();
        mReplaceName.clear();
        changed();
        setStatus("Your railway is laid! Rez a train on it from Build a Train..." + mLaidNote, false);
        // DESIGN.md 10: the old railway of a Change goes only now that the new one is laid everywhere
        if (replaces.isDefined() && !(replaces.isString() && replaces.asString().empty())) replaceOld(replaces);
        else mReplaceLaid.clear();
    }
    else
    {
        S32 laid = 0;
        for (const Item& it : mItems) laid += it.mLaid ? 1 : 0;
        // DESIGN.md 5.3: "Part 1 of 2 is laid in Badger Vale. Walk or fly into Otter Brook and press Lay the next part."
        LLViewerRegion* rgn = gAgent.getRegion();
        partsMessage(llformat("Part %d of %d is laid", laid, (S32)mItems.size()) + (rgn ? " in " + rgn->getName() + "." : std::string(".")));
    }
    if (mMineCheck->get()) refreshMine();
    updateControls();
}

void WolfFloaterTrack::replaceOld(const LLSD& old_id)
{
    LL_INFOS("WolfTrack") << "the new railway is laid: taking the old one (" << old_id << ") away" << LL_ENDL;
    post("remove", LLSD().with("layout", old_id), [old_id](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok)
        {
            f->setStatus("Your changed railway is laid, but the old one could not be taken away: " + error
                         + " Open Your railways and press Remove on it.", true);
            return;
        }
        LLViewerRegion* rgn = gAgent.getRegion();
        const LLSD laid = reply["layout"]["laid"];
        std::vector<LLUUID> here;
        if (rgn) for (const LLSD& u : llsd::inArray(arr(laid[rgn->getRegionID().asString()]))) here.push_back(u.asUUID());
        if (rgn && !here.empty())
        {
            f->startRemoval(old_id, rgn->getRegionID(), true, true);   // nearby first, the server for what is out of reach
            return;
        }
        bool elsewhere = false;
        if (laid.isMap())
        {
            for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it) elsewhere = elsewhere || arr(it->second).size() > 0;
        }
        if (!elsewhere && rgn)
        {
            f->sendRemoved(old_id, rgn->getRegionID(), true);   // nothing of it stands anywhere: only the record goes
            return;
        }
        std::string t = "Your changed railway is laid.";
        if (laid.isMap())
        {
            for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
            {
                if (arr(it->second).size() == 0) continue;
                t += " The part of your old railway in " + f->regionName(LLUUID(it->first))
                   + " is still there. Go there, open Your railways and press Remove on it.";
            }
        }
        f->setStatus(t + f->mLaidNote, false);
    });
}

// ---- removing (DESIGN.md 10 remove / removed) -----------------------------------------------------------------------------------

// The pieces of `uuids` that this viewer has in view in `region` (the agent's) go to the Trash, as the viewer's Delete does
// (deRezToTrash), and are waited for (GONE_WAIT_S). false: the window went meanwhile.
// static
bool WolfFloaterTrack::nearbyStep(const std::function<WolfFloaterTrack*()>& self, const LLUUID& region, const std::vector<LLUUID>& uuids,
                                  NearResult& out)
{
    out = NearResult();
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != region) { out.out_of_reach = uuids.size(); return self() != nullptr; }
    std::vector<LLViewerObject*> objs;
    std::vector<LLUUID> sent;
    for (const LLUUID& id : uuids)
    {
        LLViewerObject* o = gObjectList.findObject(id);
        if (o && !o->isDead() && o->getRegion() == rgn) { objs.push_back(o); sent.push_back(id); }
        else ++out.out_of_reach;
    }
    if (sent.empty()) return self() != nullptr;
    if (WolfFloaterTrack* f = self()) f->setStatus(llformat("Taking %s away (they go to your Trash)...", plural((S32)sent.size(), "piece", "pieces").c_str()), false);
    LL_INFOS("WolfTrack") << "delete " << sent.size() << " pieces in view (" << out.out_of_reach << " out of reach) in " << rgn->getName() << LL_ENDL;
    deRezToTrash(objs);
    objs.clear();   // the pointers are not kept over a wait
    size_t standing = sent.size();
    for (S32 t = 0; t < GONE_WAIT_S * 2 && standing > 0; ++t)
    {
        llcoro::suspendUntilTimeout(0.5f);
        if (!self()) return false;
        standing = 0;
        for (const LLUUID& id : sent)
        {
            LLViewerObject* o = gObjectList.findObject(id);
            if (o && !o->isDead()) ++standing;
        }
    }
    for (const LLUUID& id : sent)
    {
        LLViewerObject* o = gObjectList.findObject(id);
        if (!o || o->isDead()) out.gone.push_back(id);
    }
    out.taken = out.gone.size();
    out.stayed = sent.size() - out.taken;
    return self() != nullptr;
}

// Take away what was placed: only the pieces that appeared, from nearby; the job's item stays in Objects, nothing is recorded.
void WolfFloaterTrack::takeAwayPart(const LLUUID& region, const std::vector<LLUUID>& uuids)
{
    if (busy())
    {
        setStatus("The window is busy with your railway just now - wait until it has finished, then try again.", true);
        return;
    }
    mRemoving = true;
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    LLCoros::instance().launch("WolfTrackTakeAway", [h, gen, region, uuids]()
    {
        auto self = [&h, gen]() -> WolfFloaterTrack*
        {
            WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
            return f && f->mGen == gen ? f : nullptr;
        };
        NearResult step;
        if (!nearbyStep(self, region, uuids, step)) return;
        WolfFloaterTrack* f = self();
        if (!f) return;
        f->mRemoving = false;
        f->mPartialShown = false;
        f->mMatched.clear();
        const size_t left = step.stayed + step.out_of_reach;
        f->setStatus(left > 0
            ? plural((S32)left, "piece", "pieces") + " could not be taken away - delete them by hand (they are yours)."
            : std::string("The pieces that appeared were taken away. Your railway is still in your inventory (Objects): press Lay "
                          "the next part to try again."), left > 0);
        f->updateControls();
    });
    updateControls();
}

void WolfFloaterTrack::sendRemoved(const LLSD& layout, const LLUUID& region, bool replacing)
{
    LLSD body;
    body["layout"] = layout;
    body["region"] = region.asString();
    post("removed", body, [layout, region, replacing](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok)
        {
            f->setStatus("The pieces were taken away, but the record could not be updated: " + error + " Press Remove again.", true);
            return;
        }
        std::string t = replacing ? "Your changed railway is laid, and the old one was taken away here." : "The railway was taken away here.";
        const LLSD laid = reply["layout"]["laid"];
        if (laid.isMap())
        {
            for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
            {
                if (arr(it->second).size() == 0 || LLUUID(it->first) == region) continue;
                t += replacing ? " The part of your old railway in " + f->regionName(LLUUID(it->first)) + " is still there. Go there, "
                                 "open Your railways and press Remove on it."
                               : " The part in " + f->regionName(LLUUID(it->first)) + " is still there. Go there and press Remove again.";
            }
        }
        if (replacing)
        {
            f->mReplaceLaid.clear();
            t += f->mLaidNote;
        }
        f->mShown.clear();
        f->setStatus(t, false);
        f->refreshMine();
        // [CUTTINGS] DESIGN.md 3.6 contract: "removed answers fill_job: JOB | null" - the ground put back as it was
        if (reply["fill_job"].isMap()) f->startGroundJob(reply["fill_job"], "fill", layout, region, std::string());
    });
}

// ---- your railways (DESIGN.md 1.5, 10 open / get) -------------------------------------------------------------------------------

void WolfFloaterTrack::onMineCheck()
{
    relayout();
    // [T961 2026-11-21] opened in a window cut short by a small screen: scroll the panel into view (track_content's coordinates)
    if (mMineCheck->get() && mScroll && mMinePanel) mScroll->scrollToShowRect(mMinePanel->getRect());
    if (mMineCheck->get()) refreshMine();
}

void WolfFloaterTrack::refreshMine()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !onWolf()) return;
    const LLUUID region = rgn->getRegionID();
    mMineRegion = region;
    post("open", LLSD().with("region", region.asString()), [region](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (f->mMineRegion != region) return;   // the agent moved on meanwhile: draw() asks for the new region
        if (!ok)
        {
            f->mMine = LLSD::emptyArray();
            f->fillMineList();
            if (reply.isMap() && reply["code"].asString() == "not_member")
            {
                f->mMayLay = false;
                f->mNoLayReason = error;
            }
            f->setStatus("Your railways here could not be listed: " + error, true);
            return;
        }
        f->mMine = arr(reply["layouts"]);
        const LLSD& access = reply["access"];
        f->mMayLay = !access.isMap() || !access.has("lay") || access["lay"].asBoolean();
        f->mNoLayReason = access.isMap() ? access["reason"].asString() : std::string();
        f->mMayTunnel = access.isMap() && access["tunnel"].asBoolean();
        f->mNoTunnelReason = access.isMap() ? access["tunnel_reason"].asString() : std::string();
        f->mMayCut = access.isMap() && access["cut"].asBoolean();   // [CUTTINGS] DESIGN.md 3.6 contract "open"
        f->mNoCutReason = access.isMap() ? access["cut_reason"].asString() : std::string();
        f->fillMineList();
        f->updateControls();
    });
}

void WolfFloaterTrack::fillMineList()
{
    mMineList->deleteAllItems();
    for (LLSD::Integer i = 0; i < (LLSD::Integer)mMine.size(); ++i)
    {
        const LLSD& l = mMine[i];
        if (l["state"].asString() == "removed")
        {
            // [CUTTINGS] a removed railway stays listed while its cutting here is not filled in yet (DESIGN.md 3.6 LAYOUT
            // cuttings: waiting | filling | failed), so the ground can still be put back from here
            const LLSD& cut = l["cuttings"][mMineRegion.asString()];
            if (!cut.isMap() || cut["state"].asString() == "filled") continue;
        }
        LLSD e;
        e["value"] = i;
        e["columns"][0]["column"] = "name";
        e["columns"][0]["value"] = l["name"].asString().empty() ? std::string("Railway") : l["name"].asString();
        e["columns"][1]["column"] = "state";
        e["columns"][1]["value"] = stateWords(l["state"].asString());
        e["columns"][2]["column"] = "date";
        e["columns"][2]["value"] = dateWords(l["created"]);
        mMineList->addElement(e);
    }
    if (mMineList->getItemCount() == 0)
    {
        LLSD e;
        e["value"] = -1;
        e["columns"][0]["column"] = "name";
        e["columns"][0]["value"] = "No railways of yours in this region yet.";
        mMineList->addElement(e);
    }
}

LLSD WolfFloaterTrack::pickedLayout() const
{
    const LLScrollListItem* it = mMineList ? mMineList->getFirstSelected() : nullptr;
    if (!it) return LLSD();
    const S32 i = it->getValue().asInteger();
    if (i < 0 || i >= (S32)mMine.size()) return LLSD();
    return mMine[i];
}

void WolfFloaterTrack::loadDrawing(const LLSD& req)
{
    // DESIGN.md 10 REQ: lines, stations, signals, turntables (a stored request has no ground or others: the server keeps only
    // the drawing, track_tool_lib.php tt_lay_insert, so they are always gathered again by buildRequest)
    mLines.clear();
    mStations.clear();
    mSignals.clear();
    mTurntables.clear();
    mTunnels.clear();
    mCurTunnel = -1;
    mCuttings.clear();
    mCurCutting = -1;
    mEditTurntable = -1;
    mCurLine = mCurStation = mEditStation = -1;
    auto gp = [](const LLSD& v) { GP p; readXY(v, p.x, p.y); return p; };
    for (const LLSD& e : llsd::inArray(arr(req["tunnels"])))
    {
        Tunnel t;
        t.mA = gp(e["a"]);
        t.mB = gp(e["b"]);
        t.mHasB = true;
        t.mStamp = nextStamp();
        mTunnels.push_back(t);
    }
    for (const LLSD& e : llsd::inArray(arr(req["cuttings"])))   // [CUTTINGS] DESIGN.md 3.6 contract REQ cuttings
    {
        Cutting c;
        c.mA = gp(e["a"]);
        c.mB = gp(e["b"]);
        c.mHasB = true;
        c.mStamp = nextStamp();
        mCuttings.push_back(c);
    }
    for (const LLSD& e : llsd::inArray(arr(req["lines"])))
    {
        Line ln;
        ln.mBranch = e["kind"].asString() == "branch";
        if (ln.mBranch) ln.mStart = gp(e["start"]);
        for (const LLSD& c : llsd::inArray(arr(e["clicks"]))) ln.mClicks.push_back(gp(c));
        ln.mClosed = e["closed"].asBoolean();
        ln.mStamp = nextStamp();
        mLines.push_back(ln);
    }
    for (const LLSD& e : llsd::inArray(arr(req["stations"])))
    {
        Station s;
        s.mA = gp(e["a"]);
        s.mB = gp(e["b"]);
        s.mHasB = true;
        s.mSide = e["side"].asString();
        s.mStyle = e["style"].asString();
        s.mName = e["name"].asString();
        s.mStamp = nextStamp();
        mStations.push_back(s);
    }
    for (const LLSD& e : llsd::inArray(arr(req["signals"]))) { Signal s; s.mAt = gp(e); s.mStamp = nextStamp(); mSignals.push_back(s); }
    for (const LLSD& e : llsd::inArray(arr(req["turntables"])))
    {
        Turntable t;
        t.mAt = gp(e["at"]);
        t.mSize = e["size"].asInteger();
        t.mRoads = e["roads"].asInteger();
        t.mSpread = e.has("spread") ? ll_round((F32)e["spread"].asReal()) : t.mSpread;
        t.mShed = e["shed"].asBoolean();
        t.mStamp = nextStamp();
        mTurntables.push_back(t);
    }
}

bool WolfFloaterTrack::requestFor(const LLSD& stored, const std::vector<LLUUID>& own_laid, LLSD& req, std::string& why)
{
    // the drawing in the window is kept as it is: the stored one is put in its place only while its REQ is made
    const std::vector<Line> lines = mLines;
    const std::vector<Station> stations = mStations;
    const std::vector<Signal> signals = mSignals;
    const std::vector<Turntable> turntables = mTurntables;
    const std::vector<Tunnel> tunnels = mTunnels;
    const S32 cur_tunnel = mCurTunnel;
    const std::vector<Cutting> cuttings = mCuttings;
    const S32 cur_cutting = mCurCutting;
    const std::vector<LLUUID> replace_laid = mReplaceLaid;
    const S32 cur_line = mCurLine, cur_station = mCurStation, edit_station = mEditStation, edit_tt = mEditTurntable;
    loadDrawing(stored);
    mReplaceLaid = own_laid;   // its own pieces are not "someone else's railway"
    const bool ok = buildRequest(req, why);
    mLines = lines;
    mStations = stations;
    mSignals = signals;
    mTurntables = turntables;
    mTunnels = tunnels;
    mCurTunnel = cur_tunnel;
    mCuttings = cuttings;
    mCurCutting = cur_cutting;
    mReplaceLaid = replace_laid;
    mCurLine = cur_line;
    mCurStation = cur_station;
    mEditStation = edit_station;
    mEditTurntable = edit_tt;
    return ok;
}

// ---- [TRACK TOOL TUNNELS 2026-11-15] tunnels (DESIGN.md 3.4) ----------------------------------------------------------------

bool WolfFloaterTrack::planLineAtMouse(GP& out) const
{
    // Source: WolfStorm floater_wolf_track_tool.js _planLineAt - the plan's rails (renderRouteExtras draws them at their rail
    // level) projected to the screen as drawRouteExtras projects its labels; the nearest vertex within LINE_PICK_PX
    if (mPlanDirty || mPlan.mRails.v.empty()) return false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 mx = (F32)gViewerWindow->getCurrentMouseX(), my = (F32)gViewerWindow->getCurrentMouseY();
    F32 best = LINE_PICK_PX * LINE_PICK_PX;
    bool found = false;
    for (const LLVector3d& v : mPlan.mRails.v)
    {
        LLCoordGL sp;
        if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(rgn->getPosRegionFromGlobal(v)), sp, false)) continue;
        const F32 dx = (F32)sp.mX - mx, dy = (F32)sp.mY - my;
        if (dx * dx + dy * dy < best)
        {
            best = dx * dx + dy * dy;
            out.x = v.mdV[VX];
            out.y = v.mdV[VY];
            found = true;
        }
    }
    return found;
}

LLSD WolfFloaterTrack::pendingSuggestion() const
{
    // the plan's first suggestion the resident has not answered (Climb the hill is kept here only: climbing is the default);
    // [CUTTINGS] then the hills where only a cutting is the other choice (DESIGN.md 3.6 contract: PLAN cut_suggestions [{line,
    // s0, s1, at, cutting, climb}] - their ends are the cutting's own a, b, put where the posts and the answer look for them)
    if (mPlanDirty) return LLSD();
    auto key = [](const LLSD& g, const char* kind)
    {
        return std::string(kind) + g["line"].asString()
             + llformat(":%d:%d", ll_round((F32)g["s0"].asReal() / 10.f), ll_round((F32)g["s1"].asReal() / 10.f));
    };
    for (const LLSD& g : llsd::inArray(arr(mPlan.mSuggestions)))
    {
        if (!mSugHidden.count(key(g, ""))) return g;
    }
    for (const LLSD& g : llsd::inArray(arr(mPlan.mCutSuggestions)))
    {
        if (mSugHidden.count(key(g, "cut:"))) continue;
        LLSD c = g;
        c["cut_only"] = true;
        c["key"] = key(g, "cut:");
        if (!c.has("a")) c["a"] = g["cutting"]["a"];
        if (!c.has("b")) c["b"] = g["cutting"]["b"];
        return c;
    }
    return LLSD();
}

void WolfFloaterTrack::onSuggestion(ESugChoice choice)
{
    const LLSD g = pendingSuggestion();
    if (!g.isMap() || busy()) return;
    if (choice == SUG_CLIMB)
    {
        mSugHidden.insert(g.has("key") ? g["key"].asString()
                                       : g["line"].asString() + llformat(":%d:%d", ll_round((F32)g["s0"].asReal() / 10.f), ll_round((F32)g["s1"].asReal() / 10.f)));
        setStatus("The line climbs over the hill.", false);
        updateControls();
        return;
    }
    if (choice == SUG_CUTTING)
    {
        // [CUTTINGS] DESIGN.md 3.6 contract: "cutting: {possible, text, a, b, ...} (a, b: the ends to send in REQ.cuttings when
        // chosen)"; access.cut false: the resident may not dig here
        const LLSD& cut = g["cutting"];
        if (!mMayCut) { setStatus(mNoCutReason.empty() ? std::string("You cannot dig cuttings in this region.") : mNoCutReason, true); return; }
        if (!cut.isMap() || !cut["possible"].asBoolean())
        {
            setStatus(cut.isMap() && !cut["text"].asString().empty() ? cut["text"].asString() : std::string("A cutting is not possible here."), true);
            return;
        }
        if (mCuttings.size() >= MAX_CUTTINGS)
        {
            setStatus(llformat("That is the most cuttings one railway can have (%d).", (S32)MAX_CUTTINGS), true);
            return;
        }
        Cutting c;
        if (!readXY(cut["a"], c.mA.x, c.mA.y) || !readXY(cut["b"], c.mB.x, c.mB.y))
        {
            setStatus("The railway service did not say where the cutting would go. Mark it with Cutting instead: click where it "
                      "starts and where it ends on the line.", true);
            return;
        }
        c.mHasB = true;
        const std::string clash = overlapWords(c, true);
        if (!clash.empty()) { setStatus(clash, true); return; }
        c.mStamp = nextStamp();
        mCuttings.push_back(c);
        const bool joined = mergeCuttings(mCuttings.size() - 1);
        setStatus(std::string("The line now runs through the hill in a cutting: the ground there is dug down to the rails when your "
                              "railway is laid, and put back if you remove it. (Undo last point, or Take out in the list, climbs it again.)")
                  + (joined ? " It joins a cutting you marked before." : ""), false);
        changed();
        return;
    }
    if (!mMayTunnel) { setStatus(mNoTunnelReason.empty() ? std::string("You cannot make tunnels in this region.") : mNoTunnelReason, true); return; }
    if (mTunnels.size() >= MAX_TUNNELS)
    {
        setStatus(llformat("That is the most tunnels one railway can have (%d).", (S32)MAX_TUNNELS), true);
        return;
    }
    // DESIGN.md 10 REQ tunnels: the suggestion's own ends (a, b), on its line
    Tunnel t;
    readXY(g["a"], t.mA.x, t.mA.y);
    readXY(g["b"], t.mB.x, t.mB.y);
    t.mHasB = true;
    // [CUTTINGS] DESIGN.md 3.6 contract: a cutting may not overlap a tunnel
    const std::string clash = overlapWords(t, false);
    if (!clash.empty()) { setStatus(clash, true); return; }
    t.mStamp = nextStamp();
    mTunnels.push_back(t);
    setStatus("The line now goes through the hill in a tunnel. (Undo last point, or Take out in the list, climbs it again.)", false);
    changed();
}

// ---- [TRACK TOOL CUTTINGS 2026-11-17] cuttings (DESIGN.md 3.6) ----------------------------------------------------------------

bool WolfFloaterTrack::drawnChainage(const GP& p, S32& line, F64& s) const
{
    // the drawn lines (not the plan's, which may be of an older drawing): the nearest piece of any line, and the length along
    // that line to there. Both ends of a mark are snapped onto the plan's line, which is near the drawn one (tracktool/geom.py
    // fits a curve inside each corner), so the nearest drawn line is the one it is on
    F64 best = 1e300;
    bool found = false;
    for (size_t li = 0; li < mLines.size(); ++li)
    {
        const Line& l = mLines[li];
        std::vector<GP> pts;
        if (l.mBranch) pts.push_back(l.mStart);
        pts.insert(pts.end(), l.mClicks.begin(), l.mClicks.end());
        if (l.mClosed && pts.size() >= 3) pts.push_back(pts.front());
        F64 run = 0.0;
        for (size_t k = 1; k < pts.size(); ++k)
        {
            const GP& a = pts[k - 1];
            const GP& b = pts[k];
            const F64 vx = b.x - a.x, vy = b.y - a.y, len = sqrt(vx * vx + vy * vy);
            const F64 u = len > 1e-9 ? llclamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / (len * len), 0.0, 1.0) : 0.0;
            const F64 qx = a.x + vx * u - p.x, qy = a.y + vy * u - p.y;
            const F64 d = qx * qx + qy * qy;
            if (d < best) { best = d; line = (S32)li; s = run + u * len; found = true; }
            run += len;
        }
    }
    return found;
}

bool WolfFloaterTrack::stretchesOverlap(const Stretch& x, const Stretch& y) const
{
    S32 lxa = -1, lxb = -1, lya = -1, lyb = -1;
    F64 sxa = 0.0, sxb = 0.0, sya = 0.0, syb = 0.0;
    if (!drawnChainage(x.mA, lxa, sxa) || !drawnChainage(x.mB, lxb, sxb) || !drawnChainage(y.mA, lya, sya) || !drawnChainage(y.mB, lyb, syb))
        return false;
    if (lxa != lxb || lya != lyb || lxa != lya) return false;   // not on one line
    const F64 x0 = llmin(sxa, sxb), x1 = llmax(sxa, sxb), y0 = llmin(sya, syb), y1 = llmax(sya, syb);
    constexpr F64 POINT_M = 1e-3;
    // a stretch of no length (a first click) overlaps when it lies inside the other; two stretches when they share some line.
    // Ends that only meet (a cutting up to a tunnel's portal) do not overlap
    if (x1 - x0 < POINT_M) return x0 > y0 && x0 < y1;
    if (y1 - y0 < POINT_M) return y0 > x0 && y0 < x1;
    return llmin(x1, y1) - llmax(x0, y0) > POINT_M;
}

std::string WolfFloaterTrack::overlapWords(const Stretch& s, bool is_cutting) const
{
    // DESIGN.md 3.6 contract: "a cutting may not overlap a tunnel" (the planner refuses it; the window says so first)
    const std::vector<Stretch>& others = is_cutting ? mTunnels : mCuttings;
    for (size_t i = 0; i < others.size(); ++i)
    {
        if (!others[i].mHasB || !stretchesOverlap(s, others[i])) continue;
        return is_cutting
            ? llformat("A cutting cannot overlap a tunnel (tunnel %d is there). Choose one for this hill: end the cutting before "
                       "the tunnel starts, or take the tunnel out in the list.", (S32)i + 1)
            : llformat("A tunnel cannot overlap a cutting (cutting %d is there). Choose one for this hill: end the tunnel before "
                       "the cutting starts, or take the cutting out in the list.", (S32)i + 1);
    }
    return std::string();
}

bool WolfFloaterTrack::mergeCuttings(size_t at)
{
    // DESIGN.md 3.6 contract: "marks that overlap are one cutting" - the earliest mark takes the furthest ends of both
    if (at >= mCuttings.size() || !mCuttings[at].mHasB) return false;
    bool joined = false;
    for (size_t i = 0; i < mCuttings.size();)
    {
        if (i == at || !mCuttings[i].mHasB || !stretchesOverlap(mCuttings[at], mCuttings[i])) { ++i; continue; }
        // the two ends furthest apart along the line, out of the four
        std::vector<GP> ends = { mCuttings[at].mA, mCuttings[at].mB, mCuttings[i].mA, mCuttings[i].mB };
        S32 line = -1;
        F64 lo = 1e300, hi = -1e300;
        GP a = ends[0], b = ends[1];
        for (const GP& e : ends)
        {
            F64 s = 0.0;
            if (!drawnChainage(e, line, s)) continue;
            if (s < lo) { lo = s; a = e; }
            if (s > hi) { hi = s; b = e; }
        }
        const size_t keep = llmin(at, i), drop = llmax(at, i);
        mCuttings[keep].mA = a;
        mCuttings[keep].mB = b;
        mCuttings[keep].mStamp = nextStamp();
        mCuttings.erase(mCuttings.begin() + (std::ptrdiff_t)drop);
        at = keep;
        joined = true;
        i = 0;   // the longer cutting may now overlap one more
    }
    return joined;
}

void WolfFloaterTrack::startGroundJob(const LLSD& job, const std::string& kind, const LLSD& layout, const LLUUID& region, const std::string& lead)
{
    // DESIGN.md 3.6 contract: a JOB (saved's cut_job, removed's fill_job, the cut / fill actions' job) or a job id (a
    // remove_server JOB result's fill_job); JOB kind "cut" | "fill"
    const LLSD id = job.isMap() ? job["id"] : job;
    if (!id.isDefined() || (id.isString() && id.asString().empty())) return;
    mGroundJob = id;
    mGroundKind = kind;
    mGroundLayout = layout;
    mGroundRegion = region;
    mGroundLead = lead;
    mGroundRetry = LLSD();
    // [EMBANKMENTS 2026-11-18] track-tool DESIGN.md 3.8: a dig that only raises embankments (JOB shape) is "shaping the ground"
    mGroundShape = job.isMap() && job["shape"].asBoolean();
    const std::string words = kind == "cut" ? (mGroundShape ? "Shaping the ground..." : "Digging the cutting...") : "Putting the ground back...";
    mGroundSaid = words;
    setStatus(lead.empty() ? words : lead + " " + words, !lead.empty());
    const U32 gen = mGen, ground_gen = ++mGroundGen;   // an older job's coroutine stops at its next wake-up
    mGroundPolling = true;
    LLHandle<LLFloater> h = getHandle();
    LL_INFOS("WolfTrack") << kind << " job " << id << " for layout " << layout << " in " << region << LL_ENDL;
    LLCoros::instance().launch("WolfTrackGround", [h, gen, ground_gen]() { groundJobCoro(h, gen, ground_gen); });
    updateControls();
}

// static
void WolfFloaterTrack::groundJobCoro(LLHandle<LLFloater> h, U32 gen, U32 ground_gen)
{
    // DESIGN.md 3.6 "Window" / 10: status every 2-3 s while the job is queued or running, then its end in plain words
    auto self = [&h, gen, ground_gen]() -> WolfFloaterTrack*
    {
        WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
        return f && f->mGen == gen && f->mGroundGen == ground_gen ? f : nullptr;
    };
    const F64 started = LLFrameTimer::getElapsedSeconds();
    S32 fails = 0;
    while (true)
    {
        llcoro::suspendUntilTimeout(GROUND_POLL_S);
        WolfFloaterTrack* f = self();
        if (!f) return;
        const LLSD id = f->mGroundJob;
        LLSD r;
        std::string e;
        const bool got = callNow("status", LLSD().with("job", id), r, e);
        f = self();
        if (!f) return;
        if (got && r["job"].isMap() && r["job"].has("shape")) f->mGroundShape = r["job"]["shape"].asBoolean();
        const bool cut = f->mGroundKind == "cut";
        const bool shape = cut && f->mGroundShape;   // [EMBANKMENTS] only raised banks: "the ground", not "the cutting"
        auto say = [f](const std::string& words, bool error)
        {
            f->setStatus(f->mGroundLead.empty() ? words : f->mGroundLead + " " + words, error || !f->mGroundLead.empty());
        };
        auto stop = [f]()
        {
            f->mGroundJob = LLSD();
            f->mGroundPolling = false;
            f->refreshMine();   // Your railways shows the cutting's state as the record has it now
            f->updateControls();
        };
        const LLSD& job = r["job"];
        const std::string st = got && job.isMap() ? job["state"].asString() : std::string();
        if (st == "done")
        {
            // JOB result {layout, region, cells, kept, state}: kept = cells left on record, land no longer the resident's
            const LLSD& res = job["result"];
            std::string words = shape ? "The ground is shaped." : cut ? "The cutting is dug." : "The ground is back as it was.";
            if (!cut && res["kept"].asInteger() > 0)
                words = "The ground is back as it was, except on land that is no longer yours: that part could not be put back, and "
                        "is left as it is.";
            LL_INFOS("WolfTrack") << f->mGroundKind << " job " << id << " done: " << res["cells"].asInteger() << " cells, kept "
                                  << res["kept"].asInteger() << LL_ENDL;
            say(words, false);
            stop();
            return;
        }
        if (st == "failed")
        {
            const std::string err = job["error"].asString();
            LL_INFOS("WolfTrack") << f->mGroundKind << " job " << id << " failed (" << job["code"].asString() << "): " << err << LL_ENDL;
            LLSD retry;
            retry["action"] = f->mGroundKind;
            retry["layout"] = f->mGroundLayout;
            retry["region"] = f->mGroundRegion;
            f->mGroundRetry = retry;
            say((shape ? "The ground was not shaped: " : cut ? "The cutting was not dug: " : "The ground was not put back: ")
                + (err.empty() ? std::string("the region did not say why.") : err)
                + (cut ? " Press Dig again to try once more." : " Press Put the ground back to try once more."), true);
            stop();
            return;
        }
        if (st == "queued" || st == "running")
        {
            fails = 0;
            const std::string progress = job["progress"].asString();
            const std::string lead = shape ? "Shaping the ground" : cut ? "Digging the cutting" : "Putting the ground back";
            // the server's own progress often begins with the same words ("Digging the cutting: 3 of 77"): said once
            const std::string words = progress.compare(0, lead.size(), lead) == 0 ? progress
                                    : lead + ": " + (progress.empty() ? std::string("waiting for its turn...") : progress);
            if (words != f->mGroundSaid)   // said only when it changes, so the resident's own steps meanwhile stay readable
            {
                f->mGroundSaid = words;
                say(words, false);
            }
        }
        else if (++fails >= REMOVE_POLL_FAILS)
        {
            say(std::string("This window has stopped waiting for ") + (shape ? "the ground to be shaped" : cut ? "the cutting to be dug" : "the ground to be put back")
                + (e.empty() ? std::string(".") : ": " + e) + " Open Your railways in a while to see how it went.", true);
            stop();
            return;
        }
        if (LLFrameTimer::getElapsedSeconds() - started > GROUND_JOB_MAX_S)
        {
            say(std::string("The region has not finished ") + (shape ? "shaping the ground" : cut ? "digging the cutting" : "putting the ground back")
                + " after 20 minutes, so this window has stopped waiting. It may still be at work: open Your railways in a while "
                  "to see how it went.", true);
            stop();
            return;
        }
    }
}

void WolfFloaterTrack::groundAgain(const std::string& action, const LLSD& layout, const LLUUID& region)
{
    // DESIGN.md 3.6 contract, new actions: cut {layout, region} (only while that part is laid), fill {layout, region} (only when
    // that part is not laid) -> job: JOB
    if (busy())
    {
        setStatus("The window is busy with your railway just now - wait until it has finished, then try again.", true);
        return;
    }
    const bool cut = action == "cut";
    mGroundRetry = LLSD();
    setStatus(cut ? "Asking the region to dig the cutting again..." : "Asking the region to put the ground back...", false);
    post(action, LLSD().with("layout", layout).with("region", region.asString()),
         [action, layout, region, cut](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok)
        {
            LLSD retry;
            retry["action"] = action;
            retry["layout"] = layout;
            retry["region"] = region;
            f->mGroundRetry = retry;
            f->setStatus((cut ? "The cutting could not be dug again: " : "The ground could not be put back: ") + error, true);
            f->updateControls();
            return;
        }
        f->startGroundJob(reply["job"], action, layout, region, std::string());
    });
    updateControls();
}

std::string WolfFloaterTrack::cuttingWords(const LLSD& layout, bool& failed, bool& laid_here) const
{
    // DESIGN.md 3.6 contract LAYOUT cuttings {region uuid: {state: waiting | digging | dug | filling | filled | failed, job, kept}}
    failed = false;
    laid_here = false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !layout.isMap()) return std::string();
    const std::string r = rgn->getRegionID().asString();
    laid_here = arr(layout["laid"][r]).size() > 0;
    const LLSD& cut = layout["cuttings"][r];
    if (!cut.isMap()) return std::string();
    const std::string st = cut["state"].asString();
    const std::string kept = cut["kept"].asInteger() > 0
        ? " Some of the ground could not be put back, because that land is no longer yours." : std::string();
    // [EMBANKMENTS 2026-11-18] track-tool DESIGN.md 3.8: a part that only raises embankments (LAYOUT cuttings[r].shape) is
    // said as its ground, not a cutting
    if (cut["shape"].asBoolean())
    {
        if (st == "waiting") return laid_here ? "Its ground here is waiting its turn to be shaped." : "Its ground here is waiting its turn to be put back.";
        if (st == "digging") return "Its ground here is being shaped.";
        if (st == "dug") return "Its ground here is shaped.";
        if (st == "filling") return "Its ground here is being put back.";
        if (st == "filled") return "Its ground here is back as it was." + kept;
        if (st == "failed")
        {
            failed = true;
            return laid_here ? std::string("Shaping its ground here did not finish - press Dig again.")
                             : "Putting its ground back here did not finish - press Put the ground back." + kept;
        }
        return std::string();
    }
    if (st == "waiting") return laid_here ? "Its cutting here is waiting its turn to be dug." : "Its ground here is waiting its turn to be put back.";
    if (st == "digging") return "Its cutting here is being dug.";
    if (st == "dug") return "Its cutting here is dug.";
    if (st == "filling") return "The ground at its cutting here is being put back.";
    if (st == "filled") return "The ground at its cutting here is back as it was." + kept;
    if (st == "failed")
    {
        failed = true;
        return laid_here ? std::string("Digging its cutting here did not finish - press Dig again.")
                         : "Putting the ground back at its cutting here did not finish - press Put the ground back." + kept;
    }
    return std::string();
}

void WolfFloaterTrack::onPickMine()
{
    // the picked railway's cutting here, in plain words (DESIGN.md 3.6 "Window": Your railways shows a layout's cuttings' state)
    const LLSD l = pickedLayout();
    bool failed = false, laid_here = false;
    const std::string words = cuttingWords(l, failed, laid_here);
    if (!words.empty())
    {
        const std::string name = l["name"].asString().empty() ? std::string("This railway") : l["name"].asString();
        setStatus(name + ": " + words, failed);
    }
    updateControls();
}

// static
std::string WolfFloaterTrack::finishTunnels(const LLSD& layout, const Item& item, const std::function<LLUUID(const std::string&)>& uuid_of,
                                            const std::function<void(const std::string&)>& progress)
{
    // Source: WolfStorm js/world/wolf_track_tool.js finishTunnels, the same steps; wolffloatertunnels.cpp buildCoro step 5
    // (the new box is the id not seen before)
    LLSD saved = LLSD::emptyArray();
    S32 n = 0;
    for (const LLSD& t : llsd::inArray(item.mTunnels))
    {
        ++n;
        const LLUUID bore = uuid_of(t["object"].asString());
        if (bore.isNull()) return llformat("Tunnel %d was not found among the placed pieces, so it was not opened.", n);
        std::set<S32> known;
        for (const WolfTunnels::Box& b : WolfTunnels::boxes()) known.insert(b.mId);
        LLSD added = LLSD::emptyArray();
        auto drop = [&]()
        {
            for (const LLSD& id : llsd::inArray(added))
            {
                LLSD a;
                std::string e;
                WolfTunnels::capChange(LLSD().with("op", "remove").with("id", id.asInteger()), a, e);
            }
        };
        const LLSD& vols = t["volumes"];
        S32 k = 0;
        for (const LLSD& v : llsd::inArray(vols))
        {
            progress(llformat("Opening tunnel %d: part %d of %d...", n, ++k, (S32)vols.size()));
            LLSD answer;
            std::string err;
            if (!WolfTunnels::capChange(LLSD().with("op", "add").with("tunnel", LLSD().with("c", v["c"]).with("s", v["s"]).with("yaw", v["yaw"])), answer, err))
            {
                drop();
                return llformat("Tunnel %d could not be opened: ", n) + err + ".";
            }
            S32 got = -1;
            for (const LLSD& b : llsd::inArray(answer["tunnels"])) if (!known.count(b["id"].asInteger())) got = llmax(got, b["id"].asInteger());
            for (const LLSD& b : llsd::inArray(answer["tunnels"])) known.insert(b["id"].asInteger());
            if (got < 0) { drop(); return llformat("Tunnel %d could not be opened: the region did not list the new part.", n); }
            added.append(got);   // (append, never with(i, ...): llsd-with-integer-adds-null)
        }
        // the Tunnel tool's record (php/tunnels.php put): the same design, the bore's uuid and the box ids
        LLSD objects = LLSD::emptyArray();
        objects.append(bore.asString());
        LLSD built;
        built["item"] = LLSD();
        built["objects"] = objects;
        built["volumes"] = added;
        LLSD rec;
        rec["id"] = t["id"].asString();
        rec["name"] = t["name"].asString();
        rec["design"] = t["design"];
        rec["built"] = built;
        LLSD body;
        body["region"] = item.mRegion.asString();
        body["op"] = "put";
        body["tunnel"] = rec;
        std::string err;
        if (!WolfTunnels::recordChange(body, err)) { drop(); return llformat("Tunnel %d could not be recorded: ", n) + err; }
        LLSD one;
        one["id"] = t["id"].asString();
        one["volumes"] = added;
        saved.append(one);
    }
    LLSD reply;
    std::string err;
    LLSD body;
    body["layout"] = layout;
    body["region"] = item.mRegion.asString();
    body["tunnels"] = saved;
    if (!callNow("tunnels_saved", body, reply, err)) return "The tunnels are open, but your railway's record of them could not be saved: " + err;
    return std::string();
}

// static
std::string WolfFloaterTrack::removeTunnels(const LLSD& layout, const LLUUID& region, const LLSD& list, bool& rights)
{
    // Source: WolfStorm js/world/wolf_track_tool.js removeTunnels; wolffloatertunnels.cpp's Delete (the boxes, then the record)
    rights = true;
    LLSD done = LLSD::emptyArray();
    for (const LLSD& t : llsd::inArray(arr(list)))
    {
        if (!t["recorded"].asBoolean()) continue;
        std::set<S32> present;
        for (const WolfTunnels::Box& b : WolfTunnels::boxes()) present.insert(b.mId);
        for (const LLSD& id : llsd::inArray(arr(t["volume_ids"])))
        {
            if (!present.count(id.asInteger())) continue;
            LLSD a;
            std::string e;
            if (!WolfTunnels::capChange(LLSD().with("op", "remove").with("id", id.asInteger()), a, e) && e.find("not here any more") == std::string::npos)
            {
                rights = e.find("estate") == std::string::npos;
                return rights ? "A tunnel could not be closed: " + e + ". Press Remove again."
                              : "The railway's tunnel stays open in the ground: " + e + ". The region's owner can take it away in the Tunnels window.";
            }
        }
        std::string e;
        LLSD body;
        body["region"] = region.asString();
        body["op"] = "delete";
        body["id"] = t["id"].asString();
        if (!WolfTunnels::recordChange(body, e)) return "A tunnel was closed, but its record could not be taken away: " + e + " Press Remove again.";
        done.append(t["id"].asString());
    }
    LLSD reply;
    std::string err;
    LLSD body;
    body["layout"] = layout;
    body["region"] = region.asString();
    body["ids"] = done;
    if (!callNow("tunnels_removed", body, reply, err)) return "The tunnels were closed, but your railway's record of them could not be updated: " + err;
    return std::string();
}

void WolfFloaterTrack::openTunnelsHere(const LLSD& layout_id)
{
    // "Open the tunnel" (Your railways): a part laid here whose tunnel was not opened. The bore's uuid: "saved" keeps the uuids
    // in the order of the item's objects (track_tool_lib.php tt_api_saved), so the object named as the bore is at that place
    if (busy()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const LLUUID region = rgn->getRegionID();
    mRezzing = true;
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    updateControls();
    LLCoros::instance().launch("WolfTrackOpenTunnel", [h, gen, layout_id, region]()
    {
        auto self = [&h, gen]() -> WolfFloaterTrack*
        {
            WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
            return f && f->mGen == gen ? f : nullptr;
        };
        LLSD reply;
        std::string error;
        const bool ok = callNow("get", LLSD().with("layout", layout_id), reply, error);
        WolfFloaterTrack* f = self();
        if (!f) return;
        auto end = [&](const std::string& text, bool err)
        {
            f->mRezzing = false;
            f->setStatus(text, err);
            f->refreshMine();
            f->updateControls();
        };
        if (!ok) { end("That railway could not be fetched: " + error, true); return; }
        const std::vector<Item> items = readItems(reply["items"]);
        const LLSD laid = arr(reply["layout"]["laid"][region.asString()]);
        std::set<std::string> closed;
        for (const LLSD& t : llsd::inArray(arr(reply["layout"]["tunnels"][region.asString()]))) if (!t["recorded"].asBoolean()) closed.insert(t["id"].asString());
        for (const Item& it : items)
        {
            if (it.mRegion != region) continue;
            if (laid.size() == 0) break;
            Item part = it;
            part.mTunnels = LLSD::emptyArray();
            for (const LLSD& t : llsd::inArray(it.mTunnels)) if (closed.count(t["id"].asString())) part.mTunnels.append(t);
            const auto uuid_of = [&it, &laid](const std::string& name) -> LLUUID
            {
                for (size_t k = 0; k < it.mObjects.size(); ++k)
                    if (it.mObjects[k].mName == name && (LLSD::Integer)k < (LLSD::Integer)laid.size()) return laid[(LLSD::Integer)k].asUUID();
                return LLUUID::null;
            };
            const std::string why = finishTunnels(layout_id, part, uuid_of, [&](const std::string& t) { if (WolfFloaterTrack* g = self()) g->setStatus(t, false); });
            f = self();
            if (!f) return;
            end(why.empty() ? plural((S32)part.mTunnels.size(), "tunnel was", "tunnels were") + " opened through the hill." : why, !why.empty());
            return;
        }
        end("This railway is not laid in this region.", true);   // the WolfStorm window's words
    });
}

void WolfFloaterTrack::onShow()
{
    const LLSD l = pickedLayout();
    if (!l.isMap() || busy()) return;
    const std::string name = l["name"].asString();
    if (mShownId.isDefined() && mShownId.asString() == l["id"].asString() && !mShown.mLines.empty())
    {
        mShown.clear();
        mShownName.clear();
        mShownId = LLSD();
        setStatus("The railway is not shown any more.", false);
        return;
    }
    setStatus("Fetching " + name + "...", false);
    const LLSD id = l["id"];
    post("get", LLSD().with("layout", id), [name, id](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok) { f->setStatus("That railway could not be fetched: " + error, true); return; }
        // [live test 14 Nov] the stored request has no ground and no others (track_tool_lib.php tt_lay_insert keeps only the
        // drawing), and the planner refuses a request without them (tracktool/plan.py parse "ground"): the REQ is made here
        // again from the stored drawing, with the ground and the guide-shaped objects this viewer has now, as Lay does
        std::vector<LLUUID> own;
        const LLSD& laid = reply["layout"]["laid"];
        if (laid.isMap())
        {
            for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
                for (const LLSD& u : llsd::inArray(arr(it->second))) own.push_back(u.asUUID());
        }
        LLSD req;
        std::string why;
        if (!f->requestFor(reply["request"], own, req, why))
        {
            f->setStatus("That railway could not be drawn: " + (why.empty() ? std::string("it has no line to draw.") : why), true);
            return;
        }
        // [CUTTINGS] DESIGN.md 3.6 contract: plan with "layout": id is planned on the ground before that layout's cuttings - the
        // ground this viewer has is dug where they are, so without it a laid railway's cuttings would not show
        f->post("plan", LLSD().with("request", req).with("layout", id), [name, id](WolfFloaterTrack* f2, bool ok2, const LLSD& r2, const std::string& e2)
        {
            if (!ok2) { f2->setStatus("That railway could not be drawn: " + e2, true); return; }
            f2->applyPlan(r2["plan"], f2->mShown);
            f2->mShownName = name;
            f2->mShownId = id;
            f2->setStatus(name + " is drawn in the world in light blue. Press Show again to hide it.", false);
        });
    });
}

void WolfFloaterTrack::onChange()
{
    const LLSD l = pickedLayout();
    if (!l.isMap() || busy()) return;
    auto load = [this, l]()
    {
        post("get", LLSD().with("layout", l["id"]), [](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
        {
            if (!ok) { f->setStatus("That railway could not be fetched: " + error, true); return; }
            if (f->busy()) return;
            f->loadDrawing(reply["request"]);
            // DESIGN.md 10: Change = a new layout that replaces this one once it is laid everywhere; until then both stand, and
            // this one's pieces are not counted as someone else's railway in the plan
            const LLSD& layout = reply["layout"];
            f->mReplaces = layout["id"];
            f->mReplaceName = layout["name"].asString();
            f->mReplaceLaid.clear();
            const LLSD& laid = layout["laid"];
            if (laid.isMap())
            {
                for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
                    for (const LLSD& u : llsd::inArray(arr(it->second))) f->mReplaceLaid.push_back(u.asUUID());
            }
            f->mShown.clear();
            f->mShownName.clear();
            f->changed();
            f->setStatus("You are changing " + f->mReplaceName + ". Change the drawing, then press Lay track: your old railway "
                         "stays until the changed one is laid, and is then taken away.", false);
        });
    };
    if (!drawingEmpty()) ask("Put " + l["name"].asString() + " in the drawing instead of what you have drawn?", "Yes, change it", load);
    else load();
}

void WolfFloaterTrack::onLayPart()
{
    const LLSD l = pickedLayout();
    if (!l.isMap() || busy()) return;
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        const std::string r = rgn->getRegionID().asString();
        bool to_open = false;
        if (arr(l["laid"][r]).size() > 0)
            for (const LLSD& t : llsd::inArray(arr(l["tunnels"][r]))) to_open = to_open || !t["recorded"].asBoolean();
        if (to_open) { openTunnelsHere(l["id"]); return; }
        // [CUTTINGS] a dig or fill that failed here: the same button asks again (DESIGN.md 3.6 "Window": Your railways offers
        // Dig again while the part is laid here, Put the ground back when it is not)
        bool failed = false, laid_here = false;
        cuttingWords(l, failed, laid_here);
        if (failed) { groundAgain(laid_here ? "cut" : "fill", l["id"], rgn->getRegionID()); return; }
    }
    post("get", LLSD().with("layout", l["id"]), [](WolfFloaterTrack* f, bool ok, const LLSD& reply, const std::string& error)
    {
        if (!ok) { f->setStatus("That railway could not be fetched: " + error, true); return; }
        if (f->busy()) return;
        f->mItems = readItems(reply["items"]);
        f->mItemsFromDrawing = false;   // another railway's parts: the drawing is not touched when it is laid
        f->mLayLayout = reply["layout"]["id"];
        for (const Item& it : f->mItems) if (!it.mRegionName.empty()) f->mRegionNames[it.mRegion] = it.mRegionName;
        f->layHere();
    });
}

void WolfFloaterTrack::onRemove()
{
    const LLSD l = pickedLayout();
    if (!l.isMap() || busy()) return;
    const std::string name = l["name"].asString().empty() ? std::string("this railway") : l["name"].asString();
    // DESIGN.md 10: the region itself may take the pieces away (for good, not to the Trash); the railway stays in Objects
    setStatus("Remove takes this railway's pieces away from this region for good. The railway itself stays in your inventory "
              "(Objects), so you can lay it again.", false);
    ask("Take " + name + " away from this region?", "Remove it", [this, l]()
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) { setStatus("You are not in a region just now - try again in a moment.", true); return; }
        // a railway laid in this session: its pieces are all in view and the region may not have saved them yet, so they are
        // taken from nearby first; any other: the region first (the pieces need not be in view)
        startRemoval(l["id"], rgn->getRegionID(), false, mLaidThisSession.count(l["id"].asString()) > 0);
    });
}

void WolfFloaterTrack::startRemoval(const LLSD& layout, const LLUUID& region, bool replacing, bool nearby_first)
{
    if (busy())
    {
        setStatus("The window is busy with your railway just now - wait until it has finished, then press Remove again.", true);
        return;
    }
    mRemoving = true;   // until the whole removal ends (removalCoro's finish)
    LLHandle<LLFloater> h = getHandle();
    const U32 gen = mGen;
    LLCoros::instance().launch("WolfTrackRemoval", [h, gen, layout, region, replacing, nearby_first]()
    {
        removalCoro(h, gen, layout, region, replacing, nearby_first);
    });
    updateControls();
}

// One removal of a layout's part in the agent's region, start to end (DESIGN.md 10 remove / remove_server / removed):
//  1. what is on record here ("remove");
//  2. nearby_first (laid this session, or a Change's old railway): what is in view goes from here (the region saves new
//     pieces only 60-600 s after they were placed, so the server would not find them yet);
//  3. what is left: the region itself (remove_server, its job's status every 2.5 s, at most REMOVE_JOB_MAX_S);
//  4. what is still on record after it ("remove" again), less the pieces this window deleted itself this session (the
//     server counts those as not found: they are gone from the region's records) - in view: from here;
//  5. nothing real left: "removed" (only when the window's own deletions are all that the record still names), else the
//     counts in plain words, and "Forget them?" for pieces the region did not find.
// The window stays busy throughout. Source: WolfStorm floater_wolf_track_tool.js _afterServerRemove, the same order.
// static
void WolfFloaterTrack::removalCoro(LLHandle<LLFloater> h, U32 gen, LLSD layout, LLUUID region, bool replacing, bool nearby_first)
{
    std::function<WolfFloaterTrack*()> self = [h, gen]() -> WolfFloaterTrack*
    {
        WolfFloaterTrack* f = dynamic_cast<WolfFloaterTrack*>(h.get());
        return f && f->mGen == gen ? f : nullptr;
    };
    WolfFloaterTrack* f = self();
    if (!f) return;
    const std::string key = layout.asString();
    std::string tunnel_note;   // [TUNNELS] a tunnel left open (no rights to close it), said with the last words
    // [CUTTINGS] DESIGN.md 3.6 contract: the fill of this region's cutting, queued when its part is gone - "removed" answers
    // fill_job: JOB | null, a remove_server JOB's result has fill_job: id | null; followed once the removal has ended
    LLSD fill_job;
    auto finish = [&](const std::string& text, bool error, bool gone_here)
    {
        f->mRemoving = false;
        f->mProgress->setVisible(false);
        if (gone_here && replacing) f->mReplaceLaid.clear();
        if (gone_here) f->mShown.clear();
        f->setStatus(text + tunnel_note, error);
        f->updateControls();
        f->refreshMine();
        if (fill_job.isMap() || fill_job.isInteger() || (fill_job.isString() && !fill_job.asString().empty()))
            f->startGroundJob(fill_job, "fill", layout, region, error ? text + tunnel_note : std::string());
    };
    // the record's laid map, or false with the words
    auto onRecord = [&](LLSD& laid, std::string& error) -> bool
    {
        LLSD reply;
        if (!callNow("remove", LLSD().with("layout", layout), reply, error)) return false;
        laid = reply["layout"]["laid"];
        return true;
    };
    auto hereOf = [&](const LLSD& laid)
    {
        std::vector<LLUUID> out;
        const std::set<LLUUID>& mine = f->mRemovedEarlier[key];
        for (const LLSD& u : llsd::inArray(arr(laid[region.asString()])))
        {
            const LLUUID id = u.asUUID();
            if (!mine.count(id)) out.push_back(id);   // this window took it away itself already
        }
        return out;
    };
    auto elsewhereWords = [&](const LLSD& laid)
    {
        std::string t;
        if (!laid.isMap()) return t;
        for (LLSD::map_const_iterator it = laid.beginMap(); it != laid.endMap(); ++it)
        {
            if (arr(it->second).size() == 0 || LLUUID(it->first) == region) continue;
            t += replacing ? " The part of your old railway in " + f->regionName(LLUUID(it->first)) + " is still there. Go there, open "
                             "Your railways and press Remove on it."
                           : " The part in " + f->regionName(LLUUID(it->first)) + " is still there. Go there and press Remove again.";
        }
        return t;
    };
    const std::string done_words = replacing ? "Your changed railway is laid, and the old one was taken away here."
                                             : "The railway was taken away here.";
    // "removed" for this region: the record forgets this region's part (only when nothing real of it is left)
    auto sendRemovedNow = [&](const std::string& words)
    {
        LLSD reply;
        std::string error;
        const bool ok = callNow("removed", LLSD().with("layout", layout).with("region", region.asString()), reply, error);
        f = self();
        if (!f) return;
        if (!ok) { finish("The pieces were taken away, but the record could not be updated: " + error + " Press Remove again.", true, false); return; }
        if (reply["fill_job"].isMap()) fill_job = reply["fill_job"];   // (a fill the server's removal queued is kept)
        finish(words + elsewhereWords(reply["layout"]["laid"]) + (replacing ? f->mLaidNote : std::string()), false, true);
    };

    // 0. [TUNNELS] DESIGN.md 3.4 "Remove": this region's tunnels go first, the way the Tunnels window's Delete takes one -
    //    their boxes (the ground closes) and their records; where the resident may no longer change tunnels here they stay,
    //    the window says who can take them away, and the railway's pieces still go
    {
        LLSD rec;
        std::string terr;
        if (callNow("remove", LLSD().with("layout", layout), rec, terr))
        {
            const LLSD& list = rec["layout"]["tunnels"][region.asString()];
            bool any = false;
            for (const LLSD& t : llsd::inArray(arr(list))) any = any || t["recorded"].asBoolean();
            if (any)
            {
                f = self();
                if (!f) return;
                f->setStatus("Closing the railway's tunnel...", false);
                bool rights = true;
                const std::string why = removeTunnels(layout, region, list, rights);
                f = self();
                if (!f) return;
                if (!why.empty() && rights) { finish(why, true, false); return; }
                if (!why.empty()) tunnel_note = " " + why;
            }
        }
        f = self();
        if (!f) return;
    }

    // 1. on record here
    LLSD laid;
    std::string error;
    f->setStatus("Looking up your railway's pieces...", false);
    bool ok = onRecord(laid, error);
    f = self();
    if (!f) return;
    if (!ok) { finish("The railway was not taken away: " + error, true, false); return; }
    std::vector<LLUUID> here = hereOf(laid);
    if (here.empty())
    {
        const std::string other = elsewhereWords(laid);
        if (!other.empty() && arr(laid[region.asString()]).size() == 0) { finish("This railway has no pieces in this region." + other, false, false); return; }
        sendRemovedNow(done_words);   // nothing (or only what this window took away) on record here
        return;
    }
    size_t near_taken = 0;   // went to the Trash from here, this removal
    // 2. nearby first
    if (nearby_first)
    {
        NearResult step;
        if (!nearbyStep(self, region, here, step)) return;
        f = self();
        if (!f) return;
        for (const LLUUID& id : step.gone) f->mRemovedEarlier[key].insert(id);
        near_taken += step.taken;
        if (step.stayed == 0 && step.out_of_reach == 0) { sendRemovedNow(done_words); return; }
    }
    // 3. the region itself
    f->mProgress->setVisible(true);
    f->setStatus("Asking the region to take your railway away...", false);
    LLSD reply;
    ok = callNow("remove_server", LLSD().with("layout", layout).with("region", region.asString()), reply, error);
    f = self();
    if (!f) return;
    bool server_done = false;
    S32 deleted = 0, not_found = 0, still_there = 0, left = 0, laid_left = -1;
    std::string server_said;   // why the region could not (shown when something is left)
    if (!ok)
    {
        const std::string code = reply.isMap() ? reply["code"].asString() : std::string();
        LL_INFOS("WolfTrack") << "remove_server refused (" << code << "): " << error << LL_ENDL;
        if (code == "nothing_laid") { sendRemovedNow(done_words); return; }   // nothing of it on record here now
        server_said = error;   // any other refusal (429 too): the in-view way below, as before
    }
    else
    {
        LLSD job = reply["job"];
        const LLSD job_id = job["id"];
        S32 fails = 0;
        const F64 started = LLFrameTimer::getElapsedSeconds();
        std::string state = job["state"].asString();
        while (state != "done" && state != "failed")
        {
            const std::string progress = job["progress"].asString();
            f->setStatus("Taking your railway away: " + (progress.empty() ? std::string("waiting for its turn") : progress), false);
            if (LLFrameTimer::getElapsedSeconds() - started > REMOVE_JOB_MAX_S)
            {
                finish("The region has not finished taking your railway away after 10 minutes, so this window has stopped waiting. "
                       "It may still be at work: open Your railways in a while to see what is left.", true, false);
                return;
            }
            llcoro::suspendUntilTimeout(REMOVE_POLL_S);
            f = self();
            if (!f) return;
            LLSD r;
            std::string e;
            const bool got = callNow("status", LLSD().with("job", job_id), r, e);
            f = self();
            if (!f) return;
            // (review of ec46b58654) a reply without the job, or a state this window does not know, is a failed ask too
            const std::string st = got && r["job"].isMap() ? r["job"]["state"].asString() : std::string();
            if (st != "queued" && st != "running" && st != "done" && st != "failed")
            {
                if (++fails >= REMOVE_POLL_FAILS)
                {
                    finish("This window has stopped waiting for the region to take your railway away" + (e.empty() ? std::string(".") : ": " + e)
                           + " Open Your railways in a while to see what is left.", true, false);
                    return;
                }
                continue;
            }
            fails = 0;
            job = r["job"];
            state = st;
        }
        const LLSD& res = job["result"];
        deleted = res["deleted"].asInteger();
        not_found = res["not_found"].asInteger();
        still_there = res["still_there"].asInteger();
        left = res["left"].asInteger();
        laid_left = res.has("laid_left") ? res["laid_left"].asInteger() : -1;
        if (res.has("fill_job") && res["fill_job"].isDefined()) fill_job = res["fill_job"];   // [CUTTINGS] its last piece went
        LL_INFOS("WolfTrack") << "remove_server " << state << " (" << job["code"].asString() << "): deleted " << deleted << ", not found "
                              << not_found << ", still there " << still_there << ", not looked at " << left << ", on record here "
                              << laid_left << LL_ENDL;
        if (state == "done")
        {
            server_done = true;
            if (laid_left == 0)
            {
                std::string t = done_words + " " + SERVER_GONE_WORDS;
                if (near_taken > 0) t += " (" + plural((S32)near_taken, "piece near you went", "pieces near you went") + " into your Trash.)";
                if (res["state"].asString() == "partly_removed")
                    t += replacing ? " Part of your old railway is still in another region: go there, open Your railways and press Remove on it."
                                   : " Its part in another region is still there: go there and press Remove again.";
                finish(t + (replacing ? f->mLaidNote : std::string()), false, true);
                return;
            }
        }
        else
        {
            const std::string err = job["error"].asString();
            server_said = err.empty() ? std::string("the region could not do it") : err;
        }
    }
    // 4. still on record, less what this window took away itself; what is in view goes from here
    ok = onRecord(laid, error);
    f = self();
    if (!f) return;
    if (!ok) { finish("The railway was not taken away completely: " + error + " Press Remove again.", true, false); return; }
    here = hereOf(laid);
    if (here.empty()) { sendRemovedNow(done_words + (deleted > 0 ? std::string(" ") + SERVER_GONE_WORDS : std::string())); return; }
    NearResult step;
    if (!nearbyStep(self, region, here, step)) return;
    f = self();
    if (!f) return;
    for (const LLUUID& id : step.gone) f->mRemovedEarlier[key].insert(id);
    near_taken += step.taken;
    const size_t still = step.stayed + step.out_of_reach;
    if (still == 0) { sendRemovedNow(done_words + (deleted > 0 ? std::string(" ") + SERVER_GONE_WORDS : std::string())); return; }
    // 5. what is left, in plain words
    std::string t;
    if (deleted > 0) t += plural(deleted, "piece", "pieces") + " taken away by the region. ";
    if (near_taken > 0) t += plural((S32)near_taken, "piece", "pieces") + " taken away from here (into your Trash). ";
    if (!server_said.empty()) t += "The region could not take the rest away: " + server_said + ". ";
    if (step.stayed > 0) t += plural((S32)step.stayed, "piece was", "pieces were") + " not taken away (the region did not allow it). ";
    const bool not_found_only = server_done && still_there == 0 && left == 0 && step.stayed == 0;
    if (not_found_only)
    {
        // (review of ec46b58654) the server forces a save first, so "not found" means gone already or someone else's now
        finish(t + plural((S32)step.out_of_reach, "piece was", "pieces were") + " not found in the region (perhaps taken away already, "
               "or given to someone else).", true, false);
        f->ask(llformat("Forget the %s?", plural((S32)step.out_of_reach, "piece that was not found", "pieces that were not found").c_str()),
               "Forget them", [f, layout, region, replacing]() { f->sendRemoved(layout, region, replacing); });
        return;
    }
    t += plural((S32)step.out_of_reach, "piece is", "pieces are") + " still there, out of reach. Come closer to them and press Remove "
         "again. If you took them away yourself, press Forget them.";
    finish(t, true, false);
    if (step.out_of_reach > 0)
    {
        f->ask(llformat("Forget the %s still on record here?", plural((S32)step.out_of_reach, "piece", "pieces").c_str()), "Forget them",
               [f, layout, region, replacing]() { f->sendRemoved(layout, region, replacing); });
    }
}

// ---- the camera (wolffloaterlifts.cpp:1076-1128 lookFromAbove / giveCameraBack, over the whole drawing) ---------------------------

void WolfFloaterTrack::lookFromAbove()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || LLApp::isExiting()) return;
    if (gAgentCamera.cameraMouselook()) gAgentCamera.changeCameraToDefault();
    if (!mCamSaved)
    {
        mCamSaved = true;
        mCamWasOnAvatar = gAgentCamera.getFocusOnAvatar();
        mCamPos = gAgentCamera.getCameraPositionGlobal();
        mCamFocus = gAgentCamera.getFocusTargetGlobal();
    }
    F64 cx = 0.0, cy = 0.0, span = 0.0;
    if (!mFlat.empty())
    {
        F64 x1 = 1e300, y1 = 1e300, x2 = -1e300, y2 = -1e300;
        for (const LLVector2& p : mFlat)
        {
            x1 = llmin(x1, (F64)p.mV[0]); y1 = llmin(y1, (F64)p.mV[1]); x2 = llmax(x2, (F64)p.mV[0]); y2 = llmax(y2, (F64)p.mV[1]);
        }
        cx = (x1 + x2) * 0.5;
        cy = (y1 + y2) * 0.5;
        span = llmax(x2 - x1, y2 - y1);
    }
    else
    {
        const LLVector3 me = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        cx = me.mV[VX];
        cy = me.mV[VY];
    }
    const LLVector3d c = rgn->getPosGlobalFromRegion(LLVector3((F32)cx, (F32)cy, 0.f));
    const F32 g = LLWorld::getInstance()->resolveLandHeightGlobal(c);
    const F32 half = llmax(0.05f, tanf(LLViewerCamera::getInstance()->getView() * 0.5f));
    // a click finds the ground only within the draw distance (wolffloaterlifts.cpp:1109-1113): the camera stays low enough.
    // [TRACK PICK 2026-11-18] the resident's own draw distance, as the pick (llviewerwindow.cpp mousePointOnLandGlobal)
    const F32 reach = llmax(40.f, WolfAdaptiveDrawDistance::userDrawDistance() * 0.55f);
    const F32 h = llmin(reach, llmax(TOP_VIEW_M, 0.6f * (F32)span / half));
    const LLVector3d focus(c.mdV[VX], c.mdV[VY], (F64)g);
    const LLVector3d cam = focus + LLVector3d(0.0, -(F64)(h * TOP_VIEW_TILT), (F64)h);
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(cam, focus, LLUUID::null);
    mCamFocusSet = focus;
}

void WolfFloaterTrack::giveCameraBack()
{
    if (!mCamSaved) return;
    mCamSaved = false;
    const bool still_ours = !gAgentCamera.getFocusOnAvatar() && (gAgentCamera.getFocusTargetGlobal() - mCamFocusSet).magVecSquared() < 0.01;
    if (!still_ours) return;
    if (mCamWasOnAvatar) gAgentCamera.setFocusOnAvatar(true, true, false);
    else gAgentCamera.setCameraPosAndFocusGlobal(mCamPos, mCamFocus, LLUUID::null);
}

// ---- the pen (wolffloaterlifts.cpp:1132-1152 penAllowed / keepPen) ---------------------------------------------------------------

bool WolfFloaterTrack::penAllowed() const
{
    if (!mPenWanted || busy() || !getVisible() || isMinimized() || !onWolf() || LLApp::isExiting()) return false;
    if (LLFloaterReg::instanceVisible("wolf_tunnels") || LLFloaterReg::instanceVisible("wolf_terraform")
        || LLFloaterReg::instanceVisible("wolf_lifts")) return false;
    return gAgent.getRegion() != nullptr;
}

void WolfFloaterTrack::keepPen()
{
    if (!WolfToolTunnel::instanceExists() && !mPenWanted) return;
    WolfToolTunnel* t = WolfToolTunnel::getInstance();
    if (!penAllowed())
    {
        if (t->pen() != WolfToolTunnel::PEN_NONE && t->hostIs(this)) t->cancel();
        return;
    }
    if (t->current()) return;
    if (gFloaterTools && gFloaterTools->getVisible()) return;
    if (LLToolMgr::getInstance()->usingTransientTool() || gFocusMgr.getMouseCapture() || gAgentCamera.cameraMouselook()) return;
    t->begin(getHandle(), WolfToolTunnel::PEN_ROUTE);
}

void WolfFloaterTrack::penStopped()
{
    updateControls();
}

void WolfFloaterTrack::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatusError = error && !text.empty();
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

// ---- the window ---------------------------------------------------------------------------------------------------------------

void WolfFloaterTrack::draw()
{
    if (!onWolf())
    {
        // a hypergrid move off Wolf Territories: the window and its pen go (paul-wolf-features-wolf-grid-only)
        closeFloater();
        return;
    }
    // one pen at a time, the window opened last keeps it: the Tunnels, Terraform or Lifts window opened after this one puts
    // this one away (as this one puts them away in onOpen); the drawing is kept for the next open
    for (const char* other : { "wolf_tunnels", "wolf_terraform", "wolf_lifts" })
    {
        if (!mStartPending && LLFloaterReg::instanceVisible(other) && !busy())
        {
            closeFloater();
            return;
        }
    }
    if (mStartPending && !LLApp::isExiting())
    {
        // here and not in onOpen: the Build window's focus would put the pen down at once (wolffloaterlifts.cpp:1158-1169)
        mStartPending = false;
        if (gFloaterTools && gFloaterTools->getVisible())
        {
            gFloaterTools->closeFloater();
            setStatus("The Build window was closed so that your clicks draw the railway.", false);
        }
        lookFromAbove();
        mPenWanted = true;
    }
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        if (rgn->getHandle() != mFlatRegion) rebuildFlat();   // the drawing is in grid metres: seen from the new region
        if (rgn->getRegionID() != mMineRegion) refreshMine();
    }
    if (mPlanDirty && !mPlanInFlight && LLFrameTimer::getElapsedSeconds() - mChangedAt >= PLAN_WAIT_S) sendPlan();
    // [T961 2026-11-21] the floaters' area changed (the viewer window resized, the UI size changed, docked here): fit again
    if (gFloaterView && getParent() == gFloaterView && !isMinimized() && gFloaterView->getSnapRect().getHeight() != mAreaH) relayout();
    updateControls();
    keepPen();
    LLFloater::draw();
}

// updateControls runs every frame: a text box is only set when its words change (wolffloaterlifts.cpp:1205-1210)
void WolfFloaterTrack::setTextIfChanged(LLTextBox* box, const std::string& text)
{
    if (box && box->getText() != text) box->setText(text);
}

void WolfFloaterTrack::relayout()
{
    // [T961 2026-11-21] Shadow: "nothing is ever off screen" (residents on small laptop screens). The window is its content
    // (track_content: closed, or down to the Your railways panel when it is open) plus the title bar, but never taller than the
    // floaters' area; cut short, track_scroll shows its bar and the window widens by the bar, so no control is covered.
    // History: 14 Nov a closed height of 624 drew the Your railways box below the floater (its click started a new line in the
    // world); the fix, a constant 680 (the XML's height), still lost LLFloater::initFloaterXML's header stretch at the foot, and
    // open (820) the panel ran off any floaters' area under 820 high. Source: wolffloaterterraform.cpp relayout (ticket 961, the
    // height as built) and wolffloaterhuds.cpp fitToArea (the scroll recomputed after the document changes, kept on screen).
    if (!mScroll || !mContent) return;
    const bool open = mMineCheck && mMineCheck->get();
    if (mMinePanel) mMinePanel->setVisible(open);
    const S32 content_h = open ? llmax(mClosedH, mOpenH) : mClosedH;
    // the document grows downwards: its children follow its top (LLView::reshape), the scroll places it (updateScroll)
    if (mContent->getRect().getHeight() != content_h) mContent->reshape(mContentW, content_h);
    const bool in_area = gFloaterView && getParent() == gFloaterView;
    const S32 area_h = in_area ? gFloaterView->getSnapRect().getHeight() : S32_MAX;
    const S32 full = content_h + mHeaderH;
    // CHOSEN: never shorter than the title bar and 120 px of content (about four rows), even on a tinier area
    const S32 h = llmax(llmin(full, area_h), mHeaderH + 120);
    // Source: llscrollcontainer.cpp:98-99 - the bar's width is UIScrollbarSize
    static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
    const S32 w = mContentW + (h < full ? (S32)scrollbar_size : 0);
    LLRect r = getRect();
    if (r.getHeight() != h || r.getWidth() != w)
    {
        // the top-left stays where it is
        r.mBottom = r.mTop - h;
        r.mRight = r.mLeft + w;
        setShape(r);
    }
    // setShape reshapes only when the rect changed; the document may have changed alone (wolffloaterhuds.cpp fitToArea)
    mScroll->reshape(mScroll->getRect().getWidth(), mScroll->getRect().getHeight());
    if (in_area)
    {
        // lift it when its foot went below the floaters' area, never its title bar above that area's top
        // Source: wolffloaterterraform.cpp relayout (llfloater.h LLFloaterView::getSnapRect)
        const LLRect snap = gFloaterView->getSnapRect();
        const LLRect now = getRect();
        if (now.mBottom < snap.mBottom)
        {
            const S32 up = llmin(snap.mBottom - now.mBottom, llmax(0, snap.mTop - now.mTop));
            if (up > 0) translate(0, up);
        }
        // and, widened by the scroll bar at the right edge, back left, never its left edge past the area's
        if (now.mRight > snap.mRight)
        {
            const S32 left = llmin(now.mRight - snap.mRight, llmax(0, now.mLeft - snap.mLeft));
            if (left > 0) translate(-left, 0);
        }
    }
    mAreaH = in_area ? area_h : -1;
}

void WolfFloaterTrack::updateControls()
{
    if (!mHint) return;
    const bool b = busy();
    setTextIfChanged(mHint, onWolf() ? routeHint() : std::string("Lay Railway Track works on Wolf Territories regions only."));
    mMode->setEnabled(!b);
    mUndoBtn->setEnabled(!b && !drawingEmpty());
    mFinishBtn->setEnabled(!b && mCurLine >= 0);
    mStartOverBtn->setEnabled(!b && !drawingEmpty() && !mAsking);
    const bool station_edit = mEditStation >= 0 && (size_t)mEditStation < mStations.size();
    // the turntable's choices take the station row's place while a turntable is being put down or is picked
    const bool tt_edit = mEditTurntable >= 0 && (size_t)mEditTurntable < mTurntables.size();
    const S32 mode_now = mMode->getSelectedIndex();
    const bool show_tt = mode_now == MODE_TURNTABLE || (tt_edit && !station_edit);   // the last one picked shows
    mTTPanel->setVisible(show_tt);
    // [TUNNELS] where the plan finds a hill in the way, its two choices take the station row's place (not while a station or a
    // turntable is being put down or changed)
    const LLSD sug = pendingSuggestion();
    // (also in station mode - Paul 15 Nov: the choice is never out of sight while a hill is in the way; only a station or a
    // turntable being changed takes the row)
    const bool show_sug = !show_tt && !station_edit && sug.isMap();
    mSugPanel->setVisible(show_sug);
    if (show_sug)
    {
        // each choice in a line or two from the plan's own figures (tracktool/layout.py _tunnel_stats: length_m, grade,
        // deepest_m; _climb_stats: rise_m, grade, highest_m; _climb_impossible / the MAX_FILL refusal: possible false with
        // rise_m or highest_m); the plan's whole sentence is the row's tool tip
        const LLSD& tun = sug["tunnel"];
        const LLSD& climb = sug["climb"];
        const LLSD& cut = sug["cutting"];
        // [CUTTINGS] a hill with no tunnel choice (PLAN cut_suggestions): Dig a cutting and Climb the hill in the first two rows
        const bool cut_only = sug["cut_only"].asBoolean();
        mSugTunnelBtn->setVisible(!cut_only);
        mSugTunnelText->setVisible(!cut_only);
        mSugCutBtn->setRect(mSugRowRect[cut_only ? 0 : 1][0]);
        mSugCutText->setRect(mSugRowRect[cut_only ? 0 : 1][1]);
        mSugClimbBtn->setRect(mSugRowRect[cut_only ? 1 : 2][0]);
        mSugClimbText->setRect(mSugRowRect[cut_only ? 1 : 2][1]);
        setTextIfChanged(mSugText, std::string("A hill is in the way of your line:"));
        // and how much of the whole line each leaves up on viaducts (layout.py _viaduct_m: viaduct_m)
        auto viaducts = [](const LLSD& o) -> std::string
        {
            if (!o.has("viaduct_m")) return std::string(".");
            const S32 m = o["viaduct_m"].asInteger();
            return m <= 0 ? std::string("; no viaduct on the line.") : llformat("; %d m of viaduct on the line.", m);
        };
        auto climbing = [](const LLSD& o) -> std::string
        {
            const std::string gr = o["grade"].asString();
            return gr == "level" ? std::string("level") : "climbing at most " + gr;
        };
        const std::string tt = mMayTunnel
            ? llformat("%d m of tunnel, ", tun["length_m"].asInteger()) + climbing(tun)
              + llformat(", up to %d m of hill over it", ll_round((F32)tun["deepest_m"].asReal())) + viaducts(tun)
            : (mNoTunnelReason.empty() ? std::string("You cannot make tunnels in this region.") : mNoTunnelReason);
        setTextIfChanged(mSugTunnelText, tt);
        mSugTunnelText->setToolTip(tun["text"].asString());
        const bool climb_no = climb.isMap() && climb["possible"].isBoolean() && !climb["possible"].asBoolean();
        std::string ct;
        if (!climb.isMap()) ct = "The line goes over the hill.";
        else if (climb_no && climb.has("highest_m"))
            ct = llformat("Not possible as drawn: the line would stand %d m above the ground.", ll_round((F32)climb["highest_m"].asReal()));
        else if (climb_no)
            ct = llformat("Not possible as drawn: the ground rises %d m, too steeply for trains.", ll_round((F32)climb["rise_m"].asReal()));
        else if (climb["possible"].isBoolean())
            ct = llformat("Over the top %d m up, ", ll_round((F32)climb["rise_m"].asReal())) + climbing(climb)
                 + llformat(", up to %d m above the ground", ll_round((F32)climb["highest_m"].asReal())) + viaducts(climb);
        else ct = climb["text"].asString();
        setTextIfChanged(mSugClimbText, ct);
        mSugClimbText->setToolTip(climb.isMap() ? climb["text"].asString() : std::string());
        mSugTunnelBtn->setEnabled(!b && mMayTunnel);
        // [CUTTINGS] DESIGN.md 3.6 contract suggestions[].cutting / cut_suggestions[].cutting {possible, text, a, b, length_m,
        // deepest_m, walls_m, viaduct_m}: its figures in a line or two (the plan's sentence is the tool tip); when it cannot be
        // dug there, the plan's words say why and the button is off; "open" access.cut false: the region's reason
        const bool cut_possible = cut.isMap() && cut["possible"].asBoolean();
        std::string kt;
        if (!mMayCut) kt = mNoCutReason.empty() ? std::string("You cannot dig cuttings in this region.") : mNoCutReason;
        else if (!cut.isMap()) kt = "A cutting is not offered here.";
        else if (!cut_possible || !cut.has("length_m"))
        {
            kt = cut["text"].asString();
            static const std::string LEAD("Dig a cutting: ");   // the button says it already
            if (kt.compare(0, LEAD.size(), LEAD) == 0) kt = kt.substr(LEAD.size());
            if (kt.empty()) kt = cut_possible ? std::string("Dig the ground down along the line.") : std::string("Not possible here.");
        }
        else
        {
            kt = llformat("%d m of cutting, up to %d m deep", cut["length_m"].asInteger(), ll_round((F32)cut["deepest_m"].asReal()));
            const S32 walls = ll_round((F32)cut["walls_m"].asReal());
            if (walls > 0) kt += llformat(", %d m of it with stone walls", walls);
            kt += viaducts(cut);
        }
        setTextIfChanged(mSugCutText, kt);
        mSugCutText->setToolTip(cut.isMap() ? cut["text"].asString() : std::string());
        mSugCutBtn->setEnabled(!b && mMayCut && cut_possible);
        // the climb cannot be chosen when it cannot be made (the plan says so in its words)
        mSugClimbBtn->setEnabled(!b && !(climb.isMap() && climb.has("possible") && climb["possible"].isBoolean() && !climb["possible"].asBoolean()));
    }
    for (LLView* v : { (LLView*)mSide, (LLView*)mStyle, (LLView*)mName }) v->setVisible(!show_tt && !show_sug);
    if (LLView* lbl = findChild<LLView>("station_label")) lbl->setVisible(!show_tt && !show_sug);
    mTTPanel->setEnabled(!b);
    {
        // DESIGN.md 10: the longest train a deck turns is the deck less 1 m (15 / 20 / 24 m: 14 / 19 / 23)
        const S32 size = mTTSize->getValue().asInteger();
        setTextIfChanged(mTTTurns, llformat("Turns trains up to %d m long.", size - TT_SPARE_M));
    }
    const bool station_ctrls = !b && (mMode->getSelectedIndex() == MODE_STATION || station_edit);
    mSide->setEnabled(station_ctrls);
    mStyle->setEnabled(station_ctrls);
    mName->setEnabled(!b && station_edit);
    mItemsList->setEnabled(!b);
    mTakeOutBtn->setEnabled(!b && mItemsList->getFirstSelected() && !mItemsList->getFirstSelected()->getValue().asString().empty());

    std::string summary;
    if (usableLines() > 0) summary = (mPlanDirty || mPlanInFlight) && mPlan.mSummary.empty() ? std::string("Working out your track...") : mPlan.mSummary;
    setTextIfChanged(mSummary, summary);
    std::string problems = mPlan.mProblemText;
    if (!mMayLay)
    {
        const std::string why = "You cannot lay track here" + (mNoLayReason.empty() ? std::string(".") : ": " + mNoLayReason);
        problems = problems.empty() ? why : why + "\n" + problems;
    }
    setTextIfChanged(mProblems, problems);
    mProblems->setColor(LLUIColorTable::instance().getColor(mPlan.mRefused || !mMayLay ? "AlertCautionTextColor" : "LabelTextColor"));

    mLayBtn->setLabel(mReplaces.isDefined() ? std::string("Lay the changed railway") : std::string("Lay track"));
    bool items_unlaid = false;
    for (const Item& it : mItems) items_unlaid = items_unlaid || !it.mLaid;
    mLayBtn->setEnabled(!b && mMayLay && mPlan.mOk && !mPlanDirty && !mPlanInFlight && mPlannedReq.isMap() && onWolf()
                        && !(mItemsFromDrawing && items_unlaid && mLaySerial == mPlanSerial));
    mProgress->setVisible(mLaying || mRezzing || (mRemoving && mProgress->getVisible()));
    LLViewerRegion* rgn = gAgent.getRegion();
    bool unlaid = false, here = false;
    for (const Item& it : mItems)
    {
        if (it.mLaid) continue;
        unlaid = true;
        here = here || (rgn && it.mRegion == rgn->getRegionID());
    }
    // after a rez whose pieces were not all found, Look again (never a second copy beside pieces that may be streaming in)
    mNextPartBtn->setVisible(unlaid && !b && !mPartialShown);
    mNextPartBtn->setEnabled(here);
    mLookAgainBtn->setVisible(mPartialShown && !b);
    mTakeAwayBtn->setVisible(mPartialShown && !b && !mMatched.empty());

    const LLSD picked = pickedLayout();
    const std::string st = picked.isMap() ? picked["state"].asString() : std::string();
    mShowBtn->setEnabled(!b && picked.isMap());
    mChangeBtn->setEnabled(!b && picked.isMap() && st != "building" && st != "failed");
    // [TUNNELS] a part laid here whose tunnel was not opened: the same button opens it (DESIGN.md 3.4)
    bool to_open = false;
    if (picked.isMap() && rgn)
    {
        const std::string r = rgn->getRegionID().asString();
        if (arr(picked["laid"][r]).size() > 0)
            for (const LLSD& t : llsd::inArray(arr(picked["tunnels"][r]))) to_open = to_open || !t["recorded"].asBoolean();
    }
    // [CUTTINGS] a dig or fill that failed here: the same button asks again (DESIGN.md 3.6 "Window")
    bool cut_failed = false, laid_here = false;
    if (!to_open) cuttingWords(picked, cut_failed, laid_here);
    const std::string part_label = to_open ? std::string("Open the tunnel")
                                 : cut_failed ? (laid_here ? std::string("Dig again") : std::string("Put the ground back"))
                                 : std::string("Lay its part here");
    if (mLayPartBtn->getLabelSelected() != part_label)
    {
        mLayPartBtn->setLabel(part_label);
        mLayPartBtn->setToolTip(to_open ? std::string("Open this railway's tunnel through the hill here")
                                : cut_failed ? (laid_here ? std::string("Ask the region again to dig this railway's cutting here")
                                                          : std::string("Ask the region again to put the ground back as it was"))
                                : std::string("Put the part of this railway that is in the region you are in on the land"));
    }
    mLayPartBtn->setEnabled(!b && picked.isMap() && (to_open || cut_failed || st == "built" || st == "partly_laid"));
    mRemoveBtn->setEnabled(!b && picked.isMap() && st != "building" && st != "removed");
    // [CUTTINGS] Dig again / Put the ground back after a dig or fill this window followed failed (never with Look again)
    const bool ground_retry = mGroundRetry.isMap() && !b && !mPartialShown && !mGroundJob.isDefined();
    mGroundBtn->setVisible(ground_retry);
    if (ground_retry)
    {
        const std::string gl = mGroundRetry["action"].asString() == "cut" ? std::string("Dig again") : std::string("Put the ground back");
        if (mGroundBtn->getLabelSelected() != gl) mGroundBtn->setLabel(gl);
    }
}
