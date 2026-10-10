/**
 * @file wolffloaterairports.h
 * @brief WolfViewer: the Airports window - find a Wolf Territories airport by name and fly there.
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

#ifndef WOLF_FLOATERAIRPORTS_H
#define WOLF_FLOATERAIRPORTS_H

#include "llfloater.h"

class LLButton;
class LLScrollListCtrl;
class LLSearchEditor;
class LLTextBox;

// [AIRPORTS 2026-10-10] Paul: "users should be able to search for airports ... by airport name ... type input
// airport name or paste from clipboard, it looks up the airport in our database and then gets them to the right
// place on the region", "we dont teleport we fly with autopilot or follow the route", "the airports button on the
// aircraft hud should have its own floater for search from airports not open web browser".
//
// The list is WolfAirports (the grid's airports directory, airportsapi.php), searched by WolfAirports::search;
// with nothing typed, every airport, nearest first. Fly there is WolfFlight::flyToAirport: that airport as the
// destination and the route engaged; the autoland finds its runway on arrival.
class WolfFloaterAirports : public LLFloater
{
public:
    WolfFloaterAirports(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void fill();
    void updateButtons();
    void onFly();
    void onShowMap();

    LLSearchEditor* mSearch = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mSummary = nullptr;
    LLButton* mFly = nullptr;
    LLButton* mShowMap = nullptr;
    bool mFilledLoaded = false;     // the list was filled from a loaded directory (else: wait for it)
};

#endif // WOLF_FLOATERAIRPORTS_H
