/**
 * @file wolffishingoverlay.h
 * @brief WolfViewer: Fishing on the screen (ticket 679, 2026-11-11) - the fishing session shared by the Fishing HUD and the
 *        Board window, and the Fishing HUD drawn over the world (in the Ski HUD's style).
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

#ifndef WOLF_FISHING_OVERLAY_H
#define WOLF_FISHING_OVERLAY_H

#include <string>
#include <vector>

#include "llsd.h"
#include "llsingleton.h"
#include "lluictrl.h"
#include "wolfhudscale.h"

/**
 * [FISHING OVERLAY 2026-11-11] Paul approved (11-11) Fishing as a game on the screen instead of a plain window: a bar of big
 * buttons along the bottom (Cast, Reel in, Stop, Board, and a close x), the state in the middle ("A fish is biting! Reel it in
 * whenever you are ready", a gentle pulse, no countdown), a catch card that stays until it is tapped or the next cast, this
 * week's score / place / fish at the top right, and Challenge mode ("yes challenge mode"), off by default and remembered per
 * resident: reeling becomes holding Reel in while the line's tension stays out of the red.
 * Later on 11-11 Paul: "the fishing hud should be in the same style as the other huds": the screen part is the Fishing HUD
 * below (WolfFishingHud), drawn as the Ski HUD is; the week is in its band (left), the state in its middle, the buttons right.
 * Design of fishing itself: firestorm/region-terrain-work/fishing-t679-2026-11-10/README.md (the rod protocol, the server).
 *
 * Persona: Margaret, 62, a quiet hour by the sea with friends; UI Size 1.2. Story: "I open Fishing and big buttons appear
 * along the bottom; I press Cast, and when the screen tells me a fish is biting I press Reel in and a card shows me what I
 * caught and my place this week."
 * Flow: Wolf World > Games & Stories > Fishing (or the Wolf HUDs window) -> the HUD; the rod is found / given / put on (as the window always did)
 * -> Cast -> "Your float is in the sea. Waiting for a bite..." -> "A fish is biting!" (the fish waits) -> Reel in (Challenge
 * mode: hold Reel in, let go before the red) -> the catch card and the week panel -> Cast again. Board opens the Fishing
 * window (your week, the top 10). CLOSE ends fishing as closing the window did: the rod goes back to its touch menu.
 *
 * WolfFishingSession is the one fishing session: the rod (found, given, worn, answering), the line's state, the last catch,
 * the week; the HUD and the Board window (wolffloaterfishing.cpp) are two views of it. The session is open while either
 * view is open; when both have closed it says "bye" to the rod.
 *
 * CHALLENGE MODE AND FAIR PLAY: the server still decides every fish and its weight (wolffishing.php op=reel). The mini-game
 * is played in the viewer BEFORE the reel is sent: landed -> "WFISH|reel" exactly as in the default mode (the server then
 * picks the fish); lost -> "WFISH|stop", which makes the rod post op=cancel, deleting the cast on the server, so a lost fish
 * is never rolled and never counted. No rod, server or other-viewer change is needed, and a viewer that skips the game gains
 * nothing over the default mode (where one press reels in).
 */
class WolfFishingSession : public LLSingleton<WolfFishingSession>
{
    LLSINGLETON_EMPTY_CTOR(WolfFishingSession);

public:
    enum ERod { ROD_NONE, ROD_FINDING, ROD_READY };
    enum EFish { FISH_IDLE, FISH_WAITING, FISH_BITE, FISH_BUSY };
    enum EView { VIEW_HUD = 1, VIEW_BOARD = 2 };

    /** A line of the HUD's message area (Paul 11-11: "have a message area on the HUD that shows what's happened instead" of
     *  big message boxes over the scenery). Newest first; `mTransient` lines ("Casting...") are replaced by the next line. */
    struct Msg
    {
        std::string mText;
        bool mError = false;
        bool mTransient = false;
        bool mCatch = false;        // a catch, a junk catch or a lost fish (drawn in the catch colour)
    };

    /** One thing on the pile of today's catch (Paul 11-11: "a pile of fish as they catch them"). */
    struct PileItem
    {
        bool mJunk = false;
        S32 mLook = 0;              // wolffishingoverlay.cpp FISH_LOOKS / JUNK_LOOKS
    };
    static const S32 PILE_SHOWN = 15;  // CHOSEN: the most drawn one by one; past it a bigger heap and the count

    /** This resident's week, as the week panel shows it. */
    struct Me
    {
        bool mKnown = false;
        std::string mScore;
        S32 mFish = 0;
        S32 mPlace = 0;
    };

    void viewOpened(EView v);
    void viewClosed(EView v);
    bool active() const { return mViews != 0; }
    /** The Fishing HUD is open (WolfFishingHud draws). */
    bool hudOn() const { return (mViews & VIEW_HUD) != 0; }
    /** Wolf World > Games & Stories > Fishing, the Wolf HUDs tile: open the HUD (and the session). */
    static void openHud();
    /** The HUD's CLOSE: the HUD and the Board window close, the rod goes back to its touch menu. */
    static void closeAll();

    void cast();          // also "Get my rod" when there is no rod
    void reel();          // default mode: reel in; Challenge mode at a bite: start the mini-game
    void stop();
    void putAway();
    void refreshWeek();
    /** TIP THEM BACK: the pile of today's catch is emptied (the fish were weighed and let go anyway). */
    void tipBack();

    /** A "WolfFish1|..." line from the rod (WolfFishing::onObjectChat). */
    void onRodLine(const std::string& text);

    /** Called every frame by the views that are open: the no-answer recovery. */
    void tick();

    // Challenge mode
    bool challengeOn() const;
    void setChallenge(bool on);
    bool challengePlaying() const { return mPlaying; }
    /** One frame of the mini-game; `holding` = Reel in is held down. */
    void challengeStep(F32 dt, bool holding);
    F32 tension() const { return mTension; }
    F32 landed() const { return mLanded; }

    ERod rod() const { return mRod; }
    EFish fish() const { return mFish; }
    bool weekBusy() const { return mWeekBusy; }
    const std::string& status() const { return mStatus; }
    bool statusIsError() const { return mStatusError; }
    const std::vector<Msg>& messages() const { return mLog; }
    /** Goes up by one for every message added at the top (and down when a "just a moment" line gives way): the HUD keeps a
     *  scrolled-back view on the same lines as new ones arrive. */
    S32 logShift() const { return mLogShift; }
    const std::vector<PileItem>& pile() const { return mPile; }
    S32 pileCount() const { return mPileCount; }
    const Me& me() const { return mMe; }
    const LLSD& week() const { return mWeek; }
    const std::string& weekError() const { return mWeekError; }
    /** Goes up on every change: a view redraws its words when it differs from the one it last showed. */
    U32 version() const { return mVersion; }

    // the line tension the fish snaps (0..1 bar): the red starts here (wolffishingoverlay.cpp CHOSEN game tuning)
    static const F32 RED_AT;
    static const F32 AMBER_AT;

private:
    void changed() { ++mVersion; }
    void say(const std::string& text);
    void setStatus(const std::string& text, bool error, bool transient = false, bool isCatch = false);
    void addToPile(bool junk, const std::string& name);
    void setBusy();
    void needRod();
    void rodFailed(const std::string& why);
    void endChallenge();
    static void rodCoro(U32 gen);
    static void weekCoro();
    static WolfFishingSession* live(U32 gen);

    U32 mViews = 0;
    ERod mRod = ROD_NONE;
    EFish mFish = FISH_IDLE;
    std::string mStatus;
    bool mStatusError = false;
    std::vector<Msg> mLog;         // newest first, at most LOG_KEPT
    S32 mLogShift = 0;
    std::vector<PileItem> mPile;   // the last PILE_SHOWN things caught this session
    S32 mPileCount = 0;            // everything caught this session (the heap's count)
    bool mLostShown = false;       // a lost fish was just said: the rod's "Your line is out of the water." is not repeated
    Me mMe;
    LLSD mWeek;
    std::string mWeekError;
    bool mWeekBusy = false;
    bool mRodCoroRunning = false;
    U32 mRodGen = 0;               // a rod search belongs to one opening of the session (reviewer 11-11, items 2 and 3)
    U32 mVersion = 1;
    F64 mBusySince = 0.0;          // FISH_BUSY since (cast / reel / stop sent, no answer yet)
    S32 mHelloTries = 0;           // hellos sent since the rod last answered while busy
    bool mPlaying = false;         // Challenge mode's mini-game is on
    F32 mTension = 0.f;
    F32 mLanded = 0.f;             // 0..1: how much of the line is in
    F32 mPull = 0.f, mPullTarget = 0.f, mPullLeft = 0.f;
};

/**
 * [FISHING HUD 2026-11-11] The Fishing HUD. Paul (11-11): "the fishing hud should be in the same style as the other huds", so
 * it is one control the width of the screen in main_view.xml ("wolf_fishing_hud"), in the dashboard family exactly as the Ski
 * HUD (wolfskihud.cpp): the same 1600 x 304 mock frame with left / centre / right anchors, the night-blue band with its
 * frosted top edge (or, with PANEL off, only the read-outs on their own dark backings), B612 lettering, the same rounded
 * buttons, the same call-outs above the band, and the same size handle on its top edge (WolfFishingHudScale, wolfhudscale.h).
 * It follows UI Size as the Ski HUD does (its height is a share of the window times the UI scale). Left: your week (score,
 * place, fish). Centre: the message area - what has happened, newest at the top, older lines fading (Paul 11-11: "stop the
 * big message box ... people want to see the wonderful environment"), and in Challenge mode the line's tension. Right:
 * CAST, REEL IN, STOP, BOARD, CHALLENGE, BACKGROUND, CLOSE and the Wolf Fishing logo. At a bite the state line and REEL IN
 * pulse gently (no countdown). Above the band at the left: the pile of today's catch, with TIP THEM BACK. BACKGROUND off
 * (per resident, WolfFishingHudBackground) leaves only the buttons, the words (outlined so they read over any sky) and the
 * pictures over the world.
 * Draws nothing and takes no clicks unless Fishing is open (Wolf World > Games & Stories > Fishing, or the Wolf HUDs window);
 * Wolf Territories only.
 */
class WolfFishingHud : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfFishingHud(const Params& p);
    ~WolfFishingHud() override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

    enum EHit { H_NONE = 0, H_CAST, H_REEL, H_STOP, H_BOARD, H_CHALLENGE, H_PANEL, H_CLOSE, H_TIP,
                H_SCROLL_UP, H_SCROLL_DOWN, H_SCROLL_TRACK, H_SCROLL_THUMB };

private:
    struct Hit
    {
        F32 mL, mB, mR, mT;
        EHit mId;
        std::string mTip;
    };
    std::vector<Hit> mHits;             // in the picture's units (see mHitScale)
    F32 mPanelB = 0.f, mPanelT = 0.f;   // view-local, this frame
    WolfHudResize mResize{ "WolfFishingHudScale" };
    F32 mHitScale = 1.f;
    bool mDrawn = false;
    S32 mPressed = H_NONE;
    F64 mLastFrame = 0.0;
    // the message area's history (newest first): how far back it is scrolled, and this frame's layout for the bar
    S32 mScroll = 0;
    S32 mSeenShift = 0;
    S32 mMsgShown = 0, mMsgTotal = 0;
    F32 mMsgL = 0.f, mMsgR = 0.f, mMsgT = 0.f, mMsgB = 0.f;      // picture units
    F32 mTrackT = 0.f, mTrackB = 0.f, mThumbT = 0.f, mThumbB = 0.f;
    F32 mDragY = 0.f;
    S32 mDragScroll = 0;

    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip);
    const Hit* hitAt(S32 x, S32 y) const;
    bool onPanel(S32 x, S32 y) const;
    void press(EHit id);
    void drawPile(F32 band_top);
};

#endif // WOLF_FISHING_OVERLAY_H
