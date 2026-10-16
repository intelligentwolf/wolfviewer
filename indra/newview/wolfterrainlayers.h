/**
 * @file wolfterrainlayers.h
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

#ifndef WOLF_TERRAINLAYERS_H
#define WOLF_TERRAINLAYERS_H

#include <map>
#include <memory>
#include <string>

#include "llfloater.h"
#include "llpointer.h"
#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v4color.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLGLSLShader;
class LLImageRaw;
class LLSpinCtrl;
class LLTextBox;
class LLTextureCtrl;
class LLViewerFetchedTexture;
class LLViewerRegion;

// [WOLF TERRAIN TEXTURES 2026-10-16] Paul: "Terrain textures - the standard ones only go up to 500m
// can we have a Wolf Environment Terrain Textures instead that adds another 4 and goes up to 4096
// metres so wolfviewer and wolfstorm people can see on high mountains so it makes more sense."
//
// RECORD: the optional `wolfTextures` key of the region's terrain paint layout (wolfstorm.app
// php/terrain_paint.php — its header comment is the format and the band model; WolfStorm
// js/world/terrain_paint.js normaliseWolf and terrain_manager.js wsWolfLayers are the same rules,
// keep all three in step). Region holders only set it; absent = the region's own four textures,
// as every viewer draws them (the OpenSim RegionInfo terrain path is untouched).
//
// RENDERING: the eight textures of a set are copied (512² RGBA, from each texture's saved raw image)
// into ONE GL_TEXTURE_2D_ARRAY, bound to terrainF.glsl's reserved sampler wolfTerrainLayers, so
// the whole feature costs one texture unit (the classic terrain shader reaches 16, Apple's limit;
// see terrainF.glsl). lldrawpoolterrain.cpp draws a region with a ready set through the classic
// texture shader even if the region's own terrain is PBR materials; terrainF.glsl then takes the
// ground colour from the eight layers (wolfLayerGround()) instead of the four detail textures.
struct WolfTerrainLayerSet
{
    static constexpr S32 LAYERS = 8;
    struct Layer
    {
        LLUUID mId;          // null = layer not used (layer 0 is always set)
        F32    mH = 0.f;     // start height, metres (the middle of its blend)
        F32    mB = 10.f;    // blend width, metres
        F32    mS = 12.f;    // tile, metres per texture repeat
    };
    Layer mLayers[LAYERS];
    F32   mCorners[4] = { 0.f, 0.f, 0.f, 0.f };   // SW, SE, NW, NE band offsets (m), x east / y north
    F32   mNoise = 12.f;                          // band-edge noise amplitude (m)
    S32   mCliff = -1;                            // layer for steep faces, -1 = none
    /** The eight texture ids, the array cache key. */
    std::string key() const;
};

class WolfTerrainLayers : public LLSingleton<WolfTerrainLayers>
{
    LLSINGLETON(WolfTerrainLayers);
    ~WolfTerrainLayers();

public:
    typedef WolfTerrainLayerSet Set;
    // Source: php/terrain_paint.php WOLF_* (terrain_paint.js TerrainPaint.WOLF) — the same numbers.
    static constexpr S32 LAYERS = WolfTerrainLayerSet::LAYERS;
    static constexpr F32 MIN_H = -100.f;        // OpenSim Constants.cs:165 MinTerrainHeightmap
    static constexpr F32 MAX_H = 4096.f;        // Paul: "goes up to 4096 metres"
    static constexpr F32 MIN_BLEND = 0.5f;
    static constexpr F32 MAX_BLEND = 1024.f;
    static constexpr F32 MIN_TILE = 1.f;
    static constexpr F32 MAX_TILE = 256.f;
    static constexpr F32 DEFAULT_TILE = 12.f;   // settings.xml RenderTerrainScale default
    static constexpr F32 MAX_CORNER = 1024.f;
    static constexpr F32 MAX_NOISE = 64.f;
    static constexpr F32 DEFAULT_NOISE = 12.f;
    static constexpr S32 PX = 512;              // array layer size (terrain_paint.js WOLF.PX)
    static constexpr F64 LOAD_TIMEOUT_SECS = 60.0;   // a layer still loading after this counts as failed (WOLF.LOAD_TIMEOUT_MS)

    /**
     * php/terrain_paint.php wolf_textures_normalise, the same rules and messages: returns false
     * with `error` (and `bad_layer` >= 0 for a height that falls) when the value is refused;
     * true with has = false for null / absent, true with has = true and `out` otherwise.
     */
    static bool parse(const LLSD& w, bool& has, Set& out, std::string& error, S32* bad_layer = nullptr);
    /** The same rules on an edited set (round-trips through toLLSD). */
    static bool validate(const Set& s, Set& out, std::string& error, S32* bad_layer = nullptr);
    static LLSD toLLSD(const Set& s);
    /** High-mountain defaults: the region's own four textures at its estate heights, then rock,
     *  alpine scree, granite and snow (terrain_paint.js wolfDefaults, the same numbers). */
    static Set defaults(LLViewerRegion* regionp);

    /** Can this GPU draw the layers at all? They add a 16th sampler to the terrain program, so only where the GPU
     *  reports more than 16 texture image units (GL_MAX_TEXTURE_IMAGE_UNITS), and never on macOS, whose GL gives 16
     *  (Paul 10-16: "protect the mac version"). Elsewhere regions draw their standard four textures. */
    static bool supported();
    /** lldrawpoolterrain.cpp: does this region draw the Wolf layers this frame (set known, array ready)? */
    bool activeFor(LLViewerRegion* regionp);
    /** lldrawpoolterrain.cpp renderFullShaderTextures: uniforms + the array, or wolf_layers_on = 0. */
    void bind(LLGLSLShader* shader, LLViewerRegion* regionp);
    void unbind(LLGLSLShader* shader);

    // ── editing (WolfFloaterTerrainLayers) ───────────────────────────────────────────
    struct Edit
    {
        U64  mHandle = 0;
        std::string mUuid;
        std::string mName;
        S32  mBaseVersion = 0;
        bool mHasBase = false;   // the stored set the edit started from (sent as wolfBase)
        Set  mBase;
        bool mOn = false;        // "Use Wolf terrain textures on this region"
        Set  mSet;
        bool mHasLastGood = false;   // a half-typed edit that does not validate keeps the last good preview
        Set  mLastGood;
        bool mDirty = false;
        U32  mRev = 0;           // bumped by editChanged: a save keeps changes made while it was in flight
    };
    Edit* beginEdit();
    Edit* edit() { return mEdit.get(); }
    void  endEdit();
    void  editChanged();
    void  save();
    bool  saving() const { return mSaving; }
    const std::string& lastError() const { return mLastError; }
    /** A save finished since the floater last asked (cleared when read): 1 saved, -1 failed, 0 nothing. */
    S32   takeSaveResult() { S32 r = mSaveResult; mSaveResult = 0; return r; }
    /** State of layer i of the array the agent's region draws: 0 none, 1 loading, 2 ready, 3 failed. */
    S32   layerState(S32 i) const;
    /** WolfTerrainPaint fetchCoro: drop a clean edit, or one for a region we have left. */
    void  onRecordsFetched(U64 agent_handle);

    enum EState : U8 { ST_EMPTY = 0, ST_LOADING = 1, ST_READY = 2, ST_FAILED = 3 };

private:
    struct Array
    {
        LLUUID mIds[LAYERS];
        LLPointer<LLViewerFetchedTexture> mTex[LAYERS];
        U8   mState[LAYERS] = {};
        U32  mGL = 0;             // GL_TEXTURE_2D_ARRAY, LAYERS x PX x PX RGBA8, mipmapped
        F64  mLastUsed = 0.0;
        F64  mStarted = 0.0;      // when its layers were asked for (LOAD_TIMEOUT_SECS)
        bool ready() const;
        bool settled() const;     // no layer still loading
    };
    /** What each region drew last (activeFor): the array key and a copy of its set. bind() draws
     *  exactly this, so a changed set keeps showing the old one until its own textures are in. */
    struct Shown
    {
        std::string mKey;
        Set mSet;
    };
    /** The set a region draws: the edit preview for the agent's region, else its stored set. */
    const Set* setFor(LLViewerRegion* regionp) const;
    Array& arrayFor(const Set& s);
    void poll(Array& a);
    void uploadLayer(Array& a, S32 i, const LLImageRaw* raw);
    void releaseArray(Array& a);
    void saveCoro(std::string region_uuid, LLSD wolf, LLSD base, S32 version, U32 rev);

    std::map<std::string, std::unique_ptr<Array>> mArrays;
    std::map<U64, Shown> mShown;
    std::unique_ptr<Edit> mEdit;
    bool mSaving = false;
    S32  mSaveResult = 0;
    std::string mLastError;
    S32  mBoundChannel = -1;      // the unit bind() put the array on (unbind clears it)
    F64  mNextPrune = 0.0;
};

/** Region / Estate > Terrain > "Wolf terrain textures..." (floater_wolf_terrain_textures.xml). */
class WolfFloaterTerrainLayers : public LLFloater
{
public:
    WolfFloaterTerrainLayers(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    void show();                 // controls from the edit
    void onLayerTexture(S32 i);
    void onLayerValue(S32 i);
    void onClearLayer(S32 i);
    void onCorner();
    void onNoise();
    void onCliff();
    void turnOnForEdit(WolfTerrainLayers::Edit* ed);
    void onOn();
    void onDefaults();
    void onRevert();
    void onSave();
    void setStatus(const std::string& msg, bool error);
    void showValidation();

    LLCheckBoxCtrl* mOn = nullptr;
    LLTextureCtrl*  mTex[WolfTerrainLayers::LAYERS] = {};
    LLSpinCtrl*     mH[WolfTerrainLayers::LAYERS] = {};
    LLSpinCtrl*     mB[WolfTerrainLayers::LAYERS] = {};
    LLSpinCtrl*     mS[WolfTerrainLayers::LAYERS] = {};
    LLButton*       mClear[WolfTerrainLayers::LAYERS] = {};
    LLTextBox*      mState[WolfTerrainLayers::LAYERS] = {};
    LLSpinCtrl*     mCorner[4] = {};
    LLSpinCtrl*     mNoise = nullptr;
    LLComboBox*     mCliff = nullptr;
    LLTextBox*      mNote = nullptr;
    LLTextBox*      mError = nullptr;
    LLTextBox*      mStatus = nullptr;
    LLButton*       mSave = nullptr;
    U64  mShownHandle = 0;
    S32  mShownVersion = -1;
    bool mWasSaving = false;
    LLColor4 mLabelColor;        // the spinners' own label colour (restored after a validation mark)
};

#endif // WOLF_TERRAINLAYERS_H
