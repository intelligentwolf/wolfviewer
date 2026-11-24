/**
 * @file wolfstarbase.cpp
 * @brief WolfViewer: Starfold's player starbases - what the region says about them, and the Starbase window's orders.
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

#include "wolfstarbase.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "llagent.h"
#include "llframetimer.h"
#include "llviewerregion.h"
#include "wolfgrid.h"
#include "wolfstarfold.h"

namespace
{
    // ── the wire's limits (Source: wolf-space/src/SpaceBases.cs) ─────────────────────────────────────────────────────
    const F64 BASE_ACTION_S = 1.0;           // SpaceBases.cs BASE_ACTION_MS: one base action a second per pilot (status, ghost
                                             // and own are not counted: BaseVerb's exemption)
    const size_t GHOSTS_MAX = 12;            // SpaceBases.cs Ghost: BasePlacement.Ghosts max = 12
    const size_t OWN_MAX = 12;               // README wire k=bown: at most 12
    const size_t PARTS_MAX = 64;             // a base's parts (design.html#bases: 20 home-made + the kit's, far below this)
    const size_t BASES_MAX = 24;             // BaseRules.CAP_MAX

    // ── GAME TUNING (the viewer's own) ────────────────────────────────────────────────────────────────────────────────
    const F64 ANSWER_FOR_S = 20.0;           // a k=msg this soon after an order is the window's answer
    const F64 FINISHED_SHOW_S = 8.0;         // the "ready" celebration

    F64 now() { return LLFrameTimer::getElapsedSeconds(); }

    std::string clean(const std::string& s, size_t most = 240)
    {
        std::string o;
        for (unsigned char ch : s) { if (ch >= 32) o.push_back((char)ch); }   // no control characters
        return o.size() > most ? o.substr(0, most) : o;
    }
    // an id the region gave us, echoed back on the wire: only [A-Za-z0-9_-], so it can never add a field (| = ;)
    std::string token(const std::string& s)
    {
        std::string o;
        for (char ch : s)
        {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') o.push_back(ch);
        }
        return o.size() > 64 ? o.substr(0, 64) : o;
    }
    std::vector<std::string> split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t a = 0;
        while (true)
        {
            const size_t b = s.find(sep, a);
            out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return out;
    }
    S32 toInt(const std::string& s, S32 dflt = 0)
    {
        char* end = nullptr;
        const long v = strtol(s.c_str(), &end, 10);
        return end != s.c_str() ? (S32)llclamp(v, -1000000000L, 1000000000L) : dflt;
    }
    F32 toF32(const std::string& s, F32 dflt = 0.f)
    {
        char* end = nullptr;
        const F32 f = strtof(s.c_str(), &end);
        return (end != s.c_str() && std::isfinite(f)) ? f : dflt;
    }
    bool three(const std::string& s, S32 out[3])
    {
        const std::vector<std::string> v = split(s, ',');
        if (v.size() != 3) return false;
        for (S32 i = 0; i < 3; ++i) out[i] = toInt(v[(size_t)i]);
        return true;
    }
    bool vec(const std::string& s, LLVector3& out)
    {
        const std::vector<std::string> v = split(s, ',');
        if (v.size() != 3) return false;
        const F32 x = toF32(v[0], NAN), y = toF32(v[1], NAN), z = toF32(v[2], NAN);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
        out.set(x, y, z);
        return true;
    }
    std::string cellStr(const S32 c[3]) { return llformat("%d,%d,%d", c[0], c[1], c[2]); }
    bool sameCell(const S32 a[3], const S32 b[3]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

    // Source: wolf-space BaseRules.cs CellTurn: Axes (+X -X +Y -Y +Z -Z), UpOrder (+Z -Z +Y -Y +X -X); for each front f the ups
    // at right angles to it are taken in UpOrder, spin 0..3, and the module's own Y goes to up x front (Build()).
    struct Turn { S32 x[3], y[3], z[3]; };
    const Turn* turns()
    {
        static Turn t[24];
        static bool made = false;
        if (made) return t;
        const S32 axes[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        const S32 ups[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
        for (S32 f = 0; f < 6; ++f)
        {
            const S32* fx = axes[f];
            S32 spin = 0;
            for (const auto& up : ups)
            {
                if (up[0] * fx[0] + up[1] * fx[1] + up[2] * fx[2] != 0) continue;
                Turn& r = t[4 * f + spin++];
                for (S32 i = 0; i < 3; ++i) { r.x[i] = fx[i]; r.z[i] = up[i]; }
                // y = up x front (CellTurn.Build: new(up.Y*fx.Z - up.Z*fx.Y, up.Z*fx.X - up.X*fx.Z, up.X*fx.Y - up.Y*fx.X))
                r.y[0] = up[1] * fx[2] - up[2] * fx[1];
                r.y[1] = up[2] * fx[0] - up[0] * fx[2];
                r.y[2] = up[0] * fx[1] - up[1] * fx[0];
            }
        }
        made = true;
        return t;
    }
}

WolfStarbase::WolfStarbase() {}
WolfStarbase::~WolfStarbase() {}

// ── the geometry ─────────────────────────────────────────────────────────────────────────────────────────────────────

void WolfStarbase::turn(S32 rot, const S32 in[3], S32 out[3])
{
    // Source: BaseRules.cs CellTurn.Apply: x * v.X + y * v.Y + z * v.Z
    const Turn& t = turns()[llclamp(rot, 0, 23)];
    for (S32 i = 0; i < 3; ++i) out[i] = t.x[i] * in[0] + t.y[i] * in[1] + t.z[i] * in[2];
}

void WolfStarbase::front(S32 rot, S32 out[3])
{
    const Turn& t = turns()[llclamp(rot, 0, 23)];
    for (S32 i = 0; i < 3; ++i) out[i] = t.x[i];
}

std::vector<std::vector<S32>> WolfStarbase::footprint(const S32 size[3], const S32 at[3], S32 rot)
{
    // Source: BasePlacement.cs Footprint: for i < SX, j < SY, l < SZ: at + CellTurn.Apply(rot, (i, j, l))
    std::vector<std::vector<S32>> cells;
    for (S32 i = 0; i < llclamp(size[0], 0, 3); ++i)
        for (S32 j = 0; j < llclamp(size[1], 0, 3); ++j)
            for (S32 l = 0; l < llclamp(size[2], 0, 3); ++l)
            {
                const S32 v[3] = { i, j, l };
                S32 o[3];
                turn(rot, v, o);
                cells.push_back({ at[0] + o[0], at[1] + o[1], at[2] + o[2] });
            }
    return cells;
}

LLVector3 WolfStarbase::toRegion(const LLVector3& local) const
{
    // Source: BasePlacement.cs PartPose: baseCentre + BaseRot(yaw).Rotate(local), BaseRot = AxisAngle(+Z, (yaw & 3) * pi / 2)
    // (SpaceMath.cs: q v q^-1, a right-handed turn), so a quarter turn takes (x, y) to (-y, x)
    F32 x = local.mV[VX], y = local.mV[VY];
    for (S32 i = 0; i < (mYaw & 3); ++i) { const F32 t = x; x = -y; y = t; }
    return mPos + LLVector3(x, y, local.mV[VZ]);
}

bool WolfStarbase::Part::building() const
{
    return mState == "building" && now() < mReadyAt;
}

// ── the wire ─────────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarbase::receive(const std::string& kind, const std::vector<std::pair<std::string, std::string>>& kv)
{
    if (kind != "bases" && kind != "base" && kind != "bpart" && kind != "bghost" && kind != "bown") return false;
    // one line a message, so a test (the rig reads the log) can see what the region said; never anyone's private words
    LL_INFOS("WolfStarbase") << "region says k=" << kind << " (" << kv.size() << " fields)" << LL_ENDL;
    LLViewerRegion* r = gAgent.getRegion();
    if (r && r->getRegionID() != mRegion)
    {
        reset();
        mRegion = r->getRegionID();
    }
    if (kind == "bases")
    {
        // Source: SpaceBases.cs SendBasesList: on=0/1, then b=<id>;x,y,z;yaw;owner;dock ("none": no docking ring ready)
        mBases.clear();
        for (const auto& p : kv)
        {
            if (p.first == "on") mBasesOn = p.second == "1";
            else if (p.first == "b" && mBases.size() < BASES_MAX)
            {
                const std::vector<std::string> f = split(p.second, ';');
                if (f.size() < 5) continue;
                Known k;
                k.mId = token(f[0]);
                if (k.mId.empty() || !vec(f[1], k.mPos)) continue;
                k.mYaw = toInt(f[2]) & 3;
                k.mOwner = clean(f[3], 40);
                k.mDock = token(f[4]);
                mBases.push_back(k);
            }
        }
        return true;
    }
    if (kind == "base") { parseBase(kv); return true; }
    if (kind == "bpart") { parsePart(kv); return true; }
    if (kind == "bghost")
    {
        // Source: SpaceBases.cs Ghost: kind=<what was asked>, up to 12 g=cx,cy,cz,rot, or t=<the plain reason>
        std::string what;
        std::vector<Ghost> g;
        std::string text;
        for (const auto& p : kv)
        {
            if (p.first == "kind") what = clean(p.second, 80);
            else if (p.first == "t") text = clean(p.second);
            else if (p.first == "g" && g.size() < GHOSTS_MAX)
            {
                const std::vector<std::string> f = split(p.second, ',');
                if (f.size() != 4) continue;
                Ghost gh;
                for (S32 i = 0; i < 3; ++i) gh.mCell[i] = toInt(f[(size_t)i]);
                gh.mRot = toInt(f[3]);
                if (gh.mRot < 0 || gh.mRot > 23) continue;
                g.push_back(gh);
            }
        }
        if (what != mGhostFor) return true;   // an answer to a choice since changed
        mGhosts = g;
        mGhostText = text;
        mGhostsIn = true;
        mSpot = 0;
        mTurn = 0;
        return true;
    }
    if (kind == "bown")
    {
        // README wire k=bown: here=0/1, o=<id>;<kind>;<prims>;<metres>;<ok>;<name>;<reason>;<sx,sy,sz>, or t=<message>
        mOwn.clear();
        mOwnText.clear();
        mOwnAsked = true;
        for (const auto& p : kv)
        {
            if (p.first == "here") mOwnHere = p.second == "1";
            else if (p.first == "t") mOwnText = clean(p.second);
            else if (p.first == "o" && mOwn.size() < OWN_MAX)
            {
                const std::vector<std::string> f = split(p.second, ';');
                if (f.size() < 7) continue;
                Own o;
                o.mId = token(f[0]);
                o.mKind = token(f[1]);
                if (o.mId.empty() || o.mKind.empty()) continue;
                o.mPrims = toInt(f[2]);
                o.mMetres = toF32(f[3], -1.f);
                o.mOk = f[4] == "1";
                o.mName = clean(f[5], 30);
                o.mReason = clean(f[6]);
                if (f.size() >= 8) three(f[7], o.mSize);
                mOwn.push_back(o);
            }
        }
        return true;
    }
    return false;
}

void WolfStarbase::parseBase(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceBases.cs SendBase (has=1: id= pos= yaw= state= prims=used/400 dock= raids= starmarks=) and SendBaseStatus
    // (has=0: open= count= cap= prims=used/allowance core=<price>), each with kit= for what can be bought here. The parts follow
    // as one k=bpart each, so a k=base starts the list afresh.
    // a "spot=" answer (SpaceBases.cs Spot: k=base spot=x,y,z alone) says nothing about the base: the region flies the ship
    // there and says so by k=msg
    bool spotOnly = true;
    for (const auto& p : kv) if (p.first != "k" && p.first != "spot") spotOnly = false;
    if (spotOnly) return;
    mKnown = true;
    mKits.clear();
    mParts.clear();
    mStarmarks = -1;
    for (const auto& p : kv)
    {
        const std::string& k = p.first;
        const std::string& v = p.second;
        if (k == "has") mHas = v == "1";
        else if (k == "id") mId = token(v);
        else if (k == "pos") vec(v, mPos);
        else if (k == "yaw") mYaw = toInt(v) & 3;
        else if (k == "state") mState = token(v);
        else if (k == "dock") mDock = token(v);
        else if (k == "raids") mRaids = token(v);
        else if (k == "starmarks") mStarmarks = (S64)strtoll(v.c_str(), nullptr, 10);
        else if (k == "open") mOpen = v == "1";
        else if (k == "count") mCount = toInt(v);
        else if (k == "cap") mCap = toInt(v);
        else if (k == "core") mCorePrice = toInt(v);
        else if (k == "prims")
        {
            const std::vector<std::string> f = split(v, '/');
            mRegionPrims = clean(v, 24);
            if (f.size() == 2) { mPrimsUsed = toInt(f[0]); mPrimsMax = toInt(f[1]); }
        }
        else if (k == "kit")
        {
            const std::vector<std::string> f = split(v, ';');
            if (f.size() < 7) continue;
            Kit kt;
            kt.mKind = token(f[0]);
            if (kt.mKind.empty()) continue;
            kt.mPrice = toInt(f[1]);
            kt.mMinutes = toInt(f[2]);
            kt.mPrims = toInt(f[3]);
            three(f[4], kt.mSize);
            kt.mLane = toF32(f[5]);
            kt.mTitle = clean(f[6], 40);
            mKits.push_back(kt);
        }
    }
    if (!mHas)
    {
        mId.clear();
        mState.clear();
        mDock.clear();
    }
}

void WolfStarbase::parsePart(const std::vector<std::pair<std::string, std::string>>& kv)
{
    // Source: SpaceBases.cs SendBase: p=<part>;<kind>;<state>;<cx,cy,cz>;<rot>;<ready_s>[;<own>;<sx,sy,sz>;<powered>]
    for (const auto& p : kv)
    {
        if (p.first != "p") continue;
        const std::vector<std::string> f = split(p.second, ';');
        if (f.size() < 6) continue;
        Part pt;
        pt.mId = token(f[0]);
        pt.mKind = token(f[1]);
        pt.mState = token(f[2]);
        if (pt.mId.empty() || pt.mKind.empty() || !three(f[3], pt.mCell)) continue;
        pt.mRot = llclamp(toInt(f[4]), 0, 23);
        pt.mReadyAt = now() + (F64)llmax(0, toInt(f[5]));
        if (f.size() >= 9)
        {
            pt.mOwn = f[6] == "1";
            three(f[7], pt.mSize);
            pt.mPowered = f[8] == "1";
        }
        else if (const Kit* k = kit(pt.mKind))
        {
            for (S32 i = 0; i < 3; ++i) pt.mSize[i] = k->mSize[i];
        }
        auto it = std::find_if(mParts.begin(), mParts.end(), [&](const Part& o) { return o.mId == pt.mId; });
        if (it != mParts.end()) *it = pt;
        else if (mParts.size() < PARTS_MAX) mParts.push_back(pt);
    }
}

void WolfStarbase::noteAnswer(const std::string& text)
{
    if (!mShown || text.empty() || now() - mAskedAt > ANSWER_FOR_S) return;
    mAnswer = clean(text);
}

void WolfStarbase::reset()
{
    mKnown = mHas = false;
    mId.clear(); mState.clear(); mDock.clear(); mRaids.clear();
    mPos.clearVec();
    mYaw = mPrimsUsed = mPrimsMax = 0;
    mStarmarks = -1;
    mOpen = false;
    mCount = mCap = mCorePrice = 0;
    mRegionPrims.clear();
    mKits.clear();
    mParts.clear();
    mBases.clear();
    mBasesOn = false;
    clearGhosts();
    mOwn.clear();
    mOwnHere = mOwnAsked = false;
    mOwnText.clear();
    mAnswer.clear();
    mFinished.clear();
    mReaskAt = -1.0;
    mRegion.setNull();
}

void WolfStarbase::idle()
{
    if (!mShown) return;
    if (!available()) { mShown = false; return; }
    // a part reaching its finish time while the window is open: the celebration, and the region asked once for the part's
    // working state (it compares the clock only "when someone looks": SpaceBases.cs Working)
    for (const Part& p : mParts)
    {
        if (p.mState != "building" || now() < p.mReadyAt) continue;
        const bool seen = std::any_of(mFinished.begin(), mFinished.end(), [&](const std::pair<std::string, F64>& f) { return f.first == p.mId; });
        if (seen) continue;
        mFinished.emplace_back(p.mId, now());
        mReaskAt = now() + 1.5;   // the region's clock may be a moment behind ours
    }
    if (mReaskAt > 0.0 && now() >= mReaskAt)
    {
        mReaskAt = -1.0;
        askStatus();
    }
}

std::string WolfStarbase::justFinished() const
{
    for (const auto& f : mFinished)
    {
        if (now() - f.second > FINISHED_SHOW_S) continue;
        const Part* p = part(f.first);
        if (!p) continue;
        const Kit* k = kit(p->mKind);
        return k ? k->mTitle : p->mKind;
    }
    return std::string();
}

const WolfStarbase::Kit* WolfStarbase::kit(const std::string& kind) const
{
    for (const Kit& k : mKits) if (k.mKind == kind) return &k;
    return nullptr;
}

const WolfStarbase::Part* WolfStarbase::part(const std::string& id) const
{
    for (const Part& p : mParts) if (p.mId == id) return &p;
    return nullptr;
}

// ── the window ───────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarbase::available() const
{
    return WolfGrid::isOnWolfTerritoriesConfirmed() && WolfStarfold::instance().spaceHere();
}

void WolfStarbase::show(bool on)
{
    if (on && !available()) return;
    mShown = on;
    mAnswer.clear();
    if (!on) { clearGhosts(); return; }
    askStatus();
    if (mTab == TAB_OWN) askOwn();
}

void WolfStarbase::setTab(ETab t)
{
    if (t == mTab) return;
    mTab = t;
    clearGhosts();
    mAnswer.clear();
    if (t == TAB_OWN) askOwn();
    else askStatus();
}

// ── choosing a spot ──────────────────────────────────────────────────────────────────────────────────────────────────

S32 WolfStarbase::spotCount() const
{
    S32 n = 0;
    for (size_t i = 0; i < mGhosts.size(); ++i)
    {
        bool first = true;
        for (size_t j = 0; j < i; ++j) if (sameCell(mGhosts[j].mCell, mGhosts[i].mCell)) { first = false; break; }
        if (first) ++n;
    }
    return n;
}

S32 WolfStarbase::turnCount() const
{
    const Ghost* c = chosen();
    if (!c) return 0;
    S32 n = 0;
    for (const Ghost& g : mGhosts) if (sameCell(g.mCell, c->mCell)) ++n;
    return n;
}

const WolfStarbase::Ghost* WolfStarbase::chosen() const
{
    // the spot-th distinct cell, then the turn-th ghost at it
    S32 s = -1;
    const Ghost* cell = nullptr;
    for (size_t i = 0; i < mGhosts.size() && !cell; ++i)
    {
        bool first = true;
        for (size_t j = 0; j < i; ++j) if (sameCell(mGhosts[j].mCell, mGhosts[i].mCell)) { first = false; break; }
        if (first && ++s == mSpot) cell = &mGhosts[i];
    }
    if (!cell) return nullptr;
    S32 t = 0;
    for (const Ghost& g : mGhosts)
    {
        if (!sameCell(g.mCell, cell->mCell)) continue;
        if (t++ == mTurn) return &g;
    }
    return cell;
}

void WolfStarbase::nextSpot(S32 step)
{
    const S32 n = spotCount();
    if (n <= 0) return;
    mSpot = ((mSpot + step) % n + n) % n;
    mTurn = 0;
}

void WolfStarbase::nextTurn()
{
    const S32 n = turnCount();
    if (n > 0) mTurn = (mTurn + 1) % n;
}

void WolfStarbase::choose(size_t i)
{
    if (i >= mGhosts.size()) return;
    // the spot is the index of the ghost's cell among the distinct cells
    S32 spot = -1;
    for (size_t k = 0; k < mGhosts.size(); ++k)
    {
        bool first = true;
        for (size_t j = 0; j < k; ++j) if (sameCell(mGhosts[j].mCell, mGhosts[k].mCell)) { first = false; break; }
        if (first) ++spot;
        if (first && sameCell(mGhosts[k].mCell, mGhosts[i].mCell)) break;
    }
    mSpot = llmax(0, spot);
    S32 t = 0;
    for (size_t k = 0; k < mGhosts.size(); ++k)
    {
        if (!sameCell(mGhosts[k].mCell, mGhosts[i].mCell)) continue;
        if (k == i) break;
        ++t;
    }
    mTurn = t;
}

void WolfStarbase::ghostSize(S32 out[3]) const
{
    out[0] = out[1] = out[2] = 1;
    if (mGhostFor.rfind("own:", 0) == 0)
    {
        const std::string id = mGhostFor.substr(4);
        for (const Own& o : mOwn) if (o.mId == id) { for (S32 i = 0; i < 3; ++i) out[i] = o.mSize[i]; return; }
    }
    else if (mGhostFor.rfind("part:", 0) == 0)
    {
        if (const Part* p = part(mGhostFor.substr(5))) { for (S32 i = 0; i < 3; ++i) out[i] = p->mSize[i]; }
    }
    else if (const Kit* k = kit(mGhostFor))
    {
        for (S32 i = 0; i < 3; ++i) out[i] = k->mSize[i];
    }
}

std::string WolfStarbase::ghostKind() const
{
    if (mGhostFor.rfind("own:", 0) == 0)
    {
        const std::string id = mGhostFor.substr(4);
        for (const Own& o : mOwn) if (o.mId == id) return o.mKind;
        return std::string();
    }
    if (mGhostFor.rfind("part:", 0) == 0)
    {
        const Part* p = part(mGhostFor.substr(5));
        return p ? p->mKind : std::string();
    }
    return mGhostFor;
}

void WolfStarbase::clearGhosts()
{
    mGhostFor.clear();
    mGhosts.clear();
    mGhostText.clear();
    mGhostsIn = false;
    mSpot = mTurn = 0;
}

// ── the orders ───────────────────────────────────────────────────────────────────────────────────────────────────────

bool WolfStarbase::order(const std::string& args, bool exempt)
{
    if (!available()) return false;
    if (!exempt && now() - mOrderedAt < BASE_ACTION_S)
    {
        // the region drops a second base action within the second (SpaceBases.cs BaseVerb): said here, never silently lost
        mAnswer = "One order a second, please - press it again.";
        return false;
    }
    if (!exempt) { mOrderedAt = now(); mAnswer.clear(); }
    mAskedAt = now();
    WolfStarfold::instance().intent(args);
    return true;
}

void WolfStarbase::askStatus() { order("base|status=1", true); }
void WolfStarbase::found() { order("base|found=1"); }
void WolfStarbase::findSpot() { order("base|spot=1"); }
void WolfStarbase::restore() { order("base|restore=1"); }
void WolfStarbase::home() { order("base|home=1"); }

void WolfStarbase::flyHome()
{
    // SpaceEngine.cs "fly": to=base flies the asker's own base (SpaceBases.cs FlyToBase); a pilot only
    if (!WolfStarfold::instance().piloting()) { mAnswer = "Launch your ship first, then Fly there."; return; }
    WolfStarfold::instance().flyTo("base");
}

void WolfStarbase::dockHere()
{
    if (!WolfStarfold::instance().piloting()) { mAnswer = "You dock from your ship. Launch, fly there, then press Dock."; return; }
    if (mId.size() < 8) return;
    // SpaceBases.cs TryDockBase: at=<base id, 8+ hex>
    order("dock|at=" + mId);
}

void WolfStarbase::setDock(const std::string& who)
{
    if (who != "all" && who != "friends" && who != "group" && who != "me") return;
    order("base|set=1|dock=" + who);
}

void WolfStarbase::pack() { order("base|pack=1"); }
void WolfStarbase::scrap(bool sure) { order(sure ? "base|scrap=1|sure=1" : "base|scrap=1"); }
void WolfStarbase::takeDown(const std::string& p) { if (!token(p).empty()) order("base|down=" + token(p)); }
void WolfStarbase::putBack(const std::string& p) { if (!token(p).empty()) order("base|putback=" + token(p)); }
void WolfStarbase::buyCore(const std::string& p) { if (!token(p).empty()) order("base|core=" + token(p)); }

void WolfStarbase::showGhosts(const std::string& what)
{
    // a kit kind, own:<object id> or part:<part id>: the region answers with the spots (SpaceBases.cs Ghost)
    std::string w;
    if (what.rfind("own:", 0) == 0) w = "own:" + token(what.substr(4));
    else if (what.rfind("part:", 0) == 0) w = "part:" + token(what.substr(5));
    else w = token(what);
    if (w.empty() || w == "own:" || w == "part:") return;
    clearGhosts();
    mGhostFor = w;
    mAnswer.clear();
    order("base|ghost=" + w, true);
}

void WolfStarbase::build()
{
    const Ghost* g = chosen();
    if (!g || mGhostFor.empty()) { mAnswer = "Choose a spot from the glowing outlines first."; return; }
    const std::string at = "|at=" + cellStr(g->mCell) + llformat("|rot=%d", g->mRot);
    std::string args;
    if (mGhostFor.rfind("own:", 0) == 0) args = "base|attach=" + mGhostFor.substr(4) + at;
    else if (mGhostFor.rfind("part:", 0) == 0) args = "base|place=" + mGhostFor.substr(5) + at;
    else args = "base|add=" + mGhostFor + at;
    if (order(args)) clearGhosts();
}

void WolfStarbase::placeAsIs(const std::string& p) { if (!token(p).empty()) order("base|place=" + token(p)); }
void WolfStarbase::attachAsIs(const std::string& object) { if (!token(object).empty()) order("base|attach=" + token(object)); }
void WolfStarbase::sendToBase(const std::string& object) { if (!token(object).empty()) order("base|send=" + token(object)); }

void WolfStarbase::askOwn()
{
    mOwnAsked = false;
    order("base|own=1", true);
}
