/**
 * @file wolfcave.cpp
 * @brief WolfViewer: the Cave Builder — caves dug into the terrain from the Paint window.
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

// [WOLF CAVE 2026-10-12] See wolfcave.h. Source for every behaviour: wolfstorm
// js/world/wolf_caves.js + js/ui/floaters/floater_wolf_caves.js (written together, same day).

#include "llviewerprecompiledheaders.h"

#include "wolfcave.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <boost/json.hpp>

#include "fscommon.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "lldate.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llhttpconstants.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llrand.h"
#include "llrender.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "lltoolcomp.h"
#include "lltooldraganddrop.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvlcomposition.h"
#include "message.h"
#include "wolfgrid.h"
#include "wolfterrainpaint.h"

const char* WolfCaves::API_URL = "https://wolfstorm.app/php/caves.php";

namespace
{
    // Source: wolfstorm/rust_proxy/src/main.rs — POST /cave_mesh routes to wolfcave.rs handle().
    std::string caveMeshUrl() { return std::string(WolfGrid::MESH_API_BASE) + "/cave_mesh"; }

    // Source: TerrainModule.cs:1792-1798 / :1946 — one ModifyLand at most every 20 ms (a request
    // inside the window is DROPPED, as is every ParcelData block after the first in a packet).
    constexpr F32 MODIFY_GAP_SECS = 0.06f;
    // Source: FlattenArea.cs FloodEffect — strength = seconds * 0.04; >= 0.999 sets it exactly.
    constexpr F32 FLATTEN_SECONDS = 25.f;
    // Source: TerrainModule.cs:61-69 StandardTerrainEffects.Flatten = 0.
    constexpr U8 ACTION_FLATTEN = 0;
    constexpr F32 HEIGHT_TOLERANCE = 0.06f;
    // [WolfViewer 2026-10-14 v2] Ground sampled round the rotated box footprint, every tunnel band
    // and every door. [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "/cave_mesh request":
    // "The terrain sample the clients send covers the box footprint, every tunnel and door, plus
    // CAVE_REACH_M + CAVE_MOUND_SKIRT_M, clipped to the region" (CAVE_DESIGN_V3.md "Rights":
    // CAVE_REACH_M (4), CAVE_MOUND_SKIRT_M (26)). Caverns lie inside the box.
    constexpr F32 CAVE_REACH_M = 4.f;
    constexpr F32 CAVE_MOUND_SKIRT_M = 26.f;
    constexpr F32 CAVE_SAMPLE_MARGIN_M = CAVE_REACH_M + CAVE_MOUND_SKIRT_M;
    // [WolfViewer 2026-10-15 v3] A box handle is picked within this many screen pixels (the task
    // brief for the v3 handles: "pick radius ~10 px").
    constexpr F32 HANDLE_PICK_PX = 10.f;
    // Source: rust_proxy/src/wolfcave.rs:88 MAX_GRID_POINTS = 512 * 512 — the sampled grid's limit.
    constexpr S32 MAX_GRID_SIDE = 512;
    // Source: RenderTerrainScale default (app_settings/settings.xml, 12.0): the texgen the shell's
    // ground faces are laid with (lldrawpoolterrain.cpp:430-436). The default, not this viewer's
    // own setting — the mesh is the same for everybody.
    constexpr F32 TERRAIN_SCALE_M = 12.f;

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    void alertOK(const std::string& msg)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", msg));
    }

    F32 r2(F32 v) { return roundf(v * 100.f) / 100.f; }

    /** The cave zone's look in the Wolf Environment zone "set" (wolfstorm php/wolf_env_lib.php format 1). */
    LLSD lightSet(const std::string& look)
    {
        // Same table as wolf_caves.js WolfCaves.INSIDE_LIGHT.
        LLSD s;
        if (look == "dark")         { s["light"] = "#5a6070"; s["lightStrength"] = 30;  s["fogColor"] = "#1a1c22"; s["fogGlow"] = 0;  s["haze"] = 60; }
        else if (look == "torch")   { s["light"] = "#ffb070"; s["lightStrength"] = 90;  s["fogColor"] = "#3a2a1c"; s["fogGlow"] = 20; s["haze"] = 35; }
        else if (look == "crystal") { s["light"] = "#80d0ff"; s["lightStrength"] = 110; s["fogColor"] = "#1c2f3f"; s["fogGlow"] = 45; s["haze"] = 30; }
        else                        { s["light"] = "#8890a0"; s["lightStrength"] = 60;  s["fogColor"] = "#2a2d35"; s["fogGlow"] = 0;  s["haze"] = 40; }
        return s;
    }

    std::string lightOf(const LLSD& zone)
    {
        const std::string l = zone.has("set") ? zone["set"]["light"].asString() : std::string();
        for (const char* k : { "dark", "dim", "torch", "crystal" })
        {
            if (lightSet(k)["light"].asString() == l) return k;
        }
        return "dim";
    }

    /** A request to a Wolf service, carrying the session pair (wolfterrainpaint.cpp saveCoro). */
    LLSD wolfRequest(const std::string& url, const std::string* body, F32 timeout)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfCaves", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        // This request carries the session id: verify the host (wolfgrid.h makeVerifiedHttpOptions).
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout((S32)timeout);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        if (!body)
        {
            return adapter->getRawAndSuspend(request, url, options, headers);
        }
        // [WolfViewer 2026-10-14] No automatic retry of a POST. llcorehttp retries 5 times by
        // default and counts every 5xx and a timeout as retryable, so a slow build POST (180 s)
        // could be sent again: a second rock in Objects, a misleading "built a moment ago"
        // refusal, and the job busy for many minutes. A failed POST is reported instead.
        // Source: llcorehttp/_httpinternal.h:130 HTTP_RETRY_COUNT_DEFAULT = 5;
        // llcorehttp/httpcommon.cpp:236-262 HttpStatus::isRetryable; _httppolicy.cpp:393;
        // llcorehttp/httpoptions.h:104 HttpOptions::setRetries.
        options->setRetries(0);
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body->data(), body->size());
        return adapter->postRawAndSuspend(request, url, raw, options, headers);
    }

    /** status + JSON body of a wolfRequest result. */
    S32 replyOf(const LLSD& result, LLSD& reply, std::string& transport)
    {
        LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        reply = LLSD();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        }
        transport = st ? std::string() : st.toString();
        return st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
    }

    // The raw adapter never keeps the body of a 4xx (llcorehttputil.cpp:269-295 parses it with
    // the handler's parseBody, and HttpCoroRawHandler::parseBody returns nothing), so the
    // service's own words are lost there: say what each php/caves.php status means instead.
    // (/cave_mesh answers its refusals as HTTP 200 + success:false for this reason.)
    // [WolfViewer 2026-10-14] 401 is a sign-in problem, not land rights (Source: wolfstorm
    // php/grid_rights.php:163 wt_authenticate: no credentials at all = 401; :135-159 a bad or
    // expired session = 403, as is caves.php:521 land refused). 404 is not "unknown region":
    // Paul's 404 was caves.php not deployed (a plain web-server 404), and the body that would
    // tell the two apart is dropped above, so the honest generic wording is used.
    std::string caveServiceError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error")) return reply["error"].asString();
        switch (status)
        {
            case 401: return "the grid could not confirm you are signed in - log out and back in, then try again";
            case 403: return "the grid refused it: you may only make or change caves on land you own or hold (if you do, your session may have expired - log out and back in)";
            case 404: return "the cave service is not available on this grid yet";
            case 409: return "someone else changed this region's caves at the same moment - reopen the window and try again";
            case 413: return "too much cave data in one region - remove a cave or make this one smaller";
            case 429: return "too many cave saves - wait a minute and try again";   // [WolfViewer 2026-10-14] php/caves.php: 30 saves per person per 60 s
            case 400: return "the grid refused the cave record as invalid";
            case 503: return "the cave service is not answering - try again in a moment";
            default:  return transport.empty() ? llformat("the cave service answered HTTP %d", status) : transport;
        }
    }

    // [WolfViewer 2026-10-14] The same for /cave_mesh. Its user-facing refusals come as HTTP 200 +
    // success:false with the words in "error" (or "message"), shown as they are; what is left as
    // a non-200 status loses its body (the raw adapter above), so each status gets a clear line.
    // Source: wolfstorm rust_proxy/src/wolfcave.rs:1143-1210 handle() (400 bad request, 403 folder
    // not yours, 500 generator stopped, 502 grid did not store, 503 folder check / session check
    // via main.rs ai_identify, 401 not a Wolf Territories session).
    std::string caveMeshError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        if (reply.isMap() && reply.has("message") && !reply["message"].asString().empty()) return reply["message"].asString();
        if (status == 200) return "the cave generator refused it without saying why";
        switch (status)
        {
            case 400: return "the cave generator could not read the request - update WolfViewer, or make the cave simpler";
            case 401: return "the cave generator could not confirm you are signed in to Wolf Territories - log out and back in, then try again";
            case 403: return "the cave generator refused it: the Objects folder it was given is not yours";
            case 404: return "the cave generator is not available on this grid yet";
            case 413: return "the cave is too big to send to the generator - make it smaller";
            case 429: return "the cave generator is busy with your last request - wait a moment and try again";
            case 500: return "the cave generator stopped unexpectedly - try again, or make the cave simpler";
            case 502: return "the grid did not store the cave's rock - try again in a moment";
            case 503: return "the grid could not check your session or your inventory - try again in a moment";
            default:  return transport.empty() ? llformat("the cave generator answered HTTP %d", status) : transport;
        }
    }

    // [WolfViewer 2026-10-14] Does the record still list a rock? A removal that stops part way
    // (Stop, or leaving the region) after the rock was taken away saves "objects": [] so the next
    // Remove finishes the ground without asking to forget a rock that is already gone.
    bool rockRecorded(const WolfCaves::Cave& c)
    {
        return c.mBuilt.isMap() && c.mBuilt["objects"].isArray() && c.mBuilt["objects"].size() > 0;
    }

    // [WolfViewer 2026-10-14 v2] Plane geometry for the box design (region metres).
    F32 segDist(F32 px, F32 py, F32 ax, F32 ay, F32 bx, F32 by)
    {
        const F32 dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        const F32 t = l2 > 1e-6f ? llclamp(((px - ax) * dx + (py - ay) * dy) / l2, 0.f, 1.f) : 0.f;
        const F32 ex = ax + dx * t - px, ey = ay + dy * t - py;
        return sqrtf(ex * ex + ey * ey);
    }

    /** Distance from x,y to the box's wall line, inside or outside (CAVE_DESIGN_V3.md doors). */
    F32 wallDist(const WolfCaves::Box& b, F32 x, F32 y)
    {
        const F32 a = b.mRot * DEG_TO_RAD, c = cosf(a), s = sinf(a);
        const F32 dx = x - b.mX, dy = y - b.mY;
        // Into the box's own axes (the inverse of the turn in boxCorners).
        const F32 ox = fabsf(dx * c + dy * s) - b.mW * 0.5f, oy = fabsf(-dx * s + dy * c) - b.mD * 0.5f;
        if (ox <= 0.f && oy <= 0.f) return llmin(-ox, -oy);
        const F32 mx = llmax(ox, 0.f), my = llmax(oy, 0.f);
        return sqrtf(mx * mx + my * my);
    }

    /** Distance from x,y to a tunnel's centre line. */
    F32 tunnelDist(const WolfCaves::Tunnel& t, F32 x, F32 y)
    {
        F32 best = 1e9f;
        for (size_t k = 0; k + 3 < t.mP.size(); k += 2) best = llmin(best, segDist(x, y, t.mP[k], t.mP[k + 1], t.mP[k + 2], t.mP[k + 3]));
        return best;
    }

    /** [WolfViewer 2026-10-15 v3] The index of the tunnel x,y lies on (within its half-width,
     *  CAVE_DESIGN_V3.md caverns), the nearest one when several; -1 = none. */
    S32 tunnelUnder(const WolfCaves::Design& d, F32 x, F32 y)
    {
        S32 best = -1;
        F32 best_off = 0.f;
        for (size_t i = 0; i < d.mTunnels.size(); ++i)
        {
            const F32 off = tunnelDist(d.mTunnels[i], x, y) - d.mTunnels[i].mW * 0.5f;
            if (off <= 0.f && (best < 0 || off < best_off)) { best = (S32)i; best_off = off; }
        }
        return best;
    }

    /** [WolfViewer 2026-10-15 v3] 0 <= rot < 360, also after rounding to 0.01. */
    F32 normRot(F32 rot)
    {
        rot = fmodf(rot, 360.f);
        if (rot < 0.f) rot += 360.f;
        if (r2(rot) >= 360.f) rot = 0.f;
        return rot;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfCaves
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfCaves::WolfCaves() {}
WolfCaves::~WolfCaves() {}

// [WOLF TUNNELS 2026-10-22] See wolfcave.h: the shared service helpers, unchanged, for the Tunnel tool.
LLSD WolfCaves::serviceRequest(const std::string& url, const std::string* body, F32 timeout) { return wolfRequest(url, body, timeout); }
S32 WolfCaves::serviceReply(const LLSD& result, LLSD& reply, std::string& transport) { return replyOf(result, reply, transport); }
std::string WolfCaves::meshServiceError(S32 status, const LLSD& reply, const std::string& transport) { return caveMeshError(status, reply, transport); }

// Source: wolf_caves.js onWolf — Wolf Territories (WolfGrid::isOnWolfTerritories, the region's
// own grid, so a hypergrid jump turns it off) and the region known to the grid's database.
bool WolfCaves::onWolf()
{
    return WolfGrid::isOnWolfTerritories() && WolfTerrainPaint::instance().onGrid();
}

// Source: wolf_caves.js WolfCaves.newCave.
WolfCaves::Cave WolfCaves::newCave()
{
    Cave c;
    LLUUID id;
    id.generate();
    c.mId = id.asString();
    c.mDesign.mSeed = (S32)(ll_frand() * 2147483000.f);
    c.mDesign.mRock = terrainRockId();   // [WolfViewer 2026-10-14 v2] Paul: "default to a cave texture"
    return c;   // [WolfViewer 2026-10-15 v3] Box::mH defaults to 8 (CAVE_DESIGN_V3.md)
}

// Source: CAVE_DESIGN_V3.md "rock" (as v2) — OpenSim RegionSettings.cs:116 DEFAULT_TERRAIN_TEXTURE_4.
const LLUUID& WolfCaves::terrainRockId()
{
    static const LLUUID id("beb169c7-11ea-fff2-efe5-0f24dc881df2");
    return id;
}

// Source: CAVE_DESIGN_V3.md "rock" (as v2) — OpenSim library TexturesAssetSet.xml "rocks".
const LLUUID& WolfCaves::rocksId()
{
    static const LLUUID id("00000000-0000-1111-9999-000000000003");
    return id;
}

// Source: CAVE_DESIGN_V3.md "box" — w along the box's own x, d along its own y, rot degrees
// counter-clockwise from east about the centre.
void WolfCaves::boxCorners(const Box& b, F32 out[8])
{
    const F32 a = b.mRot * DEG_TO_RAD, c = cosf(a), s = sinf(a);
    const F32 hw = b.mW * 0.5f, hd = b.mD * 0.5f;
    const F32 lx[4] = { -hw, hw, hw, -hw }, ly[4] = { -hd, -hd, hd, hd };
    for (S32 i = 0; i < 4; ++i)
    {
        out[2 * i] = b.mX + lx[i] * c - ly[i] * s;
        out[2 * i + 1] = b.mY + lx[i] * s + ly[i] * c;
    }
}

// [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "box" — sb, the box signed distance:
// positive inside (to the nearest wall), negative outside (minus the distance to the footprint).
F32 WolfCaves::boxSigned(const Box& b, F32 x, F32 y)
{
    const F32 w = wallDist(b, x, y);
    const F32 a = b.mRot * DEG_TO_RAD, c = cosf(a), s = sinf(a);
    const F32 dx = x - b.mX, dy = y - b.mY;
    const bool inside = fabsf(dx * c + dy * s) <= b.mW * 0.5f && fabsf(-dx * s + dy * c) <= b.mD * 0.5f;
    return inside ? w : -w;
}

// [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "doors" — each must be within 1 m of the box
// wall (inside or outside) AND within (w / 2 + 1 m) of a tunnel.
std::string WolfCaves::doorProblem(const Design& d, F32 x, F32 y)
{
    if (!d.mHasBox) return "Draw the rock box first (step 1).";
    const F32 wall_off = wallDist(d.mBox, x, y) - DOOR_REACH_M;
    if (wall_off > 0.f)
        return llformat("A doorway goes where a tunnel comes out of the rock box - that spot is %.1f m too far from the box wall. Click on the wall where a tunnel meets it.", wall_off);
    if (d.mTunnels.empty()) return "A doorway goes where a tunnel meets the box wall - draw a tunnel to the wall first (step 2).";
    F32 off = 1e9f;
    for (const Tunnel& t : d.mTunnels)
    {
        const F32 o = tunnelDist(t, x, y) - t.mW * 0.5f - DOOR_REACH_M;
        if (o <= 0.f) return std::string();
        off = llmin(off, o);
    }
    return llformat("A doorway goes where a tunnel meets the box wall - that spot is on the wall but %.1f m too far from a tunnel. Click where a tunnel reaches the wall.", off);
}

// [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "caverns" — r in [2, 30]; the centre lies on a
// tunnel (distance to its polyline <= w / 2); the whole disc lies inside the box (sb >= r).
std::string WolfCaves::cavernProblem(const Design& d, F32 x, F32 y, F32 r)
{
    if (!d.mHasBox) return "Draw the rock box first (step 1).";
    if (d.mTunnels.empty()) return "A cavern is a chamber on a tunnel - draw a tunnel first (step 2).";
    if (tunnelUnder(d, x, y) < 0) return "A cavern starts on a tunnel - press inside a tunnel's width, then drag outwards.";
    if (r < CAVERN_R_MIN - 0.005f || r > CAVERN_R_MAX + 0.005f)
        return llformat("A cavern's radius must be %d to %d m.", (S32)CAVERN_R_MIN, (S32)CAVERN_R_MAX);
    const F32 sb = boxSigned(d.mBox, x, y);
    if (sb < r - 0.005f)
    {
        if (sb < CAVERN_R_MIN) return llformat("A cavern must lie wholly inside the rock box - that spot is too near the box wall for even a %d m cavern. Start it further in, or make the box bigger.", (S32)CAVERN_R_MIN);
        return llformat("A cavern must lie wholly inside the rock box - there it can be at most %.1f m across the radius. Make it smaller or the box bigger.", sb);
    }
    return std::string();
}

// [WolfViewer 2026-10-14 v2] Every limit of the stored design, checked here so the window can say
// in words why Generate cannot be pressed (the generator and caves.php refuse the same things, but
// only after a round trip). [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Stored design".
std::string WolfCaves::designProblem(const Design& d, F32 region_w)
{
    if (!d.mHasBox) return "Draw the rock box first: step 1, press Draw the box, then drag on the ground.";
    const Box& b = d.mBox;
    if (b.mW < BOX_SIDE_MIN || b.mW > BOX_SIDE_MAX || b.mD < BOX_SIDE_MIN || b.mD > BOX_SIDE_MAX)
        return llformat("The box must be %d to %d m wide and deep.", (S32)BOX_SIDE_MIN, (S32)BOX_SIDE_MAX);
    if (b.mH < BOX_H_MIN || b.mH > BOX_H_MAX) return llformat("The box height must be %d to %d m.", (S32)BOX_H_MIN, (S32)BOX_H_MAX);
    if (b.mZ < BOX_Z_MIN || b.mZ > BOX_Z_MAX) return "The floor height must be between -200 and 4096 m.";
    F32 c[8];
    boxCorners(b, c);
    for (S32 i = 0; i < 4; ++i)
    {
        if (c[2 * i] < 0.f || c[2 * i] > region_w || c[2 * i + 1] < 0.f || c[2 * i + 1] > region_w)
            return "Part of the box is outside this region - move it further in, or change its size or turn.";
    }
    if (d.mRoof < ROOF_MIN || d.mRoof > ROOF_MAX) return "The rock above must be 0.5 to 6 m thick.";
    // Source: CAVE_DESIGN_V3.md box — the contract's own words.
    if (b.mH - d.mRoof < ROOM_UNDER_ROOF_M) return "The rock box is too low for its roof — make it taller or the roof thinner.";
    if (d.mRockScale < ROCK_SCALE_MIN || d.mRockScale > ROCK_SCALE_MAX) return "The rock texture scale must be 0.5 to 64.";
    if (d.mRoughness < 0.f || d.mRoughness > 1.f) return "Rough walls must be 0 to 1.";
    if (d.mTunnels.empty()) return "Draw at least one tunnel: step 2, press Draw a tunnel, then click points inside the box. The box is solid rock - the tunnels are the way in.";
    if ((S32)d.mTunnels.size() > MAX_TUNNELS) return llformat("At most %d tunnels - press Undo to take one back.", MAX_TUNNELS);
    S32 all = 0;
    for (size_t i = 0; i < d.mTunnels.size(); ++i)
    {
        const Tunnel& t = d.mTunnels[i];
        const S32 n = (S32)t.mP.size() / 2;
        all += n;
        if (n < 2 || n > MAX_TUNNEL_POINTS) return llformat("A tunnel needs 2 to %d points.", MAX_TUNNEL_POINTS);
        if (t.mW < TUNNEL_W_MIN || t.mW > TUNNEL_W_MAX) return "A tunnel must be 2 to 40 m wide.";
        for (S32 k = 0; k < n; ++k)
        {
            // CAVE_DESIGN_V3.md: every point inside the footprint or at most TUNNEL_OUTSIDE_M outside it.
            if (boxSigned(b, t.mP[2 * k], t.mP[2 * k + 1]) < -TUNNEL_OUTSIDE_M)
                return llformat("Tunnel %d goes more than 1 m outside the rock box. Tunnels are carved through the box - make the box bigger (drag its handles) or press Undo.", (S32)i + 1);
        }
    }
    if (all > MAX_ALL_TUNNEL_POINTS) return llformat("The tunnels have more than %d points in all - press Undo to take one back.", MAX_ALL_TUNNEL_POINTS);
    if ((S32)d.mCaverns.size() > MAX_CAVERNS) return llformat("At most %d caverns - press Undo to take one back.", MAX_CAVERNS);
    for (size_t i = 0; i < d.mCaverns.size(); ++i)
    {
        const Cavern& cv = d.mCaverns[i];
        if (cv.mH < CAVERN_H_MIN || cv.mH > CAVERN_H_MAX) return llformat("Cavern %d must be 2.5 to 20 m high.", (S32)i + 1);
        const std::string why = cavernProblem(d, cv.mX, cv.mY, cv.mR);
        if (!why.empty()) return llformat("Cavern %d: ", (S32)i + 1) + why;
    }
    if (d.mDoors.empty()) return "Add at least one doorway: step 4, press Add doorways, then click where a tunnel meets the box wall.";
    if ((S32)d.mDoors.size() > MAX_DOORS) return llformat("At most %d doorways - press Undo to take one back.", MAX_DOORS);
    for (size_t i = 0; i < d.mDoors.size(); ++i)
    {
        if (!doorProblem(d, d.mDoors[i].mX, d.mDoors[i].mY).empty())
            return llformat("Doorway %d is no longer where a tunnel meets the box wall (the box or a tunnel was changed). Put it back, or press Undo until that doorway goes.", (S32)i + 1);
    }
    return std::string();
}

const WolfCaves::Cave* WolfCaves::find(const std::string& id) const
{
    for (const Cave& c : mCaves) if (c.mId == id) return &c;
    return nullptr;
}

// Source: php/caves.php header — the stored CAVE shape.
LLSD WolfCaves::toLLSD(const Cave& c)
{
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Caves from the old builder" — an old
    // cave goes back exactly as it was received ("WolfViewer keeps the cave's raw LLSD").
    if (c.mOld) return c.mRaw;
    LLSD out;
    out["id"] = c.mId;
    out["name"] = c.mName;
    out["light"] = c.mLight;   // php/caves.php CAVE_LIGHTS
    if (c.mOwner.isDefined()) out["owner"] = c.mOwner;
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Stored design" (schema v 3), field
    // for field; the same object is the /cave_mesh `cave` (meshRequest). `dig` is caves.php's own
    // field and is never sent from here.
    const Design& dz = c.mDesign;
    LLSD d;
    d["v"] = 3;
    if (dz.mHasBox)
    {
        const Box& b = dz.mBox;
        const F32 rot = normRot(b.mRot);   // 0 <= rot < 360 after rounding too
        d["box"] = LLSD().with("x", r2(b.mX)).with("y", r2(b.mY)).with("w", r2(b.mW)).with("d", r2(b.mD))
                         .with("rot", r2(rot)).with("z", r2(b.mZ)).with("h", r2(b.mH));
    }
    LLSD ts = LLSD::emptyArray();
    for (const Tunnel& t : dz.mTunnels)
    {
        LLSD pp = LLSD::emptyArray();
        for (F32 v : t.mP) pp.append(r2(v));
        ts.append(LLSD().with("p", pp).with("w", r2(t.mW)));
    }
    LLSD cs = LLSD::emptyArray();
    for (const Cavern& cv : dz.mCaverns) cs.append(LLSD().with("x", r2(cv.mX)).with("y", r2(cv.mY)).with("r", r2(cv.mR)).with("h", r2(cv.mH)));
    LLSD ds = LLSD::emptyArray();
    for (const Door& e : dz.mDoors) ds.append(LLSD().with("x", r2(e.mX)).with("y", r2(e.mY)));
    d["tunnels"] = ts;
    d["caverns"] = cs;
    d["doors"] = ds;
    d["roof"] = r2(dz.mRoof);
    d["rock"] = (dz.mRock.notNull() ? dz.mRock : terrainRockId()).asString();
    d["rockScale"] = r2(dz.mRockScale);
    d["roughness"] = r2(dz.mRoughness);
    d["seed"] = dz.mSeed;
    out["design"] = d;
    out["built"] = c.mBuilt;
    out["zone"] = c.mZone;
    out["lights"] = c.mLights;   // [WOLF CAVE LIGHTS 2026-10-23] php/caves.php "lights" (undefined = null = none)
    return out;
}

WolfCaves::Cave WolfCaves::fromLLSD(const LLSD& in)
{
    Cave c;
    c.mId = in["id"].asString();
    c.mName = in["name"].asString();
    c.mOwner = in["owner"];
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Fields caves.php sets" — per cave, next
    // to owner; a stored cave without it counts as false.
    c.mDig = in.has("dig") && in["dig"].asBoolean();
    const LLSD& d = in["design"];
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Caves from the old builder": anything not
    // v 3 is kept byte for byte (mRaw) and only listed and removable. Its box and built record are
    // still read below, for the list and for Remove (findRock, the ground to put back).
    c.mOld = d["v"].asInteger() != 3;
    if (c.mOld) c.mRaw = in;
    Design& dz = c.mDesign;
    if (d.has("box") && d["box"].isMap())
    {
        const LLSD& b = d["box"];
        dz.mHasBox = true;
        dz.mBox = Box{ (F32)b["x"].asReal(), (F32)b["y"].asReal(), (F32)b["w"].asReal(), (F32)b["d"].asReal(),
                       (F32)b["rot"].asReal(), (F32)b["z"].asReal(), (F32)b["h"].asReal() };
    }
    for (const LLSD& t : llsd::inArray(d["tunnels"]))
    {
        Tunnel tt;
        for (const LLSD& v : llsd::inArray(t["p"])) tt.mP.push_back((F32)v.asReal());
        tt.mW = (F32)t["w"].asReal();
        if (tt.mP.size() >= 4 && !(tt.mP.size() & 1)) dz.mTunnels.push_back(tt);
    }
    for (const LLSD& cv : llsd::inArray(d["caverns"]))
    {
        dz.mCaverns.push_back(Cavern{ (F32)cv["x"].asReal(), (F32)cv["y"].asReal(), (F32)cv["r"].asReal(), (F32)cv["h"].asReal() });
    }
    for (const LLSD& e : llsd::inArray(d["doors"]))
    {
        dz.mDoors.push_back(Door{ (F32)e["x"].asReal(), (F32)e["y"].asReal() });
    }
    dz.mRoof = d.has("roof") ? (F32)d["roof"].asReal() : 1.5f;
    dz.mRock = d["rock"].asUUID();
    if (dz.mRock.isNull()) dz.mRock = terrainRockId();
    dz.mRockScale = d.has("rockScale") ? (F32)d["rockScale"].asReal() : 4.f;
    dz.mRoughness = d.has("roughness") ? (F32)d["roughness"].asReal() : 0.5f;
    dz.mSeed = d["seed"].asInteger();
    c.mBuilt = in["built"];
    c.mZone = in["zone"];
    c.mLight = in.has("light") ? in["light"].asString() : lightOf(c.mZone);
    c.mLights = in["lights"].isMap() ? in["lights"] : LLSD();   // [WOLF CAVE LIGHTS 2026-10-23]
    return c;
}

void WolfCaves::refresh()
{
    if (!onWolf() || mFetching) return;
    mFetching = true;
    LLCoros::instance().launch("WolfCaves fetch", []() { WolfCaves::instance().fetchCoro(); });
}

// Source: wolf_caves.js refresh — GET ?region=<uuid>. [WolfViewer 2026-10-15 v3] + &rights=1 for the
// region's dig hint (CAVE_DESIGN_V3.md "Dig hint for the clients"; this GET carries the session).
void WolfCaves::fetchCoro()
{
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    // [WOLF GRID GATE 2026-10-17] Confirmed, as sendOk: this GET carries the session, and the paint record is found by
    // region handle, which right after a hypergrid jump can be a foreign region's
    if (!onWolf() || !WolfGrid::isOnWolfTerritoriesConfirmed() || !r) { mFetching = false; return; }
    const std::string uuid = r->mUuid;
    const U64 handle = r->mHandle;
    // [WolfViewer 2026-10-14] Another region: forget the old one's caves NOW, so a failed fetch
    // cannot leave them listed (and openable, previewable, buildable) here.
    if (uuid != mRegionUuid || handle != mHandle)
    {
        mCaves.clear();
        mRegionUuid.clear();
        mHandle = 0;
        mVersion = 0;
        mDigHint.clear();
        clearPreview();
    }
    LLSD result = wolfRequest(std::string(API_URL) + "?region=" + uuid + "&rights=1", nullptr, 20.f);
    mFetching = false;
    LLSD reply;
    std::string transport;
    const S32 status = replyOf(result, reply, transport);
    if (status != 200 || !reply["success"].asBoolean() || !reply["regions"].isArray() || reply["regions"].size() == 0)
    {
        fail("Could not load this region's caves: " + caveServiceError(status, reply, transport));
        return;
    }
    const LLSD& reg = reply["regions"][0];
    mRegionUuid = uuid;
    mHandle = handle;
    mVersion = reg["version"].asInteger();
    // "region" | "parcels" | "none"; absent (an older caves.php, or not proven) counts as "none".
    const std::string hint = reg["dig"].asString();
    if (hint != mDigHint) clearPreview();   // a preview made with the other dig no longer applies
    mDigHint = hint;
    mCaves.clear();
    for (const LLSD& c : llsd::inArray(reg["layout"]["caves"])) mCaves.push_back(fromLLSD(c));
}

// Source: wolf_caves.js _store — POST {region, layout, version}; 409 = someone saved first.
bool WolfCaves::storeCaves(const std::vector<Cave>& caves, std::string& error, S32* http_status)
{
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    if (!r || r->mUuid != mRegionUuid) { error = "You have left the region these caves belong to."; return false; }
    LLSD list = LLSD::emptyArray();
    for (const Cave& c : caves) list.append(toLLSD(c));
    LLSD body;
    body["region"] = mRegionUuid;
    body["layout"] = LLSD().with("v", 1).with("caves", list);
    body["version"] = mVersion;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = wolfRequest(API_URL, &text, 30.f);
    LLSD reply;
    std::string transport;
    const S32 status = replyOf(result, reply, transport);
    if (http_status) *http_status = status;
    if (status != 200 || !reply["success"].asBoolean())
    {
        error = caveServiceError(status, reply, transport);
        return false;
    }
    for (const LLSD& n : llsd::inArray(reply["notes"])) fail(n.asString());   // e.g. a light zone left out
    const LLSD& reg = reply["region"];
    mVersion = reg["version"].asInteger();
    mCaves.clear();
    for (const LLSD& c : llsd::inArray(reg["layout"]["caves"])) mCaves.push_back(fromLLSD(c));
    return true;
}

// Replace (or add) one cave; once more against the fresh list on a 409 (wolf_caves.js _putCave).
bool WolfCaves::putCave(const Cave& cave, std::string& error, S32* http_status)
{
    S32 status = 0;
    for (S32 attempt = 0; attempt < 2; ++attempt)
    {
        std::vector<Cave> list;
        for (const Cave& c : mCaves) if (c.mId != cave.mId) list.push_back(c);
        list.push_back(cave);
        if (storeCaves(list, error, &status)) { if (http_status) *http_status = status; return true; }
        if (status != 409 || attempt) break;
        mFetching = true;
        fetchCoro();
    }
    if (http_status) *http_status = status;
    return false;
}

// Drop one cave; once more against the fresh list on a 409.
bool WolfCaves::dropCave(const std::string& id, std::string& error)
{
    S32 status = 0;
    for (S32 attempt = 0; attempt < 2; ++attempt)
    {
        std::vector<Cave> list;
        for (const Cave& c : mCaves) if (c.mId != id) list.push_back(c);
        if (storeCaves(list, error, &status)) return true;
        if (status != 409 || attempt) break;
        mFetching = true;
        fetchCoro();
    }
    return false;
}

bool WolfCaves::sendOk() const
{
    return recordHere() && WolfGrid::isOnWolfTerritoriesConfirmed() && onWolf();
}

bool WolfCaves::recordHere() const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    // mRegionUuid, not mHandle != 0, says "loaded": handle 0 is a real place (grid 0,0).
    return rgn && !mRegionUuid.empty() && rgn->getHandle() == mHandle;
}

std::string WolfCaves::takeError()
{
    std::string all;
    for (const std::string& e : mErrors) all += (all.empty() ? "" : "\n\n") + e;
    mErrors.clear();
    return all;
}

// Source: wolf_caves.js designBox + sampleGround — heights and composition at every metre of the
// outline's box. The composition is the terrain's own per-vertex value
// (llsurfacepatch.cpp:403 getCompositionXY); the heights are the region's surface grid.
bool WolfCaves::sampleGround(const Design& d, LLSD& terrain, std::string& error) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) { error = "You are not in a region."; return false; }
    // [WolfViewer 2026-10-14 v2] The box is the cave: without it there is nothing to sample.
    if (!d.mHasBox) { error = "Draw the rock box first (step 1)."; return false; }
    F32 x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    auto grow = [&](F32 x, F32 y, F32 r) { x0 = llmin(x0, x - r); y0 = llmin(y0, y - r); x1 = llmax(x1, x + r); y1 = llmax(y1, y + r); };
    // CAVE_DESIGN_V3.md: the sample covers the ROTATED box footprint, every tunnel and every door
    // (plus CAVE_SAMPLE_MARGIN_M below). Caverns lie inside the box; they are grown in as well so
    // the sample can never be short of one, whatever the design.
    F32 corners[8];
    boxCorners(d.mBox, corners);
    for (S32 i = 0; i < 4; ++i) grow(corners[2 * i], corners[2 * i + 1], 0.f);
    for (const Tunnel& t : d.mTunnels) for (size_t k = 0; k + 1 < t.mP.size(); k += 2) grow(t.mP[k], t.mP[k + 1], t.mW * 0.5f);
    for (const Cavern& cv : d.mCaverns) grow(cv.mX, cv.mY, cv.mR);
    for (const Door& e : d.mDoors) grow(e.mX, e.mY, DOOR_REACH_M);
    const S32 size = (S32)rgn->getWidth();
    const S32 bx0 = llmax(0, (S32)floorf(x0 - CAVE_SAMPLE_MARGIN_M)), by0 = llmax(0, (S32)floorf(y0 - CAVE_SAMPLE_MARGIN_M));
    const S32 bx1 = llmin(size - 1, (S32)ceilf(x1 + CAVE_SAMPLE_MARGIN_M)), by1 = llmin(size - 1, (S32)ceilf(y1 + CAVE_SAMPLE_MARGIN_M));
    // [WolfViewer 2026-10-14] Refuse a design the generator would refuse anyway, BEFORE sampling:
    // on a var-region a long drawing would otherwise build millions of LLSD points here on the
    // main thread. [v2] The limit is the generator's grid (MAX_GRID_SIDE, wolfcave.rs:88), and the
    // cave must fit with the margin all round, so the check does not depend on the region edge.
    // floor/ceil add up to 1 m each side and the grid counts both ends: points = span + 2 margin + 3.
    const F32 max_span = (F32)MAX_GRID_SIDE - 3.f - 2.f * CAVE_SAMPLE_MARGIN_M;
    if (x1 - x0 > max_span || y1 - y0 > max_span)
    {
        error = llformat("a cave with its tunnels fits in %d m each way and this one is %d x %d m - make it smaller (Undo last).",
                         (S32)max_span, (S32)ceilf(x1 - x0), (S32)ceilf(y1 - y0));
        return false;
    }
    if (bx1 < bx0 || by1 < by0) { error = "The cave is outside this region."; return false; }
    if (bx1 - bx0 + 1 > MAX_GRID_SIDE || by1 - by0 + 1 > MAX_GRID_SIDE)
    {
        error = "the cave covers too much ground for one build - make it smaller (Undo last).";
        return false;
    }
    const LLSurface& land = rgn->getLand();
    LLSD h = LLSD::emptyArray(), comp = LLSD::emptyArray();
    for (S32 y = by0; y <= by1; ++y)
    {
        for (S32 x = bx0; x <= bx1; ++x)
        {
            LLSurfacePatch* patch = land.resolvePatchRegion((F32)x, (F32)y);
            if (!patch || !patch->getHasReceivedData())
            {
                error = "The ground here has not finished loading — wait a moment and try again.";
                return false;
            }
            h.append(roundf(land.resolveHeightRegion((F32)x, (F32)y) * 1000.f) / 1000.f);
            comp.append(roundf(rgn->getCompositionXY(x, y) * 1000.f) / 1000.f);
        }
    }
    LLSD tex = LLSD::emptyArray();
    LLVLComposition* cp = rgn->getComposition();
    for (S32 i = 0; i < 4; ++i)
    {
        const LLUUID id = cp ? cp->getDetailAssetID(i) : LLUUID::null;
        if (id.isNull()) { error = "This region's ground textures are not known yet."; return false; }
        tex.append(id.asString());
    }
    terrain = LLSD();
    terrain["x0"] = bx0;
    terrain["y0"] = by0;
    terrain["nx"] = bx1 - bx0 + 1;
    terrain["ny"] = by1 - by0 + 1;
    terrain["h"] = h;
    terrain["comp"] = comp;
    terrain["textures"] = tex;
    terrain["scale_m"] = TERRAIN_SCALE_M;
    return true;
}

// Source: wolf_caves.js _meshRequest (rust_proxy wolfcave.rs CaveRequest).
LLSD WolfCaves::meshRequest(const Cave& cave, const LLSD& terrain, bool dig) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    LLSD body;
    const LLVector3d origin = rgn ? rgn->getOriginGlobal() : LLVector3d();
    body["region"] = LLSD().with("origin_x", origin.mdV[VX]).with("origin_y", origin.mdV[VY])
                           .with("size_x", rgn ? (S32)rgn->getWidth() : 256).with("size_y", rgn ? (S32)rgn->getWidth() : 256)
                           .with("water", rgn ? rgn->getWaterHeight() : 20.f);
    body["terrain"] = terrain;
    const WolfTerrainPaint::Record* rec = WolfTerrainPaint::instance().current();
    if (rec && rec->mStored)
    {
        LLSD paint;
        LLSD tx = LLSD::emptyArray();
        for (S32 i = 0; i < WolfTerrainPaint::SLOTS; ++i)
        {
            const WolfTerrainPaint::Slot& s = rec->mTextures[i];
            tx.append(s.mId.isNull() ? LLSD() : LLSD().with("id", s.mId.asString()).with("scale", s.mScale));
        }
        LLSD st = LLSD::emptyArray();
        // [WolfViewer 2026-10-14] Only strokes whose band reaches the sampled ground matter (the
        // generator bounds its work by it); the rest of the region's paint is not sent.
        // Source: wolfstorm js/world/wolf_caves.js:257-270 _meshRequest near().
        const F32 bx0 = (F32)terrain["x0"].asInteger(), by0 = (F32)terrain["y0"].asInteger();
        const F32 bx1 = bx0 + (F32)terrain["nx"].asInteger(), by1 = by0 + (F32)terrain["ny"].asInteger();
        auto nearBox = [&](const WolfTerrainPaint::Stroke& s)   // [w66 CI] "near" is a macro on Windows (minwindef.h)
        {
            const F32 r = s.mWidth * 0.5f;
            for (size_t k = 0; k + 1 < s.mPoints.size(); k += 2)
            {
                if (s.mPoints[k] + r >= bx0 && s.mPoints[k] - r <= bx1 && s.mPoints[k + 1] + r >= by0 && s.mPoints[k + 1] - r <= by1) return true;
            }
            return false;
        };
        for (const WolfTerrainPaint::Stroke& s : rec->mStrokes)
        {
            if (s.mSlot < -1) continue;   // water is not ground paint
            if (!nearBox(s)) continue;
            LLSD p = LLSD::emptyArray();
            for (F32 v : s.mPoints) p.append(v);
            st.append(LLSD().with("t", s.mSlot).with("w", s.mWidth).with("p", p));
        }
        paint["textures"] = tx;
        paint["strokes"] = st;
        body["paint"] = paint;
    }
    // [WolfViewer 2026-10-14 v2] `cave` is the stored design itself (same fields, same rounding), so
    // the proxy's build gate, which compares it with the saved record, sees exactly what was saved.
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "/cave_mesh request" — new top-level
    // `dig` (bool): a preview sends the region hint's answer, a build the stored cave's `dig`.
    body["dig"] = dig;
    body["cave"] = toLLSD(cave)["design"];
    return body;
}

bool WolfCaves::postMesh(const std::string& mode, const LLSD& body, LLSD& reply, std::string& error) const
{
    LLSD b = body;
    b["mode"] = mode;
    const std::string text = boost::json::serialize(LlsdToJson(b));
    LLSD result = wolfRequest(caveMeshUrl(), &text, 180.f);
    std::string transport;
    const S32 status = replyOf(result, reply, transport);
    if (status != 200 || !reply["success"].asBoolean())
    {
        // [WolfViewer 2026-10-14] success:false words verbatim, a clear line per status otherwise.
        error = caveMeshError(status, reply, transport);
        return false;
    }
    return true;
}

void WolfCaves::requestPreview(const Cave& cave)
{
    if (!onWolf()) return;
    const U32 req = ++mPreviewRequest;
    // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "Dig hint": dig = the hint is "region" or "parcels".
    const bool dig = previewDig();
    LLCoros::instance().launch("WolfCaves preview", [cave, req, dig]()
    {
        WolfCaves& wc = WolfCaves::instance();
        LLSD terrain, reply;
        std::string err;
        // [WolfViewer 2026-10-14] Only against the region the record belongs to: after a crossing
        // (or a failed fetch) a preview would sample one region's ground for another's design.
        if (!wc.recordHere()) err = "this region's caves have not loaded yet - wait a moment, or reopen the window.";
        if (!err.empty() || !wc.sampleGround(cave.mDesign, terrain, err) || !wc.postMesh("preview", wc.meshRequest(cave, terrain, dig), reply, err))
        {
            if (req != wc.mPreviewRequest) return;
            wc.mPreview = Preview();
            wc.mPreviewError = err;
            ++wc.mPreviewSerial;
            return;
        }
        if (req != wc.mPreviewRequest) return;   // a newer design is on its way
        Preview p;
        p.mReady = true;
        p.mLi = reply["li"].asInteger();
        p.mTargets = (S32)reply["targets"].size() / 4;
        // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md: the summary echoes "dig". A generator from
        // before v3 does not; then what was asked for is what it did.
        p.mDig = reply.has("dig") ? reply["dig"].asBoolean() : dig;
        for (const LLSD& w : llsd::inArray(reply["warnings"])) p.mWarnings.push_back(w.asString());
        for (const LLSD& prim : llsd::inArray(reply["prims"]))
        {
            for (const LLSD& f : llsd::inArray(prim["geometry"]))
            {
                GhostFace gf;
                gf.mRock = f["rock"].asBoolean();
                const LLSD& pos = f["p"];
                for (const LLSD& i : llsd::inArray(f["i"]))
                {
                    const S32 k = i.asInteger() * 3;
                    if (k + 2 >= (S32)pos.size()) continue;
                    gf.mTris.emplace_back((F32)pos[k].asReal(), (F32)pos[k + 1].asReal(), (F32)pos[k + 2].asReal());
                }
                p.mFaces.push_back(gf);
            }
        }
        wc.mPreview = p;
        wc.mPreviewError.clear();
        ++wc.mPreviewSerial;
    });
}

// Source: wolf_caves.js _zone — the Wolf Environment zone schema + kind + z.
// [WolfViewer 2026-10-14 v2] The zone spans the rotated box and the tunnels; its height is the
// box's floor to its top (CAVE_DESIGN_V3.md: the floor is z everywhere inside, and no tunnel or
// cavern reaches above z + h - roof), so it does not need the generator's reply.
LLSD WolfCaves::zoneFor(const Cave& cave) const
{
    F32 x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
    auto grow = [&](F32 x, F32 y, F32 r) { x1 = llmin(x1, x - r); y1 = llmin(y1, y - r); x2 = llmax(x2, x + r); y2 = llmax(y2, y + r); };
    F32 corners[8];
    boxCorners(cave.mDesign.mBox, corners);
    for (S32 i = 0; i < 4; ++i) grow(corners[2 * i], corners[2 * i + 1], 0.f);
    for (const Tunnel& t : cave.mDesign.mTunnels) for (size_t k = 0; k + 1 < t.mP.size(); k += 2) grow(t.mP[k], t.mP[k + 1], t.mW * 0.5f);
    LLViewerRegion* rgn = gAgent.getRegion();
    const F32 w = rgn ? rgn->getWidth() : 256.f;
    std::string hex = cave.mId;
    hex.erase(std::remove(hex.begin(), hex.end(), '-'), hex.end());
    LLSD z;
    z["id"] = "cave-" + hex.substr(0, 12);
    z["shape"] = LLSD().with("type", "rect").with("x1", r2(llclamp(x1, 0.f, w))).with("y1", r2(llclamp(y1, 0.f, w)))
                       .with("x2", r2(llclamp(x2, 0.f, w))).with("y2", r2(llclamp(y2, 0.f, w)));
    z["edge"] = 2;
    z["z"] = LLSD().with("min", r2(cave.mDesign.mBox.mZ - 1.f))
                   .with("max", r2(cave.mDesign.mBox.mZ + cave.mDesign.mBox.mH + 1.f));
    z["set"] = lightSet(cave.mLight);
    return z;
}

// Source: wolf_caves.js _rezExact. The proxy made the item through Robust directly (no
// BulkUpdateInventory from the sim), so the Objects folder is fetched until it shows up
// (wolfmeshupload.cpp does the same refetch).
bool WolfCaves::rezExact(const LLUUID& item_id, const LLUUID& folder_id, const LLVector3& ray_end, const LLVector3& root, LLUUID& rock, std::string& error)
{
    LLViewerInventoryItem* item = nullptr;
    for (S32 i = 0; i < 20 && !item; ++i)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        if (cat) cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
        gInventory.fetchDescendentsOf(folder_id);
        llcoro::suspendUntilTimeout(1.f);
        item = gInventory.getItem(item_id);
        if (item && !item->isFinished()) item = nullptr;
    }
    if (!item)
    {
        error = "The rock was stored in your inventory (Objects), but this viewer could not see it to rez it. Rez it by hand, then press Build again.";
        return false;
    }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || !sendOk()) { error = "You left the region before the rock was rezzed; it is in your Objects folder."; return false; }
    std::set<LLUUID> before;
    for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->isRoot()) before.insert(o->getID());
    }
    // Source: wolfvehiclerez.cpp sendRezObject / lltooldraganddrop.cpp:1970-2028 dropObject — the
    // same RezObject, with the placement made exact: RayEndIsIntersection = 1 makes OpenSim take
    // RayEnd as the point (Scene.cs:2697-2706 GetNewRezLocation), and RayStart = RayEnd keeps its
    // water crossing test (Scene.cs:2561) from moving it. rust_proxy wolfcave.rs computed RayEnd so
    // the ROOT lands at `root` (InventoryAccessModule.cs:983-989 + SceneObjectGroup.cs:2135-2149).
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_RezObject);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
    msg->nextBlock("RezData");
    msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
    msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
    msg->addVector3Fast(_PREHASH_RayStart, ray_end);
    msg->addVector3Fast(_PREHASH_RayEnd, ray_end);
    msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
    msg->addBOOLFast(_PREHASH_RayEndIsIntersection, true);
    msg->addBOOLFast(_PREHASH_RezSelected, false);
    msg->addBOOLFast(_PREHASH_RemoveItem, false);
    pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
    msg->nextBlockFast(_PREHASH_InventoryData);
    item->packMessage(msg);
    msg->sendReliable(rgn->getHost());
    FSCommon::sObjectAddMsg = 0;   // lltooldraganddrop.cpp:2030-2032: no build defaults for it

    for (S32 t = 0; t < 60; ++t)
    {
        llcoro::suspendUntilTimeout(0.5f);
        if (!gAgent.getRegion() || gAgent.getRegion()->getHandle() != mHandle)
        {
            error = "You left the region while the rock was rezzing; if it rezzed, it is in the old region (and the cave was not dug).";
            return false;
        }
        for (S32 i = 0, n = gObjectList.getNumObjects(); i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!o || o->isDead() || !o->isRoot() || before.count(o->getID()) || o->getRegion() != gAgent.getRegion()) continue;
            if (dist_vec(o->getPositionRegion(), root) > 1.f) continue;
            if (!o->permYouOwner()) continue;
            rock = o->getID();
            return true;
        }
    }
    error = "The region did not rez the rock. Check that you may build here and that the parcel has room for it; the rock is in your Objects folder.";
    return false;
}

// Source: wolf_caves.js _shape — runs along y per column with one height, one flatten flood each
// (TerrainModule.cs:1859-1862: the flood covers (int)west..(int)east x (int)south..(int)north
// inclusive, so +0.5 keeps it on the point), then a read-back and one more pass where it did not
// take. Returns [x, y, wanted, got] for every point a limit or a right held.
std::vector<LLSD> WolfCaves::shape(const LLSD& targets, bool to_target, const std::string& what)
{
    struct P { S32 x, y; F32 z; };
    std::vector<P> pts;
    for (S32 k = 0; k + 3 < (S32)targets.size(); k += 4)
    {
        pts.push_back(P{ targets[k].asInteger(), targets[k + 1].asInteger(), (F32)targets[to_target ? k + 3 : k + 2].asReal() });
    }
    // [WolfViewer 2026-10-14] Put-back merges a run while its heights stay within a band and
    // floods it at the band's middle; before, every put-back point was its own flood (the
    // ground's heights all differ), 20-40 minutes for a big cave.
    // Why the terrain stays right: a flatten flood with strength >= 0.999 SETS every point of
    // the box to the one height (Source: OpenSim FloodBrushes/FlattenArea.cs:38-58
    // FloodEffect; FLATTEN_SECONDS * 0.04 = 1.0), so each point of a merged run lands within
    // half the band of its recorded height: PUT_BACK_BAND_M 0.04 -> at most 0.02 m off, under
    // the read-back's HEIGHT_TOLERANCE 0.06 (so it is not sent again) and about the precision
    // the heights were recorded with (rounded to 0.01 m, php/caves.php:343, from the viewer's
    // own LayerData samples). Digging is unchanged (wolf_caves.js _shape: same height as the
    // run's first point within 0.005, flooded at that height): the floor is level anyway.
    constexpr F32 PUT_BACK_BAND_M = 0.04f;
    auto send = [&](const std::vector<P>& list) -> bool
    {
        std::map<S32, std::vector<P>> cols;
        for (const P& p : list) cols[p.x].push_back(p);
        struct Run { S32 x, y0, y1; F32 z; };
        std::vector<Run> runs;
        for (auto& kv : cols)
        {
            std::vector<P>& col = kv.second;
            std::sort(col.begin(), col.end(), [](const P& a, const P& b) { return a.y < b.y; });
            size_t s = 0;
            F32 lo = col.empty() ? 0.f : col[0].z, hi = lo;
            for (size_t i = 1; i <= col.size(); ++i)
            {
                const bool same = i < col.size() && (to_target ? fabsf(col[i].z - col[s].z) < 0.005f
                                                               : llmax(hi, col[i].z) - llmin(lo, col[i].z) <= PUT_BACK_BAND_M);
                if (i < col.size() && col[i].y == col[i - 1].y + 1 && same)
                {
                    lo = llmin(lo, col[i].z);
                    hi = llmax(hi, col[i].z);
                    continue;
                }
                runs.push_back(Run{ kv.first, col[s].y, col[i - 1].y, to_target ? col[s].z : (lo + hi) * 0.5f });
                s = i;
                if (i < col.size()) lo = hi = col[i].z;
            }
        }
        S32 n = 0;
        for (const Run& r : runs)
        {
            // [WolfViewer 2026-10-14] Cancel stops between floods: every flood already sent is
            // whole, and the caller records where the job stood.
            if (mCancel) return false;
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!rgn || !sendOk()) return false;
            // Source: lltoolbrush.cpp:309-344 modifyLandInSelectionGlobal — the same message.
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_ModifyLand);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ModifyBlock);
            msg->addU8Fast(_PREHASH_Action, ACTION_FLATTEN);
            msg->addU8Fast(_PREHASH_BrushSize, (U8)0);
            msg->addF32Fast(_PREHASH_Seconds, FLATTEN_SECONDS);
            msg->addF32Fast(_PREHASH_Height, r.z);
            msg->nextBlockFast(_PREHASH_ParcelData);
            msg->addS32Fast(_PREHASH_LocalID, -1);
            msg->addF32Fast(_PREHASH_West, (F32)r.x);
            msg->addF32Fast(_PREHASH_South, (F32)r.y0);
            msg->addF32Fast(_PREHASH_East, (F32)r.x + 0.5f);
            msg->addF32Fast(_PREHASH_North, (F32)r.y1 + 0.5f);
            msg->nextBlock("ModifyBlockExtended");
            msg->addF32("BrushSize", 1.f);
            // Reliable (lltoolbrush.cpp sends a brush dab unreliably; a dropped flood here would be
            // a hole in the cave floor that the read-back then has to find).
            msg->sendReliable(rgn->getHost());
            if (++n % 20 == 0) mStatus = llformat("%s — %d of %d…", what.c_str(), n, (S32)runs.size());
            llcoro::suspendUntilTimeout(MODIFY_GAP_SECS);
        }
        return true;
    };
    auto misses = [&]() -> std::vector<P>
    {
        llcoro::suspendUntilTimeout(3.f);   // LayerData for the changed patches
        std::vector<P> left;
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return pts;
        for (const P& p : pts)
        {
            if (fabsf(rgn->getLand().resolveHeightRegion((F32)p.x, (F32)p.y) - p.z) > HEIGHT_TOLERANCE) left.push_back(p);
        }
        return left;
    };
    std::vector<LLSD> out;
    if (!send(pts))
    {
        for (const P& p : pts) out.push_back(LLSD().with(0, p.x).with(1, p.y).with(2, p.z).with(3, LLSD()));
        return out;
    }
    std::vector<P> left = misses();
    if (!left.empty() && !mCancel && send(left)) left = misses();
    LLViewerRegion* rgn = gAgent.getRegion();
    for (const P& p : left)
    {
        LLSD e = LLSD::emptyArray();
        e.append(p.x); e.append(p.y); e.append(r2(p.z));
        e.append(rgn ? LLSD(r2(rgn->getLand().resolveHeightRegion((F32)p.x, (F32)p.y))) : LLSD());
        out.push_back(e);
    }
    return out;
}

// [WolfViewer 2026-10-14] Accumulates: every message of the job reaches the window.
void WolfCaves::fail(const std::string& msg)
{
    if (mErrors.empty() || mErrors.back() != msg) mErrors.push_back(msg);
    LL_WARNS("WolfCaves") << msg << LL_ENDL;
}

void WolfCaves::build(const Cave& cave)
{
    // [WolfViewer 2026-10-14] Never refuse silently. Off Wolf the window has closed itself, so
    // the alert is the only place the words can go.
    if (!onWolf()) { alertOK("Caves can only be built on Wolf Territories, in a region the grid knows."); return; }
    if (mBusy) { fail("Another cave job is still running - wait for it to finish (or press Stop), then try again."); return; }
    if (!sendOk()) { fail("Building stopped: this region's caves are not loaded here (did you just move?) - reopen the Cave Builder and try again."); return; }
    mBusy = true;
    mCancel = false;
    mErrors.clear();
    mJobCave = cave.mId;
    const Cave* old = find(cave.mId);
    if (old && old->mBuilt.isMap())
    {
        // A rebuild never forgets: the old rock must be in view to be taken away first.
        LLCoros::instance().launch("WolfCaves rebuild", [id = cave.mId, cave]() { WolfCaves::instance().removeCoro(id, true, cave, false); });
    }
    else
    {
        LLCoros::instance().launch("WolfCaves build", [cave]() { WolfCaves::instance().buildCoro(cave); });
    }
}

void WolfCaves::remove(const std::string& id, bool forget)
{
    // [WolfViewer 2026-10-14] Never refuse silently (see build()).
    if (!onWolf()) { alertOK("Caves can only be removed on Wolf Territories, in a region the grid knows."); return; }
    if (mBusy) { fail("Another cave job is still running - wait for it to finish (or press Stop), then try again."); return; }
    if (!sendOk()) { fail("Removing stopped: this region's caves are not loaded here (did you just move?) - reopen the Cave Builder and try again."); return; }
    mBusy = true;
    mCancel = false;
    mErrors.clear();
    mJobCave = id;
    LLCoros::instance().launch("WolfCaves remove", [id, forget]() { WolfCaves::instance().removeCoro(id, false, Cave(), forget); });
}

// Source: wolf_caves.js build.
void WolfCaves::buildCoro(Cave cave)
{
    std::string err;
    mStatus = "Step 1 of 5: checking your rights and saving the cave…";
    // [WOLF CAVE LIGHTS 2026-10-23] A (re)built cave has no lamps: a rebuild's old ones were taken away with the old rock
    // (removeOne), and lamps placed for the old rock would hang in the new one.
    cave.mLights = LLSD();
    Cave draft = cave;
    draft.mBuilt = LLSD();
    draft.mZone = LLSD();
    if (!putCave(draft, err))
    {
        // [WolfViewer 2026-10-14] caveServiceError already says what a 403 means: no second copy.
        fail("Building stopped: " + err);
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    // [WolfViewer 2026-10-14] A safe point to stop: only the drawing is saved so far.
    if (mCancel)
    {
        fail("Building stopped as you asked: the drawing is saved, nothing was rezzed or dug.");
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    mStatus = "Step 2 of 5: making the rock…";
    err.clear();   // a 409 that putCave retried past leaves its words here
    const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT);
    LLSD terrain, reply;
    if (folder.isNull()) err = "Your Objects inventory folder has not loaded yet.";
    // [WolfViewer 2026-10-14] The save above suspended: the agent may have moved since, and the
    // ground must be sampled in the region the record (and the rock) belongs to.
    else if (!sendOk()) err = "you left the region the cave belongs to; nothing was rezzed or dug.";
    if (!err.empty() || !sampleGround(cave.mDesign, terrain, err))
    {
        fail("Building stopped: " + err);
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    // [WolfViewer 2026-10-15 v3] Source: CAVE_DESIGN_V3.md "Dig hint": "After the save, a build sends
    // the cave's stored `dig`" — the record caves.php answered the save above with.
    const Cave* saved = find(cave.mId);
    const bool dig = saved && saved->mDig;
    LLSD body = meshRequest(cave, terrain, dig);
    body["name"] = cave.mName;
    body["cave_id"] = cave.mId;
    body["folder_id"] = folder.asString();
    if (!postMesh("build", body, reply, err))
    {
        fail("Building stopped: " + err);
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    const S32 li = reply["li"].asInteger();
    mStatus = llformat("Step 3 of 5: rezzing the rock (%d prim%s)…", li, li == 1 ? "" : "s");
    const LLSD& re = reply["rez_ray_end"];
    const LLSD& rp = reply["root_pos"];
    const LLVector3 ray((F32)re[0].asReal(), (F32)re[1].asReal(), (F32)re[2].asReal());
    const LLVector3 root((F32)rp[0].asReal(), (F32)rp[1].asReal(), (F32)rp[2].asReal());
    LLUUID rock;
    if (!rezExact(reply["item_id"].asUUID(), folder, ray, root, rock, err))
    {
        fail("Building stopped: " + err);
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    LLSD built;
    built["item"] = reply["item_id"].asString();
    // [2026-10-22] append, not with(0, ...): LLSD::with(Integer) inserts, and on an empty array ImplArray::insert
    // (llsd.cpp:680-691) first resizes to index + 1 - the id plus a stray null, which caves.php refuses (objects must be ids).
    built["objects"] = LLSD::emptyArray();
    built["objects"].append(rock.asString());
    built["li"] = li;
    built["root"] = rp;
    built["terrain"] = reply["targets"];
    built["dug"] = false;
    built["limited"] = 0;
    built["at"] = (S32)LLDate::now().secondsSinceEpoch();
    cave.mBuilt = built;
    cave.mZone = zoneFor(cave);
    if (!putCave(cave, err))
    {
        fail("The rock is rezzed, but the cave record could not be saved (" + err + "), so the ground was not dug. Delete the rock and build again.");
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    // [WolfViewer 2026-10-15 v3] dig = false (CAVE_DESIGN_V3.md): no terrain targets at all, the
    // ground is left as it is.
    std::vector<LLSD> limited;
    if (reply["targets"].size() >= 4)
    {
        mStatus = llformat("Step 4 of 5: digging the ground (%d points)…", (S32)reply["targets"].size() / 4);
        limited = shape(reply["targets"], true, "Step 4 of 5: digging the ground");
    }
    else
    {
        mStatus = "Step 4 of 5: no ground to change.";
    }
    // [WolfViewer 2026-10-14] Stopped part way: the record saved just above already says the rock
    // is rezzed and the ground NOT finished ("dug" false, every point's old and new height), so
    // Rebuild can dig it all again and Remove can put it all back - nothing more to save.
    if (mCancel)
    {
        fail("Building stopped as you asked: the rock is rezzed but the ground is not finished. "
             "Press Change then Rebuild to finish digging, or Remove to take the rock away and put all the ground back.");
        mStatus = "Stopped.";
        mResult = RESULT_FAILED;
        mBusy = false;
        return;
    }
    cave.mBuilt["dug"] = true;
    cave.mBuilt["limited"] = (S32)limited.size();
    if (!putCave(cave, err)) fail("The cave is built, but its record could not be updated: " + err);
    mStatus = "Step 5 of 5: done.";
    if (!limited.empty())
    {
        const LLSD& ex = limited.front();
        fail(llformat("The cave is built, but %d ground point%s could not be lowered all the way (for example at %d, %d: wanted %.2f m, the ground stayed at %s m). ",
                      (S32)limited.size(), limited.size() == 1 ? "" : "s", ex[0].asInteger(), ex[1].asInteger(), ex[2].asReal(),
                      ex[3].isDefined() ? llformat("%.2f", ex[3].asReal()).c_str() : "?")
             + "The region's terrain limits stopped it: the estate's raise/lower limits, or - on servers before the .NET 10 update - the 0 m floor. "
               "An estate manager can widen the limits (World > Region/Estate > Terrain), then Change and Rebuild.");
    }
    else
    {
        mStatus = "\"" + cave.mName + "\" is built.";
    }
    mResult = RESULT_BUILT;
    mBusy = false;
}

LLViewerObject* WolfCaves::findRock(const Cave& cave) const
{
    if (!cave.mBuilt.isMap()) return nullptr;
    for (const LLSD& id : llsd::inArray(cave.mBuilt["objects"]))
    {
        LLViewerObject* o = gObjectList.findObject(id.asUUID());
        // Only the agent's own region: a local id means nothing in a neighbour's circuit.
        if (o && !o->isDead() && o->getRegion() == gAgent.getRegion() && gAgent.getRegion()
            && gAgent.getRegion()->getHandle() == mHandle) return o->getRootEdit();
    }
    return nullptr;
}

// Source: wolf_caves.js remove — take the rock away, put the ground back, forget the cave. A built
// cave whose rock this viewer cannot see is left alone unless `forget` (the user chose to leave
// the rock): the record is the only thing that knows the rock.
bool WolfCaves::removeOne(const Cave& cave, bool forget, std::string& error)
{
    LLViewerObject* rock = findRock(cave);
    if (rockRecorded(cave) && !rock && !forget)
    {
        error = "the cave's rock is not in view from here. Stand nearer the cave and try again (or choose to forget it and leave the rock).";
        return false;
    }
    // [WolfViewer 2026-10-15 v3] A cave from the old builder goes back to caves.php only exactly as
    // stored (toLLSD sends mRaw; CAVE_DESIGN_V3.md "Caves from the old builder": "Changing one is
    // refused"), so the saves below cannot mark its progress or check the rights. Its record is
    // dropped FIRST instead - that removal is the rights check - and the rock and the ground are
    // then dealt with from this copy.
    const bool old = cave.mOld;
    if (old && cave.mBuilt.isMap())
    {
        mStatus = "Checking your rights…";
        if (!dropCave(cave.mId, error)) return false;
        rock = findRock(cave);   // the save suspended: the world may have moved on
    }
    else if (cave.mBuilt.isMap())
    {
        // The rights check FIRST: this save is refused unless the cave is the caller's to change,
        // and nothing has been touched yet. It also marks the ground as not finished, so a restore
        // that stops half way still leaves the record knowing what to put back.
        mStatus = "Checking your rights…";
        Cave marked = cave;
        marked.mBuilt["dug"] = false;
        if (!putCave(marked, error)) return false;
        // [WolfViewer 2026-10-14] A safe point to stop: nothing has been touched yet.
        if (mCancel)
        {
            error = "stopped as you asked: nothing was taken away and the cave is kept.";
            return false;
        }
        rock = findRock(cave);   // the world may have moved on while we waited
        if (rockRecorded(cave) && !rock && !forget)
        {
            error = "the cave's rock went out of view. Stand nearer the cave and try again.";
            return false;
        }
        // [WOLF CAVE LIGHTS 2026-10-23] Paul: deleting the cave also deletes its lights (and a rebuild's lamps would hang in
        // the new rock). The lamps in view go; parts not in view are said and left, and the record forgets them.
        if (cave.mLights.isMap() && cave.mLights["objects"].size() > 0)
        {
            mStatus = "Taking the cave's lights away…";
            const LLSD unseen = WolfCaveLights::takeAwayFor(cave.mLights);
            if (unseen.size() > 0)
                fail(llformat("%d light part%s of \"%s\" %s not in view, so %s left where %s: delete %s by hand if %s still there.",
                              (S32)unseen.size(), unseen.size() == 1 ? "" : "s", cave.mName.c_str(), unseen.size() == 1 ? "was" : "were",
                              unseen.size() == 1 ? "it is" : "they are", unseen.size() == 1 ? "it is" : "they are",
                              unseen.size() == 1 ? "it" : "them", unseen.size() == 1 ? "it is" : "they are"));
            Cave unlit = marked;
            unlit.mLights = LLSD();
            if (!putCave(unlit, error)) return false;
            rock = findRock(cave);   // the save suspended
        }
    }
    LLViewerRegion* rgn = rock ? rock->getRegion() : nullptr;
    // [WolfViewer 2026-10-14] `rock` is not touched after the next suspend (it may be deleted by
    // then); this says whether it was taken away.
    bool took_rock = false;
    if (rock && rgn && sendOk())
    {
        mStatus = "Taking the rock away…";
        // Source: llselectmgr.cpp:5818-5834 packDeRezHeader + :5865-5869 packObjectLocalID, roots
        // only (Scene.Inventory.cs ignores a child). Own: deleted (DRD_TRASH, into Trash when the
        // region keeps deleted objects); someone else's on our land: back to them
        // (llselectmgr.h:83-98 DRD_TRASH = 6, DRD_RETURN_TO_OWNER = 9). Sent to the rock's OWN
        // region, which findRock() has already required to be the agent's.
        const bool own = rock->permYouOwner();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)(own ? 6 : 9));
        msg->addUUIDFast(_PREHASH_DestinationID, own ? gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH) : LLUUID::null);
        LLUUID tid;
        tid.generate();
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, (U8)1);
        msg->addU8Fast(_PREHASH_PacketNumber, (U8)1);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, rock->getLocalID());
        msg->sendReliable(rgn->getHost());
        took_rock = true;
        rock = nullptr;
        // [WolfViewer 2026-10-14] Record that the rock is gone before the long put-back, so a
        // put-back that stops (Stop, leaving the region) leaves a record Remove can finish from:
        // the ground's old heights are still all there, and putting back is safe to repeat. Not
        // fatal if refused: the cave is dropped at the end either way.
        if (!old)
        {
            Cave gone = cave;
            gone.mBuilt["dug"] = false;
            gone.mBuilt["objects"] = LLSD::emptyArray();
            std::string ignored;
            if (!putCave(gone, ignored)) LL_WARNS("WolfCaves") << "could not record the rock as taken away: " << ignored << LL_ENDL;
        }
    }
    else if (rock)
    {
        error = "you left the region before the rock could be taken away.";
        return false;
    }
    std::vector<LLSD> limited;
    if (cave.mBuilt.isMap() && cave.mBuilt["terrain"].size() >= 4)
    {
        mStatus = llformat("Putting the ground back (%d points)…", (S32)cave.mBuilt["terrain"].size() / 4);
        limited = shape(cave.mBuilt["terrain"], false, "Putting the ground back");
        // [WolfViewer 2026-10-14] Stopped between floods: the record keeps every point's old
        // height, so Remove puts the rest back (and repeats the done ones harmlessly).
        if (mCancel)
        {
            error = std::string("stopped as you asked: ") + (took_rock ? "the rock is taken away and " : "")
                  + (old ? "the ground is only partly put back. This cave was drawn with the old Cave Builder and its record is already gone, so the rest of the ground must be put back by hand."
                         : "the ground is only partly put back. The cave is kept - press Remove again to finish putting the ground back.");
            return false;
        }
        if (!sendOk())
        {
            error = old ? "you left the region while the ground was being put back. This cave was drawn with the old Cave Builder and its record is already gone, so the rest of the ground must be put back by hand."
                        : "you left the region while the ground was being put back; the cave is kept so Remove can finish it.";
            return false;
        }
    }
    mStatus = "Forgetting the cave…";
    if (!(old && cave.mBuilt.isMap()) && !dropCave(cave.mId, error)) return false;
    if (!limited.empty())
    {
        fail(llformat("The cave is removed, but %d ground point%s could not be put back exactly - a terrain limit stopped them.",
                      (S32)limited.size(), limited.size() == 1 ? "" : "s"));
    }
    if (!took_rock && rockRecorded(cave))
    {
        fail("\"" + cave.mName + "\" is forgotten and its ground put back. Its rock is still there: delete it by hand.");
    }
    return true;
}

// ── [WOLF CAVE LIGHTS 2026-10-23] Light up / Remove lights ──────────────────────────────────────────────────────────────
namespace
{
    F32 caveArch(F32 q)
    {
        // Source: wolfcave.rs arch (the cave builder's vault profile): sqrt(1 - (1 - q)^2), q held to 1.
        q = llmin(q, 1.f);
        return sqrtf(llmax(0.f, 1.f - (1.f - q) * (1.f - q)));
    }
}

std::vector<WolfCaveLights::Lamp> WolfCaves::caveLamps(const Cave& cave, WolfCaveLights::EStyle style)
{
    const Design& d = cave.mDesign;
    if (!d.mHasBox || d.mTunnels.empty()) return {};
    // Source: wolfcave.rs Interior (new: clear = box.h - roof; s_at; at: tunnel_ceiling = min(w * 0.9 + 1, clear) * arch(s / r)
    // over tunnels, cavern_ceiling = min(h, clear) * arch(s / r) over caverns, the highest) and generate's ceiling noise
    // (rough_amp = roughness * min(0.15 * clear, 1), taken off here so a lamp is never put where the noise lowers the roof).
    const F32 clear = d.mBox.mH - d.mRoof;
    const F32 rough_amp = d.mRoughness * llmin(0.15f * clear, 1.f);
    const F32 floor = d.mBox.mZ;
    auto field = [&d, clear, rough_amp, floor](F32 x, F32 y, F32& s, F32& fl, F32& cl)
    {
        s = -1e30f;
        F32 v = 0.f;
        for (const Tunnel& t : d.mTunnels)
        {
            const F32 r = t.mW * 0.5f;
            const F32 st = r - tunnelDist(t, x, y);
            s = llmax(s, st);
            if (st > 0.f) v = llmax(v, llmin(t.mW * 0.9f + 1.f, clear) * caveArch(st / r));
        }
        for (const Cavern& k : d.mCaverns)
        {
            const F32 sk = k.mR - sqrtf((x - k.mX) * (x - k.mX) + (y - k.mY) * (y - k.mY));
            s = llmax(s, sk);
            if (sk > 0.f) v = llmax(v, llmin(k.mH, clear) * caveArch(sk / k.mR));
        }
        fl = floor;
        cl = v - rough_amp;
        return true;
    };
    std::vector<WolfCaveLights::Route> routes;
    for (const Tunnel& t : d.mTunnels)
    {
        WolfCaveLights::Route r;
        for (size_t k = 0; k + 1 < t.mP.size(); k += 2) r.mPts.emplace_back(t.mP[k], t.mP[k + 1]);
        r.mHalfW = t.mW * 0.5f;
        routes.push_back(r);
    }
    std::vector<WolfCaveLights::Room> rooms;
    for (const Cavern& k : d.mCaverns) rooms.push_back(WolfCaveLights::Room{ LLVector2(k.mX, k.mY), k.mR });
    return WolfCaveLights::plan(routes, rooms, field, style, false);
}

void WolfCaves::lights(const std::string& id, bool remove, std::vector<WolfCaveLights::Lamp> lamps, WolfCaveLights::EStyle style, bool warm, bool keep_unseen)
{
    if (!onWolf()) { alertOK("Cave lights can only be placed on Wolf Territories, in a region the grid knows."); return; }
    if (mBusy) { fail("Another cave job is still running - wait for it to finish (or press Stop), then try again."); return; }
    if (!sendOk()) { fail("Stopped: this region's caves are not loaded here (did you just move?) - reopen the Cave Builder and try again."); return; }
    mBusy = true;
    mCancel = false;
    mErrors.clear();
    mJobCave = id;
    LLCoros::instance().launch("WolfCaves lights", [id, remove, lamps, style, warm, keep_unseen]()
    {
        WolfCaves::instance().lightsCoro(id, remove, lamps, style, warm, keep_unseen);
    });
}

void WolfCaves::lightsCoro(std::string id, bool remove, std::vector<WolfCaveLights::Lamp> lamps, WolfCaveLights::EStyle style, bool warm, bool keep_unseen)
{
    const Cave* c = find(id);
    if (!c) { fail("That cave is not here any more."); mResult = RESULT_FAILED; mBusy = false; return; }
    const Cave start = *c;
    const LLViewerRegion* rgn = gAgent.getRegion();
    const LLUUID region = rgn ? rgn->getRegionID() : LLUUID::null;
    // each save is the cave as stored now with only its lights changed (the rights rules of caves.php apply to the change)
    auto save = [this, id](const LLSD& l, std::string& e)
    {
        const Cave* now = find(id);
        if (!now) { e = "the cave is not here any more."; return false; }
        Cave next = *now;
        next.mLights = l;
        return putCave(next, e);
    };
    auto progress = [this](const std::string& line, F32) { mStatus = line; };
    std::string done, err;
    const bool ok = WolfCaveLights::run(start.mLights, keep_unseen, remove ? nullptr : &lamps, style, warm, start.mName, region, save,
                                        progress, [this]() { return mCancel; }, done, err);
    if (ok) mStatus = done;
    else fail((remove ? "Removing the lights stopped: " : "Lighting up stopped: ") + err);
    mResult = ok ? RESULT_NONE : RESULT_FAILED;
    mBusy = false;
}

void WolfCaves::removeCoro(std::string id, bool then_build, Cave next, bool forget)
{
    const Cave* c = find(id);
    std::string err;
    if (c)
    {
        const Cave copy = *c;
        if (!removeOne(copy, forget, err))
        {
            fail((then_build ? "Rebuilding stopped: " : "Removing stopped: ") + err);
            mResult = RESULT_FAILED;
            mBusy = false;
            return;
        }
    }
    if (then_build)
    {
        buildCoro(next);
        return;
    }
    mStatus = "Cave removed.";
    mResult = RESULT_REMOVED;
    mBusy = false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfToolCave — the pen
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfToolCave::WolfToolCave() : LLTool(std::string("WolfCave")) {}

// [WolfViewer 2026-10-14 v2] The width of the next tunnel. The setting keeps its v1 name
// (app_settings/settings.xml:13428 WolfCavePassageWidth) so a width people chose carries over.
// Source: CAVE_DESIGN_V3.md tunnels "w in [2, 40]".
F32 WolfToolCave::tunnelWidth()
{
    static LLCachedControl<F32> width(gSavedSettings, "WolfCavePassageWidth", 4.f);
    return llclamp((F32)width, WolfCaves::TUNNEL_W_MIN, WolfCaves::TUNNEL_W_MAX);
}

// [WolfViewer 2026-10-15 v3] The height of the next cavern, kept like the tunnel width
// (app_settings/settings.xml WolfCaveCavernHeight, default 5). Source: CAVE_DESIGN_V3.md caverns
// "h in [2.5, 20]".
F32 WolfToolCave::cavernHeight()
{
    static LLCachedControl<F32> height(gSavedSettings, "WolfCaveCavernHeight", 5.f);
    return llclamp((F32)height, WolfCaves::CAVERN_H_MIN, WolfCaves::CAVERN_H_MAX);
}

// Source: wolfterrainpaint.cpp WolfToolTerrainPaint::hit — the ground under the pointer, in the agent region.
bool WolfToolCave::hit(S32 x, S32 y, F32& rx, F32& ry) const
{
    LLVector3d spot;
    if (!gViewerWindow->mousePointOnLandGlobal(x, y, &spot)) return false;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    const LLVector3 pos = rgn->getPosRegionFromGlobal(spot);
    if (pos.mV[VX] < 0.f || pos.mV[VY] < 0.f || pos.mV[VX] >= rgn->getWidth() || pos.mV[VY] >= rgn->getWidth()) return false;
    rx = r2(pos.mV[VX]);
    ry = r2(pos.mV[VY]);
    return true;
}

// [WolfViewer 2026-10-15 v3] The pointer on the level plane at height z: the box handles sit at the
// box floor, which need not be the ground (the floor can be typed), so a handle drag follows that
// plane rather than the terrain. Source: llviewerwindow.h:455 mousePointOnPlaneGlobal(point, x, y,
// plane_point, plane_normal); llviewerwindow.cpp:5738-5765 returns false when the plane is behind
// the camera. llviewerregion.h:339-342 getPosRegionFromGlobal / getPosGlobalFromRegion.
bool WolfToolCave::planeHit(S32 x, S32 y, F32 z, F32& rx, F32& ry) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    LLVector3d spot;
    if (!gViewerWindow->mousePointOnPlaneGlobal(spot, x, y, rgn->getPosGlobalFromRegion(LLVector3(0.f, 0.f, z)), LLVector3::z_axis)) return false;
    const LLVector3 pos = rgn->getPosRegionFromGlobal(spot);
    // Near the horizon the plane is met kilometres away: no use for dragging a box in this region.
    const F32 w = rgn->getWidth();
    if (pos.mV[VX] < -w || pos.mV[VY] < -w || pos.mV[VX] > 2.f * w || pos.mV[VY] > 2.f * w) return false;
    rx = pos.mV[VX];
    ry = pos.mV[VY];
    return true;
}

// [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "Box resize": corners and side middles at the floor,
// the top handle over the centre at z + h. Sides: k = 0..3 between corner k and k + 1 (boxCorners
// order (-w/2,-d/2), (w/2,-d/2), (w/2,d/2), (-w/2,d/2)), so 4 = -y, 5 = +x, 6 = +y, 7 = -x.
LLVector3 WolfToolCave::handlePos(const WolfCaves::Box& b, S32 i)
{
    if (i == HANDLE_TOP) return LLVector3(b.mX, b.mY, b.mZ + b.mH);
    F32 c[8];
    WolfCaves::boxCorners(b, c);
    if (i < 4) return LLVector3(c[2 * i], c[2 * i + 1], b.mZ);
    const S32 k = i - 4, j = (k + 1) % 4;
    return LLVector3((c[2 * k] + c[2 * j]) * 0.5f, (c[2 * k + 1] + c[2 * j + 1]) * 0.5f, b.mZ);
}

// [WolfViewer 2026-10-15 v3] Screen-space pick, the nearest handle within HANDLE_PICK_PX.
// Source: llviewercamera.h:75 projectPosAgentToScreen(const LLVector3& pos_agent, LLCoordGL& out_point,
// const bool clamp = true) — false when the point is behind the camera or (clamped) off the world
// view; llviewercamera.cpp:428-497 gives the point in the scaled UI coordinates the tool's mouse
// x, y are in (lltoolgrab.cpp:1073 compares them the same way in its handleHover).
S32 WolfToolCave::pickHandle(S32 x, S32 y) const
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mEdit || !mEdit->mDesign.mHasBox || !rgn) return -1;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    S32 best = -1;
    F32 best_px = HANDLE_PICK_PX;
    for (S32 i = 0; i < HANDLE_COUNT; ++i)
    {
        LLCoordGL sp;
        if (!cam->projectPosAgentToScreen(rgn->getPosAgentFromRegion(handlePos(mEdit->mDesign.mBox, i)), sp, true)) continue;
        const F32 dx = (F32)(sp.mX - x), dy = (F32)(sp.mY - y);
        const F32 px = sqrtf(dx * dx + dy * dy);
        if (px <= best_px) { best = i; best_px = px; }
    }
    return best;
}

bool WolfToolCave::handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down)
{
    // The pen takes left clicks only; everything else (and every click with the pen off) is
    // LLToolPie's, as if the pen were not there.
    if (!mDrawing || (clicktype != CLICK_LEFT && clicktype != CLICK_DOUBLELEFT))
    {
        return LLToolPie::getInstance()->handleAnyMouseClick(x, y, mask, clicktype, down);
    }
    return LLTool::handleAnyMouseClick(x, y, mask, clicktype, down);
}

bool WolfToolCave::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    return LLToolPie::getInstance()->handleScrollWheel(x, y, clicks);
}

bool WolfToolCave::handleToolTip(S32 x, S32 y, MASK mask)
{
    return mDrawing ? false : LLToolPie::getInstance()->handleToolTip(x, y, mask);
}

// [WolfViewer 2026-10-14 v2] Box: press-drag the footprint. Tunnel: click points. Doorway: click
// where a tunnel meets the box wall. [WolfViewer 2026-10-15 v3] Box pen on an existing box: a handle
// resizes or raises it, a press inside the footprint moves it; Cavern: press on a tunnel and drag
// outwards (CAVE_DESIGN_V3.md Flow 1-4).
bool WolfToolCave::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (!mDrawing || !mEdit || !WolfCaves::onWolf()) return false;
    WolfCaves::Design& d = mEdit->mDesign;
    if (mPen == PEN_BOX && d.mHasBox)
    {
        const S32 handle = pickHandle(x, y);
        F32 px = 0.f, py = 0.f;
        const bool inside = handle < 0 && planeHit(x, y, d.mBox.mZ, px, py) && WolfCaves::boxSigned(d.mBox, px, py) >= 0.f;
        if (handle >= 0 || inside)
        {
            mDragStart = WolfCaves::BoxUndo();
            mDragStart.mHadBox = true;
            mDragStart.mBox = d.mBox;
            mHandle = handle;
            if (handle == HANDLE_TOP)
            {
                // Metres per pixel: the handle and a point 1 m above it (or, when that one does not
                // project, 1 m below) on screen. If neither pair projects, or the view is straight
                // down the vertical, it comes from the camera instead: at distance dist a metre is
                // view_height_px / (2 * dist * tan(fov / 2)) pixels (llcamera.h:161-162 getView =
                // vertical FOV in radians, getViewHeightInPixels), in the UI's scaled coordinates
                // the mouse is in (llviewerwindow.h:467 getDisplayScale).
                LLViewerRegion* rgn = gAgent.getRegion();
                LLViewerCamera* cam = LLViewerCamera::getInstance();
                const LLVector3 top = handlePos(d.mBox, HANDLE_TOP);
                F32 px_per_m = 0.f;
                if (rgn)
                {
                    LLCoordGL s0, s1;
                    const LLVector3 top_agent = rgn->getPosAgentFromRegion(top);
                    if (cam->projectPosAgentToScreen(top_agent, s0, false))
                    {
                        for (const F32 dz : { 1.f, -1.f })
                        {
                            if (cam->projectPosAgentToScreen(top_agent + LLVector3(0.f, 0.f, dz), s1, false))
                            {
                                px_per_m = fabsf((F32)(s1.mY - s0.mY));
                                break;
                            }
                        }
                    }
                    if (px_per_m < 2.f)
                    {
                        const F32 dist = llmax(dist_vec(cam->getOrigin(), top_agent), 0.5f);
                        const F32 scale_y = llmax(gViewerWindow->getDisplayScale().mV[VY], 0.01f);
                        px_per_m = (F32)cam->getViewHeightInPixels() / (2.f * dist * tanf(cam->getView() * 0.5f)) / scale_y;
                    }
                }
                // 1 cm to 0.5 m of height per pixel: never a jump of metres for one pixel.
                mMetresPerPixel = llclamp(1.f / llmax(px_per_m, 0.01f), 0.01f, 0.5f);
                mPressY = y;
                mDrag = DRAG_TOP;
            }
            else if (handle >= 4)
            {
                mDrag = DRAG_SIDE;
            }
            else if (handle >= 0)
            {
                mDrag = DRAG_CORNER;
            }
            else
            {
                // A move carries everything inside along: Undo needs them as they were.
                mDragStart.mMoved = true;
                mDragStart.mTunnels = d.mTunnels;
                mDragStart.mCaverns = d.mCaverns;
                mDragStart.mDoors = d.mDoors;
                mGrabX = px;
                mGrabY = py;
                mDrag = DRAG_MOVE;
            }
            mHover = handle;
            setMouseCapture(true);
            return true;
        }
    }
    F32 rx, ry;
    if (!hit(x, y, rx, ry))
    {
        // [WolfViewer 2026-10-14] Say why nothing happened (the floater shows tool messages).
        mMessage = "Click on the ground inside this region.";
        return true;
    }
    if (mPen == PEN_BOX)
    {
        // Off the box and off its handles: a new box is drawn (it replaces the old one; Undo puts
        // the old one back).
        mDrag = DRAG_NEW_BOX;
        mAX = mBX = rx;
        mAY = mBY = ry;
        setMouseCapture(true);
        return true;
    }
    if (!d.mHasBox)
    {
        mMessage = "Draw the rock box first (step 1).";
        return true;
    }
    if (mPen == PEN_TUNNEL)
    {
        if (mLive.empty() && (S32)d.mTunnels.size() >= WolfCaves::MAX_TUNNELS)
        {
            mMessage = llformat("A cave has at most %d tunnels - press Undo to take one back.", WolfCaves::MAX_TUNNELS);
            return true;
        }
        // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md tunnels: every point inside the box or at most
        // TUNNEL_OUTSIDE_M outside it.
        if (WolfCaves::boxSigned(d.mBox, rx, ry) < -WolfCaves::TUNNEL_OUTSIDE_M)
        {
            mMessage = "Tunnels are carved through the rock box: click inside the box (or at most 1 m outside its wall). To reach further, make the box bigger with its handles (step 1).";
            return true;
        }
        S32 all = (S32)mLive.size() / 2;
        for (const WolfCaves::Tunnel& t : d.mTunnels) all += (S32)t.mP.size() / 2;
        if (all >= WolfCaves::MAX_ALL_TUNNEL_POINTS)
        {
            finishTunnel();
            mMessage = llformat("The tunnels have %d points in all, the most a cave can have.", WolfCaves::MAX_ALL_TUNNEL_POINTS);
            return true;
        }
        mLive.push_back(rx);
        mLive.push_back(ry);
        if ((S32)mLive.size() / 2 >= WolfCaves::MAX_TUNNEL_POINTS)
        {
            finishTunnel();
            mMessage = llformat("A tunnel has at most %d points, so this one is finished - click to start another.", WolfCaves::MAX_TUNNEL_POINTS);
        }
        return true;
    }
    if (mPen == PEN_CAVERN)
    {
        if ((S32)d.mCaverns.size() >= WolfCaves::MAX_CAVERNS)
        {
            mMessage = llformat("A cave has at most %d caverns - press Undo to take one back.", WolfCaves::MAX_CAVERNS);
            return true;
        }
        const std::string why = WolfCaves::cavernProblem(d, rx, ry, WolfCaves::CAVERN_R_MIN);
        if (!why.empty())
        {
            mMessage = why;
            return true;
        }
        for (const WolfCaves::Cavern& cv : d.mCaverns)
        {
            // A double-click lands twice on one spot: one cavern is meant.
            if (fabsf(cv.mX - rx) < 0.5f && fabsf(cv.mY - ry) < 0.5f)
            {
                mMessage = "There is already a cavern there.";
                return true;
            }
        }
        mCavX = rx;
        mCavY = ry;
        mCavR = WolfCaves::CAVERN_R_MIN;
        // The whole disc inside the box (sb >= r), to the centimetre below, as caves.php checks it.
        mCavMaxR = llmin(WolfCaves::CAVERN_R_MAX, floorf(WolfCaves::boxSigned(d.mBox, rx, ry) * 100.f) / 100.f);
        mDrag = DRAG_CAVERN;
        setMouseCapture(true);
        return true;
    }
    // PEN_DOOR
    if ((S32)d.mDoors.size() >= WolfCaves::MAX_DOORS)
    {
        mMessage = llformat("A cave has at most %d doorways - press Undo to take one back.", WolfCaves::MAX_DOORS);
        return true;
    }
    const std::string why = WolfCaves::doorProblem(d, rx, ry);
    if (!why.empty())
    {
        mMessage = why;
        return true;
    }
    for (const WolfCaves::Door& e : d.mDoors)
    {
        // A double-click lands twice on one spot: one doorway is meant.
        if (fabsf(e.mX - rx) < 0.5f && fabsf(e.mY - ry) < 0.5f)
        {
            mMessage = "There is already a doorway there.";
            return true;
        }
    }
    d.mDoors.push_back(WolfCaves::Door{ rx, ry });
    d.mOrder.push_back('d');
    mChanged = true;
    return true;
}

// [WolfViewer 2026-10-15 v3] One step of a box handle drag (CAVE_DESIGN_V3.md "Box resize"). Always
// worked out from the box as it was at the press, so the drag never drifts; a step that would put
// part of the footprint outside the region is not taken (the box stays at its last good place).
void WolfToolCave::dragBox(S32 x, S32 y)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mEdit || !rgn) return;
    WolfCaves::Design& d = mEdit->mDesign;
    const WolfCaves::Box& b0 = mDragStart.mBox;
    const F32 region_w = rgn->getWidth();
    WolfCaves::Box b = b0;
    if (mDrag == DRAG_TOP)
    {
        // Height limits (CAVE_DESIGN_V3.md h in [3, 30]), and never so low that the roof leaves less
        // than 2.5 m under it (h - roof >= 2.5) while the roof allows it.
        const F32 lo = llclamp(d.mRoof + WolfCaves::ROOM_UNDER_ROOF_M, WolfCaves::BOX_H_MIN, WolfCaves::BOX_H_MAX);
        b.mH = r2(llclamp(b0.mH + (F32)(y - mPressY) * mMetresPerPixel, lo, WolfCaves::BOX_H_MAX));
        d.mBox = b;
        return;
    }
    F32 px, py;
    if (!planeHit(x, y, b0.mZ, px, py)) return;   // pointing above the horizon: keep the last place
    if (mDrag == DRAG_MOVE)
    {
        // The box and everything in it move by one offset, held so all of it stays in the region.
        F32 lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
        auto span = [&](F32 sx, F32 sy, F32 r) { lo_x = llmin(lo_x, sx - r); lo_y = llmin(lo_y, sy - r); hi_x = llmax(hi_x, sx + r); hi_y = llmax(hi_y, sy + r); };
        F32 c[8];
        WolfCaves::boxCorners(b0, c);
        for (S32 i = 0; i < 4; ++i) span(c[2 * i], c[2 * i + 1], 0.f);
        for (const WolfCaves::Tunnel& t : mDragStart.mTunnels) for (size_t k = 0; k + 1 < t.mP.size(); k += 2) span(t.mP[k], t.mP[k + 1], 0.f);
        for (const WolfCaves::Cavern& cv : mDragStart.mCaverns) span(cv.mX, cv.mY, 0.f);
        for (const WolfCaves::Door& e : mDragStart.mDoors) span(e.mX, e.mY, 0.f);
        // If something already lay outside, it may stay where it is but not go further out.
        F32 dx = px - mGrabX, dy = py - mGrabY;
        dx = llmin(llmax(dx, llmin(0.f, -lo_x)), llmax(0.f, region_w - hi_x));
        dy = llmin(llmax(dy, llmin(0.f, -lo_y)), llmax(0.f, region_w - hi_y));
        dx = r2(dx);
        dy = r2(dy);
        b.mX = r2(b0.mX + dx);
        b.mY = r2(b0.mY + dy);
        d.mBox = b;
        d.mTunnels = mDragStart.mTunnels;
        for (WolfCaves::Tunnel& t : d.mTunnels)
        {
            for (size_t k = 0; k + 1 < t.mP.size(); k += 2) { t.mP[k] = r2(t.mP[k] + dx); t.mP[k + 1] = r2(t.mP[k + 1] + dy); }
        }
        d.mCaverns = mDragStart.mCaverns;
        for (WolfCaves::Cavern& cv : d.mCaverns) { cv.mX = r2(cv.mX + dx); cv.mY = r2(cv.mY + dy); }
        d.mDoors = mDragStart.mDoors;
        for (WolfCaves::Door& e : d.mDoors) { e.mX = r2(e.mX + dx); e.mY = r2(e.mY + dy); }
        return;
    }
    // Corner or side, in the box's own (turned) frame at the press: the pointer's local x, y.
    const F32 a = b0.mRot * DEG_TO_RAD, c = cosf(a), s = sinf(a);
    const F32 lx = (px - b0.mX) * c + (py - b0.mY) * s, ly = -(px - b0.mX) * s + (py - b0.mY) * c;
    const F32 hw = b0.mW * 0.5f, hd = b0.mD * 0.5f;
    F32 w = b0.mW, dd = b0.mD, cxl = 0.f, cyl = 0.f;
    // One axis: the edge at `fixed` stays, the other follows the pointer; size clamped to the limits.
    auto axis = [](F32 p, F32 fixed, F32& size, F32& centre)
    {
        const F32 sp = p - fixed;
        size = r2(llclamp(fabsf(sp), WolfCaves::BOX_SIDE_MIN, WolfCaves::BOX_SIDE_MAX));
        centre = fixed + (sp >= 0.f ? 0.5f : -0.5f) * size;
    };
    if (mDrag == DRAG_CORNER)
    {
        // The opposite corner stays: corner i is at (sx[i] * w/2, sy[i] * d/2).
        static const F32 sx[4] = { -1.f, 1.f, 1.f, -1.f }, sy[4] = { -1.f, -1.f, 1.f, 1.f };
        axis(lx, -sx[mHandle] * hw, w, cxl);
        axis(ly, -sy[mHandle] * hd, dd, cyl);
    }
    else
    {
        // That side moves along its normal; the opposite side stays.
        switch (mHandle)
        {
            case 4:  axis(ly, hd, dd, cyl); break;    // -y side; +y side stays
            case 5:  axis(lx, -hw, w, cxl); break;    // +x side; -x side stays
            case 6:  axis(ly, -hd, dd, cyl); break;   // +y side; -y side stays
            default: axis(lx, hw, w, cxl); break;     // 7: -x side; +x side stays
        }
    }
    b.mW = w;
    b.mD = dd;
    b.mX = r2(b0.mX + cxl * c - cyl * s);
    b.mY = r2(b0.mY + cxl * s + cyl * c);
    F32 cc[8];
    WolfCaves::boxCorners(b, cc);
    for (S32 i = 0; i < 4; ++i)
    {
        if (cc[2 * i] < 0.f || cc[2 * i] > region_w || cc[2 * i + 1] < 0.f || cc[2 * i + 1] > region_w)
        {
            mMessage = "The box stops at the region edge.";
            return;
        }
    }
    d.mBox = b;
}

bool WolfToolCave::handleHover(S32 x, S32 y, MASK mask)
{
    if (!mDrawing) return LLToolPie::getInstance()->handleHover(x, y, mask);
    F32 rx, ry;
    mHaveCursor = hit(x, y, rx, ry);
    if (mHaveCursor)
    {
        mCursorX = rx;
        mCursorY = ry;
    }
    ECursorType cursor = UI_CURSOR_TOOLLAND;
    switch (mDrag)
    {
        case DRAG_NEW_BOX:
            if (mHaveCursor) { mBX = rx; mBY = ry; }
            break;
        case DRAG_CAVERN:
            if (mHaveCursor)
            {
                const F32 dx = rx - mCavX, dy = ry - mCavY;
                // Down to the centimetre, so the stored disc never pokes past the box wall.
                mCavR = floorf(llclamp(sqrtf(dx * dx + dy * dy), WolfCaves::CAVERN_R_MIN, llmax(WolfCaves::CAVERN_R_MIN, mCavMaxR)) * 100.f) / 100.f;
            }
            break;
        case DRAG_CORNER:
        case DRAG_SIDE:
            dragBox(x, y);
            cursor = UI_CURSOR_SIZEALL;
            break;
        case DRAG_TOP:
            dragBox(x, y);
            cursor = UI_CURSOR_SIZENS;
            break;
        case DRAG_MOVE:
            dragBox(x, y);
            cursor = UI_CURSOR_TOOLTRANSLATE;
            break;
        case DRAG_NONE:
            // Hover shows which handle a press would take (CAVE_DESIGN_V3.md "Box resize").
            mHover = (mPen == PEN_BOX && mEdit && mEdit->mDesign.mHasBox) ? pickHandle(x, y) : -1;
            if (mHover == HANDLE_TOP) cursor = UI_CURSOR_SIZENS;
            else if (mHover >= 0) cursor = UI_CURSOR_SIZEALL;
            else if (mPen == PEN_BOX && mEdit && mEdit->mDesign.mHasBox)
            {
                F32 px, py;
                if (planeHit(x, y, mEdit->mDesign.mBox.mZ, px, py) && WolfCaves::boxSigned(mEdit->mDesign.mBox, px, py) >= 0.f) cursor = UI_CURSOR_TOOLTRANSLATE;
            }
            break;
    }
    // Source: llcursortypes.h:37-55 UI_CURSOR_SIZEALL / UI_CURSOR_SIZENS / UI_CURSOR_TOOLTRANSLATE.
    gViewerWindow->setCursor(cursor);
    return true;
}

bool WolfToolCave::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // Read the drag first: letting go of the capture calls onMouseCaptureLost, which drops it.
    // The capture is let go even when the drag was dropped meanwhile (the pen put down from the
    // window mid-drag), so the mouse is never left captured.
    const EDrag drag = mDrag;
    mDrag = DRAG_NONE;
    const bool had_capture = hasMouseCapture();
    if (had_capture) setMouseCapture(false);
    switch (drag)
    {
        case DRAG_NONE:     return had_capture;
        case DRAG_NEW_BOX:  finishBox(); break;
        case DRAG_CAVERN:   finishCavern(); break;
        case DRAG_CORNER:
        case DRAG_SIDE:
        case DRAG_MOVE:
        case DRAG_TOP:      finishHandleDrag(); break;
    }
    mHandle = -1;
    return true;
}

// [WolfViewer 2026-10-15 v3] Every drag in progress is dropped, not finished half way; a box handle
// drag puts the box (and, for a move, what it carried) back as it was at the press.
void WolfToolCave::dropDrag()
{
    if (boxDragging() && mEdit)
    {
        WolfCaves::Design& d = mEdit->mDesign;
        d.mBox = mDragStart.mBox;
        if (mDragStart.mMoved)
        {
            d.mTunnels = mDragStart.mTunnels;
            d.mCaverns = mDragStart.mCaverns;
            d.mDoors = mDragStart.mDoors;
        }
    }
    mDrag = DRAG_NONE;
    mHandle = -1;
    if (hasMouseCapture()) setMouseCapture(false);
}

// A drag that loses the mouse (another window took it) is dropped, not finished half way.
// (handleMouseUp clears mDrag before it lets go, so a finished drag is not undone here.)
void WolfToolCave::onMouseCaptureLost()
{
    dropDrag();
}

// [WolfViewer 2026-10-15 v3] A finished handle drag is one Undo step (CAVE_DESIGN_V3.md: "Undo removes
// the last thing added ... box"): the box before it, and for a move what moved with it.
void WolfToolCave::finishHandleDrag()
{
    if (!mEdit) return;
    WolfCaves::Design& d = mEdit->mDesign;
    const WolfCaves::Box& a = mDragStart.mBox;
    const WolfCaves::Box& b = d.mBox;
    if (a.mX == b.mX && a.mY == b.mY && a.mW == b.mW && a.mD == b.mD && a.mRot == b.mRot && a.mZ == b.mZ && a.mH == b.mH) return;
    d.mBoxUndo.push_back(mDragStart);
    d.mOrder.push_back('b');
    mDragStart = WolfCaves::BoxUndo();
    mChanged = true;
}

// [WolfViewer 2026-10-14 v2] The dragged rectangle becomes the box: axis-aligned to the drag (the
// turn is set in the window), floor at the ground height at its centre (CAVE_DESIGN_V3.md Flow 1:
// "A redrawn box keeps the last height").
void WolfToolCave::finishBox()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!mEdit || !rgn) return;
    WolfCaves::Design& d = mEdit->mDesign;
    WolfCaves::Box b = d.mBox;   // a redrawn box keeps the height chosen for the last one
    const F32 dw = fabsf(mBX - mAX), dd = fabsf(mBY - mAY);
    F32 cx = (mAX + mBX) * 0.5f, cy = (mAY + mBY) * 0.5f;
    if (dw < 1.f && dd < 1.f)
    {
        // A click, not a drag: a box of the contract's example size centred there.
        // Source: CAVE_DESIGN_V2.md example box w 12, d 8 (Box defaults).
        b.mW = 12.f;
        b.mD = 8.f;
        cx = mAX;
        cy = mAY;
    }
    else
    {
        b.mW = llclamp(dw, WolfCaves::BOX_SIDE_MIN, WolfCaves::BOX_SIDE_MAX);
        b.mD = llclamp(dd, WolfCaves::BOX_SIDE_MIN, WolfCaves::BOX_SIDE_MAX);
        if (dw > WolfCaves::BOX_SIDE_MAX || dd > WolfCaves::BOX_SIDE_MAX)
        {
            mMessage = llformat("A cave box is at most %d m each way, so it was made that size.", (S32)WolfCaves::BOX_SIDE_MAX);
        }
    }
    // Keep it inside the region (the turn is 0 here, so the half sizes are its reach).
    const F32 w = rgn->getWidth();
    cx = llclamp(cx, b.mW * 0.5f, w - b.mW * 0.5f);
    cy = llclamp(cy, b.mD * 0.5f, w - b.mD * 0.5f);
    b.mX = r2(cx);
    b.mY = r2(cy);
    b.mW = r2(b.mW);
    b.mD = r2(b.mD);
    b.mRot = 0.f;
    b.mZ = r2(rgn->getLand().resolveHeightRegion(llclamp(cx, 0.f, w - 0.01f), llclamp(cy, 0.f, w - 0.01f)));
    WolfCaves::BoxUndo before;
    before.mHadBox = d.mHasBox;
    before.mBox = d.mBox;
    d.mBoxUndo.push_back(before);
    d.mHasBox = true;
    d.mBox = b;
    d.mOrder.push_back('b');
    mChanged = true;
}

// [WolfViewer 2026-10-15 v3] The dragged-out cavern is added with the window's cavern height
// (CAVE_DESIGN_V3.md Flow 3), or refused in words.
void WolfToolCave::finishCavern()
{
    if (!mEdit) return;
    WolfCaves::Design& d = mEdit->mDesign;
    if ((S32)d.mCaverns.size() >= WolfCaves::MAX_CAVERNS)
    {
        mMessage = llformat("A cave has at most %d caverns - press Undo to take one back.", WolfCaves::MAX_CAVERNS);
        return;
    }
    const std::string why = WolfCaves::cavernProblem(d, mCavX, mCavY, mCavR);
    if (!why.empty())
    {
        mMessage = why;
        return;
    }
    d.mCaverns.push_back(WolfCaves::Cavern{ mCavX, mCavY, mCavR, r2(cavernHeight()) });
    d.mOrder.push_back('c');
    mChanged = true;
}

bool WolfToolCave::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (!mDrawing) return LLToolPie::getInstance()->handleDoubleClick(x, y, mask);
    if (mPen == PEN_TUNNEL) finishTunnel();
    return true;
}

// Source: wolf_caves floater _finishPassage — a double-click adds its own point twice: dropped.
// [WolfViewer 2026-10-15 v3] Split out of finishTunnel so the window can judge Generate with the
// tunnel still being drawn counted in (CAVE_DESIGN_V3.md Flow 2: Generate finishes it).
bool WolfToolCave::liveTunnel(WolfCaves::Tunnel& out) const
{
    if (mLive.size() < 2) return false;
    std::vector<F32> pts{ mLive[0], mLive[1] };
    for (size_t k = 2; k + 1 < mLive.size(); k += 2)
    {
        const F32 dx = mLive[k] - pts[pts.size() - 2], dy = mLive[k + 1] - pts[pts.size() - 1];
        if (sqrtf(dx * dx + dy * dy) >= 0.5f) { pts.push_back(mLive[k]); pts.push_back(mLive[k + 1]); }
    }
    if (pts.size() < 4) return false;
    out.mP = pts;
    out.mW = tunnelWidth();
    return true;
}

void WolfToolCave::finishTunnel()
{
    if (mLive.empty() || !mEdit) { mLive.clear(); return; }
    WolfCaves::Tunnel t;
    const bool keep = liveTunnel(t);
    mLive.clear();
    if (!keep)
    {
        mMessage = "A tunnel needs at least two points - click points inside the box, then double-click.";
        return;
    }
    mEdit->mDesign.mTunnels.push_back(t);
    mEdit->mDesign.mOrder.push_back('t');
    mChanged = true;
}

bool WolfToolCave::handleKey(KEY key, MASK mask)
{
    // [WolfViewer 2026-10-14] The keys are the pen's only while drawing: with the pen off the
    // tool stays current (it draws the ghost), and Enter must still open chat, Esc reach the rest.
    // Source: lltool.cpp:188-191 LLTool::handleKey returns false (not handled).
    if (!mDrawing) return LLTool::handleKey(key, mask);
    if (key == KEY_RETURN && mPen == PEN_TUNNEL) { finishTunnel(); return true; }
    if (key == KEY_ESCAPE)
    {
        finishTunnel();
        dropDrag();
        mHover = -1;
        mDrawing = false;
        return true;
    }
    return LLTool::handleKey(key, mask);
}

void WolfToolCave::handleDeselect()
{
    finishTunnel();
    mDrawing = false;
    mHover = -1;
    dropDrag();
}

// [WolfViewer 2026-10-14 v2] The box in 3D at its real footprint and height (a wireframe plus a
// light see-through fill), the tunnels as 3D bands at the floor, the doorways as green posts, and
// the generator's see-through rock. Source: WolfToolTerrainPaint::render and the v1 render here for
// the gGL immediate-mode calls (begin/vertex3fv/end, LINES and TRIANGLES); llrender.h:477
// setLineWidth (used as fsmaniprotatejoint.cpp:614-648 does, reset to 1 after); llglstates.h:39
// LLGLDepthTest(enabled, write, func); llmaniptranslate.cpp:1126 LLGLDisable(GL_CULL_FACE).
// [WolfViewer 2026-10-15 v3] + the box handles, the caverns as domes, tunnels at their v3 clear height.
void WolfToolCave::render()
{
    if (!mEdit || !WolfCaves::onWolf()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getHandle() != WolfCaves::instance().recordHandle()) return;
    const LLSurface& land = rgn->getLand();
    const F32 w = rgn->getWidth();
    auto P = [&](F32 x, F32 y, F32 z) { return rgn->getPosAgentFromRegion(LLVector3(x, y, z)); };
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    // The ghost: rock brown, the hill's skin green, see-through, never writing depth.
    const WolfCaves::Preview& pv = WolfCaves::instance().preview();
    if (pv.mReady)
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        for (const WolfCaves::GhostFace& f : pv.mFaces)
        {
            gGL.color4f(f.mRock ? 0.69f : 0.44f, f.mRock ? 0.56f : 0.81f, f.mRock ? 0.42f : 0.44f, f.mRock ? 0.35f : 0.18f);
            gGL.begin(LLRender::TRIANGLES);
            for (const LLVector3& v : f.mTris) gGL.vertex3fv(rgn->getPosAgentFromRegion(v).mV);
            gGL.end();
        }
    }

    const WolfCaves::Design& d = mEdit->mDesign;
    // The box to show: the one being dragged out, else the design's.
    bool have_box = d.mHasBox;
    WolfCaves::Box b = d.mBox;
    if (mDrag == DRAG_NEW_BOX)
    {
        have_box = true;
        b.mX = (mAX + mBX) * 0.5f;
        b.mY = (mAY + mBY) * 0.5f;
        b.mW = llmax(0.1f, fabsf(mBX - mAX));
        b.mD = llmax(0.1f, fabsf(mBY - mAY));
        b.mRot = 0.f;
        b.mZ = land.resolveHeightRegion(llclamp(b.mX, 0.f, w - 0.01f), llclamp(b.mY, 0.f, w - 0.01f));
        if (!d.mHasBox) b.mH = WolfCaves::Box().mH;
    }
    if (!have_box) return;   // tunnels, caverns and doorways need the box (their floor is its floor)

    std::vector<LLVector3> box_lines, box_fill, tunnel_lines, cavern_lines, door_lines, handle_lines, hot_lines;
    auto seg = [](std::vector<LLVector3>& v, const LLVector3& a, const LLVector3& c) { v.push_back(a); v.push_back(c); };
    {
        F32 c[8];
        WolfCaves::boxCorners(b, c);
        LLVector3 lo[4], hi[4];
        for (S32 i = 0; i < 4; ++i) { lo[i] = P(c[2 * i], c[2 * i + 1], b.mZ); hi[i] = P(c[2 * i], c[2 * i + 1], b.mZ + b.mH); }
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 j = (i + 1) % 4;
            seg(box_lines, lo[i], lo[j]);
            seg(box_lines, hi[i], hi[j]);
            seg(box_lines, lo[i], hi[i]);
            // The side wall i-j as two triangles.
            box_fill.insert(box_fill.end(), { lo[i], lo[j], hi[j], lo[i], hi[j], hi[i] });
        }
        box_fill.insert(box_fill.end(), { lo[0], lo[1], lo[2], lo[0], lo[2], lo[3] });
        box_fill.insert(box_fill.end(), { hi[0], hi[1], hi[2], hi[0], hi[2], hi[3] });
    }
    // The box handles while the box pen is down on a box (CAVE_DESIGN_V3.md "Box resize"): a small
    // wire cube at each, about the same size on screen at any distance; the one under the pointer
    // (or being dragged) larger and bright.
    if (mDrawing && mPen == PEN_BOX && d.mHasBox && mDrag != DRAG_NEW_BOX)
    {
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();   // agent frame (llcoordframe.h:102)
        const S32 hot = mDrag == DRAG_NONE ? mHover : mHandle;
        for (S32 i = 0; i < HANDLE_COUNT; ++i)
        {
            const LLVector3 h = rgn->getPosAgentFromRegion(handlePos(b, i));
            F32 r = llclamp(dist_vec(eye, h) * 0.012f, 0.12f, 3.f);
            if (i == hot) r *= 1.6f;
            std::vector<LLVector3>& out = i == hot ? hot_lines : handle_lines;
            LLVector3 v[8];
            for (S32 k = 0; k < 8; ++k) v[k] = h + LLVector3((k & 1) ? r : -r, (k & 2) ? r : -r, (k & 4) ? r : -r);
            static const S32 edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (const auto& e : edges) seg(out, v[e[0]], v[e[1]]);
        }
    }
    // A tunnel: both edges at the floor and at its v3 clear height min(w * 0.9 + 1, h - roof)
    // (CAVE_DESIGN_V3.md tunnels), posts at every point, closed ends.
    auto tunnel = [&](const std::vector<F32>& p, F32 tw)
    {
        const size_t n = p.size() / 2;
        if (n < 2) return;
        const F32 top = b.mZ + llmax(0.1f, llmin(tw * 0.9f + 1.f, b.mH - d.mRoof));
        std::vector<LLVector3> el, er, tl, tr;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t a = i > 0 ? i - 1 : 0, c = llmin(n - 1, i + 1);
            F32 dx = p[2 * c] - p[2 * a], dy = p[2 * c + 1] - p[2 * a + 1];
            const F32 l = llmax(1e-3f, sqrtf(dx * dx + dy * dy));
            dx /= l; dy /= l;
            const F32 lx = p[2 * i] - dy * tw * 0.5f, ly = p[2 * i + 1] + dx * tw * 0.5f;
            const F32 rx = p[2 * i] + dy * tw * 0.5f, ry = p[2 * i + 1] - dx * tw * 0.5f;
            el.push_back(P(lx, ly, b.mZ)); er.push_back(P(rx, ry, b.mZ));
            tl.push_back(P(lx, ly, top));  tr.push_back(P(rx, ry, top));
            seg(tunnel_lines, el.back(), tl.back());
            seg(tunnel_lines, er.back(), tr.back());
            if (i > 0)
            {
                seg(tunnel_lines, el[i - 1], el[i]); seg(tunnel_lines, er[i - 1], er[i]);
                seg(tunnel_lines, tl[i - 1], tl[i]); seg(tunnel_lines, tr[i - 1], tr[i]);
            }
        }
        for (size_t i : { (size_t)0, n - 1 }) { seg(tunnel_lines, el[i], er[i]); seg(tunnel_lines, tl[i], tr[i]); }
    };
    for (const WolfCaves::Tunnel& t : d.mTunnels) tunnel(t.mP, t.mW);
    if (!mLive.empty())
    {
        std::vector<F32> lp = mLive;
        if (mHaveCursor) { lp.push_back(mCursorX); lp.push_back(mCursorY); }
        tunnel(lp, tunnelWidth());
    }
    // [WolfViewer 2026-10-15 v3] A cavern as a dome wireframe: a ring at the floor, a ring partway up
    // and one near its clear height min(h, box.h - roof) (CAVE_DESIGN_V3.md caverns), and arcs from
    // the floor to the top.
    auto dome = [&](F32 cx, F32 cy, F32 r, F32 ch)
    {
        constexpr S32 RING = 36, ARC = 8, MERIDIANS = 8;
        ch = llmax(0.1f, ch);
        for (const F32 f : { 0.f, 0.6f, 0.95f })
        {
            const F32 rr = r * sqrtf(1.f - f * f), z = b.mZ + (f > 0.f ? ch * f : 0.05f);
            for (S32 i = 0; i < RING; ++i)
            {
                const F32 a0 = (F32)i / RING * F_TWO_PI, a1 = (F32)(i + 1) / RING * F_TWO_PI;
                seg(cavern_lines, P(cx + cosf(a0) * rr, cy + sinf(a0) * rr, z), P(cx + cosf(a1) * rr, cy + sinf(a1) * rr, z));
            }
        }
        for (S32 m = 0; m < MERIDIANS; ++m)
        {
            const F32 a = (F32)m / MERIDIANS * F_TWO_PI;
            for (S32 k = 0; k < ARC; ++k)
            {
                const F32 p0 = (F32)k / ARC * F_PI_BY_TWO, p1 = (F32)(k + 1) / ARC * F_PI_BY_TWO;
                seg(cavern_lines, P(cx + cosf(a) * r * cosf(p0), cy + sinf(a) * r * cosf(p0), b.mZ + ch * sinf(p0)),
                                  P(cx + cosf(a) * r * cosf(p1), cy + sinf(a) * r * cosf(p1), b.mZ + ch * sinf(p1)));
            }
        }
    };
    for (const WolfCaves::Cavern& cv : d.mCaverns) dome(cv.mX, cv.mY, cv.mR, llmin(cv.mH, b.mH - d.mRoof));
    if (mDrag == DRAG_CAVERN) dome(mCavX, mCavY, mCavR, llmin(cavernHeight(), b.mH - d.mRoof));
    // A doorway: a post from the floor and a ring on the floor where people walk in.
    for (const WolfCaves::Door& e : d.mDoors)
    {
        const F32 top = b.mZ + llmin(b.mH, 3.f);
        seg(door_lines, P(e.mX, e.mY, b.mZ), P(e.mX, e.mY, top));
        for (S32 i = 0; i < 24; ++i)
        {
            const F32 a0 = (F32)i / 24.f * F_TWO_PI, a1 = (F32)(i + 1) / 24.f * F_TWO_PI;
            seg(door_lines, P(e.mX + cosf(a0), e.mY + sinf(a0), b.mZ + 0.05f), P(e.mX + cosf(a1), e.mY + sinf(a1), b.mZ + 0.05f));
            seg(door_lines, P(e.mX + cosf(a0) * 0.6f, e.mY + sinf(a0) * 0.6f, top), P(e.mX + cosf(a1) * 0.6f, e.mY + sinf(a1) * 0.6f, top));
        }
    }

    auto draw = [](const std::vector<LLVector3>& v, U8 mode)
    {
        if (v.empty()) return;
        gGL.begin(mode);
        for (const LLVector3& p : v) gGL.vertex3fv(p.mV);
        gGL.end();
    };
    // The fill: light, behind the hill where the hill is in front (depth tested, not written).
    {
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.color4f(1.f, 0.70f, 0.28f, 0.10f);
        draw(box_fill, LLRender::TRIANGLES);
    }
    // The lines twice: faint through the ground so the box is always visible while editing, then
    // bright and thicker where nothing is in front of them.
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.color4f(1.f, 0.70f, 0.28f, 0.35f);
        draw(box_lines, LLRender::LINES);
        draw(tunnel_lines, LLRender::LINES);
        gGL.color4f(0.55f, 0.80f, 1.f, 0.35f);
        draw(cavern_lines, LLRender::LINES);
        gGL.color4f(0.37f, 0.83f, 0.37f, 0.35f);
        draw(door_lines, LLRender::LINES);
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 0.70f, 0.28f, 1.f);
        draw(box_lines, LLRender::LINES);
        gGL.color4f(1.f, 0.85f, 0.55f, 1.f);
        draw(tunnel_lines, LLRender::LINES);
        gGL.color4f(0.55f, 0.80f, 1.f, 1.f);
        draw(cavern_lines, LLRender::LINES);
        gGL.color4f(0.37f, 0.83f, 0.37f, 1.f);
        draw(door_lines, LLRender::LINES);
        gGL.setLineWidth(1.f);
    }
    // The handles last and never hidden: they must be findable wherever the ground is.
    {
        LLGLDepthTest depth(GL_FALSE);
        gGL.setLineWidth(2.f);
        gGL.color4f(1.f, 1.f, 1.f, 0.9f);
        draw(handle_lines, LLRender::LINES);
        gGL.setLineWidth(3.f);
        gGL.color4f(1.f, 1.f, 0.3f, 1.f);
        draw(hot_lines, LLRender::LINES);
        gGL.setLineWidth(1.f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterCaves — the window
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterCaves::WolfFloaterCaves(const LLSD& key) : LLFloater(key) {}

WolfFloaterCaves::~WolfFloaterCaves()
{
    if (WolfToolCave::instanceExists()) WolfToolCave::getInstance()->setEdit(nullptr);
}

// [WolfViewer 2026-10-14 v2] The window follows the steps of the Flow, one primary action per step.
// [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md Flow: Draw the box / Draw a tunnel / Add caverns /
// Add doorways / (look) / Generate.
bool WolfFloaterCaves::postBuild()
{
    mList = getChild<LLScrollListCtrl>("cave_list");
    mNew = getChild<LLButton>("cave_new");
    mEdit = getChild<LLButton>("cave_edit");
    mRemove = getChild<LLButton>("cave_remove");
    mBuild = getChild<LLButton>("cave_build");
    mBack = getChild<LLButton>("cave_back");
    mUndo = getChild<LLButton>("cave_undo");
    mCancel = getChild<LLButton>("cave_cancel");
    mPenBox = getChild<LLButton>("cave_pen_box");
    mPenTunnel = getChild<LLButton>("cave_pen_tunnel");
    mPenCavern = getChild<LLButton>("cave_pen_cavern");
    mPenDoor = getChild<LLButton>("cave_pen_door");
    mName = getChild<LLLineEditor>("cave_name");
    mBoxW = getChild<LLSpinCtrl>("cave_box_w");
    mBoxD = getChild<LLSpinCtrl>("cave_box_d");
    mBoxH = getChild<LLSpinCtrl>("cave_box_h");
    mBoxTurn = getChild<LLSpinCtrl>("cave_box_turn");
    mBoxFloor = getChild<LLSpinCtrl>("cave_box_floor");
    mWidth = getChild<LLSliderCtrl>("cave_width");
    mCavernH = getChild<LLSliderCtrl>("cave_cavern_h");
    mRoof = getChild<LLSliderCtrl>("cave_roof");
    mRough = getChild<LLSliderCtrl>("cave_rough");
    mRock = getChild<LLComboBox>("cave_rock");
    mRockTex = getChild<LLTextureCtrl>("cave_rock_tex");
    mLight = getChild<LLComboBox>("cave_light");
    mLightsStyle = getChild<LLComboBox>("cave_lights_style");
    mLightsTone = getChild<LLComboBox>("cave_lights_tone");
    mLightsUp = getChild<LLButton>("cave_lights_up");
    mLightsOff = getChild<LLButton>("cave_lights_off");
    mCounts = getChild<LLTextBox>("cave_counts");
    mStats = getChild<LLTextBox>("cave_stats");
    mDig = getChild<LLTextBox>("cave_dig");
    mStatus = getChild<LLTextBox>("cave_status");
    mWhy = getChild<LLTextBox>("cave_why");

    mNew->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNew(); });
    mEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEdit(); });
    mRemove->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemove(); });
    mBuild->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mBack->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBack(); });
    mUndo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    mCancel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancel(); });
    mPenBox->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolCave::PEN_BOX); });
    mPenTunnel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolCave::PEN_TUNNEL); });
    mPenCavern->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolCave::PEN_CAVERN); });
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onListPick(); });
    mPenDoor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPen(WolfToolCave::PEN_DOOR); });
    auto changed = [this](LLUICtrl*, const LLSD&) { readControls(); schedulePreview(); };
    // The box spinners edit the box live: the wireframe follows at once, the rock after the preview.
    for (LLSpinCtrl* s : { mBoxW, mBoxD, mBoxH, mBoxTurn, mBoxFloor }) s->setCommitCallback(changed);
    mWidth->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedSettings.setF32("WolfCavePassageWidth", (F32)c->getValue().asReal()); });
    // [WolfViewer 2026-10-15 v3] The height of the next cavern (app_settings/settings.xml WolfCaveCavernHeight).
    mCavernH->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedSettings.setF32("WolfCaveCavernHeight", (F32)c->getValue().asReal()); });
    mRoof->setCommitCallback(changed);
    mRough->setCommitCallback(changed);
    mRock->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRockCombo(); });
    mRockTex->setCommitCallback([this](LLUICtrl*, const LLSD&) { mRock->setValue("custom"); readControls(); schedulePreview(); });
    // Picker closed without a choice: the combo goes back to the texture the cave really has.
    // Source: lltexturectrl.h:243 setOnCancelCallback.
    mRockTex->setOnCancelCallback([this](LLUICtrl*, const LLSD&) { if (mEditing) showEditor(); });
    mLight->setCommitCallback([this](LLUICtrl*, const LLSD&) { mCave.mLight = mLight->getValue().asString(); });
    mName->setCommitCallback([this](LLUICtrl*, const LLSD&) { readControls(); });
    // [WOLF CAVE LIGHTS 2026-10-23] Paul: "let them choose the light style rather than forcing it" - the last choice is kept.
    auto keep_choice = [this](LLUICtrl*, const LLSD&)
    {
        WolfCaveLights::saveChoice(WolfCaveLights::styleFrom(mLightsStyle->getValue().asString(), WolfCaveLights::STYLE_TORCHES),
                                   mLightsTone->getValue().asString() != "cool");
    };
    mLightsStyle->setCommitCallback(keep_choice);
    mLightsTone->setCommitCallback(keep_choice);
    mLightsUp->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLightUp(); });
    mLightsOff->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveLights(); });
    mLightsStyle->setValue(WolfCaveLights::styleKey(WolfCaveLights::savedStyle(WolfCaveLights::styleForWalls(true))));
    mLightsTone->setValue(WolfCaveLights::savedWarm() ? "warm" : "cool");
    mWidth->setValue(WolfToolCave::tunnelWidth());
    mCavernH->setValue(WolfToolCave::cavernHeight());
    showEditor();
    return true;
}

void WolfFloaterCaves::onOpen(const LLSD& key)
{
    if (!WolfCaves::onWolf())
    {
        WolfGrid::noteWolfOnly("Make Caves");   // [WOLF GRID GATE 2026-10-17/]
        closeFloater();
        return;
    }
    WolfCaves::instance().refresh();
    mShownCaves = (size_t)-1;
    // [WolfViewer 2026-10-14] Closing keeps the drawing (mEditing, mCave) but puts the tool down;
    // reopening gives the drawing its tool back, so the outline and the ghost show again and the
    // pen can be armed. A move to another region meanwhile is still caught by draw().
    if (mEditing)
    {
        showTool();
        schedulePreview();
    }
}

void WolfFloaterCaves::onClose(bool app_quitting)
{
    if (!WolfToolCave::instanceExists()) return;
    stopTool();
    WolfToolCave::getInstance()->setEdit(nullptr);
    if (!app_quitting && WolfCaves::instance().busy())
    {
        alertOK("The Cave Builder is still finishing in the background - reopen it to see the result.");
    }
}

void WolfFloaterCaves::stopTool()
{
    WolfToolCave* tool = WolfToolCave::getInstance();
    tool->setDrawing(false);
    if (LLToolMgr::getInstance()->getCurrentTool() == tool)
    {
        LLToolset* ts = LLToolMgr::getInstance()->getCurrentToolset();
        if (ts) ts->selectTool(LLToolCompTranslate::getInstance());
    }
}

// The tool draws the box and the ghost (LLTool::render runs for the current tool only), so it is
// the current tool while a cave is being edited; with the pen off it leaves clicks alone.
void WolfFloaterCaves::showTool()
{
    WolfToolCave* tool = WolfToolCave::getInstance();
    LLToolset* ts = LLToolMgr::getInstance()->getCurrentToolset();
    if (ts && LLToolMgr::getInstance()->getCurrentTool() != tool) ts->selectTool(tool);
    tool->setEdit(&mCave);
}

void WolfFloaterCaves::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    mStatus->setColor(error ? LLColor4(1.f, 0.6f, 0.6f, 1.f) : LLColor4(0.78f, 0.82f, 0.88f, 1.f));
}

void WolfFloaterCaves::rebuildList()
{
    const LLSD sel = mList->getSelectedValue();
    mList->deleteAllItems();
    for (const WolfCaves::Cave& c : WolfCaves::instance().caves())
    {
        std::string st;
        // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "Caves from the old builder".
        if (c.mOld) st = "Old design - remove and draw again";
        else if (!c.mBuilt.isMap()) st = "Not built yet";
        else if (!rockRecorded(c)) st = "Rock gone, ground not put back - Remove to finish";
        else if (!c.mBuilt["dug"].asBoolean()) st = "Rock rezzed, ground not finished";
        else
        {
            st = llformat("Built, land impact %d", c.mBuilt["li"].asInteger());
            if (c.mBuilt["limited"].asInteger() > 0) st += llformat(", %d points held by a limit", c.mBuilt["limited"].asInteger());
            const std::string lit = WolfCaveLights::summary(c.mLights);   // [WOLF CAVE LIGHTS 2026-10-23]
            if (!lit.empty()) st += ", " + lit;
        }
        LLSD row;
        row["value"] = c.mId;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = c.mName;
        row["columns"][1]["column"] = "state";
        row["columns"][1]["value"] = st;
        mList->addElement(row);
    }
    if (sel.isDefined()) mList->selectByValue(sel);
    mShownCaves = WolfCaves::instance().caves().size();
    mListHandle = WolfCaves::instance().recordHandle();
}

// Fill the editor from mCave, or switch it off when not editing.
void WolfFloaterCaves::showEditor()
{
    const WolfCaves::Design& d = mCave.mDesign;
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mName, mPenBox, mPenTunnel, mPenCavern, mPenDoor, mWidth, mCavernH, mRoof, mRough,
                                                         mRock, mRockTex, mLight, mUndo, mBack })
    {
        c->setEnabled(mEditing);
    }
    mNew->setEnabled(!mEditing);
    showBox();
    if (!mEditing)
    {
        mStats->setText(LLStringUtil::null);
        mDig->setText(LLStringUtil::null);
        mShownDig = "-";
        return;
    }
    mName->setText(mCave.mName);
    mRoof->setValue(d.mRoof);
    mRough->setValue(d.mRoughness);
    // [WolfViewer 2026-10-14 v2] The two built-in rocks by name, anything else is the chosen one.
    if (d.mRock == WolfCaves::terrainRockId() || d.mRock.isNull()) mRock->setValue("terrain_rock");
    else if (d.mRock == WolfCaves::rocksId()) mRock->setValue("rocks");
    else { mRock->setValue("custom"); mRockTex->setImageAssetID(d.mRock); }
    mLight->setValue(mCave.mLight);
    const WolfCaves::Cave* stored = WolfCaves::instance().find(mCave.mId);
    mBuild->setLabel(getString(stored && stored->mBuilt.isMap() ? "str_rebuild" : "str_build"));
}

// [WolfViewer 2026-10-14 v2] The box spinners: live only once there is a box to edit.
void WolfFloaterCaves::showBox()
{
    const WolfCaves::Design& d = mCave.mDesign;
    const bool on = mEditing && d.mHasBox;
    for (LLSpinCtrl* s : { mBoxW, mBoxD, mBoxH, mBoxTurn, mBoxFloor }) s->setEnabled(on);
    if (!on) return;
    mBoxW->set(d.mBox.mW);
    mBoxD->set(d.mBox.mD);
    mBoxH->set(d.mBox.mH);
    mBoxTurn->set(d.mBox.mRot);
    mBoxFloor->set(d.mBox.mZ);
}

void WolfFloaterCaves::readBox()
{
    WolfCaves::Design& d = mCave.mDesign;
    if (!mEditing || !d.mHasBox) return;
    // Source: CAVE_DESIGN_V3.md box limits (the spinners carry the same min/max in the XML).
    d.mBox.mW = llclamp(mBoxW->get(), WolfCaves::BOX_SIDE_MIN, WolfCaves::BOX_SIDE_MAX);
    d.mBox.mD = llclamp(mBoxD->get(), WolfCaves::BOX_SIDE_MIN, WolfCaves::BOX_SIDE_MAX);
    d.mBox.mH = llclamp(mBoxH->get(), WolfCaves::BOX_H_MIN, WolfCaves::BOX_H_MAX);
    F32 rot = fmodf(mBoxTurn->get(), 360.f);
    d.mBox.mRot = rot < 0.f ? rot + 360.f : rot;
    d.mBox.mZ = llclamp(mBoxFloor->get(), WolfCaves::BOX_Z_MIN, WolfCaves::BOX_Z_MAX);
}

void WolfFloaterCaves::readControls()
{
    if (!mEditing) return;
    WolfCaves::Design& d = mCave.mDesign;
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    mCave.mName = name.empty() ? "Wolf Cave" : name.substr(0, 63);
    readBox();
    d.mRoof = (F32)mRoof->getValue().asReal();
    d.mRoughness = (F32)mRough->getValue().asReal();
    const std::string rock = mRock->getValue().asString();
    if (rock == "terrain_rock") d.mRock = WolfCaves::terrainRockId();
    else if (rock == "rocks") d.mRock = WolfCaves::rocksId();
    else if (mRockTex->getImageAssetID().notNull()) d.mRock = mRockTex->getImageAssetID();
}

void WolfFloaterCaves::onRockCombo()
{
    // "Choose texture..." opens the picker; the cave keeps its texture until one is chosen.
    if (mRock->getValue().asString() == "custom")
    {
        setStatus(getString("str_choose_texture"), false);
        mRockTex->showPicker(true);
        return;
    }
    readControls();
    schedulePreview();
}

void WolfFloaterCaves::schedulePreview()
{
    WolfCaves::instance().clearPreview();
    mPreviewAt = LLFrameTimer::getElapsedSeconds() + 0.6;
    mGenerateWhenReady = false;   // a change after Generate: the user presses it again
}

// [WolfViewer 2026-10-14 v2] Paul: "the build button is not available ... it was also not clear".
// Generate is never greyed out without these words right above it.
std::string WolfFloaterCaves::whyNotGenerate() const
{
    if (!mEditing) return getString("str_not_editing");
    const WolfCaves& wc = WolfCaves::instance();
    if (wc.busy()) return getString("str_busy");
    LLViewerRegion* rgn = gAgent.getRegion();
    const F32 region_w = rgn ? rgn->getWidth() : 256.f;
    // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md Flow 2: pressing Generate finishes a tunnel still
    // being drawn instead of dropping it, so Generate is judged with that tunnel in. (Paul's first
    // tunnel was often still being drawn when he pressed Generate.) onBuild finishes it, works the
    // rock out again and goes on by itself.
    WolfCaves::Tunnel live;
    if (WolfToolCave::instanceExists() && WolfToolCave::getInstance()->liveTunnel(live))
    {
        WolfCaves::Design with = mCave.mDesign;
        with.mTunnels.push_back(live);
        return WolfCaves::designProblem(with, region_w);
    }
    const std::string problem = WolfCaves::designProblem(mCave.mDesign, region_w);
    if (!problem.empty()) return problem;
    const WolfCaves::Preview& p = wc.preview();
    if (!wc.previewError().empty()) return "The cave cannot be made like this: " + wc.previewError();
    if (!p.mReady) return getString("str_working");
    if (p.mLi <= 0) return getString("str_no_rock");
    return std::string();
}

void WolfFloaterCaves::onNew()
{
    mCave = WolfCaves::newCave();
    mEditing = true;
    mGenerateWhenReady = false;   // [WolfViewer 2026-10-15 v3] a Generate pending for another drawing
    WolfToolCave::getInstance()->setEdit(&mCave);
    showTool();
    WolfCaves::instance().clearPreview();
    showEditor();
    // Step 1 is the only thing to do first: the box pen is ready at once.
    onPen(WolfToolCave::PEN_BOX);
}

// [WolfViewer 2026-10-15 v3] The cave picked in the list.
const WolfCaves::Cave* WolfFloaterCaves::picked() const
{
    return mList->getFirstSelected() ? WolfCaves::instance().find(mList->getSelectedValue().asString()) : nullptr;
}

// [WolfViewer 2026-10-15 v3] Picking an old-design cave says why Change is greyed out
// (CAVE_DESIGN_V3.md "Caves from the old builder": Remove only).
void WolfFloaterCaves::onListPick()
{
    const WolfCaves::Cave* c = picked();
    if (c && c->mOld) setStatus(getString("str_old_design"), false);
    showLightChoice();
}

// [WOLF CAVE LIGHTS 2026-10-23] The picked cave's own light style and colour when it has lights; else the last choice; else
// torches (a cave's walls are natural rock).
void WolfFloaterCaves::showLightChoice()
{
    const WolfCaves::Cave* c = picked();
    const std::string id = c ? c->mId : std::string();
    if (id == mLightPickShown) return;
    mLightPickShown = id;
    if (!c || !c->mLights.isMap() || !c->mLights.has("style")) return;
    mLightsStyle->setValue(WolfCaveLights::styleKey(WolfCaveLights::styleFrom(c->mLights["style"].asString(), WolfCaveLights::STYLE_TORCHES)));
    mLightsTone->setValue(c->mLights["tone"].asString() == "cool" ? "cool" : "warm");
}

void WolfFloaterCaves::onLightUp()
{
    const WolfCaves::Cave* c = picked();
    if (!c) { setStatus(getString("str_pick_one"), true); return; }
    if (WolfCaves::instance().busy()) { setStatus(getString("str_busy"), true); return; }
    if (c->mOld || !c->mBuilt.isMap()) { setStatus("Generate the cave first, then light it up.", true); return; }
    const WolfCaveLights::EStyle style = WolfCaveLights::styleFrom(mLightsStyle->getValue().asString(), WolfCaveLights::STYLE_TORCHES);
    const bool warm = mLightsTone->getValue().asString() != "cool";
    const std::vector<WolfCaveLights::Lamp> lamps = WolfCaves::caveLamps(*c, style);
    if (lamps.empty()) { setStatus("No place for a lamp was found in \"" + c->mName + "\".", true); return; }
    const std::string id = c->mId;
    WolfCaveLights::confirmLightUp(c->mName, style, (S32)lamps.size(), c->mLights, [id, lamps, style, warm](bool keep)
    {
        // lights() itself says why it cannot start (busy, off Wolf, moved region)
        WolfCaves::instance().lights(id, false, lamps, style, warm, keep);
    });
}

void WolfFloaterCaves::onRemoveLights()
{
    const WolfCaves::Cave* c = picked();
    if (!c) { setStatus(getString("str_pick_one"), true); return; }
    if (WolfCaves::instance().busy()) { setStatus(getString("str_busy"), true); return; }
    const std::string id = c->mId;
    WolfCaveLights::confirmRemove(c->mName, c->mLights, [id](bool keep)
    {
        WolfCaves::instance().lights(id, true, {}, WolfCaveLights::STYLE_HIDDEN, true, keep);
    });
}

void WolfFloaterCaves::onEdit()
{
    const WolfCaves::Cave* c = WolfCaves::instance().find(mList->getSelectedValue().asString());
    if (!c) { setStatus(getString("str_pick_one"), true); return; }
    if (c->mOld) { setStatus(getString("str_old_design"), true); return; }
    mCave = *c;
    mEditing = true;
    mGenerateWhenReady = false;   // [WolfViewer 2026-10-15 v3]
    WolfToolCave::getInstance()->setEdit(&mCave);
    showTool();
    showEditor();
    schedulePreview();
}

void WolfFloaterCaves::onBack()
{
    stopTool();
    mEditing = false;
    mGenerateWhenReady = false;   // [WolfViewer 2026-10-15 v3]
    WolfToolCave::getInstance()->setEdit(nullptr);
    WolfCaves::instance().clearPreview();
    showEditor();
}

// [WolfViewer 2026-10-14 v2] Each step's button arms its own pen; pressing it again puts the pen
// down (the tool stays current so the box and the ghost stay drawn; with the pen off it hands
// clicks to LLToolPie).
void WolfFloaterCaves::onPen(WolfToolCave::EPen p)
{
    WolfToolCave* tool = WolfToolCave::getInstance();
    if (tool->drawing() && tool->pen() == p && LLToolMgr::getInstance()->getCurrentTool() == tool)
    {
        tool->setDrawing(false);
        setStatus(LLStringUtil::null, false);
        return;
    }
    if (!WolfCaves::onWolf()) { setStatus(getString("str_not_wolf"), true); return; }
    LLToolset* ts = LLToolMgr::getInstance()->getCurrentToolset();
    if (!ts) { setStatus(getString("str_no_toolset"), true); return; }
    if (p != WolfToolCave::PEN_BOX && !mCave.mDesign.mHasBox) { setStatus(getString("str_box_first"), true); return; }
    showTool();
    // CAVE_DESIGN_V3.md Flow 2: another pen finishes a tunnel still being drawn (setPen), so a
    // cavern can go on the tunnel just drawn.
    tool->setPen(p);
    if (p == WolfToolCave::PEN_CAVERN && mCave.mDesign.mTunnels.empty())
    {
        // setPen kept any tunnel being drawn, so none exists at all: no pen is left down.
        tool->setDrawing(false);
        setStatus(getString("str_tunnel_first"), true);
        return;
    }
    tool->setDrawing(true);
    setStatus(getString(p == WolfToolCave::PEN_BOX ? "str_pen_box" : p == WolfToolCave::PEN_TUNNEL ? "str_pen_tunnel"
                        : p == WolfToolCave::PEN_CAVERN ? "str_pen_cavern" : "str_pen_door"), false);
}

// Undo takes back the last thing added (CAVE_DESIGN_V3.md Flow): a doorway, a cavern, a tunnel, or
// the last box drawn or handle drag (the box before it comes back, and for a move everything it
// carried). A cave loaded from the list has no history: doorways go first, then caverns, then
// tunnels, then the box.
void WolfFloaterCaves::onUndo()
{
    // A tunnel still being drawn is the last thing added: Undo takes that back on its own.
    if (WolfToolCave::getInstance()->dropLive())
    {
        setStatus(getString("str_undo_live"), false);
        return;
    }
    WolfCaves::Design& d = mCave.mDesign;
    char last = d.mOrder.empty() ? 0 : d.mOrder.back();
    if (!d.mOrder.empty()) d.mOrder.pop_back();
    if (!last) last = !d.mDoors.empty() ? 'd' : (!d.mCaverns.empty() ? 'c' : (!d.mTunnels.empty() ? 't' : (d.mHasBox ? 'b' : 0)));
    if (last == 'd' && !d.mDoors.empty()) d.mDoors.pop_back();
    else if (last == 'c' && !d.mCaverns.empty()) d.mCaverns.pop_back();
    else if (last == 't' && !d.mTunnels.empty()) d.mTunnels.pop_back();
    else if (last == 'b')
    {
        if (!d.mBoxUndo.empty())
        {
            const WolfCaves::BoxUndo& u = d.mBoxUndo.back();
            d.mHasBox = u.mHadBox;
            d.mBox = u.mBox;
            if (u.mMoved)
            {
                // [WolfViewer 2026-10-15 v3] Undo is last-in first-out, so what the move carried is
                // exactly what is here now: it goes back to where it was before the move.
                d.mTunnels = u.mTunnels;
                d.mCaverns = u.mCaverns;
                d.mDoors = u.mDoors;
            }
            d.mBoxUndo.pop_back();
        }
        else d.mHasBox = false;
    }
    else { setStatus(getString("str_nothing_to_undo"), false); return; }
    showBox();
    schedulePreview();
}

// [WolfViewer 2026-10-14] Stop the running job at its next safe point (between floods); the job
// itself records where it stood and says so.
void WolfFloaterCaves::onCancel()
{
    if (!WolfCaves::instance().busy()) return;
    WolfCaves::instance().cancel();
    setStatus(getString("str_stopping"), false);
}

void WolfFloaterCaves::onBuild()
{
    // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md Flow 2: Generate finishes a tunnel still being drawn
    // instead of dropping it. The rock then has to be worked out again; Generate goes on by itself
    // once the new preview is ready (draw(), mGenerateWhenReady).
    if (mEditing)
    {
        WolfToolCave* tool = WolfToolCave::getInstance();
        tool->finishTunnel();
        const std::string tm = tool->takeMessage();   // e.g. a one-point tunnel could not be kept
        if (tool->takeChanged())
        {
            showBox();
            schedulePreview();
            mGenerateWhenReady = true;
            setStatus(getString("str_finished_tunnel"), false);
            return;
        }
        if (!tm.empty()) { setStatus(tm, true); return; }
    }
    readControls();
    // [WolfViewer 2026-10-14] Say why Generate did nothing.
    const std::string why = whyNotGenerate();
    if (!why.empty()) { setStatus(why, true); return; }
    const WolfCaves::Preview& p = WolfCaves::instance().preview();
    const WolfCaves::Cave* stored = WolfCaves::instance().find(mCave.mId);
    const bool rebuild = stored && stored->mBuilt.isMap();
    // A rebuild needs the old rock in view to take it away, unless the record says it is gone.
    if (rebuild && rockRecorded(*stored) && !WolfCaves::instance().findRock(*stored))
    {
        setStatus(getString("str_rebuild_far"), true);
        alertOK(getString("str_rebuild_far"));
        return;
    }
    std::string msg = rebuild ? "The old rock is taken away and its ground put back first. Then " : std::string();
    msg += llformat("\"%s\" will rez a %d-prim rock object owned by you (land impact %d on the parcel under the cave's middle) ",
                    mCave.mName.c_str(), p.mLi, p.mLi);
    // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "dig = false": no terrain targets at all.
    if (p.mDig)
        msg += llformat("and lower %d points of ground to the cave floor (%.2f m). Remove puts the ground back later.", p.mTargets, mCave.mDesign.mBox.mZ);
    else
        msg += "and change no ground: you can't change the ground here, so the cave stands on the ground as it is.";
    const WolfCaves::Cave cave = mCave;
    LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", msg), LLSD(),
        [cave](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
            // [WolfViewer 2026-10-14] build() itself says why it cannot start (busy, off Wolf,
            // moved region) instead of this callback returning silently.
            WolfCaves::instance().build(cave);
        });
}

void WolfFloaterCaves::onRemove()
{
    const WolfCaves::Cave* c = WolfCaves::instance().find(mList->getSelectedValue().asString());
    if (!c) { setStatus(getString("str_pick_one"), true); return; }
    if (WolfCaves::instance().busy()) { setStatus(getString("str_busy"), true); return; }
    LLViewerObject* rock = WolfCaves::instance().findRock(*c);
    const bool forget = rockRecorded(*c) && !rock;
    std::string msg;
    if (c->mBuilt.isMap() && !rockRecorded(*c))
    {
        // [WolfViewer 2026-10-14] A removal that stopped after the rock was taken away.
        msg = llformat("Its rock is already gone; its %d ground points are put back as they were. ", (S32)c->mBuilt["terrain"].size() / 4);
    }
    else if (c->mBuilt.isMap() && rock)
    {
        msg = llformat("The rock (%d prims) is %s and %d ground points are put back as they were. ", c->mBuilt["li"].asInteger(),
                       rock->permYouOwner() ? "deleted (into your Trash if this region keeps deleted objects)" : "returned to its owner",
                       (S32)c->mBuilt["terrain"].size() / 4);
    }
    else if (forget)
    {
        msg = llformat("The cave's rock is not in view from here, so this viewer cannot take it away. Press Cancel, stand nearer and Remove again - "
                       "or press Yes to FORGET the cave: its %d ground points are put back, the record goes, and the rock STAYS for you to delete by hand. ",
                       (S32)c->mBuilt["terrain"].size() / 4);
    }
    else
    {
        msg = "This cave was never built; only its drawing is deleted. ";
    }
    // [WOLF CAVE LIGHTS 2026-10-23] its lamps go too; any not in view are said
    if (c->mLights.isMap() && c->mLights["objects"].size() > 0 && !c->mOld)
    {
        std::vector<LLUUID> roots;
        LLSD unseen;
        WolfCaveLights::find(c->mLights["objects"], roots, unseen);
        msg += "Its " + WolfCaveLights::summary(c->mLights) + " are taken away too"
             + (unseen.size() > 0 ? llformat(" (%d light part%s not in view from here stay%s where %s: delete %s by hand). ",
                                            (S32)unseen.size(), unseen.size() == 1 ? "" : "s", unseen.size() == 1 ? "s" : "",
                                            unseen.size() == 1 ? "it is" : "they are", unseen.size() == 1 ? "it" : "them")
                                  : std::string(". "));
    }
    msg = "Remove \"" + c->mName + "\"? " + msg + "This cannot be undone.";
    const std::string id = c->mId;
    LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", msg), LLSD(),
        [id, forget](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
            // [WolfViewer 2026-10-14] remove() itself says why it cannot start.
            WolfCaves::instance().remove(id, forget);
        });
}

void WolfFloaterCaves::draw()
{
    // Off Wolf (a hypergrid jump, a region the grid does not know): put everything down and go.
    if (!WolfCaves::onWolf())
    {
        stopTool();
        closeFloater();
        return;
    }
    WolfCaves& wc = WolfCaves::instance();
    WolfToolCave* tool = WolfToolCave::getInstance();
    if (!wc.fetching() && (wc.caves().size() != mShownCaves || wc.recordHandle() != mListHandle)) rebuildList();
    // A crossing or teleport: the window shows the NEW region's caves; a drawing made in the old
    // one is put away (its coordinates mean nothing here). Not while a job runs: it stops itself.
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        const U64 here = rgn ? rgn->getHandle() : 0;
        if (!wc.busy() && !wc.fetching() && here != mShownRegion)
        {
            const bool had_drawing = mEditing && mShownRegion != 0;
            mShownRegion = here;
            if (mEditing) onBack();
            wc.refresh();
            mShownCaves = (size_t)-1;
            if (had_drawing) setStatus(getString("str_moved"), false);
        }
    }
    if (mWasBusy && !wc.busy())
    {
        rebuildList();
        mLastStatus = wc.status();
        setStatus(mLastStatus, false);
        // A finished build or removal returns to the list; a failure keeps the drawing to fix.
        // [WolfViewer 2026-10-14] Only when the job was about the cave being drawn: removing
        // another cave from the list must not throw away the drawing in the editor (the list
        // was rebuilt just above).
        const WolfCaves::EResult r = wc.takeResult();
        if ((r == WolfCaves::RESULT_BUILT || r == WolfCaves::RESULT_REMOVED) && mEditing && wc.jobCave() == mCave.mId) onBack();
    }
    mWasBusy = wc.busy();
    mCancel->setEnabled(wc.busy() && !wc.cancelling());
    mRemove->setEnabled(!wc.busy() && mList->getFirstSelected() != nullptr);
    // [WOLF CAVE LIGHTS 2026-10-23] a built cave can be lit; one with lights listed can have them taken away
    {
        const WolfCaves::Cave* pc = picked();
        const bool can_light = !wc.busy() && pc && !pc->mOld && pc->mBuilt.isMap();
        mLightsUp->setEnabled(can_light);
        mLightsStyle->setEnabled(can_light);
        mLightsTone->setEnabled(can_light);
        mLightsOff->setEnabled(!wc.busy() && pc && pc->mLights.isMap() && pc->mLights["objects"].size() > 0);
        showLightChoice();
    }
    // [WolfViewer 2026-10-15 v3] An old-design cave can only be removed (CAVE_DESIGN_V3.md "Caves from the old builder").
    const WolfCaves::Cave* sel = picked();
    mEdit->setEnabled(!wc.busy() && !mEditing && sel && !sel->mOld);
    const std::string err = wc.takeError();
    if (!err.empty())
    {
        setStatus(err, true);
        alertOK(err);   // a modal the user must acknowledge: failures never vanish by themselves
    }
    else if (wc.busy() && !wc.cancelling() && wc.status() != mLastStatus)
    {
        mLastStatus = wc.status();
        setStatus(mLastStatus, false);
    }
    if (mEditing)
    {
        if (tool->takeChanged())
        {
            showBox();
            schedulePreview();
            if (tool->pen() == WolfToolCave::PEN_BOX) setStatus(getString("str_box_placed"), false);
        }
        // [WolfViewer 2026-10-15 v3] The spinners follow a box handle drag live (set() fires no commit).
        else if (tool->boxDragging())
        {
            showBox();
        }
        // [WolfViewer 2026-10-15 v3] The dig hint arrived or changed (fetchCoro cleared the preview made
        // with the other dig): work the rock out again with the new one.
        if (wc.digHint() != mShownHint)
        {
            mShownHint = wc.digHint();
            schedulePreview();
        }
        const std::string tm = tool->takeMessage();
        if (!tm.empty()) setStatus(tm, true);
        // Each step's button shows whether its pen is down.
        const bool on = tool->drawing() && LLToolMgr::getInstance()->getCurrentTool() == tool;
        const WolfCaves::Design& d = mCave.mDesign;
        mPenBox->setToggleState(on && tool->pen() == WolfToolCave::PEN_BOX);
        mPenTunnel->setToggleState(on && tool->pen() == WolfToolCave::PEN_TUNNEL);
        mPenCavern->setToggleState(on && tool->pen() == WolfToolCave::PEN_CAVERN);
        mPenDoor->setToggleState(on && tool->pen() == WolfToolCave::PEN_DOOR);
        // [WolfViewer 2026-10-15 v3] With a box, the box pen is the handles (CAVE_DESIGN_V3.md "Box resize").
        mPenBox->setLabel(getString(mPenBox->getToggleState() ? "str_stop" : (d.mHasBox ? "str_change_box" : "str_draw_box")));
        mPenTunnel->setLabel(getString(mPenTunnel->getToggleState() ? "str_stop" : "str_draw_tunnel"));
        mPenCavern->setLabel(getString(mPenCavern->getToggleState() ? "str_stop" : "str_add_caverns"));
        mPenDoor->setLabel(getString(mPenDoor->getToggleState() ? "str_stop" : "str_add_doors"));
        mCounts->setText(llformat("%d tunnel%s, %d cavern%s, %d doorway%s", (S32)d.mTunnels.size(), d.mTunnels.size() == 1 ? "" : "s",
                                  (S32)d.mCaverns.size(), d.mCaverns.size() == 1 ? "" : "s",
                                  (S32)d.mDoors.size(), d.mDoors.size() == 1 ? "" : "s"));
        mUndo->setEnabled(d.mHasBox || !d.mTunnels.empty() || !d.mCaverns.empty() || !d.mDoors.empty());
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (mPreviewAt > 0.0 && now >= mPreviewAt)
        {
            mPreviewAt = 0.0;
            // Only a design the generator can take: anything else is said in words below instead.
            LLViewerRegion* rgn = gAgent.getRegion();
            if (rgn && WolfCaves::designProblem(d, rgn->getWidth()).empty()) wc.requestPreview(mCave);
        }
        if (wc.previewSerial() != mShownPreview)
        {
            mShownPreview = wc.previewSerial();
            const WolfCaves::Preview& p = wc.preview();
            if (p.mReady)
            {
                // [WolfViewer 2026-10-15 v3] CAVE_DESIGN_V3.md "dig = false": nothing on the ground changes.
                std::string s = llformat("Land impact: %d prim%s - counted on the parcel under the cave's middle. ", p.mLi, p.mLi == 1 ? "" : "s")
                    + (p.mDig ? llformat("%d ground points lowered to the floor (%.2f m).", p.mTargets, d.mBox.mZ)
                              : llformat("No ground changed; floor %.2f m.", d.mBox.mZ));
                for (const std::string& w : p.mWarnings) s += "\n! " + w;
                mStats->setText(s);
            }
            else
            {
                mStats->setText(getString("str_stats_empty"));
            }
        }
        // [WolfViewer 2026-10-15 v3] Whether the ground will be dug (CAVE_DESIGN_V3.md "Rights: rezzers
        // build, owners dig"): what the last preview did, else the region's dig hint from caves.php.
        std::string dig;
        const WolfCaves::Preview& pv = wc.preview();
        if (pv.mReady) dig = getString(pv.mDig ? "str_dig_yes" : "str_dig_no");
        else if (wc.digHint() == "region") dig = getString("str_hint_region");
        else if (wc.digHint() == "parcels") dig = getString("str_hint_parcels");
        else if (wc.digHint() == "none") dig = getString("str_hint_none");
        if (dig != mShownDig)
        {
            mShownDig = dig;
            mDig->setText(dig);
        }
        // [WolfViewer 2026-10-15 v3] Generate finished a tunnel still being drawn (onBuild): once the
        // rock for the finished design is worked out, Generate goes on by itself; if it cannot be
        // made, the reason is said instead.
        if (mGenerateWhenReady && mPreviewAt == 0.0 && !wc.busy())
        {
            LLViewerRegion* rgn = gAgent.getRegion();
            const std::string problem = WolfCaves::designProblem(d, rgn ? rgn->getWidth() : 256.f);
            if (!problem.empty() || !wc.previewError().empty())
            {
                mGenerateWhenReady = false;
                setStatus(problem.empty() ? "The cave cannot be made like this: " + wc.previewError() : problem, true);
            }
            else if (pv.mReady)
            {
                mGenerateWhenReady = false;
                onBuild();
            }
        }
    }
    // [WolfViewer 2026-10-14 v2] Generate, and the reason it cannot be pressed, every frame: the
    // words right above the button are always the current ones.
    const std::string why = whyNotGenerate();
    mBuild->setEnabled(why.empty());
    if (why != mShownWhy)
    {
        mShownWhy = why;
        mWhy->setText(why.empty() ? getString("str_ready") : why);
        mWhy->setColor(why.empty() ? LLColor4(0.6f, 0.9f, 0.6f, 1.f) : LLColor4(1.f, 0.85f, 0.55f, 1.f));
    }
    LLFloater::draw();
}
