/**
 * @file wolffloaterskylevels.cpp
 * @brief WolfViewer: Sky Land window (ticket 541). See wolffloaterskylevels.h.
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

#include "wolffloaterskylevels.h"

#include <algorithm>
#include <cmath>

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewerregion.h"
#include "llworld.h"   // [W104 FEATFIX review/] the region the change went to
#include "wolfgrid.h"
#include "wolfterrainlevels.h"

namespace
{
    const LLColor4 C_FAIL(1.f, 0.56f, 0.52f, 1.f);   // wolffloaterdjshows.cpp C_ERROR: the caution colour on the dark skin

    // "1,048,576" - the thousands separated as WolfStorm's m() does (toLocaleString('en-GB'))
    std::string grouped(S64 n)
    {
        std::string s = std::to_string(n < 0 ? -n : n);
        for (S32 i = (S32)s.size() - 3; i > 0; i -= 3)
        {
            s.insert((size_t)i, ",");
        }
        return n < 0 ? "-" + s : s;
    }
}

WolfFloaterSkyLevels::WolfFloaterSkyLevels(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterSkyLevels::postBuild()
{
    mStatus = getChild<LLTextBox>("status");
    mNone = getChild<LLTextBox>("none");
    mLimits = getChild<LLTextBox>("limits");
    mList = getChild<LLScrollListCtrl>("levels_list");
    mRemoveLevel = getChild<LLButton>("remove_level_btn");
    mName = getChild<LLLineEditor>("name_edit");
    mBase = getChild<LLSpinCtrl>("base_spin");
    mAdd = getChild<LLButton>("add_btn");
    mLevel = getChild<LLComboBox>("level_combo");
    mSize = getChild<LLComboBox>("size_combo");

    mAdd->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdd(); });
    mRemoveLevel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveLevel(); });
    getChild<LLButton>("make_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShape("make"); });
    getChild<LLButton>("raise_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShape("raise"); });
    getChild<LLButton>("lower_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShape("lower"); });
    getChild<LLButton>("flatten_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShape("flatten"); });
    getChild<LLButton>("smooth_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShape("smooth"); });
    getChild<LLButton>("remove_here_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveHere(); });
    getChild<LLButton>("undo_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        // [W104 FEATFIX review 2026-11-11] only the window's own changes (wolfterrainlevels.h windowMayUndo): the region's
        // "undo" takes back whatever this login changed last, and the brush / Terraform window undo by sending heights back
        std::string why;
        if (WolfTerrainLevels* s = sky(); s && s->getRegion() && !WolfTerrainLevels::windowMayUndo(s->getRegion()->getHandle(), why))
        {
            say(why, true);
            return;
        }
        LLSD body = LLSD::emptyMap();
        body["op"] = "undo";
        act(body, "Undone.");
    });
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshUI(); });
    mSize->selectByValue(LLSD(16));   // 16 by 16, as WolfStorm's window opens
    return true;
}

void WolfFloaterSkyLevels::onOpen(const LLSD& key)
{
    if (WolfTerrainLevels* s = sky())
    {
        s->refresh();
    }
    mSeenChange = WolfTerrainLevels::changeCount() - 1;   // draw() fills the window at once
}

WolfTerrainLevels* WolfFloaterSkyLevels::sky() const
{
    // Wolf tools only on Wolf Territories (wolfgrid.h); the region is the one the avatar stands in (the region takes changes
    // only from avatars in it: WolfSkyLevelsModule.OnRequest "you are not in this region").
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp || !WolfGrid::isOnWolfTerritories())
    {
        return nullptr;
    }
    WolfTerrainLevels* s = regionp->getWolfSkyLevels();
    return (s && s->hasCapability()) ? s : nullptr;
}

void WolfFloaterSkyLevels::draw()
{
    if (WolfTerrainLevels::changeCount() != mSeenChange || gAgent.getRegion() != mSeenRegion)
    {
        mSeenChange = WolfTerrainLevels::changeCount();
        mSeenRegion = gAgent.getRegion();
        refreshUI();
    }
    LLFloater::draw();
}

void WolfFloaterSkyLevels::refreshUI()
{
    WolfTerrainLevels* s = sky();
    const bool on = s && s->info().isMap();
    mNone->setVisible(!on);
    getChild<LLPanel>("main_panel")->setVisible(on);
    if (!on)
    {
        return;
    }
    const LLSD& info = s->info();
    const bool manage = s->mayManage();

    std::vector<const WolfTerrainLevels::Level*> list;
    for (const WolfTerrainLevels::Level& l : s->levels())
    {
        list.push_back(&l);
    }
    std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return a->mBase < b->mBase; });

    const LLSD keep_list = mList->getSelectedValue();
    mList->deleteAllItems();
    if (list.empty())
    {
        mList->setCommentText(std::string("No levels yet."));
    }
    for (const WolfTerrainLevels::Level* l : list)
    {
        LLSD row = LLSD::emptyMap();
        row["value"] = l->mId;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = l->mName;
        row["columns"][1]["column"] = "from";
        row["columns"][1]["value"] = grouped((S64)std::lround(l->mBase)) + " m";
        row["columns"][2]["column"] = "area";
        row["columns"][2]["value"] = grouped((S64)l->mPatches.size() * 256) + " m\xC2\xB2";
        mList->addElement(row);
    }
    if (keep_list.isDefined())
    {
        mList->selectByValue(keep_list);
    }
    mRemoveLevel->setVisible(manage);
    mRemoveLevel->setEnabled(manage && mList->getFirstSelected() != nullptr && !mBusy);

    mLimits->setText("Up to " + grouped(info["max_levels"].asInteger()) + " levels and "
                     + grouped((S64)info["quota"].asInteger() * 256) + " m\xC2\xB2 of sky land in all ("
                     + grouped((S64)info["used"].asInteger() * 256) + " m\xC2\xB2 used); ground up to "
                     + grouped((S64)info["max_height"].asReal()) + " m.");

    const S32 max_levels = info.has("max_levels") ? info["max_levels"].asInteger() : 4;
    getChild<LLPanel>("add_panel")->setVisible(manage && (S32)list.size() < max_levels);

    const LLSD keep = mLevel->getValue();
    mLevel->removeall();
    for (const WolfTerrainLevels::Level* l : list)
    {
        mLevel->add(l->mName + " (" + grouped((S64)std::lround(l->mBase)) + " m)", LLSD(l->mId));
    }
    bool kept = false;
    for (const WolfTerrainLevels::Level* l : list)
    {
        if (keep.isDefined() && keep.asInteger() == l->mId)
        {
            mLevel->selectByValue(keep);
            kept = true;
        }
    }
    if (!kept && !list.empty())
    {
        // the level nearest below where the avatar is, else the lowest (WolfStorm's _render: levelUnder)
        LLVector3 p;
        S32 pick = list.front()->mId;
        F32 best = -F32_MAX;
        if (here(p))
        {
            for (const WolfTerrainLevels::Level* l : list)
            {
                F32 h;
                if (s->surface(*l, p.mV[VX], p.mV[VY], h) && h <= p.mV[VZ] + 0.5f && h > best)
                {
                    best = h;
                    pick = l->mId;
                }
            }
        }
        mLevel->selectByValue(LLSD(pick));
    }
}

void WolfFloaterSkyLevels::say(const std::string& text, bool failure)
{
    mStatus->setColor(failure ? LLUIColor(C_FAIL) : LLUIColorTable::instance().getColor("LabelTextColor"));
    mStatus->setText(text);
    // what the window told the person, in the log too (support tickets and tests read it there)
    LL_INFOS("WolfSkyLevels") << "window " << (failure ? "refused: " : "says: ") << text << LL_ENDL;
}

bool WolfFloaterSkyLevels::here(LLVector3& pos) const
{
    LLViewerRegion* regionp = gAgent.getRegion();
    if (!regionp)
    {
        return false;
    }
    pos = regionp->getPosRegionFromGlobal(gAgent.getPositionGlobal());
    return llfinite(pos.mV[VX]) && llfinite(pos.mV[VY]);
}

S32 WolfFloaterSkyLevels::chosenLevel() const
{
    const LLSD v = mLevel->getValue();
    return v.isDefined() ? v.asInteger() : 0;
}

void WolfFloaterSkyLevels::act(const LLSD& body, const std::string& done_text)
{
    WolfTerrainLevels* s = sky();
    if (!s)
    {
        say("This region does not have sky land.", true);
        return;
    }
    if (mBusy)
    {
        return;
    }
    mBusy = true;
    say("Working...", false);
    LLHandle<LLFloater> h = getHandle();
    const U64 region = s->getRegion() ? s->getRegion()->getHandle() : 0;
    const std::string op = body["op"].asString();
    s->post(body, [h, done_text, region, op](const LLSD& answer)
    {
        // [W104 FEATFIX review 2026-11-11] which change the region now remembers as this login's last (wolfterrainlevels.h):
        // create / delete / set always make a step; apply / remove only when they changed something; undo takes one back
        if (answer["success"].asBoolean())
        {
            if (op == "undo") WolfTerrainLevels::noteWindowUndo(region);
            else if (op == "create" || op == "delete" || op == "set" || answer["changed"].asInteger() > 0)
                WolfTerrainLevels::noteChange(region, WolfTerrainLevels::EDITOR_WINDOW);
        }
        WolfFloaterSkyLevels* self = static_cast<WolfFloaterSkyLevels*>(h.get());
        if (!self)
        {
            return;
        }
        self->mBusy = false;
        // WolfSkyLevelsModule.Edit.cs Handle: a change that went through but changed nothing because every cell was refused (no
        // terraform rights there) or over the quota is success with changed 0 - a refusal to the person pressing
        const bool nothing = answer["success"].asBoolean() && answer["changed"].asInteger() == 0
                          && (answer["refused"].asInteger() > 0 || answer["no_room"].asInteger() > 0);
        if (nothing)
        {
            self->say(answer.has("message") ? answer["message"].asString()
                                            : std::string("Nothing changed: you may not change the ground here."), true);
        }
        else if (answer["success"].asBoolean())
        {
            self->say(answer.has("message") ? answer["message"].asString() : done_text, false);
        }
        else
        {
            const std::string why = answer["message"].asString();
            self->say(why.empty() ? std::string("The region did not do that. Please try again.") : why, true);
        }
        self->refreshUI();
    });
}

void WolfFloaterSkyLevels::onAdd()
{
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    const F32 base = (F32)mBase->getValue().asReal();
    if (name.empty())
    {
        say("Please give the level a name.", true);
        return;
    }
    if (!(base >= 1.f && base < 4000.f))
    {
        say("The height must be between 1 and 3,990 metres.", true);
        return;
    }
    LLSD body = LLSD::emptyMap();
    body["op"] = "create";
    body["name"] = name;
    body["base"] = base;
    act(body, "\"" + name + "\" added at " + grouped((S64)std::lround(base)) + " m. Fly up there and press Make ground here.");
}

void WolfFloaterSkyLevels::onRemoveLevel()
{
    WolfTerrainLevels* s = sky();
    LLScrollListItem* item = mList->getFirstSelected();
    const WolfTerrainLevels::Level* l = (s && item) ? s->findLevel(item->getValue().asInteger()) : nullptr;
    if (!l)
    {
        say("Choose the level to remove in the list first.", true);
        return;
    }
    LLSD body = LLSD::emptyMap();
    body["op"] = "delete";
    body["level"] = l->mId;
    act(body, "\"" + l->mName + "\" removed. Undo my last change puts it back.");
}

void WolfFloaterSkyLevels::onShape(const std::string& tool)
{
    WolfTerrainLevels* s = sky();
    const WolfTerrainLevels::Level* l = s ? s->findLevel(chosenLevel()) : nullptr;
    LLVector3 p;
    if (!l)
    {
        say("Add a level first.", true);
        return;
    }
    if (!here(p))
    {
        say("Your position is not known yet. Please wait a moment.", true);
        return;
    }
    const S32 size = llmax(1, mSize->getValue().asInteger());
    S32 x0, y0, w, h;
    std::vector<F32> heights;
    if (!s->brush(*l, tool, p.mV[VX], p.mV[VY], size, 2.f, l->mBase + 1.f, x0, y0, w, h, heights))
    {
        say("You are outside this region.", true);
        return;
    }
    if (tool != "make" && std::none_of(heights.begin(), heights.end(), [](F32 v) { return llfinite(v); }))
    {
        say("There is no \"" + l->mName + "\" ground where you stand. Press Make ground here first.", true);
        return;
    }
    const std::string words = tool == "make" ? "Ground made." : tool == "raise" ? "Raised." : tool == "lower" ? "Lowered."
                            : tool == "flatten" ? "Flattened." : "Smoothed.";
    act(WolfTerrainLevels::applyBody(l->mId, x0, y0, w, h, heights, tool), words);
}

void WolfFloaterSkyLevels::onRemoveHere()
{
    WolfTerrainLevels* s = sky();
    const WolfTerrainLevels::Level* l = s ? s->findLevel(chosenLevel()) : nullptr;
    LLVector3 p;
    if (!l || !here(p))
    {
        say("Add a level first, and stand where the ground should go.", true);
        return;
    }
    // whole 16 m squares only (WolfSkyLevelsModule.Remove): the squares under the chosen area, inside the region
    const F32 size = (F32)llmax(16, mSize->getValue().asInteger());
    const S32 P = WolfTerrainLevels::PATCH;
    const S32 R = (S32)s->getRegion()->getWidth();
    const S32 x0 = llmax(0, (S32)std::floor((p.mV[VX] - size / 2.f) / P) * P);
    const S32 y0 = llmax(0, (S32)std::floor((p.mV[VY] - size / 2.f) / P) * P);
    const S32 x1 = llmin(R, (S32)std::ceil((p.mV[VX] + size / 2.f) / P) * P);
    const S32 y1 = llmin(R, (S32)std::ceil((p.mV[VY] + size / 2.f) / P) * P);
    LLSD body = LLSD::emptyMap();
    body["op"] = "remove";
    body["level"] = l->mId;
    body["x"] = x0;
    body["y"] = y0;
    body["w"] = x1 - x0;
    body["h"] = y1 - y0;
    act(body, "Ground removed. Undo my last change puts it back.");
}
