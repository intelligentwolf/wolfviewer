/**
 * @file wolffloaternpc.cpp
 * @brief WolfViewer: Wolf World > WolfNPC... See wolffloaternpc.h.
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

#include "wolffloaternpc.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "apr_base64.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lldraghandle.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llimage.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llradiogroup.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"
#include "lllocalcliprect.h"
#include "llscrollbar.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdutil.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "llfloaterwolfavatars.h"
#include "llagentui.h"
#include "wolffloateradventuregm.h"
#include "wolffloaternpcgame.h"
#include "wolffloaternpcmind.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llweb.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "wolfadventure.h"
#include "wolfgame.h"
#include "wolfgrid.h"
#include "wolfnpc.h"
#include "wolfpaint.h"

// Every op, field, range and word: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md (cited by section below).
// The brain calls (section 5: npc_avatars, npc_picture, npc_ai_info, npc_scenes) go through WolfAdventure::api, as the
// WolfAvatars window's do (llfloaterwolfavatars.cpp loadAvatars / loadPicture: LLHandle + serial guards).

namespace
{
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    F64 nowSec() { return LLFrameTimer::getTotalSeconds(); }

    /**
     * A number for the region's JSON: an integral value goes as a JSON integer, anything else as a real (boost::json writes
     * a double as 2E0, which a reader expecting an integer may refuse; an integer is read as a double by any reader).
     */
    LLSD wireNum(F64 v)
    {
        if (!std::isfinite(v)) return LLSD(0);
        const F64 r = std::round(v);
        if (std::fabs(v - r) < 1e-9 && std::fabs(r) < 2.0e9) return LLSD((LLSD::Integer)(S32)r);
        return LLSD(v);
    }

    /** A position as [x,y,z], two decimals (PROTOCOL.md section 3 "home": [x,y,z]). */
    LLSD wireVec(const LLVector3& p)
    {
        LLSD a = LLSD::emptyArray();
        for (S32 i = 0; i < 3; ++i) a.append(wireNum(std::round((F64)p.mV[i] * 100.0) / 100.0));
        return a;
    }

    bool readVec(const LLSD& a, LLVector3& out)
    {
        if (!a.isArray() || a.size() < 3) return false;
        for (S32 i = 0; i < 3; ++i)
        {
            const LLSD& v = a[i];
            if (!(v.isReal() || v.isInteger())) return false;
            const F64 d = v.asReal();
            if (!std::isfinite(d)) return false;
            out.mV[i] = (F32)d;
        }
        return true;
    }

    std::string vecText(const LLVector3& p) { return llformat("%.0f, %.0f, %.0f", p.mV[VX], p.mV[VY], p.mV[VZ]); }

    /** "about N credits": the brain's prices are small (npc_ai_info perReply about 0.017, perVoiceReply about 1.467), so
     *  under 1 shows three decimals, under 10 two, else one; trailing zeros go ("0.017", "1.47", "12", "250"). */
    std::string creditsText(const LLSD& v)
    {
        const F64 d = v.asReal();
        if (!std::isfinite(d)) return "?";
        const F64 a = std::fabs(d);
        std::string s = llformat(a < 1.0 ? "%.3f" : a < 10.0 ? "%.2f" : "%.1f", d);
        if (s.find('.') != std::string::npos)
        {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        if (s.empty() || s == "-0") s = "0";
        return s;
    }

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

    // Source: llfloaterwolfavatars.cpp:692-696 buyUrlOf - a credits_short refusal carries its buy link
    std::string buyUrlOf(const LLSD& reply)
    {
        if (!reply.isMap() || reply["code"].asString() != "credits_short") return std::string();
        return reply["buyurl"].asString();
    }

    // Source: llfloaterwolfavatars.cpp:67-70 - a picture's base64 refused above 4 MB and its sides above 2048 px
    const size_t PICTURE_B64_MAX = 4u * 1024u * 1024u;
    const S32 PICTURE_SIDE_MAX = 2048;

    // Source: llfloaterwolfavatars.cpp:705-732 pictureTexture - npc_picture {mime: "image/jpeg", data: base64} (PROTOCOL.md
    // section 5) -> a texture, the same way: apr_base64_decode_binary, LLImageFormatted::loadFromMemory + decode,
    // LLViewerTextureManager::getLocalTexture
    bool pictureTexture(const std::string& mime, const std::string& b64, LLPointer<LLViewerTexture>& out, std::string& why)
    {
        out = nullptr;
        if (mime != "image/jpeg") { why = "the picture came in a format WolfViewer does not show"; return false; }
        if (b64.empty() || b64.size() > PICTURE_B64_MAX) { why = "the picture was empty or too large"; return false; }
        const int max_len = apr_base64_decode_len(b64.c_str());
        if (max_len <= 0) { why = "the picture could not be read"; return false; }
        std::vector<U8> bytes((size_t)max_len);
        const int len = apr_base64_decode_binary(bytes.data(), b64.c_str());
        if (len <= 0 || len > max_len) { why = "the picture could not be read"; return false; }
        bytes.resize((size_t)len);
        LLPointer<LLImageFormatted> img = LLImageFormatted::loadFromMemory(bytes.data(), (U32)bytes.size(), mime);
        if (img.isNull()) { why = "the picture is not a JPEG WolfViewer can read"; return false; }
        if (img->getWidth() <= 0 || img->getHeight() <= 0 || img->getWidth() > PICTURE_SIDE_MAX || img->getHeight() > PICTURE_SIDE_MAX)
        {
            why = "the picture has an unexpected size";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        if (!img->decode(raw, 0.f) || raw->isBufferInvalid() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        {
            why = "the picture could not be decoded";
            return false;
        }
        out = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        if (out.isNull()) { why = "the picture could not be put on screen"; return false; }
        return true;
    }

    // Source: wolfvehiclerez.cpp:178-183 rlvBlocksRez (lltooldraganddrop.cpp dropObject): no rez while RLV @rez or @interact
    bool rlvBlocksRez()
    {
        return RlvActions::isRlvEnabled() && (gRlvHandler.hasBehaviour(RLV_BHVR_REZ) || gRlvHandler.hasBehaviour(RLV_BHVR_INTERACT));
    }

    struct Choice { const char* value; const char* label; };

    // Source: PROTOCOL.md section 4.1 - the presets (behaviour values) and what each does
    const Choice ANIMAL_BEHAVIOURS[] = {
        { "roam", "Roams" }, { "stay", "Stays put" }, { "guard", "Guards" }, { "follow", "Follows" },
        { "predator", "Predator (hunts game players)" }, { "pet", "Pet (follows me)" } };
    const Choice PERSON_BEHAVIOURS[] = {
        { "stroll", "Strolls" }, { "stay", "Stays put" }, { "guard", "Guards" }, { "follow", "Follows" },
        { "work", "Works (job)" }, { "zombie", "Zombie" } };
    std::string behaviourNote(const std::string& b, bool animal)
    {
        if (b == "roam") return "Wanders its area: sniffs, scratches, lies down, stretches, howls at night, comes to look at curious visitors and backs off if crowded.";
        if (b == "stay") return "Stays at its home.";
        if (b == "guard") return animal ? "Patrols its area and faces and growls at strangers." : "Patrols its area and faces strangers.";
        if (b == "follow") return "Follows you, its owner.";
        if (b == "predator") return "Roams, and hunts players of the region's game; a pack hunts together. Turn on the Game section.";
        if (b == "stroll") return "Strolls and looks around, sits on free seats and waves at people.";
        if (b == "work") return "Does its job (Work and routine) at its job spot.";
        if (b == "pet") return "Your pet: it follows you at about 2.5 m, sits when you sit and lies down when you stand still. Not Game Mode: anyone sees it.";
        if (b == "zombie") return "Shambles, groans and wanders as a horde, and hunts players of the region's game, slowly.";
        return std::string();
    }
    // Source: PROTOCOL.md section 3 area.mode, indoors, visitors, work
    const Choice AREA_MODES[] = { { "radius", "A circle round its home" }, { "parcel", "Its home's parcel" }, { "region", "The whole region" } };
    const Choice INDOORS[] = { { "never", "Never goes indoors" }, { "allowed", "May go indoors" }, { "area", "Indoors only inside its area" } };
    const Choice VISITORS[] = { { "friendly", "Friendly" }, { "ignore", "Ignores them" }, { "wary", "Wary" }, { "hostile", "Hostile" } };
    const Choice WORKS[] = { { "", "None" }, { "shopkeeper", "Shopkeeper" }, { "guard", "Guard" }, { "bartender", "Bartender" },
                             { "dancer", "Dancer" }, { "musician", "Musician" }, { "wanderer", "Wanderer" } };
    // Source: PROTOCOL.md section 3 routine do: goto|work|home|sit|roam|rest ("goto" is a place name in the line)
    const char* const ROUTINE_DOS[] = { "work", "home", "sit", "roam", "rest" };

    // Source: PROTOCOL.md section 6 when / who / do / once
    const Choice WHENS[] = { { "near", "comes near" }, { "leave", "leaves" } };
    const Choice WHOS[] = {
        { "anyone", "Anyone" }, { "npc", "Any NPC" }, { "anything", "Anything (a person or an NPC)" }, { "owner", "Owner (you)" },
        { "group", "A member of its group" }, { "friend", "A friend of mine" }, { "stranger", "A stranger" },
        { "player", "Playing the game" }, { "notplayer", "Not playing the game" }, { "attacker", "Attacked me" },
        { "name", "Name contains... (* ? wildcards)" }, { "key", "A particular person" },
        { "kind", "A kind (wolf, horse, person...)" }, { "pack", "Pack..." }, { "groupname", "Group..." }, { "tag", "Tag..." } };
    const Choice ACTIONS[] = {
        { "anim", "Play animation" }, { "loop", "Loop animation" }, { "stop", "Stop animation" }, { "sound", "Play sound" },
        { "say", "Say" }, { "face", "Face them" }, { "follow", "Follow" }, { "approach", "Approach" }, { "flee", "Flee" },
        { "ai", "AI line" }, { "intent", "Start: intention" } };
    const Choice OFTENS[] = { { "", "Every time" }, { "visit", "Once per visit" }, { "day", "Once a day" }, { "ever", "Once ever" } };
    // Source: PROTOCOL.md section 4.1 - the intentions the engine executes
    const char* const INTENTIONS[] = { "roam", "rest", "howl", "guard", "sit", "work", "home", "follow", "approach", "flee",
                                       "hunt", "circle", "talk", "goto" };
    // Source: PROTOCOL.md section 6 "up to 16" reactions, "do (up to 6)", m 1-96, cooldown 0-86400, gap 1-1440
    const S32 MAX_RULES = 16, MAX_DO = 6;
    // Source: PROTOCOL.md section 3 - places up to 16, routine up to 12, name 1-48, pack 0-24, place names 1-24
    const size_t MAX_ROUTINE = 12;   // places: wolffloaternpcmind.cpp MAX_PLACES
    // Source: PROTOCOL.md section 10 "at `speed` (at most 20 m/s)"
    // Source: PROTOCOL.md section 8 - built-in animation names per kind (not the tail_* set)
    const char* const ANIMAL_ANIMS[] = { "animal_stand", "animal_walk", "animal_run", "animal_sit", "animal_lie", "avatar_talk",
                                         "wolfnpc_sniff", "wolfnpc_howl", "wolfnpc_stretch", "wolfnpc_scratch", "wolfnpc_bite", "wolfnpc_die" };
    const char* const PERSON_ANIMS[] = { "wolfnpc_shamble", "wolfnpc_zombie_idle", "wolfnpc_strike", "wolfnpc_die" };
    // Source: PROTOCOL.md section 8 "default avatar animations by name" - OpenSim bin/data/avataranimations.xml names (the
    // gestures and poses; walking, flying and aiming ones are left out: the engine moves it)
    const char* const AVATAR_ANIMS[] = {
        "ANGRY_FINGERWAG", "ANGRY_TANTRUM", "AWAY", "BACKFLIP", "BLOWKISS", "BOW", "BRUSH", "BUSY", "CLAP", "COURTBOW", "CROUCH",
        "DANCE1", "DANCE2", "DANCE3", "DANCE4", "DANCE5", "DANCE6", "DANCE7", "DANCE8", "DEAD", "DRINK", "EXPRESS_LAUGH",
        "EXPRESS_SMILE", "EXPRESS_SHRUG", "EXPRESS_WINK", "FALLDOWN", "FIST_PUMP", "HELLO", "IMPATIENT", "JUMPFORJOY",
        "KICK_ROUNDHOUSE_R", "KISSMYBUTT", "LAUGH_SHORT", "MUSCLEBEACH", "NO_HEAD", "NO_UNHAPPY", "NYANYA", "PEACE", "POINT_ME",
        "POINT_YOU", "PUNCH_L", "PUNCH_ONETWO", "PUNCH_R", "SALUTE", "SHOUT", "SIT", "SIT_FEMALE", "SIT_GROUND", "SLEEP",
        "SMOKE_IDLE", "SNAPSHOT", "STRETCH", "SWORD_STRIKE_R", "TALK", "THROW_R", "TYPE", "WHISTLE", "WINK_HOLLYWOOD",
        "YES_HAPPY", "YES_HEAD", "YOGA_FLOAT" };

    template <size_t N>
    void fillCombo(LLComboBox* c, const Choice (&list)[N])
    {
        c->removeall();
        for (const Choice& ch : list) c->add(ch.label, LLSD(ch.value));
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

    template <size_t N>
    bool known(const Choice (&list)[N], const std::string& value)
    {
        for (const Choice& ch : list)
        {
            if (value == ch.value) return true;
        }
        return false;
    }

    /** Select `value` in a combo when it is one of its items, else the first item. */
    void selectOr(LLComboBox* c, const std::string& value)
    {
        if (!c->selectByValue(LLSD(value)) && c->getItemCount() > 0) c->selectFirstItem();
    }

    // [ROUND 4 LOOK] the window paints its own scene (WolfNPCBackdrop), so its words have fixed colours no skin can lower.
    // Contrast (WCAG relative luminance) on the body plate (#090d10 at 92% over the scene, about #0a0e11):
    //   NPC_CREAM #f2ebd6 16.3:1, NPC_MUTED #ccd1db 12.8:1, NPC_ERR #ffb385 11.1:1 (on the error plate #2e0d0f 10.2:1)
    const LLColor4 NPC_CREAM(0.95f, 0.92f, 0.84f, 1.f);
    const LLColor4 NPC_MUTED(0.80f, 0.82f, 0.86f, 1.f);
    const LLColor4 NPC_ERR(1.f, 0.70f, 0.52f, 1.f);
    LLUIColor caution() { return LLUIColor(NPC_ERR); }
    LLUIColor normalText() { return LLUIColor(NPC_CREAM); }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// The avatar pictures (step 1)
// ════════════════════════════════════════════════════════════════════════════════════════════════════

/**
 * The resident's WolfAvatars as tiles: the picture (npc_picture, 256 px) over the name and the kind. Click, or the arrow
 * keys and Enter with the keyboard on it (a tab stop), chooses one; the chosen one has a thick border AND the word "Chosen" on it
 * (never colour alone). Created in code (not registered), with its own Params.
 */
class WolfNPCAvatarGrid : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    struct Item
    {
        std::string mJob, mName, mKind, mSpecies, mWhy;
        LLPointer<LLViewerTexture> mTexture;
        bool mLoading = false;
    };

    static const S32 TILE_W = 108, TILE_H = 132, PIC_SIDE = 92;

    explicit WolfNPCAvatarGrid(const Params& p) : LLUICtrl(p) {}

    std::vector<Item> mItems;
    S32 mSelected = -1;
    std::function<void()> mOnPick;

    S32 columns() const { return llmax(1, getRect().getWidth() / TILE_W); }
    S32 neededHeight() const
    {
        const S32 cols = columns();
        const S32 rows = ((S32)mItems.size() + cols - 1) / cols;
        return rows * TILE_H;
    }

    S32 indexAt(S32 x, S32 y) const
    {
        const S32 cols = columns();
        const S32 c = x / TILE_W, r = (getRect().getHeight() - y) / TILE_H;
        if (x < 0 || c >= cols || r < 0) return -1;
        const S32 i = r * cols + c;
        return i < (S32)mItems.size() ? i : -1;
    }

    void pick(S32 i)
    {
        if (i < 0 || i >= (S32)mItems.size()) return;
        mSelected = i;
        if (mOnPick) mOnPick();
    }

    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        setFocus(true);
        pick(indexAt(x, y));
        return true;
    }

    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (mItems.empty() || mask != MASK_NONE) return LLUICtrl::handleKeyHere(key, mask);
        const S32 cols = columns();
        S32 i = mSelected < 0 ? 0 : mSelected;
        if (key == KEY_RIGHT) i = llmin(i + 1, (S32)mItems.size() - 1);
        else if (key == KEY_LEFT) i = llmax(i - 1, 0);
        else if (key == KEY_DOWN) i = llmin(i + cols, (S32)mItems.size() - 1);
        else if (key == KEY_UP) i = llmax(i - cols, 0);
        else if (key == KEY_RETURN || key == ' ') { pick(i); return true; }   // Enter / Space choose it
        else return LLUICtrl::handleKeyHere(key, mask);
        mSelected = i;   // the arrows only move (choosing goes on to the next step)
        return true;
    }

    void draw() override
    {
        const S32 cols = columns();
        const S32 h = getRect().getHeight();
        const LLColor4 ink = normalText().get();
        const LLColor4 accent = LLUIColorTable::instance().getColor("EmphasisColor").get();
        const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
        for (S32 i = 0; i < (S32)mItems.size(); ++i)
        {
            const Item& it = mItems[(size_t)i];
            const S32 left = (i % cols) * TILE_W, top = h - (i / cols) * TILE_H;
            const LLRect tile(left + 3, top - 3, left + TILE_W - 3, top - TILE_H + 3);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(tile, LLColor4(0.f, 0.f, 0.f, 0.25f), true);
            const LLRect pic(tile.getCenterX() - PIC_SIDE / 2, tile.mTop - 4, tile.getCenterX() + PIC_SIDE / 2, tile.mTop - 4 - PIC_SIDE);
            if (it.mTexture.notNull() && it.mTexture->getWidth() > 0 && it.mTexture->getHeight() > 0)
            {
                // Source: llfloaterwolfavatars.cpp:735-743 drawPictureIn - scaled to fit, centred
                const F32 scale = llmin((F32)pic.getWidth() / (F32)it.mTexture->getWidth(), (F32)pic.getHeight() / (F32)it.mTexture->getHeight());
                const S32 tw = ll_round((F32)it.mTexture->getWidth() * scale), th = ll_round((F32)it.mTexture->getHeight() * scale);
                gl_draw_scaled_image(pic.getCenterX() - tw / 2, pic.getCenterY() - th / 2, tw, th, it.mTexture.get());
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            else
            {
                gl_rect_2d(pic, LLColor4(0.f, 0.f, 0.f, 0.35f), true);
                const std::string words = it.mLoading ? "Loading..." : (it.mWhy.empty() ? "No picture" : "Picture not shown");
                font->renderUTF8(words, 0, (F32)pic.getCenterX(), (F32)pic.getCenterY(), ink, LLFontGL::HCENTER, LLFontGL::VCENTER,
                                 LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, pic.getWidth() - 4, nullptr, true);
            }
            font->renderUTF8(it.mName, 0, (F32)tile.getCenterX(), (F32)(pic.mBottom - 9), ink, LLFontGL::HCENTER, LLFontGL::VCENTER,
                             i == mSelected ? LLFontGL::BOLD : LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, tile.getWidth() - 6, nullptr, true);
            const std::string kind = it.mKind == "animal" ? (it.mSpecies.empty() ? std::string("Animal") : "Animal: " + it.mSpecies)
                                                          : std::string("Person");
            font->renderUTF8(kind, 0, (F32)tile.getCenterX(), (F32)(pic.mBottom - 23), ink, LLFontGL::HCENTER, LLFontGL::VCENTER,
                             LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, tile.getWidth() - 6, nullptr, true);
            if (i == mSelected)
            {
                gl_rect_2d(tile, accent, false);
                gl_rect_2d(LLRect(tile.mLeft + 1, tile.mTop - 1, tile.mRight - 1, tile.mBottom + 1), accent, false);
                gl_rect_2d(LLRect(tile.mLeft + 2, tile.mTop - 2, tile.mRight - 2, tile.mBottom + 2), accent, false);
                // the word, on its own plate (white on the accent's dark plate)
                const LLRect badge(pic.mLeft, pic.mTop, pic.mLeft + font->getWidth("Chosen") + 10, pic.mTop - 16);
                gl_rect_2d(badge, LLColor4(0.f, 0.f, 0.f, 0.85f), true);
                font->renderUTF8("Chosen", 0, (F32)badge.getCenterX(), (F32)badge.getCenterY(), LLColor4::white, LLFontGL::HCENTER,
                                 LLFontGL::VCENTER, LLFontGL::BOLD, LLFontGL::NO_SHADOW);
            }
            else if (hasFocus() && mSelected < 0 && i == 0)
            {
                gl_rect_2d(tile, ink, false);
            }
        }
        LLUICtrl::draw();
    }
};

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// [ROUND 4 LOOK 2026-10-24] Paul: "we need to upgrade the look of the wolfnpc floater to be of the same quality as our others".
// "The Campfire": a clearing at the edge of a night forest - pines against a teal sky, a crescent moon, fireflies, and a
// campfire with a sitting wolf and a standing person lit by it ("bringing characters to life"). Painted in code behind the
// XUI controls, the technique of the radio, WolfAvatars and The Forge (wolfpaint.h; llfloaterwolfavatars.cpp
// WolfAvatarsBackdrop; wolfai.cpp WolfAIForge), with its own picture and palette. Every control is still an XUI widget laid
// over the drawing, so the keyboard, focus and tooltips work as before.
// ════════════════════════════════════════════════════════════════════════════════════════════════════

namespace
{
    using namespace WolfPaint;

    // ---- palette: night forest and firelight (fixed colours: a painted scene) ----
    const LLColor4 CF_SKY_TOP(0.020f, 0.045f, 0.085f, 1.f);
    const LLColor4 CF_SKY_LOW(0.060f, 0.135f, 0.170f, 1.f);
    const LLColor4 CF_HILL_FAR(0.050f, 0.095f, 0.110f, 1.f);
    const LLColor4 CF_PINES(0.018f, 0.035f, 0.040f, 1.f);
    const LLColor4 CF_FLOOR_TOP(0.030f, 0.045f, 0.045f, 1.f);
    const LLColor4 CF_FLOOR_BOTTOM(0.012f, 0.016f, 0.018f, 1.f);
    const LLColor4 CF_MOON(0.96f, 0.95f, 0.86f, 1.f);
    const LLColor4 CF_FIRE(1.f, 0.62f, 0.20f, 1.f);
    const LLColor4 CF_FLAME(1.f, 0.85f, 0.42f, 1.f);
    const LLColor4 CF_EMBER(1.f, 0.45f, 0.14f, 1.f);
    const LLColor4 CF_GLOW(1.f, 0.55f, 0.18f, 1.f);
    const LLColor4 CF_FIREFLY(0.80f, 1.f, 0.55f, 1.f);
    const LLColor4 CF_FIGURE(0.012f, 0.016f, 0.020f, 1.f);
    const LLColor4 CF_TITLE(1.f, 0.82f, 0.50f, 1.f);
    const LLColor4 CF_CLEAR(0.f, 0.f, 0.f, 0.f);
    // plates and buttons (contrast in the palette note above: the words are NPC_CREAM / NPC_MUTED / NPC_ERR)
    const LLColor4 CF_PLATE(0.035f, 0.052f, 0.062f, 0.92f);
    const LLColor4 CF_PLATE_EDGE(0.36f, 0.46f, 0.34f, 0.65f);     // moss
    const LLColor4 CF_LEDGER(0.10f, 0.075f, 0.040f, 0.90f);       // the cost panel: firelit wood
    const LLColor4 CF_LEDGER_EDGE(0.86f, 0.58f, 0.24f, 0.75f);
    const LLColor4 CF_ERR_PLATE(0.18f, 0.05f, 0.06f, 0.95f);
    const LLColor4 CF_ERR_EDGE(1.f, 0.52f, 0.40f, 1.f);
    const LLColor4 CF_STONE_TOP(0.20f, 0.25f, 0.27f, 1.f);         // CREAM on it 9.0:1 (hovered 7.3:1, bottom 12.9:1)
    const LLColor4 CF_STONE_BOTTOM(0.12f, 0.15f, 0.16f, 1.f);
    const LLColor4 CF_STONE_EDGE(0.46f, 0.55f, 0.50f, 1.f);
    const LLColor4 CF_STONE_OFF(0.15f, 0.16f, 0.18f, 1.f);         // the disabled label #9e9ea8 on it 5.5:1
    const LLColor4 CF_EMBER_TOP(0.58f, 0.27f, 0.08f, 1.f);         // CREAM on the ember button: top 5.6:1 (hovered 4.7:1), bottom 7.9:1
    const LLColor4 CF_EMBER_BOTTOM(0.46f, 0.20f, 0.06f, 1.f);
    const LLColor4 CF_DANGER_TOP(0.50f, 0.13f, 0.12f, 1.f);        // CREAM 8.2:1 (bottom 11.6:1)
    const LLColor4 CF_DANGER_BOTTOM(0.34f, 0.08f, 0.08f, 1.f);
    const LLColor4 CF_SHADOW(0.f, 0.f, 0.f, 0.45f);

    // ---- the figures (also the cards' picture when an NPC's own is not known) ----

    /** A wolf sitting, facing right, base on the ground at (x, y), `h` tall. */
    void drawWolf(F32 x, F32 y, F32 h, const LLColor4& c)
    {
        const F32 u = h / 10.f;
        ellipse(x, y + 3.2f * u, 2.6f * u, 3.4f * u, c, c);                  // body, upright
        ellipse(x - 1.8f * u, y + 1.0f * u, 2.2f * u, 1.1f * u, c, c);       // haunch
        quad(x + 0.4f * u, y + 5.6f * u, x + 2.2f * u, y + 7.2f * u, x + 2.0f * u, y + 4.8f * u, x + 0.6f * u, y + 3.6f * u, c);   // chest and neck
        disc(x + 2.2f * u, y + 7.6f * u, 1.5f * u, c, c);                   // head
        quad(x + 3.0f * u, y + 8.0f * u, x + 5.0f * u, y + 7.3f * u, x + 5.0f * u, y + 6.8f * u, x + 3.0f * u, y + 6.8f * u, c);   // muzzle
        tri(x + 1.4f * u, y + 8.4f * u, x + 2.0f * u, y + 10.f * u, x + 2.6f * u, y + 8.6f * u, c);   // ears
        tri(x + 2.5f * u, y + 8.6f * u, x + 3.2f * u, y + 9.9f * u, x + 3.4f * u, y + 8.3f * u, c);
        quad(x + 1.2f * u, y, x + 1.9f * u, y, x + 1.6f * u, y + 3.6f * u, x + 0.8f * u, y + 3.6f * u, c);   // foreleg
        quad(x - 3.6f * u, y + 0.2f * u, x - 2.6f * u, y + 0.6f * u, x - 5.6f * u, y + 2.4f * u, x - 6.0f * u, y + 1.6f * u, c);   // tail
    }

    /** A person standing, `h` tall, feet at (x, y). */
    void drawPerson(F32 x, F32 y, F32 h, const LLColor4& c)
    {
        const F32 u = h / 10.f;
        disc(x, y + 9.0f * u, 0.95f * u, c, c);                              // head
        quad(x - 1.3f * u, y + 7.8f * u, x + 1.3f * u, y + 7.8f * u, x + 1.0f * u, y + 4.2f * u, x - 1.0f * u, y + 4.2f * u, c);   // body
        quad(x - 1.0f * u, y + 4.4f * u, x - 0.1f * u, y + 4.4f * u, x - 0.3f * u, y, x - 0.9f * u, y, c);   // legs
        quad(x + 0.1f * u, y + 4.4f * u, x + 1.0f * u, y + 4.4f * u, x + 0.9f * u, y, x + 0.3f * u, y, c);
        quad(x - 1.3f * u, y + 7.7f * u, x - 0.9f * u, y + 7.6f * u, x - 1.7f * u, y + 4.6f * u, x - 2.1f * u, y + 4.8f * u, c);   // arms
        quad(x + 1.3f * u, y + 7.7f * u, x + 0.9f * u, y + 7.6f * u, x + 2.2f * u, y + 5.3f * u, x + 2.5f * u, y + 5.6f * u, c);
    }

    /** A pine, its foot at (x, y). */
    void drawPine(F32 x, F32 y, F32 h, const LLColor4& c)
    {
        const F32 w = h * 0.36f;
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 base = y + h * 0.18f + (F32)i * h * 0.24f, top = base + h * 0.44f;
            const F32 half = w * (1.f - 0.26f * (F32)i);
            tri(x - half, base, x + half, base, x, top, c);
        }
        quad(x - w * 0.06f, y, x + w * 0.06f, y, x + w * 0.06f, y + h * 0.2f, x - w * 0.06f, y + h * 0.2f, c);
    }

    /** The campfire: logs, the glow on the ground, three flames that flicker. */
    void drawFire(F32 x, F32 y, F32 s, F32 t)
    {
        const F32 flick = 0.85f + 0.15f * sinf(t * 7.3f) * sinf(t * 3.1f);
        ellipse(x, y, s * 5.f, s * 1.4f, withAlpha(CF_GLOW, 0.42f * flick), CF_CLEAR);
        limb(x - s * 1.4f, y - s * 0.1f, x + s * 1.4f, y + s * 0.4f, s * 0.42f, LLColor4(0.16f, 0.09f, 0.05f, 1.f));
        limb(x - s * 1.4f, y + s * 0.4f, x + s * 1.4f, y - s * 0.1f, s * 0.42f, LLColor4(0.20f, 0.11f, 0.06f, 1.f));
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 k = (F32)i;
            const F32 sway = sinf(t * (4.f + k) + k * 1.7f) * s * 0.25f;
            const F32 hh = s * (2.6f - 0.55f * k) * (0.9f + 0.1f * sinf(t * 9.f + k));
            const F32 ww = s * (1.1f - 0.25f * k);
            const LLColor4 c = i == 0 ? CF_EMBER : i == 1 ? CF_FIRE : CF_FLAME;
            tri(x - ww, y + s * 0.3f, x + ww, y + s * 0.3f, x + sway, y + s * 0.3f + hh, c);
        }
        for (S32 i = 0; i < 5; ++i)   // sparks rising
        {
            const F32 h1 = hash01((F32)i * 5.17f + 0.3f);
            const F32 rise = s * 6.f;
            const F32 ly = fmodf(t * (s * 1.4f + s * h1) + h1 * rise, rise);
            sparkle(x + sinf(t * 2.f + (F32)i) * s * 0.8f, y + s * 1.5f + ly, 0.9f, withAlpha(CF_FLAME, 0.8f * (1.f - ly / rise)));
        }
    }

    // ---- status chips: a word on a tinted pill (never colour alone) ----
    enum EChip { CHIP_OK = 0, CHIP_NEUTRAL, CHIP_WARN, CHIP_DANGER, CHIP_INFO, CHIP_GM };
    void chipColours(EChip tone, LLColor4& bg, LLColor4& ink)
    {
        // contrast of the ink on each pill (WCAG): OK 5.3:1, NEUTRAL 6.8:1, WARN 8.0:1 (dark ink), DANGER 6.5:1, INFO 5.6:1, GM 6.3:1
        switch (tone)
        {
        case CHIP_OK:      bg = LLColor4(0.20f, 0.42f, 0.26f, 1.f); ink = NPC_CREAM; break;
        case CHIP_NEUTRAL: bg = LLColor4(0.30f, 0.31f, 0.36f, 1.f); ink = NPC_CREAM; break;
        case CHIP_WARN:    bg = LLColor4(0.85f, 0.62f, 0.20f, 1.f); ink = LLColor4(0.10f, 0.06f, 0.02f, 1.f); break;
        case CHIP_DANGER:  bg = LLColor4(0.60f, 0.16f, 0.14f, 1.f); ink = NPC_CREAM; break;
        case CHIP_INFO:    bg = LLColor4(0.20f, 0.36f, 0.60f, 1.f); ink = NPC_CREAM; break;
        default:           bg = LLColor4(0.42f, 0.26f, 0.55f, 1.f); ink = NPC_CREAM; break;
        }
    }

    /** A chip with its left edge at x, centred on y; returns its width. */
    F32 drawChip(const LLFontGL* f, const std::string& word, F32 x, F32 y, EChip tone)
    {
        LLColor4 bg, ink;
        chipColours(tone, bg, ink);
        const F32 w = f->getWidthF32(word) + 12.f, h = (F32)f->getLineHeight() + 4.f;
        untextured();
        fillRounded(LLRect((S32)x, (S32)(y + h * 0.5f), (S32)(x + w), (S32)(y - h * 0.5f)), allCorners(h * 0.5f), bg, bg);
        text(f, word, x + w * 0.5f, y, ink, LLFontGL::HCENTER);
        return w;
    }
}

/**
 * The region's NPCs as character cards: the NPC's own WolfAvatars picture when it is known (an NPC of yours, once opened
 * in Edit or made here), else a painted wolf or person; its name; kind, owner and distance; and its status chips (Awake /
 * Asleep, Paused, Down, AI talk, AI paused: nobody here, AI stopped, Game Mode only, Run by). Click chooses one, a double
 * click (or Enter) opens it; the arrows move. Created in code (not registered), with its own Params.
 */
class WolfNPCCardGrid : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    struct Card
    {
        std::string mId, mName, mSub, mKind, mTip;
        std::vector<std::pair<std::string, EChip>> mChips;
        LLPointer<LLViewerTexture> mTexture;
    };
    static const S32 CARD_W = 139, CARD_H = 146, PIC_H = 66;

    explicit WolfNPCCardGrid(const Params& p) : LLUICtrl(p) {}

    std::vector<Card> mCards;
    std::string mSelected;
    std::string mComment;
    std::function<void()> mOnPick, mOnOpen;

    S32 columns() const { return llmax(1, getRect().getWidth() / CARD_W); }
    S32 neededHeight() const
    {
        const S32 cols = columns();
        return (((S32)mCards.size() + cols - 1) / cols) * CARD_H + 4;
    }
    S32 indexAt(S32 x, S32 y) const
    {
        const S32 cols = columns(), c = x / CARD_W, r = (getRect().getHeight() - y) / CARD_H;
        if (x < 0 || c >= cols || r < 0) return -1;
        const S32 i = r * cols + c;
        return i < (S32)mCards.size() ? i : -1;
    }
    S32 selectedIndex() const
    {
        for (S32 i = 0; i < (S32)mCards.size(); ++i)
        {
            if (mCards[(size_t)i].mId == mSelected) return i;
        }
        return -1;
    }
    void pick(S32 i)
    {
        if (i < 0 || i >= (S32)mCards.size()) return;
        mSelected = mCards[(size_t)i].mId;
        if (mOnPick) mOnPick();
    }
    bool handleMouseDown(S32 x, S32 y, MASK mask) override
    {
        setFocus(true);
        pick(indexAt(x, y));
        return true;
    }
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override
    {
        if (indexAt(x, y) >= 0 && mOnOpen) mOnOpen();
        return true;
    }
    bool handleToolTip(S32 x, S32 y, MASK mask) override
    {
        const S32 i = indexAt(x, y);
        if (i < 0) return LLUICtrl::handleToolTip(x, y, mask);
        setToolTip(mCards[(size_t)i].mTip);
        return LLUICtrl::handleToolTip(x, y, mask);
    }
    bool handleKeyHere(KEY key, MASK mask) override
    {
        if (mCards.empty() || mask != MASK_NONE) return LLUICtrl::handleKeyHere(key, mask);
        const S32 cols = columns();
        S32 i = llmax(0, selectedIndex());
        if (key == KEY_RIGHT) i = llmin(i + 1, (S32)mCards.size() - 1);
        else if (key == KEY_LEFT) i = llmax(i - 1, 0);
        else if (key == KEY_DOWN) i = llmin(i + cols, (S32)mCards.size() - 1);
        else if (key == KEY_UP) i = llmax(i - cols, 0);
        else if (key == KEY_RETURN) { if (mOnOpen && selectedIndex() >= 0) mOnOpen(); return true; }
        else return LLUICtrl::handleKeyHere(key, mask);
        pick(i);
        return true;
    }
    void draw() override
    {
        const S32 cols = columns(), h = getRect().getHeight();
        const LLFontGL* bold = LLFontGL::getFontSansSerifBold();
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        LLGLSUIDefault ui;
        untextured();
        if (mCards.empty() && !mComment.empty())
        {
            text(LLFontGL::getFontSansSerif(), mComment, (F32)getRect().getWidth() * 0.5f, (F32)h - 40.f, NPC_CREAM, LLFontGL::HCENTER, (F32)getRect().getWidth() - 20.f);
        }
        for (S32 i = 0; i < (S32)mCards.size(); ++i)
        {
            const Card& c = mCards[(size_t)i];
            const S32 left = (i % cols) * CARD_W, top = h - (i / cols) * CARD_H;
            const LLRect r(left + 4, top - 4, left + CARD_W - 4, top - CARD_H + 4);
            const bool sel = c.mId == mSelected;
            S32 mx = 0, my = 0;
            LLUI::getInstance()->getMousePositionLocal(this, &mx, &my);
            const bool hover = indexAt(mx, my) == i && pointInView(mx, my);
            untextured();
            LLRect shadow = r;
            shadow.translate(2, -3);
            fillRounded(shadow, allCorners(9.f), CF_SHADOW, CF_SHADOW);
            if (sel) halo(r, 9.f, 8, withAlpha(CF_GLOW, 0.55f));
            fillRounded(r, allCorners(9.f), lighten(CF_STONE_TOP, hover ? 0.04f : 0.f), CF_STONE_BOTTOM);
            strokeRounded(r, allCorners(9.f), 0.f, sel ? 2.5f : 1.f, sel ? CF_FIRE : CF_STONE_EDGE, sel ? CF_EMBER : withAlpha(CF_STONE_EDGE, 0.6f));
            // the picture, or the figure in a pool of firelight
            const LLRect pic(r.mLeft + 6, r.mTop - 6, r.mRight - 6, r.mTop - 6 - PIC_H);
            fillRounded(pic, allCorners(6.f), LLColor4(0.06f, 0.10f, 0.13f, 1.f), LLColor4(0.03f, 0.05f, 0.06f, 1.f));
            if (c.mTexture.notNull() && c.mTexture->getWidth() > 0 && c.mTexture->getHeight() > 0)
            {
                const F32 scale = llmin((F32)pic.getWidth() / (F32)c.mTexture->getWidth(), (F32)pic.getHeight() / (F32)c.mTexture->getHeight());
                const S32 tw = ll_round((F32)c.mTexture->getWidth() * scale), th = ll_round((F32)c.mTexture->getHeight() * scale);
                gGL.color4f(1.f, 1.f, 1.f, 1.f);
                gl_draw_scaled_image(pic.getCenterX() - tw / 2, pic.getCenterY() - th / 2, tw, th, c.mTexture.get());
                untextured();
            }
            else
            {
                ellipse((F32)pic.getCenterX(), (F32)pic.mBottom + 6.f, (F32)pic.getWidth() * 0.4f, 9.f, withAlpha(CF_GLOW, 0.35f), CF_CLEAR);
                const LLColor4 fig = LLColor4(0.55f, 0.62f, 0.60f, 1.f);
                if (c.mKind == "person") drawPerson((F32)pic.getCenterX(), (F32)pic.mBottom + 5.f, (F32)PIC_H - 12.f, fig);
                else drawWolf((F32)pic.getCenterX() - 2.f, (F32)pic.mBottom + 5.f, (F32)PIC_H - 14.f, fig);
            }
            if (sel)
            {
                // the chosen card says so with a mark as well as its glow
                disc((F32)r.mRight - 14.f, (F32)r.mTop - 14.f, 9.f, CF_FIRE, CF_EMBER);
                checkMark((F32)r.mRight - 14.f, (F32)r.mTop - 14.f, 8.f, 1.6f, LLColor4(0.10f, 0.06f, 0.02f, 1.f));
            }
            text(bold, c.mName, (F32)r.getCenterX(), (F32)pic.mBottom - 11.f, NPC_CREAM, LLFontGL::HCENTER, (F32)r.getWidth() - 10.f);
            text(small, c.mSub, (F32)r.getCenterX(), (F32)pic.mBottom - 26.f, NPC_MUTED, LLFontGL::HCENTER, (F32)r.getWidth() - 10.f);
            // the chips, two rows at most (the rest in the tooltip)
            F32 cx = (F32)r.mLeft + 6.f, cy = (F32)pic.mBottom - 42.f;
            S32 rows = 0;
            for (const auto& chip : c.mChips)
            {
                const F32 w = small->getWidthF32(chip.first) + 12.f;
                if (cx + w > (F32)r.mRight - 6.f)
                {
                    if (++rows >= 2) break;
                    cx = (F32)r.mLeft + 6.f;
                    cy -= (F32)small->getLineHeight() + 7.f;
                }
                cx += drawChip(small, chip.first, cx, cy, chip.second) + 4.f;
            }
            if (hasFocus() && sel)
            {
                untextured();
                strokeRounded(grown(r, 2), allCorners(11.f), 0.f, 1.f, NPC_CREAM, NPC_CREAM);
            }
        }
        LLUICtrl::draw();
    }
};

/** The view behind every control that paints the scene. Not mouse-opaque: clicks fall through to the drag handle.
 *  Source: llfloaterwolfavatars.cpp:860-878 WolfAvatarsBackdrop. */
class WolfNPCBackdrop : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };
    WolfNPCBackdrop(const Params& p, WolfFloaterNPC* owner) : LLView(p), mOwner(owner) {}
    void draw() override { mOwner->drawScene(); }
private:
    WolfFloaterNPC* mOwner;
};

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// The settings form
// ════════════════════════════════════════════════════════════════════════════════════════════════════

WolfNPCForm::WolfNPCForm(WolfFloaterNPC* owner, LLPanel* panel, LLScrollContainer* scroll, bool editing, const Quick* quick)
:   mOwner(owner), mPanel(panel), mScroll(scroll), mEditing(editing)
{
    for (S32 i = 0; i < SECTIONS; ++i)
    {
        mHead[i] = mPanel->getChild<LLButton>(llformat("head_%d", i));
        mBody[i] = mPanel->getChild<LLPanel>(llformat("body_%d", i));
        mHeadLabel[i] = mHead[i]->getLabelUnselected();
        mHead[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { openSection(i, !mOpen[i]); });
    }
    mName = mPanel->getChild<LLLineEditor>("npc_name");
    mBehaviour = mPanel->getChild<LLComboBox>("behaviour");
    mBehaviourNote = mPanel->getChild<LLTextBox>("behaviour_note");
    mBehaviour->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBehaviourChanged(); });

    mHome = mPanel->getChild<LLTextBox>("home_text");
    mAreaMode = mPanel->getChild<LLComboBox>("area_mode");
    fillCombo(mAreaMode, AREA_MODES);
    mAreaMode->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshArea(); });
    mRadius = mPanel->getChild<LLSliderCtrl>("area_radius");
    mIndoors = mPanel->getChild<LLComboBox>("indoors");
    fillCombo(mIndoors, INDOORS);
    mSwim = mPanel->getChild<LLCheckBoxCtrl>("swim");
    // [ROUND C] PROTOCOL.md §26 travel (pets only) and §27 noises (animals)
    mTravel = mPanel->getChild<LLCheckBoxCtrl>("travel");
    mNoises = mPanel->getChild<LLCheckBoxCtrl>("noises");
    mNoiseVolume = mPanel->getChild<LLSliderCtrl>("noise_volume");
    mNoises->setCommitCallback([this](LLUICtrl*, const LLSD&) { mNoiseVolume->setEnabled(mNoises->get() && mName->getEnabled()); });

    mShy = mPanel->getChild<LLSliderCtrl>("shy");
    mVisitors = mPanel->getChild<LLComboBox>("visitors");
    fillCombo(mVisitors, VISITORS);
    mPackLabel = mPanel->getChild<LLTextBox>("pack_lbl");
    mPack = mPanel->getChild<LLLineEditor>("pack");
    mLeader = mPanel->getChild<LLCheckBoxCtrl>("leader");

    mWork = mPanel->getChild<LLComboBox>("work");
    fillCombo(mWork, WORKS);
    mWorkAtText = mPanel->getChild<LLTextBox>("work_at_text");
    mPanel->getChild<LLButton>("work_at_here")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mHasWorkAt) { mHasWorkAt = false; refreshWorkAt(); return; }
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        mWorkAt = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
        mHasWorkAt = true;
        refreshWorkAt();
    });
    mRoutine = mPanel->getChild<LLTextEditor>("routine");
    // [ROUND 4] its places are kept on the Places tab (Edit); the form only reads them for the routine
    mGameOnly = mPanel->getChild<LLCheckBoxCtrl>("game_only");
    mGameOnly->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->showAiLine(this); });

    mFight = mPanel->getChild<LLCheckBoxCtrl>("fight");
    mFight->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        refreshGame();
        relayout();
        if (mEditing) mOwner->refreshDetailTabs();   // [ROUND 3] the Game tab follows "Can fight in game mode"
    });
    // [ROUND 3] Edit only: the NPC must exist before it has quests, dialogues... (New NPC says so instead)
    {
        LLButton* more = mPanel->getChild<LLButton>("game_more");
        more->setVisible(mEditing);
        more->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->openGameTab(); });
    }
    mGameNote = mPanel->getChild<LLTextBox>("game_note");
    mGameDetails = mPanel->getChild<LLPanel>("game_details");
    mHuntable = mPanel->getChild<LLCheckBoxCtrl>("huntable");
    mHealth = mPanel->getChild<LLSpinCtrl>("health");
    mRespawn = mPanel->getChild<LLSpinCtrl>("respawn");
    mHunt = mPanel->getChild<LLCheckBoxCtrl>("hunt");
    mSight = mPanel->getChild<LLSpinCtrl>("sight");
    mChase = mPanel->getChild<LLSpinCtrl>("chase");
    mGiveUp = mPanel->getChild<LLSpinCtrl>("give_up");
    mCooldown = mPanel->getChild<LLSpinCtrl>("cooldown");
    for (S32 i = 0; i < DAMAGE_ROWS; ++i)
    {
        mDamageStat[i] = mPanel->getChild<LLLineEditor>(llformat("dmg_stat_%d", i));
        mDamageAmount[i] = mPanel->getChild<LLSpinCtrl>(llformat("dmg_amount_%d", i));
    }

    mAiOn = mPanel->getChild<LLCheckBoxCtrl>("ai_on");
    mAiOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->loadAiInfo(this); });
    mAiLine = mPanel->getChild<LLTextBox>("ai_line");
    mPersonality = mPanel->getChild<LLTextEditor>("personality");
    mVoice = mPanel->getChild<LLComboBox>("voice");
    mVoice->add("Written only", LLSD(""));
    mVoice->selectFirstItem();
    mVoice->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->showAiLine(this); });
    mDailyLimit = mPanel->getChild<LLSpinCtrl>("daily_limit");
    mNpcTalk = mPanel->getChild<LLCheckBoxCtrl>("npc_talk");

    mVehicle = mPanel->getChild<LLComboBox>("vehicle");
    mVehicle->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshVehicle(); });
    mPanel->getChild<LLButton>("vehicle_find")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->requestVehicles(this); });
    mRoads = mPanel->getChild<LLCheckBoxCtrl>("roads");
    mRoads->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshVehicle(); });
    mLoop = mPanel->getChild<LLCheckBoxCtrl>("loop");
    mRouteText = mPanel->getChild<LLTextBox>("route_text");
    mPanel->getChild<LLButton>("route_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAddRoutePoint(); });
    mPanel->getChild<LLButton>("route_clear")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mRoute.clear(); refreshVehicle(); });
    mVehicleSpeed = mPanel->getChild<LLSpinCtrl>("vehicle_speed");
    mPassenger = mPanel->getChild<LLCheckBoxCtrl>("passenger");
    mPassengerNote = mPanel->getChild<LLTextBox>("passenger_note");

    if (quick)
    {
        // [ROUND 2] the quick step's own name and AI talk widgets stand in for the form's (never two of each on screen):
        // the form's name row goes and the rows under it move up 28 px; the AI section keeps voice, daily limit and
        // "Talks to other NPCs", moved up to its top (150 px) and the section 64 px high
        for (const char* n : { "name_lbl", "npc_name", "ai_on", "ai_line", "personality_lbl", "personality" }) mPanel->getChild<LLView>(n)->setVisible(false);
        for (const char* n : { "behaviour_lbl", "behaviour", "behaviour_note" }) mPanel->getChild<LLView>(n)->translate(0, 28);
        mTopY = 64;
        for (const char* n : { "voice_lbl", "voice", "daily_limit", "npc_talk" }) mBody[4]->getChild<LLView>(n)->translate(0, 150);
        mBody[4]->reshape(mBody[4]->getRect().getWidth(), 64, false);   // children that follow the top keep their place under it
        mName = quick->mName;
        mAiOn = quick->mAiOn;
        mPersonality = quick->mPersonality;
        mAiLine = quick->mAiLine;
        mAiOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->loadAiInfo(this); });
        mDailyLimit->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->showAiLine(this); });
        mNpcTalk->setCommitCallback([this](LLUICtrl*, const LLSD&) { mOwner->showAiLine(this); });
    }

    setKind("animal");
    fill(LLSD::emptyMap());
}

void WolfNPCForm::openSection(S32 n, bool open)
{
    if (n < 0 || n >= SECTIONS) return;
    mOpen[n] = open;
    // AI talk: the price line is read when the section opens (the live balance, never one kept from before)
    if (open && n == 4) mOwner->loadAiInfo(this);
    relayout();
    if (open && mScroll)
    {
        LLRect r = mBody[n]->getRect();
        r.mTop = mHead[n]->getRect().mTop;
        mScroll->scrollToShowRect(r);
    }
}

void WolfNPCForm::setKind(const std::string& kind)
{
    mKind = kind == "person" ? "person" : "animal";
    fillBehaviours();
    const bool animal = mKind == "animal";
    mPackLabel->setText(animal ? std::string("Pack") : std::string("Group"));
    mLeader->setLabel(animal ? std::string("Leads its pack") : std::string("Leads its group"));
    // Source: PROTOCOL.md section 10 "Passenger (animals, or passenger: true)"
    mPassenger->setVisible(!animal);
    mPassengerNote->setVisible(animal);
    mNoises->setVisible(animal && mOwner->petsSupported());        // [ROUND C GATE] §27 noises
    mNoiseVolume->setVisible(animal && mOwner->petsSupported());
    onBehaviourChanged();
    relayout();
}

void WolfNPCForm::fillBehaviours()
{
    const std::string was = mBehaviour->getValue().asString();
    mBehaviour->removeall();
    if (mKind == "animal")
    {
        // [ROUND C GATE] "pet" only where the region has pets (its hello carries "rules"); an NPC already a pet keeps it
        const bool pets = mOwner->petsSupported() || was == "pet" || mBase["behaviour"].asString() == "pet";
        for (const Choice& ch : ANIMAL_BEHAVIOURS)
        {
            if (!pets && std::string(ch.value) == "pet") continue;
            mBehaviour->add(ch.label, LLSD(ch.value));
        }
    }
    else fillCombo(mBehaviour, PERSON_BEHAVIOURS);
    selectOr(mBehaviour, was);
}

void WolfNPCForm::onBehaviourChanged()
{
    const std::string b = mBehaviour->getValue().asString();
    mBehaviourNote->setText(behaviourNote(b, mKind == "animal"));
    mTravel->setVisible(b == "pet" && mOwner->petsSupported());   // §26.1 "travel": true (only with "pet")
    // Source: PROTOCOL.md section 3 game.hunt "behaviour predator/zombie turn it on": the Game section shows it
    if ((b == "predator" || b == "zombie") && !mFight->get())
    {
        mFight->set(true);
        mHunt->set(true);
        refreshGame();
        relayout();
    }
}

void WolfNPCForm::refreshArea()
{
    mRadius->setEnabled(mAreaMode->getValue().asString() == "radius" && mName->getEnabled());
}

void WolfNPCForm::refreshGame()
{
    mGameDetails->setVisible(mFight->get());
}

void WolfNPCForm::refreshWorkAt()
{
    mWorkAtText->setText(mHasWorkAt ? "Works at " + vecText(mWorkAt) : std::string("Works at its home"));
    mPanel->getChild<LLButton>("work_at_here")->setLabel(mHasWorkAt ? std::string("Use its home") : std::string("Where I stand"));
}

void WolfNPCForm::refreshPetSupport()
{
    // [ROUND C GATE] the region's hello arrived (or changed): the pet behaviour and the noise / travel rows follow it
    // (not setKind: that would also run the behaviour's own defaults again)
    fillBehaviours();
    const bool animal = mKind == "animal", pets = mOwner->petsSupported();
    mNoises->setVisible(animal && pets);
    mNoiseVolume->setVisible(animal && pets);
    mTravel->setVisible(pets && mBehaviour->getValue().asString() == "pet");
    mBehaviourNote->setText(behaviourNote(mBehaviour->getValue().asString(), animal));
    relayout();
}

void WolfNPCForm::setPlaces(const LLSD& cfg)
{
    // [ROUND 4] the Places tab saved them: the routine is checked against these places and tool roles (§23.1)
    mPlacesMap = cfg["places"].isMap() ? cfg["places"] : LLSD::emptyMap();
    mBase["places"] = mPlacesMap;
    mBase["tools"] = cfg["tools"];
}

void WolfNPCForm::onAddRoutePoint()
{
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    mRoute.push_back(rgn->getPosRegionFromAgent(gAgent.getPositionAgent()));
    refreshVehicle();
}

void WolfNPCForm::refreshVehicle()
{
    const bool has = !mVehicle->getValue().asString().empty();
    const bool on = has && mName->getEnabled();
    mRoads->setEnabled(on);
    mLoop->setEnabled(on && !mRoads->get());
    mPanel->getChild<LLButton>("route_add")->setEnabled(on && !mRoads->get());
    mPanel->getChild<LLButton>("route_clear")->setEnabled(on && !mRoads->get() && !mRoute.empty());
    mVehicleSpeed->setEnabled(on);
    mPassenger->setEnabled(on);
    mRouteText->setText(mRoads->get() ? std::string("Route: the roads") : llformat("Route: %d points", (S32)mRoute.size()));
}

void WolfNPCForm::setVehicles(const LLSD& vehicles)
{
    mVehicles = vehicles;
    const std::string was = mVehicle->getValue().asString();
    const std::string base = mBase["vehicle"]["id"].asString();
    mVehicle->removeall();
    mVehicle->add(base.empty() ? std::string("None") : std::string("None (stop using it)"), LLSD(""));
    bool base_listed = false;
    for (const LLSD& v : llsd::inArray(vehicles))
    {
        const std::string id = v["id"].asString();
        if (id.empty()) continue;
        if (id == base) base_listed = true;
        mVehicle->add(llformat("%s (%.0f m)", cap(v["name"].asString(), 48).c_str(), v["dist"].asReal()), LLSD(id));
    }
    if (!base.empty() && !base_listed) mVehicle->add("Its vehicle (not near you now)", LLSD(base));
    selectOr(mVehicle, was);
    refreshVehicle();
}

void WolfNPCForm::setVoices(const LLSD& voices)
{
    const std::string was = mVoice->getValue().asString();
    mVoice->removeall();
    mVoice->add("Written only", LLSD(""));
    bool listed = was.empty();
    for (const LLSD& v : llsd::inArray(voices))
    {
        const std::string id = v["id"].asString();
        if (id.empty()) continue;
        if (id == was) listed = true;
        mVoice->add(cap(v["name"].asString(), 48), LLSD(id));
    }
    if (!listed) mVoice->add("Its voice (" + cap(was, 24) + ")", LLSD(was));
    selectOr(mVoice, was);
}

void WolfNPCForm::setGameInfo(const LLSD& game)
{
    // Source: PROTOCOL.md section 1 - "be hunted ... the region's game is managed with a health stat"; "make it hurt game
    // players: its owner must be the region game's owner"
    std::string note;
    if (!game.isMap()) note = std::string();
    else if (!game["managed"].asBoolean()) note = "This region has no managed game, so the game settings wait until it has one.";
    else if (!game["health"].asBoolean()) note = "This region's game has no health stat, so it cannot be hunted here.";
    else if (!game["mine"].asBoolean()) note = "It can be hunted here; only NPCs of the game's owner can hurt players.";
    mGameNote->setText(note);
}

void WolfNPCForm::setHomeText(const std::string& text)
{
    mHome->setText(text);
}

void WolfNPCForm::setAiLine(const std::string& text, bool problem)
{
    mAiLine->setText(text);
    mAiLine->setColor(problem ? caution() : normalText());
}

void WolfNPCForm::setName(const std::string& name) { mName->setText(name); }
std::string WolfNPCForm::name() const { return trimmed(mName->getText()); }
bool WolfNPCForm::voiceChosen() const { return !mVoice->getValue().asString().empty(); }
bool WolfNPCForm::gameOnly() const { return mGameOnly->get(); }
bool WolfNPCForm::npcTalkOn() const { return mNpcTalk->get(); }
F64 WolfNPCForm::dailyLimit() const { return std::round((F64)mDailyLimit->get()); }

void WolfNPCForm::setEnabled(bool on)
{
    for (LLView* v : { (LLView*)mName, (LLView*)mBehaviour, (LLView*)mAreaMode, (LLView*)mIndoors, (LLView*)mSwim, (LLView*)mTravel, (LLView*)mNoises,
                       (LLView*)mNoiseVolume, (LLView*)mShy, (LLView*)mVisitors,
                       (LLView*)mPack, (LLView*)mLeader, (LLView*)mWork, (LLView*)mRoutine, (LLView*)mFight, (LLView*)mAiOn,
                       (LLView*)mPersonality, (LLView*)mVoice, (LLView*)mDailyLimit, (LLView*)mNpcTalk, (LLView*)mVehicle })
    {
        v->setEnabled(on);
    }
    mGameDetails->setEnabled(on);
    refreshArea();
    refreshVehicle();
}

void WolfNPCForm::readRoutine(const LLSD& routine)
{
    std::string text;
    for (const LLSD& r : llsd::inArray(routine))
    {
        const std::string at = cap(r["at"].asString(), 5);
        const std::string d = r["do"].asString();
        const std::string what = d == "goto" ? cap(r["place"].asString(), 24) : cap(d, 8);
        if (at.empty() || what.empty()) continue;
        if (!text.empty()) text += "\n";
        text += at + " " + what;
    }
    mRoutine->setText(text);
}

void WolfNPCForm::fill(const LLSD& from)
{
    mBase = from.isMap() ? from : LLSD::emptyMap();
    const LLSD& c = mBase;
    // Source: PROTOCOL.md section 3 - the defaults where a key is missing
    mName->setText(cap(c["name"].asString(), 48));
    if (c.has("kind")) setKind(c["kind"].asString());
    selectOr(mBehaviour, c.has("behaviour") ? c["behaviour"].asString() : std::string(mKind == "animal" ? "roam" : "stroll"));
    mBehaviourNote->setText(behaviourNote(mBehaviour->getValue().asString(), mKind == "animal"));

    LLVector3 home;
    if (mEditing) setHomeText(readVec(c["home"], home) ? vecText(home) + " (Make here its home moves it)" : std::string("Not known"));
    const LLSD& area = c["area"];
    selectOr(mAreaMode, area.has("mode") ? area["mode"].asString() : std::string("radius"));
    mRadius->setValue(LLSD(area.has("radius") ? llclamp(area["radius"].asReal(), 2.0, 128.0) : 20.0));
    selectOr(mIndoors, c.has("indoors") ? c["indoors"].asString() : std::string("never"));
    mSwim->set(c["swim"].asBoolean());
    mTravel->set(c["travel"].asBoolean());
    mTravel->setVisible(mBehaviour->getValue().asString() == "pet" && mOwner->petsSupported());
    // §27 noises {on, volume}: on, at 0.7, when it was never set
    mNoises->set(c["noises"].has("on") ? c["noises"]["on"].asBoolean() : true);
    mNoiseVolume->setValue(LLSD(c["noises"].has("volume") ? llclamp(c["noises"]["volume"].asReal(), 0.0, 1.0) : 0.7));    mShy->setValue(LLSD(c.has("shy") ? llclamp(c["shy"].asReal(), 0.0, 1.0) : 0.3));
    selectOr(mVisitors, c.has("visitors") ? c["visitors"].asString() : std::string("friendly"));
    mPack->setText(cap(c["pack"].asString(), 24));
    mLeader->set(c["leader"].asBoolean());

    selectOr(mWork, c["work"].asString());
    mHasWorkAt = readVec(c["workAt"], mWorkAt);
    refreshWorkAt();
    mPlacesMap = c["places"].isMap() ? c["places"] : LLSD::emptyMap();
    mGameOnly->set(c["gameOnly"].asBoolean());
    readRoutine(c["routine"]);

    const LLSD& g = c["game"];
    mFight->set(g["huntable"].asBoolean() || g["hunt"].asBoolean());
    mHuntable->set(g["huntable"].asBoolean());
    mHealth->setValue(LLSD(g.has("health") ? g["health"].asReal() : 100.0));
    mRespawn->setValue(LLSD(g.has("respawn") ? g["respawn"].asReal() : 300.0));
    mHunt->set(g["hunt"].asBoolean());
    mSight->setValue(LLSD(g.has("sight") ? g["sight"].asReal() : 20.0));
    mChase->setValue(LLSD(g.has("chase") ? g["chase"].asReal() : 0.0));
    mGiveUp->setValue(LLSD(g.has("giveUp") ? g["giveUp"].asReal() : 40.0));
    mCooldown->setValue(LLSD(g.has("cooldown") ? g["cooldown"].asReal() : 2.0));
    {
        S32 i = 0;
        const LLSD dmg = g.has("damage") && g["damage"].isMap() ? g["damage"] : LLSD().with("health", 5);
        for (auto it = dmg.beginMap(); it != dmg.endMap() && i < DAMAGE_ROWS; ++it, ++i)
        {
            mDamageStat[i]->setText(cap(it->first, 32));
            mDamageAmount[i]->setValue(LLSD(llclamp(it->second.asReal(), 1.0, 1000.0)));
        }
        for (; i < DAMAGE_ROWS; ++i)
        {
            mDamageStat[i]->setText(LLStringUtil::null);
            mDamageAmount[i]->setValue(LLSD(1.0));
        }
    }
    refreshGame();

    const LLSD& ai = c["ai"];
    mAiOn->set(ai["on"].asBoolean());
    mPersonality->setText(cap(ai["personality"].asString(), 1000));
    {
        const std::string voice = ai["voice"].asString();
        if (!voice.empty() && !mVoice->selectByValue(LLSD(voice)))
        {
            mVoice->add("Its voice (" + cap(voice, 24) + ")", LLSD(voice));
            mVoice->selectByValue(LLSD(voice));
        }
        else if (voice.empty()) mVoice->selectFirstItem();
    }
    mDailyLimit->setValue(LLSD(ai.has("dailyLimit") ? ai["dailyLimit"].asReal() : 0.0));
    mNpcTalk->set(ai["npcTalk"].asBoolean());

    const LLSD& v = c["vehicle"];
    mRoute.clear();
    for (const LLSD& p : llsd::inArray(v["route"]))
    {
        LLVector3 q;
        if (readVec(p, q)) mRoute.push_back(q);
    }
    mRoads->set(v["roads"].asBoolean());
    mLoop->set(v.has("loop") ? v["loop"].asBoolean() : true);
    mVehicleSpeed->setValue(LLSD(v.has("speed") ? llclamp(v["speed"].asReal(), 1.0, 20.0) : 8.0));
    mPassenger->set(v["passenger"].asBoolean());
    setVehicles(mVehicles);
    selectOr(mVehicle, v["id"].asString());
    refreshVehicle();
    refreshArea();
    relayout();

    std::string ignored;
    mFilledCfg = LLSD::emptyMap();
    cfg(mFilledCfg, ignored);
}

bool WolfNPCForm::cfg(LLSD& out, std::string& error) const
{
    out = LLSD::emptyMap();
    const std::string name = trimmed(mName->getText());
    // Source: PROTOCOL.md section 3 name "1-48 chars"
    if (name.empty() || utf8Chars(name) > 48) { error = "Give it a name of 1 to 48 characters."; return false; }
    out["name"] = name;
    out["behaviour"] = mBehaviour->getValue().asString();
    LLSD area = mBase["area"].isMap() ? mBase["area"] : LLSD::emptyMap();
    area["mode"] = mAreaMode->getValue().asString();
    area["radius"] = wireNum(std::round((F64)mRadius->getValueF32()));
    out["area"] = area;
    out["indoors"] = mIndoors->getValue().asString();
    out["swim"] = mSwim->get();   // [ROUND 2] off = never walks into water
    out["gameOnly"] = mGameOnly->get();   // [ROUND 4] PROTOCOL.md §24
    if (mKind == "animal" && (mOwner->petsSupported() || mBase.has("noises")))
    {
        // [ROUND C] §27 noises {on, volume 0-1}; §26.1 travel only with behaviour "pet" (gated: a region without pets never
        // gets these keys from the window)
        LLSD noises = mBase["noises"].isMap() ? mBase["noises"] : LLSD::emptyMap();
        noises["on"] = mNoises->get();
        noises["volume"] = wireNum(std::round((F64)mNoiseVolume->getValueF32() * 100.0) / 100.0);
        out["noises"] = noises;
        const bool pet = out["behaviour"].asString() == "pet";
        if (pet || mBase.has("travel")) out["travel"] = pet && mTravel->get();
    }
    out["shy"] = wireNum(std::round((F64)mShy->getValueF32() * 100.0) / 100.0);
    out["visitors"] = mVisitors->getValue().asString();
    const std::string pack = trimmed(mPack->getText());
    if (utf8Chars(pack) > 24) { error = "The pack or group name can be at most 24 characters."; return false; }
    out["pack"] = pack;
    out["leader"] = mLeader->get();

    if (mKind == "person")
    {
        out["work"] = mWork->getValue().asString();
        if (mHasWorkAt) out["workAt"] = wireVec(mWorkAt);
        else if (mBase.has("workAt")) out["workAt"] = mBase["home"].isArray() ? mBase["home"] : mBase["workAt"];
        // Source: PROTOCOL.md section 3 routine [{at, do, place}], up to 12; do goto|work|home|sit|roam|rest
        LLSD routine = LLSD::emptyArray();
        std::vector<std::string> lines;
        LLStringUtil::getTokens(mRoutine->getText(), lines, "\n");
        S32 n = 0;
        for (std::string line : lines)
        {
            line = trimmed(line);
            if (line.empty()) continue;
            ++n;
            const size_t sp = line.find_first_of(" \t");
            const std::string at = line.substr(0, sp);
            const std::string what = sp == std::string::npos ? std::string() : trimmed(line.substr(sp + 1));
            const bool time_ok = at.size() == 5 && at[2] == ':' && isdigit((unsigned char)at[0]) && isdigit((unsigned char)at[1])
                                 && isdigit((unsigned char)at[3]) && isdigit((unsigned char)at[4])
                                 && std::stoi(at.substr(0, 2)) < 24 && std::stoi(at.substr(3, 2)) < 60;
            if (!time_ok || what.empty())
            {
                error = "Routine line " + std::to_string(n) + " (\"" + cap(line, 40) + "\"): write a time like 08:00, then a place or work, home, sit, roam or rest.";
                return false;
            }
            LLSD r = LLSD::emptyMap();
            r["at"] = at;
            const std::string w = lower(what);
            bool keyword = false;
            for (const char* d : ROUTINE_DOS) keyword = keyword || w == d;
            if (keyword) r["do"] = w;
            else
            {
                bool tool_role = false;   // [ROUND 4] §23.1 a tool's role works as a place name
                for (const LLSD& t : llsd::inArray(mBase["tools"])) tool_role = tool_role || t["role"].asString() == what;
                if (!mPlacesMap.has(what) && !tool_role)
                {
                    error = "Routine line " + std::to_string(n) + ": \"" + cap(what, 24) + "\" is not one of its places. Name it first on the Places tab (Edit).";
                    return false;
                }
                r["do"] = "goto";
                r["place"] = what;
            }
            routine.append(r);
        }
        if (routine.size() > MAX_ROUTINE) { error = "A routine can have at most 12 lines."; return false; }
        out["routine"] = routine;
    }

    // Game: kept only while it can fight (or it had game settings before, which are then kept with both switches off)
    // §26.1 "A travelling pet is never huntable"
    if (out["travel"].asBoolean() && mFight->get() && mHuntable->get()) { error = "A travelling pet cannot be huntable: untick Can be hunted in the Game section."; return false; }
    if (mFight->get() || mBase.has("game"))
    {
        LLSD g = mBase["game"].isMap() ? mBase["game"] : LLSD::emptyMap();
        g["huntable"] = mFight->get() && mHuntable->get();
        g["hunt"] = mFight->get() && mHunt->get();
        if (mFight->get())
        {
            g["health"] = wireNum(std::round((F64)mHealth->get()));
            g["respawn"] = wireNum(std::round((F64)mRespawn->get()));
            g["sight"] = wireNum(std::round((F64)mSight->get()));
            g["chase"] = wireNum(std::round((F64)mChase->get() * 10.0) / 10.0);
            g["giveUp"] = wireNum(std::round((F64)mGiveUp->get()));
            g["cooldown"] = wireNum(std::round((F64)mCooldown->get() * 10.0) / 10.0);
            LLSD dmg = LLSD::emptyMap();
            for (S32 i = 0; i < DAMAGE_ROWS; ++i)
            {
                const std::string stat = trimmed(mDamageStat[i]->getText());
                if (stat.empty()) continue;
                dmg[stat] = wireNum(std::round((F64)mDamageAmount[i]->get()));
            }
            if (mHunt->get() && dmg.size() == 0) { error = "A hunter needs the damage of a hit: give at least one stat name (e.g. health)."; return false; }
            g["damage"] = dmg;
        }
        out["game"] = g;
    }

    LLSD ai = mBase["ai"].isMap() ? mBase["ai"] : LLSD::emptyMap();
    ai["on"] = mAiOn->get();
    const std::string personality = mPersonality->getText();
    if (utf8Chars(personality) > 1000) { error = "The personality can be at most 1000 characters."; return false; }
    ai["personality"] = personality;
    ai["voice"] = mVoice->getValue().asString();
    ai["dailyLimit"] = wireNum(std::round((F64)mDailyLimit->get()));
    ai["npcTalk"] = mNpcTalk->get();
    out["ai"] = ai;

    const std::string vid = mVehicle->getValue().asString();
    if (!vid.empty() || mBase["vehicle"].has("id"))
    {
        LLSD v = mBase["vehicle"].isMap() ? mBase["vehicle"] : LLSD::emptyMap();
        v["id"] = vid;
        LLSD route = LLSD::emptyArray();
        for (const LLVector3& p : mRoute) route.append(wireVec(p));
        v["route"] = route;
        v["roads"] = mRoads->get();
        v["loop"] = mLoop->get();
        v["speed"] = wireNum(std::round((F64)mVehicleSpeed->get()));
        v["passenger"] = mKind == "animal" ? true : mPassenger->get();
        if (!vid.empty() && !mRoads->get() && mRoute.size() < 2)
        {
            error = "Give the vehicle a route of at least 2 points (Add where I stand), or tick Drive the region's roads.";
            return false;
        }
        out["vehicle"] = v;
    }
    return true;
}

LLSD WolfNPCForm::changedKeys(const LLSD& now) const
{
    LLSD changes = LLSD::emptyMap();
    for (auto it = now.beginMap(); it != now.endMap(); ++it)
    {
        if (!mFilledCfg.has(it->first) || !llsd_equals(it->second, mFilledCfg[it->first])) changes[it->first] = it->second;
    }
    return changes;
}

void WolfNPCForm::relayout()
{
    // The headings and open sections, top to bottom (heading 22 px, 4 px apart; an open body then 6 px).
    S32 y = mTopY;
    struct Place { LLView* v; S32 top; S32 h; };
    std::vector<Place> places;
    for (S32 i = 0; i < SECTIONS; ++i)
    {
        const bool shown = i != 2 || mKind == "person";   // Work and routine: people only (PROTOCOL.md section 3 work)
        mHead[i]->setVisible(shown);
        mBody[i]->setVisible(shown && mOpen[i]);
        if (!shown) continue;
        mHead[i]->setLabel(std::string(mOpen[i] ? "\xE2\x96\xBE  " : "\xE2\x96\xB8  ") + mHeadLabel[i]);   // U+25BE / U+25B8
        mHead[i]->setToggleState(mOpen[i]);
        places.push_back({ mHead[i], y, 22 });
        y += 26;
        if (mOpen[i])
        {
            S32 h = mBody[i]->getRect().getHeight();
            if (i == 3) h = GAME_TOP + (mFight->get() ? mGameDetails->getRect().getHeight() : 0);
            // [ROUND C GATE] Where and Character are as tall as round 4's unless their pet rows show (panel_wolf_npc_settings.xml)
            if (i == 0) h = mTravel->getVisible() ? 128 : 104;
            if (i == 1) h = mNoises->getVisible() ? 132 : 106;
            places.push_back({ mBody[i], y, h });
            y += h + 6;
        }
    }
    const S32 total = y + 8;
    const S32 w = mPanel->getRect().getWidth();
    LLRect pr = mPanel->getRect();
    mPanel->reshape(w, total, false);
    mPanel->setRect(LLRect(pr.mLeft, pr.mTop, pr.mLeft + w, pr.mTop - total));
    for (const Place& p : places)
    {
        const LLRect r = p.v->getRect();
        if (r.getHeight() != p.h) p.v->reshape(r.getWidth(), p.h, false);
        p.v->setOrigin(r.mLeft, total - p.top - p.h);
    }
    if (mOpen[3])
    {
        const LLRect d = mGameDetails->getRect();
        mGameDetails->setOrigin(d.mLeft, mBody[3]->getRect().getHeight() - GAME_TOP - d.getHeight());
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════════════════
// The window
// ════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterNPC::WolfFloaterNPC(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterNPC::~WolfFloaterNPC()
{
    if (mRegionChangedConnection.connected()) mRegionChangedConnection.disconnect();
    if (WolfNPC::instanceExists())
    {
        WolfNPC::instance().setChangedListener(nullptr);
        WolfNPC::instance().setGmEndedListener(nullptr);
    }
    LLFloaterWolfAvatars::setCharacterDoneListener(nullptr);
    delete mGame;
    delete mMind;
    delete mNewForm;
    delete mEditForm;
}

bool WolfFloaterNPC::postBuild()
{
    mTabs = getChild<LLTabContainer>("tabs");
    mNewTab = getChild<LLPanel>("tab_new");
    mRegionTab = getChild<LLPanel>("tab_region");
    mScenesTab = getChild<LLPanel>("tab_scenes");
    mTabs->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTabChanged(); });
    mStatus = getChild<LLTextBox>("status");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mBuyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuyCredits(); });
    mQuestion = getChild<LLPanel>("question");
    mQuestionText = mQuestion->getChild<LLTextBox>("question_text");
    mQuestionYes = mQuestion->getChild<LLButton>("question_yes");
    mQuestionNo = mQuestion->getChild<LLButton>("question_no");
    mQuestionYes->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        std::function<void()> go = mQuestionGo;
        closeQuestion();
        if (go) go();
    });
    mQuestionNo->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeQuestion(); });

    // ---- New NPC ----
    // [ROUND 2] three steps: start -> pick -> quick (More options holds the full form)
    mStartPanel = mNewTab->getChild<LLPanel>("start_panel");
    mPickPanel = mNewTab->getChild<LLPanel>("pick_panel");
    mQuickPanel = mNewTab->getChild<LLPanel>("quick_panel");
    mChosenText = mQuickPanel->getChild<LLTextBox>("chosen_text");
    mQuickName = mQuickPanel->getChild<LLLineEditor>("quick_name");
    mSays = mQuickPanel->getChild<LLLineEditor>("says");
    mQuickAi = mQuickPanel->getChild<LLCheckBoxCtrl>("quick_ai");
    mQuickPersonality = mQuickPanel->getChild<LLTextEditor>("quick_personality");
    mMoreBtn = mQuickPanel->getChild<LLButton>("more_btn");
    mNewScroll = mQuickPanel->getChild<LLScrollContainer>("new_scroll");
    {
        WolfNPCForm::Quick q;
        q.mName = mQuickName;
        q.mAiOn = mQuickAi;
        q.mPersonality = mQuickPersonality;
        q.mAiLine = mQuickPanel->getChild<LLTextBox>("quick_ai_line");
        mNewForm = new WolfNPCForm(this, mQuickPanel->getChild<LLPanel>("new_settings"), mNewScroll, false, &q);
    }
    mNewForm->setHomeText("In front of you, where it is rezzed");
    mMoreBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const bool open = !mNewScroll->getVisible();
        mNewScroll->setVisible(open);
        mMoreBtn->setToggleState(open);
        mMoreBtn->setLabel(std::string(open ? "\xE2\x96\xBE  Fewer options" : "\xE2\x96\xB8  More options"));
    });
    mMoreBtn->setLabel(std::string("\xE2\x96\xB8  More options"));
    mQuickPanel->getChild<LLButton>("chosen_change")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showNewStep(NEW_PICK); });
    mQuickPanel->getChild<LLButton>("quick_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showNewStep(NEW_PICK); });
    mPickPanel->getChild<LLButton>("pick_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showNewStep(NEW_START); });
    mStartPanel->getChild<LLButton>("use_mine_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mHasSpot = false; setPetMode(false); showNewStep(NEW_PICK); });
    mStartPanel->getChild<LLButton>("create_new_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setPetMode(false); onCreateNew(); });
    // [ROUND C] Make a pet (PROTOCOL.md §26.1): an animal WolfAvatar, a name, Follow me everywhere, personality optional
    // [WOLF PETS 2026-10-24] §29.4: Make a pet starts on Wear it (the default); Let it roam is the §26 create above
    mStartPanel->getChild<LLButton>("make_pet_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mHasSpot = false; setPetMode(true); showNewStep(NEW_PICK); });
    mQuickTravel = mQuickPanel->getChild<LLCheckBoxCtrl>("quick_travel");
    mPetHow = mQuickPanel->getChild<LLRadioGroup>("pet_how");
    mWearPanel = mQuickPanel->getChild<LLPanel>("wear_panel");
    mWearSide = mWearPanel->getChild<LLComboBox>("wear_side");
    mWearDistance = mWearPanel->getChild<LLSpinCtrl>("wear_distance");
    mWearResult = mWearPanel->getChild<LLTextBox>("wear_result");
    mPetHow->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        clearStatus();
        mWearResult->setText(LLStringUtil::null);
        setWearPet(mPetHow->getSelectedValue().asString() != "roam");
    });
    mNewWait = mStartPanel->getChild<LLTextBox>("new_wait");
    mNewRezBtn = mStartPanel->getChild<LLButton>("new_rez_btn");
    mNewRezBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onNewRez(); });
    mNewWaitStop = mStartPanel->getChild<LLButton>("new_wait_stop");
    mNewWaitStop->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        mWaitingNew = false;
        mHasSpot = false;
        mNewJob.clear();
        mNewWait->setText(LLStringUtil::null);
        mNewRezBtn->setVisible(false);
        mNewWaitStop->setVisible(false);
    });
    {
        LLHandle<LLFloater> h = getHandle();
        LLFloaterWolfAvatars::setCharacterDoneListener([h](const std::string& job, const std::string& kind, const std::string& name)
        {
            if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->onCharacterDone(job, kind, name);
        });
    }
    mGridScroll = mNewTab->getChild<LLScrollContainer>("grid_scroll");
    mGridHolder = mNewTab->getChild<LLPanel>("grid_holder");
    {
        WolfNPCAvatarGrid::Params p;
        p.name("avatar_grid");
        p.rect(mGridHolder->getLocalRect());
        p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        p.tab_stop(true);
        p.mouse_opaque(true);
        p.tool_tip("Your WolfAvatars: click one to choose it (or the arrow keys, then Enter)");
        mGrid = new WolfNPCAvatarGrid(p);
        mGridHolder->addChild(mGrid);
        mGrid->mOnPick = [this]() { onAvatarPicked(); showNewStep(NEW_QUICK); };
    }
    mAvatarsNote = mNewTab->getChild<LLTextBox>("avatars_note");
    mAvatarsMakeBtn = mNewTab->getChild<LLButton>("avatars_make");
    mAvatarsMakeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { openWolfAvatars(); });
    mNewTab->getChild<LLButton>("open_avatars")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mPetMode) onCreateNew();   // "Make a new animal": the WolfAvatars flow, its price first, back here for the pet
        else openWolfAvatars();
    });
    mCreateNote = mNewTab->getChild<LLTextBox>("create_note");
    mRezBtn = mNewTab->getChild<LLButton>("rez_btn");
    mRezBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRez(); });

    // ---- On this region ----
    mListView = mRegionTab->getChild<LLPanel>("list_view");
    mDetailView = mRegionTab->getChild<LLPanel>("detail_view");
    mCardScroll = mListView->getChild<LLScrollContainer>("npc_scroll");
    mCardHolder = mListView->getChild<LLPanel>("npc_holder");
    {
        WolfNPCCardGrid::Params p;
        p.name("npc_cards");
        p.rect(mCardHolder->getLocalRect());
        p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        p.tab_stop(true);
        p.mouse_opaque(true);
        mCards = new WolfNPCCardGrid(p);
        mCardHolder->addChild(mCards);
    }
    mCards->mOnOpen = [this]() { onEdit(); };
    mCards->mOnPick = [this]()
    {
        mSelected = mCards->mSelected;
        mRenameRow->setVisible(false);
        refreshActions();
    };
    mCountText = mListView->getChild<LLTextBox>("count_text");
    mMonthCost = mListView->getChild<LLTextBox>("month_cost");
    mListView->getChild<LLButton>("costs_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCostsPage(); });
    mSelNote = mListView->getChild<LLTextBox>("sel_note");
    mListView->getChild<LLButton>("refresh_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { clearStatus(); requestList(); });
    mEditBtn = mListView->getChild<LLButton>("edit_btn");
    mEditBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEdit(); });
    mRenameBtn = mListView->getChild<LLButton>("rename_btn");
    mRenameBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRename(); });
    mPauseBtn = mListView->getChild<LLButton>("pause_btn");
    mPauseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPauseResume(); });
    mHomeBtn = mListView->getChild<LLButton>("home_btn");
    mHomeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD n = selectedNpc();
        if (n.isMap()) sendCmd(n["id"].asString(), "home", "", cap(n["name"].asString(), 48) + " is walking home.");
    });
    mHereBtn = mListView->getChild<LLButton>("here_btn");
    mHereBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD n = selectedNpc();
        if (n.isMap()) sendCmd(n["id"].asString(), "here", "", "Where " + cap(n["name"].asString(), 48) + " stands is its home now.");
    });
    mStopBtn = mListView->getChild<LLButton>("stop_btn");
    mStopBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD n = selectedNpc();
        if (n.isMap()) sendCmd(n["id"].asString(), "stop", "", cap(n["name"].asString(), 48) + " stands still.");
    });
    // [ROUND C] §26.5 "Send home" (cmd sendhome): its owner, the region's holders, the parcel's owner - the region decides
    mSendHomeBtn = mListView->getChild<LLButton>("sendhome_btn");
    mSendHomeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLSD n = selectedNpc();
        if (n.isMap()) sendCmd(n["id"].asString(), "sendhome", "", cap(n["name"].asString(), 48) + " is on its way home.");
    });
    for (const char* which : { "rules_region_pets", "rules_region_noises", "rules_parcel_pets", "rules_parcel_noises" })
    {
        LLCheckBoxCtrl* cb = mListView->getChild<LLCheckBoxCtrl>(which);
        const bool parcel = std::string(which).find("parcel") != std::string::npos;
        cb->setCommitCallback([this, parcel](LLUICtrl*, const LLSD&) { sendRules(parcel); });
    }
    mDeleteBtn = mListView->getChild<LLButton>("delete_btn");
    mDeleteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDelete(); });
    mRenameRow = mListView->getChild<LLPanel>("rename_row");
    mRenameName = mRenameRow->getChild<LLLineEditor>("rename_name");
    mRenameRow->getChild<LLButton>("rename_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRenameSave(); });
    mRenameRow->getChild<LLButton>("rename_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mRenameRow->setVisible(false); });

    mDetailView->getChild<LLButton>("back_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showDetail(false); requestList(); });
    mDetailTitle = mDetailView->getChild<LLTextBox>("detail_title");
    mDetailState = mDetailView->getChild<LLTextBox>("detail_state");
    mDetailView->getChild<LLButton>("detail_sendhome")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        sendCmd(mDetailId, "sendhome", "", cap(mDetail["name"].asString(), 48) + " is on its way home.");
    });
    mParkBtn = mDetailView->getChild<LLButton>("park_btn");
    mParkBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { sendCmd(mDetailId, "park", "", "Parked."); });
    mExitBtn = mDetailView->getChild<LLButton>("exit_btn");
    mExitBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { sendCmd(mDetailId, "exit", "", "It has left the vehicle."); });
    mDetailTabs = mDetailView->getChild<LLTabContainer>("detail_tabs");
    mSettingsTab = mDetailView->getChild<LLPanel>("dtab_settings");
    mAnimsTab = mDetailView->getChild<LLPanel>("dtab_anims");
    mRulesTab = mDetailView->getChild<LLPanel>("dtab_rules");
    // [ROUND 3] Game (quests, dialogues, trader, pet, events, touch) and Game Master
    mGameTab = mDetailView->getChild<LLPanel>("dtab_game");
    mMindTab = mDetailView->getChild<LLPanel>("dtab_mind");
    mPlacesTab = mDetailView->getChild<LLPanel>("dtab_places");
    {
        LLHandle<LLFloater> h = getHandle();
        mMind = new WolfNPCMind(h, mMindTab->getChild<LLPanel>("mind_panel"), mPlacesTab->getChild<LLPanel>("places_panel"),
            [h](const LLSD& changes, const std::string& words, std::function<void(const LLSD&)> after)
            {
                if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->doUpdate(changes, words, false, after);
            },
            [h](const std::string& text, const std::string& yes, std::function<void()> go)
            {
                if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->ask(text, yes, go);
            },
            [h](const std::string& text, const LLSD& reply) { if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->setError(text, reply); },
            [h](const std::string& text, const LLSD&) { if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->setDone(text); });
    }
    mGmTab = mDetailView->getChild<LLPanel>("dtab_gm");
    {
        LLHandle<LLFloater> h = getHandle();
        mGame = new WolfNPCGame(mGameTab->getChild<LLPanel>("game_panel"),
            [h](const LLSD& changes, const std::string& words, std::function<void(const LLSD&)> after)
            {
                if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->doUpdate(changes, words, false, after);
            },
            [h](const std::string& text, const std::string& yes, std::function<void()> go)
            {
                if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->ask(text, yes, go);
            },
            [h](const std::string& text) { if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->setError(text); },
            [h](const std::string& text) { if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->setDone(text); });
    }
    mGmState = mGmTab->getChild<LLTextBox>("gm_state");
    mGmSay = mGmTab->getChild<LLLineEditor>("gm_say");
    mGmWho = mGmTab->getChild<LLComboBox>("gm_who");
    mGmAnim = mGmTab->getChild<LLComboBox>("gm_anim");
    mGmEvent = mGmTab->getChild<LLComboBox>("gm_event");
    mGmTab->getChild<LLButton>("gm_take")->setCommitCallback([this](LLUICtrl*, const LLSD&) { gmSend("gm_take", LLSD::emptyMap(), std::string()); });
    mGmTab->getChild<LLButton>("gm_release")->setCommitCallback([this](LLUICtrl*, const LLSD&) { gmSend("gm_release", LLSD::emptyMap(), std::string()); });
    auto say = [this]()
    {
        const std::string text = trimmed(mGmSay->getText());
        // Source: §19 gm_say text 1-1000
        if (text.empty() || utf8Chars(text) > 1000) { setError("Type what it says (1 to 1000 characters)."); return; }
        LLSD f;
        f["text"] = text;
        gmSend("gm_say", f, "Said.");
        mGmSay->setText(LLStringUtil::null);
    };
    mGmTab->getChild<LLButton>("gm_say_btn")->setCommitCallback([say](LLUICtrl*, const LLSD&) { say(); });
    mGmSay->setCommitCallback([say](LLUICtrl*, const LLSD&) { say(); });
    mGmTab->getChild<LLButton>("gm_click")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLHandle<LLFloater> h = getHandle();
        setDone("Click the ground (or an object) where it should walk. Escape cancels.");
        // Source: wolffloateradventuregm.h WolfToolAdventurePick - the next left click in the world, then the old tool back
        WolfToolAdventurePick::instance().start([h](const LLVector3d& global)
        {
            WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
            LLViewerRegion* rgn = gAgent.getRegion();
            if (!self || !rgn) return;
            LLSD f;
            f["pos"] = wireVec(rgn->getPosRegionFromGlobal(global));
            self->gmSend("gm_goto", f, "On its way there.");
        }, false);
    });
    mGmTab->getChild<LLButton>("gm_here")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLViewerRegion* rgn = gAgent.getRegion();
        if (!rgn) return;
        LLSD f;
        f["pos"] = wireVec(rgn->getPosRegionFromAgent(gAgent.getPositionAgent()));
        gmSend("gm_goto", f, "On its way to you.");
    });
    mGmTab->getChild<LLButton>("gm_face_me")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLSD f;
        f["who"] = gAgent.getID().asString();
        gmSend("gm_face", f, "It turns to you.");
    });
    mGmTab->getChild<LLButton>("gm_people")->setCommitCallback([this](LLUICtrl*, const LLSD&) { gmPeople(); });
    auto who = [this]() -> std::string { return mGmWho->getValue().asString(); };
    mGmTab->getChild<LLButton>("gm_follow")->setCommitCallback([this, who](LLUICtrl*, const LLSD&)
    {
        if (who().empty()) { setError("Choose someone first (Find people near)."); return; }
        LLSD f;
        f["who"] = who();
        gmSend("gm_follow", f, "It follows them.");
    });
    mGmTab->getChild<LLButton>("gm_face")->setCommitCallback([this, who](LLUICtrl*, const LLSD&)
    {
        if (who().empty()) { setError("Choose someone first (Find people near)."); return; }
        LLSD f;
        f["who"] = who();
        gmSend("gm_face", f, "It turns to them.");
    });
    mGmTab->getChild<LLButton>("gm_unfollow")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLSD f;
        f["who"] = "";
        gmSend("gm_follow", f, "It stops following.");
    });
    for (const char* mode : { "once", "loop", "stop" })
    {
        const std::string m(mode);
        mGmTab->getChild<LLButton>("gm_anim_" + m)->setCommitCallback([this, m](LLUICtrl*, const LLSD&)
        {
            const std::string a = trimmed(mGmAnim->getSimple());
            if (a.empty()) { setError("Choose or type an animation first."); return; }
            LLSD f;
            f["anim"] = a;
            f["mode"] = m;
            gmSend("gm_anim", f, m == "stop" ? "Stopped " + a + "." : m == "loop" ? "Looping " + a + "." : "Playing " + a + ".");
        });
    }
    for (const char* which : { "start", "end" })
    {
        const bool end = std::string(which) == "end";
        mGmTab->getChild<LLButton>(std::string("gm_event_") + which)->setCommitCallback([this, end](LLUICtrl*, const LLSD&)
        {
            const std::string ev = mGmEvent->getValue().asString();
            if (ev.empty()) { setError("It has no events yet (Game > Events)."); return; }
            LLSD f;
            f["event"] = ev;
            f["end"] = end;
            gmSend("gm_event", f, end ? "Event ended." : "Event started.");
        });
    }
    mEditForm = new WolfNPCForm(this, mSettingsTab->getChild<LLPanel>("edit_settings"), mSettingsTab->getChild<LLScrollContainer>("edit_scroll"), true);
    mSaveBtn = mSettingsTab->getChild<LLButton>("save_btn");
    mSaveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSaveSettings(); });
    mAnims = mAnimsTab->getChild<LLScrollListCtrl>("anims");
    mAnimsTab->getChild<LLButton>("anim_play")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAnim("anim"); });
    mAnimsTab->getChild<LLButton>("anim_loop")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAnim("anim_loop"); });
    mAnimsTab->getChild<LLButton>("anim_stop")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAnim("anim_stop"); });

    mRules = mRulesTab->getChild<LLScrollListCtrl>("rules");
    mRules->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const bool sel = mRules->getFirstSelected() != nullptr;
        mRuleChangeBtn->setEnabled(sel);
        mRuleRemoveBtn->setEnabled(sel);
    });
    mRules->setDoubleClickCallback([this]() { editRule(mRules->getFirstSelectedIndex()); });
    mRulesCount = mRulesTab->getChild<LLTextBox>("rules_count");
    mRuleNewBtn = mRulesTab->getChild<LLButton>("rule_new");
    mRuleNewBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { editRule(-1); });
    mRuleChangeBtn = mRulesTab->getChild<LLButton>("rule_change");
    mRuleChangeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { editRule(mRules->getFirstSelectedIndex()); });
    mRuleRemoveBtn = mRulesTab->getChild<LLButton>("rule_remove");
    mRuleRemoveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleRemove(); });
    mRuleEditor = mRulesTab->getChild<LLPanel>("rule_editor");
    mRuleWhen = mRuleEditor->getChild<LLComboBox>("rule_when");
    fillCombo(mRuleWhen, WHENS);
    mRuleRange = mRuleEditor->getChild<LLSliderCtrl>("rule_range");
    mRuleWho = mRuleEditor->getChild<LLComboBox>("rule_who");
    mRuleWho->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleWhoChanged(); });
    mRuleNot = mRuleEditor->getChild<LLCheckBoxCtrl>("rule_not");
    mRuleWhoValue = mRuleEditor->getChild<LLLineEditor>("rule_who_value");
    mRulePerson = mRuleEditor->getChild<LLComboBox>("rule_person");
    mRulePeopleBtn = mRuleEditor->getChild<LLButton>("rule_people");
    mRulePeopleBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestPeople(); });
    mRuleAction = mRuleEditor->getChild<LLComboBox>("rule_action");
    fillCombo(mRuleAction, ACTIONS);
    mRuleAction->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleActionChanged(); });
    mRuleActionValue = mRuleEditor->getChild<LLComboBox>("rule_action_value");
    mRuleAddActionBtn = mRuleEditor->getChild<LLButton>("rule_add_action");
    mRuleAddActionBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleAddAction(); });
    mRuleActions = mRuleEditor->getChild<LLScrollListCtrl>("rule_actions");
    mRuleRemoveActionBtn = mRuleEditor->getChild<LLButton>("rule_remove_action");
    mRuleRemoveActionBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleRemoveAction(); });
    mRuleOften = mRuleEditor->getChild<LLComboBox>("rule_often");
    fillCombo(mRuleOften, OFTENS);
    mRuleOften->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleOftenChanged(); });
    mRuleCooldown = mRuleEditor->getChild<LLSpinCtrl>("rule_cooldown");
    mRuleGap = mRuleEditor->getChild<LLSpinCtrl>("rule_gap");
    mRuleError = mRuleEditor->getChild<LLTextBox>("rule_error");
    mRuleEditor->getChild<LLButton>("rule_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeRuleEditor(); });
    mRuleEditor->getChild<LLButton>("rule_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRuleSave(); });

    // ---- Scenes ----
    mSceneList = mScenesTab->getChild<LLScrollListCtrl>("scene_list");
    mSceneList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSceneSelected(); });
    mSceneBlurb = mScenesTab->getChild<LLTextBox>("scene_blurb");
    mSceneCost = mScenesTab->getChild<LLTextBox>("scene_cost");
    mSceneStartBtn = mScenesTab->getChild<LLButton>("scene_start");
    mSceneStartBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSceneStart(); });
    mScenePanel = mScenesTab->getChild<LLPanel>("scene_panel");
    mSceneStep = mScenePanel->getChild<LLTextBox>("scene_step");
    mScenePiece = mScenePanel->getChild<LLTextBox>("scene_piece");
    mSceneAvatar = mScenePanel->getChild<LLComboBox>("scene_avatar");
    mSceneName = mScenePanel->getChild<LLLineEditor>("scene_name");
    mSceneRezBtn = mScenePanel->getChild<LLButton>("scene_rez");
    mSceneRezBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSceneRez(); });
    mSceneSkipBtn = mScenePanel->getChild<LLButton>("scene_skip");
    mSceneSkipBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSceneSkip(); });
    mSceneStopBtn = mScenePanel->getChild<LLButton>("scene_stop");
    mSceneStopBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSceneStop(); });
    mSceneNone = mScenePanel->getChild<LLTextBox>("scene_none");
    mSceneMakeBtn = mScenePanel->getChild<LLButton>("scene_make");
    mSceneMakeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { openWolfAvatars(); });
    mSceneStartBtn->setEnabled(false);

    mRegionChangedConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
    setDefaultBtn(mRezBtn);
    showNewStep(NEW_START);
    refreshActions();

    // [ROUND 4 LOOK] The Campfire behind every control, then the drag handle behind it, so the sky drags the window.
    // Source: llfloaterwolfavatars.cpp:1030-1040 (the WolfAvatarsBackdrop set-up)
    WolfNPCBackdrop::Params bp;
    bp.name("wolf_npc_backdrop");
    bp.rect(getLocalRect());
    bp.follows.flags(FOLLOWS_ALL);
    bp.mouse_opaque(false);
    mBackdrop = new WolfNPCBackdrop(bp, this);
    addChild(mBackdrop);
    sendChildToBack(mBackdrop);
    if (getDragHandle()) sendChildToBack(getDragHandle());
    recolourControls(this);
    collectPainted(this);
    applyPetSupport();   // [ROUND C GATE] rounds 4 + B until a hello with "rules" arrives
    return true;
}

void WolfFloaterNPC::onOpen(const LLSD& key)
{
    // Wolf Territories only: the menu item is hidden elsewhere, and this closes the window if it is opened another way.
    // Source: llfloaterwolfavatars.cpp:1177-1185 onOpen
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    WolfNPC::instance().setChangedListener([h = getHandle()](const std::string& id)
    {
        if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->onChangedPush(id);
    });
    WolfNPC::instance().setGmEndedListener([h = getHandle()](const std::string& id, const std::string& why)
    {
        if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->onGmEnded(id, why);
    });
    clearStatus();
    sendHello();
    loadAvatars();
    loadAiInfo(nullptr);   // the prices, pounds rate and buy link (the live balance is asked again before anything costs)
    onTabChanged();
}

// ─── [ROUND 2] New NPC in 3 clicks ───────────────────────────────────────────────────────────────────

bool WolfFloaterNPC::petsSupported() const
{
    // [ROUND C GATE] PROTOCOL.md §26.5: a region with travelling pets, noises and their rules says so with hello "rules";
    // the frozen region set of this release never sends it, and the window then looks and works as rounds 4 + B
    return mHello.isMap() && mHello["rules"].isMap();
}

void WolfFloaterNPC::applyPetSupport()
{
    const bool pets = petsSupported();
    LLButton* mine = mStartPanel->getChild<LLButton>("use_mine_btn");
    LLButton* fresh = mStartPanel->getChild<LLButton>("create_new_btn");
    // [WOLF PETS 2026-10-24] the three cards on every Wolf region (the 16 / 206 / 396 grid, floater_wolf_npc.xml): Make a pet
    // is Wear it (PROTOCOL.md §29) where the region has no pets, and needs nothing of the region
    mine->setLabel(std::string("Use my avatar"));
    mine->setFont(LLFontGL::getFontSansSerifBold());
    fresh->setFont(LLFontGL::getFontSansSerifBold());
    mStartPanel->getChild<LLTextBox>("create_new_note")->setText(std::string(pets
        ? "Create a new one opens WolfAvatars (it shows the price first); when it is made it comes back here. Make a pet: wear one of your animals and it walks beside you everywhere, or let it roam here as an NPC that follows you about. Nothing here needs Game Mode."
        : "Create a new one opens WolfAvatars (it shows the price first); when it is made it comes back here. Make a pet: wear one of your animals and it walks beside you everywhere you go. Nothing here needs Game Mode."));
    if (mPetMode)
    {
        // Let it roam is offered only while the region offers pets (setWearPet); a roaming pet begun on such a region becomes
        // one to wear on a region without them
        const bool was_roam = !mWearPet;
        setWearPet(mWearPet);
        if (was_roam && mWearPet) setDone("This region does not offer roaming pets, so your pet will be one you wear.");
    }
    // the region tab: the cards as tall as round 4's when there are no rules rows
    const LLRect sr = mCardScroll->getRect();
    const S32 h = pets ? 276 : 330;
    if (sr.getHeight() != h)
    {
        mCardScroll->setRect(LLRect(sr.mLeft, sr.mTop, sr.mRight, sr.mTop - h));
        fillList();
    }
    mNewForm->refreshPetSupport();
    mEditForm->refreshPetSupport();
    refreshActions();
}

void WolfFloaterNPC::setPetMode(bool pet)
{
    mPetMode = pet;
    // the quick step: a pet has "Follow me everywhere" where a greeter has its welcome line
    for (const char* n : { "says_lbl", "says", "says_note" }) mQuickPanel->getChild<LLView>(n)->setVisible(!pet);
    if (pet) mQuickTravel->set(true);
    mQuickPanel->getChild<LLTextBox>("step2_lbl")->setText(std::string(pet ? "2. Your pet" : "2. What does it do?"));
    mWearResult->setText(LLStringUtil::null);
    setWearPet(true);   // [WOLF PETS] PROTOCOL.md §29.4 "Wear it" (default); a greeter hides the pet rows
    buildGrid();
}

void WolfFloaterNPC::setWearPet(bool wear)
{
    // Source: PROTOCOL.md §29.4 "First choice: Wear it (default) or Let it roam ... Let it roam: the §26 create, as now".
    // Let it roam only where the region offers pets: petsSupported() is hello "rules", which the region sends only while
    // roaming pets are on (WolfNPCModule.Viewer.cs:318-324 NpcCfg.TravelAllowed); elsewhere the window keeps them hidden.
    const bool roam_ok = petsSupported();
    mWearPet = wear || !roam_ok;
    const bool wearing = mPetMode && mWearPet, roaming = mPetMode && !mWearPet;
    mPetHow->setVisible(mPetMode && roam_ok);
    mPetHow->setValue(LLSD(mWearPet ? "wear" : "roam"));
    // the wear rows take the choice's place when it is not shown (floater_wolf_npc.xml: pet_how top 86, wear_panel top 110)
    const S32 ph = mQuickPanel->getRect().getHeight(), top = mPetHow->getVisible() ? 110 : 86;
    const LLRect wr = mWearPanel->getRect();
    mWearPanel->setRect(LLRect(wr.mLeft, ph - top, wr.mRight, ph - top - wr.getHeight()));
    mWearPanel->setVisible(wearing);
    mQuickTravel->setVisible(roaming);
    mQuickPanel->getChild<LLView>("pet_note")->setVisible(roaming);
    // a worn pet is no NPC (§29.4): no AI talk, no NPC settings, no region count
    for (const char* n : { "quick_ai", "quick_personality", "quick_ai_line", "more_btn", "create_note" }) mQuickPanel->getChild<LLView>(n)->setVisible(!wearing);
    if (wearing && mNewScroll->getVisible())
    {
        mNewScroll->setVisible(false);
        mMoreBtn->setToggleState(false);
        mMoreBtn->setLabel(std::string("\xE2\x96\xB8  More options"));
    }
    // Source: wolfpet_lib.php:34 WPET_NAME_CHARS = 40 (wolfpet.py:88 NAME_MAX: "<name> (wear me)" within 63 characters)
    mQuickName->setMaxTextChars(wearing ? 40 : 48);
    mQuickName->setToolTip(std::string(wearing ? "Your pet's name, 1 to 40 characters. In your inventory it is \"<name> (wear me)\"."
                                               : "Shown in chat and in the list (1 to 48 characters)."));
    mRezBtn->setLabel(std::string(wearing ? "Make my pet" : mPetMode ? "Rez my pet" : "Rez"));
    if (mNewStep == NEW_QUICK && wearing) mRezBtn->setToolTip(std::string("It goes into your inventory: Objects > WolfPets. Free."));
    else if (mNewStep == NEW_QUICK && roaming) mRezBtn->setToolTip(std::string("It appears beside you"));
}

void WolfFloaterNPC::showNewStep(ENewStep step)
{
    mNewStep = step;
    mStartPanel->setVisible(step == NEW_START);
    mPickPanel->setVisible(step == NEW_PICK);
    mQuickPanel->setVisible(step == NEW_QUICK);
    if (step == NEW_QUICK)
    {
        const S32 i = mGrid->mSelected;
        if (i < 0 || i >= (S32)mGrid->mItems.size()) { showNewStep(NEW_PICK); return; }
        if (mPetMode && mWearPet) mRezBtn->setToolTip(std::string("It goes into your inventory: Objects > WolfPets. Free."));
        else if (mPetMode) mRezBtn->setToolTip(std::string("It appears beside you"));
        else if (mHasSpot) mRezBtn->setToolTip(std::string("It appears at the spot you chose before WolfAvatars made it"));
        else mRezBtn->setToolTip(std::string("It appears 2 m in front of you, facing you"));
        mQuickName->setFocus(true);
    }
    else if (step == NEW_PICK) mGrid->setFocus(true);
}

void WolfFloaterNPC::onCreateNew()
{
    clearStatus();
    if (mPetMode && mWearPet)
    {
        // [WOLF PETS] a pet to wear needs no spot: the new animal comes back here to be made into the pet (§29.4)
        LLHandle<LLFloater> h = getHandle();
        ask("Make a new animal in WolfAvatars? It shows its price in Wolf Credits before anything is spent. When it is made, it comes back here to become your pet.",
            "Make a new animal", [h]()
        {
            WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
            if (!self) return;
            self->mHasSpot = false;
            self->mWaitingNew = true;
            self->mNewJob.clear();
            self->mKnownJobs.clear();
            for (const LLSD& a : llsd::inArray(self->mAvatars)) self->mKnownJobs.insert(a["job"].asString());
            self->mNewWait->setText(std::string("Making a new animal in the WolfAvatars window. Describe it, get the price, pick a picture - when it is made it comes back here to become your pet."));
            self->mNewWait->setColor(normalText());
            self->mNewRezBtn->setVisible(false);
            self->mNewWaitStop->setVisible(true);
            self->openWolfAvatars();
        });
        return;
    }
    LLVector3 pos, agent;
    F32 facing = 0.f;
    if (!placeInFront(pos, facing, agent)) { setError("Your position is not known yet - try again in a moment."); return; }
    LLHandle<LLFloater> h = getHandle();
    // the spot is confirmed now, where they stand (Paul: "rez it ... at the spot they confirmed at the start")
    ask("Make a new WolfAvatar and put it here? WolfAvatars opens and shows its price in Wolf Credits before anything is spent. When it is made, it can be rezzed 2 m in front of where you stand now.",
        "Make a new one for here", [h, pos, facing]()
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || !gAgent.getRegion()) return;
        self->mHasSpot = true;
        self->mSpotRegion = gAgent.getRegion()->getRegionID();
        self->mSpotPos = pos;
        self->mSpotHome = pos;   // its home is where it is rezzed (as New NPC)
        self->mSpotFacing = facing;
        self->mWaitingNew = true;
        self->mNewJob.clear();
        self->mKnownJobs.clear();
        for (const LLSD& a : llsd::inArray(self->mAvatars)) self->mKnownJobs.insert(a["job"].asString());
        self->mNewWait->setText(std::string("Making a new WolfAvatar in the WolfAvatars window. Describe it, get the price, pick a picture - when it is made it comes back here to be rezzed at the spot you chose."));
        self->mNewWait->setColor(normalText());
        self->mNewRezBtn->setVisible(false);
        self->mNewWaitStop->setVisible(true);
        self->openWolfAvatars();
    });
}

void WolfFloaterNPC::onCharacterDone(const std::string& job, const std::string& kind, const std::string& name)
{
    if (!mWaitingNew || job.empty() || mKnownJobs.count(job) || !mNewJob.empty()) return;
    mWaitingNew = false;
    if (mPetMode && kind != "animal")
    {
        // §26.1 a pet is an animal; the person is kept in their WolfAvatars for another NPC
        mNewWait->setText((name.empty() ? std::string("Your new WolfAvatar") : name) + " is a person, and a pet must be an animal. It is in your WolfAvatars for another NPC; make an animal for your pet.");
        mNewWait->setColor(caution());
        mNewWaitStop->setVisible(false);
        loadAvatars();
        return;
    }
    mNewJob = job;
    mNewJobName = name.empty() ? std::string("Your new WolfAvatar") : name;
    if (mPetMode && mWearPet)
    {
        // [WOLF PETS] §29.4 Wear it
        mNewWait->setText(mNewJobName + " is made and is now one of your WolfAvatars. Make it your pet now, or later from Make a pet.");
        mNewRezBtn->setLabel("Make " + cap(mNewJobName, 30) + " my pet");
    }
    else
    {
        mNewWait->setText(mNewJobName + " is made and is now one of your WolfAvatars. Rez it as an NPC at the spot you chose, or choose it later from Use one of my avatars.");
        mNewRezBtn->setLabel("Rez " + cap(mNewJobName, 30) + " as an NPC");
    }
    mNewRezBtn->setVisible(true);
    mNewWaitStop->setVisible(true);
    loadAvatars();   // it is in their list now (npc_avatars), for this and for later
    if (getVisible())
    {
        mTabs->selectTabPanel(mNewTab);
        showNewStep(NEW_START);
    }
}

void WolfFloaterNPC::onNewRez()
{
    // the new avatar, chosen in the list (npc_avatars has it once its job is done), then the quick step at the saved spot
    for (S32 i = 0; i < (S32)mGrid->mItems.size(); ++i)
    {
        if (mGrid->mItems[(size_t)i].mJob != mNewJob) continue;
        mGrid->mSelected = i;
        onAvatarPicked();
        mNewRezBtn->setVisible(false);
        mNewWaitStop->setVisible(false);
        mNewWait->setText(LLStringUtil::null);
        LLViewerRegion* rgn = gAgent.getRegion();
        if (mHasSpot && (!rgn || rgn->getRegionID() != mSpotRegion))
        {
            mHasSpot = false;
            setDone("You are on another region now, so it will appear 2 m in front of you instead.");
        }
        showNewStep(NEW_QUICK);
        return;
    }
    setError("Your new WolfAvatar is not in your list yet. Wait a moment and press it again.");
    loadAvatars();
}

void WolfFloaterNPC::onClose(bool app_quitting)
{
    closeQuestion();
    if (!app_quitting && WolfNPC::instanceExists()) WolfNPC::instance().setChangedListener(nullptr);
}

void WolfFloaterNPC::draw()
{
    // Source: llfloaterwolfavatars.cpp:1177-1185 - Wolf Territories only: off Wolf (a hypergrid jump) the window goes
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    if (mRegionPending && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mRegionPending = false;
        sendHello();
        if (mTabs->getCurrentPanel() == mRegionTab) requestList();
    }
    // [ROUND B] a scene piece's brain settings, once their time has come (bounded retries, not a polling loop)
    for (size_t i = 0; i < mPendingBrain.size(); ++i)
    {
        if (nowSec() >= mPendingBrain[i].mAt) { applyBrain(i); break; }
    }
    // a "changed" push asked for the list again (once, a moment later: several pushes in a row make one request)
    if (mListWantAt > 0.0 && nowSec() >= mListWantAt)
    {
        mListWantAt = 0.0;
        requestList();
    }
    LLFloater::draw();
}

bool WolfFloaterNPC::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE && mask == MASK_NONE && mQuestion->getVisible())
    {
        closeQuestion();
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

// ─── [ROUND 4 LOOK] The Campfire ────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::recolourControls(LLView* view)
{
    for (LLView* child : *view->getChildList())
    {
        if (LLCheckBoxCtrl* cb = dynamic_cast<LLCheckBoxCtrl*>(child))   // radio buttons too (LLRadioCtrl is one)
        {
            cb->setEnabledColor(LLUIColor(NPC_CREAM));
            cb->setDisabledColor(LLUIColor(LLColor4(0.62f, 0.62f, 0.66f, 1.f)));   // on the plate 6.9:1
        }
        else if (LLSliderCtrl* sl = dynamic_cast<LLSliderCtrl*>(child))
        {
            sl->setLabelColor(LLUIColor(NPC_CREAM));
            sl->setDisabledLabelColor(LLUIColor(LLColor4(0.62f, 0.62f, 0.66f, 1.f)));
        }
        else if (LLSpinCtrl* sp = dynamic_cast<LLSpinCtrl*>(child))
        {
            sp->setLabelColor(LLUIColor(NPC_CREAM));
            sp->setDisabledLabelColor(LLUIColor(LLColor4(0.62f, 0.62f, 0.66f, 1.f)));
        }
        recolourControls(child);
    }
}

void WolfFloaterNPC::collectPainted(LLView* view)
{
    for (LLView* child : *view->getChildList())
    {
        // the skin's own inner buttons are left alone: a combo's arrow, a spinner's arrows, a check box's box, a scroll
        // bar's, a list's column headers, the tab strip's tabs (direct children of the tab container) and the floater's
        // title bar buttons (direct children of the floater, but for buy_btn)
        if (dynamic_cast<LLComboBox*>(child) || dynamic_cast<LLSpinCtrl*>(child) || dynamic_cast<LLCheckBoxCtrl*>(child)
            || dynamic_cast<LLScrollbar*>(child) || dynamic_cast<LLScrollListCtrl*>(child) || dynamic_cast<LLSliderCtrl*>(child)
            || dynamic_cast<LLLineEditor*>(child) || dynamic_cast<LLTextEditor*>(child))
        {
            continue;
        }
        if (LLButton* b = dynamic_cast<LLButton*>(child))
        {
            if (dynamic_cast<LLTabContainer*>(view)) continue;
            if (view == this && b->getName() != "buy_btn") continue;
            Painted p;
            p.mButton = b;
            const std::string n = b->getName();
            static const std::set<std::string> PRIMARY = {
                "rez_btn", "scene_rez", "save_btn", "new_rez_btn", "scene_start", "rule_save", "rename_save", "q_save", "d_save", "s_save",
                "e_save", "t_save", "sk_save", "gm_take", "k_teach_btn", "f_find", "pl_save", "tl_link", "f_adopt", "w_make", "gm_say_btn",
                "p_link", "n_save", "k_role_save", "buy_btn", "question_yes", "avatars_make", "scene_make", "g_quote", "g_style_on" };
            static const std::set<std::string> DANGER = {
                "delete_btn", "k_forget", "quest_remove", "dlg_remove", "ev_remove", "sk_delete", "rule_remove", "tl_unlink", "pl_remove",
                "k_delete", "p_unlink" };
            if (n == "use_mine_btn" || n == "create_new_btn" || n == "make_pet_btn") p.mRole = BTN_CARD;
            else if (n.rfind("head_", 0) == 0) p.mRole = BTN_HEAD;
            else if (PRIMARY.count(n)) p.mRole = BTN_PRIMARY;
            else if (DANGER.count(n)) p.mRole = BTN_DANGER;
            for (LLView* up = b->getParent(); up && up != this; up = up->getParent())
            {
                if (dynamic_cast<LLScrollContainer*>(up)) { p.mClip = up; break; }
            }
            mPainted.push_back(p);
            continue;
        }
        collectPainted(child);
    }
}

void WolfFloaterNPC::paintButton(const Painted& p, F32 t)
{
    LLButton* b = p.mButton;
    if (!b || !b->isInVisibleChain()) return;
    const LLRect r = rectIn(b, mBackdrop);
    if (r.getWidth() < 8 || r.getHeight() < 8) return;
    const bool enabled = b->isInEnabledChain();
    const bool pressed = enabled && b->hasMouseCapture() && mouseOver(b);
    const bool hover = enabled && !pressed && mouseOver(b);
    const bool focus = b->hasFocus();
    const bool toggled = b->getToggleState();
    EButtonRole role = p.mRole;
    if (b == mQuestionYes && mQuestionDanger) role = BTN_DANGER;
    const F32 rad = role == BTN_CARD ? 12.f : role == BTN_HEAD ? 5.f : 6.f;
    untextured();
    LLRect shadow = r;
    shadow.translate(1, -3);
    fillRounded(shadow, allCorners(rad), CF_SHADOW, CF_SHADOW);
    if (!enabled)
    {
        fillRounded(r, allCorners(rad), CF_STONE_OFF, CF_STONE_OFF);
        strokeRounded(r, allCorners(rad), 0.f, 1.f, withAlpha(CF_STONE_EDGE, 0.4f), withAlpha(CF_STONE_EDGE, 0.4f));
        return;
    }
    LLColor4 top = CF_STONE_TOP, bottom = CF_STONE_BOTTOM, edge = CF_STONE_EDGE;
    if (role == BTN_PRIMARY) { top = CF_EMBER_TOP; bottom = CF_EMBER_BOTTOM; edge = CF_FLAME; }
    else if (role == BTN_DANGER) { top = CF_DANGER_TOP; bottom = CF_DANGER_BOTTOM; edge = CF_ERR_EDGE; }
    else if (role == BTN_HEAD && toggled) { top = lighten(CF_STONE_TOP, 0.05f); edge = CF_FIRE; }
    if (hover) { top = lighten(top, 0.05f); bottom = lighten(bottom, 0.04f); }
    if (pressed) { top = lighten(top, -0.06f); bottom = lighten(bottom, -0.05f); }
    if (role == BTN_PRIMARY) halo(r, rad, 8, withAlpha(CF_GLOW, (hover ? 0.75f : 0.45f) + 0.15f * sinf(t * 2.2f)));
    else if (hover || focus) halo(r, rad, 6, withAlpha(role == BTN_DANGER ? CF_ERR_EDGE : CF_FIREFLY, 0.35f));
    fillRounded(r, allCorners(rad), top, bottom);
    strokeRounded(r, allCorners(rad), 0.f, focus || hover ? 2.f : 1.f, edge, withAlpha(edge, 0.7f));
    if (role == BTN_PRIMARY)
    {
        const LLRect sheen(r.mLeft + 3, r.mTop - 2, r.mRight - 3, r.getCenterY());
        fillRounded(sheen, Radii{ rad - 2.f, rad - 2.f, 0.f, 0.f }, withAlpha(LLColor4::white, 0.18f), withAlpha(LLColor4::white, 0.f));
    }
    if (role == BTN_CARD)
    {
        // the two first choices: a wolf and a person from your own collection, or a new one appearing in a spark
        const F32 cx = (F32)r.mLeft + 32.f, base = (F32)r.mBottom + 12.f;
        ellipse(cx, base, 30.f, 6.f, withAlpha(CF_GLOW, 0.45f), CF_CLEAR);
        if (b->getName() == "make_pet_btn")
        {
            // a small dog trotting at a person's heel
            drawPerson(cx + 10.f, base, 42.f, LLColor4(0.70f, 0.76f, 0.72f, 1.f));
            drawWolf(cx - 14.f, base, 22.f, LLColor4(0.86f, 0.70f, 0.50f, 1.f));
        }
        else if (b->getName() == "use_mine_btn")
        {
            drawWolf(cx - 8.f, base, 34.f, LLColor4(0.70f, 0.76f, 0.72f, 1.f));
            drawPerson(cx + 18.f, base, 42.f, LLColor4(0.70f, 0.76f, 0.72f, 1.f));
        }
        else
        {
            const F32 pulse = 0.7f + 0.3f * sinf(t * 2.6f);
            disc(cx, base + 22.f, 22.f, withAlpha(CF_FLAME, 0.30f * pulse), CF_CLEAR);
            sparkle(cx, base + 22.f, 6.f, withAlpha(CF_FLAME, pulse));
            sparkle(cx - 16.f, base + 34.f, 2.5f, withAlpha(CF_FLAME, 0.8f));
            sparkle(cx + 15.f, base + 10.f, 2.f, withAlpha(CF_FLAME, 0.7f));
        }
    }
    else if (role == BTN_HEAD)
    {
        // an open section says so in its mark (the label's arrow) and an ember edge on its left
        if (toggled) fillRounded(LLRect(r.mLeft, r.mTop, r.mLeft + 4, r.mBottom), allCorners(2.f), CF_FIRE, CF_EMBER);
    }
}

void WolfFloaterNPC::drawBanner(F32 t)
{
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const F32 top = H, bottom = H - 84.f;   // under the title bar (18) and the banner (64): where the tabs start
    // the sky, the stars that twinkle, the crescent moon
    box(0.f, top, W, bottom, CF_SKY_TOP, CF_SKY_LOW);
    for (S32 i = 0; i < 40; ++i)
    {
        const F32 x = hash01((F32)i * 1.71f + 0.2f) * W, y = bottom + 26.f + hash01((F32)i * 2.93f + 4.1f) * (top - bottom - 30.f);
        const F32 tw = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * (0.8f + hash01((F32)i * 3.3f) * 1.6f) + (F32)i));
        disc(x, y, 1.1f, withAlpha(CF_MOON, 0.65f * tw), CF_CLEAR);
    }
    const F32 mx = W * 0.62f, my = top - 30.f;
    disc(mx, my, 26.f, withAlpha(CF_MOON, 0.10f), CF_CLEAR);
    disc(mx, my, 11.f, CF_MOON, lerpColour(CF_MOON, CF_SKY_LOW, 0.15f));
    disc(mx + 5.f, my + 3.f, 10.f, CF_SKY_TOP, CF_SKY_TOP);   // the crescent
    // far hills, then the pines along the forest edge
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < 12; ++i)
    {
        const F32 x0 = W * (F32)i / 12.f, x1 = W * (F32)(i + 1) / 12.f;
        const F32 h0 = 14.f + 12.f * hash01((F32)i * 4.7f), h1 = 14.f + 12.f * hash01((F32)(i + 1) * 4.7f);
        vtx(x0, bottom, CF_HILL_FAR); vtx(x1, bottom, CF_HILL_FAR); vtx(x1, bottom + h1, CF_HILL_FAR);
        vtx(x0, bottom, CF_HILL_FAR); vtx(x1, bottom + h1, CF_HILL_FAR); vtx(x0, bottom + h0, CF_HILL_FAR);
    }
    gGL.end();
    for (S32 i = 0; i < 26; ++i)
    {
        const F32 x = (F32)i * (W / 25.f) + (hash01((F32)i * 6.1f) - 0.5f) * 14.f;
        if (x > W - 190.f && x < W - 10.f) continue;   // the clearing round the fire
        drawPine(x, bottom - 2.f, 26.f + 22.f * hash01((F32)i * 8.3f + 1.f), CF_PINES);
    }
    box(0.f, bottom + 6.f, W, bottom, CF_PINES, CF_PINES);
    // the campfire in the clearing, a wolf and a person in its light (their firelit side a warm rim)
    const F32 fx = W - 96.f, fy = bottom + 8.f;
    ellipse(fx, fy + 10.f, 110.f, 46.f, withAlpha(CF_GLOW, 0.13f + 0.03f * sinf(t * 5.f)), CF_CLEAR);
    drawWolf(fx - 52.f + 1.5f, fy, 34.f, withAlpha(CF_FIRE, 0.55f));
    drawWolf(fx - 52.f, fy, 34.f, CF_FIGURE);
    drawPerson(fx + 46.f - 1.5f, fy, 46.f, withAlpha(CF_FIRE, 0.55f));
    drawPerson(fx + 46.f, fy, 46.f, CF_FIGURE);
    drawFire(fx, fy + 2.f, 7.f, t);
    // fireflies drifting at the forest edge
    for (S32 i = 0; i < 9; ++i)
    {
        const F32 h1 = hash01((F32)i * 2.37f + 0.9f), h2 = hash01((F32)i * 5.53f + 0.4f);
        const F32 x = W * (0.05f + 0.55f * h1) + 18.f * sinf(t * (0.4f + 0.5f * h2) + (F32)i);
        const F32 y = bottom + 10.f + 28.f * h2 + 8.f * sinf(t * (0.7f + h1) + (F32)i * 1.3f);
        const F32 a = 0.5f + 0.5f * sinf(t * (1.5f + h2 * 2.f) + (F32)i * 2.1f);
        disc(x, y, 5.f, withAlpha(CF_FIREFLY, 0.20f * a), CF_CLEAR);
        disc(x, y, 1.4f, withAlpha(CF_FIREFLY, 0.9f * a), withAlpha(CF_FIREFLY, 0.5f * a));
    }
    // the title, glowing in firelight (Cinzel, WolfDisplay), and what the window is for
    const std::string title("WolfNPC"), sub("Bring your characters to life");
    const LLFontGL* tf = fitDisplayFont(34.f, W * 0.45f, { title }, 6);
    const F32 tx = 18.f, ty = top - 40.f;
    static const F32 OFFSETS[4][2] = { { -1.5f, 0.f }, { 1.5f, 0.f }, { 0.f, -1.5f }, { 0.f, 1.5f } };
    for (const auto& o : OFFSETS) text(tf, title, tx + o[0], ty + o[1], withAlpha(CF_GLOW, 0.25f), LLFontGL::LEFT);
    text(tf, title, tx + 0.5f, ty - 1.f, LLColor4(0.f, 0.f, 0.f, 0.6f), LLFontGL::LEFT);
    text(tf, title, tx, ty, CF_TITLE, LLFontGL::LEFT);   // CF_TITLE on the sky (about #0a1721): 12.7:1
    text(LLFontGL::getFontSansSerifSmall(), sub, tx + 2.f, ty - 22.f, NPC_CREAM, LLFontGL::LEFT, W * 0.45f);   // 15.2:1
}

void WolfFloaterNPC::drawScene()
{
    const S32 W = getRect().getWidth(), H = getRect().getHeight();
    if (W < 200 || H < 200) return;
    const F32 t = animSeconds();
    LLGLSUIDefault ui;
    untextured();
    // the forest floor under everything, the fire's light reaching it from above
    box(0.f, (F32)H, (F32)W, 0.f, CF_FLOOR_TOP, CF_FLOOR_BOTTOM);
    ellipse((F32)W - 96.f, (F32)H - 84.f, (F32)W * 0.6f, 120.f, withAlpha(CF_GLOW, 0.06f), CF_CLEAR);
    drawBanner(t);
    // the body plate under the tabs' pages (the tab strip itself stays on the floor)
    {
        const LLRect tabs = rectIn(mTabs, mBackdrop);
        const LLRect plate(tabs.mLeft, tabs.mTop - 22, tabs.mRight, tabs.mBottom);
        fillRounded(plate, allCorners(10.f), CF_PLATE, CF_PLATE);
        strokeRounded(plate, allCorners(10.f), 0.f, 1.f, CF_PLATE_EDGE, withAlpha(CF_PLATE_EDGE, 0.5f));
    }
    // the cost panel: firelit wood behind the monthly estimate and its button; the scene's cost line too
    for (LLView* v : { (LLView*)mMonthCost, (LLView*)mSceneCost })
    {
        if (!v || !v->isInVisibleChain() || v->getRect().getHeight() <= 0) continue;
        LLRect r = rectIn(v, mBackdrop);
        if (v == mMonthCost) r.unionWith(rectIn(mListView->getChildView("costs_btn"), mBackdrop));
        r = grown(r, 4);
        untextured();
        fillRounded(r, allCorners(7.f), CF_LEDGER, CF_LEDGER);
        strokeRounded(r, allCorners(7.f), 0.f, 1.f, CF_LEDGER_EDGE, withAlpha(CF_LEDGER_EDGE, 0.6f));
    }
    // the status line on its own plate: an error on red with a warning mark (never colour alone: the words say it)
    if (mStatus->getVisible() && !mStatus->getText().empty())
    {
        const bool err = mStatusError;
        const LLRect s = rectIn(mStatus, mBackdrop);
        const LLRect plate(s.mLeft - (err ? 26 : 6), s.mTop + 3, s.mRight + 4, s.mBottom - 3);
        untextured();
        fillRounded(plate, allCorners(7.f), err ? CF_ERR_PLATE : CF_PLATE, err ? CF_ERR_PLATE : CF_PLATE);
        strokeRounded(plate, allCorners(7.f), 0.f, 1.f, err ? CF_ERR_EDGE : CF_PLATE_EDGE, err ? withAlpha(CF_ERR_EDGE, 0.7f) : withAlpha(CF_PLATE_EDGE, 0.5f));
        if (err)
        {
            const F32 x = (F32)plate.mLeft + 13.f, y = (F32)plate.mTop - 14.f;
            disc(x, y, 8.f, CF_ERR_EDGE, CF_ERR_EDGE);
            line(x, y + 4.5f, x, y - 1.f, 2.f, CF_ERR_PLATE);
            disc(x, y - 4.f, 1.2f, CF_ERR_PLATE, CF_ERR_PLATE);
        }
    }
    // the question, on an amber-edged plate across the bottom
    if (mQuestion->getVisible())
    {
        const LLRect q = grown(rectIn(mQuestion, mBackdrop), -2);
        untextured();
        fillRounded(q, allCorners(9.f), LLColor4(0.06f, 0.05f, 0.03f, 0.97f), LLColor4(0.04f, 0.03f, 0.02f, 0.97f));
        strokeRounded(q, allCorners(9.f), 0.f, 2.f, mQuestionDanger ? CF_ERR_EDGE : CF_FIRE, mQuestionDanger ? CF_ERR_EDGE : CF_EMBER);
    }
    // every button's face, under its own label (clipped to the scroll container it sits in)
    for (const Painted& p : mPainted)
    {
        if (p.mClip)
        {
            if (!p.mClip->isInVisibleChain()) continue;
            LLLocalClipRect clip(rectIn(p.mClip, mBackdrop));
            paintButton(p, t);
        }
        else paintButton(p, t);
    }
    untextured();
}

// ─── status, questions ──────────────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::setError(const std::string& text, const LLSD& reply)
{
    mStatus->setText(text);
    mStatus->setColor(caution());
    mStatusError = !text.empty();
    mBuyUrl = buyUrlOf(reply);
    mBuyBtn->setVisible(!mBuyUrl.empty());
}

void WolfFloaterNPC::setDone(const std::string& text)
{
    mStatus->setText(text);
    mStatus->setColor(normalText());
    mStatusError = false;
    mBuyUrl.clear();
    mBuyBtn->setVisible(false);
}

void WolfFloaterNPC::clearStatus()
{
    setDone(LLStringUtil::null);
}

void WolfFloaterNPC::ask(const std::string& text, const std::string& yes_label, std::function<void()> yes)
{
    mQuestionText->setText(text);
    mQuestionYes->setLabel(yes_label);
    // a yes that deletes, removes or forgets is painted as dangerous (the words say what it does as well)
    mQuestionDanger = yes_label.rfind("Delete", 0) == 0 || yes_label.rfind("Remove", 0) == 0 || yes_label.rfind("Forget", 0) == 0;
    mQuestionGo = yes;
    LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    mQuestionReturn = focus ? focus->getHandle() : LLHandle<LLView>();
    mQuestion->setVisible(true);
    mStatus->setVisible(false);
    mBuyBtn->setVisible(false);
    mQuestionNo->setFocus(true);
}

void WolfFloaterNPC::closeQuestion()
{
    if (!mQuestion || !mQuestion->getVisible()) return;
    mQuestion->setVisible(false);
    mQuestionGo = nullptr;
    mStatus->setVisible(true);
    mBuyBtn->setVisible(!mBuyUrl.empty());
    // focus goes back to what had it when the question came up
    if (LLUICtrl* back = dynamic_cast<LLUICtrl*>(mQuestionReturn.get()))
    {
        if (back->isInVisibleChain()) back->setFocus(true);
    }
    mQuestionReturn = LLHandle<LLView>();
}

void WolfFloaterNPC::onBuyCredits()
{
    // Source: llfloaterwolfavatars.cpp:1895-1901 onBuyCredits - only a Wolf Territories page is opened
    if (mBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuyUrl.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(mBuyUrl);
    else setError("The credits page address did not come from Wolf Territories, so it was not opened.");
}

void WolfFloaterNPC::openWolfAvatars()
{
    LLFloaterReg::showInstance("wolf_avatars");
}

// ─── the region ─────────────────────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::onRegionChanged()
{
    // A new region: its own NPCs, rights and game (asked again once it has said it is Wolf: draw())
    closeQuestion();
    mNpcs = LLSD::emptyArray();
    mSelected.clear();
    mHello = LLSD();
    ++mHelloSerial;
    ++mListSerial;
    if (mStartPanel) applyPetSupport();   // [ROUND C GATE] no pets until the new region's hello says so
    ++mDetailSerial;
    mListOut = false;
    mListAgain = false;
    if (mCards) { mCards->mCards.clear(); mCards->mSelected.clear(); }
    if (mDetailView && mDetailView->getVisible()) showDetail(false);
    refreshActions();
    mRegionPending = getVisible();
}

void WolfFloaterNPC::sendHello()
{
    const U32 serial = ++mHelloSerial;
    LLHandle<LLFloater> h = getHandle();
    // Source: PROTOCOL.md section 2 hello -> {enabled, max, count, holder, canCreateHere, whyNot, game}
    WolfNPC::instance().request("hello", LLSD::emptyMap(), [h, serial](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mHelloSerial) return;
        if (!reply["ok"].asBoolean())
        {
            self->mHello = LLSD();
            self->showRules();
            self->applyPetSupport();
            self->mCreateNote->setText(cap(reply["error"].asString(), 300));
            self->mCreateNote->setColor(caution());
            return;
        }
        self->mHello = reply;
        self->showRules();
        self->applyPetSupport();
        self->mNewForm->setGameInfo(reply["game"]);
        self->mEditForm->setGameInfo(reply["game"]);
        std::string note;
        bool problem = false;
        if (!reply["enabled"].asBoolean())
        {
            note = "WolfNPC is switched off on this region.";
            problem = true;
        }
        else if (!reply["canCreateHere"].asBoolean())
        {
            note = "You cannot make an NPC where you stand: " + cap(reply["whyNot"].asString(), 200);
            problem = true;
        }
        else
        {
            note = llformat("This region has %d of the %d NPCs it allows.", reply["count"].asInteger(), reply["max"].asInteger());
        }
        self->mCreateNote->setText(note);
        self->mCreateNote->setColor(problem ? caution() : normalText());
    });
}

void WolfFloaterNPC::onTabChanged()
{
    closeQuestion();
    LLPanel* p = mTabs->getCurrentPanel();
    if (p == mNewTab) sendHello();   // can they make one where they stand now?
    else if (p == mRegionTab)
    {
        sendHello();   // [ROUND C] the region's and parcel's pet / noise rules (hello rules, canSetRegionRules)
        if (!mDetailView->getVisible()) requestList();
    }
    else if (p == mScenesTab) { if (!mScenesRead) loadScenes(); }
}

void WolfFloaterNPC::onChangedPush(const std::string& id)
{
    // Source: PROTOCOL.md section 2 push "changed" {id}: the list again (a moment later, one request for a burst), and the
    // open NPC again when nothing on its screen is waiting to be saved
    if (!getVisible()) return;
    if (mListWantAt <= 0.0) mListWantAt = nowSec() + 1.0;
    if (mDetailView->getVisible() && id == mDetailId && mDetailOwner && !mRuleEditor->getVisible() && !mGame->editing() && !mMind->editing())
    {
        LLSD now;
        std::string err;
        if (mEditForm->cfg(now, err) && mEditForm->changedKeys(now).size() == 0) requestGet(mDetailId);
    }
}

bool WolfFloaterNPC::placeInFront(LLVector3& pos, F32& facing, LLVector3& agent_pos) const
{
    // Source: wolffloateradventurestudioedit.cpp:3778-3787 (agent region position, facing 0 = east) and wolfvehiclerez.cpp:
    // 872-877 (the flat facing: at axis with z = 0, the x axis when looking straight up or down)
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return false;
    agent_pos = rgn->getPosRegionFromAgent(gAgent.getPositionAgent());
    if (!std::isfinite(agent_pos.mV[VX]) || !std::isfinite(agent_pos.mV[VY]) || !std::isfinite(agent_pos.mV[VZ])) return false;
    LLVector3 at = gAgent.getAtAxis();
    at.mV[VZ] = 0.f;
    if (at.normVec() < 0.01f) at = LLVector3::x_axis;
    pos = agent_pos + at * 2.f;
    const F32 w = rgn->getWidth();
    pos.mV[VX] = llclamp(pos.mV[VX], 0.f, w - 0.01f);
    pos.mV[VY] = llclamp(pos.mV[VY], 0.f, w - 0.01f);
    // facing back towards the agent; PROTOCOL.md section 2 create "facing (degrees, 0 = east, counter-clockwise)"
    facing = std::round(atan2f(-at.mV[VY], -at.mV[VX]) * RAD_TO_DEG);
    if (facing < 0.f) facing += 360.f;
    if (facing >= 360.f) facing -= 360.f;
    return true;
}

// ─── AI talk ────────────────────────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::loadAiInfo(WolfNPCForm* form)
{
    // Source: PROTOCOL.md section 5 npc_ai_info -> {balance (live), perReply, perVoiceReply, voices [{id, name}], buyurl}
    const U32 serial = ++mAiSerial;
    LLHandle<LLFloater> h = getHandle();
    if (form) form->setAiLine("Reading the price of AI talk...", false);
    WolfAdventure::api("npc_ai_info", LLSD::emptyMap(), [h, serial, form](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mAiSerial) return;
        if (!ok)
        {
            if (form) form->setAiLine("The price of AI talk could not be read: " + cap(error, 200), true);
            return;
        }
        self->mAiInfo = reply;
        if (self->mMind) self->mMind->setPrices(reply);
        self->mNewForm->setVoices(reply["voices"]);
        self->mEditForm->setVoices(reply["voices"]);
        if (form) self->showAiLine(form);
        if (self->mScene < 0 && self->mSceneList->getFirstSelected()) self->showSceneCost();
    });
}

void WolfFloaterNPC::showAiLine(WolfNPCForm* form)
{
    if (!mAiInfo.isMap()) return;
    // the brain (npc_think, PROTOCOL.md section 5): with AI on it also chooses what to do every few minutes while people are
    // near, charged like a reply
    static const std::string DECIDES("It also decides what to do every few minutes while people are near, at the same price.");
    const std::string have = creditsText(mAiInfo["balance"]);
    // the wording Paul's spec gives (British English): "AI talk uses your Wolf Credits: about N credits per reply. You have X."
    // [ROUND 2] credits AND pounds, and the most it can spend in a day at its settings; prices are "at most" (the dearer of
    // the two AI providers plus 10%)
    const LLSD per = form->voiceChosen() ? mAiInfo["perVoiceReply"] : mAiInfo["perReply"];
    const std::string reply_words = form->voiceChosen() ? " credits per voice reply" : " credits per reply";
    const F64 day = dailyMax(form);
    form->setAiLine("AI talk uses your Wolf Credits: about " + creditsText(per) + reply_words + pounds(per.asReal()) + " at most. You have " + have + ". "
                    + DECIDES + " This NPC can spend at most about " + creditsText(LLSD(day)) + " credits a day" + pounds(day) + " at its current settings."
                    + (form->gameOnly() ? std::string(" Game Mode only: it talks only to people playing in Game Mode, so it usually costs much less.") : std::string())
                    + lookupWords(), false);
}

std::string WolfFloaterNPC::lookupWords() const
{
    // [ROUND B] npc_ai_info perLookupWeb {credits, maxCredits}: web look-ups are off unless switched on (Edit > Learning)
    const LLSD& v = mAiInfo["perLookupWeb"];
    if (!v.isDefined()) return std::string();
    const F64 about = v.isMap() ? v["credits"].asReal() : v.asReal(), most = v.isMap() ? v["maxCredits"].asReal() : about;
    return " Web look-ups (off unless you switch them on in Edit > Learning) cost about " + creditsText(LLSD(about)) + " credits each" + pounds(about)
           + ", at most " + creditsText(LLSD(most)) + ".";
}

F64 WolfFloaterNPC::dailyMax(const WolfNPCForm* form) const
{
    // Source: the brain's npc_ai_info (coordinator, round 2): maxRepliesPerDay * perReply (perVoiceReply with a voice) +
    // maxDecisionsPerDay * perDecision (+ 24 * perNpcChatHour when it talks to other NPCs), capped by its daily limit
    if (!mAiInfo.isMap()) return 0.0;
    const F64 per = (form->voiceChosen() ? mAiInfo["perVoiceReply"] : mAiInfo["perReply"]).asReal();
    F64 total = mAiInfo["maxRepliesPerDay"].asReal() * per + mAiInfo["maxDecisionsPerDay"].asReal() * mAiInfo["perDecision"].asReal();
    if (form->npcTalkOn()) total += 24.0 * mAiInfo["perNpcChatHour"].asReal();
    const F64 limit = form->dailyLimit();
    if (limit > 0.0) total = llmin(total, limit);
    return std::isfinite(total) && total > 0.0 ? total : 0.0;
}

std::string WolfFloaterNPC::pounds(F64 credits) const
{
    // pounds = credits * pencePerCredit / 100 (npc_ai_info pencePerCredit; never a fixed rate). Not known: credits only.
    const F64 ppc = mAiInfo.isMap() ? mAiInfo["pencePerCredit"].asReal() : 0.0;
    if (!(ppc > 0.0) || !std::isfinite(credits) || credits < 0.0) return std::string();
    const F64 gbp = credits * ppc / 100.0;
    if (gbp > 0.0 && gbp < 0.01) return " (under \xC2\xA3" "0.01)";
    return llformat(" (about \xC2\xA3%.2f)", gbp);
}

void WolfFloaterNPC::confirmAi(const std::string& npc_name, const WolfNPCForm* form, std::function<void()> go)
{
    // The live balance (never one kept from before): npc_ai_info is asked again right before the question
    setDone("Reading the price of AI talk...");
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_ai_info", LLSD::emptyMap(), [h, npc_name, form, go](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!ok)
        {
            self->setError("AI talk was not turned on: its price could not be read (" + cap(error, 200) + ").", reply);
            return;
        }
        self->mAiInfo = reply;
        self->clearStatus();
        const std::string n = creditsText(reply["perReply"]), m = creditsText(reply["perVoiceReply"]), x = creditsText(reply["balance"]);
        const F64 day = self->dailyMax(form);
        self->ask("Turn on AI talk for " + npc_name + "? Each reply costs you about " + n + " credits (voice: about " + m + "). You have " + x
                  + " credits. It goes quiet when you run out. This NPC can spend at most about " + creditsText(LLSD(day)) + " credits a day"
                  + self->pounds(day) + " at its current settings.", "Turn on AI talk", go);
    });
}

// ─── New NPC ────────────────────────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::loadAvatars()
{
    // Source: PROTOCOL.md section 5 npc_avatars -> avatars [{job, name, kind, species, created, picture}], wolfavatarsUrl
    const U32 serial = ++mAvatarsSerial;
    mAvatarsNote->setText(std::string("Looking for your WolfAvatars..."));
    mAvatarsNote->setColor(normalText());
    mAvatarsNote->setVisible(mGrid->mItems.empty());
    mAvatarsMakeBtn->setVisible(false);
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_avatars", LLSD::emptyMap(), [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mAvatarsSerial) return;
        if (!ok)
        {
            self->mAvatarsNote->setText("Your WolfAvatars could not be listed: " + cap(error, 200));
            self->mAvatarsNote->setColor(caution());
            self->mAvatarsNote->setVisible(true);
            self->setError("Your WolfAvatars could not be listed: " + cap(error, 300), reply);
            return;
        }
        self->mAvatars = LLSD::emptyArray();
        for (const LLSD& a : llsd::inArray(reply["avatars"]))
        {
            const std::string job = a["job"].asString();
            const std::string kind = a["kind"].asString();
            if (job.empty() || (kind != "animal" && kind != "person") || self->mAvatars.size() >= 50) continue;
            self->mAvatars.append(a);
            if (self->mPictures.count(job) || !a["picture"].asBoolean()) continue;
            // asked once per job; one that failed is asked again when the window lists the avatars again
            auto why = self->mPictureWhy.find(job);
            if (why != self->mPictureWhy.end())
            {
                self->mPictureWhy.erase(why);
                self->mPictureAsked.erase(job);
            }
            if (self->mPictureAsked.insert(job).second) self->mPictureQueue.push_back(job);
        }
        self->mAvatarsRead = true;
        self->buildGrid();
        for (S32 i = 0; i < 3; ++i)
        {
            if (self->mPictureQueue.empty() || self->mPicturesOut >= 3) break;
            const std::string job = self->mPictureQueue.front();
            self->mPictureQueue.erase(self->mPictureQueue.begin());
            self->loadPicture(job);
        }
        if (self->mScene >= 0) self->showSceneStep();   // the scene's avatar list follows
    });
}

void WolfFloaterNPC::buildGrid()
{
    // the tiles from npc_avatars; [ROUND C] a pet is an animal, so "Make a pet" shows the animals only
    const std::string chosen = mGrid->mSelected >= 0 && mGrid->mSelected < (S32)mGrid->mItems.size()
                                   ? mGrid->mItems[(size_t)mGrid->mSelected].mJob : std::string();
    std::vector<WolfNPCAvatarGrid::Item> items;
    S32 select = -1;
    for (const LLSD& a : llsd::inArray(mAvatars))
    {
        const std::string job = a["job"].asString();
        if (mPetMode && a["kind"].asString() != "animal") continue;
        WolfNPCAvatarGrid::Item it;
        it.mJob = job;
        it.mName = cap(a["name"].asString(), 48);
        if (it.mName.empty()) it.mName = "Unnamed";
        it.mKind = a["kind"].asString();
        it.mSpecies = cap(lower(a["species"].asString()), 24);
        auto pic = mPictures.find(job);
        if (pic != mPictures.end()) it.mTexture = pic->second;
        else if (a["picture"].asBoolean() && !mPictureWhy.count(job)) it.mLoading = true;
        else if (mPictureWhy.count(job)) it.mWhy = mPictureWhy[job];
        if (job == chosen) select = (S32)items.size();
        items.push_back(it);
    }
    mGrid->mItems = items;
    mGrid->mSelected = select;
    // the holder as tall as the tiles (the scroll container scrolls it)
    const S32 h_needed = llmax(mGridScroll->getRect().getHeight(), mGrid->neededHeight());
    const LLRect hr = mGridHolder->getRect();
    mGridHolder->setRect(LLRect(hr.mLeft, hr.mTop, hr.mRight, hr.mTop - h_needed));
    mGrid->setRect(LLRect(0, h_needed, hr.getWidth(), 0));
    mPickPanel->getChild<LLTextBox>("step1_lbl")->setText(std::string(mPetMode ? "1. Choose an animal for your pet" : "1. Choose one of your WolfAvatars"));
    mPickPanel->getChild<LLButton>("open_avatars")->setLabel(std::string(mPetMode ? "Make a new animal" : "Open WolfAvatars"));
    if (!mAvatarsRead) return;
    if (items.empty())
    {
        mAvatarsNote->setText(std::string(mPetMode ? "You have no animal WolfAvatars yet - make one first (Make a new animal)." : "You have no WolfAvatars yet - make one first."));
        mAvatarsNote->setColor(normalText());
        mAvatarsNote->setVisible(true);
        mAvatarsMakeBtn->setVisible(true);
    }
    else
    {
        mAvatarsNote->setVisible(false);
        mAvatarsMakeBtn->setVisible(false);
    }
}

void WolfFloaterNPC::loadPicture(const std::string& job)
{
    // Source: PROTOCOL.md section 5 npc_picture {job} -> {mime "image/jpeg", data base64 (256 px)}; at most 3 at once
    ++mPicturesOut;
    LLSD body;
    body["job"] = job;
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_picture", body, [h, job](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        --self->mPicturesOut;
        std::string why = error;
        LLPointer<LLViewerTexture> tex;
        if (ok) ok = pictureTexture(reply["mime"].asString(), reply["data"].asString(), tex, why);
        // the brain's "no_picture": the avatar has no HUD picture - the tile shows "No picture", not a failure
        const bool none = !ok && reply.isMap() && reply["code"].asString() == "no_picture";
        if (none) why.clear();
        if (ok) self->mPictures[job] = tex;
        else if (!none) self->mPictureWhy[job] = cap(why, 160);
        for (WolfNPCAvatarGrid::Item& it : self->mGrid->mItems)
        {
            if (it.mJob != job) continue;
            it.mLoading = false;
            it.mTexture = ok ? tex : LLPointer<LLViewerTexture>();
            it.mWhy = ok ? std::string() : cap(why, 160);
        }
        if (!self->mPictureQueue.empty())
        {
            const std::string next = self->mPictureQueue.front();
            self->mPictureQueue.erase(self->mPictureQueue.begin());
            self->loadPicture(next);
        }
    });
}

void WolfFloaterNPC::onAvatarPicked()
{
    const S32 i = mGrid->mSelected;
    if (i < 0 || i >= (S32)mGrid->mItems.size()) return;
    const WolfNPCAvatarGrid::Item& it = mGrid->mItems[(size_t)i];
    clearStatus();
    mNewForm->setKind(it.mKind);
    // the avatar's own name, unless they typed one of their own
    if (mNewForm->name().empty() || mNewForm->name() == mNewAutoName) mNewForm->setName(it.mName);
    mNewAutoName = it.mName;
    mChosenText->setText("Chosen: " + it.mName + (it.mKind == "animal" ? (it.mSpecies.empty() ? std::string(" (an animal)") : " (" + it.mSpecies + ")") : std::string(" (a person)")));
    if (!it.mWhy.empty()) mGrid->setToolTip(it.mName + ": the picture is not shown (" + it.mWhy + ")");
    else mGrid->setToolTip(std::string("Your WolfAvatars: click one to choose it (or the arrow keys, then Enter)"));
}

void WolfFloaterNPC::onRez()
{
    clearStatus();
    if (mCreating) return;
    if (mPetMode && mWearPet) { onWearMake(); return; }   // [WOLF PETS] §29.4 Wear it: the brain makes it, nothing is rezzed
    const S32 i = mGrid->mSelected;
    if (i < 0 || i >= (S32)mGrid->mItems.size())
    {
        setError(mGrid->mItems.empty() ? std::string("You have no WolfAvatars yet - make one first.")
                                       : std::string("Choose one of your WolfAvatars first (step 1)."));
        return;
    }
    if (rlvBlocksRez())
    {
        setError("It can't be rezzed now: an RLV restriction (rez) is on.");
        return;
    }
    LLSD cfg;
    std::string err;
    if (!mNewForm->cfg(cfg, err)) { setError(err); return; }
    if (mPetMode)
    {
        // [ROUND C] PROTOCOL.md §26.1 / §27: {"behaviour":"pet","travel":<bool>,"noises":{"on":true,"volume":0.7}}
        if (mGrid->mItems[(size_t)i].mKind != "animal") { setError("A pet must be an animal: choose one of your animal WolfAvatars."); return; }
        cfg["behaviour"] = "pet";
        cfg["travel"] = mQuickTravel->get();
        if (!cfg["noises"].isMap()) { cfg["noises"]["on"] = true; cfg["noises"]["volume"] = LLSD(0.7); }
        if (cfg["travel"].asBoolean() && cfg["game"]["huntable"].asBoolean())
        {
            setError("A travelling pet cannot be huntable: untick Can be hunted under More options, Game.");
            return;
        }
    }
    // [ROUND 2] "What does it say to visitors?": one reaction {when near, who *, 8 m, say, once a visit, gap 10}
    const std::string says = mPetMode ? std::string() : trimmed(mSays->getText());
    if (!says.empty())
    {
        // Source: PROTOCOL.md section 6 "say:<text> (no ;)"
        if (says.find(';') != std::string::npos || says.find('|') != std::string::npos) { setError("Leave ; and | out of what it says."); return; }
        LLSD r = LLSD::emptyMap();
        r["id"] = "r1";
        r["when"] = "near";
        r["who"] = "*";
        r["m"] = 8;
        r["do"] = LLSD::emptyArray();
        r["do"].append("say:" + says);
        r["once"] = "visit";
        r["cooldown"] = 0;
        r["gap"] = 10;
        LLSD rules = LLSD::emptyArray();
        rules.append(r);
        cfg["reactions"] = rules;
    }
    LLVector3 pos, agent;
    F32 facing = 0.f;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (mHasSpot && rgn && rgn->getRegionID() == mSpotRegion)
    {
        // "Create a new one": the spot they confirmed before WolfAvatars made it
        pos = mSpotPos;
        agent = mSpotHome;
        facing = mSpotFacing;
    }
    else if (!placeInFront(pos, facing, agent)) { setError("Your position is not known yet - try again in a moment."); return; }
    if (mPetMode && rgn)
    {
        // §26.1 "create near the owner": 1.5 m to their left, facing the way they face
        LLVector3 left = gAgent.getLeftAxis();
        left.mV[VZ] = 0.f;
        if (left.normVec() < 0.01f) left = LLVector3::y_axis;
        pos = agent + left * 1.5f;
        const F32 w = rgn->getWidth();
        pos.mV[VX] = llclamp(pos.mV[VX], 0.f, w - 0.01f);
        pos.mV[VY] = llclamp(pos.mV[VY], 0.f, w - 0.01f);
        facing = facing >= 180.f ? facing - 180.f : facing + 180.f;   // placeInFront faced it back at them: a pet faces their way
    }
    // Source: PROTOCOL.md section 2 create {job, name, cfg (partial allowed), pos, facing}; section 3 home / facing
    cfg["home"] = wireVec(pos);
    cfg["facing"] = wireNum(facing);
    LLSD fields;
    fields["job"] = mGrid->mItems[(size_t)i].mJob;
    fields["name"] = cfg["name"];
    fields["cfg"] = cfg;
    fields["pos"] = wireVec(pos);
    fields["facing"] = wireNum(facing);
    const std::string name = cfg["name"].asString();
    if (cfg["ai"]["on"].asBoolean())
    {
        LLHandle<LLFloater> h = getHandle();
        confirmAi(name, mNewForm, [h, fields, name]()
        {
            if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->doCreate(fields, name);
        });
        return;
    }
    doCreate(fields, name);
}

void WolfFloaterNPC::onWearMake()
{
    // [WOLF PETS 2026-10-24] Wear it. Source: PROTOCOL.md §29.2 wolfpet_make {job, name?, side?, distance?} and §29.4 (what the
    // window shows); wolfstorm/php/wolfpet_lib.php:108-153 wa_api_wolfpet_make (the brain's own checks, repeated here first)
    mWearResult->setText(LLStringUtil::null);
    const S32 i = mGrid->mSelected;
    if (i < 0 || i >= (S32)mGrid->mItems.size())
    {
        setError(mGrid->mItems.empty() ? std::string("You have no animal WolfAvatars yet - make one first (Make a new animal).")
                                       : std::string("Choose one of your animals first (step 1)."));
        return;
    }
    const WolfNPCAvatarGrid::Item& it = mGrid->mItems[(size_t)i];
    // Source: wolfpet_lib.php:110 - the brain's own words
    if (it.mKind != "animal") { setError("Only WolfAvatars animals can be worn as pets."); return; }
    // Source: wolfpet_lib.php:111-113 - at most WPET_NAME_CHARS (:34, 40) characters, '"' and '\' dropped, then "Give your pet a name."
    std::string name = mQuickName->getText();
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '"' || c == '\\'; }), name.end());
    name = trimmed(name);
    if (name.empty()) { setError("Give your pet a name."); mQuickName->setFocus(true); return; }
    if (utf8Chars(name) > 40) { setError("Your pet's name is at most 40 characters."); mQuickName->setFocus(true); return; }
    // Source: wolfpet_lib.php:35 WPET_SIDES left|right|behind (default left), :36-38 distance 1.0-1.5 m (default 1.2)
    std::string side = mWearSide->getValue().asString();
    if (side != "left" && side != "right" && side != "behind") side = "left";
    const F64 distance = llclamp((F64)std::lround((F64)mWearDistance->get() * 10.0) / 10.0, 1.0, 1.5);
    LLSD body;
    body["job"] = it.mJob;
    body["name"] = name;
    body["side"] = side;
    body["distance"] = distance;
    mCreating = true;
    mRezBtn->setEnabled(false);
    setDone("Making " + name + "...");
    mWearResult->setText("Making " + name + " from your WolfAvatar. This can take a minute or two - you can keep playing.");
    mWearResult->setColor(normalText());
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("wolfpet_make", body, [h, name](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        self->mCreating = false;
        self->mRezBtn->setEnabled(true);
        if (!ok)
        {
            // Source: PROTOCOL.md §29.2 "Refusals (success false, plain British English, shown as they are)"; §29.4 "its error"
            const std::string why = error.empty() ? std::string("Your pet could not be made. Try again in a minute.") : cap(error, 500);
            self->setError(why, reply);
            self->mWearResult->setText(why);
            self->mWearResult->setColor(caution());
            return;
        }
        // Source: wolfpet_lib.php:149-153 {item, name, folder, folderName, ..., wearer {body, measured}}; the item is
        // "<name> (wear me)" (wolfpet.py:785)
        std::string pet = cap(reply["name"].asString(), 63);
        const std::string tail(" (wear me)");
        if (pet.size() > tail.size() && pet.compare(pet.size() - tail.size(), tail.size(), tail) == 0) pet.erase(pet.size() - tail.size());
        if (pet.empty()) pet = name;
        const std::string folder = reply["folderName"].asString().empty() ? std::string("WolfPets") : cap(reply["folderName"].asString(), 64);
        // Source: PROTOCOL.md §29.4 - the words on success, and the height hint when wearer.measured is false
        std::string words = pet + " is in your inventory: Objects > " + folder + ". Choose Add to wear it - it walks beside you everywhere.";
        if (!reply["wearer"]["measured"].asBoolean()) words += " If its feet float or sink, touch it and use Position > Higher or Lower.";
        self->setDone(pet + " is in your inventory: Objects > " + folder + ". Choose Add to wear it.");
        self->mWearResult->setText(words);
        self->mWearResult->setColor(normalText());
    });
}

void WolfFloaterNPC::doCreate(const LLSD& fields, const std::string& name)
{
    mCreating = true;
    mRezBtn->setEnabled(false);
    mSceneRezBtn->setEnabled(false);
    setDone("Rezzing " + name + "...");
    LLHandle<LLFloater> h = getHandle();
    LLSD send = fields;
    send.erase("brain");   // the window's own note for after the create (PROTOCOL.md §7), not a create field
    WolfNPC::instance().request("create", send, [h, name, fields](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        self->mCreating = false;
        self->mRezBtn->setEnabled(true);
        self->mSceneRezBtn->setEnabled(true);
        if (!reply["ok"].asBoolean())
        {
            // shown as is: e.g. "You already have a travelling pet: <name>." / "A travelling pet cannot be huntable" (§26)
            self->setError(cap(reply["error"].asString(), 500), reply);
            self->mCreateNote->setText(cap(reply["error"].asString(), 300));
            self->mCreateNote->setColor(caution());
            if (self->mScene >= 0) self->showSceneStep();
            return;
        }
        const std::string made = reply.has("name") ? cap(reply["name"].asString(), 48) : name;
        if (!reply["id"].asString().empty() && !fields["job"].asString().empty()) self->mJobOf[reply["id"].asString()] = fields["job"].asString();
        self->setDone(made + " is on its way.");
        self->mHasSpot = false;   // a spot confirmed for "Create a new one" is used once
        self->sendHello();
        if (self->mScene >= 0)
        {
            // [ROUND B] a piece's "brain" {teacher?, lookups?, styles?: [keys]} is applied after the create, as the owner
            if (fields.has("brain") && !reply["id"].asString().empty()) self->queueBrain(reply["id"].asString(), made, fields["brain"]);
            ++self->mSceneStepN;
            const bool more = self->mSceneStepN < self->mSceneSteps;
            self->showSceneStep();   // the last step says the scene is done
            if (more) self->setDone(made + " is on its way.");
        }
    });
}

// ─── On this region ─────────────────────────────────────────────────────────────────────────────────

void WolfFloaterNPC::requestList()
{
    if (mListOut) { mListAgain = true; return; }
    mListOut = true;
    const U32 serial = ++mListSerial;
    if (mNpcs.size() == 0) mCards->mComment = "Reading the region's NPCs...";
    LLHandle<LLFloater> h = getHandle();
    // Source: PROTOCOL.md section 2 list -> npcs [{id, name, kind, species, owner, ownerName, behaviour, paused, dead,
    // asleep, ai, mine, canEdit, canControl, pos, dist}] (nearest first)
    WolfNPC::instance().request("list", LLSD::emptyMap(), [h, serial](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mListSerial) return;
        self->mListOut = false;
        if (!reply["ok"].asBoolean())
        {
            self->setError(cap(reply["error"].asString(), 500), reply);
            if (self->mNpcs.size() == 0) self->mCards->mComment = "The list could not be read.";
        }
        else
        {
            self->mNpcs = reply["npcs"].isArray() ? reply["npcs"] : LLSD::emptyArray();
            self->fillList();
            self->requestCosts();
            self->showMonth();
        }
        if (self->mListAgain)
        {
            self->mListAgain = false;
            self->requestList();
        }
    });
}

void WolfFloaterNPC::fillList()
{
    // [ROUND 4 LOOK] one character card per NPC: picture (when its job is known) or a painted figure, name, kind / owner /
    // distance, and status chips in words (PROTOCOL.md section 2 list rows: asleep, paused, dead, ai, aiPaused, quiet,
    // gameOnly, gm)
    std::vector<WolfNPCCardGrid::Card> cards;
    for (const LLSD& n : llsd::inArray(mNpcs))
    {
        const std::string id = n["id"].asString();
        if (id.empty()) continue;
        WolfNPCCardGrid::Card c;
        c.mId = id;
        c.mName = cap(n["name"].asString(), 48);
        c.mKind = n["kind"].asString() == "person" ? "person" : "animal";
        const std::string species = cap(n["species"].asString(), 24);
        const std::string kind = !species.empty() ? species : c.mKind;
        const std::string owner = n["mine"].asBoolean() ? std::string("yours") : cap(n["ownerName"].asString(), 32) + "'s";
        c.mSub = kind + ", " + owner + llformat(", %.0f m", n["dist"].asReal());
        if (n["dead"].asBoolean()) c.mChips.emplace_back("Down", CHIP_DANGER);
        else if (n["paused"].asBoolean()) c.mChips.emplace_back("Paused", CHIP_NEUTRAL);
        else if (n["asleep"].asBoolean()) c.mChips.emplace_back("Asleep", CHIP_NEUTRAL);
        else c.mChips.emplace_back("Awake", CHIP_OK);
        // [ROUND 2] the region's AI states: quiet (credits ran out) and aiPaused (AI on, nobody within 20 m)
        if (n["quiet"].asBoolean()) c.mChips.emplace_back("AI stopped: no credits", CHIP_DANGER);
        else if (n["aiPaused"].asBoolean()) c.mChips.emplace_back("AI paused: nobody here", CHIP_WARN);
        else if (n["ai"].asBoolean()) c.mChips.emplace_back("AI talk", CHIP_INFO);
        if (n["gameOnly"].asBoolean()) c.mChips.emplace_back("Game Mode only", CHIP_INFO);   // [ROUND 4] §24
        if (petsSupported() && n["behaviour"].asString() == "pet")
        {
            // [ROUND C] §26: a travelling pet (the row's travel, else what get said)
            const bool travel = n.has("travel") ? n["travel"].asBoolean() : mTravelOf.count(id) && mTravelOf[id];
            c.mChips.emplace_back(travel ? "Travelling pet" : "Pet", CHIP_GM);
        }
        if (!n["gm"].asString().empty()) c.mChips.emplace_back("Run by " + cap(n["gm"].asString(), 20), CHIP_GM);   // [ROUND 3] §19
        c.mTip = c.mName + " (" + c.mSub + ")";
        for (const auto& chip : c.mChips) c.mTip += "\n" + chip.first;
        if (mCosts.has(id)) c.mTip += "\nSpent today " + creditsText(mCosts[id]["today"]) + " credits, this month " + creditsText(mCosts[id]["month"]);
        c.mTip += "\nDouble-click (or Enter) to open it.";
        auto job = mJobOf.find(id);
        if (job != mJobOf.end())
        {
            auto pic = mPictures.find(job->second);
            if (pic != mPictures.end()) c.mTexture = pic->second;
        }
        cards.push_back(c);
    }
    mCards->mCards = cards;
    mCards->mComment = mNpcs.size() == 0 ? std::string("No NPCs on this region yet. Make one on the New NPC tab.") : std::string();
    mCards->mSelected = mSelected;
    if (mCards->selectedIndex() < 0) { mSelected.clear(); mCards->mSelected.clear(); }
    // the holder as tall as the cards (the scroll container scrolls it)
    const S32 need = llmax(mCardScroll->getRect().getHeight(), mCards->neededHeight());
    const LLRect hr = mCardHolder->getRect();
    mCardHolder->setRect(LLRect(hr.mLeft, hr.mTop, hr.mRight, hr.mTop - need));
    mCards->setRect(LLRect(0, need, hr.getWidth(), 0));
    std::string count = llformat("%d NPCs on this region", (S32)mNpcs.size());
    if (mHello.isMap() && mHello["max"].asInteger() > 0) count += llformat(" (it allows %d)", mHello["max"].asInteger());
    mCountText->setText(count + ".");
    refreshActions();
}

void WolfFloaterNPC::showRules()
{
    // [ROUND C] hello "rules": {pets, noises, parcel: {id, pets, noises, canSet}}, "canSetRegionRules" (PROTOCOL.md §26.5)
    const LLSD& r = mHello["rules"];
    const bool region = petsSupported() && mHello["canSetRegionRules"].asBoolean();
    const bool parcel = petsSupported() && r["parcel"]["canSet"].asBoolean() && !r["parcel"]["id"].asString().empty();
    for (const char* n : { "rules_region_lbl", "rules_region_pets", "rules_region_noises" }) mListView->getChild<LLView>(n)->setVisible(region);
    for (const char* n : { "rules_parcel_lbl", "rules_parcel_pets", "rules_parcel_noises" }) mListView->getChild<LLView>(n)->setVisible(parcel);
    // on by default (§26.5)
    mListView->getChild<LLCheckBoxCtrl>("rules_region_pets")->set(r.has("pets") ? r["pets"].asBoolean() : true);
    mListView->getChild<LLCheckBoxCtrl>("rules_region_noises")->set(r.has("noises") ? r["noises"].asBoolean() : true);
    mListView->getChild<LLCheckBoxCtrl>("rules_parcel_pets")->set(r["parcel"].has("pets") ? r["parcel"]["pets"].asBoolean() : true);
    mListView->getChild<LLCheckBoxCtrl>("rules_parcel_noises")->set(r["parcel"].has("noises") ? r["parcel"]["noises"].asBoolean() : true);
}

void WolfFloaterNPC::sendRules(bool parcel)
{
    // [ROUND C] {"op":"pet_rules","parcel":"" (the region) | the parcel's global id,"pets":bool,"noises":bool}
    LLSD f;
    f["parcel"] = parcel ? mHello["rules"]["parcel"]["id"].asString() : std::string();
    f["pets"] = mListView->getChild<LLCheckBoxCtrl>(parcel ? "rules_parcel_pets" : "rules_region_pets")->get();
    f["noises"] = mListView->getChild<LLCheckBoxCtrl>(parcel ? "rules_parcel_noises" : "rules_region_noises")->get();
    const std::string where = parcel ? "this parcel" : "this region";
    const bool pets = f["pets"].asBoolean(), noises = f["noises"].asBoolean();
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request("pet_rules", f, [h, where, pets, noises](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean())
        {
            self->setError(cap(reply["error"].asString(), 500), reply);
            self->showRules();   // the switches go back to what the region holds
            return;
        }
        self->setDone("Saved for " + where + ": visiting pets " + (pets ? "allowed" : "not allowed") + ", NPC noises " + (noises ? "allowed." : "not allowed."));
        self->sendHello();
    });
}

void WolfFloaterNPC::requestCosts()
{
    // [ROUND 2] npc_costs {npcs: [ids]} -> {costs: {<id>: {today, month}}}, the owner's own NPCs only
    LLSD ids = LLSD::emptyArray();
    for (const LLSD& n : llsd::inArray(mNpcs))
    {
        if (n["mine"].asBoolean() && !n["id"].asString().empty()) ids.append(n["id"].asString());
    }
    if (ids.size() == 0) return;
    LLSD body;
    body["npcs"] = ids;
    const U32 serial = ++mCostsSerial;
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_costs", body, [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mCostsSerial) return;
        if (!ok)
        {
            self->mSelNote->setText("What your NPCs spent could not be read: " + cap(error, 200));
            return;
        }
        self->mCosts = reply["costs"].isMap() ? reply["costs"] : LLSD::emptyMap();
        self->fillList();
    });
}

LLSD WolfFloaterNPC::selectedNpc() const
{
    if (mSelected.empty()) return LLSD();
    for (const LLSD& n : llsd::inArray(mNpcs))
    {
        if (n["id"].asString() == mSelected) return n;
    }
    return LLSD();
}

void WolfFloaterNPC::refreshActions()
{
    if (!mEditBtn) return;
    const LLSD n = selectedNpc();
    const bool sel = n.isMap();
    // Source: PROTOCOL.md section 1 - edit (and rename) the owner only; pause / resume / home / delete / stop the owner or a holder
    const bool edit = sel && n["canEdit"].asBoolean();
    const bool control = sel && n["canControl"].asBoolean();
    mEditBtn->setEnabled(edit || control);
    mEditBtn->setLabel(edit || !control ? std::string("Edit...") : std::string("Animations..."));
    mRenameBtn->setEnabled(edit);
    mPauseBtn->setEnabled(control);
    mPauseBtn->setLabel(sel && n["paused"].asBoolean() ? std::string("Resume") : std::string("Pause"));
    mHomeBtn->setEnabled(control);
    mHereBtn->setEnabled(control);
    mStopBtn->setEnabled(control);
    mDeleteBtn->setEnabled(control);
    // [ROUND C] a pet can be sent home by its owner, the region's holders or the parcel's owner (the region checks; the
    // window cannot see who owns the parcel under it, so the button shows for every pet)
    mSendHomeBtn->setVisible(petsSupported() && sel && n["behaviour"].asString() == "pet");
    std::string note;
    if (!sel) note = mNpcs.size() ? "Select an NPC in the list to see what you can do with it." : std::string();
    else if (n["mine"].asBoolean())
    {
        note = "Yours. Edit changes its settings, animations and reactions.";
        if (mCosts.has(mSelected))
        {
            note += " Spent today " + creditsText(mCosts[mSelected]["today"]) + " credits" + pounds(mCosts[mSelected]["today"].asReal())
                    + ", this month " + creditsText(mCosts[mSelected]["month"]) + pounds(mCosts[mSelected]["month"].asReal()) + ".";
        }
        if (n["quiet"].asBoolean())
        {
            // [ROUND 2] credits ran out: said, with Buy credits (npc_ai_info buyurl)
            note = "AI talk stopped: your Wolf Credits have run out. " + note;
            LLSD buy;
            buy["code"] = "credits_short";
            buy["buyurl"] = mAiInfo["buyurl"];
            if (!mAiInfo["buyurl"].asString().empty()) setError("AI talk for " + cap(n["name"].asString(), 48) + " stopped: your Wolf Credits have run out.", buy);
        }
    }
    else if (control) note = "Not yours (" + cap(n["ownerName"].asString(), 64) + "'s). As the region's owner or manager you can pause it, call it home, play its animations or delete it; only its owner changes its settings.";
    else note = "Not yours (" + cap(n["ownerName"].asString(), 64) + "'s): only its owner or the region's owner can change it.";
    mSelNote->setText(note);
}

void WolfFloaterNPC::sendCmd(const std::string& id, const std::string& cmd, const std::string& value, const std::string& done_words)
{
    if (id.empty()) return;
    clearStatus();
    // Source: PROTOCOL.md section 2 cmd {id, cmd, value} -> done {id, cmd}
    LLSD fields;
    fields["id"] = id;
    fields["cmd"] = cmd;
    fields["value"] = value;
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request("cmd", fields, [h, done_words, cmd](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean())
        {
            self->setError(cap(reply["error"].asString(), 500), reply);
            return;
        }
        self->setDone(done_words);
        if (cmd == "delete" || cmd == "pause" || cmd == "resume") self->requestList();
        else if (self->mDetailView->getVisible() && self->mDetailOwner && (cmd == "here" || cmd == "park" || cmd == "exit"))
        {
            self->requestGet(self->mDetailId);
        }
    });
}

void WolfFloaterNPC::onPauseResume()
{
    const LLSD n = selectedNpc();
    if (!n.isMap()) return;
    const std::string name = cap(n["name"].asString(), 48);
    if (n["paused"].asBoolean()) sendCmd(n["id"].asString(), "resume", "", name + " carries on.");
    else sendCmd(n["id"].asString(), "pause", "", name + " is paused.");
}

void WolfFloaterNPC::onDelete()
{
    const LLSD n = selectedNpc();
    if (!n.isMap()) return;
    const std::string id = n["id"].asString(), name = cap(n["name"].asString(), 48);
    LLHandle<LLFloater> h = getHandle();
    ask("Delete " + name + "? It is taken off the region for good (the WolfAvatar it was made from stays yours).", "Delete " + name, [h, id, name]()
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (self->mDetailId == id && self->mDetailView->getVisible()) self->showDetail(false);
        self->sendCmd(id, "delete", "", name + " has been deleted.");
    });
}

void WolfFloaterNPC::onRename()
{
    const LLSD n = selectedNpc();
    if (!n.isMap()) return;
    mRenameName->setText(cap(n["name"].asString(), 48));
    mRenameRow->setVisible(true);
    mRenameName->setFocus(true);
    mRenameName->selectAll();
}

void WolfFloaterNPC::onRenameSave()
{
    const LLSD n = selectedNpc();
    if (!n.isMap()) return;
    const std::string name = trimmed(mRenameName->getText());
    if (name.empty() || utf8Chars(name) > 48) { setError("Give it a name of 1 to 48 characters."); return; }
    LLSD fields;
    fields["id"] = n["id"];
    fields["cfg"]["name"] = name;
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request("update", fields, [h, name](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean()) { self->setError(cap(reply["error"].asString(), 500), reply); return; }
        self->mRenameRow->setVisible(false);
        self->setDone("Renamed to " + name + ".");
        self->requestList();
    });
}

void WolfFloaterNPC::onEdit()
{
    const LLSD n = selectedNpc();
    if (!n.isMap() || !(n["canEdit"].asBoolean() || n["canControl"].asBoolean())) return;
    clearStatus();
    mDetailId = n["id"].asString();
    mDetail = LLSD();
    mDetailOwner = false;
    mDetailTitle->setText(cap(n["name"].asString(), 48) + (n["mine"].asBoolean() ? std::string(" (yours)") : " (" + cap(n["ownerName"].asString(), 64) + "'s)"));
    mDetailState->setText(n["canEdit"].asBoolean() ? std::string("Reading its settings...") : std::string());
    mParkBtn->setVisible(false);
    mExitBtn->setVisible(false);
    mDetailView->getChild<LLView>("detail_sendhome")->setVisible(petsSupported() && n["behaviour"].asString() == "pet");   // [ROUND C]
    closeRuleEditor();
    // Source: PROTOCOL.md section 1 - the full settings (get, edit) are its owner's only; others who may control it get the
    // animations (the standard ones: its inventory list comes with get)
    const bool edit = n["canEdit"].asBoolean();
    mGameWanted = false;
    mGmControlling = false;
    mGmBy = n["gm"].asString();
    refreshDetailTabs();
    mDetailTabs->selectTabPanel(edit ? mSettingsTab : mAnimsTab);
    refreshGm();
    mEditForm->setKind(n["kind"].asString());
    mEditForm->setEnabled(false);
    mSaveBtn->setEnabled(false);
    fillAnims();
    showDetail(true);
    if (edit) requestGet(mDetailId);
}

void WolfFloaterNPC::showDetail(bool on)
{
    mDetailView->setVisible(on);
    mListView->setVisible(!on);
    if (!on)
    {
        ++mDetailSerial;
        mDetailId.clear();
        mDetail = LLSD();
        mDetailOwner = false;
        closeRuleEditor();
    }
}

void WolfFloaterNPC::requestGet(const std::string& id)
{
    const U32 serial = ++mDetailSerial;
    LLSD fields;
    fields["id"] = id;
    LLHandle<LLFloater> h = getHandle();
    // Source: PROTOCOL.md section 2 get {id} -> npc {id, name, cfg, anims, sounds, state, canEdit, canControl, ownerName}
    WolfNPC::instance().request("get", fields, [h, serial](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mDetailSerial) return;
        if (!reply["ok"].asBoolean())
        {
            self->mDetailState->setText(std::string("Its settings could not be read."));
            self->setError(cap(reply["error"].asString(), 500), reply);
            return;
        }
        self->applyGet(reply);
    });
}

void WolfFloaterNPC::applyGet(const LLSD& reply)
{
    mDetail = reply;
    mDetailOwner = true;
    const LLSD& cfg = reply["cfg"];
    mEditForm->setKind(cfg["kind"].asString());
    mEditForm->fill(cfg);
    mEditForm->setEnabled(reply["canEdit"].asBoolean());
    mSaveBtn->setEnabled(reply["canEdit"].asBoolean());
    mDetailTitle->setText(cap(reply["name"].asString(), 48));
    // Source: PROTOCOL.md section 2 get state {intent, who, health, max, dead, respawnIn, asleep, driving}
    const LLSD& st = reply["state"];
    std::string state;
    if (st["dead"].asBoolean()) state = llformat("Down: back in %.0f s", st["respawnIn"].asReal());
    else if (st["asleep"].asBoolean()) state = "Asleep (nobody within 96 m)";
    else
    {
        const std::string intent = cap(st["intent"].asString(), 32), who = cap(st["who"].asString(), 48);
        state = intent.empty() ? std::string("Idle") : "Doing: " + intent + (who.empty() ? std::string() : " " + who);
    }
    if (st["max"].asReal() > 0.0) state += llformat(", health %.0f/%.0f", st["health"].asReal(), st["max"].asReal());
    if (st["driving"].asBoolean()) state += ", driving";
    // [ROUND 2] the region's AI states and its reason
    if (st["quiet"].asBoolean())
    {
        state += ". AI talk stopped: your Wolf Credits have run out";
        LLSD buy;
        buy["code"] = "credits_short";
        buy["buyurl"] = mAiInfo["buyurl"];
        if (!mAiInfo["buyurl"].asString().empty()) setError("AI talk stopped: your Wolf Credits have run out.", buy);
    }
    else if (st["aiPaused"].asBoolean()) state += ". AI paused: nobody here";
    const std::string why = cap(st["why"].asString(), 160);
    if (!why.empty()) state += ". " + why;
    // [ROUND 3] §19 state.gm: who controls it ("" = nobody)
    mGmBy = cap(st["gm"].asString(), 64);
    if (mGmBy.empty()) mGmControlling = false;
    else
    {
        std::string me;
        LLAgentUI::buildFullname(me);
        if (mGmBy == me) mGmControlling = true;
    }
    if (!mGmBy.empty()) state += ". Run by " + mGmBy;
    mDetailState->setText(state);
    mAiCache[mDetailId] = cfg["ai"];
    if (!cfg["job"].asString().empty()) mJobOf[mDetailId] = cfg["job"].asString();   // [ROUND 4 LOOK] its picture on the card
    mTravelOf[mDetailId] = cfg["travel"].asBoolean();   // [ROUND C]
    mGame->setNpc(mDetailId, cfg, st, mNpcs);
    mMind->setNpc(mDetailId, cfg, st);   // [ROUND 4]
    if (mAiInfo.isMap()) mMind->setPrices(mAiInfo);
    // the Game Master's lists: its animations and its events
    {
        const std::string was = mGmAnim->getSimple();
        mGmAnim->removeall();
        for (const LLSD& a : llsd::inArray(reply["anims"])) mGmAnim->add(cap(a.asString(), 64), LLSD(cap(a.asString(), 64)));
        if (!was.empty()) mGmAnim->setTextEntry(was);
        mGmEvent->removeall();
        if (cfg["events"].isMap())
        {
            for (auto it = cfg["events"].beginMap(); it != cfg["events"].endMap(); ++it)
            {
                if (it->second.isMap()) mGmEvent->add(it->first + (it->second["title"].asString().empty() ? std::string() : " - " + cap(it->second["title"].asString(), 40)), LLSD(it->first));
            }
        }
        mGmEvent->selectFirstItem();
    }
    refreshDetailTabs();
    refreshGm();
    mParkBtn->setVisible(st["driving"].asBoolean());
    mDetailView->getChild<LLView>("detail_sendhome")->setVisible(petsSupported() && cfg["behaviour"].asString() == "pet");
    mExitBtn->setVisible(st["driving"].asBoolean());
    fillAnims();
    fillRules();
}

void WolfFloaterNPC::onSaveSettings()
{
    clearStatus();
    if (!mDetailOwner || mDetailId.empty()) return;
    LLSD now;
    std::string err;
    if (!mEditForm->cfg(now, err)) { setError(err); return; }
    const LLSD changes = mEditForm->changedKeys(now);
    if (changes.size() == 0) { setDone("Nothing has changed, so nothing was sent."); return; }
    // AI talk being turned on asks first (its owner pays)
    if (now["ai"]["on"].asBoolean() && !mEditForm->filled()["ai"]["on"].asBoolean())
    {
        LLHandle<LLFloater> h = getHandle();
        confirmAi(now["name"].asString(), mEditForm, [h, changes]()
        {
            if (WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get())) self->doUpdate(changes, "Saved.", true);
        });
        return;
    }
    doUpdate(changes, "Saved.", true);
}

void WolfFloaterNPC::doUpdate(const LLSD& cfg_changes, const std::string& done_words, bool settings, std::function<void(const LLSD& cfg)> after)
{
    // Source: PROTOCOL.md section 2 update {id, cfg (partial: only the keys given change)} -> updated {id, cfg (whole)}
    LLSD fields;
    fields["id"] = mDetailId;
    fields["cfg"] = cfg_changes;
    const U32 serial = mDetailSerial;
    if (settings) mSaveBtn->setEnabled(false);
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request("update", fields, [h, serial, done_words, settings, after](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mDetailSerial) return;
        self->mSaveBtn->setEnabled(true);
        if (!reply["ok"].asBoolean())
        {
            if (settings || after) self->setError(cap(reply["error"].asString(), 500), reply);
            else
            {
                self->mRuleError->setText(cap(reply["error"].asString(), 300));
                self->setError(cap(reply["error"].asString(), 500), reply);
            }
            return;
        }
        if (reply["cfg"].isMap())
        {
            if (settings)
            {
                self->mDetail["cfg"] = reply["cfg"];
                self->mEditForm->fill(reply["cfg"]);
            }
            else if (after)
            {
                // [ROUND 3] a Game tab part: the whole stored cfg is kept; the Settings tab's unsaved changes stay on screen
                self->mDetail["cfg"] = reply["cfg"];
                self->mEditForm->setPlaces(reply["cfg"]);   // [ROUND 4] places / tools saved on the Places tab
            }
            else
            {
                // only the reactions: unsaved changes on the Settings tab are left as they are
                self->mDetail["cfg"]["reactions"] = reply["cfg"]["reactions"];
            }
        }
        if (!settings && !after) self->closeRuleEditor();
        self->fillRules();
        self->setDone(done_words);
        if (after) after(self->mDetail["cfg"]);
        self->refreshDetailTabs();
        self->requestList();
    });
}

void WolfFloaterNPC::fillAnims()
{
    const std::string sel = mAnims->getFirstSelected() ? mAnims->getFirstSelected()->getValue().asString() : std::string();
    mAnims->deleteAllItems();
    std::set<std::string> seen;
    auto add = [&](const std::string& name, const std::string& from)
    {
        if (name.empty() || !seen.insert(name).second) return;
        LLSD row;
        row["value"] = name;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = name;
        row["columns"][1]["column"] = "from";
        row["columns"][1]["value"] = from;
        mAnims->addElement(row);
    };
    // Source: PROTOCOL.md section 2 get anims (its inventory) and section 8 (the built-in and default names)
    for (const LLSD& a : llsd::inArray(mDetail["anims"])) add(cap(a.asString(), 64), "its own");
    LLSD n = selectedNpc();
    const std::string kind = mDetailOwner ? mDetail["cfg"]["kind"].asString() : n["kind"].asString();
    if (kind == "person")
    {
        for (const char* a : PERSON_ANIMS) add(a, "WolfNPC");
        for (const char* a : AVATAR_ANIMS) add(a, "standard avatar");
    }
    else
    {
        for (const char* a : ANIMAL_ANIMS) add(a, "WolfNPC");
    }
    if (!sel.empty()) mAnims->selectByValue(LLSD(sel));
}

void WolfFloaterNPC::onAnim(const std::string& cmd)
{
    LLScrollListItem* it = mAnims->getFirstSelected();
    if (!it) { setError("Select an animation in the list first."); return; }
    const std::string a = it->getValue().asString();
    sendCmd(mDetailId, cmd, a, cmd == "anim" ? "Playing " + a + "." : cmd == "anim_loop" ? "Looping " + a + "." : "Stopped " + a + ".");
}

// ─── reactions (PROTOCOL.md section 6) ──────────────────────────────────────────────────────────────

namespace
{
    std::string doWords(const std::string& d)
    {
        const size_t c = d.find(':');
        const std::string type = d.substr(0, c), value = c == std::string::npos ? std::string() : d.substr(c + 1);
        const std::string label = labelOf(ACTIONS, type);
        return value.empty() ? label : label + " " + value;
    }

    std::string whoWords(const std::string& who)
    {
        std::vector<std::string> terms;
        LLStringUtil::getTokens(who, terms, ",");
        std::string out;
        for (std::string t : terms)
        {
            t = trimmed(t);
            std::string w;
            const bool neg = t.rfind("not:", 0) == 0;
            if (neg) t = t.substr(4);
            const size_t c = t.find(':');
            const std::string head = c == std::string::npos ? t : t.substr(0, c), value = c == std::string::npos ? std::string() : t.substr(c + 1);
            if (t == "*" || t == "anyone") w = "Anyone";
            else if (head == "name") w = "name " + value;
            else if (head == "key") w = "a particular person";
            else if (head == "kind") w = "kind " + value;
            else if (head == "pack") w = "pack " + value;
            else if (head == "group" && !value.empty()) w = "group " + value;
            else if (head == "tag") w = "tag " + value;
            else w = labelOf(WHOS, head);
            if (!out.empty()) out += " or ";
            out += (neg ? "not " : "") + w;
        }
        return out;
    }
}

void WolfFloaterNPC::fillRules()
{
    const S32 sel = mRules->getFirstSelectedIndex();
    mRules->deleteAllItems();
    const LLSD& rules = mDetail["cfg"]["reactions"];
    S32 i = 0;
    for (const LLSD& r : llsd::inArray(rules))
    {
        std::string does;
        for (const LLSD& d : llsd::inArray(r["do"]))
        {
            if (!does.empty()) does += ", ";
            does += doWords(cap(d.asString(), 120));
        }
        const std::string once = r["once"].asString();
        std::string often = labelOf(OFTENS, once);
        if (once.empty() && r["cooldown"].asReal() > 0.0) often += llformat(" (%.0f s)", r["cooldown"].asReal());
        LLSD row;
        row["value"] = i++;
        row["columns"][0]["column"] = "when";
        row["columns"][0]["value"] = labelOf(WHENS, r["when"].asString());
        row["columns"][1]["column"] = "who";
        row["columns"][1]["value"] = whoWords(cap(r["who"].asString(), 300));
        row["columns"][2]["column"] = "m";
        row["columns"][2]["value"] = llformat("%.0f m", r["m"].asReal());
        row["columns"][3]["column"] = "do";
        row["columns"][3]["value"] = does;
        row["columns"][4]["column"] = "often";
        row["columns"][4]["value"] = often;
        mRules->addElement(row);
    }
    if (rules.size() == 0) mRules->setCommentText("No reactions yet. New rule: when someone comes near, it does something.");
    if (sel >= 0 && sel < (S32)rules.size()) mRules->selectNthItem(sel);
    mRulesCount->setText(llformat("%d of %d rules", (S32)rules.size(), MAX_RULES));
    const bool selected = mRules->getFirstSelected() != nullptr;
    mRuleNewBtn->setEnabled(mDetailOwner && (S32)rules.size() < MAX_RULES);
    mRuleChangeBtn->setEnabled(mDetailOwner && selected);
    mRuleRemoveBtn->setEnabled(mDetailOwner && selected);
}

void WolfFloaterNPC::editRule(S32 index)
{
    const LLSD& rules = mDetail["cfg"]["reactions"];
    if (index >= (S32)rules.size()) return;
    if (index < 0 && (S32)rules.size() >= MAX_RULES) { setError("It already has 16 reactions, the most it can. Remove one first."); return; }
    mRuleIndex = index;
    const LLSD r = index >= 0 ? rules[index] : LLSD::emptyMap();
    selectOr(mRuleWhen, r.has("when") ? r["when"].asString() : std::string("near"));
    mRuleRange->setValue(LLSD(r.has("m") ? llclamp(r["m"].asReal(), 1.0, 96.0) : 10.0));
    // who: one term (maybe with not:) is shown term by term; more than one is kept as it is (set by a script)
    fillCombo(mRuleWho, WHOS);
    mRuleRawWho.clear();
    mRuleNot->set(false);
    mRuleWhoValue->setText(LLStringUtil::null);
    mRulePerson->removeall();
    for (const LLSD& p : llsd::inArray(mPeople)) mRulePerson->add(cap(p["name"].asString(), 64), LLSD(p["id"].asString()));
    const std::string who = trimmed(r["who"].asString());
    if (who.empty()) mRuleWho->selectByValue(LLSD("anyone"));
    else if (who.find(',') != std::string::npos)
    {
        mRuleRawWho = who;
        mRuleWho->add("As set by a script: " + cap(whoWords(who), 60), LLSD("raw"));
        mRuleWho->selectByValue(LLSD("raw"));
    }
    else
    {
        std::string t = who;
        if (t.rfind("not:", 0) == 0) { mRuleNot->set(true); t = t.substr(4); }
        const size_t c = t.find(':');
        const std::string head = c == std::string::npos ? t : t.substr(0, c), value = c == std::string::npos ? std::string() : t.substr(c + 1);
        if (t == "*" || t == "anyone") mRuleWho->selectByValue(LLSD("anyone"));
        else if (head == "name")
        {
            // "Name contains": *x* is shown as x
            std::string v = value;
            if (v.size() >= 2 && v.front() == '*' && v.back() == '*' && v.substr(1, v.size() - 2).find_first_of("*?") == std::string::npos) v = v.substr(1, v.size() - 2);
            mRuleWho->selectByValue(LLSD("name"));
            mRuleWhoValue->setText(v);
        }
        else if (head == "key")
        {
            mRuleWho->selectByValue(LLSD("key"));
            if (!mRulePerson->selectByValue(LLSD(value)))
            {
                mRulePerson->add("The person chosen before (" + cap(value, 8) + "...)", LLSD(value));
                mRulePerson->selectByValue(LLSD(value));
            }
        }
        else if (head == "group" && !value.empty()) { mRuleWho->selectByValue(LLSD("groupname")); mRuleWhoValue->setText(value); }
        else if ((head == "kind" || head == "pack" || head == "tag") && !value.empty()) { mRuleWho->selectByValue(LLSD(head)); mRuleWhoValue->setText(value); }
        else if (c == std::string::npos && known(WHOS, head)) mRuleWho->selectByValue(LLSD(head));
        else
        {
            mRuleRawWho = who;
            mRuleWho->add("As set by a script: " + cap(who, 60), LLSD("raw"));
            mRuleWho->selectByValue(LLSD("raw"));
        }
    }
    mRuleDo.clear();
    for (const LLSD& d : llsd::inArray(r["do"])) mRuleDo.push_back(d.asString());
    fillRuleActions();
    mRuleAction->selectFirstItem();
    onRuleActionChanged();
    selectOr(mRuleOften, r["once"].asString());
    mRuleCooldown->setValue(LLSD(r.has("cooldown") ? llclamp(r["cooldown"].asReal(), 0.0, 86400.0) : 0.0));
    mRuleGap->setValue(LLSD(r.has("gap") ? llclamp(r["gap"].asReal(), 1.0, 1440.0) : 10.0));
    onRuleOftenChanged();
    onRuleWhoChanged();
    mRuleError->setText(LLStringUtil::null);
    mRuleEditor->setVisible(true);
    mRuleWhen->setFocus(true);
}

void WolfFloaterNPC::closeRuleEditor()
{
    if (mRuleEditor) mRuleEditor->setVisible(false);
    mRuleIndex = -1;
    mRuleDo.clear();
    mRuleRawWho.clear();
}

void WolfFloaterNPC::onRuleWhoChanged()
{
    const std::string w = mRuleWho->getValue().asString();
    const bool text = w == "name" || w == "kind" || w == "pack" || w == "groupname" || w == "tag";
    mRuleWhoValue->setVisible(text);
    mRuleWhoValue->setLabel(w == "name" ? std::string("part of a name, or a pattern with * and ?")
                            : w == "kind" ? std::string("wolf, horse, person...")
                            : w == "tag" ? std::string("a tag (a-z, 0-9, -)") : std::string("the name"));
    mRulePerson->setVisible(w == "key");
    mRulePeopleBtn->setVisible(w == "key");
    mRuleNot->setEnabled(w != "raw");
}

void WolfFloaterNPC::onRuleOftenChanged()
{
    const std::string o = mRuleOften->getValue().asString();
    mRuleCooldown->setVisible(o.empty());
    mRuleGap->setVisible(o == "visit");
}

void WolfFloaterNPC::onRuleActionChanged()
{
    const std::string a = mRuleAction->getValue().asString();
    mRuleActionValue->removeall();
    mRuleActionValue->setTextEntry(LLStringUtil::null);
    const bool needs = !(a == "face" || a == "follow" || a == "approach" || a == "flee");
    mRuleActionValue->setEnabled(needs);
    if (a == "anim" || a == "loop" || a == "stop")
    {
        for (const LLScrollListItem* it : mAnims->getAllData()) mRuleActionValue->add(it->getValue().asString(), it->getValue());
    }
    else if (a == "sound")
    {
        for (const LLSD& s : llsd::inArray(mDetail["sounds"])) mRuleActionValue->add(cap(s.asString(), 64), LLSD(cap(s.asString(), 64)));
    }
    else if (a == "intent")
    {
        for (const char* i : INTENTIONS) mRuleActionValue->add(i, LLSD(i));
    }
    mRuleActionValue->setToolTip(a == "say" ? std::string("What it says (no ;)") : a == "ai" ? std::string("An instruction for one AI line (your Wolf Credits)")
                                 : a == "sound" ? std::string("A sound in its inventory, or a sound's UUID") : std::string());
}

void WolfFloaterNPC::fillRuleActions()
{
    mRuleActions->deleteAllItems();
    S32 i = 0;
    for (const std::string& d : mRuleDo)
    {
        LLSD row;
        row["value"] = i++;
        row["columns"][0]["column"] = "do";
        row["columns"][0]["value"] = doWords(cap(d, 120));
        mRuleActions->addElement(row);
    }
    if (mRuleDo.empty()) mRuleActions->setCommentText("Nothing yet: choose what it does and press Add.");
    mRuleAddActionBtn->setEnabled((S32)mRuleDo.size() < MAX_DO);
}

void WolfFloaterNPC::onRuleAddAction()
{
    mRuleError->setText(LLStringUtil::null);
    if ((S32)mRuleDo.size() >= MAX_DO) { mRuleError->setText(std::string("A rule can do at most 6 things.")); return; }
    const std::string a = mRuleAction->getValue().asString();
    const bool needs = !(a == "face" || a == "follow" || a == "approach" || a == "flee");
    std::string v = trimmed(mRuleActionValue->getSimple());
    if (needs && v.empty()) { mRuleError->setText("Choose or type what to " + lower(labelOf(ACTIONS, a)) + " first."); return; }
    // Source: PROTOCOL.md section 6 "say:<text> (no ;)"; the script form also splits on ; and |
    if (v.find(';') != std::string::npos || v.find('|') != std::string::npos) { mRuleError->setText(std::string("Leave out ; and | (they separate the parts of a rule).")); return; }
    if (utf8Chars(v) > 200) { mRuleError->setText(std::string("That is too long: at most 200 characters.")); return; }
    mRuleDo.push_back(needs ? a + ":" + v : a);
    fillRuleActions();
    mRuleActionValue->setTextEntry(LLStringUtil::null);
}

void WolfFloaterNPC::onRuleRemoveAction()
{
    const S32 i = mRuleActions->getFirstSelectedIndex();
    if (i < 0 || i >= (S32)mRuleDo.size()) { mRuleError->setText(std::string("Select one of the things it does first.")); return; }
    mRuleDo.erase(mRuleDo.begin() + i);
    fillRuleActions();
}

void WolfFloaterNPC::onRuleSave()
{
    mRuleError->setText(LLStringUtil::null);
    // who
    const std::string w = mRuleWho->getValue().asString();
    std::string term;
    if (w == "raw") term = mRuleRawWho;
    else if (w == "name" || w == "kind" || w == "pack" || w == "groupname" || w == "tag")
    {
        std::string v = trimmed(mRuleWhoValue->getText());
        if (v.empty()) { mRuleError->setText("Type the " + std::string(w == "name" ? "name" : w == "kind" ? "kind" : w == "tag" ? "tag" : "name") + " it reacts to."); return; }
        if (w == "name" && v.find_first_of("*?") == std::string::npos) v = "*" + v + "*";   // "contains"
        if (w == "kind" || w == "tag")
        {
            v = lower(v);
            // Source: PROTOCOL.md section 3 species "one lower-case word", tags "1-24 chars [a-z0-9-]"
            if (utf8Chars(v) > 24 || v.find_first_not_of(w == "tag" ? "abcdefghijklmnopqrstuvwxyz0123456789-" : "abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
            {
                mRuleError->setText(std::string(w == "tag" ? "A tag is one word of a-z, 0-9 and - (up to 24)." : "A kind is one word (wolf, horse, person...)."));
                return;
            }
        }
        if ((w == "pack" || w == "groupname") && utf8Chars(v) > 24) { mRuleError->setText(std::string("A pack or group name is at most 24 characters.")); return; }
        if (v.find_first_of(",|;") != std::string::npos) { mRuleError->setText(std::string("Leave out , | and ; in the name.")); return; }
        term = (w == "groupname" ? std::string("group") : w) + ":" + v;
    }
    else if (w == "key")
    {
        const std::string id = mRulePerson->getValue().asString();
        if (id.empty() || !LLUUID::validate(id)) { mRuleError->setText(std::string("Choose the person (Find people near lists who is within 96 m of you).")); return; }
        term = "key:" + id;
    }
    else term = w;
    if (w != "raw" && mRuleNot->get()) term = "not:" + term;
    if (mRuleDo.empty()) { mRuleError->setText(std::string("Add at least one thing it does.")); return; }

    LLSD rules = mDetail["cfg"]["reactions"].isArray() ? mDetail["cfg"]["reactions"] : LLSD::emptyArray();
    std::string id;
    if (mRuleIndex >= 0 && mRuleIndex < (S32)rules.size()) id = rules[mRuleIndex]["id"].asString();
    if (id.empty())
    {
        std::set<std::string> used;
        for (const LLSD& r : llsd::inArray(rules)) used.insert(r["id"].asString());
        for (S32 n = 1; n <= 64 && id.empty(); ++n)
        {
            if (!used.count(llformat("r%d", n))) id = llformat("r%d", n);
        }
    }
    // Source: PROTOCOL.md section 6 {id, when, who, m, do, once, cooldown, gap}
    LLSD r = LLSD::emptyMap();
    r["id"] = id;
    r["when"] = mRuleWhen->getValue().asString();
    r["who"] = term;
    r["m"] = wireNum(std::round((F64)mRuleRange->getValueF32()));
    LLSD d = LLSD::emptyArray();
    for (const std::string& s : mRuleDo) d.append(s);
    r["do"] = d;
    r["once"] = mRuleOften->getValue().asString();
    r["cooldown"] = wireNum(std::round((F64)mRuleCooldown->get()));
    r["gap"] = wireNum(std::round((F64)mRuleGap->get()));
    if (mRuleIndex >= 0 && mRuleIndex < (S32)rules.size()) rules[mRuleIndex] = r;
    else rules.append(r);
    saveRules(rules, "Reaction saved.");
}

void WolfFloaterNPC::onRuleRemove()
{
    const S32 i = mRules->getFirstSelectedIndex();
    const LLSD& rules = mDetail["cfg"]["reactions"];
    if (i < 0 || i >= (S32)rules.size()) return;
    LLSD kept = LLSD::emptyArray();
    for (S32 k = 0; k < (S32)rules.size(); ++k)
    {
        if (k != i) kept.append(rules[k]);
    }
    saveRules(kept, "Reaction removed.");
}

void WolfFloaterNPC::saveRules(const LLSD& rules, const std::string& done_words)
{
    LLSD changes = LLSD::emptyMap();
    changes["reactions"] = rules;
    doUpdate(changes, done_words, false);
}

void WolfFloaterNPC::requestPeople()
{
    // Source: PROTOCOL.md section 2 people -> people [{id, name, dist}] (within 96 m of the agent, nearest first, at most 40)
    LLHandle<LLFloater> h = getHandle();
    mRulePeopleBtn->setEnabled(false);
    WolfNPC::instance().request("people", LLSD::emptyMap(), [h](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        self->mRulePeopleBtn->setEnabled(true);
        if (!reply["ok"].asBoolean()) { self->mRuleError->setText(cap(reply["error"].asString(), 300)); return; }
        self->mPeople = reply["people"].isArray() ? reply["people"] : LLSD::emptyArray();
        const std::string was = self->mRulePerson->getValue().asString();
        self->mRulePerson->removeall();
        for (const LLSD& p : llsd::inArray(self->mPeople))
        {
            self->mRulePerson->add(llformat("%s (%.0f m)", cap(p["name"].asString(), 64).c_str(), p["dist"].asReal()), LLSD(p["id"].asString()));
        }
        if (self->mPeople.size() == 0) self->mRuleError->setText(std::string("Nobody is within 96 m of you."));
        if (!was.empty() && !self->mRulePerson->selectByValue(LLSD(was)))
        {
            self->mRulePerson->add("The person chosen before (" + cap(was, 8) + "...)", LLSD(was));
            self->mRulePerson->selectByValue(LLSD(was));
        }
    });
}

void WolfFloaterNPC::requestVehicles(WolfNPCForm* form)
{
    // Source: PROTOCOL.md section 2 vehicles -> vehicles [{id, name, dist}]; section 1 drive rule
    LLHandle<LLFloater> h = getHandle();
    setDone("Looking for vehicles near you...");
    WolfNPC::instance().request("vehicles", LLSD::emptyMap(), [h, form](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean()) { self->setError(cap(reply["error"].asString(), 500), reply); return; }
        const LLSD list = reply["vehicles"].isArray() ? reply["vehicles"] : LLSD::emptyArray();
        form->setVehicles(list);
        self->setDone(list.size() ? llformat("%d vehicles near you that it may use.", (S32)list.size())
                                  : std::string("No vehicle within 96 m of you that it may use (yours, or one whose description contains wolfnpc-driver)."));
    });
}

// ─── Scenes (PROTOCOL.md section 7) ─────────────────────────────────────────────────────────────────

void WolfFloaterNPC::loadScenes()
{
    const U32 serial = ++mScenesSerial;
    mSceneList->setCommentText("Reading the scenes...");
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_scenes", LLSD::emptyMap(), [h, serial](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mScenesSerial) return;
        self->mSceneList->deleteAllItems();
        if (!ok)
        {
            self->mSceneList->setCommentText("The scenes could not be read.");
            self->setError("The scenes could not be read: " + cap(error, 300), reply);
            return;
        }
        self->mScenesRead = true;
        self->mScenes = reply["scenes"].isArray() ? reply["scenes"] : LLSD::emptyArray();
        S32 i = 0;
        for (const LLSD& s : llsd::inArray(self->mScenes))
        {
            std::string npcs;
            for (const LLSD& p : llsd::inArray(s["pieces"]))
            {
                if (!npcs.empty()) npcs += ", ";
                npcs += llformat("%d x ", llmax(1, p["count"].asInteger())) + cap(p["label"].asString(), 40);
            }
            LLSD row;
            row["value"] = i++;
            row["columns"][0]["column"] = "title";
            row["columns"][0]["value"] = cap(s["title"].asString(), 64);
            row["columns"][1]["column"] = "npcs";
            row["columns"][1]["value"] = npcs;
            self->mSceneList->addElement(row);
        }
        if (self->mScenes.size() == 0) self->mSceneList->setCommentText("No ready-made scenes yet.");
    });
}

void WolfFloaterNPC::onSceneSelected()
{
    const S32 i = mSceneList->getFirstSelectedIndex();
    if (i < 0 || i >= (S32)mScenes.size()) { mSceneStartBtn->setEnabled(false); mSceneBlurb->setText(LLStringUtil::null); return; }
    mSceneBlurb->setText(cap(mScenes[i]["blurb"].asString(), 600));
    mSceneStartBtn->setEnabled(mScene < 0);
    showSceneCost();
}

void WolfFloaterNPC::showSceneCost()
{
    // [ROUND 2] the total for all its pieces before Rez: every scene NPC is made with AI talk off (PROTOCOL.md section 7), so
    // placing one costs nothing; the live balance beside it
    const S32 i = mSceneList->getFirstSelectedIndex();
    if (i < 0 || i >= (S32)mScenes.size()) { mSceneCost->setText(LLStringUtil::null); return; }
    S32 npcs = 0;
    for (const LLSD& p : llsd::inArray(mScenes[i]["pieces"])) npcs += llclamp(p["count"].asInteger(), 1, 50);
    std::string text = llformat("%d NPCs, 0 Wolf Credits to place (AI talk is off in scenes).", npcs);
    // [ROUND B] a teaching piece: what a lesson costs once its owner turns AI talk on (npc_ai_info perLesson)
    bool teaches = false;
    for (const LLSD& p : llsd::inArray(mScenes[i]["pieces"])) teaches = teaches || p["brain"]["teacher"].asBoolean();
    if (teaches && mAiInfo["perLesson"].isMap())
    {
        const F64 about = mAiInfo["perLesson"]["credits"].asReal(), most = mAiInfo["perLesson"]["maxCredits"].asReal();
        text += " Once you turn its AI talk on, a lesson costs about " + creditsText(LLSD(about)) + pounds(about) + ", at most " + creditsText(LLSD(most)) + ".";
    }
    if (mAiInfo.isMap()) text += " You have " + creditsText(mAiInfo["balance"]) + ".";
    mSceneCost->setText(text);
    // [ROUND 3] what this region's set-up would cost a month with the scene added (npc_month: the scene's NPCs with AI off)
    LLSD extra = LLSD::emptyArray();
    for (S32 k = 0; k < npcs && k < 100; ++k)
    {
        LLSD e;
        e["npc"] = LLUUID::generateNewID().asString();
        e["ai"]["on"] = false;
        extra.append(e);
    }
    const U32 serial = ++mSceneMonthSerial;
    LLHandle<LLFloater> h = getHandle();
    requestMonth(extra, [h, serial, text](const std::string& words)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mSceneMonthSerial || words.empty()) return;
        self->mSceneCost->setText(text + " With it, " + words);
    });
}

void WolfFloaterNPC::requestMonth(const LLSD& extra, std::function<void(const std::string& words)> show)
{
    // Source: PROTOCOL.md §5 npc_month {region, npcs: [{npc, ai: {on, voice, dailyLimit, npcTalk}}]} (at most 100) ->
    // {region: {typical, max, spent30}, assumption, freeMonthly} - credits a month; pounds via pencePerCredit
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn) return;
    LLSD npcs = LLSD::emptyArray();
    for (const LLSD& n : llsd::inArray(mNpcs))
    {
        if (!n["mine"].asBoolean() || npcs.size() >= 100) continue;
        const std::string id = n["id"].asString();
        LLSD e;
        e["npc"] = id;
        // the settings read with get when known; else only what the list says (AI on or off)
        if (mAiCache.has(id) && mAiCache[id].isMap())
        {
            const LLSD& a = mAiCache[id];
            e["ai"]["on"] = a["on"].asBoolean();
            e["ai"]["voice"] = a["voice"].asString();
            e["ai"]["dailyLimit"] = a["dailyLimit"].asInteger();
            e["ai"]["npcTalk"] = a["npcTalk"].asBoolean();
        }
        else e["ai"]["on"] = n["ai"].asBoolean();
        // [ROUND 4] §24: a Game Mode only NPC talks only to players (the list rows carry gameOnly); the brain may use it
        e["gameOnly"] = n["gameOnly"].asBoolean();
        npcs.append(e);
    }
    for (const LLSD& e : llsd::inArray(extra))
    {
        if (npcs.size() < 100) npcs.append(e);
    }
    if (npcs.size() == 0) { show(std::string()); return; }
    LLSD body;
    body["region"] = rgn->getRegionID().asString();
    body["npcs"] = npcs;
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_month", body, [h, show](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!ok) { show("the monthly cost could not be worked out: " + cap(error, 200) + "."); return; }
        const LLSD& r = reply["region"];
        const F64 typical = r["typical"].asReal(), most = r["max"].asReal();
        std::string words = "this set-up will cost about " + creditsText(LLSD(typical)) + " credits a month to run" + self->pounds(typical)
                            + ", at most " + creditsText(LLSD(most)) + self->pounds(most) + ".";
        if (r.has("spent30")) words += " Spent in the last 30 days: " + creditsText(r["spent30"]) + ".";
        if (reply["freeMonthly"].asReal() > 0.0) words += " Your free region credits each month: " + creditsText(reply["freeMonthly"]) + ".";
        show(words);
    });
}

void WolfFloaterNPC::showMonth()
{
    const U32 serial = ++mMonthSerial;
    LLHandle<LLFloater> h = getHandle();
    requestMonth(LLSD::emptyArray(), [h, serial](const std::string& words)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self || serial != self->mMonthSerial) return;
        if (words.empty()) { self->mMonthCost->setText(std::string("You have no NPCs here, so nothing here costs you anything.")); return; }
        std::string w = words;
        w[0] = (char)toupper((unsigned char)w[0]);
        self->mMonthCost->setText("Your NPCs here: " + w);
    });
}

void WolfFloaterNPC::onCostsPage()
{
    // Source: PROTOCOL.md §5 npc_ai_info costsUrl ("" until set); only a Wolf Territories page is opened (onBuyCredits rule)
    // [ROUND 4] costsUrl "" until the brain sets it: the account's AI costs page
    std::string url = mAiInfo["costsUrl"].asString();
    if (url.empty()) url = "https://www.wolf-grid.com/index.php?f=account&p=aicosts";
    if (url.rfind("https://www.wolf-grid.com/", 0) == 0 || url.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(url);
    else setError("The AI costs page address did not come from Wolf Territories, so it was not opened.");
}

// ─── [ROUND 3] Game tab and Game Master (PROTOCOL.md §12-§21) ────────────────────────────────────────

void WolfFloaterNPC::refreshDetailTabs()
{
    if (!mDetailTabs) return;
    const LLSD n = selectedNpc();
    const bool edit = mDetailOwner ? mDetail["canEdit"].asBoolean() : (n.isMap() && n["canEdit"].asBoolean());
    mDetailTabs->setTabVisibility(mSettingsTab, edit);
    mDetailTabs->setTabVisibility(mRulesTab, edit);
    // [ROUND 4] Learning and Places: its owner (the brain's npc_learn_* / npc_skill_* are owner only, §5)
    mDetailTabs->setTabVisibility(mMindTab, edit && mDetailOwner);
    mDetailTabs->setTabVisibility(mPlacesTab, edit && mDetailOwner);
    // Game: only when it can fight in game mode, already uses a game part, or the owner asked for it (Paul: simple and fun)
    LLSD now;
    std::string ignored;
    const bool fighting = mDetailOwner && mEditForm->cfg(now, ignored) && (now["game"]["huntable"].asBoolean() || now["game"]["hunt"].asBoolean());
    const bool game = edit && mDetailOwner && (mGameWanted || fighting || WolfNPCGame::hasGameParts(mDetail["cfg"]));
    mDetailTabs->setTabVisibility(mGameTab, game);
    // Game Master: §19 "The NPC's owner (MayEdit)"
    mDetailTabs->setTabVisibility(mGmTab, edit);
}

void WolfFloaterNPC::openGameTab()
{
    mGameWanted = true;
    refreshDetailTabs();
    mDetailTabs->selectTabPanel(mGameTab);
}

void WolfFloaterNPC::refreshGm()
{
    if (!mGmState) return;
    const std::string name = mDetail.isMap() ? cap(mDetail["name"].asString(), 48) : std::string("it");
    if (mGmControlling) mGmState->setText("You are controlling " + name + ".");
    else if (!mGmBy.empty()) mGmState->setText(mGmBy + " is running " + name + ".");
    else mGmState->setText(name + " is living its own life. Take control to run it yourself.");
    mGmState->setColor(mGmControlling ? caution() : normalText());   // controlling: said in words and in the caution colour
    mGmTab->getChild<LLButton>("gm_take")->setEnabled(!mGmControlling && mGmBy.empty());
    mGmTab->getChild<LLButton>("gm_release")->setEnabled(mGmControlling);
    for (const char* w : { "gm_say", "gm_say_btn", "gm_click", "gm_here", "gm_face_me", "gm_who", "gm_people", "gm_follow", "gm_face",
                           "gm_unfollow", "gm_anim", "gm_anim_once", "gm_anim_loop", "gm_anim_stop", "gm_event", "gm_event_start", "gm_event_end" })
    {
        mGmTab->getChild<LLView>(w)->setEnabled(mGmControlling);
    }
}

void WolfFloaterNPC::gmSend(const std::string& op, const LLSD& fields, const std::string& done_words)
{
    if (mDetailId.empty()) return;
    clearStatus();
    // Source: PROTOCOL.md §19 gm_take / gm_say / gm_goto / gm_follow / gm_face / gm_anim / gm_event / gm_release -> gm
    LLSD f = fields.isMap() ? fields : LLSD::emptyMap();
    f["id"] = mDetailId;
    const std::string id = mDetailId;
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request(op, f, [h, op, id, done_words](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean())
        {
            self->setError(cap(reply["error"].asString(), 500), reply);
            return;
        }
        if (id != self->mDetailId) return;
        if (op == "gm_take" || op == "gm_release")
        {
            self->mGmControlling = reply["controlling"].asBoolean();
            if (self->mGmControlling) { std::string me; LLAgentUI::buildFullname(me); self->mGmBy = me; }
            else self->mGmBy.clear();
            const std::string name = cap(self->mDetail["name"].asString(), 48);
            self->setDone(self->mGmControlling ? "You are controlling " + name + ". It does only what you tell it." : name + " is living its own life again.");
            self->refreshGm();
            self->requestList();
            return;
        }
        if (!done_words.empty()) self->setDone(done_words);
    });
}

void WolfFloaterNPC::onGmEnded(const std::string& id, const std::string& why)
{
    if (id != mDetailId) return;
    mGmControlling = false;
    mGmBy.clear();
    refreshGm();
    setDone("Your control of " + cap(mDetail["name"].asString(), 48) + " ended" + (why.empty() ? std::string(".") : ": " + why));
}

void WolfFloaterNPC::gmPeople()
{
    LLHandle<LLFloater> h = getHandle();
    WolfNPC::instance().request("people", LLSD::emptyMap(), [h](const LLSD& reply)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!reply["ok"].asBoolean()) { self->setError(cap(reply["error"].asString(), 500), reply); return; }
        self->mGmWho->removeall();
        for (const LLSD& p : llsd::inArray(reply["people"]))
        {
            self->mGmWho->add(llformat("%s (%.0f m)", cap(p["name"].asString(), 64).c_str(), p["dist"].asReal()), LLSD(p["id"].asString()));
        }
        self->mGmWho->selectFirstItem();
        if (self->mGmWho->getItemCount() == 0) self->setDone("Nobody is within 96 m of you.");
    });
}

void WolfFloaterNPC::onSceneStart()
{
    const S32 i = mSceneList->getFirstSelectedIndex();
    if (i < 0 || i >= (S32)mScenes.size()) return;
    clearStatus();
    mScene = i;
    mSceneStepN = 0;
    mSceneSteps = 0;
    for (const LLSD& p : llsd::inArray(mScenes[i]["pieces"])) mSceneSteps += llclamp(p["count"].asInteger(), 1, 50);
    mSceneStartBtn->setEnabled(false);
    mSceneList->setEnabled(false);
    mScenePanel->setVisible(true);
    if (!mAvatarsRead) loadAvatars();
    showSceneStep();
}

void WolfFloaterNPC::showSceneStep()
{
    if (mScene < 0 || mScene >= (S32)mScenes.size()) return;
    const LLSD& scene = mScenes[mScene];
    // which piece is step n (a piece of count 5 is 5 steps)
    S32 n = mSceneStepN, k = 0;
    LLSD piece;
    for (const LLSD& p : llsd::inArray(scene["pieces"]))
    {
        const S32 c = llclamp(p["count"].asInteger(), 1, 50);
        if (n < c) { piece = p; k = n; break; }
        n -= c;
    }
    if (!piece.isMap())
    {
        const std::string title = cap(scene["title"].asString(), 64);
        onSceneStop();
        setDone("Done: every NPC of " + title + " is in place. Change any of them on the On this region tab.");
        return;
    }
    const S32 count = llclamp(piece["count"].asInteger(), 1, 50);
    const std::string kind = piece["kind"].asString() == "person" ? "person" : "animal";
    mSceneStep->setText(llformat("Step %d of %d: ", mSceneStepN + 1, mSceneSteps) + cap(piece["step"].asString(), 300));
    mScenePiece->setText(cap(piece["label"].asString(), 64) + (count > 1 ? llformat(" (%d of %d)", k + 1, count) : std::string())
                         + (kind == "person" ? " - a person" : " - an animal"));
    const std::string was = mSceneAvatar->getValue().asString();
    mSceneAvatar->removeall();
    for (const LLSD& a : llsd::inArray(mAvatars))
    {
        if (a["kind"].asString() != kind) continue;
        const std::string species = cap(lower(a["species"].asString()), 24);
        mSceneAvatar->add(cap(a["name"].asString(), 48) + (species.empty() ? std::string() : " (" + species + ")"), LLSD(a["job"].asString()));
    }
    const bool none = mSceneAvatar->getItemCount() == 0;
    if (!none) selectOr(mSceneAvatar, was);
    mSceneAvatar->setEnabled(!none);
    mSceneName->setText(cap(piece["label"].asString(), 44) + (count > 1 ? llformat(" %d", k + 1) : std::string()));
    mSceneNone->setText(!mAvatarsRead ? std::string("Looking for your WolfAvatars...")
                        : none ? (kind == "person" ? std::string("You have no WolfAvatars that are people, so this one cannot be placed. Make one in WolfAvatars, or skip it.")
                                                   : std::string("You have no WolfAvatars that are animals, so this one cannot be placed. Make one in WolfAvatars, or skip it."))
                        : std::string());
    mSceneMakeBtn->setVisible(mAvatarsRead && none);
    mSceneRezBtn->setEnabled(!none && !mCreating);
    // [ROUND 3] §7 "optional": true - the window offers Skip for it (and for a piece they have no avatar for, so they are
    // never stuck: Stop the scene is the other way out)
    const bool optional = piece["optional"].asBoolean();
    mSceneSkipBtn->setVisible(optional || none);
    if (optional) mScenePiece->setText(mScenePiece->getText() + " (optional - you may skip it)");
}

void WolfFloaterNPC::onSceneRez()
{
    clearStatus();
    if (mCreating || mScene < 0 || mScene >= (S32)mScenes.size()) return;
    const LLSD& scene = mScenes[mScene];
    S32 n = mSceneStepN;
    LLSD piece;
    for (const LLSD& p : llsd::inArray(scene["pieces"]))
    {
        const S32 c = llclamp(p["count"].asInteger(), 1, 50);
        if (n < c) { piece = p; break; }
        n -= c;
    }
    if (!piece.isMap()) return;
    const std::string job = mSceneAvatar->getValue().asString();
    if (job.empty()) { setError("Choose one of your WolfAvatars for it first."); return; }
    const std::string name = trimmed(mSceneName->getText());
    if (name.empty() || utf8Chars(name) > 48) { setError("Give it a name of 1 to 48 characters."); return; }
    if (rlvBlocksRez()) { setError("It can't be rezzed now: an RLV restriction (rez) is on."); return; }
    LLVector3 pos, agent;
    F32 facing = 0.f;
    if (!placeInFront(pos, facing, agent)) { setError("Your position is not known yet - try again in a moment."); return; }
    // Source: PROTOCOL.md section 7 - "made by the normal create, with home = where they stand and pack = the scene's group
    // name. AI is off in every scene."
    LLSD cfg = piece["cfg"].isMap() ? piece["cfg"] : LLSD::emptyMap();
    // a piece with its own pack keeps it (the survivor never joins the zombie horde); the others get the scene's group
    std::string pack = cap(cfg["pack"].asString(), 24);
    if (pack.empty()) pack = cap(scene["group"].asString(), 24);
    if (pack.empty()) pack = cap(scene["title"].asString(), 24);
    cfg["name"] = name;
    cfg["pack"] = pack;
    cfg["home"] = wireVec(agent);
    cfg["facing"] = wireNum(facing);
    LLSD ai = cfg["ai"].isMap() ? cfg["ai"] : LLSD::emptyMap();
    ai["on"] = false;
    cfg["ai"] = ai;
    LLSD fields;
    fields["job"] = job;
    fields["name"] = name;
    fields["cfg"] = cfg;
    fields["pos"] = wireVec(pos);
    fields["facing"] = wireNum(facing);
    if (piece["brain"].isMap()) fields["brain"] = piece["brain"];   // kept by the window only (doCreate strips it before sending)
    doCreate(fields, name);
}

void WolfFloaterNPC::queueBrain(const std::string& npc, const std::string& name, const LLSD& brain)
{
    PendingBrain b;
    b.mNpc = npc;
    b.mName = name;
    b.mBrain = brain;
    b.mAt = nowSec() + 5.0;   // the region reports a new NPC to the brain in a batch (at most once a minute): first try soon
    mPendingBrain.push_back(b);
}

void WolfFloaterNPC::applyBrain(size_t index)
{
    // Source: PROTOCOL.md §5 round B - npc_learn_set {teacher, lookups}; npc_style_list -> the id of each key -> npc_style_set.
    // The brain refuses an NPC the region has not reported yet (404): tried again every 30 s, 8 times at most.
    PendingBrain b = mPendingBrain[index];
    mPendingBrain.erase(mPendingBrain.begin() + (S64)index);
    LLSD body;
    body["npc"] = b.mNpc;
    if (b.mBrain.has("teacher")) body["teacher"] = b.mBrain["teacher"].asBoolean();
    if (b.mBrain.has("lookups")) body["lookups"] = b.mBrain["lookups"].asBoolean();
    LLHandle<LLFloater> h = getHandle();
    WolfAdventure::api("npc_learn_set", body, [h, b](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterNPC* self = dynamic_cast<WolfFloaterNPC*>(h.get());
        if (!self) return;
        if (!ok)
        {
            if (b.mTries < 8)
            {
                PendingBrain again = b;
                ++again.mTries;
                again.mAt = nowSec() + 30.0;
                self->mPendingBrain.push_back(again);
                return;
            }
            self->setError("Setting up " + b.mName + "'s teaching and voice did not work: " + cap(error, 300) + " Set it in Edit > Learning.", reply);
            return;
        }
        if (!b.mBrain["styles"].isArray() || b.mBrain["styles"].size() == 0)
        {
            self->setDone(b.mName + " is set up.");
            return;
        }
        LLSD list;
        list["npc"] = b.mNpc;
        WolfAdventure::api("npc_style_list", list, [h, b](bool ok2, const LLSD& reply2, const std::string& error2)
        {
            WolfFloaterNPC* self2 = dynamic_cast<WolfFloaterNPC*>(h.get());
            if (!self2) return;
            if (!ok2) { self2->setError("Its voice could not be set: " + cap(error2, 300) + " Choose it in Edit > Learning > Growing.", reply2); return; }
            for (const LLSD& key : llsd::inArray(b.mBrain["styles"]))
            {
                for (const LLSD& st : llsd::inArray(reply2["styles"]))
                {
                    if (st["key"].asString() != key.asString()) continue;
                    LLSD set;
                    set["npc"] = b.mNpc;
                    set["id"] = st["id"];
                    set["on"] = true;
                    const std::string name = b.mName;
                    WolfAdventure::api("npc_style_set", set, [h, name](bool ok3, const LLSD& reply3, const std::string& error3)
                    {
                        WolfFloaterNPC* self3 = dynamic_cast<WolfFloaterNPC*>(h.get());
                        if (!self3) return;
                        if (!ok3) self3->setError("Its voice could not be set: " + cap(error3, 300), reply3);
                        else self3->setDone(name + " is set up, with its voice.");
                    });
                }
            }
        });
    });
}

void WolfFloaterNPC::onSceneSkip()
{
    clearStatus();
    ++mSceneStepN;
    showSceneStep();
}

void WolfFloaterNPC::onSceneStop()
{
    const bool was = mScene >= 0 && mSceneStepN < mSceneSteps;
    mScene = -1;
    mSceneStepN = mSceneSteps = 0;
    mScenePanel->setVisible(false);
    mSceneList->setEnabled(true);
    onSceneSelected();
    if (was) setDone("Scene stopped. The NPCs already placed stay.");
}
