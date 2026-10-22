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

#include <string>
#include <vector>

#include "llfloater.h"
#include "llsingleton.h"
#include "lltool.h"
#include "lluuid.h"
#include "v3math.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLPanel;
class LLRadioGroup;
class LLSliderCtrl;
class LLGLSLShader;
class LLViewerRegion;
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
 *
 * [WOLF TUNNELS 2026-10-22] Paul stopped the w76 release: "make the tunneling work like the cave builder at the moment there's
 * just a box and no real instructions users should be able to 'Draw' the tunnel and it creates the mesh with caverns".
 * Persona: a region owner who wants a railway tunnel through their mountain, or a cave passage to explore.
 * Flow: New tunnel -> 1. Draw the route (click points on the ground, double-click / Enter / Finish) -> 2. Shape it (type,
 * Smooth or Rough, floor heights at the mouths with one smooth grade between, caverns clicked onto the route, stone portals;
 * a see-through preview, drawn here from the design, shows it in the ground with its depth and the warnings) -> 3. Build:
 * the design is saved (php/tunnels.php, region holders only), the proxy builds the rock (POST /tunnel_mesh, wolftunnel.rs:
 * lining + transparent box prims to walk on), it is rezzed here, and the walkable boxes it needs are added through the
 * capability. Delete and Build again take the rock and the boxes away first. The old box list is under Advanced.
 * Contract: firestorm/wolfstorm/tools/TUNNEL_DESIGN_V1.md.
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

    // [WOLF TUNNELS 2026-10-21] Paul in-world on Moonfold: "there is no hole in the terrain" and "when i walk the terrain
    // flashes". The boxes are now read on every arrival in a region that has the capability (not only while the window is
    // open), the terrain is not drawn inside them, and the camera of an avatar standing in one stays in the tunnel.

    /** Once, at start-up (llstartup.cpp): reads the boxes on each region change once that region's capabilities are in. */
    void init();
    /** Replace the stored boxes with a WolfTunnels answer ({tunnels:[{id,c,s,yaw}]}) for that region. */
    void setFromAnswer(const LLSD& answer, const LLUUID& region);
    /** Is this region position (agent's region) inside one of the boxes? */
    bool contains(const LLVector3& region_pos);
    /**
     * The avatar's head (agent frame) is in a tunnel: pull the wanted camera position (agent frame) back along the line from
     * the head so it stays inside the boxes. False (camera untouched) when the head is in no box.
     */
    bool keepCameraInside(const LLVector3& head_agent, LLVector3& camera_agent);
    /** Terrain shaders (lldrawpoolterrain.cpp): the boxes near the camera that reach the ground, or none for this region. */
    void bindTerrain(LLGLSLShader* shader, LLViewerRegion* regionp);

    // [WOLF TUNNELS 2026-10-22] Paul: "make the tunneling work like the cave builder ... users should be able to 'Draw' the
    // tunnel and it creates the mesh with caverns". The drawn tunnel: firestorm/wolfstorm/tools/TUNNEL_DESIGN_V1.md "Stored
    // design" (the contract with php/tunnels.php and rust_proxy/src/wolftunnel.rs, which builds it).
    struct Cavern { F32 mX = 0.f, mY = 0.f, mR = 8.f, mH = 6.f; };
    struct Design
    {
        std::string mStyle = "smooth";   // "smooth" | "rough"
        std::string mKind = "walk";      // "walk" | "wide" | "rail" | "custom"
        F32 mW = 4.f, mH = 3.5f;
        std::vector<F32> mP;             // the route at floor level: x0, y0, x1, y1, ... region metres
        F32 mZ0 = 0.f, mZ1 = 0.f;        // floor at the first and the last point
        std::vector<Cavern> mCaverns;
        bool mPortals = true;
        LLUUID mRock;
        F32 mRockScale = 4.f;
        S32 mSeed = 0;
    };
    /** One record of php/tunnels.php. */
    struct Record
    {
        std::string mId, mName, mOwner;
        Design mDesign;
        LLSD mBuilt;                     // undefined = not built; {item, objects:[...], volumes:[...], at}
    };
    /** Why the design cannot be built, in plain words (the generator's own limits); empty = it can. */
    std::string designProblem(const Design& d, F32 region_w);
    /** The route's length, metres. */
    F32 routeLength(const Design& d);
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

    /** Status line: confirmations quietly, failures in the caution colour and kept until the next action. */
    void setStatus(const std::string& text, bool error);

    // The pens (WolfToolTunnel) call these.
    void addRoutePoint(F32 x, F32 y);
    void finishRoute();
    void undoPoint();
    void addCavernAt(F32 x, F32 y);
    void penStopped();

    /** The open window that is editing a tunnel, or null (the tool draws its design and preview). */
    static WolfFloaterTunnels* editing();
    const WolfTunnels::Design& design() const { return mDesign; }
    bool routeOpen() const { return mRouteOpen; }

    /** The local preview, worked out from the design and the ground this viewer has (no server call). */
    struct Preview
    {
        std::vector<LLVector3> mRoof, mFloor;   // triangles, region metres
        std::vector<LLVector3> mPosts;          // depth posts: ground, roof pairs
        std::vector<std::string> mPostLabels;   // one per post
        std::vector<LLVector3> mBreakouts;      // where the roof is out of the ground
        std::vector<std::string> mWarnings;
    };
    const Preview& preview() const { return mPreview; }

private:
    // the region's walkable boxes (Advanced)
    void refresh();                 // GET the WolfTunnels capability and the tunnel records
    void post(const LLSD& body);    // POST to the capability, then show the answer
    void apply(const LLSD& answer, bool from_post);
    void fillBoxList();
    void onSelectBox();
    void onSaveBox();
    void onDeleteBox();
    void onGoInsideBox();
    void selectBox(S32 id);
    static void getCoro(LLHandle<LLFloater> h, std::string url, LLUUID region);
    static void postCoro(LLHandle<LLFloater> h, std::string url, LLSD body, LLUUID region);

    // the tunnels
    void fillTunnelList();
    const WolfTunnels::Record* pickedTunnel() const;
    void onNewTunnel();
    void onEditTunnel();
    void onDeleteTunnel();
    void onGoInsideTunnel();
    void onBuild();
    void onStopEditing();
    void onKind();
    void onStyle();
    void onSize();
    void onFloors();
    void onGround();
    void onUndoCavern();
    void onPen(S32 pen);
    void onAdvanced();
    void showDesign();
    void designChanged();
    void updateButtons();
    void setEditing(bool on);

    LLScrollListCtrl* mTunList = nullptr;
    LLButton *mNewBtn = nullptr, *mEditBtn = nullptr, *mGoTunBtn = nullptr, *mDelTunBtn = nullptr;
    LLLineEditor* mName = nullptr;
    LLButton *mDrawBtn = nullptr, *mUndoPointBtn = nullptr, *mFinishBtn = nullptr, *mGroundBtn = nullptr;
    LLButton *mCavernBtn = nullptr, *mUndoCavernBtn = nullptr, *mBuildBtn = nullptr, *mStopBtn = nullptr;
    LLComboBox* mKind = nullptr;
    LLSpinCtrl *mW = nullptr, *mH = nullptr, *mZ0 = nullptr, *mZ1 = nullptr;
    LLRadioGroup* mStyle = nullptr;
    LLSliderCtrl* mCavernR = nullptr;
    LLCheckBoxCtrl *mPortals = nullptr, *mAdvancedCheck = nullptr;
    LLTextBox *mRouteInfo = nullptr, *mGradeInfo = nullptr, *mWarnings = nullptr;
    LLPanel* mAdvanced = nullptr;

    LLScrollListCtrl* mBoxList = nullptr;
    LLSpinCtrl* mSpin[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };   // cx cy cz length width height turn
    LLButton* mSaveBtn = nullptr;
    LLButton* mDeleteBtn = nullptr;
    LLButton* mGoBtn = nullptr;
    LLButton* mRefreshBtn = nullptr;
    LLCheckBoxCtrl* mShowCheck = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mInfo = nullptr;
    bool mEdit = false;             // the capability says this agent may change boxes here
    bool mBusy = false;             // a capability request is out
    S32 mMax = 0;
    LLUUID mRegionAtFetch;
    std::string mCapUrl;

    // the tunnel being drawn
    bool mEditing = false;
    std::string mEditId;            // empty until saved once
    WolfTunnels::Design mDesign;
    bool mRouteOpen = false;        // the route pen is still adding points
    Preview mPreview;
    std::string mProblem;           // why Build cannot be pressed (designProblem), shown in the warnings
    U32 mShownJob = 0;              // the tunnel job serial the window last showed
    U32 mShownRecords = 0;          // the records serial the list was filled from
};

/** The pens of the Tunnel tool: route points and caverns on the ground (transient over the pie, as WolfToolAdventurePlace),
 *  and the drawing of the design, its preview and the region's boxes while the Tunnels window is open. */
class WolfToolTunnel : public LLTool, public LLSingleton<WolfToolTunnel>
{
    LLSINGLETON(WolfToolTunnel);
public:
    enum EPen { PEN_NONE = 0, PEN_ROUTE = 1, PEN_CAVERN = 2 };
    void begin(LLHandle<LLFloater> floater, EPen pen);
    void cancel();
    EPen pen() const { return mActive ? mPen : PEN_NONE; }

    /** The drawing while the Tunnels window is open and this tool is not the current one. */
    static void renderIdle();
    static void drawIdle();

    bool handleAnyMouseClick(S32 x, S32 y, MASK mask, EMouseClickType clicktype, bool down) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKey(KEY key, MASK mask) override;
    bool isAlwaysRendered() override { return true; }
    void render() override;
    void draw() override;

private:
    bool surface(S32 x, S32 y, LLVector3& out) const;
    bool shown() const;

    LLHandle<LLFloater> mFloater;
    EPen mPen = PEN_NONE;
    bool mActive = false;
    bool mHaveCursor = false;
    LLVector3 mCursor;
};

#endif // WOLF_FLOATER_TUNNELS_H
