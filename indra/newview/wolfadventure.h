/**
 * @file wolfadventure.h
 * @brief WolfViewer: Wolf Adventure, the player's side - the adventure messages the region's hands
 *        send on the wolfGame channel, the `wolfadventure` verbs back, the stage cues, and the
 *        grid's adventure service (wolfadventure_api.php).
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

#ifndef WOLF_ADVENTURE_H
#define WOLF_ADVENTURE_H

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4color.h"

/**
 * [WOLF ADVENTURE P1 2026-10-16] The contract is firestorm/region-terrain-work/wolf-story/spec/
 * HUD_MESSAGES_V1.md (messages §2, what is drawn §3, verbs §4, state rules §5) and
 * BRAIN_API_V1.md (the viewer API §5, §10). P1 handles `quest`, `journal`, `vignette`, `rep` and
 * `stage` (§2.1-2.3, §2.5-2.6); P2 (2026-10-16) adds `talk`, the talk panel (§2.4, §3.2), and
 * `gm`, `sheet5e`, `roll`, `dice`, `turns`, `quiz` are P4/P5's - until those phases they go the
 * way of any type this viewer does not know (logged, ignored), which is what the spec promises an
 * older viewer (HUD_MESSAGES_V1.md §1.1).
 *
 * The messages arrive on the wolfGame channel (GenericMessage "WolfGame", assembled by
 * WolfGame::receivePacket); WolfGame::apply hands every type it does not know to apply() here.
 * The state lives in this object, not in WolfGame, because WolfGame::onRegionChanged clears the
 * game screen and adventures span regions (§5.2).
 */
class WolfAdventure : public LLSingleton<WolfAdventure>
{
    LLSINGLETON(WolfAdventure);
    ~WolfAdventure();

public:
    // ---- §2.1 quest ----
    struct QuestMarker
    {
        bool mOn = false;
        std::string mId, mRegion, mLabel;
        LLVector3 mPos;
    };
    struct Party
    {
        bool mOn = false;
        std::string mId, mLeader;
        S32 mSize = 0, mWaiting = 0;
        bool mReady = false;
    };
    struct Ending
    {
        bool mOn = false;
        std::string mId, mTitle, mText;
    };
    /** [WOLF ADVENTURE P2 2026-10-16] §2.1 `cast`: a story character on the player's region, for
     *  click-to-talk (§3.2). Its agent key is fixed per character (REGION_HANDS_V1.md §4.1). */
    struct CastRef { LLUUID mNpc; std::string mName; };
    /** [WOLF ADVENTURE AUDIT 2026-10-17] HUD_MESSAGES_V1.md §2.1 `look`: the adventure's own logo and panel colours. */
    struct Look
    {
        std::string mLogo;                   // a texture uuid, or ""
        bool mColours = false;               // accent and panel both given
        LLColor4 mAccent, mPanel;
    };
    struct Quest
    {
        std::string mAdv, mSrc, mChapter, mAct, mScene, mSceneTitle, mBeat, mObjective, mPausedText;
        Look mLook;
        QuestMarker mMarker;
        std::vector<std::string> mDone;      // newest first, at most 10
        Party mParty;
        bool mPaused = false, mGm = false, mAi = true;
        Ending mEnded;
        std::vector<CastRef> mCast;          // P2: the talk panel's click-to-talk list
    };

    // ---- [WOLF ADVENTURE P2 2026-10-16] §2.4 talk: the conversation panel's state ----
    struct TalkCite { std::string mTitle, mWhere; };
    struct TalkReply { std::string mId, mLabel; };
    struct Talk
    {
        bool mOn = false;                    // the panel shows
        std::string mAdv, mSrc, mName, mTitle, mPortrait;
        LLUUID mNpc;
        bool mWaiting = false;               // the player clicked; no `open` yet
        bool mTyping = false;                // "<name> is thinking..." (§2.4 typing)
        bool mHasLine = false;
        std::string mText, mEmote;
        bool mAi = false, mVoiced = false;
        std::vector<TalkCite> mCites;
        std::vector<TalkReply> mReplies;     // at most 6 (§3.2)
        bool mFree = false, mGive = false;
        bool mHasMood = false;
        std::string mMoodLabel, mMoodBand;   // band: hostile | wary | neutral | friendly | trusted
        std::string mHint, mNotice;
        std::string mYouSaid;                // the player's own last line, shown above the answer
        std::string mError;                  // a send that did not go: shown until the next action
        F64 mSince = 0.0;                    // when the panel opened (the waiting timeout)
        U32 mSeq = 0;                        // bumped on every change (the panel re-measures)
    };

    // ---- §2.2 journal ----
    struct Chapter { std::string mAct, mTitle, mState; };            // state: done | current | locked
    struct Entry { std::string mKind, mText, mAct; S64 mAt = 0; };   // mAct: optional, "Settled in P1"
    struct Handout { std::string mId, mKind, mTitle, mTexture, mText; };
    struct Row { std::string mLabel, mValue; };
    struct QuizRow { std::string mId, mTitle, mState, mScore; };
    struct Journal
    {
        bool mValid = false;
        S32 mPage = 0, mPages = 1;
        std::string mTitle, mPurpose, mRating, mObjective;
        std::vector<Chapter> mChapters;
        std::vector<Entry> mEntries;
        std::vector<Handout> mHandouts;
        std::vector<Row> mFlags;
        std::vector<QuizRow> mQuizzes;
        std::vector<Row> mCredits;        // role, name
        std::vector<Row> mAttribution;    // work, text
        bool mConversations = false;
    };

    // ---- §2.5 rep ----
    struct Band
    {
        std::string mId, mLabel;
        S32 mFrom = 0;                    // the pack's band start (bands[] only)
        LLColor4 mColour;
        bool mHasColour = false;
    };
    struct Meter
    {
        bool mOn = false;
        std::string mId, mLabel;
        S32 mValue = 0, mMin = -100, mMax = 100, mDelta = 0;
        std::string mWhy;
        bool mHasBand = false;
        Band mBand;                       // the band the value is in now
        std::vector<Band> mBands;         // every band, low to high (optional, "Settled in P1")
    };
    struct Opinion { std::string mId, mName; S32 mValue = 0; };
    struct Rep
    {
        bool mValid = false;
        Meter mHonour;
        std::vector<Meter> mFactions;
        std::vector<Opinion> mCast;
    };

    /** One adventure this player is in (or was sent anything about). */
    struct Adventure
    {
        bool mHasQuest = false;
        Quest mQuest;
        LLSD mQuestRaw;                   // the merged quest message (§2.1 patches merge into it)
        Journal mJournal;
        Rep mRep;
        U32 mSeq = 0;                     // last touched (the objective line shows the newest)
        U32 mFirst = 0;                   // arrival order: the Journal's chips keep it
    };

    // ---- §2.3 vignette ----
    struct Option { std::string mId, mLabel, mDetail; S32 mTally = -1; };
    struct Card
    {
        std::string mAdv, mSrc, mId, mTitle, mText, mImage, mSpeaker, mPortrait, mWho;
        std::vector<Option> mOptions;
        bool mMayChoose = true;
        F32 mSec = 0.f;                   // the decision's whole time (the bar's scale)
        F64 mEndsAt = 0.0;                // 0 = untimed
        std::string mChosen;              // the option label this player chose, "" until then
        S32 mMoreAsked = 0;               // "More time" presses on this card
        bool mWaiting = false;            // put away by the player: it waits in the Journal (§3.3)
        U32 mSeq = 0;
    };

    // ---- §2.6 stage ----
    enum ECamera { CAM_NONE = 0, CAM_LOOK, CAM_PAN };

    // ---- [WOLF ADVENTURE P4 2026-10-16] §2.9 roll and dice, §2.10 turns, §2.11 quiz, §2.7 gm ----
    struct Roll
    {
        bool mOn = false;
        std::string mAdv, mSrc, mId, mLabel, mCheck, mDice;
        S32 mAdvantage = 0;               // 1 advantage, -1 disadvantage, 0 neither
        bool mHasDc = false;
        S32 mDc = 0;
        bool mPublic = true;
        F32 mSec = 0.f;
        F64 mEndsAt = 0.0;                // 0 = untimed
        LLUUID mWho;                      // the agent who presses Roll
        std::string mWhoName;             // resolved for "Waiting for <name> to roll"
        bool mSent = false;               // this player pressed Roll (one press; the brain rolls once)
        U32 mSeq = 0;
    };
    struct Die { S32 mSides = 0, mValue = 0; bool mKept = true; };
    struct Dice
    {
        bool mOn = false;
        std::string mAdv, mSrc, mId, mLabel, mByName, mOutcome, mNote;
        LLUUID mBy;
        std::vector<Die> mDice;
        S32 mMod = 0, mTotal = 0;
        bool mHasDc = false;
        S32 mDc = 0;
        bool mPublic = true;
        F64 mShownAt = 0.0;               // §3.9 "a card in the same place for 4 s"
        U32 mSeq = 0;
    };
    struct TurnRow
    {
        std::string mKey, mName, mSide;
        S32 mInit = 0;
        bool mYou = false;
        bool mHasHp = false;
        S32 mHp = 0, mHpMax = 0;
        std::vector<std::string> mCond;
        std::string mState;                  // [WOLF STORY FIRST M4 2026-10-17] battles: ok | defending | down | fled
    };
    struct TurnTarget { std::string mKey, mName; S32 mHp = 0, mHpMax = 0; };
    struct Turns
    {
        bool mOn = false;
        std::string mAdv, mSrc, mCurrent;
        S32 mRound = 0;
        std::vector<TurnRow> mOrder;
        // [WOLF STORY FIRST M4 2026-10-17] COMBAT_V1.md / wolfadventure_combat_lib.php wab_show: a battle's name and last
        // line, this player's healing draughts, and - on their own turn - the moves allowed and the enemies to aim at
        std::string mLabel, mLine;
        S32 mDraughts = 0;
        std::vector<std::string> mActions;
        std::vector<TurnTarget> mTargets;
        bool mActSending = false;
        F64 mActAt = 0.0;                    // [review] when the move went (mActSending gives up after 20 s)
        F32 mSec = 0.f;
        F64 mEndsAt = 0.0;
        U32 mSeq = 0;
    };
    struct QuizChoice { std::string mId, mLabel; };
    struct QuizItem
    {
        std::string mId, mType, mPrompt;  // type: choice | text | essay
        std::vector<QuizChoice> mChoices;
        S32 mMax = 1;
    };
    struct Quiz
    {
        bool mOn = false;
        std::string mAdv, mSrc, mId, mTitle;
        std::vector<QuizItem> mItems;
        std::map<std::string, std::string> mAnswers;   // item id -> answer (choice ids comma-separated, or text)
        std::map<std::string, bool> mSentOk;           // item id -> saved by the brain
        S32 mIndex = 0;
        bool mHasResult = false;
        S32 mScore = 0, mScoreMax = 0;
        bool mPassed = false;
        std::map<std::string, S32> mMarked;            // item id -> 1 correct, 0 wrong
        std::string mError;
        bool mSending = false;
        U32 mSeq = 0;
    };
    struct GmArea { std::string mId; bool mCircle = true; LLVector3 mCentre; F32 mRadius = 0.f; std::vector<LLVector3> mPoly; };
    struct GmPath { std::string mId; std::vector<LLVector3> mPoints; };
    struct GmNpc { LLUUID mNpc; std::string mHandle, mName, mPossessedBy; };
    struct GmLayers
    {
        std::string mAdv, mSession;
        bool mHasAreas = false, mHasPaths = false, mHasNpcs = false, mHasBudget = false;
        std::vector<GmArea> mAreas;
        std::vector<GmPath> mPaths;
        std::vector<GmNpc> mNpcs;
        LLSD mBudget;
        U32 mSeq = 0;
        F64 mAt = 0.0;                                 // the last message for it (applyGm drops the oldest)
    };

    // ---- receiving (WolfGame::apply) ----
    /** One assembled message whose `t` WolfGame does not know: true when it was an adventure type
     *  this phase handles (the screen changed). */
    bool apply(const std::string& t, const LLSD& m);

    /** Once a frame, before the camera is updated (WolfGame::idle, llappviewer.cpp idle). */
    void idle();
    /** Escape (WolfGame::handleEscape), in the order of HUD_MESSAGES_V1.md §5.5. */
    enum EEscape
    {
        ESC_FOCUSED_CARD = 0,   ///< the card window, when it has the keyboard
        ESC_STAGE_FIRST,        ///< the stage, never blocked by a text field - when no card is up
        ESC_CARD_THEN_STAGE     ///< the top card, else the stage
    };
    bool handleEscape(EEscape step);
    /** Logout, or off Wolf Territories: everything goes (§5.1, §5.3). */
    void clearAll(bool logout = false);

    // ---- §4 viewer -> region verbs (GenericMessage "wolfadventure") ----
    // Each returns false when nothing was sent (no region, the region not confirmed Wolf, §1.2).
    bool sendHello();
    bool sendTalk(const LLUUID& npc);
    bool sendReply(const LLUUID& npc, const std::string& reply_id);
    bool sendSay(const LLUUID& npc, const std::string& text);
    bool sendTalkEnd(const LLUUID& npc);
    bool sendGive(const LLUUID& npc, const std::string& item, S32 qty);
    bool sendChoose(const std::string& card, const std::string& option);
    bool sendMore(const std::string& card);
    bool sendJournalPage(const std::string& adv, S32 page);
    bool sendReady(const std::string& adv, bool ready);
    bool sendRoll(const std::string& roll);
    bool sendEndTurn(const std::string& adv);
    bool sendCallGm(const std::string& adv, const std::string& text);
    bool sendGmSay(const LLUUID& npc, const std::string& text);
    bool sendGmMove(const LLUUID& npc, const LLVector3& pos, const std::string& gait);
    bool sendGmAnim(const LLUUID& npc, const std::string& anim, bool on);
    /** §1.2: the most text a verb carries, 3 parameters of 240 bytes. */
    static const size_t MAX_TEXT_BYTES = 720;
    /** At most `max_bytes` of `text`, cut on a UTF-8 character boundary. */
    static std::string cutUtf8(const std::string& text, size_t max_bytes);

    // ---- the state, for the HUD, the Journal and the cards ----
    U32 generation() const { return mGeneration; }
    const std::map<std::string, Adventure>& adventures() const { return mAdventures; }
    const Adventure* find(const std::string& adv) const;
    /** The adventures with a quest, in arrival order (the Journal's selector). */
    std::vector<std::string> playing() const;
    /** The adventure the objective line shows: the one whose quest changed last. */
    const Adventure* current() const;

    const std::vector<Card>& cards() const { return mCards; }
    /** The card on top of the stack (not put away), or null. */
    const Card* topCard() const;
    /** Cards with options that wait for the player (put away, or underneath), for the Journal. */
    std::vector<const Card*> waitingCards() const;
    // [WOLF ADVENTURE AUDIT 2026-10-17] a card is found by its adventure AND its id: two adventures may both have a "v-1"
    void choose(const std::string& adv, const std::string& card, const std::string& option);
    bool askMoreTime(const std::string& adv, const std::string& card);
    /** The card's own close: a card with no options goes; one with options waits in the Journal. */
    void putAway(const std::string& adv, const std::string& card);
    /** The Journal's "Waiting for your choice": bring a card back to the top. */
    void bringBack(const std::string& adv, const std::string& card);
    /** "More time" is offered (§3.3: up to 10 times; in a vote, once). */
    static bool moreTimeAllowed(const Card& c);

    // stage: what the HUD draws this frame (0..1)
    F32 letterboxNow() const;           ///< 0 = no bars, 1 = full bars
    F32 fadeNow() const;                ///< 0 = clear, 1 = black
    bool titleOn() const;
    const std::string& titleText() const { return mTitleText; }
    const std::string& titleSub() const { return mTitleSub; }
    /** [WOLF ADVENTURE AUDIT 2026-10-17] HUD_MESSAGES_V1.md §2.6 title `image`: a texture uuid, or "". */
    const std::string& titleImage() const { return mTitleImage; }
    F32 titleAlpha() const;
    void skipTitle();
    /** Every stage effect off (§2.6 clear: bars, fade, title, camera given back); music plays on. */
    void clearStage();
    bool cameraCueRunning() const { return mCamMode != CAM_NONE; }
    bool musicPlaying() const { return !mMusicUrl.empty(); }

    /** The per-account settings (§5.6). */
    static bool storyCamera();
    static bool reduceMotion();
    /** [WOLF ADVENTURE P4 2026-10-16] §5.6 "Character voices" volume, 0..1 (1 = as made). */
    static F32 voiceVolume();
    /** §5.6 "Show AI tag (default on, cannot be hidden in classes)": shown for this adventure? An
     *  education adventure (joined by class code, ADVENTURE_PACK_V1.md §2 purpose) always shows it,
     *  and so does one whose purpose is not known yet (the safe side). */
    bool aiTagShown(const std::string& adv) const;

    // ---- [WOLF ADVENTURE P4 2026-10-16] rolls, dice, turns, quizzes, gm layers ----
    const Roll& roll() const { return mRoll; }
    const Dice& dice() const { return mDice; }
    const Turns& turns() const { return mTurns; }
    const Quiz& quiz() const { return mQuiz; }
    const std::map<std::string, GmLayers>& gmLayers() const { return mGm; }
    /** The Roll button (§3.9): sends `roll` once. False (with the reason in the card) when not sent. */
    bool pressRoll();
    /** §3.10 End turn on the player's own turn. */
    bool pressEndTurn();
    /** [WOLF STORY FIRST M4 2026-10-17] A battle move on the player's own turn (combat_act {adventure, move, target?}). */
    void combatAct(const std::string& move, const std::string& target);
    /** §3.11: one item's answer, kept locally until it is sent. */
    void setQuizAnswer(const std::string& item, const std::string& answer);
    /** Move to item `index` (Previous / Next), sending the item left behind (§3.11 "Answers are sent
     *  per item when the player moves on"). */
    void quizGo(S32 index);
    /** Submit: the current item goes, and the quiz waits for the brain's `result`. */
    void quizSubmit();
    /** [WOLF ADVENTURE AUDIT 2026-10-17] The Journal's open quiz row: the quiz plate again - from memory, else asked for
     *  again (BRAIN_API_V1.md quiz_get), with the answers already saved. */
    void quizReopen(const std::string& adv, const std::string& quiz);
    void quizClose();
    /** The dice card has been shown its time (§3.9 4 s). */
    void diceDone() { mDice.mOn = false; ++mGeneration; }
    /** The name for an agent key (the HUD's lookup), "" when unknown. */
    static std::string agentName(const LLUUID& id);

    // ---- [WOLF ADVENTURE P2 2026-10-16] the talk panel (HUD_MESSAGES_V1.md §2.4, §3.2, §4) ----
    /** A story character of an adventure this player is in, on this region (quest `cast`)? */
    bool isStoryCharacter(const LLUUID& avatar) const;
    /** A left click on an avatar (lltoolpie.cpp): a story character starts a talk (true: the
     *  click is taken); any other avatar is left alone (false). */
    bool talkTo(const LLUUID& avatar);
    const Talk& talk() const { return mTalk; }
    /** The quick reply `index` of the panel. False (and the panel says why) when not sent. */
    bool talkReply(S32 index);
    /** Free text (§1.2: at most 720 bytes). False when not sent; the panel says why. */
    bool talkSay(const std::string& text);
    bool talkGive(const std::string& item, S32 qty);
    /** X, Escape or walking away: `talkend` to the region (§4) and the panel closes. */
    void talkClose();
    /** §4 "say at most one per 1.5 s": seconds until the next say may go (0 = now). */
    F64 talkSayWait() const;
    /** Since the panel's text box last sent. */
    static const F64 SAY_GAP;

    /**
     * [WOLF ADVENTURE P2 2026-10-16] Play a short-lived WAV the adventure service made (a voice
     * preview, the Studio's "Talk to them now"): fetched from the service's own hosts only, played
     * through the viewer's sound engine as wolfspeech.cpp plays /tts. `done` gets "" or the reason.
     */
    static void playClip(const std::string& url, std::function<void(const std::string& error)> done);

    /** The distance from the agent to the quest's marker in metres, or -1 when unknown. */
    static F32 markerDistance(const Quest& q);
    /** Global position of the quest's marker, when known (the wolfGame marker of the same id). */
    static bool markerGlobal(const Quest& q, LLVector3d& out);

    // ---- the grid's adventure service (BRAIN_API_V1.md §5, §10) ----
    typedef std::function<void(bool ok, const LLSD& reply, const std::string& error)> api_callback_t;
    /** POST {action, ...body} to wolfadventure_api.php with this session's identity; `done` runs on
     *  the main loop. Off Wolf Territories it answers at once with an error and sends nothing; between
     *  Wolf regions (not yet isOnWolfTerritoriesConfirmed) the call is held and sent once the region
     *  confirms (or refused, unsent, if it does not within 30 s or turns out not to be Wolf).
     *  A pack goes as body["pack_json"] (its JSON text) and comes back as reply["pack_json"] too,
     *  so its numbers are never narrowed by LLSD. */
    static void api(const std::string& action, const LLSD& body, api_callback_t done);

    // ---- §3.1 Adventures here (the brain's adventures_here, BRAIN_API_V1.md §10.1) ----
    void refreshHere();
    /** The list is for the region the agent is on now (else it is stale: refresh it). */
    bool hereCurrent() const;
    const LLSD& here() const { return mHere; }
    bool hereLoading() const { return mHereLoading; }
    const std::string& hereError() const { return mHereError; }

private:
    void ensureRegion();
    void onRegionChanged();
    void applyQuest(const LLSD& m);
    void applyJournal(const LLSD& m);
    void applyVignette(const LLSD& m);
    void applyRep(const LLSD& m);
    void applyStage(const LLSD& m);
    void applyTalk(const LLSD& m);
    void applyRoll(const LLSD& m);       // [WOLF ADVENTURE P4 2026-10-16/]
    void applyDice(const LLSD& m);
    void applyTurns(const LLSD& m);
    void applyQuiz(const LLSD& m);
    void applyGm(const LLSD& m);
    void sendQuizItem(const std::string& item);
    void talkChanged() { ++mTalk.mSeq; ++mGeneration; }
    void parseQuest(const LLSD& raw, Quest& q) const;
    void dropAdventure(std::string adv);   // by value: callers pass map keys it erases
    Adventure& adventure(const std::string& adv);
    Card* findCard(const std::string& adv, const std::string& id);
    void notice(const std::string& title, const std::string& text, bool good);
    void startCamera(ECamera mode, const LLVector3d& target, const LLVector3d& from, F32 sec);
    void driveCamera(F64 t);
    void endCamera();
    bool cameraAllowed(const std::string& adv) const;
    bool startMusic(const std::string& url);
    /** `restore` false at logout: the parcel's own stream is not started again while the viewer goes. */
    void stopMusic(bool restore = true);
    /** The stage skip: title, bars, fade, camera (never music). True when something was skipped. */
    bool skipStage();
    bool send(const std::vector<std::string>& params);
    bool sendWithText(std::vector<std::string> params, const std::string& text);

    std::map<std::string, Adventure> mAdventures;   // by adventure uuid (lower case)
    std::string mCurrent;                            // the adventure whose quest changed last
    U32 mSeq = 0;
    U32 mGeneration = 1;
    std::vector<Card> mCards;
    Talk mTalk;
    F64 mLastSayAt = -100.0;
    // [WOLF ADVENTURE P4 2026-10-16] §2.7, §2.9-2.11
    Roll mRoll;
    Dice mDice;
    Turns mTurns;
    Quiz mQuiz;
    std::map<std::string, GmLayers> mGm;   // by adventure

    LLUUID mRegionId;
    F64 mHelloAt = 0.0;
    int mOnWolf = -1;

    // stage
    bool mBoxOn = false;
    F32 mBoxFrom = 0.f;                  // the bars' amount when they last started moving
    F64 mBoxChanged = -100.0;            // when the bars started sliding in or out
    F32 mBoxSec = 0.6f;
    F64 mBoxSince = 0.0;                 // when they came on (the 120 s safety, §5.5)
    F32 mFadeFrom = 0.f, mFadeTo = 0.f, mFadeSec = 0.f;
    F64 mFadeStart = 0.0;
    F64 mBlackSince = 0.0;               // fully black since (the 10 s safety, §2.6)
    std::string mTitleText, mTitleSub, mTitleImage;
    F64 mTitleStart = 0.0;
    F32 mTitleSec = 0.f;
    ECamera mCamMode = CAM_NONE;
    LLVector3d mCamPos, mCamFromFocus, mCamFocus;
    LLVector3d mCamLastFocus;            // the focus the cue set last frame (is the camera still ours?)
    F64 mLastDriveAt = 0.0;
    F64 mCamStart = 0.0;
    F32 mCamSec = 0.f;
    F64 mLastCameraCueAt = -100.0;
    std::string mMusicUrl;
    std::string mMusicAdv;               // the adventure whose cue started it

    // Adventures here
    LLSD mHere;
    LLUUID mHereRegion;                  // the region the list is for
    bool mHereLoading = false;
    std::string mHereError;
    U32 mHereSerial = 0;
};

#endif // WOLF_ADVENTURE_H
