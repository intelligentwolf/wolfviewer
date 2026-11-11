/**
 * @file wolfgreedyhud.cpp
 * @brief WolfViewer: the Greedy HUD. Design: wolfgreedyhud.h.
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

#include "wolfgreedyhud.h"

#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "llapp.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llfontregistry.h"
#include "llframetimer.h"
#include "llrender.h"
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluiimage.h"
#include "llviewercontrol.h"
#include "llwindow.h"
#include "wolffloatergreedy.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfGreedyHud> r_wolf_greedy_hud("wolf_greedy_hud");

namespace
{
    const F32 PI_F = 3.14159265f;

    // Source: wolfskihud.cpp - the family's night blue and ice and its status colours. Its two insets: band on, or (ours, as
    // Paul asked of the Fishing HUD 11-11) nothing behind the read-outs at all with BACKGROUND off, the words outlined to read
    // over any sky.
    const LLColor4 NAVY_HI(0.09f, 0.17f, 0.28f, 0.94f), NAVY_LO(0.02f, 0.05f, 0.09f, 0.94f);
    const LLColor4 INSET_ON(0.f, 0.f, 0.f, 0.42f);
    const LLColor4 INSET_OFF(0.f, 0.f, 0.f, 0.f);
    LLColor4 INSET = INSET_ON;                         // this frame's (draw() sets it from WolfGreedyHudBackground)
    bool OUTLINE = false;                              // this frame's: background off
    const LLColor4 ICE(0.62f, 0.88f, 1.00f, 1.f), ICE_DIM(0.38f, 0.55f, 0.68f, 1.f);
    const LLColor4 SNOW(0.97f, 0.98f, 1.00f, 1.f);
    const LLColor4 GREY(0.66f, 0.72f, 0.78f, 1.f);
    const LLColor4 GOLD(1.00f, 0.82f, 0.25f, 1.f);
    const LLColor4 AMBER(1.00f, 0.66f, 0.00f, 1.f);
    const LLColor4 GREEN(0.30f, 0.90f, 0.45f, 1.f);
    // Greedy's own dice tints, as the window's (wolffloatergreedy.cpp GOLD_DIE / RED_DIE): a die that scored, a bust
    const LLColor4 GOLD_DIE(1.f, 0.8f, 0.3f, 1.f);
    const LLColor4 RED_DIE(1.f, 0.42f, 0.4f, 1.f);
    // Source: wolfskihud.cpp button faces; ROLL green and BANK gold, as the window's Roll / Bank (floater_wolf_greedy.xml)
    const LLColor4 BTN_MAIN_HI(0.20f, 0.42f, 0.66f, 0.97f), BTN_MAIN_LO(0.08f, 0.18f, 0.32f, 0.97f);
    const LLColor4 BTN_HI(0.16f, 0.26f, 0.38f, 0.97f), BTN_LO(0.06f, 0.11f, 0.18f, 0.97f);
    const LLColor4 BTN_GO_HI(0.16f, 0.52f, 0.30f, 0.97f), BTN_GO_LO(0.05f, 0.24f, 0.12f, 0.97f);
    const LLColor4 BTN_GOLD_HI(0.80f, 0.58f, 0.12f, 0.97f), BTN_GOLD_LO(0.45f, 0.30f, 0.04f, 0.97f);
    const LLColor4 BTN_OFF_HI(0.12f, 0.16f, 0.21f, 0.80f), BTN_OFF_LO(0.05f, 0.07f, 0.10f, 0.80f);
    // the moment's banner: as the window's (wolffloatergreedy.cpp BANNER_PULSE_SECS)
    const F32 BANNER_PULSE_SECS = 0.6f;

    LLColor4 alpha(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    bool reduce_motion()
    {
        static LLCachedControl<bool> reduce(gSavedPerAccountSettings, "WolfAdventureReduceMotion", false);
        return reduce;
    }

    std::string number(S32 n)
    {
        // 10,000 style, as the server's lines (wolfdice_lib.php wd_n; wolffloatergreedy.cpp number)
        std::string s = llformat("%d", n < 0 ? -n : n), out;
        S32 c = 0;
        for (size_t i = s.size(); i-- > 0;)
        {
            out.insert(out.begin(), s[i]);
            if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
        }
        return n < 0 ? "-" + out : out;
    }

    // ---- primitives, view-local, untextured (Source: wolfskihud.cpp's, which are wolfhelmhud.cpp's) ----
    void noTex() { gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE); }
    void vtx(F32 x, F32 y, const LLColor4& c) { gGL.color4fv(c.mV); gGL.vertex2f(x, y); }

    void rectF(F32 l, F32 b, F32 r, F32 t, const LLColor4& c)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, c); vtx(r, b, c); vtx(r, t, c);
        vtx(l, b, c); vtx(r, t, c); vtx(l, t, c);
        gGL.end();
    }

    void rectG(F32 l, F32 b, F32 r, F32 t, const LLColor4& top, const LLColor4& bot)
    {
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b, bot); vtx(r, b, bot); vtx(r, t, top);
        vtx(l, b, bot); vtx(r, t, top); vtx(l, t, top);
        gGL.end();
    }

    void frameW(F32 l, F32 b, F32 r, F32 t, F32 w, const LLColor4& c)
    {
        rectF(l, b, r, b + w, c);
        rectF(l, t - w, r, t, c);
        rectF(l, b, l + w, t, c);
        rectF(r - w, b, r, t, c);
    }

    void roundRectF(F32 l, F32 b, F32 r, F32 t, F32 rad, const LLColor4& top, const LLColor4& bot)
    {
        // Source: wolfskihud.cpp roundRectF (wolfhelmhud.cpp roundRectF)
        rad = llmin(rad, (r - l) * 0.5f, (t - b) * 0.5f);
        const F32 h = llmax(0.001f, t - b);
        auto col = [&](F32 y) { return lerp(bot, top, llclamp((y - b) / h, 0.f, 1.f)); };
        noTex();
        gGL.begin(LLRender::TRIANGLES);
        vtx(l, b + rad, col(b + rad)); vtx(r, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad));
        vtx(l, b + rad, col(b + rad)); vtx(r, t - rad, col(t - rad)); vtx(l, t - rad, col(t - rad));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t));
        vtx(l + rad, t - rad, col(t - rad)); vtx(r - rad, t, col(t)); vtx(l + rad, t, col(t));
        vtx(l + rad, b, col(b)); vtx(r - rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad));
        vtx(l + rad, b, col(b)); vtx(r - rad, b + rad, col(b + rad)); vtx(l + rad, b + rad, col(b + rad));
        const F32 cxs[4] = { r - rad, l + rad, l + rad, r - rad };
        const F32 cys[4] = { t - rad, t - rad, b + rad, b + rad };
        for (S32 k = 0; k < 4; ++k)
        {
            for (S32 i = 0; i < 6; ++i)
            {
                const F32 a0 = PI_F * 0.5f * ((F32)k + (F32)i / 6.f), a1 = PI_F * 0.5f * ((F32)k + (F32)(i + 1) / 6.f);
                const F32 x0 = cxs[k] + rad * cosf(a0), y0 = cys[k] + rad * sinf(a0);
                const F32 x1 = cxs[k] + rad * cosf(a1), y1 = cys[k] + rad * sinf(a1);
                vtx(cxs[k], cys[k], col(cys[k])); vtx(x0, y0, col(y0)); vtx(x1, y1, col(y1));
            }
        }
        gGL.end();
    }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::HCENTER, LLFontGL::VAlign v = LLFontGL::VCENTER)
    {
        if (OUTLINE)
        {
            // [W104 FEATFIX 2026-11-11] ours: a dark outline about a pixel round each letter (background off)
            const F32 o = 1.f / llmax(0.1f, WolfHudScale::current());
            const LLColor4 dark(0.01f, 0.03f, 0.06f, 0.85f * c.mV[VALPHA]);
            WolfHudScale::text(f, s, x - o, y, dark, h, v);
            WolfHudScale::text(f, s, x + o, y, dark, h, v);
            WolfHudScale::text(f, s, x, y - o, dark, h, v);
            WolfHudScale::text(f, s, x, y + o, dark, h, v);
        }
        WolfHudScale::text(f, s, x, y, c, h, v);   // sharp at the size the resident set (wolfhudscale.h)
    }

    // Source: wolfskihud.cpp face() - the B612 face (fonts.xml Avionics0..11) at the largest size whose line fits px.
    const LLFontGL* face(const char* name, U8 style, F32 px)
    {
        static std::map<std::string, std::vector<const LLFontGL*>> cache;
        const std::string key = std::string(name) + (style ? "B" : "R");
        auto it = cache.find(key);
        if (it == cache.end())
        {
            std::vector<const LLFontGL*> v;
            for (S32 i = 0; i < 12; ++i)
            {
                if (const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor(name, llformat("Avionics%d", i), style)))
                {
                    v.push_back(f);
                }
            }
            if (v.empty())
            {
                LL_WARNS("WolfGreedyHud") << "font " << name << " missing, using the default" << LL_ENDL;
                v.push_back(LLFontGL::getFontSansSerif());
            }
            it = cache.emplace(key, v).first;
        }
        const LLFontGL* best = it->second.front();
        for (const LLFontGL* f : it->second)
        {
            if ((F32)f->getLineHeight() <= px) best = f;
        }
        return best;
    }

    // THE LAYOUT, in the dashboards' frame (Source: wolfskihud.cpp "THE LAYOUT"): 1600 x 900 with the
    // panel from y 596 to 900 (y down), scaled by u; the left part keeps its distance from the left edge (L), the centre stays
    // centred (C), the buttons keep theirs from the right edge (R). LESS: only the top row, y 596 to 716.
    struct Anchor { F32 ox = 0.f, top0 = 900.f, u = 1.f; };
    struct Geo
    {
        F32 W = 1600.f;
        Anchor L, C, R, cur;
        F32 bandB = 0.f;
        F32 panelTop = 304.f;
    } g;
    const F32 C_L = 600.f, C_R = 1000.f, R_L = 1040.f;   // the centre and right parts' edges, mock units (left: 0..560)
    const F32 ROW_BOTTOM = 716.f;                        // LESS: the band ends here
    void use(const Anchor& a) { g.cur = a; }
    F32 X(F32 x) { return g.cur.ox + x * g.cur.u; }
    F32 YM(F32 y) { return g.cur.top0 - y * g.cur.u; }
    F32 S(F32 v) { return v * g.cur.u; }

    // [W104 FEATFIX 2026-11-11] (CHOSEN) every word a step bigger than the Ski HUD's sizes: Paul 11-11 of the Fishing HUD, "can
    // you make the text slightly bigger please, it's difficult to read"
    constexpr F32 TEXT_UP = 1.22f;

    const LLFontGL* font(F32 mock_px, bool bold = false, bool mono = false)
    {
        return face(mono ? "B612Mono" : "B612", bold ? LLFontGL::BOLD : LLFontGL::NORMAL, llmax(6.f, S(mock_px) * 1.25f * TEXT_UP));
    }

    void T(F32 x, F32 y, const std::string& s, F32 px, const LLColor4& c, bool bold = false, bool mono = false,
           LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        text(font(px, bold, mono), s, x, y, c, h, LLFontGL::VCENTER);
    }

    // Text that must fit `maxw` (picture units): smaller a size at a time down to `min_px`, then cut with "...".
    // Source: wolfskihud.cpp fitT (with the alignment as a parameter).
    void fitT(F32 x, F32 y, std::string str, F32 px, F32 min_px, F32 maxw, const LLColor4& c, bool bold = false,
              LLFontGL::HAlign h = LLFontGL::HCENTER)
    {
        F32 size = px;
        const LLFontGL* f = font(size, bold);
        while (f && f->getWidthF32(str) > maxw && size > min_px)
        {
            size -= 1.f;
            f = font(size, bold);
        }
        if (f && f->getWidthF32(str) > maxw)
        {
            while (str.size() > 4 && f->getWidthF32(str + "...") > maxw)
            {
                str.pop_back();
                while (!str.empty() && ((unsigned char)str.back() & 0xC0) == 0x80) str.pop_back();
                if (!str.empty() && (unsigned char)str.back() >= 0xC0) str.pop_back();
            }
            str += "...";
        }
        text(f, str, x, y, c, h, LLFontGL::VCENTER);
    }

    /** [W104 FEATFIX 2026-11-11] A message in lines no wider than `maxw` (at most `max_lines`, the last cut by fitT), centred
     *  from `top` down. Returns the height it took. */
    F32 msgT(F32 x, F32 top, const std::string& str, F32 px, F32 maxw, S32 max_lines, const LLColor4& c)
    {
        const LLFontGL* f = font(px);
        std::vector<std::string> lines;
        std::string cur;
        size_t at = 0;
        while (at < str.size())
        {
            size_t sp = str.find(' ', at);
            if (sp == std::string::npos) sp = str.size();
            const std::string w = str.substr(at, sp - at);
            at = sp + 1;
            if (w.empty()) continue;
            const std::string next = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && f->getWidthF32(next) > maxw) { lines.push_back(cur); cur = w; }
            else cur = next;
        }
        if (!cur.empty()) lines.push_back(cur);
        if ((S32)lines.size() > max_lines)
        {
            std::string last;
            for (size_t i = (size_t)max_lines - 1; i < lines.size(); ++i) last += (last.empty() ? "" : " ") + lines[i];
            lines.resize((size_t)max_lines);
            lines.back() = last;
        }
        const F32 lh = S(px * 1.45f * TEXT_UP);
        for (size_t i = 0; i < lines.size(); ++i) fitT(x, top - lh * ((F32)i + 0.5f), lines[i], px, px, maxw, c);
        return lh * (F32)lines.size();
    }

    struct Ctx
    {
        std::function<void(F32, F32, F32, F32, WolfGreedyHud::EHit, const std::string&)> hit;
        S32 pressed;
    };

    // Source: wolfskihud.cpp button() - one rounded button, its face lit from above, a pale rim, B612 bold; [W104 FEATFIX]
    // `enabled` false: drawn dim with grey words and no hit
    void button(Ctx& c, F32 l, F32 b, F32 r, F32 t, const std::string& label, WolfGreedyHud::EHit id, const std::string& tip,
                F32 px, const LLColor4& face_hi, const LLColor4& face_lo, bool enabled = true)
    {
        if (!enabled)
        {
            roundRectF(l, b, r, t, (t - b) * 0.25f, BTN_OFF_HI, BTN_OFF_LO);
            frameW(l, b, r, t, 1.f, alpha(SNOW, 0.18f));
            fitT((l + r) * 0.5f, (b + t) * 0.5f, label, px, 8.f, (r - l) - S(10.f), alpha(GREY, 0.75f), true);
            return;
        }
        const bool down = c.pressed == id;
        roundRectF(l, b, r, t, (t - b) * 0.25f, down ? face_lo : face_hi, down ? face_hi : face_lo);
        frameW(l, b, r, t, 1.2f, alpha(SNOW, 0.55f));
        fitT((l + r) * 0.5f, (b + t) * 0.5f, label, px, 8.f, (r - l) - S(10.f), SNOW, true);
        c.hit(l, b, r, t, id, tip);
    }

    void image(const char* name, F32 l, F32 b, F32 w, F32 h, const LLColor4& c = LLColor4::white)
    {
        if (LLUIImagePtr img = LLUI::getUIImage(name))
        {
            img->draw((S32)llround(l), (S32)llround(b), (S32)llround(w), (S32)llround(h), c);
        }
    }

    /** The state in a few capital words, for the centre's top line. */
    std::string stateWords(const WolfGreedyGame& s, LLColor4& col)
    {
        col = ICE;
        const LLSD& g = s.state()["game"];
        if (!s.game() || !s.state().has("game")) { col = SNOW; return "NO GAME YET - START ONE OR JOIN ONE"; }
        if (!g["in_game"].asBoolean()) { col = SNOW; return "WATCHING - YOU ARE NOT IN THIS GAME"; }
        const std::string st = g["state"].asString();
        if (st == "finished") { col = GOLD; return "GAME OVER"; }
        if (st == "lobby") return "WAITING TO START - PRESS START WHEN EVERYONE IS IN";
        if (g["my_turn"].asBoolean()) { col = GOLD; return g["rolled"].asBoolean() ? "YOUR TURN: ROLL AGAIN OR BANK" : "YOUR TURN: ROLL"; }
        return "WAITING FOR THE OTHER PLAYERS";
    }
}

WolfGreedyHud::WolfGreedyHud(const Params& p) : LLUICtrl(p) {}
WolfGreedyHud::~WolfGreedyHud() {}

void WolfGreedyHud::addHit(F32 l, F32 b, F32 r, F32 t, EHit id, const std::string& tip)
{
    mHits.push_back({ l, b, r, t, id, tip });
}

const WolfGreedyHud::Hit* WolfGreedyHud::hitAt(S32 x, S32 y) const
{
    const F32 px = (F32)x / mHitScale, py = (F32)y / mHitScale;
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (px >= it->mL && px <= it->mR && py >= it->mB && py <= it->mT) return &*it;
    }
    return nullptr;
}

bool WolfGreedyHud::onPanel(S32 x, S32 y) const
{
    if (!mDrawn) return false;
    if (hitAt(x, y)) return true;
    // Source: wolfskihud.cpp onPanel - with the background on the whole band is solid; off, only the controls
    if (!gSavedPerAccountSettings.getBOOL("WolfGreedyHudBackground")) return false;
    return (F32)y >= mPanelB && (F32)y <= mPanelT;
}

void WolfGreedyHud::press(EHit id)
{
    WolfGreedyGame& s = WolfGreedyGame::instance();
    switch (id)
    {
    case H_ROLL:     s.landNow(); s.roll(); break;
    case H_BANK:     s.landNow(); s.bank(); break;
    case H_NEW:      s.newGame(false, 10000); break;   // the classic game; the window offers 5,000 and 2,000 too
    case H_PRACTICE: s.newGame(true, 2000); break;
    case H_JOIN:     WolfFloaterGreedy::showLobbyWindow(); break;
    case H_LEAVE:    s.leave(); break;
    case H_PASS:     s.pass(); break;
    case H_HELP:     WolfFloaterGreedy::showGuideWindow(); break;
    case H_WINDOW:   WolfFloaterGreedy::showLobbyWindow(); break;
    case H_PANEL:    gSavedPerAccountSettings.setBOOL("WolfGreedyHudBackground", !gSavedPerAccountSettings.getBOOL("WolfGreedyHudBackground")); break;
    case H_LESS:     gSavedPerAccountSettings.setBOOL("WolfGreedyHudLess", !gSavedPerAccountSettings.getBOOL("WolfGreedyHudLess")); break;
    case H_CLOSE:    WolfGreedyGame::closeHud(); break;
    case H_DICE:     s.landNow(); break;   // a click on the dice lands them at once
    default: break;
    }
}

void WolfGreedyHud::draw()
{
    mHits.clear();
    mDrawn = false;
    if (LLApp::isExiting()) return;   // [review 2] nothing of the vehicles or the game is asked for while the viewer quits
    if (WolfGreedyGame::instanceExists()) WolfGreedyGame::instance().pendingTick();   // [review 2 W4] a waiting HUD appears
    if (!WolfGreedyGame::instanceExists() || !WolfGreedyGame::instance().hudOn() || !WolfGrid::isOnWolfTerritories())
    {
        mOtherWasUp = WolfGreedyGame::instanceExists() && !WolfGreedyGame::vehiclePanelUp().empty();
        if (hasMouseCapture()) gFocusMgr.setMouseCapture(nullptr);
        mPressed = H_NONE;
        mResize.hide();
        return;
    }
    WolfGreedyGame& s = WolfGreedyGame::instance();
    // [W104 FEATFIX review 2026-11-11] one bottom panel at a time (wolffloatergreedy.h vehiclePanelUp): a vehicle's panel
    // switched on after this one sends Greedy back to waiting - the game goes on, and Greedy comes back by itself when that
    // panel goes (playing while wandering). [W104 MERGE 2026-11-11] The game HUD is no longer handled here: it steps aside
    // by itself while Greedy shows (wolfgame.cpp bottomBusy), and shown by hand it closes Greedy (WolfGreedyGame::gameHudClaims).
    // Fishing never comes up while Greedy shows (it waits for Greedy, wolffishingoverlay.cpp vehiclePanelUp).
    const bool other_up = !WolfGreedyGame::vehiclePanelUp().empty();
    const bool newly = other_up && !mOtherWasUp;
    mOtherWasUp = other_up;
    if (newly)
    {
        WolfGreedyGame::closeHud();
        mResize.hide();
        WolfGreedyGame::openHud();   // (waits: vehiclePanelUp is not empty, and says so)
        return;
    }
    s.tick(false);
    const bool less = gSavedPerAccountSettings.getBOOL("WolfGreedyHudLess");

    // the band, as wolfskihud.cpp draw(): above the bottom toolbar, its height a share of the
    // window times UI Size
    const F32 screen_w = (F32)getRect().getWidth();
    const F32 Hv = (F32)getRect().getHeight();
    F32 screen_bottom = 0.f;
    if (gToolBarView)
    {
        if (LLView* bp = gToolBarView->findChildView("bottom_toolbar_panel", true))
        {
            if (bp->isInVisibleChain())
            {
                const LLRect sr = bp->calcScreenRect();
                S32 lx = 0, ly = 0;
                screenPointToLocal(sr.mLeft, sr.mTop, &lx, &ly);
                screen_bottom = llclamp((F32)ly, 0.f, Hv * 0.4f);
            }
        }
    }
    const F32 zoom = mResize.scale();
    const F32 W = screen_w / zoom;
    const F32 bottom = screen_bottom / zoom;
    const F32 ui = llmax(0.1f, LLUI::getScaleFactor().mV[VY]);
    // Source: wolfskihud.cpp - 26 % of the window, 120..330 px, never more than 30 % of what is above the toolbar
    const F32 want_h = llclamp(Hv * ui * 0.26f, 120.f, 330.f);
    const F32 max_h = llmax(60.f, (Hv - screen_bottom) * 0.30f);
    const F32 uH = llmin(want_h, max_h) / 304.f;
    const F32 one = llmin(uH, W / 1600.f);
    const F32 rows_bottom = less ? ROW_BOTTOM : 900.f;   // LESS: the band ends under the top row
    g.W = W;
    g.bandB = bottom;
    g.L.u = g.C.u = g.R.u = one;
    g.L.top0 = g.C.top0 = g.R.top0 = bottom + rows_bottom * one;
    g.L.ox = 0.f;
    g.C.ox = W * 0.5f - 800.f * one;
    g.R.ox = W - 1600.f * one;
    g.panelTop = g.C.top0 - 596.f * g.C.u;
    mPanelB = screen_bottom;
    mPanelT = g.panelTop * zoom;
    mDrawn = true;

    LLGLSUIDefault gls_ui;
    std::optional<WolfHudScale::Scope> zoomed;
    zoomed.emplace(zoom);
    Ctx c{ [this](F32 l, F32 b, F32 r, F32 tp, EHit id, const std::string& tip) { addHit(l, b, r, tp, id, tip); }, mPressed };
    const bool bg = gSavedPerAccountSettings.getBOOL("WolfGreedyHudBackground");
    INSET = bg ? INSET_ON : INSET_OFF;
    OUTLINE = !bg;
    const LLSD& state = s.state();
    const LLSD& gm = state["game"];
    const bool inGame = s.inGame();
    const std::string buttons = gm["buttons"].asString();
    const std::string st = gm["state"].asString();
    const bool going = st == "playing" || st == "final";

    use(g.L);
    if (bg)
    {
        rectG(0.f, g.bandB, W, g.panelTop, NAVY_HI, NAVY_LO);
        rectF(0.f, g.panelTop - S(4.f), W, g.panelTop, alpha(ICE, 0.35f));
        rectF(0.f, g.panelTop - S(2.f), W, g.panelTop - S(1.f), alpha(SNOW, 0.8f));
    }

    // ── LEFT: the logo, and the scoreboard ──
    use(g.L);
    {
        image("WolfGreedy_Logo", X(20.f), YM(704.f), S(88.f), S(88.f));
        image("WolfGreedy_Wordmark", X(116.f), YM(668.f), S(220.f), S(55.f));
        if (less)
        {
            // one row: your score and this turn's, under the name
            S32 mine = 0;
            for (const LLSD& p : llsd::inArray(state["players"])) if (p["me"].asBoolean()) mine = p["score"].asInteger();
            fitT(X(120.f), YM(690.f), s.game() ? "YOUR SCORE " + number(mine) + (going ? "   THIS TURN " + number(gm["turn_points"].asInteger()) : std::string())
                                               : std::string("NO GAME YET"), 12.f, 9.f, S(430.f), SNOW, true, LLFontGL::LEFT);
        }
        else
        {
            if (bg) roundRectF(X(20.f), YM(892.f), X(540.f), YM(716.f), S(10.f), alpha(GOLD, 0.12f), INSET);
            T(X(40.f), YM(734.f), "PLAYERS", 11.f, GOLD, true, false, LLFontGL::LEFT);
            T(X(524.f), YM(734.f), "SCORE", 11.f, GOLD, true, false, LLFontGL::RIGHT);
            if (!state.has("players") || state["players"].size() == 0)
            {
                msgT(X(280.f), YM(752.f), s.game() ? "Waiting for players..." : "No game yet. Press NEW GAME or PRACTICE - or JOIN a game being played now.",
                     12.f, S(480.f), 3, SNOW);
            }
            else
            {
                // as the window's scoreboard: whose turn (an arrow, in gold), you (green), the winner (a star), busts as stars
                S32 row = 0;
                const S32 n = (S32)state["players"].size();
                for (const LLSD& p : llsd::inArray(state["players"]))
                {
                    if (row == 4 && n > 5) { T(X(40.f), YM(764.f + 32.f * 4.f), llformat("+ %d more (the window shows them all)", n - 4), 10.f, GREY, false, false, LLFontGL::LEFT); break; }
                    const bool cur = p["current"].asBoolean(), me = p["me"].asBoolean(), win = p["winner"].asBoolean(), away = p["away"].asBoolean();
                    const LLColor4 col = away ? GREY : cur ? GOLD : (me || win) ? GREEN : SNOW;
                    const F32 y = YM(764.f + 32.f * (F32)row);
                    T(X(30.f), y, win ? "\xE2\x98\x85" : cur ? "\xE2\x96\xB6" : "", 12.f, col, true);   // U+2605 star, U+25B6 arrow
                    std::string busts;
                    for (S32 i = 0; i < p["busts"].asInteger() && i < 3; ++i) busts += "*";
                    fitT(X(48.f), y, p["name"].asString() + (me ? " (you)" : "") + (away ? " (away)" : "") + (busts.empty() ? "" : "  " + busts),
                         13.f, 9.f, S(330.f), col, cur, LLFontGL::LEFT);
                    T(X(524.f), y, number(p["score"].asInteger()), 14.f, col, true, true, LLFontGL::RIGHT);
                    ++row;
                }
            }
        }
    }

    // ── CENTRE: the state, the dice, this turn, the moment, the message area ──
    use(g.C);
    {
        LLColor4 sc;
        const std::string sw = stateWords(s, sc);
        roundRectF(X(C_L), YM(642.f), X(C_R), YM(606.f), S(8.f), INSET, INSET);
        fitT(X((C_L + C_R) * 0.5f), YM(624.f), sw, 13.f, 9.f, S(C_R - C_L - 20.f), sc, true);
        // the six 3D dice (greedy-art/render_dice.py): gold scored, red a bust; tumbling while a roll lands
        const LLSD& dice = gm["dice"];
        const std::string kept = gm["kept"].asString();
        const F32 dw = 58.f;
        addHit(X(C_L), YM(712.f), X(C_R), YM(650.f), H_DICE, "Click to land the dice at once");
        for (S32 i = 0; i < 6; ++i)
        {
            const S32 face = dice.isArray() && i < (S32)dice.size() ? llclamp(dice[i].asInteger(), 0, 6) : 0;
            const char k = i < (S32)kept.size() ? kept[i] : '0';
            const F32 l = X(C_L + 8.f + 65.f * (F32)i), b = YM(652.f + dw);
            if (s.rolling() && face > 0)
            {
                image(llformat("WolfGreedy_Roll%d", s.rollFrame(i)).c_str(), l, b, S(dw), S(dw));
            }
            else if (face > 0)
            {
                image(llformat("WolfGreedy_Die%d", face).c_str(), l, b, S(dw), S(dw), k == '1' ? GOLD_DIE : k == 'x' ? RED_DIE : LLColor4::white);
            }
            else
            {
                roundRectF(l + S(4.f), b + S(4.f), l + S(dw - 4.f), b + S(dw - 4.f), S(8.f), alpha(SNOW, 0.10f), alpha(SNOW, 0.05f));
                frameW(l + S(4.f), b + S(4.f), l + S(dw - 4.f), b + S(dw - 4.f), 1.f, alpha(SNOW, 0.25f));
            }
        }
        // the moment over the dice: "Bust!", "You banked 350!" - a brief flash, then steady, gone after a moment
        if (!s.banner().empty() && !s.rolling())
        {
            LLColor4 col = s.bannerColor();
            const F32 age = s.bannerAge();
            if (!reduce_motion() && age < BANNER_PULSE_SECS)
            {
                const F32 pulse = 0.5f + 0.5f * cosf(age / BANNER_PULSE_SECS * 3.f * 2.f * PI_F);
                col = lerp(LLColor4::white, s.bannerColor(), 1.f - pulse);
            }
            roundRectF(X(C_L + 20.f), YM(706.f), X(C_R - 20.f), YM(656.f), S(12.f), alpha(NAVY_LO, 0.88f), alpha(NAVY_LO, 0.88f));
            frameW(X(C_L + 20.f), YM(706.f), X(C_R - 20.f), YM(656.f), 1.5f, alpha(col, 0.8f));
            fitT(X((C_L + C_R) * 0.5f), YM(681.f), s.banner(), 26.f, 14.f, S(C_R - C_L - 60.f), col, true);
        }
        if (!less)
        {
            // this turn's points, big and gold
            T(X(C_L + 10.f), YM(728.f), going ? "THIS TURN" : (s.game() ? "FIRST TO" : ""), 11.f, GOLD, true, false, LLFontGL::LEFT);
            if (s.game())
            {
                T(X(C_L + 10.f), YM(758.f), number(going ? gm["turn_points"].asInteger() : gm["target"].asInteger()), 24.f, GOLD, true, true, LLFontGL::LEFT);
                if (going) T(X(C_R - 10.f), YM(758.f), llformat("DICE TO ROLL: %d", gm["dice_left"].asInteger()), 12.f, SNOW, true, false, LLFontGL::RIGHT);
            }
            // the message area: the newest first (what the server said, or a problem in amber with a bar beside it), then
            // the game's last event (as the window's event line)
            roundRectF(X(C_L), YM(892.f), X(C_R), YM(780.f), S(10.f), INSET, INSET);
            F32 top = YM(786.f);
            std::vector<std::pair<std::string, LLColor4>> lines;
            const std::string ev = gm["event"].asString();
            for (const WolfGreedyGame::Msg& m : s.messages())
            {
                if (lines.size() >= 2) break;
                lines.emplace_back(m.mText, m.mError ? AMBER : SNOW);
            }
            if (!ev.empty()) lines.insert(lines.begin(), { ev, gm["event_kind"].asString() == "bust" ? RED_DIE : ICE });
            if (state.has("tips") && state["tips"].size() > 0) lines.emplace_back(state["tips"][0].asString(), ICE_DIM);
            for (size_t i = 0; i < lines.size() && top > YM(880.f); ++i)
            {
                const F32 used = msgT(X((C_L + C_R) * 0.5f), top, lines[i].first, i == 0 ? 12.f : 11.f, S(C_R - C_L - 40.f), 2,
                                      alpha(lines[i].second, i == 0 ? 1.f : 0.8f));
                if (lines[i].second == AMBER) rectF(X(C_L + 8.f), top - used + S(2.f), X(C_L + 13.f), top - S(2.f), AMBER);
                top -= used + S(4.f);
            }
        }
    }

    // ── RIGHT: the buttons, big and clear ──
    use(g.R);
    {
        const bool wolf = WolfGrid::isOnWolfTerritories();
        const bool changes_ok = !s.changeInFlight();   // [W104 FEATFIX review 2026-11-11] change buttons off while one is out
        const bool canRoll = changes_ok && inGame && (buttons.find('R') != std::string::npos || buttons.find('S') != std::string::npos || buttons.find('A') != std::string::npos);
        const std::string rollLabel = buttons.find('S') != std::string::npos ? "START" : buttons.find('A') != std::string::npos ? "PLAY AGAIN" : "ROLL";
        const F32 rr = less ? R_L + 200.f : R_L + 268.f;   // LESS: room on the row for MORE and CLOSE
        const F32 bl = less ? R_L + 206.f : R_L + 280.f, br = less ? R_L + 406.f : 1588.f;
        button(c, X(R_L), YM(700.f), X(rr), YM(612.f), rollLabel, H_ROLL,
               "Roll the dice that are left (the Greedy server rolls them, so nobody can cheat)", 18.f, BTN_GO_HI, BTN_GO_LO, wolf && canRoll);
        button(c, X(bl), YM(700.f), X(br), YM(612.f), "BANK", H_BANK, "Keep this turn's points; then it is the next player's turn",
               18.f, BTN_GOLD_HI, BTN_GOLD_LO, wolf && changes_ok && inGame && buttons.find('B') != std::string::npos);
        if (less)
        {
            button(c, X(R_L + 412.f), YM(700.f), X(R_L + 478.f), YM(612.f), "MORE", H_LESS, "Show the whole Greedy HUD again", 10.f, BTN_HI, BTN_LO);
            button(c, X(R_L + 484.f), YM(700.f), X(1588.f), YM(612.f), "CLOSE", H_CLOSE,
                   "Close the Greedy HUD (Wolf World > Greedy brings it back; your game goes on)", 10.f, BTN_HI, BTN_LO);
        }
        else
        {
            if (inGame)
            {
                button(c, X(R_L), YM(780.f), X(R_L + 176.f), YM(716.f), "LEAVE", H_LEAVE, "Leave this game", 12.f, BTN_HI, BTN_LO, changes_ok);
                button(c, X(R_L + 182.f), YM(780.f), X(R_L + 358.f), YM(716.f), "PASS THEIR TURN", H_PASS,
                       "When the player whose turn it is has been away for a few minutes", 11.f, BTN_HI, BTN_LO, changes_ok && going && !gm["my_turn"].asBoolean());
                button(c, X(R_L + 364.f), YM(780.f), X(1588.f), YM(716.f), "WINDOW", H_WINDOW,
                       "The Greedy window: the whole game, every player, and games to join", 12.f, BTN_HI, BTN_LO);
            }
            else
            {
                button(c, X(R_L), YM(780.f), X(R_L + 176.f), YM(716.f), "NEW GAME", H_NEW,
                       "Start a game to 10,000 that others can join from their Greedy HUD or window, or a Greedy table", 12.f, BTN_MAIN_HI, BTN_MAIN_LO, wolf && changes_ok);
                button(c, X(R_L + 182.f), YM(780.f), X(R_L + 358.f), YM(716.f), "PRACTICE", H_PRACTICE,
                       "A game on your own with tips. Nothing is saved and it is not ranked.", 12.f, BTN_HI, BTN_LO, wolf && changes_ok);
                button(c, X(R_L + 364.f), YM(780.f), X(1588.f), YM(716.f), "JOIN A GAME", H_JOIN,
                       "The games being played now (the Greedy window)", 12.f, BTN_HI, BTN_LO, wolf);
            }
            button(c, X(R_L), YM(860.f), X(R_L + 150.f), YM(796.f), "HOW TO PLAY", H_HELP, "The rules, on nine short cards", 11.f, BTN_HI, BTN_LO);
            // the word as well as the light: ON / OFF (never colour alone)
            button(c, X(R_L + 156.f), YM(860.f), X(R_L + 320.f), YM(796.f), bg ? "BACKGROUND: ON" : "BACKGROUND: OFF", H_PANEL,
                   bg ? "The night-blue background is on. Click to take it away and see more of the world: just the buttons and words stay."
                      : "The background is off. Click to put the night-blue background back behind the HUD.", 10.f,
                   bg ? BTN_MAIN_HI : BTN_HI, BTN_LO);
            button(c, X(R_L + 326.f), YM(860.f), X(R_L + 440.f), YM(796.f), "LESS", H_LESS,
                   "Just one row: the dice, ROLL and BANK (MORE brings the rest back)", 11.f, BTN_HI, BTN_LO);
            button(c, X(R_L + 446.f), YM(860.f), X(1588.f), YM(796.f), "CLOSE", H_CLOSE,
                   "Close the Greedy HUD (Wolf World > Greedy brings it back; your game goes on)", 11.f, BTN_HI, BTN_LO);
        }
    }

    zoomed.reset();
    mHitScale = zoom;
    mResize.place(screen_bottom, mPanelT, screen_w);
    mResize.draw();
}

bool WolfGreedyHud::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.begin(y);
        gFocusMgr.setMouseCapture(this);
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    const Hit* hit = hitAt(x, y);
    if (!hit)
    {
        return true;   // the panel itself: not a click on the world behind it
    }
    mPressed = hit->mId;
    gFocusMgr.setMouseCapture(this);
    press(hit->mId);
    return true;
}

bool WolfGreedyHud::handleMouseUp(S32 x, S32 y, MASK mask)
{
    const bool was = mPressed != H_NONE || mResize.dragging() || mResize.onGrip(x, y);
    mResize.end();
    if (hasMouseCapture())
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    mPressed = H_NONE;
    return was || onPanel(x, y);
}

bool WolfGreedyHud::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture() && mResize.dragging())
    {
        mResize.drag(y);
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && mDrawn && mResize.onGrip(x, y))
    {
        mResize.hovered();
        getWindow()->setCursor(UI_CURSOR_SIZENS);
        return true;
    }
    if (!hasMouseCapture() && !onPanel(x, y))
    {
        return false;
    }
    getWindow()->setCursor(hitAt(x, y) ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfGreedyHud::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return (mDrawn && mResize.onGrip(x, y)) || onPanel(x, y);   // no world context menu through the panel
}

bool WolfGreedyHud::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        mResize.reset();
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    // [W104 FEATFIX 2026-11-11] the second click of a double-click is ignored here on every button:
    // each is a switch or must not go twice (ROLL, BANK, a new game), and many residents double-click buttons
    return true;
}

bool WolfGreedyHud::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (mDrawn && mResize.onGrip(x, y))
    {
        LLToolTipMgr::instance().show(WolfHudResize::tip());
        return true;
    }
    if (!onPanel(x, y))
    {
        return false;
    }
    if (const Hit* hit = hitAt(x, y))
    {
        if (!hit->mTip.empty()) LLToolTipMgr::instance().show(hit->mTip);
    }
    return true;
}

void WolfGreedyHud::onMouseCaptureLost()
{
    mResize.end();
    mPressed = H_NONE;
    LLUICtrl::onMouseCaptureLost();
}
