/**
 * @file wolffloaterexperiences.h
 * @brief WolfViewer: Experiences - the welcome chooser (Wolf Territories only).
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

#ifndef WOLF_FLOATEREXPERIENCES_H
#define WOLF_FLOATEREXPERIENCES_H

#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLIconCtrl;
class LLLineEditor;
class LLPanel;
class LLScrollContainer;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextureCtrl;

// [WolfViewer 2026-10-14] Paul: "we need to build experiences so when someone lands at welcome they
// choose experiences". Contract: wolfstorm/tools/EXPERIENCES_V1.md.
//
// Who and what: a resident who has just signed up lands in the Wolf Territories Home welcome region
// and, within a minute, picks something to do and is taken straight there, with a one-line "what to
// do when you arrive".
//
// Flow: the window opens by itself right after login for a newly signed-up resident (the site's
// "newAccount", once per login; Paul: "when a user first logs in they should SHOW experiences when
// they have just signed up"), or any time from Wolf World > Experiences... / the toolbar button -> pick a card (optionally narrow by category)
// -> Go -> teleport -> on arrival, a notice with the card's "arrive" line.
//
// The list is https://wolfstorm.app/php/experiences.php (GET with X-Wolf-Agent/X-Wolf-Session so the
// site can say newAccount; the list itself is public), so Paul can change it without a viewer release.
namespace WolfExperiences
{
    // Every frame (llappviewer.cpp idle, next to WolfInvisible::idle): opens the chooser once after
    // login for a new account, and shows the "arrive" notice after a Go teleport.
    // [WolfViewer 2026-10-14] Also keeps What's on's countdowns current (WolfFloaterExperiences::tick).
    void idle();
}

class WolfFloaterExperiences : public LLFloater
{
public:
    WolfFloaterExperiences(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    bool handleKeyHere(KEY key, MASK mask) override;   // Escape closes

    // The shared list was fetched (or the fetch failed): redraw the open window, if any.
    static void dataChanged();

    // [WolfViewer 2026-10-14] What's on (EVENTS_V1.md): the events feed was fetched (or failed).
    static void eventsChanged();
    // Called from WolfExperiences::idle every frame: while What's on is on screen, rewrites each card's
    // "LIVE NOW" / "Starts in ..." line every 30 s.
    static void tick();

private:
    void load(bool force);
    void refreshView();
    void buildChips(const LLSD& categories);
    void buildCards();
    void onGo(const std::string& id);
    void setStatus(const std::string& text, bool error, bool offer_retry);

    // [WolfViewer 2026-10-14] What's on: the toggle at the top switches the window between the two.
    void showWhatsOn(bool whats_on);
    void placeScroll(S32 top);
    void clearCards();
    void buildEventCards();
    void updateEventTimes();
    void onEventGo(const std::string& id);

    bool mWhatsOn = false;              // the What's on section is showing
    LLButton* mModeExperiences = nullptr;
    LLButton* mModeWhatsOn = nullptr;
    LLTextBox* mSubtitle = nullptr;
    std::string mSubtitleExperiences;   // the layout's subtitle, for the Experiences section
    struct EventTime { LLTextBox* mWhen; S64 mStarts; S64 mEnds; };
    std::vector<EventTime> mEventTimes; // the shown event cards' time lines (children of mCards)
    F64 mLastTick = 0.0;

    LLButton* mAddBtn = nullptr;        // [WolfViewer 2026-10-14] "Add your experience" (builder tools)
    LLScrollContainer* mScroll = nullptr;
    LLPanel* mCards = nullptr;          // made in code: the scrolled view that holds the cards
    LLTextBox* mStatus = nullptr;
    LLButton* mRetry = nullptr;
    LLCheckBoxCtrl* mDontShow = nullptr;
    std::vector<LLButton*> mChips;      // cat_0 ("All") .. cat_6
    std::vector<std::string> mChipIds;  // the category id each chip shows ("" = All)
    std::string mCategory;              // "" = All
    // [WolfViewer 2026-10-14] where the chip rows start and where the card list ends, as the layout
    // places them (buildChips wraps the chips and stretches the list up to the last row)
    S32 mChipLeft0 = 12;
    S32 mChipTop0 = 0;
    S32 mScrollBottom = 0;
};

// [WolfViewer 2026-10-14] Builder tools (EXPERIENCES_V1.md "Builder tools - Add your experience (v1.1)";
// Paul: "GIVE OUR BUILDERS EXPERIENCE TOOLS IN OUR VIEWERS").
//
// Who and what: a Wolf Territories builder who made something worth visiting stands where people should
// arrive, fills in a short card and sends it; once a grid admin approves it, new residents see it.
//
// Flow: Wolf Experiences > "Add your experience" -> this window, Add section, filled from where they stand
// -> title / description / when you arrive / category / picture -> "Submit for review" -> "My experiences"
// shows it waiting, live or not approved (with the admin's note), and can withdraw it. Admins (only when
// the site answers GET ?review=1 - the server decides, never this viewer) also get "Waiting for review":
// Go to visit, then Approve or Reject with a note.
//
// One floater with three sections switched by the buttons along the top, not a tab container: every
// control sits straight on the floater so Escape reaches it and closes it (LLPanel::handleKeyHere eats
// Escape, llpanel.cpp:271-281, and a tab's panel would sit between the controls and the floater).
class WolfFloaterExperienceSubmit : public LLFloater
{
public:
    WolfFloaterExperienceSubmit(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    bool handleKeyHere(KEY key, MASK mask) override;   // Escape closes

    // The chooser's list (and so its categories) changed.
    static void dataChanged();

    // [WolfViewer 2026-10-14] "Upload a picture..." finished: the new texture's asset id, or why not.
    // Called by the upload (wolffloaterexperiences.cpp WolfExperiencePictureUpload) on the main thread.
    void pictureUploaded(const LLUUID& asset_id, const std::string& error);

private:
    enum ESection { SECTION_ADD, SECTION_MINE, SECTION_REVIEW };

    void showSection(ESection section);
    void fillCategories();
    void useWhereIStand();
    void onUploadPicture();     // [WolfViewer 2026-10-14] Paul: "users should be able to upload their own texture"
    void uploadPictureFile(const std::string& filename);
    void onSubmit();
    void loadMine();
    void loadReview();
    void showMine();
    void showReview();
    void onMineSelect();
    void onReviewSelect();
    void onWithdraw();
    void onReview(bool approve);
    void updateButtons();
    void setStatus(LLTextBox* box, const std::string& text, bool error);

    // The coroutines' answers (only when this window still exists: they hold its handle).
    void submitDone(bool ok, const std::string& message);
    void mineLoaded(bool ok, const LLSD& submissions, const std::string& error);
    void reviewLoaded(bool ok, const LLSD& submissions, const std::string& error);
    void actionDone(ESection section, bool ok, const std::string& message);

    ESection mSection = SECTION_ADD;
    bool mAdmin = false;            // the site answered GET ?review=1 for this login
    bool mSubmitting = false;
    bool mMineBusy = false;
    bool mReviewBusy = false;
    bool mActionBusy = false;       // a withdraw or a review decision in flight
    bool mUploading = false;        // [WolfViewer 2026-10-14] a picture upload in flight (picker or upload)
    LLUUID mDefaultPicture;         // the parcel snapshot the picture was last filled with
    LLSD mMine = LLSD::emptyArray();
    LLSD mReview = LLSD::emptyArray();

    LLButton* mSecAdd = nullptr;
    LLButton* mSecMine = nullptr;
    LLButton* mSecReview = nullptr;

    LLLineEditor* mRegion = nullptr;
    LLSpinCtrl* mX = nullptr;
    LLSpinCtrl* mY = nullptr;
    LLSpinCtrl* mZ = nullptr;
    LLLineEditor* mTitle = nullptr;
    LLLineEditor* mBlurb = nullptr;
    LLLineEditor* mArrive = nullptr;
    LLComboBox* mCategory = nullptr;
    LLTextureCtrl* mPicture = nullptr;
    LLButton* mUploadBtn = nullptr; // [WolfViewer 2026-10-14] "Upload a picture..."
    LLTextBox* mAddStatus = nullptr;
    LLButton* mSubmit = nullptr;

    LLScrollListCtrl* mMineList = nullptr;
    LLTextBox* mMineDetail = nullptr;
    LLTextBox* mMineStatus = nullptr;
    LLButton* mWithdraw = nullptr;

    LLScrollListCtrl* mReviewList = nullptr;
    LLIconCtrl* mReviewPicture = nullptr;
    LLTextBox* mReviewDetail = nullptr;
    LLLineEditor* mReviewNote = nullptr;
    LLTextBox* mReviewStatus = nullptr;
    LLButton* mReviewGo = nullptr;
    LLButton* mApprove = nullptr;
    LLButton* mReject = nullptr;
};

#endif // WOLF_FLOATEREXPERIENCES_H
