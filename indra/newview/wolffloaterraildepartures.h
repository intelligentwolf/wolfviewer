/**
 * @file wolffloaterraildepartures.h
 * @brief WolfViewer: Railway Departures - a station's departures and arrivals from its railway's ONE timetable (wolf-rail brain,
 *        region-terrain-work/wolf-rail-brain-2026-11-20/DESIGN.md 3.3a).
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
#ifndef WOLF_FLOATERRAILDEPARTURES_H
#define WOLF_FLOATERRAILDEPARTURES_H

#include "llfloater.h"
#include "lltimer.h"
#include "lluuid.h"

class LLComboBox;
class LLScrollListCtrl;
class LLTextBox;

class WolfFloaterRailDepartures : public LLFloater
{
public:
    WolfFloaterRailDepartures(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    /// Source: DESIGN.md 3.3a - read only while open, again at most every READ_S (the page's own cache is 5 s).
    static constexpr F32 READ_S = 10.f;
    static constexpr F32 TIMEOUT_S = 15.f;

private:
    void read();
    void findHere();
    static void readCoro(LLHandle<LLFloater> h, std::string url);
    static void hereCoro(LLHandle<LLFloater> h, std::string url);
    void onHere(S32 status, const LLSD& reply);
    void choose(const std::string& network);
    void fillRailways();
    static std::vector<std::string> recent();
    void onReply(S32 status, const LLSD& reply, const std::string& error);
    void fill(const LLSD& reply);
    void setStatus(const std::string& text, bool problem);

    LLComboBox* mNetwork = nullptr;
    LLComboBox* mStation = nullptr;
    LLScrollListCtrl* mDepartures = nullptr;
    LLScrollListCtrl* mArrivals = nullptr;
    LLTextBox* mTitle = nullptr;
    LLTextBox* mStatus = nullptr;
    LLFrameTimer mSince;
    bool mBusy = false;
    std::string mTag;           // the slice last shown: an unchanged answer redraws nothing
    std::string mStationsKey;   // the station list last put in the combo
    LLSD mHere;                 // the railways with a station in the avatar's region: [{network, title, stations}]
    LLUUID mHereRegion;         // the region mHere is for (asked again when the avatar is in another)
    bool mHereFailed = false;   // the railways here could not be read: asked again every READ_S, and its message kept until then
    bool mProblem = false;
    bool mInputProblem = false; // the problem shown is the resident's own typing: kept until they pick or type again      // the status line says a problem: kept until the next good read (never replaced by a hint)
};

#endif // WOLF_FLOATERRAILDEPARTURES_H
