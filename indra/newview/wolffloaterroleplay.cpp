/**
 * @file wolffloaterroleplay.cpp
 * @brief WolfViewer: Wolf Roleplay windows. See wolffloaterroleplay.h.
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

#include "wolffloaterroleplay.h"

#include <algorithm>
#include <cmath>
#include <deque>

#include "llagent.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llfilepicker.h"
#include "llfloaterreg.h"
#include "llfloaterworldmap.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventory.h"
#include "lllocalbitmaps.h"
#include "lllocalcliprect.h"
#include "llmediactrl.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llscriptfloater.h"
#include "lltextbox.h"
#include "lltoolbarview.h"
#include "lltooldraganddrop.h"
#include "lltooltip.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llwindow.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfGameConsole> r_wolf_game_console("wolf_game_console");
static LLDefaultChildRegistry::Register<WolfGameDialogView> r_wolf_game_dialog_view("wolf_game_dialog_view");

using namespace WolfGameDraw;

namespace
{
    // Source: WolfGameTypes.cs WolfGameLimits.MAX_BUTTONS = 12 (the sim refuses more).
    const S32 MAX_DIALOG_BUTTONS = 12;
    const size_t MAX_DIALOGS = 8;   // [SECURITY 2026-10-09] at most 8 open, the oldest goes
    const F64 STATUS_LIFE = 5.0;    // a confirmation stays this long; an error until the next action

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    LLColor4 fade(const LLColor4& c, F32 a)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a);
    }

    /** "3 days" from seconds (§3.3 Pets "the age ... from age in seconds"). */
    std::string ageText(S64 s)
    {
        auto plural = [](S64 n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
        if (s < 60) return plural(s, "second");
        if (s < 3600) return plural(s / 60, "minute");
        if (s < 86400) return plural(s / 3600, "hour");
        return plural(s / 86400, "day");
    }

    bool webUrl(const std::string& u)
    {
        std::string l = u.substr(0, 8);
        LLStringUtil::toLower(l);
        return l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0;
    }

    /** The right / top pads script dialogs use (llscriptfloater.cpp LLScriptFloater::show). */
    S32 rightPad()
    {
        if (!gToolBarView) return 0;
        LLToolBar* rt = gToolBarView->getToolbar(LLToolBarEnums::TOOLBAR_RIGHT);
        return rt && rt->hasButtons() ? rt->getRect().getWidth() : 0;
    }

    // In WolfGameConsole::EPage order.
    const char* const PAGE_NAMES[] = { "Stats", "Combat", "Bag", "Pets", "Game", "Look", "Help" };
    const char* const PAGE_TIPS[] = {
        "Your stats as gauges, the game's own information, and markers on other regions",
        "Your weapons, armour, abilities and the effects on you",
        "Your bag: everything you hold in this game, and what the game has that you have not",
        "Your pets and how they are doing",
        "About this region's game: its description, rules and website",
        "Choose the picture behind this window",
        "How to play, and the credits",
    };
    static_assert(sizeof(PAGE_NAMES) / sizeof(PAGE_NAMES[0]) == WolfGameConsole::PAGE_COUNT, "a name per page");
    static_assert(sizeof(PAGE_TIPS) / sizeof(PAGE_TIPS[0]) == WolfGameConsole::PAGE_COUNT, "a tip per page");
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the background picture
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace WolfGameBackground
{
    namespace
    {
        // The tints over a picture. Names, not only colours: the Look page shows each word.
        struct Tint { const char* mName; F32 r, g, b; };
        const Tint TINTS[] = {
            { "Natural",   1.00f, 1.00f, 1.00f },
            { "Amber",     1.00f, 0.78f, 0.45f },
            { "Moonlight", 0.70f, 0.82f, 1.00f },
            { "Blood",     1.00f, 0.45f, 0.42f },
            { "Forest",    0.55f, 0.95f, 0.60f },
            { "Dusk",      0.82f, 0.62f, 1.00f },
        };
        const char* const TINT_KEYS[] = { "natural", "amber", "moonlight", "blood", "forest", "dusk" };
        const S32 TINT_COUNT = (S32)(sizeof(TINTS) / sizeof(TINTS[0]));

        LLPointer<LLViewerFetchedTexture> sTexture;   // the inventory texture, fetched
        std::string sTextureId;
        LLUUID sTracking;                             // the local bitmap we added, if any
        std::string sTrackingFile;
        bool sFileTried = false;                      // one load attempt per file per session

        void dropLocal()
        {
            if (sTracking.notNull())
            {
                LLLocalBitmapMgr::getInstance()->delUnit(sTracking);
                sTracking.setNull();
            }
            sTrackingFile.clear();
        }
    }

    ESource source()
    {
        const std::string s = gSavedPerAccountSettings.getString("WolfGameBgSource");
        if (s == "texture") return SRC_TEXTURE;
        if (s == "file") return SRC_FILE;
        return SRC_THEME;
    }

    LLViewerTexture* texture()
    {
        switch (source())
        {
        case SRC_TEXTURE:
        {
            const std::string id = gSavedPerAccountSettings.getString("WolfGameBgTexture");
            if (!LLUUID::validate(id) || LLUUID(id).isNull()) return nullptr;
            if (id != sTextureId || sTexture.isNull())
            {
                sTextureId = id;
                // Source: wolfmapoverlays.cpp - a texture by asset id, fetched for UI drawing.
                sTexture = LLViewerTextureManager::getFetchedTexture(LLUUID(id), FTT_DEFAULT, MIPMAP_TRUE,
                                                                     LLGLTexture::BOOST_UI, LLViewerTexture::LOD_TEXTURE);
            }
            return sTexture.get();
        }
        case SRC_FILE:
        {
            const std::string file = gSavedPerAccountSettings.getString("WolfGameBgFile");
            if (file.empty()) return nullptr;
            if (sTracking.isNull() || sTrackingFile != file)
            {
                if (sFileTried && sTrackingFile == file) return nullptr;   // it failed once: not every frame
                dropLocal();
                sFileTried = true;
                sTrackingFile = file;
                // Source: lltexturectrl.cpp LLFloaterTexturePicker::onPickerCallback - a picture file
                // becomes a local texture (addUnit shows its own notice when it cannot be read).
                sTracking = LLLocalBitmapMgr::getInstance()->addUnit(file);
                if (sTracking.isNull()) return nullptr;
            }
            // The world id changes when the file is edited (LLLocalBitmap::updateSelf), so it is
            // looked up each time; gTextureList holds the texture under it.
            const LLUUID world = LLLocalBitmapMgr::getInstance()->getWorldID(sTracking);
            if (world.isNull()) return nullptr;
            return gTextureList.findImage(world, TEX_LIST_STANDARD);
        }
        default:
            return nullptr;
        }
    }

    std::string describe()
    {
        switch (source())
        {
        case SRC_TEXTURE: return "A texture from your inventory";
        case SRC_FILE:
        {
            const std::string file = gSavedPerAccountSettings.getString("WolfGameBgFile");
            return "Picture file: " + gDirUtilp->getBaseFileName(file);
        }
        default:
            return WolfGame::instance().wolvesTheme() ? "The Wolves theme's picture" : "The theme's own picture";
        }
    }

    void useTexture(const LLUUID& asset_id)
    {
        dropLocal();
        gSavedPerAccountSettings.setString("WolfGameBgTexture", asset_id.asString());
        gSavedPerAccountSettings.setString("WolfGameBgSource", "texture");
    }

    bool useFile(const std::string& path, std::string& why)
    {
        dropLocal();
        sFileTried = true;
        sTrackingFile = path;
        sTracking = LLLocalBitmapMgr::getInstance()->addUnit(path);
        if (sTracking.isNull())
        {
            why = "That picture could not be read. Use a PNG, JPEG, TGA or BMP file.";
            return false;
        }
        gSavedPerAccountSettings.setString("WolfGameBgFile", path);
        gSavedPerAccountSettings.setString("WolfGameBgSource", "file");
        return true;
    }

    void reset()
    {
        dropLocal();
        sFileTried = false;
        gSavedPerAccountSettings.setString("WolfGameBgSource", "theme");
    }

    void release()
    {
        sTexture = nullptr;
        sTextureId.clear();
        dropLocal();
    }

    F32 opacity() { return llclamp(gSavedPerAccountSettings.getF32("WolfGameBgOpacity"), 0.f, 1.f); }
    void setOpacity(F32 v) { gSavedPerAccountSettings.setF32("WolfGameBgOpacity", llclamp(v, 0.f, 1.f)); }

    S32 tint()
    {
        const std::string t = gSavedPerAccountSettings.getString("WolfGameBgTint");
        for (S32 i = 0; i < TINT_COUNT; ++i)
        {
            if (t == TINT_KEYS[i]) return i;
        }
        return 0;
    }

    void setTint(S32 index)
    {
        gSavedPerAccountSettings.setString("WolfGameBgTint", TINT_KEYS[llclamp(index, 0, TINT_COUNT - 1)]);
    }

    S32 tintCount() { return TINT_COUNT; }
    const char* tintName(S32 index) { return TINTS[llclamp(index, 0, TINT_COUNT - 1)].mName; }

    LLColor4 tintColour(S32 index)
    {
        const Tint& t = TINTS[llclamp(index, 0, TINT_COUNT - 1)];
        return LLColor4(t.r, t.g, t.b, 1.f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfGameHits
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfGameHits::add(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable, bool enabled)
{
    Hit h;
    h.mL = l; h.mB = b; h.mR = r; h.mT = t;
    h.mId = id; h.mArg = arg; h.mTip = tip;
    h.mFocusable = focusable; h.mEnabled = enabled;
    mHits.push_back(h);
}

const WolfGameHits::Hit* WolfGameHits::at(S32 x, S32 y) const
{
    // The last added is on top (a confirmation over the page).
    for (auto it = mHits.rbegin(); it != mHits.rend(); ++it)
    {
        if (x >= it->mL && x <= it->mR && y >= it->mB && y <= it->mT) return &*it;
    }
    return nullptr;
}

const WolfGameHits::Hit* WolfGameHits::focused() const
{
    for (const Hit& h : mHits)
    {
        if (h.mId == mFocusId && h.mArg == mFocusArg) return &h;
    }
    return nullptr;
}

bool WolfGameHits::moveFocus(bool forward)
{
    std::vector<const Hit*> order;
    for (const Hit& h : mHits)
    {
        if (h.mFocusable && h.mEnabled) order.push_back(&h);
    }
    if (order.empty()) return false;
    S32 at = -1;
    for (S32 i = 0; i < (S32)order.size(); ++i)
    {
        if (order[i]->mId == mFocusId && order[i]->mArg == mFocusArg) { at = i; break; }
    }
    const S32 n = (S32)order.size();
    const S32 next = at < 0 ? (forward ? 0 : n - 1) : (at + (forward ? 1 : n - 1)) % n;
    mFocusId = order[next]->mId;
    mFocusArg = order[next]->mArg;
    return true;
}

void WolfGameHits::keepFocusValid()
{
    if (mFocusId == 0) return;
    const Hit* f = focused();
    if (f && f->mFocusable && f->mEnabled) return;
    mFocusId = mFocusArg = 0;
    moveFocus(true);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfGameConsole - the Roleplay window's face
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    const LLColor4 DANGER_TEXT(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5 (7.5:1 on the cards)
    const LLColor4 OK_TEXT(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80 (7.3:1)
    const LLColor4 WARN_FILL(0.961f, 0.620f, 0.043f, 1.f);     // #f59e0b

    /** The page's photograph for a pet type with one bundled (Paul: photographs of wolves; a raven
     *  for the Raven) - kit/out/kit_manifest.json pet types "wolf_pup" and "raven". */
    const char* petPhoto(const std::string& type)
    {
        if (type == "wolf_pup") return "WolfGame_Pup";
        if (type == "raven") return "WolfGame_Raven";
        return nullptr;
    }

    std::string initialsOf(const std::string& label)
    {
        LLWString w = utf8str_to_wstring(label);
        LLWString out;
        bool start = true;
        for (llwchar c : w)
        {
            if (c == ' ') { start = true; continue; }
            if (start) out.push_back(c);
            start = false;
            if (out.size() >= 2) break;
        }
        return wstring_to_utf8str(out);
    }

    /** The help and instructions (Paul: "include help and instructions"). Each is a heading and a
     *  paragraph; '\n' starts a new line. Words only - every name in it is the viewer's own. */
    struct HelpSection { const char* mTitle; const char* mBody; };
    const HelpSection HELP[] = {
        { "What Game Mode is",
          "Game Mode lets you play the roleplay game of the region you are on - the grid's own Wolf Territories game, or a "
          "region's own game. While it is on, your stats, Bones, bag, pets and weapons show on screen and the game's objects "
          "count you in. It is OFF until you turn it on.\nTurn it on or off with the big switch at the top of this window, "
          "the Game Mode button on your toolbar, or World > Game Mode. The region answers within a few seconds; wait about "
          "5 seconds before switching again." },
        { "When a region locks Game Mode",
          "Some regions require Game Mode, so nobody can switch it off to escape a fight. There the switch shows LOCKED ON "
          "with a padlock and the words \"This region requires Game Mode\", the menu item is greyed and \"Leave this game\" "
          "is hidden. Your own setting is not changed: on the next region it applies again." },
        { "Your stats",
          "The frame at the top left of your screen is you: the photograph, your name, the game, your wallet, and a bar for "
          "every stat with its number (Health, Stamina, Hunger and any the game adds). The rings round the portrait are the "
          "first three stats; the badge under it is your level. Click the frame to open this window; the arrow folds it." },
        { "Bones - your wallet",
          "Bones are the Wolf Territories game's money (other games name their own). Your balance shows in the frame, at the "
          "top of this window and on the Bag page. You earn Bones from the game's objects and by trading at a Pack Trader." },
        { "The bag",
          "The Bag page holds everything you carry in this game, one slot per kind of item, with how many you hold in the "
          "corner. Point at a slot for its name and count. Items come from the game's objects in-world: pick berries at a "
          "Berry Bush, gather herbs at a Herb Patch, fish at a Fishing Jetty, hunt deer at a Hunting Ground. Weapons and "
          "armour have an Equip button." },
        { "Eating, drinking and resting",
          "Hunger and thirst go down as you play. Eat what you gather (cook it at a Campfire), drink at a Drinking Pool or a "
          "Fresh Water Spring, and heal at a Healer's Stone. Touch an object to use it; it tells you what it did." },
        { "Pets",
          "Adopt a Wolf Pup or a Raven at a Pet Den. Each pet's card shows its photograph, its name, its age and how it is "
          "doing. Feed and look after them, or they fade." },
        { "Weapons and armour",
          "The Combat page shows your three weapon slots (main hand, off hand, ranged) and your armour on the figure (head, "
          "neck, body, hands, legs, feet, ring). Equip from the Bag, Unequip on the slot. A gun or bow shows rounds / "
          "magazine and the reserve; when it is empty it reads Reload - press Reload on the card. The weapons also show "
          "on screen, bottom right." },
        { "The action bar",
          "When the game gives you abilities they appear on the bar at the bottom of the screen. Click one, or press its key: "
          "1 to 9, 0, - and = for the first twelve (only while Game Mode is on and you are not typing). A dark sweep and a "
          "number show the cooldown; LOW means not enough of what it costs; a padlock means not available now; a crosshair "
          "means it needs a target. While you cast, a bar shows above it - Escape cancels." },
        { "Targets and fighting",
          "Click an avatar or object in the world to target it; its frame shows at the top centre with its health and whether "
          "it is hostile, neutral or friendly. Escape or the X clears it. Numbers rising from targets are the damage you deal; "
          "red numbers over you are damage you take. Helpful effects (buffs) and harmful ones (debuffs, red with a down arrow) "
          "show under your frame with their time left." },
        { "Down, dead and respawn",
          "When your health runs out you are down: the screen darkens and a countdown may show, and you cannot move. You "
          "respawn at the region's spawn point when it ends. Escape or Clear screen effects takes the overlay off your screen "
          "if you need to see - the game still holds you until it lets you go." },
        { "Your own background",
          "The Look page sets the picture behind this window. Drag a texture from your inventory anywhere onto the window, or "
          "choose a picture file on your computer. Picture strength sets how much it shows; a tint colours it; Use the "
          "theme's picture puts the howling wolf back. A dark veil over the picture keeps the text readable. Saved for your "
          "account." },
        { "Messages, dialogs and safety",
          "The game's notices appear at the top right and its questions as dialogs with buttons. Anything that is not from "
          "the region's own game is marked Unverified (a warning sign and the word) and is drawn plain - it may be any "
          "object or HUD." },
        { "Making your own game",
          "Region owners and creators run their own games with the wolfGame functions in LSL - for example wolfGameSet to "
          "name the game and its money, wolfGameShowStat and wolfGameSetStat for bars, wolfGameNotify and wolfGameDialog for "
          "messages, wolfGameSetButtons for actions, wolfGameItemAdd and wolfGameExchange for items, wolfGameSetAbility, "
          "wolfGameSetGear, wolfGameSetAmmo, wolfGameSetTarget, wolfGameAddBuff and wolfGameCombatText for combat, and "
          "wolfGameForceMode to require Game Mode on your region. The script editor completes them and shows their help." },
        { "Credits",
          "Photographs (cropped and colour-graded): portrait - Gary Kramer, US Fish and Wildlife Service (public domain); "
          "background - \"[7237] Howling wolf\" by zoofanatic (CC BY 2.0); figure - \"The Wolf (explored 27.4.2019)\" by "
          "E L from Western-Finland (CC BY 2.0); Wolf Pup - \"It's hot out there....\" by Keven Law (CC BY-SA 2.0); Raven - "
          "\"Portrait of Common Raven\" by Alan Vernon (CC BY 2.0). All from Wikimedia Commons; the full list with links is "
          "skins/default/textures/wolfgame/CREDITS.txt. Lettering: Cinzel Bold, The Cinzel Project Authors (SIL Open Font "
          "License 1.1)." },
    };
}

WolfGameConsole::WolfGameConsole(const Params& p)
:   LLUICtrl(p)
{
}

LLFloater* WolfGameConsole::floater()
{
    return getParentByType<LLFloater>();
}

void WolfGameConsole::showPage(EPage page)
{
    mPage = page;
    mConfirm = CONFIRM_NONE;
    mHits.setFocus(H_TAB, page);
}

void WolfGameConsole::setStatus(const std::string& text, bool error)
{
    mStatus = text;
    mStatusError = error;
    // An error stays until the next action (never vanishes by itself); a confirmation fades.
    mStatusUntil = error ? 0.0 : now() + STATUS_LIFE;
}

void WolfGameConsole::hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable, bool enabled)
{
    if (mInPage)
    {
        // A hit inside the scrolled page is cut to what shows of it.
        l = llmax(l, (F32)mPageRect.mLeft);
        r = llmin(r, (F32)mPageRect.mRight);
        b = llmax(b, (F32)mPageRect.mBottom);
        t = llmin(t, (F32)mPageRect.mTop);
        if (r <= l || t <= b) return;
    }
    mHits.add(l, b, r, t, id, arg, tip, focusable, enabled);
}

U32 WolfGameConsole::flagsFor(S32 id, S32 arg, bool enabled) const
{
    U32 f = 0;
    if (!enabled) f |= BTN_DISABLED;
    if (mHits.isHover(id, arg)) f |= BTN_HOVER;
    if (mHits.isPressed(id, arg)) f |= BTN_PRESSED;
    if (mKeyboardNav && hasFocus() && mHits.isFocused(id, arg)) f |= BTN_FOCUS;
    return f;
}

void WolfGameConsole::drawBackground(F32 W, F32 H, F32 cut, F32 alpha, const Palette& p)
{
    // The plate, then the picture over it at the player's strength and tint, then the frame.
    chamfer(0.f, 0.f, W, H, cut, fade(p.mBaseTop, alpha), fade(p.mBaseBottom, alpha));
    const F32 strength = WolfGameBackground::opacity() * alpha;
    LLColor4 tint = WolfGameBackground::tintColour(WolfGameBackground::tint());
    tint.mV[VALPHA] = strength;
    bool drawn = false;
    if (WolfGameBackground::source() != WolfGameBackground::SRC_THEME)
    {
        drawn = imageCover(WolfGameBackground::texture(), 1.f, 1.f, W - 1.f, H - 1.f, cut, tint);
    }
    if (!drawn)
    {
        // The theme's own picture: the howling wolf photograph (wolves), or steel (plain).
        if (p.mWolves)
        {
            if (!imageCover(photo("WolfGame_Background"), 1.f, 1.f, W - 1.f, H - 1.f, cut, tint))
            {
                nightScene(1.f, 1.f, W - 1.f, H - 1.f, cut, strength);   // until the photograph has loaded
            }
        }
        else
        {
            steelScene(1.f, 1.f, W - 1.f, H - 1.f, cut, strength);
        }
    }
    // A dark veil over the picture, so text keeps 4.5:1 even on a white picture: whatever the
    // strength, the picture may add at most 0.22 of white over the base - the base colour at 0.78
    // over white leaves luminance about 0.055: silver 8.8:1, dim silver 4.8:1, amber 4.9:1 (plain:
    // dim grey 4.7:1). So the veil is 1 - 0.22 / strength (none below strength 0.22).
    if (strength > 0.22f * alpha)
    {
        const F32 veil = 1.f - 0.22f * alpha / strength;
        LLColor4 v = p.mBaseBottom;
        v.mV[VALPHA] = veil;
        chamfer(1.f, 1.f, W - 1.f, H - 1.f, cut, v, v);
    }
    frame(0.f, 0.f, W, H, p, alpha, cut, false);
}

void WolfGameConsole::draw()
{
    mHits.clear();
    layoutAndDraw();
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfGameConsole::layoutAndDraw()
{
    WolfGame& g = WolfGame::instance();
    const Palette& p = gamePalette();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    LLFloater* f = floater();
    const F32 alpha = f ? llclamp(f->getCurrentTransparency(), 0.35f, 1.f) : 1.f;
    const F32 cut = 14.f, M = 14.f;
    LLGLSUIDefault gls_ui;
    drawBackground(W, H, cut, alpha, p);

    // ---- the title band (under the floater's drag strip: drag the window by it) ----
    const LLFontGL* caps_font = LLFontGL::getFontSansSerifSmallBold();
    F32 y = H - 10.f;
    F32 x = M + 2.f;
    crescent(x + 7.f, y - 9.f, 7.f, p.mText, p.mBaseTop);
    x += 20.f;
    x = textL(caps_font, "ROLEPLAY", x, y - 2.f, p.mDim) + 8.f;
    const std::string gname = g.game().mValid && !g.game().mName.empty() ? g.game().mName : std::string("No game here");
    textL(display(17.f), gname, x, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - x - 44.f);
    const F32 xcx = W - M - 8.f, xcy = y - 9.f;
    closeX(xcx, xcy, 5.f, mHits.isHover(H_CLOSE, 0) ? p.mAccent : p.mText);
    if (mKeyboardNav && hasFocus() && mHits.isFocused(H_CLOSE, 0)) focusRing(xcx - 9.f, xcy - 9.f, xcx + 9.f, xcy + 9.f, p);
    hit(xcx - 11.f, xcy - 11.f, xcx + 11.f, xcy + 11.f, H_CLOSE, 0, "Close this window (Escape)");

    // ---- hero, tabs ----
    F32 top = drawHero(M, W - M, H - 34.f, p);
    top = drawTabs(M, W - M, top - 6.f, p);

    // ---- footer, status, the game's action buttons ----
    drawFooter(M, W - M, 10.f, p);
    F32 bottom = 10.f + 34.f + 6.f;
    if (!mStatus.empty() && mStatusUntil > 0.0 && now() > mStatusUntil) mStatus.clear();
    if (!mStatus.empty())
    {
        const LLFontGL* sf = LLFontGL::getFontSansSerifSmall();
        const F32 sh = (F32)sf->getLineHeight() + 6.f;
        card(M, bottom, W - M, bottom + sh, p, 4.f);
        // an icon AND the words: a warning triangle for errors, a tick for confirmations
        if (mStatusError)
        {
            const F32 cx = M + 12.f, cy = bottom + sh * 0.5f;
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(WARN_FILL.mV);
            gGL.vertex2f(cx - 6.f, cy - 5.f); gGL.vertex2f(cx + 6.f, cy - 5.f); gGL.vertex2f(cx, cy + 6.f);
            gGL.end();
        }
        else
        {
            lineW(M + 7.f, bottom + sh * 0.5f, M + 10.f, bottom + sh * 0.5f - 3.f, 2.f, OK_TEXT);
            lineW(M + 10.f, bottom + sh * 0.5f - 3.f, M + 16.f, bottom + sh * 0.5f + 4.f, 2.f, OK_TEXT);
        }
        textL(sf, mStatus, M + 24.f, bottom + sh * 0.5f, mStatusError ? DANGER_TEXT : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, W - 2.f * M - 30.f);
        bottom += sh + 6.f;
    }
    if (!g.buttons().empty()) bottom = drawActions(M, W - M, bottom) + 6.f;

    // ---- the page ----
    drawPage(M, bottom, W - M, top - 6.f, p);

    // a texture being dragged over: say what dropping it does
    if (now() < mDragOverUntil)
    {
        chamferEdge(4.f, 4.f, W - 4.f, H - 4.f, cut - 2.f, 3.f, p.mAccent);
        const LLFontGL* bf = display(18.f);
        const std::string msg = "Drop to make this picture the background";
        const F32 tw = bf->getWidthF32(msg) + 30.f;
        card(W * 0.5f - tw * 0.5f, H * 0.5f - 20.f, W * 0.5f + tw * 0.5f, H * 0.5f + 20.f, p, 6.f);
        textL(bf, msg, W * 0.5f, H * 0.5f, p.mAccent, LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    if (mConfirm != CONFIRM_NONE) drawConfirm(p);
}

F32 WolfGameConsole::drawHero(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* small_bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    // the portrait with its rings and level badge (WoW unit frame + EVE arcs)
    const F32 pr = 38.f, ring = pr * 1.5f + 2.f;
    const F32 pcx = l + ring, pcy = top - ring - 2.f;
    wolfGamePortrait(pcx, pcy, pr, p);
    F32 x = l + 2.f * ring + 14.f;
    F32 y = top - 4.f;
    std::string name;
    LLAvatarName av;
    if (LLAvatarNameCache::get(gAgentID, &av)) name = av.getDisplayName();
    textL(display(24.f), name, x, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - x);
    y -= (F32)display(24.f)->getLineHeight() + 1.f;
    const WolfGame::Game& game = g.game();
    std::string state;
    if (g.hasLevel()) state = "Level " + thousands(g.level()) + "  -  ";
    if (!game.mValid) state += g.gameMode() ? "Game Mode is on - this region has no game" : "Game Mode is off";
    else if (g.playing()) state += "Playing " + game.mName;
    else state += (g.gameMode() || g.forced()) ? "Not playing " + game.mName + " here" : "Turn Game Mode on to play " + game.mName;
    textL(small, state, x, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - x);
    y -= slh + 8.f;

    // ---- the Game Mode switch (§3.2; §6 locked) - a big lit toggle, its state as a word ----
    const bool forced = g.forced();
    const bool pending = g.gameModePending();
    const bool on = forced || (pending ? g.gameModeWanted() : g.gameMode());
    const F32 sw = 206.f, sh = 44.f, sl = x, st = y, sb = st - sh;
    const LLColor4 lit = forced ? LLColor4(0.937f, 0.267f, 0.267f, 1.f) : p.mAccent;
    chamfer(sl, sb, sl + sw, st, 9.f, fade(lit, on ? 0.20f : 0.05f), fade(lit, on ? 0.06f : 0.02f));
    chamferEdge(sl + 0.5f, sb + 0.5f, sl + sw - 0.5f, st - 0.5f, 9.f, on ? 1.5f : 1.f, on ? lit : p.mButtonEdge);
    // the track and the knob
    const F32 tl = sl + 8.f, tw = 64.f, th = 28.f, tb = sb + (sh - th) * 0.5f;
    const LLColor4 track_on(0.973f, 0.761f, 0.361f, 1.f), track_off = p.mTrack;
    chamfer(tl, tb, tl + tw, tb + th, 13.f, on ? track_on : track_off, on ? lit : track_off);
    const F32 kr = 12.f, kcx = on ? tl + tw - kr - 2.f : tl + kr + 2.f, kcy = tb + th * 0.5f;
    disc(kcx, kcy, kr, LLColor4(0.05f, 0.08f, 0.14f, 1.f), 24);
    if (forced)
    {
        // a padlock on the knob (§6)
        rect(kcx - 5.f, kcy - 6.f, kcx + 5.f, kcy + 1.f, DANGER_TEXT);
        arc(kcx, kcy + 1.f, 3.5f, 1.8f, 0.f, F_PI, DANGER_TEXT, 10);
    }
    else
    {
        paw(kcx, kcy, 13.f, on ? p.mAccent : p.mDim);
    }
    const F32 wx = tl + tw + 12.f;
    textL(small_bold, "GAME MODE", wx, st - 6.f, p.mDim);
    const std::string word = forced ? "LOCKED ON" : pending ? (g.gameModeWanted() ? "TURNING ON" : "TURNING OFF") : (on ? "ON" : "OFF");
    textL(display(19.f), word, wx, sb + 5.f, forced ? DANGER_TEXT : (on ? p.mAccent : p.mText), LLFontGL::LEFT, LLFontGL::BOTTOM, sl + sw - wx - 4.f);
    if (mKeyboardNav && hasFocus() && mHits.isFocused(H_GAMEMODE, 0)) focusRing(sl, sb, sl + sw, st, p);
    std::string tip;
    if (forced) tip = "Game Mode is locked on: " + g.forcedText() + ".";
    else if (pending) tip = "Waiting for the region to answer...";
    else if (!g.toggleReady()) tip = std::string("Game Mode is ") + (on ? "on" : "off") + ". Wait a few seconds before switching again.";
    else tip = on ? "Game Mode is on. Click to turn it off." : "Game Mode is off. Click to play the region's game wherever you go.";
    // Always pressable: a press that cannot switch (locked, pending, too soon) says why.
    hit(sl, sb, sl + sw, st, H_GAMEMODE, 0, tip);

    // ---- the wallet (Paul: "where are the tokens") ----
    const std::string wallet = currentWallet();
    const F32 wl = sl + sw + 10.f;
    F32 bottom = sb;
    if (r - wl > 90.f)
    {
        card(wl, sb, r, st, p, 6.f);
        if (!wallet.empty())
        {
            walletIcon(wl + 22.f, sb + sh * 0.5f, 14.f, p);
            const std::string amount = thousands(g.balance());
            const std::string money = !g.currencyName().empty() ? g.currencyName() : game.mCurrency;
            textL(display(21.f), amount, wl + 42.f, st - 5.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - wl - 48.f);
            textL(small, money.empty() ? g.currencySymbol() : money, wl + 42.f, sb + 5.f, p.mDim, LLFontGL::LEFT, LLFontGL::BOTTOM, r - wl - 48.f);
            hit(wl, sb, r, st, H_INFO, 1, "Your wallet: " + wallet + ". Earn it from the game's objects and by trading.", false);
        }
        else
        {
            textL(small, game.mValid && !game.mCurrency.empty() ? "Play to earn " + game.mCurrency : std::string("No money in this game"),
                  wl + 10.f, sb + sh * 0.5f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, r - wl - 16.f);
        }
    }
    // §6: the reason, in words, under the switch
    if (forced)
    {
        bottom -= 4.f;
        textL(small_bold, g.forcedText() + ".", sl, bottom, DANGER_TEXT, LLFontGL::LEFT, LLFontGL::TOP, r - sl);
        bottom -= slh;
    }
    bottom = llmin(bottom, pcy - ring);
    // the XP bar under it all (WoW's experience bar)
    if (g.hasLevel())
    {
        bottom -= 8.f;
        const std::string xp = g.xpNext() > 0 ? thousands(g.xp()) + " / " + thousands(g.xpNext()) + " XP" : thousands(g.xp()) + " XP";
        tabularRight(small, xp, r, bottom + slh + 1.f, p.mDim);
        rect(l, bottom - 4.f, r, bottom, p.mTrack);
        const F32 frac = g.xpNext() > 0 ? llclamp((F32)((F64)g.xp() / (F64)g.xpNext()), 0.f, 1.f) : 1.f;
        rectG(l, bottom - 4.f, l + (r - l) * frac, bottom, readable(p.mAccent, 0.6f), p.mAccent);
        bottom -= 4.f;
    }
    return bottom - 4.f;
}

F32 WolfGameConsole::drawTabs(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* f = LLFontGL::getFontSansSerifSmallBold();
    const F32 h = 28.f, n = (F32)PAGE_COUNT;
    const F32 w = (r - l) / n;
    for (S32 i = 0; i < PAGE_COUNT; ++i)
    {
        const F32 tl = l + w * i, tr = tl + w;
        const bool on = mPage == i;
        if (on) rectG(tl, top - h, tr, top, fade(p.mAccent, 0.18f), fade(p.mAccent, 0.02f));
        if (on) rect(tl, top - h, tr, top - h + 2.f, p.mAccent);
        const bool hover = mHits.isHover(H_TAB, i);
        textL(f, caps(PAGE_NAMES[i]), (tl + tr) * 0.5f, top - h * 0.5f, on ? p.mText : (hover ? p.mAccent : p.mDim),
              LLFontGL::HCENTER, LLFontGL::VCENTER, w - 4.f);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(H_TAB, i)) focusRing(tl + 2.f, top - h + 2.f, tr - 2.f, top - 2.f, p);
        hit(tl, top - h, tr, top, H_TAB, i, PAGE_TIPS[i]);
    }
    // the hairline under the tabs, brightest in the middle
    rect(l, top - h - 1.f, r, top - h, fade(p.mAccent, 0.35f));
    return top - h - 1.f;
}

void WolfGameConsole::drawFooter(F32 l, F32 r, F32 bottom, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const F32 h = 30.f;
    // §3.2 "Leave this game": this game only; hidden where the region forces Game Mode (§6).
    if (g.game().mValid && g.playing() && !g.forced())
    {
        const bool ready = g.toggleReady();
        const F32 bw = 150.f;
        button(l, bottom, l + bw, bottom + h, "Leave this game", p, flagsFor(H_LEAVE, 0, ready) | BTN_DANGER);
        hit(l, bottom, l + bw, bottom + h, H_LEAVE, 0,
            ready ? "Stop playing this game only. Your stats are kept and Game Mode stays on." : "Wait a few seconds - the region ignores a second change that quickly.",
            true, ready);
    }
    const F32 bw = 170.f;
    button(r - bw, bottom, r, bottom + h, "Clear screen effects", p, flagsFor(H_CLEARFX, 0));
    hit(r - bw, bottom, r, bottom + h, H_CLEARFX, 0, "Take every screen effect off now (flash, blackout, blur, the down overlay). Escape does it too.");
}

F32 WolfGameConsole::drawActions(F32 l, F32 r, F32 bottom)
{
    WolfGame& g = WolfGame::instance();
    // [SECURITY 2026-10-09] Buttons that are not the game's own never wear its theme, and say so.
    const bool verified = g.buttonsVerified();
    const Palette& p = palette(verified);
    const LLFontGL* small_bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small_bold->getLineHeight();
    const S32 n = (S32)g.buttons().size();
    const S32 per_row = (r - l) >= 440.f ? 4 : 3;
    const S32 rows = (n + per_row - 1) / per_row;
    const F32 bh = 28.f, gap = 6.f;
    const F32 h = slh + 6.f + rows * bh + (rows - 1) * gap;
    const F32 top = bottom + h;
    // the header: who set the buttons, and "Unverified" (icon + word)
    F32 hx = l;
    hx = textL(small_bold, caps("Actions"), hx, top, p.mAccent) + 8.f;
    if (!g.buttonsSrc().empty()) hx = textL(LLFontGL::getFontSansSerifSmall(), "from " + g.buttonsSrc(), hx, top, p.mDim) + 8.f;
    if (!verified)
    {
        F32 ur = hx;
        unverifiedMark(hx, top, LLFontGL::getFontSansSerifSmall(), &ur);
    }
    const F32 bw = ((r - l) - gap * (per_row - 1)) / per_row;
    for (S32 i = 0; i < n; ++i)
    {
        const S32 col = i % per_row, row = i / per_row;
        const F32 bl = l + col * (bw + gap), bt = top - slh - 6.f - row * (bh + gap);
        const WolfGame::Button& b = g.buttons()[i];
        button(bl, bt - bh, bl + bw, bt, b.mLabel, p, flagsFor(H_ACTION, i));
        hit(bl, bt - bh, bl + bw, bt, H_ACTION, i,
            b.mLabel + (verified ? std::string(" - the game's own action") : " - Unverified: from " + (g.buttonsSrc().empty() ? std::string("a script") : g.buttonsSrc()) + ", not the game's own script"));
    }
    return top;
}

// ---- the page: a scrolled, clipped area ----

void WolfGameConsole::drawPage(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    if (t - b < 20.f) return;
    mPageRect = LLRect((S32)l, (S32)t, (S32)r, (S32)b);
    const F32 bar_w = 6.f;
    const F32 cr = r - bar_w - 6.f;          // the content stops short of the scroll bar
    F32& scroll = mScroll[mPage];
    F32 bottom = 0.f;
    {
        LLLocalClipRect clip(mPageRect);
        mInPage = true;
        const F32 top = t - 6.f + scroll;
        switch (mPage)
        {
        case PAGE_STATS:  bottom = pageStats(l, cr, top, p); break;
        case PAGE_COMBAT: bottom = pageCombat(l, cr, top, p); break;
        case PAGE_BAG:    bottom = pageBag(l, cr, top, p); break;
        case PAGE_PETS:   bottom = pagePets(l, cr, top, p); break;
        case PAGE_GAME:   bottom = pageGame(l, cr, top, p); break;
        case PAGE_LOOK:   bottom = pageLook(l, cr, top, p); break;
        case PAGE_HELP:   bottom = pageHelp(l, cr, top, p); break;
        default: break;
        }
        mInPage = false;
        mContentH = top - bottom + 8.f;
    }
    // keep the scroll inside the content
    const F32 view_h = t - b;
    const F32 max_scroll = llmax(0.f, mContentH - view_h);
    scroll = llclamp(scroll, 0.f, max_scroll);
    if (max_scroll > 0.f)
    {
        // a thin scroll bar: track and thumb, draggable
        const F32 tl = r - bar_w, th = llmax(24.f, view_h * view_h / mContentH);
        const F32 ty = t - (view_h - th) * (scroll / max_scroll);
        rect(tl, b, r, t, fade(p.mTrack, 0.8f));
        rect(tl, ty - th, r, ty, mHits.isHover(H_SCROLLBAR, 0) || mDraggingScroll ? p.mAccent : fade(p.mDim, 0.8f));
        mHits.add(tl - 3.f, b, r + 3.f, t, H_SCROLLBAR, 0, "Drag to scroll (or use the mouse wheel, Page Up and Page Down)", false);
    }
}

F32 WolfGameConsole::sectionHead(F32 l, F32 r, F32 top, const std::string& text, const Palette& p)
{
    const LLFontGL* f = LLFontGL::getFontSansSerifSmallBold();
    const F32 lh = (F32)f->getLineHeight();
    textL(f, caps(text), l, top, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    rect(l, top - lh - 3.f, r, top - lh - 2.f, fade(p.mAccent, 0.3f));
    return top - lh - 8.f;
}

F32 WolfGameConsole::paragraph(F32 l, F32 r, F32 top, const std::string& text, const LLColor4& colour, const LLFontGL* font)
{
    if (!font) font = LLFontGL::getFontSansSerif();
    const F32 lh = (F32)font->getLineHeight();
    // The Help page wraps several KB each frame otherwise: kept by font, width and text.
    static std::map<std::string, std::vector<std::string>> s_wrapped;
    const std::string key = llformat("%p|%d|", (const void*)font, (S32)(r - l)) + text;
    auto it = s_wrapped.find(key);
    if (it == s_wrapped.end())
    {
        if (s_wrapped.size() > 128) s_wrapped.clear();
        it = s_wrapped.emplace(key, wrap(font, text, r - l, 80)).first;
    }
    F32 y = top;
    for (const std::string& line : it->second)
    {
        textL(font, line, l, y, colour);
        y -= lh;
    }
    return y;
}

F32 WolfGameConsole::emptyCard(F32 l, F32 r, F32 top, const std::string& head, const std::string& body, const Palette& p)
{
    // Carbon / PatternFly empty state: what is missing, why, and how to get it - as words.
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const std::vector<std::string> lines = wrap(f, body, r - l - 24.f, 8);
    const F32 h = 12.f + (F32)display(16.f)->getLineHeight() + 4.f + lines.size() * (F32)f->getLineHeight() + 12.f;
    card(l, top - h, r, top, p, 6.f);
    F32 y = top - 12.f;
    textL(display(16.f), head, l + 12.f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
    y -= (F32)display(16.f)->getLineHeight() + 4.f;
    for (const std::string& line : lines)
    {
        textL(f, line, l + 12.f, y, p.mDim);
        y -= (F32)f->getLineHeight();
    }
    return top - h - 10.f;
}

// ---- Stats ----

F32 WolfGameConsole::pageStats(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    F32 y = top;
    if (g.stats().empty())
    {
        y = emptyCard(l, r, y, "No stats yet",
                      g.game().mValid ? (g.gameMode() || g.forced() ? "Your stats appear here as soon as the game sends them."
                                                                    : "Turn Game Mode on to play " + g.game().mName + " - your stats appear here.")
                                      : "This region isn't running a roleplay game. A worn game HUD can still show stats here.",
                      p);
    }
    else
    {
        // Every stat as a ring gauge (EVE's arcs), its number in the middle, its name under it.
        y = sectionHead(l, r, y, "Stats", p);
        const F32 cw = 108.f, ch = 112.f;
        const S32 cols = llmax(1, (S32)((r - l) / cw));
        const F32 gap = ((r - l) - cols * cw) / llmax(1, cols);
        S32 i = 0;
        for (const WolfGame::Stat& s : g.stats())
        {
            const S32 col = i % cols, row = i / cols;
            const F32 cl = l + col * (cw + gap) + gap * 0.5f, ct = y - row * ch;
            card(cl + 2.f, ct - ch + 6.f, cl + cw - 2.f, ct, p, 6.f);
            const F32 cx = cl + cw * 0.5f, cy = ct - 46.f;
            const F32 frac = s.mMax > 0 ? (F32)s.mValue / (F32)s.mMax : 0.f;
            gauge(cx, cy, 32.f, 7.f, frac, s.mColor, p.mTrack);
            textL(display(22.f), thousands(s.mValue), cx, cy + 3.f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER);
            textL(small, "/ " + thousands(s.mMax), cx, cy - 12.f, p.mDim, LLFontGL::HCENTER, LLFontGL::VCENTER);
            F32 lx = cx;
            const F32 lw = LLFontGL::getFontSansSerifSmallBold()->getWidthF32(s.mLabel);
            if (!s.mIcon.empty())
            {
                const F32 is = 16.f;
                icon(s.mIcon, cx - (lw + is + 4.f) * 0.5f, ct - ch + 14.f, is, p, std::string(), false);
                lx += (is + 4.f) * 0.5f;
            }
            textL(LLFontGL::getFontSansSerifSmallBold(), s.mLabel, lx, ct - ch + 22.f, p.mText, LLFontGL::HCENTER, LLFontGL::VCENTER, cw - 8.f);
            hit(cl, ct - ch, cl + cw, ct, H_INFO, 100 + i, s.mLabel + ": " + thousands(s.mValue) + " of " + thousands(s.mMax), false);
            ++i;
        }
        y -= ((i + cols - 1) / cols) * ch + 8.f;
    }
    if (g.hasLevel())
    {
        y = sectionHead(l, r, y, "Level", p);
        const std::string xp = g.xpNext() > 0 ? thousands(g.xp()) + " / " + thousands(g.xpNext()) + " XP to level " + thousands(g.level() + 1)
                                              : thousands(g.xp()) + " XP";
        textL(display(18.f), "Level " + thousands(g.level()), l, y, p.mText);
        tabularRight(small, xp, r, y - 2.f, p.mDim);
        y -= (F32)display(18.f)->getLineHeight() + 4.f;
        rect(l, y - 6.f, r, y, p.mTrack);
        const F32 frac = g.xpNext() > 0 ? llclamp((F32)((F64)g.xp() / (F64)g.xpNext()), 0.f, 1.f) : 1.f;
        rectG(l, y - 6.f, l + (r - l) * frac, y, readable(p.mAccent, 0.6f), p.mAccent);
        y -= 18.f;
    }
    // the game's own information panel (§3.3)
    if (!g.panelRows().empty())
    {
        y = sectionHead(l, r, y, g.panelTitle().empty() ? std::string("Information") : g.panelTitle(), p);
        const F32 lh = (F32)body->getLineHeight() + 4.f;
        card(l, y - lh * g.panelRows().size() - 8.f, r, y, p, 6.f);
        y -= 6.f;
        for (const WolfGame::Row& row : g.panelRows())
        {
            textL(body, row.mLabel, l + 10.f, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.45f);
            textL(body, row.mValue, r - 10.f, y, p.mText, LLFontGL::RIGHT, LLFontGL::TOP, (r - l) * 0.5f);
            y -= lh;
        }
        y -= 12.f;
    }
    // markers the game placed on other regions (§3.9), each with Show on map
    mMarkerIds.clear();
    LLViewerRegion* rgn = gAgent.getRegion();
    const std::string here = rgn ? rgn->getName() : std::string();
    for (const auto& kv : g.markers())
    {
        if (LLStringUtil::compareInsensitive(kv.second.mRegion, here) != 0) mMarkerIds.push_back(kv.first);
    }
    if (!mMarkerIds.empty())
    {
        y = sectionHead(l, r, y, "Markers on other regions", p);
        for (size_t i = 0; i < mMarkerIds.size(); ++i)
        {
            const WolfGame::Marker& m = g.markers().at(mMarkerIds[i]);
            const std::string line = llformat("%s - %s (%d, %d, %d)", (m.mLabel.empty() ? m.mId : m.mLabel).c_str(), m.mRegion.c_str(),
                                              (S32)llround(m.mPos.mV[VX]), (S32)llround(m.mPos.mV[VY]), (S32)llround(m.mPos.mV[VZ]));
            rect(l, y - 14.f, l + 10.f, y - 4.f, m.mColor);
            textL(body, line, l + 16.f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 140.f);
            const F32 bl = r - 120.f;
            button(bl, y - 22.f, r, y, "Show on map", p, flagsFor(H_MARKER_MAP, (S32)i));
            hit(bl, y - 22.f, r, y, H_MARKER_MAP, (S32)i, "Show " + (m.mLabel.empty() ? std::string("this marker") : m.mLabel) + " on the world map; teleport from there");
            y -= 28.f;
        }
    }
    return y;
}

// ---- Combat (§5.3) ----

F32 WolfGameConsole::weaponCard(F32 l, F32 r, F32 top, S32 idx, const Palette& game_p)
{
    WolfGame& g = WolfGame::instance();
    const std::string& slot = WolfGame::weaponSlots()[idx];
    const WolfGame::SlotView v = g.slotView(slot);
    const Palette& p = v.mFilled ? palette(v.mVerified) : game_p;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 h = 132.f, b = top - h;
    const bool active = g.activeWeapon() == slot;
    card(l, b, r, top, p, 6.f);
    if (active) chamferEdge(l + 1.f, b + 1.f, r - 1.f, top - 1.f, 6.f, 2.f, p.mAccent);
    textL(bold, caps(WolfGame::slotLabel(slot)) + (active ? " - ACTIVE" : ""), l + 8.f, top - 6.f, active ? p.mAccent : p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 16.f);
    const F32 IS = 48.f, il = l + (r - l - IS) * 0.5f, it = top - 8.f - slh - 4.f;
    icon(v.mIcon, il, it - IS, IS, p, v.mFilled ? initialsOf(v.mLabel) : std::string());
    const std::string pending = g.equipPending();
    F32 y = it - IS - 4.f;
    std::string tip = WolfGame::slotLabel(slot) + ": ";
    if (!v.mFilled)
    {
        textL(small, pending == slot ? "Equipping..." : "Empty", (l + r) * 0.5f, y, p.mDim, LLFontGL::HCENTER, LLFontGL::TOP, r - l - 8.f);
        tip += "empty. Equip a weapon from the Bag.";
        hit(l, b, r, top, H_GEAR, idx, tip, false);
        return b;
    }
    textL(bold, v.mLabel, (l + r) * 0.5f, y, p.mText, LLFontGL::HCENTER, LLFontGL::TOP, r - l - 8.f);
    y -= slh;
    std::string detail = v.mDamage;
    if (!v.mRange.empty()) detail += (detail.empty() ? "" : " - ") + v.mRange;
    if (!v.mFireMode.empty()) detail += (detail.empty() ? "" : " - ") + v.mFireMode;
    if (!detail.empty()) textL(small, detail, (l + r) * 0.5f, y, p.mDim, LLFontGL::HCENTER, LLFontGL::TOP, r - l - 8.f);
    y -= slh;
    tip += v.mLabel + (detail.empty() ? "" : " (" + detail + ")");
    const WolfGame::Ammo* am = g.ammoFor(slot);
    const F64 t = LLFrameTimer::getTotalSeconds();
    if (am && am->mRounds >= 0)
    {
        const bool out = am->mRounds == 0;
        std::string count = thousands(am->mRounds) + (am->mMagazine > 0 ? " / " + thousands(am->mMagazine) : std::string());
        if (am->mReserve >= 0) count += " - " + thousands(am->mReserve);
        textL(bold, out ? count + "  RELOAD" : count, (l + r) * 0.5f, y, out ? DANGER_TEXT : p.mText, LLFontGL::HCENTER, LLFontGL::TOP, r - l - 8.f);
        tip += ", ammo " + count + (v.mAmmoName.empty() ? std::string() : " (" + v.mAmmoName + ")");
    }
    // the buttons along the foot: Reload when the ammo is counted, Unequip when it is a loadout item
    const F32 bb = b + 6.f, bh = 20.f;
    const bool reloading = am && am->mReloadEnds > t;
    F32 bx = l + 6.f;
    const F32 half = (r - l - 18.f) * 0.5f;
    if (am && am->mRounds >= 0)
    {
        button(bx, bb, bx + half, bb + bh, reloading ? "Reloading" : "Reload", p, flagsFor(H_RELOAD, idx, !reloading) | (am->mRounds == 0 ? BTN_DANGER : 0), small);
        hit(bx, bb, bx + half, bb + bh, H_RELOAD, idx, "Reload " + v.mLabel, true, !reloading);
    }
    if (!v.mItem.empty())
    {
        const bool can = pending.empty();
        const F32 ul = r - 6.f - half;
        button(ul, bb, r - 6.f, bb + bh, pending == slot ? "Equipping..." : "Unequip", p, flagsFor(H_UNEQUIP, idx, can), small);
        hit(ul, bb, r - 6.f, bb + bh, H_UNEQUIP, idx, "Take " + v.mLabel + " off (it goes back to your bag)", true, can);
    }
    if (!v.mVerified) tip += " - Unverified: shown by " + (v.mSrc.empty() ? std::string("a script") : v.mSrc) + ", not the game's own";
    hit(l, b, r, top, H_GEAR, idx, tip, false);
    return b;
}

F32 WolfGameConsole::armourSlot(F32 l, F32 top, F32 w, S32 idx, bool right_aligned, const Palette& game_p)
{
    WolfGame& g = WolfGame::instance();
    const std::string& slot = WolfGame::armourSlots()[idx];
    const S32 arg = 3 + idx;   // after the three weapon slots
    const WolfGame::SlotView v = g.slotView(slot);
    const Palette& p = v.mFilled ? palette(v.mVerified) : game_p;
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 S = 44.f, h = S + 6.f;
    const F32 sl = right_aligned ? l + w - S : l;
    icon(v.mIcon, sl, top - S, S, p, v.mFilled ? initialsOf(v.mLabel) : std::string());
    const F32 tl = right_aligned ? l : l + S + 6.f, tr = right_aligned ? l + w - S - 6.f : l + w;
    const F32 tx = right_aligned ? tr : tl;
    const LLFontGL::HAlign ha = right_aligned ? LLFontGL::RIGHT : LLFontGL::LEFT;
    F32 y = top;
    textL(bold, caps(WolfGame::slotLabel(slot)), tx, y, p.mDim, ha, LLFontGL::TOP, tr - tl);
    y -= slh;
    const std::string pending = g.equipPending();
    // the item and its defence on one line (§5.3 "Each shows the item and its defence, or Empty")
    std::string what = v.mFilled ? v.mLabel + (v.mDefence.empty() ? std::string() : " - " + v.mDefence) : std::string("Empty");
    if (pending == slot) what = "Equipping...";
    textL(small, what, tx, y, pending == slot ? p.mAccent : (v.mFilled ? p.mText : p.mDim), ha, LLFontGL::TOP, tr - tl);
    y -= slh + 2.f;
    std::string tip = WolfGame::slotLabel(slot) + ": " + (v.mFilled ? v.mLabel + (v.mDefence.empty() ? "" : " (" + v.mDefence + ")") : "empty. Equip armour from the Bag.");
    if (v.mFilled && !v.mItem.empty())
    {
        // Unequip, a small button under the words
        const F32 bw = 64.f, bh = 18.f, bl = right_aligned ? tr - bw : tl;
        const bool can = pending.empty();
        button(bl, y - bh, bl + bw, y, "Unequip", p, flagsFor(H_UNEQUIP, arg, can), small);
        hit(bl, y - bh, bl + bw, y, H_UNEQUIP, arg, "Take " + v.mLabel + " off (it goes back to your bag)", true, can);
    }
    if (v.mFilled && !v.mVerified) tip += " - Unverified: shown by " + (v.mSrc.empty() ? std::string("a script") : v.mSrc);
    hit(sl, top - S, sl + S, top, H_GEAR, arg, tip, false);
    return top - h;
}

F32 WolfGameConsole::pageCombat(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    // Weapons: the three slots as large cards (Destiny's slots)
    y = sectionHead(l, r, y, "Weapons", p);
    const F32 gap = 8.f, cw = ((r - l) - 2.f * gap) / 3.f;
    F32 low = y;
    for (S32 i = 0; i < 3; ++i) low = llmin(low, weaponCard(l + i * (cw + gap), l + i * (cw + gap) + cw, y, i, p));
    y = low - 12.f;
    // Armour: the paper doll - the photographed wolf in the middle, the seven slots round it
    y = sectionHead(l, r, y, "Armour", p);
    const F32 doll_h = 4.f * 50.f + 4.f;
    const F32 side = llmin(150.f, (r - l) * 0.3f);
    card(l, y - doll_h, r, y, p, 6.f);
    {
        const F32 fl = l + side + 8.f, fr = r - side - 8.f, fw = fr - fl;
        LLViewerTexture* fig = photo("WolfGame_Figure");
        if (fig && fig->hasGLTexture() && fw > 40.f)
        {
            const F32 aspect = (F32)llmax(1, fig->getFullWidth()) / (F32)llmax(1, fig->getFullHeight());
            F32 iw = fw, ih = fw / aspect;
            if (ih > doll_h - 20.f) { ih = doll_h - 20.f; iw = ih * aspect; }
            const F32 ix = fl + (fw - iw) * 0.5f, iy = y - doll_h * 0.5f - ih * 0.5f;
            // a warm glow behind the figure, then the photograph (its own alpha cut-out)
            disc(fl + fw * 0.5f, y - doll_h * 0.55f, llmin(fw, doll_h) * 0.45f, fade(p.mAccent, 0.07f), 40);
            imageCover(fig, ix, iy, ix + iw, iy + ih, 0.f, LLColor4::white);
        }
    }
    {
        const F32 sy = y - 6.f;
        armourSlot(l + 8.f, sy, side, 0, false, p);          // head
        armourSlot(l + 8.f, sy - 50.f, side, 1, false, p);   // neck
        armourSlot(l + 8.f, sy - 100.f, side, 2, false, p);  // body
        armourSlot(l + 8.f, sy - 150.f, side, 3, false, p);  // hands
        armourSlot(r - 8.f - side, sy, side, 4, true, p);            // legs
        armourSlot(r - 8.f - side, sy - 50.f, side, 5, true, p);     // feet
        armourSlot(r - 8.f - side, sy - 100.f, side, 6, true, p);    // ring
    }
    y -= doll_h + 12.f;
    // Abilities, as a list (§5.3), each usable from here too
    y = sectionHead(l, r, y, "Abilities", p);
    if (g.abilities().empty())
    {
        y = paragraph(l, r, y, "No abilities yet. When the game gives you some they appear here and on the action bar at the bottom of the screen.", p.mDim) - 8.f;
    }
    const F64 t = LLFrameTimer::getTotalSeconds();
    S32 i = 0;
    for (const WolfGame::Ability& a : g.abilities())
    {
        const Palette& ap = palette(a.mVerified);
        icon(a.mIcon, l, y - 30.f, 30.f, ap, initialsOf(a.mLabel));
        textL(bold, a.mLabel, l + 38.f, y, ap.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 160.f);
        std::string d;
        if (a.mAmount > 0 && !a.mCost.empty()) d = std::to_string(a.mAmount) + " " + (a.mCostLabel.empty() ? a.mCost : a.mCostLabel);
        if (a.mCooldown > 0.f) d += (d.empty() ? "" : " - ") + llformat("%.0f s cooldown", a.mCooldown);
        if (a.mCast > 0.f) d += (d.empty() ? "" : " - ") + llformat("%.1f s cast", a.mCast);
        if (a.mCooldownEnds > t) d += (d.empty() ? "" : " - ") + llformat("ready in %d s", (S32)ceil(a.mCooldownEnds - t));
        textL(small, d, l + 38.f, y - slh, ap.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 160.f);
        static const char* KEYS[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
        const std::string key = i < 12 ? KEYS[i] : std::string();
        const F32 bl = r - 110.f;
        const bool can = a.mOn && a.mCooldownEnds <= t;
        button(bl, y - 26.f, r, y - 2.f, "Use (" + key + ")", ap, flagsFor(H_ABILITY, i, can), small);
        hit(bl, y - 26.f, r, y - 2.f, H_ABILITY, i, a.mLabel + (can ? ": use it now, or press " + key : std::string(": not ready")), true, can);
        y -= 38.f;
        ++i;
    }
    // Buffs and debuffs with their full text (§5.3)
    y = sectionHead(l, r, y - 4.f, "Effects on you", p);
    if (g.buffs().empty())
    {
        y = paragraph(l, r, y, "None right now. Helpful effects and harmful ones (red, with a down arrow) appear here and under your frame.", p.mDim) - 8.f;
    }
    for (const WolfGame::Buff& b : g.buffs())
    {
        const Palette& bp = palette(b.mVerified);
        icon(b.mIcon, l, y - 28.f, 28.f, bp, initialsOf(b.mLabel));
        if (!b.mGood) chamferEdge(l + 0.5f, y - 27.5f, l + 27.5f, y - 0.5f, 4.f, 2.f, LLColor4(0.937f, 0.267f, 0.267f, 1.f));
        std::string head = b.mLabel + (b.mGood ? "  (helpful)" : "  (harmful)");
        if (b.mStacks > 1) head += llformat("  x%d", b.mStacks);
        if (b.mDuration > 0.f) head += llformat("  - %d s left", (S32)ceil(llmax(0.0, b.mEnds - t)));
        textL(bold, head, l + 36.f, y, b.mGood ? bp.mText : DANGER_TEXT, LLFontGL::LEFT, LLFontGL::TOP, r - l - 36.f);
        F32 ty = y - slh;
        if (!b.mText.empty()) ty = paragraph(l + 36.f, r, ty, b.mText, bp.mDim, small);
        if (!b.mVerified)
        {
            F32 ur = 0.f;
            unverifiedMark(l + 36.f, ty, small, &ur);
            ty -= slh;
        }
        y = llmin(ty, y - 32.f) - 6.f;
    }
    return y;
}

// ---- Bag: always a slot grid, empty slots too (Albion's slots; Paul: "where are the game inventory items") ----

F32 WolfGameConsole::pageBag(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    // the wallet and the count along the top
    {
        bag((l + 10.f), y - 10.f, 18.f, p.mAccent);
        F32 x = textL(display(17.f), "Bag", l + 24.f, y, p.mAccent);
        textL(small, g.game().mItemDefs.empty() ? llformat("%d kinds of item", (S32)g.items().size())
                                                : llformat("%d of %d kinds of item", (S32)g.items().size(), (S32)g.game().mItemDefs.size()),
              x + 10.f, y - 3.f, p.mDim);
        const std::string wallet = currentWallet();
        if (!wallet.empty())
        {
            const F32 ww = bold->getWidthF32(wallet) + 34.f;
            card(r - ww, y - 22.f, r, y, p, 4.f);
            walletIcon(r - ww + 12.f, y - 11.f, 8.f, p);
            textL(bold, wallet, r - ww + 24.f, y - 11.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        y -= 30.f;
    }
    // The slots: what the player holds, then every other item the game defines at 0 (game.items),
    // all sorted by order then name (WolfGameModule.cs ItemDefsJson / SendPlayer).
    struct Slot { const WolfGame::Item* mItem; S32 mHeld; };   // mHeld: index in items(), or -1 at 0
    std::vector<Slot> slots;
    for (S32 i = 0; i < (S32)g.items().size(); ++i) slots.push_back({ &g.items()[i], i });
    for (const WolfGame::Item& d : g.game().mItemDefs)
    {
        const bool held = std::any_of(g.items().begin(), g.items().end(), [&](const WolfGame::Item& h) { return h.mName == d.mName; });
        if (!held) slots.push_back({ &d, -1 });
    }
    std::stable_sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b)
    {
        if (a.mItem->mOrder != b.mItem->mOrder) return a.mItem->mOrder < b.mItem->mOrder;
        return a.mItem->mName < b.mItem->mName;
    });
    if (g.items().empty())
    {
        y = paragraph(l, r, y, g.game().mValid
                      ? "Your bag is empty. Items come from this game's objects in-world: pick berries at a Berry Bush, gather herbs at a "
                        "Herb Patch, fish at a Fishing Jetty, hunt at a Hunting Ground, trade at a Pack Trader. What you gather appears in "
                        "these slots."
                      : "This region isn't running a roleplay game, so there is nothing to carry here.", p.mDim) - 6.f;
    }
    // the grid: at least four rows, every slot drawn
    const F32 S = 56.f, cell_w = 66.f, cell_h = S + slh + 26.f;
    const S32 cols = llmax(1, (S32)((r - l) / cell_w));
    const F32 gap = ((r - l) - cols * cell_w) / llmax(1, cols);
    const S32 n = (S32)slots.size();
    const S32 rows = llmax(4, (n + cols - 1) / cols);
    const std::string pending = g.equipPending();
    for (S32 i = 0; i < rows * cols; ++i)
    {
        const S32 col = i % cols, row = i / cols;
        const F32 cl = l + col * (cell_w + gap) + gap * 0.5f + (cell_w - S) * 0.5f, ct = y - row * cell_h;
        if (i >= n)
        {
            // an empty slot: the recessed well with a faint ring
            icon(std::string(), cl, ct - S, S, p);
            arc(cl + S * 0.5f, ct - S * 0.5f, S * 0.22f, 1.f, 0.f, 2.f * F_PI, fade(p.mDim, 0.18f), 24);
            continue;
        }
        const WolfGame::Item& it = *slots[i].mItem;
        const bool held = slots[i].mHeld >= 0;
        icon(it.mIcon, cl, ct - S, S, p, initialsOf(it.mLabel));
        if (held)
        {
            chamferEdge(cl + 0.5f, ct - S + 0.5f, cl + S - 0.5f, ct - 0.5f, 5.f, 1.5f, fade(p.mAccent, 0.6f));
        }
        else
        {
            // one the game has that the player holds none of: dimmed, the count 0 (a number, not
            // colour alone)
            rect(cl + 2.f, ct - S + 2.f, cl + S - 2.f, ct - 2.f, LLColor4(0.f, 0.f, 0.f, 0.55f));
        }
        // the count, bottom-right on a dark fade (Albion)
        const std::string q = thousands(it.mQty);
        const F32 qw = bold->getWidthF32(q) + 6.f;
        rectG(cl + S - qw - 3.f, ct - S + 2.f, cl + S - 2.f, ct - S + slh + 2.f, LLColor4(0.f, 0.f, 0.f, 0.75f), LLColor4(0.f, 0.f, 0.f, 0.75f));
        bold->renderUTF8(q, 0, cl + S - 4.f, ct - S + 3.f, held ? LLColor4::white : p.mDim, LLFontGL::RIGHT, LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
        // the label under it - icon AND words, never the icon alone
        textL(small, it.mLabel, cl + S * 0.5f, ct - S - 2.f, held ? p.mText : p.mDim, LLFontGL::HCENTER, LLFontGL::TOP, cell_w - 2.f);
        std::string tip = it.mLabel + (held ? " x" + q : std::string(" - none yet"));
        if (!it.mKind.empty()) tip += " (" + it.mKind + ")";
        if (held && !it.mSlot.empty())
        {
            // §5.3 "Each Items line with a slot has an Equip button"
            const S32 idx = slots[i].mHeld;
            const bool waiting = pending == it.mSlot || pending == it.mName;
            const bool can = pending.empty();
            const F32 bt = ct - S - slh - 3.f;
            button(cl - 4.f, bt - 18.f, cl + S + 4.f, bt, waiting ? "Equipping..." : "Equip", p, flagsFor(H_EQUIP, idx, can), small);
            hit(cl - 4.f, bt - 18.f, cl + S + 4.f, bt, H_EQUIP, idx, "Equip " + it.mLabel + " (" + WolfGame::slotLabel(it.mSlot) + ")", true, can);
            tip += " - equips to " + WolfGame::slotLabel(it.mSlot);
        }
        hit(cl, ct - S, cl + S, ct, H_SLOT, i, tip, false);
    }
    return y - rows * cell_h;
}

// ---- Pets: always shown, with an empty state ----

F32 WolfGameConsole::pagePets(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    F32 y = top;
    paw(l + 8.f, y - 9.f, 15.f, p.mAccent);
    F32 x = textL(display(17.f), "Pets", l + 22.f, y, p.mAccent);
    textL(small, g.pets().empty() ? std::string("none yet") : llformat("%d companions", (S32)g.pets().size()), x + 10.f, y - 3.f, p.mDim);
    y -= 30.f;
    if (g.pets().empty())
    {
        return emptyCard(l, r, y, "No pets yet",
                         "Adopt a Wolf Pup or a Raven at a Pet Den in the world. Each pet you own gets a card here with its photograph, "
                         "its name and age, and how hungry and happy it is - feed and look after them.", p);
    }
    const F32 gap = 10.f;
    const S32 cols = (r - l) >= 440.f ? 2 : 1;
    const F32 cw = ((r - l) - gap * (cols - 1)) / cols;
    F32 row_top = y, row_low = y;
    S32 i = 0;
    for (const WolfGame::Pet& pet : g.pets())
    {
        const S32 col = i % cols;
        if (col == 0 && i > 0) { row_top = row_low - gap; }
        const F32 cl = l + col * (cw + gap), ct = row_top;
        // the card's height from what it holds: name, kind and age, a bar per stat
        const F32 text_h = 10.f + (F32)display(16.f)->getLineHeight() + (F32)small->getLineHeight() + 4.f
                         + (F32)statRowHeight(5) * (F32)pet.mStats.size() + 6.f;
        const F32 cb = ct - llmax(text_h, 2.f * 28.f + 20.f);
        card(cl, cb, cl + cw, ct, p, 6.f);
        // the portrait: the bundled photograph for the kit's Wolf Pup and Raven, else the pet's icon
        const F32 pr = 28.f, pcx = cl + 10.f + pr, pcy = ct - 10.f - pr;
        disc(pcx, pcy, pr + 2.f, fade(p.mAccent, 0.6f), 40);
        bool drawn = false;
        if (const char* ph = petPhoto(pet.mType)) drawn = imageDisc(photo(ph), pcx, pcy, pr, LLColor4::white);
        if (!drawn)
        {
            disc(pcx, pcy, pr, p.mBaseBottom, 40);
            icon(pet.mIcon, pcx - pr * 0.72f, pcy - pr * 0.72f, pr * 1.44f, p, initialsOf(pet.mName.empty() ? pet.mTypeLabel : pet.mName), false);
        }
        if (!pet.mAlive) disc(pcx, pcy, pr, LLColor4(0.f, 0.f, 0.f, 0.55f), 40);
        const F32 tl = pcx + pr + 12.f, tr = cl + cw - 10.f;
        F32 ty = ct - 10.f;
        textL(display(16.f), pet.mName.empty() ? pet.mTypeLabel : pet.mName, tl, ty, pet.mAlive ? p.mText : p.mDim, LLFontGL::LEFT, LLFontGL::TOP, tr - tl - 50.f);
        // "Died" as a word, not colour alone (§3.3)
        if (!pet.mAlive) textL(LLFontGL::getFontSansSerifSmallBold(), "Died", tr, ty, DANGER_TEXT, LLFontGL::RIGHT, LLFontGL::TOP);
        ty -= (F32)display(16.f)->getLineHeight();
        textL(small, pet.mTypeLabel + " - " + ageText(pet.ageNow()), tl, ty, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, tr - tl);
        ty -= (F32)small->getLineHeight() + 4.f;
        for (const WolfGame::Stat& s : pet.mStats)
        {
            drawStatRow(s, tl, ty, tr - tl, p, 5);
            ty -= (F32)statRowHeight(5);
        }
        const F32 bottom = cb;
        hit(cl, bottom, cl + cw, ct, H_PET, i, (pet.mName.empty() ? pet.mTypeLabel : pet.mName) + ", " + pet.mTypeLabel + ", " + ageText(pet.ageNow()) + (pet.mAlive ? "" : ", died"), false);
        row_low = llmin(row_low, bottom);
        ++i;
    }
    return row_low - 8.f;
}

// ---- Game: the region's game (§3.3) ----

F32 WolfGameConsole::pageGame(F32 l, F32 r, F32 top, const Palette& p)
{
    WolfGame& g = WolfGame::instance();
    const WolfGame::Game& game = g.game();
    if (!game.mValid)
    {
        return emptyCard(l, r, top, "No game here", "This region isn't running a roleplay game. Game Mode follows you: the next region with a game picks you up.", p);
    }
    F32 y = top;
    const F32 LS = 72.f;
    card(l, y - LS - 12.f, r, y, p, 6.f);
    // the logo, or the portrait's emblem
    if (!game.mLogo.empty()) icon(game.mLogo, l + 6.f, y - 6.f - LS, LS, p);
    else wolfGamePortrait(l + 6.f + LS * 0.5f, y - 6.f - LS * 0.5f, LS * 0.32f, p);
    const F32 tl = l + LS + 18.f;
    textL(display(22.f), game.mName, tl, y - 8.f, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - tl - 8.f);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    std::string sub = game.mKind == "external" ? "Run by the region's own scripts" : "Run by the grid";
    if (!game.mCurrency.empty()) sub += " - money: " + game.mCurrency;
    textL(small, sub, tl, y - 8.f - (F32)display(22.f)->getLineHeight(), p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - tl - 8.f);
    textL(LLFontGL::getFontSansSerifSmallBold(), g.playing() ? "You are playing" : (g.gameMode() || g.forced() ? "You are not playing here" : "Turn Game Mode on to play"),
          tl, y - LS + 6.f, g.playing() ? OK_TEXT : p.mText, LLFontGL::LEFT, LLFontGL::BOTTOM, r - tl - 8.f);
    y -= LS + 24.f;
    if (!game.mDescription.empty())
    {
        y = paragraph(l, r, y, game.mDescription, p.mText) - 10.f;
    }
    if (!game.mRules.empty())
    {
        y = sectionHead(l, r, y, "Rules", p);
        y = paragraph(l, r, y, game.mRules, p.mText) - 10.f;
    }
    if (webUrl(game.mWebsite))
    {
        button(l, y - 28.f, l + 140.f, y, "Website", p, flagsFor(H_WEBSITE, 0));
        hit(l, y - 28.f, l + 140.f, y, H_WEBSITE, 0, "Open the game's website: " + game.mWebsite);
        y -= 36.f;
    }
    return y;
}

// ---- Look: the background picture (Paul: "users can set the background image") ----

F32 WolfGameConsole::pageLook(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    F32 y = sectionHead(l, r, top, "Window background", p);
    // the preview, and what it is
    const F32 pw = llmin(200.f, (r - l) * 0.45f), ph = pw * 9.f / 16.f;
    const F32 strength = WolfGameBackground::opacity();
    LLColor4 tint = WolfGameBackground::tintColour(WolfGameBackground::tint());
    tint.mV[VALPHA] = 1.f;
    chamfer(l, y - ph, l + pw, y, 6.f, p.mBaseTop, p.mBaseBottom);
    bool drawn = false;
    if (WolfGameBackground::source() != WolfGameBackground::SRC_THEME) drawn = imageCover(WolfGameBackground::texture(), l, y - ph, l + pw, y, 6.f, tint);
    if (!drawn)
    {
        if (p.mWolves) drawn = imageCover(photo("WolfGame_Background"), l, y - ph, l + pw, y, 6.f, tint);
        if (!drawn) steelScene(l, y - ph, l + pw, y, 6.f, 1.f);
    }
    chamferEdge(l + 0.5f, y - ph + 0.5f, l + pw - 0.5f, y - 0.5f, 6.f, 1.f, p.mEdge);
    const F32 tl = l + pw + 12.f;
    F32 ty = y;
    textL(bold, WolfGameBackground::describe(), tl, ty, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - tl);
    ty -= (F32)bold->getLineHeight() + 4.f;
    paragraph(tl, r, ty, "Drag a texture from your inventory and drop it anywhere on this window, or choose a picture on your computer.", p.mDim, small);
    y -= ph + 12.f;
    // the two choices; the theme's picture as the reset
    const F32 bw = ((r - l) - 8.f) * 0.5f;
    button(l, y - 30.f, l + bw, y, "Choose a picture file...", p, flagsFor(H_BG_FILE, 0));
    hit(l, y - 30.f, l + bw, y, H_BG_FILE, 0, "Pick a PNG, JPEG, TGA or BMP picture on this computer");
    const bool themed = WolfGameBackground::source() == WolfGameBackground::SRC_THEME;
    button(l + bw + 8.f, y - 30.f, r, y, "Use the theme's picture", p, flagsFor(H_BG_RESET, 0, !themed));
    hit(l + bw + 8.f, y - 30.f, r, y, H_BG_RESET, 0, themed ? "The theme's picture is already in use" : "Go back to the game's own picture", true, !themed);
    y -= 44.f;
    // strength: a drawn slider (arrow keys move it by 5 %)
    textL(bold, caps("Picture strength"), l, y, p.mDim);
    tabularRight(bold, llformat("%d%%", (S32)llround(strength * 100.f)), r, y, p.mText);
    y -= (F32)bold->getLineHeight() + 10.f;
    mSliderL = l + 8.f;
    mSliderR = r - 8.f;
    rect(mSliderL, y - 2.f, mSliderR, y + 2.f, p.mTrack);
    rect(mSliderL, y - 2.f, mSliderL + (mSliderR - mSliderL) * strength, y + 2.f, p.mAccent);
    const F32 kx = mSliderL + (mSliderR - mSliderL) * strength;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    const LLColor4 knob = mHits.isHover(H_BG_OPACITY, 0) || mDraggingSlider ? readable(p.mAccent, 0.7f) : p.mAccent;
    gGL.color4fv(knob.mV);
    gGL.vertex2f(kx, y - 8.f); gGL.vertex2f(kx + 8.f, y); gGL.vertex2f(kx, y + 8.f);
    gGL.vertex2f(kx, y - 8.f); gGL.vertex2f(kx, y + 8.f); gGL.vertex2f(kx - 8.f, y);
    gGL.end();
    if (mKeyboardNav && hasFocus() && mHits.isFocused(H_BG_OPACITY, 0)) focusRing(mSliderL - 6.f, y - 10.f, mSliderR + 6.f, y + 10.f, p);
    hit(mSliderL - 8.f, y - 12.f, mSliderR + 8.f, y + 12.f, H_BG_OPACITY, 0,
        llformat("Picture strength %d%% - drag, or use the Left and Right arrow keys", (S32)llround(strength * 100.f)));
    y -= 24.f;
    // tint: named swatches (the name under each - never colour alone)
    textL(bold, caps("Tint"), l, y, p.mDim);
    y -= (F32)bold->getLineHeight() + 6.f;
    const S32 n = WolfGameBackground::tintCount();
    const F32 sw = ((r - l) - 6.f * (n - 1)) / n;
    for (S32 i = 0; i < n; ++i)
    {
        const F32 sl = l + i * (sw + 6.f);
        const bool on = WolfGameBackground::tint() == i;
        LLColor4 c = WolfGameBackground::tintColour(i);
        c.mV[VALPHA] = 0.9f;
        chamfer(sl, y - 22.f, sl + sw, y, 4.f, c, c);
        if (on) chamferEdge(sl - 2.f, y - 24.f, sl + sw + 2.f, y + 2.f, 5.f, 2.f, p.mAccent);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(H_BG_TINT, i)) focusRing(sl, y - 22.f, sl + sw, y, p);
        textL(small, WolfGameBackground::tintName(i), sl + sw * 0.5f, y - 26.f, on ? p.mText : p.mDim, LLFontGL::HCENTER, LLFontGL::TOP, sw);
        hit(sl, y - 24.f - (F32)small->getLineHeight(), sl + sw, y, H_BG_TINT, i,
            std::string(WolfGameBackground::tintName(i)) + " tint" + (on ? " (in use)" : ""));
    }
    y -= 26.f + (F32)small->getLineHeight() + 12.f;
    return paragraph(l, r, y, "Saved for this account. A dark veil over the picture keeps the text readable, however bright the picture.", p.mDim, small);
}

// ---- Help (the flight deck's HELP pattern: opens by itself the first time, the tab reopens it) ----

F32 WolfGameConsole::pageHelp(F32 l, F32 r, F32 top, const Palette& p)
{
    F32 y = top;
    textL(display(20.f), "How to play", l, y, p.mAccent);
    y -= (F32)display(20.f)->getLineHeight() + 6.f;
    for (const HelpSection& s : HELP)
    {
        y = sectionHead(l, r, y, s.mTitle, p);
        y = paragraph(l, r, y, s.mBody, p.mText) - 12.f;
    }
    return y;
}

// ---- a yes / no question inside the window (never the browser-style alert) ----

void WolfGameConsole::askConfirm(EConfirm what, const std::string& marker_id)
{
    mConfirm = what;
    mConfirmMarker = marker_id;
    // focus on the least destructive answer (W3C APG alertdialog)
    mHits.setFocus(H_CONFIRM_NO, 0);
    mKeyboardNav = true;
    setFocus(true);
}

void WolfGameConsole::drawConfirm(const Palette& p)
{
    // Everything under it stops taking clicks: only this question's buttons count.
    mHits.clear();
    WolfGame& g = WolfGame::instance();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    rect(0.f, 0.f, W, H, LLColor4(0.f, 0.f, 0.f, 0.55f));
    std::string title, body, yes, no;
    if (mConfirm == CONFIRM_LEAVE)
    {
        title = "Leave " + g.game().mName + "?";
        body = "Your stats are kept. Game Mode stays on; you can join again later.";
        yes = "Leave";
        no = "Stay";
    }
    else
    {
        title = "Show on the world map?";
        body = "A script on this region placed this marker on another region. The map shows where; you choose whether to teleport.";
        yes = "Show on map";
        no = "Cancel";
    }
    const F32 cw = llmin(W - 40.f, 380.f);
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const std::vector<std::string> lines = wrap(f, body, cw - 28.f, 6);
    const F32 ch = 16.f + (F32)display(18.f)->getLineHeight() + 6.f + lines.size() * (F32)f->getLineHeight() + 16.f + 30.f + 14.f;
    const F32 cl = (W - cw) * 0.5f, ct = (H + ch) * 0.5f, cb = ct - ch;
    frame(cl, cb, cl + cw, ct, p, 1.f, 10.f);
    F32 y = ct - 16.f;
    textL(display(18.f), title, cl + 14.f, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 28.f);
    y -= (F32)display(18.f)->getLineHeight() + 6.f;
    for (const std::string& line : lines)
    {
        textL(f, line, cl + 14.f, y, p.mDim);
        y -= (F32)f->getLineHeight();
    }
    // the consequential answer on the left, apart from the safe one on the right (NN/G)
    const F32 bw = 120.f, bb = cb + 14.f;
    button(cl + 14.f, bb, cl + 14.f + bw, bb + 30.f, yes, p, flagsFor(H_CONFIRM_YES, 0) | (mConfirm == CONFIRM_LEAVE ? BTN_DANGER : 0));
    mHits.add(cl + 14.f, bb, cl + 14.f + bw, bb + 30.f, H_CONFIRM_YES, 0, yes);
    button(cl + cw - 14.f - bw, bb, cl + cw - 14.f, bb + 30.f, no, p, flagsFor(H_CONFIRM_NO, 0) | BTN_PRIMARY);
    mHits.add(cl + cw - 14.f - bw, bb, cl + cw - 14.f, bb + 30.f, H_CONFIRM_NO, 0, no + " (Escape)");
}

// ---- input ----

bool WolfGameConsole::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h)
    {
        if (mConfirm != CONFIRM_NONE) return true;   // the question is modal to this window
        return false;                                 // the floater: drag, resize
    }
    setFocus(true);
    mKeyboardNav = false;
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    if (h->mId == H_BG_OPACITY)
    {
        mDraggingSlider = true;
        setOpacityFromX(x);
    }
    else if (h->mId == H_SCROLLBAR)
    {
        mDraggingScroll = true;
        mScrollGrabY = (F32)y;
        mScrollGrabStart = mScroll[mPage];
    }
    return true;
}

bool WolfGameConsole::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    // [FIX 2026-10-09] Paul: "none of the tabs work ... the X doesn't work". Read what was pressed
    // BEFORE letting go of the mouse: releasing the capture calls onMouseCaptureLost, which clears
    // the pressed hit and the drags - so every click used to end with nothing pressed.
    const S32 pid = mHits.pressedId(), parg = mHits.pressedArg();
    const bool was_drag = mDraggingSlider || mDraggingScroll;
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    mDraggingSlider = mDraggingScroll = false;
    if (was_drag) return true;
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == pid && h->mArg == parg && h->mEnabled) press(pid, parg);
    return true;
}

void WolfGameConsole::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
    mDraggingSlider = mDraggingScroll = false;
}

bool WolfGameConsole::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (mDraggingSlider) setOpacityFromX(x);
        if (mDraggingScroll && mContentH > 0.f)
        {
            const F32 view_h = (F32)mPageRect.getHeight();
            const F32 max_scroll = llmax(0.f, mContentH - view_h);
            const F32 th = llmax(24.f, view_h * view_h / mContentH);
            const F32 per_px = (view_h - th) > 1.f ? max_scroll / (view_h - th) : 0.f;
            mScroll[mPage] = llclamp(mScrollGrabStart + (mScrollGrabY - (F32)y) * per_px, 0.f, max_scroll);
        }
        gViewerWindow->setCursor(UI_CURSOR_HAND);
        return true;
    }
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    const bool clickable = h->mEnabled && (h->mFocusable || h->mId == H_SCROLLBAR);
    gViewerWindow->setCursor(clickable ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

bool WolfGameConsole::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!mPageRect.pointInRect(x, y)) return false;
    mScroll[mPage] = llmax(0.f, mScroll[mPage] + (F32)clicks * 40.f);   // clamped when drawn
    return true;
}

bool WolfGameConsole::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfGameConsole::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        return true;
    }
    if (mask != MASK_NONE) return false;
    const WolfGameHits::Hit* f = mHits.focused();
    switch (key)
    {
    case KEY_ESCAPE:
        escape();
        return true;
    case KEY_RETURN:
        if (f && f->mEnabled) { mKeyboardNav = true; press(f->mId, f->mArg); }
        return true;
    case KEY_LEFT:
    case KEY_RIGHT:
        mKeyboardNav = true;
        if (f && f->mId == H_BG_OPACITY)
        {
            WolfGameBackground::setOpacity(WolfGameBackground::opacity() + (key == KEY_RIGHT ? 0.05f : -0.05f));
        }
        else if (f && f->mId == H_TAB)
        {
            const S32 next = (f->mArg + (key == KEY_RIGHT ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT;
            showPage((EPage)next);
        }
        else
        {
            mHits.moveFocus(key == KEY_RIGHT);
        }
        return true;
    case KEY_UP:   mScroll[mPage] = llmax(0.f, mScroll[mPage] - 40.f); return true;
    case KEY_DOWN: mScroll[mPage] += 40.f; return true;
    case KEY_PAGE_UP:   mScroll[mPage] = llmax(0.f, mScroll[mPage] - (F32)mPageRect.getHeight() * 0.9f); return true;
    case KEY_PAGE_DOWN: mScroll[mPage] += (F32)mPageRect.getHeight() * 0.9f; return true;
    case KEY_HOME: mScroll[mPage] = 0.f; return true;
    default:
        return false;
    }
}

void WolfGameConsole::escape()
{
    if (mConfirm != CONFIRM_NONE)
    {
        mConfirm = CONFIRM_NONE;
        return;
    }
    if (LLFloater* fl = floater()) fl->closeFloater();
}

bool WolfGameConsole::handleUnicodeCharHere(llwchar uni_char)
{
    if (uni_char != ' ') return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (f && f->mEnabled)
    {
        mKeyboardNav = true;
        press(f->mId, f->mArg);
    }
    return true;
}

bool WolfGameConsole::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                        EAcceptance* accept, std::string& tooltip_msg)
{
    // Source: wolfmapoverlays.cpp dragAndDrop / lltexturectrl.cpp handleDragAndDrop - DAD_TEXTURE
    // cargo is the LLInventoryItem being dragged. Only what is in the player's own inventory (or
    // the library): a texture in an object's contents is not theirs to keep showing.
    if (cargo_type != DAD_TEXTURE) return false;
    *accept = ACCEPT_NO;
    const LLToolDragAndDrop::ESource src = LLToolDragAndDrop::getInstance()->getSource();
    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item || (src != LLToolDragAndDrop::SOURCE_AGENT && src != LLToolDragAndDrop::SOURCE_LIBRARY))
    {
        tooltip_msg = "Drag a texture from your own inventory to use it as the background.";
        return true;
    }
    if (item->getAssetUUID().isNull())
    {
        tooltip_msg = "This texture's picture is not available to the viewer.";
        return true;
    }
    *accept = ACCEPT_YES_COPY_SINGLE;
    tooltip_msg = "Use \"" + item->getName() + "\" as this window's background";
    mDragOverUntil = now() + 0.25;
    if (!drop) return true;
    WolfGameBackground::useTexture(item->getAssetUUID());
    mDragOverUntil = 0.0;
    setStatus("Background set to \"" + item->getName() + "\". Use the Look page to change its strength and tint.", false);
    return true;
}

void WolfGameConsole::setOpacityFromX(S32 x)
{
    if (mSliderR <= mSliderL) return;
    WolfGameBackground::setOpacity(((F32)x - mSliderL) / (mSliderR - mSliderL));
}

// ---- actions ----

void WolfGameConsole::press(S32 id, S32 arg)
{
    WolfGame& g = WolfGame::instance();
    const std::string not_connected = "Not sent: the viewer is not connected to a Wolf Territories region.";
    switch (id)
    {
    case H_CLOSE:
        if (LLFloater* f = floater()) f->closeFloater();
        break;
    case H_GAMEMODE: onGameMode(); break;
    case H_TAB: showPage((EPage)llclamp(arg, 0, (S32)PAGE_COUNT - 1)); break;
    case H_ACTION: onAction(arg); break;
    case H_LEAVE: askConfirm(CONFIRM_LEAVE); break;
    case H_CLEARFX: onClearEffects(); break;
    case H_MARKER_MAP:
        if (arg >= 0 && arg < (S32)mMarkerIds.size()) askConfirm(CONFIRM_MAP, mMarkerIds[arg]);
        break;
    case H_WEBSITE: onWebsite(); break;
    case H_BG_FILE: onChooseFile(); break;
    case H_BG_RESET:
        WolfGameBackground::reset();
        setStatus("Back to the theme's own picture.", false);
        break;
    case H_BG_TINT:
        WolfGameBackground::setTint(arg);
        setStatus(std::string(WolfGameBackground::tintName(arg)) + " tint.", false);
        break;
    case H_CONFIRM_YES:
    {
        const EConfirm what = mConfirm;
        mConfirm = CONFIRM_NONE;
        if (what == CONFIRM_LEAVE)
        {
            if (g.leaveThisGame()) setStatus("Leaving " + g.game().mName + "... your stats are kept.", false);
            else setStatus(g.forced() ? g.forcedText() + "." : "Wait a few seconds before leaving - the region ignores a second change that quickly.", true);
        }
        else if (what == CONFIRM_MAP)
        {
            showMarkerOnMap(mConfirmMarker);
        }
        break;
    }
    case H_CONFIRM_NO: mConfirm = CONFIRM_NONE; break;
    case H_EQUIP:
        if (arg >= 0 && arg < (S32)g.items().size())
        {
            const WolfGame::Item& it = g.items()[arg];
            if (g.equip(it.mName)) setStatus("Equipping " + it.mLabel + "...", false);
            else setStatus(g.equipPending().empty() ? not_connected : std::string("Wait for the last change to finish."), true);
        }
        break;
    case H_UNEQUIP:
    {
        const S32 nw = (S32)WolfGame::weaponSlots().size();
        const std::string slot = arg < nw ? WolfGame::weaponSlots()[arg] : WolfGame::armourSlots()[llclamp(arg - nw, 0, (S32)WolfGame::armourSlots().size() - 1)];
        if (g.unequip(slot)) setStatus("Taking off " + WolfGame::slotLabel(slot) + "...", false);
        else setStatus(g.equipPending().empty() ? not_connected : std::string("Wait for the last change to finish."), true);
        break;
    }
    case H_RELOAD:
        if (arg >= 0 && arg < (S32)WolfGame::weaponSlots().size())
        {
            if (g.reload(WolfGame::weaponSlots()[arg])) setStatus("Reloading " + WolfGame::slotLabel(WolfGame::weaponSlots()[arg]) + "...", false);
            else setStatus(not_connected, true);
        }
        break;
    case H_ABILITY:
        if (arg >= 0 && arg < (S32)g.abilities().size())
        {
            // The answer is the sim's: a cooldown, a cast, or a refusal in the upper third.
            if (!g.useAbility(g.abilities()[arg].mId)) setStatus(not_connected, true);
        }
        break;
    default:
        break;
    }
}

void WolfGameConsole::onGameMode()
{
    WolfGame& g = WolfGame::instance();
    if (g.forced())
    {
        setStatus(g.forcedText() + ". Game Mode stays on here; your own setting applies again on the next region.", false);
        return;
    }
    // §3.2: flipping it sends ["gamemode","1"|"0"] - now, or as soon as the region can take it
    // (WolfGame::requestGameMode); the switch shows the wish, "Connecting...", until it answers.
    const bool on = !g.gameModeShownOn();
    g.requestGameMode(on);
    setStatus("Connecting...", false);
}

void WolfGameConsole::onAction(S32 index)
{
    WolfGame& g = WolfGame::instance();
    if (index < 0 || index >= (S32)g.buttons().size()) return;
    const WolfGame::Button b = g.buttons()[index];
    if (!gAgent.getRegion())
    {
        setStatus("\"" + b.mLabel + "\" was not sent: the viewer is not connected to the region.", true);
        return;
    }
    // §2 buttons: pressing one sends ["button", id]. The sim sends no acknowledgement (it is the
    // script's to answer), so the confirmation is that it left the viewer.
    g.sendButton(b.mId);
    setStatus("\"" + b.mLabel + "\" sent.", false);
}

void WolfGameConsole::onClearEffects()
{
    WolfGame& g = WolfGame::instance();
    bool any = g.downedOverlay();
    for (S32 e = 0; e < WolfGame::FX_COUNT; ++e) any = any || g.effectNow((WolfGame::EEffect)e) > 0.f;
    g.clearEffectsByPlayer();
    setStatus(any ? "Screen effects cleared." : "No screen effects are running.", false);
}

void WolfGameConsole::onWebsite()
{
    const std::string url = WolfGame::instance().game().mWebsite;
    if (webUrl(url)) LLWeb::loadURL(url);   // the viewer's normal URL handling
    else setStatus("This game's website is not a web address the viewer can open.", true);
}

void WolfGameConsole::onChooseFile()
{
    // Source: lltexturectrl.cpp LLFloaterTexturePicker::onBtnAdd - the viewer's own image picker
    // (FFLOAD_IMAGE), answered on the main thread.
    LLHandle<WolfGameConsole> handle = getDerivedHandle<WolfGameConsole>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (handle.isDead() || files.empty() || files.front().empty()) return;
            WolfGameConsole* self = handle.get();
            std::string why;
            if (WolfGameBackground::useFile(files.front(), why))
            {
                self->setStatus("Background set to " + gDirUtilp->getBaseFileName(files.front()) + ".", false);
            }
            else
            {
                self->setStatus(why, true);
            }
        },
        LLFilePicker::FFLOAD_IMAGE, false);
}

void WolfGameConsole::showMarkerOnMap(const std::string& id)
{
    const auto& markers = WolfGame::instance().markers();
    auto it = markers.find(id);
    if (it == markers.end())
    {
        setStatus("That marker has gone.", true);
        return;
    }
    const WolfGame::Marker& m = it->second;
    // The world map finds the region by name and tracks the point; Teleport is on the map.
    LLFloaterWorldMap* map = LLFloaterWorldMap::getInstance();
    if (!map)
    {
        setStatus("The world map is not available.", true);
        return;
    }
    LLFloaterReg::showInstance("world_map", "center");
    LLVector3d global;
    if (WolfGame::markerGlobal(m, global)) map->trackLocation(global);
    else map->trackURL(m.mRegion, (S32)llround(m.mPos.mV[VX]), (S32)llround(m.mPos.mV[VY]), (S32)llround(m.mPos.mV[VZ]));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterRoleplay
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterRoleplay::WolfFloaterRoleplay(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterRoleplay::postBuild()
{
    mConsole = getChild<WolfGameConsole>("console");
    // The console draws the whole window (its own plate, picture and frame); the floater keeps
    // only what floaters do - moving, resizing, layering.
    // [FIX 2026-10-09] Paul: "the x on the game mode boxes doesn't close the window". The console
    // stays IN FRONT of the floater's drag handle. LLFloater::layoutDragHandle makes the drag
    // handle the whole window (llfloater.cpp: rect = getLocalRect()) and LLDragHandle::
    // handleMouseDown takes every click (lldraghandle.cpp: "don't pass on to children", return
    // true), so with the console sent behind it (the old sendChildToBack) no click reached the X,
    // the tabs or any button. In front, a click the console does not use falls through to the
    // drag handle (mouse_opaque="false" in floater_wolf_roleplay.xml), so the window still drags;
    // LLFloater::buildFromFile puts the resize handles in front after postBuild
    // (moveResizeHandlesToFront).
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterRoleplay::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE 2026-10-09] Wolf Roleplay works only on Wolf Territories (wolfgrid.h, the
    // Flight Mode gate): anywhere else the window refuses to open, and says why.
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();   // silently: off Wolf nothing of the game shows, not even a refusal
        return;
    }
    // VIEWER_SPEC.md §1: hello when the Roleplay floater opens.
    WolfGame::instance().sendHello();
    if (mConsole && !sOpenQuietly) mConsole->setFocus(true);
}

bool WolfFloaterRoleplay::sOpenQuietly = false;

void WolfFloaterRoleplay::draw()
{
    // [WOLF GRID GATE 2026-10-09] A hypergrid jump away closes it.
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

void WolfFloaterRoleplay::showPage(WolfGameConsole::EPage page, bool toggle)
{
    if (!WolfGrid::isOnWolfTerritories()) return;
    if (toggle && showingPage(page))
    {
        LLFloaterReg::hideInstance("wolf_roleplay");
        return;
    }
    // From the HUD, mid-play: open without taking the keyboard (the review, 2026-10-09 - the
    // console keeps the arrow keys, Space and Return once it has the focus).
    sOpenQuietly = true;
    WolfFloaterRoleplay* f = LLFloaterReg::showTypedInstance<WolfFloaterRoleplay>("wolf_roleplay");
    sOpenQuietly = false;
    if (f && f->mConsole) f->mConsole->showPage(page);
}

bool WolfFloaterRoleplay::showingPage(WolfGameConsole::EPage page)
{
    WolfFloaterRoleplay* f = LLFloaterReg::findTypedInstance<WolfFloaterRoleplay>("wolf_roleplay");
    return f && f->getVisible() && !f->isMinimized() && f->mConsole && f->mConsole->page() == page;
}

bool WolfFloaterRoleplay::escapeFocused()
{
    WolfFloaterRoleplay* f = LLFloaterReg::findTypedInstance<WolfFloaterRoleplay>("wolf_roleplay");
    if (!f || !f->getVisible() || !f->mConsole || !gFocusMgr.childHasKeyboardFocus(f)) return false;
    f->mConsole->escape();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfGameDialogView - a game dialog's drawn face
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    enum EDialogHit { DH_CLOSE = 1, DH_BUTTON };
    const S32 DIALOG_PER_ROW = 3;   // §3.5 "the buttons in a grid of 3 per row"
}

WolfGameDialogView::WolfGameDialogView(const Params& p)
:   LLUICtrl(p)
{
}

void WolfGameDialogView::setContent(const std::string& title, const std::string& src, bool verified, const std::string& text,
                                    const std::vector<std::string>& buttons, std::function<void(const std::string&)> on_button)
{
    mTitle = title;
    mSrc = src;
    mVerified = verified;
    mText = text;
    mButtons = buttons;
    mOnButton = std::move(on_button);
    // the first answer has the keyboard focus (not modal; nothing is pressed for the player)
    mHits.setFocus(DH_BUTTON, 0);
}

S32 WolfGameDialogView::measure(S32 width) const
{
    return (S32)ceilf(layout((F32)width, 0.f, false));
}

/**
 * Lays the dialog out from the top down; with `draw` it also draws and records the hits.
 * Returns the height it needs. Only the game's own dialogs (verified) wear its theme.
 */
F32 WolfGameDialogView::layout(F32 W, F32 H, bool draw) const
{
    const Palette& p = palette(mVerified);
    const LLFontGL* title_font = display(18.f);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* body = LLFontGL::getFontSansSerif();
    const F32 PAD = 14.f, top = H;
    F32 y = -12.f;   // measured from the top, downward
    const std::vector<std::string> title = wrap(title_font, mTitle, W - 2.f * PAD - 30.f, 2);
    const std::vector<std::string> text = wrap(body, mText, W - 2.f * PAD, 24);
    if (draw)
    {
        mHits.clear();
        frame(0.f, 0.f, W, H, p, 1.f, 10.f);
        const F32 xc = W - PAD - 4.f, yc = top - 16.f;
        closeX(xc, yc, 5.f, mHits.isHover(DH_CLOSE, 0) ? p.mAccent : p.mText);
        mHits.add(xc - 11.f, yc - 11.f, xc + 11.f, yc + 11.f, DH_CLOSE, 0, "Close without answering (Escape)");
    }
    for (const std::string& line : title)
    {
        if (draw) textL(title_font, line, PAD, top + y, mVerified ? p.mAccent : p.mText);
        y -= (F32)title_font->getLineHeight();
    }
    // the source, and "Unverified" (icon + word) when it is not the game's own script
    if (!mSrc.empty() || !mVerified)
    {
        y -= 2.f;
        if (draw)
        {
            F32 sx = PAD;
            if (!mVerified)
            {
                unverifiedMark(sx, top + y, small, &sx);
                sx += 6.f;
            }
            textL(small, mSrc, sx, top + y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, W - PAD - sx);
        }
        y -= (F32)small->getLineHeight();
    }
    y -= 8.f;
    for (const std::string& line : text)
    {
        if (draw) textL(body, line, PAD, top + y, p.mText);
        y -= (F32)body->getLineHeight();
    }
    y -= 12.f;
    const S32 n = (S32)mButtons.size();
    const F32 gap = 6.f, bh = 30.f;
    const F32 bw = (W - 2.f * PAD - gap * (DIALOG_PER_ROW - 1)) / DIALOG_PER_ROW;
    const S32 rows = (n + DIALOG_PER_ROW - 1) / DIALOG_PER_ROW;
    for (S32 i = 0; i < n && draw; ++i)
    {
        const S32 col = i % DIALOG_PER_ROW, row = i / DIALOG_PER_ROW;
        const F32 bl = PAD + col * (bw + gap), bt = top + y - row * (bh + gap);
        U32 flags = 0;
        if (mHits.isHover(DH_BUTTON, i)) flags |= BTN_HOVER;
        if (mHits.isPressed(DH_BUTTON, i)) flags |= BTN_PRESSED;
        if (mKeyboardNav && hasFocus() && mHits.isFocused(DH_BUTTON, i)) flags |= BTN_FOCUS;
        button(bl, bt - bh, bl + bw, bt, mButtons[i], p, flags);
        mHits.add(bl, bt - bh, bl + bw, bt, DH_BUTTON, i, mButtons[i]);
    }
    y -= rows * bh + (rows > 0 ? (rows - 1) * gap : 0.f) + 14.f;
    return -y;
}

void WolfGameDialogView::draw()
{
    LLGLSUIDefault gls_ui;
    layout((F32)getRect().getWidth(), (F32)getRect().getHeight(), true);
    LLUICtrl::draw();
}

void WolfGameDialogView::press(S32 id, S32 arg)
{
    LLFloater* f = getParentByType<LLFloater>();
    if (id == DH_CLOSE)
    {
        if (f) f->closeFloater();   // §3.5: X sends nothing
        return;
    }
    if (id == DH_BUTTON && arg >= 0 && arg < (S32)mButtons.size() && mOnButton)
    {
        const std::string label = mButtons[arg];
        std::function<void(const std::string&)> cb = mOnButton;
        cb(label);   // may close (and so delete) the floater: nothing after this
    }
}

bool WolfGameDialogView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;   // the floater: drag it
    mHits.setPressed(h);
    mKeyboardNav = false;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfGameDialogView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    const S32 id = mHits.pressedId(), arg = mHits.pressedArg();   // before the release clears it (see the console)
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == id && h->mArg == arg) press(id, arg);
    return true;
}

bool WolfGameDialogView::handleHover(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    mHits.setHover(h);
    if (!h) return false;
    gViewerWindow->setCursor(UI_CURSOR_HAND);
    return true;
}

bool WolfGameDialogView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

bool WolfGameDialogView::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        mHits.moveFocus(mask == MASK_NONE);
        return true;
    }
    if (mask != MASK_NONE) return false;
    if (key == KEY_LEFT || key == KEY_RIGHT)
    {
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_RIGHT);
        return true;
    }
    if (key == KEY_RETURN)
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f) press(f->mId, f->mArg);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterGameDialog
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterGameDialog::WolfFloaterGameDialog(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterGameDialog::postBuild()
{
    mView = getChild<WolfGameDialogView>("view");
    // In front of the drag handle, as WolfFloaterRoleplay::postBuild explains (the X and buttons
    // never got a click behind it).
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterGameDialog::draw()
{
    // [WOLF GRID GATE 2026-10-09] Paul: "other grids can't use these interfaces they are only for
    // wolf" - a jump away closes it (WolfGame::clearAll does too; this is the window's own guard).
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

// Source: WolfGameModule.cs Dialog {t,id,title,text,buttons:[label...],src,verified}.
void WolfFloaterGameDialog::showDialog(const LLSD& msg)
{
    const std::string id = msg["id"].asString();
    if (id.empty() || !msg["buttons"].isArray() || msg["buttons"].size() == 0) return;
    // At most MAX_DIALOGS open: a script looping wolfGameDialog cannot bury the screen; the oldest
    // goes (WolfStorm review).
    static std::deque<std::string> s_order;
    s_order.erase(std::remove_if(s_order.begin(), s_order.end(),
                                 [](const std::string& k) { return !LLFloaterReg::findInstance("wolf_game_dialog", LLSD(k)); }),
                  s_order.end());
    while (s_order.size() >= MAX_DIALOGS)
    {
        if (LLFloater* old = LLFloaterReg::findInstance("wolf_game_dialog", LLSD(s_order.front()))) old->closeFloater();
        s_order.pop_front();
    }
    WolfFloaterGameDialog* f = LLFloaterReg::getTypedInstance<WolfFloaterGameDialog>("wolf_game_dialog", LLSD(id));
    if (!f) return;
    // the same id again replaces its dialog: one place in the order, at the newest end
    s_order.erase(std::remove(s_order.begin(), s_order.end(), id), s_order.end());
    s_order.push_back(id);
    f->build(msg);
    f->setOpenPositioning(LLFloaterEnums::POSITIONING_SPECIFIED);
    // Not modal and never takes the focus: like llDialog (LLScriptFloater::show, EXT-5445).
    LLFloaterReg::showTypedInstance<WolfFloaterGameDialog>("wolf_game_dialog", LLSD(id), false);
    f->place();
}

void WolfFloaterGameDialog::build(const LLSD& msg)
{
    mDialogId = msg["id"].asString();
    const std::string title = msg["title"].asString();
    const std::string src = msg["src"].asString();
    // [SECURITY 2026-10-09] WolfGameModule.cs Dialog {.., verified}: 0 = not the game's own script.
    const bool verified = !msg.has("verified") || msg["verified"].asInteger() != 0;
    setTitle(title.empty() ? src : title);   // the floater list's name for it
    std::vector<std::string> buttons;
    const LLSD& b = msg["buttons"];
    for (S32 i = 0; i < llmin((S32)b.size(), MAX_DIALOG_BUTTONS); ++i) buttons.push_back(b[i].asString());
    const std::string dialog_id = mDialogId;
    mView->setContent(title.empty() ? std::string("Game") : title, src, verified, msg["text"].asString(), buttons,
                      [this, dialog_id](const std::string& label)
                      {
                          // §2 dialog: pressing a button sends ["dialog", id, label].
                          WolfGame::instance().sendDialog(dialog_id, label);
                          closeFloater();
                      });
    // The floater's height follows the content.
    const S32 w = getRect().getWidth();
    const S32 h = llclamp(mView->measure(w), 120, gViewerWindow ? llmax(120, gViewerWindow->getWorldViewHeightScaled() - 80) : 600);
    reshape(w, h);
    mView->setRect(LLRect(0, h, w, 0));
}

void WolfFloaterGameDialog::place()
{
    // Source: llscriptfloater.cpp LLScriptFloater::show POS_TOP_RIGHT, then down past every
    // script dialog and game dialog already in that column (§3.5 "stacking under any other
    // llDialogs").
    const S32 top_pad = LLScriptFloaterManager::instance().getTopPad();
    const S32 w = getRect().getWidth(), h = getRect().getHeight();
    const S32 left = gViewerWindow->getWorldViewWidthScaled() - w - rightPad();
    S32 top = gViewerWindow->getWorldViewHeightScaled() - top_pad;
    std::vector<LLFloater*> others;
    for (LLFloater* f : LLFloaterReg::getFloaterList("script_floater")) others.push_back(f);
    for (LLFloater* f : LLFloaterReg::getFloaterList("wolf_game_dialog")) if (f != this) others.push_back(f);
    for (S32 pass = 0; pass < 16; ++pass)
    {
        bool moved = false;
        for (LLFloater* f : others)
        {
            if (!f || !f->getVisible() || f->isMinimized()) continue;
            const LLRect fr = f->getRect();
            const bool overlaps = fr.mRight > left && fr.mLeft < left + w && fr.mBottom < top && fr.mTop > top - h;
            if (overlaps)
            {
                top = fr.mBottom - 4;
                moved = true;
            }
        }
        if (!moved) break;
    }
    top = llmax(top, h);
    LLRect r;
    r.setLeftTopAndSize(left, top, w, h);
    setRect(r);
}

void WolfFloaterGameDialog::closeAll()
{
    std::vector<LLFloater*> list;
    for (LLFloater* f : LLFloaterReg::getFloaterList("wolf_game_dialog")) list.push_back(f);
    for (LLFloater* f : list) f->closeFloater();
}

bool WolfFloaterGameDialog::closeFocused()
{
    for (LLFloater* f : LLFloaterReg::getFloaterList("wolf_game_dialog"))
    {
        if (f && f->getVisible() && gFocusMgr.childHasKeyboardFocus(f))
        {
            f->closeFloater();   // §3.5: Escape sends nothing
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterGameWeb
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterGameWeb::WolfFloaterGameWeb(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterGameWeb::postBuild()
{
    mBrowser = getChild<LLMediaCtrl>("browser");
    mBrowser->setWolfNoSLURL(true);   // [SECURITY 2026-10-09] a game's page never dispatches SLURLs
    mHostLabel = findChild<LLTextBox>("host_label");
    if (LLButton* close = findChild<LLButton>("close_overlay"))
    {
        close->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    }
    return true;
}

void WolfFloaterGameWeb::draw()
{
    // [WOLF GRID GATE 2026-10-09] Paul: "other grids can't use these interfaces they are only for
    // wolf" - a jump away closes it (WolfGame::clearAll does too; this is the window's own guard).
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

void WolfFloaterGameWeb::onClose(bool app_quitting)
{
    // A hidden single-instance floater keeps its browser: stop the page (and its sound) now.
    if (mBrowser && !app_quitting) mBrowser->navigateTo("about:blank");
}

U32 WolfFloaterGameWeb::sPageSerial = 0;

void WolfFloaterGameWeb::showPage(const std::string& url, const std::string& mode, S32 w, S32 h, const std::string& src,
                                  bool verified)
{
    const U32 serial = ++sPageSerial;   // §2 web: a second web replaces the first, prompt or page
    LLFloaterReg::hideInstance("wolf_game_web_prompt");
    std::string host;
    if (!WolfGame::validWebUrl(url, host)) return;   // [SECURITY] checked again here, before any use
    if (verified)
    {
        openPage(url, mode, w, h, src, true);
        return;
    }
    // [SECURITY 2026-10-09] verified 0 - not the game's own script: nothing loads until the player
    // says Open, in a small drawn prompt (no link parsing). The real host first.
    // The sim refuses unverified overlays; one arriving anyway is shown as a panel.
    const std::string safe_mode = mode == "overlay" ? std::string("panel") : mode;
    WolfFloaterGameWebPrompt::ask(host, src, [url, safe_mode, w, h, src, serial]()
    {
        if (serial != sPageSerial) return;   // replaced or closed by the script since
        openPage(url, safe_mode, w, h, src, false);
    });
}

void WolfFloaterGameWeb::openPage(const std::string& url, const std::string& mode_in, S32 w, S32 h, const std::string& src,
                                  bool verified)
{
    // WolfStorm review: the page's REAL host is always the main label, the script's name only
    // second, so a region cannot open a page titled "Wolf Territories login". [SECURITY] The host
    // is the validated one (WolfGame::validWebUrl), never LLURI's own reading.
    std::string host;
    if (!WolfGame::validWebUrl(url, host)) return;
    const std::string from = src.empty() ? std::string() : (verified ? src : src + ", unverified");
    const std::string label = from.empty() ? (verified ? host : host + "  (unverified)") : host + "  (opened by " + from + ")";
    closeWindows();
    const std::string mode = (mode_in == "overlay" && verified) || mode_in == "window" ? mode_in : std::string("panel");
    const bool overlay = mode == "overlay";
    const char* name = overlay ? "wolf_game_web_overlay" : "wolf_game_web";
    WolfFloaterGameWeb* f = LLFloaterReg::getTypedInstance<WolfFloaterGameWeb>(name);
    if (!f || !f->mBrowser) return;
    f->mMode = mode;
    const LLRect view = gFloaterView ? gFloaterView->getLocalRect() : LLRect(0, 768, 1024, 0);
    const S32 top_pad = LLScriptFloaterManager::instance().getTopPad();
    f->setOpenPositioning(LLFloaterEnums::POSITIONING_SPECIFIED);
    LLRect r;
    if (overlay)
    {
        r = view;   // §3.8: over the whole world view
        f->setCanDrag(false);
        if (f->mHostLabel)
        {
            f->mHostLabel->setText(label);
        }
    }
    else
    {
        // §3.8: w x h, default 420 x 600; held inside the view.
        const S32 fw = llclamp(w > 0 ? w : 420, 200, llmax(200, view.getWidth() - 20));
        const S32 fh = llclamp(h > 0 ? h : 600, 160, llmax(160, view.getHeight() - top_pad - 20));
        if (mode == "panel")
        {
            // Docked on the right, under the navigation bar.
            r.setLeftTopAndSize(view.mRight - fw - rightPad(), view.mTop - top_pad, fw, fh);
            f->setCanDrag(false);
        }
        else
        {
            r.setLeftTopAndSize(view.getCenterX() - fw / 2, view.getCenterY() + fh / 2, fw, fh);
            f->setCanDrag(true);
        }
        f->setTitle(label);   // §3.8 'the title "<src>"' - after the real host (see above)
    }
    f->setShape(r);
    LLFloaterReg::showInstance(name, LLSD(), overlay);
    f->setRect(r);
    f->mBrowser->navigateTo(url);
}

void WolfFloaterGameWeb::closePage()
{
    ++sPageSerial;   // a prompt still on screen no longer opens anything
    LLFloaterReg::hideInstance("wolf_game_web_prompt");
    closeWindows();
}

void WolfFloaterGameWeb::closeWindows()
{
    for (const char* name : { "wolf_game_web", "wolf_game_web_overlay" })
    {
        LLFloater* f = LLFloaterReg::findInstance(name);
        if (f && f->getVisible()) f->closeFloater();
    }
}

bool WolfFloaterGameWeb::closeOverlayIfFocusAllows()
{
    LLFloater* f = LLFloaterReg::findInstance("wolf_game_web_overlay");
    if (!f || !f->getVisible()) return false;
    if (gFocusMgr.getKeyboardFocus() && !gFocusMgr.childHasKeyboardFocus(f)) return false;
    f->closeFloater();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterGameWebPrompt  [SECURITY 2026-10-09]
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterGameWebPrompt::WolfFloaterGameWebPrompt(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterGameWebPrompt::postBuild()
{
    mView = getChild<WolfGameDialogView>("view");
    // In front of the drag handle, as WolfFloaterRoleplay::postBuild explains.
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterGameWebPrompt::draw()
{
    // [WOLF GRID GATE 2026-10-09] Paul: "other grids can't use these interfaces they are only for
    // wolf" - a jump away closes it (WolfGame::clearAll does too; this is the window's own guard).
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}

bool WolfFloaterGameWebPrompt::closeFocused()
{
    LLFloater* f = LLFloaterReg::findInstance("wolf_game_web_prompt");
    if (!f || !f->getVisible() || !gFocusMgr.childHasKeyboardFocus(f)) return false;
    if (WolfFloaterGameWebPrompt* p = dynamic_cast<WolfFloaterGameWebPrompt*>(f)) p->mOpen = nullptr;
    f->closeFloater();   // as "Not now": nothing opens
    return true;
}

void WolfFloaterGameWebPrompt::ask(const std::string& host, const std::string& src, std::function<void()> open)
{
    WolfFloaterGameWebPrompt* f = LLFloaterReg::getTypedInstance<WolfFloaterGameWebPrompt>("wolf_game_web_prompt");
    if (!f || !f->mView) return;
    f->mOpen = std::move(open);
    // Drawn as plain text: host and source are shown, never linked. Always the plain palette
    // with the Unverified mark (the page is not from the game's own script).
    const std::string who = src.empty() ? std::string("a script") : src;
    f->mView->setContent("Open a page?", "from " + who, false,
                         host + " (from " + who + ", unverified) wants to open a page. Nothing loads unless you choose Open.",
                         { "Open", "Not now" },
                         [f](const std::string& label)
                         {
                             std::function<void()> open = label == "Open" ? f->mOpen : std::function<void()>();
                             f->mOpen = nullptr;
                             f->closeFloater();
                             if (open) open();
                         });
    const S32 w = f->getRect().getWidth();
    const S32 h = llmax(120, f->mView->measure(w));
    f->reshape(w, h);
    f->mView->setRect(LLRect(0, h, w, 0));
    f->setOpenPositioning(LLFloaterEnums::POSITIONING_SPECIFIED);
    LLFloaterReg::showInstance("wolf_game_web_prompt", LLSD(), false);
    // Top-centre, under the navigation bar.
    const LLRect view = gFloaterView ? gFloaterView->getLocalRect() : LLRect(0, 768, 1024, 0);
    LLRect r = f->getRect();
    r.setLeftTopAndSize(view.getCenterX() - r.getWidth() / 2, view.mTop - LLScriptFloaterManager::instance().getTopPad() - 6,
                        r.getWidth(), r.getHeight());
    f->setRect(r);
}
