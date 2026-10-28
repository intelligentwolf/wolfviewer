/**
 * @file wolffloaterseatunnels.h
 * @brief WolfViewer: Wolf World > Build & Land > Sea Tunnels... - glass tubes with air inside, under the sea.
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

#ifndef WOLF_FLOATERSEATUNNELS_H
#define WOLF_FLOATERSEATUNNELS_H

#include "llfloater.h"
#include "llframetimer.h"

class LLButton;
class LLSpinCtrl;
class LLTextBox;

/**
 * [WOLF SEA TUNNEL 2026-10-28] Ticket 319 (Malachi Coltrane): "An underwater tunnel device that clears the water murkiness
 * ... for vehicle and train traffic. For making the WTG version of the Chunnel." Paul: "yes i do want it".
 * Persona: a builder making a glass rail or road tunnel across the sea bed, who knows the Build window but not shaders.
 * Story: "I make a glass tube piece, stretch and copy it along the sea bed, and inside it is clear air - the trains run dry."
 * Flow: 1. Make a tube piece here (primary): a hollow glass cylinder, Length x Across, lying the way you face, its floor at
 * your feet, already air-filled (description "wolfdry", wolfdryvolumes.h). 2. Move, turn, stretch and Shift-copy it with
 * Build, end to end (overlap a little at bends). Or: select your own cylinders, boxes, spheres or mesh tubes and Fill with
 * air (Make ordinary takes it away). The status line says what happened; failures stay in the caution colour.
 * Wolf Territories only (the menu item is gone elsewhere; opened off Wolf it says so and closes).
 */
class WolfFloaterSeaTunnels : public LLFloater
{
public:
    explicit WolfFloaterSeaTunnels(const LLSD& key);
    ~WolfFloaterSeaTunnels() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    /** Confirmations quietly, failures in the caution colour, kept until the next action. */
    void setStatus(const std::string& text, bool error);

private:
    void onMakePiece();
    /** Add (true) or take away (false) the keyword on every selected prim of a usable shape that you may change. */
    void onSetAir(bool air);
    void updateControls();

    static void makePieceCoro(LLHandle<LLFloater> h, F32 length, F32 across);

    LLSpinCtrl *mLength = nullptr, *mAcross = nullptr;
    LLButton *mMakeBtn = nullptr, *mFillBtn = nullptr, *mPlainBtn = nullptr;
    LLTextBox *mCount = nullptr, *mStatus = nullptr;
    bool mMaking = false;
    LLFrameTimer mRefresh;
};

#endif // WOLF_FLOATERSEATUNNELS_H
