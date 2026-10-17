/**
 * @file wolffloateradventurestudio.cpp
 * @brief WolfViewer: Adventure Studio Lite. See wolffloateradventurestudio.h.
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

#include "wolffloateradventurestudio.h"

#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

#include <boost/json.hpp>

#include "llagent.h"
#include "llassetstorage.h"
#include "llclipboard.h"
#include "llfilepicker.h"
#include "llfilesystem.h"
#include "llfloateravatarpicker.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llinventory.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "lllocalcliprect.h"
#include "llnotecard.h"
#include "llrender.h"
#include "llsdjson.h"
#include "lltooldraganddrop.h"
#include "lltooltip.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "wolfadventure.h"
#include "wolfadventurestudioutil.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfAdventureStudioView> r_wolf_adventure_studio_view("wolf_adventure_studio_view");

using namespace WolfGameDraw;

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as the Roleplay window
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24
    const F64 STATUS_LIFE = 6.0;    // a confirmation fades; an error stays until the next action (§1 Feedback)

    // Source: ADVENTURE_PACK_V1.md §2 purpose, and what each switches on; the preset each one
    // defaults to is STUDIO_AND_CONSOLE_V1.md §2.1 (Adventure: Wild West; Education: Education;
    // Experience: Experience; Event: Wild West).
    struct Purpose { const char* mId; const char* mLabel; const char* mLine; S32 mPreset; };
    const Purpose PURPOSES[] = {
        { "adventure",  "Adventure",  "A story to play: parties, honour and rewards.", 0 },
        { "education",  "Education",  "For classes: joined with a class code, no free chat, answers kept accurate, rated General.", 2 },
        { "experience", "Experience", "A place to visit and explore, with no stats.", 3 },
        { "event",      "Event",      "Runs like an adventure, for one-off happenings.", 0 },
    };
    const S32 PURPOSE_COUNT = (S32)(sizeof(PURPOSES) / sizeof(PURPOSES[0]));
    // Source: ADVENTURE_PACK_V1.md §3 rules.preset; the 5E name as STUDIO_AND_CONSOLE_V1.md §2.6.
    struct Preset { const char* mId; const char* mLabel; };
    const Preset PRESETS[] = {
        { "wild-west", "Wild West" }, { "mystery", "Mystery" }, { "education", "Education" }, { "experience", "Experience" },
        { "5e-srd-5.2", "5E compatible (SRD 5.2.1)" },
    };
    const S32 PRESET_COUNT = (S32)(sizeof(PRESETS) / sizeof(PRESETS[0]));
    // Settled in P1 (BRAIN_API_V1.md §5.3 note): `expires` is unix seconds; none = never.
    struct Expiry { const char* mLabel; S64 mSeconds; };
    const Expiry EXPIRIES[] = { { "Never", 0 }, { "1 day", 86400 }, { "7 days", 604800 }, { "30 days", 2592000 } };
    const S32 EXPIRY_COUNT = (S32)(sizeof(EXPIRIES) / sizeof(EXPIRIES[0]));
    // Source: adventure_pack_v1.schema.json adventure.regions[].npcBudget 0..40.
    const S32 NPC_BUDGET_MAX = 40;
    // Source: BRAIN_API_V1.md §3 "a pack is 2 MiB".
    const size_t PACK_MAX_BYTES = 2 * 1024 * 1024;
    // Source: ADVENTURE_PACK_V1.md §2 title "1 to 64 characters".
    const size_t TITLE_MAX = 64;
    const char* const LOOK_PREFIX = "Look - ";   // ADVENTURE_PACK_V1.md §13.1 `Look - <name>`

    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }

    /** A pack's JSON text with one edit made (boost::json keeps every other value exactly). */
    std::string packWith(const std::string& json, const std::function<void(boost::json::object&)>& edit)
    {
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(json, ec);
        if (ec || !v.is_object()) return json;
        edit(v.as_object());
        return boost::json::serialize(v);
    }

    void setIn(boost::json::object& root, const char* section, const char* key, const std::string& value)
    {
        boost::json::value& sec = root[section];
        if (!sec.is_object()) sec = boost::json::object();
        sec.as_object()[key] = value;
    }

    std::string statusWord(const std::string& status, S32 revision)
    {
        // BRAIN_API_V1.md §5.1: draft | published | disabled
        if (status == "published") return llformat("Published rev %d", revision);
        if (status == "disabled") return "Disabled";
        return "Draft";
    }

    std::string stateWord(const std::string& state)
    {
        // BRAIN_API_V1.md §5.2 states (§2.1 "Requested / Enabled / Refused")
        if (state == "enabled") return "Enabled";
        if (state == "refused") return "Refused";
        if (state == "disabled") return "Disabled";
        return "Requested";
    }

    std::string aiWord(const std::string& mode)
    {
        // ADVENTURE_PACK_V1.md §4 ai.mode: absent or "none" = written lines only
        return mode.empty() || mode == "none" ? std::string("Written lines only") : std::string("AI characters");
    }

    S32 countProblems(const LLSD& problems, const char* level)
    {
        S32 n = 0;
        for (size_t i = 0; problems.isArray() && i < (size_t)problems.size(); ++i)
        {
            if (problems[(LLSD::Integer)i]["level"].asString() == level) ++n;
        }
        return n;
    }

    /** A time the service gives as unix seconds (BRAIN_API_V1.md §5.3 note) as a local date and time;
     *  any other form is shown as it came. */
    std::string whenText(const LLSD& v)
    {
        if (v.isInteger() || v.isReal())
        {
            const time_t t = (time_t)v.asReal();
            struct tm lt;
#if LL_WINDOWS
            if (localtime_s(&lt, &t) != 0) return std::string();
#else
            if (!localtime_r(&t, &lt)) return std::string();
#endif
            char buf[64];
            strftime(buf, sizeof(buf), "%d %b %Y %H:%M", &lt);
            return buf;
        }
        return WolfGame::cleanText(v.asString(), 32);
    }

    std::string plural(S32 n, const char* one, const char* many) { return llformat("%d %s", n, n == 1 ? one : many); }
}

// static
std::string WolfAdventureStudioView::problemWords(const LLSD& pr)
{
    // AUDIT_2026-10-17.md §5.1: `say` is the brain's plain sentence; a brain without it gives only `text` (schema words)
    const std::string say = pr["say"].asString();
    return say.empty() ? pr["text"].asString() : say;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the view
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfAdventureStudioView::WolfAdventureStudioView(const Params& p)
:   LLUICtrl(p)
{
}

bool WolfAdventureStudioView::advancedOpen()
{
    if (mAdvancedState < 0) mAdvancedState = gSavedPerAccountSettings.getBOOL("WolfAdventureAdvancedOpen") ? 1 : 0;
    return mAdvancedState == 1;
}

void WolfAdventureStudioView::setAdvancedOpen(bool on)
{
    mAdvancedState = on ? 1 : 0;
    gSavedPerAccountSettings.setBOOL("WolfAdventureAdvancedOpen", on);
}

void WolfAdventureStudioView::onOpened(const LLSD& key)
{
    loadList();
    loadRequests();
    // [FRONT DOOR 2026-10-17] the first time anyone opens the new Studio: the helper's greeting (frontDoor / helperColumn show it
    // while nothing is said yet) and the Guide beside it, saying the same
    if (!gSavedPerAccountSettings.getBOOL("WolfAdventureStudioWelcomed"))
    {
        gSavedPerAccountSettings.setBOOL("WolfAdventureStudioWelcomed", TRUE);
        gSavedSettings.setBOOL("WolfAdventureGuideOpen", TRUE);
        mPage = SP_HELPER;
    }
    // the story box has the keyboard on the front door: type at once (the persona's 10 seconds)
    if (mPage == SP_HELPER && mOpenId.empty()) { mField = F_HELPER; mHits.setFocus(SH_FIELD, F_HELPER); }
    // [WOLF ADVENTURE P2 2026-10-16] Wolf World > GM Console...: the Run page
    if (key.isString() && key.asString() == "run")
    {
        mPage = SP_RUN;
        loadSessions();
    }
    // §6 3.3.7: the last-used region is offered as the default - here, the region you stand on
    if (mImportRegion.empty())
    {
        if (LLViewerRegion* rgn = gAgent.getRegion())
        {
            mImportRegion = rgn->getRegionID().asString();
        }
    }
}

void WolfAdventureStudioView::setStatus(const std::string& text, bool error)
{
    mStatus = text;
    mStatusError = error;
    mStatusUntil = error ? 0.0 : now() + STATUS_LIFE;
}

void WolfAdventureStudioView::setProblems(const LLSD& problems)
{
    mProblems = problems.isArray() ? problems : LLSD::emptyArray();
    // [WOLF ADVENTURE P3 2026-10-16] made once here, read every frame by the editors: how many problems sit at or
    // under each JSON pointer (the Outline's counts), and the message at each exact pointer (an error before a warning)
    mProblemCount.clear();
    mProblemText.clear();
    for (const LLSD& pr : llsd::inArray(mProblems))
    {
        const std::string path = pr["path"].asString();
        const bool err = pr["level"].asString() == "error";
        auto t = mProblemText.find(path);
        // [WOLF ADVENTURE AUDIT 2026-10-17] the brain's plain sentence (`say`, AUDIT_2026-10-17.md §5.1) beside the field;
        // its schema words (`text`) only from a brain that has no `say` yet
        if (t == mProblemText.end() || (err && !t->second.first)) mProblemText[path] = { err, cap(problemWords(pr), 300) };
        for (size_t at = 0; at != std::string::npos; at = path.find('/', at + 1))
        {
            if (at > 0) ++mProblemCount[path.substr(0, at)];
            if (at + 1 >= path.size()) break;
        }
        ++mProblemCount[path];
    }
}

S32 WolfAdventureStudioView::requestIndex(const std::string& adv, const std::string& region) const
{
    for (size_t i = 0; mRequests.isArray() && i < (size_t)mRequests.size(); ++i)
    {
        const LLSD& rq = mRequests[(LLSD::Integer)i];
        if (rq["adventure"].asString() == adv && rq["region"].asString() == region) return (S32)i;
    }
    return -1;
}

S32 WolfAdventureStudioView::grantIndex(const std::string& agent, const std::string& role, const std::string& region) const
{
    for (size_t i = 0; mGrants.isArray() && i < (size_t)mGrants.size(); ++i)
    {
        const LLSD& g = mGrants[(LLSD::Integer)i];
        if (g["agent"].asString() == agent && g["role"].asString() == role && g["region"].asString() == region) return (S32)i;
    }
    return -1;
}

const LLSD* WolfAdventureStudioView::listed(const std::string& id) const
{
    for (size_t i = 0; mList.isArray() && i < (size_t)mList.size(); ++i)
    {
        if (mList[(LLSD::Integer)i]["id"].asString() == id) return &mList[(LLSD::Integer)i];
    }
    return nullptr;
}

std::string& WolfAdventureStudioView::fieldText(EField which)
{
    switch (which)
    {
    case F_FILTER: return mPickFilter;
    case F_DELETE: return mDeleteTyped;
    case F_TALK: return mTestBox;              // [WOLF ADVENTURE P2 2026-10-16/]
    case F_ASSIST: return mAssistBox;
    case F_BOOK_TITLE: return mBookTitle;
    case F_BOOK_AUTHOR: return mBookAuthor;
    case F_HELPER: return mHelperBox;          // [WOLF STORY FIRST 2026-10-17/]
    default: return mNewTitle;
    }
}

// static
size_t WolfAdventureStudioView::fieldCap(EField which)
{
    // [WOLF ADVENTURE P2 2026-10-16] Source: BRAIN_API_V1.md §6.2 say "text (at most 500 characters)"
    // for "Talk to them now" and the assistant's line; a book's title and author are short labels.
    switch (which)
    {
    case F_TALK:
    case F_ASSIST: return 500;
    case F_BOOK_TITLE:
    case F_BOOK_AUTHOR: return 128;
    // [WOLF STORY FIRST 2026-10-17] a first message may be a whole story (kept as adventure.story, at most 20,000 characters);
    // later turns at most 2,000 (helperSend says so) - STORY_FIRST_DESIGN.md §5
    case F_HELPER: return 20000;
    default: return TITLE_MAX;
    }
}

void WolfAdventureStudioView::collectNotecards()
{
    mPickItems.clear();
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    LLIsType is_notecard(LLAssetType::AT_NOTECARD);
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, is_notecard);
    for (const LLPointer<LLViewerInventoryItem>& it : items)
    {
        if (it.notNull()) mPickItems.push_back({ it->getUUID(), it->getName() });
    }
    std::sort(mPickItems.begin(), mPickItems.end(), [](const std::pair<LLUUID, std::string>& a, const std::pair<LLUUID, std::string>& b)
    {
        return LLStringUtil::compareDict(a.second, b.second) < 0;
    });
}

void WolfAdventureStudioView::saveJson(const std::string& name, const std::string& json)
{
    if (json.empty())
    {
        setStatus("There is nothing to save yet - wait for the draft to load.", true);
        return;
    }
    std::string file = name;
    for (char& c : file)
    {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') c = '_';
    }
    if (file.empty()) file = "adventure";
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // the viewer's own save picker, answered on the main thread
    LLFilePickerReplyThread::startPicker(
        [handle, json](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (handle.isDead() || files.empty() || files.front().empty()) return;
            WolfAdventureStudioView* self = handle.get();
            llofstream out(files.front().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                self->setStatus("That file could not be written.", true);
                return;
            }
            out << json;
            out.close();
            self->setStatus("Saved " + gDirUtilp->getBaseFileName(files.front()) + ".", false);
        },
        LLFilePicker::FFSAVE_ALL, file + ".json");
}

void WolfAdventureStudioView::download(S32 revision)
{
    if (revision < 0)
    {
        saveJson(mOpenTitle, mDraftJson);
        return;
    }
    LLSD get;
    get["adventure"] = mOpenId;
    get["revision"] = revision;
    const std::string title = mOpenTitle;
    // §5.1 get_revision {adventure, revision} -> {pack}
    call("get_revision", get, [title, revision](WolfAdventureStudioView* self, const LLSD& rev)
    {
        self->saveJson(title + llformat("-rev%d", revision), rev["pack_json"].asString());
    }, "Fetching the revision...");
}

// ---- the service ----

void WolfAdventureStudioView::call(const std::string& action, const LLSD& body,
                                   std::function<void(WolfAdventureStudioView*, const LLSD&)> on_ok, const std::string& doing)
{
    ++mBusy;
    if (!doing.empty()) setStatus(doing, false);
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api(action, body, [handle, on_ok](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mBusy = llmax(0, self->mBusy - 1);
        if (!ok)
        {
            self->setStatus(error, true);
            return;
        }
        if (on_ok) on_ok(self, reply);
    });
}

void WolfAdventureStudioView::loadList()
{
    // Source: BRAIN_API_V1.md §5.1 list_mine {} -> {adventures:[{id,title,purpose,status,revision,aiMode,regions,updated,role?}]}
    call("list_mine", LLSD::emptyMap(), [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mList = reply["adventures"].isArray() ? reply["adventures"] : LLSD::emptyArray();
        self->mListLoaded = true;
        self->loadHomeSpend();   // [WOLF ADVENTURE P2 2026-10-16/] §2.1 "today's AI spend" on each card
        if (const LLSD* a = self->listed(self->mOpenId)) self->mOpenTitle = cap((*a)["title"].asString(), TITLE_MAX);
    }, std::string());
}

void WolfAdventureStudioView::loadRequests()
{
    // Source: BRAIN_API_V1.md §5.2 region_requests {} -> {requests:[...]} for the regions the caller holds
    call("region_requests", LLSD::emptyMap(), [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mRequests = reply["requests"].isArray() ? reply["requests"] : LLSD::emptyArray();
    }, std::string());
}

void WolfAdventureStudioView::openAdventure(const std::string& id)
{
    leaveOpen();   // [WOLF ADVENTURE P3 2026-10-16/] the adventure open now is saved and let go first
    mOpenId = id;
    const LLSD* a = listed(id);
    mOpenTitle = a ? cap((*a)["title"].asString(), TITLE_MAX) : std::string();
    setDraftText(std::string(), LLSD());
    mEtag.clear();
    mProblems = LLSD::emptyArray();
    mRevisions = LLSD::emptyArray();
    mGrants = LLSD::emptyArray();
    // [WOLF ADVENTURE AUDIT 2026-10-17] the owner lands on Start here at the step they left (or the first one not done yet,
    // chosen once the draft has loaded); a game master or teacher, who cannot edit, on Publish (B8)
    const bool owner = !a || (*a)["role"].asString().empty();
    setProblems(mProblems);   // [review] the last adventure's inline problems and counts go too
    mWizError.clear();
    mWizTested = false;
    mWizAuto = true;
    {
        const LLSD steps = gSavedSettings.getLLSD("WolfAdventureWizardSteps");
        if (steps.isMap() && steps.has(id))
        {
            mWizStep = (EWizStep)llclamp(steps[id]["step"].asInteger(), (S32)W_WHERE, (S32)W_COUNT - 1);
            mWizTested = steps[id]["tested"].asBoolean();
            mWizAuto = false;
        }
    }
    // [WOLF STORY FIRST 2026-10-17] the main way to build is the helper (STORY_FIRST_DESIGN.md principle); Start here stays
    mPage = owner ? SP_HELPER : SP_PUBLISH;
    mScroll[mPage] = 0.f;
    mFlowSel.clear();
    mFlowGen = 0;
    mHelperQuestion.clear();
    mHelperError.clear();
    mHelperScript.clear();                     // [M6] a script for the last adventure is not offered for this one
    mLivePreview = LLSD();                     // [M7] nor its live-update preview
    mLiveMoves.clear();
    mHelperTodo = LLSD::emptyArray();          // [review] nothing of the last adventure's conversation shows
    mHelperTodayCredits = -1.0;
    mHelperLastCredits = 0.0;
    mHelperPending = false;
    mFlowPanX = mFlowPanY = 0.f;
    mFlowZoom = 1.f;
    mHelperWalking = false;                    // [M3] read again from helper_history.walk
    mHelperWalkRegion.clear();
    mHelperLookFrom.clear();
    mHelperNext = LLSD();
    loadOpen();
    if (owner) helperHistory();
    if (mPage == SP_WIZARD && (mWizStep == W_CAST || mWizStep == W_TEST)) loadBudget();   // [review] the cost line's estimate
}

void WolfAdventureStudioView::closeOpenAdventure()
{
    // [WOLF ADVENTURE AUDIT 2026-10-17] saved and let go (leaveOpen), then nothing is open: Home's "Make a new adventure"
    // and Start here's "Make a different adventure"
    leaveOpen();
    mOpenId.clear();
    mOpenTitle.clear();
    setDraftText(std::string(), LLSD());
    mEtag.clear();
    mProblems = LLSD::emptyArray();
    setProblems(mProblems);
    mWizTemplate.clear();
    mWizError.clear();
    mFlowSel.clear();          // [WOLF STORY FIRST 2026-10-17/]
    mFlowGen = 0;
    mHelperQuestion.clear();
    mHelperError.clear();
    mHelperScript.clear();
    mLivePreview = LLSD();
    mLiveMoves.clear();
    mHelperMissing = false;    // [review] asked again with the next adventure's history; the front door's Build it never stays dead
    mField = F_HELPER;         // back on the front door the story box has the keyboard
}

void WolfAdventureStudioView::loadOpen()
{
    if (mOpenId.empty()) return;
    stopPlacing();   // [WOLF ADVENTURE P3 2026-10-16/] the draft is about to be replaced
    LLSD body;
    body["adventure"] = mOpenId;
    const LLSD* a = listed(mOpenId);
    const bool owner = !a || (*a)["role"].asString().empty();
    if (owner)
    {
        // §5.1 get_draft {adventure} -> {pack, etag, problems}
        const std::string id = mOpenId;
        call("get_draft", body, [id](WolfAdventureStudioView* self, const LLSD& reply)
        {
            if (self->mOpenId != id) return;
            self->setDraftText(reply["pack_json"].asString(), reply["pack"]);
            self->mEtag = reply["etag"].asString();
            self->setProblems(reply["problems"]);
            // [WOLF ADVENTURE AUDIT 2026-10-17] a draft the old Studio saved with {} where text belongs: the brain tidies it on
            // get_draft and says so (`tidied`); an older brain did not, so the viewer tidies what is left and saves it
            const S32 fixed = self->repairLegacyDraft();
            if (reply["tidied"].isMap() && !reply["tidied"]["say"].asString().empty()) self->setStatus(cap(reply["tidied"]["say"].asString(), 240), false);
            else if (fixed > 0)
                self->setStatus(llformat("We tidied %d blank title%s left by an older version of the Studio: they now say what to write.", fixed, fixed == 1 ? "" : "s"), false);
            self->checkRecovery();   // [WOLF ADVENTURE P3 2026-10-16/] unsaved changes from the last time, offered back
        }, std::string());
    }
    // §5.1 revisions, §5.3 grants (owner or GM)
    call("revisions", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mRevisions = reply["revisions"].isArray() ? reply["revisions"] : LLSD::emptyArray();
    }, std::string());
    call("grants", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mGrants = reply["grants"].isArray() ? reply["grants"] : LLSD::emptyArray();
    }, std::string());
}

void WolfAdventureStudioView::createAdventure()
{
    std::string title = mNewTitle;
    LLStringUtil::trim(title);
    // STUDIO §6 3.3.1 / 3.3.3: the field says what is wrong and how to fix it, beside it
    if (title.empty())
    {
        mTitleError = "Type a title for the adventure (up to 64 characters).";
        mField = F_TITLE;
        return;
    }
    mTitleError.clear();
    title = cap(title, TITLE_MAX);
    const Purpose& pu = PURPOSES[llclamp(mNewPurpose, 0, PURPOSE_COUNT - 1)];
    const std::string preset = PRESETS[llclamp(mNewPreset, 0, PRESET_COUNT - 1)].mId;
    LLSD body;
    body["title"] = title;
    body["purpose"] = pu.mId;
    // Source: BRAIN_API_V1.md §5.1 create {title, purpose} -> {adventure:{id,title,status}, etag}: an
    // empty valid draft. The preset chosen here is then written into that draft's rules.preset.
    call("create", body, [title, preset](WolfAdventureStudioView* self, const LLSD& reply)
    {
        const std::string id = reply["adventure"]["id"].asString();
        if (id.empty())
        {
            self->setStatus("The adventure service did not say which adventure it made - reopen the Studio.", true);
            return;
        }
        self->mNewTitle.clear();
        self->loadList();
        LLSD get;
        get["adventure"] = id;
        self->call("get_draft", get, [id, title, preset](WolfAdventureStudioView* self, const LLSD& draft)
        {
            const std::string have = draft["pack"]["rules"]["preset"].asString();
            if (have == preset)
            {
                self->openAdventure(id);
                self->setStatus("Made \"" + title + "\". Import a story into it, or publish it when it is ready.", false);
                return;
            }
            const std::string json = packWith(draft["pack_json"].asString(), [preset](boost::json::object& o) { setIn(o, "rules", "preset", preset); });
            LLSD save;
            save["adventure"] = id;
            save["pack_json"] = json;
            save["etag"] = draft["etag"].asString();
            self->call("save_draft", save, [id, title](WolfAdventureStudioView* self, const LLSD& saved)
            {
                self->openAdventure(id);
                self->setProblems(saved["problems"]);
                self->setStatus("Made \"" + title + "\". Import a story into it, or publish it when it is ready.", false);
            }, std::string());
        }, std::string());
    }, "Making \"" + title + "\"...");
}

void WolfAdventureStudioView::duplicate(const std::string& id)
{
    const LLSD* a = listed(id);
    if (!a) return;
    const std::string title = cap("Copy of " + (*a)["title"].asString(), TITLE_MAX);
    const std::string purpose = (*a)["purpose"].asString();
    LLSD get;
    get["adventure"] = id;
    // get the draft, make a new adventure, save the draft into it under the new title
    call("get_draft", get, [title, purpose](WolfAdventureStudioView* self, const LLSD& draft)
    {
        const std::string json = packWith(draft["pack_json"].asString(), [title](boost::json::object& o) { setIn(o, "adventure", "title", title); });
        LLSD body;
        body["title"] = title;
        body["purpose"] = purpose.empty() ? std::string("adventure") : purpose;
        self->call("create", body, [json, title](WolfAdventureStudioView* self, const LLSD& made)
        {
            const std::string nid = made["adventure"]["id"].asString();
            if (nid.empty())
            {
                self->setStatus("The copy was made but the service did not say which it is - reopen the Studio.", true);
                return;
            }
            self->saveDraftTo(nid, json, title);
            self->loadList();
        }, std::string());
    }, "Copying...");
}

void WolfAdventureStudioView::saveDraftTo(const std::string& adv, const std::string& pack_json, const std::string& title)
{
    // §5.1 save_draft {adventure, pack, etag}: the etag of the draft it replaces (get_draft)
    LLSD get;
    get["adventure"] = adv;
    call("get_draft", get, [adv, pack_json, title](WolfAdventureStudioView* self, const LLSD& draft)
    {
        LLSD save;
        save["adventure"] = adv;
        save["pack_json"] = pack_json;
        save["etag"] = draft["etag"].asString();
        self->call("save_draft", save, [adv, title](WolfAdventureStudioView* self, const LLSD& saved)
        {
            self->mImportedJson.clear();
            self->mImportedPack = LLSD();
            self->mReport = LLSD();
            self->mImportProblems = LLSD::emptyArray();
            self->openAdventure(adv);
            self->setProblems(saved["problems"]);
            const S32 errors = countProblems(saved["problems"], "error");
            self->setStatus(errors ? "Saved as the draft of \"" + title + "\" - " + plural(errors, "problem", "problems") + " to fix before publishing."
                                   : "Saved as the draft of \"" + title + "\".", false);
        }, std::string());
    }, "Saving the draft...");
}

void WolfAdventureStudioView::deleteAdventure()
{
    if (mDeleteId.empty() || mDeleteTyped != mDeleteTitle) return;
    LLSD body;
    body["adventure"] = mDeleteId;
    body["confirm"] = mDeleteTitle;   // §5.1 delete {adventure, confirm:"<title>"}
    const std::string id = mDeleteId, title = mDeleteTitle;
    call("delete", body, [id, title](WolfAdventureStudioView* self, const LLSD&)
    {
        if (self->mOpenId == id)
        {
            self->mOpenId.clear();
            self->mPage = SP_HELPER;   // [FRONT DOOR] back to the front door
        }
        self->loadList();
        self->setStatus("Deleted \"" + title + "\".", false);
    }, "Deleting...");
    mDeleteTyped.clear();
}

void WolfAdventureStudioView::publish(S32 republish_revision)
{
    if (mOpenId.empty()) return;
    const std::string id = mOpenId, title = mOpenTitle;
    auto do_publish = [id, title](WolfAdventureStudioView* self, const std::string& etag)
    {
        LLSD body;
        body["adventure"] = id;
        body["etag"] = etag;
        // §5.1 publish {adventure, etag} -> {revision, sha256}
        self->call("publish", body, [title](WolfAdventureStudioView* self, const LLSD& reply)
        {
            self->setStatus(llformat("Published \"%s\" as revision %d.", title.c_str(), reply["revision"].asInteger()), false);
            self->loadList();
            self->loadOpen();
        }, "Publishing...");
    };
    if (republish_revision < 0)
    {
        do_publish(this, mEtag);
        return;
    }
    // §2.11 "Republish this revision (it becomes revision N+1)": the old revision becomes the
    // draft, then publishes.
    LLSD get;
    get["adventure"] = id;
    get["revision"] = republish_revision;
    call("get_revision", get, [id, do_publish](WolfAdventureStudioView* self, const LLSD& rev)
    {
        const std::string json = rev["pack_json"].asString();
        LLSD g;
        g["adventure"] = id;
        self->call("get_draft", g, [id, json, do_publish](WolfAdventureStudioView* self, const LLSD& draft)
        {
            LLSD save;
            save["adventure"] = id;
            save["pack_json"] = json;
            save["etag"] = draft["etag"].asString();
            self->call("save_draft", save, [do_publish](WolfAdventureStudioView* self, const LLSD& saved)
            {
                self->setProblems(saved["problems"]);
                do_publish(self, saved["etag"].asString());
            }, std::string());
        }, std::string());
    }, "Bringing the old revision back...");
}

void WolfAdventureStudioView::unpublish()
{
    if (mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    call("unpublish", body, [](WolfAdventureStudioView* self, const LLSD&)
    {
        self->setStatus("Unpublished: every session closed, and its characters leave the regions.", false);
        self->loadList();
        self->loadOpen();
    }, "Unpublishing...");
}

void WolfAdventureStudioView::requestRegion(const std::string& region)
{
    if (mOpenId.empty() || region.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["region"] = region;
    // §5.2 request_region: a region you hold is enabled at once
    call("request_region", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->setStatus(reply["state"].asString() == "enabled" ? "The region is enabled." : "Asked the region's holder.", false);
        self->loadList();
    }, "Asking...");
}

void WolfAdventureStudioView::approve(S32 index)
{
    if (!mRequests.isArray() || index < 0 || index >= (S32)mRequests.size()) return;
    const LLSD& rq = mRequests[index];
    LLSD body;
    body["adventure"] = rq["adventure"].asString();
    body["region"] = rq["region"].asString();
    body["mayEnv"] = mApproveEnv ? 1 : 0;
    body["npcBudget"] = llclamp(mApproveBudget, 0, NPC_BUDGET_MAX);
    // §5.2 enable_region {adventure, region, mayEnv, npcBudget}; the service refuses (with the
    // reason) a rating above the region's maturity
    call("enable_region", body, [](WolfAdventureStudioView* self, const LLSD&)
    {
        self->mApproveAdv.clear();
        self->mApproveRegion.clear();
        self->setStatus("Approved: the adventure may run on your region.", false);
        self->loadRequests();
    }, "Approving...");
}

void WolfAdventureStudioView::refuse(S32 index)
{
    if (!mRequests.isArray() || index < 0 || index >= (S32)mRequests.size()) return;
    const LLSD& rq = mRequests[index];
    LLSD body;
    body["adventure"] = rq["adventure"].asString();
    body["region"] = rq["region"].asString();
    call("refuse_region", body, [](WolfAdventureStudioView* self, const LLSD&)
    {
        self->setStatus("Refused.", false);
        self->loadRequests();
    }, "Refusing...");
}

void WolfAdventureStudioView::pickGrantee(const std::string& role)
{
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // the viewer's own resident picker (search, friends, near me)
    LLFloaterAvatarPicker::show([handle, role](const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
    {
        if (handle.isDead() || ids.empty()) return;
        handle.get()->grant(role, ids.front(), names.empty() ? ids.front().asString() : names.front().getCompleteName());
    }, false, true);
}

void WolfAdventureStudioView::grant(const std::string& role, const LLUUID& agent, const std::string& name)
{
    if (mOpenId.empty() || agent.isNull()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["agent"] = agent.asString();
    body["role"] = role;
    const S64 secs = EXPIRIES[llclamp(mGrantExpiry, 0, EXPIRY_COUNT - 1)].mSeconds;
    // BRAIN_API_V1.md §5.3 note: unix seconds, an integer (fits LLSD's 32 bits until 2038)
    if (secs > 0) body["expires"] = (LLSD::Integer)((S64)time(nullptr) + secs);
    const std::string what = role == "teacher" ? "teacher" : "game master";
    // §5.3 grant {adventure, agent, role, region?, expires?}
    call("grant", body, [name, what](WolfAdventureStudioView* self, const LLSD&)
    {
        self->setStatus(name + " is now a " + what + " of this adventure.", false);
        self->loadOpen();
    }, "Granting...");
}

void WolfAdventureStudioView::revoke(S32 index)
{
    if (!mGrants.isArray() || index < 0 || index >= (S32)mGrants.size()) return;
    const LLSD& g = mGrants[index];
    LLSD body;
    body["adventure"] = mOpenId;
    body["agent"] = g["agent"].asString();
    body["role"] = g["role"].asString();
    if (!g["region"].asString().empty()) body["region"] = g["region"].asString();
    call("revoke", body, [](WolfAdventureStudioView* self, const LLSD&)
    {
        self->setStatus("Taken away.", false);
        self->loadOpen();
    }, "Taking it away...");
}

void WolfAdventureStudioView::runImport()
{
    if (mStoryText.empty())
    {
        setStatus("Drop the STORY notecard from your inventory onto this page first.", true);
        return;
    }
    if (mImportRegion.empty())
    {
        setStatus("Choose the region the story is for.", true);
        return;
    }
    LLSD body;
    if (!mOpenId.empty()) body["adventure"] = mOpenId;
    body["kind"] = "story-v1";
    body["name"] = mStoryName;
    body["content"] = mStoryText;
    body["assets"] = mLooks.isMap() ? mLooks : LLSD::emptyMap();
    body["region"] = mImportRegion;
    // Source: BRAIN_API_V1.md §5.1 import {adventure?, kind:"story-v1", name, content, assets, region}
    // -> {pack, problems, report}; never saved automatically
    call("import", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mImportedJson = reply["pack_json"].asString();
        self->mImportedPack = reply["pack"];
        self->mReport = reply["report"].isArray() ? reply["report"] : LLSD::emptyArray();
        self->mImportProblems = reply["problems"].isArray() ? reply["problems"] : LLSD::emptyArray();
        self->setStatus(self->mImportedJson.empty() ? std::string("The import made no pack - see the report.")
                                                    : std::string("Imported. Look over the report, then Use as draft."), self->mImportedJson.empty());
    }, "Importing...");
}

void WolfAdventureStudioView::choosePackFile()
{
    // Source: wolffloaterroleplay.cpp onChooseFile - the viewer's own file picker, answered on the main thread
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (handle.isDead() || files.empty() || files.front().empty()) return;
            WolfAdventureStudioView* self = handle.get();
            const std::string path = files.front();
            llifstream in(path.c_str(), std::ios::in | std::ios::binary);
            if (!in.is_open())
            {
                self->setStatus("That file could not be opened.", true);
                return;
            }
            // the size first: a huge file is refused before anything is read
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0 || (U64)size > PACK_MAX_BYTES)
            {
                self->setStatus("That file is larger than a pack may be (2 MiB).", true);
                return;
            }
            in.seekg(0, std::ios::beg);
            std::ostringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(text, ec);
            // ADVENTURE_PACK_V1.md §1: a pack is one JSON object with format "wolf-adventure-pack"
            if (ec || !v.is_object() || !v.as_object().contains("format") || !v.as_object().at("format").is_string()
                || v.as_object().at("format").as_string() != "wolf-adventure-pack")
            {
                self->setStatus("That is not a Wolf Adventure pack (a JSON file whose \"format\" is \"wolf-adventure-pack\").", true);
                return;
            }
            self->mPackFile = gDirUtilp->getBaseFileName(path);
            self->mImportedJson = boost::json::serialize(v);
            self->mImportedPack = LlsdFromJson(v);
            self->mReport = LLSD::emptyArray();
            self->mReport.append("Read " + self->mPackFile + llformat(" (%d bytes).", (S32)text.size()));
            // §2.10 "Upload a .json pack, then validate, then Use as draft" - §5.1 validate {pack}
            LLSD body;
            body["pack_json"] = self->mImportedJson;
            self->call("validate", body, [](WolfAdventureStudioView* self, const LLSD& reply)
            {
                self->mImportProblems = reply["problems"].isArray() ? reply["problems"] : LLSD::emptyArray();
                const S32 errors = countProblems(reply["problems"], "error");
                self->setStatus(errors ? plural(errors, "problem", "problems") + " found - you can still use it as the draft and fix them later."
                                       : std::string("The pack is valid. Use as draft to keep it."), false);
            }, "Checking the pack...");
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void WolfAdventureStudioView::useAsDraft()
{
    if (mImportedJson.empty()) return;
    if (!mOpenId.empty())
    {
        saveDraftTo(mOpenId, mImportedJson, mOpenTitle);
        return;
    }
    // no adventure open: make one from the pack's own title and purpose, then save into it
    std::string title = cap(mImportedPack["adventure"]["title"].asString(), TITLE_MAX);
    if (title.empty()) title = "Imported adventure";
    std::string purpose = mImportedPack["adventure"]["purpose"].asString();
    bool known = false;
    for (const Purpose& pu : PURPOSES) known = known || purpose == pu.mId;
    if (!known) purpose = "adventure";
    LLSD body;
    body["title"] = title;
    body["purpose"] = purpose;
    const std::string json = mImportedJson;
    call("create", body, [json, title](WolfAdventureStudioView* self, const LLSD& made)
    {
        const std::string id = made["adventure"]["id"].asString();
        if (id.empty())
        {
            self->setStatus("The adventure service did not say which adventure it made - reopen the Studio.", true);
            return;
        }
        self->loadList();
        self->saveDraftTo(id, json, title);
    }, "Making \"" + title + "\"...");
}

void WolfAdventureStudioView::readNotecard(LLInventoryItem* item)
{
    const std::string name = item->getName();
    // ADVENTURE_PACK_V1.md §13.1: the `Look - <name>` notecards give their asset UUIDs only; the
    // STORY notecard gives its text.
    if (name.rfind(LOOK_PREFIX, 0) == 0)
    {
        if (!mLooks.isMap()) mLooks = LLSD::emptyMap();
        mLooks[name] = item->getAssetUUID().asString();
        setStatus("Added the look \"" + name + "\".", false);
        return;
    }
    const U32 serial = ++mNotecardSerial;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    setStatus("Reading \"" + name + "\"...", false);
    // Source: aoengine.cpp importNotecard - an inventory notecard's text through the asset store
    // (getInvItemAsset), read from the cache when it lands (LLFileSystem).
    gAssetStorage->getInvItemAsset(LLHost(), gAgentID, gAgentSessionID, item->getPermissions().getOwner(), LLUUID::null,
        item->getUUID(), item->getAssetUUID(), item->getType(),
        [handle, serial, name](const LLUUID& asset_id, LLAssetType::EType type, void*, S32 status, LLExtStat)
        {
            if (handle.isDead()) return;
            WolfAdventureStudioView* self = handle.get();
            if (serial != self->mNotecardSerial) return;   // another notecard was dropped since
            if (status != LL_ERR_NOERR)
            {
                self->setStatus("\"" + name + "\" could not be read from the grid. Try dropping it again.", true);
                return;
            }
            LLFileSystem file(asset_id, type, LLFileSystem::READ);
            const S32 size = file.getSize();
            if (size <= 0 || (size_t)size > PACK_MAX_BYTES)
            {
                self->setStatus("\"" + name + "\" is empty or too large.", true);
                return;
            }
            std::string raw((size_t)size, '\0');
            if (!file.read((U8*)&raw[0], size))
            {
                self->setStatus("\"" + name + "\" could not be read from the cache.", true);
                return;
            }
            // the Linden notecard wrapper ("Linden text version 2 ... Text length N") around the text
            LLNotecard nc;
            std::istringstream in(raw);
            if (!nc.importStream(in))
            {
                self->setStatus("\"" + name + "\" is not a notecard the viewer can read.", true);
                return;
            }
            self->mStoryName = name;
            self->mStoryText = nc.getText();
            self->mImportedJson.clear();
            self->mImportedPack = LLSD();
            self->mReport = LLSD();
            self->setStatus("Read \"" + name + llformat("\" (%d characters). Choose the region, then Import.", (S32)utf8str_to_wstring(nc.getText()).size()), false);
        }, nullptr, true);
}

// ---- drawing ----

void WolfAdventureStudioView::hit(F32 l, F32 b, F32 r, F32 t, S32 id, S32 arg, const std::string& tip, bool focusable, bool enabled)
{
    if (mInPage)
    {
        l = llmax(l, (F32)mPageRect.mLeft);
        r = llmin(r, (F32)mPageRect.mRight);
        b = llmax(b, (F32)mPageRect.mBottom);
        t = llmin(t, (F32)mPageRect.mTop);
        if (r <= l || t <= b) return;
    }
    mHits.add(l, b, r, t, id, arg, tip, focusable, enabled);
}

U32 WolfAdventureStudioView::flagsFor(S32 id, S32 arg, bool enabled) const
{
    U32 f = 0;
    if (!enabled) f |= BTN_DISABLED;
    if (mHits.isHover(id, arg)) f |= BTN_HOVER;
    if (mHits.isPressed(id, arg)) f |= BTN_PRESSED;
    if (mKeyboardNav && hasFocus() && mHits.isFocused(id, arg)) f |= BTN_FOCUS;
    return f;
}

void WolfAdventureStudioView::btn(F32 l, F32 b, F32 r, F32 t, const std::string& label, S32 id, S32 arg, const std::string& tip,
                                  const Palette& p, bool enabled, U32 extra)
{
    button(l, b, r, t, label, p, flagsFor(id, arg, enabled) | extra);
    hit(l, b, r, t, id, arg, tip, true, enabled);
}

F32 WolfAdventureStudioView::head(F32 l, F32 r, F32 top, const std::string& text, const Palette& p)
{
    const LLFontGL* f = LLFontGL::getFontSansSerifSmallBold();
    const F32 lh = (F32)f->getLineHeight();
    textL(f, caps(text), l, top, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    rect(l, top - lh - 3.f, r, top - lh - 2.f, fade(p.mAccent, 0.3f));
    return top - lh - 8.f;
}

F32 WolfAdventureStudioView::para(F32 l, F32 r, F32 top, const std::string& text, const LLColor4& colour, bool small)
{
    const LLFontGL* f = small ? LLFontGL::getFontSansSerifSmall() : LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    F32 y = top;
    for (const std::string& line : wrap(f, text, r - l, 40))
    {
        textL(f, line, l, y, colour);
        y -= lh;
    }
    return y;
}

void WolfAdventureStudioView::field(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, EField which,
                                    const Palette& p)
{
    const bool focused = hasFocus() && mField == which;
    chamfer(l, b, r, t, 4.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
    const bool invalid = which == F_TITLE && !mTitleError.empty();
    chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, 4.f, focused || invalid ? 2.f : 1.f,
                invalid ? DANGER_INK : (focused ? p.mAccent : p.mDim));
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 cy = (b + t) * 0.5f;
    F32 x = l + 8.f;
    if (text.empty()) textL(f, placeholder, x, cy, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 16.f);
    else x = textL(f, text, x, cy, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 16.f);
    if (focused && fmod(now(), 1.0) < 0.6)
    {
        rect(x + 1.f, cy - (F32)f->getLineHeight() * 0.45f, x + 2.f, cy + (F32)f->getLineHeight() * 0.45f, p.mText);
    }
    std::string tip;
    switch (which)
    {
    case F_TITLE: tip = "The adventure's title (up to 64 characters)"; break;
    case F_DELETE: tip = "Type the title exactly to confirm"; break;
    case F_TALK: tip = "What you say to the character (Enter sends; up to 500 characters)"; break;
    case F_ASSIST: tip = "The player's line the assistant should answer (up to 500 characters)"; break;
    case F_BOOK_TITLE: tip = "The book's title (it goes into the draft's attribution)"; break;
    case F_BOOK_AUTHOR: tip = "The book's author (it goes into the draft's attribution)"; break;
    case F_HELPER: tip = "Tell the helper what happens, or ask it anything (Enter sends; Ctrl+V pastes)"; break;
    default: tip = "Type to narrow the list"; break;
    }
    hit(l, b, r, t, SH_FIELD, (S32)which, tip);
}

void WolfAdventureStudioView::draw()
{
    autosaveTick();   // [WOLF ADVENTURE P2 2026-10-16/]
    editTick();       // [WOLF ADVENTURE P3 2026-10-16/]
    mHits.clear();
    mActs.clear();
    mDrops.clear();
    layoutAndDraw();
    if (hasFocus()) mHits.keepFocusValid();
    LLUICtrl::draw();
}

void WolfAdventureStudioView::layoutAndDraw()
{
    const Palette& p = gamePalette();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    LLGLSUIDefault gls_ui;
    frame(0.f, 0.f, W, H, p, 1.f, 14.f);
    // ---- the title band (under the floater's 34 px drag strip) ----
    const F32 M = 14.f;
    F32 x = M + 2.f;
    const F32 ty = H - 10.f;
    crescent(x + 7.f, ty - 9.f, 7.f, p.mText, p.mBaseTop);
    x = textL(bold, "ADVENTURE STUDIO", x + 20.f, ty - 2.f, p.mDim) + 10.f;
    textL(display(17.f), mOpenId.empty() ? std::string(mPage == SP_HELPER ? "Make a game" : "Adventures") : mOpenTitle, x, ty, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, W - x - 170.f);   // room for Guide and Close
    {
        const F32 xc = W - M - 8.f, yc = ty - 9.f;
        closeX(xc, yc, 5.f, mHits.isHover(SH_CLOSE, 0) ? p.mAccent : p.mText);
        if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_CLOSE, 0)) focusRing(xc - 9.f, yc - 9.f, xc + 9.f, yc + 9.f, p);
        hit(xc - 11.f, yc - 11.f, xc + 11.f, yc + 11.f, SH_CLOSE, 0, "Close the Studio (Escape)");
    }
    {
        // [WOLF ADVENTURE AUDIT 2026-10-17] the Guide (help and "What do I do next?") beside the Studio, shown or hidden
        const bool shown = WolfFloaterAdventureStudio::guideShown();
        const std::string lab = shown ? "Hide guide" : "Guide";
        const F32 gw = (F32)bold->getWidthF32(lab) + 24.f, gr = W - M - 26.f;
        btn(gr - gw, H - 32.f, gr, H - 8.f, lab, SH_GUIDE, 0, shown ? "Hide the Guide beside the Studio" : "Show the Guide: what each page and step is for, and what to do next", p);
    }
    const F32 top = H - 40.f;
    // ---- the status line along the foot ----
    F32 bottom = 10.f;
    if (!mStatus.empty() && mStatusUntil > 0.0 && now() > mStatusUntil) mStatus.clear();
    if (!mStatus.empty() || mBusy > 0)
    {
        const F32 sh = (F32)small->getLineHeight() + 8.f;
        card(M, bottom, W - M, bottom + sh, p, 4.f);
        const F32 cy = bottom + sh * 0.5f;
        const std::string text = mStatus.empty() ? std::string("Working...") : mStatus;
        if (mStatusError)
        {
            // an icon AND the words (never colour alone)
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gGL.begin(LLRender::TRIANGLES);
            gGL.color4fv(WARN_INK.mV);
            gGL.vertex2f(M + 6.f, cy - 5.f); gGL.vertex2f(M + 18.f, cy - 5.f); gGL.vertex2f(M + 12.f, cy + 6.f);
            gGL.end();
        }
        else if (!mStatus.empty())
        {
            lineW(M + 7.f, cy, M + 10.f, cy - 3.f, 2.f, OK_INK);
            lineW(M + 10.f, cy - 3.f, M + 16.f, cy + 4.f, 2.f, OK_INK);
        }
        textL(small, text, M + 24.f, cy, mStatusError ? DANGER_INK : p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, W - 2.f * M - 30.f);
        bottom += sh + 6.f;
    }
    // ---- the left rail: the pages ----
    // [WOLF STORY FIRST FRONT DOOR 2026-10-17] Paul: "the viewer set up looks the same?" - the story-first screen (SP_HELPER) is
    // the front door and uses the whole width; the rail shows only on the other pages, or when "Advanced editing" is open.
    // Persona: a resident with a story idea opens the Studio and is talking to the helper within 10 seconds (Hick's Law: one
    // primary action on the front door; the 12 expert pages behind one disclosure, NN/G progressive disclosure).
    // a game master or teacher has no main screen of their own (pageHelper says so): the rail stays, with Publish and Run
    const bool showRail = mPage != SP_HELPER || advancedOpen() || (!mOpenId.empty() && !openIsOwner());
    const F32 rail_r = showRail ? M + 140.f : M - 14.f;
    if (showRail)
    {
        static const char* NAMES[SP_COUNT] = { "Adventures", "Import", "Outline", "Cast", "Places", "Story", "Rules", "Lore", "Test Play",
                                               "Publish", "AI and voice", "Cast talk", "Book and assist", "Run", "Start here",
                                               "Your game" };
        static const char* TIPS[SP_COUNT] = { "All your adventures (duplicate, delete, quick make) and requests for your regions",
                                               "Bring in a Wolf Story (STORY v1) or a pack JSON file as a draft",
                                               "Acts, scenes and beats: add, reorder, the flow of each scene, endings",
                                               "The characters: name and look, who they are, what they know, lines, voice, day, may do, fights",
                                               "Spots, areas, paths and chests, placed in the world with the mouse",
                                               "Edit a scene or a beat: its stage, triggers, decisions and hints",
                                               "The rules preset and your changes to it; the wolfGame link; flags and reputation",
                                               "Story and source cards, encounters, rewards, handouts, learning and quizzes",
                                               "Play the draft from any beat with the debug overlay",
                                               "Checks, regions, game masters and teachers, history, publish, update the live game",
                                               "AI characters or written lines only, the daily caps, voices, and today's spend",
                                               "Each character's voice, and Talk to them now",
                                               "Give it a book: draft an adventure from a text; ask the assistant for ideas",
                                               "Run it live: take control from the AI Director, pause, and hand it back",
                                               "Make an adventure step by step, with no AI",
                                               "Your story, the flowchart and the helper: the main screen" };
        // the main three, then "Advanced editing" (a disclosure: shut, it is one row) holding the expert pages
        const bool adv = advancedOpen();
        const bool inAdvPage = mPage != SP_HELPER && mPage != SP_TEST && mPage != SP_PUBLISH && mPage != SP_WIZARD;
        const bool advRows = adv || inAdvPage;
        static const S32 MAIN[] = { SP_HELPER, SP_TEST, SP_PUBLISH };
        static const S32 ADV[] = { SP_HOME, SP_OUTLINE, SP_CASTED, SP_PLACES, SP_STORY, SP_RULES, SP_LORE, SP_IMPORT, SP_BOOK, SP_AI, SP_CAST, SP_RUN };
        F32 y = top;
        auto row = [&](S32 i)
        {
            const bool can = i == SP_HOME || i == SP_IMPORT || i == SP_WIZARD || i == SP_HELPER || !mOpenId.empty();
            const std::string tip = can ? std::string(TIPS[i]) : std::string("Open a game first (Your game), or make one from a story");
            button(M, y - 25.f, rail_r, y, NAMES[i], p, flagsFor(SH_PAGE, i, can) | (mPage == i ? BTN_ACTIVE : 0));
            hit(M, y - 25.f, rail_r, y, SH_PAGE, i, tip, true, can);
            y -= 27.f;
        };
        for (S32 i : MAIN) row(i);
        y -= 8.f;
        {
            const std::string lab = advRows ? "Advanced editing \xE2\x96\xBE" : "Advanced editing \xE2\x96\xB8";   // the triangle AND the word
            // shut while on an expert page: back to your game (the rows go with it)
            const S32 ia = act([this, adv, advRows]() { if (advRows && !adv) { mPage = SP_HELPER; } else setAdvancedOpen(!adv); if (!advancedOpen() && mPage != SP_TEST && mPage != SP_PUBLISH) mPage = SP_HELPER; });
            btn(M, y - 25.f, rail_r, y, lab, SH_E, ia, advRows ? "Hide the expert pages" : "Show the expert pages: outline, cast, places, story, rules, lore and more", p, true, advRows ? BTN_ACTIVE : 0);
            y -= 29.f;
        }
        if (advRows) for (S32 i : ADV) row(i);
        if (mPage == SP_WIZARD) row(SP_WIZARD);   // reached from "Make it box by box (no AI)": shown while it is open
    }
    // ---- the right drawer: problems and help ----
    // [WOLF STORY FIRST 2026-10-17] the helper page uses the whole width (its problems sit on the flowchart's boxes)
    const F32 drawer_l = mPage == SP_HELPER ? W - M + 14.f : W - M - 230.f;
    if (mPage != SP_HELPER) drawDrawer(drawer_l, bottom, W - M, top, p);
    // ---- the page ----
    const F32 l = rail_r + 14.f, r = drawer_l - 14.f, b = bottom, t = top;
    mPageRect = LLRect((S32)l, (S32)t, (S32)r, (S32)b);
    F32& scroll = mScroll[mPage];
    F32 low = 0.f;
    {
        LLLocalClipRect clip(mPageRect);
        mInPage = true;
        const F32 pt = t + scroll;
        const F32 cr = r - 10.f;
        switch (mPage)
        {
        case SP_HOME: low = pageHome(l, cr, pt, p); break;
        case SP_IMPORT: low = pageImport(l, cr, pt, p); break;
        case SP_PUBLISH: low = pagePublish(l, cr, pt, p); break;
        case SP_AI: low = pageAi(l, cr, pt, p); break;          // [WOLF ADVENTURE P2 2026-10-16/]
        case SP_CAST: low = pageCast(l, cr, pt, p); break;
        case SP_BOOK: low = pageBook(l, cr, pt, p); break;
        case SP_RUN: low = pageRun(l, cr, pt, p); break;
        case SP_OUTLINE: low = pageOutline(l, cr, pt, p); break;   // [WOLF ADVENTURE P3 2026-10-16/]
        case SP_CASTED: low = pageCastEd(l, cr, pt, p); break;
        case SP_PLACES: low = pagePlaces(l, cr, pt, p); break;
        case SP_STORY: low = pageStory(l, cr, pt, p); break;
        case SP_RULES: low = pageRules(l, cr, pt, p); break;
        case SP_LORE: low = pageLore(l, cr, pt, p); break;
        case SP_TEST: low = pageTest(l, cr, pt, p); break;
        case SP_WIZARD: low = pageWizard(l, cr, pt, p); break;   // [WOLF ADVENTURE AUDIT 2026-10-17/]
        case SP_HELPER: low = pageHelper(l, cr, pt, p); break;   // [WOLF STORY FIRST 2026-10-17/]
        default: break;
        }
        mInPage = false;
        mContentH = pt - low + 8.f;
    }
    const F32 view_h = t - b, max_scroll = llmax(0.f, mContentH - view_h);
    scroll = llclamp(scroll, 0.f, max_scroll);
    if (max_scroll > 0.f)
    {
        const F32 th = llmax(24.f, view_h * view_h / mContentH), tyy = t - (view_h - th) * (scroll / max_scroll);
        rect(r - 6.f, b, r, t, fade(p.mTrack, 0.8f));
        rect(r - 6.f, tyy - th, r, tyy, mHits.isHover(SH_SCROLL, 0) || mDraggingScroll ? p.mAccent : fade(p.mDim, 0.8f));
        mHits.add(r - 9.f, b, r + 3.f, t, SH_SCROLL, 0, "Drag to scroll (or the mouse wheel)", false);
    }
    if (now() < mDragOverUntil)
    {
        chamferEdge(4.f, 4.f, W - 4.f, H - 4.f, 12.f, 3.f, p.mAccent);
    }
    if (mPicker.mOn && mConfirm == SC_NONE) drawPicker(p);   // [WOLF ADVENTURE P3 2026-10-16/]
    if (mConfirm != SC_NONE) drawConfirm(p);
}

void WolfAdventureStudioView::drawDrawer(F32 l, F32 b, F32 r, F32 t, const Palette& p)
{
    card(l, b, r, t, p, 6.f);
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = t - 10.f;
    // the page's own list: the import's on Import, the open draft's elsewhere
    const LLSD& problems = mPage == SP_IMPORT ? mImportProblems : mProblems;
    const S32 errors = countProblems(problems, "error"), warnings = countProblems(problems, "warning");
    if (problems.isArray() && problems.size() > 0)
    {
        // §1 "The brain is the only validator. The Studio shows the brain's problems list."
        y = head(l + 10.f, r - 10.f, y, "Problems", p);
        textL(small, plural(errors, "thing to fix", "things to fix") + ", " + plural(warnings, "to look at", "to look at"), l + 10.f, y, p.mText);
        y -= slh + 6.f;
        size_t shown = 0;
        for (size_t i = 0; i < (size_t)problems.size() && y > b + 40.f; ++i, ++shown)
        {
            const LLSD& pr = problems[(LLSD::Integer)i];
            const bool err = pr["level"].asString() == "error";
            const F32 item_top = y;
            // an icon AND the word (§6 3.3.1); "Must fix" / "Worth a look" in words a beginner reads
            textL(LLFontGL::getFontSansSerifSmallBold(), err ? "Must fix" : "Worth a look", l + 10.f, y, err ? DANGER_INK : WARN_INK);
            y -= slh;
            // [WOLF ADVENTURE AUDIT 2026-10-17] the plain sentence and where it is in words (AUDIT §5.1), never a JSON pointer
            y = para(l + 10.f, r - 10.f, y, cap(problemWords(pr), 500), p.mText, true);
            const std::string where = cap(pr["where"].asString(), 200);
            if (!where.empty()) y = para(l + 10.f, r - 10.f, y, where, p.mDim, true);
            const std::string path = pr["path"].asString();
            // [WOLF ADVENTURE P3 2026-10-16] STUDIO §2.2 "Clicking one opens its JSON pointer's editor and focuses the field"
            if (mPage != SP_IMPORT && !path.empty())
            {
                textL(LLFontGL::getFontSansSerifSmallBold(), "Fix it >", r - 10.f, y, p.mAccent, LLFontGL::RIGHT, LLFontGL::TOP);
                y -= slh;
                if (mHits.isHover(SH_PROBLEM, (S32)i)) rect(l + 4.f, y - 2.f, r - 4.f, item_top + 2.f, fade(p.mAccent, 0.12f));
                hit(l + 4.f, y - 2.f, r - 4.f, item_top + 2.f, SH_PROBLEM, (S32)i, "Go to this and fix it");
                if (mKeyboardNav && hasFocus() && mHits.isFocused(SH_PROBLEM, (S32)i)) focusRing(l + 4.f, y - 2.f, r - 4.f, item_top + 2.f, p);
            }
            y -= 6.f;
        }
        // [WOLF ADVENTURE AUDIT 2026-10-17] what did not fit is said, never silently dropped
        if (shown < (size_t)problems.size())
            textL(small, llformat("...and %d more: fix these first, or open Publish > Checks.", (S32)(problems.size() - shown)), l + 10.f, b + 26.f, p.mDim,
                  LLFontGL::LEFT, LLFontGL::TOP, r - l - 20.f);
        return;
    }
    y = head(l + 10.f, r - 10.f, y, "Help", p);
    const char* help = mPage == SP_AI
        ? "AI characters talk freely in character and the AI Director keeps the story moving; you pay for what they use from "
          "your WolfCredits. Written lines only costs nothing. When the day's cap or your credits run out, the adventure "
          "carries on with written lines by itself. Changes save by themselves two seconds after your last change."
        : mPage == SP_CAST
        ? "Give each character a voice, then try them out with Talk to them now on any beat. Each AI turn is paid like a "
          "player's; Preview without AI shows what players get when AI is off."
        : mPage == SP_BOOK
        ? "Give it a book: a public-domain text or your own outline becomes a draft to edit - nothing goes live. You see a "
          "quote first and set the most it may spend. The assistant's ideas are only for you; players never see them."
        : mPage == SP_RUN
        ? "Every adventure runs by itself. Take control to run it yourself; the AI Director stands back until you hand it "
          "back, and carries on from exactly where you left it."
        : mPage == SP_HOME
        ? "Make an adventure, open one to edit or publish it, or approve adventures that want to run on your regions."
        // [WOLF ADVENTURE AUDIT 2026-10-17] Start here
        : mPage == SP_WIZARD
        ? "One step at a time: each says what it is for and shows an example. Next checks the step and says what to fix. "
          "Your work saves by itself, and you can leave and come back: Start here remembers the step. The Guide beside the "
          "Studio explains more and shows what is left to do."
        // [WOLF ADVENTURE P3 2026-10-16] the editors' help
        : mPage == SP_OUTLINE
        ? "Select an act, scene or beat, then add, move, duplicate or delete it with the buttons. The flow shows where each beat "
          "leads; a beat that leads nowhere is marked dead end. Changes save by themselves two seconds after your last change."
        : mPage == SP_CASTED
        ? "Each character's sheet. Use what I'm wearing saves your outfit as their look (stand on a region the adventure runs on). "
          "A character the story still uses cannot be deleted: the places are listed."
        : mPage == SP_PLACES
        ? "While this page is open the places are drawn in the world. Place in world, Draw circle, Draw polygon and Draw path use "
          "the mouse in the world; Escape stops. Arrow keys nudge a spot, [ and ] turn it."
        : mPage == SP_STORY
        ? "Triggers read as sentences: choose one, then fill in its parts. Only if takes a condition, Do takes steps, Then go to "
          "moves the story on. Play from this beat tests it at once."
        : mPage == SP_RULES
        ? "Pick the preset closest to your story, then change what you need: changed values are marked, and Reset to preset "
          "puts them back."
        : mPage == SP_LORE
        ? "Story cards are lore the characters recall when a key word comes up; source cards are cited facts. Drag a texture "
          "onto a picture to use it."
        : mPage == SP_TEST
        ? "Test Play uses the saved draft and only you are in it. Change flags and fire triggers from the overlay to try every "
          "branch; nothing it gives is real."
        : mPage == SP_IMPORT
        ? "Wolf Story: drop the STORY notecard and its Look notecards from your inventory onto this window, choose the region, "
          "then Import. Pack JSON: choose a .json file. Nothing is saved until you press Use as draft."
        : "Publish makes the draft the live adventure. Players in progress move to the new revision at their next beat. "
          "Regions must be approved by whoever holds them; a region you hold is enabled at once.";
    para(l + 10.f, r - 10.f, y, help, p.mDim, true);
}

F32 WolfAdventureStudioView::pageHome(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const bool idle = mBusy == 0;
    F32 y = head(l, r - 100.f, top, "My adventures", p);
    btn(r - 90.f, top + 2.f - 24.f, r, top + 2.f, "Refresh", SH_REFRESH, 0, "Load the list again", p, idle);
    if (!mListLoaded) y = para(l, r, y, "Loading your adventures...", p.mDim) - 6.f;
    else if (!mList.isArray() || mList.size() == 0) y = para(l, r, y, "No adventures yet. Press Make a new adventure, step by step: it walks you through everything.", p.mDim) - 6.f;
    for (size_t i = 0; mList.isArray() && i < (size_t)mList.size(); ++i)
    {
        const LLSD& a = mList[(LLSD::Integer)i];
        const S32 arg = (S32)i;
        const std::string title = cap(a["title"].asString(), TITLE_MAX);
        const std::string role = a["role"].asString();
        const bool owner = role.empty();
        std::string words = statusWord(a["status"].asString(), a["revision"].asInteger());
        for (const Purpose& pu : PURPOSES) if (a["purpose"].asString() == pu.mId) words = std::string(pu.mLabel) + " \xC2\xB7 " + words;
        words += " \xC2\xB7 " + aiWord(a["aiMode"].asString());
        if (!owner) words += role == "teacher" ? " \xC2\xB7 You are a teacher here" : " \xC2\xB7 You are a game master here";
        {
            const auto sp = mHomeSpend.find(a["id"].asString());   // [WOLF ADVENTURE P2 2026-10-16/]
            if (sp != mHomeSpend.end()) words += " \xC2\xB7 " + sp->second;
        }
        std::string regions;
        const LLSD& rs = a["regions"];
        for (size_t k = 0; rs.isArray() && k < (size_t)rs.size(); ++k)
        {
            const LLSD& rg = rs[(LLSD::Integer)k];
            regions += (regions.empty() ? std::string() : std::string(", ")) + cap(rg["name"].asString(), 64) + ": " + stateWord(rg["state"].asString());
        }
        const std::vector<std::string> rl = wrap(small, regions.empty() ? std::string("No regions yet") : "Regions: " + regions, r - l - 24.f, 3);
        const F32 ch = 10.f + (F32)display(17.f)->getLineHeight() + slh + (F32)rl.size() * slh + 8.f + 28.f + 10.f;
        card(l, y - ch, r, y, p, 6.f);
        F32 cy = y - 10.f;
        textL(display(17.f), title, l + 12.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
        cy -= (F32)display(17.f)->getLineHeight();
        textL(small, words, l + 12.f, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
        cy -= slh;
        for (const std::string& line : rl)
        {
            textL(small, line, l + 12.f, cy, p.mText);
            cy -= slh;
        }
        cy -= 8.f;
        const F32 bw = 100.f;
        btn(l + 12.f, cy - 26.f, l + 12.f + bw, cy, "Open", SH_OPEN, arg, "Open " + title + " to import into it or publish it", p);
        if (owner)
        {
            btn(l + 20.f + bw, cy - 26.f, l + 20.f + 2.f * bw, cy, "Duplicate", SH_DUPLICATE, arg, "Make a copy of its draft as a new adventure", p, idle);
            // Delete far from the safe buttons (Fitts / NN/G: distance between Delete and the rest)
            btn(r - 12.f - bw, cy - 26.f, r - 12.f, cy, "Delete...", SH_DELETE, arg, "Delete " + title + " (asks, and needs the title typed)", p, idle, BTN_DANGER);
        }
        y -= ch + 8.f;
    }
    y -= 6.f;
    // [WOLF ADVENTURE AUDIT 2026-10-17] the beginner's way in, first and largest (Paul 2026-10-17: "a step-by-step wizard")
    {
        const std::string lab = "Make a new adventure with the helper";
        const F32 bw = llmax(300.f, (F32)bold->getWidthF32(lab) + 40.f);
        // a new one: whatever is open is saved and let go first (leaveOpen), then Start here begins at "What kind"
        // [WOLF STORY FIRST 2026-10-17] the helper first: the author's first line makes the adventure (pageHelper)
        const S32 idx = act([this]() { closeOpenAdventure(); mPage = SP_HELPER; mScroll[SP_HELPER] = 0.f; mHelperLog = LLSD::emptyArray(); mHelperFor.clear(); });
        btn(l, y - 40.f, l + llmin(bw, r - l), y, lab, SH_E, idx, "Start here: what kind, where, the story, characters, places, rewards, the look, test and publish", p, true, BTN_PRIMARY);
        y -= 48.f;
        y = para(l, r, y, "Tell the helper your idea in a line, paste a story, or load a book: it builds a first game you then talk through and walk round. "
                          "Prefer a checklist? Start here goes step by step, with no AI.", p.mDim, true) - 10.f;
    }
    // ---- New adventure (§2.1): the title, the purpose as four large choices, the preset ----
    y = head(l, r, y, "Quick make (if you know your way round)", p);
    field(l, y - 30.f, r, y, mNewTitle, "Title (up to 64 characters)", F_TITLE, p);
    y -= 34.f;
    if (!mTitleError.empty())
    {
        textL(bold, mTitleError, l, y, DANGER_INK, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 4.f;
    }
    {
        const F32 gap = 8.f, cw = (r - l - gap) * 0.5f, chh = 58.f;
        for (S32 i = 0; i < PURPOSE_COUNT; ++i)
        {
            const S32 col = i % 2, row = i / 2;
            const F32 cl = l + (F32)col * (cw + gap), ct = y - (F32)row * (chh + gap);
            const bool on = mNewPurpose == i;
            button(cl, ct - chh, cl + cw, ct, std::string(), p, flagsFor(SH_PURPOSE, i, true) | (on ? BTN_ACTIVE : 0));
            textL(bold, PURPOSES[i].mLabel + std::string(on ? " - chosen" : ""), cl + 10.f, ct - 8.f, on ? p.mAccent : p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            const std::vector<std::string> ln = wrap(small, PURPOSES[i].mLine, cw - 20.f, 2);
            F32 ly = ct - 8.f - slh - 2.f;
            for (const std::string& s : ln) { textL(small, s, cl + 10.f, ly, p.mDim); ly -= slh; }
            hit(cl, ct - chh, cl + cw, ct, SH_PURPOSE, i, std::string(PURPOSES[i].mLabel) + ": " + PURPOSES[i].mLine);
        }
        y -= 2.f * (chh + gap) + 4.f;
    }
    textL(bold, caps("Rules"), l, y, p.mDim);
    y -= slh + 4.f;
    {
        F32 x = l;
        for (S32 i = 0; i < PRESET_COUNT; ++i)
        {
            const F32 w = bold->getWidthF32(PRESETS[i].mLabel) + 22.f;
            if (x > l && x + w > r) { x = l; y -= 30.f; }
            const bool on = mNewPreset == i;
            btn(x, y - 24.f, x + w, y, PRESETS[i].mLabel, SH_PRESET, i, std::string("Rules: ") + PRESETS[i].mLabel + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += w + 6.f;
        }
        y -= 32.f;
    }
    btn(l, y - 30.f, l + 180.f, y, "Make the adventure", SH_CREATE, 0, "Make it as a draft (nothing is live until you publish)", p, idle);
    y -= 44.f;
    // ---- Requests for my regions (§2.1) ----
    if (mRequests.isArray() && mRequests.size() > 0)
    {
        y = head(l, r, y, "Requests for my regions", p);
        for (size_t i = 0; i < (size_t)mRequests.size(); ++i)
        {
            const LLSD& rq = mRequests[(LLSD::Integer)i];
            const S32 arg = (S32)i;
            const std::string title = cap(rq["title"].asString(), TITLE_MAX);
            std::string words = "By " + cap(rq["ownerName"].asString(), 64);
            const std::string region = cap(rq["regionName"].asString(), 64);
            if (!region.empty()) words += " \xC2\xB7 for " + region;
            words += " \xC2\xB7 rated " + cap(rq["rating"].asString(), 24) + llformat(" \xC2\xB7 asks for %d characters at once", rq["npcBudgetAsked"].asInteger());
            const bool open = !mApproveAdv.empty() && mApproveAdv == rq["adventure"].asString() && mApproveRegion == rq["region"].asString();
            const F32 ch = 10.f + slh * 2.f + 8.f + 28.f + (open ? 70.f : 0.f) + 10.f;
            card(l, y - ch, r, y, p, 6.f);
            F32 cy = y - 10.f;
            textL(bold, title, l + 12.f, cy, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            cy -= slh;
            textL(small, words, l + 12.f, cy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 24.f);
            cy -= slh + 8.f;
            if (!open)
            {
                btn(l + 12.f, cy - 26.f, l + 132.f, cy, "Approve...", SH_APPROVE, arg, "Choose what it may do here, then approve", p, idle);
                btn(r - 132.f, cy - 26.f, r - 12.f, cy, "Refuse", SH_REFUSE, arg, "Refuse it (asks first)", p, idle, BTN_DANGER);
            }
            else
            {
                // enable_region options (§2.1: may change the environment, NPC budget)
                btn(l + 12.f, cy - 26.f, l + 300.f, cy, mApproveEnv ? "May change the sky and weather: Yes" : "May change the sky and weather: No",
                    SH_APPROVE_ENV, arg, "Whether its scenes may change this region's environment", p, true, mApproveEnv ? BTN_ACTIVE : 0);
                cy -= 34.f;
                textL(small, llformat("Story characters at once: %d", mApproveBudget), l + 12.f, cy - 13.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
                const F32 sx = l + 210.f;
                btn(sx, cy - 26.f, sx + 32.f, cy, "-", SH_APPROVE_LESS, arg, "Fewer", p, mApproveBudget > 0);
                btn(sx + 38.f, cy - 26.f, sx + 70.f, cy, "+", SH_APPROVE_MORE, arg, "More", p, mApproveBudget < NPC_BUDGET_MAX);
                cy -= 34.f;
                btn(l + 12.f, cy - 26.f, l + 132.f, cy, "Approve", SH_APPROVE_OK, arg, "Approve it now", p, idle);
                btn(l + 140.f, cy - 26.f, l + 260.f, cy, "Cancel", SH_APPROVE_CANCEL, arg, "Not now", p);
            }
            y -= ch + 8.f;
        }
    }
    return y;
}

F32 WolfAdventureStudioView::pageImport(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const bool idle = mBusy == 0;
    F32 y = head(l, r, top, "Import", p);
    y = para(l, r, y, mOpenId.empty() ? "Nothing is saved until Use as draft; with no adventure open, that makes a new one."
                                      : "Use as draft replaces the draft of \"" + mOpenTitle + "\" (asks first).", p.mDim, true) - 8.f;
    {
        const F32 bw = 180.f;
        btn(l, y - 28.f, l + bw, y, "Wolf Story (STORY v1)", SH_KIND, 0, "Import the storyteller's notecards", p, true, !mImportPack ? BTN_ACTIVE : 0);
        btn(l + bw + 8.f, y - 28.f, l + 2.f * bw + 8.f, y, "Pack JSON", SH_KIND, 1, "Load a hand-written pack (.json)", p, true, mImportPack ? BTN_ACTIVE : 0);
        y -= 40.f;
    }
    if (!mImportPack)
    {
        // the drop zone (STUDIO §6 2.5.7: dropping is not the only way - the notecards may also be
        // dropped one at a time; there is no drag movement inside the window)
        const F32 zh = 90.f;
        chamfer(l, y - zh, r, y, 8.f, LLColor4(0.f, 0.f, 0.f, 0.3f), LLColor4(0.f, 0.f, 0.f, 0.3f));
        chamferEdge(l + 0.5f, y - zh + 0.5f, r - 0.5f, y - 0.5f, 8.f, now() < mDragOverUntil ? 2.f : 1.f, now() < mDragOverUntil ? p.mAccent : p.mDim);
        F32 zy = y - 12.f;
        textL(bold, mStoryName.empty() ? std::string("Drop the STORY notecard here") : "Story: " + mStoryName, l + 14.f, zy, mStoryName.empty() ? p.mText : OK_INK,
              LLFontGL::LEFT, LLFontGL::TOP, r - l - 28.f);
        zy -= slh + 4.f;
        const S32 looks = mLooks.isMap() ? (S32)mLooks.size() : 0;
        textL(small, looks ? plural(looks, "look notecard", "look notecards") + " added (\"Look - <name>\")"
                           : std::string("...and the \"Look - <name>\" notecards for the characters' looks"),
              l + 14.f, zy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 28.f);
        zy -= slh + 4.f;
        textL(small, "From your own inventory. A look needs full permissions for the viewer to see it.", l + 14.f, zy, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l - 28.f);
        hit(l, y - zh, r, y, SH_DROP, 0, "Drag notecards from your inventory and drop them here", false);
        y -= zh + 10.f;
        // STUDIO §6 2.5.7: the drop has a picker as well - a list of your notecards
        btn(l, y - 26.f, l + 220.f, y, mPickOpen ? "Hide my notecards" : "Choose from my notecards...", SH_PICK_TOGGLE, 0,
            "Pick the STORY and Look notecards from a list instead of dragging", p);
        y -= 34.f;
        if (mPickOpen)
        {
            field(l, y - 28.f, r, y, mPickFilter, "Type part of a name to narrow the list", F_FILTER, p);
            y -= 34.f;
            mPickShown.clear();
            std::string want = mPickFilter;
            LLStringUtil::toLower(want);
            for (const auto& it : mPickItems)
            {
                if (mPickShown.size() >= 30) break;
                std::string n = it.second;
                LLStringUtil::toLower(n);
                if (!want.empty() && n.find(want) == std::string::npos) continue;
                const S32 arg = (S32)mPickShown.size();
                mPickShown.push_back(it.first);
                textL(small, it.second, l + 4.f, y - 13.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 110.f);
                btn(r - 96.f, y - 24.f, r, y, "Use", SH_PICK_USE, arg, "Use \"" + it.second + "\"", p);
                y -= 28.f;
            }
            if (mPickShown.empty()) y = para(l, r, y, mPickItems.empty() ? "No notecards in your inventory." : "No notecard matches.", p.mDim, true) - 4.f;
            y -= 8.f;
        }
        // the region (§2.10 "pick the region"): the one you stand on, or one of the open adventure's
        textL(bold, caps("Region"), l, y, p.mDim);
        y -= slh + 4.f;
        {
            F32 x = l;
            std::vector<std::pair<std::string, std::string>> choices;
            if (LLViewerRegion* rgn = gAgent.getRegion()) choices.push_back({ rgn->getRegionID().asString(), rgn->getName() + " (here)" });
            if (const LLSD* a = listed(mOpenId))
            {
                const LLSD& rs = (*a)["regions"];
                for (size_t k = 0; rs.isArray() && k < (size_t)rs.size(); ++k)
                {
                    const std::string id = rs[(LLSD::Integer)k]["region"].asString();
                    bool dup = false;
                    for (const auto& c : choices) dup = dup || c.first == id;
                    if (!dup) choices.push_back({ id, cap(rs[(LLSD::Integer)k]["name"].asString(), 64) });
                }
            }
            for (size_t k = 0; k < choices.size() && k < 12; ++k)
            {
                const F32 w = bold->getWidthF32(choices[k].second) + 22.f;
                if (x > l && x + w > r) { x = l; y -= 30.f; }
                const bool on = mImportRegion == choices[k].first;
                btn(x, y - 24.f, x + w, y, choices[k].second, SH_IMPORT_REGION, (S32)k, "The story's places are on this region" + std::string(on ? " (chosen)" : ""),
                    p, true, on ? BTN_ACTIVE : 0);
                x += w + 6.f;
            }
            y -= 32.f;
        }
        btn(l, y - 30.f, l + 140.f, y, "Import", SH_IMPORT_RUN, 0, "Send the story to the grid to turn into a pack", p, idle && !mStoryText.empty());
        y -= 44.f;
    }
    else
    {
        btn(l, y - 30.f, l + 200.f, y, "Choose a .json file...", SH_PACK_FILE, 0, "Pick a pack file on this computer (at most 2 MiB)", p, idle);
        if (!mPackFile.empty()) textL(small, mPackFile, l + 212.f, y - 15.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 212.f);
        y -= 44.f;
    }
    // the result: the report, an outline, then Use as draft / Cancel (§2.10)
    if (!mImportedJson.empty() || (mReport.isArray() && mReport.size() > 0))
    {
        y = head(l, r, y, "Result", p);
        for (size_t i = 0; mReport.isArray() && i < (size_t)mReport.size() && i < 60; ++i)
        {
            y = para(l, r, y, cap(mReport[(LLSD::Integer)i].asString(), 500), p.mText, true) - 2.f;
        }
        if (!mImportedJson.empty())
        {
            const LLSD& pk = mImportedPack;
            S32 scenes = 0, beats = 0;
            const LLSD& acts = pk["acts"];
            for (size_t a = 0; acts.isArray() && a < (size_t)acts.size(); ++a)
            {
                const LLSD& sc = acts[(LLSD::Integer)a]["scenes"];
                for (size_t s = 0; sc.isArray() && s < (size_t)sc.size(); ++s)
                {
                    ++scenes;
                    beats += (S32)sc[(LLSD::Integer)s]["beats"].size();
                }
            }
            const std::string outline = "\"" + cap(pk["adventure"]["title"].asString(), TITLE_MAX) + "\": "
                + plural(acts.isArray() ? (S32)acts.size() : 0, "act", "acts") + ", " + plural(scenes, "scene", "scenes") + ", "
                + plural(beats, "beat", "beats") + ", " + plural(pk["cast"].isArray() ? (S32)pk["cast"].size() : 0, "character", "characters") + ", "
                + plural(pk["endings"].isArray() ? (S32)pk["endings"].size() : 0, "ending", "endings") + ".";
            y = para(l, r, y - 4.f, outline, p.mText) - 8.f;
            btn(l, y - 30.f, l + 160.f, y, "Use as draft", SH_USE_DRAFT, 0,
                mOpenId.empty() ? std::string("Make a new adventure from it") : "Replace the draft of " + mOpenTitle + " (asks first)", p, idle);
            btn(l + 168.f, y - 30.f, l + 288.f, y, "Cancel", SH_IMPORT_CANCEL, 0, "Throw this result away", p);
            y -= 44.f;
        }
    }
    return y;
}

F32 WolfAdventureStudioView::pagePublish(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const bool idle = mBusy == 0;
    const LLSD* a = listed(mOpenId);
    const bool owner = !a || (*a)["role"].asString().empty();
    F32 y = top;
    if (!owner)
    {
        y = para(l, r, y, "You hold a role in this adventure; only its owner can publish it, change its regions or its roles.", p.mDim) - 8.f;
    }
    // ---- Checks ----
    y = head(l, r - 120.f, y, "Checks", p);
    btn(r - 110.f, y + slh + 10.f - 24.f, r, y + slh + 10.f, "Check again", SH_CHECK, 0, "Ask the grid to check the draft", p, idle && owner);
    const S32 errors = countProblems(mProblems, "error"), warnings = countProblems(mProblems, "warning");
    if (owner)
    {
        y = para(l, r, y, errors == 0 ? (warnings ? plural(warnings, "warning", "warnings") + "; nothing stops publishing. They are listed on the right."
                                                  : std::string("No problems."))
                                      : plural(errors, "error", "errors") + " to fix before publishing; they are listed on the right.",
                 errors ? DANGER_INK : p.mText) - 8.f;
    }
    // ---- Regions (§2.11) ----
    y = head(l, r, y, "Regions", p);
    std::vector<std::string> requested;
    if (a)
    {
        const LLSD& rs = (*a)["regions"];
        for (size_t k = 0; rs.isArray() && k < (size_t)rs.size(); ++k)
        {
            const LLSD& rg = rs[(LLSD::Integer)k];
            requested.push_back(rg["region"].asString());
            textL(small, cap(rg["name"].asString(), 64), l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, (r - l) * 0.6f);
            const std::string st = rg["state"].asString();
            textL(bold, stateWord(st), r, y, st == "enabled" ? OK_INK : (st == "refused" ? DANGER_INK : p.mText), LLFontGL::RIGHT, LLFontGL::TOP);
            y -= slh + 3.f;
        }
    }
    // the pack's regions not asked for yet, with their NPC budget asked
    const LLSD& pack_regions = std::as_const(mDraftPack)["adventure"]["regions"];
    for (size_t k = 0; pack_regions.isArray() && k < (size_t)pack_regions.size(); ++k)
    {
        const LLSD& pr = pack_regions[(LLSD::Integer)k];
        const std::string id = pr["region"].asString();
        if (std::find(requested.begin(), requested.end(), id) != requested.end()) continue;
        const std::string name = cap(pr["name"].asString(), 64);
        textL(small, (name.empty() ? id : name) + llformat(" - not asked yet, %d characters at once", pr["npcBudget"].asInteger()), l, y - 4.f, p.mText,
              LLFontGL::LEFT, LLFontGL::TOP, r - l - 110.f);
        btn(r - 100.f, y - 24.f, r, y, "Request", SH_REQUEST, (S32)k, "Ask the region's holder (yours: enabled at once)", p, idle && owner);
        y -= 30.f;
    }
    if (LLViewerRegion* rgn = gAgent.getRegion())
    {
        const std::string here = rgn->getRegionID().asString();
        if (std::find(requested.begin(), requested.end(), here) == requested.end())
        {
            btn(l, y - 26.f, l + 260.f, y, "Request this region (" + rgn->getName() + ")", SH_REQUEST_HERE, 0,
                "Ask to run on the region you stand on", p, idle && owner);
            y -= 34.f;
        }
    }
    y -= 6.f;
    // ---- Roles (§2.11 grant or revoke GM and teacher, with an expiry) ----
    y = head(l, r, y, "Game masters and teachers", p);
    if (!mGrants.isArray() || mGrants.size() == 0) y = para(l, r, y, "Nobody yet.", p.mDim, true) - 4.f;
    for (size_t k = 0; mGrants.isArray() && k < (size_t)mGrants.size(); ++k)
    {
        const LLSD& g = mGrants[(LLSD::Integer)k];
        std::string line = cap(g["name"].asString(), 64) + " - " + (g["role"].asString() == "teacher" ? "teacher" : "game master");
        if (!g["region"].asString().empty()) line += " (one region)";
        const std::string until = whenText(g["expires"]);
        if (!until.empty()) line += " - until " + until;
        textL(small, line, l, y - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 110.f);
        btn(r - 100.f, y - 24.f, r, y, "Revoke", SH_REVOKE, (S32)k, "Take this role away (asks first)", p, idle && owner, BTN_DANGER);
        y -= 30.f;
    }
    if (owner)
    {
        textL(bold, caps("New roles last"), l, y, p.mDim);
        y -= slh + 4.f;
        F32 x = l;
        for (S32 i = 0; i < EXPIRY_COUNT; ++i)
        {
            const F32 w = bold->getWidthF32(EXPIRIES[i].mLabel) + 22.f;
            const bool on = mGrantExpiry == i;
            btn(x, y - 24.f, x + w, y, EXPIRIES[i].mLabel, SH_EXPIRY, i, std::string("Roles given now last: ") + EXPIRIES[i].mLabel, p, true, on ? BTN_ACTIVE : 0);
            x += w + 6.f;
        }
        y -= 32.f;
        btn(l, y - 28.f, l + 190.f, y, "Add a game master...", SH_GRANT, 0, "Pick a resident to run this adventure as its game master", p, idle);
        btn(l + 198.f, y - 28.f, l + 360.f, y, "Add a teacher...", SH_GRANT, 1, "Pick a resident to run classes with it", p, idle);
        y -= 40.f;
    }
    // ---- [M7] change the story while people play ----
    if (owner && a && (*a)["status"].asString() == "published") y = liveUpdateSection(l, r, y, p, errors);
    // ---- History (§2.11) ----
    if (mRevisions.isArray() && mRevisions.size() > 0)
    {
        y = head(l, r, y, "History", p);
        for (size_t k = 0; k < (size_t)mRevisions.size() && k < 30; ++k)
        {
            const LLSD& rv = mRevisions[(LLSD::Integer)k];
            const std::string line = llformat("Revision %d", rv["revision"].asInteger()) + " - " + whenText(rv["at"]);
            textL(small, line, l, y - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 310.f);
            // "Open (read-only)" is a download in Studio Lite (no editor to open it in; as WolfStorm)
            btn(r - 300.f, y - 24.f, r - 196.f, y, "Download", SH_DOWNLOAD, rv["revision"].asInteger(), "Save this revision as a pack file (read-only copy)", p, idle);
            btn(r - 190.f, y - 24.f, r, y, "Republish this revision", SH_REPUBLISH, rv["revision"].asInteger(), "It becomes the next revision (asks first)", p, idle && owner);
            y -= 30.f;
        }
        y -= 6.f;
    }
    // ---- Publish / Unpublish ----
    if (owner)
    {
        y = head(l, r, y, "Publish", p);
        const bool can = idle && errors == 0 && !mEtag.empty();
        btn(l, y - 32.f, l + 160.f, y, "Publish...", SH_PUBLISH, 0,
            errors ? "Fix the errors first (listed on the right)" : std::string("Make the draft live (asks first)"), p, can);
        if (errors) textL(small, plural(errors, "error stops", "errors stop") + " publishing", l, y - 40.f, DANGER_INK, LLFontGL::LEFT, LLFontGL::TOP);
        if (errors) y -= slh + 4.f;
        btn(l + 168.f, y - 32.f, l + 368.f, y, "Download the draft (.json)", SH_DOWNLOAD, -1,
            "Save the draft as a pack file, to fix by hand and import again", p, !mDraftJson.empty());
        const bool published = a && (*a)["status"].asString() == "published";
        if (published) btn(r - 140.f, y - 32.f, r, y, "Unpublish...", SH_UNPUBLISH, 0, "Take it off every region (asks first)", p, idle, BTN_DANGER);
        y -= 44.f;
    }
    return y;
}

// [WOLF STORY FIRST FRONT DOOR 2026-10-17] ═══════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::fieldMulti(F32 l, F32 b, F32 r, F32 t, const std::string& text, const std::string& placeholder, EField which,
                                         const Palette& p)
{
    const bool focused = hasFocus() && mField == which;
    chamfer(l, b, r, t, 6.f, LLColor4(0.f, 0.f, 0.f, 0.45f), LLColor4(0.f, 0.f, 0.f, 0.55f));
    chamferEdge(l + 0.5f, b + 0.5f, r - 0.5f, t - 0.5f, 6.f, focused ? 2.f : 1.f, focused ? p.mAccent : p.mDim);
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const F32 lh = (F32)f->getLineHeight();
    const S32 fit = llmax(1, (S32)((t - b - 16.f) / lh));
    F32 y = t - 8.f;
    if (text.empty())
    {
        for (const std::string& ln : wrap(f, placeholder, r - l - 20.f, fit)) { textL(f, ln, l + 10.f, y, p.mDim); y -= lh; }
        if (focused && fmod(now(), 1.0) < 0.6) rect(l + 10.f, t - 8.f - lh * 0.9f, l + 11.f, t - 8.f, p.mText);
    }
    else
    {
        // the end of the text is where typing goes: its last lines show. Only the tail is wrapped (wrap() keeps 2048
        // characters, wolfgamehud.cpp; the box holds 20,000): the last 1,200 characters, cut on a character boundary
        const LLWString all = utf8str_to_wstring(text);
        const size_t TAIL = 1200;
        const std::string tail = all.size() > TAIL ? wstring_to_utf8str(all.substr(all.size() - TAIL)) : text;
        const std::vector<std::string> lines = wrap(f, tail, r - l - 20.f, 0);
        const size_t first = lines.size() > (size_t)fit ? lines.size() - (size_t)fit : 0;
        F32 x = l + 10.f;
        for (size_t i = first; i < lines.size(); ++i) { x = textL(f, lines[i], l + 10.f, y, p.mText); y -= lh; }
        if (first > 0 || all.size() > TAIL)
            textL(LLFontGL::getFontSansSerifSmall(), llformat("%d characters - the end shows", (S32)all.size()), r - 10.f, b + 4.f, p.mDim, LLFontGL::RIGHT, LLFontGL::BOTTOM);
        if (focused && fmod(now(), 1.0) < 0.6) rect(x + 1.f, y + lh * 0.1f, x + 2.f, y + lh, p.mText);
    }
    hit(l, b, r, t, SH_FIELD, (S32)which, which == F_HELPER && mOpenId.empty() ? std::string("Your idea or your whole story (Enter builds it; Ctrl+V pastes; up to 20,000 characters)")
                                                                                : std::string("Tell the helper what happens, or ask it anything (Enter sends; Ctrl+V pastes)"));
}

F32 WolfAdventureStudioView::frontDoor(F32 l, F32 r, F32 top, const Palette& p)
{
    // Persona: a resident with a story idea opens the Studio and is talking to the helper within 10 seconds.
    // Flow: type or paste -> Build it (the first line makes the game and goes to the helper) -> the main screen. One primary
    // action (Build it); the games to continue below it; the no-AI way quiet and last.
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    const F32 colW = llmin(780.f, r - l), cl = l + ((r - l) - colW) * 0.5f, cr = cl + colW;
    F32 y = top - 18.f;
    textL(display(28.f), "Make a game from a story", (cl + cr) * 0.5f, y, p.mAccent, LLFontGL::HCENTER, LLFontGL::TOP, colW);
    y -= (F32)display(28.f)->getLineHeight() + 14.f;
    // the helper speaks first (the greeting; the first time, the Guide beside the Studio says the same)
    {
        const std::vector<std::string> gl = wrap(LLFontGL::getFontSansSerif(), WolfAdvStudio::greeting(), colW - 90.f, 4);
        const F32 gh = 16.f + slh + (F32)gl.size() * (F32)LLFontGL::getFontSansSerif()->getLineHeight();
        card(cl, y - gh, cr, y, p, 10.f);
        crescent(cl + 24.f, y - 22.f, 10.f, p.mAccent, p.mBaseTop);
        textL(bold, "YOUR HELPER", cl + 44.f, y - 8.f, p.mAccent);
        F32 gy = y - 8.f - slh;
        for (const std::string& ln : gl) { textL(LLFontGL::getFontSansSerif(), ln, cl + 44.f, gy, p.mText); gy -= (F32)LLFontGL::getFontSansSerif()->getLineHeight(); }
        y -= gh + 12.f;
    }
    if (mHelperMissing)
        y = para(cl, cr, y, "The helper is not on this grid yet. You can still make a game box by box, with no AI.", WARN_INK, true) - 6.f;
    // the story box: large, ready for a line or a whole pasted story
    fieldMulti(cl, y - 150.f, cr, y, mHelperBox, "Your idea in one line, or paste your whole story...\n\nExample: \"A Wild West bank robbery where the players "
                                                 "can side with the outlaws or the sheriff.\"", F_HELPER, p);
    y -= 160.f;
    if (!mHelperError.empty()) y = para(cl, cr, y, mHelperError, DANGER_INK, true) - 6.f;
    {
        F32 x = cl;
        const std::string go = mHelperBusy ? "Making your game..." : "Build it";
        const F32 gw = llmax(180.f, WolfAdvStudio::btnW(go) + 30.f);
        const S32 ig = act([this]() { helperSend(); });
        btn(x, y - 44.f, x + gw, y, go, SH_E, ig, "Make the game from your idea or story: characters, choices and endings, then we talk it through (Enter)",
            p, !mHelperBusy && !mHelperMissing && !mHelperBox.empty(), BTN_PRIMARY);
        x += gw + 10.f;
        const S32 ib = act([this]()
        {
            std::string t = mHelperBox;
            LLStringUtil::trim(t);
            helperCreate(t.empty() ? std::string("My adventure") : t, true);
        });
        btn(x, y - 44.f, x + WolfAdvStudio::btnW("Load a book") + 20.f, y, "Load a book", SH_E, ib,
            "Read a whole book in and build from it (a quote first). The box above is its title.", p, !mHelperBusy && !mHelperMissing);
        // the no-AI way, quiet and furthest from the primary (Fitts: distance between unlike actions)
        const std::string na = "Make it box by box (no AI)";
        const S32 iw = act([this]() { openWizardStep(W_KIND); });
        btn(cr - WolfAdvStudio::btnW(na) - 10.f, y - 44.f, cr, y, na, SH_E, iw, "The step-by-step pages, with no AI", p);
        y -= 56.f;
    }
    // the games to continue, as cards
    std::vector<S32> mine;
    for (S32 i = 0; mList.isArray() && i < (S32)mList.size(); ++i) mine.push_back(i);
    if (!mListLoaded) y = para(cl, cr, y, "Loading your games...", p.mDim, true) - 6.f;
    else if (!mine.empty())
    {
        y = head(cl, cr, y, "Continue a game", p);
        const S32 per = colW >= 700.f ? 3 : (colW >= 460.f ? 2 : 1);
        const F32 gap = 10.f, cw = (colW - gap * (F32)(per - 1)) / (F32)per, ch = 78.f;
        for (size_t k = 0; k < mine.size() && k < 12; ++k)
        {
            const LLSD& a = std::as_const(mList)[mine[k]];
            const F32 x0 = cl + (F32)(k % per) * (cw + gap);
            const F32 y0 = y - (F32)(k / per) * (ch + gap);
            const std::string id = a["id"].asString(), title = cap(a["title"].asString(), TITLE_MAX);
            std::string words = statusWord(a["status"].asString(), a["revision"].asInteger());
            const std::string when = whenText(a["updated"]);
            if (!when.empty()) words += " \xC2\xB7 " + when;
            if (!a["role"].asString().empty()) words += a["role"].asString() == "teacher" ? " \xC2\xB7 teacher" : " \xC2\xB7 game master";
            const S32 io = act([this, id]() { openAdventure(id); });
            btn(x0, y0 - ch, x0 + cw, y0, std::string(), SH_E, io, "Continue " + title, p);
            textL(display(17.f), title, x0 + 12.f, y0 - 10.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, cw - 24.f);
            textL(small, words, x0 + 12.f, y0 - 14.f - (F32)display(17.f)->getLineHeight(), p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cw - 24.f);
            textL(bold, "Continue \xE2\x96\xB8", x0 + 12.f, y0 - ch + 10.f, p.mAccent, LLFontGL::LEFT, LLFontGL::BOTTOM);
        }
        y -= (F32)((llmin((S32)mine.size(), 12) + per - 1) / per) * (ch + gap) + 4.f;
        if (mine.size() > 12) y = para(cl, cr, y, llformat("%d more under Advanced editing > Adventures.", (S32)mine.size() - 12), p.mDim, true) - 4.f;
    }
    // requests to run adventures on the author's regions wait on the Adventures page (Advanced editing): said here
    if (mRequests.isArray() && mRequests.size() > 0)
    {
        const S32 n = (S32)mRequests.size();
        const std::string lab = llformat("%d request%s to run adventures on your regions", n, n == 1 ? "" : "s");
        const S32 ir = act([this]() { mPage = SP_HOME; mScroll[SP_HOME] = 0.f; loadList(); loadRequests(); });
        btn(cl, y - 30.f, cl + WolfAdvStudio::btnW(lab + " \xE2\x96\xB8") + 10.f, y, lab + " \xE2\x96\xB8", SH_E, ir, "Allow or refuse them on the Adventures page", p, true, BTN_ACTIVE);
        y -= 38.f;
    }
    // the expert way in, last
    {
        const bool adv = advancedOpen();
        const std::string lab = adv ? "Advanced editing \xE2\x96\xBE" : "Advanced editing \xE2\x96\xB8";
        const S32 ia = act([this, adv]() { setAdvancedOpen(!adv); });
        btn(cl, y - 30.f, cl + WolfAdvStudio::btnW(lab) + 10.f, y, lab, SH_E, ia, "The expert pages (import a pack, outline, rules and more) in the rail", p, true, adv ? BTN_ACTIVE : 0);
        y -= 38.f;
    }
    return y;
}

// [WOLF STORY FIRST M7 2026-10-17] Source: wolfadventure_live_lib.php wa_api_update_preview / update_live / update_undo.
F32 WolfAdventureStudioView::liveUpdateSection(F32 l, F32 r, F32 top, const Palette& p, S32 errors)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const bool idle = mBusy == 0 && !mLiveBusy;
    F32 y = head(l, r, top, "Change the live game", p);
    const LLSD& pv = std::as_const(mLivePreview);
    if (!pv.isMap())
    {
        y = para(l, r, y, "Players keep playing while you change the story: each party moves to the new version at its next box. "
                          "First you see who is where and what happens to them.", p.mDim, true) - 4.f;
        const bool can = idle && errors == 0 && !mEtag.empty() && mDirtyAt <= 0.0 && !mSaving;
        const S32 iu = act([this]() { livePreview(false); });
        btn(l, y - 30.f, l + WolfAdvStudio::btnW("Update the live game..."), y, "Update the live game...", SH_E, iu,
            errors ? "Fix the errors first (listed on the right)" : std::string("See what the update does to the players, then update"), p, can, BTN_PRIMARY);
        const F32 x = l + WolfAdvStudio::btnW("Update the live game...") + 6.f;
        const S32 ib = act([this]() { livePreview(true); });
        btn(x, y - 30.f, x + WolfAdvStudio::btnW("Undo the last update..."), y, "Undo the last update...", SH_E, ib, "Put back the version that was live before the last update (see first)", p, idle);
        return y - 40.f;
    }
    // the preview
    y = para(l, r, y, mLiveUndo ? llformat("Going back to version %d:", pv["undo"]["restores"].asInteger()) : std::string("What the update does:"), p.mText, true) - 2.f;
    for (const LLSD& s : llsd::inArray(pv["say"])) y = para(l + 8.f, r, y, cap(s.asString(), 400), p.mText, true) - 2.f;
    y = para(l, r, y, llformat("%d player%s in %d part%s now; %d in a fight (it finishes first); %d finished.", pv["players"].asInteger(), pv["players"].asInteger() == 1 ? "" : "s",
                               pv["parties"].asInteger(), pv["parties"].asInteger() == 1 ? "y" : "ies", pv["battles"].asInteger(), pv["finished"].asInteger()), p.mDim, true) - 4.f;
    // removed boxes: where their players go (the brain's proposal, the author may change it while updating)
    for (const LLSD& b : llsd::inArray(pv["boxes"]))
    {
        if (b["fate"].asString() != "removed") continue;
        const std::string id = b["id"].asString();
        const auto mv = mLiveMoves.find(id);
        std::string toTitle = b["goesToTitle"].asString();
        if (mv != mLiveMoves.end())
            for (const LLSD& n : llsd::inArray(std::as_const(mDraftPack)["acts"]))
                for (const LLSD& sc : llsd::inArray(n["scenes"])) for (const LLSD& bt : llsd::inArray(sc["beats"]))
                    if (bt["id"].asString() == mv->second) toTitle = bt["title"].asString().empty() ? bt["objective"].asString() : bt["title"].asString();
        const std::string line = "\"" + cap(b["title"].asString(), 50) + "\" is removed" + (b["players"].asInteger() ? llformat(" (%d player%s there)", b["players"].asInteger(), b["players"].asInteger() == 1 ? "" : "s") : std::string())
                               + ": they go to \"" + cap(toTitle, 50) + "\"" + (b["by"].asString() == "helper" && mv == mLiveMoves.end() ? " (the helper's idea)" : "");
        const F32 cw = mLiveUndo ? 0.f : WolfAdvStudio::btnW("Change...");
        textL(small, line, l + 8.f, y - 4.f, b["players"].asInteger() ? WARN_INK : p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - cw - 16.f);
        if (!mLiveUndo)
        {
            const S32 ic = act([this, id]()
            {
                flowLayout();   // the draft's boxes: where the update can send them
                std::vector<std::pair<std::string, std::string>> to;
                for (const FlowNode& n : mFlowNodes) if (!n.mEnding) to.push_back({ n.mId, cap(n.mTitle.empty() ? n.mId : n.mTitle, 70) });
                openPicker("Where do its players go?", to, [this, id](const std::string& v) { if (!v.empty()) mLiveMoves[id] = v; }, "The new version has no boxes.");
            });
            btn(r - cw, y - 24.f, r, y, "Change...", SH_E, ic, "Choose the box its players go to", p, idle);
        }
        y -= 28.f;
        if (!b["line"].asString().empty() && mv == mLiveMoves.end())
            y = para(l + 16.f, r, y, "They are told: \"" + cap(b["line"].asString(), 240) + "\"", p.mDim, true) - 2.f;
    }
    std::string added;
    for (const LLSD& b : llsd::inArray(pv["added"])) added += (added.empty() ? "" : ", ") + cap(b["title"].asString(), 40);
    if (!added.empty()) y = para(l, r, y, "New boxes: " + added, p.mDim, true) - 2.f;
    if (!pv["helperText"].asString().empty()) y = para(l, r, y, "Helper: " + cap(pv["helperText"].asString(), 400), p.mDim, true) - 2.f;
    y -= 4.f;
    F32 x = l;
    const std::string go = mLiveUndo ? "Undo now" : "Update now";
    const S32 ig = act([this]() { liveApply(); });
    btn(x, y - 30.f, x + WolfAdvStudio::btnW(go), y, mLiveBusy ? "Working..." : go, SH_E, ig, mLiveUndo ? "Go back to the earlier version now" : "Make the draft the live version now", p, idle, BTN_PRIMARY);
    x += WolfAdvStudio::btnW(go) + 6.f;
    const S32 ix = act([this]() { mLivePreview = LLSD(); mLiveMoves.clear(); });
    btn(x, y - 30.f, x + WolfAdvStudio::btnW("Cancel"), y, "Cancel", SH_E, ix, "Change nothing", p, !mLiveBusy);
    return y - 40.f;
}

void WolfAdventureStudioView::livePreview(bool undo, const std::string& why)
{
    if (mOpenId.empty() || mLiveBusy) return;
    LLSD body;
    body["adventure"] = mOpenId;
    if (undo) body["undo"] = 1;
    mLiveBusy = true;
    setStatus(why.empty() ? std::string("Looking at who is playing...") : why, false);
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // update_preview {adventure, undo?} -> {live, players, parties, battles, finished, boxes, added, helper, helperText?, say, etag | undo}
    WolfAdventure::api("update_preview", body, [handle, adv, undo, why](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mLiveBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok) { self->setStatus(error, true); return; }
        self->mLivePreview = reply;
        self->mLiveUndo = undo;
        self->mLiveMoves.clear();
        self->setStatus(why.empty() ? std::string("Here is what it would do - nothing has changed yet.") : why, !why.empty());
    });
}

void WolfAdventureStudioView::liveApply()
{
    const LLSD pv = mLivePreview;
    if (mOpenId.empty() || mLiveBusy || !pv.isMap()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["live"] = pv["live"];                  // the version the preview showed (refused with 409 if it changed since)
    if (!mLiveUndo) body["etag"] = pv["etag"];  // the draft the preview showed
    else body["restores"] = pv["undo"]["restores"];   // the version the undo preview showed going back to
    // where the players of each removed box go: the author's pick, else the preview's proposal (with its line)
    LLSD moves = LLSD::emptyMap();
    for (const LLSD& b : llsd::inArray(pv["boxes"]))
    {
        if (b["fate"].asString() != "removed") continue;
        const std::string id = b["id"].asString();
        const auto mv = mLiveMoves.find(id);
        LLSD m;
        if (mv != mLiveMoves.end()) m["to"] = mv->second;   // the brain writes its own line for the author's box
        else if (!b["goesTo"].asString().empty()) { m["to"] = b["goesTo"]; if (!b["line"].asString().empty()) m["line"] = b["line"]; }
        else continue;
        moves[id] = m;
    }
    body["moves"] = moves;
    const bool undo = mLiveUndo;
    mLiveBusy = true;
    setStatus(undo ? "Going back..." : "Updating the live game...", false);
    const std::string adv = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // update_live {adventure, etag, live, moves} / update_undo {adventure, live, moves} -> {revision, ..., players, moves}
    WolfAdventure::api(undo ? "update_undo" : "update_live", body, [handle, adv, undo](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mLiveBusy = false;
        if (self->mOpenId != adv) return;
        if (!ok)
        {
            // the draft or the live game changed since the preview (409, with the draft's etag): look again
            if (reply.isMap() && reply.has("etag")) { self->loadOpen(); self->livePreview(undo, "Your draft changed since the preview - here it is again: " + error); return; }
            if (reply.isMap() && reply.has("live")) { self->livePreview(undo, "The live game changed since the preview - here it is again."); return; }   // 409 {live, restores?}
            self->setStatus(error, true);
            return;
        }
        self->mLivePreview = LLSD();
        self->mLiveMoves.clear();
        const S32 players = reply["players"].asInteger();
        self->setStatus(llformat("%s: version %d is live. ", undo ? "Went back" : "Updated", reply["revision"].asInteger())
                        + (players ? llformat("%d player%s move%s to it at their next box.", players, players == 1 ? "" : "s", players == 1 ? "s" : "") : std::string("Nobody was playing.")), false);
        self->loadList();
        self->loadOpen();
    });
}

void WolfAdventureStudioView::drawConfirm(const Palette& p)
{
    // Everything under it stops taking clicks: only the question counts (the Roleplay window's way).
    mHits.clear();
    const F32 W = (F32)getRect().getWidth(), H = (F32)getRect().getHeight();
    rect(0.f, 0.f, W, H, LLColor4(0.f, 0.f, 0.f, 0.55f));
    std::string title, body, yes, no = "Cancel";
    bool danger = false, yes_enabled = true;
    switch (mConfirm)
    {
    case SC_DELETE:
        title = "Delete \"" + mDeleteTitle + "\"?";
        body = "It cannot be undone. Type the title exactly to confirm.";
        yes = "Delete";
        danger = true;
        yes_enabled = mDeleteTyped == mDeleteTitle;
        break;
    case SC_PUBLISH:
    {
        const LLSD* a = listed(mOpenId);
        const S32 next = (a ? (*a)["revision"].asInteger() : 0) + 1;
        // Source: STUDIO_AND_CONSOLE_V1.md §2.11
        title = llformat("Publish revision %d?", next);
        body = "Players in progress move to it at their next beat.";
        yes = "Publish";
        break;
    }
    case SC_UNPUBLISH:
        title = "Unpublish \"" + mOpenTitle + "\"?";
        body = "Every session closes and its characters leave the regions. You can publish it again later.";
        yes = "Unpublish";
        danger = true;
        break;
    case SC_REPUBLISH:
        title = llformat("Republish revision %d?", mConfirmArg);
        body = "It replaces the draft and becomes the next revision. Players in progress move to it at their next beat.";
        yes = "Republish";
        break;
    case SC_REVOKE:
        title = "Take this role away?";
        body = "They can no longer run this adventure in that role.";
        yes = "Revoke";
        danger = true;
        break;
    case SC_REFUSE:
        title = "Refuse this adventure on your region?";
        body = "Its owner sees that you refused it.";
        yes = "Refuse";
        danger = true;
        break;
    case SC_JOB_START:
    {
        // [WOLF ADVENTURE P2 2026-10-16] money: the quote and the most it may spend, before it runs
        const S32 quote = (S32)ceil(mJob["quote"]["credits"].asReal());
        title = "Draft an adventure from \"" + (mBookTitle.empty() ? mBookName : mBookTitle) + "\"?";
        body = llformat("The quote is about %d credits (%s tokens of text). It stops before spending more than %d credits. "
                        "Each part is charged to your WolfCredits as it finishes. Nothing goes live: the result is a draft.",
                        quote, thousands((S64)mJob["quote"]["tokens"].asReal()).c_str(), mBookMax);
        yes = "Start drafting";
        break;
    }
    case SC_JOB_CANCEL:
        title = "Stop drafting?";
        body = "What it has done so far stays paid for; the rest is not done.";
        yes = "Stop";
        danger = true;
        break;
    case SC_TAKE:
        title = "Take control of this session?";
        body = "The AI Director stands back while you run it. Characters still talk unless you switch AI off. Hand it back any time.";
        yes = "Take control";
        break;
    case SC_REPLACE:
        title = "Replace the draft of \"" + mOpenTitle + "\"?";
        body = "The imported pack becomes its draft. What is published now stays live until you publish again.";
        yes = "Replace the draft";
        break;
    default:
        // [WOLF ADVENTURE P3 2026-10-16] the editors' questions (wolffloateradventurestudioedit.cpp)
        if (!editConfirmText(mConfirm, title, body, yes, danger)) return;
        break;
    }
    const F32 cw = llmin(W - 40.f, 440.f);
    const LLFontGL* f = LLFontGL::getFontSansSerif();
    const std::vector<std::string> lines = wrap(f, body, cw - 28.f, 6);
    const F32 field_h = mConfirm == SC_DELETE ? 40.f : 0.f;
    const F32 ch = 16.f + (F32)display(18.f)->getLineHeight() + 6.f + (F32)lines.size() * (F32)f->getLineHeight() + 12.f + field_h + 30.f + 14.f;
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
    y -= 8.f;
    if (mConfirm == SC_DELETE)
    {
        field(cl + 14.f, y - 30.f, cl + cw - 14.f, y, mDeleteTyped, mDeleteTitle, F_DELETE, p);
    }
    // the consequential answer on the left, apart from the safe one on the right (NN/G)
    const F32 bw = 150.f, bb = cb + 14.f;
    button(cl + 14.f, bb, cl + 14.f + bw, bb + 30.f, yes, p, flagsFor(SH_YES, 0, yes_enabled) | (danger ? BTN_DANGER : 0));
    mHits.add(cl + 14.f, bb, cl + 14.f + bw, bb + 30.f, SH_YES, 0, yes, true, yes_enabled);
    button(cl + cw - 14.f - bw, bb, cl + cw - 14.f, bb + 30.f, no, p, flagsFor(SH_NO, 0, true) | BTN_PRIMARY);
    mHits.add(cl + cw - 14.f - bw, bb, cl + cw - 14.f, bb + 30.f, SH_NO, 0, no + " (Escape)");
}

// ---- input ----

void WolfAdventureStudioView::ask(EConfirm what, S32 arg)
{
    if (mField == F_EDIT) commitEdit();   // [WOLF ADVENTURE P3 2026-10-16/]
    mConfirm = what;
    mConfirmArg = arg;
    // the safe answer has the focus (W3C APG alertdialog); the delete question starts in its field
    mField = what == SC_DELETE ? F_DELETE : F_NONE;
    mHits.setFocus(what == SC_DELETE ? (S32)SH_FIELD : (S32)SH_NO, what == SC_DELETE ? (S32)F_DELETE : 0);
    mKeyboardNav = true;
    setFocus(true);
}

void WolfAdventureStudioView::answer(bool yes)
{
    const EConfirm what = mConfirm;
    const S32 arg = mConfirmArg;
    mConfirm = SC_NONE;
    mField = F_NONE;
    if (!yes)
    {
        mDeleteTyped.clear();
        editDeclined(what);   // [WOLF ADVENTURE P3 2026-10-16/]
        return;
    }
    switch (what)
    {
    case SC_DELETE: deleteAdventure(); break;
    case SC_PUBLISH: publish(-1); break;
    case SC_UNPUBLISH: unpublish(); break;
    case SC_REPUBLISH: publish(arg); break;
    case SC_REVOKE: revoke(grantIndex(mConfirmKeyA, mConfirmKeyB, mConfirmKeyC)); break;
    case SC_REFUSE: refuse(requestIndex(mConfirmKeyA, mConfirmKeyB)); break;
    case SC_REPLACE: saveDraftTo(mOpenId, mImportedJson, mOpenTitle); break;
    case SC_JOB_START: bookStart(); break;   // [WOLF ADVENTURE P2 2026-10-16/]
    case SC_JOB_CANCEL:
    {
        LLSD body;
        body["job"] = mJobId;
        call("job_cancel", body, [](WolfAdventureStudioView* self, const LLSD&)
        {
            self->setStatus("Drafting stopped.", false);
            self->pollJob();
        }, "Stopping...");
        break;
    }
    case SC_TAKE: gmOp("take", LLSD::emptyMap(), "Taking control..."); break;
    default: editAnswer(what); break;   // [WOLF ADVENTURE P3 2026-10-16/]
    }
}

void WolfAdventureStudioView::press(S32 id, S32 arg)
{
    // [WOLF ADVENTURE P3 2026-10-16] an editor field is written before anything else happens
    if (id != SH_FIELD && id != SH_E_TEXT)
    {
        if (mField == F_EDIT) commitEdit();
        mField = F_NONE;
    }
    if (editPress(id, arg)) return;
    switch (id)
    {
    case SH_CLOSE:
        if (LLFloater* f = getParentByType<LLFloater>()) f->closeFloater();
        break;
    case SH_GUIDE:   // [WOLF ADVENTURE AUDIT 2026-10-17]
        if (WolfFloaterAdventureStudio* f = dynamic_cast<WolfFloaterAdventureStudio*>(getParentByType<LLFloater>()))
            f->showGuide(!WolfFloaterAdventureStudio::guideShown());
        break;
    case SH_PAGE:
        mPage = (EPage)llclamp(arg, 0, (S32)SP_COUNT - 1);
        if (mPage == SP_HOME) { loadList(); loadRequests(); }
        // [WOLF ADVENTURE P2 2026-10-16] each page loads what it shows (a failed load is asked again
        // when the page is chosen again, never every frame)
        mVoicesAsked = mVoicesLoaded;
        mOwnerAsked = mOwnerLoaded;   // loaded limits (and unsaved edits to them) are kept
        if (mPage == SP_AI) { loadBudget(); loadVoices(); loadOwnerSettings(); }
        if (mPage == SP_CAST) { loadVoices(); loadBudget(); }
        if (mPage == SP_BOOK) loadJobs();
        if (mPage == SP_RUN) loadSessions();
        if (mPage == SP_CASTED) loadVoices();            // [WOLF ADVENTURE P3 2026-10-16]
        if (mPage == SP_RULES) mGameLinksFor.clear();    // the link status read again
        if (mPage == SP_HELPER) { helperHistory(); if (mOpenId.empty()) loadTemplates(); }   // [WOLF STORY FIRST 2026-10-17]
        // [WOLF ADVENTURE AUDIT 2026-10-17] Start here: with nothing open it starts at "What kind"; the starter list loads
        if (mPage == SP_WIZARD)
        {
            if (mOpenId.empty()) mWizStep = W_KIND;
            mWizError.clear();
            loadTemplates();
            if (mWizStep == W_CAST || mWizStep == W_TEST) loadBudget();
        }
        break;
    case SH_REFRESH: loadList(); loadRequests(); break;
    case SH_OPEN:
        if (mList.isArray() && arg >= 0 && arg < (S32)mList.size()) openAdventure(mList[arg]["id"].asString());
        break;
    case SH_DUPLICATE:
        if (mList.isArray() && arg >= 0 && arg < (S32)mList.size()) duplicate(mList[arg]["id"].asString());
        break;
    case SH_DELETE:
        if (mList.isArray() && arg >= 0 && arg < (S32)mList.size())
        {
            mDeleteId = mList[arg]["id"].asString();
            mDeleteTitle = cap(mList[arg]["title"].asString(), TITLE_MAX);
            mDeleteTyped.clear();
            ask(SC_DELETE, arg);
        }
        break;
    case SH_PURPOSE:
        mNewPurpose = llclamp(arg, 0, PURPOSE_COUNT - 1);
        mNewPreset = PURPOSES[mNewPurpose].mPreset;   // §2.1 "the rules preset, defaulted by the purpose"
        break;
    case SH_PRESET: mNewPreset = llclamp(arg, 0, PRESET_COUNT - 1); break;
    case SH_FIELD:
        mField = (EField)arg;
        mHits.setFocus(SH_FIELD, arg);
        break;
    case SH_CREATE: createAdventure(); break;
    case SH_APPROVE:
        if (mRequests.isArray() && arg >= 0 && arg < (S32)mRequests.size())
        {
            mApproveAdv = mRequests[arg]["adventure"].asString();
            mApproveRegion = mRequests[arg]["region"].asString();
            mApproveEnv = true;
            mApproveBudget = llclamp(mRequests[arg]["npcBudgetAsked"].asInteger(), 0, NPC_BUDGET_MAX);
        }
        break;
    case SH_APPROVE_ENV: mApproveEnv = !mApproveEnv; break;
    case SH_APPROVE_LESS: mApproveBudget = llmax(0, mApproveBudget - 1); break;
    case SH_APPROVE_MORE: mApproveBudget = llmin(NPC_BUDGET_MAX, mApproveBudget + 1); break;
    case SH_APPROVE_OK:
        // by key: a refresh may have moved the rows since the form opened
        approve(requestIndex(mApproveAdv, mApproveRegion));
        break;
    case SH_APPROVE_CANCEL: mApproveAdv.clear(); mApproveRegion.clear(); break;
    case SH_REFUSE:
        if (mRequests.isArray() && arg >= 0 && arg < (S32)mRequests.size())
        {
            mConfirmKeyA = mRequests[arg]["adventure"].asString();
            mConfirmKeyB = mRequests[arg]["region"].asString();
            ask(SC_REFUSE, arg);
        }
        break;
    case SH_KIND: mImportPack = arg == 1; break;
    case SH_IMPORT_REGION:
    {
        // the same list pageImport drew
        std::vector<std::pair<std::string, std::string>> choices;
        if (LLViewerRegion* rgn = gAgent.getRegion()) choices.push_back({ rgn->getRegionID().asString(), rgn->getName() });
        if (const LLSD* a = listed(mOpenId))
        {
            const LLSD& rs = (*a)["regions"];
            for (size_t k = 0; rs.isArray() && k < (size_t)rs.size(); ++k)
            {
                const std::string rid = rs[(LLSD::Integer)k]["region"].asString();
                bool dup = false;
                for (const auto& c : choices) dup = dup || c.first == rid;
                if (!dup) choices.push_back({ rid, cap(rs[(LLSD::Integer)k]["name"].asString(), 64) });
            }
        }
        if (arg >= 0 && arg < (S32)choices.size())
        {
            mImportRegion = choices[arg].first;
        }
        break;
    }
    case SH_IMPORT_RUN: runImport(); break;
    case SH_PICK_TOGGLE:
        mPickOpen = !mPickOpen;
        if (mPickOpen) collectNotecards();
        break;
    case SH_PICK_USE:
        if (arg >= 0 && arg < (S32)mPickShown.size())
        {
            LLViewerInventoryItem* item = gInventory.getItem(mPickShown[arg]);
            if (!item) setStatus("That notecard is no longer in your inventory.", true);
            // ADVENTURE_PACK_V1.md §13.1: the viewer sees a notecard's asset only with full permissions
            else if (item->getAssetUUID().isNull()) setStatus("The viewer cannot see this notecard's contents: it needs full permissions.", true);
            else readNotecard(item);
        }
        break;
    case SH_DOWNLOAD: download(arg); break;
    case SH_PACK_FILE: choosePackFile(); break;
    case SH_USE_DRAFT:
        if (!mOpenId.empty()) ask(SC_REPLACE, 0);
        else useAsDraft();
        break;
    case SH_IMPORT_CANCEL:
        mImportedJson.clear();
        mImportedPack = LLSD();
        mReport = LLSD();
        mImportProblems = LLSD::emptyArray();
        setStatus("Thrown away.", false);
        break;
    case SH_CHECK:
    {
        LLSD body;
        body["adventure"] = mOpenId;
        call("validate", body, [](WolfAdventureStudioView* self, const LLSD& reply)
        {
            self->setProblems(reply["problems"]);
            const S32 errors = countProblems(reply["problems"], "error");
            self->setStatus(errors ? plural(errors, "error", "errors") + " to fix." : std::string("Checked: nothing stops publishing."), false);
        }, "Checking...");
        break;
    }
    case SH_REQUEST:
    {
        const LLSD& pr = std::as_const(mDraftPack)["adventure"]["regions"];
        if (pr.isArray() && arg >= 0 && arg < (S32)pr.size()) requestRegion(pr[arg]["region"].asString());
        break;
    }
    case SH_REQUEST_HERE:
        if (LLViewerRegion* rgn = gAgent.getRegion()) requestRegion(rgn->getRegionID().asString());
        break;
    case SH_EXPIRY: mGrantExpiry = llclamp(arg, 0, EXPIRY_COUNT - 1); break;
    case SH_GRANT: pickGrantee(arg == 1 ? "teacher" : "gm"); break;
    case SH_REVOKE:
        if (mGrants.isArray() && arg >= 0 && arg < (S32)mGrants.size())
        {
            mConfirmKeyA = mGrants[arg]["agent"].asString();
            mConfirmKeyB = mGrants[arg]["role"].asString();
            mConfirmKeyC = mGrants[arg]["region"].asString();
            ask(SC_REVOKE, arg);
        }
        break;
    case SH_REPUBLISH: ask(SC_REPUBLISH, arg); break;
    case SH_PUBLISH:
        // [WOLF ADVENTURE P2 2026-10-16] never publish a draft older than what is on screen
        if (draftDirty()) { saveDraftNow(); setStatus("Saving your changes first - press Publish again when it says Saved.", false); break; }
        ask(SC_PUBLISH, 0);
        break;
    case SH_UNPUBLISH: ask(SC_UNPUBLISH, 0); break;
    case SH_YES: answer(true); break;
    case SH_NO: answer(false); break;
    default: pressP2(id, arg); break;   // [WOLF ADVENTURE P2 2026-10-16/]
    }
}

bool WolfAdventureStudioView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h)
    {
        if (mConfirm != SC_NONE) return true;   // the question is modal to this window
        if (mField == F_EDIT) { commitEdit(); mField = F_NONE; }   // [WOLF ADVENTURE P3 2026-10-16/]
        return false;
    }
    setFocus(true);
    mKeyboardNav = false;
    if (!h->mEnabled) return true;
    mHits.setPressed(h);
    gFocusMgr.setMouseCapture(this);
    if (flowMouseDown(x, y, h)) return true;   // [WOLF STORY FIRST 2026-10-17/] pan, or a connection begun
    if (h->mId == SH_SCROLL)
    {
        mDraggingScroll = true;
        mScrollGrabY = (F32)y;
        mScrollGrabStart = mScroll[mPage];
    }
    return true;
}

bool WolfAdventureStudioView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (!hasMouseCapture()) return false;
    if (flowMouseUp(x, y)) { gFocusMgr.setMouseCapture(nullptr); mHits.setPressed(nullptr); return true; }   // [WOLF STORY FIRST 2026-10-17/]
    // what was pressed, before the release clears it (wolffloaterroleplay.cpp's [FIX 2026-10-09])
    const S32 pid = mHits.pressedId(), parg = mHits.pressedArg();
    const bool was_drag = mDraggingScroll;
    gFocusMgr.setMouseCapture(nullptr);
    mHits.setPressed(nullptr);
    mDraggingScroll = false;
    if (was_drag) return true;
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (h && h->mId == pid && h->mArg == parg && h->mEnabled) press(pid, parg);
    return true;
}

void WolfAdventureStudioView::onMouseCaptureLost()
{
    mHits.setPressed(nullptr);
    mDraggingScroll = false;
    mFlowPanning = false;      // [WOLF STORY FIRST 2026-10-17/]
    mFlowConnectFrom = -1;
}

bool WolfAdventureStudioView::handleHover(S32 x, S32 y, MASK mask)
{
    if (hasMouseCapture())
    {
        if (flowHover(x, y)) { gViewerWindow->setCursor(UI_CURSOR_HAND); return true; }   // [WOLF STORY FIRST 2026-10-17/]
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
    gViewerWindow->setCursor(h->mId == SH_FIELD || h->mId == SH_E_TEXT ? UI_CURSOR_IBEAM : (h->mEnabled && h->mFocusable ? UI_CURSOR_HAND : UI_CURSOR_ARROW));
    return true;
}

bool WolfAdventureStudioView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (editWheel(clicks)) return true;   // [WOLF ADVENTURE P3 2026-10-16/] the open list
    if (mConfirm == SC_NONE && flowWheel(x, y, clicks)) return true;   // [WOLF STORY FIRST 2026-10-17/] zoom the flowchart
    if (!mPageRect.pointInRect(x, y) || mConfirm != SC_NONE) return false;
    mScroll[mPage] = llmax(0.f, mScroll[mPage] + (F32)clicks * 40.f);
    return true;
}

bool WolfAdventureStudioView::handleToolTip(S32 x, S32 y, MASK mask)
{
    const WolfGameHits::Hit* h = mHits.at(x, y);
    if (!h || h->mTip.empty()) return false;
    LLToolTipMgr::instance().show(h->mTip);
    return true;
}

void WolfAdventureStudioView::escape()
{
    if (mConfirm != SC_NONE)
    {
        answer(false);
        return;
    }
    if (editEscape()) return;   // [WOLF ADVENTURE P3 2026-10-16/] the open list, or the place tool
    if (LLFloater* f = getParentByType<LLFloater>()) f->closeFloater();
}

bool WolfAdventureStudioView::handleKeyHere(KEY key, MASK mask)
{
    // [WOLF ADVENTURE P3 2026-10-16] the open list and an editor field first
    if (mConfirm == SC_NONE && editKey(key, mask)) return true;
    // a text field first: Backspace, Enter, paste (STUDIO §6 3.3.8 paste)
    if (mField != F_NONE)
    {
        std::string& text = fieldText(mField);
        if (key == KEY_BACKSPACE && mask == MASK_NONE)
        {
            LLWString w = utf8str_to_wstring(text);
            if (!w.empty()) w.pop_back();
            text = wstring_to_utf8str(w);
            if (mField == F_TITLE) mTitleError.clear();
            return true;
        }
        if (key == KEY_RETURN && mask == MASK_NONE)
        {
            // [WOLF ADVENTURE AUDIT 2026-10-17] on Start here, Enter in the name is the step's Next (Make it)
            if (mField == F_TITLE && mBusy == 0 && mPage == SP_WIZARD) wizNext();
            else if (mField == F_TITLE && mBusy == 0) createAdventure();   // one request at a time, as the button
            else if (mField == F_DELETE && mDeleteTyped == mDeleteTitle) answer(true);
            else if (mField == F_TALK && !mTestBusy) testSay(mTestBox, std::string());   // [WOLF ADVENTURE P2 2026-10-16/]
            else if (mField == F_ASSIST && !mAssistBusy) assistRun();
            else if (mField == F_HELPER && !mHelperBusy && !mHelperMissing) helperSend();   // [WOLF STORY FIRST 2026-10-17/] (as the button)
            return true;
        }
        if (key == 'V' && (mask & MASK_CONTROL))
        {
            LLWString pasted;
            if (LLClipboard::instance().pasteFromClipboard(pasted)) text = cap(text + wstring_to_utf8str(pasted), fieldCap(mField));
            return true;
        }
    }
    if (key == KEY_TAB && (mask == MASK_NONE || mask == MASK_SHIFT))
    {
        mKeyboardNav = true;
        if (mField == F_EDIT) commitEdit();   // [WOLF ADVENTURE P3 2026-10-16/]
        mHits.moveFocus(mask == MASK_NONE);
        const WolfGameHits::Hit* f = mHits.focused();
        mField = f && f->mId == SH_FIELD ? (EField)f->mArg : F_NONE;
        if (f && f->mId == SH_E_TEXT) startEdit(f->mArg);
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_ESCAPE:
        escape();
        return true;
    case KEY_RETURN:
    {
        const WolfGameHits::Hit* f = mHits.focused();
        if (f && f->mEnabled)
        {
            mKeyboardNav = true;
            press(f->mId, f->mArg);
        }
        return true;
    }
    case KEY_LEFT:
    case KEY_RIGHT:
        mKeyboardNav = true;
        mHits.moveFocus(key == KEY_RIGHT);
        return true;
    case KEY_UP: mScroll[mPage] = llmax(0.f, mScroll[mPage] - 40.f); return true;
    case KEY_DOWN: mScroll[mPage] += 40.f; return true;
    case KEY_PAGE_UP: mScroll[mPage] = llmax(0.f, mScroll[mPage] - (F32)mPageRect.getHeight() * 0.9f); return true;
    case KEY_PAGE_DOWN: mScroll[mPage] += (F32)mPageRect.getHeight() * 0.9f; return true;
    default:
        return false;
    }
}

bool WolfAdventureStudioView::handleUnicodeCharHere(llwchar uni_char)
{
    if (mConfirm == SC_NONE && editChar(uni_char)) return true;   // [WOLF ADVENTURE P3 2026-10-16/]
    if (mField != F_NONE)
    {
        if (uni_char < 0x20 || uni_char == 0x7f) return true;
        std::string& text = fieldText(mField);
        LLWString w = utf8str_to_wstring(text);
        if (w.size() < fieldCap(mField)) w.push_back(uni_char);
        text = wstring_to_utf8str(w);
        if (mField == F_TITLE) mTitleError.clear();
        return true;
    }
    if (uni_char != ' ') return false;
    const WolfGameHits::Hit* f = mHits.focused();
    if (f && f->mEnabled)
    {
        mKeyboardNav = true;
        press(f->mId, f->mArg);
    }
    return true;
}

bool WolfAdventureStudioView::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data,
                                                EAcceptance* accept, std::string& tooltip_msg)
{
    // [WOLF ADVENTURE P3 2026-10-16] a texture onto an editor's picture (handouts, portraits, decision images)
    if (editDrop(x, y, drop, cargo_type, cargo_data, accept, tooltip_msg)) return true;
    // Source: wolffloaterroleplay.cpp handleDragAndDrop - inventory cargo is the LLInventoryItem;
    // only the player's own inventory (or the library).
    if (cargo_type != DAD_NOTECARD) return false;
    *accept = ACCEPT_NO;
    if (mPage != SP_IMPORT || mImportPack)
    {
        tooltip_msg = "Open Import > Wolf Story to drop a story's notecards.";
        return true;
    }
    const LLToolDragAndDrop::ESource src = LLToolDragAndDrop::getInstance()->getSource();
    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item || (src != LLToolDragAndDrop::SOURCE_AGENT && src != LLToolDragAndDrop::SOURCE_LIBRARY))
    {
        tooltip_msg = "Drag the notecard from your own inventory.";
        return true;
    }
    if (item->getAssetUUID().isNull())
    {
        // ADVENTURE_PACK_V1.md §13.1: "Asset UUIDs are visible to the viewer only for full-permission items"
        tooltip_msg = "The viewer cannot see this notecard's contents: it needs full permissions.";
        return true;
    }
    *accept = ACCEPT_YES_COPY_SINGLE;
    const bool look = item->getName().rfind(LOOK_PREFIX, 0) == 0;
    tooltip_msg = look ? "Add \"" + item->getName() + "\" as a character's look" : "Use \"" + item->getName() + "\" as the story";
    mDragOverUntil = now() + 0.25;
    if (!drop) return true;
    mDragOverUntil = 0.0;
    readNotecard(item);
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the floater
// ═══════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAdventureStudio::WolfFloaterAdventureStudio(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterAdventureStudio::postBuild()
{
    mView = getChild<WolfAdventureStudioView>("studio");
    // In front of the drag handle, as WolfFloaterRoleplay::postBuild explains.
    setBackgroundVisible(false);
    return true;
}

void WolfFloaterAdventureStudio::onOpen(const LLSD& key)
{
    // Wolf Territories only (HUD_MESSAGES_V1.md §5.1; the service also refuses any other grid)
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
    // [WOLF ADVENTURE AUDIT 2026-10-17] the Guide beside it, unless its owner hid it
    if (gSavedSettings.getBOOL("WolfAdventureGuideOpen")) showGuide(true);
}

// static
bool WolfFloaterAdventureStudio::guideShown()
{
    return LLFloaterReg::instanceVisible("wolf_adventure_guide");
}

void WolfFloaterAdventureStudio::showGuide(bool on)
{
    gSavedSettings.setBOOL("WolfAdventureGuideOpen", on);
    if (!on)
    {
        if (LLFloater* g = LLFloaterReg::findInstance("wolf_adventure_guide")) g->closeFloater();
        return;
    }
    // Source: llfloater.cpp addDependentFloater(floaterp, reposition, resize) - placed beside this floater (findNeighboringPosition),
    // made as tall as it, snapped to it so it moves with it, and closed with it
    if (LLFloater* g = LLFloaterReg::showInstance("wolf_adventure_guide", LLSD(), false))
    {
        if (!g->getDependee()) addDependentFloater(g, true, true);
    }
}

// [WOLF ADVENTURE P3 2026-10-16] the field written, the draft saved, the place tool released, a test ended
void WolfFloaterAdventureStudio::onClose(bool app_quitting)
{
    if (mView) mView->editClosed(app_quitting);
    LLFloater::onClose(app_quitting);
}

void WolfFloaterAdventureStudio::draw()
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        closeFloater();
        return;
    }
    LLFloater::draw();
}
