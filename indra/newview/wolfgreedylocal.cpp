/**
 * @file wolfgreedylocal.cpp
 * @brief WolfViewer: Greedy practice against the computer. Design: wolfgreedylocal.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfgreedylocal.h"

#include <algorithm>
#include <array>

#include "llrand.h"
#include "wolfgreedyrules.h"

const char* const WolfGreedyLocal::COMPUTER_NAME = "Bramble";

namespace
{
    // Source: wolfdice_lib.php WD_BUSTS_TO_ZERO - the notecard's third bust sends the score back to 0
    constexpr S32 BUSTS_TO_ZERO = 3;

    std::string number(S32 n)
    {
        // 10,000 style (wolfdice_lib.php wd_n; wolffloatergreedy.cpp number)
        std::string s = llformat("%d", n < 0 ? -n : n), out;
        S32 c = 0;
        for (size_t i = s.size(); i-- > 0;)
        {
            out.insert(out.begin(), s[i]);
            if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
        }
        return n < 0 ? "-" + out : out;
    }

    std::string possessive(const std::string& name)
    {
        // wolfdice_lib.php wd_possessive
        return name + (!name.empty() && name.back() == 's' ? "'" : "'s");
    }

    std::string countWord(S32 n)
    {
        static const char* const W[] = { "", "a", "two", "three", "four", "five", "six" };
        return n >= 0 && n <= 6 ? W[n] : llformat("%d", n);
    }

    /** The dice that scored, in plain words for the tips ("a 1 and a 5", "three 4s", "a run of five"). */
    std::string describeKept(const std::vector<S32>& dice, const std::vector<bool>& used)
    {
        std::array<S32, 7> c{};
        S32 n = 0;
        for (size_t i = 0; i < dice.size(); ++i) if (used[i]) { c[dice[i]]++; n++; }
        std::vector<S32> vals;
        for (S32 f = 1; f <= 6; ++f) if (c[f]) vals.push_back(c[f]);
        std::sort(vals.begin(), vals.end());
        if (n == 6)
        {
            if (vals == std::vector<S32>{ 1, 1, 1, 1, 1, 1 }) return "a full run, 1 to 6";
            if (vals == std::vector<S32>{ 2, 2, 2 }) return "three pairs";
            if (vals == std::vector<S32>{ 2, 4 }) return "four alike and a pair";
            if (vals == std::vector<S32>{ 3, 3 }) return "two threes alike";
        }
        std::vector<std::string> parts;
        const bool run15 = c[1] && c[2] && c[3] && c[4] && c[5];
        const bool run26 = c[2] && c[3] && c[4] && c[5] && c[6];
        if (run15 || run26)
        {
            parts.push_back("a run of five");
            for (S32 f = run15 ? 1 : 2; f <= (run15 ? 5 : 6); ++f) c[f]--;
        }
        for (S32 f = 1; f <= 6; ++f)
        {
            if (c[f] >= 3) { parts.push_back(countWord(c[f]) + " " + llformat("%d", f) + "s"); c[f] = 0; }
        }
        for (S32 f : { 1, 5 })
        {
            if (c[f] == 1) parts.push_back(llformat("a %d", f));
            else if (c[f] > 1) parts.push_back(countWord(c[f]) + " " + llformat("%d", f) + "s");
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0) out += i + 1 == parts.size() ? " and " : ", ";
            out += parts[i];
        }
        return out;
    }
}

WolfGreedyLocal::WolfGreedyLocal(const std::string& my_name, S32 target) : mTarget(target)
{
    mPlayers.resize(2);
    mPlayers[0].mName = my_name.empty() ? std::string("You") : my_name;
    mPlayers[1].mName = COMPUTER_NAME;
    start();
}

void WolfGreedyLocal::start()
{
    for (Player& p : mPlayers) { p.mScore = 0; p.mBusts = 0; p.mFinalDue = false; }
    mState = "playing";
    mTurn = 0;   // the resident first: practice is for learning, so they never wait to begin
    mTurnPoints = 0;
    mDiceLeft = 6;
    mRolled = false;
    mDice.assign(6, 0);
    mKept = "000000";
    mEvent = "Practice with " + std::string(COMPUTER_NAME) + " - you go first";
    mEventKind = "start";
    mEventBy = -1;
    mWhat.clear();
    mLastPoints = 0;
    mFinalBy = -1;
    mWinner = -1;
    ++mVersion;
}

bool WolfGreedyLocal::computerTurn() const
{
    return (mState == "playing" || mState == "final") && mTurn == 1;
}

S32 WolfGreedyLocal::leaderOther(S32 who) const
{
    return mPlayers[who == 0 ? 1 : 0].mScore;
}

// Source: wolfdice_lib.php wd_roll
bool WolfGreedyLocal::roll(bool computer)
{
    if ((mState != "playing" && mState != "final") || mTurn != (computer ? 1 : 0)) return false;
    Player& me = mPlayers[mTurn];
    const S32 n = llclamp(mDiceLeft, 1, 6);
    std::vector<int> dice;
    for (S32 i = 0; i < n; ++i) dice.push_back(ll_rand(6) + 1);
    std::vector<bool> used;
    const S32 pts = WolfGreedyRules::score(dice, used);
    mDice.assign(6, 0);
    for (S32 i = 0; i < n; ++i) mDice[i] = dice[i];
    mEventBy = mTurn;
    std::string rolled = "Rolled";
    for (int d : dice) rolled += llformat(" %d", d);
    if (pts == 0)
    {
        mLastPoints = mTurnPoints;
        me.mBusts++;
        mEvent = "BUST! " + me.mName + (mTurnPoints > 0 ? " loses " + number(mTurnPoints) : std::string());
        mEventKind = "bust";
        if (me.mBusts >= BUSTS_TO_ZERO)
        {
            me.mScore = 0;
            me.mBusts = 0;
            mEvent = "Third bust! " + me.mName + " back to 0";
        }
        mKept = std::string(n, 'x') + std::string(6 - n, '0');
        me.mFinalDue = false;   // wd_clear_final_due
        endTurn();
        return true;
    }
    std::vector<S32> dv(dice.begin(), dice.end());
    mWhat = describeKept(dv, used);
    mLastPoints = pts;
    mKept.clear();
    S32 usedN = 0;
    for (bool u : used) { mKept += u ? '1' : '0'; if (u) usedN++; }
    mKept += std::string(6 - n, '0');
    const S32 left = n - usedN;
    const bool lucky = left == 0;
    mTurnPoints += pts;
    mDiceLeft = lucky ? 6 : left;
    mRolled = true;
    mEvent = lucky ? "LUCKY SIX! +" + number(pts) + ", roll all 6" : rolled + " +" + number(pts);
    mEventKind = lucky ? "lucky" : "roll";
    ++mVersion;
    return true;
}

// Source: wolfdice_lib.php wd_bank
bool WolfGreedyLocal::bank(bool computer)
{
    if ((mState != "playing" && mState != "final") || mTurn != (computer ? 1 : 0)) return false;
    if (!mRolled || mTurnPoints <= 0) return false;
    Player& me = mPlayers[mTurn];
    me.mScore += mTurnPoints;
    mLastPoints = mTurnPoints;
    mEventBy = mTurn;
    mEvent = me.mName + " banks " + number(mTurnPoints) + " = " + number(me.mScore);
    mEventKind = "bank";
    me.mFinalDue = false;
    if (mState == "playing" && me.mScore >= mTarget)
    {
        // README s1: every other player gets one last turn
        for (S32 i = 0; i < (S32)mPlayers.size(); ++i) mPlayers[i].mFinalDue = i != mTurn;
        mState = "final";
        mFinalBy = mTurn;
        mEvent = me.mName + " reaches " + number(me.mScore) + "! Last turns";
    }
    endTurn();
    return true;
}

// Source: wolfdice_lib.php wd_end_turn / wd_next_player (two players, nobody away)
void WolfGreedyLocal::endTurn()
{
    mTurnPoints = 0;
    mDiceLeft = 6;
    mRolled = false;
    const S32 next = mTurn == 0 ? 1 : 0;
    if (mState == "final" && !mPlayers[next].mFinalDue) finish();
    else mTurn = next;
    ++mVersion;
}

// Source: wolfdice_lib.php wd_finish (never ranked: nothing here is ever sent)
void WolfGreedyLocal::finish()
{
    S32 win = -1;
    for (S32 i = 0; i < (S32)mPlayers.size(); ++i)
    {
        if (win < 0 || mPlayers[i].mScore > mPlayers[win].mScore || (mPlayers[i].mScore == mPlayers[win].mScore && i == mFinalBy)) win = i;
    }
    mState = "finished";
    mWinner = win;
    mEventKind = "win";
    mEvent = mPlayers[win].mName + " wins with " + number(mPlayers[win].mScore) + "!";
}

bool WolfGreedyLocal::computerStep()
{
    if (!computerTurn()) return false;
    if (!mRolled) return roll(true);
    // a wobble of -50, 0 or +50, so it does not play the same way every time
    const S32 wobble = (ll_rand(3) - 1) * 50;
    if (WolfGreedyRules::computerBanks(mTurnPoints, mDiceLeft, mPlayers[1].mScore, leaderOther(1), mTarget, mState == "final", wobble))
        return bank(true);
    return roll(true);
}

// Source: wolfdice_lib.php wd_status_text (for the resident)
std::string WolfGreedyLocal::status() const
{
    if (mState == "finished")
    {
        if (mWinner == 0) return "YOU WIN! " + number(mPlayers[0].mScore);
        return std::string(COMPUTER_NAME) + " wins! Press PLAY AGAIN";
    }
    const std::string pre = mState == "final" ? "LAST TURN! " : "";
    if (mTurn == 0) return pre + (mRolled && mTurnPoints > 0 ? "YOUR TURN: ROLL or BANK" : "YOUR TURN: press ROLL");
    return pre + possessive(COMPUTER_NAME) + " turn";
}

/** The button to press next, pulsed by the views: R roll (also Start / Play again), B bank, "" none (the computer's turn). */
std::string WolfGreedyLocal::coach() const
{
    if (mState == "finished") return "R";
    if (mTurn != 0) return "";
    if (!mRolled) return "R";
    if (mState == "final") return mPlayers[0].mScore + mTurnPoints > leaderOther(0) ? "B" : "R";
    if (mPlayers[0].mScore + mTurnPoints >= mTarget) return "B";
    if (mDiceLeft == 6) return mTurnPoints >= 1000 ? "B" : "R";
    return mDiceLeft <= 2 || mTurnPoints >= 350 ? "B" : "R";
}

/** What is going on, in one or two plain sentences (Paul: "instructions to what is going on"). */
std::string WolfGreedyLocal::tip() const
{
    const std::string bot(COMPUTER_NAME);
    if (mState == "finished")
    {
        return mWinner == 0 ? "You won! Press PLAY AGAIN for another game, or NEW GAME to play with other people."
                            : bot + " won this time. Press PLAY AGAIN to have another go - you can do it!";
    }
    const bool myTurn = mTurn == 0;
    std::string before;   // what just happened, when the turn has just changed
    if (!mRolled && mEventBy >= 0)
    {
        const bool wasMe = mEventBy == 0;
        if (mEventKind == "bust")
        {
            const bool third = mEvent.compare(0, 10, "Third bust") == 0;
            before = wasMe ? std::string("No scoring dice: that's a bust, so your turn's points are lost. ")
                             + (third ? "That was your third bust, so your score goes back to 0. " : "")
                           : bot + " rolled no scoring dice: a bust" + (mLastPoints > 0 ? ", losing " + number(mLastPoints) : std::string()) + ". ";
        }
        else if (mEventKind == "bank")
        {
            before = wasMe ? "You banked " + number(mLastPoints) + ": it's " + possessive(bot) + " turn - watch the dice. "
                           : bot + " banked " + number(mLastPoints) + ". ";
        }
    }
    if (!myTurn)
    {
        if (!mRolled) return before.empty() ? possessive(bot) + " turn - watch the dice." : before;
        return bot + "'s dice score: " + mWhat + " (" + number(mLastPoints) + "). " + bot + " has " + number(mTurnPoints)
             + " this turn and is thinking...";
    }
    std::string last;
    if (mState == "final") last = "Last turn! Beat " + number(leaderOther(0)) + " to win. ";
    if (!mRolled) return before + last + "Your turn: press ROLL to roll all six dice.";
    if (mEventKind == "lucky")
        return last + "Lucky six! All six scored (" + mWhat + "). ROLL all six again, or BANK " + number(mTurnPoints) + ".";
    // short enough for the HUD's two lines
    std::string t = last + "These dice score: " + mWhat + " (" + number(mLastPoints) + "), kept in gold. ROLL the "
                  + llformat("%d", mDiceLeft) + " left, or BANK " + number(mTurnPoints) + ".";
    if (mDiceLeft <= 2) t += " Few dice left: BANK is safer.";
    return t;
}

// Source: wolfdice_window.php wdw_state - the same fields, so the views need nothing new
LLSD WolfGreedyLocal::reply() const
{
    LLSD g;
    g["id"] = -1;
    g["version"] = mVersion;
    g["state"] = mState;
    g["target"] = mTarget;
    g["practice"] = true;
    g["label"] = "Practice with " + std::string(COMPUTER_NAME);
    g["region"] = "";
    g["host_kind"] = "win";
    g["turn_points"] = mTurnPoints;
    g["dice_left"] = mDiceLeft;
    g["rolled"] = mRolled;
    LLSD dice = LLSD::emptyArray();
    for (S32 d : mDice) dice.append(d);
    g["dice"] = dice;
    g["kept"] = mKept;
    g["event"] = mEvent;
    g["event_kind"] = mEventKind;
    g["status"] = status();
    const bool playing = mState == "playing" || mState == "final";
    g["my_turn"] = playing && mTurn == 0;
    g["in_game"] = true;
    // wolfdice_lib.php wd_buttons: A play again, R roll, B bank once a roll has scored
    g["buttons"] = mState == "finished" ? std::string("A") : (playing && mTurn == 0 ? std::string("R") + (mRolled && mTurnPoints > 0 ? "B" : "") : std::string());
    LLSD players = LLSD::emptyArray();
    for (S32 i = 0; i < (S32)mPlayers.size(); ++i)
    {
        LLSD p;
        p["name"] = i == 1 ? mPlayers[i].mName + " (computer)" : mPlayers[i].mName;
        p["score"] = mPlayers[i].mScore;
        p["busts"] = mPlayers[i].mBusts;
        p["away"] = false;
        p["current"] = playing && i == mTurn;
        p["me"] = i == 0;
        p["winner"] = mState == "finished" && i == mWinner;
        p["via"] = "win";
        players.append(p);
    }
    LLSD out;
    out["ok"] = true;
    out["game"] = g;
    out["players"] = players;
    out["tips"] = LLSD::emptyArray();
    out["tips"].append(tip());
    out["coach"] = coach();
    return out;
}
