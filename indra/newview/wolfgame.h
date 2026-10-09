/**
 * @file wolfgame.h
 * @brief WolfViewer: Wolf Roleplay (wolfGame) - the state the region's game puts on this
 *        player's screen, and the GenericMessage protocol that carries it.
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

#ifndef WOLF_GAME_H
#define WOLF_GAME_H

#include <array>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "indra_constants.h"
#include "llpointer.h"
#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "v3math.h"
#include "v4color.h"

class LLHUDText;

/**
 * [WOLF GAME 2026-10-09] Wolf Roleplay, the viewer half.
 *
 * Spec: firestorm/region-terrain-work/wolfgame/VIEWER_SPEC.md. The sim half is WolfSim
 * OptionalModules/World/WolfGame/WolfGameModule.cs, and every field name below was read from
 * its Send() calls (SendGame, SendPlayer, FlushChanges and each public method's message).
 *
 * SIM -> VIEWER. GenericMessage "WolfGame", Invoice = the region's UUID. Parameter 0 is
 * "id/part/total" (WolfGameModule.cs Send: `id + "/" + part + "/" + total`), parameters 1..3 are
 * pieces of one UTF-8 JSON object (CHUNK_BYTES 240, CHUNKS_PER_PACKET 3, never splitting a
 * character - Chunk()). The pieces of part 0..total-1, in order, are the JSON.
 *
 * VIEWER -> SIM. GenericMessage "wolfgame" (OnNewClient: AddGenericPacketHandler("wolfgame")),
 * parameters "hello" | "join" | "leave" | "dialog",id,label | "button",id (OnViewerMessage).
 * send_generic_message writes every non-empty parameter with its NUL
 * (LLTemplateMessageBuilder::addString: strlen + 1), which is what LLClientView's
 * Util.FieldToString strips.
 *
 * This class holds what the screen shows; WolfGameHUD (wolfgamehud.cpp) draws the always-on
 * parts, wolffloaterroleplay.cpp the floaters.
 */
class WolfGame : public LLSingleton<WolfGame>
{
    LLSINGLETON(WolfGame);
    ~WolfGame();

public:
    // [FARMING] one item the player holds (the "item" message / player items[]).
    struct Item
    {
        std::string mName;      // n
        std::string mLabel;     // l
        std::string mIcon;      // i
        S32         mOrder = 0; // o
        S64         mQty = 0;   // q
        std::string mKind;      // k [COMBAT]: "", weapon, armour, spell, consumable, material
        std::string mSlot;      // s [COMBAT]: the equipment slot of a weapon or armour item, else ""
    };

    // ---- what the game is (the "game" message) ----
    struct Game
    {
        bool        mValid = false;
        std::string mKey, mName, mKind, mDescription, mWebsite, mRules, mLogo, mCurrency, mSymbol;
        std::string mTheme;   // "wolves" (the grid default game) | "plain" (WolfGameTypes.cs GameDef.Theme)
        std::string mCurrencyIcon;   // [WOLF GAME ICONS 2026-10-09] the money's texture UUID, or ""
        // [BAG 2026-10-09] every item the game defines (game.items {n,l,i,o,k,s}), so the bag can
        // show the ones held at 0. mQty is 0 in these; the held counts are WolfGame::items().
        std::vector<Item> mItemDefs;
    };

    // ---- one bar (the "player" stats[] entry, or a "stat" message) ----
    struct Stat
    {
        std::string mName;      // n
        std::string mLabel;     // l
        S32         mValue = 0; // v
        S32         mMax = 0;   // m
        LLColor4    mColor;     // c (#rrggbb)
        std::string mIcon;      // i: a texture UUID or a short text glyph (wolfgame.cpp iconStr)
        S32         mOrder = 0; // o
        bool        mShown = false;   // shown:1, a wolfGameShowStat display-only stat
        U32         mArrival = 0;     // ties in o keep their arrival order
    };


    // [PETS] one pet (the "pet" message's pet object / player pets[]); php wg_pet_payload().
    struct Pet
    {
        std::string mKey, mOwner, mType, mTypeLabel, mIcon, mName;
        bool        mAlive = true;
        S64         mAge = 0;           // seconds, when received
        F64         mReceived = 0.0;    // so the age keeps counting
        std::vector<Stat> mStats;       // {n,l,v,m,c,i}
        S64 ageNow() const;
    };

    struct Button { std::string mId, mLabel; };

    // ---- [COMBAT 2026-10-09] VIEWER_SPEC.md §5 - every field name from WolfGameModule.cs
    //      LoadoutJson, GearJson, AmmoJson, AbilityJson, TargetJson, BuffJson and Ctext ----
    /** A loadout entry (the item equipped in a slot). */
    struct LoadoutItem
    {
        std::string mName, mLabel, mIcon, mKind, mDamage, mRange, mFireMode, mAmmo, mDefence;
        bool mTwoHand = false;
        S32  mMagazine = 0;
    };
    /** What a slot shows: the gear's fields where a script set them, else the loadout item's. */
    struct SlotView
    {
        bool mFilled = false;
        bool mFromGear = false;           // a script's display (an external game)
        std::string mItem;                // the loadout item's name ("" for gear only)
        std::string mLabel, mIcon, mDamage, mRange, mFireMode, mDefence, mAmmoName, mSrc;
        S32  mMagazine = 0;
        bool mVerified = true;
    };
    struct Ammo
    {
        S32 mRounds = -1;                 // a: -1 = not tracked
        S32 mMagazine = 0;                // m: 0 = unknown
        S32 mReserve = -1;                // r: -1 = not shown
        F64 mReloadEnds = 0.0;            // now + rl when it arrived; 0 = not reloading
        F32 mReloadTotal = 0.f;           // rt
    };
    struct Ability
    {
        std::string mId, mLabel, mIcon, mCost, mCostLabel, mKey, mGroup, mWeapon, mSrc;
        F32 mCooldown = 0.f, mCast = 0.f;
        S32 mAmount = 0, mAmmoPerUse = 0, mOrder = 0;
        bool mOn = true, mNeedsTarget = false, mVerified = true;
        F64 mCooldownEnds = 0.0;          // now + left when it arrived
        F32 mCooldownTotal = 0.f;
        F64 mReadyAt = 0.0;               // when the sweep ended, for the "ready" edge pulse
        F64 mPressedAt = 0.0;             // the 100 ms pressed flash
        U32 mArrival = 0;
    };
    struct Cast
    {
        bool mOn = false;
        std::string mId, mLabel;
        F32 mSeconds = 0.f;
        F64 mEnds = 0.0;
        std::string mEnd;                 // "done" | "interrupted" | "cancelled" once over
        F64 mEndedAt = 0.0;
    };
    struct Target
    {
        bool mOn = false;
        std::string mKey, mKind, mName, mIcon, mSub, mHost, mSrc;
        S32 mHealth = -1, mMax = 0, mLevel = 0;
        bool mLive = false, mDead = false, mVerified = true;
    };
    struct Buff
    {
        std::string mId, mLabel, mIcon, mText, mSrc;
        bool mGood = true, mVerified = true;
        S32 mStacks = 1;
        F32 mDuration = 0.f;              // 0 = until removed
        F64 mEnds = 0.0;
    };
    struct CombatText
    {
        LLUUID mAt;
        S64 mAmount = 0;
        std::string mKind;                // hit crit heal miss dodge block immune absorb
        LLColor4 mColor;
        bool mHasColor = false;
        bool mSelf = false;
        F64 mBorn = 0.0;
        U32 mSerial = 0;
    };
    struct Row { std::string mLabel, mValue; };

    enum ENoticeKind { NOTICE_INFO = 0, NOTICE_SUCCESS, NOTICE_WARNING, NOTICE_DANGER };
    struct Notice
    {
        U32         mSerial = 0;
        std::string mTitle, mText, mSrc;
        // [SECURITY 2026-10-09] WolfGameModule.cs Verified(): 1 = the game's own registered
        // script, 0 = any other. A notice the sim makes itself (no "verified" key) is the sim's.
        bool        mVerified = true;
        ENoticeKind mKind = NOTICE_INFO;
        F64         mStart = 0.0;
        F64         mExpires = 0.0;   // 0 = until closed (danger)
    };

    struct Marker
    {
        std::string mId, mRegion, mLabel;
        LLVector3   mPos;              // region-local, as sent
        LLColor4    mColor;
        F64         mExpires = 0.0;    // 0 = until cleared
        bool        mHaveGlobal = false;
        LLVector3d  mGlobal;
        F64         mAskedAt = -1.0;   // when the map server was last asked for mRegion
        U32         mSeq = 0;          // arrival order, for dropping the oldest past the cap
        LLUUID      mSender;           // [SECURITY] the region that sent it
    };

    enum EEffect { FX_FLASH = 0, FX_PULSE, FX_BLUR, FX_BLACKOUT, FX_DESATURATE, FX_SHAKE, FX_COUNT };
    struct Effect
    {
        bool mOn = false;
        F32  mStrength = 0.f;   // s / 100
        F32  mSeconds = 0.f;    // sec; 0 = held until "clear"
        F64  mStart = 0.0;
    };

    // ---- sent to the sim ----
    void sendHello();
    void sendJoin();
    void sendLeave();
    void sendDialog(const std::string& id, const std::string& label);
    void sendButton(const std::string& id);
    // [COMBAT] §5.7 - each returns false when nothing was sent (no region, off Wolf, a request
    // already in flight); the caller says so on screen.
    bool useAbility(const std::string& id);
    bool cancelCast();
    bool reload(const std::string& slot);
    bool equip(const std::string& item);
    bool unequip(const std::string& slot);
    bool selectTarget(const std::string& key);
    /** Keys 1-9, 0, -, = press the first 12 action buttons (§5.4), from LLViewerWindow::handleKey
     *  after the focused control, the tool and gestures have had them. */
    bool handleActionKey(KEY key, MASK mask);

    /** From the GenericMessage dispatcher: one packet of a "WolfGame" message. */
    void receivePacket(const LLUUID& invoice, const std::vector<std::string>& strings);

    /** Once a frame, after the object list has moved the avatars (llappviewer.cpp idle). */
    void idle();
    /** Around gAgentCamera.updateCamera(): take last frame's shake off, put this frame's on. */
    void unshakeCamera();
    void shakeCamera();
    /**
     * Escape: closes the overlay page or a focused game dialog, else clears the screen effects
     * when no text field has the keyboard (so a blackout or blur can always be dismissed).
     * llviewerwindow.cpp LLViewerWindow::handleKey asks before the keyboard focus does.
     */
    bool handleEscape();

    // ---- the state, for the HUD and the floaters ----
    U32 generation() const { return mGeneration; }   ///< bumped on every change
    const Game& game() const { return mGame; }
    bool playing() const { return mPlaying; }
    const std::vector<Stat>& stats() const { return mStats; }
    bool hasCurrency() const { return mHasCurrency; }
    const std::string& currencySymbol() const { return mCurrencySymbol; }
    const std::string& currencyName() const { return mCurrencyName; }
    S64 balance() const { return mBalance; }
    bool hasLevel() const { return mHasLevel; }
    S32 level() const { return mLevel; }
    S64 xp() const { return mXp; }
    S64 xpNext() const { return mXpNext; }
    const std::string& panelTitle() const { return mPanelTitle; }
    const std::vector<Row>& panelRows() const { return mPanelRows; }
    const std::vector<Button>& buttons() const { return mButtons; }
    /** Who set the buttons ("buttons" message src / verified). The player message replays the
     *  buttons without either, so the last "buttons" message's values are kept. */
    const std::string& buttonsSrc() const { return mButtonsSrc; }
    bool buttonsVerified() const { return mButtonsVerified; }
    const std::vector<Item>& items() const { return mItems; }
    const std::vector<Pet>& pets() const { return mPets; }
    bool dead() const { return mDead; }
    bool downed() const { return mDowned || mDownedOn; }
    /** Anything for the stat panel to show (§3.1: hidden with no stats, no money, no level). */
    bool hasScreenStats() const { return !mStats.empty() || mHasCurrency || mHasLevel; }

    // ---- [COMBAT] §5 ----
    static const std::vector<std::string>& weaponSlots();   // main, off, ranged
    static const std::vector<std::string>& armourSlots();   // head, neck, body, hands, legs, feet, ring
    /** A slot's word for people: "Main hand", "Head". */
    static std::string slotLabel(const std::string& slot);
    SlotView slotView(const std::string& slot) const;
    const Ammo* ammoFor(const std::string& slot) const;
    bool hasWeapons() const;
    const std::vector<Ability>& abilities() const { return mAbilities; }
    const Cast& castBar() const { return mCast; }
    const Target& target() const { return mTarget; }
    /** [FIX 2026-10-09] Drop the target here at once (the target frame's X, Game Mode off): the
     *  region is told too, but the frame must not wait for - or depend on - its answer. */
    void clearTargetLocal() { mTarget = Target(); }
    const std::vector<Buff>& buffs() const { return mBuffs; }
    const std::deque<CombatText>& combatText() const { return mCombatText; }
    /** The refused press's words while they show (3 s), else "". */
    const std::string& combatError() const { return mCombatError; }
    F64 hitMarkerAt() const { return mHitMarkerAt; }
    bool hitMarkerCrit() const { return mHitMarkerCrit; }
    /** The weapon slot whose ability was used last (§5.3 "the active weapon"). */
    const std::string& activeWeapon() const { return mActiveWeapon; }
    /** An equip / unequip waiting for its answer: the slot (or the item) it is for, else "". */
    const std::string& equipPending() const { return mEquipPending; }

    // ---- [FORCED] §6 ----
    bool forced() const { return mForced; }
    const std::string& forcedText() const { return mForcedText; }

    bool downedOverlay() const { return mDownedOn; }
    const std::string& downedText() const { return mDownedText; }
    /** Seconds left on the downed countdown, or -1 when there is none. */
    S32 downedCountdown() const;

    const std::deque<Notice>& notices() const { return mNotices; }
    void closeNotice(U32 serial);

    const std::map<std::string, Marker>& markers() const { return mMarkers; }

    /** 0..1 now, faded over the effect's seconds; 0 when off. */
    F32 effectNow(EEffect e) const;
    /** The red pulse's beat this frame, 0..1 (about once a second). */
    F32 pulseBeat() const;


    // ---- Game Mode (§3.2): OFF by default, the player's own switch ----
    bool gameMode() const { return mGameMode; }
    /** A switch was flipped and the region has not yet answered with a `game` message. */
    bool gameModePending() const { return mGameModePending; }
    bool gameModeWanted() const { return mGameModeWanted; }
    /** False for a few seconds after a switch / leave: the sim ignores a second one inside
     *  VIEWER_TOGGLE_MS (WolfGameModule.cs OnViewerMessage), so the UI does not offer it. */
    bool toggleReady() const;
    /** Sends ["gamemode","1"|"0"]; false when not sent (pending, too soon, no region). */
    bool setGameMode(bool on);
    /** [2026-10-09] Paul: no "wait a few seconds" box - "just have a message on the bar to say
     *  connecting". The player's switch: sent now if it can be, else remembered and sent by idle()
     *  as soon as the region can take it. False only off Wolf or to switch off a forced region. */
    bool requestGameMode(bool on);
    /** Waiting to send, or waiting for the region's answer: the switches say "Connecting...". */
    bool gameModeConnecting() const { return mQueuedMode >= 0 || mGameModePending; }
    /** What the switches show: the wish while connecting, else the region's answer. */
    bool gameModeShownOn() const
    {
        if (mForced) return true;
        if (mQueuedMode >= 0) return mQueuedMode == 1;
        return mGameModePending ? mGameModeWanted : mGameMode;
    }
    /** "Leave this game": ["leave"] - this game only, Game Mode stays on. */
    bool leaveThisGame();
    // ---- [GAME HUD 2026-10-09] the game HUD across the bottom (wolfgamehud.cpp drawBottomHud) ----
    // Paul: "i was expecting a hud like affair across the bottom like eve online or wow". One
    // bottom panel at a time with the Flight / Sailing deck and the car dashboard: the HUD gives
    // the bottom up while either of those shows and comes back by itself when they go; the
    // player's own show (the corner icon, switching Game Mode on) takes the bottom back.
    /** Something for the HUD to show: Game Mode on, locked on or turning on - or, with it off, a
     *  worn game HUD's stats, abilities or weapons (§5.8). Wolf Territories only. */
    bool hudWanted() const;
    /** The flight / sailing deck shows, or the car dashboard does: the bottom is theirs. */
    static bool bottomTaken();
    /** The HUD is on screen now: wanted, not hidden by the player, the bottom free. */
    bool hudShowing() const;
    bool hudHidden() const { return mHudHidden; }
    /** The corner icon (lltoolbarview.cpp wolf_game_toggle) while Game Mode is on: hide the HUD
     *  when it shows (the game keeps running), else show it, taking the bottom. */
    void toggleHud();
    /** Show the HUD and take the bottom from the deck (hidden, its mode and autopilot running)
     *  or the dashboard (closed). */
    void claimBottom();
    /** The first-time guide on the HUD (non-blocking): on the first time the player switches Game
     *  Mode on; "Got it" or "Full help" dismisses it. */
    bool guideOn() const { return mGuideOn; }
    void dismissGuide();

    // ---- the theme (§3 Themes) ----
    bool wolvesTheme() const { return mGame.mValid && mGame.mTheme == "wolves"; }

    /** The player's own "Clear screen effects" (Roleplay floater, or Escape). */
    void clearEffectsByPlayer();

    /** Logout: everything goes (§4). Also called when the agent has no region. */
    void clearAll();
    /** Viewer shutdown: let go of the tag HUD texts before LLHUDObject::cleanupHUDObjects, and of
     *  the game UI's textures before the texture list goes. */
    void releaseTags();

    /** [SECURITY 2026-10-09] A game web page's URL is https with a plain authority; `host` gets
     *  its host, the only name any label may show for the page. */
    static bool validWebUrl(const std::string& url, std::string& host);

    /** Global position of a marker, when known. */
    static bool markerGlobal(const Marker& m, LLVector3d& out);

    /** Read by the render pipeline (pipeline.cpp renderFinalize), set each idle(). 0..1. */
    static F32 sDesaturate;
    static F32 sBlur;

private:
    void ensureRegion();
    void onRegionChanged();
    void clearScreen();          ///< the region-change set (§4)
    void clearPlayer();          ///< the `game:null` set (§2 game)
    void apply(const LLSD& msg);
    void applyGame(const LLSD& m);
    void applyPlayer(const LLSD& m);
    void applyStat(const LLSD& m);
    void applyCurrency(const LLSD& m);
    void applyLevel(const LLSD& m);
    void applyNotify(const LLSD& m);
    void applyDialog(const LLSD& m);
    static bool takeToken(F64& tokens, F64& last);
    void applyButtons(const LLSD& buttons);
    void applyEffect(const LLSD& m);
    void applyDowned(const LLSD& m);
    void applyWeb(const LLSD& m);
    void applyMarker(const LLSD& m);
    void applyTag(const LLSD& m);
    void applyPanel(const LLSD& m);
    void applyItem(const LLSD& m);
    void applyPet(const LLSD& pet);
    // [COMBAT]
    void applyLoadout(const LLSD& slots);
    void applyGear(const std::string& slot, const LLSD& g);
    void applyAmmo(const std::string& slot, const LLSD& m);
    void applyAbility(const LLSD& a);
    void applyCooldown(const LLSD& m);
    void applyCast(const LLSD& m);
    void applyCombatError(const LLSD& m);
    void applyTarget(const LLSD& m);
    void applyTargetHealth(const LLSD& m);
    void applyBuff(const LLSD& b);
    void applyCombatText(const LLSD& m);
    void sortAbilities();
    void clearCombat();
    void tickCombat(F64 t);
    void sortItems();
    void sortStats();
    void clearEffects();
    void clearTags();
    void closeDialogs();
    void updateTags();
    static void setTagText(LLHUDText* hud, const std::string& text, const LLColor4& colour);
    void resolveMarkers();
    void send(const std::vector<std::string>& params);
    void updateGameModeButtons();
    void gateToolbar(bool on_wolf);   // [WOLF GRID GATE 2026-10-09]

    struct Pending
    {
        S32 mTotal = 0;
        F64 mFirst = 0.0;
        std::map<S32, std::string> mParts;
    };
    std::map<S64, Pending> mPending;   // by message id

    struct Tag
    {
        std::string mText;
        LLColor4 mColor;
        LLPointer<LLHUDText> mHud;
        U32 mSeq = 0;
    };
    std::map<LLUUID, Tag> mTags;

    boost::signals2::connection mRegionChangedConnection;
    LLUUID mRegionId;
    F64 mHelloAt = 0.0;          // when to send the arrival hello (0 = none due)
    U32 mGeneration = 1;
    U32 mStatArrival = 0;
    U32 mNoticeSerial = 0;

    Game mGame;
    bool mPlaying = false;
    std::vector<Stat> mStats;
    bool mHasCurrency = false;
    std::string mCurrencySymbol, mCurrencyName;
    S64 mBalance = 0;
    bool mHasLevel = false;
    S32 mLevel = 0;
    S64 mXp = 0, mXpNext = 0;
    std::string mPanelTitle;
    std::vector<Row> mPanelRows;
    std::vector<Button> mButtons;
    std::string mButtonsSrc;
    bool mButtonsVerified = false;
    std::vector<Item> mItems;
    std::vector<Pet> mPets;
    // [COMBAT]
    std::map<std::string, LoadoutItem> mLoadout;
    std::map<std::string, LLSD> mGear;            // slot -> the fields a script set, capped
    std::map<std::string, Ammo> mAmmo;
    std::vector<Ability> mAbilities;
    U32 mAbilityArrival = 0;
    Cast mCast;
    Target mTarget;
    std::vector<Buff> mBuffs;
    std::deque<CombatText> mCombatText;
    U32 mCombatTextSerial = 0;
    std::string mCombatError;
    F64 mCombatErrorUntil = 0.0;
    F64 mCombatErrorShownAt = -100.0;
    F64 mHitMarkerAt = -100.0;
    bool mHitMarkerCrit = false;
    std::string mActiveWeapon;
    std::string mEquipPending;
    F64 mEquipPendingAt = 0.0;
    F64 mLeavePendingAt = 0.0;     // "Leave this game" asked, not yet answered
    // [FORCED]
    bool mForced = false;
    std::string mForcedText;
    bool mDead = false;
    bool mDowned = false;        // the player message's flag
    bool mDownedOn = false;      // the overlay (the downed message)
    std::string mDownedText;
    F64 mLastFlashAt = -100.0;   // flashes at most 3 a second (photosensitivity)
    F64 mDownedUntil = 0.0;      // 0 = no countdown
    std::deque<Notice> mNotices;
    std::map<std::string, Marker> mMarkers;
    std::array<Effect, FX_COUNT> mEffects;
    LLVector3 mShakeApplied;

    bool mHudHidden = true;      // [GAME HUD] hidden until shown (corner icon / switching Game Mode on); a session starts hidden
    bool mGuideOn = false;       // [GAME HUD] the first-time guide is up
    bool mGameMode = false;
    bool mGameModePending = false;
    S32  mQueuedMode = -1;       // [2026-10-09] requestGameMode: -1 none, else the wish waiting to be sent
    bool mGameModeWanted = false;
    F64 mGameModeAskedAt = 0.0;
    F64 mLastToggleAt = -100.0;
    F64 mLastButtonsUpdate = 0.0;
    std::string mButtonsShownState;
    U32 mMarkerSeq = 0;
    // [WOLF GRID GATE 2026-10-09] Paul: "on other grids the game ... things dont appear because
    // they wont work". -1 not yet known, 0 off Wolf Territories, 1 on.
    int mOnWolf = -1;
    F64 mLastGridGate = 0.0;
    // [SECURITY 2026-10-09] rate limits and the de-duplicated map lookups
    F64 mDialogTokens = 3.0, mDialogTokenAt = 0.0;
    F64 mNoticeTokens = 3.0, mNoticeTokenAt = 0.0;
    std::map<std::string, F64> mMapAsked;   // lower-case region name -> when asked
    // The removed toolbar buttons are kept in the per-account setting WolfGameToolbarHidden.
    U32 mTagSeq = 0;
    bool mWasTeleporting = false;
};

#endif // WOLF_GAME_H
