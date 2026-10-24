/**
 * @file wolffloaternpcmind.h
 * @brief WolfViewer: WolfNPC window, Edit > Learning (knowledge, skills, what was asked for, changes by chat) and Edit > Places
 *        (places and the objects it uses).
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

#ifndef WOLF_FLOATER_NPC_MIND_H
#define WOLF_FLOATER_NPC_MIND_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llhandle.h"
#include "llsd.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLFloater;
class LLLineEditor;
class LLPanel;
class LLRadioGroup;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLTextEditor;

/**
 * [WOLFNPC ROUND 4 2026-10-24] Contract: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md §5 (the brain's
 * npc_learn_* and npc_skill_* actions, the NPC's owner only), §13.3 (the skill step types: the only ones the editor offers,
 * with plain labels), §22.5 (state.byChat and state.skills), §3 places, §23.1 tools.
 */
class WolfNPCMind
{
public:
    typedef std::function<void(const LLSD& changes, const std::string& done_words, std::function<void(const LLSD& cfg)> after)> save_fn;
    typedef std::function<void(const std::string& text, const std::string& yes, std::function<void()> go)> ask_fn;
    typedef std::function<void(const std::string& text, const LLSD& reply)> say_fn;

    WolfNPCMind(LLHandle<LLFloater> owner, LLPanel* learning, LLPanel* places, save_fn save, ask_fn ask, say_fn error, say_fn done);

    /** The NPC shown (op get: cfg and state). A new NPC reads its knowledge, skills and what was asked for from the brain. */
    void setNpc(const std::string& id, const LLSD& cfg, const LLSD& state);
    /** npc_ai_info: the prices for teaching and finding (credits and pounds). */
    void setPrices(const LLSD& ai_info);
    /** An editor is open (a "changed" push must not refill under it). */
    bool editing() const;

private:
    bool alive(U32 serial) const;
    std::string pounds(F64 credits) const;
    void charged(const LLSD& reply);
    // knowledge
    void loadKnowledge(S32 offset);
    void fillKnowledge();
    LLSD selectedItem() const;
    void itemOp(const std::string& op);
    // skills
    void loadSkills();
    void fillSkills();
    LLSD selectedSkill() const;
    void editSkill(const LLSD& skill, const std::string& when);
    void closeSkill();
    void onStepDo();
    void fillSteps();
    bool buildStep(LLSD& step, std::string& why) const;
    static bool parseSimpleSteps(const std::string& text, LLSD& out, std::string& why);
    static std::string stepWords(const LLSD& step);
    void onSkillSave();
    // [ROUND B] growing: look-ups, teaching, research, voices (PROTOCOL.md §5 round B)
    void setLearnSwitch(const char* key, bool on);
    void loadResearch();
    void onResearchQuote();
    void loadStyles();
    void setStyle(bool on);
    // asked for, find
    void loadWished();
    void fillWished();
    void onFind();
    // changes by chat
    void fillChanges();
    void onUndo();
    // places and tools
    void fillPlaces();
    void fillTools();
    void onSavePlace();
    void onLink();

    LLHandle<LLFloater> mOwner;
    /** Brain replies come back after the window (and this) may be gone: each callback holds a weak_ptr to this and checks
     *  it before touching anything. */
    std::shared_ptr<int> mLife = std::make_shared<int>(0);
    LLPanel* mLearning;
    LLPanel* mPlacesPanel;
    save_fn mSave;
    ask_fn mAsk;
    say_fn mError, mDone;
    std::string mId;
    U32 mSerial = 0;
    LLSD mCfg, mState, mPrices;
    std::map<std::string, LLSD> mFirstCfg;   // each NPC's cfg as first read in this session (what Undo puts back)

    // knowledge
    LLRadioGroup* mKMode = nullptr;
    LLTextEditor* mKRole = nullptr;
    LLLineEditor* mKSearch = nullptr;
    LLTextBox* mKCount = nullptr;
    LLScrollListCtrl* mKItems = nullptr;
    LLLineEditor* mKEdit = nullptr;
    LLLineEditor* mKTeach = nullptr;
    LLComboBox* mKTeachKind = nullptr;
    LLTextBox* mKNote = nullptr;
    LLButton* mKMore = nullptr;
    LLSD mItems;                 // npc_learn_get items (pages joined)
    S32 mItemsCount = 0, mItemsMax = 0;
    // skills
    LLScrollListCtrl* mSkList = nullptr;
    LLTextBox* mSkCount = nullptr;
    LLPanel* mSkEditor = nullptr;
    LLLineEditor* mSkName = nullptr;
    LLComboBox* mSkScope = nullptr;
    LLLineEditor* mSkWhen = nullptr;
    LLScrollListCtrl* mSkSteps = nullptr;
    LLComboBox* mSkStepDo = nullptr;
    LLLineEditor* mSkStepA = nullptr;
    LLLineEditor* mSkStepB = nullptr;
    LLLineEditor* mSkStepThen = nullptr;
    LLLineEditor* mSkStepElse = nullptr;
    LLTextBox* mSkStepHelp = nullptr;
    LLTextBox* mSkError = nullptr;
    LLSD mSkills;                // npc_skill_list skills
    LLSD mOffers;
    std::string mSkEditing;      // the id being changed ("" = new)
    std::vector<LLSD> mStepList;
    // growing
    LLCheckBoxCtrl* mGLookups = nullptr;
    LLCheckBoxCtrl* mGTeacher = nullptr;
    LLTextBox* mGLookupsCost = nullptr;
    LLTextBox* mGTeacherCost = nullptr;
    LLLineEditor* mGTopic = nullptr;
    LLTextBox* mGQuote = nullptr;
    LLScrollListCtrl* mGJobs = nullptr;
    LLScrollListCtrl* mGStyles = nullptr;
    LLSD mStyles;
    // asked for, find
    LLScrollListCtrl* mWList = nullptr;
    LLSD mWished;
    LLLineEditor* mFText = nullptr;
    LLTextBox* mFNote = nullptr;
    LLScrollListCtrl* mFList = nullptr;
    LLSD mFound;
    // changes
    LLScrollListCtrl* mCList = nullptr;
    LLTextBox* mCUndoNote = nullptr;
    LLScrollListCtrl* mCSkills = nullptr;
    // places, tools
    LLScrollListCtrl* mPlList = nullptr;
    LLLineEditor* mPlName = nullptr;
    LLScrollListCtrl* mTlList = nullptr;
    LLComboBox* mTlType = nullptr;
    LLLineEditor* mTlRole = nullptr;
    LLSpinCtrl* mTlFace = nullptr;
    LLTextBox* mPlError = nullptr;
};

#endif // WOLF_FLOATER_NPC_MIND_H
