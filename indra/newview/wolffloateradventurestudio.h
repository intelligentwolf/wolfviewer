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
#include "wolfimportflow.h"   // [IMPORT WORLDS 2026-10-18/]
#include <memory>

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
    // [WOLF ADVENTURE AUDIT 2026-10-17] SP_WIZARD ("Start here") is last of the expert pages (the other values are unchanged).
    // [SIMPLE STUDIO 2026-10-17] SIMPLE_STUDIO_SPEC.md: the default path is THREE pages - SP_HELPER "Your story" (page 1),
    // SP_CHARS "Your characters" (page 2), SP_MAP "Your story map" (page 3). The expert pages stay behind "Advanced editing"
    // on page 3 (never remembered open); the wizard is reached from page 1's "Make it box by box, with no AI".
    enum EPage { SP_HOME = 0, SP_IMPORT, SP_OUTLINE, SP_CASTED, SP_PLACES, SP_STORY, SP_RULES, SP_LORE, SP_TEST,
                 SP_PUBLISH, SP_AI, SP_CAST, SP_BOOK, SP_RUN, SP_WIZARD,
                 SP_HELPER,   // page 1 "Your story": the story box, Load a book file, Build my game, the games to continue
                 SP_CHARS,    // page 2 "Your characters": one simple card per character
                 SP_MAP,      // page 3 "Your story map": the flowchart, the box panel, the guided walk, the helper
                 SP_COUNT };
public:
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The "Start here" wizard's steps (AUDIT_2026-10-17.md §5.4), in order. */
    enum EWizStep { W_KIND = 0, W_WHERE, W_STORY, W_CAST, W_PLACES, W_REWARDS, W_LOOK, W_TEST, W_PUBLISH, W_COUNT };
    /** What the guide floater shows: the page on screen, and the wizard step when it is Start here. */
    S32 guidePage() const { return (S32)mPage; }
    S32 guideStep() const { return mPage == SP_WIZARD ? (S32)mWizStep : -1; }
    /** [SIMPLE STUDIO 2026-10-17] The simple step on screen: 0 Your story, 1 Your characters, 2 Your story map; -1 on an expert page. */
    S32 simpleStep() const { return mPage == SP_HELPER ? 0 : mPage == SP_CHARS ? 1 : mPage == SP_MAP ? 2 : -1; }
    /** Whether that step can be shown now (pages 2 and 3 need a game of the author's own, open and loaded). */
    bool simpleCanGo(S32 step) const;
    /** [TABS 2026-10-17] The open draft has at least one box (a beat under acts > scenes): the Characters and Gameplay tabs open. */
    bool hasBoxes() const;
    /** The Guide's step was clicked: show that page. */
    void goSimple(S32 step);
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
        SH_MINIMIZE,   // [MINIMIZE 2026-10-17] Paul: "it needs a minimize button on the floater" - the band's "-" (LLFloater::setMinimized)
        // [WOLF STORY FIRST 2026-10-17] the flowchart: its empty ground (pan), a box (select; arg = node), a box's handle (drag to connect)
        SH_FLOW_BG, SH_FLOW_BOX, SH_FLOW_HANDLE
    };
    enum EConfirm { SC_NONE = 0, SC_DELETE, SC_PUBLISH, SC_UNPUBLISH, SC_REPUBLISH, SC_REVOKE, SC_REFUSE, SC_REPLACE,
                    SC_JOB_START, SC_JOB_CANCEL, SC_TAKE,
                    SC_EDIT_DELETE, SC_TEST_TELEPORT, SC_PLACE_TELEPORT, SC_PRESET,     // [WOLF ADVENTURE P3 2026-10-16]
                    SC_RECOVER, SC_PREMIUM, SC_COND_REPLACE,
                    SC_DELETE_GAME,     // [SIMPLE STUDIO 2026-10-17] page 1's cards: a plain Delete / Keep it (no typed title)
                    SC_FLOW_TIDY,       // page 3's "Tidy up": every box back to its automatic place (the saved positions go)
                    SC_MESH_CANCEL,     // [IN-WORLD STUFF 2026-10-17] "Stop" on a thing being made (Meshy): job_cancel, asked first
                    SC_LAND_CLEANUP,    // [IN-WORLD STUFF 2026-10-17] "Clean up the land": the game's name typed, then land_cleanup
                    SC_READY_GAME,      // [GAME CARD 2026-10-18] a ready game picked while page 1 has words or a book: asked first
                    SC_CHAPTER,         // [WORDING 2026-10-18] "Add the next chapter - N credits": asked first, with US $ (as page 1)
                    SC_SETUP_CLEAN };   // [SETUP WALK 2026-10-18] the walk's "Clean up": asked first, then setup_clean
    /** [IN-WORLD STUFF 2026-10-17] The questions answered by typing the game's name (the field F_DELETE, mDeleteTyped == mDeleteTitle). */
    bool typedConfirm() const { return mConfirm == SC_DELETE || mConfirm == SC_LAND_CLEANUP; }
    enum EField { F_NONE = 0, F_TITLE, F_DELETE, F_FILTER, F_TALK, F_ASSIST, F_BOOK_TITLE, F_BOOK_AUTHOR,
                  F_EDIT,     // [WOLF ADVENTURE P3 2026-10-16] an editor field (mEditPtr)
                  F_HELPER,   // [WOLF STORY FIRST 2026-10-17] what the author says to the helper
                  F_STORY,    // [SIMPLE STUDIO 2026-10-17] page 1's story box (the game's story, up to 20,000 characters)
                  F_PLAN_YOU, // [GAME CARD 2026-10-18] "Choose your game": Who are you? (the card's youAre, 240)
                  F_PLAN_WIN };  //   ... How do you win? (the card's win, 240)

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
    /** [FIXED FOOT 2026-10-17] Paul: "keep the buttons at the bottom visible please" - the page's way-on buttons sit in a bar
     *  under the scrolled page, inside the floater, never scrolled away: page 2 "Your story" / "Next: set up your places";
     *  page 3 "Your characters", "My games", "Rebuild my game" / "Advanced editing", "Publish". 0 = the page has no bar. */
    F32 footBarHeight() const;
    void drawFootBar(F32 l, F32 b, F32 r, F32 t, const WolfGameDraw::Palette& p);
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
    // ---- [SIMPLE STUDIO 2026-10-17] page 1's one builder (SIMPLE_STUDIO_SPEC.md "Build my game"), wolffloateradventurestudioai.cpp ----
    /** The steps of one build, in order; B_NONE between builds. Every step that can fail ends in buildFail (red text on page 1). */
    enum EBuild { B_NONE = 0, B_CREATING, B_WRITE, B_SAVING, B_BUDGET, B_QUOTING, B_QUOTED, B_STARTING, B_RUNNING, B_APPLYING };
    /** "Build my game": checks the words or the book and its rights tick, makes the game when none is open, then buildTick goes on. */
    void buildStart();
    void buildCreate(const std::string& name);
    /** Each frame: the draft loaded -> the story written and saved -> the balance read -> the quote asked (then the inline question). */
    void buildTick();
    void buildBudget();
    void buildQuote(S32 max_credits, bool balance_known);
    /** "Yes, build it" / "No" on the inline question. */
    void buildYes();
    void buildNo();
    void buildFail(const std::string& why);
    /** [SIMPLE STUDIO 2026-10-17] Paul: "it shouldn't save the game until the user has approved payment". The game this build
     *  attempt made (mBuildMadeId) is deleted again (the brain's delete action) when the attempt ends before "Yes, build it". */
    void buildDiscard(const std::string& id, const std::string& title);
    /** The game this attempt made closed (page 1 keeps the words and the book) and discarded. */
    void buildUnmake();
    /** Paul: "please let it pick up where i left off": the author's drafting jobs asked for (jobs); a queued or running build of
     *  the open game (or, with nothing open, of the newest game built) is polled again, a done one put in, an applied one opened. */
    void resumeBuild();
    /** A done job whose output is already in the draft (one of its boxes is there): an older brain never marked it applied. */
    bool jobOutputIn(const LLSD& job) const;
    /** The failed or stopped jobs whose words page 1 already showed once (WolfAdventureJobsSeen, per account). */
    bool jobSeen(const std::string& job) const;
    void jobMarkSeen(const std::string& job);
    /** Page 1's cards: the brain's delete action for one game (the exact title as its confirm), the list read again. */
    void deleteGame(const std::string& id, const std::string& title);
    /** The clipboard's text as the book; "" when it was taken, else why not. */
    std::string bookFromClipboard();
    std::string bookFromText(const std::string& text);   // a pasted text too long for the story box becomes the book
    /** The book file's name without its extension (the game's name, the brain's `title`). */
    std::string bookStem() const;
    void assistRun();
    void loadSessions();
    void pollGmState(bool force);
    void gmOp(const std::string& op, const LLSD& extra, const std::string& doing);
    /** The open draft's beats in pack order: {beat id, "scene title - objective"}. */
    std::vector<std::pair<std::string, std::string>> draftBeats() const;
    /** The id of the scene a beat of the open draft is in ("" when none). */
    std::string sceneOfBeat(const std::string& beat) const;

    WolfGameHits mHits;
    EPage mPage = SP_HELPER;   // [SIMPLE STUDIO 2026-10-17] the Studio opens on page 1, "Your story"
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
    // [SIMPLE STUDIO 2026-10-17] page 1
    std::string mStoryBox;          // the story box (F_STORY); the open game's story once its draft has loaded (mStoryFor)
    std::string mStoryFor;          // the game mStoryBox was read from (or typed for)
    EBuild mBuildStep = B_NONE;
    S32 mBuildMax = 0;              // the most credits this build may spend (sent as maxCredits)
    std::string mBuildError;        // page 1's red text; stays until the next Build my game
    bool mBuildNeedCredits = false; // the last failure was credits: page 1 shows Buy credits... (Paul 10-17: no link to buy them)
    std::string mBuildBuyUrl;       // ai_budget's buyurl (wolfmind_lib.php WM_BUY_URL), for that button
    std::string mLandNote;          // "This land belongs to someone else..." (create's region.state not enabled), kept on every page
    bool mLandPending = false;      // openAdventure: once the draft loads, land on page 3 when it has boxes, else page 1
    std::string mBuildMadeId;       // the game THIS build attempt made (buildCreate), gone again if the attempt ends before Yes
    std::string mBuildMadeTitle;    // its title as the brain stored it (create's reply): the delete action's confirm
    time_t mBuildRunAt = 0;         // when the build began running (Yes pressed, or the job's created time when picked up): the card's clock
    S32 mBuildActs = 0;             // how many acts the brain is writing, once its progress says "Act k of N" (0 = not known yet)
    S32 mBuildKind = 0;             // what is being built: 1 a typed story, 2 a loaded book, 0 not known (picked up after a relaunch)
    std::string mCardsError;        // red text under the Continue cards (a delete the brain refused); stays
    bool mResumeOnList = false;     // onOpened: once the list has loaded, resumeBuild() (the list names the game to open)
    bool mLandChars = false;        // resumeBuild: a build already in the game - land on the characters when it has no boxes
    std::string mCharsError;        // page 2's red text
    std::string mCharsWho;          // [CHARACTER GALLERY 2026-10-17] page 2: "" the gallery, "c:<id>" a character's page, "e:<id>" an enemy's page
    bool mLabelsAbove = false;      // [TIDY FORM 2026-10-17] fText draws its label above the box (one column of boxes of the same width)
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
    /** [SIMPLE STUDIO 2026-10-17] page 2 "Your characters": one simple card per character (SIMPLE_STUDIO_SPEC.md). */
    F32 pageChars(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    // [CHARACTER GALLERY 2026-10-17] Paul: "I would rather have pictures for each character, then you click through to work with
    // that character" - page 2 is a gallery of picture cards (one per character, the enemies under their own head), and a card
    // opens that one's page with the fields. mCharsWho says which is shown.
    /** The gallery: the intro line, Add someone, Take the pads away, the owner tick, the cards, the Enemies cards. */
    F32 charsGallery(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** One card of the gallery, `cw` wide, its top-left at (l, top): the picture, the name, one line, the look badge. Returns its height. */
    F32 charsCard(F32 l, F32 top, F32 cw, const LLSD& sheet, bool enemy, const WolfGameDraw::Palette& p);
    /** One character's (or enemy's) page: All characters / Previous / Next, the big picture with Set picture..., the fields. */
    F32 charsDetail(F32 l, F32 r, F32 top, S32 at, bool enemy, const WolfGameDraw::Palette& p);
    /** The picture of a character or enemy, `sz` square with its bottom-left at (l, b): the portrait texture when there is one,
     *  else a plate in the colour of their id with their initials and a silhouette outline (so every card has a picture). */
    void charsPortrait(F32 l, F32 b, F32 sz, const LLSD& sheet, bool enemy, const WolfGameDraw::Palette& p);
    /** The portrait texture of a character (castSheet.portrait), or of an enemy that looks like one (enemy.look.kind "cast": that
     *  character's portrait); "" when there is none. */
    std::string charsPortraitId(const LLSD& sheet, bool enemy) const;
    /** A plate colour from an id: FNV-1a over the id picks the hue; 45 % saturation, 28 % lightness, so the palette's light text
     *  reads on it at 4.5:1 or better whatever the hue. */
    static LLColor4 idColour(const std::string& id);
    /** An enemy's look in words: as lookWords for an NPC, "Looks like <character>" for look.kind "cast", "An object from a chest" for "object". */
    std::string enemyLookWords(const LLSD& e) const;
    /** The voice row of a character (the picker, premium voices shown on request; `rate` adds the speed field), for Cast and page 2. */
    F32 voiceRow(F32 l, F32 r, F32 top, S32 cast_index, bool rate, const WolfGameDraw::Palette& p);
    /** A new character from the schema's defaults, appended to the cast and selected on the Cast page. */
    void addCastMember();
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
    /** `remove`: page 2's words ("Remove <name>?" / Remove / Keep) instead of the expert pages' Delete. */
    void askDelete(S32 doc, const std::string& ptr, const std::string& what, bool remove = false);
    /** Write the open field. A new map id and a test flag are written only with Enter (`enter`). */
    void commitEdit(bool enter = false);
    void startEdit(S32 act_index);
    void openPicker(const std::string& title, const std::vector<std::pair<std::string, std::string>>& opts,
                    const std::function<void(const std::string&)>& set, const std::string& empty);
    void pickRef(const std::string& title, const std::string& ptr, const std::string& kind, S32 doc, bool optional);
    std::vector<std::pair<std::string, std::string>> refOptions(const std::string& kind, const std::string& ptr) const;
    std::string castUses(const std::string& cast_id) const;
    /** Every pointer that names the character (outside its own sheet). */
    std::vector<std::string> castUsesList(const std::string& cast_id) const;
    /** The land note from the list's regions[].state (enabled = the holder said yes), for a game opened from a card. */
    void landNoteFromList();
    void gotoProblem(const std::string& path);
    S32 problemsUnder(const std::string& ptr) const;
    std::string problemAt(const std::string& ptr) const;
    // rules
    void loadPreset();
    void rulesSet(const std::string& ptr, const boost::json::value* v);
    bool rulesOverridden(const std::string& ptr) const;
    // test play
    /** A test from `beat` ("" = the first box); `then` runs once the brain has started it (page 3's "Play it now" opens the test page). */
    void testStart(const std::string& beat, std::function<void()> then = std::function<void()>());
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

    // ---- [IN-WORLD STUFF 2026-10-17] IN_WORLD_STUFF_PLAN.md: look pads (page 2), the things a box needs and "Make it for me"
    //      (page 3's box panel), "Clean up the land" (page 3): wolffloateradventurestudioedit.cpp ----
    /** Page 2's top: "Anyone can help dress my characters" (the owner setting lookPadsAnyone), shown once the brain reports it. */
    F32 lookPadsHead(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** The card's look line: "Standard look until you dress them..." / "Dressed like <who>" / "Dressed in what you were wearing". */
    static std::string lookWords(const LLSD& c);
    /** look_pad {adventure, cast, here}: a pad for the character at the author's feet (clicked wearing an outfit, it becomes their look). */
    void lookPad(const std::string& cast_id, const std::string& name);
    /** look_pads_clear {adventure}: every pad of the game goes. */
    void lookPadsClear();
    // ---- [SETUP WALK 2026-10-18] "Let's set up your game!" (GAME_CARD_SPINE_SPEC.md "Change (Paul, ~14:30Z)" and "Change (brain,
    //      setup walk, ~15:00Z)"): get_draft.setup and the setup / setup_rez / setup_skip / setup_undo / setup_clean actions ----
    /** The walk is on: the owner's open game has a setup list that is not complete. */
    bool setupActive() const;
    /** The walk has steps and none is left (the web's _setupOf `complete`). */
    bool setupComplete() const;
    /** The walk's card at the top of Gameplay: Step n of m, the step's words, waiting, the red line, its button, Skip, Back, Clean up. */
    F32 setupCard(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** One walk action; the reply's setup replaces the list, its pack + etag become the draft, its say goes to the status line. */
    void setupDo(const std::string& action, const LLSD& extra, const std::string& step_key, const std::string& busy_word);
    /** While a step is being put down or waits in the inventory, `setup` is asked again every few seconds (Gameplay showing). */
    void setupPollTick();
    /** After a game is made: `setup` read, then Gameplay with the walk when it has one to do, else `or_page`; `made` starts the words. */
    void setupAfterMade(const std::string& adv, S32 or_page, const std::string& made);
    /** [WORDING 2026-10-18] get_draft read once for its `pads` (the brain's own count of the pads standing), after pads change. */
    void padsRefresh();
    /** owner_settings {lookPadsAnyone}: whether anyone (not only the author) may click a pad to dress a character. */
    void lookPadsAnyone(bool on);
    /** While pads are out: the draft read again now and then, so a pad clicked in the world shows on the card. */
    void padsTick();
    /** Page 3's box panel: "Things to build here" (box_things), each with Copy the script and Make it for me (Meshy). */
    F32 boxThings(F32 l, F32 r, F32 top, const std::string& beat_id, const WolfGameDraw::Palette& p);
    void loadBoxThings(const std::string& beat_id);
    /** mesh_quote {adventure, thing, prompt}: the cost of making the thing, then the inline question. */
    void meshQuote(const std::string& thing_id, const std::string& name);
    /** "Yes, make it": mesh_start {job, confirm:true}; "No": the quoted job is cancelled (nothing charged). */
    void meshStart();
    void meshNo(bool say = true);
    void meshPoll();
    void meshTick();
    /** land_cleanup: asked once per game whether the brain has it (an empty confirm never cleans anything); the button shows only then. */
    void landCleanupTick();
    /** "Clean up the land": the typed question (the game's name), then land_cleanup {adventure, region, confirm}. */
    void landCleanupAsk();
    void landCleanup();
    bool landCleanupShown() const { return mCleanupKnown == 1 && !mOpenId.empty() && openIsOwner(); }

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
    /** One entry per wizard step, done or not, read from the draft itself (the wizard's own summary). */
    std::vector<bool> wizardChecks() const;
    /** Open the wizard at that step (page 1's "Make it box by box, with no AI"). */
    void openWizardStep(S32 step);
    void loadTemplates();
    void wizCreate();
    /** A beat's "done when" trigger: the first of its triggers that leads on (next, or a goto / end step); -1 when none. */
    S32 doneTrigger(const LLSD& beat) const;
    void setDoneWhen(const std::string& beat_ptr, const std::string& on);
    void removeStoryBeat(const std::string& beat_ptr);
    void placeAreaHere(const std::string& area_id);
    std::string newPackId(const std::string& base) const;
    void rememberWizStep();

    // ---- [SIMPLE STUDIO 2026-10-17] pages 1 and 3, the helper and the flowchart: wolffloateradventurestudioflow.cpp ----
    /** Page 1 "Your story": the story box or the loaded book, Build my game, its inline cost question and progress, the games to continue. */
    F32 pageYourStory(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** Page 3 "Your story map": the instruction band (Next place, Walk with me, Play it now), the flowchart, the box panel, the helper. */
    F32 pageMap(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** The games to continue, as cards (page 1). */
    F32 continueCards(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** The slim helper column of page 3: the last lines, the box (when no flowchart box is selected), Send, Undo that, today's cost. */
    F32 helperColumn(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** A text field several lines high: the text wrapped, its last lines shown, the caret at the end. */
    void fieldMulti(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, EField which, const WolfGameDraw::Palette& p);
    F32 drawFlow(F32 l, F32 r, F32 top, F32 h, const WolfGameDraw::Palette& p);
    F32 flowBoxPanel(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void flowLayout();
    void helperSend();
    void helperUndo();
    void helperHistory();
    void helperApply(const LLSD& reply);
    /** STORY_FIRST_DESIGN.md §4: a finished build merged into the draft (job_apply, never destroying the author's own work);
     *  false when it could not be sent now (a save still out). Done, the Studio goes to page 2. */
    bool jobApply();
    /** §9: the helper NPC walks with the author (helper_walk, with where they stand), or goes. */
    void helperWalk(bool on);
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
    // [SIMPLE STUDIO 2026-10-17] Paul: "i can't read the text on the box it's tiny - i need to be able to move the boxes around"
    /** The chart zoomed by `factor` with the view point (x, y) staying put (the wheel's point, or the frame's middle for + / -). */
    void flowZoomAt(F32 factor, S32 x, S32 y);
    /** Boxes at their natural size (zoom 1, never smaller: the small font stays readable), the chart's top left in view. */
    void flowFit();
    /** A box's dragged position written into the draft (top-level `flow` map, adventure_pack_v1.schema.json) - one save per drag. */
    void flowSavePos(const std::string& key, F32 x, F32 y);
    /** "Tidy up": every saved position goes, the boxes are laid out automatically again. */
    void flowTidy();
    // ---- [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md sections 4-5: the game card, the main quest, ready games, imports ----
    /** The open game is "Just roleplay" (adventure.card.shape roleplay): no goal to win, no main quest, no Win / Lose lines. */
    bool isRoleplay() const;
    /** The game card at the top of pages 2 and 3: You are / Goal (What it's about) / Win / Lose / how long / players / kind of
     *  story, each written into adventure.card as it is typed (the usual draft save). Returns the y under it. */
    F32 gameCard(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** Page 1: "Or start from a ready game" - one card per starter (templates), click = create from it (free) -> page 2. */
    F32 readyGames(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void startFromTemplate(const std::string& id, const std::string& title);
    /** Page 1: "Start from something you built" - the two imports, their cost question and their progress card. */
    F32 importSection(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** Page 1, a build whose job asks (state "asking", output.plan): the shape cards, three questions, Build chapter 1. */
    F32 choosePanel(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void choosePick(const LLSD& plan, const std::string& shape_id);
    void chooseAnswer(bool you_choose, S32 max_credits = 0);
    /** Page 3: the chapters not built yet (acts[].built false), the next one's quote, then its build (one act pass). */
    std::string nextUnbuiltChapter(std::string* title = nullptr) const;
    S32 unbuiltChapters() const;
    void chapterQuote();
    void chapterStart();
    /** Page 3's Publish: the check first (validate), the problems in plain words with Show me, else on to the Publish page. */
    void publishCheck();
    F32 publishCheckCard(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    /** "Show me": the box a problem's path is on selected on the map (its side paths opened), else its field opened. */
    void flowShowMe(const std::string& path);
    /** The drawn kind icon (place, character, challenge, item, gate) centred on (cx, cy), `s` across. */
    static void kindIcon(const std::string& kind, F32 cx, F32 cy, F32 s, const LLColor4& c);
    static std::string kindWord(const std::string& kind);
    /** A box's kind when the beat names none: the brain's wa_beat_kind rule (GAME_CARD_SPINE_SPEC.md section 1), for drawing only. */
    static std::string beatKind(const LLSD& beat);
    /** [WORDING 2026-10-18] A way on (trigger) and a condition in plain words, as the web Studio's triggerWords / condWords. */
public:
    static std::string triggerWords(const LLSD& t, const LLSD& pk);
    static std::string condWords(const LLSD& c, const LLSD& pk, S32 depth = 0);
private:
    struct FlowNode
    {
        std::string mPtr, mId, mTitle, mPlace;   // a beat's pointer and id, or an ending's (mEnding)
        std::string mWhere;                      // [SIMPLE STUDIO] the story's own hint of where it happens (the scene's gmNotes "Where: ...")
        bool mEnding = false, mPlaced = false, mStart = false;
        // [GAME CARD 2026-10-18] beat main / kind, ending kind, acts[].built (GAME_CARD_SPINE_SPEC.md section 1)
        bool mMain = false, mBuilt = true, mHidden = false;
        S32 mStep = 0;                           // a main quest step's number (1..m), 0 for a side box or an ending
        S32 mOwner = 0;                          // a side box: the main step whose "+N side paths" it is under (1..m), 0 none
        S32 mSides = 0;                          // a main step: how many side boxes are under it
        std::string mKind;                       // place | character | challenge | item | gate; an ending: win | lose | other
        std::string mObjective;                  // a main step's objective, drawn under it
        std::string mFight, mFightId;            // [M4 2026-10-17] the box starts a fight: its label and battle id (onEnter battle)
        S32 mLayer = 0, mRow = 0;
        F32 mX = 0.f, mY = 0.f;                  // the box's top left in chart units (before pan and zoom)
    };
    // mLabel = the arrow's words on the map (short, cut with "..."); mFull = the same words whole, for the box panel's "Leads to"
    struct FlowEdge { S32 mFrom = -1, mTo = -1; std::string mLabel, mFull, mCond; bool mDashed = false; };   // mCond: [WORDING 2026-10-18] "Only if" words
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
    std::string mFlowDragKey;                    // [SIMPLE STUDIO] a box being dragged to move it (its key; "" = none)
    S32 mFlowDragX = 0, mFlowDragY = 0;          // where the mouse went down on it
    F32 mFlowDragOrigX = 0.f, mFlowDragOrigY = 0.f;   // the box's chart position then
    bool mFlowDragMoved = false;                 // it moved (3 px or more): a drag, not a click
    S32 mFlowMouseX = 0, mFlowMouseY = 0;
    U32 mFlowFitGen = 0;                         // [SIMPLE STUDIO] the chart is fitted to its frame whenever its boxes or its frame change
    F32 mFlowFitW = 0.f, mFlowFitH = 0.f;
    bool mBoxMore = false;                       // [SIMPLE STUDIO] the box panel's "More...": the full box tools shown
    // ---- [GAME CARD 2026-10-18] ----
    bool mFlowMainMode = false;                  // the draft has main quest steps: the thick line, side paths hidden behind "+N"
    bool mFlowShowAll = false;                   // "Show everything": every side path shown
    std::set<std::string> mFlowOpenSteps;        // the main steps (beat ids) whose side paths are open
    std::string mFlowSelEnd;                     // an ending the author clicked (its id): the ending's small panel
    LLSD mPublishCheck;                          // the last check's problems shown on page 3 (undefined = none shown)
    bool mPublishChecking = false;
    std::string mPublishCheckError;              // the check itself failed: red text in the card
    std::string mChapterFor;                     // "<game>|<act id>|<draft gen>" the quote below was asked for
    std::string mChapterJob;                     // the quoted chapter build (draft_start {adventure, chapter}), state quoting
    S32 mChapterCredits = -1;                    // its quote, rounded up (-1: not known)
    bool mChapterBusy = false;
    std::string mChapterError;                   // red text in the band; stays until the next try
    bool mBuildChapter = false;                  // the running build is one chapter: it lands on the story map, not page 2
    std::string mBuildChapterTitle;              // that chapter's title (the progress card)
    // "Choose your game" (the job's plan)
    std::string mPlanFor;                        // the job the answers below were filled for
    std::string mPlanShape;                      // the chosen card's id ("" none chosen yet)
    std::string mPlanYouAre, mPlanWin, mPlanLength;
    bool mAnswerBusy = false;
    std::string mAnswerError;
    S32 mAnswerRequote = 0;
    bool mAnswerYouChoose = false;               // the last answer was "Let us pick" (a requote re-sends it as it was)
    std::string mAnswerSentFor;                  // the job answered last (a poll still saying "asking" for it is an older reply)
    std::string mFlowAutoOpenedFor;              // the selected box whose side paths were opened for it (once, so Hide works)
    U32 mPublishCheckGen = 0;                    // the draft generation the check above was made for
    S32 mJobPollFails = 0;                       // reads of the build's progress that failed in a row (5: given up, said)                    // reads of the import's progress that failed in a row                      // job_answer refused with requote {credits}: the whole build's new most-credits to send
    bool mChapterNeedBook = false;
    std::string mChapterApplyJob;                // a chapter built (and paid for) but not put in: "Put it into my game" tries again               // the chapter quote was refused with needBook: the book must be loaded again (page 1)
    // ready games
    std::string mTemplateMaking;                 // a starter being made (its id), the card says so
    std::string mReadyError;                     // red text under the ready games; stays until the next click
    // imports (GAME_CARD_SPINE_SPEC.md section 7). [IMPORT WORLDS 2026-10-18] the flow itself is shared with the Import worlds
    // window (wolfimportflow.cpp); the Studio only draws it. Made in the constructor; an obstacle course makes a game by default here.
    std::shared_ptr<WolfImportFlow> mImportFlow;
    std::string mFlowSel;                        // the box the author clicked: its beat id (the helper's `node`)
    std::string mHelperBox;                      // what the author is typing to the helper
    LLSD mHelperLog;                             // [{who: "you"|"helper", text}]
    std::string mHelperQuestion, mHelperError;
    std::string mHelperScript;
    LLSD mLivePreview;                                  // [M7] the last update_preview reply (undefined: none shown)
    bool mLiveUndo = false, mLiveBusy = false;          // [M7] the preview is of "Undo the last update"; a call is out
    std::map<std::string, std::string> mLiveMoves;      // [M7] removed box id -> the box its players go to (author's pick)   // [M6 2026-10-17] LSL the helper wrote on request (reply `script`), for the author to copy
    F64 mHelperTodayCredits = -1.0;              // today's helper cost in credits (-1: not read yet), page 3's one cost line
    bool mHelperBusy = false, mHelperLoaded = false, mHelperMissing = false;
    // [WOLF STORY FIRST M3 2026-10-17] the helper in the world (STORY_FIRST_DESIGN.md §9, §9.1)
    bool mHelperWalking = false;                 // helper_walk on, as the brain last said
    std::string mHelperWalkRegion;               // the region it walks on (a region change re-sends helper_walk)
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
    // ---- [IN-WORLD STUFF 2026-10-17] ----
    bool mPadBusy = false;          // look_pad / look_pads_clear out
    bool mPadsOut = false;
    S32 mPadCount = 0;
    LLSD mSetup;                    // [SETUP WALK 2026-10-18] get_draft / setup*'s `setup` for the open game (undefined: none)
    std::string mSetupBusy;         // "rez" | "skip" | "undo" | "clean" while an action is out ("" none)
    std::string mSetupError, mSetupErrorFor;   // the brain's refusal (red, until the next press) and the step it is about
    F64 mSetupPollAt = 0.0;
    bool mSetupPolling = false;
    std::string mSetupWaitKey;      // the step whose piece is being waited for, and since when (a long wait gets a hint)
    F64 mSetupWaitSince = 0.0;
    U32 mSetupSeq = 0;              // bumped by every walk action: a poll sent before one is not taken after it
    S32 mSetupPollFails = 0;        // polls that failed in a row (5: said in red, the poll stops)
    bool mSetupReloadPending = false;   // the walk changed the game while an edit was unsaved: read again once it is saved
    std::string mSetupJustDone;
    F64 mDoneShownAt = 0.0;         // when doneText first showed (it shows for 20 s, then the usual band)     // the game whose walk has just finished: its doneText shows once on Gameplay              // [WORDING 2026-10-18] get_draft `pads`: the look pads this game has standing ("Take the pads away" only then)          // a pad was put down from this Studio: the draft is read again now and then (padsTick)
    F64 mPadPollAt = 0.0;
    bool mPadPolling = false;
    bool mAnyoneBusy = false;       // the owner setting lookPadsAnyone is being saved
    std::string mThingsFor;         // the box mThings were read for ("" = none)
    U32 mThingsGen = 0;             // the draft generation they were read at (a changed draft reads them again)
    LLSD mThings;                   // box_things things[{id, name, why, script, done}]
    std::string mThingsError;       // red text in the section; stays until the next read
    bool mThingsLoading = false, mThingsMissing = false;
    bool mMeshMissing = false;      // mesh_quote answered "unknown action": no Make it for me buttons (never a dead button)
    bool mMeshBusy = false;         // mesh_quote / mesh_start / job_cancel out
    std::string mMeshError;         // red text in the section; stays until the next Make it for me
    LLSD mMeshQuote;                // {job, credits, thing, name}: the inline question (undefined = none)
    std::string mMeshJobId, mMeshThing, mMeshName;   // the thing being made
    LLSD mMeshJob;                  // job {job} reply for mMeshJobId
    F64 mMeshPollAt = 0.0;
    time_t mMeshRunAt = 0;          // when it began (the card's clock)
    std::string mMeshDone;          // "<name> is in your inventory..." (green), until the next Make it for me
    S32 mCleanupKnown = 0;          // land_cleanup: 0 not asked yet, 1 the brain has it, -1 "unknown action" (the button is not shown)
    std::string mCleanupFor;        // the game the probe was sent for
    bool mCleanupBusy = false;
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
    /** Show the Guide docked beside the Studio (a dependent floater that moves with it), or hide it. [SIMPLE STUDIO 2026-10-17]
     *  only by hand (the Studio's Guide button): it never opens on its own, and nothing is remembered. */
    void showGuide(bool on);
    static bool guideShown();
    /** [MINIMIZE 2026-10-17] Minimized to the viewer's standard bar (its title and Restore button); the Studio's face is hidden
     *  meanwhile and comes back exactly as it was (nothing of the view is touched). */
    void setMinimized(bool minimize) override;

protected:
    /** The floater's own minimize button is never shown: the drawn band has its own "-" (same style as Guide and x). */
    void updateTitleButtons() override;

private:
    WolfAdventureStudioView* mView = nullptr;
};

#endif // WOLF_FLOATER_ADVENTURE_STUDIO_H
