/**
 * @file wolffloaterweatherreport.h
 * @brief WolfViewer: Jimmy's Weather Report - the region's real weather and which of Jimmy Olsen's
 *        skies the automatic environment is showing, as his weather controller reports it.
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

#ifndef WOLF_FLOATERWEATHERREPORT_H
#define WOLF_FLOATERWEATHERREPORT_H

#include "llfloater.h"

class LLTextBox;

// [JIMMY V9C 2026-10-10] Paul, after Jimmy showed his master and slave scripts' reports ("Weather for
// Tromso, Norway: Overcast clouds 2.00 C ... Elev: 3 deg | solar time 8.061h -> Using:
// PCLOUDY_E02_AM_NH"): "add a thing somewhere that shows this information maybe in the environment
// sub menu". The same lines for the region you are on: the place, the conditions (Jimmy's words for
// his weather id), temperature, humidity, pressure, wind, the place's local time; then the sun's
// height, the solar time and the sky in use (the region's day-cycle frame for now, which the region
// names after Jimmy's sky - WolfAutoEnvironmentModule.cs), and the aurora level. Wolf only (the data
// is Wolf's own site); refreshed every 30 s while open.
class WolfFloaterWeatherReport : public LLFloater
{
public:
    WolfFloaterWeatherReport(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void fetch();
    static void fetchCoro(LLHandle<LLFloater> handle, std::string region_id);
    void show(const LLSD& reply);
    void showSky();

    LLTextBox* mReport = nullptr;
    LLTextBox* mSky = nullptr;
    LLTextBox* mStatus = nullptr;
    LLSD mReply;
    std::string mFetchedFor;
    F64 mNextFetch = 0.0;
    bool mBusy = false;
};

#endif // WOLF_FLOATERWEATHERREPORT_H
