/**
 * @file wolffloaterimportworlds.cpp
 * @brief WolfViewer: Build > Import worlds... - bring in a Minecraft build, a Roblox place or a MagicaVoxel model.
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

#include "wolffloaterimportworlds.h"

#include <ctime>

#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llfloaterreg.h"
#include "llstyle.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgrid.h"

// [IMPORT WORLDS 2026-10-18] Source for every word and step: GAME_CARD_SPINE_SPEC.md "Change (Paul, 2026-10-18 ~15:20Z): Build ->
// Import worlds..." (the three choices and their lines, the trademark and legal lines, "Import it - free" / "Import it - N
// credits", the amber warnings, "Also make it a game (checkpoints and a finish)", "Set up your game", red failures that stay,
// Buy credits... on credits_short). The flow is WolfImportFlow (wolfimportflow.cpp), the same one as the Studio's Story page.

namespace
{
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    /** Of a light and a dark version of a status colour, the one that reads better on the floater's background (every skin:
     *  dark skins get the light one, light skins the dark one). Source: WolfAdvStudio::contrast (WCAG 2.2 contrast ratio). */
    LLColor4 statusInk(const LLColor4& light, const LLColor4& dark)
    {
        LLColor4 bg = LLUIColorTable::instance().getColor("FloaterDefaultBackgroundColor");
        bg.mV[VALPHA] = 1.f;
        return WolfAdvStudio::contrast(light, bg) >= WolfAdvStudio::contrast(dark, bg) ? light : dark;
    }
    // Tailwind's red-400 / red-700, amber-400 / amber-800, green-400 / green-700 (the Studio's WARN_INK is amber-400, #fbbf24)
    LLColor4 dangerInk() { return statusInk(LLColor4(0.973f, 0.443f, 0.443f, 1.f), LLColor4(0.725f, 0.110f, 0.110f, 1.f)); }
    LLColor4 warnInk()   { return statusInk(LLColor4(0.984f, 0.749f, 0.141f, 1.f), LLColor4(0.573f, 0.251f, 0.055f, 1.f)); }
    LLColor4 okInk()     { return statusInk(LLColor4(0.290f, 0.871f, 0.502f, 1.f), LLColor4(0.082f, 0.502f, 0.239f, 1.f)); }

    const char* choiceWords(WolfImportFlow::EChoice c)
    {
        switch (c)
        {
        case WolfImportFlow::C_MINECRAFT: return "Minecraft build";
        case WolfImportFlow::C_ROBLOX: return "Roblox place";
        case WolfImportFlow::C_VOXEL: return "MagicaVoxel model";
        default: return "Build";
        }
    }
}

WolfFloaterImportWorlds::WolfFloaterImportWorlds(const LLSD& key)
:   LLFloater(key)
{
}

// static
std::shared_ptr<WolfImportFlow>& WolfFloaterImportWorlds::flow()
{
    static std::shared_ptr<WolfImportFlow> sFlow;
    if (!sFlow)
    {
        sFlow = WolfImportFlow::create();   // "Also make it a game" starts off (opt in)
        // read on while the window is closed too (the status says "You can close this window: it carries on"); tick() only
        // calls out when a poll is due. Source: llcallbacklist.h gIdleCallbacks.addFunction(callback_t, void*)
        gIdleCallbacks.addFunction([](void*) { if (sFlow) sFlow->tick(); }, nullptr);
    }
    return sFlow;
}

bool WolfFloaterImportWorlds::postBuild()
{
    static const WolfImportFlow::EChoice CHOICES[3] = { WolfImportFlow::C_MINECRAFT, WolfImportFlow::C_ROBLOX, WolfImportFlow::C_VOXEL };
    static const char* NAMES[3] = { "choice_minecraft", "choice_roblox", "choice_voxel" };
    for (S32 i = 0; i < 3; ++i)
    {
        mChoice[i] = getChild<LLButton>(NAMES[i]);
        const WolfImportFlow::EChoice c = CHOICES[i];
        mChoice[i]->setCommitCallback([c](LLUICtrl*, const LLSD&) { flow()->pick(c); });
    }
    mPrimary = getChild<LLButton>("primary_btn");
    mPrimary->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPrimary(); });
    mCancel = getChild<LLButton>("cancel_btn");
    mCancel->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancel(); });
    mBuy = getChild<LLButton>("buy_btn");
    // the one quiet link button bottom left: Buy credits... after a credits_short refusal, or Rent a region... for a non-owner
    // when the brain names a shop (shopUrl)
    mBuy->setCommitCallback([](LLUICtrl*, const LLSD&) { if (flow()->importAllowed()) flow()->openBuyPage(); else flow()->openShopPage(); });
    mMakeGame = getChild<LLCheckBoxCtrl>("make_game");
    mMakeGame->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { flow()->setMakeGame(ctrl->getValue().asBoolean()); });
    mStatus = getChild<LLTextBox>("status");
    mError = getChild<LLTextBox>("error");
    mReport = getChild<LLTextEditor>("report");
    setDefaultBtn(mPrimary);   // the one primary action: Enter presses it
    refresh();
    return true;
}

void WolfFloaterImportWorlds::onOpen(const LLSD& key)
{
    // Wolf Territories only (paul-wolf-features-wolf-grid-only; the menu items are hidden elsewhere, and the brain's api refuses
    // any other grid unsent: WolfAdventure::api)
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    flow()->checkAllowed();   // [IMPORT OWNERS 2026-10-18/] region owners only: greyed with the reason before they try
    mShown.clear();
    refresh();
}

void WolfFloaterImportWorlds::draw()
{
    refresh();   // the job is read by the idle callback (flow())
    LLFloater::draw();
}

void WolfFloaterImportWorlds::onPrimary()
{
    WolfImportFlow& fl = *flow();
    if (fl.asking()) fl.start();
    else if (!fl.madeAdventure().empty()) onSetUp();
    else if (fl.hasJob() && !fl.running()) fl.finish();
}

void WolfFloaterImportWorlds::onCancel()
{
    WolfImportFlow& fl = *flow();
    if (fl.asking()) fl.dismissQuote();
    else if (fl.hasJob() && !fl.running()) fl.finish();
}

void WolfFloaterImportWorlds::onSetUp()
{
    // "Set up your game" opens the Adventure Studio on that game (WolfAdventureStudioView::onOpened {adventure}: openAdventure,
    // the owner lands on Gameplay, the story map), [SETUP WALK 2026-10-18] with its setup walk on top ("Rez imported game"):
    // setup:true reads the walk again when that game is open already
    const std::string adv = flow()->madeAdventure();
    if (adv.empty()) return;
    LLSD key = LLSD::emptyMap();
    key["adventure"] = adv;
    key["setup"] = true;
    LLFloaterReg::showInstance("wolf_adventure_studio", key, true);
}

void WolfFloaterImportWorlds::refresh()
{
    if (!mPrimary) return;
    WolfImportFlow& fl = *flow();
    const LLSD& job = fl.job();
    const bool running = fl.running(), asking = fl.asking(), busy = fl.busy();
    const std::string state = fl.state();
    const bool finished = fl.hasJob() && !running;
    const std::string made = fl.madeAdventure();
    // [IMPORT OWNERS 2026-10-18] Paul: only region owners import; for anyone else the choices stay, greyed, with the brain's reason
    const bool allowed = fl.importAllowed();

    // the status line (what is happening now)
    std::string status;
    LLColor4 statusColour = LLUIColorTable::instance().getColor("LabelTextColor");
    if (running)
    {
        const S64 el = fl.runAt() > 0 ? llmax((S64)0, (S64)(time(nullptr) - fl.runAt())) : 0;
        const std::string pr = job["progress"].asString();
        status = "Bringing in \"" + fl.file() + "\"..." + (pr.empty() ? std::string() : " Now: " + cap(pr, 80) + ".") + " "
               + (el >= 60 ? llformat("Working for %d min %d s.", (S32)(el / 60), (S32)(el % 60)) : llformat("Working for %d s.", (S32)el))
               + " You can close this window: it carries on.";
    }
    else if (state == "done")
    {
        // the heading and the brain's lines are in the report below; only what to press next is said here
        status = made.empty() ? std::string() : std::string("Press Set up your game to put it on your land.");
    }
    else if (asking)
    {
        // Source: wolfadventure_import_lib.php wa_api_import_quote -> quote{credits, micro}: credits for the upload only (section 7)
        const S32 credits = fl.quoteCredits();
        status = llformat("%s: \"%s\" (%.1f MB). ", choiceWords(fl.choice()), fl.file().c_str(), (F64)fl.bytes() / (1024.0 * 1024.0))
               + (credits > 0 ? llformat("Importing it costs %d credits (US $%.2f).", credits, (F64)credits / 100.0) : std::string("Importing it is free."));
    }
    else if (busy) status = fl.hasData() ? "Reading \"" + fl.file() + "\" and working out the cost..." : "Sending \"" + fl.file() + "\"...";
    else if (!allowed) { status = fl.notAllowedWhy(); statusColour = warnInk(); }
    else if (fl.error().empty()) status = "Choose what you are bringing in, then pick the file.";

    // the primary button: the cost on it while asking; Set up your game / Done when finished; else what is going on
    std::string primary = "Import it";
    bool primaryOn = false;
    if (asking)
    {
        const S32 credits = fl.quoteCredits();
        primary = busy ? std::string("Sending...") : credits > 0 ? llformat("Import it - %d credits", credits) : std::string("Import it - free");
        primaryOn = !busy;
    }
    else if (finished) { primary = made.empty() ? "Done" : "Set up your game"; primaryOn = true; }
    else if (running) primary = "Importing...";
    else if (busy) primary = "Working out the cost...";
    const bool cancelShown = (asking && !busy) || (finished && !made.empty());
    const std::string cancel = asking ? "Cancel" : "Done";
    const bool choicesOn = allowed && !busy && !running && !asking;
    const bool gameShown = asking && fl.kind() == "obby";
    const bool buyShown = allowed ? (fl.needCredits() && !fl.error().empty()) : !fl.shopUrl().empty();

    // the report: where it went, the brain's report lines, then the warnings in amber
    const std::string reportSig = state == "done" ? fl.jobId() : std::string();   // a done job's report never changes

    const std::string sig = status + (allowed ? "|" : "|n") + primary + (primaryOn ? "1" : "0") + (cancelShown ? cancel : "") + (choicesOn ? "c" : "") + (gameShown ? "g" : "")
                          + (fl.makeGame() ? "m" : "") + (buyShown ? "b" : "") + "|" + fl.error() + "|" + reportSig;
    if (sig == mShown) return;
    mShown = sig;

    for (LLButton* b : mChoice) if (b) b->setEnabled(choicesOn);
    mStatus->setText(status);
    mStatus->setColor(statusColour);
    mPrimary->setLabel(primary);
    mPrimary->setEnabled(primaryOn);
    mCancel->setVisible(cancelShown);
    mCancel->setLabel(cancel);
    mCancel->setToolTip(asking ? std::string("Nothing is sent or charged") : std::string("Put this away"));
    mMakeGame->setVisible(gameShown);
    mMakeGame->set(fl.makeGame());
    mBuy->setVisible(buyShown);
    mBuy->setLabel(allowed ? std::string("Buy credits...") : std::string("Rent a region..."));
    mBuy->setToolTip(allowed ? std::string("Opens the Wolf Territories credits page in your browser") : std::string("Opens the page where you can rent a region, in your browser"));
    // failures in red that stay (until the next file): the flow keeps them
    mError->setText(fl.error());
    mError->setColor(dangerInk());

    mReport->clear();
    mReport->setVisible(state == "done");
    if (state == "done")
    {
        LLStyle::Params ok, text, warn;
        ok.color = okInk();
        text.color = LLUIColorTable::instance().getColor("TextFgReadOnlyColor");
        warn.color = warnInk();
        mReport->appendText(WolfImportFlow::doneWords(job), false, ok);
        for (const LLSD& rp : llsd::inArray(job["report"])) mReport->appendText(cap(rp.asString(), 400), true, text);
        // Source: wolfadventure_import_lib.php wai_job_out `warnings` (wai_script_warnings: each line starts "Warning: ...")
        for (const LLSD& w : llsd::inArray(job["warnings"])) mReport->appendText(cap(w.asString(), 800), true, warn);
    }
}
