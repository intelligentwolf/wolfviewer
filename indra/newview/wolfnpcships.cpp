/**
 * @file wolfnpcships.cpp
 * @brief WolfViewer: the WolfNPC window's NPC ships tab. See wolfnpcships.h.
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

#include "wolfnpcships.h"

#include <cmath>
#include <initializer_list>
#include <set>

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llparcel.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llviewerparcelmgr.h"
#include "wolfgame.h"
#include "wolfnpc.h"

namespace
{
    // Source: wolffloaternpc.cpp:354-359 - the window paints its own scene, so its words have these fixed colours
    // (NPC_CREAM 16.3:1, NPC_MUTED 12.8:1, NPC_ERR 11.1:1 on the body plate)
    const LLColor4 SHIP_CREAM(0.95f, 0.92f, 0.84f, 1.f);
    const LLColor4 SHIP_MUTED(0.80f, 0.82f, 0.86f, 1.f);
    const LLColor4 SHIP_ERR(1.f, 0.70f, 0.52f, 1.f);

    // Source: PROTOCOL.md §30.4 - the four classes an owner may choose; §30.1 the haven name is 1-40 characters
    constexpr size_t CLASS_SLOTS = 4;
    constexpr size_t HAVEN_NAME_MAX = 40;

    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    std::string trimmed(std::string s)
    {
        LLStringUtil::trim(s);
        return s;
    }

    size_t utf8Chars(const std::string& s) { return utf8str_to_wstring(s).size(); }

    /** A whole number with a thousands separator: "2,140" (as the pirates' own words, NpcFleet.M). */
    std::string thousands(F64 m)
    {
        if (!std::isfinite(m)) return "?";
        const S64 v = (S64)std::llround(std::fabs(m));
        const std::string digits = llformat("%lld", (long long)v);
        std::string out;
        for (size_t i = 0; i < digits.size(); ++i)
        {
            if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }

    std::string metres(F64 m) { return thousands(m) + " m"; }

    /** Which way `dx` east and `dy` north lies, in the eight compass words. */
    std::string compass(F64 dx, F64 dy)
    {
        static const char* const WORDS[8] = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" };
        F64 b = std::atan2(dx, dy) * (F64)RAD_TO_DEG;   // 0 = north, clockwise (region +X east, +Y north)
        if (b < 0.0) b += 360.0;
        const S32 i = (S32)std::lround(b / 45.0) % 8;
        return WORDS[i];
    }

    LLSD row(S32 value, std::initializer_list<std::pair<const char*, std::string>> cols)
    {
        LLSD r;
        r["value"] = value;
        S32 i = 0;
        for (const auto& c : cols)
        {
            r["columns"][i]["column"] = c.first;
            r["columns"][i]["value"] = c.second;
            ++i;
        }
        return r;
    }

    /** Source: PROTOCOL.md §30.4 hunterCount - "hunters on: 2 with 6+ pirates, 1 with 3-5, else 0" (shown while editing,
     *  before the region has the new numbers). */
    S32 huntersFor(bool on, S32 pirates)
    {
        if (!on) return 0;
        return pirates >= 6 ? 2 : pirates >= 3 ? 1 : 0;
    }

    /** "Black Gull (pirate ship)" with its kind "pirate sloop" -> "Black Gull (pirate sloop)". */
    std::string shipName(const std::string& name, const std::string& kind)
    {
        std::string base = name;
        const size_t p = base.rfind(" (");
        if (p != std::string::npos && !base.empty() && base.back() == ')') base = base.substr(0, p);
        return kind.empty() ? base : base + " (" + kind + ")";
    }
}

WolfNPCShips::WolfNPCShips(LLPanel* panel, ask_fn ask, say_fn error, say_fn done)
:   mPanel(panel), mAsk(ask), mError(error), mDone(done)
{
    mNote = mPanel->getChild<LLTextBox>("sh_note");
    mBody = mPanel->getChild<LLPanel>("sh_body");
    mOn = mBody->getChild<LLCheckBoxCtrl>("sh_on");
    mOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwitch(); });
    mChart = mBody->getChild<LLTextBox>("sh_chart");
    mPirates = mBody->getChild<LLSpinCtrl>("sh_pirates");
    mTraders = mBody->getChild<LLSpinCtrl>("sh_traders");
    mTotal = mBody->getChild<LLTextBox>("sh_total");
    mHunters = mBody->getChild<LLCheckBoxCtrl>("sh_hunters");
    mHuntersNote = mBody->getChild<LLTextBox>("sh_hunters_note");
    auto edited = [this](LLUICtrl*, const LLSD&) { mDirty = true; updateTotals(); };
    mPirates->setCommitCallback(edited);
    mTraders->setCommitCallback(edited);
    mHunters->setCommitCallback(edited);
    mAmbient = mBody->getChild<LLCheckBoxCtrl>("sh_ambient");
    mAmbient->setCommitCallback(edited);
    mMore = mBody->getChild<LLButton>("sh_more");
    mClasses = mBody->getChild<LLPanel>("sh_classes");
    mMore->setCommitCallback([this](LLUICtrl*, const LLSD&) { mClasses->setVisible(mMore->getToggleState()); });
    for (size_t i = 0; i < CLASS_SLOTS; ++i)
    {
        mClass[i] = mClasses->getChild<LLCheckBoxCtrl>(llformat("sh_cls_%u", (U32)i));
        mClass[i]->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDirty = true; });
    }
    mBody->getChild<LLButton>("sh_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    mHavenName = mBody->getChild<LLLineEditor>("sh_haven_name");
    mHavenSize = mBody->getChild<LLComboBox>("sh_haven_size");
    mHavenSize->selectByValue(LLSD("medium"));
    mBody->getChild<LLButton>("sh_haven_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHavenAdd(); });
    mHavens = mBody->getChild<LLScrollListCtrl>("sh_havens");
    mBody->getChild<LLButton>("sh_haven_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onHavenRemove(); });
    mShips = mBody->getChild<LLScrollListCtrl>("sh_ships");
    mBody->getChild<LLButton>("sh_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { requestState(); });
    mBody->getChild<LLButton>("sh_clear")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClear(); });
    mAwake = mBody->getChild<LLTextBox>("sh_awake");
    showNote("Asking the region...", false);
}

void WolfNPCShips::onRegionChanged()
{
    ++mSerial;
    mReading = false;   // the old region's answer is dropped by the serial
    mHelloDone = false;
    mHolder = false;
    mDirty = false;
    mHello = LLSD();
    mState = LLSD();
    mHavens->deleteAllItems();
    mShips->deleteAllItems();
    showNote("Asking the region...", false);
}

void WolfNPCShips::onShown()
{
    if (mReading) return;   // a hello or get is already on its way (the tab shown twice before it was answered)
    // a tab shown again with the region known: the state again (where the ships are now); else hello first
    if (mHelloDone && mHolder) requestState();
    else sendHello();
}

void WolfNPCShips::showNote(const std::string& text, bool problem)
{
    mNote->setText(text);
    mNote->setColor(problem ? SHIP_ERR : SHIP_CREAM);
    mNote->setVisible(true);
    mBody->setVisible(false);
}

void WolfNPCShips::sendHello()
{
    if (mReading) return;
    mReading = true;
    const U32 serial = mSerial;
    showNote("Asking the region...", false);
    // Source: PROTOCOL.md §30.3 hello -> {version, holder; a holder also size, max, classes [{id, name, guns}], havenSizes}
    WolfNPC::instance().requestShips("hello", LLSD::emptyMap(), [this, life = std::weak_ptr<int>(mLife), serial](const LLSD& reply)
    {
        if (life.expired() || serial != mSerial) return;
        mReading = false;
        if (!reply["ok"].asBoolean())
        {
            // "NPC ships are not available on this region yet." (no answer in 6 s) or the region's own words
            showNote(cap(reply["error"].asString(), 300), true);
            return;
        }
        mHelloDone = true;
        mHello = reply;
        mHolder = reply["holder"].asBoolean();
        if (!mHolder)
        {
            // Source: PROTOCOL.md §30.1 - everything but hello is for the estate owner or an estate manager
            showNote("Only the region owner or an estate manager can set up NPC ships on this region.", false);
            return;
        }
        mMax = llclamp(reply["max"].asInteger(), 1, 64);
        mPirates->setMaxValue((F32)mMax);
        mTraders->setMaxValue((F32)mMax);
        const LLSD& classes = reply["classes"];
        for (size_t i = 0; i < CLASS_SLOTS; ++i)
        {
            const bool have = classes.isArray() && i < (size_t)classes.size();
            mClassId[i] = have ? cap(classes[(S32)i]["id"].asString(), 64) : std::string();
            mClass[i]->setVisible(have);
            if (have)
            {
                const S32 guns = classes[(S32)i]["guns"].asInteger();
                mClass[i]->setLabel(cap(classes[(S32)i]["name"].asString(), 40) + (guns > 0 ? llformat(" (%d guns)", guns) : std::string()));
            }
        }
        requestState();
    });
}

void WolfNPCShips::requestState()
{
    if (!mHelloDone) { sendHello(); return; }
    if (!mHolder || mReading) return;
    mReading = true;
    const U32 serial = mSerial;
    WolfNPC::instance().requestShips("get", LLSD::emptyMap(), [this, life = std::weak_ptr<int>(mLife), serial](const LLSD& reply)
    {
        if (life.expired() || serial != mSerial) return;
        mReading = false;
        if (!reply["ok"].asBoolean())
        {
            const std::string why = cap(reply["error"].asString(), 300);
            if (!mState.isMap()) showNote(why, true);   // nothing to show yet: the reason in the tab itself
            mError("The NPC ships could not be read: " + why);
            return;
        }
        applyState(reply);
    });
}

void WolfNPCShips::send(const std::string& op, const LLSD& fields, const std::string& done_words, std::function<void(bool ok)> after)
{
    const U32 serial = mSerial;
    WolfNPC::instance().requestShips(op, fields, [this, life = std::weak_ptr<int>(mLife), serial, done_words, after](const LLSD& reply)
    {
        if (life.expired() || serial != mSerial) return;
        const bool ok = reply["ok"].asBoolean();
        if (!ok)
        {
            mError(cap(reply["error"].asString(), 300));
            if (after) after(false);   // first: it may keep unsaved edits on screen (mDirty) before the state is shown
            // Source: PROTOCOL.md §30.3 - a refusal of set / haven_add / haven_remove / clear is a `ships` reply with ok
            // false, error, and the state as it now is: show it
            if (reply["op"].asString() == "ships" && reply["cfg"].isMap()) applyState(reply);
            return;
        }
        applyState(reply);
        mDone(done_words);
        if (after) after(true);
    });
}

void WolfNPCShips::applyState(const LLSD& reply)
{
    // Source: PROTOCOL.md §30.4 the state: cfg, hunterCount, chart, awake, ships, here
    mState = reply;
    mNote->setVisible(false);
    mBody->setVisible(true);
    fillControls();
    fillHavens();
    fillShips();
}

void WolfNPCShips::fillControls()
{
    const LLSD& cfg = mState["cfg"];
    mOn->set(cfg["on"].asBoolean());
    if (!mDirty)
    {
        mPirates->set((F32)llclamp(cfg["pirates"].asInteger(), 0, mMax));
        mTraders->set((F32)llclamp(cfg["traders"].asInteger(), 0, mMax));
        mHunters->set(cfg["hunters"].asBoolean());
        mAmbient->set(cfg["ambient"].asBoolean());
        std::set<std::string> chosen;
        const LLSD& cls = cfg["classes"];
        if (cls.isArray())
        {
            for (LLSD::array_const_iterator it = cls.beginArray(); it != cls.endArray(); ++it) chosen.insert(it->asString());
        }
        for (size_t i = 0; i < CLASS_SLOTS; ++i) mClass[i]->set(!mClassId[i].empty() && chosen.count(mClassId[i]) > 0);
    }
    updateTotals();

    // the chart: why they cannot sail here (caution), a warning, or how much deep water they have
    const LLSD& chart = mState["chart"];
    if (!chart["ok"].asBoolean())
    {
        mChart->setText(cap(chart["why"].asString(), 400));
        mChart->setColor(SHIP_ERR);
    }
    else if (!chart["warn"].asString().empty())
    {
        mChart->setText(cap(chart["warn"].asString(), 400));
        mChart->setColor(SHIP_ERR);
    }
    else
    {
        const F64 km2 = chart["seaKm2"].asReal();
        mChart->setText(std::isfinite(km2) && km2 > 0.0 ? llformat("The ships have %.1f square km of deep water here.", km2) : std::string());
        mChart->setColor(SHIP_MUTED);
    }

    if (!cfg["on"].asBoolean()) mAwake->setText(std::string("NPC ships are off on this region."));
    else if (mState["awake"].asBoolean()) mAwake->setText(std::string("Someone is on the region, so the ships are sailing."));
    else mAwake->setText(std::string("The ships lie quiet until someone comes to the region."));
}

void WolfNPCShips::updateTotals()
{
    const S32 pirates = (S32)std::lround(mPirates->get()), traders = (S32)std::lround(mTraders->get());
    mTotal->setText(llformat("%d of %d ships", pirates + traders, mMax));
    mTotal->setColor(pirates + traders > mMax ? SHIP_ERR : SHIP_MUTED);
    // the region's own count when nothing is being edited, else the §30.4 rule for the numbers on screen
    const S32 hunters = mDirty || !mState.isMap() ? huntersFor(mHunters->get(), pirates) : mState["hunterCount"].asInteger();
    if (!mHunters->get()) mHuntersNote->setText(std::string("(fast xebecs that go after fast ships)"));
    else if (hunters <= 0) mHuntersNote->setText(std::string("(fast xebecs that go after fast ships - they need at least 3 pirate ships)"));
    else mHuntersNote->setText(llformat("(fast xebecs that go after fast ships - %d of your pirates)", hunters));
}

void WolfNPCShips::fillHavens()
{
    mHavens->deleteAllItems();
    const LLSD& havens = mState["cfg"]["havens"];
    if (!havens.isArray()) return;
    for (S32 i = 0; i < (S32)havens.size(); ++i)
    {
        const LLSD& h = havens[i];
        const F64 x0 = h["x0"].asReal(), y0 = h["y0"].asReal(), x1 = h["x1"].asReal(), y1 = h["y1"].asReal();
        mHavens->addElement(row(i, {
            { "name", cap(h["name"].asString(), HAVEN_NAME_MAX) },
            { "where", llformat("%d, %d", (S32)std::lround((x0 + x1) / 2.0), (S32)std::lround((y0 + y1) / 2.0)) },
            { "size", thousands(x1 - x0) + " x " + metres(y1 - y0) },
        }));
    }
}

void WolfNPCShips::fillShips()
{
    mShips->deleteAllItems();
    const LLSD& ships = mState["ships"];
    const LLSD& here = mState["here"];
    const bool haveHere = here.isArray() && here.size() >= 2;
    if (!ships.isArray()) return;
    for (S32 i = 0; i < (S32)ships.size(); ++i)
    {
        const LLSD& s = ships[i];
        std::string where;
        if (s["atSea"].asBoolean() && s.has("x") && s.has("y"))
        {
            const F64 x = s["x"].asReal(), y = s["y"].asReal();
            where = llformat("(%d, %d)", (S32)std::lround(x), (S32)std::lround(y));
            if (haveHere)
            {
                const F64 dx = x - here[0].asReal(), dy = y - here[1].asReal();
                const F64 d = std::sqrt(dx * dx + dy * dy);
                where = (d < 20.0 ? std::string("beside you") : metres(d) + " " + compass(dx, dy) + " of you") + " " + where;
            }
        }
        mShips->addElement(row(i, {
            { "name", shipName(cap(s["name"].asString(), 64), cap(s["kind"].asString(), 40)) },
            { "doing", cap(s["doing"].asString(), 80) },
            { "where", where },
        }));
    }
}

void WolfNPCShips::onSwitch()
{
    // the switch acts at once (the region may refuse it: "NPC ships can't sail on this region: ...", §30.3)
    const bool on = mOn->get();
    LLSD cfg;
    cfg["on"] = on;
    LLSD fields;
    fields["cfg"] = cfg;
    send("set", fields, on ? "NPC ships are on: they sail whenever someone is on the region." : "NPC ships are off: they have left the sea.",
         [this, life = std::weak_ptr<int>(mLife), on](bool ok)
         {
             if (life.expired()) return;
             if (!ok) mOn->set(!on);   // back as the region still has it
         });
}

void WolfNPCShips::onSave()
{
    if (!mState.isMap()) { mError("The NPC ships have not been read yet - press Refresh."); return; }
    const LLSD& now = mState["cfg"];
    const S32 pirates = (S32)std::lround(mPirates->get()), traders = (S32)std::lround(mTraders->get());
    // Source: PROTOCOL.md §30.3 refusals - pirates + traders above the most; no class chosen
    if (pirates + traders > mMax) { mError(llformat("Pirate and peaceful ships together are at most %d.", mMax)); return; }
    LLSD classes = LLSD::emptyArray();
    std::set<std::string> chosen, before;
    for (size_t i = 0; i < CLASS_SLOTS; ++i)
    {
        if (!mClassId[i].empty() && mClass[i]->get()) { classes.append(LLSD(mClassId[i])); chosen.insert(mClassId[i]); }
    }
    if (chosen.empty()) { mError("Choose at least one kind of ship (More options)."); return; }
    if (now["classes"].isArray())
    {
        for (LLSD::array_const_iterator it = now["classes"].beginArray(); it != now["classes"].endArray(); ++it) before.insert(it->asString());
    }
    LLSD cfg = LLSD::emptyMap();
    if (pirates != now["pirates"].asInteger()) cfg["pirates"] = pirates;
    if (traders != now["traders"].asInteger()) cfg["traders"] = traders;
    if (mHunters->get() != now["hunters"].asBoolean()) cfg["hunters"] = mHunters->get();
    if (mAmbient->get() != now["ambient"].asBoolean()) cfg["ambient"] = mAmbient->get();   // §30.3 set ambient
    if (chosen != before) cfg["classes"] = classes;
    if (cfg.size() == 0) { mDirty = false; updateTotals(); mDone("Nothing has changed."); return; }
    LLSD fields;
    fields["cfg"] = cfg;
    mDirty = false;   // the reply's numbers are what the region kept
    send("set", fields, "Saved. Ships that changed sail in again in a moment.", [this, life = std::weak_ptr<int>(mLife)](bool ok)
    {
        if (life.expired()) return;
        if (!ok) mDirty = true;   // still unsaved: keep what is on screen
    });
}

void WolfNPCShips::onHavenAdd()
{
    std::string name = trimmed(mHavenName->getText());
    // only a name the resident typed can be refused for its length; the parcel's own name is shortened to fit
    if (!name.empty() && utf8Chars(name) > HAVEN_NAME_MAX) { mError("A haven's name is at most 40 characters."); return; }
    if (name.empty())
    {
        LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
        if (parcel)
        {
            LLWString w = utf8str_to_wstring(trimmed(cap(parcel->getName(), 200)));   // whole characters, never a split one
            if (w.size() > HAVEN_NAME_MAX) w.resize(HAVEN_NAME_MAX);
            name = trimmed(wstring_to_utf8str(w));
        }
    }
    if (name.empty())
    {
        const S32 n = mState["cfg"]["havens"].isArray() ? (S32)mState["cfg"]["havens"].size() : 0;
        name = llformat("Haven %d", n + 1);
    }
    LLSD fields;
    fields["name"] = name;
    const std::string size = mHavenSize->getValue().asString();
    fields["size"] = size.empty() ? std::string("medium") : size;
    send("haven_add", fields, "\"" + name + "\" is a safe haven: no pirate fires within about 600 m of it.", [this, life = std::weak_ptr<int>(mLife)](bool ok)
    {
        if (!life.expired() && ok) mHavenName->clear();
    });
}

void WolfNPCShips::onHavenRemove()
{
    const S32 i = mHavens->getFirstSelectedIndex();
    const LLSD& havens = mState["cfg"]["havens"];
    if (i < 0 || !havens.isArray() || i >= (S32)havens.size()) { mError("Choose a haven in the list first."); return; }
    const std::string name = cap(havens[i]["name"].asString(), HAVEN_NAME_MAX);
    mAsk("Remove the safe haven \"" + name + "\"? Pirates may then fight near it.", "Remove haven", [this, life = std::weak_ptr<int>(mLife), i, name]()
    {
        if (life.expired()) return;
        // the list may have been read again while the question was up: the same haven still at that place?
        const LLSD& now = mState["cfg"]["havens"];
        if (!now.isArray() || i >= (S32)now.size() || cap(now[i]["name"].asString(), HAVEN_NAME_MAX) != name)
        {
            mError("The havens changed meanwhile - choose the one to remove again.");
            return;
        }
        LLSD fields;
        fields["index"] = i;
        send("haven_remove", fields, "\"" + name + "\" is no longer a safe haven.");
    });
}

void WolfNPCShips::onClear()
{
    mAsk("Take every NPC ship off the sea now? If NPC ships are on, fresh ones sail in a minute later.", "Remove every ship now",
         [this, life = std::weak_ptr<int>(mLife)]()
         {
             if (life.expired()) return;
             send("clear", LLSD::emptyMap(), "Every NPC ship has left the sea.");
         });
}
