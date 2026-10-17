/**
 * @file wolffloateradventurestudio.h
 * @brief WolfViewer: Adventure Studio Lite - the creator's home, imports and publishing.
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

#ifndef WOLF_FLOATER_ADVENTURE_STUDIO_H
#define WOLF_FLOATER_ADVENTURE_STUDIO_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "llfloater.h"
#include "llsd.h"
#include "lluictrl.h"
#include "wolffloaterroleplay.h"
#include "wolfadventure.h"

#include <map>
#include <set>
#include <boost/json.hpp>

class LLInventoryItem;
namespace WolfGameDraw { struct Palette; }

/**
 * [WOLF ADVENTURE P1 2026-10-16] STUDIO_AND_CONSOLE_V1.md §2 in its P1 form (§5): Home (§2.1),
 * Import (§2.10: STORY v1 notecards and pack JSON) and Publish (§2.11: checks, regions, roles,
 * history), with no editors yet. Everything goes to the grid's adventure service
 * (wolfadventure_api.php, BRAIN_API_V1.md §5), which is the only validator; the window shows its
 * `problems`. Drawn in the wolfGame family like the Roleplay window (§1 "never plain form
 * floaters"): a left rail of pages, the page in the centre, a right drawer with the problems and
 * help, a status line for every result, questions inside the window with the safe answer focused.
 */
class WolfAdventureStudioView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureStudioView(const Params& p);

    /** The window opened: load the creator's adventures and the requests for their regions.
     *  [WOLF ADVENTURE P2 2026-10-16] key "run" (Wolf World > GM Console...) opens the Run page. */
    void onOpened(const LLSD& key = LLSD());

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                           EAcceptance* accept, std::string& tooltip_msg) override;
    void onMouseCaptureLost() override;
    /** Escape: the question inside the window takes it, else the window closes. */
    void escape();

private:
    // [WOLF ADVENTURE P2 2026-10-16] AI and voice (STUDIO §2.12), Cast talk (§2.3 voice and "Talk to
    // them now"), Book (§2.10 "Give it a book" and the GM assistant), Run (GM Console Lite: §3's
    // Take control / Hand back, Pause, AI on, the meter, the log and Assist).
    // [WOLF ADVENTURE P3 2026-10-16] the full Studio (STUDIO §2.2-2.9): Outline, Cast, Places, Story,
    // Rules, Lore and Test Play, between Import and Publish (wolffloateradventurestudioedit.cpp).
    // [WOLF ADVENTURE AUDIT 2026-10-17] SP_WIZARD ("Start here") is last in the enum (the other values are unchanged).
    // [FRONT DOOR 2026-10-17] SP_HELPER is the main screen; the rail (layoutAndDraw MAIN / ADV) shows the expert pages under
    // "Advanced editing"; the wizard is reached from the front door's "Make it box by box (no AI)".
    enum EPage { SP_HOME = 0, SP_IMPORT, SP_OUTLINE, SP_CASTED, SP_PLACES, SP_STORY, SP_RULES, SP_LORE, SP_TEST,
                 SP_PUBLISH, SP_AI, SP_CAST, SP_BOOK, SP_RUN, SP_WIZARD,
                 SP_HELPER,   // [WOLF STORY FIRST 2026-10-17] "Talk to your helper": the conversation and the flowchart
                 SP_COUNT };
public:
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The "Start here" wizard's steps (AUDIT_2026-10-17.md §5.4), in order. */
    enum EWizStep { W_KIND = 0, W_WHERE, W_STORY, W_CAST, W_PLACES, W_REWARDS, W_LOOK, W_TEST, W_PUBLISH, W_COUNT };
    /** What the guide floater shows: the page on screen, and the wizard step when it is Start here. */
    S32 guidePage() const { return (S32)mPage; }
    S32 guideStep() const { return mPage == SP_WIZARD ? (S32)mWizStep : -1; }
    bool guideHasAdventure() const { return !mOpenId.empty(); }
    /** The "What do I do next?" checklist: one entry per wizard step, done or not, read from the draft itself. */
    std::vector<bool> wizardChecks() const;
    /** The guide's checklist was clicked: open the wizard at that step. */
    void openWizardStep(S32 step);
private:
    enum EHit
    {
        SH_NONE = 0, SH_CLOSE, SH_PAGE, SH_SCROLL,
        // Home
        SH_OPEN, SH_DUPLICATE, SH_DELETE, SH_PURPOSE, SH_PRESET, SH_CREATE, SH_FIELD, SH_REFRESH,
        SH_APPROVE, SH_APPROVE_ENV, SH_APPROVE_LESS, SH_APPROVE_MORE, SH_APPROVE_OK, SH_APPROVE_CANCEL, SH_REFUSE,
        // Import
        SH_KIND, SH_IMPORT_REGION, SH_IMPORT_RUN, SH_PACK_FILE, SH_USE_DRAFT, SH_IMPORT_CANCEL, SH_DROP,
        SH_PICK_TOGGLE, SH_PICK_USE, SH_DOWNLOAD,
        // Publish
        SH_CHECK, SH_REQUEST, SH_REQUEST_HERE, SH_GRANT, SH_EXPIRY, SH_REVOKE, SH_REPUBLISH, SH_PUBLISH, SH_UNPUBLISH,
        // the question
        SH_YES, SH_NO,
        // [WOLF ADVENTURE P2 2026-10-16] AI and voice
        SH_AI_MODE, SH_AI_TOGGLE, SH_AI_STEP, SH_AI_REFRESH, SH_AI_BUY, SH_OWNER_STEP, SH_OWNER_SAVE,
        // Cast talk
        SH_CAST_PICK, SH_VOICE_PICK, SH_RATE_STEP, SH_VOICE_PREVIEW, SH_BEAT_STEP, SH_TEST_AI, SH_TEST_SEND, SH_TEST_REPLY,
        SH_TEST_CLEAR, SH_TEST_PLAY,
        // Book and assist
        SH_BOOK_FILE, SH_BOOK_PASTE, SH_BOOK_ACTS, SH_BOOK_REGION, SH_BOOK_RIGHTS, SH_BOOK_MAX, SH_BOOK_QUOTE, SH_BOOK_START,
        SH_JOB_OPEN, SH_JOB_CANCEL, SH_JOB_USE, SH_JOBS_REFRESH, SH_ASSIST_ASK, SH_ASSIST_RUN, SH_ASSIST_COPY,
        // Run
        SH_RUN_SESSION, SH_RUN_REFRESH, SH_RUN_TAKE, SH_RUN_LEAVE, SH_RUN_PAUSE, SH_RUN_RESUME, SH_RUN_AI, SH_RUN_ASSIST,
        SH_RUN_COPY,
        // the draft changed in another window
        SH_CONFLICT_RELOAD, SH_CONFLICT_KEEP,
        // [WOLF ADVENTURE P3 2026-10-16] the editors: SH_E runs mActs[arg], SH_E_TEXT edits the field mActs[arg]
        // describes, SH_PROBLEM opens the field of the drawer's problem `arg`
        SH_E, SH_E_TEXT, SH_PROBLEM,
        SH_GUIDE,   // [WOLF ADVENTURE AUDIT 2026-10-17] show / hide the Guide beside the Studio
        // [WOLF STORY FIRST 2026-10-17] the flowchart: its empty ground (pan), a box (select; arg = node), a box's handle (drag to connect)
        SH_FLOW_BG, SH_FLOW_BOX, SH_FLOW_HANDLE
    };
    enum EConfirm { SC_NONE = 0, SC_DELETE, SC_PUBLISH, SC_UNPUBLISH, SC_REPUBLISH, SC_REVOKE, SC_REFUSE, SC_REPLACE,
                    SC_JOB_START, SC_JOB_CANCEL, SC_TAKE,
                    SC_EDIT_DELETE, SC_TEST_TELEPORT, SC_PLACE_TELEPORT, SC_PRESET,     // [WOLF ADVENTURE P3 2026-10-16]
                    SC_RECOVER, SC_PREMIUM, SC_COND_REPLACE };
    enum EField { F_NONE = 0, F_TITLE, F_DELETE, F_FILTER, F_TALK, F_ASSIST, F_BOOK_TITLE, F_BOOK_AUTHOR,
                  F_EDIT,     // [WOLF ADVENTURE P3 2026-10-16] an editor field (mEditPtr)
                  F_HELPER }; // [WOLF STORY FIRST 2026-10-17] what the author says to the helper   // [WOLF ADVENTURE P3 2026-10-16] an editor field (mEditPtr)

    // drawing
    void layoutAndDraw();
    F32 pageHome(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageImport(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pagePublish(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** [WOLF STORY FIRST M7 2026-10-17] "Update the live game" / "Undo the last update" (STORY_FIRST_DESIGN.md §7, 7.1):
     *  the preview (update_preview), where the players of each removed box go, then update_live / update_undo. */
    F32 liveUpdateSection(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p, S32 errors);
    void livePreview(bool undo, const std::string& why = std::string());
    void liveApply();
    void drawDrawer(F32 l, F32 b, F32 r, F32 t, const WolfGameDraw::Palette& p);
    void drawConfirm(const WolfGameDraw::Palette& p);
    F32 head(F32 l, F32 r, F32 top, const std::string& text, const WolfGameDraw::Palette& p);
    F32 para(F32 l, F32 r, F32 top, const std::string& text, const LLColor4& colour, bool small = false);
    void field(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, EField which,
               const WolfGameDraw::Palette& p);
    void btn(F32 l, F32 b, F32 r, F32 t, const std::string& label, S32 id, S32 arg, const std::string& tip,
             const WolfGameDraw::Palette& p, bool enabled = true, U32 extra = 0);
    void hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable = true, bool enabled = true);
    U32 flagsFor(S32 id, S32 arg, bool enabled) const;

    // actions
    void press(S32 id, S32 arg);
    void ask(EConfirm what, S32 arg);
    void answer(bool yes);
    void setStatus(const std::string& text, bool error);
    void call(const std::string& action, const LLSD& body, std::function<void(WolfAdventureStudioView*, const LLSD&)> on_ok,
              const std::string& doing);
    void loadList();
    void loadRequests();
    void openAdventure(const std::string& id);
    void loadOpen();
    void closeOpenAdventure();
    void createAdventure();
    void duplicate(const std::string& id);
    void deleteAdventure();
    void publish(S32 republish_revision);
    void unpublish();
    void requestRegion(const std::string& region);
    void approve(S32 index);
    void refuse(S32 index);
    void pickGrantee(const std::string& role);
    void grant(const std::string& role, const LLUUID& agent, const std::string& name);
    void revoke(S32 index);
    void runImport();
    void choosePackFile();
    void useAsDraft();
    void saveDraftTo(const std::string& adv, const std::string& pack_json, const std::string& title);
    void readNotecard(LLInventoryItem* item);
    /** The notecard list (STUDIO §6 2.5.7: a picker for every drop): the agent's notecards by name. */
    void collectNotecards();
    /** Save a pack's JSON text to a file the player picks (Studio Lite's read-only "Open", as WolfStorm's). */
    void saveJson(const std::string& name, const std::string& json);
    void download(S32 revision);
    void setProblems(const LLSD& problems);
    /** [WOLF ADVENTURE AUDIT 2026-10-17] A problem's words: the brain's plain `say`, else its `text`. */
    static std::string problemWords(const LLSD& problem);
    const LLSD* listed(const std::string& id) const;
    S32 requestIndex(const std::string& adv, const std::string& region) const;
    S32 grantIndex(const std::string& agent, const std::string& role, const std::string& region) const;
    std::string& fieldText(EField which);
    /** [WOLF ADVENTURE P2 2026-10-16] The most characters a field holds. */
    static size_t fieldCap(EField which);

    // ---- [WOLF ADVENTURE P2 2026-10-16] wolffloateradventurestudioai.cpp ----
    F32 pageAi(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageCast(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageBook(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageRun(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** A -/+ stepper with its label and value as words; ids SH_*_STEP with arg = which * 2 (+1 for +). */
    void stepper(F32 l, F32 top, F32 label_w, const std::string& label, const std::string& value, S32 id, S32 which,
                 bool can_less, bool can_more, const std::string& tip, const WolfGameDraw::Palette& p);
    /** A drawn on/off switch as a button with its state as a word. */
    void toggle(F32 l, F32 top, F32 w, const std::string& label, bool on, S32 id, S32 arg, const std::string& tip,
                const WolfGameDraw::Palette& p, bool enabled = true);
    bool pressP2(S32 id, S32 arg);
    /** Change the open draft (the JSON text and the read copy) and save it 2 s after the last change
     *  (STUDIO §1 "Saving is automatic, debounced 2 s after the last edit"). */
    void editDraft(const std::function<void(boost::json::object&)>& edit);
    void saveDraftNow(bool keep_mine_etag = false);
    void autosaveTick();
    bool draftDirty() const { return mDirtyAt > 0.0; }
    bool openIsOwner() const;
    void loadBudget();
    void loadVoices();
    void loadOwnerSettings();
    void saveOwnerSettings();
    void loadHomeSpend();
    void testSay(const std::string& text, const std::string& reply);
    void previewVoice(S32 cast_index);
    void readBookFile();
    void bookQuote();
    void bookStart();
    void pollJob();
    void loadJobs();
    void assistRun();
    void loadSessions();
    void pollGmState(bool force);
    void gmOp(const std::string& op, const LLSD& extra, const std::string& doing);
    /** The open draft's beats in pack order: {beat id, "scene title - objective"}. */
    std::vector<std::pair<std::string, std::string>> draftBeats() const;
    /** The id of the scene a beat of the open draft is in ("" when none). */
    std::string sceneOfBeat(const std::string& beat) const;

    WolfGameHits mHits;
    EPage mPage = SP_HELPER;   // [WOLF STORY FIRST FRONT DOOR 2026-10-17] the Studio opens on the story-first screen
    F32 mScroll[SP_COUNT] = { 0.f, 0.f, 0.f };
    F32 mContentH = 0.f;
    LLRect mPageRect;
    bool mInPage = false;
    bool mKeyboardNav = false;
    bool mDraggingScroll = false;
    F32 mScrollGrabY = 0.f, mScrollGrabStart = 0.f;
    S32 mBusy = 0;
    std::string mStatus;
    bool mStatusError = false;
    F64 mStatusUntil = 0.0;
    EConfirm mConfirm = SC_NONE;
    S32 mConfirmArg = 0;
    std::string mConfirmKeyA, mConfirmKeyB, mConfirmKeyC;   // what a question is about, by key (not a row number)
    EField mField = F_NONE;

    // Home
    LLSD mList;                     // list_mine adventures
    bool mListLoaded = false;
    LLSD mRequests;                 // region_requests
    std::string mNewTitle;
    std::string mTitleError;        // shown under the title field (a form error, inline)
    S32 mNewPurpose = 0;            // PURPOSES index
    S32 mNewPreset = 0;             // PRESETS index
    std::string mDeleteId, mDeleteTitle, mDeleteTyped;
    std::string mApproveAdv, mApproveRegion;   // the request whose Approve form is open
    bool mApproveEnv = true;
    S32 mApproveBudget = 0;

    // the open adventure
    std::string mOpenId, mOpenTitle;
    std::string mDraftJson;         // its draft pack (get_draft), as its JSON text
    LLSD mDraftPack;                // the same, read, for what the pages show (its regions)
    std::string mEtag;
    LLSD mProblems;                 // the open draft's [{path, level, text}]
    LLSD mImportProblems;           // the import result's (shown on Import, never gating Publish)
    LLSD mRevisions, mGrants;
    S32 mGrantExpiry = 0;           // EXPIRIES index

    // Import
    bool mImportPack = false;       // false: STORY v1, true: pack JSON
    std::string mStoryName, mStoryText;
    LLSD mLooks;                    // {"Look - Name": asset uuid}
    std::string mImportRegion;
    std::string mImportedJson;      // the pack the service made (or the file), until Use as draft / Cancel
    LLSD mImportedPack;             // the same, read, for the outline shown
    LLSD mReport;                   // [text]
    std::string mPackFile;
    F64 mDragOverUntil = 0.0;
    bool mPickOpen = false;
    std::vector<std::pair<LLUUID, std::string>> mPickItems;   // every notecard, by name
    std::vector<LLUUID> mPickShown;                           // the rows shown, by hit arg
    std::string mPickFilter;
    U32 mNotecardSerial = 0;

    // ---- [WOLF ADVENTURE P2 2026-10-16] ----
    F64 mDirtyAt = 0.0;             // the draft changed here and is not saved yet (0 = saved)
    bool mSaving = false;
    bool mConflict = false;         // the save met a newer draft (409): Reload / Keep mine
    std::string mConflictEtag;      // that newer draft's etag (Keep mine saves over it)
    std::map<std::string, std::string> mHomeSpend;   // adventure -> "AI today: ..." (Home cards)
    // AI and voice
    LLSD mBudget;                   // ai_budget
    std::string mBudgetFor, mBudgetError;
    bool mBudgetLoading = false;
    LLSD mOwner;                    // owner_settings.settings (the editable copy)
    bool mOwnerLoaded = false, mOwnerDirty = false;
    bool mOwnerAsked = false, mVoicesAsked = false;   // asked once per visit to a page (never every frame)
    F64 mOwnerRetryAt = 0.0, mVoicesRetryAt = 0.0;    // after a failed load, asked again no sooner than this (seconds, LLTimer)
    // voices and Cast talk
    LLSD mVoices;                   // voices.voices
    bool mVoicesLoaded = false;
    S32 mCastSel = 0;
    S32 mBeatSel = 0;
    bool mTestAi = true;            // false = "Preview without AI"
    std::string mTestBox;
    bool mTestBusy = false;
    LLSD mTestHistory;              // [{who, text}] - sent as `history`
    WolfAdventure::Talk mTestTalk;  // the face, drawn with the talk panel's own face
    std::vector<std::pair<std::string, std::string>> mTestReplies;   // id, label
    S64 mTestLastMicro = 0, mTestTotalMicro = 0;
    std::string mTestVoiceUrl, mTestCastFor;
    // Book
    std::string mBookName, mBookText, mBookTitle, mBookAuthor, mBookRegion;
    S32 mBookActs = 5;
    bool mBookRights = false;
    S32 mBookMax = 400;
    std::string mJobId;
    LLSD mJob;                      // job {job}
    LLSD mJobs;                     // jobs
    F64 mJobPollAt = 0.0;
    // Assist (Studio, on the draft)
    S32 mAssistAsk = 0;             // 0 next, 1 boxed, 2 reply
    std::string mAssistBox;
    bool mAssistBusy = false;
    LLSD mAssist;                   // suggestions
    std::string mAssistCost;
    // Run (GM Console Lite)
    LLSD mSessions;
    std::string mRunSession;
    LLSD mGm;                       // gm_state
    std::string mGmError;
    F64 mGmPollAt = 0.0;
    bool mGmLoading = false;
    LLSD mRunSuggest;
    std::string mRunSuggestCost;

public:
    // ---- [WOLF ADVENTURE P3 2026-10-16] the full Studio: wolffloateradventurestudioedit.cpp ----
    /** The place tool finished a shape (WolfToolAdventurePlace): its members are written into the place `id` of
     *  places/<kind>, found again by its id (a list reordered meanwhile still gets the right place). */
    void placed(const std::string& kind, const std::string& id, const boost::json::value& v, const std::string& what);
    /** The place tool's progress or refusal, as the status line. */
    void placeSay(const std::string& what);
    /** A place of the open draft by list and id (undefined when it is gone). */
    const LLSD& placeOf(const std::string& kind, const std::string& id) const;
    /** Bumped on every change of the open draft (the caches - rules, reference lists, the overlay - follow it). */
    U32 draftGen() const { return mDraftGen; }
    /** The places of the open draft on `region` for the in-world overlay (STUDIO §2.4 "Overlay"). */
    const LLSD& draftPack() const { return mDraftPack; }
    bool placesPageShown() const;
    // the window's hooks into the editors (wolffloateradventurestudioedit.cpp)
    void editTick();
    bool editPress(S32 id, S32 arg);
    bool editKey(KEY key, MASK mask);
    bool editChar(llwchar c);
    bool editEscape();
    bool editWheel(S32 clicks);
    bool editConfirmText(S32 what, std::string& title, std::string& body, std::string& yes, bool& danger) const;
    void editAnswer(S32 what);
    void editDeclined(S32 what);
    bool editDrop(S32 x, S32 y, bool drop, S32 cargo_type, void* cargo_data, EAcceptance* accept, std::string& tip);
    /** The Studio closed: the field written, the draft saved, the place tool and any test ended (no network when the
     *  viewer is quitting: the service is not called from a shutting-down viewer). */
    void editClosed(bool app_quitting);

private:
    /** One thing a click (or Enter on the focused control) does on an editor page, rebuilt every frame. */
    struct EditAct
    {
        std::function<void()> mFn;
        std::string mPtr;           // a text field: what it edits (a JSON pointer into the doc)
        S32 mDoc = 0;               // 0 the draft pack, 1 the merged rules (writes become rules.overrides)
        size_t mCap = 0;            // the most characters
        char mType = 's';           // s text, i integer, n number, w comma-separated words, j JSON, k a new map key
        std::string mExtra;         // k: the new entry's JSON
        bool mOptional = true;      // an emptied optional field is removed
        F64 mMin = -1e18, mMax = 1e18;
    };
    struct Picker
    {
        bool mOn = false;
        std::string mTitle;
        std::vector<std::pair<std::string, std::string>> mOpts;   // value, words
        std::function<void(const std::string&)> mSet;
        std::string mEmpty;         // shown when there are no options (what to make first)
    };
    S32 act(const std::function<void()>& fn);
    S32 textAct(const std::string& ptr, S32 doc, size_t cap, char type, bool optional, F64 mn = -1e18, F64 mx = 1e18);
    F32 pageOutline(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** [WOLF ADVENTURE AUDIT 2026-10-17] Add an act (0), a scene (1) or a beat (2) for the Outline's selection `sel`, its new
     *  beat wired into the story's chain after the beat before it (never born unreachable or a dead end). */
    void addStoryItem(S32 what, const std::string& sel);
    /** [WOLF ADVENTURE AUDIT 2026-10-17] Where the schema wants text and the open draft holds something else ({}, a list or
     *  null: what the old Studio's makeDefault wrote), starter words go in (never over the author's own text). The draft is
     *  then saved by the usual autosave. Returns how many were tidied. */
    S32 repairLegacyDraft();
    /** A problem's field to focus is `ptr`, or `ptr` is the first field drawn under it (a trigger's problem lands on its field). */
    bool focusWants(const std::string& ptr) const;
    /** [WOLF ADVENTURE AUDIT 2026-10-17] A colour field (#rrggbb): the box and a swatch of the colour. */
    F32 fColour(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, S32 doc, bool optional,
                const WolfGameDraw::Palette& p);
    F32 pageCastEd(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pagePlaces(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageStory(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageRules(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageLore(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 pageTest(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void drawPicker(const WolfGameDraw::Palette& p);
    std::vector<S32> pickerRows() const;
    void pickerChoose(S32 opt);
    void closePicker();
    F32 conflictBanner(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 fTexture(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, const WolfGameDraw::Palette& p);
    void chooseTexture(const std::string& ptr);
    F32 fQuiz(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& quiz, const WolfGameDraw::Palette& p);
    void releasePlaceTool();
    // the schema-driven form (adventure_pack_v1.schema.json / rules_preset_v1.schema.json)
    F32 fValue(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& schema,
               const LLSD& v, bool optional, S32 doc, const WolfGameDraw::Palette& p, const std::string& label = std::string());
    F32 fObject(F32 l, F32 r, F32 top, const std::string& ptr, const boost::json::value& schema, const LLSD& v, S32 doc,
                const WolfGameDraw::Palette& p, const std::vector<std::string>& only = std::vector<std::string>(),
                const std::vector<std::string>& skip = std::vector<std::string>());
    F32 fArray(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& schema,
               const LLSD& v, S32 doc, const WolfGameDraw::Palette& p, const std::string& label);
    F32 fText(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, S32 doc, size_t cap,
              char type, bool optional, const std::string& tip, const WolfGameDraw::Palette& p, F64 mn = -1e18, F64 mx = 1e18);
    F32 fRef(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const std::string& kind, const LLSD& v,
             S32 doc, bool optional, const WolfGameDraw::Palette& p);
    F32 fEnum(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const std::vector<std::string>& opts,
              const LLSD& v, const std::string& dflt, S32 doc, bool optional, const WolfGameDraw::Palette& p);
    F32 fBool(F32 l, F32 r, F32 top, const std::string& label, const std::string& ptr, const LLSD& v, bool dflt, S32 doc,
              const WolfGameDraw::Palette& p, const std::string& tip = std::string());
    F32 fCondition(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, S32 doc, const WolfGameDraw::Palette& p, S32 depth);
    F32 fEffects(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const WolfGameDraw::Palette& p, const std::string& label);
    F32 fTriggers(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const WolfGameDraw::Palette& p);
    F32 fRow(F32 l, F32 r, F32 top, const std::string& label, F32& cl);
    F32 cardHead(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& title, S32 index, S32 count,
                 const std::string& arrPtr, bool can_delete, const WolfGameDraw::Palette& p, bool open_toggle = true, S32 doc = 0);
    F32 fMap(F32 l, F32 r, F32 top, const std::string& ptr, const std::string& key, const boost::json::value& itemSchema,
             const LLSD& v, S32 doc, const WolfGameDraw::Palette& p, const std::string& label);
    F32 scheduleRibbon(F32 l, F32 r, F32 top, const std::string& ptr, const LLSD& v, const WolfGameDraw::Palette& p);
    void importQti();
    // the doc
    const LLSD& docRead(S32 doc) const;
    void docSet(S32 doc, const std::string& ptr, const boost::json::value& v);
    void docErase(S32 doc, const std::string& ptr);
    void docAppend(S32 doc, const std::string& ptr, const boost::json::value& v);
    void docMove(S32 doc, const std::string& arrPtr, S32 index, S32 by);
    void docDuplicate(S32 doc, const std::string& arrPtr, S32 index);
    void askDelete(S32 doc, const std::string& ptr, const std::string& what);
    /** Write the open field. A new map id and a test flag are written only with Enter (`enter`). */
    void commitEdit(bool enter = false);
    void startEdit(S32 act_index);
    void openPicker(const std::string& title, const std::vector<std::pair<std::string, std::string>>& opts,
                    const std::function<void(const std::string&)>& set, const std::string& empty);
    void pickRef(const std::string& title, const std::string& ptr, const std::string& kind, S32 doc, bool optional);
    std::vector<std::pair<std::string, std::string>> refOptions(const std::string& kind, const std::string& ptr) const;
    std::string castUses(const std::string& cast_id) const;
    void gotoProblem(const std::string& path);
    S32 problemsUnder(const std::string& ptr) const;
    std::string problemAt(const std::string& ptr) const;
    // rules
    void loadPreset();
    void rulesSet(const std::string& ptr, const boost::json::value* v);
    bool rulesOverridden(const std::string& ptr) const;
    // test play
    void testStart(const std::string& beat);
    void testOp(const std::string& action, const LLSD& extra, const std::string& doing);
    void pollTest(bool force);
    // place tool
    void startPlace(const std::string& kind, const std::string& id, S32 mode, const std::string& what);
    void placeUseMyPosition(const std::string& spot_id);
    std::string placePtr(const std::string& kind, const std::string& id) const;
    void stopPlacing();
    void pickVoice(S32 cast_index, const std::string& voice_id);
    std::vector<std::pair<std::string, std::string>> refOptionsMake(const std::string& kind, const std::string& ptr) const;
    // the draft as one parsed document (P3); leaving an adventure; the recovery file
    bool editDoc(const std::function<bool(boost::json::value&)>& edit);
    void setDraftText(const std::string& json, const LLSD& pack);
    void leaveOpen(bool app_quitting = false);
    std::string recoveryPath(const std::string& adv) const;
    void writeRecovery();
    void checkRecovery();
    void saveLook(const std::string& cast_id);
    void lookReload();

    // ---- [WOLF ADVENTURE AUDIT 2026-10-17] the "Start here" wizard: wolffloateradventurestudiowizard.cpp ----
    F32 pageWizard(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizKind(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizWhere(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizStory(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizCast(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizPlaces(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizRewards(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizLook(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 wizPublish(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** The step's own check before Next: "" when it may go on, else what to fix (shown at the top of the step). */
    std::string wizCheck(S32 step) const;
    void wizGo(S32 step);
    void wizNext();
    void loadTemplates();
    void wizCreate();
    /** A beat's "done when" trigger: the first of its triggers that leads on (next, or a goto / end step); -1 when none. */
    S32 doneTrigger(const LLSD& beat) const;
    void setDoneWhen(const std::string& beat_ptr, const std::string& on);
    void removeStoryBeat(const std::string& beat_ptr);
    void placeAreaHere(const std::string& area_id);
    std::string newPackId(const std::string& base) const;
    void rememberWizStep();

    // ---- [WOLF STORY FIRST 2026-10-17] the helper and the flowchart: wolffloateradventurestudioflow.cpp ----
    F32 pageHelper(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** [FRONT DOOR 2026-10-17] nothing open: the story box, Build it, and the games to continue (wolffloateradventurestudio.cpp). */
    F32 frontDoor(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** [FRONT DOOR] the main screen's columns: the story (left) and the helper conversation (right). */
    F32 storyColumn(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 helperColumn(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** [FRONT DOOR] a text field several lines high: the text wrapped, its last lines shown, the caret at the end. */
    void fieldMulti(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, EField which, const WolfGameDraw::Palette& p);
    /** [FRONT DOOR] "Advanced editing": the old pages in the rail, open or shut, remembered per account. */
    bool advancedOpen();
    void setAdvancedOpen(bool on);
    /** [FRONT DOOR] the first line makes the adventure (create), then the line goes to the helper, or Load a book opens. */
    void helperCreate(const std::string& text, bool then_book);
    F32 drawFlow(F32 l, F32 r, F32 top, F32 h, const WolfGameDraw::Palette& p);
    F32 flowBoxPanel(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void flowLayout();
    void helperSend();
    void helperUndo();
    void helperHistory();
    void helperApply(const LLSD& reply);
    /** STORY_FIRST_DESIGN.md §4: "Build it" from the saved story (draft_start fromStory), and a finished job merged into the
     *  draft (job_apply, never destroying the author's own work). */
    void storyQuote();
    void jobApply();
    /** §9: the helper NPC walks with the author (helper_walk, with where they stand), or goes. */
    void helperWalk(bool on);
    void helperLook(const std::string& from, const std::string& asset);
    /** The box the author clicked, told to the brain (helper_focus): what they say to the helper in the world is about it. */
    void helperFocus(const std::string& node);
    /** The non-AI checklist: the next box that needs something (helper_next), and one of its buttons done (helper_do). */
    void helperNextBox(const std::string& after);
    /** `then` runs after the op is saved (M4: a new enemy, then the fight that names it). */
    void helperDo(const LLSD& op, std::function<void()> then = std::function<void()>());
    /** "It leads to...": how the box is done, then where it goes, chosen in pickers, then helper_do set_lead. */
    void pickLead(const std::string& beat_id);
    /** "Walks with them from here": who, how, until where, then helper_do set_companion. */
    void pickCompanion(const std::string& beat_id);
    /** [M4 2026-10-17] "A fight happens here...": who they fight (an enemy of the story, a character turning on them, or a
     *  ready-made enemy of the rules - enemy_templates), how many, where winning leads and what losing does; then
     *  helper_do add_enemy (when new) and add_battle. */
    void pickBattle(const std::string& beat_id);
    void pickBattleWho(const std::string& beat_id, const LLSD& templates);
    /** [M6 2026-10-17] the box's scripts: "When my script says <name>" (add_script_hook when script, then where it leads)
     *  and "Tell my scripts when this box starts" (when enter: a signal step); names made from the box's id. */
    void pickScriptHook(const std::string& beat_id, bool starts);
    /** [M6] a ready-to-paste LSL script: touch to fire the box's script trigger `name`, or listen for the signal `name`. */
    std::string scriptFor(const std::string& name, bool listen) const;
    LLSD helperHere() const;
    /** Box `from` leads on to box `to` (a beat's done trigger `next`; an ending's box takes an `end` step). */
    void flowConnect(const std::string& from_key, const std::string& to_key);
    S32 flowFind(const std::string& key) const;
    bool flowMouseDown(S32 x, S32 y, const WolfGameHits::Hit* h);
    bool flowHover(S32 x, S32 y);
    bool flowMouseUp(S32 x, S32 y);
    bool flowWheel(S32 x, S32 y, S32 clicks);
    struct FlowNode
    {
        std::string mPtr, mId, mTitle, mPlace;   // a beat's pointer and id, or an ending's (mEnding)
        bool mEnding = false, mPlaced = false, mStart = false;
        std::string mFight, mFightId;            // [M4 2026-10-17] the box starts a fight: its label and battle id (onEnter battle)
        S32 mLayer = 0, mRow = 0;
        F32 mX = 0.f, mY = 0.f;                  // the box's top left in chart units (before pan and zoom)
    };
    struct FlowEdge { S32 mFrom = -1, mTo = -1; std::string mLabel; bool mDashed = false; };
    /** A box's key: "b:<beat id>" or "e:<ending id>" (ids, never positions: the draft may change under a drag or a list). */
    static std::string flowKey(const FlowNode& n) { return (n.mEnding ? "e:" : "b:") + n.mId; }

    std::vector<FlowNode> mFlowNodes;
    std::vector<FlowEdge> mFlowEdges;
    U32 mFlowGen = 0;
    F32 mFlowPanX = 0.f, mFlowPanY = 0.f, mFlowZoom = 1.f;
    LLRect mFlowRect;                            // where the chart is drawn this frame (view coordinates)
    bool mFlowPanning = false;
    S32 mFlowGrabX = 0, mFlowGrabY = 0;
    F32 mFlowGrabPanX = 0.f, mFlowGrabPanY = 0.f;
    S32 mFlowConnectFrom = -1;                   // dragging from this box's handle (this frame's index, checked against the key)
    std::string mFlowConnectKey;                 // ... and that box's key
    S32 mFlowMouseX = 0, mFlowMouseY = 0;
    bool mFlowOnlyHere = false;
    std::string mFlowSel;                        // the box the author clicked: its beat id (the helper's `node`)
    std::string mHelperBox;                      // what the author is typing to the helper
    LLSD mHelperLog;                             // [{who: "you"|"helper", text}]
    LLSD mHelperTodo;                            // the "still to decide" list
    std::string mHelperQuestion, mHelperError;
    std::string mHelperScript;
    LLSD mLivePreview;                                  // [M7] the last update_preview reply (undefined: none shown)
    bool mLiveUndo = false, mLiveBusy = false;          // [M7] the preview is of "Undo the last update"; a call is out
    std::map<std::string, std::string> mLiveMoves;      // [M7] removed box id -> the box its players go to (author's pick)   // [M6 2026-10-17] LSL the helper wrote on request (reply `script`), for the author to copy
    F64 mHelperTodayCredits = -1.0, mHelperCap = 0.0, mHelperLastCredits = 0.0;
    bool mHelperBusy = false, mHelperLoaded = false, mHelperMissing = false;
    bool mHelperPending = false;                 // the first line waits for the new adventure's draft to load
    S32 mAdvancedState = -1;                     // [FRONT DOOR] -1 not read yet from WolfAdventureAdvancedOpen, else 0 / 1
    // [WOLF STORY FIRST M3 2026-10-17] the helper in the world (STORY_FIRST_DESIGN.md §9, §9.1)
    bool mHelperWalking = false;                 // helper_walk on, as the brain last said
    std::string mHelperWalkRegion;               // the region it walks on (a region change re-sends helper_walk)
    std::string mHelperLookFrom;                 // wearing | notecard | default
    bool mHelperWalkBusy = false;
    std::string mHelperWalkFailedRegion;         // a region that refused the helper: not asked again until the author moves on
    F64 mHelperWalkCheckAt = 0.0;                // when the walking state is read again (the brain's 30-minute sweep may end it)
    LLSD mHelperNext;                            // the checklist's last helper_next reply (the non-AI path)
    std::string mHelperFor;                      // the adventure mHelperLog belongs to

    EWizStep mWizStep = W_KIND;
    LLSD mTemplates;                // templates.templates
    bool mTemplatesLoading = false, mTemplatesLoaded = false;
    std::string mTemplatesError;
    std::string mWizTemplate;       // the kind chosen on step 1: a template id, "" = none chosen, "~book" / "~empty"
    S32 mWizRating = 0;             // 0 general, 1 moderate, 2 adult (adventure_pack_v1 adventure.rating)
    std::string mWizError;          // the step's check, shown at its top (a summary) until the step is right
    bool mWizTested = false;        // a test was started from the wizard for the open adventure
    bool mWizAuto = false;          // choose the step once the draft loads: the first one not done yet

    std::vector<EditAct> mActs;
    std::string mEditPtr, mEditBuf, mEditError, mEditErrorPtr;
    S32 mEditDoc = 0;
    size_t mEditCap = 0;
    char mEditType = 's';
    std::string mEditExtra;
    bool mEditOptional = true;
    F32 mPickerScroll = 0.f;
    std::string mPickerFilter;      // typed while the list is open
    S32 mPickerSel = 0;             // the row Up / Down / Enter work on (in the filtered rows)
    bool mPickerSelMoved = false;   // scroll it into view on the next draw
    std::vector<std::pair<LLRect, std::string>> mDrops;   // texture drop targets this frame: where, and the pointer it sets
    LLHandle<LLFloater> mTexPicker;
    LLSD mGameLinks;                // game_links {adventure}
    std::string mGameLinksFor, mGameLinksError;
    bool mGameLinksLoading = false;
    U32 mRulesGen = 0;              // the draft generation mRulesMerged was made from
    boost::json::value mDraftDoc;   // the open draft, parsed once (editDoc edits it in place)
    U32 mDraftGen = 1;
    std::map<std::string, S32> mProblemCount;                        // pointer -> problems at or under it (setProblems)
    std::map<std::string, std::pair<bool, std::string>> mProblemText; // pointer -> (is an error, its message)
    mutable std::map<std::string, std::vector<std::pair<std::string, std::string>>> mRefCache;
    mutable U32 mRefCacheGen = 0;
    bool mPremiumOk = false;        // a premium voice's cost was confirmed once in this Studio
    S32 mPremiumCast = -1;
    std::string mPremiumVoice;
    std::string mCondPtr, mCondNew; // SC_COND_REPLACE: the group and the rule that replaces it
    std::string mRecoveryText;      // SC_RECOVER: the unsaved draft offered back
    F64 mFocusAt = 0.0;
    S32 mRibbonFrom = -1;           // the schedule ribbon: the first hour clicked of a new range (§2.3 "Click a time range")
    LLSD mTestFlags;                // Test Play: the flags as a doc (doc 2), {id: value}
    F64 mEditMin = -1e18, mEditMax = 1e18;
    F64 mEditAt = 0.0;              // the field changed and is not written yet (0 = nothing waiting)
    std::string mFocusPtr;          // a problem's field to bring into view and focus (STUDIO §2.2 "focuses the field")
    bool mFocusScroll = false;
    std::set<std::string> mOpen;    // the cards shown open, by pointer
    Picker mPicker;
    std::string mDeletePtr;         // SC_EDIT_DELETE: what goes
    S32 mDeleteDoc = 0;
    std::string mSel;               // Outline / Story: the node shown (a pointer: /acts/i[/scenes/j[/beats/k]])
    S32 mCastSel2 = 0;              // the Cast page's character
    S32 mLoreTab = 0;               // the Lore page's list
    S32 mPlaceTab = 0;              // the Places page's list
    S32 mRulesTab = 0;              // the Rules page's table
    LLSD mPreset;                   // the pack's preset (app_settings/wolfadventure/rules/<id>.json)
    std::string mPresetId;
    LLSD mRulesMerged;              // the preset with the pack's overrides merged (RFC 7396)
    std::string mPresetPick;        // SC_PRESET: the preset asked for
    // Test Play
    std::string mTestSession, mTestRegionName, mTestBeat;
    bool mTestNoAi = false;
    LLSD mTestState;
    std::string mTestError;
    F64 mTestPollAt = 0.0;
    bool mTestLoading = false;
    // Use what I'm wearing
    F64 mLookReloadAt = 0.0;
    std::string mLookCast, mLookBefore;
    S32 mLookTries = 0;
    bool mShowPremium = false;      // Cast talk: the premium (ElevenLabs) voice group, off by default (lead's P3 voices contract)
};

class WolfFloaterAdventureStudio : public LLFloater
{
public:
    WolfFloaterAdventureStudio(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The Studio's face, for the Guide (it shows help for the page on screen). */
    WolfAdventureStudioView* view() const { return mView; }
    /** Show the Guide docked beside the Studio (a dependent floater that moves with it), or hide it; remembered. */
    void showGuide(bool on);
    static bool guideShown();

private:
    WolfAdventureStudioView* mView = nullptr;
};

#endif // WOLF_FLOATER_ADVENTURE_STUDIO_H
