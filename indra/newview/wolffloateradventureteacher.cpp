/**
 * @file wolffloateradventureteacher.cpp
 * @brief WolfViewer: Wolf Adventure Teacher Console (STUDIO_AND_CONSOLE_V1.md §4).
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

#include "wolffloateradventureteacher.h"

#include <boost/json.hpp>

#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "wolfadventure.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureTeacherView> r_wolf_adventure_teacher_view("wolf_adventure_teacher_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as the GM Console
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24

    // BRAIN_API_V1.md §5.5: "The Teacher Console polls it [class_get] every 5 s."
    const F64 CLASS_POLL_SEC = 5.0;
    // Source: wolfadventure_gm_lib.php wa_api_lrs_add: version 2.0.0 or 1.0.3.
    const char* const LRS_VERSIONS[] = { "2.0.0", "1.0.3" };
    const char* const SETTING = "WolfAdventureTeacher";

    LLSD parseJson(const std::string& text)
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    bool languageOk(const std::string& l)
    {
        // Source: wolfadventure_gm_lib.php wag_class_settings_in: ^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8}){0,2}$ or empty.
        if (l.empty()) return true;
        size_t i = 0;
        while (i < l.size() && isalpha((unsigned char)l[i])) ++i;
        if (i < 2 || i > 3) return false;
        S32 parts = 0;
        while (i < l.size())
        {
            if (l[i] != '-' || ++parts > 2) return false;
            size_t j = ++i;
            while (j < l.size() && isalnum((unsigned char)l[j])) ++j;
            if (j - i < 2 || j - i > 8) return false;
            i = j;
        }
        return true;
    }
}

WolfAdventureTeacherView::WolfAdventureTeacherView(const Params& p)
:   WolfAdventureDeskView(p)
{
    text(F_POINTS) = "1";
}

std::string WolfAdventureTeacherView::titleText() const
{
    return mClass.isMap() && !mClass["name"].asString().empty() ? cap(mClass["name"].asString(), 64) : std::string("Classes");
}

size_t WolfAdventureTeacherView::fieldCap(S32 f) const
{
    switch (f)
    {
    case F_NAME: return 64;            // wa_api_class_create: the class name is 1 to 64 characters
    case F_LANGUAGE: return 24;
    case F_SPOT: case F_QUIZ: return 64;
    case F_MESSAGE: return 500;        // the whisper (WA_CHAT_MAX, cut by the brain)
    case F_POINTS: return 3;           // wa_api_quiz_mark: 0 to 100
    case F_LRS_ENDPOINT: return 1024;
    case F_LRS_KEY: case F_LRS_SECRET: return 255;   // wa_api_lrs_add: 1 to 255
    default: return 240;
    }
}

const LLSD& WolfAdventureTeacherView::remembered(const char* list) const
{
    // Read from the per-account settings once, then kept: remember() and forget() are the only writers here and
    // refresh it, and onOpened reads it again. selectedClass() runs several times a frame.
    static const LLSD EMPTY = LLSD::emptyArray();
    if (!mRememberedLoaded)
    {
        mRemembered = gSavedPerAccountSettings.getLLSD(SETTING);
        mRememberedLoaded = true;
    }
    const LLSD& all = mRemembered;   // const: the reads below never copy or insert
    if (!all.isMap() || !all.has(list)) return EMPTY;
    const LLSD& v = all[list];
    return v.isArray() ? v : EMPTY;
}

void WolfAdventureTeacherView::remember(const char* list, const LLSD& entry, const char* key)
{
    LLSD all = gSavedPerAccountSettings.getLLSD(SETTING);
    if (!all.isMap()) all = LLSD::emptyMap();
    LLSD out = LLSD::emptyArray();
    out.append(entry);
    const LLSD old = all.has(list) && all[list].isArray() ? all[list] : LLSD::emptyArray();
    for (size_t i = 0; i < (size_t)old.size() && out.size() < 50; ++i)
    {
        if (old[(LLSD::Integer)i][key].asString() != entry[key].asString()) out.append(old[(LLSD::Integer)i]);
    }
    all[list] = out;
    gSavedPerAccountSettings.setLLSD(SETTING, all);
    mRemembered = all;
    mRememberedLoaded = true;
}

void WolfAdventureTeacherView::forget(const char* list, const std::string& id, const char* key)
{
    LLSD all = gSavedPerAccountSettings.getLLSD(SETTING);
    if (!all.isMap() || !all.has(list)) return;
    LLSD out = LLSD::emptyArray();
    for (size_t i = 0; i < (size_t)all[list].size(); ++i)
    {
        if (all[list][(LLSD::Integer)i][key].asString() != id) out.append(all[list][(LLSD::Integer)i]);
    }
    all[list] = out;
    gSavedPerAccountSettings.setLLSD(SETTING, all);
    mRemembered = all;
    mRememberedLoaded = true;
}

std::string WolfAdventureTeacherView::selectedClass() const
{
    const LLSD cls = remembered("classes");
    if (cls.size() == 0) return std::string();
    return cls[(LLSD::Integer)llclamp(mClassSel, 0, (S32)cls.size() - 1)]["class"].asString();
}

LLSD WolfAdventureTeacherView::quizList() const
{
    // Source: lead (P4) class_get gains quizzes:[{id, title}]; else the pack's quizzes (get_revision, owner or GM).
    const LLSD& c = mClass["quizzes"];
    if (c.isArray()) return c;
    const LLSD& q = mPack["quizzes"];
    return q.isArray() ? q : LLSD::emptyArray();
}

LLSD WolfAdventureTeacherView::spotList() const
{
    // Source: lead (P4) class_get gains spots:[{id, label, region}]; else the pack's places.spots.
    const LLSD& c = mClass["spots"];
    if (c.isArray()) return c;
    const LLSD& s = mPack["places"]["spots"];
    return s.isArray() ? s : LLSD::emptyArray();
}

std::string WolfAdventureTeacherView::studentName(const std::string& agent) const
{
    const LLSD& st = mClass["students"];
    for (size_t i = 0; st.isArray() && i < (size_t)st.size(); ++i)
    {
        if (st[(LLSD::Integer)i]["agent"].asString() == agent) return cap(st[(LLSD::Integer)i]["name"].asString(), 64);
    }
    return WolfAdventure::agentName(LLUUID(agent));
}

void WolfAdventureTeacherView::onOpened(const LLSD& key)
{
    mRememberedLoaded = false;   // read the remembered classes and connections afresh
    loadAdventures();
    if (!selectedClass().empty())
    {
        mPage = P_LIVE;
        loadClass(true);
    }
}

void WolfAdventureTeacherView::loadAdventures()
{
    // BRAIN_API_V1.md §5.1 list_mine (own and granted, with role); class_create needs a published adventure.
    call("list_mine", LLSD::emptyMap(), [](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
        self->mAdvs = LLSD::emptyArray();
        const LLSD& list = reply["adventures"];
        for (size_t i = 0; list.isArray() && i < (size_t)list.size(); ++i)
        {
            const LLSD& a = list[(LLSD::Integer)i];
            if (a["status"].asString() == "published") self->mAdvs.append(a);
        }
        self->mNewAdv = 0;
    });
}

void WolfAdventureTeacherView::loadClass(bool force)
{
    const std::string cls = selectedClass();
    if (cls.empty() || mClassLoading) return;
    if (!force && now() < mClassAt) return;
    mClassLoading = true;
    mClassAt = now() + CLASS_POLL_SEC;
    LLSD body;
    body["class"] = cls;
    LLHandle<LLUICtrl> handle = getHandle();
    // BRAIN_API_V1.md §5.5 class_get {class} -> the class model.
    WolfAdventure::api("class_get", body, [handle, cls](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(handle.get());
        self->mClassLoading = false;
        if (cls != self->selectedClass()) return;
        if (!ok)
        {
            self->setStatus("The class could not be read: " + error, true);
            return;
        }
        if (self->mClassFor != cls) { self->mAnswers = LLSD(); self->mReportRows = LLSD(); self->mPack = LLSD(); self->mPackFor.clear(); }
        self->mClass = reply;
        self->mClassFor = cls;
        self->loadPack();
        const std::string sess = reply["session"].asString();
        if (sess.empty()) { self->mState = LLSD(); return; }
        LLSD b;
        b["session"] = sess;
        // gm_state (BRAIN §8.2): a class's teacher passes wm_need_gm; for the parties (gather) and the pause state.
        WolfAdventure::api("gm_state", b, [handle, sess](bool ok2, const LLSD& state, const std::string&)
        {
            if (handle.isDead() || !ok2) return;
            WolfAdventureTeacherView* me = static_cast<WolfAdventureTeacherView*>(handle.get());
            if (me->mClass["session"].asString() == sess) me->mState = state;
        });
    });
}

void WolfAdventureTeacherView::loadPack()
{
    const std::string adv = mClass["adventure"].asString();
    const S32 rev = mState["session"]["revision"].asInteger();
    if (adv.empty() || rev <= 0) return;
    const std::string key = adv + llformat(":%d", rev);
    if (key == mPackFor) return;
    mPackFor = key;   // asked once: a teacher without the GM role is refused, and then types the ids
    LLSD body;
    body["adventure"] = adv;
    body["revision"] = rev;
    LLHandle<LLUICtrl> handle = getHandle();
    // BRAIN_API_V1.md §5.1 get_revision (owner, GM): the quizzes and spots to pick from.
    WolfAdventure::api("get_revision", body, [handle, key](bool ok, const LLSD& reply, const std::string&)
    {
        if (handle.isDead() || !ok) return;
        WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(handle.get());
        if (self->mPackFor == key) self->mPack = reply["pack"];
    });
}

void WolfAdventureTeacherView::loadReport(const std::string& format, bool save)
{
    LLSD body;
    body["class"] = selectedClass();
    body["format"] = format;
    // BRAIN_API_V1.md §5.5 class_report {class, format} -> {name, mime, content}
    call("class_report", body, [save, format](WolfAdventureDeskView* d, const LLSD& reply)
    {
        WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
        if (format == "json")
        {
            // Source: wolfadventure_gm_lib.php wa_api_class_report: content = {"class": model, "answers": rows}
            const LLSD doc = parseJson(reply["content"].asString());
            self->mAnswers = doc["answers"].isArray() ? doc["answers"] : LLSD::emptyArray();
            self->mReportRows = doc["class"]["students"].isArray() ? doc["class"]["students"] : LLSD::emptyArray();
            if (!save) self->setStatus("Loaded.", false);
        }
        if (save) self->saveFile(reply["name"].asString(), reply["content"].asString());
    }, save ? "Preparing the file..." : "Loading...");
}

void WolfAdventureTeacherView::saveFile(const std::string& name, const std::string& content)
{
    LLHandle<LLUICtrl> handle = getHandle();
    const bool csv = name.size() > 4 && name.compare(name.size() - 4, 4, ".csv") == 0;
    // As wolffloatergraphicshelper.cpp onSave: the platform's save dialog, then the bytes as they came.
    LLFilePickerReplyThread::startPicker(
        [handle, content](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (filenames.empty() || handle.isDead()) return;
            bool ok = false;
            {
                llofstream out(filenames[0].c_str(), std::ios::binary);
                if (out.is_open())
                {
                    out << content;
                    out.close();
                    ok = !out.fail();
                }
            }
            WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(handle.get());
            if (ok) self->setStatus("Saved " + gDirUtilp->getBaseFileName(filenames[0]) + ".", false);
            else self->setStatus("The file could not be written to " + filenames[0] + ". Choose another folder.", true);
        },
        csv ? LLFilePicker::FFSAVE_CSV : LLFilePicker::FFSAVE_ALL, name.empty() ? std::string("class-report.json") : name);
}

void WolfAdventureTeacherView::gmOp(const std::string& op, const LLSD& fields, const std::string& doing)
{
    const std::string sess = mClass["session"].asString();
    if (sess.empty())
    {
        setStatus("This class has no open session (it has ended).", true);
        return;
    }
    LLSD body = fields.isMap() ? fields : LLSD::emptyMap();
    body["session"] = sess;
    body["op"] = op;
    // BRAIN_API_V1.md §8.1 gm, the teacher column (wolfadventure_gm_lib.php WAG_OPS).
    call("gm", body, [doing](WolfAdventureDeskView* d, const LLSD&)
    {
        WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
        self->setStatus("Done: " + doing, false);
        self->loadClass(true);
    }, doing + "...");
}

void WolfAdventureTeacherView::tick()
{
    if (mPage == P_LIVE || mPage == P_CLASSES) loadClass(false);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// drawing
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureTeacherView::face(F32 W, F32 H, F32 top, F32 bottom, const Palette& p)
{
    const F32 M = 14.f;
    F32 y = top;
    // the class picker (the classes this viewer made or opened)
    {
        const LLSD cls = remembered("classes");
        std::string cw = "Class: none yet";
        if (cls.size() > 0)
        {
            const S32 i = llclamp(mClassSel, 0, (S32)cls.size() - 1);
            cw = "Class: " + cap(cls[i]["name"].asString(), 48) + " \xC2\xB7 code " + cap(cls[i]["code"].asString(), 12);
            if (cls.size() > 1) cw += llformat("  (%d of %d)", i + 1, (S32)cls.size());
        }
        btn(M, y - 28.f, M + 420.f, y, cw, H_CLASS_NEXT, 0, "The next of your classes", p, cls.size() > 1);
        btn(M + 426.f, y - 28.f, M + 516.f, y, "Forget", H_CLASS_FORGET, 0, "Take this class off this list (the class itself is not changed)", p, cls.size() > 0);
    }
    y -= 36.f;
    static const char* NAMES[P_COUNT] = { "Classes", "Live", "Answers", "Report", "Connections" };
    static const char* TIPS[P_COUNT] = { "Make a class and give out its join code", "Every student, live; pause, gather, ask, message",
                                         "Quiz answers by student; mark essays", "Download the class report (CSV or JSON)", "A learning record store for xAPI" };
    const F32 tw = (W - 2.f * M) / (F32)P_COUNT;
    for (S32 i = 0; i < P_COUNT; ++i)
    {
        const F32 tl = M + tw * (F32)i;
        btn(tl, y - 28.f, tl + tw - 4.f, y, NAMES[i], H_PAGE, i, TIPS[i], p, true, mPage == i ? BTN_ACTIVE : 0);
    }
    y -= 36.f;
    const F32 l = M, r = W - M;
    F32 t = beginScroll(1 + (S32)mPage, l, bottom, r, y);
    {
        LLLocalClipRect clip(LLRect((S32)l, (S32)y, (S32)r, (S32)bottom));
        switch (mPage)
        {
        case P_CLASSES: t = pageClasses(l, r - 10.f, t, p); break;
        case P_LIVE: t = pageLive(l, r - 10.f, t, p); break;
        case P_ANSWERS: t = pageAnswers(l, r - 10.f, t, p); break;
        case P_REPORT: t = pageReport(l, r - 10.f, t, p); break;
        default: t = pageConnections(l, r - 10.f, t, p); break;
        }
    }
    endScroll(1 + (S32)mPage, t, p);
}

F32 WolfAdventureTeacherView::pageClasses(F32 l, F32 r, F32 top, const Palette& p)
{
    // §4 Classes: New class: the adventure, the name, teams, free typing, the language; the join code large + Copy.
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "New class", p);
    std::string aw = "Adventure: none published that you own or teach";
    if (mAdvs.isArray() && mAdvs.size() > 0)
    {
        const LLSD& a = mAdvs[(LLSD::Integer)llclamp(mNewAdv, 0, (S32)mAdvs.size() - 1)];
        aw = "Adventure: " + cap(a["title"].asString(), 64) + (a["purpose"].asString() == "education" ? " \xC2\xB7 education" : "");
        if (mAdvs.size() > 1) aw += llformat("  (%d of %d)", llclamp(mNewAdv, 0, (S32)mAdvs.size() - 1) + 1, (S32)mAdvs.size());
    }
    btn(l, y - 28.f, l + 460.f, y, aw, H_NEW_ADV, 0, "Education adventures, or any you have the teacher role for (click for the next)", p, mAdvs.isArray() && mAdvs.size() > 1);
    y -= 36.f;
    field(l, y - 30.f, l + 360.f, y, F_NAME, "Class name (e.g. Year 9 History)", "1 to 64 characters", p);
    y -= 36.f;
    btn(l, y - 28.f, l + 220.f, y, mNewTeams == 0 ? std::string("Teams: none (one party)") : llformat("Teams: %d", mNewTeams), H_NEW_TEAMS, 0,
        "No teams (the class plays as one party) or 2 to 12 teams", p);
    btn(l + 226.f, y - 28.f, l + 446.f, y, mNewFree ? "Free typing: On" : "Free typing: Off", H_NEW_FREE, 0,
        mNewFree ? "Students may type freely to the characters (click to turn off)" : "Students pick from suggested lines only (click to allow free typing)", p, true, mNewFree ? BTN_ACTIVE : 0);
    y -= 36.f;
    const bool lang_bad = !languageOk(text(F_LANGUAGE));
    field(l, y - 30.f, l + 220.f, y, F_LANGUAGE, "Language, e.g. en-GB (optional)", "The session language, as a code such as en-GB or fr", p, lang_bad);
    y -= 34.f;
    if (lang_bad) y = para(l, r, y, "The language is a code such as en-GB or fr (or leave it empty).", DANGER_INK, true);
    const bool can = idle && mAdvs.isArray() && mAdvs.size() > 0 && !text(F_NAME).empty() && !lang_bad;
    btn(l, y - 32.f, l + 200.f, y, "Create the class", H_CREATE, 0, "Make the class and open its session; you get a join code", p, can, BTN_PRIMARY);
    y -= 44.f;
    if (!mNewCode.empty())
    {
        y = head(l, r, y, "Join code", p);
        textL(display(40.f), mNewCode, l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP);
        const F32 ch = (F32)display(40.f)->getLineHeight();
        btn(l + 300.f, y - 32.f, l + 400.f, y, "Copy", H_COPY_CODE, 0, "Copy the code to give to your students", p);
        y -= ch + 6.f;
        y = para(l, r, y, "Students open Wolf World \xE2\x86\x92 Games & Stories \xE2\x86\x92 Adventures..., type this code in \"Class code\" and press \"Join class\".", p.mDim, true) - 8.f;
    }
    y = head(l, r, y, "Your classes", p);
    const LLSD cls = remembered("classes");
    if (cls.size() == 0) y = para(l, r, y, "No classes yet. Make one above.", p.mDim, true);
    for (size_t i = 0; i < (size_t)cls.size(); ++i)
    {
        const LLSD& c = cls[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), cap(c["name"].asString(), 64) + " \xC2\xB7 code " + cap(c["code"].asString(), 12), l, y - 14.f, p.mText,
              LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 110.f);
        btn(r - 100.f, y - 26.f, r, y, "Open", H_OPEN_CLASS, (S32)i, "Watch this class live", p);
        y -= 30.f;
    }
    return y;
}

F32 WolfAdventureTeacherView::pageLive(F32 l, F32 r, F32 top, const Palette& p)
{
    const bool idle = mBusy == 0;
    if (selectedClass().empty()) return para(l, r, top, "No class yet. Make one on the Classes page.", p.mDim);
    if (!mClass.isMap()) return para(l, r, top, "Reading the class...", p.mDim);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const bool open = !mClass["session"].asString().empty();
    F32 y = top;
    // the controls (§4 Live)
    const bool paused = mState["session"]["control"].asString() == "paused";
    if (paused) btn(l, y - 28.f, l + 150.f, y, "Resume", H_RESUME, 0, "Resume the class", p, idle && open, BTN_PRIMARY);
    else btn(l, y - 28.f, l + 150.f, y, "Pause everyone", H_PAUSE, 0, "Pause timers, hints and the story for the whole class", p, idle && open);
    const bool free = mClass["settings"]["freeChat"].asBoolean();
    btn(l + 156.f, y - 28.f, l + 336.f, y, free ? "Free typing: On" : "Free typing: Off", H_FREE, 0,
        free ? "Students may type freely to characters (click to turn off)" : "Suggested lines only (click to allow free typing)", p, idle, free ? BTN_ACTIVE : 0);
    btn(r - 190.f, y - 28.f, r, y, "End the class session", H_END, 0, "End the session for every student (asks first)", p, idle && open, BTN_DANGER);
    y -= 36.f;
    if (!open) y = para(l, r, y, "This class's session has ended.", WARN_INK, true);
    // Gather everyone at... (a spot of the story)
    const LLSD spots = spotList();
    if (spots.size() > 0)
    {
        const LLSD& s = spots[(LLSD::Integer)llclamp(mSpot, 0, (S32)spots.size() - 1)];
        btn(l, y - 28.f, l + 300.f, y, "At: " + cap(s["label"].asString().empty() ? s["id"].asString() : s["label"].asString(), 48), H_GATHER_SPOT, 0, "Which spot (click for the next)", p);
    }
    else field(l, y - 28.f, l + 300.f, y, F_SPOT, "Spot id (from the adventure)", "A spot of the story to call everyone to", p);
    btn(l + 306.f, y - 28.f, l + 506.f, y, "Gather everyone", H_GATHER, 0, "Each student gets a teleport offer to the spot; nobody is moved without their own click", p, idle && open);
    y -= 34.f;
    // Ask a question... (a quiz of the story)
    const LLSD quizzes = quizList();
    if (quizzes.size() > 0)
    {
        const LLSD& q = quizzes[(LLSD::Integer)llclamp(mQuiz, 0, (S32)quizzes.size() - 1)];
        btn(l, y - 28.f, l + 300.f, y, "Quiz: " + cap(q["title"].asString().empty() ? q["id"].asString() : q["title"].asString(), 48), H_QUIZ_PICK, 0, "Which quiz (click for the next)", p);
    }
    else field(l, y - 28.f, l + 300.f, y, F_QUIZ, "Quiz id (from the adventure)", "A quiz of the story to send to everyone", p);
    btn(l + 306.f, y - 28.f, l + 506.f, y, "Ask the class", H_ASK, 0, "Every student gets the quiz now", p, idle && open);
    y -= 34.f;
    field(l, y - 30.f, r - 160.f, y, F_MESSAGE, "A message to every student...", "Each student sees it privately (Enter sends)", p);
    btn(r - 150.f, y - 30.f, r, y, "Message the class", H_MESSAGE, 0, "Send it to every student", p, idle && open && !text(F_MESSAGE).empty());
    y -= 42.f;
    // the students (§4 Live: name, team, region, scene and beat, scenes done as a number AND a bar, quiz, stuck)
    y = head(l, r, y, "Students", p);
    const LLSD& st = mClass["students"];
    if (!st.isArray() || st.size() == 0) return para(l, r, y, "No students have joined yet. Give them the join code " + cap(mClass["code"].asString(), 12) + ".", p.mDim);
    const F32 cName = l, cTeam = l + 190.f, cWhere = l + 240.f, cScene = l + 400.f, cDone = r - 300.f, cQuiz = r - 170.f, cStuck = r - 70.f;
    textL(bold, "Name", cName, y, p.mDim); textL(bold, "Team", cTeam, y, p.mDim); textL(bold, "Where", cWhere, y, p.mDim);
    textL(bold, "Scene \xC2\xB7 beat", cScene, y, p.mDim); textL(bold, "Scenes done", cDone, y, p.mDim); textL(bold, "Quizzes", cQuiz, y, p.mDim);
    y -= (F32)bold->getLineHeight() + 4.f;
    for (size_t i = 0; i < (size_t)st.size(); ++i)
    {
        const LLSD& s = st[(LLSD::Integer)i];
        const F32 cy = y - 12.f;
        if (i % 2 == 0) rect(l, y - 24.f, r, y, fade(p.mTrack, 0.35f));
        textL(small, cap(s["name"].asString(), 48), cName + 4.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, cTeam - cName - 8.f);
        textL(small, s["team"].asInteger() > 0 ? llformat("%d", s["team"].asInteger()) : std::string("-"), cTeam, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        textL(small, s["region"].asString().empty() ? std::string("away") : cap(s["region"].asString(), 40), cWhere, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, cScene - cWhere - 8.f);
        std::string sc = s["ending"].asString().empty() ? cap(s["scene"].asString(), 32) + " \xC2\xB7 " + cap(s["beat"].asString(), 32) : "Finished: " + cap(s["ending"].asString(), 32);
        textL(small, sc, cScene, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, cDone - cScene - 8.f);
        const S32 done = s["scenesDone"].asInteger(), total = llmax(1, s["scenesTotal"].asInteger());
        textL(small, llformat("%d/%d", done, s["scenesTotal"].asInteger()), cDone, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        rect(cDone + 44.f, cy - 3.f, cQuiz - 12.f, cy + 3.f, p.mTrack);
        rect(cDone + 44.f, cy - 3.f, cDone + 44.f + (cQuiz - 12.f - cDone - 44.f) * llclamp((F32)done / (F32)total, 0.f, 1.f), cy + 3.f, p.mAccent);
        std::string qz;
        const LLSD& q = s["quiz"];
        for (LLSD::map_const_iterator it = q.beginMap(); q.isMap() && it != q.endMap(); ++it) qz += (qz.empty() ? "" : ", ") + cap(it->first, 16) + " " + cap(it->second.asString(), 8);
        textL(small, qz.empty() ? std::string("-") : qz, cQuiz, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, cStuck - cQuiz - 6.f);
        if (s["stuck"].asInteger() != 0)
        {
            // the badge: an icon AND the word (never colour alone)
            disc(cStuck + 6.f, cy, 5.f, WARN_INK);
            textL(bold, "Stuck", cStuck + 14.f, cy, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
        }
        y -= 24.f;
    }
    return para(l, r, y - 6.f, "\"Stuck\" means no progress for 5 minutes while they were here.", p.mDim, true);
}

F32 WolfAdventureTeacherView::pageAnswers(F32 l, F32 r, F32 top, const Palette& p)
{
    // §4 Answers: quiz answers by student; essays wait for Mark correct / Mark incorrect with points.
    const bool idle = mBusy == 0;
    if (selectedClass().empty()) return para(l, r, top, "No class yet. Make one on the Classes page.", p.mDim);
    F32 y = top;
    btn(l, y - 28.f, l + 160.f, y, "Load answers", H_LOAD_ANSWERS, 0, "Read every answer of this class's session", p, idle, BTN_PRIMARY);
    field(l + 170.f, y - 28.f, l + 250.f, y, F_POINTS, "Points", "Points for a correct essay (0 to 100)", p,
          text(F_POINTS).find_first_not_of("0123456789") != std::string::npos || atoi(text(F_POINTS).c_str()) > 100);
    y -= 36.f;
    if (!mAnswers.isArray()) return para(l, r, y, "Press Load answers.", p.mDim);
    if (mAnswers.size() == 0) return para(l, r, y, "No answers yet.", p.mDim);
    std::string last;
    for (size_t i = 0; i < (size_t)mAnswers.size(); ++i)
    {
        const LLSD& a = mAnswers[(LLSD::Integer)i];
        const std::string who = a["agent_uuid"].asString();
        if (who != last)
        {
            y = head(l, r, y - 4.f, studentName(who), p);
            last = who;
        }
        const LLSD& c = a["correct"];
        // Source: wolfadventure_gm_lib.php wa_api_quiz_mark: correct 0|1, null until marked (an essay).
        const bool waiting = c.isUndefined();
        std::string mark = waiting ? "Waiting for you" : (c.asInteger() != 0 ? llformat("Correct (%d)", a["points"].asInteger()) : std::string("Incorrect"));
        const F32 t0 = y;
        y = para(l + 8.f, r - 260.f, y, cap(a["quiz_id"].asString(), 32) + " \xC2\xB7 " + cap(a["item_id"].asString(), 32) + ": " + cap(a["answer"].asString(), 2000), p.mText, true, 10);
        textL(LLFontGL::getFontSansSerifSmallBold(), mark, r - 250.f, t0, waiting ? WARN_INK : (c.asInteger() != 0 ? OK_INK : DANGER_INK), LLFontGL::LEFT, LLFontGL::TOP);
        // quiz_mark needs the answer's id (wolfadv_answers.id); the report gives it as "id" when it has it.
        const bool has_id = a.has("id");
        btn(r - 160.f, t0 - 22.f, r - 82.f, t0, "Correct", H_MARK, (S32)i * 2 + 1, has_id ? "Mark it correct with the points above" : "The service does not give this answer's id yet", p, idle && has_id);
        btn(r - 78.f, t0 - 22.f, r, t0, "Incorrect", H_MARK, (S32)i * 2, has_id ? "Mark it incorrect" : "The service does not give this answer's id yet", p, idle && has_id);
        y = llmin(y, t0 - 26.f) - 4.f;
    }
    return y;
}

F32 WolfAdventureTeacherView::pageReport(F32 l, F32 r, F32 top, const Palette& p)
{
    // §4 Report: CSV (one row per student) and JSON (everything), shown as a table first; Conversations.
    const bool idle = mBusy == 0;
    if (selectedClass().empty()) return para(l, r, top, "No class yet. Make one on the Classes page.", p.mDim);
    F32 y = top;
    btn(l, y - 28.f, l + 150.f, y, "Show the table", H_REPORT_LOAD, 0, "Read the report", p, idle, BTN_PRIMARY);
    btn(l + 156.f, y - 28.f, l + 306.f, y, "Download CSV", H_SAVE_CSV, 0, "One row per student: scenes done, quiz scores, ending, time spent", p, idle);
    btn(l + 312.f, y - 28.f, l + 462.f, y, "Download JSON", H_SAVE_JSON, 0, "Everything, including every answer", p, idle);
    btn(l + 468.f, y - 28.f, l + 618.f, y, "Conversations", H_CONVO, 0, "Open the GM Console's conversations for this class's session", p, !mClass["session"].asString().empty());
    y -= 40.f;
    if (!mReportRows.isArray()) return para(l, r, y, "Press Show the table.", p.mDim);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 c1 = l, c2 = l + 220.f, c3 = l + 290.f, c4 = l + 400.f, c5 = r - 160.f;
    textL(bold, "Name", c1, y, p.mDim); textL(bold, "Team", c2, y, p.mDim); textL(bold, "Scenes done", c3, y, p.mDim);
    textL(bold, "Quizzes", c4, y, p.mDim); textL(bold, "Ending", c5, y, p.mDim);
    y -= (F32)bold->getLineHeight() + 4.f;
    for (size_t i = 0; i < (size_t)mReportRows.size(); ++i)
    {
        const LLSD& s = mReportRows[(LLSD::Integer)i];
        const F32 cy = y - 11.f;
        if (i % 2 == 0) rect(l, y - 22.f, r, y, fade(p.mTrack, 0.35f));
        textL(small, cap(s["name"].asString(), 48), c1 + 4.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, c2 - c1 - 8.f);
        textL(small, s["team"].asInteger() > 0 ? llformat("%d", s["team"].asInteger()) : std::string("-"), c2, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
        tabularRight(small, llformat("%d/%d", s["scenesDone"].asInteger(), s["scenesTotal"].asInteger()), c3 + 70.f, cy, p.mText);
        std::string qz;
        const LLSD& q = s["quiz"];
        for (LLSD::map_const_iterator it = q.beginMap(); q.isMap() && it != q.endMap(); ++it) qz += (qz.empty() ? "" : ", ") + cap(it->first, 16) + " " + cap(it->second.asString(), 8);
        textL(small, qz.empty() ? std::string("-") : qz, c4, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, c5 - c4 - 8.f);
        textL(small, s["ending"].asString().empty() ? std::string("-") : cap(s["ending"].asString(), 32), c5, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - c5);
        y -= 22.f;
    }
    return y;
}

F32 WolfAdventureTeacherView::pageConnections(F32 l, F32 r, F32 top, const Palette& p)
{
    // §4 Connections: the LRS for xAPI statements (endpoint, key, secret, Send a test statement); LTI (P7).
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "Learning record store (xAPI)", p);
    y = para(l, r, y, "Statements about your classes (started, answered, passed, completed) go to the store you connect. The key and secret are kept encrypted by the service.", p.mDim, true) - 6.f;
    const bool ep_bad = !text(F_LRS_ENDPOINT).empty() && text(F_LRS_ENDPOINT).compare(0, 8, "https://") != 0;
    field(l, y - 30.f, r, y, F_LRS_ENDPOINT, "https://lrs.example.org/xapi", "The LRS endpoint (https only)", p, ep_bad);
    y -= 34.f;
    if (ep_bad) y = para(l, r, y, "The endpoint starts with https://.", DANGER_INK, true) - 2.f;
    const F32 half = (r - l) * 0.5f;
    field(l, y - 30.f, l + half - 6.f, y, F_LRS_KEY, "Key", "The LRS key (no \":\")", p, text(F_LRS_KEY).find(':') != std::string::npos);
    field(l + half, y - 30.f, r, y, F_LRS_SECRET, "Secret", "The LRS secret (shown as stars)", p);
    y -= 36.f;
    btn(l, y - 28.f, l + 150.f, y, std::string("xAPI ") + LRS_VERSIONS[mLrsVersion], H_LRS_VERSION, 0, "The xAPI version the store speaks (click to change)", p);
    const bool can = idle && !ep_bad && !text(F_LRS_ENDPOINT).empty() && !text(F_LRS_KEY).empty() && !text(F_LRS_SECRET).empty() && text(F_LRS_KEY).find(':') == std::string::npos;
    btn(l + 156.f, y - 28.f, l + 306.f, y, "Connect", H_LRS_ADD, 0, "Save this connection", p, can, BTN_PRIMARY);
    y -= 40.f;
    y = head(l, r, y, "Your connections", p);
    const LLSD lrs = remembered("lrs");
    if (lrs.size() == 0) y = para(l, r, y, "None yet.", p.mDim, true);
    for (size_t i = 0; i < (size_t)lrs.size(); ++i)
    {
        const LLSD& c = lrs[(LLSD::Integer)i];
        textL(LLFontGL::getFontSansSerif(), cap(c["endpoint"].asString(), 120), l, y - 14.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 300.f);
        btn(r - 290.f, y - 26.f, r - 100.f, y, "Send a test statement", H_LRS_TEST, (S32)i, "Send one statement now and show the store's answer", p, idle);
        btn(r - 94.f, y - 26.f, r, y, "Remove", H_LRS_REMOVE, (S32)i, "Remove this connection (asks first)", p, idle, BTN_DANGER);
        y -= 30.f;
    }
    if (!mLrsResult.empty()) y = para(l, r, y - 4.f, mLrsResult, p.mText, true);
    y = head(l, r, y - 8.f, "LTI (your school's LMS)", p);
    return para(l, r, y, "The LTI tool links for your LMS administrator arrive with the WolfStorm consoles (phase 7). Until then, students join with the class code.", p.mDim, true);
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// actions
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureTeacherView::fieldEnter(S32 f)
{
    if (f == F_MESSAGE) press(H_MESSAGE, 0);
}

void WolfAdventureTeacherView::press(S32 id, S32 arg)
{
    switch (id)
    {
    case H_PAGE:
        mPage = (EPage)llclamp(arg, 0, (S32)P_COUNT - 1);
        if (mPage == P_LIVE) loadClass(true);
        return;
    case H_CLASS_NEXT:
    {
        const S32 n = (S32)remembered("classes").size();
        if (n > 0) { mClassSel = (mClassSel + 1) % n; mClass = LLSD(); mState = LLSD(); loadClass(true); }
        return;
    }
    case H_CLASS_FORGET:
    {
        const std::string cls = selectedClass();
        if (cls.empty()) return;
        ask("Forget this class?", "It leaves this list in this viewer only. The class, its session and its students are not changed.", "Forget", false, [this, cls]()
        {
            forget("classes", cls, "class");
            mClassSel = 0;
            mClass = LLSD();
            mState = LLSD();
            loadClass(true);
        });
        return;
    }
    case H_NEW_ADV: if (mAdvs.isArray() && mAdvs.size() > 0) mNewAdv = (mNewAdv + 1) % (S32)mAdvs.size(); return;
    case H_NEW_TEAMS: mNewTeams = mNewTeams == 0 ? 2 : (mNewTeams >= 12 ? 0 : mNewTeams + 1); return;
    case H_NEW_FREE: mNewFree = !mNewFree; return;
    case H_CREATE:
    {
        if (!mAdvs.isArray() || mAdvs.size() == 0) return;
        const LLSD& a = mAdvs[(LLSD::Integer)llclamp(mNewAdv, 0, (S32)mAdvs.size() - 1)];
        LLSD body;
        body["adventure"] = a["id"].asString();
        body["name"] = text(F_NAME);
        body["teams"] = mNewTeams;
        body["freeChat"] = mNewFree;
        body["language"] = text(F_LANGUAGE);
        const std::string name = text(F_NAME), adv = a["id"].asString();
        // BRAIN_API_V1.md §5.5 class_create {adventure, name, teams, freeChat, language} -> {class, code} (+ session)
        call("class_create", body, [name, adv](WolfAdventureDeskView* d, const LLSD& reply)
        {
            WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
            LLSD e;
            e["class"] = reply["class"].asString();
            e["name"] = name;
            e["code"] = reply["code"].asString();
            e["adventure"] = adv;
            self->remember("classes", e, "class");
            self->mClassSel = 0;
            self->mNewCode = reply["code"].asString();
            self->text(F_NAME).clear();
            self->mClass = LLSD();
            self->loadClass(true);
            self->setStatus("The class is ready. Give your students the join code " + self->mNewCode + ".", false);
        }, "Making the class...");
        return;
    }
    case H_COPY_CODE:
    {
        const LLWString w = utf8str_to_wstring(mNewCode);
        LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
        setStatus("The join code is copied.", false);
        return;
    }
    case H_OPEN_CLASS:
        mClassSel = arg;
        mClass = LLSD();
        mState = LLSD();
        mPage = P_LIVE;
        loadClass(true);
        return;
    case H_PAUSE: gmOp("pause", LLSD::emptyMap(), "the class is paused"); return;
    case H_RESUME: gmOp("resume", LLSD::emptyMap(), "the class goes on"); return;
    case H_FREE:
    {
        LLSD body;
        body["class"] = selectedClass();
        body["freeChat"] = !mClass["settings"]["freeChat"].asBoolean();
        // BRAIN_API_V1.md §5.5 class_update {class, freeChat?} -> the class model
        call("class_update", body, [](WolfAdventureDeskView* d, const LLSD& reply)
        {
            WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
            self->mClass = reply;
            self->setStatus(reply["settings"]["freeChat"].asBoolean() ? "Free typing is on." : "Free typing is off.", false);
        }, "Changing free typing...");
        return;
    }
    case H_GATHER_SPOT: { const S32 n = (S32)spotList().size(); if (n > 0) mSpot = (mSpot + 1) % n; return; }
    case H_QUIZ_PICK: { const S32 n = (S32)quizList().size(); if (n > 0) mQuiz = (mQuiz + 1) % n; return; }
    case H_GATHER:
    {
        const LLSD spots = spotList();
        const std::string spot = spots.size() > 0 ? spots[llclamp(mSpot, 0, (S32)spots.size() - 1)]["id"].asString() : text(F_SPOT);
        if (spot.empty()) { setStatus("Name the spot to gather at.", true); return; }
        // gather is per party (BRAIN §8.1 gather {party, spot}): one per team of the class.
        const LLSD& parties = mState["parties"];
        S32 n = 0;
        for (size_t i = 0; parties.isArray() && i < (size_t)parties.size(); ++i)
        {
            if (!parties[(LLSD::Integer)i]["ending"].asString().empty()) continue;
            LLSD f;
            f["party"] = parties[(LLSD::Integer)i]["party"].asString();
            f["spot"] = spot;
            gmOp("gather", f, "teleport offers sent");
            ++n;
        }
        if (n == 0) setStatus("Nobody is playing yet, so there is no one to gather.", true);
        return;
    }
    case H_ASK:
    {
        const LLSD quizzes = quizList();
        const std::string quiz = quizzes.size() > 0 ? quizzes[llclamp(mQuiz, 0, (S32)quizzes.size() - 1)]["id"].asString() : text(F_QUIZ);
        if (quiz.empty()) { setStatus("Name the quiz to ask.", true); return; }
        LLSD f;
        f["class"] = 1;
        f["quiz"] = quiz;
        gmOp("question", f, "the question is out");
        return;
    }
    case H_MESSAGE:
    {
        if (text(F_MESSAGE).empty()) return;
        LLSD f;
        f["class"] = 1;
        f["text"] = text(F_MESSAGE);
        gmOp("whisper", f, "message sent");
        text(F_MESSAGE).clear();
        return;
    }
    case H_END:
        ask("End the class session?", "Every student's adventure stops with \"Your class session is over.\" The report stays available.", "End the session", true, [this]()
        {
            LLSD body;
            body["class"] = selectedClass();
            // BRAIN_API_V1.md §5.5 class_end {class}
            call("class_end", body, [](WolfAdventureDeskView* d, const LLSD&)
            {
                WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
                self->setStatus("The class session has ended.", false);
                self->loadClass(true);
            }, "Ending the class session...");
        });
        return;
    case H_LOAD_ANSWERS: loadReport("json", false); return;
    case H_MARK:
    {
        const S32 i = arg / 2;
        if (!mAnswers.isArray() || i < 0 || i >= (S32)mAnswers.size() || !mAnswers[i].has("id")) return;
        const S32 pts = atoi(text(F_POINTS).c_str());
        if (text(F_POINTS).find_first_not_of("0123456789") != std::string::npos || pts > 100) { setStatus("Points are a whole number from 0 to 100.", true); return; }
        LLSD body;
        body["answer"] = mAnswers[i]["id"].asInteger();
        body["correct"] = arg % 2;
        body["points"] = pts;
        // BRAIN_API_V1.md §5.5 quiz_mark {answer, correct, points}
        call("quiz_mark", body, [](WolfAdventureDeskView* d, const LLSD&)
        {
            WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
            self->setStatus("Marked.", false);
            self->loadReport("json", false);
        }, "Marking...");
        return;
    }
    case H_REPORT_LOAD: loadReport("json", false); return;
    case H_SAVE_CSV: loadReport("csv", true); return;
    case H_SAVE_JSON: loadReport("json", true); return;
    case H_CONVO:
        // the conversations live in the GM Console's log drawer (a class's teacher may read them, BRAIN §10.3)
        LLFloaterReg::showInstance("wolf_adventure_gm");
        return;
    case H_LRS_VERSION: mLrsVersion = (mLrsVersion + 1) % 2; return;
    case H_LRS_ADD:
    {
        LLSD body;
        body["endpoint"] = text(F_LRS_ENDPOINT);
        body["key"] = text(F_LRS_KEY);
        body["secret"] = text(F_LRS_SECRET);
        body["version"] = LRS_VERSIONS[mLrsVersion];
        const std::string ep = text(F_LRS_ENDPOINT);
        // BRAIN_API_V1.md §5.5 lrs_add {endpoint, key, secret, version} -> {lrs}
        call("lrs_add", body, [ep](WolfAdventureDeskView* d, const LLSD& reply)
        {
            WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
            LLSD e;
            e["lrs"] = reply["lrs"].asString();
            e["endpoint"] = ep;
            self->remember("lrs", e, "lrs");
            self->text(F_LRS_SECRET).clear();
            self->text(F_LRS_KEY).clear();
            self->setStatus("Connected. Send a test statement to check it.", false);
        }, "Connecting...");
        text(F_LRS_SECRET).clear();   // never kept in the window once sent
        return;
    }
    case H_LRS_TEST:
    case H_LRS_REMOVE:
    {
        const LLSD lrs = remembered("lrs");
        if (arg < 0 || arg >= (S32)lrs.size()) return;
        const std::string lid = lrs[arg]["lrs"].asString();
        LLSD body;
        body["lrs"] = lid;
        if (id == H_LRS_TEST)
        {
            // lrs_test {lrs} -> {ok, status, error}
            call("lrs_test", body, [](WolfAdventureDeskView* d, const LLSD& reply)
            {
                WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
                if (reply["ok"].asInteger() != 0)
                {
                    self->mLrsResult = llformat("The store accepted the test statement (HTTP %d).", reply["status"].asInteger());
                    self->setStatus("Test sent.", false);
                }
                else
                {
                    self->mLrsResult.clear();
                    self->setStatus(llformat("The store refused the test statement (HTTP %d) ", reply["status"].asInteger()) + cap(reply["error"].asString(), 200), true);
                }
            }, "Sending a test statement...");
        }
        else
        {
            ask("Remove this connection?", "Statements waiting to be sent to it are dropped.", "Remove", true, [this, body, lid]()
            {
                call("lrs_remove", body, [lid](WolfAdventureDeskView* d, const LLSD&)
                {
                    WolfAdventureTeacherView* self = static_cast<WolfAdventureTeacherView*>(d);
                    self->forget("lrs", lid, "lrs");
                    self->setStatus("Removed.", false);
                }, "Removing...");
            });
        }
        return;
    }
    default: return;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventureTeacher::WolfFloaterAdventureTeacher(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureTeacher::postBuild()
{
    mView = getChild<WolfAdventureTeacherView>("teacher");
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterAdventureTeacher::onOpen(const LLSD& key)
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    if (mView)
    {
        mView->onOpened(key);
        mView->setFocus(true);
    }
}

void WolfFloaterAdventureTeacher::draw()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}
