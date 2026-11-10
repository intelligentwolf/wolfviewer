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
#include "llsingleton.h"
#include "lluuid.h"

class LLButton;
class LLComboBox;
class LLIconCtrl;
class LLTextBox;
class LLTextEditor;

// [GREEDY WINDOW 2026-11-10] Ticket 721 (Jimmy Olsen), Paul: "anything we make like that must be built into the viewer NOT a
// hud", "the UI for Greedy should SCALE with the user's UI settings", "so the greedy is up to the user".
// Greedy is played on the Wolf dice server (wolfstorm/php/wolfdice.php + wolfdice_window.php; the server rolls, nobody can
// cheat). This window is the player's controls, signed in as this login (X-Wolf-Agent + X-Wolf-Session, checked there with
// the grid's presence service), in one of three views the player picks and the viewer remembers (WolfGreedyMode):
//   full  - the whole game here, anywhere;
//   table - just the controls, when a Greedy table in-world is the big display;
//   strip - a small bar at the bottom of the screen, to play while moving around.
// Design and wire format: region-terrain-work/greedy-dice-2026-11-10/README.md s3b (the window's JSON), s3c (the table).
class WolfFloaterGreedy : public LLFloater
{
public:
    WolfFloaterGreedy(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    /** The table the avatar sits at told us its game (WolfGreedy below): join it and offer the table view. */
    void onTable(S32 game, const std::string& label);
    /** They stood up from the table. */
    void onLeftTable();

private:
    enum class EStatus { INFO, ERR };

    void setMode(const std::string& mode, bool remember);
    void send(const std::string& op, const std::string& extra = std::string());
    void onReply(const std::string& op, const LLSD& reply, const std::string& error);
    static void sendCoro(LLHandle<LLFloater> handle, std::string op, std::string body);
    void show(const LLSD& state);
    void showNoGame();
    void showDice(const LLSD& dice, const std::string& kept);
    void setMessage(const std::string& text, EStatus kind);
    void refreshButtons();
    void onRoll();
    void onBank();
    void onNew(bool practice);
    void onJoin();
    void showGuide(S32 page);
    void playSound(const std::string& kind);

    std::string mMode = "full";
    std::string mModeBeforeStrip = "full";
    S32 mGame = 0;
    S32 mVersion = -1;
    LLSD mState;              // the last game answer ("game", "players", "tips")
    LLSD mGames;              // the last "list" answer
    bool mBusy = false;
    std::string mQueued;      // one action pressed while a request was out
    F64 mNextPoll = 0.0;
    S32 mGuidePage = -1;      // -1: the guide is not showing
    S32 mTableGame = 0;       // the game of the table we sit at (0: none)
    std::string mTableLabel;
    bool mOfferDismissed = false;
    std::string mModeBeforeOffer;   // the view to go back to if the table offer is declined (the offer shows in Full)
};

// The table link (README s3c): seated on an object whose root's description starts "greedy-table", this viewer says
// "greedy hello 1" on HELLO_CHANNEL until the table answers by llRegionSayTo(us, 0, "WolfGreedy1|table|<game>|<label>")
// ("WolfGreedy1|bye" when we stand). Those lines are taken out of chat here (llviewermessage.cpp), as the skis' are.
// Nothing is said or read off Wolf Territories, and a table never sends anything to a viewer that did not say hello.
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
