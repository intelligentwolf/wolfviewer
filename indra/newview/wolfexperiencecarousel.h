/**
 * @file wolfexperiencecarousel.h
 * @brief WolfViewer: Wolf Experiences' 3D carousel - category pills, the ring of cards, details, on the map's sky.
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

#ifndef WOLF_EXPERIENCECAROUSEL_H
#define WOLF_EXPERIENCECAROUSEL_H

#include <functional>
#include <string>
#include <vector>

#include "lluictrl.h"
#include "lluiimage.h"
#include "lluuid.h"

class LLViewerTexture;

/**
 * [2026-11-21] Paul: "the Experiences floater: I'd like something far more exciting, with a 3D scrolling carousel. You
 * click on a category and then spin the carousel to see the entries, with a space background like the map."
 * The same design as WolfStorm's (js/ui/floaters/floater_wolf_experiences.js ExperienceRing, 2026-11-21).
 *
 * Who and what: Jean, 64, UI Size at its largest, has just landed at Wolf Territories Home; she presses "Explore new
 * worlds", spins the ring with the big arrows until Europa is at the front, reads what it is and presses Visit.
 * Flow: a category pill -> the ring fills (dealt in with a short spin) -> spin (drag, wheel, arrow keys, the big
 * round arrows) -> the caption under the ring names the front card -> Visit, or Details (the panel: picture, what
 * it is, "When you arrive", Visit / Back) -> the teleport (WolfFloaterExperiences::onGo).
 *
 * One control covering the whole Wolf Experiences window (floater_wolf_experiences.xml "carousel"), custom-drawn in
 * the flight deck family (WolfGameDraw, the wolves palette): behind everything the map's sky (wolfspacesky.h,
 * painted ONCE per size into one texture: a frame costs one quad for it), then the pills, the ring and the caption.
 * The window's own widgets (the Experiences / What's on buttons, the status line, the footer) sit on top of it.
 * [2026-11-22] What's on fills it too (see below); with no list (loading, failed, nothing on) only the sky shows.
 *
 * The ring: `mPos` is its turn in cards (never wrapped, so the spin and the sky's drift stay continuous), `mTarget`
 * the whole card it eases to. Only cards within VISIBLE_SLOTS of the front are drawn, and a card's picture is asked
 * for the first time it comes into view. Each card is a true perspective quad (its corners turned and projected as
 * WolfStorm's CSS 3D does), the picture drawn in strips so it does not shear.
 *
 * Keyboard (the control takes focus when the window opens): Left / Right spin (Up / Down too), Home / End,
 * Page Up / Down three cards, Enter or Space opens the details; Tab moves pills -> ring -> Details -> Visit, then on
 * out of the control; on the pills Left / Right choose which, Enter picks it. In the details Tab / Left / Right move
 * between Back and Visit, Escape goes back. Reduce motion (WolfAdventureReduceMotion, the viewer's Wolf
 * reduce-motion setting): every move jumps straight to the card, no deal-in, the sky does not drift.
 *
 * [2026-11-22] Paul: "The events part of the Experiences floater: make it like the experiences rather than a long list,
 * it's disappointing." What's on fills this same control (WolfFloaterExperiences::fillEventsCarousel): an entry with
 * mEvent set is an event's card - a shorter picture (or a calendar page for its day), a LIVE NOW / Starting soon badge,
 * its time line under the title; the caption says when and where; the details show the time in full and the
 * description. Events keep the window's order (soonest first). An entry can stand under more than one pill (mTags:
 * an event is in "Today" and in "Music"). The same as WolfStorm's What's on (floater_wolf_experiences.js, 2026-11-22).
 */
class WolfExperienceCarousel : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    struct Category
    {
        std::string mId;        // "" = All
        std::string mTitle;
        std::string mIcon;      // a UI image name
        S32 mCount = 0;
    };
    struct Entry
    {
        std::string mId, mTitle, mRegion, mBy, mBlurb, mArrive, mCategory, mIcon;
        LLUUID mImage;          // null = draw mIcon (an event: its calendar page)
        bool mFeatured = false;
        LLPointer<LLUIImage> mPicture;   // asked for the first time the card is in view
        // [2026-11-22] More pills it stands under besides mCategory (What's on: "now", "today", "cat:music", ...).
        std::vector<std::string> mTags;
        // [2026-11-22] An event's card (What's on). The words are made by the window and refreshed every 30 s.
        bool mEvent = false;
        std::string mBadge;             // "LIVE NOW" / "Starting soon" / "" (none)
        bool mBadgeLive = false;        // the LIVE NOW red, else amber
        std::string mWhen;              // the card's time line: "Starts in 1 h 20 min" / "On now, until 22:00" / "Sat 28 Nov 20:00"
        std::string mWhenLong;          // the caption's: "Today 20:00 your time (12:00 SLT)"
        std::string mWhenMore;          // the details' second line: "Starts in 1 h 20 min  ·  Lasts about 2 hours"
        std::string mKicker;            // the details' amber line: "LIVE NOW  ·  Music"
        std::string mDateTop, mDateDay, mDateMonth;   // the calendar page: "SAT", "28", "Nov"
        std::string mVisitLabel = "Visit";   // the button's word ("Go", "Open page"); empty = no button
        std::string mVisitTip;          // its tooltip ("" = "Teleport to <region>")
    };

    WolfExperienceCarousel(const Params& p);
    ~WolfExperienceCarousel() override;

    /** The pills (All first) and every entry, in the order the ring shows them. The pill with id `keep_category` is
     *  chosen (else All); when that is the pill already showing, the ring stays at the card it was at (a refresh). */
    void setData(const std::vector<Category>& categories, const std::vector<Entry>& entries, const std::string& keep_category);
    /** [2026-11-22] The chosen pill's id ("" = All), for the window to keep per section. */
    std::string category() const;
    /** [2026-11-22] The pills' count word: "place" / "places" (Experiences), "event" / "events" (What's on). */
    void setCountWords(const std::string& one, const std::string& many) { mCountOne = one; mCountMany = many; }
    /** No list (loading, failed, nothing on, another grid): sky only, the window's status line says why. */
    void clearData();
    /** [2026-11-22] It holds a list (setData since the last clearData). */
    bool hasData() const { return mHaveData; }

    std::function<void(const std::string& id)> mOnVisit;

    bool detailOpen() const { return mDetail >= 0; }

    /** What it shows, for LEAP's UI getValue (tests read the state this way; nothing sets it): the chosen category's
     *  id and title, how many cards, the front card's index and title, and whether the details are open. */
    LLSD getValue() const override;

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onMouseCaptureLost() override;
    void onFocusReceived() override;

private:
    enum EHit { H_NONE = 0, H_STAGE, H_PILL, H_PILL_LEFT, H_PILL_RIGHT, H_PREV, H_NEXT, H_CARD, H_DETAILS, H_VISIT,
                H_D_BACK, H_D_VISIT, H_D_PANEL, H_D_SCRIM };
    enum EZone { Z_PILLS = 0, Z_RING, Z_DETAILS, Z_VISIT };
    struct Hit { F32 mL, mB, mR, mT; EHit mId; S32 mIndex; std::string mTip; };

    void choose(S32 pill);
    void step(S32 delta);
    void goTo(S32 index);
    S32 count() const { return (S32)mShown.size(); }
    S32 frontIndex() const;     // the card the ring is at or easing to
    void openDetail(S32 index);
    void closeDetail();
    void visit(S32 index);
    bool reduceMotion() const;
    void animate();

    void layout();
    void drawSky();
    void drawPills();
    void drawRing();
    void drawCaption();
    void drawDetail();
    void drawCalendarPage(const Entry& e, F32 cx, F32 cy, F32 size, F32 fade);   // [2026-11-22] an event without a picture
    std::string countText(S32 n) const;
    void addHit(F32 l, F32 b, F32 r, F32 t, EHit id, S32 index = -1, const std::string& tip = std::string());
    const Hit* hitAt(S32 x, S32 y) const;

    std::vector<Category> mCategories;
    std::vector<Entry> mEntries;
    std::vector<S32> mShown;            // indexes into mEntries on the ring
    S32 mPill = 0;                      // the chosen category pill
    bool mHaveData = false;
    std::string mCountOne = "place", mCountMany = "places";

    F32 mPos = 0.f, mTarget = 0.f;
    F64 mLast = 0.0;
    bool mMoving = false;

    // A press: where, the ring's turn then, what was under it, and the last few positions for a flick.
    bool mDragging = false;
    bool mDragMoved = false;
    S32 mDownX = 0;
    F32 mDownPos = 0.f;
    EHit mDownHit = H_NONE;
    S32 mDownIndex = -1;
    struct Sample { S32 x; F64 t; };
    std::vector<Sample> mSamples;

    EZone mZone = Z_RING;
    S32 mPillFocus = 0;
    S32 mDetail = -1;                   // the card whose details are open
    bool mDetailVisitFocus = true;      // Visit (else Back) has the details' focus

    F32 mPillScroll = 0.f, mPillScrollMax = 0.f;
    bool mPillFollow = false;           // the keyboard moved to a pill: scroll it into view at the next draw
    S32 mHoverHit = H_NONE, mHoverIndex = -1;
    std::vector<Hit> mHits;

    // This frame's layout (view-local, y up).
    F32 mStageB = 0.f, mStageT = 0.f, mCapB = 0.f, mCapT = 0.f, mPillB = 0.f, mPillT = 0.f;
    F32 mCardW = 0.f, mCardH = 0.f;
};

#endif // WOLF_EXPERIENCECAROUSEL_H
