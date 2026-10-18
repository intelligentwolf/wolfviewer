/**
 * @file wolffloateradventurestudioflow.cpp
 * @brief WolfViewer: the Adventure Studio's helper conversation and flowchart. See wolffloateradventurestudio.h.
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
 * [WOLF STORY FIRST 2026-10-17] region-terrain-work/wolf-story/STORY_FIRST_DESIGN.md. Paul: "AI IS VERY IMPORTANT HERE.
 * You build your game by talking it through with the AI ... so you could actually not define that much and define as
 * you go." and "you get to a place, click on a box in the flowchart and tell the AI helper what is happening".
 *
 * Persona and story: a resident who has never made a game writes one line about a gold robbery, then walks their land,
 * clicking a box at each place and telling the helper what happens there.
 *
 * The page: the conversation (what the author said, what the helper changed, its question, the "still to decide" list,
 * Undo, a running cost meter) beside the flowchart of the whole game (boxes = beats and endings, arrows = a trigger's
 * `next`, `goto` / `end` steps, decision options, scene exits; ADVENTURE_PACK_V1.md §11). The box the author clicks is
 * the helper's `node` (STORY_FIRST_DESIGN.md §5 helper_chat); "Only this place" keeps the boxes anchored near the author.
 * Every helper reply carries the saved draft, so the flowchart follows the conversation at once.
 */

#include "llviewerprecompiledheaders.h"
#include "llweb.h"

#include "wolffloateradventurestudio.h"

#include <cmath>
#include <deque>
#include <map>
#include <set>

#include "llagent.h"
#include "llclipboard.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llselectmgr.h"
#include "wolfadventure.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgamehud.h"

using namespace WolfGameDraw;
using WolfAdvStudio::btnW;

namespace
{
    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as wolffloateradventurestudio.cpp
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }
    std::string trimmed(std::string s) { LLStringUtil::trim(s); return s; }
    F64 now() { return LLFrameTimer::getTotalSeconds(); }   // [SETUP WALK 2026-10-18] as the other Studio files
    // [WORDING 2026-10-18] whole credits on the three pages, as the web Studio's _cr: "45 credits", a part of one "under 1 credit"
    std::string wholeCredits(F64 v)
    {
        if (v > 0.0 && v < 1.0) return "under 1 credit";
        const S64 w = llround(v);
        return thousands(w) + (w == 1 ? " credit" : " credits");
    }
    const F32 ROW = 26.f;
    // the chart's own units (zoom 1): a box and the gaps between layers and rows. [SIMPLE STUDIO 2026-10-17] a box holds its
    // head, two lines of words, "Where: ..." and its place line (five small lines at 13 px, plus the padding)
    // [MAP LABELS 2026-10-17] the gap between layers holds an arrow's words on their plate (drawFlow: LABEL_W of words + 8 of
    // plate at zoom 1) without touching the boxes either side
    const F32 BOX_W = 176.f, BOX_H = 80.f, GAP_X = 120.f, GAP_Y = 22.f;
    const F32 LABEL_W = 110.f;
    // [GAME CARD 2026-10-18] under a main step: its objective (two small lines) and its "+N side paths" button
    const F32 MAIN_UNDER = 70.f;
    // [SIMPLE STUDIO] the story's hint of where a scene happens: the build writes the scene's gmNotes as "Where: <hint>\n<summary>"
    // (wolfmind_lib.php:2621)
    const char* const WHERE_PREFIX = "Where: ";
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the flowchart's model: boxes and arrows from the draft
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [WORDING 2026-10-18] what makes a way on happen, and a condition, in plain words - as the web Studio's WolfAdventureFlow
// triggerWords / condWords (floater_wolf_adventure_studio.js), for the map, the box panel and Test play
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    std::string flowCastName(const LLSD& pk, const std::string& id)
    {
        for (const LLSD& c : llsd::inArray(pk["cast"]))
            if (c["id"].asString() == id)
            {
                if (c["first"].asString() == "New" && c["last"].asString() == "Character") return "a new character (not named yet)";
                std::string n = cap(c["first"].asString(), 32) + " " + cap(c["last"].asString(), 32);
                LLStringUtil::trim(n);
                return n;
            }
        return id.empty() ? std::string("someone") : id;
    }
    std::string flowPlaceLabel(const LLSD& pk, const char* kind, const std::string& id)
    {
        for (const LLSD& x : llsd::inArray(pk["places"][kind]))
            if (x["id"].asString() == id) return x["label"].asString().empty() ? id : cap(x["label"].asString(), 32);
        return id;
    }
    std::string flowSpaced(std::string v) { for (char& ch : v) if (ch == '-' || ch == '_') ch = ' '; return v; }
    std::string flowFlagName(const LLSD& pk, const std::string& id)
    {
        for (const LLSD& f : llsd::inArray(pk["state"]["flags"]))
            if (f["id"].asString() == id && !f["journal"].asString().empty()) return cap(f["journal"].asString(), 32);
        return flowSpaced(id);
    }
    std::string flowBeatTitle(const LLSD& pk, const std::string& id)
    {
        for (const LLSD& a : llsd::inArray(pk["acts"]))
            for (const LLSD& sc : llsd::inArray(a["scenes"]))
                for (const LLSD& b : llsd::inArray(sc["beats"]))
                    if (b["id"].asString() == id)
                    {
                        const std::string t = !b["title"].asString().empty() ? cap(b["title"].asString(), 64) : cap(b["objective"].asString(), 64);
                        return t.empty() ? std::string("a box") : "\"" + t + "\"";
                    }
        return "that box";
    }
    std::string flowValueWord(const LLSD& v)
    {
        if (v.isBoolean()) return v.asBoolean() ? "yes" : "no";
        if (v.isInteger()) return llformat("%d", v.asInteger());
        if (v.isReal()) return llformat("%g", v.asReal());
        return v.asString();
    }
}

// static
std::string WolfAdventureStudioView::condWords(const LLSD& c, const LLSD& pk, S32 depth)
{
    if (!c.isMap() || depth > 6) return std::string();
    auto join = [&](const LLSD& list, const char* sep)
    {
        std::string out;
        for (const LLSD& x : llsd::inArray(list)) { const std::string w = condWords(x, pk, depth + 1); if (!w.empty()) out += (out.empty() ? "" : sep) + w; }
        return out;
    };
    if (c["all"].isArray()) return join(c["all"], " and ");
    if (c["any"].isArray()) return join(c["any"], " or ");
    if (c.has("not")) { const std::string w = condWords(c["not"], pk, depth + 1); return w.empty() ? std::string() : "it is not so that " + w; }
    auto cmp = [&c](const std::string& subject) -> std::string
    {
        if (c.has("eq")) return subject + " is " + flowValueWord(c["eq"]);
        if (c.has("ne")) return subject + " is not " + flowValueWord(c["ne"]);
        if (c.has("gte")) return subject + " is at least " + flowValueWord(c["gte"]);
        if (c.has("lte")) return subject + " is at most " + flowValueWord(c["lte"]);
        if (c.has("gt")) return subject + " is more than " + flowValueWord(c["gt"]);
        if (c.has("lt")) return subject + " is less than " + flowValueWord(c["lt"]);
        if (c.has("band")) return subject + " is " + c["band"].asString();
        return std::string();
    };
    auto either = [](const std::string& a, const std::string& b) { return a.empty() ? b : a; };
    if (c["flag"].isString()) return either(cmp(flowFlagName(pk, c["flag"].asString())), flowFlagName(pk, c["flag"].asString()));
    if (c["visited"].isString()) return "they have been to " + flowBeatTitle(pk, c["visited"].asString());
    if (c["item"].isString()) return either(cmp("their " + c["item"].asString()), "they have " + c["item"].asString());
    if (c["stat"].isString()) return cmp("their " + c["stat"].asString());
    if (c["rep"].isString()) return cmp("what " + flowSpaced(c["rep"].asString()) + " think of them");
    if (c["castRep"].isString()) return cmp("what " + flowCastName(pk, c["castRep"].asString()) + " thinks of them");
    if (c.has("honour") && c["honour"].asBoolean()) return cmp("their honour");
    if (c.has("partySize") && c["partySize"].asBoolean()) return cmp("the party size");
    if (c["hour"].isArray()) return llformat("it is between %d:00 and %d:00", c["hour"][0].asInteger(), c["hour"][1].asInteger());
    if (c.has("gmPresent") && c["gmPresent"].asBoolean()) return "a host is there";
    if (c["aiMode"].isString()) return c["aiMode"].asString() == "ai" ? "AI is on" : "AI is off";
    if (c["chance"].isReal() || c["chance"].isInteger()) return llformat("a %d%% chance", (S32)llround(c["chance"].asReal() * 100.0));
    return "something is true";
}

// static
std::string WolfAdventureStudioView::triggerWords(const LLSD& t, const LLSD& pk)
{
    const std::string on = t["on"].asString();
    if (on == "arrive") return "they reach " + (t["area"].isString() ? flowPlaceLabel(pk, "areas", t["area"].asString()) : std::string("a place (not chosen yet)"));
    if (on == "leave") return "they leave " + flowPlaceLabel(pk, "areas", t["area"].asString());
    if (on == "talk") return "they talk to " + (t["cast"].isString() ? flowCastName(pk, t["cast"].asString()) : std::string("someone (not chosen yet)"));
    if (on == "say")
    {
        std::string w;
        S32 n = 0;
        for (const LLSD& i : llsd::inArray(t["intent"])) { if (n++ >= 3) break; w += (w.empty() ? "" : "\" or \"") + i.asString(); }
        return "they say \"" + w + "\"";
    }
    if (on == "touch") return "they touch " + (!t["object"].asString().empty() ? "\"" + cap(t["object"].asString(), 64) + "\"" : std::string("an object (not named yet)"));
    if (on == "sit") return "they sit on \"" + cap(t["object"].asString(), 64) + "\"";
    if (on == "choose") return "they choose";
    if (on == "give") return "they give " + cap(t["item"].asString(), 24) + " to " + flowCastName(pk, t["cast"].asString());
    if (on == "defeat") return "they beat " + flowCastName(pk, t["cast"].asString());
    if (on == "timer") return llformat("after %d seconds", t["sec"].asInteger());
    if (on == "flag") { const std::string w = condWords(t["if"], pk, 0); return "when " + (w.empty() ? std::string("something becomes true") : w); }
    if (on == "gm")
    {
        // the helper's placeholder way on: a gm step "Still to decide" (id tbd-..., wolfadventure_story_lib.php was_chain_after)
        if (t["id"].asString().rfind("tbd-", 0) == 0) return "still to decide (tell your helper, or the box, how this box leads on)";
        return "the host presses \"" + cap(t["label"].asString(), 32) + "\"";
    }
    if (on == "script") return "your script sends \"" + cap(t["id"].asString(), 20) + "\"";   // [M6] the name wolfAdventureFire uses
    if (on == "roll") return "after a dice roll";
    if (on == "item") return "they hold " + cap(t["item"].asString(), 24);
    if (on == "time") return "at a time of day";
    if (on == "quiz") return "they finish the quiz";
    if (on == "ready") return "everyone in the group is ready";
    if (on == "ai") return "when the character decides";
    if (on == "scene") return "when the part changes";
    if (on == "won") return "if they win";     // [M4] a fight's ways on
    if (on == "lost") return "if they lose";
    return "something happens";
}

void WolfAdventureStudioView::flowLayout()
{
    if (mFlowGen == mDraftGen && !mFlowNodes.empty()) return;
    mFlowGen = mDraftGen;
    mFlowNodes.clear();
    mFlowEdges.clear();
    const LLSD& pk = mDraftPack;
    std::map<std::string, S32> beatIx, endIx, sceneFirst;
    // where each place is (area centre or spot), for the box's "place" line and the near filter
    auto placeOfBeat = [&pk](const LLSD& sc, const LLSD& bt, bool& placed) -> std::string
    {
        placed = false;
        const std::string marker = bt["marker"].asString(), area = sc["area"].asString();
        for (const LLSD& s : llsd::inArray(pk["places"]["spots"]))
            if (!marker.empty() && s["id"].asString() == marker) { placed = s["pos"][0].asReal() >= 0.0; return s["label"].asString().empty() ? marker : s["label"].asString(); }
        for (const LLSD& a : llsd::inArray(pk["places"]["areas"]))
            if (!area.empty() && a["id"].asString() == area) { placed = a["shape"]["circle"]["centre"][0].asReal() >= 0.0 || a["shape"]["polygon"][0][0].asReal() >= 0.0; return a["label"].asString().empty() ? area : a["label"].asString(); }
        return std::string();
    };
    const LLSD& acts = pk["acts"];
    for (S32 a = 0; acts.isArray() && a < (S32)acts.size(); ++a)
        for (S32 s = 0; s < (S32)acts[a]["scenes"].size(); ++s)
        {
            const LLSD& sc = acts[a]["scenes"][s];
            for (S32 b = 0; b < (S32)sc["beats"].size(); ++b)
            {
                const LLSD& bt = sc["beats"][b];
                FlowNode n;
                n.mPtr = llformat("/acts/%d/scenes/%d/beats/%d", a, s, b);
                n.mId = bt["id"].asString();
                // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 1: beat `main` (a main quest step, numbered in pack order),
                // beat `kind` (the box's icon; absent = the brain's wa_beat_kind rule, beatKind), acts[].built (absent = true)
                n.mMain = bt["main"].isBoolean() && bt["main"].asBoolean();
                n.mBuilt = !(acts[a]["built"].isBoolean() && !acts[a]["built"].asBoolean());
                {
                    const std::string k = bt["kind"].asString();
                    n.mKind = (k == "place" || k == "character" || k == "challenge" || k == "item" || k == "gate") ? k : beatKind(bt);
                }
                n.mObjective = cap(bt["objective"].asString(), 240);
                n.mTitle = cap(bt["title"].asString().empty() ? bt["objective"].asString() : bt["title"].asString(), 120);
                n.mPlace = cap(placeOfBeat(sc, bt, n.mPlaced), 40);
                {
                    // the scene's own hint of where it happens (its gmNotes' first line, "Where: ...")
                    const std::string notes = sc["gmNotes"].asString();
                    if (notes.rfind(WHERE_PREFIX, 0) == 0)
                    {
                        const size_t nl = notes.find('\n');
                        n.mWhere = cap(notes.substr(strlen(WHERE_PREFIX), nl == std::string::npos ? std::string::npos : nl - strlen(WHERE_PREFIX)), 80);
                    }
                }
                for (const LLSD& e : llsd::inArray(bt["onEnter"]))   // [M4] a fight box (wolfadventure_story_lib.php was_battle_op)
                    if (e["battle"].isString())
                    {
                        n.mFight = n.mFightId = e["battle"].asString();
                        for (const LLSD& f : llsd::inArray(pk["battles"])) if (f["id"].asString() == n.mFight && !f["label"].asString().empty()) n.mFight = f["label"].asString();
                    }
                if (b == 0) sceneFirst[sc["id"].asString()] = (S32)mFlowNodes.size();
                beatIx[n.mId] = (S32)mFlowNodes.size();
                mFlowNodes.push_back(n);
            }
        }
    for (const LLSD& en : llsd::inArray(pk["endings"]))
    {
        FlowNode n;
        n.mEnding = true;
        n.mId = en["id"].asString();
        n.mTitle = cap(en["title"].asString(), 80);
        n.mPtr = "/endings/" + llformat("%d", (S32)endIx.size());
        n.mPlaced = true;
        // [GAME CARD 2026-10-18] ending `kind` win | lose | other (absent = other, older packs)
        {
            const std::string k = en["kind"].asString();
            n.mKind = k == "win" || k == "lose" ? k : std::string("other");
        }
        endIx[n.mId] = (S32)mFlowNodes.size();
        mFlowNodes.push_back(n);
    }
    // the arrows (ADVENTURE_PACK_V1.md §11: trigger next, effects goto / end, decision options, scene exits)
    // `full` = the whole words (the box panel's "Leads to"); the map shows at most 26 characters of them (textL cuts with "...")
    // [WORDING 2026-10-18] the arrows' words as the web Studio's (floater_wolf_adventure_studio.js WolfAdventureFlow.triggerWords,
    // condWords, castName, placeLabel, flagName, _beatTitle, graph): what makes a way on happen, and apart from it the condition
    // ("Only if ..." in the box panel); the map shows the words, cut to 26 characters, with " (if...)" when there is a condition
    auto add = [this](S32 from, S32 to, const std::string& full, const std::string& cond, bool dashed)
    {
        if (from < 0 || to < 0) return;
        for (const FlowEdge& e : mFlowEdges) if (e.mFrom == from && e.mTo == to && e.mFull == full && e.mCond == cond) return;
        FlowEdge e; e.mFrom = from; e.mTo = to; e.mFull = full; e.mCond = cond; e.mDashed = dashed;
        e.mLabel = cap(full, 26) + (cond.empty() ? std::string() : std::string(" (if...)"));
        mFlowEdges.push_back(e);
    };
    auto target = [&](const LLSD& e) -> S32
    {
        if (e.has("end")) { auto it = endIx.find(e["end"].asString()); return it == endIx.end() ? -1 : it->second; }
        if (e["goto"]["beat"].isString()) { auto it = beatIx.find(e["goto"]["beat"].asString()); return it == beatIx.end() ? -1 : it->second; }
        if (e["goto"]["scene"].isString()) { auto it = sceneFirst.find(e["goto"]["scene"].asString()); return it == sceneFirst.end() ? -1 : it->second; }
        return -1;
    };
    auto words = [&pk](const LLSD& t) { return triggerWords(t, pk); };
    auto condWords = [&pk](const LLSD& c, S32 depth) { return WolfAdventureStudioView::condWords(c, pk, depth); };
    for (S32 a = 0; acts.isArray() && a < (S32)acts.size(); ++a)
        for (S32 s = 0; s < (S32)acts[a]["scenes"].size(); ++s)
        {
            const LLSD& sc = acts[a]["scenes"][s];
            S32 lastBeat = -1;
            for (S32 b = 0; b < (S32)sc["beats"].size(); ++b)
            {
                const LLSD& bt = sc["beats"][b];
                const S32 from = beatIx[bt["id"].asString()];
                lastBeat = from;
                for (const LLSD& t : llsd::inArray(bt["triggers"]))
                {
                    const std::string cond = t.has("if") && t["on"].asString() != "flag" ? condWords(t["if"], 0) : std::string();
                    if (t["next"].isString()) { auto it = beatIx.find(t["next"].asString()); if (it != beatIx.end()) add(from, it->second, words(t), cond, false); }
                    for (const LLSD& e : llsd::inArray(t["do"])) add(from, target(e), words(t), cond, false);
                }
                for (const LLSD& d : llsd::inArray(bt["decisions"]))
                    for (const LLSD& o : llsd::inArray(d["options"]))
                        for (const LLSD& e : llsd::inArray(o["do"])) add(from, target(e), "\"" + cap(o["label"].asString(), 24) + "\"", o.has("if") ? condWords(o["if"], 0) : std::string(), false);
                for (const LLSD& e : llsd::inArray(bt["onEnter"])) add(from, target(e), "straight away", std::string(), false);
            }
            for (const LLSD& x : llsd::inArray(sc["exits"]))
            {
                auto it = sceneFirst.find(x["to"].asString());
                // [WORDING 2026-10-18] from the part's FIRST box, as the web Studio's graph (an exit is the part's own way out)
                auto first = sceneFirst.find(sc["id"].asString());
                if (it != sceneFirst.end() && first != sceneFirst.end() && lastBeat >= 0)
                    add(first->second, it->second, x["label"].asString().empty() ? std::string("a way out") : cap(x["label"].asString(), 32), x.has("if") ? condWords(x["if"], 0) : std::string(), true);
            }
        }
    // layers: breadth first from the start beat (the shortest way there); what no way reaches gets a column of its own;
    // endings one layer past the furthest box leading to them. Rows in discovery order. Deterministic: pack order.
    const std::string start = pk["adventure"]["start"]["beat"].asString();
    S32 s0 = beatIx.count(start) ? beatIx[start] : (mFlowNodes.empty() || mFlowNodes[0].mEnding ? -1 : 0);
    std::vector<S32> layer(mFlowNodes.size(), -1);
    std::deque<S32> q;
    if (s0 >= 0) { layer[s0] = 0; q.push_back(s0); mFlowNodes[s0].mStart = true; }
    while (!q.empty())
    {
        const S32 c = q.front(); q.pop_front();
        for (const FlowEdge& e : mFlowEdges)
            if (e.mFrom == c && layer[e.mTo] < 0 && !mFlowNodes[e.mTo].mEnding) { layer[e.mTo] = layer[c] + 1; q.push_back(e.mTo); }
    }
    S32 maxLayer = 0;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding) maxLayer = llmax(maxLayer, layer[i]);
    bool unreached = false;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding && layer[i] < 0) { layer[i] = maxLayer + 1; unreached = true; }
    const S32 endLayer = maxLayer + (unreached ? 2 : 1);
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
        if (mFlowNodes[i].mEnding)
        {
            S32 l = -1;
            for (const FlowEdge& e : mFlowEdges) if (e.mTo == i && layer[e.mFrom] >= 0) l = llmax(l, layer[e.mFrom] + 1);
            layer[i] = l < 0 ? endLayer : llmax(l, 1);
        }
    std::map<S32, S32> rows;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
    {
        FlowNode& n = mFlowNodes[i];
        n.mLayer = layer[i];
        n.mRow = rows[n.mLayer]++;
        n.mX = (F32)n.mLayer * (BOX_W + GAP_X);
        n.mY = (F32)n.mRow * (BOX_H + GAP_Y);
    }
    // [SIMPLE STUDIO 2026-10-17] a box the author dragged keeps its place: the pack's top-level `flow` map, "b:<beat id>" /
    // "e:<ending id>" -> {x, y} in chart units (adventure_pack_v1.schema.json properties.flow; flowSavePos writes it). Kept at
    // the top level, not on the beat, so a move is not an author's change of the beat (the brain's was_hash would then keep
    // the box as it is at the next rebuild, wolfadventure_story_lib.php:76, 186-232)
    // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5 page 3: with main quest steps the map is the main quest as ONE line,
    // left to right, numbered 1..m; every other box is a side path under the main step it branches from (the nearest main step
    // that leads to it), hidden until that step's "+N side paths" or "Show everything" opens it; the endings sit after the last
    // step. With no main steps (a roleplay world, an older game) the map stays as above.
    mFlowMainMode = false;
    {
        std::vector<S32> mains;
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding && mFlowNodes[i].mMain) mains.push_back(i);
        if (!mains.empty())
        {
            mFlowMainMode = true;
            const S32 m = (S32)mains.size();
            for (S32 k = 0; k < m; ++k) mFlowNodes[mains[k]].mStep = k + 1;
            // every side box: owned by the first main step a way forward from the main steps reaches it from (all steps at once,
            // in step order, so the nearest step wins)
            std::deque<S32> q2;
            for (S32 k = 0; k < m; ++k) { mFlowNodes[mains[k]].mOwner = k + 1; q2.push_back(mains[k]); }
            while (!q2.empty())
            {
                const S32 c = q2.front(); q2.pop_front();
                for (const FlowEdge& e : mFlowEdges)
                {
                    if (e.mFrom != c) continue;
                    FlowNode& t = mFlowNodes[e.mTo];
                    if (t.mEnding || t.mMain || t.mOwner > 0) continue;
                    t.mOwner = mFlowNodes[c].mOwner;
                    q2.push_back(e.mTo);
                }
            }
            // a box no main step leads to (the start before step 1, a box left on its own): under the first main step it leads to,
            // else under step 1
            for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
            {
                FlowNode& n = mFlowNodes[i];
                if (n.mEnding || n.mMain || n.mOwner > 0) continue;
                std::vector<bool> seen(mFlowNodes.size(), false);
                std::deque<S32> f;
                f.push_back(i);
                seen[i] = true;
                S32 found = 0;
                while (!f.empty() && !found)
                {
                    const S32 c = f.front(); f.pop_front();
                    for (const FlowEdge& e : mFlowEdges)
                    {
                        if (e.mFrom != c || seen[e.mTo]) continue;
                        seen[e.mTo] = true;
                        if (mFlowNodes[e.mTo].mMain) { found = mFlowNodes[e.mTo].mStep; break; }
                        if (!mFlowNodes[e.mTo].mEnding) f.push_back(e.mTo);
                    }
                }
                n.mOwner = found > 0 ? found : 1;
            }
            // the places: the line at the top; each step's side boxes in a column under it (under its objective and its button);
            // the endings in a column after the last step
            std::vector<S32> sideRow(m + 1, 0);
            S32 endRow = 0;
            for (FlowNode& n : mFlowNodes)
            {
                if (n.mEnding)
                {
                    n.mX = (F32)m * (BOX_W + GAP_X);
                    n.mY = (F32)(endRow++) * (BOX_H + GAP_Y);
                    continue;
                }
                if (n.mMain)
                {
                    n.mX = (F32)(n.mStep - 1) * (BOX_W + GAP_X);
                    n.mY = 0.f;
                    continue;
                }
                const S32 o = llclamp(n.mOwner, 1, m);
                ++mFlowNodes[mains[o - 1]].mSides;
                const bool open = mFlowShowAll || mFlowOpenSteps.count(mFlowNodes[mains[o - 1]].mId) > 0;
                n.mHidden = !open;
                n.mX = (F32)(o - 1) * (BOX_W + GAP_X);
                n.mY = BOX_H + MAIN_UNDER + (F32)(sideRow[o]++) * (BOX_H + GAP_Y);
            }
        }
    }
    const LLSD& saved = pk["flow"];
    if (saved.isMap())
        for (FlowNode& n : mFlowNodes)
        {
            if (n.mMain) continue;   // [GAME CARD 2026-10-18] the main quest stays one straight line; side boxes and endings may move
            const LLSD& at = saved[flowKey(n)];
            if (!at.isMap() || !(at["x"].isReal() || at["x"].isInteger()) || !(at["y"].isReal() || at["y"].isInteger())) continue;
            n.mX = (F32)at["x"].asReal();
            n.mY = (F32)at["y"].asReal();
        }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// drawing the flowchart
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::drawFlow(F32 l, F32 r, F32 top, F32 h, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    flowLayout();
    // [GAME CARD 2026-10-18] the box chosen (a click elsewhere, Next place, the helper) is never hidden behind "+N side paths":
    // its main step opens
    // (once per selection: "Hide side paths" pressed afterwards is not undone)
    if (mFlowMainMode && !mFlowSel.empty() && mFlowAutoOpenedFor != mFlowSel)
    {
        mFlowAutoOpenedFor = mFlowSel;
        std::string stepId;
        for (const FlowNode& n : mFlowNodes)
            if (!n.mEnding && n.mId == mFlowSel && n.mHidden)
                for (const FlowNode& m : mFlowNodes) if (m.mMain && m.mStep == n.mOwner) stepId = m.mId;
        if (!stepId.empty())
        {
            mFlowOpenSteps.insert(stepId);
            mFlowGen = 0;
            flowLayout();
        }
    }
    const F32 y = top;
    const F32 b = y - h;
    mFlowRect = LLRect((S32)l, (S32)y, (S32)r, (S32)b);
    card(l, b, r, y, p, 4.f);
    // the ground: drag to pan (registered first, so every box is on top of it)
    hit(l, b, r, y, SH_FLOW_BG, 0, "Drag to move round the map; the mouse wheel zooms; click a box to set it up", false);
    if (mFlowNodes.empty())
    {
        para(l + 12.f, r - 12.f, y - 12.f, "No boxes yet. Go to Story and press Build my game, or tell the helper your idea on the right.", p.mDim, true);
        return b - 8.f;
    }
    // [SIMPLE STUDIO 2026-10-17] whenever the boxes or the frame change, the map is shown from its top left at its natural size
    // (Paul: "i can't read the text on the box it's tiny" - never fitted smaller than readable; a map too big for the frame is
    // dragged round, or the + / - buttons and the wheel zoom it); a drag or the wheel then moves it until the next change
    {
        const F32 aw = r - l - 28.f, ah = h - 28.f;
        if (mFlowFitGen != mFlowGen || mFlowFitW != aw || mFlowFitH != ah)
        {
            mFlowFitGen = mFlowGen;
            mFlowFitW = aw;
            mFlowFitH = ah;
            flowFit();
        }
    }
    const F32 z = mFlowZoom;
    // the box's words grow with the zoom (the chart's own units scale; a font does not): the Studio's small font at natural
    // size, the body font from 1.2x, the big one from 1.7x
    const LLFontGL* bf = z >= 1.7f ? LLFontGL::getFontSansSerifBig() : (z >= 1.2f ? LLFontGL::getFontSansSerif() : small);
    const LLFontGL* bbf = z >= 1.2f ? LLFontGL::getFontSansSerifBold() : bold;
    const F32 blh = (F32)bf->getLineHeight();
    auto sx = [&](F32 x) { return l + 14.f + x * z + mFlowPanX; };
    auto sy = [&](F32 yy) { return y - 14.f - yy * z + mFlowPanY; };
    {
        LLLocalClipRect clip(LLRect((S32)l + 2, (S32)y - 2, (S32)r - 2, (S32)b + 2));
        // [GAME CARD 2026-10-18] what is drawn in the map is clicked in the map only: a box (or a step's button) panned past the frame
        // never takes a click meant for the band above or the panel below (hit() clips to mPageRect, narrowed here, put back after)
        const LLRect pageRect = mPageRect;
        mPageRect.intersectWith(LLRect((S32)l + 2, (S32)y - 2, (S32)r - 2, (S32)b + 2));
        struct PageBack { LLRect& mR; LLRect mWas; ~PageBack() { mR = mWas; } } pageBack{ mPageRect, pageRect };
        // arrows under the boxes: source's right middle to target's left middle; a back arrow bends round above.
        // [MAP LABELS 2026-10-17] Paul: two arrows leaving one box had their words drawn on the same spot, on top of each
        // other and over the lines. Each arrow's words are placed along its own line (the k-th arrow out of a box at
        // 30% + k x 20% of the way along it, so arrows sharing a box spread out), drawn after the boxes on a small plate of
        // the card colour so they read over the lines; words that would sit on other words or on a box move down a line at a
        // time until clear (bounded), with a hairline back to their arrow when moved
        struct Label { std::string mText; F32 mX = 0.f, mY = 0.f, mMinX = 0.f, mMaxX = 0.f; };   // the anchor, and the run the plate may slide along
        std::vector<Label> labels;
        std::map<S32, S32> outCount;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // [GAME CARD 2026-10-18] the main quest: ONE thick line through its steps in order, under every arrow and box
        if (mFlowMainMode)
        {
            const FlowNode* prev = nullptr;
            for (S32 k = 1; ; ++k)
            {
                const FlowNode* cur = nullptr;
                for (const FlowNode& n : mFlowNodes) if (n.mMain && n.mStep == k) cur = &n;
                if (!cur) break;
                if (prev)
                    lineW(sx(prev->mX + BOX_W), sy(prev->mY + BOX_H * 0.5f), sx(cur->mX), sy(cur->mY + BOX_H * 0.5f), llmax(3.f, 7.f * z), fade(p.mAccent, 0.75f));
                prev = cur;
            }
        }
        for (const FlowEdge& e : mFlowEdges)
        {
            const FlowNode& a = mFlowNodes[e.mFrom];
            const FlowNode& c = mFlowNodes[e.mTo];
            if (a.mHidden || c.mHidden) continue;   // [GAME CARD 2026-10-18] a side path not opened: its arrows wait with it
            const F32 x1 = sx(a.mX + BOX_W), y1 = sy(a.mY + BOX_H * 0.5f);
            const F32 x2 = sx(c.mX), y2 = sy(c.mY + BOX_H * 0.5f);
            const LLColor4 col = fade(p.mDim, 0.9f);
            auto seg = [&](F32 ax, F32 ay, F32 bx, F32 by)
            {
                if (!e.mDashed) { lineW(ax, ay, bx, by, 1.5f, col); return; }
                const F32 len = std::hypot(bx - ax, by - ay);
                for (F32 t = 0.f; t < len; t += 10.f)
                {
                    const F32 t2 = llmin(len, t + 6.f);
                    lineW(ax + (bx - ax) * t / len, ay + (by - ay) * t / len, ax + (bx - ax) * t2 / len, ay + (by - ay) * t2 / len, 1.5f, col);
                }
            };
            // the arrow's line as the points it bends at, from the source to the target; its words go on the segments
            // [wordsFrom, wordsTo): the whole line of a forward arrow, the run over the boxes of a loop
            std::vector<std::pair<F32, F32>> pts;
            size_t wordsFrom = 1, wordsTo;
            if (x2 > x1 + 4.f)
            {
                const F32 mx = (x1 + x2) * 0.5f;
                pts = { { x1, y1 }, { mx, y1 }, { mx, y2 }, { x2, y2 } };
                wordsTo = pts.size();
            }
            else
            {
                // backwards (a loop): up over the boxes and down into the target from its left
                const F32 up = llmax(y1, y2) + BOX_H * z * 0.5f + 12.f;
                pts = { { x1, y1 }, { x1 + 12.f, y1 }, { x1 + 12.f, up }, { x2 - 12.f, up }, { x2 - 12.f, y2 }, { x2, y2 } };
                wordsFrom = 3; wordsTo = 4;
            }
            F32 len = 0.f;
            for (size_t i = 1; i < pts.size(); ++i)
            {
                seg(pts[i - 1].first, pts[i - 1].second, pts[i].first, pts[i].second);
                if (i >= wordsFrom && i < wordsTo) len += std::hypot(pts[i].first - pts[i - 1].first, pts[i].second - pts[i - 1].second);
            }
            // the arrow's head (pointing right, into the target's left side)
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(col.mV);
            gGL.vertex2f(x2, y2); gGL.vertex2f(x2 - 7.f, y2 + 4.f); gGL.vertex2f(x2 - 7.f, y2 - 4.f);
            gGL.end();
            if (!e.mLabel.empty() && z >= 0.7f && len > 0.f)
            {
                // the words' anchor: the point 30% + k x 20% (at most 90%) of the way along this arrow, k = how many arrows
                // already left the same box
                const S32 k = outCount[e.mFrom]++;
                F32 want = llmin(0.9f, 0.3f + 0.2f * (F32)k) * len;
                Label lb;
                lb.mText = e.mLabel;
                lb.mX = pts[wordsTo - 1].first; lb.mY = pts[wordsTo - 1].second;
                lb.mMinX = llmin(pts[wordsFrom - 1].first, pts[wordsTo - 1].first);
                lb.mMaxX = llmax(pts[wordsFrom - 1].first, pts[wordsTo - 1].first);
                for (size_t i = wordsFrom; i < wordsTo; ++i)
                {
                    const F32 sl = std::hypot(pts[i].first - pts[i - 1].first, pts[i].second - pts[i - 1].second);
                    if (want <= sl)
                    {
                        const F32 f = sl > 0.f ? want / sl : 0.f;
                        lb.mX = pts[i - 1].first + (pts[i].first - pts[i - 1].first) * f;
                        lb.mY = pts[i - 1].second + (pts[i].second - pts[i - 1].second) * f;
                        break;
                    }
                    want -= sl;
                }
                labels.push_back(lb);
            }
        }
        // the boxes: amber while not placed, green once placed (and the words say so: never colour alone); endings in the accent
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
        {
            const FlowNode& n = mFlowNodes[i];
            if (n.mHidden) continue;   // [GAME CARD 2026-10-18] behind its main step's "+N side paths"
            const F32 bl = sx(n.mX), bt = sy(n.mY), br = bl + BOX_W * z, bb = bt - BOX_H * z;
            // [GAME CARD 2026-10-18] a main step's objective and button hang under it: drawn while any of that is in view
            if (br < l || bl > r || bt < b || (bb - (n.mMain ? MAIN_UNDER * z : 0.f)) > y) continue;
            const bool sel = n.mEnding ? (!mFlowSelEnd.empty() && mFlowSelEnd == n.mId) : (!mFlowSel.empty() && mFlowSel == n.mId);
            const S32 np = problemsUnder(n.mPtr);
            // [GAME CARD 2026-10-18] endings: green a win, red a loss, grey any other (and the words WIN / LOSE / END say it too)
            const LLColor4 endTint = n.mKind == "win" ? OK_INK : n.mKind == "lose" ? DANGER_INK : p.mDim;
            const LLColor4 tint = n.mEnding ? endTint : (n.mPlaced ? OK_INK : WARN_INK);
            const F32 dim = n.mBuilt ? 1.f : 0.45f;   // a chapter not built yet: drawn dimmed, "Not built yet"
            chamfer(bl, bb, br, bt, 5.f, fade(tint, 0.26f * dim), fade(tint, 0.16f * dim));
            chamferEdge(bl + 0.5f, bb + 0.5f, br - 0.5f, bt - 0.5f, 5.f, sel ? 2.5f : (n.mMain ? 2.f : 1.f),
                        sel ? p.mAccent : (np ? WARN_INK : (n.mMain ? fade(p.mAccent, dim) : fade(tint, 0.7f * dim))));
            // the box's kind: a drawn icon at its top right (the tooltip and the box panel say the word)
            if (z >= 0.55f) kindIcon(n.mKind, br - 12.f * z, bt - 12.f * z, 14.f * z, n.mEnding ? endTint : fade(p.mText, 0.85f * dim));
            if (n.mMain)
            {
                // the step's number on a disc at the box's top left
                const F32 rr = llmax(8.f, 11.f * z);
                disc(bl, bt, rr, p.mAccent);
                // dark ink on the light accent disc (the palette's text is light, for the dark cards)
                textL(bbf, llformat("%d", n.mStep), bl, bt, LLColor4(0.06f, 0.06f, 0.08f, 1.f), LLFontGL::HCENTER, LLFontGL::VCENTER);
                // under the box: the step's objective, then "+N side paths" (or Hide them) when it has any
                F32 uy = bb - 4.f;
                if (z >= 0.55f && !n.mObjective.empty())
                    for (const std::string& ln : wrap(small, n.mObjective, BOX_W * z, 2)) { textL(small, ln, bl, uy, fade(p.mDim, dim)); uy -= slh; }
                if (n.mSides > 0 && !mFlowShowAll)
                {
                    const bool open = mFlowOpenSteps.count(n.mId) > 0;
                    const std::string lab = open ? "Hide side paths" : llformat("+%d side path%s", n.mSides, n.mSides == 1 ? "" : "s");
                    const std::string sid = n.mId;
                    const S32 is = act([this, sid, open]()
                    {
                        if (open) mFlowOpenSteps.erase(sid); else mFlowOpenSteps.insert(sid);
                        mFlowGen = 0;   // laid out again (the same draft: the view is not re-fitted)
                    });
                    const F32 bw = llmin(BOX_W * z, btnW(lab));
                    btn(bl, uy - 4.f - 22.f, bl + bw, uy - 4.f, lab, SH_E, is,
                        open ? "Hide the boxes that branch off this step" : "Show the boxes that branch off this step and come back to the main quest", p);
                }
            }
            if (z >= 0.55f)
            {
                F32 ty = bt - 5.f;
                std::string head = n.mEnding ? (n.mKind == "win" ? "WIN" : n.mKind == "lose" ? "LOSE" : "END")
                                             : (n.mStart ? "START" : std::string());
                if (n.mMain) head = llformat("STEP %d", n.mStep) + (n.mStart ? " \xC2\xB7 START" : "");
                if (!n.mFight.empty()) head += (head.empty() ? "" : " \xC2\xB7 ") + std::string("FIGHT: ") + n.mFight;   // [M4] words, not colour alone
                if (!head.empty()) { textL(bbf, head, bl + (n.mMain ? 14.f : 6.f), ty, n.mEnding ? endTint : (n.mFight.empty() ? (n.mMain ? p.mAccent : OK_INK) : WARN_INK), LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 34.f); ty -= blh; }
                // [MAP LABELS 2026-10-17] the title gets two lines whenever two fit above "Where:" and the place line (the box's
                // padding: 5 above, 4 below the place line, 2 spare), one only when they do not - never cut mid-word while a
                // second line fits; an ending has no "Where:" or place line
                S32 room = (S32)std::floor((BOX_H * z - 5.f - 4.f - 2.f - (n.mEnding ? 0.f : blh)) / blh);
                if (!head.empty()) --room;
                if (!n.mEnding && !n.mWhere.empty()) --room;
                for (const std::string& ln : wrap(bf, n.mTitle.empty() ? std::string("(no words yet)") : n.mTitle, BOX_W * z - (head.empty() ? 30.f : 12.f), llclamp(room, 1, 2)))
                { textL(bf, ln, bl + 6.f, ty, fade(p.mText, n.mBuilt ? 1.f : 0.7f)); ty -= blh; }
                if (!n.mEnding)
                {
                    if (!n.mWhere.empty()) { textL(bf, "Where: " + n.mWhere, bl + 6.f, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 12.f); ty -= blh; }
                    if (!n.mBuilt) textL(bf, "Not built yet", bl + 6.f, bb + 4.f + blh, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 30.f);
                    else textL(bf, n.mPlaced && !n.mPlace.empty() ? "Placed: " + n.mPlace : std::string("not placed yet"),
                               bl + 6.f, bb + 4.f + blh, n.mPlaced && !n.mPlace.empty() ? OK_INK : WARN_INK, LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 30.f);
                }
            }
            if (np > 0)
            {
                // a warning triangle AND the number (never colour alone)
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                gGL.begin(LLRender::TRIANGLES);
                gGL.color4fv(WARN_INK.mV);
                gGL.vertex2f(br - 22.f, bb + 4.f); gGL.vertex2f(br - 10.f, bb + 4.f); gGL.vertex2f(br - 16.f, bb + 14.f);
                gGL.end();
                textL(bold, llformat("%d", np), br - 8.f, bb + 4.f, WARN_INK, LLFontGL::LEFT, LLFontGL::BOTTOM);
            }
            std::string tip = (n.mEnding ? (n.mKind == "win" ? "Winning ending: " : n.mKind == "lose" ? "Losing ending: " : "Ending: ")
                                         : (n.mMain ? llformat("Main quest step %d (%s): ", n.mStep, kindWord(n.mKind).c_str())
                                                    : "Box (" + kindWord(n.mKind) + "): ")) + (n.mTitle.empty() ? n.mId : n.mTitle);
            if (!n.mEnding) tip += !n.mBuilt ? " - not built yet" : n.mPlaced ? " - placed" : " - not placed yet";
            if (np) tip += llformat(" - %d thing%s to look at (click it, then see below)", np, np == 1 ? "" : "s");
            hit(bl, bb, br, bt, SH_FLOW_BOX, i, tip + ". Click to set it up; drag to move it");
            if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_FLOW_BOX, i)) focusRing(bl, bb, br, bt, p);
            if (!n.mEnding)
            {
                // the handle: drag from it to another box to make this one lead there
                const F32 hx = br, hy = (bt + bb) * 0.5f;
                disc(hx, hy, 5.f, mFlowConnectFrom == i ? p.mAccent : p.mText);
                hit(hx - 7.f, hy - 7.f, hx + 7.f, hy + 7.f, SH_FLOW_HANDLE, i, "Drag from here to another box: this box then leads there", false);
            }
        }
        // the arrows' words, on top of the boxes (a plate under each, so they read over lines and edges)
        if (!labels.empty())
        {
            struct Box { F32 mL, mB, mR, mT; };
            std::vector<Box> placed;
            for (const FlowNode& n : mFlowNodes) if (!n.mHidden) placed.push_back({ sx(n.mX), sy(n.mY) - BOX_H * z - (n.mMain ? MAIN_UNDER * z : 0.f), sx(n.mX) + BOX_W * z, sy(n.mY) });
            const LLColor4 plate(p.mCard.mV[VRED], p.mCard.mV[VGREEN], p.mCard.mV[VBLUE], 0.85f);
            const F32 th = slh + 4.f;
            for (const Label& lb : labels)
            {
                const F32 tw = llmin(LABEL_W * z, small->getWidthF32(lb.mText)) + 8.f;
                // centred on the anchor, slid along the arrow's run to stay between the boxes when it fits there
                F32 pl = lb.mX - tw * 0.5f;
                if (tw <= lb.mMaxX - lb.mMinX - 4.f) pl = llclamp(pl, lb.mMinX + 2.f, lb.mMaxX - 2.f - tw);
                const F32 pr = pl + tw;
                F32 pt = lb.mY + th * 0.5f, pb = pt - th;
                // down a line at a time while it sits on a box or on words already placed (at most eight lines)
                for (S32 tries = 0; tries < 8; ++tries)
                {
                    bool clash = false;
                    for (const Box& o : placed) if (pl < o.mR && pr > o.mL && pb < o.mT && pt > o.mB) { clash = true; break; }
                    if (!clash) break;
                    pt -= th; pb -= th;
                }
                placed.push_back({ pl, pb, pr, pt });
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                if (pt < lb.mY) lineW(lb.mX, lb.mY, lb.mX, pt, 1.f, fade(p.mDim, 0.9f));   // moved down: a hairline back to its arrow
                chamfer(pl, pb, pr, pt, 3.f, plate, plate);
                textL(small, lb.mText, pl + 4.f, pt - 2.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, tw - 8.f);
            }
        }
        // a connection being dragged
        if (mFlowConnectFrom >= 0 && mFlowConnectFrom < (S32)mFlowNodes.size())
        {
            const FlowNode& a = mFlowNodes[mFlowConnectFrom];
            lineW(sx(a.mX + BOX_W), sy(a.mY + BOX_H * 0.5f), (F32)mFlowMouseX, (F32)mFlowMouseY, 2.f, p.mAccent);
        }
    }
    // [SIMPLE STUDIO 2026-10-17] + / - / Fit / Tidy up at the map's top right (after the boxes: on top of them). + and - zoom
    // about the frame's middle; Fit = natural size from the top left; Tidy up puts every moved box back (asks first)
    {
        const S32 cx = (S32)((l + r) * 0.5f), cy = (S32)((y + b) * 0.5f);
        F32 x = r - 8.f;
        auto small_btn = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, F32 min_w)
        {
            const F32 bw = btnW(lab, min_w);
            const S32 idx = act(fn);
            btn(x - bw, y - 8.f - 24.f, x, y - 8.f, lab, SH_E, idx, tip, p, enabled);
            x -= bw + 4.f;
        };
        // [GAME CARD 2026-10-18] "Show everything": every side path open at once (or back to the main quest only)
        if (mFlowMainMode)
            small_btn(mFlowShowAll ? "Main quest only" : "Show everything", mFlowShowAll ? "Hide the side paths again: only the main quest's steps and the endings"
                                                                                  : "Show every box, the side paths too",
                      [this]() { mFlowShowAll = !mFlowShowAll; mFlowGen = 0; }, true, 0.f);
        small_btn("Tidy up", "Every box back to its automatic place (asks first)", [this]() { ask(SC_FLOW_TIDY, 0); }, std::as_const(mDraftPack)["flow"].isMap() && std::as_const(mDraftPack)["flow"].size() > 0, 0.f);
        small_btn("Fit", "The map at its natural size, from the top left", [this]() { flowFit(); }, true, 44.f);
        small_btn("\xE2\x88\x92", "Zoom out (the mouse wheel over the map zooms too)", [this, cx, cy]() { flowZoomAt(1.f / 1.25f, cx, cy); }, mFlowZoom > 0.36f, 32.f);
        small_btn("+", "Zoom in: bigger boxes and bigger words (the mouse wheel over the map zooms too)", [this, cx, cy]() { flowZoomAt(1.25f, cx, cy); }, mFlowZoom < 2.99f, 32.f);
        textL(small, llformat("%d%%", (S32)(z * 100.f + 0.5f)), x, y - 8.f - 12.f, p.mDim, LLFontGL::RIGHT, LLFontGL::VCENTER);
    }
    return b - 8.f;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [SIMPLE STUDIO 2026-10-17] zoom, fit, moving boxes (Paul: "i need to be able to zoom in", "move the boxes around")
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::flowZoomAt(F32 factor, S32 x, S32 y)
{
    // the chart point under (x, y) stays under it: view x = l + 14 + cx * z + panX, view y = top - 14 - cy * z + panY (drawFlow's
    // sx / sy), so the pan is re-set for the new zoom from the same chart point
    const F32 z0 = mFlowZoom;
    const F32 z1 = llclamp(z0 * factor, 0.35f, 3.f);
    if (z1 == z0) return;
    const F32 l = (F32)mFlowRect.mLeft, top = (F32)mFlowRect.mTop;
    const F32 cx = ((F32)x - l - 14.f - mFlowPanX) / z0;
    const F32 cy = (top - 14.f + mFlowPanY - (F32)y) / z0;
    mFlowZoom = z1;
    mFlowPanX = (F32)x - l - 14.f - cx * z1;
    mFlowPanY = (F32)y - top + 14.f + cy * z1;
}

void WolfAdventureStudioView::flowFit()
{
    // natural size, never smaller: the chart's units are drawn 1:1 and the Studio's small font is readable; a chart larger than
    // the frame is dragged round or zoomed out by hand (Paul: "i can't read the text on the box it's tiny")
    mFlowZoom = 1.f;
    mFlowPanX = mFlowPanY = 0.f;
}

void WolfAdventureStudioView::flowSavePos(const std::string& key, F32 x, F32 y)
{
    // Source: adventure_pack_v1.schema.json properties.flow: "b:<beat id>" / "e:<ending id>" -> {x, y} (numbers within +-100000);
    // the draft is edited in place (editDoc, wolffloateradventurestudioai.cpp:326-357) and saved by the autosave 2 s later
    // (autosaveTick, AUTOSAVE_SEC) - one save per drag
    const F64 px = llclamp((F64)llround(x), -100000.0, 100000.0), py = llclamp((F64)llround(y), -100000.0, 100000.0);
    const bool ok = editDoc([&key, px, py](boost::json::value& root)
    {
        if (!root.is_object()) return false;
        boost::json::value& fl = root.as_object()["flow"];
        if (!fl.is_object()) fl = boost::json::object();
        fl.as_object()[key] = boost::json::object{ { "x", px }, { "y", py } };
        return true;
    });
    if (!ok) return;
    // the boxes follow the draft (flowLayout reads the saved place); the view stays where it is, not re-fitted
    flowLayout();
    mFlowFitGen = mFlowGen;
}

void WolfAdventureStudioView::flowTidy()
{
    const bool ok = editDoc([](boost::json::value& root)
    {
        if (!root.is_object() || !root.as_object().contains("flow")) return false;
        root.as_object().erase("flow");
        return true;
    });
    if (!ok) return;
    flowLayout();
    setStatus("Tidied up: every box is back in its automatic place.", false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [GAME CARD 2026-10-18] the game card (GAME_CARD_SPINE_SPEC.md section 5, and the roleplay change of 2026-10-18)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::gameCard(F32 l, F32 r, F32 top, const Palette& p)
{
    // Persona and story (GAME_CARD_SPINE_SPEC.md): a resident who has never made a game sees, at the top of pages 2 and 3, a card that
    // says who they are, what they must do and how they win - each line a box they can type in, saved like every other change.
    // Source: GAME_CARD_SPINE_SPEC.md section 1, adventure.card {youAre text240, goal text240, win text240, lose text240 ("" = you
    // cannot lose), shape, length short|medium|long, players solo|party|either, genre text64}; the roleplay change: shape `roleplay`
    // shows "What it's about" for goal and hides Win / Lose.
    const LLSD& gc = std::as_const(mDraftPack)["adventure"]["card"];
    const bool rp = isRoleplay();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* sbold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const std::string goal = trimmed(gc["goal"].asString());
    // the fields: drawn once to measure (the card's frame goes under them), so the draw happens inside the frame afterwards; the
    // frame is a plain card drawn first at the height the last frame measured
    static F32 sLastH[SP_COUNT] = {};
    const F32 pad = 12.f;
    if (sLastH[mPage] > 0.f) card(l, top - sLastH[mPage], r, top, p, 8.f);
    F32 y = top - pad;
    textL(sbold, rp ? "YOUR GAME CARD - JUST ROLEPLAY" : "YOUR GAME CARD", l + pad, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l - 2.f * pad);
    y -= slh + 2.f;
    y = para(l + pad, r - pad, y, rp ? "What your world is, in a few lines. There is no goal and no winning: people come to play a part. Click a line to change it."
                                     : "What your game is, in a few lines: who the players are, what they must do and how they win. Click a line to change it.",
             p.mDim, true) - 6.f;
    if (!rp && goal.empty())
        y = para(l + pad, r - pad, y, "Your game has no goal yet - say what the players must do.", WARN_INK) - 4.f;
    const bool two = r - l >= 760.f;
    const F32 mid = two ? l + (r - l) * 0.56f : r;
    const F32 lcL = l + pad, lcR = (two ? mid - 10.f : r - pad);
    const F32 rcL = two ? mid + 6.f : l + pad, rcR = r - pad;
    F32 ly = y;
    ly = fText(lcL, lcR, ly, "You are", "/adventure/card/youAre", gc["youAre"], 0, 240, 's', true, "Who the players are in your game, e.g. \"A deputy new in town\"", p);
    ly = fText(lcL, lcR, ly, rp ? "What it's about" : "Goal", "/adventure/card/goal", gc["goal"], 0, 240, 's', true,
               rp ? "What your world is about, e.g. \"Life in a frontier town\"" : "The one thing the players must do, e.g. \"Get the payroll gold back before sundown\"", p);
    if (!rp)
    {
        ly = fText(lcL, lcR, ly, "How you win", "/adventure/card/win", gc["win"], 0, 240, 's', true, "What winning is, e.g. \"The gold is back in the bank\"", p);
        ly = fText(lcL, lcR, ly, "How you lose", "/adventure/card/lose", gc["lose"], 0, 240, 's', true,
                   "What losing is - leave it empty if you can't lose and just keep trying", p);
        if (trimmed(gc["lose"].asString()).empty()) ly = para(lcL + llmin(150.f, (lcR - lcL) * 0.4f), lcR, ly + 4.f, "Empty: you can't lose, you just keep trying.", p.mDim, true) - 4.f;
    }
    F32 ry = two ? y : ly;
    auto choices = [&](const std::string& label, const std::string& key, const std::vector<std::pair<std::string, std::string>>& opts)
    {
        const F32 lw = llmin(150.f, (rcR - rcL) * 0.4f);
        textL(small, label, rcL, ry - 6.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, lw - 6.f);
        const std::string cur = gc[key].asString();
        F32 x = rcL + lw;
        for (const auto& o : opts)
        {
            const F32 bw = btnW(o.second);
            if (x + bw > rcR && x > rcL + lw) { x = rcL + lw; ry -= 26.f + 4.f; }
            const bool on = cur == o.first;
            const std::string val = o.first, ptr = "/adventure/card/" + key;
            const S32 i = act([this, ptr, val, on]() { if (on) docErase(0, ptr); else docSet(0, ptr, boost::json::value(val)); });
            btn(x, ry - 26.f, x + bw, ry, o.second, SH_E, i, label + ": " + o.second + (on ? " (chosen - press again to clear)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += bw + 4.f;
        }
        ry -= 26.f + 8.f;
    };
    choices("How long", "length", { { "short", "About 15 minutes" }, { "medium", "About 45 minutes" }, { "long", "2 hours or more" } });
    choices("Players", "players", { { "solo", "One player" }, { "party", "A group" }, { "either", "Either" } });
    ry = fText(rcL, rcR, ry, "Type of story", "/adventure/card/genre", gc["genre"], 0, 64, 's', true, "The kind of story, e.g. \"Wild West\" or \"Fantasy\"", p);
    {
        // Just roleplay: shape roleplay; turned off, the shape goes (the game is a game with a goal again)
        const F32 lw = llmin(150.f, (rcR - rcL) * 0.4f);
        textL(small, "Just roleplay", rcL, ry - 6.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, lw - 6.f);
        const S32 i = act([this, rp]()
        {
            if (rp) docErase(0, "/adventure/card/shape");
            else docSet(0, "/adventure/card/shape", boost::json::value("roleplay"));
        });
        btn(rcL + lw, ry - 26.f, rcL + lw + 90.f, ry, rp ? "Yes" : "No", SH_E, i,
            rp ? "Just roleplay: no goal and no winning - press to make it a game with a goal" : "A game with a goal - press for just roleplay (no goal, no winning)",
            p, true, rp ? BTN_ACTIVE : 0);
        textL(small, rp ? "No goal, no winning." : "It has a goal to win.", rcL + lw + 98.f, ry - 13.f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, rcR - rcL - lw - 98.f);
        ry -= 26.f + 8.f;
    }
    const F32 bottom = llmin(ly, ry) - pad + 6.f;
    sLastH[mPage] = top - bottom;
    return bottom - 10.f;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [GAME CARD 2026-10-18] the kinds of box (GAME_CARD_SPINE_SPEC.md section 1) and the roleplay shape
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventureStudioView::isRoleplay() const
{
    // Source: GAME_CARD_SPINE_SPEC.md "Change (Paul, 2026-10-18)": adventure.card.shape gains `roleplay`
    return std::as_const(mDraftPack)["adventure"]["card"]["shape"].asString() == "roleplay";
}

// static
std::string WolfAdventureStudioView::kindWord(const std::string& kind)
{
    if (kind == "character") return "Character";
    if (kind == "challenge") return "Challenge";
    if (kind == "item") return "Item";
    if (kind == "gate") return "Gate";
    if (kind == "win") return "Win";
    if (kind == "lose") return "Lose";
    if (kind == "other") return "Other ending";
    return "Place";
}

// static
std::string WolfAdventureStudioView::beatKind(const LLSD& bt)
{
    // Source: GAME_CARD_SPINE_SPEC.md section 1 (the brain's wa_beat_kind, which fills `kind` in on every save; this is only for a
    // draft saved before that): a fight (an onEnter battle step, a won / lost way on) or a quiz -> challenge; else every way on
    // guarded by an `if` -> gate; else a touch -> item; else a talk / say or someone on duty -> character; else place.
    // The trigger kinds are adventure_pack_v1.schema.json $defs.trigger.on; a battle step is onEnter[].battle (was_battle_op).
    // Source: wolfadventure_lib.php wa_beat_kind (834-850) and wa_is_placeholder (798-800), mirrored line for line
    for (const LLSD& e : llsd::inArray(bt["onEnter"])) if (e.has("battle") || e.has("quiz")) return "challenge";
    S32 ways = 0, guarded = 0;
    bool touch = false, talk = false;
    for (const LLSD& t : llsd::inArray(bt["triggers"]))
    {
        const std::string on = t["on"].asString();
        if (on == "won" || on == "lost" || on == "quiz" || on == "defeat") return "challenge";
        bool leads = t.has("next");
        for (const LLSD& e : llsd::inArray(t["do"])) if (e.has("goto") || e.has("end")) leads = true;
        const bool placeholder = on == "gm" && t["id"].asString().rfind("tbd-", 0) == 0;   // "still to decide"
        if (leads && !placeholder) { ++ways; if (t.has("if")) ++guarded; }
        if (on == "touch") touch = true;
        if (on == "talk" || on == "say") talk = true;
    }
    if (ways > 0 && guarded == ways) return "gate";
    if (touch) return "item";
    if (talk || (bt["onDuty"].isArray() && bt["onDuty"].size() > 0)) return "character";
    return "place";
}

// static
void WolfAdventureStudioView::kindIcon(const std::string& kind, F32 cx, F32 cy, F32 s, const LLColor4& c)
{
    // drawn shapes (never an emoji font, which a viewer may not have): a pin, a person, two crossed swords, a gem, a padlock;
    // for endings a flag (win), a cross (lose), a ring (other)
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    auto tri = [&c](F32 x1, F32 y1, F32 x2, F32 y2, F32 x3, F32 y3)
    {
        gGL.begin(LLRender::TRIANGLES);
        gGL.color4fv(c.mV);
        gGL.vertex2f(x1, y1); gGL.vertex2f(x2, y2); gGL.vertex2f(x3, y3);
        gGL.end();
    };
    const F32 h = s * 0.5f;
    const F32 w = llmax(1.5f, s * 0.13f);
    if (kind == "character")
    {
        disc(cx, cy + h * 0.45f, h * 0.36f, c, 20);
        chamfer(cx - h * 0.72f, cy - h, cx + h * 0.72f, cy - h * 0.05f, h * 0.35f, c, c);
    }
    else if (kind == "challenge")
    {
        lineW(cx - h, cy - h, cx + h, cy + h, w, c);
        lineW(cx + h, cy - h, cx - h, cy + h, w, c);
        // the guards
        lineW(cx - h * 0.85f, cy - h * 0.25f, cx - h * 0.25f, cy - h * 0.85f, w, c);
        lineW(cx + h * 0.85f, cy - h * 0.25f, cx + h * 0.25f, cy - h * 0.85f, w, c);
    }
    else if (kind == "item")
    {
        tri(cx, cy + h, cx + h * 0.75f, cy, cx - h * 0.75f, cy);
        tri(cx - h * 0.75f, cy, cx + h * 0.75f, cy, cx, cy - h);
    }
    else if (kind == "gate")
    {
        rect(cx - h * 0.7f, cy - h, cx + h * 0.7f, cy + h * 0.1f, c);
        lineW(cx - h * 0.42f, cy + h * 0.1f, cx - h * 0.42f, cy + h * 0.7f, w, c);
        lineW(cx + h * 0.42f, cy + h * 0.1f, cx + h * 0.42f, cy + h * 0.7f, w, c);
        lineW(cx - h * 0.42f - w * 0.5f, cy + h * 0.7f, cx + h * 0.42f + w * 0.5f, cy + h * 0.7f, w, c);
    }
    else if (kind == "win")
    {
        lineW(cx - h * 0.6f, cy - h, cx - h * 0.6f, cy + h, w, c);
        tri(cx - h * 0.6f, cy + h, cx + h, cy + h * 0.45f, cx - h * 0.6f, cy - h * 0.1f);
    }
    else if (kind == "lose")
    {
        lineW(cx - h * 0.8f, cy - h * 0.8f, cx + h * 0.8f, cy + h * 0.8f, w * 1.3f, c);
        lineW(cx + h * 0.8f, cy - h * 0.8f, cx - h * 0.8f, cy + h * 0.8f, w * 1.3f, c);
    }
    else if (kind == "other")
    {
        disc(cx, cy, h * 0.7f, c, 20);
    }
    else
    {
        // place: a map pin
        disc(cx, cy + h * 0.3f, h * 0.6f, c, 20);
        tri(cx - h * 0.52f, cy + h * 0.05f, cx + h * 0.52f, cy + h * 0.05f, cx, cy - h);
    }
}

F32 WolfAdventureStudioView::flowBoxPanel(F32 l, F32 r, F32 top, const Palette& p)
{
    // [SIMPLE STUDIO 2026-10-17] the box the author clicked (SIMPLE_STUDIO_SPEC.md page 3): ONLY Place it here, Bring <character>
    // here, What happens here? (the helper box + Send, about this box), Leads to..., and More... (the full box tools)
    F32 y = top;
    S32 ni = -1;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding && mFlowNodes[i].mId == mFlowSel) ni = i;
    if (ni < 0 && !mFlowSelEnd.empty())
    {
        // [GAME CARD 2026-10-18] an ending: what kind it is (a win, a loss, another ending), and its full editor
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
            if (mFlowNodes[i].mEnding && mFlowNodes[i].mId == mFlowSelEnd)
            {
                const FlowNode& n = mFlowNodes[i];
                const std::string ep = n.mPtr;
                y = head(l, r, y, "This ending: " + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70), p);
                const std::string text = cap(WolfAdvStudio::sdAt(mDraftPack, ep)["text"].asString(), 2000);
                if (!text.empty()) y = para(l, r, y, text, p.mText) - 4.f;
                textL(LLFontGL::getFontSansSerifSmallBold(), "This ending is:", l, y - ROW * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
                F32 x = l + 110.f;
                static const char* EK[3] = { "win", "lose", "other" };
                static const char* EW[3] = { "Win", "Lose", "Other ending" };
                for (S32 k = 0; k < 3; ++k)
                {
                    const std::string kk = EK[k];
                    const bool on = n.mKind == kk;
                    const F32 bw = btnW(EW[k], 80.f);
                    // Source: GAME_CARD_SPINE_SPEC.md section 1: ending kind win | lose | other (absent = other)
                    const S32 ik = act([this, ep, kk]() { docSet(0, ep + "/kind", boost::json::value(kk)); });
                    btn(x, y - ROW, x + bw, y, EW[k], SH_E, ik, std::string("This ending is: ") + EW[k] + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
                    x += bw + 4.f;
                }
                y -= ROW + 8.f;
                const std::string pr = problemAt(ep);
                if (!pr.empty()) y = para(l, r, y, pr, DANGER_INK) - 4.f;
                const S32 io = act([this, ep]() { gotoProblem(ep + "/title"); });
                btn(l, y - ROW, l + btnW("Open the full editor"), y, "Open the full editor", SH_E, io, "Every setting of this ending on Advanced editing", p);
                return y - ROW - 8.f;
            }
        mFlowSelEnd.clear();
    }
    if (ni < 0)
    {
        return para(l, r, y, mFlowNodes.empty() ? std::string() : "Click a box to see what happens there.", p.mDim) - 6.f;
    }
    // Paul (2026-10-17): "maybe have the box contents under the view" - the box's contents, in the body font, nothing clipped
    // (the page scrolls): its words, where it happens, what happens, who is there, where it leads; then the buttons
    const FlowNode& n = mFlowNodes[ni];
    const std::string bp = n.mPtr;
    const LLSD& b = WolfAdvStudio::sdAt(mDraftPack, bp);
    const std::string scp = bp.substr(0, bp.rfind("/beats/"));
    const LLSD& sc = WolfAdvStudio::sdAt(mDraftPack, scp);
    y = head(l, r, y, "This box: " + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70), p);
    if (!n.mWhere.empty()) y = para(l, r, y, "Where: " + n.mWhere, p.mDim) - 2.f;
    y = para(l, r, y, n.mPlaced && !n.mPlace.empty() ? "Placed at " + n.mPlace + "." : "Not placed yet: stand where this happens and press Place it here.",
             n.mPlaced ? OK_INK : WARN_INK) - 4.f;
    // Source: adventure_pack_v1.schema.json $defs.beat {title (text64), objective (text240: the journal's objective line), aiNotes
    // (text1000)}; $defs.scene {title, boxed (text2000: read to the players when the part starts), beats}
    {
        const std::string objective = cap(b["objective"].asString(), 240);
        if (!objective.empty() && objective != n.mTitle) y = para(l, r, y, "What happens here: " + objective, p.mText) - 2.f;
        const std::string notes = cap(b["aiNotes"].asString(), 1000);
        if (!notes.empty()) y = para(l, r, y, notes, p.mDim) - 2.f;
        const std::string boxed = cap(sc["boxed"].asString(), 2000);
        if (!boxed.empty() && bp.size() > 8 && bp.compare(bp.size() - 8, 8, "/beats/0") == 0)
            y = para(l, r, y, "Read to the players when this part (" + cap(sc["title"].asString().empty() ? std::string("untitled") : sc["title"].asString(), 40) + ") starts: " + boxed, p.mText) - 2.f;
    }
    // who is there: the characters the box names (its ways on, its duties, its choices; wolfadventure_story_lib.php was_box_cast 1771-1779)
    std::set<std::string> ids;
    for (const LLSD& t : llsd::inArray(b["triggers"])) if (t.has("cast")) ids.insert(t["cast"].asString());
    for (const LLSD& d : llsd::inArray(b["onDuty"])) ids.insert(d["cast"].asString());
    for (const LLSD& d : llsd::inArray(b["decisions"])) if (d.has("by")) ids.insert(d["by"].asString());
    {
        std::string who;
        for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"]))
            if (ids.count(c["id"].asString())) who += (who.empty() ? "" : ", ") + cap(c["first"].asString() + (c["last"].asString().empty() ? "" : " " + c["last"].asString()), 30);
        y = para(l, r, y, who.empty() ? std::string("Nobody is named here yet: tell the helper who is there.") : "Who is there: " + who, who.empty() ? p.mDim : p.mText) - 2.f;
    }
    // where it leads, in words (each way on with its "when"), with the problems on it
    std::string leads;
    for (const FlowEdge& e : mFlowEdges)
        if (e.mFrom == ni)   // [WORDING 2026-10-18] as the web: "Only if <cond>: <words> -> "<box>"" (an ending: the ending "<title>")
            leads += (leads.empty() ? "" : "; ") + (e.mCond.empty() ? std::string() : "Only if " + e.mCond + ": ") + e.mFull + " \xE2\x86\x92 "
                   + (mFlowNodes[e.mTo].mEnding ? "the ending \"" + cap(mFlowNodes[e.mTo].mTitle, 64) + "\"" : "\"" + cap(mFlowNodes[e.mTo].mTitle, 64) + "\"");
    y = para(l, r, y, leads.empty() ? std::string("Nowhere yet - press Leads to... and say what finishes this box.") : "Where it leads: " + leads,
             leads.empty() ? WARN_INK : p.mText) - 4.f;
    const std::string pr = problemAt(bp + "/id").empty() ? problemAt(bp) : problemAt(bp + "/id");
    if (!pr.empty()) y = para(l, r, y, pr, DANGER_INK) - 4.f;
    if (!n.mBuilt) y = para(l, r, y, "This box's chapter isn't built yet: only its plan is here. Chapters are built in order - press Add the next chapter (above the map).", p.mDim) - 4.f;
    // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5: the box's kind (its icon) and whether it is a main quest step
    {
        const LLFontGL* sb = LLFontGL::getFontSansSerifSmallBold();
        textL(sb, "Picture on the box:", l, y - ROW * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        F32 kx = l + 130.f;
        static const char* KINDS[5] = { "place", "character", "challenge", "item", "gate" };
        static const char* KTIPS[5] = { "Somewhere the players go", "Someone the players meet and talk to", "A fight or a quiz",
                                        "Something the players find or touch", "A way on that opens only when something is true" };
        for (S32 k = 0; k < 5; ++k)
        {
            const std::string kk = KINDS[k];
            const std::string word = kindWord(kk);
            const F32 bw = btnW(word) + 18.f;
            if (kx + bw > r) { kx = l + 130.f; y -= ROW + 4.f; }
            const bool on = n.mKind == kk;
            // Source: GAME_CARD_SPINE_SPEC.md section 1: beat kind place | character | challenge | item | gate; the author's choice is kept
            const S32 ik = act([this, bp, kk]() { docSet(0, bp + "/kind", boost::json::value(kk)); });
            btn(kx, y - ROW, kx + bw, y, std::string(), SH_E, ik, word + ": " + KTIPS[k] + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            kindIcon(kk, kx + 11.f, y - ROW * 0.5f, 12.f, on ? p.mAccent : p.mText);
            textL(LLFontGL::getFontSansSerifSmall(), word, kx + 20.f, y - ROW * 0.5f, on ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
            kx += bw + 4.f;
        }
        y -= ROW + 2.f;
        y = para(l, r, y, "The picture only shows what kind of moment it is. To add a fight or a lock, use More...", p.mDim, true) - 6.f;   // [WORDING 2026-10-18]
        if (!isRoleplay() || n.mMain)
        {
            // a roleplay world has no main quest (Change 2026-10-18): the switch shows there only to take a step off again
            const bool on = n.mMain;
            // [WORDING 2026-10-18] a checkbox with the web Studio's words: a drawn box (a tick when on), then the label; both click
            const S32 im = act([this, bp, on]()
            {
                if (on) docErase(0, bp + "/main");
                else docSet(0, bp + "/main", boost::json::value(true));
            });
            const std::string tip = on ? llformat("Main quest step %d - press to make it a side path", n.mStep) : std::string("A side path - press to make it a step of the main quest");
            btn(l, y - 22.f, l + 22.f, y, std::string(), SH_E, im, tip, p, true, on ? BTN_ACTIVE : 0);
            if (on) { lineW(l + 5.f, y - 11.f, l + 9.f, y - 16.f, 2.f, OK_INK); lineW(l + 9.f, y - 16.f, l + 17.f, y - 6.f, 2.f, OK_INK); }
            const std::string lab = std::string("Main quest step (the players must do it to win; it goes on the thick line)") + (on ? llformat(" - step %d", n.mStep) : std::string());
            textL(LLFontGL::getFontSansSerifSmall(), lab, l + 30.f, y - 11.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 30.f);
            hit(l + 26.f, y - 22.f, r, y, SH_E, im, tip, false);
            y -= 22.f + 10.f;
        }
    }
    F32 x = l;
    auto tool = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, U32 extra = 0)
    {
        const F32 bw = btnW(lab);
        if (x > l && x + bw > r) { x = l; y -= ROW + 4.f; }
        const S32 idx = act(fn);
        btn(x, y - ROW, x + bw, y, lab, SH_E, idx, tip, p, enabled, extra);
        x += bw + 4.f;
    };
    const std::string sel = bp, beat = n.mId;
    // Source: wolfadventure_story_lib.php was_apply_op 'anchor_here' (724-743) {op, id}: a spot and an area where the author stands
    // (helperDo sends `here`, the viewer's position when AlphaWolf has none - was_here_now); the same op helper_next offers (1813)
    tool(n.mPlaced ? "Place it here again" : "Place it here", "This box happens where you are standing now", [this, beat]()
    {
        LLSD op;
        op["op"] = "anchor_here";
        op["id"] = beat;
        helperDo(op);
    }, !mHelperBusy);   // [WORDING 2026-10-18] an ordinary button: page 3's one primary is the band's (or the foot's Play from the start)
    // Source: wolfadventure_story_lib.php was_box_cast (1771-1779): the characters a box names - its ways on (triggers[].cast),
    // onDuty[].cast and decisions[].by; 'bring_here' (744-749) {op, cast} brings one to the author (helper_next's button, 1815)
    {
        // `ids`: the characters named above
        for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"]))
        {
            const std::string cid = c["id"].asString();
            if (!ids.count(cid)) continue;
            const std::string nm = cap(c["first"].asString() + (c["last"].asString().empty() ? "" : " " + c["last"].asString()), 30);
            tool("Bring " + nm + " here", nm + " comes to where you are standing (and stays while the helper walks with you)", [this, cid]()
            {
                LLSD op;
                op["op"] = "bring_here";
                op["cast"] = cid;
                helperDo(op);
            }, !mHelperBusy);
        }
    }
    tool("Leads to...", "How this box is done, and which box (or ending) comes next", [this, beat]() { pickLead(beat); }, !mHelperBusy);
    tool(mBoxMore ? "Less" : "More...", mBoxMore ? "Hide the full box tools" : "The full box tools: a fight, someone walking with them, your scripts, the full editor, remove",
         [this]() { mBoxMore = !mBoxMore; }, true, mBoxMore ? BTN_ACTIVE : 0);
    y -= ROW + 8.f;
    // what happens here: said to the helper about this box (helperSend sends `node` = mFlowSel)
    textL(LLFontGL::getFontSansSerifSmallBold(), "What happens here?", l, y, p.mAccent);
    y -= (F32)LLFontGL::getFontSansSerifSmall()->getLineHeight() + 2.f;
    fieldMulti(l, y - 58.f, r, y, mHelperBox, "Say what happens at this box, in your own words...", F_HELPER, p);
    y -= 64.f;
    {
        const S32 is = act([this]() { helperSend(); });
        btn(l, y - 30.f, l + 100.f, y, mHelperBusy ? "Sending..." : "Send", SH_E, is, "Tell the helper (Enter)", p, !mHelperBusy && !mHelperMissing);
        y -= 38.f;
    }
    y = boxThings(l, r, y, beat, p);   // [IN-WORLD STUFF 2026-10-17] "Things to build here" (wolffloateradventurestudioedit.cpp)
    if (!mBoxMore) return y;
    // ---- More...: the full box tools ----
    y = head(l, r, y, "More for this box", p);
    x = l;
    tool("Open the full editor", "Every setting of this box on Advanced editing > Story", [this, sel]() { mSel = sel; mPage = SP_STORY; mScroll[SP_STORY] = 0.f; }, true);
    tool("Add a box after it", "A new box this one leads to (you then tell the helper what happens there)", [this, sel]() { addStoryItem(2, sel); }, true);
    tool("Someone walks with them...", "From this box a character walks with the players (at most 2 with a party)", [this, beat]() { pickCompanion(beat); }, true);
    tool("A script moves it on...", "Your own script (a door, a button, a trap) says when this box is done, and where it leads", [this, beat]() { pickScriptHook(beat, false); }, !mHelperBusy);
    tool("Tell my scripts it starts", "Your objects hear when this box starts (wolfAdventureListen), to open a door or set off an effect", [this, beat]() { pickScriptHook(beat, true); }, !mHelperBusy);
    // [M4] a fight as a box - only where the schema knows battles (an older schema file would refuse the draft)
    if (WolfAdvStudio::packHas("battles") && WolfAdvStudio::packHas("enemies"))
        tool(n.mFight.empty() ? "A fight happens here..." : "Change the fight...", "Who they fight here, where winning leads and what losing does",
             [this, beat]() { pickBattle(beat); }, !mHelperBusy);
    tool("Play from here", "Try your game from this box (only you)", [this, beat]() { mTestBeat = beat; testStart(beat, [this]() { mPage = SP_TEST; mScroll[SP_TEST] = 0.f; }); }, mBusy == 0);
    tool("Remove...", "Take this box out (asks first); what led to it leads where it led", [this, sel]() { askDelete(0, sel, "this box"); }, true, BTN_DANGER);
    y -= ROW + 8.f;
    // [M6 2026-10-17] the box's scripts (STORY_FIRST_DESIGN.md section 6): each script trigger as "When my script says
    // <name>", each signal it sends as "Your scripts hear <name>", with a ready script to copy
    {
        std::vector<std::pair<std::string, bool>> hooks;   // name, listen (a signal) or fire (a script trigger)
        auto signalsIn = [&hooks](const LLSD& list) { for (const LLSD& e : llsd::inArray(list)) if (e["signal"].isMap()) hooks.push_back({ e["signal"]["name"].asString(), true }); };
        for (const LLSD& t : llsd::inArray(b["triggers"]))
        {
            if (t["on"].asString() == "script") hooks.push_back({ t["id"].asString(), false });
            signalsIn(t["do"]);
        }
        signalsIn(b["onEnter"]);
        for (const LLSD& d : llsd::inArray(b["decisions"])) for (const LLSD& o : llsd::inArray(d["options"])) signalsIn(o["do"]);
        if (!hooks.empty())
        {
            y = head(l, r, y, "Scripts", p);
            for (const auto& h : hooks)
            {
                const std::string lab = h.second ? "Your scripts hear \"" + cap(h.first, 40) + "\"" : "When my script says \"" + cap(h.first, 40) + "\"";
                const F32 cw = btnW("Copy a script");
                textL(LLFontGL::getFontSansSerifSmall(), lab, l, y - ROW * 0.5f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - cw - 8.f);
                const std::string name = h.first;
                const bool listen = h.second;
                const S32 ic = act([this, name, listen]()
                {
                    const LLWString w = utf8str_to_wstring(scriptFor(name, listen));
                    LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
                    setStatus(listen ? "A listening script is on the clipboard: paste it into a New Script in your object, then add what it should do."
                                     : "A touch script is on the clipboard: paste it into a New Script in your object (yours, on the adventure's region).", false);
                });
                btn(r - cw, y - ROW, r, y, "Copy a script", SH_E, ic, listen ? "A script that hears this (wolfAdventureListen)" : "A script that says this when touched (wolfAdventureFire)", p);
                y -= ROW + 4.f;
            }
            y -= 4.f;
        }
    }
    return y;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the conversation (STORY_FIRST_DESIGN.md §5)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::helperHistory()
{
    if (mOpenId.empty() || !openIsOwner()) return;
    if (mHelperFor == mOpenId && mHelperLoaded) return;
    mHelperFor = mOpenId;
    mHelperLoaded = false;
    mHelperLog = LLSD::emptyArray();
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // STORY_FIRST_DESIGN.md §5 helper_history {adventure} -> {turns:[{who, text}], todo:[text], cost:{todayCredits, cap}}
    LLSD body;
    body["adventure"] = adv;
    WolfAdventure::api("helper_history", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv) return;
        self->mHelperLoaded = true;
        if (!ok)
        {
            // an older brain has no helper: said plainly, with the flowchart and Start here as the way on (never a dead button)
            self->mHelperMissing = error.find("unknown action") != std::string::npos || error.find("not found") != std::string::npos;
            if (!self->mHelperMissing) self->mHelperError = "The helper's notes could not be read: " + error;
            return;
        }
        self->mHelperMissing = false;
        self->mHelperLog = reply["turns"].isArray() ? reply["turns"] : LLSD::emptyArray();
        // [SIMPLE STUDIO 2026-10-17] of the cost only today's total is shown (one plain line); the "still to decide" list is not
        if (reply["cost"].isMap()) self->mHelperTodayCredits = reply["cost"]["todayCredits"].asReal();
        // §9.1 helper_history.walk {walking, region, node, look}: the helper may still be walking from an earlier visit
        const LLSD& w = reply["walk"];
        if (w.isMap())
        {
            self->mHelperWalking = w["walking"].asBoolean();
            self->mHelperWalkRegion = w["region"].asString();
        }
    });
}

void WolfAdventureStudioView::helperApply(const LLSD& reply)
{
    // every reply carries the saved draft: the flowchart and every page follow it at once
    if (!reply["pack_json"].asString().empty())
    {
        setDraftText(reply["pack_json"].asString(), reply["pack"]);
        mEtag = reply["etag"].asString();
        mDirtyAt = 0.0;
        setProblems(reply["problems"]);
    }
    std::string said = reply["say"].asString();
    if (reply["changes"].isArray() && reply["changes"].size() > 0 && said.empty()) said = llformat("I made %d change%s.", (S32)reply["changes"].size(), reply["changes"].size() == 1 ? "" : "s");
    if (!said.empty()) { LLSD t; t["who"] = "helper"; t["text"] = cap(said, 2000); mHelperLog.append(t); }
    mHelperQuestion = cap(reply["question"].asString(), 500);
    // [M6] the helper's script (wolfadventure_story_lib.php: `script`, at most WAS_SCRIPT_MAX 6000 bytes): kept until the next
    // script, so the author can copy it after more turns
    if (!reply["script"].asString().empty()) mHelperScript = reply["script"].asString().substr(0, 6000);
    if (!mHelperQuestion.empty()) { LLSD t; t["who"] = "helper"; t["text"] = mHelperQuestion; mHelperLog.append(t); }
    if (reply["cost"].isMap()) mHelperTodayCredits = reply["cost"]["todayCredits"].asReal();
}

void WolfAdventureStudioView::helperSend()
{
    std::string text = mHelperBox;
    LLStringUtil::trim(text);
    if (text.empty() || mHelperBusy) return;
    mHelperError.clear();
    // [SIMPLE STUDIO 2026-10-17] the helper talks about an open game only: page 1's Build my game makes it (helper_chat's own
    // first-message build is not used - one builder, one money rule)
    if (mOpenId.empty()) { mHelperError = "Make your game first: go to Story and press Build my game."; return; }
    // a turn is at most 2,000 characters (helper_chat's WAS_TEXT_MAX, wolfadventure_story_lib.php:30); a whole story belongs on page 1
    if (utf8str_to_wstring(text).size() > 2000)
    {
        mHelperError = "That is long for one message (2,000 characters at most). Put a whole story on the Story page and press Build my game, or say it in parts.";
        return;
    }
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the helper works on the saved draft: a change still waiting goes first
        if (!mSaving) saveDraftNow();
        mHelperError = "Saving your last change first - press Send again in a moment.";
        return;
    }
    LLSD body;
    body["adventure"] = mOpenId;
    body["text"] = text;
    body["etag"] = mEtag;
    if (!mFlowSel.empty()) body["node"] = mFlowSel;
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        const LLVector3 pos = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        if (std::isfinite(pos.mV[VX]) && std::isfinite(pos.mV[VY]) && std::isfinite(pos.mV[VZ]))
        {
            LLSD here, v = LLSD::emptyArray();
            v.append((F64)pos.mV[VX]); v.append((F64)pos.mV[VY]); v.append((F64)pos.mV[VZ]);
            here["region"] = rgn->getRegionID().asString();
            here["pos"] = v;
            body["here"] = here;
        }
    }
    { LLSD t; t["who"] = "you"; t["text"] = cap(text, 2000); mHelperLog.append(t); }
    mHelperBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // STORY_FIRST_DESIGN.md §5 helper_chat {adventure, text, node?, here?, etag}
    const std::string sent = mHelperBox;
    WolfAdventure::api("helper_chat", body, [handle, adv, sent](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mHelperBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            // the line stays in the box to send again; the log keeps it marked as not answered
            if (self->mHelperLog.isArray() && self->mHelperLog.size() > 0)
            {
                LLSD& last = self->mHelperLog[(LLSD::Integer)self->mHelperLog.size() - 1];
                if (last["who"].asString() == "you") last["text"] = last["text"].asString() + " (not answered)";
            }
            if (reply.isMap() && reply.has("etag"))
            {
                // the draft changed elsewhere: it is read again, and the line is kept for Send
                self->mHelperError = "Your game was changed in another window, so we loaded it again. Press Send to say that again.";
                self->loadOpen();
                return;
            }
            self->mHelperMissing = error.find("unknown action") != std::string::npos;
            self->mHelperError = self->mHelperMissing ? std::string("The helper is not on this grid yet. You can still place each box and say where it leads, or use Advanced editing.")
                                                      : "The helper could not answer: " + error;
            return;
        }
        if (self->mHelperBox == sent) self->mHelperBox.clear();   // answered: the box empties only now (unless more was typed meanwhile)
        self->helperApply(reply);
    });
}

void WolfAdventureStudioView::helperUndo()
{
    if (mOpenId.empty() || mHelperBusy) return;
    if (mDirtyAt > 0.0 || mSaving) { if (!mSaving) saveDraftNow(); mHelperError = "Saving your last change first - press Undo that again in a moment."; return; }
    LLSD body;
    body["adventure"] = mOpenId;
    body["etag"] = mEtag;
    mHelperBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // STORY_FIRST_DESIGN.md §5 helper_undo {adventure, etag}
    WolfAdventure::api("helper_undo", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mHelperBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok) { self->mHelperError = "Nothing was undone: " + error; return; }
        self->helperApply(reply);
    });
}

bool WolfAdventureStudioView::jobApply()
{
    if (mOpenId.empty() || mJobId.empty()) return false;
    if (mDirtyAt > 0.0 || mSaving) { if (!mSaving) saveDraftNow(); setStatus("Saving your last change first - it goes in a moment.", false); return false; }
    LLSD body;
    body["job"] = mJobId;
    body["etag"] = mEtag;
    body["mode"] = "auto";
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    ++mBusy;
    setStatus("Putting it into your game...", false);
    // Source: wolfadventure_story_lib.php wa_api_job_apply (260-299) {job, etag, mode: auto|merge|replace} -> {pack, etag, problems,
    // merged{mode, added, updated, kept, stale[]}, report[]}; 409 {error, etag} when another window saved first
    WolfAdventure::api("job_apply", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mBusy = llmax(0, self->mBusy - 1);
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            if (reply.isMap() && reply.has("etag"))
            {
                // the draft changed meanwhile: read again; a build in progress tries once more on its next poll (pollJob)
                self->loadOpen();
                if (self->mBuildStep == B_APPLYING) { self->mBuildStep = B_RUNNING; self->mJobPollAt = LLFrameTimer::getTotalSeconds() + 3.0; }
                else self->setStatus("Your game was changed in another window, so we loaded it again - press Put it into my game once more.", true);
            }
            else
            {
                // [GAME CARD 2026-10-18] a chapter built and paid for is never built again: "Put it into my game" on the map tries the
                // putting in once more (job_apply only - nothing more is charged)
                if (self->mBuildStep == B_APPLYING && self->mBuildChapter) self->mChapterApplyJob = self->mJobId;
                if (self->mBuildStep == B_APPLYING)
                    self->buildFail((self->mBuildChapter ? "The chapter is built but could not be put into your game: " : "Your game was built, but it could not be put in: ") + error);
                self->setStatus("It could not be put in: " + error, true);
            }
            return;
        }
        self->setDraftText(reply["pack_json"].asString(), reply["pack"]);
        self->mEtag = reply["etag"].asString();
        self->mDirtyAt = 0.0;
        self->setProblems(reply["problems"]);
        const LLSD& m = reply["merged"];
        std::string words = llformat("Done: %d added, %d updated, %d of yours kept as they were.", m["added"].asInteger(), m["updated"].asInteger(), m["kept"].asInteger());
        const S32 stale = m["stale"].isArray() ? (S32)m["stale"].size() : 0;
        if (stale > 0) words += llformat(" %d part%s no longer in the story %s left for you to remove or keep (see the story map).", stale, stale == 1 ? "" : "s", stale == 1 ? "is" : "are");
        // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 3.6: every finished build says how many chapters are still to build
        // (counted from the draft just put in: acts[].built false)
        const S32 left = self->unbuiltChapters();
        if (self->mBuildChapter)
            words = left > 0 ? llformat("The chapter is built. %d chapter%s still to build.", left, left == 1 ? " is" : "s are")
                             : std::string("The chapter is built. Every chapter of your game is built now.");
        else if (left > 0) words += llformat(" %d chapter%s still to build: press Add the next chapter on the story map.", left, left == 1 ? " is" : "s are");
        self->mChapterApplyJob.clear();
        self->setStatus(words, false);
        // [SIMPLE STUDIO 2026-10-17] the game is in: page 2, the characters; [GAME CARD 2026-10-18] a chapter: back on the story map
        const bool chapter = self->mBuildChapter;
        self->mBuildChapter = false;
        self->mBuildChapterTitle.clear();
        if (self->mBuildStep == B_APPLYING) self->mBuildStep = B_NONE;
        self->press(SH_PAGE, chapter ? (S32)SP_MAP : (S32)SP_CHARS);
        self->mScroll[chapter ? SP_MAP : SP_CHARS] = 0.f;
        // [SETUP WALK 2026-10-18] a game just built: Gameplay with the walk when it has one to do (as the web Studio's _suAfterMade)
        if (!chapter) self->setupAfterMade(adv, (S32)SP_CHARS, words);
    });
    return true;
}

S32 WolfAdventureStudioView::flowFind(const std::string& key) const
{
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (flowKey(mFlowNodes[i]) == key) return i;
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [WOLF STORY FIRST M3 2026-10-17] the helper in the world (STORY_FIRST_DESIGN.md §9, §9.1)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

LLSD WolfAdventureStudioView::helperHere() const
{
    // §9.1: region events carry no position, so the Studio sends where the author stands (region metres)
    LLSD here;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return here;
    const LLVector3 pos = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    if (!std::isfinite(pos.mV[VX]) || !std::isfinite(pos.mV[VY]) || !std::isfinite(pos.mV[VZ])) return here;
    LLSD v = LLSD::emptyArray();
    v.append((F64)pos.mV[VX]); v.append((F64)pos.mV[VY]); v.append((F64)pos.mV[VZ]);
    here["region"] = rgn->getRegionID().asString();
    here["pos"] = v;
    return here;
}

void WolfAdventureStudioView::helperWalk(bool on)
{
    if (mOpenId.empty() || mHelperWalkBusy) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["on"] = on;
    if (on)
    {
        const LLSD here = helperHere();
        if (!here.isMap()) { mHelperError = "Stand in the world first: the helper appears beside you."; return; }
        body["here"] = here;
    }
    mHelperWalkBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // §9 helper_walk {adventure, on, here?} -> {walking, region, say, removed?}
    WolfAdventure::api("helper_walk", body, [handle, adv, on](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mHelperWalkBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            self->mHelperError = (on ? "The helper could not come: " : "The helper could not go: ") + error;
            // [review] the brain still has the walk on (it comes back on the next region the adventure runs on): the button
            // keeps saying so, and this region is not asked again until the author moves on
            if (on && gAgent.getRegion()) self->mHelperWalkFailedRegion = gAgent.getRegion()->getRegionID().asString();
            return;
        }
        self->mHelperWalkFailedRegion.clear();
        self->mHelperError.clear();
        self->mHelperWalking = reply["walking"].asBoolean();
        self->mHelperWalkRegion = reply["region"].asString();
        if (!reply["say"].asString().empty()) { LLSD t; t["who"] = "helper"; t["text"] = cap(reply["say"].asString(), 500); self->mHelperLog.append(t); }
    });
}

void WolfAdventureStudioView::helperFocus(const std::string& node)
{
    if (mOpenId.empty() || mHelperMissing) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["node"] = node;
    const LLSD here = helperHere();
    if (here.isMap()) body["here"] = here;
    // §9 helper_focus {adventure, node, here?}: quiet - a failure only matters when the author talks to the helper in the world
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("helper_focus", body, [handle](bool ok, const LLSD&, const std::string& error)
    {
        if (handle.isDead() || ok) return;
        if (error.find("unknown action") == std::string::npos) handle.get()->mHelperError = "The helper did not get which box you clicked: " + error;
    });
}

void WolfAdventureStudioView::helperNextBox(const std::string& after)
{
    if (mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    if (!after.empty()) body["after"] = after;
    // §9 helper_next {adventure, after?} -> {done, left, node, objective, needs, say, buttons:[{id, label, op?, after?}]}
    call("helper_next", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mHelperNext = reply;
        self->mFlowSel = reply["node"].asString();
    }, "Finding the next box...");
}

void WolfAdventureStudioView::helperDo(const LLSD& op, std::function<void()> then)
{
    if (mOpenId.empty() || mHelperBusy) return;
    if (mDirtyAt > 0.0 || mSaving) { if (!mSaving) saveDraftNow(); setStatus("Saving your last change first - press it again in a moment.", false); return; }
    LLSD body;
    body["adventure"] = mOpenId;
    body["etag"] = mEtag;
    body["op"] = op;
    const LLSD here = helperHere();
    if (here.isMap()) body["here"] = here;
    mHelperBusy = true;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // §9.1 helper_do {adventure, etag, op, here?} -> as helper_chat's reply (no cost: nothing is billed)
    WolfAdventure::api("helper_do", body, [handle, adv, then](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mHelperBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            if (reply.isMap() && reply.has("etag")) { self->mHelperError = "Your game was changed in another window, so we loaded it again - press that once more."; self->loadOpen(); }
            else self->mHelperError = "That was not done: " + error;
            return;
        }
        self->mHelperError.clear();
        self->helperApply(reply);
        if (then) { then(); return; }   // a second op follows (it moves the checklist on itself)
        // the checklist moves on to what is left after this box
        if (self->mHelperNext.isMap() && !self->mHelperNext["done"].asBoolean()) self->helperNextBox(std::string());
    });
}

void WolfAdventureStudioView::pickBattle(const std::string& beat_id)
{
    if (mOpenId.empty() || mHelperBusy || beat_id.empty()) return;
    // the rules' ready-made enemies (enemy_templates {preset}, wolfadventure_combat_lib.php wa_api_enemy_templates); without
    // a preset (or if the call fails) the picker offers the story's own enemies and characters only
    const std::string preset = std::as_const(mDraftPack)["rules"]["preset"].asString();
    if (preset.empty()) { pickBattleWho(beat_id, LLSD()); return; }
    LLSD body;
    body["preset"] = preset;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("enemy_templates", body, [handle, adv, beat_id](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv) return;
        self->pickBattleWho(beat_id, ok ? reply["enemies"] : LLSD());
    });
}

void WolfAdventureStudioView::pickBattleWho(const std::string& beat_id, const LLSD& templates)
{
    // "e:<enemy id>" an enemy of the story; "c:<cast id>" a character who turns on them; "t:<template id>" ready-made
    std::vector<std::pair<std::string, std::string>> who;
    std::map<std::string, std::string> names;
    for (const LLSD& e : llsd::inArray(std::as_const(mDraftPack)["enemies"]))
    {
        who.push_back({ "e:" + e["id"].asString(), cap(e["name"].asString().empty() ? e["id"].asString() : e["name"].asString(), 60) });
        names["e:" + e["id"].asString()] = e["name"].asString();
    }
    for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"]))
    {
        const std::string nm = c["first"].asString() + (c["last"].asString().empty() ? "" : " " + c["last"].asString());
        who.push_back({ "c:" + c["id"].asString(), cap(nm, 50) + " turns on them" });
        names["c:" + c["id"].asString()] = nm;
    }
    for (const LLSD& t : llsd::inArray(templates))
    {
        who.push_back({ "t:" + t["id"].asString(), cap(t["name"].asString(), 50) + " (ready-made)" });
        names["t:" + t["id"].asString()] = t["name"].asString();
    }
    openPicker("Who do they fight? (for someone new, tell the helper: \"three giant spiders attack here\")", who,
               [this, beat_id, names](const std::string& pick)
    {
        if (pick.empty()) return;
        const std::string nm = names.count(pick) ? names.at(pick) : pick.substr(2);
        std::vector<std::pair<std::string, std::string>> how;
        for (S32 k = 1; k <= 6; ++k) how.push_back({ llformat("%d", k), k == 1 ? "One " + cap(nm, 40) : llformat("%d of them", k) });
        openPicker("How many?", how, [this, beat_id, pick, nm](const std::string& count)
        {
            if (count.empty()) return;
            flowLayout();
            std::vector<std::pair<std::string, std::string>> to;
            for (const FlowNode& n : mFlowNodes)
                if (n.mId != beat_id) to.push_back({ n.mEnding ? "end:" + n.mId : n.mId, (n.mEnding ? "END: " : "") + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
            openPicker("If they win, it leads to", to, [this, beat_id, pick, nm, count, to](const std::string& won)
            {
                if (won.empty()) return;
                std::vector<std::pair<std::string, std::string>> lose = { { "~retry", "They get up and fight again" } };
                for (const auto& o : to) if (o.first != won) lose.push_back({ o.first, "It leads to " + o.second });
                openPicker("If they lose", lose, [this, beat_id, pick, nm, count, won](const std::string& lost)
                {
                    if (lost.empty()) return;
                    // the fight (was_battle_op): lines '<count> <enemy id>', options won / lost, when retry | continue
                    // "Change the fight...": the box's battle id, so was_battle_op replaces that battle instead of adding one
                    std::string fightId;
                    for (const FlowNode& n : mFlowNodes) if (!n.mEnding && n.mId == beat_id) fightId = n.mFightId;
                    auto battle = [this, beat_id, count, won, lost, nm, fightId](const std::string& enemy_id)
                    {
                        LLSD op;
                        op["op"] = "add_battle";
                        op["id"] = beat_id;
                        op["value"] = fightId;
                        op["title"] = cap(nm, 32);
                        op["lines"] = LLSD::emptyArray();
                        op["lines"].append(count + " " + enemy_id);
                        LLSD w; w["label"] = "won"; w["next"] = won; w["do"] = LLSD::emptyArray();
                        op["options"] = LLSD::emptyArray();
                        op["options"].append(w);
                        if (lost == "~retry") op["when"] = "retry";
                        else { LLSD l; l["label"] = "lost"; l["next"] = lost; l["do"] = LLSD::emptyArray(); op["options"].append(l); op["when"] = "continue"; }
                        helperDo(op);
                    };
                    if (pick.rfind("e:", 0) == 0) { battle(pick.substr(2)); return; }
                    // a new enemy first (add_enemy: a character turning on them, or a ready-made one), then the fight naming
                    // it - its id as the brain made it (was_new_id may change it), found as the enemy that was not there before
                    std::set<std::string> before;
                    for (const LLSD& e : llsd::inArray(std::as_const(mDraftPack)["enemies"])) before.insert(e["id"].asString());
                    LLSD op;
                    op["op"] = "add_enemy";
                    op["id"] = "";
                    op["title"] = cap(nm, 32);
                    if (pick.rfind("c:", 0) == 0) op["cast"] = pick.substr(2); else op["object"] = pick.substr(2);
                    helperDo(op, [this, before, battle]()
                    {
                        std::string made;
                        for (const LLSD& e : llsd::inArray(std::as_const(mDraftPack)["enemies"])) if (!before.count(e["id"].asString())) made = e["id"].asString();
                        if (made.empty()) { mHelperError = "The enemy was added, but I could not find it: choose it from the list and try again."; return; }
                        battle(made);
                    });
                }, std::string());
            }, "There is no other box yet: add one first.");
        }, std::string());
    }, "There is nobody to fight yet: ask the helper (\"three bandits jump them here\").");
}

void WolfAdventureStudioView::pickLead(const std::string& beat_id)
{
    // the ways on the helper's set_lead knows without places (wolfadventure_story_lib.php was_apply_op set_lead: talk, touch,
    // choose, say, gm, ready); the beginner's three here, then where it goes (a box, or end:<ending id>)
    std::vector<std::pair<std::string, std::string>> how = { { "talk", "They talk to someone" }, { "touch", "They touch an object (select it in the world first)" },
                                                             { "gm", "You (or a host) press Continue" },
                                                             { "ready", "Everyone in the group is ready (only for games of 2 or more players)" } };   // [WORDING 2026-10-18]
    openPicker("This box is finished when\xE2\x80\xA6", how, [this, beat_id](const std::string& when)
    {
        if (when.empty()) return;
        auto chooseTarget = [this, beat_id, when](const std::string& cast, const std::string& object)
        {
            flowLayout();
            std::vector<std::pair<std::string, std::string>> to;
            for (const FlowNode& n : mFlowNodes)
                if (n.mId != beat_id) to.push_back({ n.mEnding ? "end:" + n.mId : n.mId, (n.mEnding ? "END: " : "") + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
            openPicker("Goes to", to, [this, beat_id, when, cast, object](const std::string& target)
            {
                if (target.empty()) return;
                LLSD op;
                op["op"] = "set_lead";
                op["id"] = beat_id;
                op["when"] = when;
                op["target"] = target;
                if (!cast.empty()) op["cast"] = cast;
                if (!object.empty()) op["object"] = object;
                helperDo(op);
            }, "There is no other box yet: add one first.");
        };
        if (when == "talk")
        {
            std::vector<std::pair<std::string, std::string>> who;
            for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"])) who.push_back({ c["id"].asString(), cap(c["first"].asString() + " " + c["last"].asString(), 60) });
            openPicker("Who do they talk to?", who, [chooseTarget](const std::string& cast) { if (!cast.empty()) chooseTarget(cast, std::string()); },
                       "There are no characters yet: ask the helper to add one.");
            return;
        }
        if (when == "touch")
        {
            LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->getFirstRootNode();
            if (!node || !node->mValid || node->mName.empty())
            {
                setStatus("Select the object in the world first (right-click it > Edit), then choose \"They touch an object\" again.", true);
                return;
            }
            chooseTarget(std::string(), cap(node->mName, 63));
            return;
        }
        chooseTarget(std::string(), std::string());
    }, std::string());
}

void WolfAdventureStudioView::pickCompanion(const std::string& beat_id)
{
    std::vector<std::pair<std::string, std::string>> who;
    for (const LLSD& c : llsd::inArray(std::as_const(mDraftPack)["cast"])) who.push_back({ c["id"].asString(), cap(c["first"].asString() + " " + c["last"].asString(), 60) });
    openPicker("Who walks with the players from this box?", who, [this, beat_id](const std::string& cast)
    {
        if (cast.empty()) return;
        std::vector<std::pair<std::string, std::string>> how = { { "player", "With whoever is there (each player)" }, { "party", "With the party (follows its leader)" } };
        openPicker("How do they follow?", how, [this, beat_id, cast](const std::string& follow)
        {
            if (follow.empty()) return;
            flowLayout();
            std::vector<std::pair<std::string, std::string>> until = { { "~none", "Until the story ends" } };
            for (const FlowNode& n : mFlowNodes) if (!n.mEnding && n.mId != beat_id) until.push_back({ n.mId, "Until " + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
            openPicker("When do they leave?", until, [this, beat_id, cast, follow](const std::string& u)
            {
                if (u.empty()) return;
                LLSD op;
                op["op"] = "set_companion";
                op["id"] = beat_id;
                op["cast"] = cast;
                op["value"] = follow;
                op["target"] = u == "~none" ? std::string() : u;
                helperDo(op);
            }, std::string());
        }, std::string());
    }, "There are no characters yet: ask the helper to add one first.");
}

std::string WolfAdventureStudioView::scriptFor(const std::string& name, bool listen) const
{
    // the names are pack ids (WA_PACK_ID_RE: lower-case letters, digits, - and _), safe inside an LSL string; mOpenId is
    // the adventure's key, as wolfAdventureFire's and wolfAdventureListen's adventure (ScriptSyntax.xml)
    if (!listen)
        return llformat("// Wolf Adventure: when touched, tells the adventure \"%s\" (the box's script step).\n"
                        "default\n{\n    touch_start(integer n)\n    {\n"
                        "        if (!wolfAdventureFire(llDetectedKey(0), \"%s\", \"%s\", \"\"))\n"
                        "            llOwnerSay(\"Not now: \" + wolfGetLastError());\n"
                        "    }\n}\n", name.c_str(), mOpenId.c_str(), name.c_str());
    // wolfAdventureListen(channel): story events as chat on the channel, "signal|av|adv|name|data" (ScriptSyntax.xml);
    // the channel is the script's own choice - any channel this object does not use for something else
    return llformat("// Wolf Adventure: hears \"%s\" from the adventure's story.\n"
                    "integer CHANNEL = -7346;\n"
                    "default\n{\n    state_entry()\n    {\n        wolfAdventureListen(CHANNEL);\n        llListen(CHANNEL, \"\", NULL_KEY, \"\");\n    }\n"
                    "    listen(integer ch, string from, key id, string msg)\n    {\n"
                    "        list f = llParseStringKeepNulls(msg, [\"|\"], []);\n"
                    "        if (llList2String(f, 0) == \"signal\" && llList2String(f, 2) == \"%s\" && llList2String(f, 3) == \"%s\")\n"
                    "        {\n            key player = llList2Key(f, 1);\n            string data = llList2String(f, 4);\n"
                    "            // what your object does now (open a door, play a sound, set off an effect)\n"
                    "            llOwnerSay(\"%s for \" + llKey2Name(player));\n        }\n    }\n}\n",
                    name.c_str(), mOpenId.c_str(), name.c_str(), name.c_str());
}

void WolfAdventureStudioView::pickScriptHook(const std::string& beat_id, bool starts)
{
    if (mOpenId.empty() || mHelperBusy || beat_id.empty()) return;
    // a name from the box's id that no step of the pack uses yet (WA_PACK_ID_RE: at most 48 characters)
    std::string base = beat_id.substr(0, 32) + (starts ? "-starts" : "-script");   // uniqueId keeps 40, plus "-NN"
    const std::string name = WolfAdvStudio::uniqueId(base, mDraftJson);
    if (starts)
    {
        LLSD op;
        op["op"] = "add_script_hook";
        op["id"] = beat_id;
        op["when"] = "enter";
        op["object"] = name;
        op["text"] = "";
        helperDo(op);
        return;
    }
    flowLayout();
    std::vector<std::pair<std::string, std::string>> to = { { "~stay", "It stays on this box (the script only does steps)" } };
    for (const FlowNode& n : mFlowNodes)
        if (n.mId != beat_id) to.push_back({ n.mEnding ? "end:" + n.mId : n.mId, (n.mEnding ? "END: " : "") + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
    openPicker("When your script says so, it leads to", to, [this, beat_id, name](const std::string& target)
    {
        if (target.empty() || target == "~stay")
        {
            if (target == "~stay") setStatus("A script with nothing to do changes nothing: tell the helper what happens (\"when my script says " + name + ", give them the key\").", true);
            return;
        }
        LLSD op;
        op["op"] = "add_script_hook";
        op["id"] = beat_id;
        op["when"] = "script";
        op["value"] = name;
        op["target"] = target;
        op["do"] = LLSD::emptyArray();
        helperDo(op);
    }, "There is no other box yet: add one first.");
}

void WolfAdventureStudioView::flowConnect(const std::string& from_key, const std::string& to_key)
{
    flowLayout();   // the boxes as the draft is now
    const S32 from = flowFind(from_key), to = flowFind(to_key);
    if (from < 0 || to < 0 || from == to)
    {
        setStatus("That box has changed or gone meanwhile - look again and connect it once more.", true);
        return;
    }
    const FlowNode a = mFlowNodes[from], c = mFlowNodes[to];
    if (a.mEnding) return;
    const LLSD& b = WolfAdvStudio::sdAt(mDraftPack, a.mPtr);
    if (!b.isMap() || b["id"].asString() != a.mId) { setStatus("That box has changed or gone meanwhile - look again.", true); return; }
    const S32 ti = doneTrigger(b);
    // a box with no way on yet: through its place when its scene has one ("when they reach it"), else after a minute -
    // both valid (ADVENTURE_PACK_V1.md §11.1 arrive needs an area; timer needs sec); the author changes it on the box
    const std::string scp = a.mPtr.substr(0, a.mPtr.rfind("/beats/"));
    const std::string area = WolfAdvStudio::sdAt(mDraftPack, scp)["area"].asString();
    bool made_timer = false;
    const bool ok = editDoc([&](boost::json::value& root)
    {
        boost::json::value* bv = WolfAdvStudio::jAt(root, a.mPtr, false);
        if (!bv || !bv->is_object()) return false;
        boost::json::value& tl = bv->as_object()["triggers"];
        if (!tl.is_array()) tl = boost::json::array();
        boost::json::object* t = nullptr;
        if (ti >= 0 && ti < (S32)tl.as_array().size() && tl.as_array()[ti].is_object()) t = &tl.as_array()[ti].as_object();
        if (!t)
        {
            boost::json::object nt{ { "id", WolfAdvStudio::uniqueId("trigger", mDraftJson) } };
            if (!area.empty()) { nt["on"] = "arrive"; nt["area"] = area; }
            else { nt["on"] = "timer"; nt["sec"] = 60; made_timer = true; }
            tl.as_array().push_back(nt);
            t = &tl.as_array().back().as_object();
        }
        // the way on now goes to the target: a box by `next`, an ending by an `end` step (ADVENTURE_PACK_V1.md §11)
        if (boost::json::value* d = t->if_contains("do"))
            if (d->is_array())
            {
                boost::json::array& da = d->as_array();
                for (size_t k = 0; k < da.size();)
                    if (da[k].is_object() && (da[k].as_object().contains("end") || da[k].as_object().contains("goto"))) da.erase(da.begin() + (std::ptrdiff_t)k); else ++k;
                if (da.empty()) t->erase("do");
            }
        t->erase("next");
        if (c.mEnding) (*t)["do"] = boost::json::array{ boost::json::object{ { "end", c.mId } } };
        else (*t)["next"] = c.mId;
        return true;
    });
    if (ok) setStatus("\"" + cap(a.mTitle, 40) + "\" now leads to " + (c.mEnding ? "the ending \"" : "\"") + cap(c.mTitle, 40) + "\"." +
                      (made_timer ? std::string(" For now it goes on after a minute: tell the helper, or open the box, to say how.") : std::string()), false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// mouse: pan, select, connect, zoom
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventureStudioView::flowMouseDown(S32 x, S32 y, const WolfGameHits::Hit* h)
{
    if (mPage != SP_MAP || !h) return false;
    if (h->mId == SH_FLOW_BG)
    {
        mFlowPanning = true;
        mFlowGrabX = x; mFlowGrabY = y;
        mFlowGrabPanX = mFlowPanX; mFlowGrabPanY = mFlowPanY;
        return true;
    }
    if (h->mId == SH_FLOW_HANDLE)
    {
        mFlowConnectFrom = h->mArg;
        mFlowConnectKey = h->mArg >= 0 && h->mArg < (S32)mFlowNodes.size() ? flowKey(mFlowNodes[h->mArg]) : std::string();
        mFlowMouseX = x; mFlowMouseY = y;
        return true;
    }
    if (h->mId == SH_FLOW_BOX && h->mArg >= 0 && h->mArg < (S32)mFlowNodes.size())
    {
        // [SIMPLE STUDIO 2026-10-17] mouse down on a box: a drag moves it (flowHover); a release without movement is the click
        // that selects it (flowMouseUp returns false then, and handleMouseUp presses SH_FLOW_BOX as before). The box is held by its
        // key: the draft may change under the drag (a helper reply), and the index with it
        // [GAME CARD 2026-10-18] a main quest step stays on its line (flowLayout keeps it there): a press is only a click
        if (mFlowMainMode && mFlowNodes[h->mArg].mMain) return false;
        mFlowDragKey = flowKey(mFlowNodes[h->mArg]);
        mFlowDragX = x; mFlowDragY = y;
        mFlowDragOrigX = mFlowNodes[h->mArg].mX;
        mFlowDragOrigY = mFlowNodes[h->mArg].mY;
        mFlowDragMoved = false;
        return true;
    }
    return false;
}

bool WolfAdventureStudioView::flowHover(S32 x, S32 y)
{
    if (mFlowPanning)
    {
        mFlowPanX = mFlowGrabPanX + (F32)(x - mFlowGrabX);
        mFlowPanY = mFlowGrabPanY + (F32)(y - mFlowGrabY);
        return true;
    }
    if (mFlowConnectFrom >= 0) { mFlowMouseX = x; mFlowMouseY = y; return true; }
    if (!mFlowDragKey.empty())
    {
        const S32 dx = x - mFlowDragX, dy = y - mFlowDragY;
        if (!mFlowDragMoved && std::abs(dx) < 3 && std::abs(dy) < 3) return true;   // not a drag yet
        mFlowDragMoved = true;
        const S32 i = flowFind(mFlowDragKey);
        if (i >= 0)
        {
            // the box follows the mouse in chart units (view y goes up, chart y goes down: drawFlow's sy)
            const F32 z = llmax(0.01f, mFlowZoom);
            mFlowNodes[i].mX = mFlowDragOrigX + (F32)dx / z;
            mFlowNodes[i].mY = mFlowDragOrigY - (F32)dy / z;
        }
        return true;
    }
    return false;
}

bool WolfAdventureStudioView::flowMouseUp(S32 x, S32 y)
{
    if (mFlowPanning)
    {
        mFlowPanning = false;
        // a click without a drag on the ground clears the selection
        if (std::abs(x - mFlowGrabX) < 3 && std::abs(y - mFlowGrabY) < 3) { mFlowSel.clear(); mFlowSelEnd.clear(); }
        return true;
    }
    if (!mFlowDragKey.empty())
    {
        const std::string key = mFlowDragKey;
        const bool moved = mFlowDragMoved;
        mFlowDragKey.clear();
        mFlowDragMoved = false;
        if (!moved) return false;   // a click: handleMouseUp presses the box (SH_FLOW_BOX selects it)
        const S32 i = flowFind(key);
        if (i >= 0) flowSavePos(key, mFlowNodes[i].mX, mFlowNodes[i].mY);
        return true;
    }
    if (mFlowConnectFrom >= 0)
    {
        const std::string fromKey = mFlowConnectKey;
        mFlowConnectFrom = -1;
        mFlowConnectKey.clear();
        const WolfGameHits::Hit* h = mHits.at(x, y);
        if (h && h->mId == SH_FLOW_BOX && h->mArg >= 0 && h->mArg < (S32)mFlowNodes.size() && flowKey(mFlowNodes[h->mArg]) != fromKey)
            flowConnect(fromKey, flowKey(mFlowNodes[h->mArg]));
        else setStatus("Drop the line on another box to make this box lead there.", false);
        return true;
    }
    return false;
}

bool WolfAdventureStudioView::flowWheel(S32 x, S32 y, S32 clicks)
{
    if (mPage != SP_MAP || !mFlowRect.pointInRect(x, y)) return false;
    // [SIMPLE STUDIO 2026-10-17] the chart point under the wheel stays under it
    flowZoomAt(clicks < 0 ? 1.12f : 1.f / 1.12f, x, y);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [SIMPLE STUDIO 2026-10-17] the three pages (region-terrain-work/wolf-story/SIMPLE_STUDIO_SPEC.md)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [GAME CARD 2026-10-18] page 1's new parts: ready games, imports, "Choose your game" (GAME_CARD_SPINE_SPEC.md section 5)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: GAME_CARD_SPINE_SPEC.md section 1: card.length short | medium | long shown as these words
    std::string lengthWords(const std::string& len)
    {
        if (len == "short") return "About 15 minutes";
        if (len == "medium") return "About 45 minutes";
        if (len == "long") return "2 hours or more";
        return std::string();
    }
}

F32 WolfAdventureStudioView::readyGames(F32 l, F32 r, F32 top, const Palette& p)
{
    // Source: wolfadventure_plain_lib.php wa_api_templates {} -> {templates:[{id, title, genre, summary, purpose, preset, beats}]}, and
    // GAME_CARD_SPINE_SPEC.md section 6: each also carries `card` and `length` for these cards
    if (!mTemplatesLoaded && !mTemplatesLoading && mTemplatesError.empty()) loadTemplates();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const F32 slh = (F32)small->getLineHeight(), blh = (F32)bold->getLineHeight();
    F32 y = head(l, r, top, "Or start from a ready game", p);
    y = para(l, r, y, "Free. You set it up on your land in a few steps, and change anything you like.", p.mDim, true) - 6.f;   // [WORDING 2026-10-18]
    if (!mTemplatesError.empty())
    {
        y = para(l, r, y, "The ready games could not be loaded: " + mTemplatesError, DANGER_INK, true) - 4.f;
        const S32 ir = act([this]() { mTemplatesError.clear(); mTemplatesLoaded = false; loadTemplates(); });
        btn(l, y - ROW, l + btnW("Try again"), y, "Try again", SH_E, ir, "Load the ready games again", p, !mTemplatesLoading);
        return y - ROW - 10.f;
    }
    if (mTemplatesLoading && !mTemplatesLoaded) return para(l, r, y, "Loading the ready games...", p.mDim, true) - 8.f;
    const S32 n = mTemplates.isArray() ? (S32)mTemplates.size() : 0;
    if (n == 0) return y;
    const F32 gap = 8.f, minW = 180.f;
    const S32 cols = llclamp((S32)floorf((r - l + gap) / (minW + gap)), 1, llmin(n, 4));
    const F32 cw = (r - l - gap * (F32)(cols - 1)) / (F32)cols;
    const F32 ch = 10.f + blh + slh + 2.f * slh + slh + 10.f;
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& t = std::as_const(mTemplates)[i];
        const std::string id = t["id"].asString();
        if (id.empty()) continue;
        const S32 col = i % cols, row = i / cols;
        const F32 cl = l + (F32)col * (cw + gap), ct = y - (F32)row * (ch + gap);
        const std::string title = cap(t["title"].asString().empty() ? id : t["title"].asString(), 64);
        const bool making = mTemplateMaking == id;
        const S32 idx = act([this, id, title]()
        {
            // what was typed or loaded on page 1 would not be used: asked first (as the web Studio)
            std::string typed = mStoryBox;
            LLStringUtil::trim(typed);
            if (!mBookText.empty() || (!typed.empty() && mOpenId.empty())) { mConfirmKeyA = id; mConfirmKeyB = title; ask(SC_READY_GAME, 0); return; }
            startFromTemplate(id, title);
        });
        const bool enabled = mTemplateMaking.empty() && mBuildStep == B_NONE;
        button(cl, ct - ch, cl + cw, ct, std::string(), p, flagsFor(SH_E, idx, enabled));
        F32 ty = ct - 10.f;
        textL(bold, title, cl + 10.f, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
        ty -= blh;
        const bool rp = t["card"]["shape"].asString() == "roleplay";
        std::string genre = cap(t["genre"].asString(), 40);
        {
            std::string gl = genre;
            LLStringUtil::toLower(gl);
            if (rp && gl.find("roleplay") == std::string::npos) genre += genre.empty() ? "Just roleplay" : " \xC2\xB7 just roleplay";
        }
        textL(small, genre, cl + 10.f, ty, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
        ty -= slh;
        // one line: what the game is (its card's goal), else the starter's summary
        const std::string line = cap(t["card"]["goal"].asString().empty() ? t["summary"].asString() : t["card"]["goal"].asString(), 200);
        for (const std::string& ln : wrap(small, line, cw - 20.f, 2)) { textL(small, ln, cl + 10.f, ty, p.mText); ty -= slh; }
        const std::string len = making ? std::string("Making it...")
                              : rp ? std::string("play as long as you like")   // [WORDING 2026-10-18] a roleplay ready game, as the web
                              : lengthWords(t["length"].asString().empty() ? t["card"]["length"].asString() : t["length"].asString());
        textL(small, len, cl + 10.f, ct - ch + 8.f, making ? p.mAccent : p.mDim, LLFontGL::LEFT, LLFontGL::BOTTOM, cw - 20.f);
        hit(cl, ct - ch, cl + cw, ct, SH_E, idx, "Start from \"" + title + "\": " + line + " (free, no AI - you go straight to its characters)", true, enabled);
    }
    y -= (F32)((n + cols - 1) / cols) * (ch + gap) + 2.f;
    if (!mReadyError.empty()) y = para(l, r, y, mReadyError, DANGER_INK) - 4.f;
    return y - 8.f;
}

F32 WolfAdventureStudioView::importSection(F32 l, F32 r, F32 top, const Palette& p)
{
    // GAME_CARD_SPINE_SPEC.md section 7: two quiet buttons; the file's cost question; the progress card; where it went.
    // [IMPORT WORLDS 2026-10-18] the flow is the shared WolfImportFlow (wolfimportflow.cpp, also the Import worlds window); this
    // page only draws it, with the same words and buttons as before.
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    F32 y = head(l, r, top, "Start from something you built (Experimental)", p);   // Paul 10-18: imports are experimental, like the Studio
    WolfImportFlow& fl = *mImportFlow;
    const LLSD& ij = fl.job();
    const bool running = fl.running();
    const bool asking = fl.asking();
    const bool busy = fl.busy();
    {
        F32 x = l;
        const std::string a = "Import a block build (.schem, .vox)", b = "Import an obstacle course (.rbxlx, .rbxl)";
        // [IMPORT OWNERS 2026-10-18] Paul: only region owners import - for anyone else the buttons stay, greyed, with the reason below
        const bool can = fl.importAllowed() && !busy && !running && !asking;
        const S32 ia = act([this]() { mImportFlow->pick(WolfImportFlow::C_BLOCK_ANY); });
        btn(x, y - 28.f, x + btnW(a), y, a, SH_E, ia, "A build made of blocks, saved as .schem, .litematic or .vox (at most 20 MB): it comes into your inventory as pieces in a box to rez", p, can);
        x += btnW(a) + 8.f;
        if (x + btnW(b) > r) { x = l; y -= 34.f; }
        const S32 ib = act([this]() { mImportFlow->pick(WolfImportFlow::C_COURSE); });
        btn(x, y - 28.f, x + btnW(b), y, b, SH_E, ib, "Your own obstacle course place file (.rbxlx or .rbxl, at most 20 MB): its plain parts, checkpoints and finish line come in", p, can);
        y -= 34.f;
    }
    y = para(l, r, y, "Only import builds you made yourself. That's your responsibility under our terms.", p.mDim, true) - 6.f;
    if (!fl.importAllowed())
    {
        y = para(l, r, y, fl.notAllowedWhy(), WARN_INK) - 6.f;
        if (!fl.shopUrl().empty())
        {
            const S32 ishop = act([this]() { mImportFlow->openShopPage(); });
            btn(l, y - 28.f, l + btnW("Rent a region..."), y, "Rent a region...", SH_E, ishop, "Opens the page where you can rent a region, in your browser", p);
            y -= 34.f;
        }
    }
    if (busy && !asking && !running) y = para(l, r, y, !fl.hasData() ? "Sending it..." : "Reading " + fl.file() + " and working out the cost...", p.mText) - 6.f;
    if (asking)
    {
        // ONE inline question with the cost (section 7: credits for the upload only, quoted); nothing is charged before Yes
        // Source: wolfadventure_import_lib.php wa_api_import_quote -> {kind, name, size, quote:{credits, micro}, limits, formats, legal}
        const S32 credits = fl.quoteCredits();
        const std::string what = fl.kind() == "block" ? "block build" : "obstacle course";
        const std::string qs = credits > 0 ? llformat("Bringing in the %s \"%s\" (%.1f MB) costs %d credits (US $%.2f). Bring it in?", what.c_str(), fl.file().c_str(),
                                                      (F64)fl.bytes() / (1024.0 * 1024.0), credits, (F64)credits / 100.0)
                                           : llformat("Bringing in the %s \"%s\" (%.1f MB) is free. Bring it in?", what.c_str(), fl.file().c_str(), (F64)fl.bytes() / (1024.0 * 1024.0));
        const std::vector<std::string> ql = wrap(f, qs, r - l - 24.f, 3);
        const bool obby = fl.kind() == "obby";
        const F32 qh = 10.f + (F32)ql.size() * lh + 8.f + (obby ? 34.f : 0.f) + 32.f + 10.f;
        card(l, y - qh, r, y, p, 8.f);
        F32 qy = y - 10.f;
        for (const std::string& ln : ql) { textL(f, ln, l + 12.f, qy, p.mText); qy -= lh; }
        qy -= 8.f;
        if (obby)
        {
            // also a game: its checkpoints become the main quest's steps and the finish line the win (the importer's starter)
            const bool mg = fl.makeGame();
            const S32 ig = act([this]() { mImportFlow->setMakeGame(!mImportFlow->makeGame()); });
            btn(l + 12.f, qy - 26.f, l + 12.f + 90.f, qy, mg ? "On" : "Off", SH_E, ig,
                mg ? "A new game is made from the course too - press for just the course" : "Just the course - press to make a game from it too",
                p, !busy, mg ? BTN_ACTIVE : 0);
            textL(small, mg ? "Also make a game from it: each checkpoint is a step of the main quest, and the Finish Line wins."
                            : "Also make a game from it: off - only the course comes in.", l + 110.f, qy - 13.f, p.mText,
                  LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 122.f);
            qy -= 34.f;
        }
        const S32 iy = act([this]() { mImportFlow->start(); });
        // not primary: Build my game is page 1's one primary button
        btn(l + 12.f, qy - 30.f, l + 12.f + btnW("Yes, bring it in", 150.f), qy, busy ? "Sending..." : "Yes, bring it in", SH_E, iy, "Send the file and make it", p, !busy);
        const S32 in = act([this]() { mImportFlow->dismissQuote(); });
        btn(r - 12.f - btnW("No", 110.f), qy - 30.f, r - 12.f, qy, "No", SH_E, in, "Nothing is sent or charged", p, !busy);
        y -= qh + 8.f;
    }
    if (fl.hasJob())
    {
        // the progress card, then where it went and what was left out
        const std::string state = fl.state();
        std::vector<std::pair<std::string, LLColor4>> lines;
        if (running)
        {
            const S64 el = fl.runAt() > 0 ? llmax((S64)0, (S64)(time(nullptr) - fl.runAt())) : 0;
            const std::string pr = ij["progress"].asString();
            lines.push_back({ "Bringing in \"" + fl.file() + "\"..." + (pr.empty() ? std::string() : " " + cap(pr, 80)), p.mText });
            lines.push_back({ el >= 60 ? llformat("Working for %d min %d s. You can close this window: it carries on.", (S32)(el / 60), (S32)(el % 60))
                                       : llformat("Working for %d s. You can close this window: it carries on.", (S32)el), p.mDim });
        }
        else if (state == "done")
        {
            // Source: wolfadventure_import_lib.php wai_job_out: output {item, folder, folderName, name, pieces, adventure}, report [text]
            // (its last line says where the box is and how to rez it)
            const LLSD& out = ij["output"];
            lines.push_back({ WolfImportFlow::doneWords(ij), OK_INK });
            for (const LLSD& rp : llsd::inArray(ij["report"])) lines.push_back({ cap(rp.asString(), 300), p.mText });
            // [IMPORT WORLDS 2026-10-18] Source: wolfadventure_import_lib.php wai_job_out `warnings` (wai_script_warnings: plain lines
            // about scripts in the file that look dodgy), shown apart from the report, in amber (GAME_CARD_SPINE_SPEC.md 15:20Z change)
            for (const LLSD& w : llsd::inArray(ij["warnings"])) lines.push_back({ cap(w.asString(), 600), WARN_INK });   // each line says "Warning: ..." itself
            if (!out["adventure"].asString().empty()) lines.push_back({ "Press Open my game to see it.", p.mText });
        }
        std::vector<std::pair<std::string, LLColor4>> wl;
        for (const auto& ln : lines) for (const std::string& w : wrap(f, ln.first, r - l - 24.f, 6)) wl.push_back({ w, ln.second });
        const F32 ch = 10.f + (running ? 14.f : 0.f) + (F32)wl.size() * lh + (running ? 0.f : 6.f + 34.f) + 10.f;
        card(l, y - ch, r, y, p, 8.f);
        F32 cy = y - 10.f;
        if (running)
        {
            const F32 bl = l + 12.f, br = r - 12.f, bt = cy, bb = cy - 6.f;
            rect(bl, bb, br, bt, fade(p.mDim, 0.3f));
            const F32 t = (F32)fmod(LLFrameTimer::getTotalSeconds(), 2.4) / 2.4f;
            const F32 u = t < 0.5f ? t * 2.f : (1.f - t) * 2.f;
            const F32 hw = (br - bl) * 0.22f;
            rect(bl + u * (br - bl - hw), bb, bl + u * (br - bl - hw) + hw, bt, p.mAccent);
            cy -= 14.f;
        }
        for (const auto& ln : wl) { textL(f, ln.first, l + 12.f, cy, ln.second); cy -= lh; }
        if (!running)
        {
            cy -= 6.f;
            const S32 ic = act([this]() { mImportFlow->finish(); });
            btn(l + 12.f, cy - 28.f, l + 12.f + btnW("Done"), cy, "Done", SH_E, ic, "Put this away", p);
            const std::string made = fl.madeAdventure();
            if (!made.empty())
            {
                // the game the course made (import_start game:true): opened like a game from its card
                const S32 io = act([this, made]() { mImportFlow->finish(); loadList(); openAdventure(made); });
                const F32 ox = l + 12.f + btnW("Done") + 8.f;
                btn(ox, cy - 28.f, ox + btnW("Open my game"), cy, "Open my game", SH_E, io, "Open the game made from your course", p, mBuildStep == B_NONE);
            }
            cy -= 34.f;
        }
        y -= ch + 8.f;
    }
    if (!fl.error().empty())
    {
        y = para(l, r, y, fl.error(), DANGER_INK) - 6.f;
        if (fl.needCredits() && mBuildStep == B_NONE)
        {
            const S32 ibuy = act([this]() { mImportFlow->openBuyPage(); });
            btn(l, y - 30.f, l + btnW("Buy credits...", 160.f), y, "Buy credits...", SH_E, ibuy, "Opens the Wolf Territories credits page in your browser", p);
            y -= 38.f;
        }
    }
    return y - 6.f;
}

F32 WolfAdventureStudioView::choosePanel(F32 l, F32 r, F32 top, const Palette& p)
{
    // GAME_CARD_SPINE_SPEC.md section 3.1-3.2 and 5: the plan's 3-4 shape cards, "You choose", the three questions, Build chapter 1
    const LLSD& plan = std::as_const(mJob)["output"]["plan"];
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const F32 slh = (F32)small->getLineHeight(), blh = (F32)bold->getLineHeight();
    F32 y = top;
    textL(display(22.f), "Choose your game", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(22.f)->getLineHeight() + 6.f;
    const S32 n = plan["shapes"].isArray() ? (S32)plan["shapes"].size() : 0;
    if (n == 0)
    {
        y = para(l, r, y, "We read your story, but the kinds of game it could be did not come through. Press Let us pick, and we'll choose the best one and build chapter 1.", WARN_INK) - 8.f;
        const S32 iyc = act([this]() { chooseAnswer(true); });
        btn(l, y - 34.f, l + llmax(200.f, btnW("Let us pick") + 30.f), y, mAnswerBusy ? "Sending..." : "Let us pick", SH_E, iyc, "We pick the kind of game and build chapter 1", p, !mAnswerBusy, BTN_PRIMARY);
        y -= 42.f;
        if (!mAnswerError.empty()) y = para(l, r, y, mAnswerError, DANGER_INK) - 6.f;
        return y;
    }
    if (mPlanFor != mJobId)
    {
        mPlanFor = mJobId;
        mPlanLength.clear();
        choosePick(plan, plan["shapes"][0]["id"].asString());   // the first card chosen, so Build chapter 1 works at once
    }
    y = para(l, r, y, "We read your story. Here are the games it could be. Pick one, check the answers under the cards, then press Build chapter 1. "
                      "Chapter 1 is already in the price you agreed; later chapters are added from the story map, each with its own price.", p.mText) - 8.f;
    // the cards, two or four across
    const F32 gap = 8.f;
    const S32 cols = r - l >= 900.f ? llmin(n, 4) : llmin(n, 2);
    const F32 cw = (r - l - gap * (F32)(cols - 1)) / (F32)cols;
    F32 rowTop = y, rowLow = y;
    for (S32 i = 0; i < n; ++i)
    {
        const LLSD& sh = plan["shapes"][i];
        const std::string id = sh["id"].asString();
        const bool rp = sh["shape"].asString() == "roleplay";
        const bool on = id == mPlanShape;
        const S32 col = i % cols;
        if (col == 0 && i > 0) { rowTop = rowLow - gap; }
        const F32 cl = l + (F32)col * (cw + gap);
        std::vector<std::pair<std::string, const LLFontGL*>> lines;
        for (const std::string& w : wrap(bold, cap(sh["title"].asString(), 80), cw - 20.f, 2)) lines.push_back({ w, bold });
        if (on) lines.push_back({ "\xE2\x9C\x93 Chosen", small });
        if (rp) lines.push_back({ "Just roleplay - no goal, no winning", small });
        for (const std::string& w : wrap(small, cap(sh["pitch"].asString(), 300), cw - 20.f, 4)) lines.push_back({ w, small });
        if (!sh["youAre"].asString().empty()) for (const std::string& w : wrap(small, "You are: " + cap(sh["youAre"].asString(), 240), cw - 20.f, 2)) lines.push_back({ w, small });
        if (!sh["goal"].asString().empty()) for (const std::string& w : wrap(small, (rp ? "About: " : "Goal: ") + cap(sh["goal"].asString(), 240), cw - 20.f, 3)) lines.push_back({ w, small });
        F32 h = 20.f;
        for (const auto& ln : lines) h += ln.second == bold ? blh : slh;
        const S32 idx = act([this, id]() { choosePick(std::as_const(mJob)["output"]["plan"], id); });
        button(cl, rowTop - h, cl + cw, rowTop, std::string(), p, flagsFor(SH_E, idx, !mAnswerBusy) | (on ? BTN_ACTIVE : 0));
        F32 ty = rowTop - 10.f;
        for (const auto& ln : lines)
        {
            const bool title = ln.second == bold;
            textL(ln.second, ln.first, cl + 10.f, ty, title ? (on ? p.mAccent : p.mText) : p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            ty -= title ? blh : slh;
        }
        hit(cl, rowTop - h, cl + cw, rowTop, SH_E, idx, "Choose \"" + cap(sh["title"].asString(), 80) + "\"" + (on ? " (chosen)" : ""), true, !mAnswerBusy);
        rowLow = llmin(rowLow, rowTop - h);
    }
    y = rowLow - 10.f;
    // the three questions, from the chosen card
    bool rp = false;
    for (const LLSD& sh : llsd::inArray(plan["shapes"])) if (sh["id"].asString() == mPlanShape) rp = sh["shape"].asString() == "roleplay";
    const F32 qw = llmin(640.f, r - l);
    textL(LLFontGL::getFontSansSerifSmallBold(), "Who are you? (who the players are in the game)", l, y, p.mText);
    y -= slh + 3.f;
    field(l, y - 30.f, l + qw, y, mPlanYouAre, "e.g. A deputy new in town", F_PLAN_YOU, p);
    y -= 38.f;
    if (!rp)
    {
        textL(LLFontGL::getFontSansSerifSmallBold(), "How do you win?", l, y, p.mText);
        y -= slh + 3.f;
        field(l, y - 30.f, l + qw, y, mPlanWin, "e.g. The gold is back in the bank before sundown", F_PLAN_WIN, p);
        y -= 38.f;
    }
    textL(LLFontGL::getFontSansSerifSmallBold(), "How long?", l, y, p.mText);
    y -= slh + 3.f;
    {
        F32 x = l;
        static const char* LENS[3] = { "short", "medium", "long" };
        for (S32 k = 0; k < 3; ++k)
        {
            const std::string len = LENS[k], words = lengthWords(len);
            const bool on = mPlanLength == len;
            const F32 bw = btnW(words);
            const S32 il = act([this, len]() { mPlanLength = len; });
            btn(x, y - 28.f, x + bw, y, words, SH_E, il, "How long a game: " + words + (on ? " (chosen)" : ""), p, !mAnswerBusy, on ? BTN_ACTIVE : 0);
            x += bw + 6.f;
        }
        y -= 36.f;
    }
    {
        const F32 bw = llmax(200.f, btnW("Build chapter 1") + 30.f);
        const S32 ib = act([this]() { chooseAnswer(false); });
        btn(l, y - 44.f, l + bw, y, mAnswerBusy ? "Sending..." : "Build chapter 1", SH_E, ib,
            "Build the first chapter of this game now (already in the price you agreed); the next chapters are added from the story map", p,
            !mAnswerBusy && !mPlanShape.empty(), BTN_PRIMARY);
        // "You choose": the brain picks the kind of game (shape ""), and builds chapter 1 at once
        const std::string yc = "Let us pick, and build chapter 1";
        const S32 iyc = act([this]() { chooseAnswer(true); });
        btn(l + bw + 10.f, y - 36.f, l + bw + 10.f + btnW(yc), y - 8.f, yc, SH_E, iyc,
            "Skip the choice: we pick the best kind of game for your story and build chapter 1 now (your answers above are not used)", p, !mAnswerBusy);
        // stop here: the build ends (job_cancel takes an asking job too, wolfmind_lib.php wa_api_job_cancel); asked first
        const std::string st = "Stop, don't build it";
        const S32 ist = act([this]() { ask(SC_JOB_CANCEL, 0); });
        btn(r - btnW(st), y - 36.f, r, y - 8.f, st, SH_E, ist, "End this build here (asks first); what reading your story cost is not given back", p, !mAnswerBusy, BTN_DANGER);
        y -= 52.f;
    }
    if (!mAnswerError.empty())
    {
        y = para(l, r, y, mAnswerError, DANGER_INK) - 6.f;
        if (mAnswerRequote > 0)
        {
            // the hour passed: the rest costs more (wa_api_job_answer requote); said on the button, sent only on this press
            const std::string lab = llformat("Yes, go on - at most %d credits in all", mAnswerRequote);
            const S32 iq = act([this]() { chooseAnswer(mAnswerYouChoose, mAnswerRequote); });
            btn(l, y - 30.f, l + btnW(lab), y, lab, SH_E, iq, "Read the book again and build chapter 1 (the most this whole build may spend)", p, !mAnswerBusy);
            y -= 38.f;
        }
        if (mBuildNeedCredits && !mBuildBuyUrl.empty())
        {
            const S32 ibuy = act([this]()
            {
                if (mBuildBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuildBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuildBuyUrl);
                else mAnswerError = "The credits page address did not come from Wolf Territories, so it was not opened.";
            });
            btn(l, y - 30.f, l + btnW("Buy credits...", 160.f), y, "Buy credits...", SH_E, ibuy, "Opens the Wolf Territories credits page in your browser", p);
            y -= 38.f;
        }
    }
    y = para(l, r, y, "You can close this window: your choice waits for you (for a day).", p.mDim, true) - 6.f;
    return y;
}

F32 WolfAdventureStudioView::pageYourStory(F32 l, F32 r, F32 top, const Palette& p)
{
    // Persona and story (SIMPLE_STUDIO_SPEC.md): a resident who knows nothing about game design has a book or an idea and wants
    // a game people can play on their land, and wants to be having fun within two minutes.
    // Flow: type, paste or load a book -> Build my game -> one cost line, Yes -> progress -> page 2. One primary button; the
    // cost on one line, never a separate page; every failure in red text that stays (mBuildError).
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 colW = llmin(820.f, r - l), cl = l + ((r - l) - colW) * 0.5f, cr = cl + colW;
    F32 y = top - 14.f;
    textL(display(28.f), "Your story", cl, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, colW);
    y -= (F32)display(28.f)->getLineHeight() + 8.f;
    y = para(cl, cr, y, "Type an idea, paste a story or a whole book, or load a book file. We'll turn it into a game with characters, "
                        "choices and different endings. You see the cost before anything is charged.", p.mText) - 10.f;
    // [WORDING 2026-10-18] the experimental notice is gone from the Studio pages (GAME_CARD_SPINE_SPEC.md "Wording"); the band's label stays
    // "Changed in another window: Reload / Keep mine" - B_SAVING's failure says "Choose Reload or Keep mine above" (buildTick), so
    // the banner is drawn here as on page 3 (pageMap)
    y = conflictBanner(cl, cr, y, p);
    if (!mOpenId.empty() && !openIsOwner())
        y = para(cl, cr, y, "The game open now is someone else's (you run it for them). Build my game makes a new game of your own.", p.mDim, true) - 6.f;
    const bool building = mBuildStep != B_NONE;
    // [SIMPLE STUDIO 2026-10-17] Paul: "stuck here don't know what to do next" (his screen said only "Outline"): from the approved
    // start on, the progress card is the only thing above the cards - the box, the book and their buttons are hidden, not just dim
    const bool progress = mBuildStep == B_STARTING || mBuildStep == B_RUNNING || mBuildStep == B_APPLYING;
    const bool book = !mBookText.empty();
    // the open game has boxes already (its draft loaded): a build now is a rebuild (the label, the question, the book note)
    bool rebuild = false;
    if (!mOpenId.empty() && openIsOwner() && !mDraftJson.empty()) { flowLayout(); rebuild = !mFlowNodes.empty(); }
    if (progress)
    {
        // nothing here: the card below
    }
    else if (!book)
    {
        fieldMulti(cl, y - 170.f, cr, y, mStoryBox, "Type an idea, or paste your story or a whole book here...\n\nExample: \"A Wild West bank robbery where the players "
                                                     "can side with the outlaws or the sheriff.\"", F_STORY, p);
        y -= 178.f;
    }
    else
    {
        // the book: its name and size, its first line; the box is not needed
        const S32 chars = (S32)utf8str_to_wstring(mBookText).size();
        std::string first = mBookText.substr(0, mBookText.find('\n'));
        first = WolfAdventure::cutUtf8(first, 200);
        for (char& ch : first) if (ch == '\r' || ch == '\t') ch = ' ';
        const std::vector<std::string> fl = wrap(small, first, colW - 24.f, 2);
        const F32 bh = 12.f + slh + 2.f + (F32)fl.size() * slh + 10.f;
        card(cl, y - bh, cr, y, p, 6.f);
        textL(bold, (mBookName.empty() ? std::string("Pasted book") : mBookName) + " - " + thousands(chars) + " characters", cl + 12.f, y - 8.f, OK_INK, LLFontGL::LEFT, LLFontGL::TOP, colW - 24.f);
        F32 fy = y - 8.f - slh - 2.f;
        for (const std::string& ln : fl) { textL(small, ln, cl + 12.f, fy, p.mDim); fy -= slh; }
        y -= bh + 8.f;
        // Paul (2026-10-17): "give us some kind of warning for a whole book" - before the quote, one dim line
        if (mBuildStep == B_NONE)
            y = para(cl, cr, y, "A book is built a chapter at a time: chapter 1 now, the next ones later from the story map. You see the exact cost next.", p.mDim, true) - 6.f;
        // no rights tick (Paul 2026-10-17: the Terms of Service make it the author's responsibility - one reminder line under Build
        // my game instead; buildQuote sends rights:true)
        const S32 iu = act([this]() { mBookText.clear(); mBookName.clear(); mBookRights = false; mBuildError.clear(); });
        btn(cl, y - 26.f, cl + btnW("Remove the book"), y, "Remove the book", SH_E, iu, "Forget this book and type or paste something else", p, !building);
        y -= 32.f;
    }
    if (!book && !progress)
    {
        // a book from a file (the existing file reader: .txt, .md, .html; UTF-8; at most 11 MB); a pasted book goes in the box above
        const S32 ifl = act([this]() { mBuildError.clear(); readBookFile(); });
        btn(cl, y - 28.f, cl + btnW("Load a book file..."), y, "Load a book file...", SH_E, ifl, "A .txt, .md or .html file (at most 11 MB)", p, !building);
        y -= 36.f;
    }
    if (!mBuildError.empty())
    {
        y = para(cl, cr, y, mBuildError, DANGER_INK) - 6.f;
        // Paul 10-17: "no link to where I could buy credits" - the credits page, right under the words that say you need them
        if (mBuildNeedCredits && !mBuildBuyUrl.empty() && (!building || mBuildStep == B_QUOTED))   // Yes refused for credits: a way on
        {
            const S32 ibuy = act([this]()
            {
                // our own site only (aicredits.php buyurl = WC_SITE_URL "/index.php?f=credits", wolfcreditslib.php:39), as SH_AI_BUY
                if (mBuildBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuildBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuildBuyUrl);
                else mBuildError = "The credits page address did not come from Wolf Territories, so it was not opened.";
            });
            btn(cl, y - 30.f, cl + btnW("Buy credits...", 160.f), y, "Buy credits...", SH_E, ibuy, "Opens the Wolf Territories credits page in your browser; then press Build my game again", p, true);   // [WORDING 2026-10-18] not primary: Build my game is page 1's one
            y -= 38.f;
        }
    }
    // ---- the one button, its question, or its progress ----
    if (mBuildStep == B_NONE)
    {
        std::string t = mStoryBox;
        LLStringUtil::trim(t);
        const bool ready = book || !t.empty();   // a loaded book is ready as it is (no tick)
        // [SIMPLE STUDIO 2026-10-17] Paul: "i dont see rebuild my game" - the open game has boxes already: the same build (buildStart:
        // draft_start, then job_apply mode auto), which keeps what the author changed themselves (wolfadventure_story_lib.php
        // wa_api_job_apply 272-281: a draft that is not fresh (was_fresh, 121-130) is merged by was_merge, 146-150)
        const std::string label = rebuild ? "Rebuild my game" : "Build my game";
        const F32 bw = llmax(200.f, btnW(label) + 30.f);
        const S32 ib = act([this]() { buildStart(); });
        btn(cl, y - 44.f, cl + bw, y, label, SH_E, ib,
            rebuild ? std::string("Build it again from your story: new boxes and characters come in, and what you changed yourself is kept (you see the cost first)")
                    : book ? std::string("Make the game from this book: characters, choices and endings (you see the cost first)")
                           : std::string("Make the game from your story: characters, choices and endings (you see the cost first)"), p, ready, BTN_PRIMARY);
        if (!ready) textL(small, "Type or paste your story first.", cl + bw + 10.f, y - 22.f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
        // [TABS 2026-10-17] "My games" lives here now, beside the build button (off the foot bar): closes the open game - not while a
        // build runs, so a paid job is not abandoned - and lists the games again. (The build button itself reads Rebuild my game when
        // the open game has boxes, so there is one of those, not two.)
        if (!mOpenId.empty())
        {
            const S32 im = act([this]() { closeOpenAdventure(); mScroll[SP_HELPER] = 0.f; mHelperLog = LLSD::emptyArray(); mHelperFor.clear(); loadList(); });
            const F32 mx = ready ? cl + bw + 10.f : cl + bw + 10.f + (F32)small->getWidthF32("Type or paste your story first.") + 10.f;
            btn(mx, y - 36.f, mx + btnW("My games"), y - 8.f, "My games", SH_E, im, "Your games, and a new one (this game stays as it is)", p, mBuildStep == B_NONE);
        }
        y -= 50.f;
        // the book's text is not kept after a build (it lives in the job, WOLF_MIND_V1.md §12: job input until the owner deletes
        // the job, at most 30 days after done - never in the draft), so a rebuild from the book needs the file again
        if (rebuild && !book) y = para(cl, cr, y, "To rebuild from the book, load the book file again. Your own changes are kept.", p.mDim, true) - 6.f;
        // Paul 2026-10-17: the rights reminder, always shown (book or typed), nothing to click
        y = para(cl, cr, y, "Only build from text you're allowed to use - your own, or a book that's out of copyright. That's your responsibility under our terms.", p.mDim, true) - 8.f;
    }
    else if (mBuildStep == B_QUOTED)
    {
        // ONE inline question: the quote in credits and US dollars (WOLF_MIND_V1.md §7.1: 1 credit = 1 US cent); nothing is charged before Yes
        const S32 credits = (S32)ceil(std::as_const(mJob)["quote"]["credits"].asReal());
        // Paul (2026-10-17): "give us some kind of warning for a whole book" - how long it takes, in the question itself: a book is
        // built in BOOK_ACTS = 5 acts (buildQuote), 2 minutes for the outline + 1.5 an act (the progress card's rule); a typed
        // story a minute or two
        // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 3.5: the quote is the ideas, the plan of the whole game and chapter 1;
        // each later chapter has its own price on the story map - said here, so it is no surprise there
        // the words as the web Studio's (floater_wolf_adventure_studio.js [GAME CARD 2026-10-18]); a book's time by Paul's rule (2
        // minutes a pass that reads the whole book - the ideas and the plan - plus 1.5 for chapter 1: about 6 minutes)
        const std::string q = llformat("This %s chapter 1 of your game for about %d credits (US $%.2f). Each later chapter is built when you ask for it, "
                                       "and its cost is shown first. %s First we read it and offer you a few kinds of game to choose from - stay for that - "
                                       "then we build chapter 1 so you can play it. %s it?", rebuild ? "rebuilds" : "builds", credits, (F64)credits / 100.0,
                                       book ? "A book takes about 6 minutes, and you can close the window while it works."
                                            : "It takes a minute or two.",
                                       rebuild ? "Rebuild" : "Build");
        const std::vector<std::string> ql = wrap(LLFontGL::getFontSansSerif(), q, colW - 24.f, 10);
        const F32 qh = 12.f + (F32)ql.size() * (F32)LLFontGL::getFontSansSerif()->getLineHeight() + 10.f + 34.f + 12.f;
        card(cl, y - qh, cr, y, p, 8.f);
        F32 qy = y - 10.f;
        for (const std::string& ln : ql) { textL(LLFontGL::getFontSansSerif(), ln, cl + 12.f, qy, p.mText); qy -= (F32)LLFontGL::getFontSansSerif()->getLineHeight(); }
        qy -= 8.f;
        const S32 iy = act([this]() { buildYes(); });
        btn(cl + 12.f, qy - 32.f, cl + 12.f + btnW("Yes, rebuild it", 150.f), qy, rebuild ? "Yes, rebuild it" : "Yes, build it", SH_E, iy, "Start building: the credits are charged as it works", p, true, BTN_PRIMARY);
        const S32 in = act([this]() { buildNo(); });
        btn(cr - 12.f - btnW("No", 110.f), qy - 32.f, cr - 12.f, qy, "No", SH_E, in, "Nothing is charged; your story stays", p);
        y -= qh + 10.f;
    }
    else if (!progress)
    {
        // before the author says Yes: one line of what is happening
        std::string words;
        switch (mBuildStep)
        {
        case B_CREATING: words = "Making your game..."; break;
        case B_WRITE:
        case B_SAVING: words = "Saving your story..."; break;
        case B_BUDGET:
        case B_QUOTING: words = "Working out what it costs..."; break;
        default: break;
        }
        y = para(cl, cr, y, words, p.mText) - 10.f;
    }
    else if (mBuildStep == B_RUNNING && std::as_const(mJob)["state"].asString() == "asking")
    {
        // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 3.1: the plan is in and the build waits for the author's choice
        y = choosePanel(cl, cr, y, p) - 10.f;
    }
    else
    {
        // [SIMPLE STUDIO 2026-10-17] the progress card: what is happening, that closing is fine, the clock, the steps, the spend,
        // Stop the build
        const LLFontGL* f = LLFontGL::getFontSansSerif();
        const F32 lh = (F32)f->getLineHeight();
        const std::vector<std::string> about = wrap(f, mBuildChapter ? std::string("This takes a minute or two. You can close this window: the build carries on, and your "
                                                                                   "game opens on its story map when the chapter is in.")
                                                                     : !std::as_const(mJob)["card"].isMap()
                                                                       ? std::string("Stay for a minute: we'll ask you to choose what kind of game it is. After that you can close this window.")
                                                                       : std::string("You can close this window: the build carries on, and your game opens on Characters when it is done."), colW - 24.f, 6);
        // Paul (2026-10-17, "stuck here again, needs to do something"): the card is alive - a clock, a moving bar, the brain's steps
        // with the current one marked and the finished ones ticked, how long a build like this takes.
        // The brain's progress words (wolfmind_lib.php): 'Waiting to start' (3209), 'Starting' (2451), 'Outline' or 'Outline i/n'
        // (2584), 'Act k of N' (2608), 'Putting it together' (2613), 'Done' (2630), any with ' (again)' (2523) on a retry.
        std::string pr = std::as_const(mJob)["progress"].asString();
        const std::string AGAIN = " (again)";
        const bool again = pr.size() > AGAIN.size() && pr.compare(pr.size() - AGAIN.size(), AGAIN.size(), AGAIN) == 0;
        if (again) pr.erase(pr.size() - AGAIN.size());
        // the steps: 0 before the outline, 1 the outline, 1 + k act k, 100 putting it together, 101 putting it into the game
        S32 cur = 0;
        std::string outlineSub;
        {
            S32 k = 0, n = 0;
            if (mBuildStep == B_APPLYING || pr == "Done") cur = 101;
            else if (pr == "Putting it together") cur = 100;
            else if (sscanf(pr.c_str(), "Act %d of %d", &k, &n) == 2 && k > 0) cur = 1 + k;
            else if (pr.rfind("Outline", 0) == 0) { cur = 1; outlineSub = pr.substr(7); LLStringUtil::trim(outlineSub); }
        }
        std::vector<std::pair<std::string, S32>> steps;   // words, index
        if (mBuildChapter)
        {
            // [GAME CARD 2026-10-18] one chapter (section 3.6: one act pass), then into the game
            if (cur != 101) cur = 2;
            steps.push_back({ "Chapter: " + (mBuildChapterTitle.empty() ? std::string("the next one") : mBuildChapterTitle), 2 });
        }
        else
        {
            // [GAME CARD 2026-10-18] section 3: the kinds of game it could be (the plan pass, then "Choose your game"), the plan of the
            // whole game (the brain's "Outline"), chapter 1 only (its "Act 1 of N": any act pass is chapter 1's here), then together
            // (the steps' names as the web Studio's _s1Steps: Reading your story, Choose your game, Outline of the whole game, Chapter 1,
            // Putting it together)
            // cur from above: 0 before the outline, 1 the outline, 1 + k a chapter; here 0 reading, 1 the choice, 2 the outline, 3 chapter 1
            if (cur == 1) cur = 2;
            else if (cur > 1 && cur < 100) cur = 3;
            const bool planned = std::as_const(mJob)["card"].isMap();   // wm_job_out 'card': the choice is made (job_answer)
            if (cur == 0 && planned) cur = 2;
            steps.push_back({ "Reading your story", 0 });
            steps.push_back({ "Choose your game", 1 });
            steps.push_back({ "Outline of the whole game" + (cur == 2 && !outlineSub.empty() ? " " + outlineSub : std::string()), 2 });
            steps.push_back({ "Chapter 1", 3 });
            steps.push_back({ "Putting it together", 100 });
        }
        steps.push_back({ "Putting it into your game", 101 });
        // how long: 2 minutes for the outline + 1.5 minutes an act (Paul's rule, 2026-10-17); [GAME CARD 2026-10-18] a build now writes
        // ONE act (chapter 1, GAME_CARD_SPINE_SPEC.md section 3.4), so a book is 2 + 1.5; a typed story is a minute or two
        std::string takes;
        if (mBuildChapter) takes = std::string();
        else if (mBuildKind == 2 || (mBuildKind == 0 && mBuildActs >= 3))
            takes = "A book takes about 6 minutes.";   // 2 + 2 + 1.5 (the two passes that read it, then chapter 1), as the web Studio
        else if (mBuildKind == 1 || (mBuildKind == 0 && mBuildActs > 0))
            takes = "A typed story takes about a minute or two.";
        // the clock
        std::string clock;
        {
            const S64 el = mBuildRunAt > 0 ? llmax((S64)0, (S64)(time(nullptr) - mBuildRunAt)) : 0;
            clock = el >= 60 ? llformat("Working for %d min %d s", (S32)(el / 60), (S32)(el % 60)) : llformat("Working for %d s", (S32)el);
            if (mBuildStep == B_STARTING) clock = "Starting...";
        }
        std::string spent;
        if (mBuildStep == B_RUNNING && std::as_const(mJob)["cost"].isMap())
        {
            // the cap is the quote (buildYes); a build picked up after a relaunch learns it from its first poll (pollJob maxCredits)
            const std::string sp = wholeCredits(std::as_const(mJob)["cost"]["credits"].asReal());
            spent = mBuildMax > 0 ? "Spent so far: " + sp + " of at most " + wholeCredits((F64)mBuildMax) + "."
                                  : "Spent so far: " + sp + ".";
        }
        const bool stoppable = mBuildStep == B_RUNNING && !mJobId.empty();
        const F32 hh = (F32)display(22.f)->getLineHeight();
        const F32 bh = 12.f + hh + 6.f + (F32)about.size() * lh + 8.f + 6.f + 8.f + lh + (takes.empty() ? 0.f : lh) + 6.f + (F32)steps.size() * lh + 4.f
                     + (spent.empty() ? 0.f : slh + 2.f) + (stoppable ? 10.f + 30.f : 0.f) + 12.f;
        card(cl, y - bh, cr, y, p, 8.f);
        F32 cy = y - 12.f;
        textL(display(22.f), mBuildChapter ? std::string("Building the next chapter...") : std::string("Building your game..."), cl + 12.f, cy, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, colW - 24.f);
        cy -= hh + 6.f;
        for (const std::string& ln : about) { textL(f, ln, cl + 12.f, cy, p.mText); cy -= lh; }
        cy -= 8.f;
        {
            // the moving bar: a thin track, a highlight sweeping left and right every 2.4 s (the view draws every frame)
            const F32 bl = cl + 12.f, br = cr - 12.f, bt = cy, bb = cy - 6.f;
            rect(bl, bb, br, bt, fade(p.mDim, 0.3f));
            const F32 t = (F32)fmod(LLFrameTimer::getTotalSeconds(), 2.4) / 2.4f;
            const F32 u = t < 0.5f ? t * 2.f : (1.f - t) * 2.f;
            const F32 hw = (br - bl) * 0.22f;
            const F32 hx = bl + u * (br - bl - hw);
            rect(hx, bb, hx + hw, bt, p.mAccent);
            cy -= 6.f + 8.f;
        }
        textL(f, clock, cl + 12.f, cy, p.mText); cy -= lh;
        if (!takes.empty()) { textL(f, takes, cl + 12.f, cy, p.mDim); cy -= lh; }
        cy -= 6.f;
        for (const auto& st : steps)
        {
            const bool done = st.second < cur, now_ = st.second == cur;
            const std::string line = (done ? "\xE2\x9C\x93 " : now_ ? "\xE2\x96\xB8 " : "   ") + st.first + (now_ && again ? " (trying again)" : "");
            textL(now_ ? LLFontGL::getFontSansSerifBold() : f, line, cl + 12.f, cy, now_ ? p.mAccent : p.mDim, LLFontGL::LEFT, LLFontGL::TOP, colW - 24.f);
            cy -= lh;
        }
        cy -= 4.f;
        if (!spent.empty()) { textL(small, spent, cl + 12.f, cy, p.mDim); cy -= slh + 2.f; }
        if (stoppable)
        {
            // the existing job_cancel path, asked first (SC_JOB_CANCEL: "Stop building? What has been spent so far is not given back.")
            cy -= 10.f;
            const S32 istop = act([this]() { ask(SC_JOB_CANCEL, 0); });
            btn(cl + 12.f, cy - 30.f, cl + 12.f + btnW("Stop the build", 150.f), cy, "Stop the build", SH_E, istop,
                "Stop it here (asks first); what has been spent so far is not given back", p, true, BTN_DANGER);
        }
        y -= bh + 10.f;
    }
    // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5 page 1: under the box and Build my game, the ready games and the imports
    // (not while a build is in hand: its question or its card is the page's one thing then)
    if (mBuildStep == B_NONE)
    {
        y = readyGames(cl, cr, y - 4.f, p);
        y = importSection(cl, cr, y, p);
    }
    // the games to continue
    y = continueCards(cl, cr, y, p);
    return y;
}

F32 WolfAdventureStudioView::pageMap(F32 l, F32 r, F32 top, const Palette& p)
{
    // Page 3 "Your story map" (SIMPLE_STUDIO_SPEC.md): the instruction band with Next place, Walk with me and Play it now; the
    // flowchart full width beside a slim helper column; the box panel under the map; one quiet line of links at the foot.
    if (mOpenId.empty())
    {
        F32 y = para(l, r, top, "Make your game first: go to Story and press Build my game.", p.mDim) - 8.f;
        const S32 ib = act([this]() { press(SH_PAGE, (S32)SP_HELPER); });
        btn(l, y - 30.f, l + btnW("\xE2\x97\x82 Story"), y, "\xE2\x97\x82 Story", SH_E, ib, "Back to Story", p, true, BTN_PRIMARY);
        return y - 38.f;
    }
    if (!openIsOwner())
    {
        F32 y = para(l, r, top, "Only the game's owner builds it. As a host or teacher, use Publish to see it and Run to run it live.", p.mDim) - 8.f;
        const S32 ip = act([this]() { press(SH_PAGE, (S32)SP_PUBLISH); });
        btn(l, y - 30.f, l + btnW("Publish"), y, "Publish", SH_E, ip, "See the game and where it runs", p);
        const S32 ir = act([this]() { press(SH_PAGE, (S32)SP_RUN); });
        btn(l + btnW("Publish") + 8.f, y - 30.f, l + btnW("Publish") + 8.f + btnW("Run"), y, "Run", SH_E, ir, "Run it live", p);
        return y - 38.f;
    }
    if (mDraftJson.empty()) return para(l, r, top, "Opening your game...", p.mDim);
    flowLayout();   // the boxes as the draft is now (Play it now needs one before the map is drawn)
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = conflictBanner(l, r, top, p);
    // [SETUP WALK 2026-10-18] "Let's set up your game!" is the first thing on Gameplay while the setup is not complete (as the web)
    const bool walk = setupActive();
    if (walk) y = setupCard(l, r, y, p);
    // [GAME CARD 2026-10-18] the game card first (top of page 3, always), then the page's heading
    y = gameCard(l, r, y, p);
    // [TABS 2026-10-17] Paul: page 3 is "Gameplay" wherever a newcomer sees it (the tab, this heading, the Guide's steps)
    textL(display(28.f), "Gameplay: your story map", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(28.f)->getLineHeight() + 8.f;
    // [GAME CARD 2026-10-18] the chapters not built yet: the next one's quote is asked for once (nothing is charged for a quote)
    const S32 unbuilt = unbuiltChapters();
    if (unbuilt > 0 && mBuildStep == B_NONE && !mChapterBusy)
    {
        const std::string key = mOpenId + "|" + nextUnbuiltChapter();   // the price does not change with an edit: asked again only for another chapter
        if (mChapterFor != key && mChapterError.empty()) chapterQuote();
    }
    // ---- the band ---- ([SETUP WALK 2026-10-18] not while the walk is on: the walk is the guide then, as the web Studio)
    if (!walk)
    {
        // [SETUP WALK 2026-10-18] a set-up game says so once, above the usual words (doneText)
        std::vector<std::string> lines;
        std::string doneText = mSetupJustDone == mOpenId && setupComplete() ? cap(std::as_const(mSetup)["doneText"].asString(), 300) : std::string();
        if (!doneText.empty())
        {
            // said once (the next draw is the usual band), with what was skipped and the way on - as the web Studio
            std::string sk;
            S32 nsk = 0;
            for (const LLSD& st : llsd::inArray(std::as_const(mSetup)["steps"]))
                if (st["skipped"].asBoolean()) { if (nsk < 5) sk += (nsk ? ", " : "") + cap(st["title"].asString(), 60); ++nsk; }
            if (nsk > 0) doneText += llformat(" You skipped %d: ", nsk) + sk + (nsk > 5 ? "\xE2\x80\xA6" : "") + " - press Clean up the land to start the setup again.";
            doneText += " When you're happy, press Publish so anyone can play.";
            if (mDoneShownAt <= 0.0) mDoneShownAt = now();
            if (now() - mDoneShownAt > 20.0) { mSetupJustDone.clear(); mDoneShownAt = 0.0; }
        }
        // [WORDING 2026-10-18] GAME_CARD_SPINE_SPEC.md "Wording" (web agent): one sentence, then the four steps as a numbered list
        if (!doneText.empty()) for (const std::string& w : wrap(LLFontGL::getFontSansSerif(), doneText, r - l - 24.f, 3)) lines.push_back(w);
        for (const std::string& w : wrap(LLFontGL::getFontSansSerif(), "This map is your whole game. Each box is one moment, and the arrows show where the "
                                                                             "players can go next.", r - l - 24.f, 3)) lines.push_back(w);
        static const char* const STEPS[4] = { "1. Press Next place: it picks a box that still needs a spot on your land.",
                                              "2. Walk there and press Place it here. Your helper - a character who walks with you - can come along: press Walk with me.",
                                              "3. Tell the helper what happens there.",
                                              "4. Press Play from the start to try your game." };
        for (const char* st : STEPS)
        {
            // the step's first line as it is, the rest indented under its words
            const std::vector<std::string> sw = wrap(LLFontGL::getFontSansSerif(), st, r - l - 44.f, 6);
            for (size_t k = 0; k < sw.size(); ++k) lines.push_back(k == 0 ? sw[k] : "     " + sw[k]);
        }
        const F32 lh = (F32)LLFontGL::getFontSansSerif()->getLineHeight();
        // the helper's own line for the chosen box (helper_next.say), the test's refusal, both in words under the buttons
        const LLSD& hn = std::as_const(mHelperNext);
        std::string say = hn.isMap() ? cap(hn["say"].asString(), 300) : std::string();
        const S32 left = hn.isMap() ? hn["left"].asInteger() : 0;
        if (!say.empty() && left > 0) say += llformat("  (%d box%s still need%s something)", left, left == 1 ? "" : "es", left == 1 ? "s" : "");
        const std::vector<std::string> sl = say.empty() ? std::vector<std::string>() : wrap(small, say, r - l - 24.f, 4);
        // [GAME CARD 2026-10-18] the chapters line, and the chapter build's refusal (red, stays until the next try)
        std::string chap;
        if (mBuildChapter && mBuildStep != B_NONE)
        {
            std::string pr = std::as_const(mJob)["progress"].asString();
            if (pr.rfind("Act ", 0) == 0) pr = "Chapter " + pr.substr(4);   // the brain's "Act k of N" in the page's words
            chap = "Building the next chapter" + (mBuildChapterTitle.empty() ? std::string() : " (" + mBuildChapterTitle + ")") + "... "
                 + (mBuildStep == B_APPLYING ? std::string("Putting it into your game...") : pr.empty() ? std::string() : "Now: " + cap(pr, 80))
                 + " You can close this window: it carries on.";
        }
        else if (unbuilt > 0)
        {
            std::string title;
            nextUnbuiltChapter(&title);
            chap = llformat("%d chapter%s not built yet (drawn dim on the map). The next one is \"%s\".", unbuilt, unbuilt == 1 ? " is" : "s are", cap(title, 60).c_str());
        }
        const std::vector<std::string> cl = chap.empty() ? std::vector<std::string>() : wrap(small, chap, r - l - 24.f, 2);
        const std::vector<std::string> cel = mChapterError.empty() ? std::vector<std::string>() : wrap(small, mChapterError, r - l - 24.f, 4);
        const bool chBuy = (!mChapterError.empty() && mBuildNeedCredits && !mBuildBuyUrl.empty()) || mChapterNeedBook;
        const F32 bh = 10.f + (F32)lines.size() * lh + 8.f + 32.f + (sl.empty() ? 0.f : 6.f + (F32)sl.size() * slh)
                     + (cl.empty() ? 0.f : 6.f + (F32)cl.size() * slh) + (cel.empty() ? 0.f : 6.f + (F32)cel.size() * slh) + (chBuy ? 38.f : 0.f) + 10.f;
        card(l, y - bh, r, y, p, 8.f);
        F32 by = y - 10.f;
        for (const std::string& ln : lines) { textL(LLFontGL::getFontSansSerif(), ln, l + 12.f, by, p.mText); by -= lh; }
        by -= 8.f;
        F32 x = l + 12.f;
        // Source: wolfadventure_story_lib.php wa_api_helper_next (1786-1832) {adventure, after?}: the next box not placed or with no
        // way on, its line and buttons; it costs nothing (no AI)
        const S32 inx = act([this]() { helperNextBox(std::string()); });
        const bool noBoxes = mFlowNodes.empty();   // nothing to place yet: the game has not been built
        btn(x, by - 30.f, x + btnW("Next place \xE2\x96\xB8"), by, "Next place \xE2\x96\xB8", SH_E, inx,
            noBoxes ? std::string("Build your game on Story first.") : std::string("The next box that is not placed yet, and what to do there (free)"), p, mBusy == 0 && !mHelperBusy && !noBoxes);
        x += btnW("Next place \xE2\x96\xB8") + 6.f;
        if (!mHelperMissing)
        {
            // Source: wolfadventure_story_lib.php wa_api_helper_walk (1651-1688) {adventure, on, here?}
            const std::string wl = mHelperWalking ? "Helper, stay here" : "Walk with me";
            const S32 iw = act([this]() { helperWalk(!mHelperWalking); });
            btn(x, by - 30.f, x + btnW(wl), by, wl, SH_E, iw, mHelperWalking ? "The helper stops following you (and the characters it brought go)"
                                                                                : "The helper appears beside you in the world and follows you", p, !mHelperWalkBusy, mHelperWalking ? BTN_ACTIVE : 0);
            x += btnW(wl) + 6.f;
        }
        const bool chapBuilding = mBuildChapter && mBuildStep != B_NONE;
        if (chapBuilding)
        {
            // the chapter being built: Stop (asked first; what was spent is not given back), as page 1's progress card
            const std::string st = "Stop building it";
            const S32 ist = act([this]() { ask(SC_JOB_CANCEL, 0); });
            btn(r - 12.f - btnW(st), by - 30.f, r - 12.f, by, st, SH_E, ist, "Stop building this chapter (asks first)", p, mBuildStep == B_RUNNING && !mJobId.empty(), BTN_DANGER);
        }
        else if (!mChapterApplyJob.empty())
        {
            // built and paid for, not put in: put in again (job_apply only - nothing more is charged)
            const std::string pl = "Put it into my game";
            const S32 ipl = act([this]()
            {
                mChapterError.clear();
                mJobId = mChapterApplyJob;
                mBuildChapter = true;
                if (jobApply()) { mBuildStep = B_APPLYING; mChapterApplyJob.clear(); }
                else mBuildChapter = false;
            });
            btn(r - 12.f - btnW(pl) - 24.f, by - 30.f, r - 12.f, by, pl, SH_E, ipl, "Put the chapter that was built into your game (nothing more is charged)", p, mBusy == 0, BTN_PRIMARY);
        }
        else if (unbuilt > 0)
        {
            // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5: the band's primary button, the quote on it
            const std::string lab = mChapterCredits >= 0 ? llformat("Add the next chapter - %d credits", mChapterCredits)
                                  : mChapterBusy ? std::string("Add the next chapter - working out the cost...") : std::string("Add the next chapter");
            const F32 pw = llmax(150.f, btnW(lab) + 24.f);
            const S32 ic = act([this]()
            {
                if (mChapterCredits >= 0 && !mChapterJob.empty()) ask(SC_CHAPTER, 0);   // [WORDING 2026-10-18] asked first, as page 1
                else { mChapterError.clear(); mChapterFor.clear(); chapterQuote(); }   // the cost first (again)
            });
            btn(r - 12.f - pw, by - 30.f, r - 12.f, by, lab, SH_E, ic,
                mChapterCredits >= 0 ? llformat("Build the next chapter now: %d credits (US $%.2f), charged as it builds", mChapterCredits, (F64)mChapterCredits / 100.0)
                                     : std::string("Work out what building the next chapter costs (nothing is charged for that)"),
                p, !mChapterBusy && mBusy == 0 && mBuildStep == B_NONE, BTN_PRIMARY);
        }
        by -= 32.f;
        if (!cl.empty()) { by -= 6.f; for (const std::string& ln : cl) { textL(small, ln, l + 12.f, by, p.mDim); by -= slh; } }
        if (!cel.empty()) { by -= 6.f; for (const std::string& ln : cel) { textL(small, ln, l + 12.f, by, DANGER_INK); by -= slh; } }
        if (chBuy && mChapterNeedBook)
        {
            // the book is no longer kept: it is loaded again on page 1 (and Rebuild my game builds from it)
            by -= 6.f;
            const S32 ib = act([this]() { press(SH_PAGE, (S32)SP_HELPER); mScroll[SP_HELPER] = 0.f; });
            btn(l + 12.f, by - 30.f, l + 12.f + btnW("Go to Story"), by, "Go to Story", SH_E, ib,
                "Load the book again there (the Story page), then build", p, true);
            by -= 32.f;
        }
        else if (chBuy)
        {
            by -= 6.f;
            const S32 ibuy = act([this]()
            {
                // our own site only (as page 1's Buy credits...: aicredits.php buyurl = WC_SITE_URL "/index.php?f=credits")
                if (mBuildBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuildBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuildBuyUrl);
                else mChapterError = "The credits page address did not come from Wolf Territories, so it was not opened.";
            });
            btn(l + 12.f, by - 30.f, l + 12.f + btnW("Buy credits...", 160.f), by, "Buy credits...", SH_E, ibuy, "Opens the Wolf Territories credits page in your browser", p, true);
            by -= 32.f;
        }
        if (!sl.empty()) { by -= 6.f; for (const std::string& ln : sl) { textL(small, ln, l + 12.f, by, p.mText); by -= slh; } }
        y -= bh + 10.f;
    }
    // [GAME CARD 2026-10-18] Publish's check: what stops (or may stop) the game from being won, each with Show me
    y = publishCheckCard(l, r, y, p);
    // ---- the map, the box panel, the helper ----
    const F32 w = r - l;
    F32 low;
    if (w >= 760.f)
    {
        const F32 hL = r - 270.f;
        const F32 fh = llmax(300.f, (F32)mPageRect.getHeight() - (top - y) - 250.f);
        F32 fy = drawFlow(l, hL - 16.f, y, fh, p);
        fy = flowBoxPanel(l, hL - 16.f, fy, p);
        const F32 hy = helperColumn(hL, r, y, p);
        low = llmin(fy, hy);
    }
    else
    {
        F32 fy = drawFlow(l, r, y, 340.f, p);
        fy = flowBoxPanel(l, r, fy, p);
        low = helperColumn(l, r, fy - 8.f, p);
    }
    // [FIXED FOOT 2026-10-17] the way-on links (Your characters, My games, Rebuild my game, Advanced editing, Publish) are in the
    // fixed bar under the page (drawFootBar), never scrolled away
    y = low - 8.f;
    return y;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [GAME CARD 2026-10-18] Publish runs the check first (GAME_CARD_SPINE_SPEC.md sections 2 and 5)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::publishCheck()
{
    if (mOpenId.empty() || mPublishChecking) return;
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the check reads the saved draft: a change still waiting goes first
        if (!mSaving) saveDraftNow();
        setStatus("Saving your last change first - press Publish again in a moment.", false);
        return;
    }
    mPublishChecking = true;
    mPublishCheckError.clear();
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // Source: wolfadventure_lib.php wa_api_validate {adventure} -> {ok, problems:[{path, level: error|warning, text, say?}]}: the saved
    // draft through the one validator, whose can-it-be-won rule (GAME_CARD_SPINE_SPEC.md section 2) is a warning in a draft and an
    // error at publish - so both are listed here, before the Publish page
    WolfAdventure::api("validate", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mPublishChecking = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            self->mPublishCheck = LLSD::emptyArray();
            self->mPublishCheckError = "Your game could not be checked: " + error;
            self->mScroll[SP_MAP] = 0.f;
            return;
        }
        self->setProblems(reply["problems"]);
        // Source: wolfadventure_lib.php wa_is_win_problem / wa_publish_win_errors (WA_WIN_PREFIXES 'game card: ', 'main quest: ',
        // 'winning: '): a can-it-be-won warning of a draft is an error at publish for a game that has a card and is not roleplay -
        // so it is a "Must fix" here, not something Go on to Publish would then be refused for
        // as the web Studio (floater_wolf_adventure_studio.js _s3PubCheck): a game WITH a card that is not roleplay is refused for
        // its can-it-be-won problems; a roleplay game has none; wa_publish_ready also refuses while a box still waits for how it
        // leads on ('still to decide: how this beat leads on'). Only what stops publishing is listed; the rest stays on the boxes.
        const bool rp = self->isRoleplay();
        const bool hasCard = std::as_const(self->mDraftPack)["adventure"]["card"].isMap();
        LLSD shown = LLSD::emptyArray();
        for (const LLSD& pr : llsd::inArray(reply["problems"]))
        {
            const std::string t = pr["text"].asString();
            const bool win = t.rfind("game card: ", 0) == 0 || t.rfind("main quest: ", 0) == 0 || t.rfind("winning: ", 0) == 0;
            if (win && rp) continue;
            const bool waits = t == "still to decide: how this beat leads on";
            const bool error = pr["level"].asString() == "error" || (win && hasCard) || waits;
            if (!error && !win) continue;
            LLSD e = pr;
            e["level"] = error ? "error" : "warning";
            shown.append(e);
        }
        if (shown.size() > 0 && self->unbuiltChapters() > 0)
        {
            LLSD u;
            u["level"] = "error";
            u["say"] = "Some chapters are not built yet: press \"Add the next chapter\" above the map, or remove them in Advanced editing.";
            u["path"] = "";
            LLSD all = LLSD::emptyArray();
            all.append(u);
            for (const LLSD& e : llsd::inArray(shown)) all.append(e);
            shown = all;
        }
        self->mPublishCheckGen = self->mDraftGen;
        if (shown.size() == 0)
        {
            // nothing to say: on to the Publish page
            self->mPublishCheck = LLSD();
            self->setStatus(self->isRoleplay() ? std::string("Checked: nothing stops publishing.") : std::string("Checked: your game can be won and nothing stops publishing."), false);
            self->press(SH_PAGE, (S32)SP_PUBLISH);
            self->mScroll[SP_PUBLISH] = 0.f;
            return;
        }
        self->mPublishCheck = shown;
        self->mScroll[SP_MAP] = 0.f;   // the card is near the top of the page
    });
}

F32 WolfAdventureStudioView::publishCheckCard(F32 l, F32 r, F32 top, const Palette& p)
{
    if (!mPublishCheck.isArray()) return top;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    S32 errors = 0, warnings = 0;
    for (const LLSD& pr : llsd::inArray(mPublishCheck)) (pr["level"].asString() == "error" ? errors : warnings)++;
    const S32 MAX_SHOWN = 12;
    // the rows: words wrapped beside a Show me button
    const F32 sw = btnW("Show me");
    struct Row { std::vector<std::string> mLines; bool mError; std::string mPath; };
    std::vector<Row> rows;
    for (const LLSD& pr : llsd::inArray(mPublishCheck))
    {
        if ((S32)rows.size() >= MAX_SHOWN) break;
        const bool err = pr["level"].asString() == "error";
        rows.push_back({ wrap(f, (err ? "Must fix: " : "Worth a look: ") + cap(problemWords(pr), 300), r - l - 24.f - sw - 12.f, 4), err, pr["path"].asString() });
    }
    std::string intro;
    if (!mPublishCheckError.empty()) intro = mPublishCheckError;
    else if (errors > 0) intro = llformat("Before you publish: %d thing%s must be fixed so the game can be played and won.", errors, errors == 1 ? "" : "s");
    else intro = "Before you publish: these are worth a look, but they won't stop you publishing.";
    if (mPublishCheckGen != mDraftGen && mPublishCheckError.empty()) intro += " You have changed your game since: press Check again.";
    const std::vector<std::string> il = wrap(f, intro, r - l - 24.f, 3);
    F32 h = 10.f + (F32)il.size() * lh + 8.f;
    for (const Row& rw : rows) h += llmax(28.f, (F32)rw.mLines.size() * lh + 6.f) + 4.f;
    const S32 more = (S32)mPublishCheck.size() - (S32)rows.size();
    if (more > 0) h += (F32)small->getLineHeight() + 4.f;
    h += 8.f + 30.f + 10.f;
    card(l, top - h, r, top, p, 8.f);
    // a red rim when something must be fixed (and the words say so: never colour alone)
    chamferEdge(l + 0.5f, top - h + 0.5f, r - 0.5f, top - 0.5f, 8.f, 1.5f, errors > 0 || !mPublishCheckError.empty() ? DANGER_INK : WARN_INK);
    F32 y = top - 10.f;
    for (const std::string& ln : il) { textL(f, ln, l + 12.f, y, errors > 0 || !mPublishCheckError.empty() ? DANGER_INK : p.mText); y -= lh; }
    y -= 8.f;
    for (const Row& rw : rows)
    {
        const F32 rh = llmax(28.f, (F32)rw.mLines.size() * lh + 6.f);
        F32 ty = y - 3.f;
        for (const std::string& ln : rw.mLines) { textL(f, ln, l + 12.f, ty, rw.mError ? DANGER_INK : WARN_INK); ty -= lh; }
        const std::string path = rw.mPath;
        const S32 is = act([this, path]() { flowShowMe(path); });
        btn(r - 12.f - sw, y - 26.f, r - 12.f, y, "Show me", SH_E, is, "Select the box (or open the line) this is about", p, !path.empty());
        y -= rh + 4.f;
    }
    if (more > 0) { textL(small, llformat("... and %d more on Advanced editing > Publish.", more), l + 12.f, y, p.mDim); y -= (F32)small->getLineHeight() + 4.f; }
    y -= 8.f;
    {
        F32 x = l + 12.f;
        const S32 ia = act([this]() { mPublishCheck = LLSD(); publishCheck(); });
        btn(x, y - 30.f, x + btnW("Check again"), y, mPublishChecking ? "Checking..." : "Check again", SH_E, ia, "Check your game again after fixing things", p, !mPublishChecking);
        x += btnW("Check again") + 8.f;
        if (errors == 0 && mPublishCheckError.empty())
        {
            const S32 ig = act([this]() { mPublishCheck = LLSD(); press(SH_PAGE, (S32)SP_PUBLISH); mScroll[SP_PUBLISH] = 0.f; });
            btn(x, y - 30.f, x + btnW("Go on to Publish"), y, "Go on to Publish", SH_E, ig, "The Publish page: where your game runs, and publishing it", p);
            x += btnW("Go on to Publish") + 8.f;
        }
        const S32 ic = act([this]() { mPublishCheck = LLSD(); mPublishCheckError.clear(); });
        btn(r - 12.f - btnW("Close"), y - 30.f, r - 12.f, y, "Close", SH_E, ic, "Hide this list", p);
    }
    return top - h - 10.f;
}

void WolfAdventureStudioView::flowShowMe(const std::string& path)
{
    // a box's or an ending's own path, or anything under it: that box selected on the map (its side paths opened), in view
    if (path.rfind("/adventure/card", 0) == 0 || path == "/adventure")
    {
        // the game card at the top of this page: its line opened (a problem about the card as a whole goes to its goal)
        static const char* FIELDS[] = { "/adventure/card/youAre", "/adventure/card/goal", "/adventure/card/win", "/adventure/card/lose", "/adventure/card/genre" };
        std::string want = "/adventure/card/goal";
        for (const char* fp : FIELDS) if (path.rfind(fp, 0) == 0) want = fp;
        mFocusPtr = want;
        mFocusScroll = true;
        mScroll[SP_MAP] = 0.f;
        return;
    }
    flowLayout();
    S32 found = -1;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
    {
        const std::string& np = mFlowNodes[i].mPtr;
        if (path == np || path.rfind(np + "/", 0) == 0) { found = i; break; }
    }
    if (found < 0) { gotoProblem(path); return; }   // not a box: its field on Advanced editing
    if (mFlowNodes[found].mHidden)
    {
        // open the main step it hangs under, then lay out again (the same draft: the view is not re-fitted)
        for (const FlowNode& n : mFlowNodes) if (n.mMain && n.mStep == mFlowNodes[found].mOwner) mFlowOpenSteps.insert(n.mId);
        mFlowGen = 0;
        flowLayout();
        found = -1;
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
        {
            const std::string& np = mFlowNodes[i].mPtr;
            if (path == np || path.rfind(np + "/", 0) == 0) { found = i; break; }
        }
        if (found < 0) return;
    }
    const FlowNode& n = mFlowNodes[found];
    if (n.mEnding) { mFlowSelEnd = n.mId; mFlowSel.clear(); }
    else { mFlowSelEnd.clear(); mFlowSel = n.mId; helperFocus(mFlowSel); }
    // the box near the map's top left: view x = l + 14 + x * z + panX, view y = top - 14 - y * z + panY (drawFlow's sx / sy)
    mFlowPanX = 40.f - n.mX * mFlowZoom;
    mFlowPanY = n.mY * mFlowZoom - 26.f;
    mFlowFitGen = mFlowGen;
    setStatus("That box is selected on the map; what it needs is under the map.", false);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// [SETUP WALK 2026-10-18] "Let's set up your game!" - GAME_CARD_SPINE_SPEC.md "Change (Paul, ~14:30Z)" and "Change (brain, setup
// walk, ~15:00Z)" (php/wolfadventure_setup_lib.php). As the web Studio's _suCard / _suDo / _suPoll / _suAfterMade.
//
// Persona and story: a resident who has just made a game is told "Let's set up your game!" and walked through it one thing at a
// time - the start board, each character, each thing, each place - so at the end there is a game anyone can see and play.
// Flow: a game is made -> Gameplay, the walk on top -> each step: its words, ONE primary button (the step's own `button`), Skip
// for now, Back -> in inventory mode the piece goes to the inventory and the author drags it onto the ground: "Waiting for you to
// put it down..." until the piece reports itself (`setup` asked again) -> the next step -> doneText. A quiet Clean up (asks first).
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Source: floater_wolf_adventure_studio.js SU_POLL_MS = 3000
    const F64 SETUP_POLL_SEC = 3.0;
    /** The step the walk is on: `next`, else the first not done or skipped, else the first. -1 when there are no steps. */
    S32 setupCurrent(const LLSD& su)
    {
        const LLSD& st = su["steps"];
        if (!st.isArray() || st.size() == 0) return -1;
        const std::string next = su["next"].asString();
        for (S32 i = 0; i < (S32)st.size(); ++i) if (!next.empty() && st[i]["key"].asString() == next) return i;
        for (S32 i = 0; i < (S32)st.size(); ++i) if (!st[i]["done"].asBoolean() && !st[i]["skipped"].asBoolean()) return i;
        return 0;
    }
    bool setupWaiting(const LLSD& su)
    {
        for (const LLSD& x : llsd::inArray(su["steps"])) { const std::string s = x["state"].asString(); if (s == "waiting" || s == "placing") return true; }
        return false;
    }
}

bool WolfAdventureStudioView::setupComplete() const
{
    const LLSD& su = mSetup;
    if (!su.isMap() || !su["steps"].isArray() || su["steps"].size() == 0) return false;
    return su["complete"].asBoolean() || su["next"].asString().empty();
}

namespace
{
    /** A `setup` the walk can use: {steps: [...]} (the web's _setupOf returns null otherwise and the old list stays). */
    bool setupUsable(const LLSD& v) { return v.isMap() && v["steps"].isArray(); }
}

bool WolfAdventureStudioView::setupActive() const
{
    // Source: wolfadventure_setup_lib.php wsu_view {steps, done, of, next, complete, mode, doneText}; complete = no next (the web's
    // _setupOf: complete === true, or no next with steps)
    const LLSD& su = mSetup;
    if (!su.isMap() || !su["steps"].isArray() || su["steps"].size() == 0 || mOpenId.empty() || !openIsOwner()) return false;
    const bool complete = su["complete"].asBoolean() || su["next"].asString().empty();
    return !complete;
}

F32 WolfAdventureStudioView::setupCard(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLSD& su = std::as_const(mSetup);
    const S32 ci = setupCurrent(su);
    if (ci < 0) return top;
    const LLSD& cur = su["steps"][ci];
    const std::string key = cur["key"].asString(), state = cur["state"].asString();
    if (state == "waiting" || state == "placing") { if (mSetupWaitKey != key) { mSetupWaitKey = key; mSetupWaitSince = now(); } }
    else mSetupWaitKey.clear();
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
    const F32 lh = (F32)f->getLineHeight();
    const std::vector<std::string> tl = wrap(bold, cap(cur["title"].asString(), 120), r - l - 24.f, 2);
    const std::vector<std::string> xl = wrap(f, cap(cur["text"].asString(), 600), r - l - 24.f, 8);
    const bool waiting = state == "waiting" || state == "placing";
    // the step's own error whenever it has one (a piece put down on the wrong region keeps waiting, with its reason)
    const std::string err = !mSetupError.empty() && mSetupErrorFor == key ? mSetupError : cap(cur["error"].asString(), 400);
    // a long wait gets a hint (the piece may have gone into a box, or not reached the ground)
    const bool longWait = (state == "waiting" || state == "placing") && mSetupWaitKey == key && mSetupWaitSince > 0.0 && now() - mSetupWaitSince > 60.0;
    const std::vector<std::string> el = err.empty() ? std::vector<std::string>() : wrap(f, err, r - l - 24.f, 5);
    const F32 hh = (F32)display(22.f)->getLineHeight();
    const std::vector<std::string> wl = longWait ? wrap(f, "Still waiting? Make sure you dragged it onto the ground, not into an object. Or press the button again for a new copy.", r - l - 24.f, 3)
                                                 : std::vector<std::string>();
    // the buttons: Clean up goes on a line of its own when the row would not fit
    const std::string goLab0 = mSetupBusy == "rez" ? std::string("Working...") : (cur["button"].asString().empty() ? std::string("Put it here") : cap(cur["button"].asString(), 64));
    const F32 rowW = llmax(160.f, btnW(goLab0) + 24.f) + 8.f + btnW("Skip for now") + 8.f + btnW("Back") + 16.f + btnW("Clean up");
    const bool twoRows = rowW > r - l - 24.f;
    const F32 h = 12.f + hh + 4.f + lh + 6.f + (F32)tl.size() * (F32)bold->getLineHeight() + 4.f + (F32)xl.size() * lh
                + (waiting ? 8.f + (F32)wrap(f, state == "waiting" ? std::string("Waiting for you to put it on the ground... If it won't go down, you may not be allowed to build here: try land you own.")
                                                                    : std::string("Putting it down..."), r - l - 24.f, 3).size() * lh + 4.f + 6.f : 0.f) + (wl.empty() ? 0.f : 6.f + (F32)wl.size() * lh) + (el.empty() ? 0.f : 8.f + (F32)el.size() * lh)
                + 12.f + 34.f + (twoRows ? 40.f : 0.f) + 12.f;
    card(l, top - h, r, top, p, 8.f);
    chamferEdge(l + 0.5f, top - h + 0.5f, r - 0.5f, top - 0.5f, 8.f, 1.5f, p.mAccent);
    F32 y = top - 12.f;
    textL(display(22.f), "Let's set up your game!", l + 12.f, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
    y -= hh + 4.f;
    textL(f, llformat("Step %d of %d", ci + 1, (S32)su["steps"].size()), l + 12.f, y, p.mDim);
    y -= lh + 6.f;
    for (const std::string& ln : tl) { textL(bold, ln, l + 12.f, y, p.mText); y -= (F32)bold->getLineHeight(); }
    y -= 4.f;
    for (const std::string& ln : xl) { textL(f, ln, l + 12.f, y, p.mText); y -= lh; }
    if (waiting)
    {
        y -= 8.f;
        for (const std::string& ln : wrap(f, state == "waiting" ? std::string("Waiting for you to put it on the ground... If it won't go down, you may not be allowed to build here: try land you own.")
                                                       : std::string("Putting it down..."), r - l - 24.f, 3))
        { textL(f, ln, l + 12.f, y, p.mAccent); y -= lh; }
        y -= 4.f;
        // the moving bar (as the progress card's), so the wait is plainly alive
        const F32 bl = l + 12.f, br = r - 12.f, bt = y, bb = y - 6.f;
        rect(bl, bb, br, bt, fade(p.mDim, 0.3f));
        const F32 t = (F32)fmod(LLFrameTimer::getTotalSeconds(), 2.4) / 2.4f;
        const F32 u = t < 0.5f ? t * 2.f : (1.f - t) * 2.f;
        const F32 hw = (br - bl) * 0.22f;
        rect(bl + u * (br - bl - hw), bb, bl + u * (br - bl - hw) + hw, bt, p.mAccent);
        y -= 6.f;
    }
    if (!wl.empty())
    {
        y -= 6.f;
        for (const std::string& ln : wl) { textL(f, ln, l + 12.f, y, p.mDim); y -= lh; }
    }
    if (!el.empty())
    {
        y -= 8.f;
        for (const std::string& ln : el) { textL(f, ln, l + 12.f, y, DANGER_INK); y -= lh; }
    }
    y -= 12.f;
    {
        const bool busy = !mSetupBusy.empty();
        const std::string& goLab = goLab0;
        F32 x = l + 12.f;
        const F32 gw = llmax(160.f, btnW(goLab) + 24.f);
        const S32 ig = act([this, key]()
        {
            // setup_rez {adventure, step, here}: where the viewer sees the author, as look_pad sends it (helperHere)
            LLSD extra;
            extra["step"] = key;
            const LLSD here = helperHere();
            if (here.isMap()) extra["here"] = here;
            setupDo("setup_rez", extra, key, "rez");
        });
        // while it waits for the piece to be put down, the waiting line is the main thing: its button is an ordinary one (as the web)
        btn(x, y - 34.f, x + gw, y, goLab, SH_E, ig, "Does this step", p, !busy, state == "waiting" ? 0 : BTN_PRIMARY);
        x += gw + 8.f;
        const std::string sk = mSetupBusy == "skip" ? std::string("Working...") : std::string("Skip for now");
        const S32 is = act([this, key]() { LLSD extra; extra["step"] = key; setupDo("setup_skip", extra, key, "skip"); });
        btn(x, y - 34.f, x + btnW(sk), y, sk, SH_E, is, "Leave this one out for now. Press Back to come back to it.", p, !busy);
        x += btnW(sk) + 8.f;
        bool anyDone = false;
        for (const LLSD& st : llsd::inArray(su["steps"]))
        {
            const std::string ss = st["state"].asString();
            if (st["done"].asBoolean() || st["skipped"].asBoolean() || ss == "waiting" || ss == "failed" || ss == "placing") anyDone = true;   // the web's canBack
        }
        const std::string bk = mSetupBusy == "undo" ? std::string("Working...") : std::string("Back");
        const S32 ib = act([this, key]() { setupDo("setup_undo", LLSD::emptyMap(), key, "undo"); });
        btn(x, y - 34.f, x + btnW(bk), y, bk, SH_E, ib, "Undo the last step: what it put on the land is taken away", p, !busy && anyDone);
        const std::string cl = mSetupBusy == "clean" ? std::string("Working...") : std::string("Clean up");
        const S32 ic = act([this]() { ask(SC_SETUP_CLEAN, 0); });
        if (twoRows) y -= 40.f;
        btn(r - 12.f - btnW(cl), y - 34.f, r - 12.f, y, cl, SH_E, ic, "Takes everything this game put on the land away, and the setup starts again", p, !busy);
    }
    return top - h - 10.f;
}

void WolfAdventureStudioView::setupDo(const std::string& action, const LLSD& extra, const std::string& step_key, const std::string& busy_word)
{
    if (mOpenId.empty() || !mSetupBusy.empty()) return;
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the walk changes the saved game (and the Studio reads it again): a change still waiting here goes first, so nothing is lost
        if (!mSaving) saveDraftNow();
        mSetupError = "Saving your last change first - press that button again in a moment.";
        mSetupErrorFor = step_key;
        return;
    }
    mSetupBusy = busy_word;
    mSetupError.clear();
    mSetupErrorFor.clear();
    ++mSetupSeq;   // a poll sent before this action is not taken after it (the web's suSeq)
    LLSD body = extra.isMap() ? extra : LLSD::emptyMap();
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId;
    const bool wasComplete = setupComplete();
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // Source: GAME_CARD_SPINE_SPEC.md "Change (brain, setup walk, ~15:00Z)": setup_rez {adventure, step, here?} -> {setup, say, etag,
    // pack?}; setup_skip {adventure, step} -> {setup, say}; setup_undo {adventure} -> {setup, say, etag?, pack?}; setup_clean
    // {adventure, confirm} -> {setup, say}. Every refusal is a plain sentence. As the web Studio's _suDo.
    WolfAdventure::api(action, body, [handle, adv, action, step_key, wasComplete](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv) return;   // another game is open now (its own walk state was reset when it opened)
        self->mSetupBusy.clear();
        ++self->mSetupSeq;
        if (!ok)
        {
            self->mSetupError = error;
            self->mSetupErrorFor = step_key;
            self->setStatus(error, true);
            return;
        }
        if (setupUsable(reply["setup"])) self->mSetup = reply["setup"];
        // the game changed (setup_rez / setup_undo send pack + etag): read again, so the pack, its etag and its problems agree; an
        // etag alone is never taken without its pack (the save's 409 guard against lost changes stays whole)
        const bool changed = reply["pack"].isMap() || (!reply["etag"].asString().empty() && reply["etag"].asString() != self->mEtag);
        if (changed) self->loadOpen();   // get_draft brings the walk too (the reply's list stays until then)
        if (action == "setup_clean") self->mPadCount = 0;   // the land clean-up takes the look pads too
        self->mSetupPollAt = now() + SETUP_POLL_SEC;
        if (self->setupComplete() && !wasComplete)
        {
            self->mSetupJustDone = adv;
            const std::string dt = cap(std::as_const(self->mSetup)["doneText"].asString(), 300);
            self->setStatus(dt.empty() ? std::string("Your game is set up! Press Play from the start to try it.") : dt, false);
            return;
        }
        const std::string say = cap(reply["say"].asString(), 400);
        if (!say.empty()) self->setStatus(say, false);
    });
}

void WolfAdventureStudioView::setupPollTick()
{
    // the walk changed the game while an edit was unsaved: read again once that edit is saved
    if (mSetupReloadPending && mDirtyAt <= 0.0 && !mSaving && !mOpenId.empty()) { mSetupReloadPending = false; loadOpen(); }
    if (mPage != SP_MAP || mOpenId.empty() || mSetupPolling || !mSetupBusy.empty() || !mSetup.isMap() || !setupWaiting(mSetup)) return;
    if (now() < mSetupPollAt) return;
    mSetupPolling = true;
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string adv = mOpenId;
    const U32 seq = mSetupSeq;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // setup {adventure} -> {setup, say: ""}: a Studio waiting for a piece to be put down asks again (as the web Studio's _suPoll)
    WolfAdventure::api("setup", body, [handle, adv, seq](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mSetupPolling = false;
        self->mSetupPollAt = now() + SETUP_POLL_SEC;
        if (self->mOpenId != adv || !self->mSetupBusy.empty() || self->mSetupSeq != seq) return;   // an action came between
        if (!ok)
        {
            // a read that keeps failing stops after 5 tries and says so (red, stays); pressing the step's button starts it again
            if (++self->mSetupPollFails >= 5)
            {
                self->mSetupPollFails = 0;
                self->mSetupError = "The Studio lost track of it: " + error + ". If you have put it down, close the Studio and open your game again.";
                self->mSetupErrorFor = std::as_const(self->mSetup)["next"].asString();
                self->mSetupPollAt = now() + 3600.0;   // stopped until an action
            }
            return;
        }
        self->mSetupPollFails = 0;
        if (!setupUsable(reply["setup"])) return;   // the list could not be made just now: the old one stays
        const bool wasComplete = self->setupComplete();
        const S32 doneBefore = std::as_const(self->mSetup)["done"].asInteger();
        self->mSetup = reply["setup"];
        auto reload = [self]()
        {
            // the pieces changed the game (the start, homes, areas): read again - after an unsaved edit is saved, never over it
            if (self->mDirtyAt > 0.0 || self->mSaving) { if (!self->mSaving) self->saveDraftNow(); self->mSetupReloadPending = true; }
            else self->loadOpen();
        };
        if (self->setupComplete() && !wasComplete)
        {
            self->mSetupJustDone = adv;
            const std::string dt = cap(std::as_const(self->mSetup)["doneText"].asString(), 300);
            self->setStatus(dt.empty() ? std::string("Your game is set up!") : dt, false);
            reload();
            return;
        }
        if (std::as_const(self->mSetup)["done"].asInteger() > doneBefore)
        {
            self->setStatus("Done - that is on the land now.", false);
            reload();
        }
    });
}

void WolfAdventureStudioView::setupAfterMade(const std::string& adv, S32 or_page, const std::string& made)
{
    if (adv.empty() || mOpenId != adv) return;
    LLSD body;
    body["adventure"] = adv;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("setup", body, [handle, adv, or_page, made](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != adv) return;
        if (ok && setupUsable(reply["setup"])) self->mSetup = reply["setup"];
        const bool walk = self->setupActive();
        self->mCharsWho.clear();
        self->press(SH_PAGE, walk ? (S32)SP_MAP : or_page);
        self->mScroll[walk ? SP_MAP : or_page] = 0.f;
        self->setStatus(walk ? made + " Let's set up your game!" : made + (or_page == (S32)SP_CHARS ? std::string(" Now check your characters.") : std::string()), false);
    });
}

F32 WolfAdventureStudioView::footBarHeight() const
{
    // [FIXED FOOT 2026-10-17] pages 2 and 3 of the owner's open game: a 34 px row of buttons with 6 px above and below.
    // [GAME CARD 2026-10-18] GAME_CARD_SPINE_SPEC.md section 5: "Play from the start" is on every page's foot once a game exists, so
    // page 1 has the bar too then (not while a build is in hand: its card is the page's one thing); a test that did not start says
    // why in one red line above the buttons.
    const bool owner = !mOpenId.empty() && openIsOwner();
    const bool bar = owner && ((mPage == SP_CHARS || mPage == SP_MAP) || (mPage == SP_HELPER && mBuildStep == B_NONE && hasBoxes()));
    if (!bar) return 0.f;
    return 46.f + (mTestError.empty() ? 0.f : (F32)LLFontGL::getFontSansSerifSmall()->getLineHeight() * 2.f + 6.f);
}

void WolfAdventureStudioView::drawFootBar(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    // drawn after the clipped page (never covered by it) and its hits registered after the page's (the bar wins, WolfGameHits::at
    // takes the last added). Paul (page 2): "keep the buttons at the bottom visible please"
    rect(l, t - 1.f, r, t, fade(p.mDim, 0.35f));
    F32 top = t;
    if (!mTestError.empty())
    {
        // the test's refusal (the brain's reason, plain words), red, until the next Play from the start
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        F32 ey = top - 4.f;
        for (const std::string& ln : wrap(small, "Play from the start did not start: " + mTestError, r - l, 2))
        {
            textL(small, ln, l, ey, DANGER_INK);
            ey -= (F32)small->getLineHeight();
        }
        top -= (F32)small->getLineHeight() * 2.f + 6.f;
    }
    const F32 bt = top - 6.f, bb = bt - 34.f;
    F32 x = l;
    auto left = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled = true, U32 extra = 0)
    {
        const F32 bw = btnW(lab);
        btn(x, bb, x + bw, bt, lab, SH_E, act(fn), tip, p, enabled, extra);
        x += bw + 8.f;
    };
    // the right-hand group, measured first so it sits against the right edge (and after the left group in a narrow window)
    auto rightGroup = [&](const std::vector<std::pair<std::string, F32>>& items, const std::function<void(size_t, F32, F32)>& draw)
    {
        F32 w = 0.f;
        for (const auto& it : items) w += it.second + 8.f;
        F32 rx = llmax(r - w + 8.f, x);
        for (size_t i = 0; i < items.size(); ++i) { draw(i, rx, rx + items[i].second); rx += items[i].second + 8.f; }
    };
    // [GAME CARD 2026-10-18] Play from the start: the test from the first box (testStart), then the test page. Primary on page 3 when
    // no chapter is waiting to be built (the band's Add the next chapter is the primary then); a quiet button on pages 1 and 2, whose
    // primary is Build my game / Next: set up your places.
    const std::string play = "Play from the start";
    const bool playPrimary = mPage == SP_MAP && unbuiltChapters() == 0 && mChapterApplyJob.empty() && !setupActive();   // the walk's button is the one   // one primary: Put it into my game first
    auto playBtn = [&](F32 bl, F32 br)
    {
        btn(bl, bb, br, bt, play, SH_E, act([this]()
        {
            mTestError.clear();
            testStart(std::string(), [this]() { mPage = SP_TEST; mScroll[SP_TEST] = 0.f; });
        }), "Try your game yourself from the first box (only you play it)", p, mBusy == 0 && hasBoxes(), playPrimary ? BTN_PRIMARY : 0);
    };
    // [TABS 2026-10-17] the bar holds each page's primary action only (the tabs across the top are the way between pages):
    // page 2 "Next: set up your places"; page 3 "Clean up the land", "Advanced editing", "Publish". My games lives on the Story page.
    if (mPage == SP_HELPER)
    {
        const F32 pw = btnW(play) + 20.f;
        rightGroup({ { play, pw } }, [&](size_t, F32 bl, F32 br) { playBtn(bl, br); });
        return;
    }
    if (mPage == SP_CHARS)
    {
        const std::string nx = "Next: Gameplay \xE2\x96\xB8";   // [WORDING 2026-10-18] GAME_CARD_SPINE_SPEC.md "Wording (2026-10-18)" (web agent)
        const F32 nw = btnW(nx) + 20.f, pw = btnW(play) + 20.f;
        rightGroup({ { play, pw }, { nx, nw } }, [&](size_t i, F32 bl, F32 br)
        {
            if (i == 0) { playBtn(bl, br); return; }
            btn(bl, bb, br, bt, nx, SH_E, act([this]() { press(SH_PAGE, (S32)SP_MAP); mScroll[SP_MAP] = 0.f; }),
                "Gameplay: your story map, where you place each box on your land", p, true, BTN_PRIMARY);
        });
        return;
    }
    // page 3 (the words and tips as the foot of pageMap had them)
    // [IN-WORLD STUFF 2026-10-17] Paul: "Clean up the land" - shown once the brain is known to have land_cleanup (landCleanupTick)
    if (landCleanupShown()) left("Clean up the land", "Takes every character, look pad and object this game put on the land you stand on (asks first; your own objects stay)",
                                 [this]() { landCleanupAsk(); }, !mCleanupBusy, BTN_DANGER);
    const std::string adv = "Advanced editing", pub = mPublishChecking ? "Checking..." : "Publish";
    rightGroup({ { adv, btnW(adv) }, { pub, btnW("Checking...") }, { play, btnW(play) + 20.f } }, [&](size_t i, F32 bl, F32 br)
    {
        if (i == 0) btn(bl, bb, br, bt, adv, SH_E, act([this]() { press(SH_PAGE, (S32)SP_OUTLINE); mScroll[SP_OUTLINE] = 0.f; }),
                        "More detailed pages for experienced makers", p);   // [WORDING 2026-10-18]
        else if (i == 1) btn(bl, bb, br, bt, pub, SH_E, act([this]() { publishCheck(); }),
                             "Checks that your game can be won first, then the land it runs on and Publish", p, !mPublishChecking);
        else playBtn(bl, br);
    });
}

F32 WolfAdventureStudioView::helperColumn(F32 l, F32 r, F32 top, const Palette& p)
{
    // the slim helper column (SIMPLE_STUDIO_SPEC.md page 3): the last few lines, the box (when no map box is selected: the box
    // panel has it then), Send, Undo that, one plain cost line
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 cr = r;
    F32 cy = head(l, cr, top, "Your helper", p);
    if (mHelperMissing)
        cy = para(l, cr, cy, "The helper is not on this grid yet. You can still place each box and say where it leads.", WARN_INK, true) - 6.f;
    {
        const S32 n = mHelperLog.isArray() ? (S32)mHelperLog.size() : 0;
        if (n == 0)
        {
            textL(bold, "Helper", l, cy, p.mAccent);
            cy -= slh;
            cy = para(l + 8.f, cr, cy, "Click a box on the map and tell me what happens there, or ask me anything about your game.", p.mText, true) - 4.f;
        }
        for (S32 i = llmax(0, n - 6); i < n; ++i)
        {
            const LLSD& t = std::as_const(mHelperLog)[i];
            const bool you = t["who"].asString() == "you" || t["who"].asString() == "author";   // helper_history says "author"
            textL(bold, you ? "You" : "Helper", l, cy, you ? p.mDim : p.mAccent);
            cy -= slh;
            cy = para(l + 8.f, cr, cy, cap(t["text"].asString(), 2000), p.mText, true) - 4.f;
        }
        if (mHelperBusy) { textL(small, "The helper is thinking...", l, cy, p.mDim); cy -= slh + 4.f; }
    }
    if (!mHelperError.empty())
    {
        textL(bold, "Problem:", l, cy, DANGER_INK);
        cy = para(l + 60.f, cr, cy, mHelperError, DANGER_INK, true) - 4.f;
    }
    if (mFlowSel.empty())
    {
        fieldMulti(l, cy - 58.f, cr, cy, mHelperBox, "Tell the helper what happens, or ask...", F_HELPER, p);
        cy -= 64.f;
        const S32 is = act([this]() { helperSend(); });
        btn(l, cy - 30.f, l + 100.f, cy, mHelperBusy ? "Sending..." : "Send", SH_E, is, "Send it to the helper (Enter)", p, !mHelperBusy && !mHelperMissing);
        cy -= 36.f;
    }
    else
    {
        std::string bt;
        for (const FlowNode& n : mFlowNodes) if (n.mId == mFlowSel && !n.mEnding) bt = n.mTitle;
        cy = para(l, cr, cy, "Say what happens at \"" + cap(bt, 50) + "\" under the map (click the empty map to talk about the whole game).", p.mDim, true) - 6.f;
    }
    {
        const S32 iu = act([this]() { helperUndo(); });
        btn(l, cy - 30.f, l + btnW("Undo that"), cy, "Undo that", SH_E, iu, "Put back what the helper last changed", p, !mHelperBusy && !mHelperMissing);
        cy -= 36.f;
    }
    // [M6] a script the helper wrote on request: copied whole (never put anywhere by the helper)
    if (!mHelperScript.empty())
    {
        const S32 ic = act([this]()
        {
            const LLWString w = utf8str_to_wstring(mHelperScript);
            LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
            setStatus("The script is on the clipboard: paste it into a New Script in your object, and read it all before you save it.", false);
        });
        btn(l, cy - 30.f, l + btnW("Copy the script it wrote"), cy, "Copy the script it wrote", SH_E, ic, "Put the whole script the helper wrote on the clipboard", p);
        cy -= 36.f;
    }
    // one plain cost line (WOLF_MIND_V1.md §7.1: 1 credit = 1 US cent)
    if (mHelperTodayCredits >= 0.0)
    {
        textL(small, "Helper today: " + wholeCredits(mHelperTodayCredits), l, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cr - l);
        cy -= slh + 4.f;
    }
    if (mHelperWalking) cy = para(l, cr, cy, "The helper is walking with you. Click it in the world to talk, or type here.", p.mDim, true) - 4.f;
    return cy;
}
