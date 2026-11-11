/**
 * @file wolfgreedysetup.h
 * @brief WolfViewer: "Put a Greedy table here", the Greedy set-up helper window.
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

#ifndef WOLF_GREEDYSETUP_H
#define WOLF_GREEDYSETUP_H

#include <string>
#include <utility>
#include <vector>

#include "llfloater.h"
#include "llhandle.h"
#include "lluuid.h"

class LLRadioGroup;
class LLTextEditor;

// [GREEDY TABLE HERE 2026-11-11] Paul testing W104: "greedy went to the menu, there's no button to rez a table". His rule
// for every game (memory paul-games-need-setup-helper): a set-up helper, "stand there, press Rez" for each piece.
// The window walks the same two steps as the in-world "Greedy - set up box" (lsl/wolfdice_setup.lsl; README s6):
//   1 the TABLE: "Greedy Table" or "Greedy Table (Wolf Grid)" (Jimmy Olsen's table rigged up, README s2b; Paul: he can choose)
//   2 the BOARD (the top 10 and the games now), or Finish
//   3 "Greedy is set up!"
// Each piece: the resident's own copy is found in the inventory (any folder, not in the Trash); when they have none the grid
// gives one into Objects > Greedy (php/wolfdice_give.php, the lift kit's give: wolffloaterlifts.cpp boxCoro) and it is waited
// for; then it is rezzed just in front of them on the surface they stand on (the box's 2.6 m / 2.0 m), and turned to face
// them as the box turns it (wolfdice_setup.lsl rezPiece: yaw from where they look; the table keeps its own upright turn, the
// box's NATURAL_ROT). Wolf Territories only, like every Wolf tool. Opened from the Greedy window and the Greedy HUD.
class WolfFloaterGreedySetup : public LLFloater
{
public:
    WolfFloaterGreedySetup(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    void setStep(S32 step);
    void setMessage(const std::string& text, bool error);
    void onPut();
    void refreshControls();
    static void putCoro(LLHandle<LLFloater> h, U32 gen, S32 piece);

    S32 mStep = 1;                  // 1 the table, 2 the board, 3 done
    bool mBusy = false;             // a piece is on its way: the buttons wait
    U32 mGen = 0;                   // goes up on open and on close: an older press stops touching the window at its next wake-up
    std::vector<std::pair<LLUUID, std::string>> mPlaced;   // the pieces this window has put down, and what each is ("table",
                                                           // "board"): the "already one here" check names the one found
    LLRadioGroup* mWhich = nullptr;
    LLTextEditor* mMessage = nullptr;
};

#endif // WOLF_GREEDYSETUP_H
