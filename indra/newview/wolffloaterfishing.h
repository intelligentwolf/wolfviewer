/**
 * @file wolffloaterfishing.h
 * @brief WolfViewer: Fishing (ticket 679) - cast, reel in, your week and the grid's weekly top 10.
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

#ifndef WOLF_FLOATER_FISHING_H
#define WOLF_FLOATER_FISHING_H

#include <string>

#include "llfloater.h"
#include "lluuid.h"

class LLButton;
class LLCheckBoxCtrl;
class LLScrollListCtrl;
class LLTextBox;

/**
 * [WOLF FISHING 2026-11-10] Ticket 679, Jimmy Olsen: "rez a fishing rod from viewer and get it attached automatically and play
 * the reeling sounds etc and catch fishes"; Paul: "have a grid leaderboard that is reset every week". Built into the viewer,
 * not a HUD (paul-build-into-viewer-not-hud); simple for older residents: no codes, no countdowns, no confirm steps.
 * Design: firestorm/region-terrain-work/fishing-t679-2026-11-10/README.md.
 *
 * Persona: Margaret, 62, likes a quiet hour by the sea with friends; UI Size 1.2. Story: "I open Fishing, the rod is in my
 * hand, I face the water and press Cast; when a fish bites I press Reel in and see what I caught and where I am this week."
 * Flow: Wolf World > Fishing... -> the window finds the worn Wolf Fishing Rod (it answers "hello"), or puts it on from the
 * inventory, or first asks php/wolffishing.php for one (it is given into Objects > Fishing) -> Cast -> "Waiting for a bite"
 * -> "A fish is biting!" (no hurry: the fish waits) -> Reel in -> the catch, the score, the place -> Cast again.
 *
 * The rod's script (lsl/fish_rod.lsl) does the casting and asks the server, which alone decides the bite and the fish. This
 * window talks to the rod on channel -76790 (a negative channel goes from the viewer as a ScriptDialogReply, heard region-wide
 * from the avatar: LLClientView.cs HandlerScriptDialogReply), and the rod answers with llRegionSayTo "WolfFish1|..." lines,
 * taken out of chat here (WolfFishing::onObjectChat, llviewermessage.cpp). The week's board comes from the server
 * (POST {"action":"week"}), read when the window opens, after each catch and on Update - never polled.
 */
namespace WolfFishing
{
    /** A "WolfFish1|..." line from an object this agent owns (llRegionSayTo): the Fishing window's, never chat. `owner` is the
     *  ChatFromSimulator OwnerID, which the region writes. */
    bool onObjectChat(const LLUUID& source, const LLUUID& owner, U8 chat_type, const std::string& text);
}

/**
 * [FISHING HUD 2026-11-11] Since the Fishing HUD (wolffishingoverlay.h) this window is the Board: the HUD's BOARD button
 * opens it, and it keeps its own Cast / Reel in / Stop / Put the rod away buttons. It no longer holds the fishing itself:
 * the rod, the line and the week are WolfFishingSession's, shared with the HUD. Closing it while the HUD is open
 * only closes the board; closing the last of the two says "bye" to the rod, as closing this window always did.
 */
class WolfFloaterFishing : public LLFloater
{
public:
    explicit WolfFloaterFishing(const LLSD& key);
    ~WolfFloaterFishing() override;
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    void updateControls();
    void showWeek(const LLSD& week);
    void refreshWords();

    LLTextBox* mStatus = nullptr;
    LLButton* mCastBtn = nullptr;
    LLButton* mReelBtn = nullptr;
    LLButton* mStopBtn = nullptr;
    LLButton* mAwayBtn = nullptr;
    LLButton* mUpdateBtn = nullptr;
    LLCheckBoxCtrl* mChallenge = nullptr;
    LLTextBox* mMine = nullptr;
    LLScrollListCtrl* mMyBest = nullptr;
    LLScrollListCtrl* mBoard = nullptr;
    LLTextBox* mBoardNote = nullptr;
    LLTextBox* mLastWeek = nullptr;
    U32 mShownVersion = 0;
    bool mOpen = false;
};

#endif // WOLF_FLOATER_FISHING_H
