/**
 * @file wolffloatergreedy.h
 * @brief WolfViewer: Greedy, the dice game window (Wolf World > Greedy).
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

#ifndef WOLF_FLOATERGREEDY_H
#define WOLF_FLOATERGREEDY_H

#include "llfloater.h"
#include "lluiimage.h"
#include "v4color.h"
#include <vector>
#include "llsingleton.h"
#include "lluuid.h"
#include <memory>
#include "wolfgreedylocal.h"   // [GREEDY COMPUTER PLAYER 2026-11-11/] practice against the computer, played here

class LLButton;
class LLComboBox;
class LLIconCtrl;
class LLTextBox;
class LLTextEditor;

// [GREEDY WINDOW 2026-11-10] Ticket 721 (Jimmy Olsen), Paul: "anything we make like that must be built into the viewer NOT a
// hud", "the UI for Greedy should SCALE with the user's UI settings", "so the greedy is up to the user".
// Greedy is played on the Wolf dice server (wolfstorm/php/wolfdice.php + wolfdice_window.php; the server rolls, nobody can
// cheat), signed in as this login (X-Wolf-Agent + X-Wolf-Session, checked there with the grid's presence service).
// Design and wire format: region-terrain-work/greedy-dice-2026-11-10/README.md s3b (the window's JSON), s3c (the table).
//
// [W104 FEATFIX 2026-11-11] Paul: "the greedy thing needs to be a HUD not a floater, so users can play it while wandering
// around the grid, with the same hiding of the background etc." The game is now one session (WolfGreedyGame below: the
// requests, the polling, the game's state, the messages, the roll and its moment) with two views of it:
//   the Greedy HUD (wolfgreedyhud.cpp) - the way to play, along the bottom of the screen in the dashboard family as the
//     Fishing and Ski HUDs; Wolf World > Greedy and the Wolf HUDs tile switch it on and off;
//   the Greedy window (this floater) - the guide's cards (HOW TO PLAY on the HUD), the games to join (JOIN), the Full
//     view, and the Table view for a Greedy table in-world (sitting at one still offers it). Its old Strip view is the HUD.
class WolfGreedyGame : public LLSingleton<WolfGreedyGame>
{
    LLSINGLETON_EMPTY_CTOR(WolfGreedyGame);

public:
    enum EView { VIEW_HUD = 1, VIEW_WINDOW = 2 };
    /** A line of the HUD's message area / the window's message line. Newest first. */
    struct Msg
    {
        std::string mText;
        bool mError = false;
    };

    void viewOpened(EView v);
    void viewClosed(EView v);
    bool hudOn() const { return (mViews & VIEW_HUD) != 0; }
    /** [W104 FEATFIX review 2 2026-11-11] switched on, or waiting to come back once the resident has finished with a vehicle's
     *  panel or Fishing (see vehiclePanelUp): the menu tick and the Wolf HUDs tile show this */
    bool hudWanted() const { return hudOn() || mHudPending; }
    /** Every frame from the HUD control (wolfgreedyhud.cpp), also while it is off: a waiting HUD appears when the bottom is free. */
    void pendingTick();
    /** [review 2] the textures let go before the texture system shuts down (LLAppViewer::cleanup) */
    void releaseImages() { mHeldImages.clear(); }
    /** Wolf World > Greedy, the Wolf HUDs tile: the HUD on or off. */
    static void toggleHud();
    static void openHud();
    static void closeHud();
    /** [W104 FEATFIX review 2026-11-11] ONE BOTTOM PANEL AT A TIME (the rule of wolfgame.cpp / wolfhelm.cpp / wolfskis.cpp
     *  claimBottom).
     *  [W104 FEATFIX review 2 2026-11-11] Paul: "play while wandering". A VEHICLE's panel (dashboard, deck, helm, skis) is never
     *  pulled away from someone driving, flying, sailing or skiing: the Greedy HUD waits ("Greedy will appear when you have
     *  finished with the dashboard") and appears by itself when that panel goes; one of those switched on while Greedy shows
     *  sends Greedy back to waiting the same way (WolfGreedyHud::draw).
     *  [W104 MERGE 2026-11-11] with the Fishing HUD: Greedy and Fishing are one at a time, each treating the other as it treats
     *  a vehicle's panel (here and wolffishingoverlay.cpp WolfFishingSession::vehiclePanelUp), so the one switched on last waits
     *  and appears when the other closes; neither ever sends the showing one away, so they cannot send each other to waiting
     *  in turn. The game HUD steps aside by itself while Greedy shows, as it does for Fishing (wolfgame.cpp bottomBusy), and
     *  comes back when Greedy closes; the game HUD shown by hand takes the bottom and Greedy closes (gameHudClaims). */
    static std::string vehiclePanelUp();   // what Greedy waits for, with its article ("the dashboard", "Fishing"; "" none)
    /** The game HUD shown by hand (wolfgame.cpp claimBottom): the Greedy HUD closes, and a waiting one stops waiting. */
    static void gameHudClaims();

    /** Every frame from a view that shows (minimised = the window is minimised): the polling. */
    void tick(bool minimised);

    // the player's presses
    void roll();
    void bank();
    void newGame(bool practice, S32 target);
    void join(S32 id);
    void leave();
    void pass();
    void refreshList();
    /** The table the avatar sits at told us its game (WolfGreedy::onObjectChat): it is loaded (the table seated them). */
    void onTable(S32 game);

    // what the views show
    S32 game() const { return mGame; }
    const LLSD& state() const { return mState; }   // the last game answer ("game", "players", "tips"), undefined: none
    const LLSD& games() const { return mGames; }   // the last "list" answer
    bool inGame() const { return mGame && mState["game"]["in_game"].asBoolean(); }
    bool busy() const { return mBusy; }
    /** [W104 FEATFIX review 2026-11-11] a press that changes the game (new, start, roll, bank, pass, leave, join) is out and
     *  not answered yet: the views show those buttons off until it is (no second roll pressed on an unchanged screen). */
    bool changeInFlight() const { return mBusy && !mLocal && !mInFlight.empty() && mInFlight != "poll" && mInFlight != "list" && mInFlight != "mine"; }
    // ([W104 review 2026-11-11 W4] not during practice: its presses are played here and never wait for a server request, and
    // the one out then is only the leave of a server game, sendLeaveOwed)
    const std::vector<Msg>& messages() const { return mLog; }
    const std::string& message() const { return mMessage; }   // the line under the window's controls ("" none)
    bool messageIsError() const { return mMessageError; }
    /** Goes up on every change: a view redraws when it differs from the one it last showed. */
    U32 version() const { return mChange; }

    // the roll's moment: the dice tumble about a second, then a banner ("Bust!", "You banked 350!")
    bool rolling() const { return mRollStart >= 0.0; }
    S32 rollFrame(S32 die) const;   // the tumble frame this die shows now (rolling only)
    void landNow();                 // a click lands the dice at once
    const std::string& banner() const { return mBanner; }   // "" none
    /** [GREEDY COMPUTER PLAYER 2026-11-11] practice against the computer (wolfgreedylocal.h): the button the views pulse next
     *  ("R" roll / start / play again, "B" bank, "" none). Real games have no coach. */
    std::string coach() const { return mLocal ? mState["coach"].asString() : std::string(); }
    bool localGame() const { return mLocal != nullptr; }
    const LLColor4& bannerColor() const { return mBannerColor; }
    F32 bannerAge() const;          // seconds since it showed
    void setMessage(const std::string& text, bool error);

private:
    void send(const std::string& op, const std::string& extra = std::string());
    static void sendCoro(std::string op, std::string body);
    void onReply(const std::string& op, const LLSD& reply, const std::string& error, bool uncertain);
    void noteActivity();
    // [GREEDY COMPUTER PLAYER 2026-11-11] the practice game played here (wolfgreedylocal.h)
    void startLocal();
    void sendLeaveOwed();                    // [W104 review W4] the next server game still to be left, when nothing is out
    void dropLeaveOwed(S32 game);            // [W104 review] a game joined again is no longer to be left
    void endLocal(const std::string& why);
    void showLocal(const std::string& op);   // the local game's answer, through onReply as a server answer
    void localTick();                        // the computer's moves, paced so they can be watched
    void moments();                 // the roll's tumble ending, the banner going (from tick)
    void changed() { ++mChange; }

    U32 mViews = 0;
    S32 mGame = 0;
    S32 mVersion = -1;
    LLSD mState;
    LLSD mGames;
    bool mBusy = false;
    std::string mQueued;            // one action pressed while a request was out
    std::string mInFlight;          // [review] the request out now ("" none), and its arguments (W5: a join of another game is not a repeat)
    std::string mInFlightExtra;
    std::vector<S32> mLeaveOwed;    // [W104 review 2026-11-11 W4] server games to leave, sent beside a practice game (not queued:
                                    // the queue is cleared when practice starts and by a server answer that comes during it)
    bool mHudPending = false;       // [review 2] waiting for a vehicle's panel to go
    LLUUID mPendingSession;         // [review 3 F2] the login a wait belongs to
    std::vector<LLPointer<LLUIImage>> mHeldImages;   // [review] the dice faces and tumble frames, loaded while a view is open
    F64 mNextPoll = 0.0;
    F64 mLastActivity = 0.0;        // [A6] the last press, game change or return to a view: the poll pace slows after it
    bool mWasMinimised = false;     // [A6] no polls while the only view is minimised; one at once when it is opened out
    bool mPollNow = false;          // [A3/A6] the next frame polls whatever the pace (after a press whose answer was lost)
    std::vector<Msg> mLog;
    std::string mMessage;
    bool mMessageError = false;
    U32 mChange = 1;
    F64 mRollStart = -1.0;
    std::string mPendingBanner;
    LLColor4 mPendingBannerColor;
    std::string mBanner;
    LLColor4 mBannerColor;
    F64 mBannerStart = -1.0;
    std::unique_ptr<WolfGreedyLocal> mLocal;   // [GREEDY COMPUTER PLAYER 2026-11-11/] the practice game, when one is on
    F64 mLocalNext = 0.0;                      // the computer's next move, not before this
};

class WolfFloaterGreedy : public LLFloater
{
public:
    WolfFloaterGreedy(const LLSD& key);
    ~WolfFloaterGreedy() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;   // [W104 FEATFIX 2026-11-11] a click ends the dice tumble

    /** The table the avatar sits at told us its game (WolfGreedy below): offer the table view. */
    void onTable(S32 game, const std::string& label);
    /** They stood up from the table. */
    void onLeftTable();
    /** [W104 FEATFIX 2026-11-11] HOW TO PLAY on the HUD: the window opens on the guide's first card. */
    static void showGuideWindow();
    /** [W104 FEATFIX 2026-11-11] JOIN on the HUD: the window opens in the Full view, the games list read again. */
    static void showLobbyWindow();

private:
    void setMode(const std::string& mode, bool remember);
    void render();                  // the session's state into the controls (when its version changes)
    void show(const LLSD& state);
    void showNoGame();
    void showMessage();
    void applyDice(const LLSD& dice, const std::string& kept);
    void animate();                 // every frame: the tumble and the banner
    void refreshButtons();
    void onRoll();
    void onNew(bool practice);
    void onJoin();
    void showGuide(S32 page);

    std::string mMode = "full";
    S32 mGuidePage = -1;            // -1: the guide is not showing
    S32 mTableGame = 0;             // the game of the table we sit at (0: none)
    std::string mTableLabel;
    bool mOfferDismissed = false;
    std::string mModeBeforeOffer;   // the view to go back to if the table offer is declined (the offer shows in Full)
    U32 mSeen = 0;                  // the session's version last rendered
    bool mBoardEmpty = true;        // the scoreboard's empty state shows (no game)
    bool mHasTips = false;          // a practice game's tips show under the scoreboard
    bool mWasRolling = false;
};

class WolfGreedy : public LLSingleton<WolfGreedy>
{
    LLSINGLETON_EMPTY_CTOR(WolfGreedy);

public:
    void idle();
    /** True when the line was ours (never shown as chat). */
    bool onObjectChat(const LLUUID& source, U8 chat_type, const std::string& text);

private:
    void reset(const LLUUID& seat);

    LLUUID mSeatRoot;
    bool mDescKnown = false;
    bool mIsTable = false;
    bool mAnswered = false;
    S32 mHelloTries = 0;
    F64 mHelloAt = -100.0;
};

#endif // WOLF_FLOATERGREEDY_H
