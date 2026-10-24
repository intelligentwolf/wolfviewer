/**
 * @file wolffloaternpcgame.h
 * @brief WolfViewer: WolfNPC window, Edit > Game - quests, dialogue choices, a trader, a pet, events and touch for one NPC.
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

#ifndef WOLF_FLOATER_NPC_GAME_H
#define WOLF_FLOATER_NPC_GAME_H

#include <functional>
#include <string>
#include <vector>

#include "llsd.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLPanel;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextEditor;

/**
 * [WOLFNPC ROUND 3 2026-10-24] The Game tab of the WolfNPC window (panel_wolf_npc_game.xml). Contract:
 * firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md §12 (the game NPC rule: the game parts run only for players of
 * the region's game when the NPC's owner owns that game), §14 (touch, actions, dialogues), §15 (quests), §16 (traders),
 * §17 (pets), §18 (events), §20 (handouts). Fill-in-the-boxes forms; each part is saved on its own through the window's
 * op update (quests, dialogues and events one id at a time, §14.1 merge rule: id -> definition, id -> null removes).
 */
class WolfNPCGame
{
public:
    /** Save `changes` (a partial cfg); `done_words` on success; `after` gets the whole stored cfg back. */
    typedef std::function<void(const LLSD& changes, const std::string& done_words, std::function<void(const LLSD& cfg)> after)> save_fn;
    /** The window's in-window question (yes far from Cancel). */
    typedef std::function<void(const std::string& text, const std::string& yes, std::function<void()> go)> ask_fn;
    typedef std::function<void(const std::string& text)> say_fn;

    WolfNPCGame(LLPanel* panel, save_fn save, ask_fn ask, say_fn error, say_fn done);

    /** The NPC shown: its cfg and state (op get), and the owner's other NPCs on the region (list rows) for event copies. */
    void setNpc(const std::string& id, const LLSD& cfg, const LLSD& state, const LLSD& npcs);
    /** An editor (quest, dialogue, event) is open: a "changed" push must not refill the tab under it. */
    bool editing() const;
    /** True when the cfg uses any game part (the Game tab is then shown without asking). */
    static bool hasGameParts(const LLSD& cfg);

private:
    // shared
    static std::vector<std::string> splitActions(const std::string& text);
    static std::string joinActions(const LLSD& list);
    /** Up to `max` actions; a take (take:, money:-) only where `takes_ok` (§12 consent). "" when fine, else why. */
    static std::string checkActions(const std::vector<std::string>& acts, size_t max, bool takes_ok);
    static bool idOk(const std::string& id, bool lower_only);
    static LLSD toArray(const std::vector<std::string>& v);
    void saveAndRefresh(const LLSD& changes, const std::string& words);

    // quests (§15)
    void fillQuests();
    void editQuest(const std::string& id);
    void closeQuest();
    void onQuestStepDo();
    void fillQuestSteps();
    void onQuestAddStep();
    void onQuestSave();
    // dialogues (§14.4)
    void fillDialogues();
    void editDialogue(const std::string& id);
    void closeDialogue();
    void fillNodes();
    void loadNode(const std::string& id);
    void fillChoices();
    void fillNodeCombos();
    void onAddChoice();
    void onKeepNode();
    void onDialogueSave();
    // trader (§16), pet (§17), touch (§14.2)
    void fillShop();
    void fillShopLines();
    void onShopAdd();
    void onShopSave();
    void fillPet();
    void fillTouch();
    void onTouchSave();
    // events (§18)
    void fillEvents();
    void editEvent(const std::string& id);
    void closeEvent();
    void fillSpawns();
    void onSpawnAdd();
    void onEventSave();

    LLPanel* mPanel;
    save_fn mSave;
    ask_fn mAsk;
    say_fn mError, mDone;
    std::string mId;
    LLSD mCfg, mState, mNpcs;

    // quests
    LLScrollListCtrl* mQuests = nullptr;
    LLTextBox* mQuestsCount = nullptr;
    LLPanel* mQuestEditor = nullptr;
    LLLineEditor* mQId = nullptr;
    LLLineEditor* mQTitle = nullptr;
    LLTextEditor* mQText = nullptr;
    LLLineEditor* mQNeeds = nullptr;
    LLCheckBoxCtrl* mQRepeat = nullptr;
    LLScrollListCtrl* mQSteps = nullptr;
    LLComboBox* mQStepDo = nullptr;
    LLLineEditor* mQStepWhat = nullptr;
    LLLineEditor* mQStepNpc = nullptr;
    LLSpinCtrl* mQStepN = nullptr;
    LLLineEditor* mQStepText = nullptr;
    LLSpinCtrl* mQStepR = nullptr;
    LLLineEditor* mQStepEvent = nullptr;
    LLSpinCtrl* mQMoney = nullptr;
    LLSpinCtrl* mQXp = nullptr;
    LLLineEditor* mQItems = nullptr;
    LLLineEditor* mQDo = nullptr;
    LLTextBox* mQError = nullptr;
    std::string mQEditing;          // the id being changed ("" = a new quest)
    std::vector<LLSD> mQStepList;
    // dialogues
    LLScrollListCtrl* mDialogues = nullptr;
    LLTextBox* mDialoguesCount = nullptr;
    LLPanel* mDlgEditor = nullptr;
    LLLineEditor* mDId = nullptr;
    LLLineEditor* mDTitle = nullptr;
    LLComboBox* mDStart = nullptr;
    LLScrollListCtrl* mDNodes = nullptr;
    LLLineEditor* mNId = nullptr;
    LLLineEditor* mNDo = nullptr;
    LLTextEditor* mNText = nullptr;
    LLScrollListCtrl* mNChoices = nullptr;
    LLLineEditor* mCLabel = nullptr;
    LLComboBox* mCNext = nullptr;
    LLComboBox* mCFail = nullptr;
    LLLineEditor* mCIf = nullptr;
    LLLineEditor* mCDo = nullptr;
    LLTextBox* mDError = nullptr;
    std::string mDEditing;          // the dialogue id being changed ("" = new)
    LLSD mDNodesMap;                // node id -> {text, do, choices}
    std::string mNEditing;          // the node loaded in the line editor ("" = a new line)
    std::vector<LLSD> mNChoiceList;
    // trader
    LLCheckBoxCtrl* mSOn = nullptr;
    LLLineEditor* mSTitle = nullptr;
    LLScrollListCtrl* mSLines = nullptr;
    LLComboBox* mSKind = nullptr;
    LLLineEditor* mSItem = nullptr;
    LLSpinCtrl* mSQty = nullptr;
    LLSpinCtrl* mSPrice = nullptr;
    LLLineEditor* mSLabel = nullptr;
    LLSpinCtrl* mSFriend = nullptr;
    LLSpinCtrl* mSDiscount = nullptr;
    LLSpinCtrl* mSRefuse = nullptr;
    LLTextBox* mSError = nullptr;
    std::vector<LLSD> mShopLines;   // {kind: sell|buy, item, qty, price, label}
    // pet
    LLLineEditor* mPKey = nullptr;
    LLLineEditor* mPPlay = nullptr;
    LLSpinCtrl* mPAdd = nullptr;
    LLTextBox* mPCard = nullptr;
    // events
    LLScrollListCtrl* mEvents = nullptr;
    LLTextBox* mEventsCount = nullptr;
    LLPanel* mEvEditor = nullptr;
    LLLineEditor* mEId = nullptr;
    LLLineEditor* mETitle = nullptr;
    LLComboBox* mEWhen = nullptr;
    LLLineEditor* mEAt = nullptr;
    LLSpinCtrl* mEDuration = nullptr;
    LLSpinCtrl* mECooldown = nullptr;
    LLLineEditor* mESay = nullptr;
    LLScrollListCtrl* mESpawn = nullptr;
    LLComboBox* mEFrom = nullptr;
    LLSpinCtrl* mECount = nullptr;
    LLLineEditor* mESpawnAt = nullptr;
    LLSpinCtrl* mERadius = nullptr;
    LLLineEditor* mEGroupTag = nullptr;
    LLComboBox* mEGroupDo = nullptr;
    LLLineEditor* mEDo = nullptr;
    LLLineEditor* mEEnd = nullptr;
    LLCheckBoxCtrl* mEKeep = nullptr;
    LLTextBox* mEError = nullptr;
    std::string mEEditing;
    std::vector<LLSD> mSpawnList;
    // touch
    LLLineEditor* mTDo = nullptr;
    LLLineEditor* mTFaction = nullptr;
    LLLineEditor* mTHandName = nullptr;
    LLTextBox* mTError = nullptr;
};

#endif // WOLF_FLOATER_NPC_GAME_H
