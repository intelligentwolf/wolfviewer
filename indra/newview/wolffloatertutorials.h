/**
 * @file wolffloatertutorials.h
 * @brief WolfViewer: Tutorial Videos - "How to ..." cards that open our YouTube tutorials in the web browser (ticket 847).
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
#ifndef WOLF_FLOATERTUTORIALS_H
#define WOLF_FLOATERTUTORIALS_H

#include "llfloater.h"

class LLButton;
class LLPanel;
class LLScrollContainer;
class LLTextBox;

// [TUTORIALS 2026-11-13] Ticket 847 (IndigoQueen King): "How to ..." entries that open the matching Wolf Territories YouTube
// video. The list is our server's (php/wolftutorials.php, from wolftutorials_list.php), read when the window opens and kept
// for the session (fetched again only when over LIST_MAX_AGE_S old); no timer. Watch opens
// https://www.youtube.com/watch?v=<id>, built here from an id checked to be 11 characters of [A-Za-z0-9_-], never a link from
// the reply. Design: region-terrain-work/weather-props-tutorials-2026-11-13/DESIGN.md s2. Wolf Territories only.
class WolfFloaterTutorials : public LLFloater
{
public:
    WolfFloaterTutorials(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    /** Is this a YouTube video id (11 characters of A-Z a-z 0-9 _ -)? */
    static bool validId(const std::string& id);

private:
    static void fetch();
    static void fetchCoro();
    void refreshView();
    void buildCards();

    LLScrollContainer* mScroll = nullptr;
    LLPanel* mCards = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mRetry = nullptr;
    F64 mBuiltFor = -1.0;   // the list (by when it arrived) the cards were built from
};

#endif // WOLF_FLOATERTUTORIALS_H
