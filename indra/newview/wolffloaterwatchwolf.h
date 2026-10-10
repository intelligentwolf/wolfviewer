/**
 * @file wolffloaterwatchwolf.h
 * @brief WolfViewer: WatchWolf - who is on the regions you own or manage, with IM, profile, eject
 *        and ban (Wolf Territories only).
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

#ifndef WOLF_FLOATERWATCHWOLF_H
#define WOLF_FLOATERWATCHWOLF_H

#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;

// [WATCHWOLF 2026-10-10] Paul: "alpha wolf knows where people are allow region owners to get a list of
// people who are on their regions with a sortable list box ... i have hundreds of regions so maybe we
// show paged ... make sure it doesn't crash the viewer", "this is for wolf only", "allow me to
// remotely ban, eject and im people in that list", "and allow sorting", "i can see any regions that
// i'm an estate manager for", "default no npcs, but have an option to show it", "call it watchwolf
// ... and use a wolf icon".
//
// The site does the work (wolfstorm/php/watchwolf.php): which regions you hold (god, owner, estate
// owner or manager), who AlphaWolf sees on them, sorted, filtered and cut to ONE page (10-100 rows) -
// so this window never holds more than a page however many regions there are. A click on a column
// heading sorts the whole list on the site; Previous / Next page through it. Refreshed every 20 s
// while open.
class WolfFloaterWatchWolf : public LLFloater
{
public:
    WolfFloaterWatchWolf(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void fetch();
    static void fetchCoro(LLHandle<LLFloater> handle, std::string query, U64 serial);
    static void actionCoro(LLHandle<LLFloater> handle, std::string action, LLUUID avatar, LLUUID region, std::string name);
    void show(const LLSD& reply);
    void onSortChanged();
    void onSelection();
    void act(const std::string& action);
    LLSD selectedRow() const;
    void setStatus(const std::string& text, bool error);

    LLScrollListCtrl* mList = nullptr;
    LLLineEditor* mFilter = nullptr;
    LLCheckBoxCtrl* mNpc = nullptr;
    LLCheckBoxCtrl* mMine = nullptr;    // gods only: their own regions, not the whole grid
    LLComboBox* mPer = nullptr;
    LLButton* mPrev = nullptr;
    LLButton* mNext = nullptr;
    LLTextBox* mPageText = nullptr;
    LLTextBox* mSummary = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mIm = nullptr;
    LLButton* mProfile = nullptr;
    LLButton* mEject = nullptr;
    LLButton* mBan = nullptr;

    LLSD mRows;                 // this page, as the site sent it
    S32 mPage = 1, mPages = 1;
    std::string mSort = "name";
    bool mDesc = false;
    U64 mSerial = 0;            // the newest request; older answers are dropped
    bool mBusy = false;
    F64 mNextFetch = 0.0;
};

#endif // WOLF_FLOATERWATCHWOLF_H
