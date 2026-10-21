/**
 * @file wolffloatertunnels.h
 * @brief WolfViewer: the Tunnel tool - walkable spaces under the terrain (WolfSim WolfTunnelsModule).
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

#ifndef WOLF_FLOATER_TUNNELS_H
#define WOLF_FLOATER_TUNNELS_H

#include <vector>

#include "llfloater.h"
#include "llsingleton.h"
#include "lltool.h"
#include "lluuid.h"
#include "v3math.h"

class LLButton;
class LLCheckBoxCtrl;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;

/**
 * [WOLF TUNNELS 2026-10-21] Paul: "the goal of this is to allow tunneling under terrains we need to add a tool" (a Tunnel tool
 * in the build floater). Persona: a region owner who wants to carve a walkable tunnel under their hill without scripting.
 * Flow: open Tunnels (Build floater land tools, or Wolf World > Tunnels...) -> Draw walk tunnel / Draw rail tunnel -> press on
 * the ground at the tunnel's mouth and drag along where it runs, release -> the box is saved to the region at once (its floor
 * 1 m below where the drag began) -> pick it in the list to fine-tune centre, size and turn, Save changes; Go inside; Delete.
 *
 * The region keeps the boxes (WolfSim WolfTunnelsModule, the "WolfTunnels" capability, LLSD XML): GET answers
 * {success, edit, max, region, tunnels:[{id, c:[x,y,z], s:[x,y,z], yaw}]}, POST {op: add|update|remove, tunnel, id} answers the
 * same after the change. Inside a box the region does not push an avatar up to the terrain surface, so a hollow mesh built
 * below the ground can be walked in (the build itself is ordinary mesh/prims; the box only makes the space walkable).
 * Only the estate owner and estate managers may change them (the region checks); everyone else sees them read-only.
 * The boxes are drawn as outlines in the world while this window is open.
 */
namespace WolfTunnels
{
    struct Box
    {
        S32 mId = 0;
        LLVector3 mCentre;     // region metres
        LLVector3 mSize;       // full sizes: length (along the turn), width, height
        F32 mYawDeg = 0.f;     // turn about +Z, degrees
    };
    /** The boxes last read from the region the agent stands on. */
    const std::vector<Box>& boxes();
    LLUUID boxesRegion();
    S32 selectedId();
}

class WolfFloaterTunnels : public LLFloater
{
public:
    explicit WolfFloaterTunnels(const LLSD& key);
    ~WolfFloaterTunnels() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    /** The draw tool finished a box: add it to the region. */
    void addDrawn(const WolfTunnels::Box& b);
    /** Status line: confirmations quietly, failures in red and kept until the next action. */
    void setStatus(const std::string& text, bool error);
    /** Kinds the draw tool can make (sizes: walk 4 x 4 x 3.5 m, rail 6 x 6 x 6 m). */
    enum EKind { K_WALK = 0, K_RAIL = 1 };

private:
    void refresh();                 // GET
    void post(const LLSD& body);    // POST, then show the answer
    void apply(const LLSD& answer, bool from_post);
    void fillList();
    void onSelect();
    void onDraw(EKind kind);
    void onSave();
    void onDelete();
    void onGoInside();
    void updateButtons();
    void selectId(S32 id);
    static void getCoro(LLHandle<LLFloater> h, std::string url, LLUUID region);
    static void postCoro(LLHandle<LLFloater> h, std::string url, LLSD body, LLUUID region);

    LLScrollListCtrl* mList = nullptr;
    LLSpinCtrl* mSpin[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };   // cx cy cz length width height turn
    LLButton* mWalkBtn = nullptr;
    LLButton* mRailBtn = nullptr;
    LLButton* mSaveBtn = nullptr;
    LLButton* mDeleteBtn = nullptr;
    LLButton* mGoBtn = nullptr;
    LLButton* mRefreshBtn = nullptr;
    LLCheckBoxCtrl* mShowCheck = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mInfo = nullptr;
    bool mEdit = false;
    bool mBusy = false;
    S32 mMax = 0;
    LLUUID mRegionAtFetch;
};

/** The press-and-drag tool that draws a new tunnel box (transient over the pie, as WolfToolAdventurePlace). */
class WolfToolTunnel : public LLTool, public LLSingleton<WolfToolTunnel>
{
    LLSINGLETON(WolfToolTunnel);
public:
    void begin(LLHandle<LLFloater> floater, S32 kind);
    void cancel();
    bool active() const { return mActive; }

    /** The outlines while the Tunnels window is open and this tool is not the current one. */
    static void renderIdle();
    static void drawIdle();

    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKey(KEY key, MASK mask) override;
    void onMouseCaptureLost() override;
    bool isAlwaysRendered() override { return true; }
    void render() override;
    void draw() override;

private:
    bool surface(S32 x, S32 y, LLVector3& out) const;
    WolfTunnels::Box drawnBox() const;
    bool shown() const;

    LLHandle<LLFloater> mFloater;
    S32 mKind = 0;
    bool mActive = false;
    bool mDragging = false;
    LLVector3 mStart, mEnd;
};

#endif // WOLF_FLOATER_TUNNELS_H
