/**
 * @file wolfgreedylocal.h
 * @brief WolfViewer: Greedy practice against the computer, played in the viewer.
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

#ifndef WOLF_GREEDYLOCAL_H
#define WOLF_GREEDYLOCAL_H

#include <string>
#include <vector>

#include "llsd.h"
#include "stdtypes.h"

// [GREEDY COMPUTER PLAYER 2026-11-11] Paul: "the greedy practice game: can we have a 'greedy player' that plays against you as
// well, rather than just you playing and getting stuck, and instructions to what is going on".
//
// WHERE IT RUNS (CHOSEN): in the viewer, not on the dice server. Practice was a server game (wolfdice_window.php new
// practice=1) for one player; a computer player there would need the server to play its turns between the resident's polls,
// a second player row the ranking code would count as a second avatar (wolfdice_lib.php wd_finish ranks any finished game with
// 2 avatars), and a deploy before anyone could try it. Played here, a practice game is never sent anywhere: it cannot reach a
// score or the top 10 by construction, it costs the servers nothing (Paul: think of scale), and it works the moment the viewer
// is installed. The rules are the server's own (wolfgreedyrules.h, every roll checked against the PHP).
//
// The game answers in the server's own JSON shape (README s3b: game, players, tips), so the Greedy HUD and window show it as
// they show any game; "coach" says which button to press next (they pulse it). Two players: the resident, who goes first, and
// the computer, Bramble. The target is the practice target, 2,000 (wolffloatergreedy.cpp newGame). Busts, the third-bust
// reset, the last turns and the winner follow wolfdice_lib.php wd_roll / wd_bank / wd_end_turn / wd_finish.
class WolfGreedyLocal
{
public:
    static const char* const COMPUTER_NAME;     // "Bramble"

    WolfGreedyLocal(const std::string& my_name, S32 target);

    void start();                   // a new game (Play again): scores from 0, the resident first
    bool roll(bool computer);       // false: not that player's turn
    bool bank(bool computer);
    /** The computer's next move (roll, or bank after a roll that scored) when it is its turn: true if it moved. */
    bool computerStep();
    bool computerTurn() const;
    bool finished() const { return mState == "finished"; }
    /** The game as the dice server would answer for the resident (wolfdice_window.php wdw_state), with "coach". */
    LLSD reply() const;

private:
    struct Player
    {
        std::string mName;
        S32 mScore = 0;
        S32 mBusts = 0;
        bool mFinalDue = false;
    };
    void endTurn();
    void finish();
    std::string tip() const;
    std::string coach() const;
    std::string status() const;
    S32 leaderOther(S32 who) const;   // the best score of the other player

    std::vector<Player> mPlayers;   // [0] the resident, [1] the computer
    S32 mTarget;
    std::string mState = "playing"; // playing | final | finished
    S32 mTurn = 0;
    S32 mTurnPoints = 0;
    S32 mDiceLeft = 6;
    bool mRolled = false;
    std::vector<S32> mDice;         // the last roll as shown (6, 0 = no die)
    std::string mKept = "000000";
    std::string mEvent;
    std::string mEventKind = "start";
    S32 mEventBy = -1;              // who the event was about (the tip explains it)
    std::string mWhat;              // the last scoring roll in words ("a 1 and a 5")
    S32 mLastPoints = 0;            // that roll's points, or the points lost / banked
    S32 mFinalBy = -1;
    S32 mWinner = -1;
    S32 mVersion = 1;
};

#endif // WOLF_GREEDYLOCAL_H
