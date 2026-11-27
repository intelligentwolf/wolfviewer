/**
 * @file wolffarproxy.cpp
 * @brief WolfViewer: beyond a switch distance a region's static objects are drawn as one baked low-detail model
 * (a proxy) and the real objects standing there are not drawn at all. See wolffarproxy.h.
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

#include "wolffarproxy.h"

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldrawable.h"
#include "llfloatertools.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "llregionhandle.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llstaticstringtable.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llworld.h"
#include "pipeline.h"
#include "vk/wolfvkstate.h"
#include "wolfgrid.h"
#include "workqueue.h"

#include <sstream>
#include <unordered_set>
#include <zlib.h>

extern bool gCubeSnapshot;   // pipeline.cpp: a reflection probe or mirror is being rendered (as wolfsubpixelcull.cpp:34)

namespace
{
    // Source: firestorm/region-terrain-work/far-hlod-2026-11-14/DESIGN.md section 1 "File" (the baker's writer is
    // baker/ in the same folder) - every offset and size below is that table's.
    constexpr size_t HEADER_BYTES = 64;
    // Source: baker/hlodfile.py VERSION = 2 - version 1 with the keep-real boxes (header offset 60). A version 1 file
    // lists no place to keep real because it was baked before there were any, so it is not read: the region stays stock.
    constexpr U16 FORMAT_VERSION = 2;
    constexpr size_t PART_BYTES = 24;
    constexpr size_t VERTEX_BYTES = 16;
    constexpr size_t KEEP_BOX_BYTES = 8;           // u16 x0, y0, x1, y1 (0..65535 = 0..region size)
    // Source: baker/hlodfile.py KEEP_ID_BYTES = 16 and write(): after the boxes, the rest of the geometry block is the
    // keep-real ids - a root prim's UUID each, its 16 bytes in the order of its text form (LLUUID::mData, lluuid.h:188)
    constexpr size_t KEEP_ID_BYTES = 16;
    constexpr U32 MAX_KEEP_BOXES = 65535;          // the baker's cap
    constexpr U32 MAX_PART_VERTS = 65535;          // LLVertexBuffer indices are 16-bit (llvertexbuffer.cpp:1265)
    constexpr U32 MAX_REGION_M = 4096;             // DESIGN.md section 4: 16-bit positions, 6 cm at most
    constexpr U32 MAX_ATLAS = 4096;
    // what a well-formed model can never exceed: the baker's budget is 600,000 vertices a region (DESIGN.md section 3)
    constexpr size_t MAX_BODY_BYTES = 96u * 1024u * 1024u;
    constexpr size_t MAX_GEOM_BYTES = 256u * 1024u * 1024u;
    constexpr U32 CELL_M = 64;                     // DESIGN.md: the one cell size of version 1
    constexpr U32 MAX_PARTS_PER_CELL = 64;         // 64 x 65,535 vertices on one cell: seven times a whole region's budget
    constexpr F32 MAX_HEIGHT_M = 100000.f;         // no object is higher or lower than this (a sanity bound on z_min / z_max)

    // Source: php/hlod.php in the same folder (modelled on elev.php): rx, ry = the region's grid origin in 256 m
    // cells, f = info (the 64-byte header) or model (the whole file); 404 = no model for that region.
    const char* HLOD_URL = "https://wolf-grid.com/hlod.php?rx=%u&ry=%u&f=%s";
    constexpr U32 GRID_CELL_M = 256;

    // DESIGN.md section 2 (round 2): a cell goes over to its proxy at D + HYSTERESIS_M and comes back at D, and the
    // change itself is a dissolve of DISSOLVE_SECS on the clock - so nothing is mid-dissolve while the camera stands
    // still (the first version's distance band left a ring of cells half dissolved for as long as one stood there)
    constexpr F32 HYSTERESIS_M = 32.f;
    constexpr F32 DISSOLVE_SECS = 0.75f;
    constexpr F32 KEEP_CELL_M = 8.f;               // the keep-real mask's cells (what the model lists, and moving objects)
    constexpr F32 KEEP_LIVE_MARGIN_M = 1.f;        // round a listed object's box
    constexpr F32 DISTANCE_MIN_M = 256.f, DISTANCE_MAX_M = 4096.f;   // WolfFarProxyDistance is held to this
    constexpr F64 REFRESH_SECS = 1.0;              // the region list is looked at this often
    constexpr F64 INFO_SECS = 600.0;               // hlod.php answers with max-age 600; a model is rebuilt hourly at most
    constexpr F64 RETRY_MISSING_SECS = 600.0;      // 404: no model for this region (yet)
    constexpr F64 RETRY_ERROR_SECS = 60.0;
    constexpr S32 MAX_FETCHING = 2;
    constexpr U32 BUILD_VERTS_PER_FRAME = 120000;  // vertex buffers filled per frame while a model is being built
    constexpr F64 LOG_SECS = 10.0;

    // read while culling (every octree node of every cull), so plain statics rather than a singleton look-up
    bool sAnyHidden = false;
    U32 sHiddenNodes = 0;      // octree nodes failed since the last log line
}

std::vector<const WolfFarProxy::Region*> WolfFarProxy::sHiding;

namespace
{
    // The spatial bridges standing in a region's static volume octree, kept exactly as LLSpatialGroup keeps its
    // mBridgeList (put in where that is pushed, taken out where that is erased, llspatialpartition.cpp) and taken out
    // once more by the bridge's destructor. Main thread only.
    std::unordered_set<LLSpatialBridge*> sBridges;
    constexpr F32 BRIDGE_MARGIN_M = 16.f;          // round a bridge's box: what a fast vehicle moves between idle and the cull
}

// static
void WolfFarProxy::bridgeIn(LLSpatialBridge* bridge, LLSpatialGroup* group)
{
    LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
    // A volume's bridge is made in the region's static volume partition (LLSpatialBridge's constructor,
    // lldrawable.cpp:1329-1340) and goes over to the region's bridge partition at its first updateMove
    // (LLVolumeBridge sets PARTITION_BRIDGE, llvovolume.cpp:5436; LLSpatialBridge::updateMove moves it there,
    // lldrawable.cpp:1694-1707 and LLSpatialPartition::move, llspatialpartition.cpp:1065-1072). Both are followed:
    // the first for the keep-real mask, the second for the log's count of movers on proxy cells.
    if (bridge && part && (part->mPartitionType == LLViewerRegion::PARTITION_VOLUME
                           || part->mPartitionType == LLViewerRegion::PARTITION_BRIDGE))
    {
        sBridges.insert(bridge);
    }
}

// static
void WolfFarProxy::bridgeOut(LLSpatialBridge* bridge)
{
    sBridges.erase(bridge);
}

namespace
{

    LLStaticHashedString sFadeUniform("wolf_far_fade");

    U16 rd16(const U8* p) { return (U16)((U16)p[0] | ((U16)p[1] << 8)); }
    U32 rd32(const U8* p) { return (U32)p[0] | ((U32)p[1] << 8) | ((U32)p[2] << 16) | ((U32)p[3] << 24); }
    F32 rdf32(const U8* p)
    {
        const U32 u = rd32(p);
        F32 f;
        memcpy(&f, &u, sizeof(f));
        return f;
    }

    struct Header
    {
        U32 mBuildTime = 0, mRegionSize = 0, mCellM = 0, mSide = 0, mParts = 0, mVerts = 0, mIndices = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
        U32 mGeomZip = 0, mGeomRaw = 0, mAtlasBytes = 0, mAtlasW = 0, mAtlasH = 0, mCrc = 0, mKeepBoxes = 0;
        U32 mKeepIds = 0;      // not a header field: what is left of the geometry block after the boxes, in ids
    };

    bool power_of_two(U32 v) { return v != 0 && (v & (v - 1)) == 0; }

    /** The 64-byte header, checked for everything that can be checked without the rest of the file. */
    bool parseHeader(const U8* p, size_t size, Header& h, std::string& error)
    {
        if (size < HEADER_BYTES || p[0] != 'W' || p[1] != 'H' || p[2] != 'L' || p[3] != 'D')
        {
            error = "not a WHLD file";
            return false;
        }
        if (rd16(p + 4) != FORMAT_VERSION || rd16(p + 6) != HEADER_BYTES)
        {
            error = "a version this viewer does not read";
            return false;
        }
        h.mBuildTime = rd32(p + 8);
        h.mRegionSize = rd32(p + 12);
        h.mCellM = rd16(p + 16);
        h.mSide = rd16(p + 18);
        h.mParts = rd32(p + 20);
        h.mVerts = rd32(p + 24);
        h.mIndices = rd32(p + 28);
        h.mZMin = rdf32(p + 32);
        h.mZMax = rdf32(p + 36);
        h.mGeomZip = rd32(p + 40);
        h.mGeomRaw = rd32(p + 44);
        h.mAtlasBytes = rd32(p + 48);
        h.mAtlasW = rd16(p + 52);
        h.mAtlasH = rd16(p + 54);
        h.mCrc = rd32(p + 56);
        h.mKeepBoxes = rd32(p + 60);
        if (h.mBuildTime == 0 || h.mRegionSize == 0 || h.mRegionSize > MAX_REGION_M || h.mCellM != CELL_M || h.mSide == 0
            || h.mCellM * h.mSide != h.mRegionSize || h.mParts > h.mSide * h.mSide * MAX_PARTS_PER_CELL
            || !(h.mZMin <= h.mZMax) || !(h.mZMin >= -MAX_HEIGHT_M) || !(h.mZMax <= MAX_HEIGHT_M)
            || !power_of_two(h.mAtlasW) || !power_of_two(h.mAtlasH) || h.mAtlasW > MAX_ATLAS || h.mAtlasH > MAX_ATLAS
            || h.mGeomRaw > MAX_GEOM_BYTES || h.mKeepBoxes > MAX_KEEP_BOXES)
        {
            error = "a header that does not add up";
            return false;
        }
        // parts, vertices, indices and boxes, then whole ids to the end of the block
        const U64 counted = (U64)h.mParts * PART_BYTES + (U64)h.mVerts * VERTEX_BYTES + (U64)h.mIndices * 2
                            + (U64)h.mKeepBoxes * KEEP_BOX_BYTES;
        if (counted > (U64)h.mGeomRaw || ((U64)h.mGeomRaw - counted) % KEEP_ID_BYTES != 0)
        {
            error = "a header that does not add up";
            return false;
        }
        h.mKeepIds = (U32)(((U64)h.mGeomRaw - counted) / KEEP_ID_BYTES);
        return true;
    }
}

struct WolfFarProxy::Decoded
{
    struct P
    {
        U16 mCellX = 0, mCellY = 0;
        U32 mVerts = 0, mOpaque = 0, mCut = 0;
        F32 mZMin = 0.f, mZMax = 0.f;
        size_t mVertexAt = 0, mIndexAt = 0;      // offsets into mGeom
    };
    bool mOK = false;
    std::string mError;
    Header mHeader;
    std::vector<P> mParts;                       // sorted by cell
    std::vector<U8> mGeom;                       // the geometry block, unpacked
    size_t mKeepAt = 0;                          // offset of the keep-real boxes in mGeom (mHeader.mKeepBoxes of them)
    size_t mKeepIdAt = 0;                        // offset of the keep-real ids in mGeom (mHeader.mKeepIds of them)
    LLPointer<LLImageRaw> mAtlas;
};

namespace
{
    /** A whole model file -> its parts and its atlas picture. Runs on a worker thread: nothing of the viewer's state
     *  is touched. Every count and every index is checked here, so the main thread can fill vertex buffers blindly. */
    std::shared_ptr<WolfFarProxy::Decoded> decodeModel(std::shared_ptr<std::vector<U8>> body)
    {
        auto d = std::make_shared<WolfFarProxy::Decoded>();
        const U8* p = body->data();
        const size_t size = body->size();
        Header& h = d->mHeader;
        if (!parseHeader(p, size, h, d->mError))
        {
            return d;
        }
        if ((U64)size != (U64)HEADER_BYTES + h.mGeomZip + h.mAtlasBytes)
        {
            d->mError = "a file of the wrong length";
            return d;
        }
        if (crc32(crc32(0L, Z_NULL, 0), p + HEADER_BYTES, (uInt)(size - HEADER_BYTES)) != h.mCrc)
        {
            d->mError = "a checksum that does not match";
            return d;
        }
        d->mGeom.resize(h.mGeomRaw);
        uLongf unpacked = (uLongf)h.mGeomRaw;
        if (h.mGeomRaw == 0
            || uncompress(d->mGeom.data(), &unpacked, p + HEADER_BYTES, (uLong)h.mGeomZip) != Z_OK || unpacked != (uLongf)h.mGeomRaw)
        {
            d->mError = "geometry that does not unpack";
            return d;
        }
        const U8* g = d->mGeom.data();
        size_t at = (size_t)h.mParts * PART_BYTES;
        U64 verts = 0, indices = 0;
        d->mParts.resize(h.mParts);
        for (U32 i = 0; i < h.mParts; ++i)
        {
            const U8* t = g + (size_t)i * PART_BYTES;
            WolfFarProxy::Decoded::P& part = d->mParts[i];
            part.mCellX = rd16(t);
            part.mCellY = rd16(t + 2);
            part.mVerts = rd32(t + 4);
            part.mOpaque = rd32(t + 8);
            part.mCut = rd32(t + 12);
            part.mZMin = rdf32(t + 16);
            part.mZMax = rdf32(t + 20);
            const U64 count = (U64)part.mOpaque + part.mCut;
            if (part.mCellX >= h.mSide || part.mCellY >= h.mSide || part.mVerts == 0 || part.mVerts > MAX_PART_VERTS
                || count == 0 || part.mOpaque % 3 != 0 || part.mCut % 3 != 0
                || !(part.mZMin <= part.mZMax) || !llfinite(part.mZMin) || !llfinite(part.mZMax))
            {
                d->mError = "a part that does not add up";
                return d;
            }
            verts += part.mVerts;
            indices += count;
            if (verts > h.mVerts || indices > h.mIndices)
            {
                d->mError = "parts bigger than the file";
                return d;
            }
            part.mVertexAt = at;
            at += (size_t)part.mVerts * VERTEX_BYTES;
            part.mIndexAt = at;
            at += (size_t)count * 2;
            const U8* ix = g + part.mIndexAt;
            for (U64 k = 0; k < count; ++k)
            {
                if (rd16(ix + k * 2) >= part.mVerts)
                {
                    d->mError = "an index outside its part";
                    return d;
                }
            }
        }
        if (verts != h.mVerts || indices != h.mIndices
            || (U64)at + (U64)h.mKeepBoxes * KEEP_BOX_BYTES + (U64)h.mKeepIds * KEEP_ID_BYTES != (U64)h.mGeomRaw)
        {
            d->mError = "parts that do not fill the file";
            return d;
        }
        d->mKeepAt = at;
        d->mKeepIdAt = at + (size_t)h.mKeepBoxes * KEEP_BOX_BYTES;
        for (U32 i = 0; i < h.mKeepBoxes; ++i)
        {
            const U8* b = g + at + (size_t)i * KEEP_BOX_BYTES;
            if (rd16(b) > rd16(b + 4) || rd16(b + 2) > rd16(b + 6))
            {
                d->mError = "a keep-real box that is inside out";
                return d;
            }
        }
        std::stable_sort(d->mParts.begin(), d->mParts.end(), [](const WolfFarProxy::Decoded::P& a, const WolfFarProxy::Decoded::P& b)
        {
            return a.mCellY != b.mCellY ? a.mCellY < b.mCellY : a.mCellX < b.mCellX;
        });

        // the atlas: a JPEG 2000 codestream, decoded as any texture is (LLImageJ2C; lllocalbitmaps.cpp:367-371)
        if (h.mAtlasBytes == 0)
        {
            d->mError = "no atlas";
            return d;
        }
        LLPointer<LLImageJ2C> j2c = new LLImageJ2C;
        // the codestream into the image's own buffer, then its header read (LLImageFormatted::copyData does these
        // two steps, llimage.cpp, but is protected)
        U8* codestream = j2c->allocateData((S32)h.mAtlasBytes);
        if (codestream)
        {
            memcpy(codestream, p + HEADER_BYTES + h.mGeomZip, h.mAtlasBytes);
        }
        if (!codestream || !j2c->updateData())
        {
            d->mError = "an atlas that is not JPEG 2000";
            return d;
        }
        // the codestream's own size before anything is allocated for its pixels
        if ((U32)j2c->getWidth() != h.mAtlasW || (U32)j2c->getHeight() != h.mAtlasH)
        {
            d->mError = "an atlas of another size than the header gives";
            return d;
        }
        j2c->setDiscardLevel(0);
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!j2c->decode(raw, 0.f) || raw->getData() == nullptr || (U32)raw->getWidth() != h.mAtlasW
            || (U32)raw->getHeight() != h.mAtlasH || (raw->getComponents() != 3 && raw->getComponents() != 4))
        {
            d->mError = "an atlas that does not decode to the size the header gives";
            return d;
        }
        d->mAtlas = raw;
        d->mOK = true;
        return d;
    }
}

bool WolfFarProxy::enabled()
{
    static LLCachedControl<bool> on(gSavedSettings, "WolfFarProxy", true);
    return on && WolfGrid::showsWolfRegion();   // as the far terrain: what is drawn follows the region underfoot
}

WolfFarProxy::WolfFarProxy()
{
}

WolfFarProxy::~WolfFarProxy()
{
    sAnyHidden = false;
    sHiding.clear();
}

void WolfFarProxy::clear()
{
    // requests in flight find no region (or a new serial) when they come back and are dropped
    sHiding.clear();
    sAnyHidden = false;
    mRegions.clear();
    mActive = false;
}

void WolfFarProxy::releaseAll()
{
    clear();
}

void WolfFarProxy::idle()
{
    if (!enabled())
    {
        if (!mRegions.empty())
        {
            clear();
        }
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mNextRefresh)
    {
        mNextRefresh = now + REFRESH_SECS;
        refreshRegions();
    }
    bool building = false;
    for (auto& kv : mRegions)
    {
        step(kv.first, kv.second, now);
        // one model at a time is turned into vertex buffers, so a frame never fills more than BUILD_VERTS_PER_FRAME
        if (!building && kv.second.mState == Region::BUILDING)
        {
            building = true;
            if (buildSome(kv.second))
            {
                kv.second.mState = Region::IDLE;
                kv.second.mNextInfoAt = now + INFO_SECS;
            }
        }
    }
    // every frame (review, 17 Nov: once a second left a listed linkset that had just been sent to this viewer hidden
    // until the next look); a look-up per listed linkset and a few bytes per prim of it
    refreshKeepLive();
    // an object far away cannot be picked to edit while it is not drawn, so with the build tools open everything is
    // the real object (as WolfSubPixelCull stands down then, wolfsubpixelcull.cpp:75)
    // ... and without the proxy program (it failed to build, or the shaders are being reloaded) nothing may be hidden
    mActive = !(gFloaterTools && gFloaterTools->getVisible()) && gWolfFarProxyProgram.mProgramObject != 0;
    computeFades();
    logStats(now);
}

/** The connected regions, each once; a region that has gone takes its model with it. */
void WolfFarProxy::refreshRegions()
{
    LLWorld* world = LLWorld::getInstance();
    for (auto it = mRegions.begin(); it != mRegions.end(); )
    {
        if (world->getRegionFromHandle(it->first) == nullptr)
        {
            sHiding.clear();        // until computeFades, later this frame, lists them again
            sAnyHidden = false;
            it = mRegions.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (LLViewerRegion* regionp : world->getRegionList())
    {
        if (!regionp || mRegions.count(regionp->getHandle()))
        {
            continue;
        }
        // Source: wolffarterrain.cpp:399 - a region's handle is its origin in metres; hlod.php (as elev.php) names
        // a region by that origin in 256 m cells
        U32 ox = 0, oy = 0;
        from_region_handle(regionp->getHandle(), &ox, &oy);
        Region& r = mRegions[regionp->getHandle()];
        r.mGridX = ox / GRID_CELL_M;
        r.mGridY = oy / GRID_CELL_M;
    }
}

void WolfFarProxy::step(U64 handle, Region& r, F64 now)
{
    if ((r.mState == Region::NEW || r.mState == Region::IDLE) && now >= r.mNextInfoAt && mFetching < MAX_FETCHING
        // a read from a Wolf service waits for the region underfoot to have said it is one of ours (wolfgrid.h,
        // WOLF VISITOR SEES); no agent or session is sent (hlod.php is public, as elev.php)
        && WolfGrid::isOnWolfRegionConfirmed())
    {
        fetch(handle, r, false);
    }
}

void WolfFarProxy::fetch(U64 handle, Region& r, bool model)
{
    r.mState = model ? Region::MODEL : Region::INFO;
    r.mSerial = ++mNextSerial;
    ++mFetching;
    const std::string url = llformat(HLOD_URL, r.mGridX, r.mGridY, model ? "model" : "info");
    const U32 serial = r.mSerial;
    LLCoros::instance().launch("WolfFarProxy", [handle, serial, model, url]() { fetchCoro(handle, serial, model, url); });
}

// static
void WolfFarProxy::fetchCoro(U64 handle, U32 serial, bool model, std::string url)
{
    // Source: wolffarterrain.cpp:681-690 fetchCoro - the same adapter, verified options and raw read
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfFarProxy", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(model ? 120 : 15);
    LLSD result = adapter->getRawAndSuspend(request, url, options);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);

    if (!WolfFarProxy::instanceExists())
    {
        return;
    }
    WolfFarProxy& self = WolfFarProxy::instance();
    self.mFetching = llmax(0, self.mFetching - 1);
    auto it = self.mRegions.find(handle);
    if (it == self.mRegions.end() || it->second.mSerial != serial)
    {
        return;   // the region went, or everything was cleared, while this was on its way
    }
    Region& r = it->second;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLSD::Binary* body = result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW)
        ? &result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary() : nullptr;
    if (!status || !body)
    {
        // no model for this region (404), or the service having a moment: what is drawn now stays as it is
        r.mState = Region::IDLE;
        r.mNextInfoAt = now + (status.getType() == HTTP_NOT_FOUND ? RETRY_MISSING_SECS : RETRY_ERROR_SECS);
        if (status.getType() == HTTP_NOT_FOUND && !model)
        {
            sHiding.clear();        // until the next frame's computeFades
            sAnyHidden = false;
            r.mModel.reset();   // the grid no longer has a model for it: back to the real objects
        }
        return;
    }
    if (!model)
    {
        if (body->size() != HEADER_BYTES)   // hlod.php f=info is the 64-byte header and nothing else
        {
            r.mState = Region::IDLE;
            r.mNextInfoAt = now + RETRY_ERROR_SECS;
            return;
        }
        self.gotInfo(r, *body, now);
        if (r.mState == Region::MODEL)   // gotInfo wants the model: asked for at once
        {
            self.fetch(handle, r, true);
        }
        return;
    }
    if (body->size() > MAX_BODY_BYTES)
    {
        LL_WARNS("WolfFarProxy") << "model of region " << r.mGridX << "," << r.mGridY << " is too big: " << body->size() << " bytes" << LL_ENDL;
        r.mState = Region::IDLE;
        r.mNextInfoAt = now + INFO_SECS;
        return;
    }
    self.gotModel(handle, r, std::make_shared<std::vector<U8>>(body->begin(), body->end()));
}

/** The header of the model the grid has now: fetched when it is not the one drawn (or being made). */
void WolfFarProxy::gotInfo(Region& r, const std::vector<U8>& body, F64 now)
{
    Header h;
    std::string error;
    r.mState = Region::IDLE;
    r.mNextInfoAt = now + INFO_SECS;
    if (!parseHeader(body.data(), body.size(), h, error))
    {
        LL_WARNS("WolfFarProxy") << "region " << r.mGridX << "," << r.mGridY << ": " << error << LL_ENDL;
        return;
    }
    if ((r.mModel && r.mModel->mBuildTime == h.mBuildTime) || h.mBuildTime == r.mFailedBuildTime)
    {
        return;   // the one drawn is the one the grid has, or the grid's is one this viewer could not use
    }
    r.mState = Region::MODEL;   // fetchCoro sends the request
}

/** The whole file is in: checked and unpacked on a worker thread (zlib, JPEG 2000), then built here a little a frame. */
void WolfFarProxy::gotModel(U64 handle, Region& r, std::shared_ptr<std::vector<U8>> body)
{
    r.mState = Region::DECODING;
    const U32 serial = r.mSerial;
    // Source: wolfnaturalwater.cpp:329-349 - work on the General queue, its result handed back on the main loop
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        decoded(handle, serial, decodeModel(body));
        return;
    }
    main_queue->postTo(
        general_queue,
        [body]() // General queue
        {
            return decodeModel(body);
        },
        [handle, serial](std::shared_ptr<Decoded> d) // main thread
        {
            if (WolfFarProxy::instanceExists())
            {
                WolfFarProxy::instance().decoded(handle, serial, d);
            }
        });
}

void WolfFarProxy::decoded(U64 handle, U32 serial, std::shared_ptr<Decoded> d)
{
    auto it = mRegions.find(handle);
    if (it == mRegions.end() || it->second.mSerial != serial || it->second.mState != Region::DECODING)
    {
        return;
    }
    Region& r = it->second;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromHandle(handle);
    // a model made for another size of region (the region was resized since) would put its cells in the wrong places
    if (!d || !d->mOK || !regionp || d->mHeader.mRegionSize != (U32)ll_round(regionp->getWidth()))
    {
        LL_WARNS("WolfFarProxy") << "region " << r.mGridX << "," << r.mGridY << ": the model is not used: "
                                 << (d && !d->mOK ? d->mError : std::string("it is for a region of another size")) << LL_ENDL;
        // a file that parsed as far as its header is not fetched again until the grid has baked another
        r.mFailedBuildTime = d ? d->mHeader.mBuildTime : 0;
        r.mState = Region::IDLE;
        r.mNextInfoAt = now + INFO_SECS;
        return;
    }
    const Header& h = d->mHeader;
    r.mBuilding = std::make_unique<Model>();
    Model& m = *r.mBuilding;
    m.mBuildTime = h.mBuildTime;
    m.mRegionSize = h.mRegionSize;
    m.mCellM = h.mCellM;
    m.mSide = h.mSide;
    m.mParts.reserve(d->mParts.size());
    r.mDecoded = d;
    r.mBuiltParts = 0;
    r.mState = Region::BUILDING;
}

/** Vertex buffers for the next parts of the model being built; true when the model is whole and has been swapped in. */
bool WolfFarProxy::buildSome(Region& r)
{
    Model& m = *r.mBuilding;
    const Decoded& d = *r.mDecoded;
    const Header& h = d.mHeader;
    const F32 xy_scale = (F32)h.mRegionSize / 65535.f;
    const F32 z_scale = (h.mZMax - h.mZMin) / 65535.f;
    U32 budget = BUILD_VERTS_PER_FRAME;
    while (r.mBuiltParts < d.mParts.size())
    {
        const Decoded::P& src = d.mParts[r.mBuiltParts];
        if (src.mVerts > budget && budget != BUILD_VERTS_PER_FRAME)
        {
            return false;   // the rest next frame
        }
        budget -= llmin(budget, src.mVerts);
        const U32 indices = src.mOpaque + src.mCut;
        // Source: wolffarterrain.cpp:789-804 - the same buffer layout (the deferred diffuse vertex shader's inputs)
        LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                          | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                          | LLVertexBuffer::MAP_TEXTURE_INDEX);
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        LLStrider<U16> idx;
        if (!vb->allocateBuffer(src.mVerts, indices) || !vb->getVertexStrider(pos) || !vb->getNormalStrider(norm)
            || !vb->getTexCoord0Strider(uv) || !vb->getColorStrider(col) || !vb->getIndexStrider(idx))
        {
            // no video memory for it: this model is given up (the one drawn, if any, stays) and asked for again later
            LL_WARNS("WolfFarProxy") << "region " << r.mGridX << "," << r.mGridY << ": no vertex buffer of " << src.mVerts << " vertices" << LL_ENDL;
            r.mFailedBuildTime = m.mBuildTime;
            r.mBuilding.reset();
            r.mDecoded.reset();
            r.mState = Region::IDLE;
            r.mNextInfoAt = LLFrameTimer::getElapsedSeconds() + INFO_SECS;
            return false;
        }
        const U8* v = d.mGeom.data() + src.mVertexAt;
        for (U32 i = 0; i < src.mVerts; ++i, v += VERTEX_BYTES)
        {
            // region-local metres; the region's place in the agent frame is a matrix at draw time, so nothing here
            // is rebuilt when the agent frame moves
            pos[i].set((F32)rd16(v) * xy_scale, (F32)rd16(v + 2) * xy_scale, h.mZMin + (F32)rd16(v + 4) * z_scale);
            LLVector3 n((F32)(S8)v[6], (F32)(S8)v[7], (F32)(S8)v[8]);
            if (n.normalize() < 0.5f)
            {
                n.set(0.f, 0.f, 1.f);
            }
            norm[i] = n;
            // the emissive weight rides in the colour's red (wolfFarProxyF.glsl)
            col[i] = LLColor4U(v[9], 0, 0, 255);
            // v = 0 is the picture's first row (DESIGN.md); the decoder hands the picture over bottom row first
            // (llimagej2coj.cpp:690 "for (S32 y = height - 1; y >= 0; y--)"), which is where OpenGL puts t = 0
            uv[i].set((F32)rd16(v + 10) / 65535.f, 1.f - (F32)rd16(v + 12) / 65535.f);
        }
        const U8* ix = d.mGeom.data() + src.mIndexAt;
        for (U32 i = 0; i < indices; ++i)
        {
            idx[i] = rd16(ix + (size_t)i * 2);
        }
        vb->unmapBuffer();
        Part part;
        part.mCellX = src.mCellX;
        part.mCellY = src.mCellY;
        part.mVB = vb;
        part.mVerts = src.mVerts;
        part.mOpaque = src.mOpaque;
        part.mCut = src.mCut;
        part.mZMin = src.mZMin;
        part.mZMax = src.mZMax;
        m.mParts.push_back(part);
        ++r.mBuiltParts;
    }

    // every part has its buffer: the cells' part ranges and heights, the atlas, and the swap
    const U32 cells = m.mSide * m.mSide;
    m.mCellFirst.assign(cells + 1, 0);
    m.mCellZMin.assign(cells, 1.f);
    m.mCellZMax.assign(cells, 0.f);     // min > max: no part on the cell
    for (const Part& part : m.mParts)   // sorted by cell (decodeModel)
    {
        const U32 c = (U32)part.mCellY * m.mSide + part.mCellX;
        ++m.mCellFirst[c + 1];
        if (m.mCellZMin[c] > m.mCellZMax[c])
        {
            m.mCellZMin[c] = part.mZMin;
            m.mCellZMax[c] = part.mZMax;
        }
        else
        {
            m.mCellZMin[c] = llmin(m.mCellZMin[c], part.mZMin);
            m.mCellZMax[c] = llmax(m.mCellZMax[c], part.mZMax);
        }
    }
    for (U32 c = 0; c < cells; ++c)
    {
        m.mCellFirst[c + 1] += m.mCellFirst[c];
    }
    // The places of everything the baker left out as a mover or a kept-real scripted object (DESIGN.md: the keep-real
    // boxes), as fine cells: marked once here, they keep their real objects for as long as this model is drawn.
    m.mKeepSide = (U32)llceil((F32)m.mRegionSize / KEEP_CELL_M);
    m.mKeepBaked.assign((size_t)m.mKeepSide * m.mKeepSide, 0);
    m.mKeepBoxes = h.mKeepBoxes;
    {
        const F32 to_cells = (F32)m.mRegionSize / 65535.f / KEEP_CELL_M;
        const U8* b = d.mGeom.data() + d.mKeepAt;
        for (U32 i = 0; i < h.mKeepBoxes; ++i, b += KEEP_BOX_BYTES)
        {
            const U32 cx0 = llmin((U32)((F32)rd16(b) * to_cells), m.mKeepSide - 1);
            const U32 cy0 = llmin((U32)((F32)rd16(b + 2) * to_cells), m.mKeepSide - 1);
            const U32 cx1 = llmin((U32)((F32)rd16(b + 4) * to_cells), m.mKeepSide - 1);
            const U32 cy1 = llmin((U32)((F32)rd16(b + 6) * to_cells), m.mKeepSide - 1);
            for (U32 cy = cy0; cy <= cy1; ++cy)
            {
                memset(m.mKeepBaked.data() + (size_t)cy * m.mKeepSide + cx0, 1, cx1 - cx0 + 1);
            }
        }
    }
    m.mKeepIds.resize(h.mKeepIds);
    for (U32 i = 0; i < h.mKeepIds; ++i)
    {
        memcpy(m.mKeepIds[i].mData, d.mGeom.data() + d.mKeepIdAt + (size_t)i * KEEP_ID_BYTES, KEEP_ID_BYTES);
    }
    // Source: wolffarterrain.cpp:1076 - a texture of the viewer's own from a raw picture, with mipmaps
    m.mAtlas = LLViewerTextureManager::getLocalTexture(r.mDecoded->mAtlas.get(), true);
    const U32 total_verts = h.mVerts, total_indices = h.mIndices;
    r.mDecoded.reset();   // d and h are gone from here on
    if (m.mAtlas.isNull())
    {
        LL_WARNS("WolfFarProxy") << "region " << r.mGridX << "," << r.mGridY << ": no texture for the atlas" << LL_ENDL;
        r.mFailedBuildTime = m.mBuildTime;
        r.mBuilding.reset();
        return true;   // back to IDLE; this build is not asked for again
    }
    m.mAtlas->setAddressMode(LLTexUnit::TAM_CLAMP);
    LL_INFOS("WolfFarProxy") << "region " << r.mGridX << "," << r.mGridY << ": model of build " << m.mBuildTime << " in use, "
                             << m.mParts.size() << " parts, " << total_verts << " vertices, " << total_indices / 3 << " triangles, atlas "
                             << m.mAtlas->getWidth() << " x " << m.mAtlas->getHeight() << LL_ENDL;
    r.mModel = std::move(r.mBuilding);
    r.mFade.assign(cells, 0.f);
    r.mFar.assign(cells, 0);
    r.mHidden.assign(cells, 0);
    r.mAnyFade = false;
    r.mKeep.clear();
    r.mKeepSum.clear();      // computeFades builds the table afresh for this model (its size no longer matches)
    r.mKeepLive.clear();     // refreshKeepLive, later this frame, lists it for this model
    r.mKeepRects.clear();
    r.mKeepRectsBuilt.clear();
    return true;
}

/** Each cell's f for this frame, from the main camera, and the keep-real mask (DESIGN.md section 2, round 2). */
void WolfFarProxy::computeFades()
{
    static LLCachedControl<F32> distance(gSavedSettings, "WolfFarProxyDistance", 512.f);
    const F32 near_m = llclamp((F32)distance, DISTANCE_MIN_M, DISTANCE_MAX_M);
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLWorld* world = LLWorld::getInstance();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // the dissolve runs on the clock: a long frame (a hitch, a teleport) moves it one step at most
    const F32 step = llclamp((F32)(now - mLastFadeAt), 0.f, 0.1f) / DISSOLVE_SECS;
    mLastFadeAt = now;
    bool any_hidden = false;
    sHiding.clear();
    for (auto& kv : mRegions)
    {
        Region& r = kv.second;
        r.mAnyFade = false;
        r.mRegionp = nullptr;
        if (!r.mModel)
        {
            continue;
        }
        LLViewerRegion* regionp = mActive ? world->getRegionFromHandle(kv.first) : nullptr;
        if (!regionp)
        {
            // standing down (build tools, no shader): everything is the real object at once
            std::fill(r.mFade.begin(), r.mFade.end(), 0.f);
            std::fill(r.mFar.begin(), r.mFar.end(), (U8)0);
            std::fill(r.mHidden.begin(), r.mHidden.end(), (U8)0);
            continue;
        }
        const Model& m = *r.mModel;
        r.mRegionp = regionp;
        r.mOriginAgent = regionp->getOriginAgent();
        const LLVector3 local = cam - r.mOriginAgent;
        const F32 cell = (F32)m.mCellM;
        bool region_hides = false;
        for (U32 cy = 0; cy < m.mSide; ++cy)
        {
            const F32 y0 = (F32)cy * cell;
            const F32 dy = llmax(y0 - local.mV[VY], 0.f, local.mV[VY] - (y0 + cell));
            for (U32 cx = 0; cx < m.mSide; ++cx)
            {
                const U32 c = cy * m.mSide + cx;
                const F32 x0 = (F32)cx * cell;
                const F32 dx = llmax(x0 - local.mV[VX], 0.f, local.mV[VX] - (x0 + cell));
                // a cell with no proxy triangles has no height of its own: the distance on the ground decides
                const F32 dz = m.mCellZMin[c] > m.mCellZMax[c] ? 0.f
                    : llmax(m.mCellZMin[c] - local.mV[VZ], 0.f, local.mV[VZ] - m.mCellZMax[c]);
                const F32 d = sqrtf(dx * dx + dy * dy + dz * dz);
                // which side the cell is on changes only outside the HYSTERESIS_M ring, so a camera that stands
                // still (or sways) leaves every cell where it is and nothing is mid-dissolve
                if (d >= near_m + HYSTERESIS_M)
                {
                    r.mFar[c] = 1;
                }
                else if (d <= near_m)
                {
                    r.mFar[c] = 0;
                }
                F32 f = r.mFade[c];
                f = r.mFar[c] ? llmin(1.f, f + step) : llmax(0.f, f - step);
                r.mFade[c] = f;
                r.mHidden[c] = f >= 1.f ? 1 : 0;
                r.mAnyFade = r.mAnyFade || f > 0.f;
                region_hides = region_hides || f >= 1.f;
            }
        }
        if (region_hides)
        {
            any_hidden = true;
            sHiding.push_back(&r);
        }
    }

    // The keep-real mask of every region that hides something: the places the model itself lists (what the baker left
    // out as a mover or a kept-real scripted object, standing still or not) and, fresh every frame, the spatial
    // bridges - the objects that have moved - standing in its octree (wolffarproxy.h).
    // [W114 FRAME RATE 2026-11-26] Looked at every frame as before, but as the list of boxes (KeepRect) laid over
    // mKeepBaked; the mask and its summed-area table, (size / 8 m)^2 cells (262,144 on a 4,096 m region), are rebuilt
    // below only when that list differs from the one they were built from (the Aurora review, 26 Nov: both were rebuilt
    // for every hiding region every frame).
    mKeepCells = 0;
    mFarMovers = 0;
    mFarMoversSeen = 0;
    for (const Region* hiding : sHiding)
    {
        Region& r = const_cast<Region&>(*hiding);   // an entry of mRegions, listed just above
        r.mKeepRects = r.mKeepLive;
    }
    if (any_hidden)
    {
        for (LLSpatialBridge* bridge : sBridges)
        {
            LLSpatialGroup* group = bridge->getSpatialGroup();
            LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
            if (!part || (part->mPartitionType != LLViewerRegion::PARTITION_VOLUME
                          && part->mPartitionType != LLViewerRegion::PARTITION_BRIDGE))
            {
                continue;
            }
            for (const Region* hiding : sHiding)
            {
                if (hiding->mRegionp == part->mRegionp)
                {
                    Region& r = const_cast<Region&>(*hiding);   // an entry of mRegions
                    // only a bridge still in the static volume octree can be hidden with its node; one in the
                    // bridge partition is never looked at by the cull hooks (allHidden: PARTITION_VOLUME only)
                    if (part->mPartitionType == LLViewerRegion::PARTITION_VOLUME)
                    {
                        KeepRect rc;
                        if (keepRect(*r.mModel, r.mOriginAgent, bridge->getSpatialExtents(), BRIDGE_MARGIN_M, rc))
                        {
                            r.mKeepRects.push_back(rc);
                        }
                    }
                    // for the log: a mover whose own centre stands on a cell drawn as its proxy, and whether the
                    // cull still let it through (the proof that moving objects are never hidden)
                    const LLVector4a* ext = bridge->getSpatialExtents();
                    const F32 mx = 0.5f * (ext[0][VX] + ext[1][VX]) - r.mOriginAgent.mV[VX];
                    const F32 my = 0.5f * (ext[0][VY] + ext[1][VY]) - r.mOriginAgent.mV[VY];
                    const Model& m = *r.mModel;
                    if (mx >= 0.f && my >= 0.f && mx < (F32)m.mRegionSize && my < (F32)m.mRegionSize
                        && r.mHidden[(size_t)((U32)(my / (F32)m.mCellM)) * m.mSide + (U32)(mx / (F32)m.mCellM)])
                    {
                        const bool seen = bridge->isRecentlyVisible();
                        if (mFarMovers < FAR_MOVER_SAMPLES)
                        {
                            FarMover& sample = mFarSample[mFarMovers];
                            sample.mGridX = r.mGridX;
                            sample.mGridY = r.mGridY;
                            sample.mLocal.set(mx, my, 0.5f * (ext[0][VZ] + ext[1][VZ]) - r.mOriginAgent.mV[VZ]);
                            sample.mSeen = seen;
                        }
                        ++mFarMovers;
                        mFarMoversSeen += seen ? 1 : 0;
                    }
                    break;
                }
            }
        }
    }
    for (const Region* hiding : sHiding)
    {
        Region& r = const_cast<Region&>(*hiding);
        // summed-area table: mKeepSum[(y + 1) * (n + 1) + (x + 1)] = the marked fine cells in [0..x] x [0..y], so
        // "is anything marked under this octree node" is four reads whatever the node's size
        const U32 n = r.mModel->mKeepSide;
        if (r.mKeepSum.size() == (size_t)(n + 1) * (n + 1) && r.mKeepRects == r.mKeepRectsBuilt)
        {
            mKeepCells += r.mKeepSum.back();   // built from the same boxes over the same model: as it stands
            continue;
        }
        r.mKeep = r.mModel->mKeepBaked;
        for (const KeepRect& rc : r.mKeepRects)
        {
            markKeep(*r.mModel, r.mKeep, rc);
        }
        r.mKeepRectsBuilt = r.mKeepRects;
        r.mKeepSum.assign((size_t)(n + 1) * (n + 1), 0);
        for (U32 y = 0; y < n; ++y)
        {
            U32 row = 0;
            const U8* src = r.mKeep.data() + (size_t)y * n;
            const U32* above = r.mKeepSum.data() + (size_t)y * (n + 1);
            U32* out = r.mKeepSum.data() + (size_t)(y + 1) * (n + 1);
            for (U32 x = 0; x < n; ++x)
            {
                row += src[x];
                out[x + 1] = above[x + 1] + row;
            }
        }
        mKeepCells += r.mKeepSum.back();
    }
    sAnyHidden = any_hidden;
}

/** The linksets the models list by id (DESIGN.md: the keep-real ids), where they stand NOW: a train the baker saw at
 *  platform 3 and that has since come to rest at platform 7 is kept real there, whether or not this viewer saw it
 *  move. Each listed root prim this viewer has an object for, and its child prims, mark the fine cells under their
 *  boxes on the region they stand on - when that region has a model. A drawable that is active is skipped: it and
 *  its whole linkset are drawn through a spatial bridge (LLDrawable::makeActive, lldrawable.cpp:507-560: parent and
 *  children all go active together), which computeFades marks every frame. */
void WolfFarProxy::refreshKeepLive()
{
    mKeepIdsFound = 0;
    for (auto& kv : mRegions)
    {
        kv.second.mKeepLive.clear();   // [W114 2026-11-26] a list of boxes now; markKeepLive adds a region's only if it has a model
    }
    for (auto& kv : mRegions)
    {
        if (!kv.second.mModel)
        {
            continue;
        }
        for (const LLUUID& id : kv.second.mModel->mKeepIds)
        {
            const LLViewerObject* root = gObjectList.findObject(id);
            if (!root || root->isDead())
            {
                continue;   // not (yet) sent to this viewer: nothing of it is drawn, so nothing of it can be hidden
            }
            ++mKeepIdsFound;
            markKeepLive(root);
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (child.notNull() && !child->isDead())
                {
                    markKeepLive(child.get());
                }
            }
        }
    }
}

void WolfFarProxy::markKeepLive(const LLViewerObject* object)
{
    const LLDrawable* drawable = object->mDrawable.get();
    const LLViewerRegion* regionp = object->getRegion();
    if (!drawable || drawable->isDead() || drawable->isActive() || !regionp)
    {
        return;
    }
    auto it = mRegions.find(regionp->getHandle());
    if (it == mRegions.end() || !it->second.mModel)
    {
        return;   // a region with no model hides nothing
    }
    // a static drawable's box is in the agent frame, as an octree node's is (allHidden reads those the same way)
    KeepRect rc;
    if (keepRect(*it->second.mModel, regionp->getOriginAgent(), drawable->getSpatialExtents(), KEEP_LIVE_MARGIN_M, rc))
    {
        it->second.mKeepLive.push_back(rc);
    }
}

/** The fine cells under a box (agent frame, with a margin) in a keep-real grid of the region; false when it marks none. */
// static
bool WolfFarProxy::keepRect(const Model& m, const LLVector3& origin_agent, const LLVector4a* ext, F32 margin, KeepRect& out)
{
    const F32 size = (F32)m.mRegionSize;
    const F32 x0 = ext[0][VX] - origin_agent.mV[VX] - margin, x1 = ext[1][VX] - origin_agent.mV[VX] + margin;
    const F32 y0 = ext[0][VY] - origin_agent.mV[VY] - margin, y1 = ext[1][VY] - origin_agent.mV[VY] + margin;
    if (!(x0 <= x1) || !(y0 <= y1) || x1 < 0.f || y1 < 0.f || x0 >= size || y0 >= size)
    {
        return false;   // an unset box, or wholly off the region
    }
    const U32 n = m.mKeepSide;
    out.x0 = (U32)(llmax(x0, 0.f) / KEEP_CELL_M);
    out.x1 = llmin((U32)(llmin(x1, size) / KEEP_CELL_M), n - 1);
    out.y0 = (U32)(llmax(y0, 0.f) / KEEP_CELL_M);
    out.y1 = llmin((U32)(llmin(y1, size) / KEEP_CELL_M), n - 1);
    return true;
}

/** Marks a keepRect's fine cells in a keep-real grid of the region. */
// static
void WolfFarProxy::markKeep(const Model& m, std::vector<U8>& grid, const KeepRect& rc)
{
    const U32 n = m.mKeepSide;
    for (U32 cy = rc.y0; cy <= rc.y1; ++cy)
    {
        memset(grid.data() + (size_t)cy * n + rc.x0, 1, rc.x1 - rc.x0 + 1);
    }
}

// static
bool WolfFarProxy::allHidden(const LLSpatialGroup* group, bool subtree)
{
    // gCubeSnapshot: a reflection probe's pictures (and the shadow maps made for them) keep the real objects - the
    // proxies are not drawn into them
    // ... nor into a reflection render (LLPipeline::sReflectionRender)
    if (!sAnyHidden || !group || gCubeSnapshot || LLPipeline::sReflectionRender)
    {
        return false;
    }
    // Firestorm 7.2.5 (6f806fb9cc9) removed the const LLOcclusionCullingGroup::getSpatialPartition(); LLSpatialGroup's own
    // accessor returns the same mSpatialPartition (llspatialpartition.h:326), read-only here
    const LLViewerOctreePartition* part = const_cast<LLSpatialGroup*>(group)->getSpatialPartition();
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME || !part->mRegionp)
    {
        return false;   // only a region's static volumes are in a model
    }
    // (the node's box is current: LLSpatialPartition::cull rebounds the tree from its root before the cull walks it,
    // llspatialpartition.cpp:1533 group->rebound())
    const Region* r = nullptr;
    for (const Region* candidate : sHiding)
    {
        if (candidate->mRegionp == part->mRegionp)
        {
            r = candidate;
            break;
        }
    }
    if (!r || !r->mModel)
    {
        return false;
    }
    const Model& m = *r->mModel;
    // the node's box in the region's own frame (the partition's region is alive while it is being culled)
    const LLVector3 origin = part->mRegionp->getOriginAgent();
    const LLVector4a* ext = subtree ? group->getExtents() : group->getObjectExtents();
    const F32 x0 = ext[0][VX] - origin.mV[VX], x1 = ext[1][VX] - origin.mV[VX];
    const F32 y0 = ext[0][VY] - origin.mV[VY], y1 = ext[1][VY] - origin.mV[VY];
    const F32 size = (F32)m.mRegionSize;
    // an empty or unset box, or one that reaches off the region (nothing off the region is in its model): stock
    if (!(x0 <= x1) || !(y0 <= y1) || !(x0 >= 0.f) || !(y0 >= 0.f) || !(x1 < size) || !(y1 < size))
    {
        return false;
    }
    const F32 cell = (F32)m.mCellM;
    const U32 cx0 = (U32)(x0 / cell), cx1 = llmin((U32)(x1 / cell), m.mSide - 1);
    const U32 cy0 = (U32)(y0 / cell), cy1 = llmin((U32)(y1 / cell), m.mSide - 1);
    for (U32 cy = cy0; cy <= cy1; ++cy)
    {
        const U8* row = r->mHidden.data() + (size_t)cy * m.mSide;
        for (U32 cx = cx0; cx <= cx1; ++cx)
        {
            if (!row[cx])
            {
                return false;
            }
        }
    }
    // nothing scripted and nothing moving may stand under the box: those are in no model (the summed-area table of
    // computeFades; the box is widened to whole fine cells)
    const U32 n = m.mKeepSide;
    if (r->mKeepSum.size() != (size_t)(n + 1) * (n + 1))
    {
        return false;
    }
    const U32 kx0 = (U32)(x0 / KEEP_CELL_M), kx1 = llmin((U32)(x1 / KEEP_CELL_M), n - 1) + 1;
    const U32 ky0 = (U32)(y0 / KEEP_CELL_M), ky1 = llmin((U32)(y1 / KEEP_CELL_M), n - 1) + 1;
    const U32* sum = r->mKeepSum.data();
    const U32 w = n + 1;
    if (sum[(size_t)ky1 * w + kx1] - sum[(size_t)ky0 * w + kx1] - sum[(size_t)ky1 * w + kx0] + sum[(size_t)ky0 * w + kx0] != 0)
    {
        return false;
    }
    ++sHiddenNodes;
    return true;
}

// static
bool WolfFarProxy::hidesSubtree(const LLSpatialGroup* group)
{
    return allHidden(group, true);
}

// static
bool WolfFarProxy::hidesObjects(const LLSpatialGroup* group)
{
    return allHidden(group, false);
}

/** The parts of every cell with f > 0 that the camera can see. The caller has bound the program and set the state. */
void WolfFarProxy::drawParts(LLCamera& camera, bool far_clip, bool opaque, bool cut, bool fade_uniform)
{
    F32 last_fade = -1.f;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    for (auto& kv : mRegions)
    {
        Region& r = kv.second;
        if (!r.mModel || !r.mAnyFade || !r.mRegionp)
        {
            continue;
        }
        Model& m = *r.mModel;   // not const: LLVertexBuffer::setBuffer is not
        const F32 cell = (F32)m.mCellM;
        bool placed = false;
        for (U32 cy = 0; cy < m.mSide; ++cy)
        {
            for (U32 cx = 0; cx < m.mSide; ++cx)
            {
                const U32 c = cy * m.mSide + cx;
                const F32 f = r.mFade[c];
                if (!(f > 0.f) || m.mCellFirst[c] == m.mCellFirst[c + 1])
                {
                    continue;
                }
                for (U32 i = m.mCellFirst[c]; i < m.mCellFirst[c + 1]; ++i)
                {
                    Part& part = m.mParts[i];
                    const U32 first = opaque ? 0 : part.mOpaque;
                    const U32 count = (opaque ? part.mOpaque : 0) + (cut ? part.mCut : 0);
                    if (count == 0)
                    {
                        continue;
                    }
                    // the part's box in the agent frame: its cell, its own heights
                    LLVector4a centre, half;
                    centre.set(r.mOriginAgent.mV[VX] + ((F32)cx + 0.5f) * cell, r.mOriginAgent.mV[VY] + ((F32)cy + 0.5f) * cell,
                               r.mOriginAgent.mV[VZ] + 0.5f * (part.mZMin + part.mZMax));
                    half.set(0.5f * cell, 0.5f * cell, 0.5f * (part.mZMax - part.mZMin) + 0.01f);
                    if (!(far_clip ? camera.AABBInFrustum(centre, half) : camera.AABBInFrustumNoFarClip(centre, half)))
                    {
                        continue;
                    }
                    if (!placed)
                    {
                        placed = true;
                        // the model is in the region's own metres: the region's place in the agent frame goes on the
                        // matrix (LLVertexBuffer::drawRange syncs it to the bound program)
                        gGL.loadMatrix(gGLModelView);
                        gGL.translatef(r.mOriginAgent.mV[VX], r.mOriginAgent.mV[VY], r.mOriginAgent.mV[VZ]);
                        if (fade_uniform || cut)
                        {
                            gGL.getTexUnit(0)->bindFast(m.mAtlas.get());
                        }
                    }
                    if (fade_uniform && f != last_fade)
                    {
                        last_fade = f;
                        LLGLSLShader::sCurBoundShaderPtr->uniform1f(sFadeUniform, f);
                    }
                    part.mVB->setBuffer();
                    part.mVB->drawRange(LLRender::TRIANGLES, 0, part.mVerts - 1, count, first);
                    if (fade_uniform)
                    {
                        ++mDrawnParts;
                    }
                    else
                    {
                        ++mShadowParts;
                    }
                }
            }
        }
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = nullptr;
}

void WolfFarProxy::renderDeferred()
{
    ++mLogFrames;
    if (!mActive || mRegions.empty() || gWolfFarProxyProgram.mProgramObject == 0)
    {
        return;
    }
    bool any = false;
    for (const auto& kv : mRegions)
    {
        any = any || (kv.second.mModel && kv.second.mAnyFade && kv.second.mRegionp);
    }
    if (!any)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LLGLDisable blend(GL_BLEND);
    LLGLEnable cull(GL_CULL_FACE);
    // just in front of the real surfaces it is laid over in the band and where a real object reaches into a far
    // cell (the sign and size LLDrawPoolTerrain uses to lay its ownership overlay over the ground,
    // lldrawpoolterrain.cpp:880-881)
    LLGLEnable offset(GL_POLYGON_OFFSET_FILL);
    WolfVkState::polygonOffset(-1.f, -1.f);
    LLGLSLShader& shader = gWolfFarProxyProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);   // DESIGN.md: texture alpha under one half is a hole
    gGLLastMatrix = nullptr;
    drawParts(*LLViewerCamera::getInstance(), true, true, true, true);
    WolfVkState::polygonOffset(0.f, 0.f);
    shader.unbind();
}

void WolfFarProxy::renderShadowOpaque(LLCamera& shadow_cam)
{
    if (!mActive || mRegions.empty() || gCubeSnapshot)
    {
        return;
    }
    drawParts(shadow_cam, false, true, false, false);
}

void WolfFarProxy::renderShadowAlphaCut(LLCamera& shadow_cam)
{
    if (!mActive || mRegions.empty() || gCubeSnapshot)
    {
        return;
    }
    LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(0.5f);
    drawParts(shadow_cam, false, false, true, false);
}

/** With WolfLogFrameRate: what the proxies did, every 10 s (the measurements' evidence). */
void WolfFarProxy::logStats(F64 now)
{
    static LLCachedControl<bool> log_on(gSavedSettings, "WolfLogFrameRate", false);
    if (!log_on || now < mNextLog)
    {
        return;
    }
    mNextLog = now + LOG_SECS;
    U32 models = 0, far_cells = 0, band_cells = 0, boxes = 0, ids = 0;
    for (const auto& kv : mRegions)
    {
        const Region& r = kv.second;
        if (!r.mModel)
        {
            continue;
        }
        ++models;
        boxes += r.mModel->mKeepBoxes;
        ids += (U32)r.mModel->mKeepIds.size();
        for (size_t c = 0; c < r.mFade.size(); ++c)
        {
            far_cells += r.mFade[c] >= 1.f ? 1 : 0;
            band_cells += (r.mFade[c] > 0.f && r.mFade[c] < 1.f) ? 1 : 0;
        }
    }
    LL_INFOS("WolfFarProxy") << "far proxies: " << models << " models of " << mRegions.size() << " regions, " << far_cells
                             << " cells far, " << band_cells << " dissolving, " << boxes << " places and " << ids << " linksets listed by the models ("
                             << mKeepIdsFound << " of them here now) and " << sBridges.size()
                             << " moving objects keep " << mKeepCells << " fine cells real (" << mFarMovers << " movers on proxy cells, "
                             << mFarMoversSeen << " of them passed a cull lately); a frame: " << mDrawnParts / llmax(mLogFrames, 1U)
                             << " parts drawn, " << mShadowParts / llmax(mLogFrames, 1U) << " as shadow casters; " << sHiddenNodes << " octree nodes not looked at in the last "
                             << (S32)LOG_SECS << " s" << (mActive ? "" : " (standing down: build tools open)") << LL_ENDL;
    // where the first few of those movers are (region origin in 256 m cells: metres on the region), so that two log
    // lines 10 s apart show an object moving over proxy cells and still passing the cull (an object behind the camera or
    // behind a hill passes none, proxies or no proxies: LLDrawable::isRecentlyVisible, lldrawable.cpp:1175)
    if (mFarMovers > 0)
    {
        std::ostringstream where;
        for (U32 i = 0; i < llmin(mFarMovers, FAR_MOVER_SAMPLES); ++i)
        {
            const FarMover& sample = mFarSample[i];
            where << (i ? "; " : "") << sample.mGridX << "," << sample.mGridY << ": " << ll_round(sample.mLocal.mV[VX]) << " "
                  << ll_round(sample.mLocal.mV[VY]) << " " << ll_round(sample.mLocal.mV[VZ]) << (sample.mSeen ? " passed the cull" : " not in any view");
        }
        LL_INFOS("WolfFarProxy") << "movers on proxy cells: " << where.str() << LL_ENDL;
    }
    sHiddenNodes = 0;
    mDrawnParts = 0;
    mShadowParts = 0;
    mLogFrames = 0;
}
