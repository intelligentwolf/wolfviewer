/**
 * @file wolffloateradventuregm.h
 * @brief WolfViewer: Wolf Adventure GM Console (STUDIO_AND_CONSOLE_V1.md §3) and the drawn "desk" both
 *        consoles are built on (the Teacher Console, §4, is wolffloateradventureteacher.*).
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

#ifndef WOLF_FLOATER_ADVENTURE_GM_H
#define WOLF_FLOATER_ADVENTURE_GM_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llsd.h"
#include "llsingleton.h"
#include "lltool.h"
#include "lluictrl.h"
#include "v3dmath.h"
#include "wolfadventure.h"
#include "wolffloaterroleplay.h"

namespace WolfGameDraw { struct Palette; }

namespace WolfAdventureDesk
{
    /** A story character's agent key: name-based (MD5) uuid of "wolfadv:" + region + ":" + handle, the
     *  bytes in order with RFC 4122 version 3 / variant bits (REGION_HANDS_V1.md §4.1, settled in P1; the
     *  same as wolfadventure_lib.php wa_npc_uuid). Test vector: region 11111111-2222-3333-4444-555555555555,
     *  handle aaaaaaaa-0000-0000-0000-000000000001:sheriff:2 -> b6aef214-ea4c-303e-9914-c79aa7ec7299. */
    LLUUID npcAgent(const LLUUID& region, const std::string& handle);
    /** A pack read back from the service (reply["pack"]): its places on one region, for the overlays. */
    void packPlaces(const LLSD& pack, const LLUUID& region, std::vector<WolfAdventure::GmArea>& areas,
                    std::vector<WolfAdventure::GmPath>& paths);
}

/**
 * One left click on the ground (or an object) in the world, for the consoles' "Move here", "Place" and the
 * possession plate's "Click to move" (STUDIO_AND_CONSOLE_V1.md §3; §6 2.1.1 Escape cancels). A transient tool:
 * it takes the next click, hands the point back, and gives the previous tool back.
 */
class WolfToolAdventurePick : public LLTool, public LLSingleton<WolfToolAdventurePick>
{
    LLSINGLETON(WolfToolAdventurePick);
public:
    typedef std::function<void(const LLVector3d& global)> pick_callback_t;
    /** Take the next click; `keep` true keeps picking (the possession plate's Click to move) until stop(). */
    void start(pick_callback_t cb, bool keep);
    void stop();
    bool active() const { return mActive; }
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    void handleDeselect() override;

private:
    pick_callback_t mCallback;
    bool mActive = false, mKeep = false;
};

/**
 * The console "desk": a custom-drawn window in the wolfGame family (STUDIO_AND_CONSOLE_V1.md §1: a left
 * rail or tab strip, the page in the centre, a right drawer, a status line where a confirmation fades and an
 * error stays until the next action, questions inside the window with the safe answer focused). The GM and
 * Teacher consoles draw their pages with these parts.
 */
class WolfAdventureDeskView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureDeskView(const Params& p);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;
    bool acceptsTextInput() const override { return mField != 0 && hasFocus(); }
    /** Escape: the question takes it, then a text field lets go of the keyboard, then the window closes. */
    virtual void escape();

protected:
    // the hit ids the desk itself uses; a console's own start at DH_FIRST
    enum EDeskHit { DH_NONE = 0, DH_CLOSE, DH_SCROLL, DH_FIELD, DH_YES, DH_NO, DH_FIRST = 20 };

    /** The console's face: everything under the title band and above the status line. */
    virtual void face(F32 W, F32 H, F32 top, F32 bottom, const WolfGameDraw::Palette& p) = 0;
    virtual void press(S32 id, S32 arg) = 0;
    /** Enter in a text field (a send). */
    virtual void fieldEnter(S32 field) {}
    virtual std::string titleWord() const = 0;
    virtual std::string titleText() const = 0;
    /** The most characters a field holds, and whether Enter makes a new line. */
    virtual size_t fieldCap(S32 field) const { return 500; }
    virtual bool fieldMultiline(S32 field) const { return false; }
    /** A secret (drawn as dots, never pasted out). */
    virtual bool fieldMasked(S32 field) const { return false; }
    virtual void tick() {}

    // ---- the parts ----
    void hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable = true, bool enabled = true);
    U32 flagsFor(S32 id, S32 arg, bool enabled) const;
    void btn(F32 l, F32 b, F32 r, F32 t, const std::string& label, S32 id, S32 arg, const std::string& tip,
             const WolfGameDraw::Palette& p, bool enabled = true, U32 extra = 0);
    F32 head(F32 l, F32 r, F32 top, const std::string& text, const WolfGameDraw::Palette& p);
    F32 para(F32 l, F32 r, F32 top, const std::string& text, const LLColor4& colour, bool small = false, S32 max_lines = 40);
    /** A text field; height `rows` lines (1 = a single line). */
    void field(F32 l, F32 b, F32 r, F32 t, S32 which, const std::string& placeholder, const std::string& tip,
               const WolfGameDraw::Palette& p, bool invalid = false);
    std::string& text(S32 which) { return mTexts[which]; }
    /** A scrolling area: begin() clips to it and returns the content's top; end() stores its height and draws
     *  the scrollbar. `slot` keeps each area's scroll apart. */
    F32 beginScroll(S32 slot, F32 l, F32 b, F32 r, F32 t);
    void endScroll(S32 slot, F32 low, const WolfGameDraw::Palette& p);

    void setStatus(const std::string& text, bool error);
    /** A request to the adventure service; `on_ok` runs only when it answered success (an error goes to the
     *  status line and stays). */
    void call(const std::string& action, const LLSD& body, std::function<void(WolfAdventureDeskView*, const LLSD&)> on_ok,
              const std::string& doing = std::string());
    /** A question inside the window (§1: destructive actions ask, the safe button focused). */
    void ask(const std::string& title, const std::string& body, const std::string& yes, bool danger, std::function<void()> on_yes);

    WolfGameHits mHits;
    bool mKeyboardNav = false;
    S32 mField = 0;                       // the text field with the keyboard (0 = none)
    std::map<S32, std::string> mTexts;
    S32 mBusy = 0;
    std::string mStatus;
    bool mStatusError = false;
    F64 mStatusUntil = 0.0;

private:
    void drawQuestion(const WolfGameDraw::Palette& p);
    struct Scroll { F32 mOffset = 0.f, mContent = 0.f; LLRect mRect; };
    std::map<S32, Scroll> mScrolls;
    S32 mScrollSlot = -1;                 // the one being drawn
    S32 mDragSlot = -1;
    F32 mDragGrabY = 0.f, mDragStart = 0.f;
    std::vector<S32> mClipStack;
    bool mAsking = false;
    std::string mAskTitle, mAskBody, mAskYes;
    bool mAskDanger = false;
    std::function<void()> mAskYesFn;
};

/**
 * [WOLF ADVENTURE P4 2026-10-16] STUDIO_AND_CONSOLE_V1.md §3, the GM Console: the top bar (session, control,
 * Take control / Hand back, Pause / Resume, AI on, the meter), the parties on the left, the tabs in the centre
 * (Story, Cast and spawn, Narrate, Party, Rewards, World, Rules, Assist), the log drawer on the right with
 * Conversations, the possession plate, the overlays and the director camera. Every action is a BRAIN_API_V1.md
 * §8 `gm` op, rights-checked by the brain on every call; the console polls gm_state every 2 s while open.
 */
class WolfAdventureGmView : public WolfAdventureDeskView
{
public:
    // Its own empty param block: LLUICtrlFactory::registerWidget (lluictrlfactory.cpp:273-281) keys widgets
    // by Params type and forces a crash when two registered widgets share one (the GM and Teacher views
    // both used WolfAdventureDeskView::Params -> SIGSEGV at start-up, 2026-10-17).
    struct Params : public LLInitParam::Block<Params, WolfAdventureDeskView::Params>
    {
        Params() {}
    };
    WolfAdventureGmView(const Params& p);
    void onOpened(const LLSD& key);
    void onClosed();

protected:
    void face(F32 W, F32 H, F32 top, F32 bottom, const WolfGameDraw::Palette& p) override;
    void press(S32 id, S32 arg) override;
    void fieldEnter(S32 field) override;
    std::string titleWord() const override { return "GM CONSOLE"; }
    std::string titleText() const override;
    size_t fieldCap(S32 field) const override;
    bool fieldMultiline(S32 field) const override;
    void tick() override;

private:
    enum ETab { T_STORY = 0, T_CAST, T_NARRATE, T_PARTY, T_REWARDS, T_WORLD, T_RULES, T_ASSIST, T_COUNT };
    enum EHit
    {
        H_ADV = DH_FIRST, H_SESSION, H_REFRESH, H_SCHEDULE, H_SESSION_END, H_TAKE, H_LEAVE, H_PAUSE, H_RESUME, H_AI,
        H_OVERLAY, H_DIRCAM, H_PARK, H_PARTY, H_TAB,
        // Story
        H_ADVANCE, H_GOTO_BEAT, H_GOTO_SCENE, H_FIRE, H_FLAG_TOGGLE, H_FLAG_STEP, H_FLAG_SET, H_HONOUR_STEP, H_REP_STEP, H_CASTREP_STEP,
        // Cast and spawn
        H_POSSESS, H_RELEASE, H_REMOVE, H_MOVEHERE, H_PLACE_CAST, H_FIRE_ENC,
        // possession plate
        H_PS_MOVE, H_PS_GAIT, H_PS_ANIM_PLAY, H_PS_ANIM_STOP, H_PS_SUGGEST, H_PS_USE, H_PS_RELEASE,
        // Narrate
        H_NARRATE_TO, H_NARRATE_ADDOPT, H_NARRATE_DELOPT, H_NARRATE_SEND, H_WHISPER_TO, H_WHISPER_SEND,
        // Party
        H_GATHER, H_READY_CHECK, H_END_PICK, H_END_PARTY, H_KICK,
        // Rewards
        H_REWARD_TO, H_REWARD_PARCEL, H_REWARD_SEND,
        // World
        H_HOUR_STEP, H_HOUR_SET, H_WEATHER, H_MUSIC_PLAY, H_MUSIC_STOP,
        // Rules
        H_DICE_PUBLIC, H_DICE_ROLL, H_CHECK_ASK, H_COMBAT, H_OVERRIDE,
        H_BATTLE_START, H_ENEMY_MOVE, H_ENEMY_HIT,   // [WOLF STORY FIRST M4 2026-10-17] start a battle (arg: its index); the enemy's move (0 defend, 1 flee); attack fighter arg
        // Assist
        H_ASSIST, H_ASSIST_CAST, H_ASSIST_COPY, H_ASSIST_USE,
        // the log drawer
        H_LOG_FILTER, H_LOG_OLDER, H_CONVO,
        H_FIND_NPCS
    };
    enum EField
    {
        F_NONE = 0, F_FLAG_TEXT = 1, F_REASON, F_PS_SAY, F_PS_ANIM, F_NARRATE_TITLE, F_NARRATE_TEXT, F_NARRATE_IMAGE,
        F_WHISPER, F_REWARD_XP, F_REWARD_MONEY, F_REWARD_ITEM, F_REWARD_QTY, F_MUSIC, F_DC, F_CHECK, F_ASSIST_TEXT,
        F_OPT0 = 40   // F_OPT0 + i: the narrate card's option i label (at most 6, BRAIN §8.1 narrate)
    };

    // drawing
    F32 drawTopBar(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    void drawParties(F32 l, F32 b, F32 r, F32 t, const WolfGameDraw::Palette& p);
    void drawLog(F32 l, F32 b, F32 r, F32 t, const WolfGameDraw::Palette& p);
    F32 drawPossession(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabStory(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabCast(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabNarrate(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabParty(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabRewards(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabWorld(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabRules(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 tabAssist(F32 l, F32 r, F32 top, const WolfGameDraw::Palette& p);
    F32 rowText(F32 l, F32 r, F32 y, const std::string& left, const LLColor4& c);
    void publishOverlay();
    /** The characters standing in the world: gm_state's npcs (BRAIN §8.2) when it has them, else the `gm`
     *  npcs layer the `overlay` op sends (HUD §2.7). Each {handle, cast, name, possessedBy}. */
    LLSD npcList() const;
    /** The last rolls: gm_state's rolls when it has them, else the `dice` entries of the log. Each {id, name, check,
     *  total, outcome}. */
    LLSD rollList() const;
    /** Ask the brain for the gm layers (zones, paths, npcs, budget) of the region the GM stands on. */
    void requestLayers();

    // data
    void loadAdventures();
    void loadSessions();
    void pollState(bool force);
    void loadPack();
    void loadLogOlder();
    void loadConversations();
    void gmOp(const std::string& op, const LLSD& fields, const std::string& doing, std::function<void(WolfAdventureGmView*, const LLSD&)> then = nullptr);
    const LLSD* selectedParty() const;
    std::string selectedAdventure() const;
    /** The pack's lists, read from mPack. */
    const LLSD& packList(const char* key) const;
    const LLSD* packBeat(const std::string& id, std::string* scene_out = nullptr) const;
    std::string castName(const std::string& id) const;
    std::string partyRegion() const;   // the selected party's scene region (uuid), from the pack
    void pick(const std::string& what, std::function<void(WolfAdventureGmView*, const LLVector3d&)> then);
    void stopPossessing();
    void setDirectorCamera(bool on);

    ETab mTab = T_STORY;
    LLSD mAdvs;                       // list_mine (owner or GM or teacher)
    S32 mAdvSel = 0;
    LLSD mSessions;                   // sessions {adventure}
    std::string mSession;
    LLSD mState;                      // gm_state
    std::string mStateError;
    F64 mPollAt = 0.0;
    bool mPolling = false;
    std::string mParty;               // the selected party
    LLSD mPack;                       // get_revision of the session's revision
    std::string mPackFor;             // "<adv>:<rev>" it was read for
    bool mPackLoading = false;
    std::string mPackFailed;          // "<adv>:<rev>" a read was refused for (a region-granted GM): not asked again
    LLSD mOlderLog;                   // gm_log pages below the live 50
    S32 mLogFilter = 0;
    bool mConvoOn = false;
    LLSD mConvo;                      // chatlog {session}
    // possession (§3 the plate)
    std::string mPossessHandle, mPossessName, mPossessCast;
    S32 mGait = 0;                    // walk | run | fly (HUD_MESSAGES_V1.md §4 gm_move gait)
    bool mClickMove = false;
    LLSD mPsLines;                    // the last player lines to this character (chatlog {session, cast})
    F64 mPsLinesAt = 0.0;
    LLSD mPsSuggest;
    // Narrate / whisper / rewards / rules / assist state
    S32 mNarrateOpts = 0;
    S32 mNarrateTo = 0;               // 0 the party, 1.. a member
    S32 mWhisperTo = 0;
    S32 mRewardTo = 0;                // 0 the party, 1.. a member
    S32 mEndPick = 0;                 // 0 no ending, 1.. the pack's endings
    S32 mHour = 12;
    bool mDicePublic = true;
    S32 mAssistAsk = 0;               // 0 next, 1 boxed, 2 reply
    S32 mAssistCast = 0;
    LLSD mSuggest;
    std::string mSuggestCost;
    // overlays and camera
    bool mOvZones = false, mOvPaths = false, mOvNpcs = false;
    bool mDirCam = false;
    // the world overlay is rebuilt only when what it shows changes (publishOverlay); between, it is kept alive
    bool mOverlayDirty = true;        // the state, pack, adventure, session or overlay switches changed
    LLUUID mOverlayRegion;            // the region it was built for
    U32 mOverlayGen = 0;              // WolfAdventure::generation() it was built at (its gm layers)
    std::string mPickWhat;            // what the next world click is for, shown in the status line
};

class WolfFloaterAdventureGm : public LLFloater
{
public:
    WolfFloaterAdventureGm(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    WolfAdventureGmView* mView = nullptr;
};

#endif // WOLF_FLOATER_ADVENTURE_GM_H
