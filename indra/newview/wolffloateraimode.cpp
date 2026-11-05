/**
 * @file wolffloateraimode.cpp
 * @brief WolfViewer: Wolf World > AI Mode: Personality and Voice... See wolffloateraimode.h.
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

#include "wolffloateraimode.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "lltextbox.h"
#include "llsdutil.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "llweb.h"
#include "wolfaimode.h"
#include "wolfgrid.h"

WolfFloaterAIMode::WolfFloaterAIMode(const LLSD& key)
    : LLFloater(key)
{
}

bool WolfFloaterAIMode::postBuild()
{
    getChild<LLComboBox>("preset_combo")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPreset(); });
    getChild<LLButton>("save_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    getChild<LLButton>("cancel_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void WolfFloaterAIMode::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE 2026-10-17] a Wolf-only window opened off Wolf says so and closes (wolffloateragecheck.cpp onOpen)
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly(getString("feature_name"));
        closeFloater();
        return;
    }
    mTurnOn = key.isMap() && key["turn_on"].asBoolean();
    getChild<LLButton>("save_btn")->setLabel(getString(mTurnOn || !WolfAIMode::instance().isOn() ? "save_on_label" : "save_label"));
    load();
}

void WolfFloaterAIMode::setStatus(const std::string& text, bool error)
{
    LLTextBox* t = getChild<LLTextBox>("status_text");
    t->setText(text);
    t->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterAIMode::load()
{
    mBusy = true;
    getChild<LLButton>("save_btn")->setEnabled(false);
    setStatus(getString("loading"), false);
    LLHandle<LLFloater> h = getHandle();
    WolfAIMode::api("open", LLSD::emptyMap(), [h](bool ok, const LLSD& r, const std::string& error, const std::string&)
    {
        WolfFloaterAIMode* self = static_cast<WolfFloaterAIMode*>(h.get());
        if (!self) return;
        self->mBusy = false;
        if (!ok) { self->setStatus(self->getString("could_not_load") + " " + error, true); return; }
        if (!r["allowed"].asBoolean()) { self->setStatus(r["reason"].asString(), true); return; }
        self->mPresets = r["presets"];
        LLComboBox* preset = self->getChild<LLComboBox>("preset_combo");
        preset->removeall();
        for (const LLSD& p : llsd::inArray(r["presets"])) preset->add(p["label"].asString(), p["id"]);
        preset->add(self->getString("own_words"), LLSD(""));
        LLComboBox* voice = self->getChild<LLComboBox>("voice_combo");
        voice->removeall();
        for (const LLSD& v : llsd::inArray(r["voices"])) voice->add(v["label"].asString(), v["id"]);
        const LLSD setup = r["setup"];
        LLTextEditor* words = self->getChild<LLTextEditor>("personality_editor");
        if (setup.isMap())
        {
            preset->setValue(setup["preset"]);
            words->setText(setup["personality"].asString());
            voice->setValue(setup["voice"]);
        }
        else
        {
            preset->selectFirstItem();
            self->onPreset();
            voice->selectFirstItem();
        }
        LLStringUtil::format_map_t args;
        args["[PER]"] = llformat("%.2f", r["perReply"].asReal());
        args["[BALANCE]"] = r["balance"].isDefined() ? llformat("%d", r["balance"].asInteger()) : self->getString("balance_unknown");
        self->getChild<LLTextBox>("cost_text")->setText(self->getString("cost_line", args));
        self->setStatus("", false);
        self->getChild<LLButton>("save_btn")->setEnabled(true);
    });
}

void WolfFloaterAIMode::onPreset()
{
    const std::string id = getChild<LLComboBox>("preset_combo")->getValue().asString();
    for (const LLSD& p : llsd::inArray(mPresets))
    {
        if (p["id"].asString() == id)
        {
            getChild<LLTextEditor>("personality_editor")->setText(p["text"].asString());
            return;
        }
    }
    // "In my own words": an empty box to write in
    getChild<LLTextEditor>("personality_editor")->setText(LLStringUtil::null);
    getChild<LLTextEditor>("personality_editor")->setFocus(true);
}

void WolfFloaterAIMode::onSave()
{
    if (mBusy) return;
    LLSD body;
    body["preset"] = getChild<LLComboBox>("preset_combo")->getValue().asString();
    std::string words = getChild<LLTextEditor>("personality_editor")->getText();
    LLStringUtil::trim(words);
    // edited words are the resident's own, whatever was picked first
    for (const LLSD& p : llsd::inArray(mPresets)) if (p["id"].asString() == body["preset"].asString() && p["text"].asString() != words) body["preset"] = "";
    body["personality"] = words;
    body["voice"] = getChild<LLComboBox>("voice_combo")->getValue().asString();
    mBusy = true;
    getChild<LLButton>("save_btn")->setEnabled(false);
    setStatus(getString("saving"), false);
    LLHandle<LLFloater> h = getHandle();
    const bool turn_on = mTurnOn || !WolfAIMode::instance().isOn();
    WolfAIMode::api("save", body, [h, turn_on](bool ok, const LLSD&, const std::string& error, const std::string&)
    {
        WolfFloaterAIMode* self = static_cast<WolfFloaterAIMode*>(h.get());
        if (self)
        {
            self->mBusy = false;
            self->getChild<LLButton>("save_btn")->setEnabled(true);
            if (!ok) { self->setStatus(error, true); return; }   // stays open: the resident reads it and changes the words
        }
        else if (!ok) return;
        if (turn_on) WolfAIMode::instance().turnOnAfterSetup();
        if (self) self->closeFloater();
    });
}
