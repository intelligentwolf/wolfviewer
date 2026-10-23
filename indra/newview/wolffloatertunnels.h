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
class LLComboBox;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLPanel;
class LLProgressBar;
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
 *
 * [WOLF TUNNELS 2026-10-22] Paul on Sandbox: "tunnel builder on sandbox could not draw inside the terrain" - "want to draw
 * underground - points only land on top of the ground; you want to see and draw the route inside the hill, at the depth you
 * choose". Flow now: set "Depth below ground" -> Draw / move points -> click the ground: each point goes in at that depth under
 * the spot (the ground is half see-through while the window is open, a level grid shows the depth; from inside a tunnel a click lands on that
 * grid instead) -> drag a point's white box to move it sideways, its yellow box to raise or lower it, or pick it and type its
 * depth -> Finish. "Mouths at ground level" keeps the first and last points on the ground. The floor runs straight from point
 * to point (design v2, a floor height per point); labels at the points give their depth below the ground.
 *
 * [WOLF TUNNELS 2026-10-22 easy] Paul on Sandbox: "the tunnel builder was bad, I couldn't do it :-( I tried drawing the points,
 * part of the floater I can't see, but I don't get the Build button enabled. Can we make it easier?"
 * Persona: a region owner who has never built a tunnel and wants one through their hill in a couple of minutes.
 * Flow: New tunnel -> click where it starts -> click the bends and the far end (the route shows live; the ends are the
 * mouths on the ground, the points between go down gently to the depth) -> Build tunnel (enabled from two points; when it
 * cannot be pressed the line under it says why and what to do). No Finish step: Enter / double-click only puts the pen
 * away. Depth, per-point depth, style, size, caverns, portals and see-through are under "More options", closed.
 *
 * [WOLF TUNNELS 2026-10-22 simple] Paul still could not make a tunnel (firestorm/wolfstorm/tools/TUNNEL_STRANGER_ASSESSMENT.md:
 * every try hit "too steep" or "piece too short", from where his clicks landed on the near face of the hill and from rules
 * applied after the clicks), and: "when I first show it, I just draw the tunnel, and it doesn't require me to create a new
 * tunnel first."
 * Persona: a region owner who has never built a tunnel and wants one through their hill in two minutes; they do not know the
 * words grade, box, rail, mouth or LSL.
 * Story: "I click where the tunnel goes in and where it comes out. It goes under the hill by itself, and I press Build."
 * Flow: 1. WHERE - the window opens drawing: the camera looks down on the avatar's area (given back on close), the pen is on,
 * the first click is the way In, then bends, the last click is the way Out; Backspace / Undo last point, Start over asks
 * first (in the window, never a modal box). 2. WHAT - Path / Road / Railway and Built / Natural, Path + Built every time the
 * window opens. 3. BUILD - one button, a progress bar, then Walk in.
 * Automatic: the floor (fitRoute): both ends on the ground, inside the hill a profile with at least COVER_M (2 m, Expert can
 * change it) of rock over the roof, never steeper than the kind's slope; a click too close to the one before is moved out to the shortest
 * piece the generator takes. The one refusal left: ends too far apart in height for the distance between them (the Out
 * marker turns red). Everything else is under Expert (closed); the region's tunnels list is under its own closed section.
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
     * [WOLF TUNNELS 2026-10-23] Paul: "when they rez in a cave because it's under the ground they can only move left and right and
     * not up and down". The lowest floor (box bottom, region metres) of the boxes this region position (agent's region) is in;
     * false when it is in none. LLWorld::getMinAllowedZ lets an object down to it instead of the land surface above the tunnel.
     */
    bool lowestBottom(const LLVector3& region_pos, F32& bottom_z);
    /**
     * The avatar's head (agent frame) is in a tunnel: pull the wanted camera position (agent frame) back along the line from
     * the head so it stays inside the boxes. False (camera untouched) when the head is in no box.
     */
    bool keepCameraInside(const LLVector3& head_agent, LLVector3& camera_agent);
    /** Terrain shaders (lldrawpoolterrain.cpp): the boxes near the camera that reach the ground, or none for this region. */
    void bindTerrain(LLGLSLShader* shader, LLViewerRegion* regionp);
    /**
     * [WOLF TUNNELS 2026-10-22] Paul: "the ground needs to be SEMI TRANSPARENT so you can see where it is but also see where your
     * tunnel is going". The share (SEE_THROUGH_MIN..SEE_THROUGH_MAX) of the ground's pixels the terrain shaders leave out on the
     * agent's region, so the hill is drawn as a fine see-through screen; 0 = off. The Tunnels window turns it on while it is open;
     * it is never saved, and goes off on any region change (and so on teleport and logout).
     */
    void setSeeThrough(F32 amount);
    bool seeThroughOn();

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
        // [WOLF TUNNELS 2026-10-22] the floor height at every point (design v2 "pz", one per point of mP; the floor is straight
        // between neighbouring points). Paul: "want to draw underground ... the route inside the hill, at the depth you choose".
        // A v1 record (one grade from z0 to z1) is read into this as the same straight grade.
        std::vector<F32> mZ;
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
        // [WOLF CAVE LIGHTS 2026-10-23] undefined = no lights; {style, tone, objects:[every part of every lamp], at}
        // (php/tunnels.php "lights", wolfcavelights.cpp)
        LLSD mLights;
    };
    /** Why the design cannot be built, in plain words (the generator's own limits); empty = it can. */
    std::string designProblem(const Design& d, F32 region_w);
    /** The route's length, metres. */
    F32 routeLength(const Design& d);
    /** The floor height at arc length t along the route (straight between the points' heights). */
    F32 floorAt(const Design& d, F32 t);
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

    // The pen (WolfToolTunnel) calls these.
    /** A click on the ground at (x, y), region metres: the room pen puts a room on the route there; else a new last point (the
     *  way Out), moved out to the shortest piece the generator takes when it is too close to the point before. */
    void clickAt(F32 x, F32 y);
    /** Clicked point k moved to (x, y) by its marker; `done` = the drag ended (the floor is worked out again at once, else at most
     *  10 times a second while dragging). */
    void moveClick(S32 k, F32 x, F32 y, bool done);
    void undoPoint();
    /** The pen was put down (by the window, another tool, the Build window): the window re-arms it while drawing is wanted. */
    void penStopped();
    /** The floor's height at the last clicked point (the level plane a click lands on when the camera is under the ground). */
    F32 planeHeight() const;

    /** The open window that is drawing a tunnel, or null (the tool draws its markers and preview). */
    static WolfFloaterTunnels* editing();
    const WolfTunnels::Design& design() const { return mDesign; }
    /** The points the user clicked (the way In, the bends, the way Out); the design has these plus the floor's own points. */
    const std::vector<LLVector2>& clicks() const { return mClicks; }
    /** The ends are too far apart in height for the distance: the Out marker is drawn red. */
    bool badEnd() const { return mBadEnd; }
    /** The point Expert's "Point" row edits (0-based), or -1 when Expert is closed. */
    S32 expertPoint() const;
    /** The one sentence about the drawing (the refusal, else the most important note), for the 3D view; empty = none. */
    const std::string& note() const { return mNote; }
    bool noteIsError() const { return mNoteError; }
    std::string nextStep() const;

    /** The local preview, worked out from the design and the ground this viewer has (no server call). */
    struct Preview
    {
        std::vector<LLVector3> mRoof, mFloor;   // triangles, region metres
        std::vector<LLVector3> mPosts;          // depth posts: ground, roof pairs (Expert)
        std::vector<std::string> mPostLabels;   // one per post
        std::vector<LLVector3> mBreakouts;      // where the roof is out of the ground
        std::vector<std::string> mWarnings;     // plain words, most important first
    };
    const Preview& preview() const { return mPreview; }

private:
    // the region's walkable boxes (Expert)
    void refresh() override;        // GET the WolfTunnels capability and the tunnel records
    void post(const LLSD& body);    // POST to the capability, then show the answer
    void apply(const LLSD& answer, bool from_post);
    void fillBoxList();
    void onSelectBox();
    void onSaveBox();
    void onDeleteBox();
    void onRemoveLeftovers();   // [2026-10-22] walkable parts no tunnel record owns
    void onGoInsideBox();
    void selectBox(S32 id);
    static void getCoro(LLHandle<LLFloater> h, std::string url, LLUUID region);
    static void postCoro(LLHandle<LLFloater> h, std::string url, LLSD body, LLUUID region);

    // the region's tunnels (their own closed section)
    void fillTunnelList();
    const WolfTunnels::Record* pickedTunnel() const;
    void onEditTunnel();
    void onDeleteTunnel();
    // [WOLF CAVE LIGHTS 2026-10-23] Light up / Remove lights for the picked tunnel (wolfcavelights.cpp).
    void onPickTunnel();
    void onLightUp();
    void onRemoveLights();
    void walkInto(const WolfTunnels::Record& r);

    // the drawing
    /** A new, empty drawing: Path + Built, Expert's numbers at their defaults (never remembered from last time). */
    void newDrawing();
    void onStartOver();
    void onStartOverAnswer(bool yes);
    void onBuild();
    void onKind();
    void onStyle();
    void onSize();
    void onExpertPoint();
    void onPointDepth();
    void onPointAuto();
    void onRoomPen();
    void onUndoRoom();
    /** The "See through the ground" box and its amount: WolfTunnels::setSeeThrough while this window is open. */
    void applySeeThrough();
    /** Show or hide Expert, the walkable boxes and the tunnels list, size the window to what shows, keep it on screen. */
    void relayout();
    /** Why Build cannot be pressed now, in plain words; empty = it can. */
    std::string buildBlocker() const;
    /** Work the floor out again (fitRoute), then the preview, the note and the buttons. */
    void designChanged();
    /** Rooms the route no longer passes are snapped back onto it and made to fit, or taken away (said in the status line). */
    void fitRooms();
    /** The first and last pieces at least the tunnel's width (wolftunnel.rs validate_design): the second / last point is moved out
     *  along its piece; false when the region's edge is in the way. */
    bool keepEndsLongEnough();
    void updateButtons();
    void showDesign();
    /** May the pen be down now (on a Wolf region that can make tunnels, the right to, no build running, the window shown)? */
    bool penAllowed() const;
    /** Put the pen down when drawing is wanted and nothing else holds the mouse (the Build window's focus drops it:
     *  llfloatertools.cpp:1619-1621 onFocusReceived -> lltoolmgr.cpp:190-195 setCurrentTool clears the transient tool). */
    void keepPen();
    // the camera: looking down on the drawing while the window is open, given back on close
    void lookFromAbove();
    void lookAtWayIn();
    void giveCameraBack();
    /** Keep an unbuilt drawing for the next time the window opens on this region (the window is destroyed on close). */
    void keepDrawing();
    bool restoreDrawing();

    LLTextBox* mHint = nullptr;                 // what to do next (nextStep)
    LLButton *mUndoPointBtn = nullptr, *mStartOverBtn = nullptr, *mTopViewBtn = nullptr;
    LLTextBox* mConfirmText = nullptr;          // Start over's question, in the window (a modal box would hold quitting up:
    LLButton *mConfirmYes = nullptr, *mConfirmNo = nullptr;   // llappviewer.cpp idleShutdown waits for every modal dialog)
    LLTextBox* mRouteInfo = nullptr;
    LLRadioGroup *mKind = nullptr, *mStyle = nullptr;
    LLButton* mRoomBtn = nullptr;
    LLButton *mBuildBtn = nullptr, *mWalkInBtn = nullptr;
    LLTextBox *mWarnings = nullptr, *mStatus = nullptr;
    LLProgressBar* mProgress = nullptr;
    LLCheckBoxCtrl *mExpertCheck = nullptr, *mListCheck = nullptr;
    LLPanel *mExpert = nullptr, *mAdvanced = nullptr, *mListPanel = nullptr;
    // Expert
    LLLineEditor* mName = nullptr;
    LLCheckBoxCtrl *mPortals = nullptr, *mMouths = nullptr, *mSeeThrough = nullptr, *mAdvancedCheck = nullptr;
    LLSpinCtrl *mW = nullptr, *mH = nullptr, *mCover = nullptr, *mPointNum = nullptr, *mPointDepth = nullptr;
    LLButton *mPointAutoBtn = nullptr, *mUndoRoomBtn = nullptr;
    LLSliderCtrl *mRoomR = nullptr, *mSeeThroughAmount = nullptr;
    LLTextBox* mGradeInfo = nullptr;
    // the region's tunnels
    LLScrollListCtrl* mTunList = nullptr;
    LLButton *mEditBtn = nullptr, *mGoTunBtn = nullptr, *mDelTunBtn = nullptr;
    LLComboBox *mLightStyle = nullptr, *mLightTone = nullptr;   // [WOLF CAVE LIGHTS 2026-10-23]
    LLButton *mLightUpBtn = nullptr, *mUnlightBtn = nullptr;
    std::string mLightPickShown;   // the tunnel whose light choice the combos show

    LLScrollListCtrl* mBoxList = nullptr;
    LLSpinCtrl* mSpin[7] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };   // cx cy cz length width height turn
    LLButton* mSaveBtn = nullptr;
    LLButton* mDeleteBtn = nullptr;
    LLButton* mGoBtn = nullptr;
    LLButton* mRefreshBtn = nullptr;
    LLButton* mLeftoversBtn = nullptr;
    LLCheckBoxCtrl* mShowCheck = nullptr;
    LLTextBox* mInfo = nullptr;
    bool mEdit = false;             // the capability says this agent may change boxes here
    bool mBusy = false;             // a capability request is out
    S32 mMax = 0;
    LLUUID mRegionAtFetch;
    std::string mCapUrl;

    // the tunnel being drawn
    bool mEditing = false;          // a drawing is in the window
    bool mPenWanted = false;        // drawing: the window keeps the pen down (keepPen) until Build, or the window closes
    bool mStartPending = false;     // onOpen ran: the Build window is closed and the camera turned in the first draw()
    bool mLookPending = false;      // ... and the camera turned to look down (the first show only)
    bool mOpened = false;           // onOpen has run for this window (it runs again on every show of an open window)
    bool mConfirming = false;       // Start over is asking
    bool mJustBuilt = false;        // the last build of this drawing finished (Walk in shows)
    bool mBuilding = false;         // this window started the build that is running (its end is shown here)
    std::string mEditId;            // empty until saved once
    std::vector<LLVector2> mClicks; // the way In, the bends, the way Out (region metres)
    std::vector<F32> mPins;         // per click: Expert's depth below the ground for its floor, or NaN (worked out)
    WolfTunnels::Design mDesign;    // the clicks plus the floor's own points, as the generator takes it
    bool mBadEnd = false;
    std::string mFitProblem;        // the one refusal left (fitRoute), shown first
    std::string mNote;              // the sentence shown under Build and in the 3D view
    bool mNoteError = false;
    bool mPreviewDirty = false;     // a marker drag changed the clicks; the floor follows at most every PREVIEW_DRAG_S
    F64 mPreviewAt = 0.0;
    Preview mPreview;
    std::string mProblem;           // designProblem, after the fit
    std::string mLoggedBlock;       // the last blocker written to the log (written once per change)
    U32 mShownJob = 0;              // the tunnel job serial the window last showed
    U32 mShownRecords = 0;          // the records serial the list was filled from
    // the camera before the window turned it (giveCameraBack)
    bool mCamSaved = false, mCamWasOnAvatar = true;
    LLVector3d mCamPos, mCamFocus, mCamFocusSet;
};

/** The pen of the Tunnel tool: route points (and a room) on the ground (transient over the pie, as WolfToolAdventurePlace),
 *  and the drawing of the design, its preview and the region's boxes while the Tunnels window is open.
 *  [2026-10-22 simple] A click always adds a point. A press on a marker drags it sideways only after the pointer has moved
 *  DRAG_START_PX while held (TUNNEL_STRANGER_ASSESSMENT.md: a shaky click used to grab a point and move it by metres). Alt-clicks
 *  go to the camera (lltool.cpp:193-209 getOverrideTool), the wheel and other buttons to the pie. Double-click and Enter never
 *  put the pen away; Escape only drops a drag (or the room pen). */
class WolfToolTunnel : public LLTool, public LLSingleton<WolfToolTunnel>
{
    LLSINGLETON(WolfToolTunnel);
public:
    enum EPen { PEN_NONE = 0, PEN_ROUTE = 1, PEN_ROOM = 2 };
    void begin(LLHandle<LLFloater> floater, EPen pen);
    void cancel();
    /** Switch between the route and the room pen while it is down. */
    void setPen(EPen pen);
    /** The window is going away: forget it first, so nothing calls back into it, then put the pen away. */
    void detach();
    EPen pen() const { return mActive ? mPen : PEN_NONE; }
    /** The pen is down and LLToolMgr still has it (another tool may have taken over without telling it). */
    bool current() const;

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
    void onMouseCaptureLost() override;

private:
    enum EDrag { DRAG_NONE, DRAG_PENDING, DRAG_MOVE };
    bool surface(S32 x, S32 y, LLVector3& out) const;
    /** The pointer on the level plane at height z, region metres (wolfcave.cpp WolfToolCave::planeHit). */
    bool planeHit(S32 x, S32 y, F32 z, F32& rx, F32& ry) const;
    /** Is the camera under the ground of the agent's region? */
    static bool cameraUnderground();
    /** Where a click at (x, y) lands on the ground (or on the level plane when the camera is underground), region metres. */
    bool place(S32 x, S32 y, LLVector3& ground) const;
    /** The clicked point whose marker is under the pointer, or -1. */
    S32 pickMarker(S32 x, S32 y) const;
    void dropDrag();
    bool shown() const;

    LLHandle<LLFloater> mFloater;
    EPen mPen = PEN_NONE;
    bool mActive = false;
    bool mHaveCursor = false;
    LLVector3 mCursor;              // the ground spot under the pointer (or on the plane)
    S32 mHover = -1;                // pickMarker under the pointer
    EDrag mDrag = DRAG_NONE;
    S32 mDragPoint = -1;
    S32 mPressX = 0, mPressY = 0;   // where the press on a marker was (screen)
    LLVector2 mDragStart;           // the point as it was at the press (put back if the drag is dropped)
    F32 mGrabDX = 0.f, mGrabDY = 0.f;
};

#endif // WOLF_FLOATER_TUNNELS_H
