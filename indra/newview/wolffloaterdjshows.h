/**
 * @file wolffloaterdjshows.h
 * @brief WolfViewer Wolf DJ "My Shows": the DJ's own listener history (2026-10-16).
 *
 * Paul: "can we have that somewhere in the viewer?" - after his 10-16 show read "2 listeners"
 * with 15 people listening. Every past show of the signed-in DJ: when, how many different people
 * listened, the most at once, and how (their own stream or WTGR; in-world or web). The numbers are
 * recorded on the website (gridmanager/radioshowlib.php) and fetched from radioshows.php, which
 * answers only for the signed-in avatar. Wolf Territories only.
 */

#ifndef WOLF_FLOATERDJSHOWS_H
#define WOLF_FLOATERDJSHOWS_H

#include "llfloater.h"
#include "lluictrl.h"

#include <string>
#include <vector>

class LLButton;
class LLTextBox;

// One show as radioshows.php sends it (radioshowlib.php rshShowsFor).
struct WolfDJShow
{
    std::string mDate;      // "Fri 16 Oct 2026", grid time
    std::string mTime;      // "09:53–11:02 SLT"
    std::string mLength;    // "68 min"
    std::string mPeakAt;    // "10:28 SLT" or ""
    bool mLive = false;
    bool mWtgrKnown = true;
    S32 mListeners = 0;
    S32 mPeak = 0;
    S32 mDirect = 0;
    S32 mWtgr = 0;
    S32 mInworld = 0;
    S32 mWeb = 0;
    S32 mOther = 0;
};

// The table: drawn and scrolled by this one control (the wolfdjdesk.cpp pattern).
class WolfDJShowsTable : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfDJShowsTable(const Params& p);

    enum EState { LOADING, READY, FAILED };
    void setShows(const std::vector<WolfDJShow>& shows, S32 busiest);
    void setState(EState state) { mState = state; }

    void draw() override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;

private:
    S32 contentHeight() const;
    void scrollBy(S32 px);

    std::vector<WolfDJShow> mShows;
    S32 mBusiest = -1;
    S32 mScroll = 0;        // pixels scrolled down from the first row
    EState mState = LOADING;
};

class WolfFloaterDJShows : public LLFloater
{
public:
    WolfFloaterDJShows(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void load();
    static void loadCoro(LLHandle<LLFloater> handle);
    void showError(const std::string& msg);

    WolfDJShowsTable* mTable = nullptr;
    LLTextBox* mSummary = nullptr;
    LLTextBox* mNote = nullptr;
    LLButton* mRetry = nullptr;
    bool mBusy = false;
};

#endif // WOLF_FLOATERDJSHOWS_H
