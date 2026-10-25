/**
 * @file wolffloaternpc.h
 * @brief WolfViewer: Wolf World > WolfNPC... - put one of your own WolfAvatars on your land as a living NPC, and change or
 *        remove the NPCs of the region.
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

#ifndef WOLF_FLOATER_NPC_H
#define WOLF_FLOATER_NPC_H

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llfloater.h"
#include "llpointer.h"
#include "llsd.h"
#include "lluuid.h"
#include "v3math.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLPanel;
class LLRadioGroup;
class LLScrollContainer;
class LLScrollListCtrl;
class LLSliderCtrl;
class LLSpinCtrl;
class LLTabContainer;
class LLTextBox;
class LLTextEditor;
class LLViewerTexture;
class WolfFloaterNPC;
class WolfNPCAvatarGrid;
class WolfNPCGame;
class WolfNPCMind;
class WolfNPCBackdrop;
class WolfNPCCardGrid;

/**
 * [WOLFNPC 2026-10-24] The settings form (panel_wolf_npc_settings.xml), used twice: on "New NPC" (filled with the defaults
 * of PROTOCOL.md section 3) and in "On this region" > Edit (filled from op get's cfg). Name and behaviour are always shown;
 * Where, Character, Work and routine (people), Game, AI talk and Vehicle are sections, closed until their heading is pressed.
 * cfg() builds the settings from what is on screen: every key the form shows, objects (area, game, ai, vehicle) merged over
 * the cfg it was filled from, so a key the form does not show is never lost.
 */
class WolfNPCForm
{
public:
    /** [WOLFNPC ROUND 2 2026-10-24] "New NPC" keeps its name and AI talk (switch, personality, price line) on the quick
     *  step, outside More options: the form then uses those widgets and hides its own. */
    struct Quick
    {
        LLLineEditor* mName = nullptr;
        LLCheckBoxCtrl* mAiOn = nullptr;
        LLTextEditor* mPersonality = nullptr;
        LLTextBox* mAiLine = nullptr;
    };
    WolfNPCForm(WolfFloaterNPC* owner, LLPanel* panel, LLScrollContainer* scroll, bool editing, const Quick* quick = nullptr);

    /** animal / person: the behaviour presets and the people-only section follow it. */
    void setKind(const std::string& kind);
    const std::string& kind() const { return mKind; }
    /** Fill every field from a cfg (an empty map gives the defaults); remembers it for cfg() and changedKeys(). */
    void fill(const LLSD& cfg);
    /** The settings shown. Fields the form checks itself (a routine line it cannot read...) set `error` and give false. */
    bool cfg(LLSD& out, std::string& error) const;
    /** Only the keys whose value differs from what fill() put on screen (op update "only the keys given change"). */
    LLSD changedKeys(const LLSD& now) const;
    /** The form's own snapshot, taken right after fill() (what cfg() gave before anything was touched). */
    const LLSD& filled() const { return mFilledCfg; }
    void setName(const std::string& name);
    std::string name() const;
    bool voiceChosen() const;
    bool npcTalkOn() const;
    /** ai.dailyLimit as shown (credits, 0 = none). */
    F64 dailyLimit() const;
    /** The live AI line under the switch (N / X from npc_ai_info; "" while not known). */
    void setAiLine(const std::string& text, bool problem);
    /** npc_ai_info voices: [{id, name}] into the Voice list (the current choice kept). */
    void setVoices(const LLSD& voices);
    /** op vehicles: [{id, name, dist}]. */
    void setVehicles(const LLSD& vehicles);
    /** hello's game {managed, health, mine}: the notes in the Game section. */
    void setGameInfo(const LLSD& game);
    /** "In front of you" (New NPC) or the stored home. */
    void setHomeText(const std::string& text);
    /** [ROUND 4] the Places tab's places (the routine is checked against them). */
    void setPlaces(const LLSD& cfg);
    /** [ROUND C GATE] the pet behaviour and the travel / noise rows follow the region's support. */
    void refreshPetSupport();
    bool gameOnly() const;
    void setEnabled(bool on);
    void relayout();

private:
    void openSection(S32 n, bool open);
    void fillBehaviours();
    void onBehaviourChanged();
    void refreshArea();
    void refreshGame();
    void refreshVehicle();
    void refreshWorkAt();
    void onAddRoutePoint();
    void readRoutine(const LLSD& routine);

    WolfFloaterNPC* mOwner;
    LLPanel* mPanel;
    LLScrollContainer* mScroll;
    bool mEditing;
    std::string mKind = "animal";
    LLSD mBase;                 // the cfg fill() was given
    LLSD mFilledCfg;            // cfg() right after fill()

    static const S32 SECTIONS = 6;   // Where, Character, Work and routine, Game, AI talk, Vehicle
    LLButton* mHead[SECTIONS] = {};
    LLPanel* mBody[SECTIONS] = {};
    bool mOpen[SECTIONS] = {};
    std::string mHeadLabel[SECTIONS];

    LLLineEditor* mName = nullptr;
    LLComboBox* mBehaviour = nullptr;
    LLTextBox* mBehaviourNote = nullptr;
    // Where
    LLTextBox* mHome = nullptr;
    LLComboBox* mAreaMode = nullptr;
    LLSliderCtrl* mRadius = nullptr;
    LLComboBox* mIndoors = nullptr;
    LLCheckBoxCtrl* mSwim = nullptr;   // [ROUND 2] cfg.swim
    LLCheckBoxCtrl* mTravel = nullptr;      // [ROUND C] cfg.travel (pets)
    LLCheckBoxCtrl* mNoises = nullptr;      // [ROUND C] cfg.noises.on (animals)
    LLSliderCtrl* mNoiseVolume = nullptr;   // [ROUND C] cfg.noises.volume
    S32 mTopY = 92;             // where the first section heading goes (64 when the name row is on the quick step)
    // Character
    LLSliderCtrl* mShy = nullptr;
    LLComboBox* mVisitors = nullptr;
    LLTextBox* mPackLabel = nullptr;
    LLLineEditor* mPack = nullptr;
    LLCheckBoxCtrl* mLeader = nullptr;
    // Work and routine
    LLComboBox* mWork = nullptr;
    LLTextBox* mWorkAtText = nullptr;
    LLTextEditor* mRoutine = nullptr;
    LLCheckBoxCtrl* mGameOnly = nullptr;   // [ROUND 4] cfg.gameOnly (PROTOCOL.md §24)
    static const S32 GAME_TOP = 90;        // the Game section's height above its details (panel_wolf_npc_settings.xml body_3)
    LLSD mPlacesMap;            // name -> [x,y,z]
    bool mHasWorkAt = false;
    LLVector3 mWorkAt;
    // Game
    LLCheckBoxCtrl* mFight = nullptr;
    LLTextBox* mGameNote = nullptr;
    LLPanel* mGameDetails = nullptr;
    LLCheckBoxCtrl* mHuntable = nullptr;
    LLSpinCtrl* mHealth = nullptr;
    LLSpinCtrl* mRespawn = nullptr;
    LLCheckBoxCtrl* mHunt = nullptr;
    LLSpinCtrl* mSight = nullptr;
    LLSpinCtrl* mChase = nullptr;
    LLSpinCtrl* mGiveUp = nullptr;
    LLSpinCtrl* mCooldown = nullptr;
    static const S32 DAMAGE_ROWS = 4;   // PROTOCOL.md section 3 game.damage "up to 4 stats"
    LLLineEditor* mDamageStat[DAMAGE_ROWS] = {};
    LLSpinCtrl* mDamageAmount[DAMAGE_ROWS] = {};
    // AI talk
    LLCheckBoxCtrl* mAiOn = nullptr;
    LLTextBox* mAiLine = nullptr;
    LLTextEditor* mPersonality = nullptr;
    LLComboBox* mVoice = nullptr;
    LLSpinCtrl* mDailyLimit = nullptr;
    LLCheckBoxCtrl* mNpcTalk = nullptr;
    // Vehicle
    LLComboBox* mVehicle = nullptr;
    LLCheckBoxCtrl* mRoads = nullptr;
    LLCheckBoxCtrl* mLoop = nullptr;
    LLTextBox* mRouteText = nullptr;
    LLSpinCtrl* mVehicleSpeed = nullptr;
    LLCheckBoxCtrl* mPassenger = nullptr;
    LLTextBox* mPassengerNote = nullptr;
    std::vector<LLVector3> mRoute;
    LLSD mVehicles;             // the last op vehicles list
};

/**
 * [WOLFNPC 2026-10-24] Paul: "people can manage an NPC on their region through the viewer".
 * Contract: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md - sections 1 (who may do what), 2 (the ops, through
 * WolfNPC in wolfnpc.cpp), 3 (cfg), 5 (npc_avatars, npc_picture, npc_ai_info, npc_scenes through WolfAdventure::api),
 * 6 (reactions), 7 (scenes), 8 (animations), 9 (help text only), 10 (vehicles).
 *
 * Who and what: a Wolf region owner wants to put one of their own WolfAvatars on their land as a living NPC - a wolf that
 * roams, a bartender at the bar - in under two minutes, and later change or remove it, without scripting.
 * Flow: Wolf World > WolfNPC... -> "New NPC": 1. pick one of YOUR WolfAvatars (pictures) -> 2. name + what it does ->
 * (optional sections, closed) -> Rez: it appears 2 m in front of you, facing you. "On this region": the region's NPCs,
 * Edit / Rename / Pause / Call home / Make here its home / Delete, animations and reactions. "Scenes": a ready-made scene
 * walked through piece by piece ("stand there, press Rez"). Every failure shows in the window, in its own words.
 * Wolf Territories only.
 */
class WolfFloaterNPC : public LLFloater
{
    friend class WolfNPCForm;
    friend class WolfNPCBackdrop;
public:
    WolfFloaterNPC(const LLSD& key);
    ~WolfFloaterNPC() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;
    /** [ROUND 4 LOOK] The Campfire, painted by WolfNPCBackdrop under every control (floater-local coordinates). */
    void drawScene();

    /** Shown in the window's own line (failures stay until the next action; never only in the log). */
    void setError(const std::string& text, const LLSD& reply = LLSD());
    void setDone(const std::string& text);
    void clearStatus();

private:
    // ---- [ROUND 4 LOOK] the painting ----
    enum EButtonRole { BTN_STONE = 0, BTN_PRIMARY, BTN_DANGER, BTN_CARD, BTN_HEAD };
    struct Painted
    {
        LLButton* mButton = nullptr;
        EButtonRole mRole = BTN_STONE;
        LLView* mClip = nullptr;   // the scroll container it sits in (its face is clipped to it), or null
    };
    /** Every button of the window's own (not the skin's inner ones: tabs, scroll bars, combos, spinners, check boxes). */
    void collectPainted(LLView* view);
    void paintButton(const Painted& p, F32 t);
    void drawBanner(F32 t);
    /** Fixed label colours for check boxes, radio buttons, sliders and spinners (they sit on the painted plates). */
    void recolourControls(LLView* view);
    std::vector<Painted> mPainted;
    WolfNPCBackdrop* mBackdrop = nullptr;
    WolfNPCCardGrid* mCards = nullptr;
    LLScrollContainer* mCardScroll = nullptr;
    LLPanel* mCardHolder = nullptr;
    std::map<std::string, std::string> mJobOf;   // NPC id -> the WolfAvatars job it was made from (its picture on the card)
    bool mQuestionDanger = false;                // the question's yes button deletes or removes something
    bool mStatusError = false;                   // the status line holds a failure (its plate is red, with a mark)

    // ---- the shared parts ----
    void onTabChanged();
    void onRegionChanged();
    void sendHello();
    void onChangedPush(const std::string& id);
    /** A question in the window (never a modal box): `yes_label` far from Cancel; Cancel has the keyboard. */
    void ask(const std::string& text, const std::string& yes_label, std::function<void()> yes);
    void closeQuestion();
    void onBuyCredits();
    void openWolfAvatars();
    /** npc_ai_info, then (when AI is being turned on) the question; `go` runs on Turn on AI talk. */
    void confirmAi(const std::string& npc_name, const WolfNPCForm* form, std::function<void()> go);
    void loadAiInfo(WolfNPCForm* form);
    /** The live AI line under the switch, from the last npc_ai_info reply (the voice chosen decides which price). */
    void showAiLine(WolfNPCForm* form);
    /** op vehicles, into that form's Vehicle list. */
    void requestVehicles(WolfNPCForm* form);
    /** Where a new NPC goes: 2 m in front of the agent along the flat facing, facing back towards them. */
    bool placeInFront(LLVector3& pos, F32& facing, LLVector3& agent_pos) const;

    // ---- New NPC ----
    void loadAvatars();
    void loadPicture(const std::string& job);
    void onAvatarPicked();
    void onRez();
    void doCreate(const LLSD& fields, const std::string& name);

    // ---- On this region ----
    void requestList();
    void fillList();
    void refreshActions();
    LLSD selectedNpc() const;
    void sendCmd(const std::string& id, const std::string& cmd, const std::string& value, const std::string& done_words);
    void onEdit();
    void onRename();
    void onRenameSave();
    void onPauseResume();
    void onDelete();
    void showDetail(bool on);
    void requestGet(const std::string& id);
    void applyGet(const LLSD& reply);
    void onSaveSettings();
    /** `settings`: the Settings tab's Save (the form is filled again from the reply); else the reactions only. */
    void doUpdate(const LLSD& cfg_changes, const std::string& done_words, bool settings,
                  std::function<void(const LLSD& cfg)> after = nullptr);
    void fillAnims();
    void onAnim(const std::string& cmd);
    // reactions
    void fillRules();
    void editRule(S32 index);
    void closeRuleEditor();
    void onRuleWhoChanged();
    void onRuleOftenChanged();
    void onRuleActionChanged();
    void onRuleAddAction();
    void onRuleRemoveAction();
    void fillRuleActions();
    void onRuleSave();
    void onRuleRemove();
    void saveRules(const LLSD& rules, const std::string& done_words);
    void requestPeople();

    // ---- Scenes ----
    void loadScenes();
    void onSceneSelected();
    void onSceneStart();
    void showSceneStep();
    void onSceneRez();
    void onSceneSkip();
    void onSceneStop();
    /** [ROUND 2] the scene's cost line before Rez (AI talk is off in every scene: no credits). */
    void showSceneCost();

    // ---- [ROUND 2] New NPC in 3 clicks ----
    enum ENewStep { NEW_START = 0, NEW_PICK, NEW_QUICK };
    void showNewStep(ENewStep step);
    /** [ROUND C] "Make a pet": animals only, Follow me everywhere, Rez beside the owner. */
    void setPetMode(bool pet);
    /** [WOLF PETS 2026-10-24] PROTOCOL.md §29.4 Make a pet's first choice: Wear it (true, the default) or Let it roam (the §26
     *  create, offered only where petsSupported()). Shows the quick step's rows for that choice. */
    void setWearPet(bool wear);
    /** [WOLF PETS] Wear it: the brain's wolfpet_make {job, name, side, distance} (§29.2), the answer shown in the window. */
    void onWearMake();
    /** [ROUND C GATE] the region's hello carries "rules" (PROTOCOL.md §26.5): pets, noises and their rules are offered. */
    bool petsSupported() const;
    void applyPetSupport();
    void buildGrid();
    void showRules();
    void sendRules(bool parcel);
    void onCreateNew();
    /** LLFloaterWolfAvatars told us a character was made (its job, characterKind, name). */
    void onCharacterDone(const std::string& job, const std::string& kind, const std::string& name);
    void onNewRez();
    /** The most this NPC can spend in a day at its settings (npc_ai_info prices; capped by its daily limit). */
    F64 dailyMax(const WolfNPCForm* form) const;
    std::string pounds(F64 credits) const;
    /** [ROUND B] the web look-up price sentence for the AI line ("" until npc_ai_info has it). */
    std::string lookupWords() const;
    /** [ROUND B] a scene piece's "brain" {teacher, lookups, styles}, applied after its create (PROTOCOL.md §7). */
    void queueBrain(const std::string& npc, const std::string& name, const LLSD& brain);
    void applyBrain(size_t index);
    struct PendingBrain
    {
        std::string mNpc, mName;
        LLSD mBrain;
        S32 mTries = 0;
        F64 mAt = 0.0;
    };
    std::vector<PendingBrain> mPendingBrain;
    // ---- [ROUND 2] what the owner's NPCs spent (npc_costs) ----
    void requestCosts();

    // ---- [ROUND 3] the Game tab, the live game master (PROTOCOL.md §19) and the monthly costs (npc_month) ----
    /** Settings / Reactions / Game / Game Master tabs as the NPC and its owner allow (Game only when it uses the game). */
    void refreshDetailTabs();
    /** The Game section's "Quests, dialogues, trader..." button. */
    void openGameTab();
    void gmSend(const std::string& op, const LLSD& fields, const std::string& done_words);
    void refreshGm();
    void onGmEnded(const std::string& id, const std::string& why);
    void gmPeople();
    /** npc_month for the owner's NPCs here, plus `extra` [{npc, ai}] (a scene's pieces); `show` gets the words. */
    void requestMonth(const LLSD& extra, std::function<void(const std::string& words)> show);
    void showMonth();
    void onCostsPage();

    LLTabContainer* mTabs = nullptr;
    LLPanel* mNewTab = nullptr;
    LLPanel* mRegionTab = nullptr;
    LLPanel* mScenesTab = nullptr;
    LLTextBox* mStatus = nullptr;
    LLButton* mBuyBtn = nullptr;
    std::string mBuyUrl;
    LLPanel* mQuestion = nullptr;
    LLTextBox* mQuestionText = nullptr;
    LLButton* mQuestionYes = nullptr;
    LLButton* mQuestionNo = nullptr;
    std::function<void()> mQuestionGo;
    LLHandle<LLView> mQuestionReturn;   // what had the keyboard when the question came up

    LLSD mHello;                    // the last hello reply
    U32 mHelloSerial = 0;
    boost::signals2::connection mRegionChangedConnection;
    bool mRegionPending = false;    // a new region: hello (and the list) once it has said it is Wolf

    // New NPC
    WolfNPCForm* mNewForm = nullptr;
    WolfNPCAvatarGrid* mGrid = nullptr;
    LLPanel* mGridHolder = nullptr;
    LLScrollContainer* mGridScroll = nullptr;
    LLTextBox* mAvatarsNote = nullptr;
    LLButton* mAvatarsMakeBtn = nullptr;
    LLTextBox* mCreateNote = nullptr;
    LLButton* mRezBtn = nullptr;
    bool mPetMode = false;
    LLCheckBoxCtrl* mQuickTravel = nullptr;
    // [WOLF PETS 2026-10-24] §29.4 Wear it / Let it roam
    bool mWearPet = true;
    LLRadioGroup* mPetHow = nullptr;
    LLPanel* mWearPanel = nullptr;
    LLComboBox* mWearSide = nullptr;
    LLSpinCtrl* mWearDistance = nullptr;
    LLTextBox* mWearResult = nullptr;
    LLButton* mSendHomeBtn = nullptr;
    std::map<std::string, bool> mTravelOf;   // NPC id -> its cfg.travel, as get said
    ENewStep mNewStep = NEW_START;
    LLPanel* mStartPanel = nullptr;
    LLPanel* mPickPanel = nullptr;
    LLPanel* mQuickPanel = nullptr;
    LLTextBox* mChosenText = nullptr;
    LLLineEditor* mQuickName = nullptr;
    LLLineEditor* mSays = nullptr;
    LLCheckBoxCtrl* mQuickAi = nullptr;
    LLTextEditor* mQuickPersonality = nullptr;
    LLButton* mMoreBtn = nullptr;
    LLScrollContainer* mNewScroll = nullptr;
    LLTextBox* mNewWait = nullptr;
    LLButton* mNewRezBtn = nullptr;
    LLButton* mNewWaitStop = nullptr;
    // "Create a new one": the spot confirmed at the start, and the character being made in WolfAvatars
    bool mWaitingNew = false;
    std::set<std::string> mKnownJobs;   // the avatars they had when they started (a finished one already there is not "new")
    std::string mNewJob, mNewJobName;
    bool mHasSpot = false;
    LLUUID mSpotRegion;
    LLVector3 mSpotPos, mSpotHome;
    F32 mSpotFacing = 0.f;
    LLSD mAvatars;                  // npc_avatars avatars[]
    bool mAvatarsRead = false;
    U32 mAvatarsSerial = 0;
    std::map<std::string, LLPointer<LLViewerTexture>> mPictures;   // by job
    std::map<std::string, std::string> mPictureWhy;                // by job: why it is not shown
    std::set<std::string> mPictureAsked;                           // jobs whose picture was asked for
    std::vector<std::string> mPictureQueue;
    S32 mPicturesOut = 0;
    bool mCreating = false;
    std::string mNewAutoName;       // the name step 1 filled in (replaced when another avatar is chosen)
    LLSD mAiInfo;                   // the last npc_ai_info reply
    U32 mAiSerial = 0;

    // On this region
    LLPanel* mListView = nullptr;
    LLPanel* mDetailView = nullptr;
    LLTextBox* mCountText = nullptr;
    LLTextBox* mSelNote = nullptr;
    LLButton* mEditBtn = nullptr;
    LLButton* mRenameBtn = nullptr;
    LLButton* mPauseBtn = nullptr;
    LLButton* mHomeBtn = nullptr;
    LLButton* mHereBtn = nullptr;
    LLButton* mStopBtn = nullptr;
    LLButton* mDeleteBtn = nullptr;
    LLPanel* mRenameRow = nullptr;
    LLLineEditor* mRenameName = nullptr;
    LLSD mNpcs;                     // op list npcs[]
    LLSD mCosts;                    // [ROUND 2] npc_costs costs {<id>: {today, month}}
    U32 mCostsSerial = 0;
    LLTextBox* mMonthCost = nullptr;
    U32 mMonthSerial = 0;
    LLSD mAiCache;                  // [ROUND 3] each owned NPC's cfg.ai from get (npc_month's input); others: {on} from the list
    bool mListOut = false;
    bool mListAgain = false;
    F64 mListWantAt = 0.0;          // a "changed" push asks for the list again at this time (0 = not asked)
    U32 mListSerial = 0;
    std::string mSelected;          // the id selected in the list

    // detail (Edit)
    std::string mDetailId;
    LLSD mDetail;                   // op get reply
    bool mDetailOwner = false;      // get was allowed (the full settings are known)
    U32 mDetailSerial = 0;
    LLTextBox* mDetailTitle = nullptr;
    LLTextBox* mDetailState = nullptr;
    LLButton* mParkBtn = nullptr;
    LLButton* mExitBtn = nullptr;
    LLTabContainer* mDetailTabs = nullptr;
    LLPanel* mSettingsTab = nullptr;
    LLPanel* mAnimsTab = nullptr;
    LLPanel* mRulesTab = nullptr;
    LLPanel* mGameTab = nullptr;
    LLPanel* mMindTab = nullptr;    // [ROUND 4] Learning
    LLPanel* mPlacesTab = nullptr;  // [ROUND 4] Places
    WolfNPCMind* mMind = nullptr;
    LLPanel* mGmTab = nullptr;
    WolfNPCGame* mGame = nullptr;
    bool mGameWanted = false;       // the owner opened the Game tab for this NPC
    bool mGmControlling = false;    // this viewer controls the NPC shown (gm_take answered)
    std::string mGmBy;              // state.gm: who controls it ("" = nobody)
    LLComboBox* mGmWho = nullptr;
    LLComboBox* mGmAnim = nullptr;
    LLComboBox* mGmEvent = nullptr;
    LLLineEditor* mGmSay = nullptr;
    LLTextBox* mGmState = nullptr;
    WolfNPCForm* mEditForm = nullptr;
    LLButton* mSaveBtn = nullptr;
    LLScrollListCtrl* mAnims = nullptr;
    // reactions
    LLScrollListCtrl* mRules = nullptr;
    LLTextBox* mRulesCount = nullptr;
    LLButton* mRuleNewBtn = nullptr;
    LLButton* mRuleChangeBtn = nullptr;
    LLButton* mRuleRemoveBtn = nullptr;
    LLPanel* mRuleEditor = nullptr;
    LLComboBox* mRuleWhen = nullptr;
    LLSliderCtrl* mRuleRange = nullptr;
    LLComboBox* mRuleWho = nullptr;
    LLCheckBoxCtrl* mRuleNot = nullptr;
    LLLineEditor* mRuleWhoValue = nullptr;
    LLComboBox* mRulePerson = nullptr;
    LLButton* mRulePeopleBtn = nullptr;
    LLComboBox* mRuleAction = nullptr;
    LLComboBox* mRuleActionValue = nullptr;
    LLButton* mRuleAddActionBtn = nullptr;
    LLScrollListCtrl* mRuleActions = nullptr;
    LLButton* mRuleRemoveActionBtn = nullptr;
    LLComboBox* mRuleOften = nullptr;
    LLSpinCtrl* mRuleCooldown = nullptr;
    LLSpinCtrl* mRuleGap = nullptr;
    LLTextBox* mRuleError = nullptr;
    S32 mRuleIndex = -1;            // the rule being changed (-1 = a new one)
    std::string mRuleRawWho;        // a who the editor cannot show term by term (kept as it is)
    std::vector<std::string> mRuleDo;
    LLSD mPeople;                   // op people

    // Scenes
    LLScrollListCtrl* mSceneList = nullptr;
    LLTextBox* mSceneBlurb = nullptr;
    LLTextBox* mSceneCost = nullptr;
    U32 mSceneMonthSerial = 0;
    LLButton* mSceneStartBtn = nullptr;
    LLPanel* mScenePanel = nullptr;
    LLTextBox* mSceneStep = nullptr;
    LLTextBox* mScenePiece = nullptr;
    LLComboBox* mSceneAvatar = nullptr;
    LLLineEditor* mSceneName = nullptr;
    LLButton* mSceneRezBtn = nullptr;
    LLButton* mSceneSkipBtn = nullptr;
    LLButton* mSceneStopBtn = nullptr;
    LLTextBox* mSceneNone = nullptr;
    LLButton* mSceneMakeBtn = nullptr;
    LLSD mScenes;                   // npc_scenes scenes[]
    bool mScenesRead = false;
    U32 mScenesSerial = 0;
    S32 mScene = -1;                // the scene being placed (-1 = none)
    S32 mSceneStepN = 0;            // 0-based step: one per NPC (a piece of count 5 is 5 steps)
    S32 mSceneSteps = 0;
    LLTextBox* mSceneHowto = nullptr;     // before Start: "You place each character yourself... N steps."
    LLTextBox* mSceneInstr = nullptr;     // the step's instruction ("Stand behind the shop counter, then press Rez")
    LLTextBox* mSceneProgress = nullptr;  // every step: done ticked, this one marked, the rest still to come
    LLTextBox* mSceneResult = nullptr;    // next to Rez: rezzing / the confirmation / why it was not rezzed
    std::vector<U8> mSceneMarks;          // per step: 0 to do, 1 rezzed, 2 skipped
    U32 mSceneRun = 0;                    // one per Start: a create answered after Stop or a new Start moves nothing on
    void setSceneResult(const std::string& text, bool problem);
    std::string sceneStepLabel(S32 step) const;   // "Townsfolk 2" (a piece of count 1: just its label)
};

#endif // WOLF_FLOATER_NPC_H
