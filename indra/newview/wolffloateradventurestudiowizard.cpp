/**
 * @file wolffloateradventurestudiowizard.cpp
 * @brief WolfViewer: the Adventure Studio's "Start here" wizard. See wolffloateradventurestudio.h.
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

/*
 * [WOLF ADVENTURE AUDIT 2026-10-17] Paul, 2026-10-17: "it needs to be really idiot proof. I just don't know what to
 * do. It should have a step-by-step wizard explaining how it works".
 *
 * Persona and story (region-terrain-work/wolf-story/AUDIT_2026-10-17.md): a resident who has never made a game wants
 * a working treasure hunt on their land in 20 minutes.
 *
 * Flow: one decision per screen - 1 what kind (a starter story from the brain's `templates`, an example, or a book),
 * name and rating; 2 where (the region they stand on); 3 the story in steps (each "the players must ... / it is done
 * when they ..."); 4 characters (names, what they say, written lines or AI with the cost in words); 5 places (stand
 * there, one button); 6 how it ends (ending text, a prize); 7 the look (cover, logo, title card, colours, with a
 * preview); 8 Test Play; 9 Publish. Each step checks itself before Next (wizCheck) and says what to fix at its top,
 * with the field marked; Back always works; "Skip to the full page" opens the matching expert page. Everything is
 * written into the draft through the same editors as the expert pages (docSet / editDoc, saved 2 s later), so the
 * wizard and the pages never disagree. The step reached is kept per adventure id in the viewer-wide setting WolfAdventureWizardSteps (settings.xml).
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloateradventurestudio.h"

#include <cmath>

#include "llagent.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "llselectmgr.h"
#include "llpermissions.h"
#include "wolfadventure.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolftooladventureplace.h"

using namespace WolfGameDraw;
using WolfAdvStudio::btnW;

namespace
{
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as wolffloateradventurestudio.cpp
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24
    const F32 ROW = 26.f;                                      // a one-line control, as the editors' ROW_H
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }
    std::string trimmed(const std::string& s) { std::string t = s; LLStringUtil::trim(t); return t; }

    // The starter words new items carry (wolffloateradventurestudioedit.cpp starterText): still there = not written yet.
    const char* STARTER_OBJECTIVE = "Describe what the players must do here";
    const char* STARTER_ROLE = "Describe who they are";
    const char* STARTER_ENDING = "Write what the players read when the story ends here.";

    // Source: adventure_pack_v1.schema.json castSheet first / last pattern ^[A-Za-z][A-Za-z0-9'-]{0,30}$ (NPC names).
    bool npcName(const std::string& s)
    {
        if (s.empty() || s.size() > 31 || !isalpha((unsigned char)s[0])) return false;
        for (char c : s) if (!isalnum((unsigned char)c) && c != '\'' && c != '-') return false;
        return true;
    }

    struct Step { const char* mTitle; const char* mWhy; const char* mExample; const char* mBrainStep; };
    // AUDIT_2026-10-17.md §5.1: `mBrainStep` is the brain's problem `step` for this wizard step.
    const Step STEPS[WolfAdventureStudioView::W_COUNT] = {
        { "What kind of adventure?", "Pick the kind closest to your idea. It starts as a short, working story that you change into your own.",
          "A treasure hunt: three clues around your land and a prize at the end.", "kind" },
        { "Where does it happen?", "An adventure runs on regions. On land you hold it works at once; on someone else's land, its holder has to say yes first.",
          "Stand on your land and press Use this region.", "where" },
        { "Your story, step by step", "Players follow the steps in order. Each step is one thing to do, and it is done when they do it - then the next one starts.",
          "1. Find the old well (they reach a place). 2. Ask the miller about the map (they talk to someone). 3. Open the chest in the barn (they touch an object).", "story" },
        { "Who is in it?", "Characters stand in your world, greet players and say their lines. Written lines are free; AI characters talk freely and cost a little.",
          "Ma Dalton, who runs the saloon: \"Howdy, stranger. Heard about the gold?\"", "cast" },
        { "Put the places in the world", "Walk to each place and press Use my position. While this step is open, the places are drawn in the world so you can see them.",
          "Stand next to the well, press Use my position - done.", "places" },
        { "How does it end?", "Write what players read when they finish, and give a prize if you like.",
          "\"You found the gold! The town will remember you.\" Prize: 50 money.", "rewards" },
        { "Make it look like yours", "A cover and a logo show in the Adventures list; the title card greets players when they start; the colours dress their story panel.",
          "A wanted-poster cover, your logo, and warm frontier colours.", "look" },
        { "Try it yourself", "Test Play runs the story for you alone. Walk the steps yourself, or press Fire to pretend a step happened. Nothing it gives is real.",
          "Start the test, walk to the well, and watch the next step appear.", "test" },
        { "Publish it", "Publishing puts it live on your region for everyone. You can change it and publish again at any time.",
          "Fix anything marked Must fix, then press Publish.", "publish" } };

    // The colour sets offered on the look step (accent on panel; every pair checked: 7.4:1 to 10.3:1, WCAG 1.4.3 >= 4.5).
    struct Theme { const char* mName; const char* mAccent; const char* mPanel; };
    const Theme THEMES[] = { { "Frontier", "#f4a340", "#2a1d12" }, { "Forest", "#7bd88f", "#102418" }, { "Night", "#9ec5ff", "#0f1630" },
                             { "Parchment", "#6b3d0c", "#f3e6c4" }, { "Ocean", "#5fe0e6", "#0b2230" }, { "Rose", "#ff9fb2", "#2b1020" } };

    std::string beatPtr(S32 a, S32 s, S32 b) { return llformat("/acts/%d/scenes/%d/beats/%d", a, s, b); }
    /** Every beat in pack order: {pointer, beat}. */
    std::vector<std::pair<std::string, const LLSD*>> beatsInOrder(const LLSD& pk)
    {
        std::vector<std::pair<std::string, const LLSD*>> out;
        const LLSD& acts = pk["acts"];
        for (S32 a = 0; acts.isArray() && a < (S32)acts.size(); ++a)
            for (S32 s = 0; s < (S32)acts[a]["scenes"].size(); ++s)
                for (S32 b = 0; b < (S32)acts[a]["scenes"][s]["beats"].size(); ++b)
                    out.push_back({ beatPtr(a, s, b), &acts[a]["scenes"][s]["beats"][b] });
        return out;
    }
    bool placedPos(const LLSD& v) { return v.isArray() && v.size() >= 2 && v[0].asReal() >= 0.0 && v[1].asReal() >= 0.0; }
    /** Is a place (spot, area or path) in the world (ADVENTURE_PACK_V1.md: unplaced = x or y below 0)? */
    bool placeIsPlaced(const std::string& kind, const LLSD& it)
    {
        if (kind == "spots") return placedPos(it["pos"]);
        if (kind == "areas")
        {
            if (it["shape"]["circle"].isMap()) return placedPos(it["shape"]["circle"]["centre"]);
            const LLSD& poly = it["shape"]["polygon"];
            if (!poly.isArray() || poly.size() < 3) return false;
            for (const LLSD& pt : llsd::inArray(poly)) if (!placedPos(pt)) return false;
            return true;
        }
        if (kind == "paths")
        {
            const LLSD& pts = it["points"];
            if (!pts.isArray() || pts.size() < 2) return false;
            for (const LLSD& pt : llsd::inArray(pts)) if (!placedPos(pt)) return false;
            return true;
        }
        return true;
    }
    std::string placeName(const LLSD& it) { return it["label"].asString().empty() ? it["id"].asString() : it["label"].asString(); }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// state
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::rememberWizStep()
{
    if (mOpenId.empty()) return;
    LLSD steps = gSavedSettings.getLLSD("WolfAdventureWizardSteps");
    if (!steps.isMap()) steps = LLSD::emptyMap();
    LLSD e;
    e["step"] = (S32)mWizStep;
    e["tested"] = mWizTested;
    steps[mOpenId] = e;
    // at most 50 adventures remembered: the oldest entries go first (LLSD maps keep no order, so any 50 is fine)
    while (steps.size() > 50)
    {
        std::string drop;
        for (const auto& kv : llsd::inMap(steps)) if (kv.first != mOpenId) { drop = kv.first; break; }
        if (drop.empty()) break;
        steps.erase(drop);
    }
    gSavedSettings.setLLSD("WolfAdventureWizardSteps", steps);
}

void WolfAdventureStudioView::wizGo(S32 step)
{
    commitEdit();
    mField = F_NONE;
    mWizStep = (EWizStep)llclamp(step, (S32)W_KIND, (S32)W_COUNT - 1);
    if (mOpenId.empty()) mWizStep = W_KIND;   // every later step works on an adventure
    mWizError.clear();
    mScroll[SP_WIZARD] = 0.f;
    mWizAuto = false;
    if (mWizStep == W_CAST || mWizStep == W_TEST) loadBudget();
    if (mWizStep == W_KIND) loadTemplates();
    rememberWizStep();
}

void WolfAdventureStudioView::openWizardStep(S32 step)
{
    mPage = SP_WIZARD;
    wizGo(step);
}

void WolfAdventureStudioView::wizNext()
{
    commitEdit();
    mField = F_NONE;
    const std::string err = wizCheck(mWizStep);
    if (!err.empty())
    {
        // the summary at the top of the step, in view, until it is right (GOV.UK validation: say what to fix, where)
        mWizError = err;
        mScroll[SP_WIZARD] = 0.f;
        return;
    }
    if (mWizStep == W_KIND && mOpenId.empty()) { wizCreate(); return; }
    wizGo((S32)mWizStep + 1);
}

std::string WolfAdventureStudioView::newPackId(const std::string& base) const
{
    return WolfAdvStudio::uniqueId(base, mDraftJson);
}

S32 WolfAdventureStudioView::doneTrigger(const LLSD& beat) const
{
    const LLSD& tr = beat["triggers"];
    for (S32 i = 0; tr.isArray() && i < (S32)tr.size(); ++i)
    {
        if (tr[i].has("next")) return i;
        for (const LLSD& e : llsd::inArray(tr[i]["do"])) if (e.has("goto") || e.has("end")) return i;
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the checks: one per step, in plain words (shown at the top of the step; the fields are marked too)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

std::string WolfAdventureStudioView::wizCheck(S32 step) const
{
    const LLSD& pk = mDraftPack;
    switch (step)
    {
    case W_KIND:
        if (mOpenId.empty())
        {
            if (mWizTemplate.empty()) return "Choose what kind of adventure it is (one of the cards).";
            if (trimmed(mNewTitle).empty()) return "Type a name for your adventure.";
            return std::string();
        }
        if (trimmed(pk["adventure"]["title"].asString()).empty()) return "Type a name for your adventure.";
        return std::string();
    case W_WHERE:
    {
        const LLSD& rs = pk["adventure"]["regions"];
        if (!rs.isArray() || rs.size() == 0) return "Press Use this region to add the region you are standing on.";
        if (const LLSD* a = listed(mOpenId))
        {
            bool any = false;
            for (const LLSD& rg : llsd::inArray((*a)["regions"])) any = any || rg["state"].asString() != "refused";
            if ((*a)["regions"].size() > 0 && !any) return "Every region's holder said no. Stand on another region and press Use this region.";
        }
        return std::string();
    }
    case W_STORY:
    {
        const auto beats = beatsInOrder(pk);
        if (beats.empty()) return "Add at least one step.";
        for (size_t i = 0; i < beats.size(); ++i)
        {
            const LLSD& b = *beats[i].second;
            const std::string obj = trimmed(b["objective"].asString());
            if (obj.empty() || obj == STARTER_OBJECTIVE) return llformat("Step %d: write what the players must do.", (S32)i + 1);
            const S32 ti = doneTrigger(b);
            // a step that ends with a choice (its decisions lead on, no trigger does) is the expert's, and right as it is
            if (ti < 0 && b["decisions"].isArray() && b["decisions"].size() > 0) continue;
            if (ti < 0) return llformat("Step %d: choose when it is done.", (S32)i + 1);
            const LLSD& t = b["triggers"][ti];
            const std::string on = t["on"].asString();
            if (on == "arrive" && t["area"].asString().empty()) return llformat("Step %d: choose the place they must reach.", (S32)i + 1);
            if (on == "talk" && t["cast"].asString().empty()) return llformat("Step %d: choose who they must talk to.", (S32)i + 1);
            if (on == "touch" && trimmed(t["object"].asString()).empty()) return llformat("Step %d: type the name of the object they must touch.", (S32)i + 1);
            // [review] Ready is pressed in a party's Journal; a solo player has no Ready (the brain sends party null for them,
            // HUD_MESSAGES_V1.md §2.1), so a Ready step only works when the adventure needs a party of two or more
            if (on == "ready" && pk["adventure"]["party"]["minSize"].asInteger() < 2)
                return llformat("Step %d is done when everyone presses Ready, which only a party has. Choose reach a place, talk to someone or touch an object.", (S32)i + 1);
        }
        return std::string();
    }
    case W_CAST:
    {
        const LLSD& cast = pk["cast"];
        for (S32 i = 0; cast.isArray() && i < (S32)cast.size(); ++i)
        {
            const LLSD& c = cast[i];
            const std::string f = c["first"].asString(), l = c["last"].asString();
            if (!npcName(f) || !npcName(l)) return llformat("Character %d: a first and a last name, one word each (letters, digits, ' or -).", i + 1);
            if (f == "New" && l == "Character") return llformat("Character %d: give them their own name.", i + 1);
            const std::string role = trimmed(c["role"].asString());
            if (role.empty() || role == STARTER_ROLE) return llformat("Character %d (%s %s): say who they are.", i + 1, f.c_str(), l.c_str());
        }
        return std::string();
    }
    case W_PLACES:
    {
        for (const char* kind : { "spots", "areas", "paths" })
            for (const LLSD& it : llsd::inArray(pk["places"][kind]))
                if (!placeIsPlaced(kind, it)) return "Put \"" + cap(placeName(it), 40) + "\" in the world (stand there and press Use my position).";
        return std::string();
    }
    case W_REWARDS:
    {
        const LLSD& en = pk["endings"];
        for (S32 i = 0; en.isArray() && i < (S32)en.size(); ++i)
        {
            if (trimmed(en[i]["title"].asString()).empty()) return llformat("Ending %d: give it a title.", i + 1);
            const std::string tx = trimmed(en[i]["text"].asString());
            if (tx.empty() || tx == STARTER_ENDING) return "Write what the players read at the end (\"" + cap(en[i]["title"].asString(), 40) + "\").";
        }
        return std::string();
    }
    case W_LOOK:
    {
        LLColor4 a, b;
        const LLSD& look = pk["adventure"]["look"];
        const std::string ac = look["accent"].asString(), pc = look["panel"].asString();
        // the brain wants both colours or neither (wolfadventure_lib.php WaValidator::look), each as #rrggbb
        if (!ac.empty() && !WolfAdvStudio::parseColour(ac, a)) return "The accent colour must be # and six digits or letters a-f, e.g. #f4a340.";
        if (!pc.empty() && !WolfAdvStudio::parseColour(pc, b)) return "The panel colour must be # and six digits or letters a-f, e.g. #2a1d12.";
        if (ac.empty() != pc.empty()) return ac.empty() ? "Set the accent colour too, or clear the panel colour." : "Set the panel colour too, or clear the accent colour.";
        if (WolfAdvStudio::parseColour(look["accent"].asString(), a) && WolfAdvStudio::parseColour(look["panel"].asString(), b)
            && WolfAdvStudio::contrast(a, b) < 4.5f)
            return "The accent colour is hard to read on the panel colour: pick a lighter or darker one, or one of the colour sets.";
        return std::string();
    }
    default:
        return std::string();
    }
}

std::vector<bool> WolfAdventureStudioView::wizardChecks() const
{
    std::vector<bool> done((size_t)W_COUNT, false);
    if (mOpenId.empty()) return done;
    done[W_KIND] = true;
    const bool loaded = !mDraftJson.empty();
    if (const LLSD* a = listed(mOpenId))
    {
        for (const LLSD& rg : llsd::inArray((*a)["regions"])) done[W_WHERE] = done[W_WHERE] || rg["state"].asString() == "enabled";
        done[W_PUBLISH] = (*a)["status"].asString() == "published";
    }
    if (loaded)
    {
        done[W_STORY] = wizCheck(W_STORY).empty();
        done[W_CAST] = wizCheck(W_CAST).empty();
        done[W_PLACES] = wizCheck(W_PLACES).empty();
        done[W_REWARDS] = wizCheck(W_REWARDS).empty();
        const LLSD& ad = std::as_const(mDraftPack)["adventure"];
        done[W_LOOK] = wizCheck(W_LOOK).empty() && (!ad["cover"].asString().empty() || !ad["look"]["logo"].asString().empty()
                                                     || !ad["look"]["accent"].asString().empty() || ad["look"]["titleCard"].isMap());
    }
    done[W_TEST] = mWizTested;
    return done;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the brain: starter stories and making the adventure
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::loadTemplates()
{
    if (mTemplatesLoaded || mTemplatesLoading) return;
    mTemplatesLoading = true;
    mTemplatesError.clear();
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // AUDIT_2026-10-17.md §5.2: templates {} -> {templates:[{id, title, genre, summary, purpose, preset, beats}]}
    WolfAdventure::api("templates", LLSD::emptyMap(), [handle](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mTemplatesLoading = false;
        if (!ok || !reply["templates"].isArray())
        {
            self->mTemplatesError = ok ? std::string("The starter stories did not come back.") : error;
            return;
        }
        self->mTemplates = reply["templates"];
        self->mTemplatesLoaded = true;
    });
}

void WolfAdventureStudioView::wizCreate()
{
    if (mBusy > 0) return;
    const std::string title = cap(trimmed(mNewTitle), 64);
    std::string purpose = "adventure";
    for (const LLSD& t : llsd::inArray(mTemplates)) if (t["id"].asString() == mWizTemplate && !t["purpose"].asString().empty()) purpose = t["purpose"].asString();
    static const char* RATINGS[3] = { "general", "moderate", "adult" };
    LLSD body;
    body["title"] = title;
    body["purpose"] = purpose;
    // ADVENTURE_PACK_V1.md §2: an education pack must be rated general
    body["rating"] = purpose == "education" ? std::string("general") : std::string(RATINGS[llclamp(mWizRating, 0, 2)]);
    if (mWizTemplate != "~empty" && mWizTemplate != "~book") body["template"] = mWizTemplate;
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        body["region"] = rgn->getRegionID().asString();
        // the template's places are laid out round where the author stands (AUDIT §5.2 `here`), region metres
        const LLVector3 pos = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        if (std::isfinite(pos.mV[VX]) && std::isfinite(pos.mV[VY]) && std::isfinite(pos.mV[VZ]))
        {
            LLSD here = LLSD::emptyArray();
            here.append((F64)pos.mV[VX]);
            here.append((F64)pos.mV[VY]);
            here.append((F64)pos.mV[VZ]);
            body["here"] = here;
        }
    }
    const std::string kind = mWizTemplate;
    // BRAIN_API_V1.md §5.1 create, with AUDIT §5.2's template, rating, region and here
    call("create", body, [title, kind](WolfAdventureStudioView* self, const LLSD& reply)
    {
        const std::string id = reply["adventure"]["id"].asString();
        if (id.empty())
        {
            self->setStatus("The adventure service did not say which adventure it made - reopen the Studio.", true);
            return;
        }
        self->mNewTitle.clear();
        self->mWizTemplate.clear();
        self->loadList();
        self->openAdventure(id);
        if (kind == "~book")
        {
            self->mPage = SP_BOOK;
            self->loadJobs();
            self->setStatus("Made \"" + title + "\". Now give it a book: choose a text file or paste the text.", false);
            return;
        }
        self->mPage = SP_WIZARD;
        self->wizGo(W_WHERE);
        self->setStatus("Made \"" + title + "\" as a draft - nothing is live until you publish. Next: where it happens.", false);
    }, "Making \"" + title + "\"...");
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// editing helpers the steps use
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::setDoneWhen(const std::string& bp, const std::string& on)
{
    const LLSD& b = WolfAdvStudio::sdAt(mDraftPack, bp);
    if (!b.isMap()) { setStatus("That step has changed or gone - look again.", true); return; }
    const S32 ti = doneTrigger(b);
    const std::string seen = mDraftJson;
    editDoc([&](boost::json::value& root)
    {
        boost::json::value* bv = WolfAdvStudio::jAt(root, bp, false);
        if (!bv || !bv->is_object()) return false;
        boost::json::value& tl = bv->as_object()["triggers"];
        if (!tl.is_array()) tl = boost::json::array();
        boost::json::array& ta = tl.as_array();
        if (ti < 0 || ti >= (S32)ta.size() || !ta[ti].is_object())
        {
            // no way on yet: one that ends the story with the first ending (endings minItems 1)
            boost::json::object t{ { "id", WolfAdvStudio::uniqueId("trigger", seen) }, { "on", on } };
            const std::string ending = std::as_const(mDraftPack)["endings"][0]["id"].asString();
            if (!ending.empty()) t["do"] = boost::json::array{ boost::json::object{ { "end", ending } } };
            ta.push_back(t);
            return true;
        }
        // ADVENTURE_PACK_V1.md §11.1: keep the trigger's id and what it does; drop the fields only the old type used
        boost::json::object& t = ta[ti].as_object();
        for (const char* k : { "area", "cast", "object", "reply", "intent", "decision", "option", "item", "qty", "sec", "hours", "check", "dc", "result", "quiz", "passed", "scene", "label" })
            t.erase(k);
        t["on"] = on;
        return true;
    });
}

namespace
{
    /** Every reference to beat `id` under `v` (a trigger's or option's `next`, a `goto {beat}` step) is pointed where the removed
     *  beat led instead (ADVENTURE_PACK_V1.md §11: `next`, effects `goto` / `end`): to beat `to_beat`, or - when it ended the
     *  story - to the ending `to_end`. Type-checked all the way (a malformed draft is skipped, never a crash). */
    void repoint(boost::json::value& v, const std::string& id, const std::string& to_beat, const std::string& to_end)
    {
        if (v.is_array()) { for (auto& x : v.as_array()) repoint(x, id, to_beat, to_end); return; }
        if (!v.is_object()) return;
        boost::json::object& o = v.as_object();
        if (o.contains("next") && o.at("next").is_string() && o.at("next").as_string() == id)
        {
            if (!to_beat.empty()) o["next"] = to_beat;
            else
            {
                o.erase("next");
                boost::json::value& d = o["do"];
                if (!d.is_array()) d = boost::json::array();
                d.as_array().push_back(boost::json::object{ { "end", to_end } });
            }
        }
        if (boost::json::value* dv = o.if_contains("do"))
            if (dv->is_array())
                for (auto& e : dv->as_array())
                {
                    if (!e.is_object()) continue;
                    boost::json::value* g = e.as_object().if_contains("goto");
                    if (!g || !g->is_object()) continue;
                    boost::json::value* gb = g->as_object().if_contains("beat");
                    if (!gb || !gb->is_string() || gb->as_string() != id) continue;
                    if (!to_beat.empty()) *gb = to_beat;
                    else e = boost::json::object{ { "end", to_end } };
                }
        for (auto& kv : o) if (kv.key() != "next") repoint(kv.value(), id, to_beat, to_end);
    }
}

void WolfAdventureStudioView::removeStoryBeat(const std::string& bp)
{
    const LLSD& b = WolfAdvStudio::sdAt(mDraftPack, bp);
    if (!b.isMap()) return;
    if (beatsInOrder(mDraftPack).size() <= 1) { setStatus("A story needs at least one step.", true); return; }
    // a scene holds at least one beat (adventure_pack_v1.schema.json scene.beats minItems 1)
    const size_t cut = bp.rfind('/');
    const LLSD& siblings = WolfAdvStudio::sdAt(mDraftPack, bp.substr(0, cut));
    if (!siblings.isArray() || siblings.size() <= 1)
    {
        setStatus("This is the only step of its scene: remove the whole scene on the Outline page instead (Open the full Outline page).", true);
        return;
    }
    const std::string id = b["id"].asString();
    // where this beat led (its done trigger): a beat (next, or a goto step), else the ending it ran, else the first ending
    std::string to_beat, to_end;
    const S32 ti = doneTrigger(b);
    if (ti >= 0)
    {
        const LLSD& t = b["triggers"][ti];
        if (t["next"].isString()) to_beat = t["next"].asString();
        for (const LLSD& e : llsd::inArray(t["do"]))
        {
            if (to_beat.empty() && e["goto"]["beat"].isString()) to_beat = e["goto"]["beat"].asString();
            if (to_end.empty() && e["end"].isString()) to_end = e["end"].asString();
        }
    }
    if (to_beat == id) to_beat.clear();
    if (to_beat.empty() && to_end.empty()) to_end = std::as_const(mDraftPack)["endings"][0]["id"].asString();
    const S32 k = atoi(bp.substr(cut + 1).c_str());
    const bool ok = editDoc([&](boost::json::value& root)
    {
        boost::json::value* arr = WolfAdvStudio::jAt(root, bp.substr(0, cut), false);
        if (!arr || !arr->is_array() || k < 0 || k >= (S32)arr->as_array().size()) return false;
        arr->as_array().erase(arr->as_array().begin() + k);
        // every way into it now leads where it led (the chain closes over the gap)
        if (boost::json::value* acts = WolfAdvStudio::jAt(root, "/acts", false)) repoint(*acts, id, to_beat, to_end);
        // the story's start follows on too: where it led, else the first beat left
        boost::json::value* st = WolfAdvStudio::jAt(root, "/adventure/start/beat", false);
        if (st && st->is_string() && st->as_string() == id)
        {
            std::string first;
            if (const boost::json::value* fb = WolfAdvStudio::jAt(root, "/acts/0/scenes/0/beats/0/id", false)) if (fb->is_string()) first = std::string(fb->as_string().c_str());
            *st = !to_beat.empty() ? to_beat : first;
        }
        return true;
    });
    if (ok) setStatus("Removed that step; the steps that led to it now lead where it led.", false);
}

void WolfAdventureStudioView::placeAreaHere(const std::string& id)
{
    LLViewerRegion* rgn = gAgent.getRegion();
    const LLSD& pl = placeOf("areas", id);
    if (!pl.isMap()) { setStatus("That place is no longer in the draft.", true); return; }
    if (!rgn || (!pl["region"].asString().empty() && pl["region"].asString() != rgn->getRegionID().asString()))
    {
        setStatus("This place is on another region: teleport there first.", true);
        return;
    }
    const LLVector3 p = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    if (!std::isfinite(p.mV[VX]) || !std::isfinite(p.mV[VY])) return;
    const F32 w = rgn->getWidth();
    // a circle round where the author stands (adventure_pack_v1.schema.json areas shape circle: centre vec2, radius 0 < r <= 512)
    F64 radius = pl["shape"]["circle"]["radius"].asReal();
    if (!(radius > 0.0 && radius <= 512.0)) radius = 5.0;
    auto d2 = [](F32 v) { return std::round((double)v * 100.0) / 100.0; };
    boost::json::object o;
    o["shape"] = boost::json::object{ { "circle", boost::json::object{ { "centre", boost::json::array{ d2(llclamp(p.mV[VX], 0.f, w - 0.01f)), d2(llclamp(p.mV[VY], 0.f, w - 0.01f)) } },
                                                                       { "radius", radius } } } };
    o["region"] = rgn->getRegionID().asString();
    placed("areas", id, o, llformat("\"%s\" is a circle %.0f m across, round where you stand.", cap(placeName(pl), 40).c_str(), radius * 2.0));
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the page
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::pageWizard(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    if (!mOpenId.empty() && !openIsOwner())
        return para(l, r, top, "Only the adventure's owner can change it. As a host or teacher, use Publish to see it and Run to run it.", p.mDim);
    if (mOpenId.empty() && mWizStep != W_KIND) mWizStep = W_KIND;
    if (!mOpenId.empty() && mDraftJson.empty()) return para(l, r, top, "Opening the draft...", p.mDim);
    // came back to an adventure with no step remembered: the first step not done yet
    if (mWizAuto && !mDraftJson.empty())
    {
        mWizAuto = false;
        const std::vector<bool> done = wizardChecks();
        S32 first = W_PUBLISH;
        for (S32 i = W_WHERE; i < W_COUNT; ++i) if (!done[i] && i != W_LOOK) { first = i; break; }
        mWizStep = (EWizStep)first;
    }
    if (mWizStep == W_TEST && !mTestSession.empty() && !mWizTested) { mWizTested = true; rememberWizStep(); }

    F32 y = conflictBanner(l, r, top, p);
    // ---- where we are: the nine steps as numbered segments, done ones ticked (an icon AND the number, never colour alone) ----
    {
        const std::vector<bool> done = wizardChecks();
        const F32 gap = 4.f, sw = (r - l - gap * (W_COUNT - 1)) / (F32)W_COUNT, sh = 24.f;
        for (S32 i = 0; i < W_COUNT; ++i)
        {
            const F32 x0 = l + i * (sw + gap);
            const bool cur = i == (S32)mWizStep;
            const bool can = i == W_KIND || !mOpenId.empty();
            const S32 idx = act([this, i]() { wizGo(i); });
            // the number, and a tick for a done step (the tooltip says "done" in words)
            const std::string lab = done[i] ? llformat("%d \xE2\x9C\x93", i + 1) : llformat("%d", i + 1);
            btn(x0, y - sh, x0 + sw, y, lab, SH_E, idx, llformat("Step %d: ", i + 1) + STEPS[i].mTitle + (done[i] ? " (done)" : ""), p, can, cur ? BTN_ACTIVE : 0);
        }
        y -= sh + 10.f;
    }
    const Step& st = STEPS[mWizStep];
    textL(small, llformat("STEP %d OF %d", (S32)mWizStep + 1, (S32)W_COUNT), l, y, p.mDim);
    y -= slh + 2.f;
    textL(display(20.f), st.mTitle, l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(20.f)->getLineHeight() + 4.f;
    y = para(l, r, y, st.mWhy, p.mText) - 6.f;
    // the example, on its own plate
    {
        const std::vector<std::string> lines = wrap(small, st.mExample, r - l - 24.f, 4);
        const F32 eh = 10.f + slh + (F32)lines.size() * slh + 8.f;
        card(l, y - eh, r, y, p, 4.f);
        textL(bold, "EXAMPLE", l + 12.f, y - 8.f, p.mAccent);
        F32 ey = y - 8.f - slh;
        for (const std::string& ln : lines) { textL(small, ln, l + 12.f, ey, p.mText); ey -= slh; }
        y -= eh + 10.f;
    }
    // the step's check, when Next found something to fix: at the top, in view, until it is right (it re-checks as you type)
    if (!mWizError.empty())
    {
        const std::string still = wizCheck(mWizStep);
        mWizError = still;   // fixed = gone; a different problem = the new words
    }
    if (!mWizError.empty())
    {
        const std::vector<std::string> lines = wrap(LLFontGL::getFontSansSerif(), mWizError, r - l - 40.f, 4);
        const F32 eh = 12.f + (F32)lines.size() * (F32)LLFontGL::getFontSansSerif()->getLineHeight() + 10.f;
        rect(l, y - eh, r, y, LLColor4(0.45f, 0.08f, 0.08f, 0.55f));
        chamferEdge(l, y - eh, r, y, 3.f, 2.f, DANGER_INK);
        // a warning triangle AND the words
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(WARN_INK.mV);
        gGL.vertex2f(l + 10.f, y - 22.f); gGL.vertex2f(l + 24.f, y - 22.f); gGL.vertex2f(l + 17.f, y - 9.f);
        gGL.end();
        F32 ey = y - 8.f;
        for (const std::string& ln : lines) { textL(LLFontGL::getFontSansSerif(), ln, l + 32.f, ey, p.mText); ey -= (F32)LLFontGL::getFontSansSerif()->getLineHeight(); }
        y -= eh + 10.f;
    }
    // ---- the step ----
    switch (mWizStep)
    {
    case W_KIND: y = wizKind(l, r, y, p); break;
    case W_WHERE: y = wizWhere(l, r, y, p); break;
    case W_STORY: y = wizStory(l, r, y, p); break;
    case W_CAST: y = wizCast(l, r, y, p); break;
    case W_PLACES: y = wizPlaces(l, r, y, p); break;
    case W_REWARDS: y = wizRewards(l, r, y, p); break;
    case W_LOOK: y = wizLook(l, r, y, p); break;
    case W_TEST: y = pageTest(l, r, y, p); break;
    case W_PUBLISH: y = wizPublish(l, r, y, p); break;
    default: break;
    }
    // ---- Back, Skip to the full page, Next (the primary action where the eye ends: bottom right) ----
    y -= 12.f;
    rect(l, y, r, y + 1.f, fade(p.mAccent, 0.3f));
    y -= 10.f;
    {
        const bool first = mWizStep == W_KIND;
        const S32 ib = act([this]() { wizGo((S32)mWizStep - 1); });
        btn(l, y - 32.f, l + 110.f, y, "Back", SH_E, ib, "The step before", p, !first);
        // the expert page for this step (Paul: "The advanced pages stay available for experts")
        static const S32 FULL[W_COUNT] = { SP_HOME, SP_PUBLISH, SP_OUTLINE, SP_CASTED, SP_PLACES, SP_LORE, SP_OUTLINE, SP_TEST, SP_PUBLISH };
        static const char* FULL_NAME[W_COUNT] = { "Home", "Publish", "Outline", "Cast", "Places", "Lore", "Outline", "Test Play", "Publish" };
        const S32 full = FULL[mWizStep];
        const std::string fl = std::string("Open the full ") + FULL_NAME[mWizStep] + " page";
        const S32 ifl = act([this, full]()
        {
            mPage = (EPage)full;
            if (full == SP_LORE) mLoreTab = 3;   // Lore > Rewards
            mScroll[mPage] = 0.f;
        });
        const F32 fw = btnW(fl);
        btn(l + 118.f, y - 32.f, l + 118.f + fw, y, fl, SH_E, ifl, "Every setting for this, on the expert page (Start here stays where you left it)", p,
            !mOpenId.empty() || full == SP_HOME);
        if (mWizStep != W_PUBLISH)
        {
            const bool making = mWizStep == W_KIND && mOpenId.empty();
            const std::string nl = making ? "Make it" : "Next";
            const S32 in = act([this]() { wizNext(); });
            btn(r - 140.f, y - 32.f, r, y, nl, SH_E, in, making ? "Make the adventure as a draft (nothing is live until you publish)" : "Check this step and go on",
                p, mBusy == 0, BTN_PRIMARY);
        }
        y -= 44.f;
    }
    return y;
}

// ── 1. what kind ──────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizKind(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (!mOpenId.empty())
    {
        // made already: its name and rating can still change here; the kind is the story it started from
        y = head(l, r, y, "Your adventure", p);
        y = fText(l, r, y, "Name", "/adventure/title", std::as_const(mDraftPack)["adventure"]["title"], 0, 64, 's', false, "The adventure's name (1 to 64 characters)", p);
        y = fEnum(l, r, y, "Rating", "/adventure/rating", { "general", "moderate", "adult" }, std::as_const(mDraftPack)["adventure"]["rating"], "general", 0, false, p);
        y = para(l, r, y, "General: fine for everyone. Moderate: some violence or grown-up themes. Adult: adults only. It must not be above your region's own rating.", p.mDim, true) - 8.f;
        const S32 ix = act([this]() { closeOpenAdventure(); wizGo(W_KIND); });
        btn(l, y - ROW, l + btnW("Make a different adventure"), y, "Make a different adventure", SH_E, ix, "Start another adventure from the beginning (this one stays as it is)", p);
        return y - ROW - 8.f;
    }
    // ---- the kinds: the brain's starter stories (AUDIT §5.2), an empty one, and a book ----
    if (!mTemplatesLoaded && !mTemplatesLoading) loadTemplates();
    y = head(l, r, y, "Choose one", p);
    struct Choice { std::string mId, mTitle, mLine; };
    std::vector<Choice> choices;
    for (const LLSD& t : llsd::inArray(mTemplates))
    {
        const std::string id = t["id"].asString();
        if (id.empty()) continue;
        choices.push_back({ id, cap(t["title"].asString().empty() ? id : t["title"].asString(), 48), cap(t["summary"].asString(), 160) });
    }
    if (mTemplatesLoading && choices.empty()) y = para(l, r, y, "Loading the starter stories...", p.mDim, true) - 6.f;
    if (!mTemplatesError.empty())
    {
        y = para(l, r, y, "The starter stories could not be loaded: " + mTemplatesError + " You can try again, or start with an empty adventure.", DANGER_INK, true) - 4.f;
        const S32 ir = act([this]() { mTemplatesLoaded = false; loadTemplates(); });
        btn(l, y - ROW, l + 120.f, y, "Try again", SH_E, ir, "Load the starter stories again", p, !mTemplatesLoading);
        y -= ROW + 8.f;
    }
    choices.push_back({ "~book", "Give it a book", "Turn a public-domain book or your own outline into a draft (costs a little, with a quote first)." });
    choices.push_back({ "~empty", "Start empty", "For experts: an empty adventure you build on the full pages." });
    const F32 gap = 8.f, cw = (r - l - gap) * 0.5f, ch = 64.f;
    for (size_t i = 0; i < choices.size(); ++i)
    {
        const S32 col = (S32)(i % 2), row = (S32)(i / 2);
        const F32 cl = l + col * (cw + gap), ct = y - row * (ch + gap);
        const bool on = mWizTemplate == choices[i].mId;
        const std::string cid = choices[i].mId;
        const S32 idx = act([this, cid]() { mWizTemplate = cid; if (!mWizError.empty()) mWizError = wizCheck(W_KIND); });
        button(cl, ct - ch, cl + cw, ct, std::string(), p, flagsFor(SH_E, idx, true) | (on ? BTN_ACTIVE : 0));
        textL(bold, choices[i].mTitle + (on ? " - chosen" : ""), cl + 10.f, ct - 8.f, on ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
        F32 ly = ct - 8.f - slh - 2.f;
        for (const std::string& s : wrap(small, choices[i].mLine, cw - 20.f, 2)) { textL(small, s, cl + 10.f, ly, p.mDim); ly -= slh; }
        hit(cl, ct - ch, cl + cw, ct, SH_E, idx, choices[i].mTitle + ": " + choices[i].mLine);
    }
    y -= (F32)((choices.size() + 1) / 2) * (ch + gap) + 6.f;
    // ---- the name ----
    y = head(l, r, y, "Its name", p);
    field(l, y - 30.f, r, y, mNewTitle, "Type a name, e.g. The Lost Gold of Wolf Creek", F_TITLE, p);
    y -= 34.f;
    if (!mTitleError.empty()) { textL(bold, mTitleError, l, y, DANGER_INK, LLFontGL::LEFT, LLFontGL::TOP, r - l); y -= slh + 4.f; }
    // ---- the rating ----
    y = head(l, r, y, "Who it is for", p);
    {
        static const char* NAMES[3] = { "General", "Moderate", "Adult" };
        bool edu = false;
        for (const LLSD& t : llsd::inArray(mTemplates)) if (t["id"].asString() == mWizTemplate) edu = t["purpose"].asString() == "education";
        F32 x = l;
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 bw = btnW(NAMES[i], 100.f);
            const bool on = edu ? i == 0 : mWizRating == i;
            const S32 idx = act([this, i]() { mWizRating = i; });
            btn(x, y - ROW, x + bw, y, NAMES[i], SH_E, idx, std::string("Rated ") + NAMES[i] + (on ? " (chosen)" : ""), p, !edu || i == 0, on ? BTN_ACTIVE : 0);
            x += bw + 6.f;
        }
        y -= ROW + 4.f;
        y = para(l, r, y, edu ? "A class adventure is always General." : "General: fine for everyone. Moderate: some violence or grown-up themes. Adult: adults only, on Adult regions.",
                 p.mDim, true) - 4.f;
    }
    return y;
}

// ── 2. where ──────────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizWhere(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = head(l, r, top, "Its regions", p);
    const LLSD* a = listed(mOpenId);
    const LLSD& pack_regions = std::as_const(mDraftPack)["adventure"]["regions"];
    if (!pack_regions.isArray() || pack_regions.size() == 0) y = para(l, r, y, "None yet.", p.mDim, true) - 4.f;
    for (const LLSD& pr : llsd::inArray(pack_regions))
    {
        const std::string rid = pr["region"].asString();
        std::string state;
        if (a) for (const LLSD& rg : llsd::inArray((*a)["regions"])) if (rg["region"].asString() == rid) state = rg["state"].asString();
        // the state in words, with what it means for the author (colour never alone)
        std::string words = state == "enabled" ? "Ready: it can run here."
                          : state == "requested" ? "Waiting for the region's holder to say yes. You can carry on making it; Test Play needs the yes."
                          : state == "refused" ? "The holder said no. Choose another region."
                          : "Not asked yet.";
        textL(bold, cap(pr["name"].asString().empty() ? rid : pr["name"].asString(), 64), l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 130.f);
        y -= slh + 1.f;
        y = para(l + 10.f, r, y, words, state == "enabled" ? OK_INK : state == "refused" ? DANGER_INK : p.mDim, true) - 2.f;
        if (state.empty() || state == "disabled")
        {
            const S32 iq = act([this, rid]() { requestRegion(rid); });
            btn(l + 10.f, y - ROW, l + 10.f + btnW("Ask for it now"), y, "Ask for it now", SH_E, iq, "On your land it is ready at once; otherwise its holder is asked", p, mBusy == 0);
            y -= ROW + 4.f;
        }
        y -= 6.f;
    }
    LLViewerRegion* rgn = gAgent.getRegion();
    if (rgn)
    {
        const std::string rid = rgn->getRegionID().asString(), rname = rgn->getName();
        bool there = false;
        for (const LLSD& pr : llsd::inArray(pack_regions)) there = there || pr["region"].asString() == rid;
        if (!there)
        {
            const S32 iu = act([this, rid, rname]()
            {
                docAppend(0, "/adventure/regions", boost::json::object{ { "region", rid }, { "name", cap(rname, 64) } });
                requestRegion(rid);
            });
            const std::string lab = "Use this region (" + cap(rname, 32) + ")";
            btn(l, y - 32.f, l + btnW(lab, 220.f), y, lab, SH_E, iu, "Add the region you are standing on, and ask for it (on your land it is ready at once)", p, mBusy == 0, BTN_PRIMARY);
            y -= 40.f;
        }
        else y = para(l, r, y, "You are standing on " + rname + ", one of its regions.", p.mDim, true) - 4.f;
    }
    y = para(l, r, y, "To add another region later, stand on it and come back to this step.", p.mDim, true) - 4.f;
    return y;
}

// ── 3. the story ──────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizStory(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    F32 y = top;
    const auto beats = beatsInOrder(mDraftPack);
    // [WOLF ADVENTURE AUDIT 2026-10-17] the acts' and scenes' titles above their steps when the story has more than one of
    // either (or one of them has a problem), so a "title of Act 2" problem is fixed here, not only on the full pages
    const LLSD& acts = std::as_const(mDraftPack)["acts"];
    S32 nscenes = 0;
    for (const LLSD& a : llsd::inArray(acts)) nscenes += a["scenes"].isArray() ? (S32)a["scenes"].size() : 0;
    const bool many = acts.size() > 1 || nscenes > 1;
    S32 last_a = -1, last_s = -1;
    for (size_t i = 0; i < beats.size(); ++i)
    {
        const std::string bp = beats[i].first;
        const LLSD& b = *beats[i].second;
        {
            S32 ai = -1, si = -1, bi = -1;
            sscanf(bp.c_str(), "/acts/%d/scenes/%d/beats/%d", &ai, &si, &bi);
            const std::string ap = llformat("/acts/%d", ai), sp = llformat("/acts/%d/scenes/%d", ai, si);
            if (ai != last_a && (many || problemsUnder(ap + "/title") > 0))
            {
                y = fText(l, r, y, llformat("Act %d's title", ai + 1), ap + "/title", acts[ai]["title"], 0, 64, 's', false, "The act's name, e.g. The Gold Run (players see it as a chapter card)", p);
            }
            if ((ai != last_a || si != last_s) && (many || problemsUnder(sp + "/title") > 0))
            {
                y = fText(l, r, y, llformat("Scene %d's title", si + 1), sp + "/title", acts[ai]["scenes"][si]["title"], 0, 64, 's', false, "Where it happens, e.g. Main Street", p);
            }
            last_a = ai;
            last_s = si;
        }
        // the step's plate head
        rect(l, y - 26.f, r, y, fade(p.mCard, 0.9f));
        textL(bold, llformat("STEP %d", (S32)i + 1), l + 8.f, y - 13.f, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER);
        {
            const S32 ix = act([this, bp]() { removeStoryBeat(bp); });
            btn(r - btnW("Remove this step"), y - 24.f, r - 2.f, y - 2.f, "Remove this step", SH_E, ix, "Take this step out; the step before it leads to the one after", p,
                beats.size() > 1, BTN_DANGER);
        }
        y -= 32.f;
        y = fText(l, r, y, "The players must...", bp + "/objective", b["objective"], 0, 240, 's', false,
                  "One thing to do, e.g. Find the old well (it shows in their Journal)", p);
        // when it is done: the beat's way on, as four plain choices (ADVENTURE_PACK_V1.md §11.1 arrive / talk / touch / ready)
        const S32 ti = doneTrigger(b);
        const LLSD& t = ti >= 0 ? b["triggers"][ti] : LLSD();
        const std::string on = t["on"].asString();
        // [review] a step that ends with the players' choice (decisions, no trigger leading on) is left as it is: offering
        // "reach a place" here would add a way round the choice
        if (ti < 0 && b["decisions"].isArray() && b["decisions"].size() > 0)
        {
            y = para(l + 150.f, r, y, "This step ends with a choice the players make. Change the choice under Advanced editing > Story (Outline, select the step, Open).",
                     p.mDim, true) - 10.f;
            continue;
        }
        {
            textL(small, "It is done when they...", l, y - 6.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, 146.f);
            const F32 cl = l + 150.f;
            struct Kind { const char* mOn; const char* mWords; };
            static const Kind KINDS[4] = { { "arrive", "reach a place" }, { "talk", "talk to someone" }, { "touch", "touch an object" }, { "ready", "are all ready" } };
            F32 x = cl, yy = y;
            for (const Kind& k : KINDS)
            {
                const F32 bw = btnW(k.mWords);
                if (x > cl && x + bw > r) { x = cl; yy -= ROW + 4.f; }
                const bool cur = on == k.mOn;
                const std::string kon = k.mOn;
                const S32 idx = act([this, bp, kon]() { setDoneWhen(bp, kon); });
                btn(x, yy - ROW, x + bw, yy, k.mWords, SH_E, idx, std::string("Done when they ") + k.mWords + (cur ? " (chosen)" : ""), p, true, cur ? BTN_ACTIVE : 0);
                x += bw + 4.f;
            }
            y = yy - ROW - 6.f;
            // a known type that is none of the four (an expert's trigger): said in words, edited under Advanced editing > Story
            if (!on.empty() && on != "arrive" && on != "talk" && on != "touch" && on != "ready")
                y = para(cl, r, y, "Set under Advanced editing > Story (\"" + on + "\"); choose one of these to replace it.", p.mDim, true) - 4.f;
        }
        if (ti >= 0)
        {
            const std::string tp = llformat("%s/triggers/%d", bp.c_str(), ti);
            if (on == "arrive")
            {
                y = fRef(l, r, y, "Which place", tp + "/area", "area", t["area"], 0, false, p);
                const S32 ia = act([this, tp, i]()
                {
                    const std::string id = newPackId("place");
                    LLViewerRegion* rgn = gAgent.getRegion();
                    const std::string region = rgn ? rgn->getRegionID().asString() : std::as_const(mDraftPack)["adventure"]["regions"][0]["region"].asString();
                    docAppend(0, "/places/areas", boost::json::object{ { "id", id }, { "label", llformat("Place for step %d", (S32)i + 1) }, { "region", region },
                        { "shape", boost::json::object{ { "circle", boost::json::object{ { "centre", boost::json::array{ -1, -1 } }, { "radius", 5 } } } } } });
                    docSet(0, tp + "/area", boost::json::value(id));
                    setStatus("Added a new place. Name it and put it in the world in step 5.", false);
                });
                btn(l + 150.f, y - ROW, l + 150.f + btnW("A new place"), y, "A new place", SH_E, ia, "Make a new place for this step (you put it in the world in step 5)", p);
                y -= ROW + 6.f;
            }
            else if (on == "talk")
            {
                y = fRef(l, r, y, "Who", tp + "/cast", "cast", t["cast"], 0, false, p);
                const S32 ic = act([this, tp]()
                {
                    const std::string id = newPackId("character");
                    docAppend(0, "/cast", boost::json::object{ { "id", id }, { "first", "New" }, { "last", "Character" }, { "role", STARTER_ROLE } });
                    docSet(0, tp + "/cast", boost::json::value(id));
                    setStatus("Added a new character. Name them and write what they say in step 4.", false);
                });
                btn(l + 150.f, y - ROW, l + 150.f + btnW("A new character"), y, "A new character", SH_E, ic, "Make a new character for this step (you name them in step 4)", p);
                y -= ROW + 6.f;
            }
            else if (on == "touch")
            {
                y = fText(l, r, y, "The object's name", tp + "/object", t["object"], 0, 64, 's', false,
                          "The name of the object in the world, exactly as it is named (or its description 'touch: <name>')", p);
                // [WOLF ADVENTURE AUDIT 2026-10-17] one click from the world: the object the author has selected (its name is known
                // once selected: LLSelectNode::mName, filled by the object properties the selection asks for)
                const S32 is = act([this, tp]()
                {
                    LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode();
                    if (!node || !node->mValid || node->mName.empty())
                    {
                        setStatus("Select the object in the world first (right-click it > Edit), then press Use the selected object.", true);
                        return;
                    }
                    docSet(0, tp + "/object", boost::json::value(cap(node->mName, 64)));
                    const bool mine = node->mPermissions && node->mPermissions->getOwner() == gAgentID;
                    setStatus("The step is done when they touch \"" + cap(node->mName, 64) + "\"." +
                              (mine ? std::string() : std::string(" It is not yours: if its owner renames or takes it, the step cannot be done.")), !mine);
                });
                btn(l + 150.f, y - ROW, l + 150.f + btnW("Use the selected object"), y, "Use the selected object", SH_E, is,
                    "Right-click an object in the world > Edit to select it, then press this to use its name", p);
                y -= ROW + 4.f;
                y = para(l + 150.f, r, y, "Or rez an object on your land, name it (Build > Edit > Name), and type the same name here.", p.mDim, true) - 4.f;
            }
            else if (on == "ready") y = para(l + 150.f, r, y, "Players press Ready in their Journal when they have done it. Only a party has Ready, so this suits adventures for 2 or more players.", p.mDim, true) - 4.f;
        }
        // [WOLF ADVENTURE AUDIT 2026-10-17] nothing leads here (the brain's problem at the beat's id: an older Studio added acts
        // without linking them): said here, with the one-click link when the step before ends the story
        const std::string unreached = i > 0 ? problemAt(bp + "/id") : std::string();
        if (!unreached.empty())
        {
            y = para(l + 150.f, r, y, unreached, DANGER_INK, true) - 2.f;
            const std::string prevp = beats[i - 1].first;
            const LLSD& pb = *beats[i - 1].second;
            const S32 pti = doneTrigger(pb);
            bool prev_ends = false;
            if (pti >= 0) for (const LLSD& e : llsd::inArray(pb["triggers"][pti]["do"])) prev_ends = prev_ends || e.has("end");
            if (pti >= 0 && prev_ends && !pb["triggers"][pti].has("next"))
            {
                const std::string id = b["id"].asString();
                const std::string ltp = llformat("%s/triggers/%d", prevp.c_str(), pti);
                const std::string lab = llformat("Let step %d lead here", (S32)i);
                const S32 il = act([this, ltp, id, i]()
                {
                    // step i-1's way on stops ending the story and goes on to this step instead (ADVENTURE_PACK_V1.md §11: next)
                    const bool ok = editDoc([&](boost::json::value& root)
                    {
                        boost::json::value* tv = WolfAdvStudio::jAt(root, ltp, false);
                        if (!tv || !tv->is_object()) return false;
                        boost::json::object& t = tv->as_object();
                        if (boost::json::value* d = t.if_contains("do"))
                            if (d->is_array())
                            {
                                boost::json::array& da = d->as_array();
                                for (size_t k = 0; k < da.size();)
                                    if (da[k].is_object() && da[k].as_object().contains("end")) da.erase(da.begin() + (std::ptrdiff_t)k); else ++k;
                                if (da.empty()) t.erase("do");
                            }
                        t["next"] = id;
                        return true;
                    });
                    if (ok) setStatus(llformat("Step %d now leads to step %d instead of ending the story.", (S32)i, (S32)i + 1), false);
                });
                btn(l + 150.f, y - ROW, l + 150.f + btnW(lab), y, lab, SH_E, il, "The step before stops ending the story and goes on to this one", p, true, BTN_PRIMARY);
                y -= ROW + 6.f;
            }
            else y = para(l + 150.f, r, y, llformat("Make step %d go on to this one: choose its done-when, then under Advanced editing > Story set Then go to.", (S32)i), p.mDim, true) - 4.f;
        }
        // where it goes next, in words
        if (ti >= 0)
        {
            std::string next;
            if (t.has("next")) { for (size_t k = 0; k < beats.size(); ++k) if ((*beats[k].second)["id"].asString() == t["next"].asString()) next = llformat("Then step %d starts.", (S32)k + 1); }
            for (const LLSD& e : llsd::inArray(t["do"]))
            {
                if (e.has("end"))
                    for (const LLSD& en : llsd::inArray(std::as_const(mDraftPack)["endings"]))
                        if (en["id"].asString() == e["end"].asString()) next = "Then the story ends: \"" + cap(en["title"].asString(), 40) + "\" (step 6).";
            }
            if (!next.empty()) y = para(l + 150.f, r, y, next, p.mDim, true) - 4.f;
        }
        y -= 10.f;
    }
    if (beats.size() < 12)
    {
        const std::string last = beats.empty() ? std::string() : beats.back().first;
        const S32 ia = act([this, last]()
        {
            if (last.empty()) addStoryItem(0, std::string());
            else addStoryItem(2, last);
            setStatus("Added a step at the end. Write what the players must do, and when it is done.", false);
        });
        btn(l, y - 30.f, l + btnW("Add a step", 140.f), y, "Add a step", SH_E, ia, "Add a step at the end of the story (3 to 5 steps is a good size)", p);
        y -= 38.f;
    }
    return y;
}

// ── 4. characters ─────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizCast(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    const S32 n = cast.isArray() ? (S32)cast.size() : 0;
    if (n == 0) y = para(l, r, y, "No characters yet. A treasure hunt can do without; add one if a step needs someone to talk to.", p.mDim, true) - 6.f;
    const auto beats = beatsInOrder(mDraftPack);
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& c = cast[i];
        const std::string cp = llformat("/cast/%d", i);
        const std::string cid = c["id"].asString();
        rect(l, y - 26.f, r, y, fade(p.mCard, 0.9f));
        textL(bold, llformat("CHARACTER %d", i + 1), l + 8.f, y - 13.f, p.mAccent, LLFontGL::LEFT, LLFontGL::VCENTER);
        {
            const std::string nm = cap(c["first"].asString() + " " + c["last"].asString(), 40);
            const S32 ix = act([this, cp, cid, nm]()
            {
                const std::string uses = castUses(cid);
                if (!uses.empty()) { setStatus(nm + " is in the story (a step talks to them): choose someone else in step 3 first.", true); return; }
                askDelete(0, cp, nm);
            });
            btn(r - btnW("Remove"), y - 24.f, r - 2.f, y - 2.f, "Remove", SH_E, ix, "Take this character out (asks first)", p, true, BTN_DANGER);
        }
        y -= 32.f;
        y = fText(l, r, y, "First name", cp + "/first", c["first"], 0, 31, 's', false, "One word: letters, digits, ' or - (e.g. Ma)", p);
        y = fText(l, r, y, "Last name", cp + "/last", c["last"], 0, 31, 's', false, "One word: letters, digits, ' or - (e.g. Dalton)", p);
        y = fText(l, r, y, "Who they are", cp + "/role", c["role"], 0, 240, 's', false, "e.g. Runs the saloon and hears every rumour", p);
        y = fText(l, r, y, "When someone talks to them they say", cp + "/greeting", c["greeting"], 0, 500, 's', true, "Their first words, e.g. Howdy, stranger. Heard about the gold?", p);
        // the steps where a player must talk to them: what they say there (the beat's onDuty lines, ADVENTURE_PACK_V1.md §8)
        for (size_t k = 0; k < beats.size(); ++k)
        {
            const LLSD& b = *beats[k].second;
            const S32 ti = doneTrigger(b);
            if (ti < 0 || b["triggers"][ti]["on"].asString() != "talk" || b["triggers"][ti]["cast"].asString() != cid) continue;
            S32 od = -1;
            for (S32 j = 0; j < (S32)b["onDuty"].size(); ++j) if (b["onDuty"][j]["cast"].asString() == cid) od = j;
            const std::string label = llformat("At step %d they say", (S32)k + 1);
            if (od >= 0 && b["onDuty"][od]["lines"][0].isMap())
                y = fText(l, r, y, label, llformat("%s/onDuty/%d/lines/0/text", beats[k].first.c_str(), od), b["onDuty"][od]["lines"][0]["text"], 0, 500, 's', false,
                          "What they tell the player at this step (written lines: free, and used when AI is off)", p);
            else
            {
                const std::string bp = beats[k].first;
                const S32 iw = act([this, bp, cid, od]()
                {
                    const boost::json::object line{ { "text", "Write the line here." } };
                    if (od >= 0) docSet(0, llformat("%s/onDuty/%d/lines", bp.c_str(), od), boost::json::array{ line });
                    else docAppend(0, bp + "/onDuty", boost::json::object{ { "cast", cid }, { "lines", boost::json::array{ line } } });
                });
                textL(small, label, l, y - 6.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, 146.f);
                const F32 cl = l + 150.f;
                btn(cl, y - ROW, cl + btnW("Write what they say"), y, "Write what they say", SH_E, iw, "Add a line for this character at this step", p);
                y -= ROW + 6.f;
            }
        }
        y -= 10.f;
    }
    {
        const S32 ia = act([this]()
        {
            const std::string id = newPackId("character");
            docAppend(0, "/cast", boost::json::object{ { "id", id }, { "first", "New" }, { "last", "Character" }, { "role", STARTER_ROLE } });
            setStatus("Added a character. Give them a name and say who they are.", false);
        });
        btn(l, y - 30.f, l + btnW("Add a character", 150.f), y, "Add a character", SH_E, ia, "Add another character", p, n < 100);
        y -= 42.f;
    }
    // ---- written lines or AI, with the cost in words (WOLF_MIND_V1.md §7.2 estimate; ai_budget) ----
    y = head(l, r, y, "How do characters talk?", p);
    {
        const bool ai_on = std::as_const(mDraftPack)["ai"]["mode"].asString() == "ai";
        const F32 gap = 8.f, cw = (r - l - gap) * 0.5f, chh = 54.f;
        struct Mode { const char* mLabel; const char* mLine; bool mOn; const char* mMode; };
        const Mode modes[2] = { { "Written lines", "Free. They say what you wrote.", !ai_on, "none" },
                                { "AI characters", "They talk freely, in character. Paid from your credits.", ai_on, "ai" } };
        for (S32 i = 0; i < 2; ++i)
        {
            const F32 cl = l + (F32)i * (cw + gap);
            const std::string mode = modes[i].mMode;
            const S32 idx = act([this, mode]() { docSet(0, "/ai/mode", boost::json::value(mode)); });
            button(cl, y - chh, cl + cw, y, std::string(), p, flagsFor(SH_E, idx, true) | (modes[i].mOn ? BTN_ACTIVE : 0));
            textL(bold, std::string(modes[i].mLabel) + (modes[i].mOn ? " - chosen" : ""), cl + 10.f, y - 8.f, modes[i].mOn ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            textL(small, modes[i].mLine, cl + 10.f, y - 10.f - slh, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            hit(cl, y - chh, cl + cw, y, SH_E, idx, std::string(modes[i].mLabel) + ": " + modes[i].mLine);
        }
        y -= chh + 8.f;
        // the estimate from ai_budget (fields as pageAi reads them), with the BRAIN defaults when it has not loaded
        const LLSD& est = std::as_const(mBudget)["estimate"];   // read only: never make mBudget a map while it has not loaded
        const S64 turn = est.has("turnMicro") ? (S64)est["turnMicro"].asInteger() : 255;
        const S64 extra = est.has("firstTurnExtraMicro") ? (S64)est["firstTurnExtraMicro"].asInteger() : 375;
        const S32 daily = std::as_const(mDraftPack)["ai"].has("dailyCapCredits") ? std::as_const(mDraftPack)["ai"]["dailyCapCredits"].asInteger() : 100;
        // 1 credit = 10,000 micro-USD = 1 US cent (WOLF_MIND_V1.md §7.1)
        y = para(l, r, y, llformat("AI costs about %.2f credits a talk turn (100 turns: about %.0f credits, 1 credit = 1 US cent); the first turn with a character "
                                   "at a step about %.2f more. This adventure spends at most %d credits a day (change it on the AI and voice page). When that "
                                   "or your credits run out, characters use your written lines by themselves - so write them either way.",
                                   (F64)turn / 10000.0, (F64)turn * 100.0 / 10000.0, (F64)extra / 10000.0, daily), p.mDim, true) - 4.f;
    }
    return y;
}

// ── 5. places ─────────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizPlaces(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    // the in-world overlay draws every place on this region while this step shows (placesPageShown)
    WolfToolAdventurePlace* tool = WolfToolAdventurePlace::getInstance();
    if (!tool->attached()) tool->attach(getDerivedHandle<WolfAdventureStudioView>());
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string here = rgn ? rgn->getRegionID().asString() : std::string();
    F32 y = top;
    if (tool->mode() != WolfToolAdventurePlace::M_NONE)
    {
        y = para(l, r, y, tool->hint(), WARN_INK, true);
        const S32 ic = act([tool]() { tool->cancel(); });
        btn(l, y - ROW, l + 140.f, y, "Stop placing", SH_E, ic, "Stop (Escape does the same)", p);
        y -= ROW + 8.f;
    }
    S32 total = 0;
    struct K { const char* mKey; const char* mHead; };
    static const K KINDS[3] = { { "areas", "Places players reach" }, { "spots", "Spots (where characters stand, where things appear)" }, { "paths", "Paths (where characters walk)" } };
    for (const K& k : KINDS)
    {
        const LLSD& list = std::as_const(mDraftPack)["places"][k.mKey];
        if (!list.isArray() || list.size() == 0) continue;
        y = head(l, r, y, k.mHead, p);
        for (S32 i = 0; i < (S32)list.size(); ++i)
        {
            ++total;
            const LLSD& it = list[i];
            const std::string ip = llformat("/places/%s/%d", k.mKey, i);
            const std::string id = it["id"].asString();
            const bool in_world = placeIsPlaced(k.mKey, it);
            const bool on_here = it["region"].asString().empty() || it["region"].asString() == here;
            textL(bold, cap(placeName(it), 40), l, y - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 200.f);
            // the state: a tick or a triangle AND the words
            textL(bold, in_world ? "In the world" : "Not in the world yet", r, y - 4.f, in_world ? OK_INK : WARN_INK, LLFontGL::RIGHT, LLFontGL::TOP);
            y -= slh + 8.f;
            y = fText(l + 10.f, r, y, "Its name", ip + "/label", it["label"], 0, 32, 's', true, "A short name you will recognise, e.g. Old well", p);
            F32 bx = l + 160.f;
            const std::string what = "\"" + cap(placeName(it), 32) + "\"";
            if (!on_here)
            {
                y = para(l + 160.f, r, y, "It is on another region: teleport there to place it (the full Places page has Teleport there).", p.mDim, true) - 4.f;
            }
            else if (std::string(k.mKey) == "spots")
            {
                const S32 im = act([this, id]() { placeUseMyPosition(id); });
                btn(bx, y - ROW, bx + btnW("Use my position"), y, "Use my position", SH_E, im, "The spot goes where you stand, facing the way you face", p, true, in_world ? 0 : BTN_PRIMARY);
                bx += btnW("Use my position") + 6.f;
                const S32 ip2 = act([this, id, what]() { startPlace("spots", id, WolfToolAdventurePlace::M_SPOT, what); });
                btn(bx, y - ROW, bx + btnW("Click in the world"), y, "Click in the world", SH_E, ip2, "Click the ground where it goes", p);
                y -= ROW + 8.f;
            }
            else if (std::string(k.mKey) == "areas")
            {
                const S32 im = act([this, id]() { placeAreaHere(id); });
                btn(bx, y - ROW, bx + btnW("Use my position"), y, "Use my position", SH_E, im, "A circle round where you stand; players are there when they step inside it", p, true, in_world ? 0 : BTN_PRIMARY);
                bx += btnW("Use my position") + 6.f;
                const S32 ic = act([this, id, what]() { startPlace("areas", id, WolfToolAdventurePlace::M_CIRCLE, what); });
                btn(bx, y - ROW, bx + btnW("Draw a circle"), y, "Draw a circle", SH_E, ic, "Click the middle, then the edge, in the world", p);
                y -= ROW + 6.f;
                if (it["shape"]["circle"].isMap())
                    y = fText(l + 10.f, r, y, "Radius (metres from the middle)", ip + "/shape/circle/radius", it["shape"]["circle"]["radius"], 0, 0, 'n', false,
                              "The circle's radius in metres: players count as there inside it", p, 0.5, 512.0);
            }
            else
            {
                const S32 ic = act([this, id, what]() { startPlace("paths", id, WolfToolAdventurePlace::M_PATH, what); });
                btn(bx, y - ROW, bx + btnW("Draw the path"), y, "Draw the path", SH_E, ic, "Click each point along the way in the world, then Enter", p, true, in_world ? 0 : BTN_PRIMARY);
                y -= ROW + 8.f;
            }
            y -= 6.f;
        }
    }
    if (total == 0) y = para(l, r, y, "This story has no places yet. A step that is done when players reach a place makes one (step 3: \"reach a place\", then A new place).", p.mDim, true) - 6.f;
    return y;
}

// ── 6. how it ends ────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizRewards(F32 l, F32 r, F32 top, const Palette& p)
{
    F32 y = top;
    const LLSD& endings = std::as_const(mDraftPack)["endings"];
    for (S32 i = 0; endings.isArray() && i < (S32)endings.size(); ++i)
    {
        const LLSD& en = endings[i];
        const std::string ep = llformat("/endings/%d", i);
        y = head(l, r, y, endings.size() > 1 ? llformat("Ending %d", i + 1) : std::string("The ending"), p);
        y = fText(l, r, y, "Title", ep + "/title", en["title"], 0, 64, 's', false, "e.g. The gold is found", p);
        y = fText(l, r, y, "What the players read", ep + "/text", en["text"], 0, 2000, 's', false, "e.g. You found the gold! The town will remember you.", p);
        // the prize: a reward parcel the ending names (adventure_pack_v1.schema.json ending.reward -> parcels[].id)
        const std::string rid = en["reward"].asString();
        S32 pi = -1;
        const LLSD& parcels = std::as_const(mDraftPack)["parcels"];
        for (S32 k = 0; parcels.isArray() && k < (S32)parcels.size(); ++k) if (parcels[k]["id"].asString() == rid) pi = k;
        if (pi < 0)
        {
            const S32 ia = act([this, ep]()
            {
                const std::string id = newPackId("prize");
                docAppend(0, "/parcels", boost::json::object{ { "id", id }, { "label", "Prize" }, { "money", 0 } });
                docSet(0, ep + "/reward", boost::json::value(id));
            });
            btn(l + 150.f, y - ROW, l + 150.f + btnW("Give a prize at this ending"), y, "Give a prize at this ending", SH_E, ia, "Players get money or an item when they reach it", p);
            y -= ROW + 8.f;
        }
        else
        {
            const std::string pp = llformat("/parcels/%d", pi);
            const LLSD& pc = parcels[pi];
            y = fText(l, r, y, "Money", pp + "/money", pc["money"], 0, 0, 'i', true, "Money from the region's game, e.g. 50", p, 0.0, 1000000000.0);
            if (pc["items"][0].isMap())
            {
                y = fText(l, r, y, "Item (its game name)", pp + "/items/0/item", pc["items"][0]["item"], 0, 24, 's', false,
                          "A wolfGame item name: a-z, 0-9 and _, e.g. gold_nugget", p);
                y = fText(l, r, y, "How many", pp + "/items/0/qty", pc["items"][0]["qty"], 0, 0, 'i', false, "How many of the item", p, 1.0, 1000000.0);
            }
            else
            {
                // rewardParcel.items[] needs both item and qty (adventure_pack_v1.schema.json): made whole, then edited
                const S32 ii = act([this, pp]() { docSet(0, pp + "/items", boost::json::array{ boost::json::object{ { "item", "gold_nugget" }, { "qty", 1 } } }); });
                btn(l + 150.f, y - ROW, l + 150.f + btnW("Give an item too"), y, "Give an item too", SH_E, ii, "Add an item to the prize (you then type its game name)", p);
                y -= ROW + 6.f;
            }
            // BRAIN_API_V1.md §8.3, in the words the Rules page uses (wolffloateradventurestudioedit.cpp pageRules)
            y = para(l + 150.f, r, y, "Money and items move only on games you own or whose owner linked this adventure; otherwise they are skipped (and logged).",
                     p.mDim, true) - 4.f;
            const S32 ix = act([this, ep]() { docErase(0, ep + "/reward"); });
            btn(l + 150.f, y - ROW, l + 150.f + btnW("No prize"), y, "No prize", SH_E, ix, "This ending gives nothing (the prize stays on the Lore page)", p, true, BTN_DANGER);
            y -= ROW + 8.f;
        }
        y -= 6.f;
    }
    return y;
}

// ── 7. the look ───────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizLook(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const LLSD& ad = std::as_const(mDraftPack)["adventure"];
    const LLSD& look = ad["look"];
    F32 y = head(l, r, top, "Pictures", p);
    y = para(l, r, y, "Choose pictures from your inventory (or drag one onto the box). They must be textures you may copy; everyone who plays sees them.", p.mDim, true) - 4.f;
    y = fTexture(l, r, y, "Cover (Adventures list)", "/adventure/cover", ad["cover"], p);
    y = fTexture(l, r, y, "Logo (list and HUD)", "/adventure/look/logo", look["logo"], p);
    y = head(l, r, y, "Title card (when a player starts)", p);
    y = fTexture(l, r, y, "Picture", "/adventure/look/titleCard/image", look["titleCard"]["image"], p);
    y = fText(l, r, y, "Big words", "/adventure/look/titleCard/title", look["titleCard"]["title"], 0, 64, 's', true, "Blank = the adventure's name", p);
    y = fText(l, r, y, "Small words", "/adventure/look/titleCard/sub", look["titleCard"]["sub"], 0, 64, 's', true, "e.g. A Wolf Creek story", p);
    y = fText(l, r, y, "Seconds on screen", "/adventure/look/titleCard/seconds", look["titleCard"]["seconds"], 0, 0, 'i', true, "2 to 10 (blank = 5)", p, 2.0, 10.0);
    if (look.has("titleCard"))
    {
        // [review] an empty title card still shows one (the adventure's name), so no card at all is its own choice
        const S32 ix = act([this]() { docErase(0, "/adventure/look/titleCard"); });
        btn(l + 150.f, y - ROW, l + 150.f + btnW("No title card"), y, "No title card", SH_E, ix, "Players start without a title card", p, true, BTN_DANGER);
        y -= ROW + 6.f;
    }
    else y = para(l + 150.f, r, y, "No title card yet: fill in any of the above to have one.", p.mDim, true) - 4.f;
    // ---- colours: a set with one click, or your own two ----
    y = head(l, r, y, "Colours of the story panel", p);
    {
        F32 x = l;
        const bool none = look["accent"].asString().empty() && look["panel"].asString().empty();
        {
            const bool has_ac = look.has("accent"), has_pc = look.has("panel");
            const S32 idx = act([this, has_ac, has_pc]() { if (has_ac) docErase(0, "/adventure/look/accent"); if (has_pc) docErase(0, "/adventure/look/panel"); });
            const F32 bw = btnW("Wolf Territories (default)");
            btn(x, y - ROW, x + bw, y, "Wolf Territories (default)", SH_E, idx, "The viewer's own colours" + std::string(none ? " (chosen)" : ""), p, has_ac || has_pc, none ? BTN_ACTIVE : 0);
            x += bw + 6.f;
        }
        for (const Theme& t : THEMES)
        {
            LLColor4 ac, pc;
            WolfAdvStudio::parseColour(t.mAccent, ac);
            WolfAdvStudio::parseColour(t.mPanel, pc);
            const F32 bw = btnW(t.mName) + 18.f;
            if (x > l && x + bw > r) { x = l; y -= ROW + 6.f; }
            const bool on = look["accent"].asString() == t.mAccent && look["panel"].asString() == t.mPanel;
            const std::string a = t.mAccent, pnl = t.mPanel;
            const S32 idx = act([this, a, pnl]() { docSet(0, "/adventure/look/accent", boost::json::value(a)); docSet(0, "/adventure/look/panel", boost::json::value(pnl)); });
            // the set's own colours on the button, its name in the readable ink (never colour alone: the name says it)
            rect(x + 1.f, y - ROW + 1.f, x + bw - 1.f, y - 1.f, pc);
            hit(x, y - ROW, x + bw, y, SH_E, idx, std::string(t.mName) + " colours" + (on ? " (chosen)" : ""));
            chamferEdge(x + 0.5f, y - ROW + 0.5f, x + bw - 0.5f, y - 0.5f, 3.f, on || mHits.isHover(SH_E, idx) ? 2.f : 1.f, on ? ac : p.mDim);
            disc(x + 10.f, y - ROW * 0.5f, 4.f, ac);
            textL(bold, t.mName, x + 18.f, y - ROW * 0.5f, WolfAdvStudio::inkOn(pc), LLFontGL::LEFT, LLFontGL::VCENTER);
            if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_E, idx)) focusRing(x, y - ROW, x + bw, y, p);
            x += bw + 6.f;
        }
        y -= ROW + 8.f;
    }
    y = fColour(l, r, y, "Accent (titles)", "/adventure/look/accent", look["accent"], 0, true, p);
    y = fColour(l, r, y, "Panel (background)", "/adventure/look/panel", look["panel"], 0, true, p);
    // the contrast check, in words with the number (WCAG 1.4.3: text at least 4.5:1)
    LLColor4 ac = p.mAccent, pc = p.mCard;
    const bool has_a = WolfAdvStudio::parseColour(look["accent"].asString(), ac);
    const bool has_p = WolfAdvStudio::parseColour(look["panel"].asString(), pc);
    if (has_a || has_p)
    {
        if (!has_a) ac = p.mAccent;
        if (!has_p) pc = p.mCard;
        pc.mV[VALPHA] = 1.f;
        const F32 cr = WolfAdvStudio::contrast(ac, pc);
        textL(bold, cr >= 4.5f ? llformat("Readable (%.1f to 1)", cr) : llformat("Hard to read (%.1f to 1): pick a lighter or darker colour", cr),
              l + 150.f, y, cr >= 4.5f ? OK_INK : DANGER_INK, LLFontGL::LEFT, LLFontGL::TOP, r - l - 150.f);
        y -= slh + 6.f;
    }
    // ---- the preview: the objective plate as players see it ----
    y = head(l, r, y, "Preview", p);
    {
        const F32 ph = 64.f, pw = llmin(r - l, 420.f);
        LLColor4 panel = has_p ? pc : p.mCard;
        panel.mV[VALPHA] = 1.f;
        rect(l, y - ph, l + pw, y, panel);
        chamferEdge(l + 0.5f, y - ph + 0.5f, l + pw - 0.5f, y - 0.5f, 4.f, 1.5f, has_a ? ac : p.mAccent);
        F32 tx = l + 10.f;
        const std::string logo = look["logo"].asString();
        LLUUID lid;
        if (!logo.empty() && LLUUID::validate(logo) && lid.set(logo, false) && lid.notNull())
        {
            if (LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(lid, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_UI))
            {
                tex->addTextureStats(44.f * 44.f);
                gl_draw_scaled_image((S32)l + 10, (S32)(y - ph) + 10, 44, 44, tex);
            }
            tx = l + 62.f;
        }
        const std::string title = cap(ad["title"].asString(), 64);
        textL(display(16.f), title.empty() ? std::string("Your adventure") : title, tx, y - 10.f, has_a ? ac : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, l + pw - tx - 10.f);
        const auto beats = beatsInOrder(mDraftPack);
        const std::string obj = beats.empty() ? std::string("The first step's objective") : cap((*beats.front().second)["objective"].asString(), 120);
        textL(small, obj, tx, y - 36.f, WolfAdvStudio::inkOn(panel), LLFontGL::LEFT, LLFontGL::TOP, l + pw - tx - 10.f);
        y -= ph + 10.f;
    }
    return y;
}

// ── 9. publish ────────────────────────────────────────────────────────────────────────────────────────────

F32 WolfAdventureStudioView::wizPublish(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    F32 y = top;
    const LLSD* a = listed(mOpenId);
    const bool published = a && (*a)["status"].asString() == "published";
    if (published)
    {
        std::string where;
        for (const LLSD& rg : llsd::inArray((*a)["regions"])) if (rg["state"].asString() == "enabled") where += (where.empty() ? "" : ", ") + cap(rg["name"].asString(), 40);
        y = para(l, r, y, llformat("It is live (revision %d). ", (*a)["revision"].asInteger()) +
                 (where.empty() ? std::string("No region has said yes yet, so nobody can play it until one does.")
                                : "Players on " + where + " find it in Wolf World > Games & Stories > Adventures. Change anything and publish again whenever you like."),
                 where.empty() ? WARN_INK : OK_INK) - 8.f;
    }
    // the brain's checks, in its plain words, each with Fix (AUDIT §5.1 say / where / step)
    S32 errors = 0, warnings = 0;
    for (const LLSD& pr : llsd::inArray(mProblems)) (pr["level"].asString() == "error" ? errors : warnings)++;
    y = head(l, r, y, errors ? llformat("%d thing%s to fix first", errors, errors == 1 ? "" : "s") : std::string("Nothing stops publishing"), p);
    S32 shown = 0;
    for (S32 i = 0; i < (S32)mProblems.size() && shown < 12; ++i)
    {
        const LLSD& pr = mProblems[i];
        if (pr["level"].asString() != "error") continue;
        ++shown;
        const F32 item_top = y;
        y = para(l, r - 90.f, y, problemWords(pr), p.mText, true);
        if (!pr["where"].asString().empty()) y = para(l + 10.f, r - 90.f, y, cap(pr["where"].asString(), 200), p.mDim, true);
        const std::string step = pr["step"].asString(), path = pr["path"].asString();
        S32 ws = -1;
        for (S32 k = 0; k < W_COUNT; ++k) if (step == STEPS[k].mBrainStep) ws = k;
        const S32 idx = act([this, ws, path]()
        {
            if (ws >= 0 && ws != W_PUBLISH)
            {
                wizGo(ws);
                // the field itself is focused and brought into view (fText / fRef focusWants)
                mFocusPtr = path;
                mFocusScroll = true;
                mFocusAt = LLFrameTimer::getTotalSeconds();
            }
            else gotoProblem(path);
        });
        btn(r - 80.f, item_top - ROW, r, item_top, "Fix", SH_E, idx, ws >= 0 && ws != W_PUBLISH ? llformat("Go to step %d and fix it", ws + 1) : std::string("Open it and fix it"), p);
        y = llmin(y, item_top - ROW) - 8.f;
    }
    if (errors > shown) y = para(l, r, y, llformat("...and %d more (the Problems list on the right has them all).", errors - shown), p.mDim, true) - 4.f;
    if (warnings) y = para(l, r, y, llformat("%d thing%s worth a look (they do not stop publishing): see the list on the right.", warnings, warnings == 1 ? "" : "s"), p.mDim, true) - 4.f;
    y -= 6.f;
    {
        const S32 ic = act([this]() { press(SH_CHECK, 0); });
        btn(l, y - ROW, l + btnW("Check again"), y, "Check again", SH_E, ic, "Ask the grid to check the draft now", p, mBusy == 0);
        y -= ROW + 10.f;
    }
    // the region must have said yes for anyone to play it
    bool any_enabled = false;
    if (a) for (const LLSD& rg : llsd::inArray((*a)["regions"])) any_enabled = any_enabled || rg["state"].asString() == "enabled";
    if (!any_enabled) y = para(l, r, y, "No region has said yes yet (step 2). You can publish now; it starts when a region's holder says yes.", WARN_INK, true) - 6.f;
    const bool can = mBusy == 0 && errors == 0 && !mEtag.empty();
    const S32 ip = act([this]() { press(SH_PUBLISH, 0); });
    const std::string pl = published ? "Publish the changes..." : "Publish...";
    btn(r - btnW(pl, 180.f), y - 34.f, r, y, pl, SH_E, ip, errors ? "Fix the things above first" : std::string("Make it live (asks first)"), p, can, BTN_PRIMARY);
    if (errors) textL(bold, "Fix the things above first.", l, y - 17.f, DANGER_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
    y -= 46.f;
    return y;
}
