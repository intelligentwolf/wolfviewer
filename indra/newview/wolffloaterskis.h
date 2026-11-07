/**
 * @file wolffloaterskis.h
 * [SKIS 2026-11-06] Wolf World > Skis and Snowboards... - pick skis or a snowboard, put them on, take them off (ticket 556).
 * [SLEDGES 2026-11-07] Now Wolf World > Winter Fun...: skis, a snowboard, a sledge or a snow disc, with how to ride the one
 * chosen (ticket 556, Indi Moon).
 * See wolfskis.h and floater_wolf_skis.xml. Wolf Territories only: opened elsewhere it says so and closes.
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
#ifndef WOLF_FLOATER_SKIS_H
#define WOLF_FLOATER_SKIS_H

#include "llfloater.h"
#include "lluuid.h"
#include "wolfskis.h"

class LLButton;
class LLTextBox;

class WolfFloaterSkis : public LLFloater
{
public:
    WolfFloaterSkis(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void pick(WolfSkis::EKind kind);
    void onPutOn();
    void onTakeOff();
    void onShowHud();
    void say(const std::string& text);

    // [Wolf 2026-11-06] The free skis on first open (Paul 2026-11-06: "if the folder is not in their inventory give them
    // what they need"): once per session the window asks php/wolfskis.php, which gives what is missing and sends the
    // standard "has given you" offer; a resident who has both hears nothing.
    void askForSkis();
    static void giveCoro(LLHandle<LLFloater> h);
    void onGiveReply(S32 status, bool ok, const LLSD& reply, const std::string& transport);

    // [SLEDGES 2026-11-07] the four choices, in WolfSkis' order: skis, snowboard, sledge, snow disc
    static constexpr S32 NUM_KINDS = 4;
    LLButton* mKindBtn[NUM_KINDS] = { nullptr, nullptr, nullptr, nullptr };
    LLTextBox* mHowText = nullptr;
    LLButton* mPutOnBtn = nullptr;
    LLButton* mTakeOffBtn = nullptr;
    LLButton* mShowHudBtn = nullptr;
    LLTextBox* mStatus = nullptr;
    WolfSkis::EKind mKind = WolfSkis::KIND_SKIS;
    bool mWasPending = false;
    bool mAsking = false;                 // the give request is out: Put on waits for it
    static LLUUID sAskedSession;          // the session the give was answered for (asked once per session)
};

#endif
