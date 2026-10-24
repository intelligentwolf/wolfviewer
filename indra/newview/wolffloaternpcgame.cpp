/**
 * @file wolffloaternpcgame.cpp
 * @brief WolfViewer: WolfNPC window, Edit > Game. See wolffloaternpcgame.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaternpcgame.h"

#include <cmath>
#include <set>

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdutil.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "wolfgame.h"

// Every key, limit and word: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md (section numbers below).

namespace
{
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    std::string trimmed(std::string s)
    {
        LLStringUtil::trim(s);
        return s;
    }

    size_t utf8Chars(const std::string& s) { return utf8str_to_wstring(s).size(); }

    std::string lower(std::string s)
    {
        LLStringUtil::toLower(s);
        return s;
    }

    /** An integral number as a JSON integer (see wolffloaternpc.cpp wireNum). */
    LLSD wireInt(F64 v)
    {
        if (!std::isfinite(v)) return LLSD(0);
        return LLSD((LLSD::Integer)(S32)llclamp(std::round(v), -2.0e9, 2.0e9));
    }

    bool allIn(const std::string& s, const char* chars)
    {
        return !s.empty() && s.find_first_not_of(chars) == std::string::npos;
    }

    const char* const LOWER_ID = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    const char* const ITEM_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_";   // §14.3 item names [a-z0-9_]{1,24}

    /** "x,y,z" -> [x,y,z]; anything else stays a name (a place). */
    LLSD placeValue(const std::string& text)
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(text, parts, ",");
        if (parts.size() == 3)
        {
            LLSD a = LLSD::emptyArray();
            for (std::string p : parts)
            {
                p = trimmed(p);
                F64 d = 0.0;
                if (!LLStringUtil::convertToF64(p, d) || !std::isfinite(d)) return LLSD(trimmed(text));
                a.append(LLSD(std::round(d * 100.0) / 100.0));
            }
            return a;
        }
        return LLSD(trimmed(text));
    }

    std::string placeText(const LLSD& v)
    {
        if (v.isArray() && v.size() >= 3) return llformat("%.1f, %.1f, %.1f", v[0].asReal(), v[1].asReal(), v[2].asReal());
        return cap(v.asString(), 48);
    }

    struct Choice { const char* value; const char* label; };
    // Source: §15.1 step do: kill | bring | give | go | talk
    const Choice STEP_DOS[] = { { "kill", "Defeat" }, { "bring", "Bring it" }, { "give", "Give to" }, { "go", "Go to" }, { "talk", "Talk to" } };
    // Source: §18.1 when: time | trigger; group do: home | pause
    const Choice WHENS[] = { { "time", "Every day at" }, { "trigger", "When it is started" } };
    const Choice GROUP_DOS[] = { { "", "are left alone" }, { "home", "go home and stay" }, { "pause", "stand still" } };

    template <size_t N>
    void fillCombo(LLComboBox* c, const Choice (&list)[N])
    {
        c->removeall();
        for (const Choice& ch : list) c->add(ch.label, LLSD(ch.value));
    }

    template <size_t N>
    std::string labelOf(const Choice (&list)[N], const std::string& value)
    {
        for (const Choice& ch : list)
        {
            if (value == ch.value) return ch.label;
        }
        return value;
    }

    void selectOr(LLComboBox* c, const std::string& value)
    {
        if (!c->selectByValue(LLSD(value)) && c->getItemCount() > 0) c->selectFirstItem();
    }

    /** One scroll list row from column name/value pairs. */
    LLSD row(const LLSD& value, std::initializer_list<std::pair<const char*, std::string>> cols)
    {
        LLSD r;
        r["value"] = value;
        S32 i = 0;
        for (const auto& c : cols)
        {
            r["columns"][i]["column"] = c.first;
            r["columns"][i]["value"] = c.second;
            ++i;
        }
        return r;
    }

    std::string selectedValue(LLScrollListCtrl* l)
    {
        return l->getFirstSelected() ? l->getFirstSelected()->getValue().asString() : std::string();
    }

    // Source: §14.4 limits (8 dialogues, 32 nodes, 10 choices, label 1-24, do up to 6, if at most 120); §15.1 (8 quests,
    // 1-8 steps, items up to 8); §16 (10 lines); §18.1 (8 events, 4 spawn lines, 20 copies an event); §14.2 touch 6
    const S32 MAX_DIALOGUES = 8, MAX_NODES = 32, MAX_CHOICES = 10, MAX_QUESTS = 8, MAX_STEPS = 8, MAX_ITEMS = 8, MAX_SHOP = 10,
              MAX_EVENTS = 8, MAX_SPAWN = 4, MAX_COPIES = 20;
    const size_t MAX_ACTS = 6;
}

WolfNPCGame::WolfNPCGame(LLPanel* panel, save_fn save, ask_fn ask, say_fn error, say_fn done)
:   mPanel(panel), mSave(save), mAsk(ask), mError(error), mDone(done)
{
    // quests
    mQuests = mPanel->getChild<LLScrollListCtrl>("quests");
    mQuests->setDoubleClickCallback([this]() { editQuest(selectedValue(mQuests)); });
    mQuestsCount = mPanel->getChild<LLTextBox>("quests_count");
    mPanel->getChild<LLButton>("quest_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { editQuest(""); });
    mPanel->getChild<LLButton>("quest_change")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (selectedValue(mQuests).empty()) { mError("Select a quest in the list first."); return; }
        editQuest(selectedValue(mQuests));
    });
    mPanel->getChild<LLButton>("quest_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string id = selectedValue(mQuests);
        if (id.empty()) { mError("Select a quest in the list first."); return; }
        mAsk("Remove the quest " + cap(mCfg["quests"][id]["title"].asString(), 48) + "? Players on it can no longer finish it here.",
             "Remove the quest", [this, id]()
        {
            LLSD changes;
            changes["quests"][id] = LLSD();   // §14.1 id -> null removes it
            closeQuest();
            saveAndRefresh(changes, "Quest removed.");
        });
    });
    mQuestEditor = mPanel->getChild<LLPanel>("quest_editor");
    mQId = mQuestEditor->getChild<LLLineEditor>("q_id");
    mQTitle = mQuestEditor->getChild<LLLineEditor>("q_title");
    mQText = mQuestEditor->getChild<LLTextEditor>("q_text");
    mQNeeds = mQuestEditor->getChild<LLLineEditor>("q_needs");
    mQRepeat = mQuestEditor->getChild<LLCheckBoxCtrl>("q_repeat");
    mQSteps = mQuestEditor->getChild<LLScrollListCtrl>("q_steps");
    mQStepDo = mQuestEditor->getChild<LLComboBox>("q_step_do");
    fillCombo(mQStepDo, STEP_DOS);
    mQStepDo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onQuestStepDo(); });
    mQStepWhat = mQuestEditor->getChild<LLLineEditor>("q_step_what");
    mQStepNpc = mQuestEditor->getChild<LLLineEditor>("q_step_npc");
    mQStepN = mQuestEditor->getChild<LLSpinCtrl>("q_step_n");
    mQStepText = mQuestEditor->getChild<LLLineEditor>("q_step_text");
    mQStepR = mQuestEditor->getChild<LLSpinCtrl>("q_step_r");
    mQStepEvent = mQuestEditor->getChild<LLLineEditor>("q_step_event");
    mQuestEditor->getChild<LLButton>("q_step_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onQuestAddStep(); });
    mQuestEditor->getChild<LLButton>("q_step_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mQSteps->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mQStepList.size()) { mQError->setText(std::string("Select a step first.")); return; }
        mQStepList.erase(mQStepList.begin() + i);
        fillQuestSteps();
    });
    mQMoney = mQuestEditor->getChild<LLSpinCtrl>("q_money");
    mQXp = mQuestEditor->getChild<LLSpinCtrl>("q_xp");
    mQItems = mQuestEditor->getChild<LLLineEditor>("q_items");
    mQDo = mQuestEditor->getChild<LLLineEditor>("q_do");
    mQError = mQuestEditor->getChild<LLTextBox>("q_error");
    mQuestEditor->getChild<LLButton>("q_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeQuest(); });
    mQuestEditor->getChild<LLButton>("q_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onQuestSave(); });

    // dialogues
    mDialogues = mPanel->getChild<LLScrollListCtrl>("dialogues");
    mDialogues->setDoubleClickCallback([this]() { editDialogue(selectedValue(mDialogues)); });
    mDialoguesCount = mPanel->getChild<LLTextBox>("dialogues_count");
    mPanel->getChild<LLButton>("dlg_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { editDialogue(""); });
    mPanel->getChild<LLButton>("dlg_change")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (selectedValue(mDialogues).empty()) { mError("Select a dialogue in the list first."); return; }
        editDialogue(selectedValue(mDialogues));
    });
    mPanel->getChild<LLButton>("dlg_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string id = selectedValue(mDialogues);
        if (id.empty()) { mError("Select a dialogue in the list first."); return; }
        mAsk("Remove the dialogue " + id + "?", "Remove the dialogue", [this, id]()
        {
            LLSD changes;
            changes["dialogues"][id] = LLSD();
            closeDialogue();
            saveAndRefresh(changes, "Dialogue removed.");
        });
    });
    mDlgEditor = mPanel->getChild<LLPanel>("dlg_editor");
    mDId = mDlgEditor->getChild<LLLineEditor>("d_id");
    mDTitle = mDlgEditor->getChild<LLLineEditor>("d_title");
    mDStart = mDlgEditor->getChild<LLComboBox>("d_start");
    mDNodes = mDlgEditor->getChild<LLScrollListCtrl>("d_nodes");
    mDNodes->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadNode(selectedValue(mDNodes)); });
    mDlgEditor->getChild<LLButton>("d_node_new")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if ((S32)mDNodesMap.size() >= MAX_NODES) { mDError->setText(std::string("A dialogue has at most 32 lines.")); return; }
        mDNodes->deselectAllItems();
        loadNode("");
        mNId->setFocus(true);
    });
    mDlgEditor->getChild<LLButton>("d_node_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string id = selectedValue(mDNodes);
        if (id.empty()) { mDError->setText(std::string("Select a line first.")); return; }
        mDNodesMap.erase(id);
        loadNode("");
        fillNodes();
    });
    mNId = mDlgEditor->getChild<LLLineEditor>("n_id");
    mNDo = mDlgEditor->getChild<LLLineEditor>("n_do");
    mNText = mDlgEditor->getChild<LLTextEditor>("n_text");
    mNChoices = mDlgEditor->getChild<LLScrollListCtrl>("n_choices");
    mCLabel = mDlgEditor->getChild<LLLineEditor>("c_label");
    mCNext = mDlgEditor->getChild<LLComboBox>("c_next");
    mCFail = mDlgEditor->getChild<LLComboBox>("c_fail");
    mCIf = mDlgEditor->getChild<LLLineEditor>("c_if");
    mCDo = mDlgEditor->getChild<LLLineEditor>("c_do");
    mDlgEditor->getChild<LLButton>("c_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddChoice(); });
    mDlgEditor->getChild<LLButton>("c_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mNChoices->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mNChoiceList.size()) { mDError->setText(std::string("Select a choice first.")); return; }
        mNChoiceList.erase(mNChoiceList.begin() + i);
        fillChoices();
    });
    mDlgEditor->getChild<LLButton>("n_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onKeepNode(); });
    mDError = mDlgEditor->getChild<LLTextBox>("d_error");
    mDlgEditor->getChild<LLButton>("d_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeDialogue(); });
    mDlgEditor->getChild<LLButton>("d_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDialogueSave(); });

    // trader
    mSOn = mPanel->getChild<LLCheckBoxCtrl>("s_on");
    mSTitle = mPanel->getChild<LLLineEditor>("s_title");
    mSLines = mPanel->getChild<LLScrollListCtrl>("s_lines");
    mSKind = mPanel->getChild<LLComboBox>("s_kind");
    mSKind->add("sells", LLSD("sell"));
    mSKind->add("buys", LLSD("buy"));
    mSKind->selectFirstItem();
    mSItem = mPanel->getChild<LLLineEditor>("s_item");
    mSQty = mPanel->getChild<LLSpinCtrl>("s_qty");
    mSPrice = mPanel->getChild<LLSpinCtrl>("s_price");
    mSLabel = mPanel->getChild<LLLineEditor>("s_label");
    mSFriend = mPanel->getChild<LLSpinCtrl>("s_friend");
    mSDiscount = mPanel->getChild<LLSpinCtrl>("s_discount");
    mSRefuse = mPanel->getChild<LLSpinCtrl>("s_refuse");
    mSError = mPanel->getChild<LLTextBox>("s_error");
    mPanel->getChild<LLButton>("s_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShopAdd(); });
    mPanel->getChild<LLButton>("s_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mSLines->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mShopLines.size()) { mSError->setText(std::string("Select a line first.")); return; }
        mShopLines.erase(mShopLines.begin() + i);
        fillShopLines();
    });
    mPanel->getChild<LLButton>("s_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShopSave(); });

    // pet
    mPKey = mPanel->getChild<LLLineEditor>("p_key");
    mPPlay = mPanel->getChild<LLLineEditor>("p_play");
    mPAdd = mPanel->getChild<LLSpinCtrl>("p_add");
    mPCard = mPanel->getChild<LLTextBox>("p_card");
    mPanel->getChild<LLButton>("p_link")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string key = trimmed(mPKey->getText());
        if (key.empty()) { mError("Give the pet's key in the region's game first."); return; }
        const std::string play = trimmed(mPPlay->getText());
        // Source: §17 pet {key, play (a pet stat name or ""), playAdd 1-20}
        LLSD pet;
        pet["key"] = key;
        pet["play"] = play;
        pet["playAdd"] = wireInt((F64)mPAdd->get());
        LLSD changes;
        changes["pet"] = pet;
        saveAndRefresh(changes, "Pet linked. It takes the pet's name once the region has read it.");
    });
    mPanel->getChild<LLButton>("p_unlink")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLSD changes;
        changes["pet"] = LLSD();   // null: unlinked
        saveAndRefresh(changes, "Pet unlinked.");
    });

    // events
    mEvents = mPanel->getChild<LLScrollListCtrl>("events");
    mEvents->setDoubleClickCallback([this]() { editEvent(selectedValue(mEvents)); });
    mEventsCount = mPanel->getChild<LLTextBox>("events_count");
    mPanel->getChild<LLButton>("ev_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { editEvent(""); });
    mPanel->getChild<LLButton>("ev_change")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (selectedValue(mEvents).empty()) { mError("Select an event in the list first."); return; }
        editEvent(selectedValue(mEvents));
    });
    mPanel->getChild<LLButton>("ev_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string id = selectedValue(mEvents);
        if (id.empty()) { mError("Select an event in the list first."); return; }
        mAsk("Remove the event " + id + "?", "Remove the event", [this, id]()
        {
            LLSD changes;
            changes["events"][id] = LLSD();
            closeEvent();
            saveAndRefresh(changes, "Event removed.");
        });
    });
    mEvEditor = mPanel->getChild<LLPanel>("ev_editor");
    mEId = mEvEditor->getChild<LLLineEditor>("e_id");
    mETitle = mEvEditor->getChild<LLLineEditor>("e_title");
    mEWhen = mEvEditor->getChild<LLComboBox>("e_when");
    fillCombo(mEWhen, WHENS);
    mEWhen->setCommitCallback([this](LLUICtrl*, const LLSD&) { mEAt->setEnabled(mEWhen->getValue().asString() == "time"); });
    mEAt = mEvEditor->getChild<LLLineEditor>("e_at");
    mEDuration = mEvEditor->getChild<LLSpinCtrl>("e_duration");
    mECooldown = mEvEditor->getChild<LLSpinCtrl>("e_cooldown");
    mESay = mEvEditor->getChild<LLLineEditor>("e_say");
    mESpawn = mEvEditor->getChild<LLScrollListCtrl>("e_spawn");
    mEFrom = mEvEditor->getChild<LLComboBox>("e_from");
    mECount = mEvEditor->getChild<LLSpinCtrl>("e_count");
    mESpawnAt = mEvEditor->getChild<LLLineEditor>("e_spawn_at");
    mERadius = mEvEditor->getChild<LLSpinCtrl>("e_radius");
    mEGroupTag = mEvEditor->getChild<LLLineEditor>("e_group_tag");
    mEGroupDo = mEvEditor->getChild<LLComboBox>("e_group_do");
    fillCombo(mEGroupDo, GROUP_DOS);
    mEDo = mEvEditor->getChild<LLLineEditor>("e_do");
    mEEnd = mEvEditor->getChild<LLLineEditor>("e_end");
    mEKeep = mEvEditor->getChild<LLCheckBoxCtrl>("e_keep");
    mEError = mEvEditor->getChild<LLTextBox>("e_error");
    mEvEditor->getChild<LLButton>("e_spawn_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSpawnAdd(); });
    mEvEditor->getChild<LLButton>("e_spawn_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mESpawn->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mSpawnList.size()) { mEError->setText(std::string("Select a line first.")); return; }
        mSpawnList.erase(mSpawnList.begin() + i);
        fillSpawns();
    });
    mEvEditor->getChild<LLButton>("e_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeEvent(); });
    mEvEditor->getChild<LLButton>("e_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEventSave(); });

    // touch
    mTDo = mPanel->getChild<LLLineEditor>("t_do");
    mTFaction = mPanel->getChild<LLLineEditor>("t_faction");
    mTHandName = mPanel->getChild<LLLineEditor>("t_hand_name");
    mTError = mPanel->getChild<LLTextBox>("t_error");
    mPanel->getChild<LLButton>("t_hand_add")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string name = trimmed(mTHandName->getText());
        if (name.empty() || name.find_first_of(";|") != std::string::npos) { mTError->setText(std::string("Type the item's name (without ; or |) first.")); return; }
        std::string acts = trimmed(mTDo->getText());
        mTDo->setText(acts.empty() ? "handout:" + name : acts + "; handout:" + name);
        mTHandName->setText(LLStringUtil::null);
        mTError->setText(std::string("Added. Press Save to keep it."));
    });
    mPanel->getChild<LLButton>("t_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTouchSave(); });
}

// ─── shared ─────────────────────────────────────────────────────────────────────────────────────────

// static
bool WolfNPCGame::hasGameParts(const LLSD& cfg)
{
    // Source: §14.1 - every key empty/null in a new cfg (off by default)
    if (!cfg.isMap()) return false;
    const LLSD& g = cfg["game"];
    if (g["huntable"].asBoolean() || g["hunt"].asBoolean()) return true;
    for (const char* k : { "dialogues", "quests", "events" })
    {
        if (cfg[k].isMap() && cfg[k].size() > 0) return true;
    }
    if (cfg["touch"].isArray() && cfg["touch"].size() > 0) return true;
    if (cfg["shop"].isMap() || cfg["pet"].isMap()) return true;
    return !cfg["faction"].asString().empty();
}

bool WolfNPCGame::editing() const
{
    return mQuestEditor->getVisible() || mDlgEditor->getVisible() || mEvEditor->getVisible();
}

// static
std::vector<std::string> WolfNPCGame::splitActions(const std::string& text)
{
    std::vector<std::string> parts, out;
    LLStringUtil::getTokens(text, parts, ";");
    for (std::string p : parts)
    {
        p = trimmed(p);
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

// static
std::string WolfNPCGame::joinActions(const LLSD& list)
{
    std::string s;
    for (const LLSD& a : llsd::inArray(list))
    {
        if (!s.empty()) s += "; ";
        s += cap(a.asString(), 200);
    }
    return s;
}

// static
std::string WolfNPCGame::checkActions(const std::vector<std::string>& acts, size_t max, bool takes_ok)
{
    if (acts.size() > max) return llformat("At most %d actions here.", (S32)max);
    for (const std::string& a : acts)
    {
        // Source: §12 consent - "Only a player's own click (a dialogue choice, a shop button) may TAKE from them"
        if (!takes_ok && (a.rfind("take:", 0) == 0 || a.rfind("money:-", 0) == 0))
        {
            return "\"" + cap(a, 40) + "\": only a player's own dialogue choice (or a shop) may take from them.";
        }
        if (a.find('|') != std::string::npos) return "\"" + cap(a, 40) + "\": leave out |.";
        if (utf8Chars(a) > 200) return "An action is at most 200 characters.";
    }
    return std::string();
}

// static
bool WolfNPCGame::idOk(const std::string& id, bool lower_only)
{
    // Source: §14.1 ids [A-Za-z0-9_-]{1,16}; quest ids lower-case [a-z0-9_-]{1,16}
    if (id.empty() || id.size() > 16) return false;
    return allIn(id, lower_only ? LOWER_ID : "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
}

// static
LLSD WolfNPCGame::toArray(const std::vector<std::string>& v)
{
    LLSD a = LLSD::emptyArray();
    for (const std::string& s : v) a.append(s);
    return a;
}

void WolfNPCGame::saveAndRefresh(const LLSD& changes, const std::string& words)
{
    mSave(changes, words, [this](const LLSD& cfg)
    {
        if (cfg.isMap()) mCfg = cfg;
        fillQuests();
        fillDialogues();
        fillShop();
        fillPet();
        fillEvents();
        fillTouch();
    });
}

void WolfNPCGame::setNpc(const std::string& id, const LLSD& cfg, const LLSD& state, const LLSD& npcs)
{
    const bool same = id == mId;
    mId = id;
    mCfg = cfg.isMap() ? cfg : LLSD::emptyMap();
    mState = state;
    mNpcs = npcs;
    if (!same)
    {
        closeQuest();
        closeDialogue();
        closeEvent();
    }
    fillQuests();
    fillDialogues();
    fillShop();
    fillPet();
    fillEvents();
    fillTouch();
}

// ─── quests (§15) ───────────────────────────────────────────────────────────────────────────────────

void WolfNPCGame::fillQuests()
{
    const std::string sel = selectedValue(mQuests);
    mQuests->deleteAllItems();
    const LLSD& q = mCfg["quests"];
    S32 n = 0;
    if (q.isMap())
    {
        for (auto it = q.beginMap(); it != q.endMap(); ++it)
        {
            if (!it->second.isMap()) continue;
            ++n;
            mQuests->addElement(row(LLSD(it->first), { { "id", it->first }, { "title", cap(it->second["title"].asString(), 48) },
                                                        { "steps", llformat("%d", (S32)it->second["steps"].size()) } }));
        }
    }
    if (n == 0) mQuests->setCommentText("No quests yet. New quest: give game players something to do for it.");
    if (!sel.empty()) mQuests->selectByValue(LLSD(sel));
    mQuestsCount->setText(llformat("%d of %d quests", n, MAX_QUESTS));
}

void WolfNPCGame::editQuest(const std::string& id)
{
    const LLSD& quests = mCfg["quests"];
    S32 count = 0;
    if (quests.isMap()) for (auto it = quests.beginMap(); it != quests.endMap(); ++it) count += it->second.isMap() ? 1 : 0;
    if (id.empty() && count >= MAX_QUESTS) { mError("It already gives 8 quests, the most it can. Remove one first."); return; }
    mQEditing = id;
    const LLSD q = id.empty() ? LLSD::emptyMap() : quests[id];
    mQId->setText(id);
    mQTitle->setText(cap(q["title"].asString(), 48));
    mQText->setText(cap(q["text"].asString(), 500));
    mQNeeds->setText(cap(q["needs"].asString(), 120));
    mQRepeat->set(q["repeat"].asBoolean());
    mQStepList.clear();
    for (const LLSD& s : llsd::inArray(q["steps"])) mQStepList.push_back(s);
    fillQuestSteps();
    const LLSD& rw = q["reward"];
    mQMoney->setValue(LLSD(rw["money"].asReal()));
    mQXp->setValue(LLSD(rw["xp"].asReal()));
    std::string items;
    if (rw["items"].isMap())
    {
        for (auto it = rw["items"].beginMap(); it != rw["items"].endMap(); ++it)
        {
            if (!items.empty()) items += ", ";
            items += cap(it->first, 24) + ":" + llformat("%d", it->second.asInteger());
        }
    }
    mQItems->setText(items);
    mQDo->setText(joinActions(rw["do"]));
    mQStepDo->selectFirstItem();
    onQuestStepDo();
    mQError->setText(LLStringUtil::null);
    mQuestEditor->setVisible(true);
    mQId->setFocus(true);
}

void WolfNPCGame::closeQuest()
{
    mQuestEditor->setVisible(false);
    mQEditing.clear();
    mQStepList.clear();
}

void WolfNPCGame::onQuestStepDo()
{
    const std::string d = mQStepDo->getValue().asString();
    mQStepWhat->setLabel(d == "kill" ? std::string("which, e.g. tag:wheat-thief") : d == "go" ? std::string("a place name, or x,y,z")
                         : d == "talk" ? std::string("NPC's name (empty = this one)") : std::string("item, e.g. wheat"));
    mQStepNpc->setVisible(d == "give");
    mQStepN->setVisible(d == "kill" || d == "bring" || d == "give");
    mQStepR->setVisible(d == "go");
}

void WolfNPCGame::fillQuestSteps()
{
    mQSteps->deleteAllItems();
    S32 i = 0;
    for (const LLSD& s : mQStepList)
    {
        const std::string d = s["do"].asString();
        std::string what = d == "kill" ? cap(s["what"].asString(), 48) : d == "go" ? placeText(s["place"]) : d == "talk" ? cap(s["npc"].asString(), 48)
                                                                                                          : cap(s["item"].asString(), 24);
        if (d == "kill" || d == "bring" || d == "give") what += llformat(" x%d", s["n"].asInteger());
        if (d == "give") what += " to " + cap(s["npc"].asString(), 48);
        if (d == "talk" && what.empty()) what = "this NPC";
        mQSteps->addElement(row(LLSD(i++), { { "do", labelOf(STEP_DOS, d) }, { "what", what }, { "text", cap(s["text"].asString(), 64) } }));
    }
    if (mQStepList.empty()) mQSteps->setCommentText("No steps yet: fill in the row below and press Add step.");
}

void WolfNPCGame::onQuestAddStep()
{
    mQError->setText(LLStringUtil::null);
    if ((S32)mQStepList.size() >= MAX_STEPS) { mQError->setText(std::string("A quest has at most 8 steps.")); return; }
    const std::string d = mQStepDo->getValue().asString();
    const std::string what = trimmed(mQStepWhat->getText());
    const std::string text = trimmed(mQStepText->getText());
    // Source: §15.1 steps - kill {what, n}, bring {item, n}, give {item, n, npc}, go {place, r}, talk {npc}; text 1-64; event
    if (text.empty() || utf8Chars(text) > 64) { mQError->setText(std::string("Give the step its quest log line (1 to 64 characters).")); return; }
    LLSD s = LLSD::emptyMap();
    s["do"] = d;
    s["text"] = text;
    if (d == "kill")
    {
        if (what.empty()) { mQError->setText(std::string("Say which NPCs to defeat, e.g. tag:wheat-thief or kind:wolf.")); return; }
        s["what"] = what;
    }
    else if (d == "bring" || d == "give")
    {
        if (!allIn(lower(what), ITEM_CHARS) || what.size() > 24) { mQError->setText(std::string("An item is one word of a-z, 0-9 and _ (up to 24), e.g. wheat.")); return; }
        s["item"] = lower(what);
        if (d == "give")
        {
            const std::string npc = trimmed(mQStepNpc->getText());
            if (npc.empty()) { mQError->setText(std::string("Say which NPC it is given to (its name).")); return; }
            s["npc"] = npc;
        }
    }
    else if (d == "go")
    {
        if (what.empty()) { mQError->setText(std::string("Say where: one of its places, or x,y,z.")); return; }
        s["place"] = placeValue(what);
        s["r"] = wireInt((F64)mQStepR->get());
    }
    else if (d == "talk") s["npc"] = what;
    if (d == "kill" || d == "bring" || d == "give") s["n"] = wireInt((F64)mQStepN->get());
    const std::string ev = trimmed(mQStepEvent->getText());
    if (!ev.empty())
    {
        if (!idOk(ev, false)) { mQError->setText(std::string("The event is one of its event ids (a-z, A-Z, 0-9, _ -).")); return; }
        s["event"] = ev;
    }
    mQStepList.push_back(s);
    fillQuestSteps();
    mQStepWhat->setText(LLStringUtil::null);
    mQStepNpc->setText(LLStringUtil::null);
    mQStepText->setText(LLStringUtil::null);
    mQStepEvent->setText(LLStringUtil::null);
}

void WolfNPCGame::onQuestSave()
{
    mQError->setText(LLStringUtil::null);
    const std::string id = lower(trimmed(mQId->getText()));
    const std::string title = trimmed(mQTitle->getText());
    const std::string text = trimmed(mQText->getText());
    const std::string needs = trimmed(mQNeeds->getText());
    // Source: §15.1 - title 1-48, offer text 1-500, needs a WHO list, steps 1-8, reward money 0-1000000, items up to 8, xp,
    // do up to 6 (no takes)
    if (!idOk(id, true)) { mQError->setText(std::string("The id is 1 to 16 of a-z, 0-9, _ and -.")); return; }
    if (title.empty() || utf8Chars(title) > 48) { mQError->setText(std::string("Give the quest a title (1 to 48 characters).")); return; }
    if (text.empty() || utf8Chars(text) > 500) { mQError->setText(std::string("Write what it says when it offers the quest (1 to 500 characters).")); return; }
    if (mQStepList.empty()) { mQError->setText(std::string("Add at least one step.")); return; }
    if (id != mQEditing && mCfg["quests"].has(id) && mCfg["quests"][id].isMap()) { mQError->setText(std::string("It already has a quest with that id.")); return; }
    LLSD items = LLSD::emptyMap();
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(mQItems->getText(), parts, ",");
        for (std::string p : parts)
        {
            p = trimmed(p);
            if (p.empty()) continue;
            const size_t c = p.find(':');
            const std::string item = lower(trimmed(p.substr(0, c)));
            S32 qty = 1;
            if (c != std::string::npos && !LLStringUtil::convertToS32(trimmed(p.substr(c + 1)), qty)) qty = 0;
            if (!allIn(item, ITEM_CHARS) || item.size() > 24 || qty < 1 || qty > 1000)
            {
                mQError->setText("Reward items are written item:number, e.g. bread:2 (\"" + cap(p, 30) + "\" is not).");
                return;
            }
            items[item] = qty;
        }
        if ((S32)items.size() > MAX_ITEMS) { mQError->setText(std::string("A reward has at most 8 kinds of item.")); return; }
    }
    const std::vector<std::string> acts = splitActions(mQDo->getText());
    const std::string bad = checkActions(acts, MAX_ACTS, false);
    if (!bad.empty()) { mQError->setText(bad); return; }
    LLSD steps = LLSD::emptyArray();
    for (const LLSD& s : mQStepList) steps.append(s);
    LLSD reward = LLSD::emptyMap();
    reward["money"] = wireInt((F64)mQMoney->get());
    reward["items"] = items;
    reward["xp"] = wireInt((F64)mQXp->get());
    reward["do"] = toArray(acts);
    LLSD def = LLSD::emptyMap();
    def["title"] = title;
    def["text"] = text;
    def["needs"] = needs;
    def["repeat"] = mQRepeat->get();
    def["steps"] = steps;
    def["reward"] = reward;
    LLSD changes;
    changes["quests"][id] = def;
    if (!mQEditing.empty() && mQEditing != id) changes["quests"][mQEditing] = LLSD();   // renamed: the old id goes
    closeQuest();
    saveAndRefresh(changes, "Quest saved: " + title + ".");
}

// ─── dialogues (§14.4) ──────────────────────────────────────────────────────────────────────────────

void WolfNPCGame::fillDialogues()
{
    const std::string sel = selectedValue(mDialogues);
    mDialogues->deleteAllItems();
    const LLSD& d = mCfg["dialogues"];
    S32 n = 0;
    if (d.isMap())
    {
        for (auto it = d.beginMap(); it != d.endMap(); ++it)
        {
            if (!it->second.isMap()) continue;
            ++n;
            mDialogues->addElement(row(LLSD(it->first), { { "id", it->first }, { "title", cap(it->second["title"].asString(), 48) },
                                                           { "nodes", llformat("%d", (S32)it->second["nodes"].size()) } }));
        }
    }
    if (n == 0) mDialogues->setCommentText("No dialogues yet. New dialogue: lines it says and the choices players click.");
    if (!sel.empty()) mDialogues->selectByValue(LLSD(sel));
    mDialoguesCount->setText(llformat("%d of %d dialogues", n, MAX_DIALOGUES));
}

void WolfNPCGame::editDialogue(const std::string& id)
{
    const LLSD& all = mCfg["dialogues"];
    S32 count = 0;
    if (all.isMap()) for (auto it = all.beginMap(); it != all.endMap(); ++it) count += it->second.isMap() ? 1 : 0;
    if (id.empty() && count >= MAX_DIALOGUES) { mError("It already has 8 dialogues, the most it can. Remove one first."); return; }
    mDEditing = id;
    const LLSD d = id.empty() ? LLSD::emptyMap() : all[id];
    mDId->setText(id);
    mDTitle->setText(cap(d["title"].asString(), 48));
    mDNodesMap = d["nodes"].isMap() ? d["nodes"] : LLSD::emptyMap();
    fillNodes();
    selectOr(mDStart, d["start"].asString());
    loadNode("");
    mDError->setText(LLStringUtil::null);
    mDlgEditor->setVisible(true);
    mDId->setFocus(true);
}

void WolfNPCGame::closeDialogue()
{
    mDlgEditor->setVisible(false);
    mDEditing.clear();
    mDNodesMap = LLSD::emptyMap();
    mNEditing.clear();
    mNChoiceList.clear();
}

void WolfNPCGame::fillNodes()
{
    const std::string sel = selectedValue(mDNodes);
    const std::string start = mDStart->getValue().asString();
    mDNodes->deleteAllItems();
    for (auto it = mDNodesMap.beginMap(); it != mDNodesMap.endMap(); ++it)
    {
        mDNodes->addElement(row(LLSD(it->first), { { "id", it->first }, { "text", cap(it->second["text"].asString(), 120) } }));
    }
    if (mDNodesMap.size() == 0) mDNodes->setCommentText("No lines yet: write the first one below and press Keep this line.");
    if (!sel.empty()) mDNodes->selectByValue(LLSD(sel));
    mDStart->removeall();
    for (auto it = mDNodesMap.beginMap(); it != mDNodesMap.endMap(); ++it) mDStart->add(it->first, LLSD(it->first));
    selectOr(mDStart, start);
    fillNodeCombos();
}

void WolfNPCGame::fillNodeCombos()
{
    for (LLComboBox* c : { mCNext, mCFail })
    {
        const std::string was = c->getSimple();
        c->removeall();
        c->add("(ends)", LLSD(""));
        for (auto it = mDNodesMap.beginMap(); it != mDNodesMap.endMap(); ++it) c->add(it->first, LLSD(it->first));
        if (!c->selectByValue(LLSD(was))) c->selectFirstItem();
    }
}

void WolfNPCGame::loadNode(const std::string& id)
{
    mNEditing = id;
    const LLSD n = id.empty() ? LLSD::emptyMap() : mDNodesMap[id];
    mNId->setText(id);
    mNText->setText(cap(n["text"].asString(), 500));
    mNDo->setText(joinActions(n["do"]));
    mNChoiceList.clear();
    for (const LLSD& c : llsd::inArray(n["choices"])) mNChoiceList.push_back(c);
    fillChoices();
    mCLabel->setText(LLStringUtil::null);
    mCIf->setText(LLStringUtil::null);
    mCDo->setText(LLStringUtil::null);
    fillNodeCombos();
}

void WolfNPCGame::fillChoices()
{
    mNChoices->deleteAllItems();
    S32 i = 0;
    for (const LLSD& c : mNChoiceList)
    {
        const std::string next = c["next"].asString(), fail = c["fail"].asString();
        mNChoices->addElement(row(LLSD(i++), { { "label", cap(c["label"].asString(), 24) }, { "next", next.empty() ? std::string("(ends)") : next },
                                               { "fail", fail.empty() ? std::string("-") : fail }, { "if", cap(c["if"].asString(), 120) },
                                               { "do", joinActions(c["do"]) } }));
    }
    if (mNChoiceList.empty()) mNChoices->setCommentText("No choices: players get one OK button.");
}

void WolfNPCGame::onAddChoice()
{
    mDError->setText(LLStringUtil::null);
    if ((S32)mNChoiceList.size() >= MAX_CHOICES) { mDError->setText(std::string("A line has at most 10 choices.")); return; }
    const std::string label = trimmed(mCLabel->getText());
    if (label.empty() || utf8Chars(label) > 24) { mDError->setText(std::string("Give the choice its button words (1 to 24 characters).")); return; }
    for (const LLSD& c : mNChoiceList)
    {
        if (c["label"].asString() == label) { mDError->setText(std::string("This line already has a choice with those words.")); return; }
    }
    const std::string next = trimmed(mCNext->getValue().asString().empty() && mCNext->getSimple() != "(ends)" ? mCNext->getSimple() : mCNext->getValue().asString());
    const std::string fail = trimmed(mCFail->getValue().asString().empty() && mCFail->getSimple() != "(ends)" ? mCFail->getSimple() : mCFail->getValue().asString());
    for (const std::string& t : { next, fail })
    {
        if (!t.empty() && !idOk(t, false)) { mDError->setText("\"" + cap(t, 20) + "\" is not a line id (a-z, A-Z, 0-9, _ -)."); return; }
    }
    const std::string cond = trimmed(mCIf->getText());
    if (utf8Chars(cond) > 120) { mDError->setText(std::string("The condition is at most 120 characters.")); return; }
    const std::vector<std::string> acts = splitActions(mCDo->getText());
    const std::string bad = checkActions(acts, MAX_ACTS, true);   // a player's own choice may take (§12 consent)
    if (!bad.empty()) { mDError->setText(bad); return; }
    // Source: §14.4 choice {label, next, fail, do, if}
    LLSD c = LLSD::emptyMap();
    c["label"] = label;
    c["next"] = next;
    if (!fail.empty()) c["fail"] = fail;
    c["do"] = toArray(acts);
    if (!cond.empty()) c["if"] = cond;
    mNChoiceList.push_back(c);
    fillChoices();
    mCLabel->setText(LLStringUtil::null);
    mCIf->setText(LLStringUtil::null);
    mCDo->setText(LLStringUtil::null);
    mDError->setText(std::string("Choice added. Press Keep this line when the line is ready."));
}

void WolfNPCGame::onKeepNode()
{
    mDError->setText(LLStringUtil::null);
    const std::string id = trimmed(mNId->getText());
    const std::string text = trimmed(mNText->getText());
    if (!idOk(id, false)) { mDError->setText(std::string("Give the line an id: 1 to 16 of a-z, A-Z, 0-9, _ and -.")); return; }
    if (text.empty() || utf8Chars(text) > 500) { mDError->setText(std::string("Write what it says (1 to 500 characters).")); return; }
    if (id != mNEditing && mDNodesMap.has(id)) { mDError->setText(std::string("There is already a line with that id.")); return; }
    if (mNEditing.empty() && (S32)mDNodesMap.size() >= MAX_NODES) { mDError->setText(std::string("A dialogue has at most 32 lines.")); return; }
    const std::vector<std::string> acts = splitActions(mNDo->getText());
    const std::string bad = checkActions(acts, MAX_ACTS, false);   // a node's do: no takes (§14.4)
    if (!bad.empty()) { mDError->setText(bad); return; }
    LLSD choices = LLSD::emptyArray();
    for (const LLSD& c : mNChoiceList) choices.append(c);
    LLSD node = LLSD::emptyMap();
    node["text"] = text;
    node["do"] = toArray(acts);
    node["choices"] = choices;
    if (!mNEditing.empty() && mNEditing != id)
    {
        // renamed: the old id goes, and every reference to it follows
        mDNodesMap.erase(mNEditing);
        for (auto it = mDNodesMap.beginMap(); it != mDNodesMap.endMap(); ++it)
        {
            LLSD& cs = it->second["choices"];
            for (S32 i = 0; i < (S32)cs.size(); ++i)
            {
                if (cs[i]["next"].asString() == mNEditing) cs[i]["next"] = id;
                if (cs[i]["fail"].asString() == mNEditing) cs[i]["fail"] = id;
            }
        }
        for (LLSD& c : mNChoiceList)
        {
            if (c["next"].asString() == mNEditing) c["next"] = id;
            if (c["fail"].asString() == mNEditing) c["fail"] = id;
        }
        node["choices"] = LLSD::emptyArray();
        for (const LLSD& c : mNChoiceList) node["choices"].append(c);
    }
    const bool start_moves = !mNEditing.empty() && mDStart->getValue().asString() == mNEditing;
    const bool first = mDNodesMap.size() == 0;
    mDNodesMap[id] = node;
    fillNodes();
    if (first || start_moves) mDStart->selectByValue(LLSD(id));
    mNEditing = id;
    mDNodes->selectByValue(LLSD(id));
    mDError->setText("Line \"" + id + "\" kept. Press Save dialogue to send it to the region.");
}

void WolfNPCGame::onDialogueSave()
{
    mDError->setText(LLStringUtil::null);
    const std::string id = trimmed(mDId->getText());
    const std::string title = trimmed(mDTitle->getText());
    if (!idOk(id, false)) { mDError->setText(std::string("The dialogue id is 1 to 16 of a-z, A-Z, 0-9, _ and -.")); return; }
    if (utf8Chars(title) > 48) { mDError->setText(std::string("The title is at most 48 characters.")); return; }
    if (mDNodesMap.size() == 0) { mDError->setText(std::string("Write at least one line and press Keep this line.")); return; }
    if (id != mDEditing && mCfg["dialogues"].has(id) && mCfg["dialogues"][id].isMap()) { mDError->setText(std::string("It already has a dialogue with that id.")); return; }
    const std::string start = mDStart->getValue().asString();
    if (!mDNodesMap.has(start)) { mDError->setText(std::string("Choose the line it starts with.")); return; }
    // Source: §14.4 "next/fail/start must name a node of the same dialogue ("" = end)"
    for (auto it = mDNodesMap.beginMap(); it != mDNodesMap.endMap(); ++it)
    {
        for (const LLSD& c : llsd::inArray(it->second["choices"]))
        {
            for (const char* k : { "next", "fail" })
            {
                const std::string t = c[k].asString();
                if (!t.empty() && !mDNodesMap.has(t))
                {
                    mDError->setText("Line \"" + it->first + "\", choice \"" + cap(c["label"].asString(), 24) + "\": there is no line \"" + t + "\" yet.");
                    return;
                }
            }
        }
    }
    LLSD def = LLSD::emptyMap();
    def["title"] = title;
    def["start"] = start;
    def["nodes"] = mDNodesMap;
    LLSD changes;
    changes["dialogues"][id] = def;
    if (!mDEditing.empty() && mDEditing != id) changes["dialogues"][mDEditing] = LLSD();
    closeDialogue();
    saveAndRefresh(changes, "Dialogue saved. Use dialog:" + id + " in a touch, reaction or choice to open it.");
}

// ─── trader (§16) ───────────────────────────────────────────────────────────────────────────────────

void WolfNPCGame::fillShop()
{
    const LLSD& s = mCfg["shop"];
    mSOn->set(s.isMap());
    mSTitle->setText(cap(s["title"].asString(), 48));
    mShopLines.clear();
    for (const char* kind : { "sell", "buy" })
    {
        for (const LLSD& l : llsd::inArray(s[kind]))
        {
            LLSD x = l;
            x["kind"] = kind;
            mShopLines.push_back(x);
        }
    }
    mSFriend->setValue(LLSD(s.has("friendAt") ? s["friendAt"].asReal() : 50.0));
    mSDiscount->setValue(LLSD(s.has("discount") ? s["discount"].asReal() : 10.0));
    mSRefuse->setValue(LLSD(s.has("refuseBelow") ? s["refuseBelow"].asReal() : -50.0));
    mSError->setText(LLStringUtil::null);
    fillShopLines();
}

void WolfNPCGame::fillShopLines()
{
    mSLines->deleteAllItems();
    S32 i = 0;
    for (const LLSD& l : mShopLines)
    {
        mSLines->addElement(row(LLSD(i++), { { "kind", l["kind"].asString() == "sell" ? std::string("sells") : std::string("buys") },
                                             { "item", cap(l["item"].asString(), 24) }, { "qty", llformat("%d", l["qty"].asInteger()) },
                                             { "price", llformat("%d", l["price"].asInteger()) }, { "label", cap(l["label"].asString(), 16) } }));
    }
    if (mShopLines.empty()) mSLines->setCommentText("Nothing to trade yet: fill in the row below and press Add line.");
}

void WolfNPCGame::onShopAdd()
{
    mSError->setText(LLStringUtil::null);
    if ((S32)mShopLines.size() >= MAX_SHOP) { mSError->setText(std::string("A trader has at most 10 lines in all.")); return; }
    const std::string item = lower(trimmed(mSItem->getText()));
    if (!allIn(item, ITEM_CHARS) || item.size() > 24) { mSError->setText(std::string("An item is one word of a-z, 0-9 and _ (up to 24), e.g. meat.")); return; }
    const std::string label = trimmed(mSLabel->getText());
    if (utf8Chars(label) > 16) { mSError->setText(std::string("The button word is at most 16 characters.")); return; }
    LLSD l = LLSD::emptyMap();
    l["kind"] = mSKind->getValue().asString();
    l["item"] = item;
    l["qty"] = wireInt((F64)mSQty->get());
    l["price"] = wireInt((F64)mSPrice->get());
    if (!label.empty()) l["label"] = label;
    mShopLines.push_back(l);
    fillShopLines();
    mSItem->setText(LLStringUtil::null);
    mSLabel->setText(LLStringUtil::null);
}

void WolfNPCGame::onShopSave()
{
    mSError->setText(LLStringUtil::null);
    LLSD changes;
    if (!mSOn->get())
    {
        changes["shop"] = LLSD();   // null: no shop
        saveAndRefresh(changes, "It no longer trades.");
        return;
    }
    if (mShopLines.empty()) { mSError->setText(std::string("Add at least one thing it sells or buys.")); return; }
    const std::string title = trimmed(mSTitle->getText());
    if (utf8Chars(title) > 48) { mSError->setText(std::string("The title is at most 48 characters.")); return; }
    // Source: §16 shop {title, sell [{item, qty, price, label}], buy [...], friendAt, discount, refuseBelow}
    LLSD shop = LLSD::emptyMap();
    shop["title"] = title;
    shop["sell"] = LLSD::emptyArray();
    shop["buy"] = LLSD::emptyArray();
    for (const LLSD& l : mShopLines)
    {
        LLSD x = l;
        x.erase("kind");
        shop[l["kind"].asString() == "sell" ? "sell" : "buy"].append(x);
    }
    shop["friendAt"] = wireInt((F64)mSFriend->get());
    shop["discount"] = wireInt((F64)mSDiscount->get());
    shop["refuseBelow"] = wireInt((F64)mSRefuse->get());
    changes["shop"] = shop;
    saveAndRefresh(changes, "Trader saved.");
}

// ─── pet (§17) ──────────────────────────────────────────────────────────────────────────────────────

void WolfNPCGame::fillPet()
{
    const LLSD& p = mCfg["pet"];
    mPKey->setText(cap(p["key"].asString(), 64));
    mPPlay->setText(cap(p["play"].asString(), 24));
    mPAdd->setValue(LLSD(p.has("playAdd") ? p["playAdd"].asReal() : 2.0));
    // Source: §17 get state.pet {key, name, type, alive, age, stats [{n, v, m}]} (null when none read yet)
    const LLSD& sp = mState["pet"];
    std::string card;
    if (!p.isMap()) card = "No pet linked.";
    else if (!sp.isMap()) card = "Linked. The region has not read the pet yet (it does while someone is near).";
    else
    {
        card = cap(sp["name"].asString(), 48) + " (" + cap(sp["type"].asString(), 32) + ")" + (sp["alive"].asBoolean() ? std::string(", alive") : std::string(", dead"));
        if (sp.has("age")) card += llformat(", age %.0f", sp["age"].asReal());
        for (const LLSD& s : llsd::inArray(sp["stats"]))
        {
            card += "\n" + cap(s["n"].asString(), 24) + llformat(": %.0f of %.0f", s["v"].asReal(), s["m"].asReal());
        }
    }
    const std::string why = cap(mState["why"].asString(), 200);
    if (!why.empty() && p.isMap()) card += "\n" + why;
    mPCard->setText(card);
}

// ─── touch (§14.2) and faction (§12.1) ──────────────────────────────────────────────────────────────

void WolfNPCGame::fillTouch()
{
    mTDo->setText(joinActions(mCfg["touch"]));
    mTFaction->setText(cap(mCfg["faction"].asString(), 24));
    mTError->setText(LLStringUtil::null);
}

void WolfNPCGame::onTouchSave()
{
    mTError->setText(LLStringUtil::null);
    const std::vector<std::string> acts = splitActions(mTDo->getText());
    const std::string bad = checkActions(acts, MAX_ACTS, false);   // §14.2 "up to 6 actions ... no takes"
    if (!bad.empty()) { mTError->setText(bad); return; }
    const std::string faction = lower(trimmed(mTFaction->getText()));
    if (!faction.empty() && (faction.size() > 24 || !allIn(faction, LOWER_ID))) { mTError->setText(std::string("A faction is one word of a-z, 0-9, _ and - (up to 24).")); return; }
    LLSD changes;
    changes["touch"] = toArray(acts);
    changes["faction"] = faction;
    saveAndRefresh(changes, "Saved.");
}

// ─── events (§18) ───────────────────────────────────────────────────────────────────────────────────

void WolfNPCGame::fillEvents()
{
    const std::string sel = selectedValue(mEvents);
    mEvents->deleteAllItems();
    const LLSD& e = mCfg["events"];
    const LLSD& running = mState["events"];   // §21 state.events {<id>: {until}}
    S32 n = 0;
    if (e.isMap())
    {
        for (auto it = e.beginMap(); it != e.endMap(); ++it)
        {
            if (!it->second.isMap()) continue;
            ++n;
            const std::string when = it->second["when"].asString() == "time" ? "daily " + cap(it->second["at"].asString(), 5) : std::string("when started");
            mEvents->addElement(row(LLSD(it->first), { { "id", it->first }, { "title", cap(it->second["title"].asString(), 48) }, { "when", when },
                                                        { "state", running.has(it->first) ? std::string("running") : std::string("-") } }));
        }
    }
    if (n == 0) mEvents->setCommentText("No events yet. New event: a wave of copies, at a time or when started.");
    if (!sel.empty()) mEvents->selectByValue(LLSD(sel));
    mEventsCount->setText(llformat("%d of %d events", n, MAX_EVENTS));
}

void WolfNPCGame::editEvent(const std::string& id)
{
    const LLSD& all = mCfg["events"];
    S32 count = 0;
    if (all.isMap()) for (auto it = all.beginMap(); it != all.endMap(); ++it) count += it->second.isMap() ? 1 : 0;
    if (id.empty() && count >= MAX_EVENTS) { mError("It already holds 8 events, the most it can. Remove one first."); return; }
    mEEditing = id;
    const LLSD e = id.empty() ? LLSD::emptyMap() : all[id];
    mEId->setText(id);
    mETitle->setText(cap(e["title"].asString(), 48));
    selectOr(mEWhen, e.has("when") ? e["when"].asString() : std::string("trigger"));
    mEAt->setText(cap(e["at"].asString(), 5));
    mEAt->setEnabled(mEWhen->getValue().asString() == "time");
    mEDuration->setValue(LLSD(e.has("duration") ? e["duration"].asReal() : 1800.0));
    mECooldown->setValue(LLSD(e["cooldown"].asReal()));
    mESay->setText(cap(e["say"].asString(), 300));
    mSpawnList.clear();
    for (const LLSD& s : llsd::inArray(e["spawn"])) mSpawnList.push_back(s);
    // copies of this NPC, or of another of the owner's NPCs here (§18.1 from "self" or a UUID of the SAME owner)
    mEFrom->removeall();
    mEFrom->add("this NPC", LLSD("self"));
    for (const LLSD& n : llsd::inArray(mNpcs))
    {
        if (!n["mine"].asBoolean() || n["id"].asString() == mId) continue;
        mEFrom->add(cap(n["name"].asString(), 48), LLSD(n["id"].asString()));
    }
    mEFrom->selectFirstItem();
    fillSpawns();
    mEGroupTag->setText(cap(e["group"]["tag"].asString(), 24));
    selectOr(mEGroupDo, e["group"].isMap() ? e["group"]["do"].asString() : std::string());
    mEDo->setText(joinActions(e["do"]));
    mEEnd->setText(joinActions(e["end"]));
    mEKeep->set(e["keep"].asBoolean());
    mEError->setText(LLStringUtil::null);
    mEvEditor->setVisible(true);
    mEId->setFocus(true);
}

void WolfNPCGame::closeEvent()
{
    mEvEditor->setVisible(false);
    mEEditing.clear();
    mSpawnList.clear();
}

void WolfNPCGame::fillSpawns()
{
    mESpawn->deleteAllItems();
    S32 i = 0;
    for (const LLSD& s : mSpawnList)
    {
        const std::string from = s["from"].asString();
        std::string from_name = from == "self" ? std::string("this NPC") : from;
        for (const LLSD& n : llsd::inArray(mNpcs))
        {
            if (n["id"].asString() == from) from_name = cap(n["name"].asString(), 48);
        }
        mESpawn->addElement(row(LLSD(i++), { { "from", from_name }, { "count", llformat("%d", s["count"].asInteger()) },
                                             { "at", placeText(s["at"]) }, { "radius", llformat("%d", s["radius"].asInteger()) } }));
    }
    if (mSpawnList.empty()) mESpawn->setCommentText("No copies: the event only says and does things.");
}

void WolfNPCGame::onSpawnAdd()
{
    mEError->setText(LLStringUtil::null);
    if ((S32)mSpawnList.size() >= MAX_SPAWN) { mEError->setText(std::string("An event has at most 4 lines of copies.")); return; }
    S32 total = 0;
    for (const LLSD& s : mSpawnList) total += s["count"].asInteger();
    const S32 count = (S32)std::round(mECount->get());
    if (total + count > MAX_COPIES) { mEError->setText(llformat("An event makes at most 20 copies (this would be %d).", total + count)); return; }
    const std::string at = trimmed(mESpawnAt->getText());
    LLSD s = LLSD::emptyMap();
    s["from"] = mEFrom->getValue().asString();
    s["count"] = count;
    s["at"] = at.empty() ? LLSD("home") : placeValue(at);
    s["radius"] = wireInt((F64)mERadius->get());
    mSpawnList.push_back(s);
    fillSpawns();
    mESpawnAt->setText(LLStringUtil::null);
}

void WolfNPCGame::onEventSave()
{
    mEError->setText(LLStringUtil::null);
    const std::string id = trimmed(mEId->getText());
    const std::string title = trimmed(mETitle->getText());
    const std::string when = mEWhen->getValue().asString();
    const std::string at = trimmed(mEAt->getText());
    const std::string say = trimmed(mESay->getText());
    if (!idOk(id, false)) { mEError->setText(std::string("The id is 1 to 16 of a-z, A-Z, 0-9, _ and -.")); return; }
    if (utf8Chars(title) > 48) { mEError->setText(std::string("The title is at most 48 characters.")); return; }
    if (id != mEEditing && mCfg["events"].has(id) && mCfg["events"][id].isMap()) { mEError->setText(std::string("It already has an event with that id.")); return; }
    if (when == "time")
    {
        const bool ok = at.size() == 5 && at[2] == ':' && isdigit((unsigned char)at[0]) && isdigit((unsigned char)at[1])
                        && isdigit((unsigned char)at[3]) && isdigit((unsigned char)at[4]) && std::stoi(at.substr(0, 2)) < 24 && std::stoi(at.substr(3, 2)) < 60;
        if (!ok) { mEError->setText(std::string("Write the time like 21:00 (grid time).")); return; }
    }
    if (utf8Chars(say) > 300) { mEError->setText(std::string("What it says is at most 300 characters.")); return; }
    const std::vector<std::string> start_acts = splitActions(mEDo->getText()), end_acts = splitActions(mEEnd->getText());
    std::string bad = checkActions(start_acts, MAX_ACTS, false);
    if (bad.empty()) bad = checkActions(end_acts, MAX_ACTS, false);
    if (!bad.empty()) { mEError->setText(bad); return; }
    const std::string tag = lower(trimmed(mEGroupTag->getText()));
    const std::string gdo = mEGroupDo->getValue().asString();
    if (!gdo.empty() && (tag.empty() || tag.size() > 24 || !allIn(tag, "abcdefghijklmnopqrstuvwxyz0123456789-")))
    {
        mEError->setText(std::string("Give the tag of the NPCs that go home or stand still (a-z, 0-9 and -)."));
        return;
    }
    // Source: §18.1 {title, when, at, duration, cooldown, say, spawn [{from, count, at, radius}], group {tag, do} | null,
    // do, end, keep}
    LLSD def = LLSD::emptyMap();
    def["title"] = title;
    def["when"] = when;
    if (when == "time") def["at"] = at;
    def["duration"] = wireInt((F64)mEDuration->get());
    def["cooldown"] = wireInt((F64)mECooldown->get());
    def["say"] = say;
    LLSD spawn = LLSD::emptyArray();
    for (const LLSD& s : mSpawnList) spawn.append(s);
    def["spawn"] = spawn;
    if (gdo.empty()) def["group"] = LLSD();
    else
    {
        def["group"]["tag"] = tag;
        def["group"]["do"] = gdo;
    }
    def["do"] = toArray(start_acts);
    def["end"] = toArray(end_acts);
    def["keep"] = mEKeep->get();
    LLSD changes;
    changes["events"][id] = def;
    if (!mEEditing.empty() && mEEditing != id) changes["events"][mEEditing] = LLSD();
    closeEvent();
    saveAndRefresh(changes, "Event saved." + std::string(when == "trigger" ? " Start it with event:" + id + ", a quest step, or the Game Master tab." : std::string()));
}
