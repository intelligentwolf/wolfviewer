/**
 * @file wolffloaternpcmind.cpp
 * @brief WolfViewer: WolfNPC window, Edit > Learning and Edit > Places. See wolffloaternpcmind.h.
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

#include "wolffloaternpcmind.h"

#include <cmath>
#include <ctime>
#include <set>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "lltabcontainer.h"
#include "llviewerobjectlist.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llradiogroup.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdutil.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "wolfadventure.h"
#include "wolfai.h"
#include "wolfgame.h"

// Every action, field, limit and step type: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md (sections below).

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

    std::string number(const LLSD& v)
    {
        const F64 d = v.asReal();
        if (!std::isfinite(d)) return "?";
        std::string s = llformat(std::fabs(d) < 1.0 ? "%.3f" : std::fabs(d) < 10.0 ? "%.2f" : "%.1f", d);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        return s.empty() ? std::string("0") : s;
    }

    /** perLookupWeb / perResearch / perLesson: {credits (about), maxCredits (at most)}, or a plain number (round B). */
    void priceOf(const LLSD& v, F64& about, F64& most)
    {
        if (v.isMap()) { about = v["credits"].asReal(); most = v["maxCredits"].asReal(); }
        else { about = v.asReal(); most = about; }
    }

    /** A unix time as the viewer's own clock, "24 Oct 14:05". */
    std::string when(const LLSD& v)
    {
        const F64 d = v.asReal();
        if (!(d > 0.0) || !std::isfinite(d)) return std::string();
        const std::time_t t = (std::time_t)d;
        std::tm tm {};
#if LL_WINDOWS
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%d %b %H:%M", &tm);
        return buf;
    }

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

    LLSD placeValue(const std::string& text)
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(text, parts, ",");
        if (parts.size() == 3)
        {
            LLSD a = LLSD::emptyArray();
            for (std::string p : parts)
            {
                F64 d = 0.0;
                if (!LLStringUtil::convertToF64(trimmed(p), d) || !std::isfinite(d)) return LLSD(trimmed(text));
                a.append(LLSD(std::round(d * 100.0) / 100.0));
            }
            return a;
        }
        return LLSD(trimmed(text));
    }

    std::string placeText(const LLSD& v)
    {
        if (v.isArray() && v.size() >= 3) return llformat("%.0f, %.0f, %.0f", v[0].asReal(), v[1].asReal(), v[2].asReal());
        return cap(v.asString(), 48);
    }

    struct Choice { const char* value; const char* label; };
    // Source: §13.3 - the step types, and nothing else (plain labels; the second field only where the type has one)
    const Choice STEP_TYPES[] = {
        { "say", "Say" }, { "emote", "Do an emote" }, { "wave", "Wave" }, { "anim", "Play an animation" },
        { "sound", "Play a sound" }, { "face", "Turn to face" }, { "follow", "Follow" }, { "goto", "Walk to a place" },
        { "sit", "Sit at a place" }, { "wait", "Wait" }, { "handout", "Hand out an item" }, { "quest", "A quest step" },
        { "routine", "Set its routine" }, { "ifflag", "If the player has a flag" } };
    // Source: §5 npc_learn_teach kind? fact | event | skill
    const Choice TEACH_KINDS[] = { { "fact", "a fact" }, { "event", "an event" }, { "skill", "how to" } };
    // Source: §23.1 tool types
    const Choice TOOL_TYPES[] = { { "whiteboard", "whiteboard" }, { "seat", "seat" }, { "counter", "bar or counter" },
                                  { "vehicle", "vehicle" }, { "dispenser", "dispenser" }, { "place", "place" } };
    // Source: §5 npc_skill_save scope private | owner
    const Choice SCOPES[] = { { "private", "only this NPC" }, { "owner", "all my NPCs" } };

    template <size_t N>
    void fillCombo(LLComboBox* c, const Choice (&list)[N])
    {
        c->removeall();
        for (const Choice& ch : list) c->add(ch.label, LLSD(ch.value));
        c->selectFirstItem();
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

    // Source: §3 places up to 16, names 1-24; §23.1 tools at most 8, role [a-z0-9 _-]{1,24}, face -1..8; §13.3 / §5 skill
    // name <= 48, when <= 300, steps <= 12, wait seconds <= 30; §5 teach text <= 300, role <= 500
    const size_t MAX_PLACES = 16, MAX_TOOLS = 8, MAX_STEPS = 12;
}

WolfNPCMind::WolfNPCMind(LLHandle<LLFloater> owner, LLPanel* learning, LLPanel* places, save_fn save, ask_fn ask, say_fn error, say_fn done)
:   mOwner(owner), mLearning(learning), mPlacesPanel(places), mSave(save), mAsk(ask), mError(error), mDone(done)
{
    // ---- knowledge ----
    mKMode = mLearning->getChild<LLRadioGroup>("k_mode");
    mKMode->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        // Source: §5 npc_learn_set {npc, mode: off | ask | auto}
        LLSD body;
        body["npc"] = mId;
        body["mode"] = mKMode->getValue().asString();
        const U32 serial = mSerial;
        WolfAdventure::api("npc_learn_set", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("The learning mode was not changed: " + error, reply); loadKnowledge(0); return; }
            const std::string m = reply["mode"].asString();
            mDone(m == "off" ? "It no longer learns from talk (what it knows already stays)." : m == "auto" ? "It keeps what it learns at once."
                             : "What it learns now waits for you to approve it.", LLSD());
        });
    });
    mKRole = mLearning->getChild<LLTextEditor>("k_role");
    mLearning->getChild<LLButton>("k_role_save")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string role = trimmed(mKRole->getText());
        if (utf8Chars(role) > 500) { mError("Its job is at most 500 characters.", LLSD()); return; }
        LLSD body;
        body["npc"] = mId;
        body["role"] = role;
        const U32 serial = mSerial;
        WolfAdventure::api("npc_learn_set", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("Its job was not saved: " + error, reply); return; }
            mDone("Its job is saved.", LLSD());
        });
    });
    mKSearch = mLearning->getChild<LLLineEditor>("k_search");
    mKSearch->setKeystrokeCallback([this](LLLineEditor*, void*) { fillKnowledge(); }, nullptr);
    mKCount = mLearning->getChild<LLTextBox>("k_count");
    mKItems = mLearning->getChild<LLScrollListCtrl>("k_items");
    mKItems->setCommitCallback([this](LLUICtrl*, const LLSD&) { mKEdit->setText(cap(selectedItem()["text"].asString(), 300)); });
    mKEdit = mLearning->getChild<LLLineEditor>("k_edit");
    mLearning->getChild<LLButton>("k_approve")->setCommitCallback([this](LLUICtrl*, const LLSD&) { itemOp("approve"); });
    mLearning->getChild<LLButton>("k_reject")->setCommitCallback([this](LLUICtrl*, const LLSD&) { itemOp("reject"); });
    mLearning->getChild<LLButton>("k_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { itemOp("delete"); });
    mLearning->getChild<LLButton>("k_edit_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { itemOp("edit"); });
    mKMore = mLearning->getChild<LLButton>("k_more");
    mKMore->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadKnowledge((S32)mItems.size()); });
    mKTeach = mLearning->getChild<LLLineEditor>("k_teach");
    mKTeachKind = mLearning->getChild<LLComboBox>("k_teach_kind");
    fillCombo(mKTeachKind, TEACH_KINDS);
    mLearning->getChild<LLButton>("k_teach_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const std::string text = trimmed(mKTeach->getText());
        if (text.empty() || utf8Chars(text) > 300) { mError("Type what it should know (1 to 300 characters).", LLSD()); return; }
        // Source: §5 npc_learn_teach {npc, text, kind} -> {item, how: added | strengthened, cost}
        LLSD body;
        body["npc"] = mId;
        body["text"] = text;
        body["kind"] = mKTeachKind->getValue().asString();
        const U32 serial = mSerial;
        WolfAdventure::api("npc_learn_teach", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("It did not learn that: " + error, reply); return; }
            charged(reply);
            mKTeach->setText(LLStringUtil::null);
            mDone(reply["how"].asString() == "strengthened" ? "It knew something like that already: now it is surer of it." : "Learned.", LLSD());
            loadKnowledge(0);
        });
    });
    mKNote = mLearning->getChild<LLTextBox>("k_note");
    mLearning->getChild<LLButton>("k_forget")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const U32 serial = mSerial;
        mAsk("Forget everything it has learned? Every memory goes for good; its settings, skills and job stay.", "Forget everything", [this, life = std::weak_ptr<int>(mLife), serial]()
        {
            if (life.expired() || !alive(serial)) return;
            LLSD body;
            body["npc"] = mId;
            WolfAdventure::api("npc_learn_forget_all", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
            {
                if (life.expired() || !alive(serial)) return;
                if (!ok) { mError("It did not forget: " + error, reply); return; }
                mDone(llformat("Forgotten: %d things.", reply["deleted"].asInteger()), LLSD());
                loadKnowledge(0);
            });
        });
    });

    // ---- skills ----
    mSkList = mLearning->getChild<LLScrollListCtrl>("sk_list");
    mSkList->setDoubleClickCallback([this]() { const LLSD s = selectedSkill(); if (s.isMap() && s["scope"].asString() != "shared") editSkill(s, std::string()); });
    mSkCount = mLearning->getChild<LLTextBox>("sk_count");
    mLearning->getChild<LLButton>("sk_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { editSkill(LLSD::emptyMap(), std::string()); });
    mLearning->getChild<LLButton>("sk_change")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD s = selectedSkill();
        if (!s.isMap()) { mError("Select a skill first.", LLSD()); return; }
        if (s["scope"].asString() == "shared") { mError("A shared skill cannot be changed here: Find it under Asked for and use a copy.", LLSD()); return; }
        editSkill(s, std::string());
    });
    auto skillCall = [this](const std::string& action, const LLSD& extra, const std::string& words)
    {
        const LLSD s = selectedSkill();
        if (!s.isMap()) { mError("Select a skill first.", LLSD()); return; }
        LLSD body = extra.isMap() ? extra : LLSD::emptyMap();
        body["npc"] = mId;
        body["id"] = s["id"];
        const U32 serial = mSerial;
        WolfAdventure::api(action, body, [this, life = std::weak_ptr<int>(mLife), serial, words, action](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok)
            {
                // npc_skill_offer refuses with problems [reasons]
                std::string why = error;
                for (const LLSD& p : llsd::inArray(reply["problems"])) why += " " + cap(p.asString(), 120);
                mError(why, reply);
                return;
            }
            mDone(words, LLSD());
            loadSkills();
        });
    };
    mLearning->getChild<LLButton>("sk_approve")->setCommitCallback([this, skillCall](LLUICtrl*, const LLSD&)
    {
        if (selectedSkill()["status"].asString() != "pending") { mError("Only a skill waiting for you (asked for by a visitor) needs approving.", LLSD()); return; }
        skillCall("npc_skill_approve", LLSD(), "Approved: it can use that skill now.");
    });
    mLearning->getChild<LLButton>("sk_offer")->setCommitCallback([skillCall](LLUICtrl*, const LLSD&)
    {
        // Source: §5 npc_skill_offer - into the staff queue (Grid Admin > NPC Skills), 5 a day
        skillCall("npc_skill_offer", LLSD(), "Sent to Wolf Territories staff for review. Once they approve it, every owner can use it.");
    });
    mLearning->getChild<LLButton>("sk_hide")->setCommitCallback([this, skillCall](LLUICtrl*, const LLSD&)
    {
        const LLSD s = selectedSkill();
        if (s["scope"].asString() != "shared") { mError("Only a shared skill can be hidden; delete one of your own instead.", LLSD()); return; }
        LLSD extra;
        extra["hide"] = !s["hidden"].asBoolean();
        skillCall("npc_skill_hide", extra, s["hidden"].asBoolean() ? "Your NPCs can use that shared skill again." : "Hidden from all your NPCs.");
    });
    mLearning->getChild<LLButton>("sk_delete")->setCommitCallback([this, skillCall](LLUICtrl*, const LLSD&)
    {
        const LLSD s = selectedSkill();
        if (!s.isMap()) { mError("Select a skill first.", LLSD()); return; }
        if (s["scope"].asString() == "shared") { mError("A shared skill is not yours to delete: Hide it instead.", LLSD()); return; }
        mAsk("Delete the skill " + cap(s["name"].asString(), 48) + "?", "Delete the skill", [skillCall]()
        {
            skillCall("npc_skill_delete", LLSD(), "Skill deleted.");
        });
    });
    mSkEditor = mLearning->getChild<LLPanel>("sk_editor");
    mSkName = mSkEditor->getChild<LLLineEditor>("sk_name");
    mSkScope = mSkEditor->getChild<LLComboBox>("sk_scope");
    fillCombo(mSkScope, SCOPES);
    mSkWhen = mSkEditor->getChild<LLLineEditor>("sk_when");
    mSkSteps = mSkEditor->getChild<LLScrollListCtrl>("sk_steps");
    mSkStepDo = mSkEditor->getChild<LLComboBox>("sk_step_do");
    fillCombo(mSkStepDo, STEP_TYPES);
    mSkStepDo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStepDo(); });
    mSkStepA = mSkEditor->getChild<LLLineEditor>("sk_step_a");
    mSkStepB = mSkEditor->getChild<LLLineEditor>("sk_step_b");
    mSkStepThen = mSkEditor->getChild<LLLineEditor>("sk_step_then");
    mSkStepElse = mSkEditor->getChild<LLLineEditor>("sk_step_else");
    mSkStepHelp = mSkEditor->getChild<LLTextBox>("sk_step_help");
    mSkError = mSkEditor->getChild<LLTextBox>("sk_error");
    mSkEditor->getChild<LLButton>("sk_step_add")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        mSkError->setText(LLStringUtil::null);
        if (mStepList.size() >= MAX_STEPS) { mSkError->setText(std::string("A skill has at most 12 steps.")); return; }
        LLSD step;
        std::string why;
        if (!buildStep(step, why)) { mSkError->setText(why); return; }
        mStepList.push_back(step);
        fillSteps();
        mSkStepA->setText(LLStringUtil::null);
        mSkStepB->setText(LLStringUtil::null);
        mSkStepThen->setText(LLStringUtil::null);
        mSkStepElse->setText(LLStringUtil::null);
    });
    mSkEditor->getChild<LLButton>("sk_step_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mSkSteps->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mStepList.size()) { mSkError->setText(std::string("Select a step first.")); return; }
        mStepList.erase(mStepList.begin() + i);
        fillSteps();
    });
    mSkEditor->getChild<LLButton>("sk_step_up")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mSkSteps->getFirstSelectedIndex();
        if (i <= 0 || i >= (S32)mStepList.size()) return;
        std::swap(mStepList[(size_t)i], mStepList[(size_t)i - 1]);
        fillSteps();
        mSkSteps->selectNthItem(i - 1);
    });
    mSkEditor->getChild<LLButton>("sk_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeSkill(); });
    mSkEditor->getChild<LLButton>("sk_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSkillSave(); });

    // ---- asked for, find ----
    mWList = mLearning->getChild<LLScrollListCtrl>("w_list");
    mLearning->getChild<LLButton>("w_make")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mWList->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mWished.size()) { mError("Select something they asked for first.", LLSD()); return; }
        if (LLTabContainer* tabs = mLearning->getChild<LLTabContainer>("mind_tabs")) tabs->selectTabByName("mtab_skills");
        editSkill(LLSD::emptyMap(), cap(mWished[i]["text"].asString(), 300));
    });
    mLearning->getChild<LLButton>("w_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mWList->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mWished.size()) { mError("Select something they asked for first.", LLSD()); return; }
        // Source: §5 npc_skill_wished {npc, delete: id}
        LLSD body;
        body["npc"] = mId;
        body["delete"] = mWished[i]["id"];
        const U32 serial = mSerial;
        WolfAdventure::api("npc_skill_wished", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("It was not removed: " + error, reply); return; }
            mWished = reply["wished"].isArray() ? reply["wished"] : LLSD::emptyArray();
            fillWished();
            mDone("Removed.", LLSD());
        });
    });
    mFText = mLearning->getChild<LLLineEditor>("f_text");
    mFNote = mLearning->getChild<LLTextBox>("f_note");
    mFList = mLearning->getChild<LLScrollListCtrl>("f_list");
    mLearning->getChild<LLButton>("f_find")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFind(); });
    mFText->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFind(); });
    mLearning->getChild<LLButton>("f_adopt")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mFList->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mFound.size()) { mError("Select a skill from the results first.", LLSD()); return; }
        // a copy of its steps becomes one of this NPC's own: npc_skill_get (the whole skill) then npc_skill_save
        LLSD body;
        body["npc"] = mId;
        body["id"] = mFound[i]["id"];
        const U32 serial = mSerial;
        WolfAdventure::api("npc_skill_get", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("That skill could not be read: " + error, reply); return; }
            const LLSD& sk = reply["skill"];
            LLSD save;
            save["npc"] = mId;
            save["skill"]["name"] = sk["name"];
            save["skill"]["when"] = sk["when"];
            save["skill"]["steps"] = sk["steps"];
            save["skill"]["scope"] = "private";
            WolfAdventure::api("npc_skill_save", save, [this, life = std::weak_ptr<int>(mLife), serial](bool ok2, const LLSD& reply2, const std::string& error2)
            {
                if (life.expired() || !alive(serial)) return;
                if (!ok2) { mError("It could not take that skill: " + error2, reply2); return; }
                charged(reply2);
                mDone("It has that skill now (a copy of its own, under Skills).", LLSD());
                loadSkills();
            });
        });
    });
    mLearning->getChild<LLButton>("f_hide")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mFList->getFirstSelectedIndex();
        if (i < 0 || i >= (S32)mFound.size()) { mError("Select a skill from the results first.", LLSD()); return; }
        LLSD body;
        body["npc"] = mId;
        body["id"] = mFound[i]["id"];
        body["hide"] = true;
        const U32 serial = mSerial;
        WolfAdventure::api("npc_skill_hide", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
        {
            if (life.expired() || !alive(serial)) return;
            if (!ok) { mError("It was not hidden: " + error, reply); return; }
            mDone("Hidden from all your NPCs.", LLSD());
            loadSkills();
        });
    });

    // ---- [ROUND B] growing ----
    mGLookups = mLearning->getChild<LLCheckBoxCtrl>("g_lookups");
    mGLookups->setCommitCallback([this](LLUICtrl*, const LLSD&) { setLearnSwitch("lookups", mGLookups->get()); });
    mGTeacher = mLearning->getChild<LLCheckBoxCtrl>("g_teacher");
    mGTeacher->setCommitCallback([this](LLUICtrl*, const LLSD&) { setLearnSwitch("teacher", mGTeacher->get()); });
    mGLookupsCost = mLearning->getChild<LLTextBox>("g_lookups_cost");
    mGTeacherCost = mLearning->getChild<LLTextBox>("g_teacher_cost");
    mGTopic = mLearning->getChild<LLLineEditor>("g_topic");
    mGQuote = mLearning->getChild<LLTextBox>("g_quote_text");
    mGJobs = mLearning->getChild<LLScrollListCtrl>("g_jobs");
    mGStyles = mLearning->getChild<LLScrollListCtrl>("g_styles");
    mLearning->getChild<LLButton>("g_quote")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResearchQuote(); });
    mGTopic->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResearchQuote(); });
    mLearning->getChild<LLButton>("g_jobs_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadResearch(); });
    mLearning->getChild<LLButton>("g_style_on")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setStyle(true); });
    mLearning->getChild<LLButton>("g_style_off")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setStyle(false); });

    // ---- changes ----
    mCList = mLearning->getChild<LLScrollListCtrl>("c_list");
    mCUndoNote = mLearning->getChild<LLTextBox>("c_undo_note");
    mCSkills = mLearning->getChild<LLScrollListCtrl>("c_skills");
    mLearning->getChild<LLButton>("c_undo")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });

    // ---- places, tools ----
    mPlList = mPlacesPanel->getChild<LLScrollListCtrl>("pl_list");
    mPlName = mPlacesPanel->getChild<LLLineEditor>("pl_name");
    mPlError = mPlacesPanel->getChild<LLTextBox>("pl_error");
    mPlacesPanel->getChild<LLButton>("pl_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSavePlace(); });
    mPlacesPanel->getChild<LLButton>("pl_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLScrollListItem* it = mPlList->getFirstSelected();
        if (!it) { mPlError->setText(std::string("Select a place first.")); return; }
        const std::string name = it->getValue().asString();
        if (!mCfg["places"].has(name)) { mPlError->setText(std::string("That one is a thing it uses: unlink it below.")); return; }
        LLSD places = mCfg["places"];
        places.erase(name);
        LLSD changes;
        changes["places"] = places;
        mSave(changes, "Removed \"" + name + "\".", [this](const LLSD& cfg) { mCfg = cfg; fillPlaces(); });
    });
    mTlList = mPlacesPanel->getChild<LLScrollListCtrl>("tl_list");
    mTlType = mPlacesPanel->getChild<LLComboBox>("tl_type");
    fillCombo(mTlType, TOOL_TYPES);
    mTlType->setCommitCallback([this](LLUICtrl*, const LLSD&) { mTlFace->setEnabled(mTlType->getValue().asString() == "whiteboard"); });
    mTlRole = mPlacesPanel->getChild<LLLineEditor>("tl_role");
    mTlFace = mPlacesPanel->getChild<LLSpinCtrl>("tl_face");
    mTlFace->setEnabled(true);
    mPlacesPanel->getChild<LLButton>("tl_link")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLink(); });
    mPlacesPanel->getChild<LLButton>("tl_unlink")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const S32 i = mTlList->getFirstSelectedIndex();
        const LLSD& tools = mCfg["tools"];
        if (i < 0 || i >= (S32)tools.size()) { mPlError->setText(std::string("Select a thing it uses first.")); return; }
        LLSD kept = LLSD::emptyArray();
        for (S32 k = 0; k < (S32)tools.size(); ++k)
        {
            if (k != i) kept.append(tools[k]);
        }
        LLSD changes;
        changes["tools"] = kept;
        mSave(changes, "Unlinked.", [this](const LLSD& cfg) { mCfg = cfg; fillPlaces(); fillTools(); });
    });
}

bool WolfNPCMind::alive(U32 serial) const
{
    return mOwner.get() != nullptr && serial == mSerial;
}

std::string WolfNPCMind::pounds(F64 credits) const
{
    // pounds = credits * pencePerCredit / 100 (npc_ai_info; never a fixed rate). Not known: credits only.
    const F64 ppc = mPrices["pencePerCredit"].asReal();
    if (!(ppc > 0.0) || !std::isfinite(credits) || credits < 0.0) return std::string();
    const F64 gbp = credits * ppc / 100.0;
    if (gbp > 0.0 && gbp < 0.01) return " (under \xC2\xA3" "0.01)";
    return llformat(" (about \xC2\xA3%.2f)", gbp);
}

void WolfNPCMind::charged(const LLSD& reply)
{
    // a call that cost credits: the status bar's balance is asked for again (wolfai.h creditsUsed)
    if (reply["cost"]["credits"].asReal() > 0.0) WolfAI::instance().creditsUsed();
}

void WolfNPCMind::setPrices(const LLSD& ai_info)
{
    mPrices = ai_info;
    if (!mPrices.isMap()) return;
    // Source: §5 npc_ai_info perLearn (one learning), perLookup (embedding one line)
    mKNote->setText("Teaching it costs about " + number(mPrices["perLearn"]) + " credits" + pounds(mPrices["perLearn"].asReal())
                    + ". What it learns from talk costs the same each time, paid from your Wolf Credits.");
    mFNote->setText("Finding costs about " + number(mPrices["perLookup"]) + " credits" + pounds(mPrices["perLookup"].asReal()) + ".");
    // [ROUND B] perLookupWeb, perLesson {credits, maxCredits, replies, assumption}, perResearch
    F64 about = 0.0, most = 0.0;
    priceOf(mPrices["perLookupWeb"], about, most);
    mGLookupsCost->setText("Each look-up costs about " + number(LLSD(about)) + " credits" + pounds(about) + ", at most " + number(LLSD(most))
                           + pounds(most) + ". Off unless you switch it on; at most 20 a day.");
    priceOf(mPrices["perLesson"], about, most);
    const S32 replies = mPrices["perLesson"]["replies"].asInteger();
    mGTeacherCost->setText(llformat("A lesson of about %d replies costs about ", replies > 0 ? replies : 12) + number(LLSD(about)) + " credits" + pounds(about)
                           + ", at most " + number(LLSD(most)) + pounds(most) + " (paid as its AI talk).");
    priceOf(mPrices["perResearch"], about, most);
    mGQuote->setText("Reading up on a topic costs about " + number(LLSD(about)) + " credits" + pounds(about) + ", at most " + number(LLSD(most))
                     + pounds(most) + ". Each topic is priced before it starts.");
}

bool WolfNPCMind::editing() const
{
    return mSkEditor->getVisible();
}

void WolfNPCMind::setNpc(const std::string& id, const LLSD& cfg, const LLSD& state)
{
    const bool fresh = id != mId;
    mCfg = cfg.isMap() ? cfg : LLSD::emptyMap();
    mState = state;
    if (!mFirstCfg.count(id)) mFirstCfg[id] = mCfg;   // what Undo puts back: as it was when first opened here
    fillChanges();
    fillPlaces();
    fillTools();
    if (fresh)
    {
        mId = id;
        ++mSerial;
        closeSkill();
        mItems = LLSD::emptyArray();
        mSkills = LLSD::emptyArray();
        mWished = LLSD::emptyArray();
        mFound = LLSD::emptyArray();
        mFList->deleteAllItems();
        mKSearch->setText(LLStringUtil::null);
        loadKnowledge(0);
        loadSkills();
        loadWished();
        loadResearch();
        loadStyles();
    }
}

// ─── [ROUND B] growing (PROTOCOL.md §5 round B) ─────────────────────────────────────────────────────

void WolfNPCMind::setLearnSwitch(const char* key, bool on)
{
    // Source: §5 npc_learn_set {npc, lookups? | teacher?} (round B)
    LLSD body;
    body["npc"] = mId;
    body[key] = on;
    const std::string k(key);
    const U32 serial = mSerial;
    WolfAdventure::api("npc_learn_set", body, [this, life = std::weak_ptr<int>(mLife), serial, k, on](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok)
        {
            (k == "lookups" ? mGLookups : mGTeacher)->set(!on);   // as it was: the switch shows what is stored
            mError("Not changed: " + error, reply);
            return;
        }
        if (k == "lookups") mDone(on ? "It may look things up now: each look-up is paid from your Wolf Credits." : "It no longer looks things up.", LLSD());
        else mDone(on ? "It teaches now. Link a whiteboard on the Places tab for its lessons." : "It no longer teaches.", LLSD());
    });
}

void WolfNPCMind::loadResearch()
{
    if (mId.empty()) return;
    LLSD body;
    body["npc"] = mId;
    const U32 serial = mSerial;
    WolfAdventure::api("npc_research_status", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        mGJobs->deleteAllItems();
        if (!ok) { mGJobs->setCommentText("Its reading could not be read: " + cap(error, 100)); return; }
        S32 i = 0;
        for (const LLSD& j : llsd::inArray(reply["jobs"]))
        {
            const std::string st = j["status"].asString();
            std::string words = st == "done" ? std::string("done") : st == "running" ? llformat("reading (%d%%)", j["progress"].asInteger())
                              : st == "queued" ? std::string("waiting to start") : st == "confirm" ? std::string("waiting for yes")
                              : st == "refused" ? std::string("refused") : st == "failed" ? std::string("failed") : cap(st, 20);
            if (!j["error"].asString().empty()) words += ": " + cap(j["error"].asString(), 60);
            mGJobs->addElement(row(LLSD(i++), { { "topic", cap(j["topic"].asString(), 120) }, { "status", words },
                                                { "items", llformat("%d", j["items"].asInteger()) }, { "charged", number(j["charged"]) } }));
        }
        if (i == 0) mGJobs->setCommentText("It has not read up on anything yet.");
    });
}

void WolfNPCMind::onResearchQuote()
{
    const std::string topic = trimmed(mGTopic->getText());
    // Source: §5 npc_research_quote {npc, topic (3-120)} -> {quote {credits, maxCredits, searches, items}, topic}
    if (utf8Chars(topic) < 3 || utf8Chars(topic) > 120) { mError("Type a topic of 3 to 120 characters.", LLSD()); return; }
    LLSD body;
    body["npc"] = mId;
    body["topic"] = topic;
    mGQuote->setText(std::string("Working out the price..."));
    const U32 serial = mSerial;
    WolfAdventure::api("npc_research_quote", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok) { mGQuote->setText(LLStringUtil::null); mError("It cannot read up on that: " + error, reply); return; }
        const LLSD& q = reply["quote"];
        const std::string topic = cap(reply["topic"].asString(), 120);
        const std::string price = "about " + number(q["credits"]) + " credits" + pounds(q["credits"].asReal()) + ", never more than "
                                  + number(q["maxCredits"]) + pounds(q["maxCredits"].asReal());
        mGQuote->setText("Reading up on "" + topic + "" costs " + price + llformat(". It keeps at most %d things.", q["items"].asInteger()));
        mAsk("Read up on "" + topic + ""? It costs you " + price + ". What it learns (in its own words, with its web sources) waits for you on the Knowledge tab.",
             "Read up on it", [this, life, serial, topic]()
        {
            if (life.expired() || !alive(serial)) return;
            // Source: §5 npc_research_start {npc, topic} -> {job}; credits_short (+ buyurl) when the live credits cannot cover maxCredits
            LLSD start;
            start["npc"] = mId;
            start["topic"] = topic;
            WolfAdventure::api("npc_research_start", start, [this, life, serial](bool ok2, const LLSD& reply2, const std::string& error2)
            {
                if (life.expired() || !alive(serial)) return;
                if (!ok2) { mError("It did not start reading: " + error2, reply2); return; }
                mGTopic->setText(LLStringUtil::null);
                mDone("It is reading up on it now. Press Refresh to see how it is going.", LLSD());
                loadResearch();
            });
        });
    });
}

void WolfNPCMind::loadStyles()
{
    if (mId.empty()) return;
    LLSD body;
    body["npc"] = mId;
    const U32 serial = mSerial;
    WolfAdventure::api("npc_style_list", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        mGStyles->deleteAllItems();
        if (!ok) { mGStyles->setCommentText("The voices could not be read: " + cap(error, 100)); return; }
        mStyles = reply["styles"].isArray() ? reply["styles"] : LLSD::emptyArray();
        S32 i = 0;
        for (const LLSD& st : llsd::inArray(mStyles))
        {
            mGStyles->addElement(row(LLSD(i++), { { "on", st["attached"].asBoolean() ? std::string("in use") : std::string() },
                                                  { "name", cap(st["name"].asString(), 48) }, { "text", cap(st["text"].asString(), 200) } }));
        }
        if (i == 0) mGStyles->setCommentText("No voices in the library yet.");
    });
}

void WolfNPCMind::setStyle(bool on)
{
    const S32 i = mGStyles->getFirstSelectedIndex();
    if (i < 0 || i >= (S32)mStyles.size()) { mError("Select a voice first.", LLSD()); return; }
    // Source: §5 npc_style_set {npc, id, on} -> {attached}; at most 2 styles an NPC
    LLSD body;
    body["npc"] = mId;
    body["id"] = mStyles[i]["id"];
    body["on"] = on;
    const std::string name = cap(mStyles[i]["name"].asString(), 48);
    const U32 serial = mSerial;
    WolfAdventure::api("npc_style_set", body, [this, life = std::weak_ptr<int>(mLife), serial, on, name](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok) { mError("The voice was not changed: " + error, reply); return; }
        mDone(on ? "It speaks with the " + name + " voice now." : "It no longer uses the " + name + " voice.", LLSD());
        loadStyles();
    });
}

// ─── knowledge (§5 npc_learn_*) ─────────────────────────────────────────────────────────────────────

void WolfNPCMind::loadKnowledge(S32 offset)
{
    if (mId.empty()) return;
    LLSD body;
    body["npc"] = mId;
    if (offset > 0) body["offset"] = offset;
    if (offset == 0) mKItems->setCommentText("Reading what it knows...");
    const U32 serial = mSerial;
    WolfAdventure::api("npc_learn_get", body, [this, life = std::weak_ptr<int>(mLife), serial, offset](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok)
        {
            mKItems->deleteAllItems();
            mKItems->setCommentText("What it knows could not be read.");
            mError("What it knows could not be read: " + error, reply);
            return;
        }
        mKMode->setValue(LLSD(reply["mode"].asString().empty() ? std::string("ask") : reply["mode"].asString()));
        // [ROUND B] lookups (default false: they cost credits) and teacher
        mGLookups->set(reply["lookups"].asBoolean());
        mGTeacher->set(reply["teacher"].asBoolean());
        mKRole->setText(cap(reply["role"].asString(), 500));
        if (offset == 0) mItems = LLSD::emptyArray();
        for (const LLSD& it : llsd::inArray(reply["items"])) mItems.append(it);
        mItemsCount = reply["count"].asInteger();
        mItemsMax = reply["max"].asInteger();
        fillKnowledge();
    });
}

void WolfNPCMind::fillKnowledge()
{
    const std::string sel = selectedItem()["id"].asString();
    const std::string find = lower(trimmed(mKSearch->getText()));
    mKItems->deleteAllItems();
    S32 shown = 0, pending = 0;
    for (S32 i = 0; i < (S32)mItems.size(); ++i)
    {
        const LLSD& it = mItems[i];
        const std::string text = cap(it["text"].asString(), 300);
        const bool waiting = it["status"].asString() == "pending";
        pending += waiting ? 1 : 0;
        if (!find.empty() && lower(text).find(find) == std::string::npos) continue;
        ++shown;
        const std::string from = it["source"].asString() == "owner" ? std::string("you") : cap(it["sourceName"].asString(), 48);
        mKItems->addElement(row(LLSD(i), { { "status", waiting ? std::string("Waiting") : std::string("Kept") }, { "kind", cap(it["kind"].asString(), 12) },
                                           { "text", text }, { "from", from }, { "uses", llformat("%d", it["uses"].asInteger()) } }));
    }
    if (shown == 0) mKItems->setCommentText(find.empty() ? "It knows nothing yet. Teach it below, or let it learn from talk." : "Nothing it knows matches that.");
    for (S32 i = 0; i < mKItems->getItemCount(); ++i)
    {
        if (mItems[mKItems->getAllData()[(size_t)i]->getValue().asInteger()]["id"].asString() == sel) mKItems->selectNthItem(i);
    }
    mKCount->setText(llformat("%d things (the most is %d), %d waiting for you", mItemsCount, mItemsMax, pending));
    mKMore->setEnabled((S32)mItems.size() < mItemsCount);
}

LLSD WolfNPCMind::selectedItem() const
{
    LLScrollListItem* it = mKItems->getFirstSelected();
    if (!it) return LLSD();
    const S32 i = it->getValue().asInteger();
    return i >= 0 && i < (S32)mItems.size() ? mItems[i] : LLSD();
}

void WolfNPCMind::itemOp(const std::string& op)
{
    const LLSD item = selectedItem();
    if (!item.isMap()) { mError("Select something it knows first.", LLSD()); return; }
    if ((op == "approve" || op == "reject") && item["status"].asString() != "pending") { mError("That one is kept already; Delete it instead.", LLSD()); return; }
    // Source: §5 npc_learn_item {npc, id, op: approve | reject | edit | delete, text}
    LLSD body;
    body["npc"] = mId;
    body["id"] = item["id"];
    body["op"] = op;
    if (op == "edit")
    {
        const std::string text = trimmed(mKEdit->getText());
        if (text.empty() || utf8Chars(text) > 300) { mError("Type its new words (1 to 300 characters).", LLSD()); return; }
        body["text"] = text;
    }
    const U32 serial = mSerial;
    const std::string id = item["id"].asString();
    WolfAdventure::api("npc_learn_item", body, [this, life = std::weak_ptr<int>(mLife), serial, op, id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok) { mError("Not changed: " + error, reply); return; }
        charged(reply);
        LLSD kept = LLSD::emptyArray();
        for (const LLSD& it : llsd::inArray(mItems))
        {
            if (it["id"].asString() != id) kept.append(it);
            else if (reply["item"].isMap()) kept.append(reply["item"]);
        }
        mItems = kept;
        if (!reply["item"].isMap()) mItemsCount = llmax(0, mItemsCount - 1);
        fillKnowledge();
        mDone(op == "approve" ? "Kept." : op == "reject" ? "Rejected: it forgets that." : op == "delete" ? "Deleted." : "Changed.", LLSD());
    });
}

// ─── skills (§5 npc_skill_*, §13.3 steps) ───────────────────────────────────────────────────────────

void WolfNPCMind::loadSkills()
{
    if (mId.empty()) return;
    LLSD body;
    body["npc"] = mId;
    body["scope"] = "all";
    const U32 serial = mSerial;
    WolfAdventure::api("npc_skill_list", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok)
        {
            mSkList->deleteAllItems();
            mSkList->setCommentText("Its skills could not be read.");
            mError("Its skills could not be read: " + error, reply);
            return;
        }
        mSkills = reply["skills"].isArray() ? reply["skills"] : LLSD::emptyArray();
        mOffers = reply["offers"].isArray() ? reply["offers"] : LLSD::emptyArray();
        const LLSD& c = reply["counts"];
        mSkCount->setText(llformat("Its own %d, all your NPCs' %d, waiting for you %d (of %d)", c["private"].asInteger(), c["owner"].asInteger(),
                                   c["pending"].asInteger(), reply["pendingMax"].asInteger()));
        fillSkills();
    });
}

void WolfNPCMind::fillSkills()
{
    mSkList->deleteAllItems();
    S32 i = 0;
    for (const LLSD& s : llsd::inArray(mSkills))
    {
        const std::string scope = s["scope"].asString();
        std::string state = s["status"].asString() == "pending" ? std::string("waiting for you") : std::string("in use");
        if (s["hidden"].asBoolean()) state = "hidden";
        for (const LLSD& o : llsd::inArray(mOffers))
        {
            if (o["id"].asString() != s["id"].asString()) continue;
            state = o["status"].asString() == "rejected" ? "not taken: " + cap(o["note"].asString(), 60) : std::string("offered, with staff");
        }
        mSkList->addElement(row(LLSD(i++), { { "name", cap(s["name"].asString(), 48) },
                                             { "for", scope == "shared" ? std::string("shared library") : scope == "owner" ? std::string("all my NPCs") : std::string("this NPC") },
                                             { "status", state }, { "uses", llformat("%d", s["uses"].asInteger()) }, { "asks", llformat("%d", s["asks"].asInteger()) } }));
    }
    if (mSkills.size() == 0) mSkList->setCommentText("No skills yet. New skill: steps it follows when someone asks, e.g. serve a drink.");
}

LLSD WolfNPCMind::selectedSkill() const
{
    const S32 i = mSkList->getFirstSelectedIndex();
    return i >= 0 && i < (S32)mSkills.size() ? mSkills[i] : LLSD();
}

void WolfNPCMind::editSkill(const LLSD& skill, const std::string& when)
{
    mSkEditing = skill["id"].asString();
    mSkName->setText(cap(skill["name"].asString(), 48));
    if (!mSkScope->selectByValue(LLSD(skill["scope"].asString()))) mSkScope->selectFirstItem();
    mSkWhen->setText(when.empty() ? cap(skill["when"].asString(), 300) : when);
    mStepList.clear();
    for (const LLSD& st : llsd::inArray(skill["steps"])) mStepList.push_back(st);
    fillSteps();
    mSkStepDo->selectFirstItem();
    onStepDo();
    mSkError->setText(LLStringUtil::null);
    mSkEditor->setVisible(true);
    mSkName->setFocus(true);
}

void WolfNPCMind::closeSkill()
{
    mSkEditor->setVisible(false);
    mSkEditing.clear();
    mStepList.clear();
}

void WolfNPCMind::onStepDo()
{
    const std::string d = mSkStepDo->getValue().asString();
    // the fields each step type has (§13.3 / §22.4): a, then b, then then/else (ifflag)
    const bool a = !(d == "wave");
    mSkStepA->setVisible(a);
    mSkStepB->setVisible(d == "quest");
    mSkStepThen->setVisible(d == "ifflag");
    mSkStepElse->setVisible(d == "ifflag");
    mSkStepA->setLabel(d == "say" ? std::string("what it says") : d == "emote" ? std::string("e.g. smiles warmly")
                       : d == "anim" ? std::string("animation name") : d == "sound" ? std::string("sound name")
                       : d == "face" || d == "follow" ? std::string("speaker, or a name") : d == "goto" || d == "sit" ? std::string("a place name, or x,y,z")
                       : d == "wait" ? std::string("seconds, 1 to 30") : d == "handout" ? std::string("item name in its contents")
                       : d == "quest" ? std::string("quest id") : d == "routine" ? std::string("08:00 market; 18:00 home")
                       : d == "ifflag" ? std::string("flag, e.g. met_martha") : std::string());
    mSkStepB->setLabel(std::string("step"));
    const std::string help =
        d == "face" || d == "follow" ? "\"speaker\" means the person who asked." :
        d == "goto" || d == "sit" ? "Use a name from its Places tab (or a thing it uses), so the skill works wherever it is." :
        d == "handout" ? "Only an item whose description contains wolfnpc-handout can go to visitors." :
        d == "quest" ? "Step 0 offers the quest; another number finishes that step when it is the current one." :
        d == "ifflag" ? "For game players. Then and otherwise take simple steps: say:, emote:, anim:, sound:, face:, follow:, goto:, wait:, wave - separated by ;" :
        d == "routine" ? "Times are grid time; each line a time and a place (or work, home, sit, roam, rest)." :
        std::string("Visitors can only set off skills made of Say, emote, animations, face, wave, wait and walking inside its area.");
    mSkStepHelp->setText(help);
}

// static
std::string WolfNPCMind::stepWords(const LLSD& st)
{
    const std::string d = st["do"].asString();
    const std::string label = labelOf(STEP_TYPES, d);
    if (d == "say" || d == "emote") return label + ": " + cap(st["text"].asString(), 120);
    if (d == "anim" || d == "sound") return label + ": " + cap(st["name"].asString(), 64);
    if (d == "face" || d == "follow")
    {
        const std::string who = st["who"].asString();
        return label + " " + (who == "speaker" || who == "{who}" ? std::string("the person who asked") : cap(who, 48));
    }
    if (d == "goto") return label + ": " + (st.has("pos") ? placeText(st["pos"]) : cap(st["place"].asString(), 48));
    if (d == "sit") return label + ": " + cap(st["place"].asString(), 48);
    if (d == "wait") return label + llformat(" %d s", st["seconds"].asInteger());
    if (d == "handout") return label + ": " + cap(st["item"].asString(), 64);
    if (d == "quest") return label + ": " + cap(st["id"].asString(), 16) + llformat(" step %d", st["step"].asInteger());
    if (d == "routine") return label + llformat(" (%d times)", (S32)st["list"].size());
    if (d == "ifflag") return label + " " + cap(st["flag"].asString(), 24) + llformat(": %d steps, otherwise %d", (S32)st["then"].size(), (S32)st["else"].size());
    return label;
}

void WolfNPCMind::fillSteps()
{
    mSkSteps->deleteAllItems();
    S32 i = 0;
    for (const LLSD& st : mStepList)
    {
        mSkSteps->addElement(row(LLSD(i), { { "n", llformat("%d", i + 1) }, { "step", stepWords(st) } }));
        ++i;
    }
    if (mStepList.empty()) mSkSteps->setCommentText("No steps yet: choose one below and press Add step.");
}

// static
bool WolfNPCMind::parseSimpleSteps(const std::string& text, LLSD& out, std::string& why)
{
    // "say:Welcome back; wave; anim:bow" -> flat steps (§22.4 ifflag then/else: up to 6 steps, one level)
    out = LLSD::emptyArray();
    std::vector<std::string> parts;
    LLStringUtil::getTokens(text, parts, ";");
    for (std::string p : parts)
    {
        p = trimmed(p);
        if (p.empty()) continue;
        const size_t c = p.find(':');
        const std::string d = lower(trimmed(p.substr(0, c))), v = c == std::string::npos ? std::string() : trimmed(p.substr(c + 1));
        LLSD st = LLSD::emptyMap();
        st["do"] = d;
        if (d == "say" || d == "emote") st["text"] = v;
        else if (d == "anim" || d == "sound") st["name"] = v;
        else if (d == "face" || d == "follow") st["who"] = v.empty() ? std::string("speaker") : v;
        else if (d == "goto") { const LLSD pv = placeValue(v); if (pv.isArray()) st["pos"] = pv; else st["place"] = v; }
        else if (d == "wait")
        {
            S32 sec = 0;
            if (!LLStringUtil::convertToS32(v, sec) || sec < 1 || sec > 30) { why = "wait: a number of seconds from 1 to 30."; return false; }
            st["seconds"] = sec;
        }
        else if (d != "wave") { why = "\"" + cap(p, 30) + "\": then and otherwise take say:, emote:, anim:, sound:, face:, follow:, goto:, wait: and wave."; return false; }
        if ((d == "say" || d == "emote" || d == "anim" || d == "sound" || d == "goto") && v.empty()) { why = "\"" + cap(p, 30) + "\" needs something after the colon."; return false; }
        out.append(st);
    }
    if (out.size() > 6) { why = "Then and otherwise each have at most 6 steps."; return false; }
    return true;
}

bool WolfNPCMind::buildStep(LLSD& st, std::string& why) const
{
    // Source: §22.4 / §13.3 the flat step shape {"do": ..., fields}
    const std::string d = mSkStepDo->getValue().asString();
    const std::string a = trimmed(mSkStepA->getText());
    st = LLSD::emptyMap();
    st["do"] = d;
    if ((d != "wave" && d != "face" && d != "follow") && a.empty()) { why = "Fill in the box beside the step first."; return false; }
    if (d == "say" || d == "emote")
    {
        if (utf8Chars(a) > 300) { why = "At most 300 characters."; return false; }
        st["text"] = a;
    }
    else if (d == "anim" || d == "sound") st["name"] = a;
    else if (d == "face" || d == "follow") st["who"] = a.empty() || lower(a) == "speaker" ? std::string("speaker") : a;
    else if (d == "goto") { const LLSD pv = placeValue(a); if (pv.isArray()) st["pos"] = pv; else st["place"] = a; }
    else if (d == "sit") st["place"] = a;
    else if (d == "handout") st["item"] = a;
    else if (d == "wait")
    {
        S32 sec = 0;
        if (!LLStringUtil::convertToS32(a, sec) || sec < 1 || sec > 30) { why = "Wait is a number of seconds from 1 to 30."; return false; }
        st["seconds"] = sec;
    }
    else if (d == "quest")
    {
        S32 n = 0;
        if (!LLStringUtil::convertToS32(trimmed(mSkStepB->getText()), n) || n < 0 || n > 8) { why = "The quest step is a number: 0 offers it, 1 to 8 finishes that step."; return false; }
        st["id"] = lower(a);
        st["step"] = n;
    }
    else if (d == "routine")
    {
        // "08:00 market; 18:00 home" -> the §3 routine list
        LLSD list = LLSD::emptyArray();
        std::vector<std::string> parts;
        LLStringUtil::getTokens(a, parts, ";");
        for (std::string p : parts)
        {
            p = trimmed(p);
            if (p.empty()) continue;
            const size_t sp = p.find(' ');
            const std::string at = p.substr(0, sp), what = sp == std::string::npos ? std::string() : trimmed(p.substr(sp + 1));
            const bool ok = at.size() == 5 && at[2] == ':' && isdigit((unsigned char)at[0]) && isdigit((unsigned char)at[1]) && isdigit((unsigned char)at[3])
                            && isdigit((unsigned char)at[4]) && std::stoi(at.substr(0, 2)) < 24 && std::stoi(at.substr(3, 2)) < 60;
            if (!ok || what.empty()) { why = "Write each time like 08:00 then where, e.g. 08:00 market; 18:00 home."; return false; }
            LLSD r;
            r["at"] = at;
            const std::string w = lower(what);
            if (w == "work" || w == "home" || w == "sit" || w == "roam" || w == "rest") r["do"] = w;
            else { r["do"] = "goto"; r["place"] = what; }
            list.append(r);
        }
        if (list.size() == 0 || list.size() > 12) { why = "A routine has 1 to 12 times."; return false; }
        st["list"] = list;
    }
    else if (d == "ifflag")
    {
        if (a.size() > 24 || a.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        {
            why = "A flag is one word of a-z, 0-9, _ and - (up to 24).";
            return false;
        }
        LLSD then_steps, else_steps;
        if (!parseSimpleSteps(mSkStepThen->getText(), then_steps, why) || !parseSimpleSteps(mSkStepElse->getText(), else_steps, why)) return false;
        if (then_steps.size() == 0 && else_steps.size() == 0) { why = "Say what it does then, or otherwise."; return false; }
        st["flag"] = a;
        st["then"] = then_steps;
        st["else"] = else_steps;
    }
    return true;
}

void WolfNPCMind::onSkillSave()
{
    mSkError->setText(LLStringUtil::null);
    const std::string name = trimmed(mSkName->getText()), when_text = trimmed(mSkWhen->getText());
    if (name.empty() || utf8Chars(name) > 48) { mSkError->setText(std::string("Give the skill a name (1 to 48 characters).")); return; }
    if (when_text.empty() || utf8Chars(when_text) > 300) { mSkError->setText(std::string("Say when it should do this (1 to 300 characters).")); return; }
    if (mStepList.empty()) { mSkError->setText(std::string("Add at least one step.")); return; }
    // Source: §5 npc_skill_save {npc, skill {id?, name, when, steps, scope}} -> {skill, cost}
    LLSD body;
    body["npc"] = mId;
    if (!mSkEditing.empty()) body["skill"]["id"] = mSkEditing;
    body["skill"]["name"] = name;
    body["skill"]["when"] = when_text;
    LLSD steps = LLSD::emptyArray();
    for (const LLSD& st : mStepList) steps.append(st);
    body["skill"]["steps"] = steps;
    body["skill"]["scope"] = mSkScope->getValue().asString();
    const U32 serial = mSerial;
    WolfAdventure::api("npc_skill_save", body, [this, life = std::weak_ptr<int>(mLife), serial, name](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok) { mSkError->setText(cap(error, 300)); mError("The skill was not saved: " + error, reply); return; }
        charged(reply);
        closeSkill();
        mDone("Skill saved: " + name + ".", LLSD());
        loadSkills();
    });
}

// ─── asked for, find (§5 npc_skill_wished, npc_skill_find) ──────────────────────────────────────────

void WolfNPCMind::loadWished()
{
    if (mId.empty()) return;
    LLSD body;
    body["npc"] = mId;
    const U32 serial = mSerial;
    WolfAdventure::api("npc_skill_wished", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        if (!ok) { mWList->deleteAllItems(); mWList->setCommentText("What people asked for could not be read."); return; }
        mWished = reply["wished"].isArray() ? reply["wished"] : LLSD::emptyArray();
        fillWished();
    });
}

void WolfNPCMind::fillWished()
{
    mWList->deleteAllItems();
    S32 i = 0;
    for (const LLSD& w : llsd::inArray(mWished))
    {
        mWList->addElement(row(LLSD(i++), { { "text", cap(w["text"].asString(), 200) }, { "asks", llformat("%d", w["asks"].asInteger()) }, { "last", when(w["last"]) } }));
    }
    if (mWished.size() == 0) mWList->setCommentText("Nobody has asked for anything it cannot do.");
}

void WolfNPCMind::onFind()
{
    const std::string text = trimmed(mFText->getText());
    if (text.empty()) { mError("Type what it should do, e.g. pour a pint.", LLSD()); return; }
    LLSD body;
    body["text"] = text;
    mFList->setCommentText("Looking in the shared library...");
    const U32 serial = mSerial;
    WolfAdventure::api("npc_skill_find", body, [this, life = std::weak_ptr<int>(mLife), serial](bool ok, const LLSD& reply, const std::string& error)
    {
        if (life.expired() || !alive(serial)) return;
        mFList->deleteAllItems();
        if (!ok) { mFList->setCommentText("The library could not be searched."); mError("The library could not be searched: " + error, reply); return; }
        charged(reply);
        mFound = reply["skills"].isArray() ? reply["skills"] : LLSD::emptyArray();
        S32 i = 0;
        for (const LLSD& s : llsd::inArray(mFound))
        {
            mFList->addElement(row(LLSD(i++), { { "name", cap(s["name"].asString(), 48) }, { "when", cap(s["when"].asString(), 200) },
                                                { "match", llformat("%.0f%%", llclamp(s["match"].asReal(), 0.0, 1.0) * 100.0) } }));
        }
        if (mFound.size() == 0) mFList->setCommentText("No shared skill like that yet. Make your own on the Skills tab.");
    });
}

// ─── changes by chat (§22.5) ────────────────────────────────────────────────────────────────────────

void WolfNPCMind::fillChanges()
{
    mCList->deleteAllItems();
    const LLSD& log = mState["byChat"];   // {who, name, what, at}, newest last
    for (S32 i = (S32)log.size() - 1; i >= 0; --i)
    {
        const LLSD& e = log[i];
        mCList->addElement(row(LLSD(i), { { "at", when(e["at"]) }, { "who", cap(e["name"].asString(), 48) }, { "what", cap(e["what"].asString(), 120) } }));
    }
    if (log.size() == 0) mCList->setCommentText("Nobody has changed it by talking to it.");
    mCSkills->deleteAllItems();
    const LLSD& runs = mState["skills"];   // {skill, step, what, ok, at}, newest last
    for (S32 i = (S32)runs.size() - 1; i >= 0; --i)
    {
        const LLSD& e = runs[i];
        mCSkills->addElement(row(LLSD(i), { { "at", when(e["at"]) }, { "skill", cap(e["skill"].asString(), 48) },
                                            { "what", llformat("%d. ", e["step"].asInteger() + 1) + cap(e["what"].asString(), 100) },
                                            { "ok", e["ok"].asBoolean() ? std::string("done") : std::string("refused") } }));
    }
    if (runs.size() == 0) mCSkills->setCommentText("It has not used a skill yet.");
    mCUndoNote->setText(std::string("Select a \"set\" change to put that setting back."));
}

void WolfNPCMind::onUndo()
{
    LLScrollListItem* it = mCList->getFirstSelected();
    const LLSD& log = mState["byChat"];
    const S32 i = it ? it->getValue().asInteger() : -1;
    if (i < 0 || i >= (S32)log.size()) { mCUndoNote->setText(std::string("Select a change first.")); return; }
    // Source: §22.5 what, e.g. "set shy 0.8"; §13.2 the keys a "set" may change
    const std::string what = log[i]["what"].asString();
    if (what.rfind("set ", 0) != 0) { mCUndoNote->setText(std::string("Only a changed setting can be undone here (not a walk, a memory or a skill).")); return; }
    std::string key = what.substr(4);
    const size_t sp = key.find(' ');
    if (sp != std::string::npos) key = key.substr(0, sp);
    const LLSD& before = mFirstCfg[mId];
    LLSD changes;
    std::string words;
    if (key == "shy" || key == "visitors" || key == "indoors" || key == "routine" || key == "behaviour")
    {
        if (!before.has(key)) { mCUndoNote->setText(std::string("Its earlier value is not known here (it was changed before you opened it).")); return; }
        if (llsd_equals(before[key], mCfg[key])) { mCUndoNote->setText(std::string("It is already as it was.")); return; }
        changes[key] = before[key];
    }
    else if (key == "area.radius")
    {
        if (!before["area"].has("radius")) { mCUndoNote->setText(std::string("Its earlier value is not known here.")); return; }
        LLSD area = mCfg["area"];
        area["radius"] = before["area"]["radius"];
        changes["area"] = area;
    }
    else if (key == "ai.voice" || key == "ai.on")
    {
        const std::string sub = key.substr(3);
        if (sub == "on" && before["ai"]["on"].asBoolean() && !mCfg["ai"]["on"].asBoolean())
        {
            mCUndoNote->setText(std::string("Turn AI talk back on in Settings, where its price is shown first."));
            return;
        }
        LLSD ai = mCfg["ai"];
        ai[sub] = before["ai"][sub];
        changes["ai"] = ai;
    }
    else if (key.rfind("places.", 0) == 0)
    {
        const std::string name = key.substr(7);
        LLSD places = mCfg["places"];
        if (before["places"].has(name)) places[name] = before["places"][name];
        else places.erase(name);
        changes["places"] = places;
    }
    else { mCUndoNote->setText("\"" + cap(key, 30) + "\" cannot be undone here."); return; }
    mSave(changes, "Put back: " + key + ".", [this](const LLSD& cfg) { mCfg = cfg; fillPlaces(); });
}

// ─── places (§3) and tools (§23.1) ──────────────────────────────────────────────────────────────────

void WolfNPCMind::fillPlaces()
{
    mPlList->deleteAllItems();
    const LLSD& places = mCfg["places"];
    if (places.isMap())
    {
        for (auto it = places.beginMap(); it != places.endMap(); ++it)
        {
            mPlList->addElement(row(LLSD(it->first), { { "name", cap(it->first, 24) }, { "where", placeText(it->second) }, { "from", std::string() } }));
        }
    }
    for (const LLSD& t : llsd::inArray(mCfg["tools"]))
    {
        mPlList->addElement(row(LLSD(t["role"].asString()), { { "name", cap(t["role"].asString(), 24) }, { "where", std::string("where the object is") },
                                                              { "from", "a " + labelOf(TOOL_TYPES, t["type"].asString()) } }));
    }
    if (mPlList->getItemCount() == 0) mPlList->setCommentText("No places yet: stand there, name it and press Save where I stand.");
}

void WolfNPCMind::fillTools()
{
    mTlList->deleteAllItems();
    S32 i = 0;
    for (const LLSD& t : llsd::inArray(mCfg["tools"]))
    {
        const LLUUID id(t["id"].asString());
        LLViewerObject* obj = id.notNull() ? gObjectList.findObject(id) : nullptr;
        const std::string object = obj ? std::string("near you") : std::string("not seen near you");
        mTlList->addElement(row(LLSD(i++), { { "type", labelOf(TOOL_TYPES, t["type"].asString()) }, { "role", cap(t["role"].asString(), 24) },
                                             { "object", object }, { "face", t["type"].asString() == "whiteboard" ? llformat("%d", t["face"].asInteger()) : std::string("-") } }));
    }
    if (mTlList->getItemCount() == 0) mTlList->setCommentText("It uses nothing yet.");
    mPlError->setText(LLStringUtil::null);
}

void WolfNPCMind::onSavePlace()
{
    mPlError->setText(LLStringUtil::null);
    const std::string name = trimmed(mPlName->getText());
    // Source: §3 places "up to 16 named spots, names 1-24 chars"
    if (name.empty() || utf8Chars(name) > 24) { mPlError->setText(std::string("Give the place a name of 1 to 24 characters.")); return; }
    LLSD places = mCfg["places"].isMap() ? mCfg["places"] : LLSD::emptyMap();
    if (!places.has(name) && places.size() >= MAX_PLACES) { mPlError->setText(std::string("It already knows 16 places, the most it can. Remove one first.")); return; }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    const LLVector3 p = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    LLSD pos = LLSD::emptyArray();
    for (S32 k = 0; k < 3; ++k) pos.append(LLSD(std::round((F64)p.mV[k] * 100.0) / 100.0));
    places[name] = pos;
    LLSD changes;
    changes["places"] = places;
    mPlName->setText(LLStringUtil::null);
    mSave(changes, "\"" + name + "\" is where you stand.", [this](const LLSD& cfg) { mCfg = cfg; fillPlaces(); });
}

void WolfNPCMind::onLink()
{
    mPlError->setText(LLStringUtil::null);
    // the object the person has selected in the world (right-click > Edit); the region checks it is theirs (§23.1)
    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode();
    LLViewerObject* obj = node ? node->getObject() : nullptr;
    if (!obj) { mPlError->setText(std::string("Select one of your objects in the world first (right-click it and choose Edit).")); return; }
    if (obj->isAvatar()) { mPlError->setText(std::string("That is an avatar: select an object.")); return; }
    const std::string role = lower(trimmed(mTlRole->getText()));
    if (role.empty() || role.size() > 24 || role.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789 _-") != std::string::npos)
    {
        mPlError->setText(std::string("Give it a name of a-z, 0-9, spaces, _ and - (up to 24), e.g. board."));
        return;
    }
    LLSD tools = mCfg["tools"].isArray() ? mCfg["tools"] : LLSD::emptyArray();
    if (tools.size() >= MAX_TOOLS) { mPlError->setText(std::string("It uses 8 things already, the most it can. Unlink one first.")); return; }
    for (const LLSD& t : llsd::inArray(tools))
    {
        if (t["role"].asString() == role) { mPlError->setText(std::string("It already uses something called that.")); return; }
    }
    LLSD t = LLSD::emptyMap();
    t["type"] = mTlType->getValue().asString();
    t["id"] = obj->getID().asString();
    t["role"] = role;
    if (t["type"].asString() == "whiteboard") t["face"] = (S32)std::round(mTlFace->get());
    tools.append(t);
    LLSD changes;
    changes["tools"] = tools;
    const std::string name = node->mName.empty() ? std::string("the object") : cap(node->mName, 48);
    mSave(changes, "Linked " + name + " as its " + role + ".", [this](const LLSD& cfg) { mCfg = cfg; fillPlaces(); fillTools(); });
}
