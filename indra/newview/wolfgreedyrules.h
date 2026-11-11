/**
 * @file wolfgreedyrules.h
 * @brief WolfViewer: Greedy's scoring and the computer player's choice, for the practice game played in the viewer.
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

#ifndef WOLF_GREEDYRULES_H
#define WOLF_GREEDYRULES_H

// [GREEDY COMPUTER PLAYER 2026-11-11] Paul: "the greedy practice game: can we have a 'greedy player' that plays against you as
// well, rather than just you playing and getting stuck". Practice against the computer is played in the viewer (wolffloatergreedy
// .cpp WolfGreedyGame, the local game): it is never sent to the dice server, so it can never touch a score or the top 10.
// Its scoring is the server's, line for line, so the two always agree:
//   Source: region-terrain-work/greedy-dice-2026-11-10/server/wolfdice_lib.php wd_kind_points, wd_best_counts, wd_score
// and test/rules_compare.sh checks every roll of 1 to 6 dice against the PHP (the same points and the same dice set aside).
// Standard C++ only (no viewer headers), so that test can build it on its own.

#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

namespace WolfGreedyRules
{
    /** wd_kind_points: three of a kind, 1s = 1000, others face x 100; each further matching die doubles it. */
    inline int kindPoints(int face, int n)
    {
        const int base = face == 1 ? 1000 : face * 100;
        return base * (1 << (n - 3));
    }

    struct Best
    {
        int points = 0;
        int dice = 0;
        std::array<int, 7> used{};   // [1..6]: how many of each face are set aside
    };

    /** wd_best_counts: the best split of a count vector c[1..6]; whole = these are all six dice of one roll. */
    inline Best bestCounts(const std::array<int, 7>& c, bool whole, std::map<std::pair<std::array<int, 7>, bool>, Best>& memo)
    {
        const auto key = std::make_pair(c, whole);
        auto hit = memo.find(key);
        if (hit != memo.end()) return hit->second;
        Best best;
        auto tryUse = [&](int pts, const std::array<int, 7>& use)
        {
            std::array<int, 7> rest = c;
            int n = 0;
            for (int f = 1; f <= 6; ++f) { rest[f] -= use[f]; n += use[f]; }
            const Best sub = bestCounts(rest, false, memo);
            const int total = pts + sub.points;
            const int dice = n + sub.dice;
            if (total > best.points || (total == best.points && dice > best.dice))
            {
                Best b = sub;
                for (int f = 1; f <= 6; ++f) b.used[f] += use[f];
                b.points = total;
                b.dice = dice;
                best = b;
            }
        };
        auto one = [](int face, int n) { std::array<int, 7> u{}; u[face] = n; return u; };
        if (whole)
        {
            // the non-zero counts, sorted (PHP: array_values(array_filter($c)), sort)
            std::vector<int> vals;
            for (int f = 1; f <= 6; ++f) if (c[f]) vals.push_back(c[f]);
            std::sort(vals.begin(), vals.end());
            const std::array<int, 7> all{ 0, 1, 1, 1, 1, 1, 1 };
            if (vals == std::vector<int>{ 1, 1, 1, 1, 1, 1 }) tryUse(1500, all);                  // full straight
            if (vals == std::vector<int>{ 2, 2, 2 }) tryUse(1500, c);                             // three pairs
            if (vals == std::vector<int>{ 2, 4 }) tryUse(c[1] == 4 ? 2500 : 2000, c);             // four of a kind + pair
            if (vals == std::vector<int>{ 3, 3 }) tryUse(2000, c);                                // double triple
        }
        if (c[1] >= 1 && c[2] >= 1 && c[3] >= 1 && c[4] >= 1 && c[5] >= 1) tryUse(1000, { 0, 1, 1, 1, 1, 1, 0 });   // 1-5
        if (c[2] >= 1 && c[3] >= 1 && c[4] >= 1 && c[5] >= 1 && c[6] >= 1) tryUse(1000, { 0, 0, 1, 1, 1, 1, 1 });   // 2-6
        for (int f = 1; f <= 6; ++f)
        {
            for (int n = 3; n <= c[f]; ++n) tryUse(kindPoints(f, n), one(f, n));
        }
        if (c[1] >= 1) tryUse(100, one(1, 1));
        if (c[5] >= 1) tryUse(50, one(5, 1));
        memo[key] = best;
        return best;
    }

    /** wd_score: the points of a roll and which dice are set aside (used[i] for dice[i]; the first of each face first). */
    inline int score(const std::vector<int>& dice, std::vector<bool>& used)
    {
        std::array<int, 7> c{};
        for (int d : dice) if (d >= 1 && d <= 6) c[d]++;
        std::map<std::pair<std::array<int, 7>, bool>, Best> memo;
        Best b = bestCounts(c, dice.size() == 6, memo);
        used.assign(dice.size(), false);
        for (size_t i = 0; i < dice.size(); ++i)
        {
            const int d = dice[i];
            if (d >= 1 && d <= 6 && b.used[d] > 0) { used[i] = true; b.used[d]--; }
        }
        return b.points;
    }

    /**
     * The computer player's choice after a roll that scored: bank (true) or roll again (false). CHOSEN to be sensible and
     * beatable (Paul: "keep it beatable"): bank at about 350 in a turn, sooner with few dice left, later when far behind; always
     * bank when that reaches the target; in the last turns, keep going until it is ahead of the leader. `wobble` (-50..50, the
     * caller's random) keeps it from playing the same way every time.
     */
    inline bool computerBanks(int turn, int diceLeft, int score, int bestOther, int target, bool lastTurn, int wobble)
    {
        if (lastTurn) return score + turn > bestOther;                  // a last turn is lost unless it ends ahead
        if (score + turn >= target) return true;                         // that wins the race to the target
        const bool farBehind = bestOther - score >= 1000;
        int enough = (farBehind ? 550 : 350) + wobble;
        if (diceLeft == 6) enough = farBehind ? 1500 : 1000;            // a lucky six: all six again is the best chance there is
        else if (diceLeft <= 2) enough = farBehind ? 300 : 200;          // one or two dice: a bust is likely
        return turn >= enough;
    }
}

#endif // WOLF_GREEDYRULES_H
