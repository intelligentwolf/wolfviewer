/**
 * @file wolfterrainlevels.cpp
 * @brief WolfViewer: sky levels - extra native terrain heightfields of a region (ticket 541).
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

#include "wolfterrainlevels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "llagent.h"
#include "llagentcamera.h"   // [SKY LAND TOOLS 2026-11-10] pickLand: the camera
#include "llbitpack.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldispatcher.h"
#include "llrender.h"
#include "llsdutil.h"
#include "llsurface.h"
#include "llsurfacepatch.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"   // [SKY LAND TOOLS 2026-11-10] pickLand: the pointer's ray
#include "llviewertexture.h"
#include "llvlcomposition.h"
#include "llworld.h"
#include "patch_code.h"
#include "patch_dct.h"
#include "pipeline.h"

U32 WolfTerrainLevels::sChangeCount = 0;
U32 WolfTerrainLevels::sNextSerial = 1;

namespace
{
    // Source: llvlmanager.cpp:39 LAND_LAYER_CODE 'L', :46 AURORA_LAND_LAYER_CODE 'M' (32-bit patch ids, regions > 256 m);
    // WolfSkyLevelsModule.cs Stream() writes the one the region's own LayerData would use.
    constexpr U8 LEVEL_LAND = 'L';
    constexpr U8 LEVEL_LAND_LARGE = 'M';
    // A whole level of the largest region a level may have (12 x 12 regions, WolfSkyLevelsModule paged guard; quota 4,096
    // patches) is a few MB on the wire; this bounds a hostile or broken answer before any decoding.
    constexpr U32 MAX_STREAM_BYTES = 64u << 20;
    // Patches a draw piece holds a side: 8 x 8 patches = 128 m, at most 1,600 underside and 8,704 wall vertices (U16 indices).
    constexpr S32 PIECE_PATCHES = 8;
    // Source: WolfSkyLevels.cs TryUnderside - the rock hangs 0.35 of the height above the base, plus 1.5 .. 5 m of rock.
    constexpr F32 UNDER_SCALE = 0.35f;
    constexpr F32 ROCK_MIN = 1.5f;
    constexpr F32 ROCK_RANGE = 3.5f;
    // The shades WolfStorm's wolf_sky_levels.js gives the rock (underside 0.6, wall foot 0.6, wall top 0.75).
    constexpr U8 SHADE_UNDER = 153;
    constexpr U8 SHADE_WALL_TOP = 191;

    class WolfSkyLevelsDispatchHandler : public LLDispatchHandler
    {
    public:
        // Source: WolfSkyLevelsModule.cs Tell - SendGenericMessage(MESSAGE_OUT, RegionID, [revision, ids]): the invoice is
        // the region whose sky land changed (the agent's or a neighbour it also reads).
        bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice, const sparam_t&) override
        {
            for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
            {
                if (regionp && regionp->getRegionID() == invoice)
                {
                    if (WolfTerrainLevels* sky = regionp->getWolfSkyLevels())
                    {
                        sky->refresh();
                    }
                    return true;
                }
            }
            return true;
        }
    };
    WolfSkyLevelsDispatchHandler sDispatchHandler;

    LLSD refusedAnswer(const std::string& message)
    {
        LLSD a = LLSD::emptyMap();
        a["success"] = false;
        a["message"] = message;
        return a;
    }
}

bool WolfTerrainLevels::Level::hasPatch(S32 px, S32 py) const
{
    if (!mSurface || px < 0 || py < 0)
    {
        return false;
    }
    const LLSurfacePatch* p = mSurface->findPatch(px, py);
    return p && p->getHasReceivedData();
}

WolfTerrainLevels::WolfTerrainLevels(LLViewerRegion* regionp)
    : mRegionp(regionp),
      mSerial(sNextSerial++)
{
    registerDispatcher();
}

WolfTerrainLevels::~WolfTerrainLevels()
{
    clear();
}

// static
void WolfTerrainLevels::registerDispatcher()
{
    // Source: wolflandsandbox.cpp WolfPanelLandSandbox() - registered once with gGenericDispatcher.
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE))
    {
        gGenericDispatcher.addHandler(MESSAGE, &sDispatchHandler);
    }
}

bool WolfTerrainLevels::hasCapability() const
{
    return mRegionp && !mRegionp->getCapability(CAP).empty();
}

void WolfTerrainLevels::onCapabilitiesReceived()
{
    if (!hasCapability())
    {
        return;   // no WolfSkyLevelsModule there (another grid, another sim, a paged region): nothing to read
    }
    refresh();
}

void WolfTerrainLevels::refresh()
{
    if (!mRegionp)
    {
        return;
    }
    const std::string url = mRegionp->getCapability(CAP);
    if (url.empty())
    {
        return;
    }
    if (mFetching)
    {
        mAgain = true;
        return;
    }
    mFetching = true;
    mAgain = false;
    const U64 handle = mRegionp->getHandle();
    const U32 serial = mSerial;
    LLCoros::instance().launch("WolfSkyLevelsFetch", [handle, serial, url]() { fetchCoro(handle, serial, url); });
}

// static
WolfTerrainLevels* WolfTerrainLevels::lookup(U64 handle, U32 serial)
{
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(handle);
    WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    return (sky && sky->mSerial == serial) ? sky : nullptr;
}

// static
void WolfTerrainLevels::fetchCoro(U64 handle, U32 serial, std::string url)
{
    // Source: wolffloatertunnels.cpp fetchCoro (adapter, getAndSuspend, status) and llfloaterworldmap.cpp wolf_cat_get
    // (getRawAndSuspend, HTTP_RESULTS_RAW) for the binary level stream.
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfSkyLevels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    while (true)
    {
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLSD list = adapter->getAndSuspend(request, url);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(list[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        WolfTerrainLevels* sky = lookup(handle, serial);
        if (!sky)
        {
            return;   // the region went away meanwhile; a new one reads its own
        }
        if (!status || !list.isMap() || !list["success"].asBoolean())
        {
            LL_WARNS("WolfSkyLevels") << sky->mRegionp->getName() << ": reading the sky land failed: "
                                      << (status ? list["message"].asString() : status.toString()) << LL_ENDL;
            sky->mFetching = false;
            if (sky->mAgain)
            {
                sky->refresh();
            }
            return;
        }
        list.erase(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS);
        sky->mInfo = list;
        ++sChangeCount;

        // the levels the region no longer has go; the ones whose revision moved (or that are new) are read again
        std::vector<S32> keep;
        struct Want { S32 id; std::string name; F32 base, top; S64 rev; };
        std::vector<Want> wanted;
        for (const LLSD& l : llsd::inArray(list["levels"]))
        {
            const S32 id = l["id"].asInteger();
            keep.push_back(id);
            // OSD.FromLong is an OSDBinary of 8 bytes, high byte first (OpenMetaverse.StructuredData OSDBinary(long)), so the
            // revision arrives as <binary>: ll_U64_from_sd reads exactly that (llsdutil.cpp, two network-order U32s)
            const LLSD& r = l["rev"];
            const S64 rev = r.isBinary() ? (S64)ll_U64_from_sd(r) : (S64)r.asReal();
            const Level* have = sky->findLevel(id);
            if (!have || have->mRev != rev || have->mName != l["name"].asString() || have->mBase != (F32)l["base"].asReal())
            {
                wanted.push_back({ id, l["name"].asString(), (F32)l["base"].asReal(), (F32)l["top"].asReal(), rev });
            }
        }
        for (S32 i = (S32)sky->mLevels.size() - 1; i >= 0; --i)
        {
            if (std::find(keep.begin(), keep.end(), sky->mLevels[i].mId) == keep.end())
            {
                sky->removeLevel(sky->mLevels[i].mId);
            }
        }
        for (const Want& want : wanted)
        {
            LLCore::HttpRequest::ptr_t req = std::make_shared<LLCore::HttpRequest>();
            LLSD raw = adapter->getRawAndSuspend(req, url + "?level=" + std::to_string(want.id));
            LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(raw[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
            sky = lookup(handle, serial);
            if (!sky)
            {
                return;
            }
            if (!st || !raw.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            {
                LL_WARNS("WolfSkyLevels") << sky->mRegionp->getName() << ": reading sky level " << want.id << " failed: " << st.toString() << LL_ENDL;
                continue;
            }
            const LLSD::Binary& bytes = raw[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            std::string why;
            if (bytes.empty())
            {
                // a level with no ground yet streams no patches at all
                Level* l = sky->findLevelMutable(want.id);
                if (!l)
                {
                    sky->mLevels.emplace_back();
                    l = &sky->mLevels.back();
                    l->mId = want.id;
                }
                l->mName = want.name;
                l->mBase = want.base;
                l->mTop = want.top;
                l->mRev = want.rev;
                l->mSurface.reset();
                l->mPatches.clear();
                l->mPieces.clear();
                ++sChangeCount;
            }
            else if (!sky->applyLevelStream(want.id, want.name, want.base, want.top, want.rev, bytes.data(), (U32)bytes.size(), why))
            {
                LL_WARNS("WolfSkyLevels") << sky->mRegionp->getName() << ": sky level " << want.id << " not shown: " << why << LL_ENDL;
            }
        }
        LL_INFOS("WolfSkyLevels") << sky->mRegionp->getName() << ": " << sky->mLevels.size() << " sky level(s)" << LL_ENDL;
        if (!sky->mAgain)
        {
            sky->mFetching = false;
            return;
        }
        sky->mAgain = false;   // something changed while this read: read once more
    }
}

WolfTerrainLevels::Level* WolfTerrainLevels::findLevelMutable(S32 id)
{
    for (Level& level : mLevels)
    {
        if (level.mId == id)
        {
            return &level;
        }
    }
    return nullptr;
}

const WolfTerrainLevels::Level* WolfTerrainLevels::findLevel(S32 id) const
{
    for (const Level& level : mLevels)
    {
        if (level.mId == id)
        {
            return &level;
        }
    }
    return nullptr;
}

bool WolfTerrainLevels::applyLevelStream(S32 id, const std::string& name, F32 base, F32 top, S64 rev, const U8* data, U32 size, std::string& why)
{
    if (!mRegionp)
    {
        why = "no region";
        return false;
    }
    // Group header: U16 stride + U8 patch size + U8 layer type (patch_code.cpp decode_patch_group_header).
    if (!data || size < 4 || size > MAX_STREAM_BYTES)
    {
        why = "level stream size " + std::to_string(size) + " is out of range";
        return false;
    }
    if (!llfinite(base) || !llfinite(top) || top < base)
    {
        why = "level height band is not valid";
        return false;
    }

    // LLBitPack reads through a non-const pointer; it never writes in read mode (llbitpack.h bitUnpack).
    std::vector<U8> copy(data, data + size);
    LLBitPack bitpack(copy.data(), size);
    LLGroupHeader goph;
    decode_patch_group_header(bitpack, &goph);
    if (goph.layer_type != LEVEL_LAND && goph.layer_type != LEVEL_LAND_LARGE)
    {
        why = "level stream layer type " + std::to_string((S32)goph.layer_type) + " is not land";
        return false;
    }

    // A new surface for every revision: the stream is the whole level, and patches the region removed must go with the old
    // surface (decompressDCTPatch only ever adds). The same grid the region's ground uses (LLViewerRegion constructor:
    // mLandp->create(terrain_grids_per_edge, terrain_grids_per_patch, mOriginGlobal, mWidth)); getGridsPerEdge() includes the
    // east/north buffer row create() adds (llsurface.cpp LLSurface::create: mGridsPerEdge = grids_per_edge + 1).
    const LLSurface& ground = mRegionp->getLand();
    auto fresh = std::make_unique<LLSurface>('l', nullptr);
    fresh->setWolfSkyLevel(true, base);     // before create(): initTextures() must not make a water plane
    fresh->setRegion(mRegionp);
    fresh->create(ground.getGridsPerEdge() - 1, ground.getGridsPerPatchEdge(), mRegionp->getOriginGlobal(), mRegionp->getWidth());
    // The ground's own decoder: patch size and patch ids are bounds-checked there (llsurface.cpp decompressDCTPatch).
    fresh->decompressDCTPatch(bitpack, &goph, goph.layer_type == LEVEL_LAND_LARGE);

    Level* level = findLevelMutable(id);
    if (!level)
    {
        mLevels.emplace_back();
        level = &mLevels.back();
        level->mId = id;
    }
    level->mName = name;
    level->mBase = base;
    level->mTop = top;
    level->mRev = rev;
    level->mSurface = std::move(fresh);
    level->mPatches.clear();
    for (const LLSurfacePatch* p : level->mSurface->getAllPatches())
    {
        if (p && p->getHasReceivedData())
        {
            level->mPatches.emplace_back(p->getPatchX(), p->getPatchY());
        }
    }
    level->mPieces.clear();
    level->mPiecesDirty = true;
    ++sChangeCount;
    return true;
}

void WolfTerrainLevels::removeLevel(S32 id)
{
    for (auto it = mLevels.begin(); it != mLevels.end(); ++it)
    {
        if (it->mId == id)
        {
            mLevels.erase(it);
            ++sChangeCount;
            return;
        }
    }
}

void WolfTerrainLevels::clear()
{
    mLevels.clear();
    ++sChangeCount;
}

void WolfTerrainLevels::idleUpdate(bool pbr, F32 max_update_time)
{
    for (Level& level : mLevels)
    {
        if (!level.mSurface)
        {
            continue;
        }
        if (pbr)
        {
            level.mSurface->idleUpdate</*PBR=*/true>(max_update_time);
        }
        else
        {
            level.mSurface->idleUpdate</*PBR=*/false>(max_update_time);
        }
        if (level.mPiecesDirty || level.mBuiltOrigin != gAgent.getAgentOriginGlobal())
        {
            buildPieces(level);
        }
    }
}

void WolfTerrainLevels::setOriginGlobal(const LLVector3d& origin_global)
{
    for (Level& level : mLevels)
    {
        if (level.mSurface)
        {
            level.mSurface->setOriginGlobal(origin_global);
        }
        level.mPiecesDirty = true;
    }
}

void WolfTerrainLevels::updatePatchVisibilities(LLAgent& agent)
{
    for (Level& level : mLevels)
    {
        if (level.mSurface)
        {
            level.mSurface->updatePatchVisibilities(agent);
        }
    }
}

F32 WolfTerrainLevels::minZ() const
{
    F32 z = std::numeric_limits<F32>::max();
    for (const Level& level : mLevels)
    {
        // the lowest the rock can hang (TryUnderside with the noise at 1)
        z = llmin(z, level.mBase - (UNDER_SCALE * llmax(0.f, level.mTop - level.mBase) + ROCK_MIN + ROCK_RANGE));
    }
    return z;
}

F32 WolfTerrainLevels::maxZ() const
{
    F32 z = -std::numeric_limits<F32>::max();
    for (const Level& level : mLevels)
    {
        z = llmax(z, level.mTop);
    }
    return z;
}

bool WolfTerrainLevels::sample(const Level& level, S32 x, S32 y, F32& out) const
{
    if (!level.mSurface || x < 0 || y < 0)
    {
        return false;
    }
    // the ground's grid: one point a metre on every region a level may have (llviewerregion.cpp: stock resolution)
    const S32 px = x / PATCH, py = y / PATCH;
    const LLSurfacePatch* p = level.mSurface->findPatch(px, py);
    if (!p || !p->getHasReceivedData() || !p->getDataZ())
    {
        return false;
    }
    out = p->getDataZ()[(x - px * PATCH) + (y - py * PATCH) * (S32)p->getDataStride()];
    return true;
}

// Source: WolfSkyLevels.cs WolfSkyLevel.VertexTop - the exact sample, else x clamped into the patch, else y clamped, else both.
bool WolfTerrainLevels::vertexTop(const Level& level, S32 X, S32 Y, S32 px, S32 py, F32& out) const
{
    if (sample(level, X, Y, out))
    {
        return true;
    }
    const S32 x0 = px * PATCH, y0 = py * PATCH;
    const S32 cx = llclamp(X, x0, x0 + PATCH - 1), cy = llclamp(Y, y0, y0 + PATCH - 1);
    return sample(level, cx, Y, out) || sample(level, X, cy, out) || sample(level, cx, cy, out);
}

// Source: WolfSkyLevels.cs WolfSkyLevel.Noise - the same 32-bit hash, unsigned arithmetic, 0..1.
static F32 wolf_sky_noise(S32 X, S32 Y)
{
    U32 h = ((U32)X * 73856093u) ^ ((U32)Y * 19349663u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (F32)(h & 0xFFFFFFu) / (F32)0xFFFFFFu;
}

// Source: WolfSkyLevels.cs WolfSkyLevel.TryUnderside lattice point: base - (0.35 * (top - base) + 1.5 + 3.5 * Noise(X, Y)).
F32 WolfTerrainLevels::undersideLattice(const Level& level, S32 X, S32 Y, S32 px, S32 py) const
{
    F32 top = level.mBase;
    vertexTop(level, X, Y, px, py, top);
    return level.mBase - (UNDER_SCALE * llmax(0.f, top - level.mBase) + ROCK_MIN + ROCK_RANGE * wolf_sky_noise(X, Y));
}

// Source: WolfSkyLevels.cs WolfSkyLevel.TrySurface - triangles (0,0)(1,0)(0,1) and (1,0)(1,1)(0,1) of each metre.
bool WolfTerrainLevels::surface(const Level& level, F32 x, F32 y, F32& out) const
{
    if (!llfinite(x) || !llfinite(y) || x < 0.f || y < 0.f)
    {
        return false;
    }
    const S32 X = llfloor(x), Y = llfloor(y);
    const S32 px = X / PATCH, py = Y / PATCH;
    if (!level.hasPatch(px, py))
    {
        return false;
    }
    F32 h00, h10, h01, h11;
    if (!vertexTop(level, X, Y, px, py, h00) || !vertexTop(level, X + 1, Y, px, py, h10)
        || !vertexTop(level, X, Y + 1, px, py, h01) || !vertexTop(level, X + 1, Y + 1, px, py, h11))
    {
        return false;
    }
    const F32 fx = x - (F32)X, fy = y - (F32)Y;
    out = (fx + fy <= 1.f) ? h00 + fx * (h10 - h00) + fy * (h01 - h00)
                           : h11 + (1.f - fx) * (h01 - h11) + (1.f - fy) * (h10 - h11);
    return true;
}

F32 WolfTerrainLevels::resolveSupportHeight(F32 x, F32 y, F32 z, F32 tolerance, bool* above_ground) const
{
    F32 best = mRegionp ? mRegionp->getLand().resolveHeightRegion(x, y) : 0.f;
    if (above_ground)
    {
        *above_ground = false;
    }
    for (const Level& level : mLevels)
    {
        F32 h;
        if (z + tolerance < level.mBase || !surface(level, x, y, h))
        {
            continue;
        }
        if (h <= z + tolerance && h > best)
        {
            best = h;
            if (above_ground)
            {
                *above_ground = true;
            }
        }
    }
    return best;
}

bool WolfTerrainLevels::brush(const Level& level, const std::string& tool, F32 cx, F32 cy, S32 size, F32 amount, F32 height,
                              S32& x0, S32& y0, S32& w, S32& h, std::vector<F32>& heights) const
{
    // Source: wolfstorm js/world/wolf_sky_levels.js brush() - the same rectangle, falloff and clamps.
    if (!mRegionp || !llfinite(cx) || !llfinite(cy))
    {
        return false;
    }
    const S32 R = (S32)mRegionp->getWidth();
    const S32 half = llmax(1, (S32)ll_round((F32)size / 2.f));
    x0 = llmax(0, (S32)ll_round(cx) - half);
    y0 = llmax(0, (S32)ll_round(cy) - half);
    const S32 x1 = llmin(R, (S32)ll_round(cx) + half), y1 = llmin(R, (S32)ll_round(cy) + half);
    w = x1 - x0;
    h = y1 - y0;
    if (w < 1 || h < 1)
    {
        return false;
    }
    const F32 cap = mInfo.isMap() && mInfo.has("max_height") ? (F32)mInfo["max_height"].asReal() : 4000.f;
    heights.assign((size_t)w * h, std::numeric_limits<F32>::quiet_NaN());
    F32 centre = 0.f;
    const bool has_centre = surface(level, cx, cy, centre);
    for (S32 j = 0; j < h; ++j)
    {
        for (S32 i = 0; i < w; ++i)
        {
            const S32 X = x0 + i, Y = y0 + j;
            F32 cur = 0.f;
            const bool has = sample(level, X, Y, cur);
            const F32 r = std::hypot((F32)X + 0.5f - cx, (F32)Y + 0.5f - cy) / (F32)half;
            const F32 fall = std::cos(r * F_PI / 2.f);
            F32 v;
            if (tool == "make")
            {
                v = height;
            }
            else if (!has || r > 1.f)
            {
                continue;
            }
            else if (tool == "raise")
            {
                v = cur + amount * fall;
            }
            else if (tool == "lower")
            {
                v = cur - amount * fall;
            }
            else if (tool == "flatten")
            {
                v = has_centre ? cur + (centre - cur) * llmin(1.f, 1.5f * fall) : cur;
            }
            else if (tool == "smooth")
            {
                F32 s = 0.f;
                S32 c = 0;
                for (S32 dy = -2; dy <= 2; ++dy)
                {
                    for (S32 dx = -2; dx <= 2; ++dx)
                    {
                        F32 q;
                        if (sample(level, X + dx, Y + dy, q))
                        {
                            s += q;
                            ++c;
                        }
                    }
                }
                v = cur + (s / (F32)c - cur) * fall;   // c >= 1: (X, Y) itself has ground
            }
            else
            {
                continue;
            }
            heights[(size_t)j * w + i] = llclamp(v, level.mBase, cap);
        }
    }
    return true;
}

// static
LLSD WolfTerrainLevels::applyBody(S32 level, S32 x, S32 y, S32 w, S32 h, const std::vector<F32>& heights, const std::string& label)
{
    // Source: WolfSkyLevelsModule.Edit.cs Handle "apply": heights = w * h float32 little-endian, row by row from y.
    LLSD body = LLSD::emptyMap();
    body["op"] = "apply";
    body["level"] = level;
    body["x"] = x;
    body["y"] = y;
    body["w"] = w;
    body["h"] = h;
    LLSD::Binary bytes(heights.size() * sizeof(F32));
    for (size_t i = 0; i < heights.size(); ++i)
    {
        U32 bits;
        std::memcpy(&bits, &heights[i], sizeof(bits));
        bytes[i * 4 + 0] = (U8)(bits & 0xFF);
        bytes[i * 4 + 1] = (U8)((bits >> 8) & 0xFF);
        bytes[i * 4 + 2] = (U8)((bits >> 16) & 0xFF);
        bytes[i * 4 + 3] = (U8)((bits >> 24) & 0xFF);
    }
    body["heights"] = bytes;
    body["label"] = label;
    return body;
}

void WolfTerrainLevels::post(const LLSD& body, std::function<void(const LLSD&)> done)
{
    const std::string url = mRegionp ? mRegionp->getCapability(CAP) : std::string();
    if (url.empty())
    {
        if (done)
        {
            done(refusedAnswer("This region does not have sky land."));
        }
        return;
    }
    const U64 handle = mRegionp->getHandle();
    const U32 serial = mSerial;
    LLCoros::instance().launch("WolfSkyLevelsPost", [handle, serial, url, body, done]() { postCoro(handle, serial, url, body, done); });
}

// static
void WolfTerrainLevels::postCoro(U64 handle, U32 serial, std::string url, LLSD body, std::function<void(const LLSD&)> done)
{
    // Source: wolffloatertunnels.cpp postCoro - postAndSuspend sends the body as LLSD XML (WolfSkyLevelsModule ReadBody).
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfSkyLevels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter->postAndSuspend(request, url, body);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD answer = status && result.isMap() && result.has("success") ? result
                : refusedAnswer("The region did not answer (" + status.toString() + "). Please try again in a moment.");
    if (WolfTerrainLevels* sky = lookup(handle, serial))
    {
        sky->refresh();
    }
    if (done)
    {
        done(answer);
    }
}

// [SKY LAND TOOLS 2026-11-10] wolfterrainlevels.h. Source: postCoro above (the same request); the caller is already a coroutine
// (wolfterraform.cpp Jobs::levelCoro). regionp may be gone when this returns: it is looked up again by handle, never used after.
// static
LLSD WolfTerrainLevels::postAndWait(LLViewerRegion* regionp, const LLSD& body)
{
    WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    const std::string url = sky ? regionp->getCapability(CAP) : std::string();
    if (url.empty())
    {
        return refusedAnswer("This region does not have sky land.");
    }
    const U64 handle = regionp->getHandle();
    const U32 serial = sky->mSerial;
    auto adapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfSkyLevels", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLSD result = adapter->postAndSuspend(request, url, body);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD answer = status && result.isMap() && result.has("success") ? result
                : refusedAnswer("The region did not answer (" + status.toString() + "). Please try again in a moment.");
    if (WolfTerrainLevels* again = lookup(handle, serial))
    {
        again->refresh();
    }
    return answer;
}

F32 WolfTerrainLevels::maxHeight() const
{
    // Source: WolfSkyLevelsModule.cs OnRequest ["max_height"] = Constants.MaxTerrainHeightmap (Constants.cs:166, 4000 m); the
    // same fallback as brush() above until the list has arrived.
    const F32 cap = mInfo.isMap() && mInfo.has("max_height") ? (F32)mInfo["max_height"].asReal() : 4000.f;
    return llmin(cap, 4000.f);
}

bool WolfTerrainLevels::rayTop(const Level& level, const LLVector3& o, const LLVector3& d, F32 max_t, F32& t_hit) const
{
    if (!level.mSurface || level.mPatches.empty() || !mRegionp || !(max_t > 0.f))
    {
        return false;
    }
    // only the part of the ray inside the level's height band can meet its top (half a metre either side for the rounding)
    const F32 lo = level.mBase - 0.5f, hi = level.mTop + 0.5f;
    F32 t0 = 0.f, t1 = max_t;
    if (fabsf(d.mV[VZ]) < 1e-6f)
    {
        if (o.mV[VZ] < lo || o.mV[VZ] > hi)
        {
            return false;
        }
    }
    else
    {
        F32 a = (lo - o.mV[VZ]) / d.mV[VZ], b = (hi - o.mV[VZ]) / d.mV[VZ];
        if (a > b)
        {
            std::swap(a, b);
        }
        t0 = llmax(t0, a);
        t1 = llmin(t1, b);
    }
    if (!(t1 > t0))
    {
        return false;
    }
    const F32 W = mRegionp->getWidth();
    // how far above the top the ray is at t (positive above; where the level has no ground, "above" while over its base, so
    // the ray meets the wall of a patch it enters from the side)
    auto over = [&](F32 t, bool& on) -> F32
    {
        const LLVector3 p = o + d * t;
        F32 h;
        on = p.mV[VX] >= 0.f && p.mV[VY] >= 0.f && p.mV[VX] < W && p.mV[VY] < W && surface(level, p.mV[VX], p.mV[VY], h);
        if (on)
        {
            return p.mV[VZ] - h;
        }
        return p.mV[VZ] > level.mBase ? 1.f : -1.f;
    };
    // half a metre steps, as mousePointOnLandGlobal's first pass walks the ground (llviewerwindow.cpp: 1 m, then 0.1 m)
    constexpr F32 STEP = 0.5f;
    bool on = false;
    F32 prev_t = t0;
    F32 prev = over(t0, on);
    for (F32 t = t0 + STEP; ; t += STEP)
    {
        const F32 tt = llmin(t, t1);
        const F32 cur = over(tt, on);
        if (prev > 0.f && cur <= 0.f && on)
        {
            // through the top between prev_t and tt: halve the gap down to a couple of centimetres
            F32 a = prev_t, b = tt;
            for (S32 k = 0; k < 6; ++k)
            {
                const F32 m = 0.5f * (a + b);
                bool on_m = false;
                if (over(m, on_m) > 0.f)
                {
                    a = m;
                }
                else
                {
                    b = m;
                }
            }
            t_hit = b;
            return true;
        }
        prev = cur;
        prev_t = tt;
        if (tt >= t1)
        {
            break;
        }
    }
    return false;
}

// static
bool WolfTerrainLevels::pickLand(S32 mx, S32 my, LLVector3d& hit_global, LLViewerRegion*& regionp, S32& level)
{
    level = 0;
    regionp = nullptr;
    // Source: llviewerwindow.cpp LLViewerWindow::mousePointOnLandGlobal - the camera position (gAgentCamera) and the pointer's
    // direction (mouseDirectionGlobal), out to the draw distance.
    LLVector3d ground;
    const bool have_ground = gViewerWindow->mousePointOnLandGlobal(mx, my, &ground);
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(mx, my);
    F32 best = have_ground ? (F32)(ground - cam).magVec() : gAgentCamera.mDrawDistance;
    bool found = false;
    for (LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
    {
        const WolfTerrainLevels* sky = r ? r->getWolfSkyLevels() : nullptr;
        if (!sky || !sky->hasLevels())
        {
            continue;
        }
        const LLVector3 o = r->getPosRegionFromGlobal(cam);
        for (const Level& l : sky->levels())
        {
            F32 t;
            if (sky->rayTop(l, o, dir, best, t) && t < best)
            {
                best = t;
                regionp = r;
                level = l.mId;
                found = true;
            }
        }
    }
    if (found)
    {
        hit_global = cam + LLVector3d(dir * best);
        return true;
    }
    if (have_ground)
    {
        hit_global = ground;
        regionp = LLWorld::getInstance()->getRegionFromPosGlobal(ground);
        return true;
    }
    return false;
}

// static
bool WolfTerrainLevels::pickOnLevel(S32 mx, S32 my, LLViewerRegion* regionp, S32 level_id, F32 plane_z, LLVector3d& hit_global)
{
    const WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
    const Level* level = sky ? sky->findLevel(level_id) : nullptr;
    if (!level)
    {
        return false;
    }
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    const LLVector3 dir = gViewerWindow->mouseDirectionGlobal(mx, my);
    F32 t;
    if (sky->rayTop(*level, regionp->getPosRegionFromGlobal(cam), dir, gAgentCamera.mDrawDistance, t))
    {
        hit_global = cam + LLVector3d(dir * t);
        return true;
    }
    // Source: llviewerwindow.h mousePointOnPlaneGlobal (as wolffloatertunnels.cpp WolfToolTunnel::planeHit)
    return gViewerWindow->mousePointOnPlaneGlobal(hit_global, mx, my, regionp->getPosGlobalFromRegion(LLVector3(0.f, 0.f, plane_z)),
                                                  LLVector3::z_axis);
}

void WolfTerrainLevels::buildPieces(Level& level)
{
    // Source: wolfstorm js/world/wolf_sky_levels.js buildArrays - the rock below each patch on the 4 m lattice and the walls
    // on the sides with no neighbouring patch (WolfSkyLevels.cs BuildBlockMesh), drawn up to the top's edge (the physics wall
    // starts 0.2 m lower; the drawn one closes the gap to the ground the terrain pool draws).
    level.mPieces.clear();
    level.mPiecesDirty = false;
    level.mBuiltOrigin = gAgent.getAgentOriginGlobal();
    if (!level.mSurface || level.mPatches.empty() || !mRegionp)
    {
        return;
    }
    std::map<std::pair<S32, S32>, std::vector<std::pair<S32, S32>>> groups;
    for (const auto& p : level.mPatches)
    {
        groups[{ p.first / PIECE_PATCHES, p.second / PIECE_PATCHES }].push_back(p);
    }
    static LLCachedControl<F32> terrain_scale(gSavedSettings, "RenderTerrainScale");   // lldrawpoolterrain.cpp sDetailScale
    const F32 uv_scale = 1.f / llmax(0.1f, (F32)terrain_scale);
    const LLVector3 origin = mRegionp->getOriginAgent();
    constexpr S32 UN = PATCH / UNDER;
    for (const auto& [key, patches] : groups)
    {
        U32 verts = 0, idx_count = 0;
        for (const auto& [px, py] : patches)
        {
            verts += (UN + 1) * (UN + 1);
            idx_count += UN * UN * 6;
            const S32 sides = (!level.hasPatch(px + 1, py)) + (!level.hasPatch(px, py + 1)) + (!level.hasPatch(px - 1, py)) + (!level.hasPatch(px, py - 1));
            verts += sides * (PATCH + 1) * 2;
            idx_count += sides * PATCH * 6;
        }
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                          | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                          | LLVertexBuffer::MAP_TEXTURE_INDEX);
        if (verts > 65535 || !vb->allocateBuffer(verts, idx_count))
        {
            LL_WARNS("WolfSkyLevels") << "could not allocate a sky land piece of " << verts << " vertices" << LL_ENDL;
            continue;
        }
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        LLStrider<U16> idx;
        if (!vb->getVertexStrider(pos) || !vb->getNormalStrider(norm) || !vb->getTexCoord0Strider(uv)
            || !vb->getColorStrider(col) || !vb->getIndexStrider(idx))
        {
            continue;
        }
        Piece piece;
        piece.mMin.setVec(F32_MAX, F32_MAX, F32_MAX);
        piece.mMax.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
        U32 v = 0;
        auto put = [&](F32 x, F32 y, F32 z, const LLVector3& n, U8 shade) {
            const LLVector3 a = origin + LLVector3(x, y, z);
            (*pos++).set(a.mV[VX], a.mV[VY], a.mV[VZ], 0.f);   // w = texture index 0
            *norm++ = n;
            *uv++ = LLVector2(x * uv_scale, (y + z) * uv_scale);
            *col++ = LLColor4U(shade, shade, shade, 255);      // diffuseAlphaMaskF.glsl: texture times this colour
            update_min_max(piece.mMin, piece.mMax, a);
            return v++;
        };
        U32 count = 0;
        for (const auto& [px, py] : patches)
        {
            const S32 x0 = px * PATCH, y0 = py * PATCH;
            const U32 under = v;
            for (S32 j = 0; j <= UN; ++j)
            {
                for (S32 i = 0; i <= UN; ++i)
                {
                    const S32 X = x0 + i * UNDER, Y = y0 + j * UNDER;
                    // the rock's own slope (central differences on the lattice), facing down and out: a flat (0, 0, -1)
                    // left the whole underside one unlit black in the first WolfViewer test (shots/t541d/B3_far_b.png)
                    const F32 ux = undersideLattice(level, X + UNDER, Y, px, py) - undersideLattice(level, X - UNDER, Y, px, py);
                    const F32 uy = undersideLattice(level, X, Y + UNDER, px, py) - undersideLattice(level, X, Y - UNDER, px, py);
                    LLVector3 n(ux, uy, -2.f * UNDER);
                    n.normVec();
                    put((F32)X, (F32)Y, undersideLattice(level, X, Y, px, py), n, SHADE_UNDER);
                }
            }
            for (S32 j = 0; j < UN; ++j)
            {
                for (S32 i = 0; i < UN; ++i)
                {
                    const U16 a00 = (U16)(under + j * (UN + 1) + i), a10 = (U16)(a00 + 1), a01 = (U16)(a00 + UN + 1), a11 = (U16)(a01 + 1);
                    // facing down (wolf_sky_levels.js: a00, a01, a10 / a10, a01, a11)
                    *idx++ = a00; *idx++ = a01; *idx++ = a10;
                    *idx++ = a10; *idx++ = a01; *idx++ = a11;
                    count += 6;
                }
            }
            // a wall from (sx0, sy0) along (dx, dy) for 16 m, facing out (wolf_sky_levels.js wall())
            auto wall = [&](S32 sx0, S32 sy0, S32 dx, S32 dy) {
                const U32 first = v;
                const LLVector3 n((F32)dy, (F32)-dx, 0.f);
                for (S32 k = 0; k <= PATCH; ++k)
                {
                    const S32 X = sx0 + dx * k, Y = sy0 + dy * k;
                    const S32 k0 = (k / UNDER) * UNDER, k1 = llmin(k0 + UNDER, PATCH);
                    const F32 u0 = undersideLattice(level, sx0 + dx * k0, sy0 + dy * k0, px, py);
                    const F32 u1 = undersideLattice(level, sx0 + dx * k1, sy0 + dy * k1, px, py);
                    const F32 f = k1 == k0 ? 0.f : (F32)(k - k0) / (F32)(k1 - k0);
                    F32 top = level.mBase;
                    vertexTop(level, X, Y, px, py, top);
                    put((F32)X, (F32)Y, u0 + (u1 - u0) * f, n, SHADE_UNDER);
                    put((F32)X, (F32)Y, top, n, SHADE_WALL_TOP);
                }
                for (S32 k = 0; k < PATCH; ++k)
                {
                    const U16 b0 = (U16)(first + 2 * k), t0 = (U16)(b0 + 1), b1 = (U16)(b0 + 2), t1 = (U16)(b0 + 3);
                    *idx++ = b0; *idx++ = b1; *idx++ = t1;
                    *idx++ = b0; *idx++ = t1; *idx++ = t0;
                    count += 6;
                }
            };
            if (!level.hasPatch(px + 1, py)) wall(x0 + PATCH, y0, 0, 1);
            if (!level.hasPatch(px, py + 1)) wall(x0 + PATCH, y0 + PATCH, -1, 0);
            if (!level.hasPatch(px - 1, py)) wall(x0, y0 + PATCH, 0, -1);
            if (!level.hasPatch(px, py - 1)) wall(x0, y0, 1, 0);
        }
        vb->unmapBuffer();
        piece.mVB = vb;
        piece.mIndices = count;
        level.mPieces[key] = piece;
    }
}

void WolfTerrainLevels::renderLevels()
{
    if (mLevels.empty() || !mRegionp)
    {
        return;
    }
    // the region's fourth detail texture (rock, the highest band) when it uses textures; white otherwise, shaded by colour
    LLViewerTexture* tex = LLViewerFetchedTexture::sWhiteImagep.get();
    if (LLVLComposition* comp = mRegionp->getComposition())
    {
        if (comp->getMaterialType() == LLTerrainMaterials::Type::TEXTURE && comp->mDetailTextures[3].notNull())
        {
            tex = comp->mDetailTextures[3].get();
        }
    }
    if (!tex)
    {
        return;
    }
    gGL.getTexUnit(0)->bindFast(tex);
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (Level& level : mLevels)
    {
        if (level.mBuiltOrigin != gAgent.getAgentOriginGlobal())
        {
            continue;   // built in an agent frame that has since moved: waits for idleUpdate's rebuild
        }
        for (auto& [key, piece] : level.mPieces)
        {
            if (piece.mVB.isNull() || piece.mIndices == 0)
            {
                continue;
            }
            LLVector4a centre, radius;
            centre.load3(((piece.mMin + piece.mMax) * 0.5f).mV);
            radius.load3(((piece.mMax - piece.mMin) * 0.5f).mV);
            if (!camera->AABBInFrustumNoFarClip(centre, radius))
            {
                continue;
            }
            piece.mVB->setBuffer();
            piece.mVB->drawRange(LLRender::TRIANGLES, 0, piece.mVB->getNumVerts() - 1, piece.mIndices, 0);
        }
    }
}

// static
void WolfTerrainLevels::renderDeferred()
{
    bool any = false;
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr;
        if (sky && sky->hasLevels())
        {
            any = true;
            break;
        }
    }
    if (!any)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: wolffarterrain.cpp WolfFarTerrain::renderDeferred - the same program, state and matrices.
    LLGLDisable blend(GL_BLEND);
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    for (LLViewerRegion* regionp : LLWorld::getInstance()->getRegionList())
    {
        if (WolfTerrainLevels* sky = regionp ? regionp->getWolfSkyLevels() : nullptr)
        {
            sky->renderLevels();
        }
    }
    shader.unbind();
}
