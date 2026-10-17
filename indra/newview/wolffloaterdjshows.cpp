/**
 * @file wolffloaterdjshows.cpp
 * @brief WolfViewer Wolf DJ "My Shows" (2026-10-16). See wolffloaterdjshows.h.
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterdjshows.h"

#include <algorithm>

#include "llagent.h"
#include "llbutton.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfontgl.h"
#include "lllocalcliprect.h"
#include "llrender2dutils.h"
#include "lltextbox.h"
#include "llui.h"
#include "lluicolortable.h"
#include "wolfgrid.h"

#include <boost/json.hpp>

static LLDefaultChildRegistry::Register<WolfDJShowsTable> r_wolf_dj_shows_table("wolf_dj_shows_table");

namespace
{
    // gridmanager/radioshows.php: the signed-in avatar's own shows, checked as gridstream.php.
    const char* SHOWS_API = "https://www.wolf-grid.com/radioshows.php";

    const S32 HEAD_H = 24;      // column titles
    const S32 ROW_H = 40;       // two lines a row
    const S32 PAD = 8;
    const S32 SCROLLBAR_W = 6;

    // Dark table on the floater; every text colour is at least 4.5:1 against its row.
    const LLColor4 C_BG(0.075f, 0.082f, 0.098f, 1.f);
    const LLColor4 C_HEAD_BG(0.12f, 0.13f, 0.155f, 1.f);
    const LLColor4 C_STRIPE(0.10f, 0.11f, 0.13f, 1.f);
    const LLColor4 C_LINE(0.22f, 0.24f, 0.28f, 1.f);
    const LLColor4 C_TEXT(0.93f, 0.94f, 0.96f, 1.f);
    const LLColor4 C_DIM(0.66f, 0.69f, 0.74f, 1.f);
    const LLColor4 C_ACCENT(0.64f, 0.90f, 0.21f, 1.f);     // the Wolf lime of the website (wolf-accent)
    const LLColor4 C_LIVE(1.f, 0.42f, 0.38f, 1.f);
    const LLColor4 C_SKELETON(0.16f, 0.17f, 0.20f, 1.f);
    const LLColor4 C_ERROR(1.f, 0.56f, 0.52f, 1.f);

    const LLFontGL* font() { return LLFontGL::getFontSansSerif(); }
    const LLFontGL* fontSmall() { return LLFontGL::getFontSansSerifSmall(); }
    const LLFontGL* fontBold() { return LLFontGL::getFontSansSerifBold(); }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::LEFT, S32 max_px = S32_MAX)
    {
        f->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, max_px, nullptr, true);
    }

    // Right edges of the number columns, as fractions of the table width (the first column takes the rest).
    struct Cols
    {
        S32 show_r, listeners_r, peak_r, how_r, kind_r;
        explicit Cols(S32 w)
        {
            const S32 inner = w - SCROLLBAR_W - PAD;
            kind_r = inner;
            how_r = inner - (S32)(inner * 0.17f);
            peak_r = how_r - (S32)(inner * 0.17f);
            listeners_r = peak_r - (S32)(inner * 0.17f);
            show_r = listeners_r - (S32)(inner * 0.13f);
        }
    };
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

WolfDJShowsTable::WolfDJShowsTable(const Params& p)
:   LLUICtrl(p)
{
}

void WolfDJShowsTable::setShows(const std::vector<WolfDJShow>& shows, S32 busiest)
{
    mShows = shows;
    mBusiest = busiest;
    mState = READY;
    scrollBy(0);
}

S32 WolfDJShowsTable::contentHeight() const
{
    return (S32)mShows.size() * ROW_H;
}

void WolfDJShowsTable::scrollBy(S32 px)
{
    const S32 view = getRect().getHeight() - HEAD_H - 1;     // the body, under the titles and their line
    mScroll = llclamp(mScroll + px, 0, std::max(0, contentHeight() - view));
}

bool WolfDJShowsTable::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    scrollBy(clicks * ROW_H);
    return true;
}

bool WolfDJShowsTable::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);     // so the arrow and page keys scroll it
    return true;
}

bool WolfDJShowsTable::handleKeyHere(KEY key, MASK mask)
{
    const S32 page = std::max(ROW_H, getRect().getHeight() - HEAD_H - ROW_H);
    switch (key)
    {
    case KEY_UP: scrollBy(-ROW_H); return true;
    case KEY_DOWN: scrollBy(ROW_H); return true;
    case KEY_PAGE_UP: scrollBy(-page); return true;
    case KEY_PAGE_DOWN: scrollBy(page); return true;
    case KEY_HOME: mScroll = 0; return true;
    case KEY_END: scrollBy(contentHeight()); return true;
    default: return false;
    }
}

void WolfDJShowsTable::draw()
{
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    const Cols c(w);
    gl_rect_2d(0, h, w, 0, C_BG, true);

    // Column titles.
    gl_rect_2d(0, h, w, h - HEAD_H, C_HEAD_BG, true);
    const F32 hy = (F32)(h - HEAD_H / 2);
    text(fontSmall(), "SHOW (GRID TIME)", (F32)PAD, hy, C_DIM);
    text(fontSmall(), "LISTENERS", (F32)c.listeners_r, hy, C_DIM, LLFontGL::RIGHT);
    text(fontSmall(), "MOST AT ONCE", (F32)c.peak_r, hy, C_DIM, LLFontGL::RIGHT);
    text(fontSmall(), "OWN / WTGR", (F32)c.how_r, hy, C_DIM, LLFontGL::RIGHT);
    text(fontSmall(), "IN-WORLD / WEB", (F32)c.kind_r, hy, C_DIM, LLFontGL::RIGHT);
    gl_rect_2d(0, h - HEAD_H, w, h - HEAD_H - 1, C_LINE, true);

    const S32 body_t = h - HEAD_H - 1;
    scrollBy(0);    // the window may have been made taller since the last scroll
    LLLocalClipRect clip(LLRect(0, body_t, w, 0));

    if (mState == LOADING)
    {
        // Skeleton rows the shape of the real ones (shown while the website answers).
        for (S32 i = 0; i < 4; ++i)
        {
            const S32 t = body_t - i * ROW_H;
            gl_rect_2d(PAD, t - 10, PAD + 140, t - 20, C_SKELETON, true);
            gl_rect_2d(PAD, t - 26, PAD + 200, t - 33, C_SKELETON, true);
            gl_rect_2d(c.listeners_r - 30, t - 12, c.listeners_r, t - 24, C_SKELETON, true);
            gl_rect_2d(c.peak_r - 30, t - 12, c.peak_r, t - 24, C_SKELETON, true);
        }
        return;
    }
    if (mShows.empty())
    {
        const F32 cy = (F32)(body_t / 2);
        if (mState == FAILED)
        {
            text(fontBold(), "Your shows couldn't be loaded.", (F32)(w / 2), cy + 10.f, C_ERROR, LLFontGL::HCENTER);
            text(font(), "The reason is below. Press Retry to try again.", (F32)(w / 2), cy - 10.f, C_DIM, LLFontGL::HCENTER);
        }
        else
        {
            text(fontBold(), "No shows yet", (F32)(w / 2), cy + 10.f, C_TEXT, LLFontGL::HCENTER);
            text(font(), "Go live from the Wolf DJ mixer and each show appears here with how many people listened.",
                 (F32)(w / 2), cy - 10.f, C_DIM, LLFontGL::HCENTER, w - 2 * PAD);
        }
        return;
    }

    for (S32 i = 0; i < (S32)mShows.size(); ++i)
    {
        const S32 t = body_t + mScroll - i * ROW_H;
        const S32 b = t - ROW_H;
        if (b > body_t || t < 0) continue;
        const WolfDJShow& s = mShows[i];
        if (i % 2 == 1) gl_rect_2d(0, t, w, b, C_STRIPE, true);
        const bool busiest = i == mBusiest;
        if (busiest) gl_rect_2d(0, t, 3, b, C_ACCENT, true);
        const F32 y1 = (F32)(t - 13), y2 = (F32)(t - 29);

        // Show: date (and a word for on air / busiest, never colour alone), then time and length.
        text(fontBold(), s.mDate, (F32)PAD, y1, C_TEXT, LLFontGL::LEFT, c.show_r - PAD);
        F32 badge_x = (F32)PAD + fontBold()->getWidthF32(s.mDate) + 10.f;
        if (s.mLive)
        {
            text(fontSmall(), "ON AIR", badge_x, y1, C_LIVE);
            badge_x += fontSmall()->getWidthF32("ON AIR") + 8.f;
        }
        if (busiest)
        {
            text(fontSmall(), "BUSIEST", badge_x, y1, C_ACCENT);
        }
        text(fontSmall(), s.mTime + "  ·  " + s.mLength, (F32)PAD, y2, C_DIM, LLFontGL::LEFT, c.show_r - PAD);

        text(fontBold(), std::to_string(s.mListeners), (F32)c.listeners_r, y1, C_TEXT, LLFontGL::RIGHT);
        text(font(), std::to_string(s.mPeak), (F32)c.peak_r, y1, C_TEXT, LLFontGL::RIGHT);
        if (!s.mPeakAt.empty()) text(fontSmall(), "at " + s.mPeakAt, (F32)c.peak_r, y2, C_DIM, LLFontGL::RIGHT);
        text(font(), std::to_string(s.mDirect) + " / " + (s.mWtgrKnown ? std::to_string(s.mWtgr) : std::string("?")),
             (F32)c.how_r, y1, C_TEXT, LLFontGL::RIGHT);
        if (!s.mWtgrKnown) text(fontSmall(), "WTGR not known", (F32)c.how_r, y2, C_DIM, LLFontGL::RIGHT);
        text(font(), std::to_string(s.mInworld) + " / " + std::to_string(s.mWeb), (F32)c.kind_r, y1, C_TEXT, LLFontGL::RIGHT);
        if (s.mOther > 0) text(fontSmall(), "+" + std::to_string(s.mOther) + " other", (F32)c.kind_r, y2, C_DIM, LLFontGL::RIGHT);
        gl_rect_2d(0, b + 1, w, b, C_LINE, true);
    }

    // Where the view is in the list, when it does not all fit.
    const S32 view = body_t;
    const S32 total = contentHeight();
    if (total > view && view > 0)
    {
        const S32 thumb = std::max(20, view * view / total);
        const S32 top = body_t - (view - thumb) * mScroll / std::max(1, total - view);
        gl_rect_2d(w - SCROLLBAR_W, top, w - 2, top - thumb, C_DIM, true);
    }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

WolfFloaterDJShows::WolfFloaterDJShows(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterDJShows::postBuild()
{
    mTable = getChild<WolfDJShowsTable>("shows_table");
    mSummary = getChild<LLTextBox>("summary");
    mNote = getChild<LLTextBox>("note");
    mRetry = getChild<LLButton>("retry");
    mRetry->setCommitCallback([this](LLUICtrl*, const LLSD&) { load(); });
    return true;
}

void WolfFloaterDJShows::onOpen(const LLSD& key)
{
    load();
}

void WolfFloaterDJShows::showError(const std::string& msg)
{
    // Stays until the next load works (an error never times out).
    mTable->setState(WolfDJShowsTable::FAILED);
    mNote->setColor(LLUIColor(C_ERROR));
    mNote->setText("Could not load your shows: " + msg);
    mRetry->setEnabled(true);
}

void WolfFloaterDJShows::load()
{
    if (!WolfGrid::isWolfTerritories())
    {
        // The request carries this login's agent and session ids: only ever to Wolf's own site.
        mSummary->setText(std::string());
        showError("My Shows is for Wolf Territories.");
        return;
    }
    if (mBusy) return;
    mBusy = true;
    mRetry->setEnabled(false);
    mTable->setState(WolfDJShowsTable::LOADING);
    mNote->setColor(LLUIColorTable::instance().getColor("LtGray_50"));
    mNote->setText(std::string("Loading your shows..."));
    const LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfDJShows", [handle]() { loadCoro(handle); });
}

// static
void WolfFloaterDJShows::loadCoro(LLHandle<LLFloater> handle)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfDJShows", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    // The same verified options and identity headers as Grid Stream (wolffloaterdj.cpp gridStreamCoro).
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    options->setRetries(1);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    LLSD result = adapter->postRawAndSuspend(request, SHOWS_API, raw, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);

    // radioshows.php: {success, shows: [...], total: {shows, listeners}, busiest: index|null} or {success: false, error}
    bool success = false;
    std::string error;
    std::vector<WolfDJShow> shows;
    S32 busiest = -1, total_shows = 0, total_listeners = 0, busiest_listeners = 0;
    std::string busiest_date;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value root = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
        if (!ec && root.is_object())
        {
            const boost::json::object& o = root.as_object();
            auto str = [](const boost::json::object& ob, const char* k) {
                const boost::json::value* v = ob.if_contains(k);
                return v && v->is_string() ? std::string(v->as_string().c_str()) : std::string();
            };
            auto num = [](const boost::json::object& ob, const char* k) {
                const boost::json::value* v = ob.if_contains(k);
                if (v && v->is_int64()) return (S32)llclamp(v->as_int64(), (S64)0, (S64)S32_MAX);
                if (v && v->is_uint64()) return (S32)std::min<std::uint64_t>(v->as_uint64(), (std::uint64_t)S32_MAX);
                return (S32)0;
            };
            auto flag = [](const boost::json::object& ob, const char* k, bool fallback) {
                const boost::json::value* v = ob.if_contains(k);
                return v && v->is_bool() ? v->as_bool() : fallback;
            };
            success = flag(o, "success", false);
            error = str(o, "error");
            const boost::json::value* list = o.if_contains("shows");
            if (success && list && list->is_array())
            {
                for (const boost::json::value& v : list->as_array())
                {
                    if (!v.is_object()) continue;
                    const boost::json::object& so = v.as_object();
                    WolfDJShow s;
                    s.mDate = str(so, "date");
                    s.mTime = str(so, "time");
                    s.mLength = str(so, "length");
                    s.mPeakAt = str(so, "peakAt");
                    s.mLive = flag(so, "live", false);
                    s.mWtgrKnown = flag(so, "wtgrKnown", true);
                    s.mListeners = num(so, "listeners");
                    s.mPeak = num(so, "peak");
                    s.mDirect = num(so, "direct");
                    s.mWtgr = num(so, "wtgr");
                    s.mInworld = num(so, "inworld");
                    s.mWeb = num(so, "web");
                    s.mOther = num(so, "other");
                    shows.push_back(s);
                }
                const boost::json::value* b = o.if_contains("busiest");
                if (b && b->is_int64()) busiest = (S32)b->as_int64();
                // The busiest of ALL the DJ's shows (it may be older than the list).
                const boost::json::value* bs = o.if_contains("busiestShow");
                if (bs && bs->is_object())
                {
                    busiest_date = str(bs->as_object(), "date");
                    busiest_listeners = num(bs->as_object(), "listeners");
                }
                const boost::json::value* t = o.if_contains("total");
                if (t && t->is_object())
                {
                    total_shows = num(t->as_object(), "shows");
                    total_listeners = num(t->as_object(), "listeners");
                }
            }
            else if (success)
            {
                success = false;
                error = "the website gave an unreadable answer";
            }
        }
    }

    WolfFloaterDJShows* self = static_cast<WolfFloaterDJShows*>(handle.get());
    if (!self) return;
    self->mBusy = false;
    self->mRetry->setEnabled(true);
    if (!success)
    {
        // A refusal (401/403/503) carries radioshows.php's own words, but the http adapter keeps
        // no body for a non-2xx answer (llcorehttputil.cpp HttpCoroRawHandler), so say it here.
        if (error.empty() && status.isHttpStatus())
        {
            const S32 code = status.getType();
            if (code == HTTP_UNAUTHORIZED || code == HTTP_FORBIDDEN)
                error = "Wolf Territories did not recognise this login. Log out and back in, then press Retry.";
            else if (code == HTTP_SERVICE_UNAVAILABLE)
                error = "the history is unavailable right now. Please try again in a few minutes.";
        }
        self->showError(!error.empty() ? error : !status ? status.toString() : std::string("the website gave an unreadable answer"));
        return;
    }
    if (busiest < 0 || busiest >= (S32)shows.size()) busiest = -1;
    self->mTable->setShows(shows, busiest);
    std::string summary = llformat("%d show%s  ·  %d listeners in all", total_shows, total_shows == 1 ? "" : "s", total_listeners);
    if (!busiest_date.empty())
    {
        summary += llformat("  ·  Busiest: %s, %d listeners", busiest_date.c_str(), busiest_listeners);
    }
    self->mSummary->setText(shows.empty() ? std::string() : summary);
    self->mNote->setColor(LLUIColorTable::instance().getColor("LtGray_50"));
    self->mNote->setText(std::string("Counted once a minute while you are on air. Listeners counts each person once a show; "
                                     "OWN / WTGR is where they listened (someone on both counts in each). WTGR's listeners "
                                     "count only while WTGR played your show."));
}
