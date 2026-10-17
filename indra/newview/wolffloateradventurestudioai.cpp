/**
 * @file wolffloateradventurestudioai.cpp
 * @brief WolfViewer: Adventure Studio, the P2 pages - AI and voice, Cast talk ("Talk to them now"),
 *        Book and assist ("Give it a book", the GM assistant) and Run (the GM Console Lite).
 *        Part of WolfAdventureStudioView (wolffloateradventurestudio.h).
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

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <sstream>

#include <boost/json.hpp>

#include "llagent.h"
#include "llclipboard.h"
#include "llfilepicker.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llsdjson.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewerregion.h"
#include "llweb.h"
#include "wolfadventure.h"
#include "wolfadventurehud.h"
#include "wolfgame.h"
#include "wolfgamehud.h"
#include "wolfgrid.h"

using namespace WolfGameDraw;

/**
 * [WOLF ADVENTURE P2 2026-10-16] The contract is STUDIO_AND_CONSOLE_V1.md §2.3 (voice, Talk to them
 * now), §2.10 (Give it a book), §2.12 (AI and voice), §3 (the GM Console, here its Lite subset);
 * WOLF_MIND_V1.md §7 (credits), §8 (voices), §10 (drafting), §11 (the assistant); BRAIN_API_V1.md
 * §5.5 and §8; and the brain's replies as the lead fixed them (scratchpad p2brief.md).
 */

namespace
{
    F64 now() { return LLFrameTimer::getTotalSeconds(); }
    LLColor4 fade(const LLColor4& c, F32 a) { return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * a); }

    /** Unix seconds as a local date and time (as wolffloateradventurestudio.cpp whenText). */
    std::string whenText(const LLSD& v)
    {
        if (!(v.isInteger() || v.isReal())) return WolfGame::cleanText(v.asString(), 32);
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

    const LLColor4 DANGER_INK(0.988f, 0.647f, 0.647f, 1.f);   // #fca5a5, as the Roleplay window
    const LLColor4 OK_INK(0.290f, 0.871f, 0.502f, 1.f);       // #4ade80
    const LLColor4 WARN_INK(0.984f, 0.749f, 0.141f, 1.f);     // #fbbf24

    // Source: STUDIO_AND_CONSOLE_V1.md §1 "Saving is automatic, debounced 2 s after the last edit".
    const F64 AUTOSAVE_SEC = 2.0;
    // A save that failed for another reason than a newer draft is tried again after this.
    const F64 SAVE_RETRY_SEC = 10.0;
    // Source: STUDIO_AND_CONSOLE_V1.md §3 "It polls gm_state every 2 s while open".
    const F64 GM_POLL_SEC = 2.0;
    // p2brief: "job polling every 3 s".
    const F64 JOB_POLL_SEC = 3.0;
    // Source: BRAIN_API_V1.md §3 - import and draft_start at most 12 MiB; the text is kept below 11
    // MiB so the JSON around it (escaped newlines and quotes) still fits.
    const size_t BOOK_MAX_BYTES = 11 * 1024 * 1024;
    // Source: adventure_pack_v1.schema.json $defs.ai and $defs.castSheet.voice.rate.
    const S32 CAP_MAX = 5000, TURNS_MAX = 2000, CHARS_MAX = 1000000;
    const F64 RATE_MIN = 0.5, RATE_MAX = 2.0, RATE_STEP = 0.05;
    // Source: WOLF_MIND_V1.md §10.1 / STUDIO §2.10 "pick acts (1-12)"; aicredits.php AIC_MAX_SPEND 5000.
    const S32 ACTS_MIN = 1, ACTS_MAX = 12, BOOK_CREDITS_MAX = 5000;
    // Source: WOLF_MIND_V1.md §7.2 "Estimate for one talk turn" - 255 micro-USD, plus 375 for the
    // first turn's cache write. 1 credit = 10,000 micro-USD (§7.1).
    const S64 MICRO_PER_CREDIT = 10000;

    std::string cap(const std::string& s, size_t n) { return WolfGame::cleanText(s, n); }
    std::string plural(S32 n, const char* one, const char* many) { return llformat("%d %s", n, n == 1 ? one : many); }

    std::string creditsText(F64 credits)
    {
        // whole credits as a number; fractions to two places (a talk turn is ~0.03 credits)
        if (credits >= 100.0) return thousands((S64)llround(credits)) + " credits";
        return llformat("%.2f credits", credits);
    }

    std::string microText(S64 micro)
    {
        return creditsText((F64)micro / (F64)MICRO_PER_CREDIT);
    }

    std::string clockText(const LLSD& v)
    {
        if (!(v.isInteger() || v.isReal())) return std::string();
        const time_t t = (time_t)v.asReal();
        struct tm lt;
#if LL_WINDOWS
        if (localtime_s(&lt, &t) != 0) return std::string();
#else
        if (!localtime_r(&t, &lt)) return std::string();
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M:%S", &lt);
        return buf;
    }

    /** Is the text valid UTF-8? (The brain decodes the request as JSON, which needs it.) */
    bool validUtf8(const std::string& s)
    {
        size_t i = 0, n = s.size();
        while (i < n)
        {
            const unsigned char c = (unsigned char)s[i];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > n) return false;
            for (size_t k = 1; k < len; ++k)
            {
                if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
            }
            i += len;
        }
        return true;
    }

    /** WOLF_MIND_V1.md §10.1 "the HTML is stripped to text": scripts and styles out, tags out (block
     *  tags as line breaks), the five common entities decoded. */
    std::string htmlToText(const std::string& html)
    {
        std::string out;
        out.reserve(html.size());
        std::string lower = html;
        LLStringUtil::toLower(lower);
        size_t i = 0;
        while (i < html.size())
        {
            if (html[i] == '<')
            {
                for (const char* skip : { "<script", "<style" })
                {
                    if (lower.compare(i, strlen(skip), skip) == 0)
                    {
                        const std::string close = std::string("</") + (skip + 1) + ">";
                        const size_t end = lower.find(close, i);
                        i = end == std::string::npos ? html.size() : end + close.size();
                        break;
                    }
                }
                if (i >= html.size()) break;
                if (html[i] != '<') continue;
                const size_t end = html.find('>', i);
                if (end == std::string::npos) break;
                const std::string tag = lower.substr(i, std::min<size_t>(end - i, 6));
                const bool block = tag.rfind("<p", 0) == 0 || tag.rfind("</p", 0) == 0 || tag.rfind("<br", 0) == 0
                                || tag.rfind("<h", 0) == 0 || tag.rfind("</h", 0) == 0 || tag.rfind("<div", 0) == 0 || tag.rfind("</div", 0) == 0
                                || tag.rfind("<li", 0) == 0;
                out += block ? '\n' : ' ';
                i = end + 1;
                continue;
            }
            if (html[i] == '&')
            {
                static const std::pair<const char*, const char*> ENT[] = {
                    { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&nbsp;", " " } };
                bool done = false;
                for (const auto& e : ENT)
                {
                    if (lower.compare(i, strlen(e.first), e.first) == 0)
                    {
                        out += e.second;
                        i += strlen(e.first);
                        done = true;
                        break;
                    }
                }
                if (done) continue;
            }
            out += html[i++];
        }
        return out;
    }

    /** "piper:en_GB-alan-medium" -> its label from the voices list, else the id. */
    std::string voiceLabel(const LLSD& voices, const std::string& id)
    {
        for (size_t i = 0; voices.isArray() && i < (size_t)voices.size(); ++i)
        {
            if (voices[(LLSD::Integer)i]["id"].asString() == id) return cap(voices[(LLSD::Integer)i]["label"].asString(), 64);
        }
        return id;
    }

    boost::json::object& objectAt(boost::json::object& o, const char* key)
    {
        boost::json::value& v = o[key];
        if (!v.is_object()) v = boost::json::object();
        return v.as_object();
    }

    std::string sourceWord(const std::string& s)
    {
        // BRAIN_API_V1.md §2 wolfadv_events.source: region | gm | director | brain | lsl
        if (s == "gm") return "GM";
        if (s == "director") return "Director";
        if (s == "region") return "Region";
        if (s == "lsl") return "Script";
        return "Story";
    }

    std::string controlWords(const std::string& control, const std::string& gm_name, bool mine)
    {
        // BRAIN_API_V1.md §2 wolfadv_sessions.control: auto | live | paused (STUDIO §3 "as a word")
        if (control == "live") return mine ? std::string("You are running it") : (gm_name.empty() ? std::string("A game master is running it") : gm_name + " is running it");
        if (control == "paused") return "Paused";
        return "AI Director running";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// shared pieces
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventureStudioView::openIsOwner() const
{
    const LLSD* a = listed(mOpenId);
    return !mOpenId.empty() && (!a || (*a)["role"].asString().empty());
}

void WolfAdventureStudioView::stepper(F32 l, F32 top, F32 label_w, const std::string& label, const std::string& value, S32 id, S32 which,
                                      bool can_less, bool can_more, const std::string& tip, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    textL(small, label, l, top - 13.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, label_w - 6.f);
    const F32 x = l + label_w;
    btn(x, top - 26.f, x + 30.f, top, "-", id, which * 2, "Less: " + tip, p, can_less);
    btn(x + 36.f, top - 26.f, x + 66.f, top, "+", id, which * 2 + 1, "More: " + tip, p, can_more);
    textL(LLFontGL::getFontSansSerifSmallBold(), value, x + 76.f, top - 13.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER);
}

void WolfAdventureStudioView::toggle(F32 l, F32 top, F32 w, const std::string& label, bool on, S32 id, S32 arg, const std::string& tip,
                                     const Palette& p, bool enabled)
{
    // the state as a word as well as the lit button (colour never alone)
    btn(l, top - 26.f, l + w, top, label + (on ? ": On" : ": Off"), id, arg, tip + (on ? " (on now)" : " (off now)"), p, enabled, on ? BTN_ACTIVE : 0);
}

std::vector<std::pair<std::string, std::string>> WolfAdventureStudioView::draftBeats() const
{
    std::vector<std::pair<std::string, std::string>> out;
    const LLSD& acts = std::as_const(mDraftPack)["acts"];
    for (size_t a = 0; acts.isArray() && a < (size_t)acts.size(); ++a)
    {
        const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s)
        {
            const LLSD& sc = scenes[(LLSD::Integer)s];
            const LLSD& beats = sc["beats"];
            for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b)
            {
                const LLSD& bt = beats[(LLSD::Integer)b];
                const std::string id = cap(bt["id"].asString(), 48);
                if (id.empty()) continue;
                const std::string title = cap(sc["title"].asString(), 64);
                out.push_back({ id, (title.empty() ? std::string() : title + " - ") + cap(bt["objective"].asString(), 120) });
            }
        }
    }
    return out;
}

std::string WolfAdventureStudioView::sceneOfBeat(const std::string& beat) const
{
    const LLSD& acts = std::as_const(mDraftPack)["acts"];
    for (size_t a = 0; acts.isArray() && a < (size_t)acts.size(); ++a)
    {
        const LLSD& scenes = acts[(LLSD::Integer)a]["scenes"];
        for (size_t s = 0; scenes.isArray() && s < (size_t)scenes.size(); ++s)
        {
            const LLSD& beats = scenes[(LLSD::Integer)s]["beats"];
            for (size_t b = 0; beats.isArray() && b < (size_t)beats.size(); ++b)
            {
                if (beats[(LLSD::Integer)b]["id"].asString() == beat) return scenes[(LLSD::Integer)s]["id"].asString();
            }
        }
    }
    return std::string();
}

// ---- the draft: change it here, save it 2 s later ----

void WolfAdventureStudioView::editDraft(const std::function<void(boost::json::object&)>& edit)
{
    editDoc([&edit](boost::json::value& v) { edit(v.as_object()); return true; });
}

// [WOLF ADVENTURE P3 2026-10-16] The draft is edited in place on one parsed document (mDraftDoc), parsed once
// when it arrives (setDraftText); each edit then re-serialises it for saving and remakes the read copy.
// `edit` returns false when it changed nothing (a pointer that no longer exists): nothing is marked to save.
bool WolfAdventureStudioView::editDoc(const std::function<bool(boost::json::value&)>& edit)
{
    if (mDraftJson.empty() || !openIsOwner())
    {
        setStatus("The draft has not loaded yet - wait a moment and try again.", true);
        return false;
    }
    if (mHelperBusy)
    {
        // [WOLF STORY FIRST 2026-10-17] the helper is changing the saved draft: an edit now would be lost or clash with its reply
        setStatus("The helper is working on your adventure - wait for its answer, then change it.", true);
        return false;
    }
    if (!mDraftDoc.is_object())
    {
        boost::system::error_code ec;
        mDraftDoc = boost::json::parse(mDraftJson, ec);
        if (ec || !mDraftDoc.is_object())
        {
            mDraftDoc = nullptr;
            setStatus("The draft could not be read - reopen the adventure.", true);
            return false;
        }
    }
    if (!edit(mDraftDoc)) return false;
    mDraftJson = boost::json::serialize(mDraftDoc);
    mDraftPack = LlsdFromJson(mDraftDoc);
    ++mDraftGen;
    mDirtyAt = now();
    return true;
}

void WolfAdventureStudioView::setDraftText(const std::string& json, const LLSD& pack)
{
    mDraftJson = json;
    boost::system::error_code ec;
    mDraftDoc = json.empty() ? boost::json::value() : boost::json::parse(json, ec);
    if (ec || !mDraftDoc.is_object()) mDraftDoc = nullptr;
    mDraftPack = pack.isDefined() ? pack : (mDraftDoc.is_object() ? LlsdFromJson(mDraftDoc) : LLSD());
    ++mDraftGen;
}

void WolfAdventureStudioView::autosaveTick()
{
    const F64 t = now();
    // [WOLF STORY FIRST 2026-10-17] not while the helper works on the saved draft: its reply carries the draft it saved
    if (mDirtyAt > 0.0 && !mSaving && !mConflict && mBusy == 0 && !mHelperBusy && t - mDirtyAt >= AUTOSAVE_SEC) saveDraftNow();
    if (mJobPollAt > 0.0 && t >= mJobPollAt) pollJob();
    if (mPage == SP_RUN && !mRunSession.empty() && !mGmLoading && t >= mGmPollAt) pollGmState(false);
}

void WolfAdventureStudioView::saveDraftNow(bool keep_mine)
{
    if (mOpenId.empty() || mDraftJson.empty() || mSaving) return;
    mSaving = true;
    mDirtyAt = 0.0;   // an edit made while this save is out marks the draft dirty again
    LLSD body;
    body["adventure"] = mOpenId;
    body["pack_json"] = mDraftJson;
    body["etag"] = keep_mine ? mConflictEtag : mEtag;
    const std::string id = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // §5.1 save_draft {adventure, pack, etag} -> {etag, problems}; 409 {error, etag} when another
    // window saved first (the reply then carries that newer etag)
    WolfAdventure::api("save_draft", body, [handle, id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mSaving = false;
        if (self->mOpenId != id) return;
        if (ok)
        {
            self->mEtag = reply["etag"].asString();
            self->mConflict = false;
            self->setProblems(reply["problems"]);
            self->setStatus("Saved.", false);
            return;
        }
        if (reply.isMap() && reply.has("etag"))
        {
            // STUDIO §1: "Changed in another window: Reload / Keep mine"
            self->mConflict = true;
            self->mConflictEtag = reply["etag"].asString();
            self->mDirtyAt = now();
            self->setStatus("This draft was changed in another window. Choose Reload (theirs) or Keep mine.", true);
            return;
        }
        self->mDirtyAt = now() + SAVE_RETRY_SEC;
        self->setStatus("Not saved yet (it will try again): " + error, true);
    });
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// loading
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::loadHomeSpend()
{
    // §2.1 Home cards "today's AI spend": ai_budget for each of the owner's own adventures (at most
    // 12, the cards the Home page shows first), quietly - a failure leaves the card as it was.
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    S32 asked = 0;
    for (size_t i = 0; mList.isArray() && i < (size_t)mList.size() && asked < 12; ++i)
    {
        const LLSD& a = mList[(LLSD::Integer)i];
        if (!a["role"].asString().empty()) continue;
        const std::string id = a["id"].asString();
        if (id.empty()) continue;
        ++asked;
        LLSD body;
        body["adventure"] = id;
        WolfAdventure::api("ai_budget", body, [handle, id](bool ok, const LLSD& reply, const std::string&)
        {
            if (handle.isDead() || !ok) return;
            const F64 c = reply["today"]["credits"].asReal();
            handle.get()->mHomeSpend[id] = "AI today: " + creditsText(c);
        });
    }
}

void WolfAdventureStudioView::loadBudget()
{
    if (mOpenId.empty()) return;
    mBudgetLoading = true;
    mBudgetError.clear();
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string id = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §5.5 ai_budget {adventure} (fields: p2brief)
    WolfAdventure::api("ai_budget", body, [handle, id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != id) return;
        self->mBudgetLoading = false;
        self->mBudgetFor = id;
        if (ok) self->mBudget = reply;
        else self->mBudgetError = error;
    });
}

void WolfAdventureStudioView::loadVoices()
{
    if (mVoicesLoaded || mVoicesAsked || LLTimer::getElapsedSeconds() < mVoicesRetryAt) return;
    mVoicesAsked = true;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §5.5 voices {} -> {voices:[{id, label, licence, price:{microPerChar}}]}
    WolfAdventure::api("voices", LLSD::emptyMap(), [handle](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (!ok)
        {
            self->mVoicesAsked = false;   // asked again when the page draws, at most every 15 s
            self->mVoicesRetryAt = LLTimer::getElapsedSeconds() + 15.0;
            self->setStatus("The voice list could not be loaded: " + error, true);
            return;
        }
        self->mVoices = reply["voices"].isArray() ? reply["voices"] : LLSD::emptyArray();
        self->mVoicesLoaded = true;
    });
}

void WolfAdventureStudioView::loadOwnerSettings()
{
    if (mOwnerAsked || LLTimer::getElapsedSeconds() < mOwnerRetryAt) return;
    mOwnerAsked = true;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §5.5 owner_settings with no fields reads them
    WolfAdventure::api("owner_settings", LLSD::emptyMap(), [handle](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (!ok)
        {
            self->mOwnerAsked = false;   // asked again when the page draws, at most every 15 s
            self->mOwnerRetryAt = LLTimer::getElapsedSeconds() + 15.0;
            self->setStatus("Your account limits could not be loaded: " + error, true);
            return;
        }
        self->mOwner = reply["settings"];
        self->mOwnerLoaded = true;
        self->mOwnerDirty = false;
    });
}

void WolfAdventureStudioView::saveOwnerSettings()
{
    if (!mOwnerLoaded) return;
    LLSD body;
    for (const char* k : { "dailyCeilingCredits", "assistCapCredits", "gmRewardMoney", "gmRewardXp" })
    {
        body[k] = mOwner[k].asInteger();
    }
    call("owner_settings", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mOwner = reply["settings"];
        self->mOwnerDirty = false;
        self->setStatus("Your account limits are saved.", false);
        self->loadBudget();
    }, "Saving your limits...");
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// AI and voice (STUDIO §2.12)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::pageAi(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (mOpenId.empty()) return para(l, r, y, "Open an adventure on Home first.", p.mDim);
    const bool owner = openIsOwner();
    if (mBudgetFor != mOpenId && !mBudgetLoading) loadBudget();
    if (!mVoicesLoaded) loadVoices();
    if (!mOwnerLoaded && owner) loadOwnerSettings();
    if (mConflict)
    {
        y = para(l, r, y, "This draft was changed in another window.", DANGER_INK) - 4.f;
        btn(l, y - 26.f, l + 130.f, y, "Reload", SH_CONFLICT_RELOAD, 0, "Throw your changes away and load the other window's draft", p);
        btn(l + 140.f, y - 26.f, l + 270.f, y, "Keep mine", SH_CONFLICT_KEEP, 0, "Save your changes over the other window's", p);
        y -= 36.f;
    }
    if (!owner) y = para(l, r, y, "Only the adventure's owner changes these settings. You can see the meter below.", p.mDim) - 8.f;

    const LLSD& ai = std::as_const(mDraftPack)["ai"];
    const std::string purpose = std::as_const(mDraftPack)["adventure"]["purpose"].asString();
    const bool edu = purpose == "education";
    const bool ai_on = ai["mode"].asString() == "ai";
    const bool can = owner && !mDraftJson.empty();
    // ---- Characters: the mode, with one line each (§2.12) ----
    y = head(l, r, y, "Characters", p);
    {
        const F32 gap = 8.f, cw = (r - l - gap) * 0.5f, chh = 54.f;
        struct Mode { const char* mLabel; const char* mLine; bool mOn; };
        const Mode modes[2] = { { "AI characters", "You pay per conversation from your WolfCredits.", ai_on },
                                { "Written lines only", "Free.", !ai_on } };
        for (S32 i = 0; i < 2; ++i)
        {
            const F32 cl = l + (F32)i * (cw + gap);
            button(cl, y - chh, cl + cw, y, std::string(), p, flagsFor(SH_AI_MODE, i, can) | (modes[i].mOn ? BTN_ACTIVE : 0));
            textL(bold, std::string(modes[i].mLabel) + (modes[i].mOn ? " - chosen" : ""), cl + 10.f, y - 8.f, modes[i].mOn ? p.mAccent : p.mText,
                  LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            textL(small, modes[i].mLine, cl + 10.f, y - 10.f - slh, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, cw - 20.f);
            hit(cl, y - chh, cl + cw, y, SH_AI_MODE, i, std::string(modes[i].mLabel) + ": " + modes[i].mLine, true, can);
        }
        y -= chh + 8.f;
        y = para(l, r, y, "When the day's cap or your credits run out, the adventure carries on with written lines by itself, and comes back to AI when they allow it.",
                 p.mDim, true) - 8.f;
    }
    // ---- Limits ----
    y = head(l, r, y, "Limits", p);
    const S32 daily = ai.has("dailyCapCredits") ? ai["dailyCapCredits"].asInteger() : 100;
    const S32 turns = ai.has("playerDailyTurns") ? ai["playerDailyTurns"].asInteger() : 200;
    stepper(l, y, 200.f, "Daily cap", llformat("%d credits a day (US $%.2f)", daily, (F64)daily / 100.0), SH_AI_STEP, 0, can && daily > 0, can && daily < CAP_MAX,
            "the most this adventure spends a day", p);
    y -= 32.f;
    stepper(l, y, 200.f, "Talk turns per player", llformat("%d a day", turns), SH_AI_STEP, 1, can && turns > 0, can && turns < TURNS_MAX,
            "AI talk turns one player may have a day", p);
    y -= 34.f;
    const bool free_chat = ai.has("freeChat") ? ai["freeChat"].asBoolean() : !edu;
    const bool accuracy = ai.has("accuracy") ? ai["accuracy"].asBoolean() : edu;
    const bool improvise = ai["improvise"].asBoolean();
    const bool translate = ai["translate"].asBoolean();
    const F32 tw = llmin(260.f, (r - l - 8.f) * 0.5f);
    toggle(l, y, tw, "Free typing", free_chat, SH_AI_TOGGLE, 0, "Players may type freely to characters; off means quick replies only", p, can);
    toggle(l + tw + 8.f, y, tw, "Accuracy mode", accuracy, SH_AI_TOGGLE, 1, "Characters answer only from the source cards and say when they do not know", p, can);
    y -= 32.f;
    toggle(l, y, tw, "Improvise", improvise, SH_AI_TOGGLE, 2, "The AI Director may write short narration within a beat", p, can);
    toggle(l + tw + 8.f, y, tw, "Translate", translate, SH_AI_TOGGLE, 3, "Characters answer in the session's language", p, can);
    y -= 40.f;
    // ---- Voices ----
    y = head(l, r, y, "Voices", p);
    const bool voice_on = ai["voice"]["enabled"].asBoolean();
    const S32 chars = ai["voice"].has("dailyChars") ? ai["voice"]["dailyChars"].asInteger() : 50000;
    toggle(l, y, tw, "Spoken voices", voice_on, SH_AI_TOGGLE, 4, "Characters with a voice speak their lines aloud through voice chat", p, can);
    y -= 32.f;
    stepper(l, y, 200.f, "Characters spoken a day", thousands(chars), SH_AI_STEP, 2, can && chars > 0, can && chars < CHARS_MAX,
            "the most characters spoken aloud a day", p);
    y -= 32.f;
    y = para(l, r, y, "Give each character a voice on the Cast talk page.", p.mDim, true) - 8.f;

    // ---- Today: the meter (§2.12) ----
    y = head(l, r - 100.f, y, "Today", p);
    btn(r - 90.f, y + slh + 10.f - 24.f, r, y + slh + 10.f, "Refresh", SH_AI_REFRESH, 0, "Load the meter again", p, !mBudgetLoading);
    if (mBudgetLoading && !mBudget.isMap()) y = para(l, r, y, "Loading the meter...", p.mDim) - 6.f;
    if (!mBudgetError.empty()) y = para(l, r, y, "The meter could not be loaded: " + mBudgetError, DANGER_INK) - 6.f;
    if (mBudget.isMap())
    {
        const LLSD& b = mBudget;
        const bool b_ai = b["mode"].asString() == "ai";
        std::string mode_text = cap(b["modeText"].asString(), 120);
        if (mode_text.empty()) mode_text = b_ai ? "AI" : "Written lines only";
        textL(bold, "Now: " + mode_text, l, y, b_ai ? OK_INK : p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 6.f;
        // today against the cap: a bar with the numbers as words
        const F64 today = b["today"]["credits"].asReal();
        const F64 capc = (F64)llmax(0, b["cap"].asInteger());
        const F32 frac = capc > 0.0 ? (F32)llclamp(today / capc, 0.0, 1.0) : 0.f;
        textL(small, creditsText(today) + " of " + creditsText(capc) + " today" + llformat(" \xC2\xB7 %d talk turns", b["today"]["turns"].asInteger()),
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 3.f;
        rect(l, y - 8.f, r, y, p.mTrack);
        rect(l, y - 8.f, l + (r - l) * frac, y, frac >= 1.f ? WARN_INK : p.mAccent);
        chamferEdge(l - 0.5f, y - 8.5f, r + 0.5f, y + 0.5f, 0.f, 1.f, fade(p.mDim, 0.9f));
        y -= 16.f;
        // the last 7 days as a small bar chart with the number over each bar (one series: no legend;
        // the heading names it). Bars from the baseline, a 2 px gap between them.
        const LLSD& last7 = b["last7"];
        if (last7.isArray() && last7.size() > 0)
        {
            textL(bold, caps("Last 7 days (credits)"), l, y, p.mDim);
            y -= slh + 4.f;
            const F32 ch = 70.f, base = y - ch - slh - 2.f;
            F64 most = 0.0;
            for (size_t i = 0; i < (size_t)last7.size(); ++i) most = llmax(most, last7[(LLSD::Integer)i]["credits"].asReal());
            const S32 n = (S32)last7.size();
            const F32 slot = (r - l) / (F32)llmax(1, n), bw = llmin(46.f, slot - 2.f);
            for (S32 i = 0; i < n; ++i)
            {
                const LLSD& d = last7[i];
                const F64 c = d["credits"].asReal();
                const F32 h = most > 0.0 ? (F32)(c / most) * (ch - slh - 4.f) : 0.f;
                const F32 bx = l + slot * (F32)i + (slot - bw) * 0.5f;
                rect(bx, base, bx + bw, base + llmax(h, c > 0.0 ? 2.f : 0.f), p.mAccent);
                textL(small, c >= 10.0 ? llformat("%.0f", c) : llformat("%.2f", c), bx + bw * 0.5f, base + h + 2.f, p.mText, LLFontGL::HCENTER, LLFontGL::BOTTOM);
                std::string day = d["day"].asString();
                if (day.size() >= 10) day = day.substr(8, 2) + "/" + day.substr(5, 2);
                textL(small, day, bx + bw * 0.5f, base - 2.f, p.mDim, LLFontGL::HCENTER, LLFontGL::TOP);
                hit(bx, base, bx + bw, base + ch - slh, SH_NONE, i, cap(d["day"].asString(), 10) + ": " + creditsText(c) + llformat(", %d turns", d["turns"].asInteger()), false);
            }
            rect(l, base - 1.f, r, base, p.mDim);   // the baseline
            y = base - slh - 8.f;
        }
        // the balance, Buy credits (the money moves only on the website, §2.12)
        const LLSD& bal = b["balance"];
        const std::string balance = bal.isUndefined() ? std::string("not known right now") : thousands(bal.asInteger()) + " credits";
        textL(small, "Your WolfCredits: " + balance + (b["unbilledCredits"].asReal() > 0.0 ? " (" + creditsText(b["unbilledCredits"].asReal()) + " used, not charged yet)" : std::string()),
              l, y - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 150.f);
        btn(r - 140.f, y - 24.f, r, y, "Buy credits...", SH_AI_BUY, 0, "Opens the Wolf Territories website's credits page in your browser", p,
            !b["buyurl"].asString().empty());
        y -= 32.f;
        textL(small, "All your adventures today: " + creditsText(b["owner"]["todayCredits"].asReal()) + llformat(" of %d", b["owner"]["ceiling"].asInteger()),
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 2.f;
        textL(small, "GM assistant today: " + creditsText(b["assist"]["todayCredits"].asReal()) + llformat(" of %d", b["assist"]["cap"].asInteger()),
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 2.f;
        textL(small, "Voice today: " + thousands(b["voice"]["todayChars"].asInteger()) + " of " + thousands(b["voice"]["cap"].asInteger()) + " characters",
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 6.f;
        // §2.12 "The per-call estimate ..., labelled estimate"
        const S64 turn = b["estimate"].has("turnMicro") ? b["estimate"]["turnMicro"].asInteger() : 255;
        const S64 extra = b["estimate"].has("firstTurnExtraMicro") ? b["estimate"]["firstTurnExtraMicro"].asInteger() : 375;
        y = para(l, r, y, "Estimate: a talk turn costs about " + microText(turn) + "; the first with a character on a beat about " + microText(extra)
                 + " more. 100 turns are about " + microText(turn * 100) + ". What you really spend is shown above.", p.mDim, true) - 8.f;
    }
    // ---- Your account limits (owner_settings) ----
    if (owner && mOwnerLoaded)
    {
        y = head(l, r, y, "Your account limits", p);
        const S32 ceiling = mOwner["dailyCeilingCredits"].asInteger(), assist = mOwner["assistCapCredits"].asInteger();
        const S32 money = mOwner["gmRewardMoney"].asInteger(), xp = mOwner["gmRewardXp"].asInteger();
        stepper(l, y, 240.f, "All my adventures, a day", llformat("%d credits", ceiling), SH_OWNER_STEP, 0, ceiling > 0, ceiling < CAP_MAX,
                "the most all your adventures together spend a day", p);
        y -= 32.f;
        stepper(l, y, 240.f, "GM assistant, a day", llformat("%d credits", assist), SH_OWNER_STEP, 1, assist > 0, assist < CAP_MAX,
                "the most the GM assistant spends a day", p);
        y -= 32.f;
        stepper(l, y, 240.f, "GM money rewards a session", thousands(money), SH_OWNER_STEP, 2, money > 0, money < 1000000,
                "money a game master may hand out in one session", p);
        y -= 32.f;
        stepper(l, y, 240.f, "GM XP rewards a session", thousands(xp), SH_OWNER_STEP, 3, xp > 0, xp < 100000,
                "XP a game master may hand out in one session", p);
        y -= 34.f;
        btn(l, y - 28.f, l + 160.f, y, "Save limits", SH_OWNER_SAVE, 0, "Save your account limits", p, mOwnerDirty && mBusy == 0);
        if (mOwnerDirty) textL(small, "Not saved yet", l + 170.f, y - 14.f, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
        y -= 40.f;
    }
    if (draftDirty() || mSaving) textL(small, mSaving ? "Saving..." : "Changes save in a moment...", l, y, p.mDim);
    return y - slh - 8.f;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// Cast talk (STUDIO §2.3: Voice, Talk to them now)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::pageCast(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (mOpenId.empty()) return para(l, r, y, "Open an adventure on Home first.", p.mDim);
    const bool owner = openIsOwner();
    if (!owner) return para(l, r, y, "Only the adventure's owner can test its characters and set their voices.", p.mDim);
    if (mDraftJson.empty()) return para(l, r, y, "Loading the draft...", p.mDim);
    if (!mVoicesLoaded) loadVoices();
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    if (!cast.isArray() || cast.size() == 0)
    {
        return para(l, r, y, "This draft has no characters yet. Import a story or a pack with a cast first.", p.mDim);
    }
    mCastSel = llclamp(mCastSel, 0, (S32)cast.size() - 1);
    // ---- the characters ----
    y = head(l, r, y, "Characters", p);
    {
        F32 x = l;
        for (size_t i = 0; i < (size_t)cast.size() && i < 40; ++i)
        {
            const LLSD& c = cast[(LLSD::Integer)i];
            const std::string name = cap(c["first"].asString() + " " + c["last"].asString(), 64);
            const F32 w = llmin(200.f, bold->getWidthF32(name) + 22.f);
            if (x > l && x + w > r) { x = l; y -= 30.f; }
            const bool on = (S32)i == mCastSel;
            btn(x, y - 24.f, x + w, y, name, SH_CAST_PICK, (S32)i, "Choose " + name + (on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += w + 6.f;
        }
        y -= 34.f;
    }
    const LLSD& c = cast[mCastSel];
    const std::string cname = cap(c["first"].asString() + " " + c["last"].asString(), 64);
    // ---- Voice: the picker from the brain's list, the rate, Preview (§2.3) ----
    y = head(l, r, y, "Voice of " + cname, p);
    const std::string voice_id = c["voice"]["id"].asString();
    const F64 rate = c["voice"].has("rate") ? c["voice"]["rate"].asReal() : 1.0;
    {
        F32 x = l;
        const bool none = voice_id.empty();
        F32 w = bold->getWidthF32("No voice") + 22.f;
        btn(x, y - 24.f, x + w, y, "No voice", SH_VOICE_PICK, -1, "This character's lines are text only", p, true, none ? BTN_ACTIVE : 0);
        x += w + 6.f;
        // [WOLF ADVENTURE P3 2026-10-16] the free (piper) voices first; premium voices (ElevenLabs: id "eleven:<voice_id>",
        // premium:1, price.microPerChar > 0 - lead's contract) in their own group, shown only when switched on or in use
        bool any_premium = false, premium_in_use = false;
        for (size_t i = 0; mVoices.isArray() && i < (size_t)mVoices.size(); ++i)
        {
            const LLSD& v = std::as_const(mVoices)[(LLSD::Integer)i];
            if (!v["premium"].asBoolean()) continue;
            any_premium = true;
            premium_in_use = premium_in_use || v["id"].asString() == voice_id;
        }
        for (S32 group = 0; group < 2; ++group)
        {
            if (group == 1)
            {
                if (!any_premium) break;
                y -= 32.f;
                x = l;
                const bool shown = mShowPremium || premium_in_use;
                const S32 it = act([this]() { mShowPremium = !mShowPremium; });
                btn(x, y - 24.f, x + 330.f, y, shown ? "Premium voice (ElevenLabs) - hide" : "Premium voice (ElevenLabs) - show",
                    SH_E, it, "Premium voices are charged per character spoken, from your WolfCredits; the free voices cost nothing", p, !premium_in_use);
                if (!shown) break;
                y -= 30.f;
            }
            for (size_t i = 0; mVoices.isArray() && i < (size_t)mVoices.size(); ++i)
            {
                const LLSD& v = std::as_const(mVoices)[(LLSD::Integer)i];
                if (v["premium"].asBoolean() != (group == 1)) continue;
                std::string label = cap(v["label"].asString(), 48);
                const F64 per = v["price"]["microPerChar"].asReal();
                // credits = micro / 10000 (WOLF_MIND_V1.md §7.1), shown per 100 characters
                if (group == 1) label += llformat(" (%.2f per 100 chars)", per * 100.0 / 10000.0);
                w = llmin(300.f, bold->getWidthF32(label) + 22.f);
                if (x > l && x + w > r) { x = l; y -= 30.f; }
                const bool on = v["id"].asString() == voice_id;
                const std::string tip = label + (per > 0.0 ? llformat(" - %.2f credits per 100 characters spoken", per * 100.0 / 10000.0) : std::string(" - free (our own server)"))
                                      + (v["licence"].asString().empty() ? std::string() : " - licence: " + cap(v["licence"].asString(), 80));
                btn(x, y - 24.f, x + w, y, label, SH_VOICE_PICK, (S32)i, tip, p, true, on ? BTN_ACTIVE : 0);
                x += w + 6.f;
            }
        }
        y -= 32.f;
        if (!mVoicesLoaded) y = para(l, r, y, "Loading the voices...", p.mDim, true) - 4.f;
        if (!voice_id.empty() && voiceLabel(mVoices, voice_id) == voice_id && mVoicesLoaded)
        {
            y = para(l, r, y, "The voice \"" + voice_id + "\" is not on the grid's list; pick one above.", WARN_INK, true) - 4.f;
        }
    }
    stepper(l, y, 120.f, "Speed", llformat("%.2f x", rate), SH_RATE_STEP, 0, !voice_id.empty() && rate > RATE_MIN + 1e-6, !voice_id.empty() && rate < RATE_MAX - 1e-6,
            "how fast this character speaks", p);
    btn(r - 150.f, y - 26.f, r, y, "Preview voice", SH_VOICE_PREVIEW, mCastSel, "Hear the greeting in this voice (only you hear it)", p,
        !voice_id.empty() && mBusy == 0);
    y -= 34.f;
    if (!std::as_const(mDraftPack)["ai"]["voice"]["enabled"].asBoolean() && !voice_id.empty())
    {
        y = para(l, r, y, "Spoken voices are off for this adventure: switch them on on the AI and voice page.", p.mDim, true) - 4.f;
    }
    y -= 6.f;

    // ---- Talk to them now (§2.3) ----
    y = head(l, r, y, "Talk to them now", p);
    const std::vector<std::pair<std::string, std::string>> beats = draftBeats();
    if (beats.empty()) return para(l, r, y, "The draft has no beats to talk on yet.", p.mDim);
    mBeatSel = llclamp(mBeatSel, 0, (S32)beats.size() - 1);
    {
        btn(l, y - 24.f, l + 30.f, y, "<", SH_BEAT_STEP, 0, "The beat before", p, mBeatSel > 0);
        btn(l + 36.f, y - 24.f, l + 66.f, y, ">", SH_BEAT_STEP, 1, "The next beat", p, mBeatSel + 1 < (S32)beats.size());
        textL(small, llformat("Beat %d of %d: ", mBeatSel + 1, (S32)beats.size()) + beats[mBeatSel].first + " - " + beats[mBeatSel].second,
              l + 76.f, y - 12.f, p.mText, LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 76.f);
        y -= 32.f;
    }
    toggle(l, y, 260.f, "Preview without AI", !mTestAi, SH_TEST_AI, 0, "Written lines only, as players get them when AI is off - and free", p);
    btn(r - 160.f, y - 26.f, r, y, "Start again", SH_TEST_CLEAR, 0, "Forget this test conversation and start again", p, !mTestBusy);
    y -= 36.f;
    // the conversation so far (oldest first, the last 8 lines)
    if (mTestHistory.isArray() && mTestHistory.size() > 0)
    {
        const S32 n = (S32)mTestHistory.size();
        for (S32 i = llmax(0, n - 8); i < n; ++i)
        {
            const LLSD& h = mTestHistory[i];
            const bool you = h["who"].asString() == "player";
            textL(bold, you ? std::string("You") : cname, l, y, you ? p.mText : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, 140.f);
            y = para(l + 146.f, r, y, h["text"].asString(), p.mText, true) - 3.f;
        }
        y -= 6.f;
    }
    // the face: the real talk panel's (wolfadventuretalk.cpp talkFace)
    if (mTestTalk.mOn)
    {
        const F32 face_top = y;
        const F32 face_bottom = WolfAdventureUI::talkFace(mTestTalk, l + 10.f, r - 10.f, y - 10.f, p, false);
        card(l, face_bottom - 4.f, r, face_top, p, 6.f);
        y = WolfAdventureUI::talkFace(mTestTalk, l + 10.f, r - 10.f, y - 10.f, p, true) - 8.f;
        if (!mTestVoiceUrl.empty())
        {
            btn(r - 120.f, y - 24.f, r, y, "Play again", SH_TEST_PLAY, 0, "Hear the line again", p);
            y -= 30.f;
        }
    }
    // the replies of this beat for this character
    if (!mTestReplies.empty())
    {
        const F32 gap = 8.f, cw = (r - l - gap) * 0.5f;
        for (size_t i = 0; i < mTestReplies.size(); ++i)
        {
            const S32 col = (S32)i % 2, row = (S32)i / 2;
            const F32 bl = l + (F32)col * (cw + gap), bt = y - (F32)row * 38.f;
            btn(bl, bt - 32.f, bl + cw, bt, llformat("%d. ", (S32)i + 1) + mTestReplies[i].second, SH_TEST_REPLY, (S32)i,
                "Say: " + mTestReplies[i].second, p, !mTestBusy);
        }
        y -= (F32)((mTestReplies.size() + 1) / 2) * 38.f;
    }
    field(l, y - 30.f, r - 110.f, y, mTestBox, "Say something to " + cname + "...", F_TALK, p);
    btn(r - 100.f, y - 30.f, r, y, mTestBusy ? "Waiting..." : "Say", SH_TEST_SEND, 0, "Send it (Enter)", p, !mTestBusy && !mTestBox.empty(),
        mTestBusy ? 0 : BTN_PRIMARY);
    y -= 38.f;
    // each turn's cost, the session total, today's adventure spend (§2.3)
    std::string costs = "This turn: " + microText(mTestLastMicro) + " \xC2\xB7 This test: " + microText(mTestTotalMicro);
    if (mBudget.isMap() && mBudgetFor == mOpenId) costs += " \xC2\xB7 This adventure today: " + creditsText(mBudget["today"]["credits"].asReal());
    textL(small, costs, l, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= slh + 4.f;
    y = para(l, r, y, mTestAi ? "AI turns here are paid like players' (as a test) from your WolfCredits." : "Written lines only: nothing is charged.", p.mDim, true);
    return y - 8.f;
}

void WolfAdventureStudioView::previewVoice(S32 cast_index)
{
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    if (!cast.isArray() || cast_index < 0 || cast_index >= (S32)cast.size()) return;
    const LLSD& c = cast[cast_index];
    const std::string voice = c["voice"]["id"].asString();
    if (voice.empty()) return;
    // §2.3 "Preview (speaks the greeting through TTS to the owner only)"; with no greeting, the first
    // idle line, else the name
    std::string text = c["greeting"].asString();
    if (text.empty() && c["idle"].isArray() && c["idle"].size() > 0) text = c["idle"][0]["text"].asString();
    if (text.empty()) text = "My name is " + c["first"].asString() + " " + c["last"].asString() + ".";
    LLSD body;
    body["adventure"] = mOpenId;
    body["voice"] = voice;
    body["rate"] = c["voice"].has("rate") ? c["voice"]["rate"].asReal() : 1.0;
    body["text"] = cap(text, 600);
    // BRAIN_API_V1.md §5.5 tts_preview {voice, text} -> {url}
    call("tts_preview", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        LLHandle<WolfAdventureStudioView> handle = self->getDerivedHandle<WolfAdventureStudioView>();
        self->setStatus("Playing the preview...", false);
        WolfAdventure::playClip(reply["url"].asString(), [handle](const std::string& error)
        {
            if (handle.isDead() || error.empty()) return;
            handle.get()->setStatus("The preview could not be played: " + error, true);
        });
    }, "Making the preview...");
}

void WolfAdventureStudioView::testSay(const std::string& text_in, const std::string& reply)
{
    if (mTestBusy) return;
    const LLSD& cast = std::as_const(mDraftPack)["cast"];
    const std::vector<std::pair<std::string, std::string>> beats = draftBeats();
    if (!cast.isArray() || mCastSel < 0 || mCastSel >= (S32)cast.size() || beats.empty()) return;
    std::string text = text_in;
    LLStringUtil::trim(text);
    if (text.empty() && reply.empty())
    {
        setStatus("Type something to say first.", true);
        return;
    }
    const std::string cast_id = cast[mCastSel]["id"].asString();
    const std::string beat = beats[llclamp(mBeatSel, 0, (S32)beats.size() - 1)].first;
    // a different character or beat starts a new test conversation
    const std::string key = cast_id + "|" + beat;
    if (mTestCastFor != key)
    {
        mTestCastFor = key;
        mTestHistory = LLSD::emptyArray();
        mTestTalk = WolfAdventure::Talk();
    }
    std::string said = text;
    if (!reply.empty())
    {
        for (const auto& rp : mTestReplies) if (rp.first == reply) said = rp.second;
    }
    LLSD body;
    body["adventure"] = mOpenId;
    body["cast"] = cast_id;
    body["beat"] = beat;
    if (!text.empty()) body["text"] = cap(text, 500);
    if (!reply.empty()) body["reply"] = reply;
    // the last 12 lines as `history` (WOLF_MIND_V1.md §3 layer 4: "the last 6 exchanges")
    LLSD history = LLSD::emptyArray();
    const S32 n = mTestHistory.isArray() ? (S32)mTestHistory.size() : 0;
    for (S32 i = llmax(0, n - 12); i < n; ++i) history.append(mTestHistory[i]);
    body["history"] = history;
    body["ai"] = mTestAi ? "ai" : "none";
    if (!mTestHistory.isArray()) mTestHistory = LLSD::emptyArray();
    LLSD mine;
    mine["who"] = "player";
    mine["text"] = said;
    mTestHistory.append(mine);
    mTestBusy = true;
    mTestTalk.mOn = true;
    mTestTalk.mTyping = true;
    mTestTalk.mYouSaid = said;
    mTestTalk.mName = cap(cast[mCastSel]["first"].asString() + " " + cast[mCastSel]["last"].asString(), 64);
    mTestTalk.mError.clear();
    mTestBox.clear();
    const std::string want = key;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §5.5 mind_test {adventure, cast, beat, text, history?} (reply fields: p2brief)
    WolfAdventure::api("mind_test", body, [handle, want](bool ok, const LLSD& r, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mTestBusy = false;
        if (self->mTestCastFor != want) return;   // the character or beat changed while it was out
        WolfAdventure::Talk& t = self->mTestTalk;
        t.mTyping = false;
        if (!ok)
        {
            t.mError = "Not answered: " + error;
            self->setStatus("Talk to them now: " + error, true);
            return;
        }
        t.mOn = true;
        t.mName = cap(r["name"].asString(), 64);
        t.mTitle = cap(r["title"].asString(), 64);
        const std::string portrait = r["portrait"].asString();
        t.mPortrait = portrait.size() == 36 && LLUUID::validate(portrait) && LLUUID(portrait).notNull() ? portrait : std::string();
        const LLSD& line = r["line"];
        t.mHasLine = line.isMap();
        t.mText = WolfGame::cleanText(line["text"].asString(), 1000, true);
        t.mEmote = cap(line["emote"].asString(), 240);
        t.mAi = line["ai"].asInteger() != 0;
        t.mCites.clear();
        for (size_t i = 0; line["cites"].isArray() && i < (size_t)line["cites"].size() && i < 8; ++i)
        {
            t.mCites.push_back({ cap(line["cites"][(LLSD::Integer)i]["title"].asString(), 64), cap(line["cites"][(LLSD::Integer)i]["where"].asString(), 240) });
        }
        t.mHint = cap(r["hint"].asString(), 240);
        t.mHasMood = r["mood"].isMap();
        t.mMoodLabel = cap(r["mood"]["label"].asString(), 24);
        t.mMoodBand = cap(r["mood"]["band"].asString(), 24);
        t.mFree = r["free"].asInteger() != 0;
        // what the character chose to do, and whether the brain would let it (§5.2): the author sees it
        const LLSD& act = r["action"];
        const std::string type = act["type"].asString();
        if (!type.empty() && type != "none")
        {
            std::string what = "The character chose: " + cap(type, 24) + (act["id"].asString().empty() ? std::string() : " " + cap(act["id"].asString(), 48));
            what += act["accepted"].asInteger() != 0 ? " - the story would accept it." : " - the story would refuse it" + (act["why"].asString().empty() ? std::string(".") : ": " + cap(act["why"].asString(), 160));
            t.mNotice = what;
        }
        else
        {
            t.mNotice.clear();
        }
        const std::string flags = cap(r["flags"].asString(), 64);
        if (!flags.empty()) t.mNotice += (t.mNotice.empty() ? std::string() : "  ") + "(" + flags + ")";
        self->mTestReplies.clear();
        for (size_t i = 0; r["replies"].isArray() && i < (size_t)r["replies"].size() && i < 6; ++i)
        {
            const LLSD& rp = r["replies"][(LLSD::Integer)i];
            self->mTestReplies.push_back({ cap(rp["id"].asString(), 64), cap(rp["label"].asString(), 24) });
        }
        self->mTestLastMicro = r["cost"]["micro"].asInteger();
        self->mTestTotalMicro += self->mTestLastMicro;
        if (t.mHasLine && !t.mText.empty())
        {
            LLSD said;
            said["who"] = "npc";
            said["text"] = t.mText;
            self->mTestHistory.append(said);
        }
        self->mTestVoiceUrl = r["voiceUrl"].asString();
        t.mVoiced = !self->mTestVoiceUrl.empty();
        if (t.mVoiced)
        {
            WolfAdventure::playClip(self->mTestVoiceUrl, [handle](const std::string& err)
            {
                if (handle.isDead() || err.empty()) return;
                handle.get()->setStatus("The spoken line could not be played: " + err, true);
            });
        }
        self->loadBudget();   // today's spend moved
    });
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// Book and assist (STUDIO §2.10 "Give it a book", WOLF_MIND_V1.md §10-11)
// ═══════════════════════════════════════════════════════════════════════════════════════

F32 WolfAdventureStudioView::pageBook(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (mOpenId.empty()) return para(l, r, y, "Open an adventure on Home first (or make a new one): the draft from a book goes into it.", p.mDim);
    if (!openIsOwner()) return para(l, r, y, "Only the adventure's owner can draft it from a book or ask the assistant.", p.mDim);
    const bool idle = mBusy == 0;
    // ---- Give it a book ----
    y = head(l, r, y, "Give it a book", p);
    y = para(l, r, y, "A public-domain book or your own outline becomes a draft: characters, places, acts, scenes and beats for you to edit. "
                      "Nothing goes live until you publish it.", p.mDim, true) - 6.f;
    btn(l, y - 28.f, l + 190.f, y, "Choose a text file...", SH_BOOK_FILE, 0, "A .txt, .md or .html file (at most 11 MB)", p, idle);
    btn(l + 198.f, y - 28.f, l + 388.f, y, "Paste from clipboard", SH_BOOK_PASTE, 0, "Use the text on the clipboard", p, idle);
    y -= 36.f;
    if (!mBookText.empty())
    {
        const S32 chars = (S32)utf8str_to_wstring(mBookText).size();
        textL(bold, (mBookName.empty() ? std::string("Pasted text") : mBookName) + " - " + thousands(chars) + " characters", l, y, OK_INK, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 2.f;
        std::string start = mBookText.substr(0, 400);
        start = WolfAdventure::cutUtf8(start, 300);
        for (char& ch : start) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
        y = para(l, r, y, start + "...", p.mDim, true) - 6.f;
    }
    else
    {
        y = para(l, r, y, "No text yet.", p.mDim, true) - 6.f;
    }
    field(l, y - 28.f, l + (r - l) * 0.5f - 4.f, y, mBookTitle, "Title of the work", F_BOOK_TITLE, p);
    field(l + (r - l) * 0.5f + 4.f, y - 28.f, r, y, mBookAuthor, "Author", F_BOOK_AUTHOR, p);
    y -= 36.f;
    stepper(l, y, 160.f, "Acts", plural(mBookActs, "act", "acts"), SH_BOOK_ACTS, 0, mBookActs > ACTS_MIN, mBookActs < ACTS_MAX, "how many acts to split it into", p);
    y -= 34.f;
    // the region: the one you stand on, or one of the adventure's (as Import)
    textL(bold, caps("Region"), l, y, p.mDim);
    y -= slh + 4.f;
    {
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
        if (mBookRegion.empty() && !choices.empty()) mBookRegion = choices.front().first;
        F32 x = l;
        for (size_t k = 0; k < choices.size() && k < 12; ++k)
        {
            const F32 w = bold->getWidthF32(choices[k].second) + 22.f;
            if (x > l && x + w > r) { x = l; y -= 30.f; }
            const bool on = mBookRegion == choices[k].first;
            btn(x, y - 24.f, x + w, y, choices[k].second, SH_BOOK_REGION, (S32)k, "The draft's places go on this region" + std::string(on ? " (chosen)" : ""), p, true, on ? BTN_ACTIVE : 0);
            x += w + 6.f;
        }
        y -= 32.f;
    }
    // §10.1 the rights statement (a tick AND the words)
    btn(l, y - 28.f, r, y, std::string(mBookRights ? "[x] " : "[ ] ") + "This text is in the public domain, or I own the rights to it", SH_BOOK_RIGHTS, 0,
        "Required: the draft records it in its attribution", p, true, mBookRights ? BTN_ACTIVE : 0);
    y -= 36.f;
    stepper(l, y, 200.f, "Spend at most", plural(mBookMax, "credit", "credits") + llformat(" (US $%.2f)", (F64)mBookMax / 100.0), SH_BOOK_MAX, 0,
            mBookMax > 10, mBookMax < BOOK_CREDITS_MAX, "the most this draft may cost", p);
    y -= 34.f;
    const bool ready = !mBookText.empty() && mBookRights && !mBookRegion.empty();
    btn(l, y - 30.f, l + 180.f, y, "Get a quote", SH_BOOK_QUOTE, 0,
        ready ? std::string("Count the text and quote the cost (nothing is charged)") : std::string("Add the text, tick the rights statement and pick the region first"),
        p, ready && idle);
    if (!mBookText.empty() && !mBookRights) textL(small, "Tick the rights statement to go on.", l + 190.f, y - 15.f, WARN_INK, LLFontGL::LEFT, LLFontGL::VCENTER);
    y -= 40.f;

    // ---- the job ----
    if (mJob.isMap() && !mJobId.empty())
    {
        y = head(l, r, y, "Drafting", p);
        const std::string state = mJob["state"].asString();
        const F64 quote = mJob["quote"]["credits"].asReal();
        std::string words;
        if (state == "quoting" || state == "quoted" || state.empty()) words = "Quoted: about " + creditsText(quote) + " for " + thousands((S64)mJob["quote"]["tokens"].asReal()) + " tokens of text.";
        else if (state == "queued") words = "Waiting to start...";
        else if (state == "running") words = "Working: " + cap(mJob["progress"].asString(), 200);
        else if (state == "done") words = "Done.";
        else if (state == "failed") words = "It stopped: " + cap(mJob["error"].asString(), 240);
        else if (state == "cancelled") words = "Stopped.";
        else words = cap(state, 24);
        y = para(l, r, y, words, state == "failed" ? DANGER_INK : (state == "done" ? OK_INK : p.mText)) - 2.f;
        if (mJob["cost"].isMap()) y = para(l, r, y, "Spent so far: " + creditsText(mJob["cost"]["credits"].asReal()) + " of at most " + plural(mJob["maxCredits"].isDefined() ? mJob["maxCredits"].asInteger() : mBookMax, "credit", "credits"), p.mDim, true) - 4.f;
        const bool quoted = state == "quoting" || state == "quoted" || state.empty();
        if (quoted)
        {
            btn(l, y - 30.f, l + 220.f, y, llformat("Start (at most %d credits)...", mBookMax), SH_BOOK_START, 0, "Asks first, then drafts", p, idle);
            y -= 38.f;
        }
        if (state == "queued" || state == "running")
        {
            btn(l, y - 28.f, l + 160.f, y, "Stop drafting...", SH_JOB_CANCEL, 0, "Stop it (asks first)", p, idle, BTN_DANGER);
            y -= 36.f;
        }
        // §10.3 the report
        const LLSD& report = mJob["report"];
        for (size_t i = 0; report.isArray() && i < (size_t)report.size() && i < 40; ++i)
        {
            y = para(l, r, y, cap(report[(LLSD::Integer)i].asString(), 500), p.mText, true) - 2.f;
        }
        if (state == "done" && mJob["output"].isMap())
        {
            y -= 4.f;
            btn(l, y - 30.f, l + 200.f, y, "Use as draft...", SH_JOB_USE, 0, "Replace the draft of " + mOpenTitle + " with it (asks first)", p, idle);
            y -= 40.f;
        }
        y -= 6.f;
    }
    // ---- my drafting jobs ----
    if (mJobs.isArray() && mJobs.size() > 0)
    {
        y = head(l, r - 100.f, y, "My drafts from books", p);
        btn(r - 90.f, y + slh + 10.f - 24.f, r, y + slh + 10.f, "Refresh", SH_JOBS_REFRESH, 0, "Load the list again", p, idle);
        for (size_t i = 0; i < (size_t)mJobs.size() && i < 12; ++i)
        {
            const LLSD& j = mJobs[(LLSD::Integer)i];
            const std::string line = cap(j["state"].asString(), 24) + " - " + whenText(j["created"]) + " - " + creditsText(j["cost"]["credits"].asReal())
                                   + (j["progress"].asString().empty() ? std::string() : " - " + cap(j["progress"].asString(), 80));
            textL(small, line, l, y - 4.f, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l - 110.f);
            btn(r - 100.f, y - 24.f, r, y, "Open", SH_JOB_OPEN, (S32)i, "Show this drafting job", p, idle);
            y -= 30.f;
        }
        y -= 6.f;
    }

    // ---- the assistant (WOLF_MIND_V1.md §11, on the draft) ----
    y = head(l, r, y, "Ask the assistant", p);
    y = para(l, r, y, "Ideas for you only: players never see them. Each answer is charged to your WolfCredits and counts against your assistant cap.", p.mDim, true) - 6.f;
    {
        static const char* ASKS[3] = { "What could happen next?", "Write boxed text for this scene", "Suggest a reply for..." };
        F32 x = l;
        for (S32 i = 0; i < 3; ++i)
        {
            const F32 w = bold->getWidthF32(ASKS[i]) + 22.f;
            if (x > l && x + w > r) { x = l; y -= 30.f; }
            btn(x, y - 24.f, x + w, y, ASKS[i], SH_ASSIST_ASK, i, ASKS[i], p, true, mAssistAsk == i ? BTN_ACTIVE : 0);
            x += w + 6.f;
        }
        y -= 32.f;
    }
    const std::vector<std::pair<std::string, std::string>> beats = draftBeats();
    if (!beats.empty())
    {
        mBeatSel = llclamp(mBeatSel, 0, (S32)beats.size() - 1);
        btn(l, y - 24.f, l + 30.f, y, "<", SH_BEAT_STEP, 0, "The beat before", p, mBeatSel > 0);
        btn(l + 36.f, y - 24.f, l + 66.f, y, ">", SH_BEAT_STEP, 1, "The next beat", p, mBeatSel + 1 < (S32)beats.size());
        textL(small, std::string(mAssistAsk == 1 ? "Scene of beat " : "Beat ") + beats[mBeatSel].first + " - " + beats[mBeatSel].second, l + 76.f, y - 12.f, p.mText,
              LLFontGL::LEFT, LLFontGL::VCENTER, r - l - 76.f);
        y -= 32.f;
    }
    if (mAssistAsk == 2)
    {
        const LLSD& cast = std::as_const(mDraftPack)["cast"];
        if (cast.isArray() && cast.size() > 0)
        {
            mCastSel = llclamp(mCastSel, 0, (S32)cast.size() - 1);
            F32 x = l;
            for (size_t i = 0; i < (size_t)cast.size() && i < 40; ++i)
            {
                const std::string name = cap(cast[(LLSD::Integer)i]["first"].asString() + " " + cast[(LLSD::Integer)i]["last"].asString(), 64);
                const F32 w = llmin(200.f, bold->getWidthF32(name) + 22.f);
                if (x > l && x + w > r) { x = l; y -= 30.f; }
                btn(x, y - 24.f, x + w, y, name, SH_CAST_PICK, (S32)i, "Suggest " + name + "'s reply", p, true, (S32)i == mCastSel ? BTN_ACTIVE : 0);
                x += w + 6.f;
            }
            y -= 32.f;
        }
        field(l, y - 28.f, r, y, mAssistBox, "The player's line to answer", F_ASSIST, p);
        y -= 36.f;
    }
    btn(l, y - 30.f, l + 140.f, y, mAssistBusy ? "Thinking..." : "Ask", SH_ASSIST_RUN, 0, "Ask the assistant (charged)", p, !mAssistBusy && !beats.empty());
    if (!mAssistCost.empty()) textL(small, mAssistCost, l + 150.f, y - 15.f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
    y -= 40.f;
    for (size_t i = 0; mAssist.isArray() && i < (size_t)mAssist.size() && i < 6; ++i)
    {
        const LLSD& sg = mAssist[(LLSD::Integer)i];
        textL(bold, "AI suggestion; players have not seen this", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l - 100.f);
        btn(r - 90.f, y + 2.f - 22.f, r, y + 2.f, "Copy", SH_ASSIST_COPY, (S32)i, "Copy this suggestion to the clipboard", p);
        y -= slh + 4.f;
        if (!sg["warning"].asString().empty()) y = para(l, r, y, "Warning: " + cap(sg["warning"].asString(), 200), WARN_INK, true) - 2.f;
        y = para(l, r, y, WolfGame::cleanText(sg["text"].asString(), 4000, true), p.mText, true) - 10.f;
    }
    return y;
}

void WolfAdventureStudioView::readBookFile()
{
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // Source: wolffloateradventurestudio.cpp choosePackFile - the viewer's own file picker
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
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size <= 0 || (U64)size > BOOK_MAX_BYTES)
            {
                self->setStatus("That file is empty or larger than 11 MB.", true);
                return;
            }
            in.seekg(0, std::ios::beg);
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string text = ss.str();
            if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
            if (!validUtf8(text))
            {
                self->setStatus("That file is not UTF-8 text. Save it as UTF-8 (plain text) and choose it again.", true);
                return;
            }
            std::string ext = gDirUtilp->getExtension(path);
            LLStringUtil::toLower(ext);
            if (ext == "html" || ext == "htm") text = htmlToText(text);
            self->mBookText = text;
            self->mBookName = gDirUtilp->getBaseFileName(path);
            self->mJobId.clear();
            self->mJob = LLSD();
            self->setStatus("Read " + self->mBookName + ". Fill in the title and author, tick the rights statement, then Get a quote.", false);
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void WolfAdventureStudioView::bookQuote()
{
    if (mBookText.empty() || !mBookRights || mBookRegion.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    body["text"] = mBookText;
    body["acts"] = mBookActs;
    body["region"] = mBookRegion;
    body["rights"] = true;
    body["maxCredits"] = mBookMax;
    if (!mBookTitle.empty()) body["title"] = mBookTitle;
    if (!mBookAuthor.empty()) body["author"] = mBookAuthor;
    // BRAIN_API_V1.md §5.5 draft_start {adventure, text, acts, region, rights, maxCredits} -> {job, quote:{tokens, credits}}
    call("draft_start", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mJobId = reply["job"].asString();
        self->mJob = LLSD::emptyMap();
        self->mJob["state"] = "quoted";
        self->mJob["quote"] = reply["quote"];
        const F64 credits = reply["quote"]["credits"].asReal();
        self->setStatus("Quoted: about " + creditsText(credits) + ". Press Start to draft it.", false);
        if (credits > (F64)self->mBookMax)
        {
            self->setStatus("The quote (" + creditsText(credits) + ") is more than you allowed: raise Spend at most, or use fewer acts.", true);
        }
    }, "Counting the text and quoting...");
}

void WolfAdventureStudioView::bookStart()
{
    if (mJobId.empty()) return;
    LLSD body;
    body["job"] = mJobId;
    body["confirm"] = true;
    call("draft_start", body, [](WolfAdventureStudioView* self, const LLSD& reply)
    {
        self->mJob["state"] = reply["state"].asString().empty() ? std::string("queued") : reply["state"].asString();
        self->mJobPollAt = now() + JOB_POLL_SEC;
        self->setStatus("Drafting has started. You can close the Studio: it carries on, and the result waits here.", false);
        self->loadJobs();
    }, "Starting...");
}

void WolfAdventureStudioView::pollJob()
{
    mJobPollAt = 0.0;
    if (mJobId.empty()) return;
    LLSD body;
    body["job"] = mJobId;
    const std::string id = mJobId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // §5.5 job {job} -> {state, progress, cost, output?, report?}
    WolfAdventure::api("job", body, [handle, id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mJobId != id) return;
        if (!ok)
        {
            self->setStatus("The drafting job could not be read: " + error, true);
            self->mJobPollAt = now() + JOB_POLL_SEC * 3.0;   // try again, more slowly
            return;
        }
        self->mJob = reply;
        const std::string state = reply["state"].asString();
        if (state == "queued" || state == "running") self->mJobPollAt = now() + JOB_POLL_SEC;
        else if (state == "done") self->setStatus("The draft is ready: read the report, then Use as draft.", false);
        else if (state == "failed") self->setStatus("Drafting stopped: " + cap(reply["error"].asString(), 240), true);
    });
}

void WolfAdventureStudioView::loadJobs()
{
    LLSD body;
    if (!mOpenId.empty()) body["adventure"] = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    WolfAdventure::api("jobs", body, [handle](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (ok) self->mJobs = reply["jobs"].isArray() ? reply["jobs"] : LLSD::emptyArray();
        else self->setStatus("Your drafting jobs could not be listed: " + error, true);
    });
}

void WolfAdventureStudioView::assistRun()
{
    if (mAssistBusy || mOpenId.empty()) return;
    const std::vector<std::pair<std::string, std::string>> beats = draftBeats();
    if (beats.empty()) return;
    static const char* ASK[3] = { "next", "boxed", "reply" };
    const S32 a = llclamp(mAssistAsk, 0, 2);
    LLSD body;
    body["adventure"] = mOpenId;
    body["ask"] = ASK[a];
    const std::string beat = beats[llclamp(mBeatSel, 0, (S32)beats.size() - 1)].first;
    body["beat"] = beat;
    body["scene"] = sceneOfBeat(beat);
    if (a == 2)
    {
        const LLSD& cast = std::as_const(mDraftPack)["cast"];
        if (!cast.isArray() || cast.size() == 0) return;
        std::string text = mAssistBox;
        LLStringUtil::trim(text);
        if (text.empty())
        {
            setStatus("Type the player's line for the assistant to answer.", true);
            mField = F_ASSIST;
            return;
        }
        body["cast"] = cast[llclamp(mCastSel, 0, (S32)cast.size() - 1)]["id"].asString();
        body["text"] = text;
    }
    mAssistBusy = true;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    setStatus("Asking the assistant (this can take a little while)...", false);
    // p2brief studio_assist {adventure, ask, beat?, scene?, cast?, text?} -> {suggestions, cost}
    WolfAdventure::api("studio_assist", body, [handle](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mAssistBusy = false;
        if (!ok)
        {
            self->setStatus("The assistant could not answer: " + error, true);
            return;
        }
        self->mAssist = reply["suggestions"].isArray() ? reply["suggestions"] : LLSD::emptyArray();
        self->mAssistCost = "This cost " + creditsText(reply["cost"]["credits"].asReal());
        self->setStatus(self->mAssist.size() ? std::string("The assistant's ideas are below.") : std::string("The assistant had nothing to suggest."), false);
    });
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// Run: the GM Console Lite (STUDIO §3, BRAIN_API_V1.md §8)
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfAdventureStudioView::loadSessions()
{
    if (mOpenId.empty()) return;
    LLSD body;
    body["adventure"] = mOpenId;
    const std::string id = mOpenId;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // p2brief sessions {adventure} -> {sessions:[...]} (may_gm)
    WolfAdventure::api("sessions", body, [handle, id](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        if (self->mOpenId != id) return;
        if (!ok)
        {
            self->mGmError = error;
            return;
        }
        self->mGmError.clear();
        self->mSessions = reply["sessions"].isArray() ? reply["sessions"] : LLSD::emptyArray();
        // the first open session is chosen when none is
        bool still = false;
        for (size_t i = 0; i < (size_t)self->mSessions.size(); ++i) still = still || self->mSessions[(LLSD::Integer)i]["session"].asString() == self->mRunSession;
        if (!still)
        {
            self->mRunSession = self->mSessions.size() ? self->mSessions[0]["session"].asString() : std::string();
            self->mGm = LLSD();
            self->mRunSuggest = LLSD();
        }
        if (!self->mRunSession.empty()) self->pollGmState(true);
    });
}

void WolfAdventureStudioView::pollGmState(bool force)
{
    if (mRunSession.empty() || (mGmLoading && !force)) return;
    mGmLoading = true;
    mGmPollAt = now() + GM_POLL_SEC;
    LLSD body;
    body["session"] = mRunSession;
    const std::string sess = mRunSession;
    LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
    // BRAIN_API_V1.md §8.2 gm_state {session}
    WolfAdventure::api("gm_state", body, [handle, sess](bool ok, const LLSD& reply, const std::string& error)
    {
        if (handle.isDead()) return;
        WolfAdventureStudioView* self = handle.get();
        self->mGmLoading = false;
        self->mGmPollAt = now() + GM_POLL_SEC;
        if (self->mRunSession != sess) return;
        if (ok)
        {
            self->mGm = reply;
            self->mGmError.clear();
        }
        else
        {
            self->mGmError = error;
            self->mGmPollAt = now() + GM_POLL_SEC * 5.0;   // a refusal is not asked again every 2 s
        }
    });
}

void WolfAdventureStudioView::gmOp(const std::string& op, const LLSD& extra, const std::string& doing)
{
    if (mRunSession.empty()) return;
    LLSD body = extra.isMap() ? extra : LLSD::emptyMap();
    body["session"] = mRunSession;
    body["op"] = op;
    // BRAIN_API_V1.md §8 gm {session, op, ...} -> {log}
    call("gm", body, [op](WolfAdventureStudioView* self, const LLSD& reply)
    {
        if (op == "assist")
        {
            self->mRunSuggest = reply["suggestions"].isArray() ? reply["suggestions"] : LLSD::emptyArray();
            self->mRunSuggestCost = "This cost " + creditsText(reply["cost"]["credits"].asReal());
            self->setStatus("The assistant's ideas are below. Players have not seen them.", false);
        }
        else
        {
            const std::string words = op == "take" ? "You are running it now. The AI Director stands back."
                                    : op == "leave" ? "Handed back: the AI Director carries on from here."
                                    : op == "pause" ? "Paused." : op == "resume" ? "Running again." : "Done.";
            self->setStatus(words, false);
        }
        self->pollGmState(true);
    }, op == "assist" ? std::string("Asking the assistant...") : doing);
}

F32 WolfAdventureStudioView::pageRun(F32 l, F32 r, F32 top, const Palette& p)
{
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* bold = LLFontGL::getFontSansSerifSmallBold();
    const F32 slh = (F32)small->getLineHeight();
    F32 y = top;
    if (mOpenId.empty()) return para(l, r, y, "Open an adventure on Home first. You can run the adventures you own or are a game master of.", p.mDim);
    const bool idle = mBusy == 0;
    y = head(l, r - 100.f, y, "Sessions", p);
    btn(r - 90.f, y + slh + 10.f - 24.f, r, y + slh + 10.f, "Refresh", SH_RUN_REFRESH, 0, "Look for sessions again", p, idle);
    if (!mGmError.empty()) y = para(l, r, y, "Not available: " + mGmError, DANGER_INK) - 6.f;
    if (!mSessions.isArray() || mSessions.size() == 0)
    {
        return para(l, r, y, "No session is running. An adventure's session opens by itself when the first player joins it.", p.mDim) - 8.f;
    }
    for (size_t i = 0; i < (size_t)mSessions.size() && i < 12; ++i)
    {
        const LLSD& s = mSessions[(LLSD::Integer)i];
        const bool on = s["session"].asString() == mRunSession;
        const std::string mine_gm = s["gm"].asString();
        const std::string label = cap(s["kind"].asString(), 16) + " session - " + controlWords(s["control"].asString(), cap(s["gmName"].asString(), 64), mine_gm == gAgentID.asString())
                                + llformat(" - %d players, %d parties", s["players"].asInteger(), s["parties"].asInteger());
        btn(l, y - 26.f, r, y, label, SH_RUN_SESSION, (S32)i, "Run this session", p, true, on ? BTN_ACTIVE : 0);
        y -= 32.f;
    }
    y -= 4.f;
    if (!mGm.isMap())
    {
        return para(l, r, y, mGmLoading ? "Loading the session..." : "Choose a session.", p.mDim) - 8.f;
    }
    const LLSD& ss = mGm["session"];
    const std::string control = ss["control"].asString();
    const bool mine = control == "live" && ss["gm"].asString() == gAgentID.asString();
    const bool ai_paused = ss["aiPaused"].asInteger() != 0;
    // ---- the control state as words, and its buttons (§3 Top bar) ----
    y = head(l, r, y, "Control", p);
    textL(display(18.f), controlWords(control, cap(ss["gmName"].asString(), 64), mine), l, y, mine ? OK_INK : p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l);
    y -= (F32)display(18.f)->getLineHeight() + 6.f;
    {
        F32 x = l;
        if (!mine)
        {
            btn(x, y - 30.f, x + 170.f, y, "Take control...", SH_RUN_TAKE, 0, "Run it yourself; the AI Director stands back (asks first)", p, idle, BTN_PRIMARY);
            x += 178.f;
        }
        else
        {
            btn(x, y - 30.f, x + 220.f, y, "Hand back to the AI Director", SH_RUN_LEAVE, 0, "The AI Director picks up from exactly here", p, idle, BTN_PRIMARY);
            x += 228.f;
        }
        if (control == "paused") btn(x, y - 30.f, x + 110.f, y, "Resume", SH_RUN_RESUME, 0, "Let the story run again", p, idle);
        else btn(x, y - 30.f, x + 110.f, y, "Pause", SH_RUN_PAUSE, 0, "Pause timers, hints and the Director; players see Paused", p, idle);
        x += 118.f;
        toggle(x, y, 150.f, "AI", !ai_paused, SH_RUN_AI, 0, "Characters' AI in this session (off: written lines only)", p, idle);
        y -= 40.f;
    }
    // ---- the AI meter ----
    const LLSD& ai = mGm["ai"];
    if (ai.isMap())
    {
        std::string mode = cap(ai["modeText"].asString(), 120);
        if (mode.empty()) mode = ai["mode"].asString() == "ai" ? "AI" : "Written lines only";
        textL(small, "AI now: " + mode + " \xC2\xB7 " + creditsText(ai["todayCredits"].asReal()) + llformat(" of %d today", ai["capCredits"].asInteger())
                     + (ai.has("turnsLeft") ? llformat(" \xC2\xB7 %d turns left", ai["turnsLeft"].asInteger()) : std::string()),
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 8.f;
    }
    // ---- parties ----
    const LLSD& parties = mGm["parties"];
    y = head(l, r, y, "Parties", p);
    if (!parties.isArray() || parties.size() == 0) y = para(l, r, y, "No party is playing now.", p.mDim, true) - 4.f;
    for (size_t i = 0; parties.isArray() && i < (size_t)parties.size() && i < 20; ++i)
    {
        const LLSD& pt = parties[(LLSD::Integer)i];
        std::string members;
        const LLSD& ms = pt["members"];
        for (size_t k = 0; ms.isArray() && k < (size_t)ms.size() && k < 16; ++k)
        {
            members += (members.empty() ? std::string() : std::string(", ")) + cap(ms[(LLSD::Integer)k]["name"].asString(), 40)
                     + (ms[(LLSD::Integer)k]["ready"].asInteger() ? " (ready)" : "");
        }
        textL(bold, "Led by " + cap(pt["leaderName"].asString(), 64) + (pt["paused"].asInteger() ? " - paused" : "") + (pt["ending"].asString().empty() ? std::string() : " - finished"),
              l, y, p.mText, LLFontGL::LEFT, LLFontGL::TOP, r - l);
        y -= slh + 1.f;
        y = para(l, r, y, "Beat " + cap(pt["beat"].asString(), 48) + ": " + cap(pt["objective"].asString(), 240), p.mText, true);
        if (!members.empty()) y = para(l, r, y, "Members: " + members, p.mDim, true);
        y -= 6.f;
    }
    // ---- Assist (§3 Assist tab: What could happen next?) ----
    y = head(l, r, y, "Assist", p);
    btn(l, y - 28.f, l + 230.f, y, "What could happen next?", SH_RUN_ASSIST, 0, "Three ways the scene could go (charged; only you see them)", p,
        idle && parties.isArray() && parties.size() > 0);
    if (!mRunSuggestCost.empty()) textL(small, mRunSuggestCost, l + 240.f, y - 14.f, p.mDim, LLFontGL::LEFT, LLFontGL::VCENTER);
    y -= 36.f;
    for (size_t i = 0; mRunSuggest.isArray() && i < (size_t)mRunSuggest.size() && i < 6; ++i)
    {
        const LLSD& sg = mRunSuggest[(LLSD::Integer)i];
        textL(bold, "AI suggestion; players have not seen this", l, y, p.mAccent, LLFontGL::LEFT, LLFontGL::TOP, r - l - 100.f);
        btn(r - 90.f, y + 2.f - 22.f, r, y + 2.f, "Copy", SH_RUN_COPY, (S32)i, "Copy this suggestion", p);
        y -= slh + 4.f;
        if (!sg["warning"].asString().empty()) y = para(l, r, y, "Warning: " + cap(sg["warning"].asString(), 200), WARN_INK, true) - 2.f;
        y = para(l, r, y, WolfGame::cleanText(sg["text"].asString(), 4000, true), p.mText, true) - 8.f;
    }
    // ---- the log (newest first) ----
    y = head(l, r, y, "Log", p);
    const LLSD& log = mGm["log"];
    if (!log.isArray() || log.size() == 0) y = para(l, r, y, "Nothing has happened yet.", p.mDim, true) - 4.f;
    for (size_t i = 0; log.isArray() && i < (size_t)log.size() && i < 50; ++i)
    {
        const LLSD& e = log[(LLSD::Integer)i];
        const std::string head_text = clockText(e["at"]) + "  " + sourceWord(e["source"].asString());
        textL(bold, head_text, l, y, p.mDim, LLFontGL::LEFT, LLFontGL::TOP, 150.f);
        const std::string text = cap(e["text"].asString().empty() ? e["type"].asString() : e["text"].asString(), 500);
        y = para(l + 156.f, r, y, text, p.mText, true) - 2.f;
    }
    return y - 8.f;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// presses
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfAdventureStudioView::pressP2(S32 id, S32 arg)
{
    switch (id)
    {
    // ---- AI and voice ----
    case SH_AI_MODE:
        editDraft([arg](boost::json::object& o) { objectAt(o, "ai")["mode"] = arg == 0 ? "ai" : "none"; });
        return true;
    case SH_AI_TOGGLE:
    {
        const std::string purpose = std::as_const(mDraftPack)["adventure"]["purpose"].asString();
        const bool edu = purpose == "education";
        const LLSD& ai = std::as_const(mDraftPack)["ai"];
        editDraft([arg, edu, ai](boost::json::object& o)
        {
            boost::json::object& a = objectAt(o, "ai");
            if (!a.contains("mode")) a["mode"] = "none";   // $defs.ai requires mode
            switch (arg)
            {
            case 0: a["freeChat"] = !(ai.has("freeChat") ? ai["freeChat"].asBoolean() : !edu); break;
            case 1: a["accuracy"] = !(ai.has("accuracy") ? ai["accuracy"].asBoolean() : edu); break;
            case 2: a["improvise"] = !ai["improvise"].asBoolean(); break;
            case 3: a["translate"] = !ai["translate"].asBoolean(); break;
            case 4: objectAt(a, "voice")["enabled"] = !ai["voice"]["enabled"].asBoolean(); break;
            default: break;
            }
        });
        return true;
    }
    case SH_AI_STEP:
    {
        const S32 which = arg / 2;
        const bool more = (arg % 2) != 0;
        const LLSD& ai = std::as_const(mDraftPack)["ai"];
        editDraft([which, more, ai](boost::json::object& o)
        {
            boost::json::object& a = objectAt(o, "ai");
            if (!a.contains("mode")) a["mode"] = "none";
            if (which == 0)
            {
                const S32 v = ai.has("dailyCapCredits") ? ai["dailyCapCredits"].asInteger() : 100;
                a["dailyCapCredits"] = llclamp(v + (more ? 10 : -10), 0, CAP_MAX);
            }
            else if (which == 1)
            {
                const S32 v = ai.has("playerDailyTurns") ? ai["playerDailyTurns"].asInteger() : 200;
                a["playerDailyTurns"] = llclamp(v + (more ? 10 : -10), 0, TURNS_MAX);
            }
            else
            {
                const S32 v = ai["voice"].has("dailyChars") ? ai["voice"]["dailyChars"].asInteger() : 50000;
                objectAt(a, "voice")["dailyChars"] = llclamp(v + (more ? 5000 : -5000), 0, CHARS_MAX);
            }
        });
        return true;
    }
    case SH_AI_REFRESH: loadBudget(); return true;
    case SH_AI_BUY:
    {
        // the credits page on our own site only (aicredits.php buyurl = WC_SITE_URL "/index.php?f=credits",
        // wolfcreditslib.php:39)
        const std::string url = mBudget["buyurl"].asString();
        if (url.rfind("https://www.wolf-grid.com/", 0) == 0 || url.rfind("https://wolf-grid.com/", 0) == 0) LLWeb::loadURLExternal(url);
        else setStatus("The credits page address did not come from Wolf Territories, so it was not opened.", true);
        return true;
    }
    case SH_OWNER_STEP:
    {
        static const char* KEYS[4] = { "dailyCeilingCredits", "assistCapCredits", "gmRewardMoney", "gmRewardXp" };
        static const S32 STEP[4] = { 50, 10, 100, 50 };
        static const S32 MAX[4] = { CAP_MAX, CAP_MAX, 1000000, 100000 };
        const S32 which = llclamp(arg / 2, 0, 3);
        const S32 v = mOwner[KEYS[which]].asInteger() + ((arg % 2) ? STEP[which] : -STEP[which]);
        mOwner[KEYS[which]] = llclamp(v, 0, MAX[which]);
        mOwnerDirty = true;
        return true;
    }
    case SH_OWNER_SAVE: saveOwnerSettings(); return true;
    case SH_CONFLICT_RELOAD:
        mConflict = false;
        mDirtyAt = 0.0;
        loadOpen();
        setStatus("Reloaded the other window's draft.", false);
        return true;
    case SH_CONFLICT_KEEP:
        mConflict = false;
        saveDraftNow(true);
        return true;
    // ---- Cast talk ----
    case SH_CAST_PICK:
        mCastSel = arg;
        return true;
    case SH_VOICE_PICK:
    {
        std::string vid;
        if (arg >= 0 && mVoices.isArray() && arg < (S32)mVoices.size()) vid = std::as_const(mVoices)[arg]["id"].asString();
        if (arg >= 0 && vid.empty()) return true;
        pickVoice(mCastSel, vid);   // [WOLF ADVENTURE P3 2026-10-16] premium voices confirm their cost first
        return true;
    }
    case SH_RATE_STEP:
    {
        const S32 c = mCastSel;
        const LLSD& cast = std::as_const(mDraftPack)["cast"];
        if (!cast.isArray() || c < 0 || c >= (S32)cast.size()) return true;
        F64 rate = cast[c]["voice"].has("rate") ? cast[c]["voice"]["rate"].asReal() : 1.0;
        rate = llclamp(floor((rate + ((arg % 2) ? RATE_STEP : -RATE_STEP)) * 100.0 + 0.5) / 100.0, RATE_MIN, RATE_MAX);
        editDraft([c, rate](boost::json::object& o)
        {
            boost::json::value& cv = o["cast"];
            if (!cv.is_array() || c < 0 || c >= (S32)cv.as_array().size() || !cv.as_array()[c].is_object()) return;
            boost::json::object& cs = cv.as_array()[c].as_object();
            if (!cs.contains("voice")) return;
            objectAt(cs, "voice")["rate"] = rate;
        });
        return true;
    }
    case SH_VOICE_PREVIEW:
        if (draftDirty()) saveDraftNow();
        previewVoice(arg);
        return true;
    case SH_BEAT_STEP:
        mBeatSel = llmax(0, mBeatSel + (arg ? 1 : -1));
        return true;
    case SH_TEST_AI: mTestAi = !mTestAi; return true;
    case SH_TEST_SEND: testSay(mTestBox, std::string()); return true;
    case SH_TEST_REPLY:
        if (arg >= 0 && arg < (S32)mTestReplies.size()) testSay(std::string(), mTestReplies[arg].first);
        return true;
    case SH_TEST_CLEAR:
        mTestHistory = LLSD::emptyArray();
        mTestTalk = WolfAdventure::Talk();
        mTestReplies.clear();
        mTestTotalMicro = mTestLastMicro = 0;
        mTestCastFor.clear();
        mTestVoiceUrl.clear();
        return true;
    case SH_TEST_PLAY:
    {
        LLHandle<WolfAdventureStudioView> handle = getDerivedHandle<WolfAdventureStudioView>();
        WolfAdventure::playClip(mTestVoiceUrl, [handle](const std::string& err)
        {
            if (handle.isDead() || err.empty()) return;
            handle.get()->setStatus("The line could not be played again (a clip lasts two minutes): " + err, true);
        });
        return true;
    }
    // ---- Book and assist ----
    case SH_BOOK_FILE: readBookFile(); return true;
    case SH_BOOK_PASTE:
    {
        LLWString pasted;
        if (!LLClipboard::instance().pasteFromClipboard(pasted) || pasted.empty())
        {
            setStatus("The clipboard has no text.", true);
            return true;
        }
        std::string text = wstring_to_utf8str(pasted);
        if (text.size() > BOOK_MAX_BYTES)
        {
            setStatus("The text on the clipboard is larger than 11 MB.", true);
            return true;
        }
        mBookText = text;
        mBookName.clear();
        mJobId.clear();
        mJob = LLSD();
        setStatus("Pasted " + thousands((S64)pasted.size()) + " characters. Fill in the title and author, tick the rights statement, then Get a quote.", false);
        return true;
    }
    case SH_BOOK_ACTS: mBookActs = llclamp(mBookActs + ((arg % 2) ? 1 : -1), ACTS_MIN, ACTS_MAX); return true;
    case SH_BOOK_REGION:
    {
        std::vector<std::string> ids;
        if (LLViewerRegion* rgn = gAgent.getRegion()) ids.push_back(rgn->getRegionID().asString());
        if (const LLSD* a = listed(mOpenId))
        {
            const LLSD& rs = (*a)["regions"];
            for (size_t k = 0; rs.isArray() && k < (size_t)rs.size(); ++k)
            {
                const std::string rid = rs[(LLSD::Integer)k]["region"].asString();
                if (std::find(ids.begin(), ids.end(), rid) == ids.end()) ids.push_back(rid);
            }
        }
        if (arg >= 0 && arg < (S32)ids.size()) mBookRegion = ids[arg];
        return true;
    }
    case SH_BOOK_RIGHTS: mBookRights = !mBookRights; return true;
    case SH_BOOK_MAX: mBookMax = llclamp(mBookMax + ((arg % 2) ? 10 : -10), 10, BOOK_CREDITS_MAX); return true;
    case SH_BOOK_QUOTE: bookQuote(); return true;
    case SH_BOOK_START: ask(SC_JOB_START, 0); return true;
    case SH_JOB_CANCEL: ask(SC_JOB_CANCEL, 0); return true;
    case SH_JOB_OPEN:
        if (mJobs.isArray() && arg >= 0 && arg < (S32)mJobs.size())
        {
            mJobId = mJobs[arg]["job"].asString();
            mJob = LLSD();
            pollJob();
        }
        return true;
    case SH_JOB_USE:
    {
        const LLSD& out = mJob["output"];
        if (!out.isMap()) return true;
        // the draft travels as its own JSON text (wolfadventure.cpp api: a job's `output` comes as
        // output_json too, never narrowed through LLSD's 32-bit integers)
        mImportedPack = out;
        mImportedJson = mJob["output_json"].asString();
        if (mImportedJson.empty())
        {
            setStatus("The draft did not come through whole - press Open on the job again.", true);
            return true;
        }
        mReport = mJob["report"].isArray() ? mJob["report"] : LLSD::emptyArray();
        // [WOLF STORY FIRST 2026-10-17] into the open adventure by a merge (job_apply: the author's own work is kept)
        if (!mOpenId.empty()) jobApply();
        else useAsDraft();
        return true;
    }
    case SH_JOBS_REFRESH: loadJobs(); return true;
    case SH_ASSIST_ASK: mAssistAsk = llclamp(arg, 0, 2); return true;
    case SH_ASSIST_RUN: assistRun(); return true;
    case SH_ASSIST_COPY:
    case SH_RUN_COPY:
    {
        const LLSD& list = id == SH_ASSIST_COPY ? mAssist : mRunSuggest;
        if (list.isArray() && arg >= 0 && arg < (S32)list.size())
        {
            const LLWString text = utf8str_to_wstring(list[arg]["text"].asString());
            LLClipboard::instance().copyToClipboard(text, 0, (S32)text.size());
            setStatus("Copied.", false);
        }
        return true;
    }
    // ---- Run ----
    case SH_RUN_SESSION:
        if (mSessions.isArray() && arg >= 0 && arg < (S32)mSessions.size())
        {
            mRunSession = mSessions[arg]["session"].asString();
            mGm = LLSD();
            mRunSuggest = LLSD();
            pollGmState(true);
        }
        return true;
    case SH_RUN_REFRESH: loadSessions(); return true;
    case SH_RUN_TAKE: ask(SC_TAKE, 0); return true;
    case SH_RUN_LEAVE: gmOp("leave", LLSD::emptyMap(), "Handing back..."); return true;
    case SH_RUN_PAUSE: gmOp("pause", LLSD::emptyMap(), "Pausing..."); return true;
    case SH_RUN_RESUME: gmOp("resume", LLSD::emptyMap(), "Resuming..."); return true;
    case SH_RUN_AI:
    {
        LLSD extra;
        const bool paused = mGm["session"]["aiPaused"].asInteger() != 0;
        extra["on"] = paused ? 1 : 0;   // §8.1 ai {on}: pauses or resumes all AI in the session
        gmOp("ai", extra, paused ? "Switching AI on..." : "Switching AI off...");
        return true;
    }
    case SH_RUN_ASSIST:
    {
        const LLSD& parties = mGm["parties"];
        if (!parties.isArray() || parties.size() == 0) return true;
        LLSD extra;
        extra["ask"] = "next";
        LLSD ctx;
        ctx["party"] = parties[0]["party"].asString();
        extra["context"] = ctx;
        gmOp("assist", extra, std::string());
        return true;
    }
    default:
        return false;
    }
}
