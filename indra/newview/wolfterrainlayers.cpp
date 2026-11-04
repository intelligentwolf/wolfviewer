/**
 * @file wolfterrainlayers.cpp
 * @brief WolfViewer: Wolf Environment terrain textures — eight ground layers up to 4096 m.
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

#include "wolfterrainlayers.h"

#include <cmath>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "lldrawpoolterrain.h"
#include "llfloaterreg.h"
#include "llcoros.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llhttpconstants.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llshadermgr.h"
#include "llspinctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltexturectrl.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewercontrol.h"
#include "llvlcomposition.h"
#include "wolfgrid.h"
#include "wolfterrainpaint.h"
#include "wolftkgl.h"

namespace
{
    // PHP round($v, 2) / round($v, 3) (php/terrain_paint.php wolf_textures_normalise).
    F32 round2(F32 v) { return roundf(v * 100.f) / 100.f; }
    F32 round3(F32 v) { return roundf(v * 1000.f) / 1000.f; }

    /** php isset() + (float): a missing or null key gives the default. */
    bool num(const LLSD& m, const char* key, F64 dflt, F64& out)
    {
        out = (m.has(key) && !m[key].isUndefined()) ? m[key].asReal() : dflt;
        return std::isfinite(out);
    }

    bool is_null_entry(const LLSD& l)
    {
        // php: $l === null || $l === '' || $l === false
        return l.isUndefined() || (l.isString() && l.asString().empty()) || (l.isBoolean() && !l.asBoolean());
    }

    void notify(const std::string& msg)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", msg));
    }

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    // Default layers 1-4 when the region's own terrain is PBR materials, not textures: OpenSim's own
    // default terrain textures, RegionSettings.cs:113-116 DEFAULT_TERRAIN_TEXTURE_1..4 (Terrain Dirt,
    // Grass, Mountain, Rock). Checked present in robust.fsassets 2026-10-16. terrain_paint.js
    // WOLF_OPENSIM_DEFAULTS is the same list.
    const char* const OPENSIM_DEFAULTS[4] = {
        "b8d3965a-ad78-bf43-699b-bff8eca6c975", "abb783e6-3e93-26c0-248a-247666855da3",
        "179cdabd-398a-9b6b-1391-4dc333ba321f", "beb169c7-11ea-fff2-efe5-0f24dc881df2" };
    // Default layers 5-8 for high mountains: OpenSim library textures, bin/assets/TexturesAssetSet/
    // TexturesAssetSet.xml ("rocks" :302, "gravel" :98, "granite" :308, "snow1" :224). Checked present
    // in robust.fsassets 2026-10-16. terrain_paint.js WOLF_DEFAULT_HIGH is the same table.
    struct HighDefault { const char* id; F32 h, b, s; };
    const HighDefault HIGH_DEFAULTS[4] = {
        { "00000000-0000-1111-9999-000000000003", 500.f, 150.f, 16.f },    // rock
        { "00000000-0000-2222-3333-100000001014", 1500.f, 300.f, 16.f },   // alpine scree
        { "00000000-0000-1111-9999-000000000004", 2500.f, 300.f, 20.f },   // granite
        { "00000000-0000-2222-3333-100000001035", 3200.f, 400.f, 24.f } }; // snow
    constexpr S32 DEFAULT_CLIFF = 4;   // steep faces turn to the rock layer (terrain_paint.js WOLF_DEFAULT_CLIFF)
    const char* const LAYER_HINT[WolfTerrainLayers::LAYERS] = {
        "lowland", "grass", "hills", "upland", "rock", "alpine scree", "granite", "snow" };
}

std::string WolfTerrainLayerSet::key() const
{
    std::string k;
    k.reserve(LAYERS * 37);
    for (S32 i = 0; i < LAYERS; ++i)
    {
        k += mLayers[i].mId.isNull() ? std::string("-") : mLayers[i].mId.asString();
        k += ',';
    }
    return k;
}

WolfTerrainLayers::WolfTerrainLayers() {}

WolfTerrainLayers::~WolfTerrainLayers()
{
    // The GL context may already be gone at shutdown; the arrays' texture names die with it.
    for (auto& kv : mArrays) kv.second->mGL = 0;
}

// ── the record ─────────────────────────────────────────────────────────────────────────

// Source: php/terrain_paint.php wolf_textures_normalise — the same checks, clamps, rounding and messages.
bool WolfTerrainLayers::parse(const LLSD& w, bool& has, Set& out, std::string& error, S32* bad_layer)
{
    has = false;
    error.clear();
    if (bad_layer) *bad_layer = -1;
    if (w.isUndefined()) return true;
    if (!w.isMap()) { error = "wolfTextures must be an object or null"; return false; }
    const LLSD& lin = w["layers"];
    const S32 n_in = lin.isArray() ? (S32)lin.size() : 0;
    if (n_in > LAYERS) { error = llformat("wolfTextures has at most %d layers", LAYERS); return false; }
    Set s;
    bool have_prev = false;
    F32 prev_h = 0.f;
    for (S32 i = 0; i < LAYERS; ++i)
    {
        const LLSD l = i < n_in ? lin[i] : LLSD();
        if (is_null_entry(l))
        {
            if (i == 0) { error = "Wolf terrain layer 1 needs a texture"; return false; }
            s.mLayers[i] = WolfTerrainLayerSet::Layer();
            continue;
        }
        if (!l.isMap() || !l.has("id") || l["id"].isUndefined())
        {
            error = llformat("wolfTextures.layers[%d] must be {id, h, b, s} or null", i);
            return false;
        }
        std::string id = l["id"].asString();
        LLStringUtil::trim(id);
        LLStringUtil::toLower(id);
        if (!LLUUID::validate(id) || LLUUID(id).isNull())
        {
            error = llformat("Wolf terrain layer %d is not a texture id", i + 1);
            return false;
        }
        F64 h = 0.0, b = 0.0, t = 0.0;
        if (!num(l, "h", 0.0, h) || !num(l, "b", 10.0, b) || !num(l, "s", DEFAULT_TILE, t))
        {
            error = llformat("Wolf terrain layer %d has a bad number", i + 1);
            return false;
        }
        WolfTerrainLayerSet::Layer& L = s.mLayers[i];
        L.mId.set(id);
        L.mH = round2(llclamp((F32)h, MIN_H, MAX_H));
        L.mB = round2(llclamp((F32)b, MIN_BLEND, MAX_BLEND));
        L.mS = round3(llclamp((F32)t, MIN_TILE, MAX_TILE));
        if (have_prev && L.mH < prev_h)
        {
            error = llformat("Wolf terrain layer %d starts lower than the layer below it — heights must rise from layer 1 to layer 8", i + 1);
            if (bad_layer) *bad_layer = i;
            return false;
        }
        have_prev = true;
        prev_h = L.mH;
    }
    const LLSD& cin = w["corners"];
    const S32 nc = cin.isArray() ? (S32)cin.size() : 0;
    for (S32 k = 0; k < 4; ++k)
    {
        const F64 c = (k < nc && !cin[k].isUndefined()) ? cin[k].asReal() : 0.0;
        if (!std::isfinite(c)) { error = "wolfTextures.corners has a bad number"; return false; }
        s.mCorners[k] = round2(llclamp((F32)c, -MAX_CORNER, MAX_CORNER));
    }
    F64 noise = 0.0;
    if (!num(w, "noise", DEFAULT_NOISE, noise)) { error = "wolfTextures.noise is not a number"; return false; }
    s.mNoise = round2(llclamp((F32)noise, 0.f, MAX_NOISE));
    s.mCliff = (w.has("cliff") && !w["cliff"].isUndefined()) ? w["cliff"].asInteger() : -1;
    if (s.mCliff < -1 || s.mCliff >= LAYERS) { error = "wolfTextures.cliff must be a layer (0-7) or -1"; return false; }
    if (s.mCliff >= 0 && s.mLayers[s.mCliff].mId.isNull())
    {
        error = llformat("the cliff layer %d has no texture", s.mCliff + 1);
        return false;
    }
    out = s;
    has = true;
    return true;
}

bool WolfTerrainLayers::validate(const Set& s, Set& out, std::string& error, S32* bad_layer)
{
    bool has = false;
    return parse(toLLSD(s), has, out, error, bad_layer) && has;
}

LLSD WolfTerrainLayers::toLLSD(const Set& s)
{
    LLSD w;
    w["v"] = 1;
    LLSD layers = LLSD::emptyArray();
    for (S32 i = 0; i < LAYERS; ++i)
    {
        const WolfTerrainLayerSet::Layer& L = s.mLayers[i];
        if (L.mId.isNull()) { layers.append(LLSD()); continue; }
        LLSD l;
        l["id"] = L.mId.asString();
        l["h"] = (F64)L.mH;
        l["b"] = (F64)L.mB;
        l["s"] = (F64)L.mS;
        layers.append(l);
    }
    w["layers"] = layers;
    LLSD corners = LLSD::emptyArray();
    for (S32 k = 0; k < 4; ++k) corners.append((F64)s.mCorners[k]);
    w["corners"] = corners;
    w["noise"] = (F64)s.mNoise;
    w["cliff"] = s.mCliff;
    return w;
}

// Source: terrain_paint.js wolfDefaults — layers 1-4 are the region's own four terrain textures at
// the heights its estate mapping puts them (the stock composition, llvlcomposition.cpp
// generateHeights: (height - start) * 4 / range, so layer i is fully shown at start + i·range/4 and
// the boundary below it is at start + (i - ½)·range/4, the fade range/4 wide), averaged over the
// four corners; layers 5-8 are rock, alpine scree, granite and snow up to the 4096 m ceiling.
WolfTerrainLayers::Set WolfTerrainLayers::defaults(LLViewerRegion* regionp)
{
    LLVLComposition* compp = regionp ? regionp->getComposition() : nullptr;
    // OpenSim RegionSettings.cs:290 m_Elevation1NW = 10, :297 m_Elevation2NW = 60 (all four corners).
    F32 start = 10.f, range = 60.f;
    bool pbr = false;
    if (compp)
    {
        F32 st = 0.f, rg = 0.f;
        for (S32 c = 0; c < LLVLComposition::CORNER_COUNT; ++c)
        {
            st += compp->getStartHeight(c);
            rg += compp->getHeightRange(c);
        }
        if (std::isfinite(st)) start = st / (F32)LLVLComposition::CORNER_COUNT;
        if (std::isfinite(rg) && rg > 0.f) range = rg / (F32)LLVLComposition::CORNER_COUNT;
        pbr = compp->getMaterialType() == LLTerrainMaterials::Type::PBR;
    }
    Set s;
    F32 prev = MIN_H;
    for (S32 i = 0; i < 4; ++i)
    {
        LLUUID id = (compp && !pbr) ? compp->getDetailAssetID(i) : LLUUID::null;
        if (id.isNull()) id.set(OPENSIM_DEFAULTS[i]);
        const F32 h = (i == 0) ? llmax(MIN_H, llmin(0.f, start)) : llmax(prev, start + ((F32)i - 0.5f) * range / 4.f);
        prev = h;
        s.mLayers[i].mId = id;
        s.mLayers[i].mH = round2(h);
        s.mLayers[i].mB = round2(llmax(MIN_BLEND, range / 4.f));
        s.mLayers[i].mS = DEFAULT_TILE;
    }
    for (S32 i = 0; i < 4; ++i)
    {
        const F32 h = llmax(prev + 1.f, HIGH_DEFAULTS[i].h);
        prev = h;
        s.mLayers[4 + i].mId.set(HIGH_DEFAULTS[i].id);
        s.mLayers[4 + i].mH = h;
        s.mLayers[4 + i].mB = HIGH_DEFAULTS[i].b;
        s.mLayers[4 + i].mS = HIGH_DEFAULTS[i].s;
    }
    s.mNoise = DEFAULT_NOISE;
    s.mCliff = DEFAULT_CLIFF;
    Set out;
    std::string err;
    return validate(s, out, err) ? out : s;
}

// [WOLF TERRAIN SCREEN 2026-11-04] The automatic set — what a Wolf region with no Wolf layers saved draws.
// Source: llvlcomposition.h:134-137 corner enum SW 0, SE 1, NW 2, NE 3; getDetailAssetID / getStartHeight / getHeightRange
// are what lldrawpoolterrain.cpp renderFullShaderTextures feeds the Enhanced blend (wolf_comp_start / wolf_comp_range, the
// four detail textures) and renderFullShader's PBR test (getMaterialType).
// static
bool WolfTerrainLayers::readStandard(LLViewerRegion* regionp, StandardTerrain& out)
{
    LLVLComposition* compp = regionp ? regionp->getComposition() : nullptr;
    if (!compp) return false;
    out.mPbr = compp->getMaterialType() == LLTerrainMaterials::Type::PBR;
    for (S32 i = 0; i < 4; ++i) out.mIds[i] = compp->getDetailAssetID(i);
    for (S32 c = 0; c < LLVLComposition::CORNER_COUNT; ++c)
    {
        out.mStart[c] = compp->getStartHeight(c);
        out.mRange[c] = compp->getHeightRange(c);
    }
    return true;
}

// Source: terrainF.glsl main(), the Enhanced blend (wolf_terrain_look, step 1c) against wolfLayerGround (the bands):
//  - composition: comp = (z - start) * 4 / max(range, 1), start/range bilinear over the region; layer k is fully shown at
//    comp = k and the boundary between k-1 and k is at comp = k - 1/2, i.e. z = start + (k - 1/2) * range / 4. Bands:
//    t_k = smoothstep(h_k - b_k/2, h_k + b_k/2, hz). So h_k = S + (k - 1/2) R / 4.
//  - blend width: the tent weights (width 1) through the height blend (wh = w + 0.45 lum, ma = max(wh) - 0.5) mix two
//    layers only while |wh_a - wh_b| < 0.5, i.e. over 0.5 comp units = R / 8 metres. b_k = R / 8.
//  - corners: the Enhanced blend's bilinear is mix(mix(SW, NW, x), mix(SE, NE, x), y) (terrainF.glsl, "generateHeights'
//    operand order"; llvlcomposition.cpp bilinear() weights v10 = NW by x): the NW value sits at x = 1, y = 0 and the SE
//    value at x = 0, y = 1. wolfLayerGround's is mix(mix(c0, c1, x), mix(c2, c3, x), y). So the Wolf corners at (0,0),
//    (1,0), (0,1), (1,1) are the offsets of SW, NW, SE, NE, each the corner's middle boundary (comp 1.5) less the mean's.
//  - noise: Enhanced adds (n1 - .5) * 0.7 + (n2 - .5) * 0.3 + (n3 - .5) * 0.3 comp units; the bands add
//    ((n1 - .5) * 0.6 + (n2 - .5) * 0.25 + (n3 - .5) * 0.15) * 2 * noise metres, the same three noises. Matching the 25 m
//    term (the largest): 1.2 * noise = 0.7 * R / 4, noise = R * 0.7 / 4.8 (the 6 m term then matches to 3 %, the 2 m one
//    is 40 % weaker).
//  - steep faces: Enhanced pulls them to slot 2 (the estate's mountain/rock slot); the bands' cliff layer = 2.
//  - tile: the stock texgen repeats every 1 / LLDrawPoolTerrain::sDetailScale metres (RenderTerrainScale), phased by the
//    origin as wolf_layer_off is.
// NOT reproduced (no such input in the band model): hollows (heightmap AO) shifting the layer, the solid top above 3/4 of
// the range, the steep rule starting at 26 degrees (bands: 39), the per-texel luminance interlock, and corners whose ranges
// differ (fitted at the middle boundary). WolfStorm terrain_paint.js builds the same set.
// static
bool WolfTerrainLayers::fromStandard(const StandardTerrain& st, F32 tile, Set& out)
{
    if (st.mPbr) return false;
    for (S32 i = 0; i < 4; ++i) if (st.mIds[i].isNull()) return false;
    F32 start = 0.f, range = 0.f, mid[4];
    for (S32 c = 0; c < 4; ++c)
    {
        if (!std::isfinite(st.mStart[c]) || !std::isfinite(st.mRange[c])) return false;
        const F32 r = llmax(st.mRange[c], 1.f);   // terrainF.glsl max(range, 1.0)
        start += st.mStart[c];
        range += r;
        mid[c] = st.mStart[c] + 0.375f * r;
    }
    start *= 0.25f;
    range *= 0.25f;
    const F32 mid_mean = start + 0.375f * range;
    Set s;
    const F32 blend = range / 8.f;
    for (S32 i = 0; i < 4; ++i)
    {
        WolfTerrainLayerSet::Layer& L = s.mLayers[i];
        L.mId = st.mIds[i];
        const F32 first = start + 0.5f * range / 4.f;
        L.mH = (i == 0) ? llmax(MIN_H, llmin(llmin(0.f, start), first)) : start + ((F32)i - 0.5f) * range / 4.f;
        L.mB = blend;
        L.mS = tile;
    }
    s.mCorners[0] = mid[LLVLComposition::SOUTHWEST] - mid_mean;   // (0,0)
    s.mCorners[1] = mid[LLVLComposition::NORTHWEST] - mid_mean;   // (1,0), see above
    s.mCorners[2] = mid[LLVLComposition::SOUTHEAST] - mid_mean;   // (0,1)
    s.mCorners[3] = mid[LLVLComposition::NORTHEAST] - mid_mean;   // (1,1)
    s.mNoise = range * 0.7f / 4.8f;
    s.mCliff = 2;
    std::string err;
    return validate(s, out, err);
}

// The inverse of fromStandard, for the region's own four (other viewers): layers in use 1-4 (the last repeated if fewer),
// their From heights fitted (least squares) to start + (k - 1/2) * range / 4, k = 1..3, the corner offsets back on the
// corners they came from. fromStandard of the result gives the same set back whenever the four corners share one range.
// static
void WolfTerrainLayers::toStandard(const Set& s, const StandardTerrain& current, StandardTerrain& out)
{
    out = current;
    out.mPbr = false;
    S32 used[LAYERS];
    S32 n_used = 0;
    for (S32 i = 0; i < LAYERS; ++i) if (s.mLayers[i].mId.notNull()) used[n_used++] = i;
    if (n_used == 0) return;   // validate refuses that (layer 1 is always set); nothing to take
    for (S32 k = 0; k < 4; ++k) out.mIds[k] = s.mLayers[used[llmin(k, n_used - 1)]].mId;
    const S32 n = llmin(n_used, 4) - 1;   // boundaries between the four taken
    F32 start = 0.f, range = 0.f;
    if (n >= 2)
    {
        F32 mx = 0.f, mb = 0.f;
        for (S32 k = 1; k <= n; ++k) { mx += ((F32)k - 0.5f) / 4.f; mb += s.mLayers[used[k]].mH; }
        mx /= (F32)n;
        mb /= (F32)n;
        F32 sxx = 0.f, sxb = 0.f;
        for (S32 k = 1; k <= n; ++k)
        {
            const F32 dx = ((F32)k - 0.5f) / 4.f - mx;
            sxx += dx * dx;
            sxb += dx * (s.mLayers[used[k]].mH - mb);
        }
        range = sxb / sxx;
        start = mb - range * mx;
    }
    else if (n == 1)
    {
        for (S32 c = 0; c < 4; ++c) range += llmax(current.mRange[c], 1.f);
        range *= 0.25f;
        start = s.mLayers[used[1]].mH - range / 8.f;
    }
    else
    {
        return;   // one texture: its heights do not matter, keep the region's
    }
    range = llmax(range, 1.f);
    out.mStart[LLVLComposition::SOUTHWEST] = start + s.mCorners[0];
    out.mStart[LLVLComposition::NORTHWEST] = start + s.mCorners[1];
    out.mStart[LLVLComposition::SOUTHEAST] = start + s.mCorners[2];
    out.mStart[LLVLComposition::NORTHEAST] = start + s.mCorners[3];
    for (S32 c = 0; c < 4; ++c) out.mRange[c] = range;
}

const WolfTerrainLayers::Set* WolfTerrainLayers::automaticFor(LLViewerRegion* regionp) const
{
    // settings.xml WolfTerrainAutoLayers (default on): off draws the standard four on such regions, as before 2026-11-04
    static LLCachedControl<bool> auto_layers(gSavedSettings, "WolfTerrainAutoLayers", true);
    if (!auto_layers) return nullptr;
    StandardTerrain st;
    if (!readStandard(regionp, st)) return nullptr;
    const F32 tile = 1.f / LLDrawPoolTerrain::sDetailScale;   // the four textures' own repeat (renderFullShaderTextures tp0)
    Automatic& a = mAutomatic[regionp->getHandle()];
    bool same = a.mTile == tile && a.mFrom.mPbr == st.mPbr;
    for (S32 i = 0; same && i < 4; ++i)
    {
        same = a.mFrom.mIds[i] == st.mIds[i] && a.mFrom.mStart[i] == st.mStart[i] && a.mFrom.mRange[i] == st.mRange[i];
    }
    if (!same)
    {
        a.mFrom = st;
        a.mTile = tile;
        a.mOk = fromStandard(st, tile, a.mSet);
        if (a.mOk)
        {
            const WolfTerrainLayerSet& w = a.mSet;
            LL_INFOS("WolfTerrainLayers") << "automatic layers for " << regionp->getName() << ": from "
                << w.mLayers[1].mH << " / " << w.mLayers[2].mH << " / " << w.mLayers[3].mH << " m, blend " << w.mLayers[1].mB
                << " m, tile " << w.mLayers[0].mS << " m, corners " << w.mCorners[0] << " " << w.mCorners[1] << " " << w.mCorners[2]
                << " " << w.mCorners[3] << ", noise " << w.mNoise << LL_ENDL;
        }
    }
    return a.mOk ? &a.mSet : nullptr;
}

// ── drawing ────────────────────────────────────────────────────────────────────────────

// The pointer points into WolfTerrainPaint::mByHandle (cleared by every fetch) or into mEdit: use it
// within the call, never keep it (activeFor copies the set into mShown for bind).
const WolfTerrainLayers::Set* WolfTerrainLayers::setFor(LLViewerRegion* regionp) const
{
    if (!regionp) return nullptr;
    const U64 handle = regionp->getHandle();
    if (mEdit && mEdit->mHandle == handle && regionp == gAgent.getRegion())
    {
        // The working copy previews on the agent's own ground (editChanged keeps mLastGood); unticked is the automatic set.
        if (mEdit->mOn) return mEdit->mHasLastGood ? &mEdit->mLastGood : nullptr;
        return automaticFor(regionp);
    }
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().recordFor(handle);
    if (r && r->mHasWolf) return &r->mWolf;
    // [WOLF TERRAIN SCREEN 2026-11-04] Paul: "on wolf every region should be switched". No Wolf layers saved (or the record
    // not in yet): the automatic set from the region's own four. activeFor/bind only get here on a Wolf region
    // (WolfGrid::showsWolfRegion), and until the set's textures are in the array the standard four draw as before.
    return automaticFor(regionp);
}

bool WolfTerrainLayers::Array::ready() const
{
    if (!mGL || mState[0] != ST_READY) return false;
    for (S32 i = 1; i < LAYERS; ++i) if (mState[i] == ST_LOADING) return false;
    return true;
}

bool WolfTerrainLayers::Array::settled() const
{
    for (S32 i = 0; i < LAYERS; ++i) if (mState[i] == ST_LOADING) return false;
    return true;
}

WolfTerrainLayers::Array& WolfTerrainLayers::arrayFor(const Set& s)
{
    const std::string key = s.key();
    auto it = mArrays.find(key);
    if (it != mArrays.end()) return *it->second;
    std::unique_ptr<Array> a = std::make_unique<Array>();
    a->mStarted = LLFrameTimer::getElapsedSeconds();
    for (S32 i = 0; i < LAYERS; ++i)
    {
        a->mIds[i] = s.mLayers[i].mId;
        if (a->mIds[i].isNull()) { a->mState[i] = ST_EMPTY; continue; }
        a->mTex[i] = LLViewerTextureManager::getFetchedTexture(a->mIds[i], FTT_DEFAULT, true, LLGLTexture::BOOST_TERRAIN);
        a->mState[i] = a->mTex[i].notNull() ? ST_LOADING : ST_FAILED;
    }
    Array& ref = *a;
    mArrays[key] = std::move(a);
    return ref;
}

// Each layer: wait for the texture's full size, ask LL to keep a raw copy at the first discard level
// that is still at least PX on its short side (LLViewerFetchedTexture::forceToSaveRawImage — the
// texture is refetched to that level if it was already decoded and released), then copy it into the
// array. A missing asset fails the layer, and so does one still not in after LOAD_TIMEOUT_SECS; the
// shader skips failed layers, and layer 1 failing keeps the region on its own four textures.
void WolfTerrainLayers::poll(Array& a)
{
    const bool timed_out = LLFrameTimer::getElapsedSeconds() - a.mStarted > LOAD_TIMEOUT_SECS;
    for (S32 i = 0; i < LAYERS; ++i)
    {
        if (a.mState[i] != ST_LOADING) continue;
        LLViewerFetchedTexture* t = a.mTex[i].get();
        if (!t || t->isMissingAsset() || timed_out)
        {
            a.mState[i] = ST_FAILED;
            a.mTex[i] = nullptr;
            continue;
        }
        t->addTextureStats((F32)(PX * PX));
        const S32 fw = t->getFullWidth(), fh = t->getFullHeight();
        if (fw <= 0 || fh <= 0) continue;   // the header is not in yet
        S32 want = 0;
        S32 m = llmin(fw, fh);
        while ((m >> 1) >= PX && want < MAX_DISCARD_LEVEL)
        {
            m >>= 1;
            ++want;
        }
        t->forceToSaveRawImage(want, 30.f);
        if (!t->hasSavedRawImage() || t->getSavedRawImageLevel() < 0 || t->getSavedRawImageLevel() > want) continue;
        uploadLayer(a, i, t->getSavedRawImage());
        a.mTex[i] = nullptr;   // copied: the array holds the pixels now
    }
}

void WolfTerrainLayers::uploadLayer(Array& a, S32 i, const LLImageRaw* raw)
{
    if (!raw || raw->getWidth() <= 0 || raw->getHeight() <= 0)
    {
        a.mState[i] = ST_FAILED;
        return;
    }
    const S32 comps = raw->getComponents();
    LLPointer<LLImageRaw> src;   // only for grey images; RGB / RGBA are scaled straight from `raw`
    if (comps == 1 || comps == 2)
    {
        // Grey (+ alpha): spread to RGBA here — LLImageRaw::copy scales 3 <-> 4 only.
        const S32 w = raw->getWidth(), h = raw->getHeight();
        src = new LLImageRaw((U16)w, (U16)h, 4);
        LLImageDataSharedLock lock_in(raw);
        LLImageDataLock lock_out(src);
        const U8* in = raw->getData();
        U8* out = src->getData();
        if (!in || !out) { a.mState[i] = ST_FAILED; return; }
        for (S32 p = 0; p < w * h; ++p)
        {
            const U8 g = in[p * comps];
            out[p * 4] = out[p * 4 + 1] = out[p * 4 + 2] = g;
            out[p * 4 + 3] = comps == 2 ? in[p * comps + 1] : 255;
        }
    }
    else if (comps != 3 && comps != 4)
    {
        a.mState[i] = ST_FAILED;
        return;
    }
    if (src.notNull() && !src->getData())
    {
        a.mState[i] = ST_FAILED;
        return;
    }
    LLPointer<LLImageRaw> dst = new LLImageRaw(PX, PX, 4);
    // LLImageRaw::copy takes the source's shared lock itself, scales, and turns 3 components into 4.
    if (src.notNull()) dst->copy(src);
    else dst->copy(raw);
    if (!dst->getData())
    {
        a.mState[i] = ST_FAILED;
        return;
    }

    LLTexUnit* unit = gGL.getTexUnit(0);
    unit->activate();
    if (!a.mGL)
    {
        LLImageGL::generateTextures(1, &a.mGL);
        glBindTexture(GL_TEXTURE_2D_ARRAY, a.mGL);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, PX, PX, LAYERS, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Source: llrender.cpp LLTexUnit::setTextureFilteringOption — the same anisotropy rule.
        if (gGLManager.mHasAnisotropic && LLImageGL::sGlobalUseAnisotropic)
        {
            glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY, gGLManager.mMaxAnisotropy);
        }
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, a.mGL);
    }
    // LL keeps GL_UNPACK_ALIGNMENT at 1 (llrender.cpp LLRender::init); tight RGBA rows need no change,
    // and the row length is put back however it was found.
    GLint row_length = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
    if (row_length != 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    {
        LLImageDataSharedLock lock(dst);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, PX, PX, 1, GL_RGBA, GL_UNSIGNED_BYTE, dst->getData());
    }
    if (row_length != 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    a.mState[i] = ST_READY;
}

void WolfTerrainLayers::releaseArray(Array& a)
{
    if (a.mGL) { LLImageGL::deleteTextures(1, &a.mGL); a.mGL = 0; }
    for (S32 i = 0; i < LAYERS; ++i) a.mTex[i] = nullptr;
}

// static
bool WolfTerrainLayers::supported()
{
#if LL_DARWIN
    return false;
#else
    // llgl.cpp initGL: glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mNumTextureImageUnits)
    return gGLManager.mNumTextureImageUnits > 16;
#endif
}

bool WolfTerrainLayers::activeFor(LLViewerRegion* regionp)
{
    if (!regionp || !supported() || !WolfGrid::showsWolfRegion()) return false;   // [WOLF VISITOR SEES 2026-10-18/] visitors on a Wolf region too
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // Arrays no region has drawn for a while (a set changed, a neighbour left) are released.
    if (now >= mNextPrune)
    {
        mNextPrune = now + 10.0;
        for (auto it = mArrays.begin(); it != mArrays.end();)
        {
            if (now - it->second->mLastUsed > 30.0) { releaseArray(*it->second); it = mArrays.erase(it); }
            else ++it;
        }
        for (auto it = mShown.begin(); it != mShown.end();)
        {
            if (mArrays.find(it->second.mKey) == mArrays.end()) it = mShown.erase(it);
            else ++it;
        }
    }
    const U64 handle = regionp->getHandle();
    const Set* s = setFor(regionp);
    if (!s) { mShown.erase(handle); return false; }
    const std::string key = s->key();
    Array& a = arrayFor(*s);
    a.mLastUsed = now;
    poll(a);
    if (a.ready())
    {
        if (!a.mLoggedReady)
        {
            a.mLoggedReady = true;   // once per array: which region first drew it (diagnostics)
            LL_INFOS("WolfTerrainLayers") << "Wolf layers drawing on " << regionp->getName() << " (" << key << ")" << LL_ENDL;
        }
        Shown& sh = mShown[handle];
        sh.mKey = key;
        sh.mSet = *s;
        return true;
    }
    if (!a.settled())
    {
        // A changed set whose textures are still loading: keep drawing the one that was showing.
        auto it = mShown.find(handle);
        if (it != mShown.end())
        {
            auto ait = mArrays.find(it->second.mKey);
            if (ait != mArrays.end() && ait->second->ready())
            {
                ait->second->mLastUsed = now;
                return true;
            }
        }
    }
    mShown.erase(handle);
    return false;
}

void WolfTerrainLayers::bind(LLGLSLShader* shader, LLViewerRegion* regionp)
{
    static LLStaticHashedString s_on("wolf_layers_on");
    static LLStaticHashedString s_layer("wolf_layer");
    static LLStaticHashedString s_off("wolf_layer_off");
    static LLStaticHashedString s_corner("wolf_layer_corner");
    static LLStaticHashedString s_noise("wolf_layer_noise");
    static LLStaticHashedString s_cliff("wolf_layer_cliff");
    static LLStaticHashedString s_size("wolf_layer_size");
    static LLStaticHashedString s_noise_off("wolf_layer_noise_off");
    mBoundChannel = -1;
    if (!shader) return;
    // Exactly what activeFor decided to draw for this region this frame.
    const Shown* sh = nullptr;
    Array* a = nullptr;
    if (regionp && WolfGrid::showsWolfRegion())   // [WOLF VISITOR SEES 2026-10-18/] as activeFor
    {
        auto sit = mShown.find(regionp->getHandle());
        if (sit != mShown.end())
        {
            sh = &sit->second;
            auto it = mArrays.find(sh->mKey);
            if (it != mArrays.end()) a = it->second.get();
        }
    }
    const Set* s = sh ? &sh->mSet : nullptr;
    if (!s || !a || !a->ready())
    {
        shader->uniform1f(s_on, 0.f);
        return;
    }
    mBoundChannel = WolfTk::bindReserved(*shader, LLShaderMgr::WOLF_TERRAIN_LAYERS, GL_TEXTURE_2D_ARRAY, a->mGL);
    if (mBoundChannel < 0)
    {
        shader->uniform1f(s_on, 0.f);
        return;
    }
    const LLVector3d origin = regionp->getOriginGlobal();
    F32 layer[LAYERS * 4];
    F32 off[LAYERS * 2];
    for (S32 i = 0; i < LAYERS; ++i)
    {
        const WolfTerrainLayerSet::Layer& L = s->mLayers[i];
        const bool present = L.mId.notNull() && a->mState[i] == ST_READY;
        const F32 tile = L.mId.notNull() ? L.mS : DEFAULT_TILE;
        layer[i * 4] = L.mH;
        layer[i * 4 + 1] = L.mB;
        layer[i * 4 + 2] = tile;
        layer[i * 4 + 3] = present ? 1.f : 0.f;
        // lldrawpoolterrain.cpp renderFullShaderTextures' detail phase (fmod of the global origin by
        // the tile), per layer and in metres, from THIS region's origin so neighbours join up.
        off[i * 2] = (F32)fmod(origin.mdV[VX], (F64)tile);
        off[i * 2 + 1] = (F32)fmod(origin.mdV[VY], (F64)tile);
    }
    shader->uniform4fv(s_layer, LAYERS, layer);
    shader->uniform2fv(s_off, LAYERS, off);
    shader->uniform4f(s_corner, s->mCorners[0], s->mCorners[1], s->mCorners[2], s->mCorners[3]);
    shader->uniform1f(s_noise, s->mNoise);
    const bool cliff_ok = s->mCliff >= 0 && s->mCliff < LAYERS && layer[s->mCliff * 4 + 3] > 0.5f;
    shader->uniform1f(s_cliff, cliff_ok ? (F32)s->mCliff : -1.f);
    shader->uniform2f(s_size, regionp->getWidth(), regionp->getWidth());
    shader->uniform2f(s_noise_off, (F32)fmod(origin.mdV[VX], 1024.0), (F32)fmod(origin.mdV[VY], 1024.0));
    shader->uniform1f(s_on, 1.f);
}

void WolfTerrainLayers::unbind(LLGLSLShader* shader)
{
    (void)shader;
    if (mBoundChannel >= 0) WolfTk::unbindUnit(mBoundChannel, GL_TEXTURE_2D_ARRAY);
    mBoundChannel = -1;
}

S32 WolfTerrainLayers::layerState(S32 i) const
{
    if (i < 0 || i >= LAYERS) return ST_EMPTY;
    // [WOLF TERRAIN SCREEN 2026-11-04/] no arrays are made where the layers cannot draw (macOS, 16 units): not "loading" for ever
    if (!supported()) return ST_EMPTY;
    const Set* s = setFor(gAgent.getRegion());
    if (!s) return ST_EMPTY;
    auto it = mArrays.find(s->key());
    return it == mArrays.end() ? (s->mLayers[i].mId.isNull() ? ST_EMPTY : ST_LOADING) : it->second->mState[i];
}

// ── editing ────────────────────────────────────────────────────────────────────────────

WolfTerrainLayers::Edit* WolfTerrainLayers::beginEdit()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return nullptr;
    if (mEdit && mEdit->mHandle == rgn->getHandle()) return mEdit.get();
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    if (!r) return nullptr;
    mEdit = std::make_unique<Edit>();
    mEdit->mHandle = r->mHandle;
    mEdit->mUuid = r->mUuid;
    mEdit->mName = r->mName;
    mEdit->mBaseVersion = r->mVersion;
    mEdit->mHasBase = r->mHasWolf;
    mEdit->mBase = r->mWolf;
    // [WOLF TERRAIN SCREEN 2026-11-04] Nothing saved: the box starts ticked on the automatic set, which is what the region
    // draws already, so opening the screen changes nothing on the ground until Apply.
    const Set* automatic = r->mHasWolf ? nullptr : automaticFor(rgn);
    mEdit->mOn = r->mHasWolf || automatic != nullptr;
    mEdit->mSet = r->mHasWolf ? r->mWolf : (automatic ? *automatic : defaults(rgn));
    std::string err;
    mEdit->mHasLastGood = validate(mEdit->mSet, mEdit->mLastGood, err);
    return mEdit.get();
}

void WolfTerrainLayers::endEdit()
{
    mEdit.reset();
}

void WolfTerrainLayers::editChanged()
{
    if (!mEdit) return;
    mEdit->mDirty = true;
    ++mEdit->mRev;
    Set v;
    std::string err;
    if (validate(mEdit->mSet, v, err))
    {
        mEdit->mLastGood = v;
        mEdit->mHasLastGood = true;
    }
}

void WolfTerrainLayers::onRecordsFetched(U64 agent_handle)
{
    // terrain_paint.js refresh, the same rule as the paint editor's working copy.
    if (mEdit && (!mEdit->mDirty || mEdit->mHandle != agent_handle))
    {
        if (mEdit->mDirty) LL_WARNS("WolfTerrainLayers") << "unsaved Wolf terrain textures for " << mEdit->mName << " dropped: the region changed" << LL_ENDL;
        mEdit.reset();
    }
}

void WolfTerrainLayers::save()
{
    // [WOLF GRID GATE 2026-10-17] the save posts this login's session to Wolf's service: only from a confirmed Wolf region
    if (!WolfGrid::isOnWolfTerritoriesConfirmed()) { mLastError = "Sorry, this function is only available on Wolf Territories Grid."; mSaveResult = -1; return; }
    if (!mEdit) { mLastError = "Nothing to save."; mSaveResult = -1; return; }
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    if (!r || r->mHandle != mEdit->mHandle) { mLastError = "You have left the region these textures belong to."; mSaveResult = -1; return; }
    if (mSaving) return;
    LLSD wolf;   // undefined = null = the region's own four textures
    if (mEdit->mOn)
    {
        Set v;
        std::string err;
        if (!validate(mEdit->mSet, v, err)) { mLastError = err; mSaveResult = -1; return; }
        wolf = toLLSD(v);
    }
    const LLSD base = mEdit->mHasBase ? toLLSD(mEdit->mBase) : LLSD();
    mSaving = true;
    mLastError.clear();
    const std::string uuid = mEdit->mUuid;
    const S32 version = mEdit->mBaseVersion;
    const U32 rev = mEdit->mRev;
    LLCoros::instance().launch("WolfTerrainLayers save", [uuid, wolf, base, version, rev]()
    {
        WolfTerrainLayers::instance().saveCoro(uuid, wolf, base, version, rev);
    });
}

// Source: wolfterrainpaint.cpp saveCoro — the same endpoint, adapter and identity headers (agent +
// session ids the service checks against the grid's presence service; no secret in this viewer).
// The body has no `layout`, so the service changes only wolfTextures (php/terrain_paint.php).
void WolfTerrainLayers::saveCoro(std::string region_uuid, LLSD wolf, LLSD base, S32 version, U32 rev)
{
    boost::json::object body;
    body["region"] = region_uuid;
    body["wolfTextures"] = wolf.isUndefined() ? boost::json::value(nullptr) : LlsdToJson(wolf);
    body["wolfBase"] = base.isUndefined() ? boost::json::value(nullptr) : LlsdToJson(base);
    body["version"] = version;
    const std::string text = boost::json::serialize(body);

    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfTerrainLayers", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(text.data(), text.size());

    LLSD result = adapter->postRawAndSuspend(request, WolfTerrainPaint::API_URL, raw, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    mSaving = false;
    if (!status || !reply.isMap() || !reply["success"].asBoolean())
    {
        mLastError = reply.isMap() && reply.has("error") ? reply["error"].asString()
                                                         : (status ? std::string("unexpected reply") : status.toString());
        mSaveResult = -1;   // also on the Terrain tab's status line (WolfPanelTerrainLayers::draw)
        notify("Could not save the Wolf terrain textures: " + mLastError);
        return;
    }
    mLastError.clear();
    WolfTerrainPaint::instance().acceptRecord(reply["region"]);
    const std::string name = reply["region"]["name"].asString();
    const bool on = !wolf.isUndefined();
    if (mEdit && mEdit->mUuid == region_uuid && mEdit->mRev != rev)
    {
        // Changed again while the save was in flight: keep those changes, now on top of what was saved.
        mEdit->mBaseVersion = reply["region"]["version"].asInteger();
        std::string err;
        bool has = false;
        WolfTerrainLayers::Set sent;
        mEdit->mHasBase = on && parse(wolf, has, sent, err) && has;
        if (mEdit->mHasBase) mEdit->mBase = sent;
    }
    else
    {
        mEdit.reset();   // the stored set (now this one) draws from here on
    }
    mSaveResult = 1;
    // [WOLF TERRAIN SCREEN 2026-11-04] said on the Terrain tab's status line (WolfPanelTerrainLayers::draw), no pop-up
    LL_INFOS("WolfTerrainLayers") << (on ? "Wolf terrain textures saved for " : "automatic Wolf terrain textures again for ") << name << LL_ENDL;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfPanelTerrainLayers — Region / Estate > Terrain on Wolf Territories
// [WOLF TERRAIN SCREEN 2026-11-04] was WolfFloaterTerrainLayers (its own window, 2026-10-16); the same controls and rules,
// now inside the tab, saved by the tab's Apply (LLPanelRegionTerrainInfo::sendUpdate).
// ═══════════════════════════════════════════════════════════════════════════════════════

static LLPanelInjector<WolfPanelTerrainLayers> t_wolf_terrain_layers("wolf_terrain_layers");

WolfPanelTerrainLayers::WolfPanelTerrainLayers() : LLPanel() {}

// static
void WolfPanelTerrainLayers::openTerrainScreen()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly("Wolf Terrain Textures");   // [WOLF GRID GATE 2026-10-17/]
        return;
    }
    LLFloater* f = LLFloaterReg::showInstance("region_info");
    if (!f) return;
    // floater_region_info.xml region_panels; the tab's panel is named "Terrain" (panel_region_terrain.xml)
    if (LLTabContainer* tabs = f->findChild<LLTabContainer>("region_panels"))
    {
        tabs->selectTabByName("Terrain");
    }
}

bool WolfPanelTerrainLayers::postBuild()
{
    mOn = getChild<LLCheckBoxCtrl>("wolf_layers_on");
    mOn->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onOn, this));
    for (S32 i = 0; i < WolfTerrainLayers::LAYERS; ++i)
    {
        mTex[i] = getChild<LLTextureCtrl>(llformat("wolf_layer_tex_%d", i));
        mH[i] = getChild<LLSpinCtrl>(llformat("wolf_layer_h_%d", i));
        mB[i] = getChild<LLSpinCtrl>(llformat("wolf_layer_b_%d", i));
        mS[i] = getChild<LLSpinCtrl>(llformat("wolf_layer_s_%d", i));
        mClear[i] = getChild<LLButton>(llformat("wolf_layer_clear_%d", i));
        mState[i] = getChild<LLTextBox>(llformat("wolf_layer_state_%d", i));
        mTex[i]->setAllowNoTexture(i > 0);
        mTex[i]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onLayerTexture, this, i));
        mH[i]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onLayerValue, this, i));
        mB[i]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onLayerValue, this, i));
        mS[i]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onLayerValue, this, i));
        mClear[i]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onClearLayer, this, i));
    }
    static const char* const corner_names[4] = { "wolf_corner_sw", "wolf_corner_se", "wolf_corner_nw", "wolf_corner_ne" };
    for (S32 k = 0; k < 4; ++k)
    {
        mCorner[k] = getChild<LLSpinCtrl>(corner_names[k]);
        mCorner[k]->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onCorner, this));
    }
    mNoise = getChild<LLSpinCtrl>("wolf_noise");
    mNoise->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onNoise, this));
    mCliff = getChild<LLComboBox>("wolf_cliff");
    mCliff->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onCliff, this));
    mNote = getChild<LLTextBox>("wolf_layers_note");
    mError = getChild<LLTextBox>("wolf_layers_error");
    mStatus = getChild<LLTextBox>("wolf_layers_status");
    getChild<LLButton>("wolf_layers_defaults")->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onDefaults, this));
    getChild<LLButton>("wolf_layers_revert")->setCommitCallback(boost::bind(&WolfPanelTerrainLayers::onRevert, this));
    mError->setColor(LLColor4(1.f, 0.6f, 0.6f, 1.f));   // wolfcave.cpp setStatus, the same error colour
    mLabelColor = mH[0]->getEnabledTextColor();
    return true;
}

void WolfPanelTerrainLayers::refreshPanel()
{
    if (!WolfGrid::isOnWolfTerritories()) return;
    WolfTerrainPaint::instance().refresh();
    mShownHandle = 0;
    mShownVersion = -1;
    show();
}

bool WolfPanelTerrainLayers::hasChanges() const
{
    const WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().edit();
    // Unticked with nothing stored is already the automatic set: nothing to store.
    return ed && ed->mDirty && (ed->mOn || ed->mHasBase);
}

bool WolfPanelTerrainLayers::editForStandard(bool& use, WolfTerrainLayers::Set& out)
{
    use = false;
    const WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().edit();
    if (!ed || !ed->mDirty || !ed->mOn) return true;   // the region's own four stay as they are
    std::string err;
    if (!WolfTerrainLayers::validate(ed->mSet, out, err))
    {
        setStatus("Not applied: " + err, true);
        showValidation();
        return false;
    }
    use = true;
    return true;
}

void WolfPanelTerrainLayers::applyEdit()
{
    WolfTerrainLayers& wl = WolfTerrainLayers::instance();
    if (!hasChanges()) return;
    wl.save();   // a refusal before the request sets the save result, which draw() shows
}

void WolfPanelTerrainLayers::onScreenClosed(bool app_quitting)
{
    const WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().edit();
    if (!app_quitting && ed && ed->mDirty && WolfGrid::isOnWolfTerritories())   // [WOLF GRID GATE 2026-10-17/]
    {
        // Kept, like the paint editor's working copy, but said so: the ground still shows the draft.
        notify("Your Wolf terrain texture changes are not applied yet. They show only on your own screen. Open Region / Estate > Terrain and press Apply to keep them, or Undo.");
    }
}

void WolfPanelTerrainLayers::draw()
{
    WolfTerrainLayers& wl = WolfTerrainLayers::instance();
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    const WolfTerrainLayers::Edit* ed = wl.edit();
    // A new record (fetch, save, region change) shows again, unless an edit is in progress.
    if (r && (r->mHandle != mShownHandle || r->mVersion != mShownVersion) && !(ed && ed->mDirty)) show();
    if (!r && mShownHandle != 0) show();
    const S32 result = wl.takeSaveResult();
    if (result > 0)
    {
        show();
        setStatus("Wolf terrain textures saved.", false);
    }
    else if (result < 0)
    {
        setStatus("Wolf terrain textures not saved: " + wl.lastError(), true);
    }
    if (mWasSaving != wl.saving())
    {
        mWasSaving = wl.saving();
        if (mWasSaving) setStatus("Saving the Wolf terrain textures…", false);
    }
    // Layer load state, live, for the layers shown (the edit's, or for anyone who cannot edit, the region's own).
    for (S32 i = 0; i < WolfTerrainLayers::LAYERS; ++i)
    {
        std::string txt;
        if (mShown.mLayers[i].mId.isNull())
        {
            txt = (i == 0) ? "needs a texture" : "not used";
        }
        else
        {
            switch (wl.layerState(i))
            {
            case WolfTerrainLayers::ST_LOADING: txt = "loading…"; break;
            case WolfTerrainLayers::ST_READY:   txt = "loaded"; break;
            case WolfTerrainLayers::ST_FAILED:  txt = "could not load"; break;
            default:                            txt = LAYER_HINT[i]; break;
            }
        }
        mState[i]->setText(txt);
    }
    LLPanel::draw();
}

void WolfPanelTerrainLayers::show()
{
    WolfTerrainLayers& wl = WolfTerrainLayers::instance();
    const WolfTerrainPaint::Record* r = WolfTerrainPaint::instance().current();
    const bool holder = gAgent.canManageEstate();
    mShownHandle = r ? r->mHandle : 0;
    mShownVersion = r ? r->mVersion : -1;
    WolfTerrainLayers::Edit* ed = (r && holder) ? wl.beginEdit() : nullptr;
    // Paul 11-03: "keep it simple" - one line on who sees what, one on what Apply does (or who may change it).
    static const std::string WHO_SEES = "WolfViewer and WolfStorm show these. Other viewers show the standard four.";
    if (!r)
    {
        mNote->setText(WolfTerrainPaint::instance().fetching() ? std::string("Asking the grid for this region's terrain…")
                                                               : std::string("Wolf terrain textures are only for Wolf Territories regions."));
    }
    else if (!holder)
    {
        mNote->setText(WHO_SEES + "\nOnly the region or estate owner, or an estate manager, can change them.");
    }
    else
    {
        mNote->setText(WHO_SEES + "\nApply also sets the standard four from your first four layers.");
    }
    const bool editable = ed != nullptr;
    // What the region draws now when nothing is being edited: its saved layers, else the automatic set.
    const WolfTerrainLayers::Set* drawn = (r && r->mHasWolf) ? &r->mWolf : wl.automaticFor(gAgent.getRegion());
    // Unticked shows the automatic set the region falls back to (read-only until ticked again).
    const WolfTerrainLayers::Set* automatic = (ed && !ed->mOn) ? wl.automaticFor(gAgent.getRegion()) : nullptr;
    const WolfTerrainLayers::Set s = automatic ? *automatic : (ed ? ed->mSet : (drawn ? *drawn : WolfTerrainLayers::Set()));
    const bool rows = editable && ed->mOn;
    mShown = s;
    mOn->set(ed ? ed->mOn : (r != nullptr && drawn != nullptr));
    mOn->setEnabled(editable);
    for (S32 i = 0; i < WolfTerrainLayers::LAYERS; ++i)
    {
        const WolfTerrainLayerSet::Layer& L = s.mLayers[i];
        const bool used = L.mId.notNull();
        mTex[i]->setImageAssetID(L.mId);
        mTex[i]->setEnabled(rows);
        mH[i]->set(L.mH);
        mB[i]->set(L.mB);
        mS[i]->set(L.mS);
        mH[i]->setEnabled(rows && used);
        mB[i]->setEnabled(rows && used);
        mS[i]->setEnabled(rows && used);
        mClear[i]->setEnabled(rows && used && i > 0);
    }
    for (S32 k = 0; k < 4; ++k)
    {
        mCorner[k]->set(s.mCorners[k]);
        mCorner[k]->setEnabled(rows);
    }
    mNoise->set(s.mNoise);
    mNoise->setEnabled(rows);
    mCliff->removeall();
    mCliff->add("none", LLSD(-1));
    for (S32 i = 0; i < WolfTerrainLayers::LAYERS; ++i)
    {
        mCliff->add(llformat("layer %d", i + 1), LLSD(i), ADD_BOTTOM, s.mLayers[i].mId.notNull());
    }
    mCliff->setValue(LLSD(s.mCliff));
    mCliff->setEnabled(rows);
    getChildView("wolf_layers_defaults")->setEnabled(editable);
    getChildView("wolf_layers_revert")->setEnabled(editable);
    showValidation();
}

void WolfPanelTerrainLayers::changed()
{
    WolfTerrainLayers::instance().editChanged();
    if (mChanged) mChanged();   // LLPanelRegionTerrainInfo: arm Apply
}

void WolfPanelTerrainLayers::showValidation()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().edit();
    std::string err;
    S32 bad = -1;
    if (ed && ed->mOn)
    {
        WolfTerrainLayers::Set v;
        WolfTerrainLayers::validate(ed->mSet, v, err, &bad);
    }
    mError->setText(err);
    for (S32 i = 0; i < WolfTerrainLayers::LAYERS; ++i)
    {
        // The field that broke the rule is marked as well as named (the error line above).
        mH[i]->setLabelColor(i == bad ? LLColor4(1.f, 0.6f, 0.6f, 1.f) : mLabelColor);
    }
}

void WolfPanelTerrainLayers::onLayerTexture(S32 i)
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    const LLUUID id = mTex[i]->getImageAssetID();
    if (id.isNull())
    {
        if (i == 0) { mTex[0]->setImageAssetID(ed->mSet.mLayers[0].mId); setStatus("Layer 1 needs a texture.", true); return; }
        onClearLayer(i);
        return;
    }
    WolfTerrainLayerSet::Layer& L = ed->mSet.mLayers[i];
    if (L.mId.isNull())
    {
        // A new layer starts just above the highest used layer below it (build_paint_tab.js same).
        F32 h = 0.f;
        for (S32 k = i - 1; k >= 0; --k)
        {
            if (ed->mSet.mLayers[k].mId.notNull()) { h = llmin(WolfTerrainLayers::MAX_H, ed->mSet.mLayers[k].mH + 100.f); break; }
        }
        L.mH = h;
        L.mB = 100.f;
        L.mS = 16.f;
    }
    L.mId = id;
    turnOnForEdit(ed);
    changed();
    show();
    setStatus(llformat("Layer %d set. It shows on your own ground now; Apply shows it to everyone.", i + 1), false);
}

void WolfPanelTerrainLayers::onLayerValue(S32 i)
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    WolfTerrainLayerSet::Layer& L = ed->mSet.mLayers[i];
    if (L.mId.isNull()) return;
    L.mH = llclamp(mH[i]->get(), WolfTerrainLayers::MIN_H, WolfTerrainLayers::MAX_H);
    L.mB = llclamp(mB[i]->get(), WolfTerrainLayers::MIN_BLEND, WolfTerrainLayers::MAX_BLEND);
    L.mS = llclamp(mS[i]->get(), WolfTerrainLayers::MIN_TILE, WolfTerrainLayers::MAX_TILE);
    turnOnForEdit(ed);
    changed();
    showValidation();
}

void WolfPanelTerrainLayers::onClearLayer(S32 i)
{
    if (i <= 0) return;
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    ed->mSet.mLayers[i] = WolfTerrainLayerSet::Layer();
    if (ed->mSet.mCliff == i) ed->mSet.mCliff = -1;
    turnOnForEdit(ed);
    changed();
    show();
    setStatus(llformat("Layer %d is not used now.", i + 1), false);
}

void WolfPanelTerrainLayers::onCorner()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    for (S32 k = 0; k < 4; ++k)
    {
        ed->mSet.mCorners[k] = llclamp(mCorner[k]->get(), -WolfTerrainLayers::MAX_CORNER, WolfTerrainLayers::MAX_CORNER);
    }
    turnOnForEdit(ed);
    changed();
}

void WolfPanelTerrainLayers::onNoise()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    ed->mSet.mNoise = llclamp(mNoise->get(), 0.f, WolfTerrainLayers::MAX_NOISE);
    turnOnForEdit(ed);
    changed();
}

void WolfPanelTerrainLayers::onCliff()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    ed->mSet.mCliff = mCliff->getValue().asInteger();
    turnOnForEdit(ed);
    changed();
    showValidation();
}

// [2026-10-16] Paul saved Jotunheim's eight layers and got "JOTUNHEIM is back on its standard four terrain textures": a region
// with no Wolf set opened with the box unticked while the window showed the eight defaults. Editing a layer, or pressing
// Mountains, means "I want these": tick the box.
void WolfPanelTerrainLayers::turnOnForEdit(WolfTerrainLayers::Edit* ed)
{
    if (ed && !ed->mOn)
    {
        ed->mOn = true;
        mOn->set(true);
    }
}

void WolfPanelTerrainLayers::onOn()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    ed->mOn = mOn->get();
    changed();
    show();
    setStatus(ed->mOn ? "Your own layers are back. Press Apply to keep them."
                      : "Layers made from the region's standard four textures. Press Apply to keep this.", false);
}

void WolfPanelTerrainLayers::onDefaults()
{
    WolfTerrainLayers::Edit* ed = WolfTerrainLayers::instance().beginEdit();
    if (!ed) return;
    ed->mSet = WolfTerrainLayers::defaults(gAgent.getRegion());
    turnOnForEdit(ed);
    changed();
    show();
    setStatus("High-mountain layers filled in. They show on your own ground now; Apply shows them to everyone.", false);
}

void WolfPanelTerrainLayers::onRevert()
{
    WolfTerrainLayers::instance().endEdit();
    mShownVersion = -1;
    show();
    setStatus("Back to the layers this region has now.", false);
}

void WolfPanelTerrainLayers::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    // wolfcave.cpp WolfFloaterCaves::setStatus — the same two colours.
    mStatus->setColor(error ? LLColor4(1.f, 0.6f, 0.6f, 1.f) : LLColor4(0.78f, 0.82f, 0.88f, 1.f));
}
