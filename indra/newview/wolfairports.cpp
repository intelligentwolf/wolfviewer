/**
 * @file wolfairports.cpp
 * @brief WolfViewer: the grid's airports (wolf-grid.com airportsapi.php) and finding their runways
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

#include "wolfairports.h"

#include <boost/json.hpp>
#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llsdjson.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llworld.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"   // names / descriptions: "runway"

namespace
{
    // Source: gridmanager/airportsapi.php (deployed to wolf-grid.com /var/www/html).
    const char* const API_URL = "https://wolf-grid.com/airportsapi.php";
    const F64 REFRESH_SECS = 600.0;     // airports are added by hand on the website, rarely
    const F64 RETRY_SECS = 60.0;        // after a failed read

    // What a runway looks like (findRunway). An airstrip is at least 60 m long and three times
    // as long as it is wide, at least 6 m wide, a slab no thicker than 4 m, lying flat (its thin
    // axis within 15 degrees of vertical).
    const F32 RUNWAY_MIN_LENGTH = 60.f;
    const F32 RUNWAY_MIN_WIDTH = 6.f;
    const F32 RUNWAY_MIN_ASPECT = 3.f;
    // Paul: "or something with runway in the description". A flat prim whose name or description
    // says "runway" is the runway even if it is shorter or squarer than the shape test allows
    // (down to these), and it wins over any unnamed slab. Names arrive by ObjectPropertiesFamily
    // (wolfobjectprops.h), asked for only for flat slabs at least this size near the airport.
    const F32 NAMED_MIN_LENGTH = 25.f;
    const F32 NAMED_MIN_ASPECT = 1.5f;
    const F32 RUNWAY_MAX_THICK = 4.f;
    const F32 RUNWAY_MIN_UPRIGHT = 0.966f;   // cos 15
    // How far from the airport's point a runway may lie: the point is where its owner stood for
    // "Copy SLurl", usually on or beside the runway; an address with no x / y is the region's
    // centre, so then anywhere in a big region.
    const F32 NEAR_EXACT_M = 600.f;
    const F32 NEAR_REGION_M = 2000.f;
    // Segments of one runway: a long runway is often several prims end to end.
    const F32 SAME_HEADING_COS = 0.9986f;   // cos 3

    // Source: wolfwavezones.cpp json_to_llsd (the same reply handling).
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    struct Slab
    {
        LLVector3 mCentre;   // agent
        LLVector3 mDir;      // the long axis, horizontal, unit
        F32 mLength = 0.f, mWidth = 0.f, mTop = 0.f;
        bool mNamed = false;   // "runway" in its name or description
    };
}

WolfAirports::WolfAirports()
{
}

WolfAirports::~WolfAirports()
{
}

F32 WolfAirports::Runway::length() const
{
    const F64 dx = mB.mdV[VX] - mA.mdV[VX], dy = mB.mdV[VY] - mA.mdV[VY];
    return (F32)sqrt(dx * dx + dy * dy);
}

void WolfAirports::refresh()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (mFetching || now < mNextFetch || !WolfGrid::isOnWolfTerritories())
    {
        return;
    }
    mFetching = true;
    mNextFetch = now + RETRY_SECS;   // moved on to REFRESH_SECS by a good reply
    LLCoros::instance().launch("WolfAirports fetch", []() { WolfAirports::instance().fetchCoro(); });
}

// Source: wolfwavezones.cpp fetchCoro for the adapter shape; llcorehttputil.h getRawAndSuspend.
void WolfAirports::fetchCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAirports", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");

    LLSD result = adapter->getRawAndSuspend(request, API_URL, options, headers);
    mFetching = false;
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
    {
        LL_WARNS("WolfAirports") << "airports list: " << status.toString() << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["ok"].asBoolean() || !reply["airports"].isArray())
    {
        LL_WARNS("WolfAirports") << "airports list: unexpected reply" << LL_ENDL;
        return;
    }
    std::vector<Airport> list;
    for (const LLSD& a : llsd::inArray(reply["airports"]))
    {
        if (list.size() >= 1000) break;   // a sane bound on what one reply may hold
        Airport ap;
        ap.mId = a["id"].asInteger();
        ap.mName = a["name"].asString();
        ap.mRegion = a["region"].asString();
        // json_to_llsd gives a whole number (z 25) as an Integer and 25.5 as a Real: both count.
        ap.mHasZ = a["z"].isInteger() || a["z"].isReal();
        ap.mGlobal.set(a["x"].asReal(), a["y"].asReal(), ap.mHasZ ? a["z"].asReal() : 0.0);
        ap.mExact = a["exact"].asBoolean();
        // Only sane positions: on the grid (global metres are never negative, and a region handle
        // holds 32 bits each way) and, for a height, within what OpenSim lets anything stand at.
        const F64 x = ap.mGlobal.mdV[VX], y = ap.mGlobal.mdV[VY], z = ap.mGlobal.mdV[VZ];
        if (ap.mRegion.empty() || ap.mRegion.size() > 128 || ap.mName.size() > 256
            || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
            || x < 0.0 || y < 0.0 || x > 4294967295.0 || y > 4294967295.0 || z < -100.0 || z > 10000.0)
        {
            continue;
        }
        list.push_back(ap);
    }
    mAirports.swap(list);
    mLoaded = true;
    mNextFetch = LLFrameTimer::getElapsedSeconds() + REFRESH_SECS;
    LL_INFOS("WolfAirports") << "airports list: " << mAirports.size() << " airports" << LL_ENDL;
}

const WolfAirports::Airport* WolfAirports::forRegion(const std::string& region) const
{
    for (const Airport& a : mAirports)
    {
        if (LLStringUtil::compareInsensitive(a.mRegion, region) == 0)
        {
            return &a;
        }
    }
    return nullptr;
}

// <WolfViewer 2026-10-10> see wolfairports.h search()
// static
std::string WolfAirports::fold(const std::string& utf8)
{
    // Latin-1 letters U+00C0..U+00FF to their plain letter ('*' and '/' stand for the multiply and divide signs).
    static const char LATIN1[] = "aaaaaaaceeeeiiiidnooooo*ouuuuytsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    const LLWString w = utf8str_to_wstring(utf8);
    std::string out;
    out.reserve(w.size());
    for (llwchar c : w)
    {
        if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if (c >= 32 && c < 127) out += (char)c;
        else if (c >= 0xC0 && c <= 0xFF) out += LATIN1[c - 0xC0];
        else if (c == 0x152 || c == 0x153) out += "oe";
        else if (c == 0x160 || c == 0x161) out += 's';
        else if (c == 0x17D || c == 0x17E) out += 'z';
        else if (c == 0x178) out += 'y';
        else out += ' ';
    }
    return out;
}

std::vector<const WolfAirports::Airport*> WolfAirports::search(const std::string& text, const LLVector3d& from) const
{
    std::string q = fold(text);
    LLStringUtil::trim(q);
    std::vector<std::pair<std::pair<S32, F64>, const Airport*>> ranked;
    if (q.empty())
    {
        return {};
    }
    for (const Airport& a : mAirports)
    {
        const std::string name = fold(a.mName), region = fold(a.mRegion);
        S32 rank = -1;
        const size_t at = name.find(q);
        if (at == 0) rank = 0;
        else if (at != std::string::npos && !isalnum((unsigned char)name[at - 1])) rank = 1;
        else if (at != std::string::npos) rank = 2;
        else if (region.find(q) != std::string::npos) rank = 3;
        if (rank < 0)
        {
            continue;
        }
        const F64 dx = a.mGlobal.mdV[VX] - from.mdV[VX], dy = a.mGlobal.mdV[VY] - from.mdV[VY];
        ranked.push_back({ { rank, dx * dx + dy * dy }, &a });
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    std::vector<const Airport*> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked)
    {
        out.push_back(r.second);
    }
    return out;
}

const WolfAirports::Airport* WolfAirports::byId(S32 id) const
{
    for (const Airport& a : mAirports)
    {
        if (a.mId == id)
        {
            return &a;
        }
    }
    return nullptr;
}
// </WolfViewer>

// static
// The runway, from the prims the viewer has round the airport's point: every flat, long, narrow
// slab (RUNWAY_*), the longest one near the point, then every other slab in line with it (same
// heading, its centre on this one's centreline, the same height) joined on, end to end.
WolfAirports::Runway WolfAirports::findRunway(const Airport& airport)
{
    Runway out;
    const LLVector3 point = gAgent.getPosAgentFromGlobal(airport.mGlobal);
    const F32 near_m = airport.mExact ? NEAR_EXACT_M : NEAR_REGION_M;
    std::vector<Slab> slabs;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAttachment() || o->isHUDAttachment()
            || o->flagPhantom() || o->mDrawable.isNull())
        {
            continue;
        }
        const LLVector3 c = o->getRenderPosition();
        if (dist_vec(LLVector2(c.mV[VX], c.mV[VY]), LLVector2(point.mV[VX], point.mV[VY])) > near_m + 1000.f)
        {
            continue;   // nothing that long
        }
        const LLVector3& s = o->getScale();
        const LLQuaternion q = o->getRenderRotation();
        const LLVector3 axes[3] = { LLVector3::x_axis * q, LLVector3::y_axis * q, LLVector3::z_axis * q };
        // the thin axis: the one nearest vertical
        S32 up = 0;
        for (S32 k = 1; k < 3; ++k)
        {
            if (fabsf(axes[k].mV[VZ]) > fabsf(axes[up].mV[VZ])) up = k;
        }
        if (fabsf(axes[up].mV[VZ]) < RUNWAY_MIN_UPRIGHT || s.mV[up] > RUNWAY_MAX_THICK)
        {
            continue;
        }
        const S32 a1 = (up + 1) % 3, a2 = (up + 2) % 3;
        const S32 lk = s.mV[a1] >= s.mV[a2] ? a1 : a2, wk = lk == a1 ? a2 : a1;
        const F32 len = s.mV[lk], wid = s.mV[wk];
        if (len < NAMED_MIN_LENGTH || wid < RUNWAY_MIN_WIDTH || len < wid * NAMED_MIN_ASPECT)
        {
            continue;
        }
        Slab sl;
        sl.mCentre = c;
        sl.mDir = LLVector3(axes[lk].mV[VX], axes[lk].mV[VY], 0.f);
        if (sl.mDir.normVec() < 0.5f)
        {
            continue;
        }
        sl.mLength = len;
        sl.mWidth = wid;
        sl.mTop = c.mV[VZ] + s.mV[up] * 0.5f;
        // near the point: the point's distance from the slab's centreline segment
        const LLVector3 d = point - c;
        const F32 along = llclamp(d.mV[VX] * sl.mDir.mV[VX] + d.mV[VY] * sl.mDir.mV[VY], -len * 0.5f, len * 0.5f);
        const F32 ex = d.mV[VX] - sl.mDir.mV[VX] * along, ey = d.mV[VY] - sl.mDir.mV[VY] * along;
        if (sqrtf(ex * ex + ey * ey) > near_m)
        {
            continue;
        }
        // on the ground (or at the airport's own height): not a long slab up in a skybox
        const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(c);
        const F32 ref = airport.mHasZ ? (F32)airport.mGlobal.mdV[VZ] : ground;
        if (fabsf(sl.mTop - ref) > 15.f && sl.mTop - ground > 15.f)
        {
            continue;
        }
        WolfObjectProps& props = WolfObjectProps::instance();
        props.want(o);   // asked once; the answer is there on a later scan
        if (const WolfObjectProps::Props* pr = props.get(o->getID()))
        {
            std::string name = pr->mName;
            LLStringUtil::toLower(name);
            sl.mNamed = name.find("runway") != std::string::npos
                        || pr->mDescriptionLower.find("runway") != std::string::npos;
        }
        if (!sl.mNamed && (len < RUNWAY_MIN_LENGTH || len < wid * RUNWAY_MIN_ASPECT))
        {
            continue;
        }
        slabs.push_back(sl);
    }
    if (slabs.empty())
    {
        return out;
    }
    size_t best = 0;
    for (size_t i = 1; i < slabs.size(); ++i)
    {
        // a named runway first, then the longest
        if (slabs[i].mNamed != slabs[best].mNamed ? slabs[i].mNamed : slabs[i].mLength > slabs[best].mLength) best = i;
    }
    const Slab& b = slabs[best];
    F32 lo = -b.mLength * 0.5f, hi = b.mLength * 0.5f;
    // join the segments in line with it, repeatedly, so a chain grows from either end
    std::vector<bool> used(slabs.size(), false);
    used[best] = true;
    for (bool grew = true; grew;)
    {
        grew = false;
        for (size_t i = 0; i < slabs.size(); ++i)
        {
            if (used[i]) continue;
            const Slab& s = slabs[i];
            if (fabsf(s.mDir * b.mDir) < SAME_HEADING_COS || fabsf(s.mTop - b.mTop) > 1.5f) continue;
            const LLVector3 d = s.mCentre - b.mCentre;
            const F32 along = d.mV[VX] * b.mDir.mV[VX] + d.mV[VY] * b.mDir.mV[VY];
            const F32 across = fabsf(d.mV[VX] * b.mDir.mV[VY] - d.mV[VY] * b.mDir.mV[VX]);
            if (across > b.mWidth * 0.5f) continue;
            const F32 s_lo = along - s.mLength * 0.5f, s_hi = along + s.mLength * 0.5f;
            if (s_hi < lo - 5.f || s_lo > hi + 5.f) continue;   // must touch the chain (5 m gap allowed)
            lo = llmin(lo, s_lo);
            hi = llmax(hi, s_hi);
            used[i] = true;
            grew = true;
        }
    }
    LLVector3 a = b.mCentre + b.mDir * lo, e = b.mCentre + b.mDir * hi;
    a.mV[VZ] = e.mV[VZ] = b.mTop;
    out.mA = gAgent.getPosGlobalFromAgent(a);
    out.mB = gAgent.getPosGlobalFromAgent(e);
    out.mWidth = b.mWidth;
    out.mTopZ = b.mTop;
    out.mValid = true;
    return out;
}
