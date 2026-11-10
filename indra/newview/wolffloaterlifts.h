/**
 * @file wolffloaterlifts.h
 * @brief WolfViewer: Cable Cars and Ski Lifts - lay out a cable car or chair lift on the ground, as a tunnel's route.
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

#ifndef WOLF_FLOATER_LIFTS_H
#define WOLF_FLOATER_LIFTS_H

#include <string>
#include <vector>

#include "llfloater.h"
#include "lluuid.h"
#include "v2math.h"
#include "v3math.h"
#include "wolffloatertunnels.h"   // WolfRouteHost, WolfToolTunnel: the Tunnel tool's pen, reused (as the Terraform window)

class LLButton;
class LLProgressBar;
class LLRadioGroup;
class LLTextBox;

/**
 * [WOLF LIFTS 2026-11-10] Ticket 691, Indi Moon: "Cable cars and ski lifts that can be laid out in a similar way to how we create
 * the paths for tunnels. Ski lift and cable cars slopes will follow the slope and angles of the terrain." Paul: built into the
 * viewer (not a HUD), simple for older residents. Design: firestorm/region-terrain-work/cable-cars-t691-2026-11-10/README.md.
 *
 * Persona: a region owner on a snowy mountain who has never built a lift. Story: "I click where the bottom station goes and
 * where the top station goes, I see the towers it will need, I press Build, and a chair lift is there to ride."
 * Flow: the window opens drawing (camera from above, the Tunnel tool's pen): 1. Cable car or Chair lift; 2. click the bottom
 * station, any bends, the top station (Undo last point, Start over, Ctrl+click on the line adds a bend, drag a point to move
 * it); the towers the line needs show in the world with the length and rise; 3. Build. Residents ride by clicking a cabin
 * or chair (every viewer); a station's touch menu calls the car and lets the owner take the lift down.
 *
 * Where the towers go is worked out in ONE place: the "Wolf Lifts - set up box" (region-terrain-work/.../lsl/lift_setup.lsl
 * plan()). The window rezzes the resident's own kit box (asked for from php/wolflifts.php when they have none), sends it the
 * clicked points and draws the plan it answers; Build tells it to build. The box listens to its owner on channel -76910: a
 * negative channel goes from the viewer as a ScriptDialogReply, which the region hears region-wide (LLClientView.cs
 * HandlerScriptDialogReply, ChatTypeEnum.Region; fsnearbychathub.cpp sendChatFromViewerFinal). It answers with
 * llRegionSayTo "WolfLift1|..." lines, which are taken out of chat (WolfLifts::onObjectChat, llviewermessage.cpp).
 */
namespace WolfLifts
{
    /** A "WolfLift1|..." line from an object to this agent (llRegionSayTo): the Lifts window's, never chat. */
    bool onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text);
}

class WolfFloaterLifts : public LLFloater, public WolfRouteHost
{
public:
    explicit WolfFloaterLifts(const LLSD& key);
    ~WolfFloaterLifts() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // WolfRouteHost (the pen)
    void clickAt(F32 x, F32 y) override;
    void moveClick(S32 k, F32 x, F32 y, bool done) override;
    void undoPoint() override;
    void removeClick(S32 k) override;
    void insertClickNear(F32 x, F32 y) override;
    void setStatus(const std::string& text, bool error) override;
    void penStopped() override;
    F32 planeHeight() const override;
    const std::vector<LLVector2>& clicks() const override { return mClicks; }
    bool routeEditing() const override;
    std::string noGroundText(bool underground) const override;
    std::string escapeText() const override;
    std::string markerLabel(size_t k) const override;
    std::string routeHint() const override;
    void renderRouteExtras(LLViewerRegion* rgn) override;

    /** A line from the set up box (WolfLifts::onObjectChat). */
    void onBoxLine(const LLUUID& source, const std::string& text);

private:
    struct Support
    {
        LLVector3 mCable;   // region metres
        F32 mGround = 0.f;
        bool mTower = false;
    };
    enum EBox { BOX_NONE, BOX_FINDING, BOX_READY };

    void changed();                       // the drawing or the kind changed: a new plan is wanted
    void sendPlan();
    void onBuild();
    void startOver();
    void lookFromAbove();
    void giveCameraBack();
    bool penAllowed() const;
    void keepPen();
    void updateControls();
    std::string kindWord() const;
    std::string pointsText() const;
    void say(const std::string& text);    // to the box (channel VCH)
    /** Find (or get, then rez) the resident's kit box; runs until it answers hello or fails. */
    void needBox();
    static void boxCoro(LLHandle<LLFloater> h);
    void boxFailed(const std::string& why);

    LLTextBox* mHint = nullptr;
    LLRadioGroup* mKind = nullptr;
    LLButton* mUndoBtn = nullptr;
    LLButton* mStartOverBtn = nullptr;
    LLButton* mTopViewBtn = nullptr;
    LLTextBox* mPlanInfo = nullptr;
    LLButton* mBuildBtn = nullptr;
    LLProgressBar* mProgress = nullptr;
    LLTextBox* mStatus = nullptr;

    std::vector<LLVector2> mClicks;
    LLUUID mRegionAt;
    bool mPenWanted = false;
    bool mStartPending = false;

    // the set up box
    EBox mBox = BOX_NONE;
    LLUUID mBoxId;
    std::string mBox8;
    LLUUID mWaitFor;                      // the box this window rezzed (its "hi" is the one taken), or null: any of ours
    bool mBoxCoroRunning = false;

    // the plan the box answered
    bool mPlanDirty = false;
    F64 mChangedAt = 0.0;
    S32 mPlanReq = 0;                     // the request the shown plan must answer
    bool mPlanOk = false;
    std::vector<Support> mPlan;
    std::string mPlanText;                // "450 m long, 3 towers, 74 m up" or the box's reason

    // building
    bool mBuilding = false;
    F64 mBuildNewsAt = 0.0;

    // the camera before the window turned it (wolffloatertunnels.cpp giveCameraBack)
    bool mCamSaved = false, mCamWasOnAvatar = true;
    LLVector3d mCamPos, mCamFocus, mCamFocusSet;
};

#endif // WOLF_FLOATER_LIFTS_H
