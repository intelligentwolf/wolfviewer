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
    const F32 ROW = 26.f;
    // the chart's own units (zoom 1): a box and the gaps between layers and rows. [SIMPLE STUDIO 2026-10-17] a box holds its
    // head, two lines of words, "Where: ..." and its place line (five small lines at 13 px, plus the padding)
    // [MAP LABELS 2026-10-17] the gap between layers holds an arrow's words on their plate (drawFlow: LABEL_W of words + 8 of
    // plate at zoom 1) without touching the boxes either side
    const F32 BOX_W = 176.f, BOX_H = 80.f, GAP_X = 120.f, GAP_Y = 22.f;
    const F32 LABEL_W = 110.f;
    // [SIMPLE STUDIO] the story's hint of where a scene happens: the build writes the scene's gmNotes as "Where: <hint>\n<summary>"
    // (wolfmind_lib.php:2621)
    const char* const WHERE_PREFIX = "Where: ";
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the flowchart's model: boxes and arrows from the draft
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

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
        endIx[n.mId] = (S32)mFlowNodes.size();
        mFlowNodes.push_back(n);
    }
    // the arrows (ADVENTURE_PACK_V1.md §11: trigger next, effects goto / end, decision options, scene exits)
    // `full` = the whole words (the box panel's "Leads to"); the map shows at most 26 characters of them (textL cuts with "...")
    auto add = [this](S32 from, S32 to, const std::string& full, bool dashed)
    {
        if (from < 0 || to < 0) return;
        for (const FlowEdge& e : mFlowEdges) if (e.mFrom == from && e.mTo == to && e.mFull == full) return;
        FlowEdge e; e.mFrom = from; e.mTo = to; e.mFull = full; e.mLabel = cap(full, 26); e.mDashed = dashed;
        mFlowEdges.push_back(e);
    };
    auto target = [&](const LLSD& e) -> S32
    {
        if (e.has("end")) { auto it = endIx.find(e["end"].asString()); return it == endIx.end() ? -1 : it->second; }
        if (e["goto"]["beat"].isString()) { auto it = beatIx.find(e["goto"]["beat"].asString()); return it == beatIx.end() ? -1 : it->second; }
        if (e["goto"]["scene"].isString()) { auto it = sceneFirst.find(e["goto"]["scene"].asString()); return it == sceneFirst.end() ? -1 : it->second; }
        return -1;
    };
    auto words = [](const LLSD& t) -> std::string
    {
        const std::string on = t["on"].asString();
        std::string w = on == "talk" ? "talk" : on == "arrive" ? "reach" : on == "touch" ? "touch" : on == "ready" ? "ready" : on == "choose" ? "choice" : on;
        // the helper's placeholder way on: a gm step "Still to decide" (id tbd-..., wolfadventure_story_lib.php was_chain_after)
        if (on == "gm" && t["id"].asString().rfind("tbd-", 0) == 0) w = "still to decide";
        if (on == "won") w = "if they win";     // [M4] a fight's ways on
        if (on == "lost") w = "if they lose";
        if (on == "script") w = "script says " + cap(t["id"].asString(), 20);   // [M6] wolfAdventureFire
        if (t.has("if")) w += " (if...)";
        return w;
    };
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
                    if (t["next"].isString()) { auto it = beatIx.find(t["next"].asString()); if (it != beatIx.end()) add(from, it->second, words(t), false); }
                    for (const LLSD& e : llsd::inArray(t["do"])) add(from, target(e), words(t), false);
                }
                for (const LLSD& d : llsd::inArray(bt["decisions"]))
                    for (const LLSD& o : llsd::inArray(d["options"]))
                        for (const LLSD& e : llsd::inArray(o["do"])) add(from, target(e), cap(o["label"].asString(), 120) + (o.has("if") ? " (if...)" : ""), false);
                for (const LLSD& e : llsd::inArray(bt["onEnter"])) add(from, target(e), "at once", false);
            }
            for (const LLSD& x : llsd::inArray(sc["exits"]))
            {
                auto it = sceneFirst.find(x["to"].asString());
                if (it != sceneFirst.end() && lastBeat >= 0) add(lastBeat, it->second, cap(x["label"].asString().empty() ? std::string("exit") : x["label"].asString(), 120), true);
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
    const LLSD& saved = pk["flow"];
    if (saved.isMap())
        for (FlowNode& n : mFlowNodes)
        {
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
    const F32 y = top;
    const F32 b = y - h;
    mFlowRect = LLRect((S32)l, (S32)y, (S32)r, (S32)b);
    card(l, b, r, y, p, 4.f);
    // the ground: drag to pan (registered first, so every box is on top of it)
    hit(l, b, r, y, SH_FLOW_BG, 0, "Drag to move round the map; the mouse wheel zooms; click a box to set it up", false);
    if (mFlowNodes.empty())
    {
        para(l + 12.f, r - 12.f, y - 12.f, "No boxes yet. Go to Your story and press Build my game, or tell the helper your idea on the right.", p.mDim, true);
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
        for (const FlowEdge& e : mFlowEdges)
        {
            const FlowNode& a = mFlowNodes[e.mFrom];
            const FlowNode& c = mFlowNodes[e.mTo];
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
            const F32 bl = sx(n.mX), bt = sy(n.mY), br = bl + BOX_W * z, bb = bt - BOX_H * z;
            if (br < l || bl > r || bt < b || bb > y) continue;
            const bool sel = !mFlowSel.empty() && mFlowSel == n.mId && !n.mEnding;
            const S32 np = problemsUnder(n.mPtr);
            const LLColor4 tint = n.mEnding ? p.mAccent : (n.mPlaced ? OK_INK : WARN_INK);
            chamfer(bl, bb, br, bt, 5.f, fade(tint, 0.26f), fade(tint, 0.16f));
            chamferEdge(bl + 0.5f, bb + 0.5f, br - 0.5f, bt - 0.5f, 5.f, sel ? 2.5f : 1.f, sel ? p.mAccent : (np ? WARN_INK : fade(tint, 0.7f)));
            if (z >= 0.55f)
            {
                F32 ty = bt - 5.f;
                std::string head = n.mEnding ? "END" : (n.mStart ? "START" : std::string());
                if (!n.mFight.empty()) head += (head.empty() ? "" : " \xC2\xB7 ") + std::string("FIGHT: ") + n.mFight;   // [M4] words, not colour alone
                if (!head.empty()) { textL(bbf, head, bl + 6.f, ty, n.mEnding ? p.mAccent : (n.mFight.empty() ? OK_INK : WARN_INK), LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 12.f); ty -= blh; }
                // [MAP LABELS 2026-10-17] the title gets two lines whenever two fit above "Where:" and the place line (the box's
                // padding: 5 above, 4 below the place line, 2 spare), one only when they do not - never cut mid-word while a
                // second line fits; an ending has no "Where:" or place line
                S32 room = (S32)std::floor((BOX_H * z - 5.f - 4.f - 2.f - (n.mEnding ? 0.f : blh)) / blh);
                if (!head.empty()) --room;
                if (!n.mEnding && !n.mWhere.empty()) --room;
                for (const std::string& ln : wrap(bf, n.mTitle.empty() ? std::string("(no words yet)") : n.mTitle, BOX_W * z - 12.f, llclamp(room, 1, 2)))
                { textL(bf, ln, bl + 6.f, ty, p.mText); ty -= blh; }
                if (!n.mEnding)
                {
                    if (!n.mWhere.empty()) { textL(bf, "Where: " + n.mWhere, bl + 6.f, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 12.f); ty -= blh; }
                    textL(bf, n.mPlaced && !n.mPlace.empty() ? "Placed: " + n.mPlace : std::string("not placed yet"),
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
            std::string tip = (n.mEnding ? "Ending: " : "Box: ") + (n.mTitle.empty() ? n.mId : n.mTitle);
            if (!n.mEnding) tip += n.mPlaced ? " - placed" : " - not placed yet";
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
            for (const FlowNode& n : mFlowNodes) placed.push_back({ sx(n.mX), sy(n.mY) - BOX_H * z, sx(n.mX) + BOX_W * z, sy(n.mY) });
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

F32 WolfAdventureStudioView::flowBoxPanel(F32 l, F32 r, F32 top, const Palette& p)
{
    // [SIMPLE STUDIO 2026-10-17] the box the author clicked (SIMPLE_STUDIO_SPEC.md page 3): ONLY Place it here, Bring <character>
    // here, What happens here? (the helper box + Send, about this box), Leads to..., and More... (the full box tools)
    F32 y = top;
    S32 ni = -1;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding && mFlowNodes[i].mId == mFlowSel) ni = i;
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
        if (e.mFrom == ni) leads += (leads.empty() ? "" : "; ") + e.mFull + " -> " + cap(mFlowNodes[e.mTo].mEnding ? "END: " + mFlowNodes[e.mTo].mTitle : mFlowNodes[e.mTo].mTitle, 40);
    y = para(l, r, y, leads.empty() ? std::string("It leads nowhere yet: press Leads to..., or tell the helper what happens next.") : "Leads to: " + leads,
             leads.empty() ? WARN_INK : p.mText) - 4.f;
    const std::string pr = problemAt(bp + "/id").empty() ? problemAt(bp) : problemAt(bp + "/id");
    if (!pr.empty()) y = para(l, r, y, pr, DANGER_INK) - 4.f;
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
    }, !mHelperBusy, BTN_PRIMARY);
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
        btn(l, y - 30.f, l + 100.f, y, mHelperBusy ? "Sending..." : "Send", SH_E, is, "Tell the helper (Enter)", p, !mHelperBusy && !mHelperMissing, BTN_PRIMARY);
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
    if (mOpenId.empty()) { mHelperError = "Make your game first: go to Your story and press Build my game."; return; }
    // a turn is at most 2,000 characters (helper_chat's WAS_TEXT_MAX, wolfadventure_story_lib.php:30); a whole story belongs on page 1
    if (utf8str_to_wstring(text).size() > 2000)
    {
        mHelperError = "That is long for one message (2,000 characters at most). Put a whole story in Your story and press Build my game, or say it in parts.";
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
                self->mHelperError = "Your adventure changed in another window, so I read it again. Press Send to say that again.";
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
                else self->setStatus("Your game changed meanwhile: I read it again - press Put it in my adventure once more.", true);
            }
            else
            {
                if (self->mBuildStep == B_APPLYING) self->buildFail("Your game was built, but it could not be put in: " + error);
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
        self->setStatus(words, false);
        // [SIMPLE STUDIO 2026-10-17] the game is in: page 2, the characters
        if (self->mBuildStep == B_APPLYING) self->mBuildStep = B_NONE;
        self->press(SH_PAGE, (S32)SP_CHARS);
        self->mScroll[SP_CHARS] = 0.f;
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
            if (reply.isMap() && reply.has("etag")) { self->mHelperError = "Your adventure changed in another window, so I read it again - press that once more."; self->loadOpen(); }
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
                                                             { "gm", "You (or a game master) press Continue" },
                                                             { "ready", "Everyone in the party presses Ready (2 or more players)" } };
    openPicker("How is this box done?", how, [this, beat_id](const std::string& when)
    {
        if (when.empty()) return;
        auto chooseTarget = [this, beat_id, when](const std::string& cast, const std::string& object)
        {
            flowLayout();
            std::vector<std::pair<std::string, std::string>> to;
            for (const FlowNode& n : mFlowNodes)
                if (n.mId != beat_id) to.push_back({ n.mEnding ? "end:" + n.mId : n.mId, (n.mEnding ? "END: " : "") + cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
            openPicker("Then it leads to", to, [this, beat_id, when, cast, object](const std::string& target)
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
        if (std::abs(x - mFlowGrabX) < 3 && std::abs(y - mFlowGrabY) < 3) mFlowSel.clear();
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
    y = experimentalNote(cl, cr, y, p);   // [W69 2026-10-17] the experimental notice, always, under the intro
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
            y = para(cl, cr, y, "A whole book costs a few hundred credits and takes several minutes to build; you see the exact cost next.", p.mDim, true) - 6.f;
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
            btn(cl, y - 30.f, cl + btnW("Buy credits...", 160.f), y, "Buy credits...", SH_E, ibuy, "Opens the Wolf Territories credits page in your browser; then press Build my game again", p, true, BTN_PRIMARY);
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
        const S32 bookMinutes = (S32)ceil(2.0 + 1.5 * 5.0);
        const std::string q = llformat("%s this game costs about %d credits (US $%.2f). %s %s it?", rebuild ? "Rebuilding" : "Building", credits, (F64)credits / 100.0,
                                       book ? llformat("A book this size takes about %d minutes, and you can close the window while it works.", bookMinutes).c_str()
                                            : "It takes a minute or two.",
                                       rebuild ? "Rebuild" : "Build");
        const std::vector<std::string> ql = wrap(LLFontGL::getFontSansSerif(), q, colW - 24.f, 3);
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
    else
    {
        // [SIMPLE STUDIO 2026-10-17] the progress card: what is happening, that closing is fine, the clock, the steps, the spend,
        // Stop the build
        const LLFontGL* f = LLFontGL::getFontSansSerif();
        const F32 lh = (F32)f->getLineHeight();
        const std::vector<std::string> about = wrap(f, "This takes a few minutes for a book (less for a short story). You can close this window: the build "
                                                       "carries on, and your game opens on Your characters when it is done.", colW - 24.f, 6);
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
        steps.push_back({ "Outline" + (cur == 1 && !outlineSub.empty() ? " " + outlineSub : std::string()), 1 });
        if (mBuildActs > 0) for (S32 a = 1; a <= mBuildActs; ++a) steps.push_back({ llformat("Act %d of %d", a, mBuildActs), 1 + a });
        else steps.push_back({ "Acts", 2 });   // how many is the outline's choice: known once the brain says "Act 1 of N"
        steps.push_back({ "Putting it together", 100 });
        steps.push_back({ "Putting it into your game", 101 });
        // how long: 2 minutes for the outline + 1.5 minutes an act (Paul's rule, 2026-10-17); a book is split into BOOK_ACTS = 5
        // acts (wolffloateradventurestudioai.cpp buildQuote) until the brain says otherwise; a typed story is a minute or two
        const S32 BOOK_ACTS_EST = 5;
        std::string takes;
        if (mBuildKind == 2 || (mBuildKind == 0 && mBuildActs >= 3))
            takes = llformat("A book this size takes about %d minutes.", (S32)ceil(2.0 + 1.5 * (F64)(mBuildActs > 0 ? mBuildActs : BOOK_ACTS_EST)));
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
            spent = mBuildMax > 0 ? llformat("Spent so far: %.0f of at most %d credits.", std::as_const(mJob)["cost"]["credits"].asReal(), mBuildMax)
                                  : llformat("Spent so far: %.0f credits.", std::as_const(mJob)["cost"]["credits"].asReal());
        }
        const bool stoppable = mBuildStep == B_RUNNING && !mJobId.empty();
        const F32 hh = (F32)display(22.f)->getLineHeight();
        const F32 bh = 12.f + hh + 6.f + (F32)about.size() * lh + 8.f + 6.f + 8.f + lh + (takes.empty() ? 0.f : lh) + 6.f + (F32)steps.size() * lh + 4.f
                     + (spent.empty() ? 0.f : slh + 2.f) + (stoppable ? 10.f + 30.f : 0.f) + 12.f;
        card(cl, y - bh, cr, y, p, 8.f);
        F32 cy = y - 12.f;
        textL(display(22.f), "Building your game...", cl + 12.f, cy, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, colW - 24.f);
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
        F32 y = para(l, r, top, "Make your game first: go to Your story and press Build my game.", p.mDim) - 8.f;
        const S32 ib = act([this]() { press(SH_PAGE, (S32)SP_HELPER); });
        btn(l, y - 30.f, l + btnW("\xE2\x97\x82 Your story"), y, "\xE2\x97\x82 Your story", SH_E, ib, "Page 1", p, true, BTN_PRIMARY);
        return y - 38.f;
    }
    if (!openIsOwner())
    {
        F32 y = para(l, r, top, "Only the game's owner builds it. As a game master or teacher, use Publish to see it and Run to run it.", p.mDim) - 8.f;
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
    // [TABS 2026-10-17] Paul: page 3 is "Gameplay" wherever a newcomer sees it (the tab, this heading, the Guide's steps)
    textL(display(28.f), "Gameplay: your story map", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(28.f)->getLineHeight() + 8.f;
    // ---- the band ----
    {
        const std::string text = "This map is your whole story. Each box is one moment, and the arrows show where the players can go next. Now walk "
                                 "your land with your helper: press Next place, go to where that moment happens and press Place it here, then tell "
                                 "the helper what happens there. When every box is placed, press Play it now to try your game.";
        const std::vector<std::string> lines = wrap(LLFontGL::getFontSansSerif(), text, r - l - 24.f, 6);
        const F32 lh = (F32)LLFontGL::getFontSansSerif()->getLineHeight();
        // the helper's own line for the chosen box (helper_next.say), the test's refusal, both in words under the buttons
        const LLSD& hn = std::as_const(mHelperNext);
        std::string say = hn.isMap() ? cap(hn["say"].asString(), 300) : std::string();
        const S32 left = hn.isMap() ? hn["left"].asInteger() : 0;
        if (!say.empty() && left > 0) say += llformat("  (%d box%s still need%s something)", left, left == 1 ? "" : "es", left == 1 ? "s" : "");
        const std::vector<std::string> sl = say.empty() ? std::vector<std::string>() : wrap(small, say, r - l - 24.f, 4);
        const std::vector<std::string> el = mTestError.empty() ? std::vector<std::string>() : wrap(small, "Play it now did not start: " + mTestError, r - l - 24.f, 4);
        const F32 bh = 10.f + (F32)lines.size() * lh + 8.f + 32.f + (sl.empty() ? 0.f : 6.f + (F32)sl.size() * slh) + (el.empty() ? 0.f : 6.f + (F32)el.size() * slh) + 10.f;
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
            noBoxes ? std::string("Build your game on Your story first.") : std::string("The next box that is not placed yet, and what to do there (free)"), p, mBusy == 0 && !mHelperBusy && !noBoxes);
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
        {
            // Source: wolffloateradventurestudioedit.cpp testStart: test_start {adventure, beat, ai}; refused with the brain's reason
            const F32 pw = llmax(150.f, btnW("Play it now") + 24.f);
            const S32 ip = act([this]()
            {
                mTestError.clear();
                testStart(std::string(), [this]() { mPage = SP_TEST; mScroll[SP_TEST] = 0.f; });
            });
            btn(r - 12.f - pw, by - 30.f, r - 12.f, by, "Play it now", SH_E, ip, "Try your game yourself, from the first box (only you)", p, mBusy == 0 && !mFlowNodes.empty(), BTN_PRIMARY);
        }
        by -= 32.f;
        if (!sl.empty()) { by -= 6.f; for (const std::string& ln : sl) { textL(small, ln, l + 12.f, by, p.mText); by -= slh; } }
        if (!el.empty()) { by -= 6.f; for (const std::string& ln : el) { textL(small, ln, l + 12.f, by, DANGER_INK); by -= slh; } }
        y -= bh + 10.f;
    }
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

F32 WolfAdventureStudioView::footBarHeight() const
{
    // [FIXED FOOT 2026-10-17] pages 2 and 3 of the owner's open game: a 34 px row of buttons with 6 px above and below.
    // Page 1's way on (Build my game) is in its content; with no game open, or someone else's, the pages draw their own way back.
    const bool bar = (mPage == SP_CHARS || mPage == SP_MAP) && !mOpenId.empty() && openIsOwner();
    return bar ? 46.f : 0.f;
}

void WolfAdventureStudioView::drawFootBar(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    // drawn after the clipped page (never covered by it) and its hits registered after the page's (the bar wins, WolfGameHits::at
    // takes the last added). Paul (page 2): "keep the buttons at the bottom visible please"
    rect(l, t - 1.f, r, t, fade(p.mDim, 0.35f));
    const F32 bt = t - 6.f, bb = bt - 34.f;
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
    // [TABS 2026-10-17] the bar holds each page's primary action only (the tabs across the top are the way between pages):
    // page 2 "Next: set up your places"; page 3 "Clean up the land", "Advanced editing", "Publish". My games lives on the Story page.
    if (mPage == SP_CHARS)
    {
        const std::string nx = "Next: set up your places \xE2\x96\xB8";
        const F32 nw = btnW(nx) + 20.f;
        rightGroup({ { nx, nw } }, [&](size_t, F32 bl, F32 br)
        {
            btn(bl, bb, br, bt, nx, SH_E, act([this]() { press(SH_PAGE, (S32)SP_MAP); mScroll[SP_MAP] = 0.f; }),
                "Page 3: your story map, where you place each box on your land", p, true, BTN_PRIMARY);
        });
        return;
    }
    // page 3 (the words and tips as the foot of pageMap had them)
    // [IN-WORLD STUFF 2026-10-17] Paul: "Clean up the land" - shown once the brain is known to have land_cleanup (landCleanupTick)
    if (landCleanupShown()) left("Clean up the land", "Takes every character, look pad and object this game put on the land you stand on (asks first; your own objects stay)",
                                 [this]() { landCleanupAsk(); }, !mCleanupBusy, BTN_DANGER);
    const std::string adv = "Advanced editing", pub = "Publish";
    rightGroup({ { adv, btnW(adv) }, { pub, btnW(pub) } }, [&](size_t i, F32 bl, F32 br)
    {
        if (i == 0) btn(bl, bb, br, bt, adv, SH_E, act([this]() { press(SH_PAGE, (S32)SP_OUTLINE); mScroll[SP_OUTLINE] = 0.f; }),
                        "The expert pages: outline, cast, places, story, rules, lore, import and more", p);
        else btn(bl, bb, br, bt, pub, SH_E, act([this]() { press(SH_PAGE, (S32)SP_PUBLISH); mScroll[SP_PUBLISH] = 0.f; }),
                 "Checks, the land it runs on, and Publish", p);
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
        btn(l, cy - 30.f, l + 100.f, cy, mHelperBusy ? "Sending..." : "Send", SH_E, is, "Send it to the helper (Enter)", p, !mHelperBusy && !mHelperMissing, BTN_PRIMARY);
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
        textL(small, llformat("Today %.2f credits", mHelperTodayCredits), l, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cr - l);
        cy -= slh + 4.f;
    }
    if (mHelperWalking) cy = para(l, cr, cy, "The helper is walking with you. Click it in the world to talk, or type here.", p.mDim, true) - 4.f;
    return cy;
}
