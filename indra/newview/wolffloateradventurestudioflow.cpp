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
#include "llinventorymodel.h"
#include "llviewerinventory.h"
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
    // the chart's own units (zoom 1): a box and the gaps between layers and rows
    const F32 BOX_W = 170.f, BOX_H = 62.f, GAP_X = 70.f, GAP_Y = 22.f;
    // "Only this place": a box anchored within this many metres of the author (design choice, STORY_FIRST_DESIGN.md §1)
    const F32 NEAR_M = 30.f;
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
    auto add = [this](S32 from, S32 to, const std::string& label, bool dashed)
    {
        if (from < 0 || to < 0) return;
        for (const FlowEdge& e : mFlowEdges) if (e.mFrom == from && e.mTo == to && e.mLabel == label) return;
        FlowEdge e; e.mFrom = from; e.mTo = to; e.mLabel = label; e.mDashed = dashed;
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
                        for (const LLSD& e : llsd::inArray(o["do"])) add(from, target(e), cap(o["label"].asString(), 24) + (o.has("if") ? " (if...)" : ""), false);
                for (const LLSD& e : llsd::inArray(bt["onEnter"])) add(from, target(e), "at once", false);
            }
            for (const LLSD& x : llsd::inArray(sc["exits"]))
            {
                auto it = sceneFirst.find(x["to"].asString());
                if (it != sceneFirst.end() && lastBeat >= 0) add(lastBeat, it->second, cap(x["label"].asString().empty() ? std::string("exit") : x["label"].asString(), 24), true);
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
    F32 y = head(l, r - 220.f, top, "The flowchart", p);
    {
        // the near filter and the zoom, as words on buttons
        const std::string fl = mFlowOnlyHere ? "Only this place: on" : "Only this place: off";
        const S32 iff = act([this]() { mFlowOnlyHere = !mFlowOnlyHere; });
        btn(r - btnW(fl), top + 2.f - 24.f, r, top + 2.f, fl, SH_E, iff, "Show only the boxes placed within 30 m of you", p, true, mFlowOnlyHere ? BTN_ACTIVE : 0);
        const S32 ir = act([this]() { mFlowPanX = mFlowPanY = 0.f; mFlowZoom = 1.f; });
        btn(r - btnW(fl) - 6.f - btnW("Fit"), top + 2.f - 24.f, r - btnW(fl) - 6.f, top + 2.f, "Fit", SH_E, ir, "Back to the start, normal size (drag the empty ground to move; the mouse wheel zooms)", p);
    }
    const F32 b = y - h;
    mFlowRect = LLRect((S32)l, (S32)y, (S32)r, (S32)b);
    card(l, b, r, y, p, 4.f);
    // the ground: drag to pan (registered first, so every box is on top of it)
    hit(l, b, r, y, SH_FLOW_BG, 0, "Drag to move round the flowchart; click a box to work on it", false);
    if (mFlowNodes.empty())
    {
        para(l + 12.f, r - 12.f, y - 12.f, mOpenId.empty() ? "Your game's flowchart appears here as you and the helper build it."
                                                          : "No boxes yet: tell the helper your idea, or add a beat on the Outline page.", p.mDim, true);
        return b - 8.f;
    }
    // which boxes show: all, or those anchored near the author (an unplaced box never counts as near)
    std::vector<bool> show(mFlowNodes.size(), true);
    if (mFlowOnlyHere)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        const LLVector3 me = rgn ? rgn->getPosRegionFromAgent(gAgent.getPositionAgent()) : LLVector3();
        const LLSD& pk = mDraftPack;
        for (size_t i = 0; i < mFlowNodes.size(); ++i)
        {
            const FlowNode& n = mFlowNodes[i];
            bool near = false;
            if (!n.mEnding && rgn)
            {
                const LLSD& bt = WolfAdvStudio::sdAt(pk, n.mPtr);
                const std::string scp = n.mPtr.substr(0, n.mPtr.rfind("/beats/"));
                const LLSD& sc = WolfAdvStudio::sdAt(pk, scp);
                auto close = [&me](F64 x, F64 yy) { return x >= 0.0 && std::hypot(x - me.mV[VX], yy - me.mV[VY]) <= NEAR_M; };
                for (const LLSD& s : llsd::inArray(pk["places"]["spots"]))
                    if (s["id"].asString() == bt["marker"].asString() && s["region"].asString() == rgn->getRegionID().asString()) near = near || close(s["pos"][0].asReal(), s["pos"][1].asReal());
                for (const LLSD& a : llsd::inArray(pk["places"]["areas"]))
                    if (a["id"].asString() == sc["area"].asString() && a["region"].asString() == rgn->getRegionID().asString())
                        near = near || close(a["shape"]["circle"]["centre"][0].asReal(), a["shape"]["circle"]["centre"][1].asReal());
            }
            show[i] = near;
        }
    }
    const F32 z = mFlowZoom;
    auto sx = [&](F32 x) { return l + 14.f + x * z + mFlowPanX; };
    auto sy = [&](F32 yy) { return y - 14.f - yy * z + mFlowPanY; };
    {
        LLLocalClipRect clip(LLRect((S32)l + 2, (S32)y - 2, (S32)r - 2, (S32)b + 2));
        // arrows under the boxes: source's right middle to target's left middle; a back arrow bends round above
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        for (const FlowEdge& e : mFlowEdges)
        {
            if (!show[e.mFrom] || !show[e.mTo]) continue;
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
            F32 lx, ly;
            if (x2 > x1 + 4.f)
            {
                const F32 mx = (x1 + x2) * 0.5f;
                seg(x1, y1, mx, y1); seg(mx, y1, mx, y2); seg(mx, y2, x2, y2);
                lx = mx + 3.f; ly = (y1 + y2) * 0.5f;
            }
            else
            {
                // backwards (a loop): up over the boxes and down into the target from its left
                const F32 up = llmax(y1, y2) + BOX_H * z * 0.5f + 12.f;
                seg(x1, y1, x1 + 12.f, y1); seg(x1 + 12.f, y1, x1 + 12.f, up); seg(x1 + 12.f, up, x2 - 12.f, up); seg(x2 - 12.f, up, x2 - 12.f, y2); seg(x2 - 12.f, y2, x2, y2);
                lx = (x1 + x2) * 0.5f; ly = up + 2.f;
            }
            // the arrow's head (pointing right, into the target's left side)
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(col.mV);
            gGL.vertex2f(x2, y2); gGL.vertex2f(x2 - 7.f, y2 + 4.f); gGL.vertex2f(x2 - 7.f, y2 - 4.f);
            gGL.end();
            if (!e.mLabel.empty() && z >= 0.7f) textL(small, e.mLabel, lx, ly, p.mText, LLFontGL::LEFT, LLFontGL::BOTTOM, 120.f * z);
        }
        // the boxes
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
        {
            if (!show[i]) continue;
            const FlowNode& n = mFlowNodes[i];
            const F32 bl = sx(n.mX), bt = sy(n.mY), br = bl + BOX_W * z, bb = bt - BOX_H * z;
            if (br < l || bl > r || bt < b || bb > y) continue;
            const bool sel = !mFlowSel.empty() && mFlowSel == n.mId && !n.mEnding;
            const S32 np = problemsUnder(n.mPtr);
            chamfer(bl, bb, br, bt, 5.f, n.mEnding ? fade(p.mAccent, 0.25f) : p.mButtonTop, n.mEnding ? fade(p.mAccent, 0.18f) : p.mButtonBottom);
            chamferEdge(bl + 0.5f, bb + 0.5f, br - 0.5f, bt - 0.5f, 5.f, sel ? 2.5f : 1.f, sel ? p.mAccent : (np ? WARN_INK : p.mButtonEdge));
            if (z >= 0.55f)
            {
                F32 ty = bt - 5.f;
                std::string head = n.mEnding ? "END" : (n.mStart ? "START" : std::string());
                if (!n.mFight.empty()) head += (head.empty() ? "" : " \xC2\xB7 ") + std::string("FIGHT: ") + n.mFight;   // [M4] words, not colour alone
                if (!head.empty()) { textL(bold, head, bl + 6.f, ty, n.mEnding ? p.mAccent : (n.mFight.empty() ? OK_INK : WARN_INK), LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 12.f); ty -= slh; }
                for (const std::string& ln : wrap(small, n.mTitle.empty() ? std::string("(no words yet)") : n.mTitle, BOX_W * z - 12.f, head.empty() ? 2 : 1))
                { textL(small, ln, bl + 6.f, ty, p.mText); ty -= slh; }
                if (!n.mEnding)
                    textL(small, n.mPlace.empty() ? std::string("not placed yet") : (n.mPlaced ? n.mPlace : n.mPlace + " - not placed yet"),
                          bl + 6.f, bb + 4.f + slh, n.mPlaced && !n.mPlace.empty() ? p.mDim : WARN_INK, LLFontGL::LEFT, LLFontGL::TOP, BOX_W * z - 30.f);
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
            if (np) tip += llformat(" - %d thing%s to look at (click it, then see below)", np, np == 1 ? "" : "s");
            hit(bl, bb, br, bt, SH_FLOW_BOX, i, tip + ". Click to work on it with the helper");
            if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_FLOW_BOX, i)) focusRing(bl, bb, br, bt, p);
            if (!n.mEnding)
            {
                // the handle: drag from it to another box to make this one lead there
                const F32 hx = br, hy = (bt + bb) * 0.5f;
                disc(hx, hy, 5.f, mFlowConnectFrom == i ? p.mAccent : p.mText);
                hit(hx - 7.f, hy - 7.f, hx + 7.f, hy + 7.f, SH_FLOW_HANDLE, i, "Drag from here to another box: this box then leads there", false);
            }
        }
        // a connection being dragged
        if (mFlowConnectFrom >= 0 && mFlowConnectFrom < (S32)mFlowNodes.size())
        {
            const FlowNode& a = mFlowNodes[mFlowConnectFrom];
            lineW(sx(a.mX + BOX_W), sy(a.mY + BOX_H * 0.5f), (F32)mFlowMouseX, (F32)mFlowMouseY, 2.f, p.mAccent);
        }
    }
    return b - 8.f;
}

F32 WolfAdventureStudioView::flowBoxPanel(F32 l, F32 r, F32 top, const Palette& p)
{
    // the box the author clicked: what the helper is working on, and the quick edits (the full editor is the Story page)
    F32 y = top;
    S32 ni = -1;
    for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i) if (!mFlowNodes[i].mEnding && mFlowNodes[i].mId == mFlowSel) ni = i;
    if (ni < 0)
    {
        return para(l, r, y, mFlowNodes.empty() ? std::string() : "Click a box to work on it: the helper then talks about that box, and you can anchor it where you stand.",
                    p.mDim, true) - 6.f;
    }
    const FlowNode& n = mFlowNodes[ni];
    const std::string bp = n.mPtr;
    const LLSD& b = WolfAdvStudio::sdAt(mDraftPack, bp);
    y = head(l, r, y, "This box", p);
    y = fText(l, r, y, "What the players do", bp + "/objective", b["objective"], 0, 240, 's', false, "One thing to do, shown in their Journal", p);
    // where it leads, in words, with the problems on it
    std::string leads;
    for (const FlowEdge& e : mFlowEdges)
        if (e.mFrom == ni) leads += (leads.empty() ? "" : "; ") + e.mLabel + " -> " + cap(mFlowNodes[e.mTo].mEnding ? "END: " + mFlowNodes[e.mTo].mTitle : mFlowNodes[e.mTo].mTitle, 40);
    y = para(l, r, y, leads.empty() ? std::string("It leads nowhere yet: drag from its handle to the next box, or tell the helper what happens next.") : "Leads: " + leads,
             leads.empty() ? WARN_INK : p.mDim, true) - 4.f;
    const std::string pr = problemAt(bp + "/id").empty() ? problemAt(bp) : problemAt(bp + "/id");
    if (!pr.empty()) y = para(l, r, y, pr, DANGER_INK, true) - 4.f;
    F32 x = l;
    auto tool = [&](const std::string& lab, const std::string& tip, const std::function<void()>& fn, bool enabled, U32 extra = 0)
    {
        const F32 bw = btnW(lab);
        if (x + bw > r) { x = l; y -= ROW + 4.f; }
        const S32 idx = act(fn);
        btn(x, y - ROW, x + bw, y, lab, SH_E, idx, tip, p, enabled, extra);
        x += bw + 4.f;
    };
    const std::string sel = bp;
    tool("Open the full editor", "Every setting of this box on the Story page", [this, sel]() { mSel = sel; mPage = SP_STORY; mScroll[SP_STORY] = 0.f; }, true);
    tool("Add a box after it", "A new box this one leads to (you then tell the helper what happens there)", [this, sel]() { addStoryItem(2, sel); }, true);
    // the keyboard way to connect (the alternative to dragging from the handle, STUDIO §6 2.5.7)
    tool("Leads to...", "Choose the box (or ending) this one leads to", [this, ni]()
    {
        std::vector<std::pair<std::string, std::string>> opts;
        for (S32 i = 0; i < (S32)mFlowNodes.size(); ++i)
            if (i != ni) opts.push_back({ flowKey(mFlowNodes[i]), (mFlowNodes[i].mEnding ? "END: " : "") + cap(mFlowNodes[i].mTitle.empty() ? mFlowNodes[i].mId : mFlowNodes[i].mTitle, 70) });
        const std::string fromKey = flowKey(mFlowNodes[ni]);
        openPicker("This box leads to", opts, [this, fromKey](const std::string& v) { if (!v.empty()) flowConnect(fromKey, v); }, "There is no other box yet.");
    }, true);
    tool("Someone walks with them...", "From this box a character walks with the players (at most 2 with a party)", [this]() { pickCompanion(mFlowSel); }, true);
    tool("A script moves it on...", "Your own script (a door, a button, a trap) says when this box is done, and where it leads", [this]() { pickScriptHook(mFlowSel, false); }, !mHelperBusy);
    tool("Tell my scripts it starts", "Your objects hear when this box starts (wolfAdventureListen), to open a door or set off an effect", [this]() { pickScriptHook(mFlowSel, true); }, !mHelperBusy);
    // [M4] a fight as a box - only where the schema knows battles (an older schema file would refuse the draft)
    if (WolfAdvStudio::packHas("battles") && WolfAdvStudio::packHas("enemies"))
        tool(mFlowNodes[ni].mFight.empty() ? "A fight happens here..." : "Change the fight...", "Who they fight here, where winning leads and what losing does",
             [this]() { pickBattle(mFlowSel); }, !mHelperBusy);
    tool("Play from here", "Test Play from this box (only you)", [this]() { const std::string id = mFlowSel; mTestBeat = id; mPage = SP_TEST; testStart(id); }, true);
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
        self->mHelperTodo = reply["todo"].isArray() ? reply["todo"] : LLSD::emptyArray();
        if (reply["cost"].isMap())
        {
            self->mHelperTodayCredits = reply["cost"]["todayCredits"].asReal();
            self->mHelperCap = reply["cost"]["cap"].asReal();
        }
        // §9.1 helper_history.walk {walking, region, node, look}: the helper may still be walking from an earlier visit
        const LLSD& w = reply["walk"];
        if (w.isMap())
        {
            self->mHelperWalking = w["walking"].asBoolean();
            self->mHelperWalkRegion = w["region"].asString();
            self->mHelperLookFrom = w["look"].isMap() ? w["look"]["from"].asString() : w["look"].asString();
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
    if (reply["todo"].isArray()) mHelperTodo = reply["todo"];
    if (reply["cost"].isMap())
    {
        mHelperLastCredits = reply["cost"]["credits"].asReal();
        mHelperTodayCredits = reply["cost"]["todayCredits"].asReal();
        mHelperCap = reply["cost"]["cap"].asReal();
    }
}

void WolfAdventureStudioView::helperCreate(const std::string& text, bool then_book)
{
    if (mHelperBusy || text.empty()) return;
    mHelperError.clear();
    LLSD body;
    std::string title = cap(text, 60);
    if (title.size() >= 60) title = title.substr(0, title.rfind(' ') == std::string::npos ? title.size() : title.rfind(' '));
    body["title"] = title.empty() ? std::string("My adventure") : title;
    body["purpose"] = "adventure";
    if (LLViewerRegion* rgn = gAgent.getRegion()) body["region"] = rgn->getRegionID().asString();
    mHelperBusy = true;
    // mHelperBox keeps the line: it is sent once the new draft has loaded (mHelperPending, pageHelper); for a book, the line
    // was the book's title and Load a book opens instead
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    setStatus("Making your adventure...", false);
    const std::string bookTitle = title;
    // BRAIN_API_V1.md §5.1 create {title, purpose, region}: a failure is said and the line stays to send again
    WolfAdventure::api("create", body, [handle, then_book, bookTitle](bool ok, const LLSD& made, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mHelperBusy = false;
        const std::string id = ok ? made["adventure"]["id"].asString() : std::string();
        if (id.empty()) { self->mHelperError = ok ? std::string("The adventure could not be made - try again.") : "Your adventure could not be made: " + error; return; }
        self->loadList();
        self->openAdventure(id);   // lands on the main screen (its history loads); the draft loads, then the line goes
        if (then_book)
        {
            self->mHelperBox.clear();
            self->mBookTitle = bookTitle == "My adventure" ? std::string() : bookTitle;
            self->mPage = SP_BOOK;
            self->loadJobs();
            return;
        }
        self->mPage = SP_HELPER;
        self->mHelperPending = true;
    });
}

void WolfAdventureStudioView::helperSend()
{
    std::string text = mHelperBox;
    LLStringUtil::trim(text);
    if (text.empty() || mHelperBusy) return;
    mHelperError.clear();
    // nothing open yet: the first line makes the adventure (its name from the line; the helper may rename it), then talks
    if (mOpenId.empty()) { helperCreate(text, false); return; }
    // later turns are at most 2,000 characters (helper_chat's limit); a long story belongs in "Your story"
    if (!mHelperLog.isArray() || mHelperLog.size() > 0)
        if (utf8str_to_wstring(text).size() > 2000)
        {
            mHelperError = "That is long for one message (2,000 characters at most). Put a whole story in Your story and press Build it, or say it in parts.";
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
            self->mHelperError = self->mHelperMissing ? std::string("The helper is not on this grid yet. You can build with the flowchart, Box by box, or the pages under Advanced editing.")
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

void WolfAdventureStudioView::storyQuote()
{
    if (mOpenId.empty() || mBusy > 0) return;
    if (mDirtyAt > 0.0 || mSaving)
    {
        // the brain reads the SAVED story (draft_start fromStory): a change still waiting goes first
        if (!mSaving) saveDraftNow();
        setStatus("Saving your story first - press Build it again in a moment.", false);
        return;
    }
    LLSD body;
    body["adventure"] = mOpenId;
    body["fromStory"] = true;
    body["rights"] = true;
    body["maxCredits"] = mBookMax;
    mBookName = "your story";
    mBookTitle.clear();
    // STORY_FIRST_DESIGN.md §4 draft_start {adventure, fromStory, rights, maxCredits} -> {job, quote:{tokens, credits}}
    call("draft_start", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mJobId = reply["job"].asString();
        self->mJob = LLSD::emptyMap();
        self->mJob["state"] = "quoted";
        self->mJob["quote"] = reply["quote"];
        if (reply["quote"]["credits"].asReal() > (F64)self->mBookMax)
        {
            self->setStatus(llformat("Building it would cost about %.0f credits, more than the %d you allow (Book and assist page: Spend at most).",
                                     reply["quote"]["credits"].asReal(), self->mBookMax), true);
            return;
        }
        self->ask(SC_JOB_START, 0);   // the quote in words, and the most it may spend, before anything runs
    }, "Working out what building it would cost...");
}

void WolfAdventureStudioView::jobApply()
{
    if (mOpenId.empty() || mJobId.empty()) return;
    if (mDirtyAt > 0.0 || mSaving) { if (!mSaving) saveDraftNow(); setStatus("Saving your last change first - press Put it in my adventure again in a moment.", false); return; }
    LLSD body;
    body["job"] = mJobId;
    body["etag"] = mEtag;
    body["mode"] = "auto";
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    ++mBusy;
    setStatus("Putting it in your adventure...", false);
    // STORY_FIRST_DESIGN.md §4 job_apply {job, etag, mode} -> {pack, pack_json, etag, problems, merged{added, updated, kept, stale}, report}
    WolfAdventure::api("job_apply", body, [handle, adv](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mBusy = llmax(0, self->mBusy - 1);
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            if (reply.isMap() && reply.has("etag")) { self->setStatus("Your adventure changed meanwhile: I read it again - press Put it in my adventure once more.", true); self->loadOpen(); }
            else self->setStatus("It could not be put in: " + error, true);
            return;
        }
        self->setDraftText(reply["pack_json"].asString(), reply["pack"]);
        self->mEtag = reply["etag"].asString();
        self->mDirtyAt = 0.0;
        self->setProblems(reply["problems"]);
        const LLSD& m = reply["merged"];
        std::string words = llformat("Done: %d added, %d updated, %d of yours kept as they were.", m["added"].asInteger(), m["updated"].asInteger(), m["kept"].asInteger());
        const S32 stale = m["stale"].isArray() ? (S32)m["stale"].size() : 0;
        if (stale > 0) words += llformat(" %d part%s no longer in the story %s left for you to remove or keep (see the flowchart).", stale, stale == 1 ? "" : "s", stale == 1 ? "is" : "are");
        self->setStatus(words, false);
        self->mPage = SP_HELPER;
    });
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

void WolfAdventureStudioView::helperLook(const std::string& from, const std::string& asset)
{
    if (mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["from"] = from;
    if (!asset.empty()) body["asset"] = asset;
    // §9 helper_look {adventure, from, asset?} -> {look:{from, asset, pending}, walking?, say}
    call("helper_look", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mHelperLookFrom = reply["look"]["from"].asString();
        if (!reply["say"].asString().empty()) { LLSD t; t["who"] = "helper"; t["text"] = cap(reply["say"].asString(), 500); self->mHelperLog.append(t); }
    }, "Changing the helper's look...");
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
    if (mPage != SP_HELPER || !h) return false;
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
    if (mPage != SP_HELPER || !mFlowRect.pointInRect(x, y)) return false;
    mFlowZoom = llclamp(mFlowZoom * (clicks < 0 ? 1.12f : 1.f / 1.12f), 0.35f, 2.f);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════
// the page
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::pageHelper(F32 l, F32 r, F32 top, const Palette& p)
{
    // [WOLF STORY FIRST FRONT DOOR 2026-10-17] the Studio's main screen. Nothing open: the front door. Open: a toolbar (My games,
    // Advanced editing, Test Play, Publish) over three columns - your story (left), the flowchart (centre, the biggest), the
    // helper conversation (right); narrower, the story and the helper share the left column; narrower still, one column.
    if (mOpenId.empty()) return frontDoor(l, r, top, p);
    if (!openIsOwner())
        return para(l, r, top, "Only the adventure's owner builds it. As a game master or teacher, use Publish to see it and Run to run it.", p.mDim);
    if (mDraftJson.empty()) return para(l, r, top, "Opening your adventure...", p.mDim);
    // the first line, kept while the new adventure was made and its draft loaded, goes now
    // (after the helper's history is in: its reply replaces the conversation, and would wipe the line just said)
    if (mHelperPending && !mDraftJson.empty() && !mHelperBusy && mHelperLoaded) { mHelperPending = false; helperSend(); }
    F32 y = top;
    // ---- the toolbar ----
    {
        const S32 ib = act([this]() { closeOpenAdventure(); mPage = SP_HELPER; mScroll[SP_HELPER] = 0.f; mHelperLog = LLSD::emptyArray(); mHelperFor.clear(); loadList(); });
        btn(l, y - 30.f, l + btnW("\xE2\x97\x82 My games"), y, "\xE2\x97\x82 My games", SH_E, ib, "Back to the front door: your games, and a new game from a story", p);
        F32 x = r;
        const LLSD* a = listed(mOpenId);
        const bool published = a && (*a)["status"].asString() == "published";
        const std::string pl = published ? "Update the live game" : "Publish";
        x -= btnW(pl);
        const S32 ip = act([this]() { mPage = SP_PUBLISH; mScroll[SP_PUBLISH] = 0.f; });
        btn(x, y - 30.f, x + btnW(pl), y, pl, SH_E, ip, published ? "Change the game while people play: see who is where first" : "Checks, regions and Publish", p);
        x -= btnW("Test Play") + 6.f;
        const S32 it = act([this]() { mPage = SP_TEST; mScroll[SP_TEST] = 0.f; });
        btn(x, y - 30.f, x + btnW("Test Play"), y, "Test Play", SH_E, it, "Play your game yourself, from any box", p, true, BTN_PRIMARY);
        const bool adv = advancedOpen();
        const std::string al = adv ? "Advanced editing \xE2\x96\xBE" : "Advanced editing \xE2\x96\xB8";
        x -= btnW(al) + 6.f;
        const S32 ia = act([this, adv]() { setAdvancedOpen(!adv); });
        btn(x, y - 30.f, x + btnW(al), y, al, SH_E, ia, adv ? "Hide the expert pages" : "Show the expert pages in a rail: outline, cast, places, story, rules, lore and more", p, true, adv ? BTN_ACTIVE : 0);
        y -= 40.f;
    }
    y = conflictBanner(l, r, y, p);
    const F32 w = r - l;
    if (w >= 1000.f)
    {
        const F32 sR = l + w * 0.25f - 8.f, hL = l + w * 0.70f + 8.f;
        const F32 sy = storyColumn(l, sR, y, p);
        const F32 fh = llmax(320.f, (F32)mPageRect.getHeight() - 300.f);
        F32 fy = drawFlow(sR + 16.f, hL - 16.f, y, fh, p);
        fy = flowBoxPanel(sR + 16.f, hL - 16.f, fy, p);
        const F32 hy = helperColumn(hL, r, y, p);
        return llmin(sy, llmin(fy, hy)) - 8.f;
    }
    if (w >= 700.f)
    {
        const F32 cr = l + w * 0.4f - 8.f;
        F32 cy = helperColumn(l, cr, y, p);
        cy = storyColumn(l, cr, cy - 8.f, p);
        const F32 fh = llmax(320.f, (F32)mPageRect.getHeight() - 300.f);
        F32 fy = drawFlow(cr + 16.f, r, y, fh, p);
        fy = flowBoxPanel(cr + 16.f, r, fy, p);
        return llmin(cy, fy) - 8.f;
    }
    F32 cy = helperColumn(l, r, y, p);
    cy = drawFlow(l, r, cy - 6.f, 360.f, p);
    cy = flowBoxPanel(l, r, cy, p);
    return storyColumn(l, r, cy - 8.f, p) - 8.f;
}

F32 WolfAdventureStudioView::storyColumn(F32 l, F32 r, F32 top, const Palette& p)
{
    // ---- your story (STORY_FIRST_DESIGN.md §4): kept with the adventure; Build it makes or updates the game from it ----
    const F32 cr = r;
    F32 cy = head(l, cr, top, "Your story", p);
    const LLSD& story = std::as_const(mDraftPack)["adventure"]["story"];
    if (story["text"].asString().empty())
        cy = para(l, cr, cy, "The story your game comes from, in your own words. Write or paste it here, then Build it - your own changes are kept.", p.mDim, true) - 4.f;
    cy = fText(l, cr, cy, std::string(), "/adventure/story/text", story["text"], 0, 20000, 's', true, "Your story, in your own words (up to 20,000 characters)", p);
    {
        const S32 ib = act([this]() { storyQuote(); });
        btn(l, cy - ROW, l + btnW("Build it from my story"), cy, "Build it from my story", SH_E, ib,
            "The AI makes characters, choices and endings from your story (a quote first; what you changed yourself is kept)", p, mBusy == 0 && !story["text"].asString().empty());
        cy -= ROW + 6.f;
    }
    // a build running or done: its state, and the way to put it in
    {
        const LLSD& job = std::as_const(mJob);   // read only while drawing
        if (!mJobId.empty() && job.isMap())
        {
            const std::string st = job["state"].asString();
            if (st == "queued" || st == "running") cy = para(l, cr, cy, "Building: " + cap(job["progress"].asString(), 120), p.mDim, true) - 4.f;
            else if (st == "failed") cy = para(l, cr, cy, "The build stopped: " + cap(job["error"].asString(), 200), DANGER_INK, true) - 4.f;
            else if (st == "done" && job["output"].isMap())
            {
                const S32 ia = act([this]() { jobApply(); });
                btn(l, cy - ROW, l + btnW("Put it in my adventure"), cy, "Put it in my adventure", SH_E, ia, "Merge what was built into your adventure (your own work stays as it is)", p, mBusy == 0, BTN_PRIMARY);
                cy -= ROW + 6.f;
            }
        }
    }
    // the no-AI way: the helper goes box by box with buttons
    if (!mHelperMissing)
    {
        const S32 ic = act([this]() { helperNextBox(std::string()); });
        btn(l, cy - ROW, l + btnW("Box by box (no AI)"), cy, "Box by box (no AI)", SH_E, ic, "The helper goes box by box with buttons: place it, bring its characters, say where it leads", p,
            true, mHelperNext.isMap() ? BTN_ACTIVE : 0);
        cy -= ROW + 8.f;
    }
    // ---- [M3] the checklist (the non-AI path, §9 helper_next / helper_do) ----
    if (!mOpenId.empty() && mHelperNext.isMap())
    {
        const LLSD& hn = std::as_const(mHelperNext);   // read only while drawing (no keys made)
        cy = head(l, cr, cy, "Checklist", p);
        cy = para(l, cr, cy, cap(hn["say"].asString(), 400), p.mText, true) - 4.f;
        const S32 left = hn["left"].asInteger();
        if (left > 0) cy = para(l, cr, cy, llformat("%d box%s still need%s something.", left, left == 1 ? "" : "es", left == 1 ? "s" : ""), p.mDim, true) - 4.f;
        F32 x = l;
        for (const LLSD& b : llsd::inArray(hn["buttons"]))
        {
            const std::string lab = cap(b["label"].asString(), 48), id = b["id"].asString();
            const F32 bw = btnW(lab);
            if (x > l && x + bw > cr) { x = l; cy -= ROW + 4.f; }
            const LLSD bb = b;
            const std::string node = hn["node"].asString();
            const S32 ib = act([this, bb, id, node]()
            {
                if (id == "next") helperNextBox(bb["after"].asString());
                else if (id == "lead") pickLead(node);
                else if (id == "battle")   // an older schema without battles would refuse the draft (as the box panel's tool)
                {
                    if (WolfAdvStudio::packHas("battles") && WolfAdvStudio::packHas("enemies")) pickBattle(node);
                    else setStatus("This viewer is too old for fights: update WolfViewer, or tell the helper what happens instead.", true);
                }   // [M4] the fight's pickers fill in the op's lines and ways on
                else if (bb["op"].isMap()) helperDo(bb["op"]);
            });
            btn(x, cy - ROW, x + bw, cy, lab, SH_E, ib, lab, p, !mHelperBusy, id == "place" ? BTN_PRIMARY : 0);
            x += bw + 4.f;
        }
        cy -= ROW + 8.f;
    }
    return cy;
}

F32 WolfAdventureStudioView::helperColumn(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 cr = r;
    F32 cy = head(l, cr, top, "Your helper", p);
    if (mHelperMissing)
        cy = para(l, cr, cy, "The helper is not on this grid yet. You can build with the flowchart, Box by box, and the pages under Advanced editing.", WARN_INK, true) - 6.f;
    // the conversation, newest at the bottom (at most 14 shown); the greeting while nothing is said yet
    {
        const S32 n = mHelperLog.isArray() ? (S32)mHelperLog.size() : 0;
        if (n == 0)
        {
            textL(bold, "Helper", l, cy, p.mAccent);
            cy -= slh;
            cy = para(l + 8.f, cr, cy, WolfAdvStudio::greeting(), p.mText, true) - 4.f;
        }
        for (S32 i = llmax(0, n - 14); i < n; ++i)
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
    // [M6 2026-10-17] a script the helper wrote: shown (the first lines) and copied; never put anywhere by the helper
    if (!mHelperScript.empty())
    {
        cy = head(l, cr, cy, "The script the helper wrote", p);
        cy = para(l, cr, cy, "In your object: Content > New Script, open it, select all its text, paste over it and save. Read it all before you save it.", p.mDim, true) - 2.f;
        // the whole script (at most 6000 bytes), tabs as spaces, every line wrapped: what is copied is what is shown
        std::string shown = mHelperScript;
        for (char& ch : shown) if (ch == '\t') ch = ' ';
        // one source line at a time (wrap() keeps 2048 characters of its text, wolfgamehud.cpp; the script may hold 6000)
        size_t at = 0;
        while (at <= shown.size())
        {
            size_t nl = shown.find('\n', at);
            if (nl == std::string::npos) nl = shown.size();
            const std::string src = shown.substr(at, nl - at);
            if (src.empty()) cy -= slh;
            for (const std::string& ln : wrap(small, src, cr - l - 8.f, 0)) { textL(small, ln, l + 8.f, cy, p.mText); cy -= slh; }
            at = nl + 1;
        }
        cy -= 4.f;
        F32 x = l;
        const S32 ic = act([this]()
        {
            const LLWString w = utf8str_to_wstring(mHelperScript);
            LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
            setStatus("The script is on the clipboard: paste it into a New Script in your object.", false);
        });
        btn(x, cy - ROW, x + btnW("Copy the script"), cy, "Copy the script", SH_E, ic, "Put the whole script on the clipboard", p, true, BTN_PRIMARY);
        x += btnW("Copy the script") + 6.f;
        const S32 ix = act([this]() { mHelperScript.clear(); });
        btn(x, cy - ROW, x + btnW("Done with it"), cy, "Done with it", SH_E, ix, "Hide the script", p);
        cy -= ROW + 8.f;
    }
    // what to say: the box it is about, the field, Send / Undo / a book
    if (!mFlowSel.empty())
    {
        std::string bt;
        for (const FlowNode& n : mFlowNodes) if (n.mId == mFlowSel && !n.mEnding) bt = n.mTitle;
        cy = para(l, cr, cy, "About the box \"" + cap(bt, 60) + "\" (click the ground to talk about the whole game).", p.mDim, true) - 2.f;
    }
    fieldMulti(l, cy - 58.f, cr, cy, mHelperBox, mFlowSel.empty() ? "Tell the helper what happens, or ask..." : "What happens here?", F_HELPER, p);
    cy -= 64.f;
    {
        F32 x = l;
        const S32 is = act([this]() { helperSend(); });
        btn(x, cy - 30.f, x + 100.f, cy, mHelperBusy ? "Sending..." : "Send", SH_E, is, "Send it to the helper (Enter)", p, !mHelperBusy && !mHelperMissing, BTN_PRIMARY);
        x += 106.f;
        const S32 iu = act([this]() { helperUndo(); });
        btn(x, cy - 30.f, x + btnW("Undo that"), cy, "Undo that", SH_E, iu, "Put back what the helper last changed", p, !mHelperBusy && !mHelperMissing);
        x += btnW("Undo that") + 6.f;
        const S32 ib = act([this]() { mPage = SP_BOOK; loadJobs(); });
        if (x + btnW("Load a book") > cr) { x = l; cy -= 34.f; }
        btn(x, cy - 30.f, x + btnW("Load a book"), cy, "Load a book", SH_E, ib, "Read a whole book in and build from it (costs a little, with a quote first)", p);
        cy -= 38.f;
    }
    // the meter: this turn and today (WOLF_MIND_V1.md §7.1: 1 credit = 1 US cent)
    if (mHelperTodayCredits >= 0.0)
    {
        textL(small, llformat("Last answer %.2f credits - today %.2f of %.0f", mHelperLastCredits, mHelperTodayCredits, mHelperCap), l, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cr - l);
        cy -= slh + 4.f;
    }
    if (mHelperTodo.isArray() && mHelperTodo.size() > 0)
    {
        cy = head(l, cr, cy, "Still to decide", p);
        for (const LLSD& t : llsd::inArray(mHelperTodo)) cy = para(l + 8.f, cr, cy, "- " + cap(t.asString(), 240), p.mText, true) - 2.f;
        cy -= 4.f;
    }
    // ---- [M3] the helper in the world (§9): walk with me, its look ----
    if (!mOpenId.empty() && !mHelperMissing)
    {
        F32 x = l;
        const std::string wl = mHelperWalking ? "Stop walking" : "Walk with me";
        const S32 iw = act([this]() { helperWalk(!mHelperWalking); });
        btn(x, cy - ROW, x + btnW(wl), cy, wl, SH_E, iw, mHelperWalking ? "The helper goes (and so do the characters it brought)"
                                                                         : "The helper appears beside you in the world and follows you: talk to it about where you stand", p,
            !mHelperWalkBusy, mHelperWalking ? BTN_ACTIVE : BTN_PRIMARY);
        x += btnW(wl) + 6.f;
        if (x + btnW("Dress it like me") > cr) { x = l; cy -= ROW + 4.f; }
        const S32 i1 = act([this]() { helperLook("wearing", std::string()); });
        btn(x, cy - ROW, x + btnW("Dress it like me"), cy, "Dress it like me", SH_E, i1, "The helper wears what you are wearing now", p);
        x += btnW("Dress it like me") + 6.f;
        if (x + btnW("Other look...") > cr) { x = l; cy -= ROW + 4.f; }
        const S32 i2 = act([this]()
        {
            // a full-permission appearance notecard from the author's inventory (the asset id is visible only with full permissions)
            collectNotecards();
            std::vector<std::pair<std::string, std::string>> opts = { { "~default", "Its own look (a friendly guide)" } };
            for (const auto& it : mPickItems) opts.push_back({ it.first.asString(), cap(it.second, 60) });
            openPicker("The helper's look", opts, [this](const std::string& v)
            {
                if (v.empty()) return;
                if (v == "~default") { helperLook("default", std::string()); return; }
                LLViewerInventoryItem* item = gInventory.getItem(LLUUID(v));
                if (!item || item->getAssetUUID().isNull()) { setStatus("The viewer cannot see that notecard's contents: it needs full permissions.", true); return; }
                helperLook("notecard", item->getAssetUUID().asString());
            }, std::string());
        });
        btn(x, cy - ROW, x + btnW("Other look..."), cy, "Other look...", SH_E, i2, "Its own look, or an appearance notecard from your inventory", p);
        cy -= ROW + 6.f;
        if (mHelperWalking) cy = para(l, cr, cy, "The helper is walking with you. Click it in the world to talk, or type here; it answers out loud to you only.", p.mDim, true) - 4.f;
    }
    return cy;
}
