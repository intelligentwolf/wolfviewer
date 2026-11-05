/**
 * @file wolffloateranimator.cpp
 * @brief WolfViewer: the Wolf Animator. See wolffloateranimator.h and
 *        region-terrain-work/wolf-animator-2026-11-05/README.md ("Window contract").
 *        Its transport, gate, credits handling, error words and polling are the Building Generator's
 *        (wolffloaterbuildinggen.cpp, itself the Plane Builder's).
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

#include "wolffloateranimator.h"

#include <algorithm>
#include <cmath>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcombobox.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrollcontainer.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewerinventory.h"
#include "llweb.h"
#include "wolfai.h"
#include "wolfcave.h"
#include "wolffloaterbugreport.h"
#include "wolffloaterfit.h"
#include "wolfgrid.h"

namespace
{
    // Source: wolf-animator-2026-11-05/README.md "Window contract (php/animator.php ...)": the same host as the Building
    // Generator's endpoint (wolffloaterbuildinggen.cpp YARD_URL).
    const char* SERVICE_URL = "https://wolfstorm.app/php/animator.php";
    // Source: the Building Generator's transport (wolffloaterbuildinggen.cpp TIMEOUT_S / POLL_S): 60 s, no retries; status
    // every 5 s while open and the job is queued or running (README "Scale and bounded stores").
    constexpr F32 TIMEOUT_S = 60.f;
    constexpr F32 POLL_S = 5.f;
    // Source: wolfstorm/php/animator_lib.php AN_REQUEST_MAX = 600, AN_COMMENT_MAX = 2000.
    constexpr S32 REQUEST_MAX_CHARS = 600;
    constexpr S32 COMMENT_MAX_CHARS = 2000;
    // Source: wanim/check.py preview(max_frames=240 in animator_job.py) and PREVIEW_JOINTS (26 joints).
    constexpr S32 PREVIEW_MAX_FRAMES = 600;
    constexpr S32 PREVIEW_MAX_JOINTS = 64;
    // The camera turns once in 24 s (the WolfStorm window's TURN_RAD_PER_S).
    constexpr F32 TURN_RAD_PER_S = F_TWO_PI / 24.f;
    const char* RATINGS[] = { "great", "ok", "retry", "fix" };

    LLColor4 rgb(S32 r, S32 g, S32 b) { return LLColor4((F32)r / 255.f, (F32)g / 255.f, (F32)b / 255.f, 1.f); }
    // The WolfStorm window's figure colours (css/animator.css: gold #f0c27a left, blue #a9d4ff right, ink #f4efe6 on #1b1e24)
    const LLColor4 GROUND = rgb(27, 30, 36), FLOOR = rgb(91, 97, 109), LEFT_COL = rgb(240, 194, 122), RIGHT_COL = rgb(169, 212, 255),
                   MID_COL = rgb(244, 239, 230);

    // As wolffloaterbuildinggen.cpp yardError, in the Wolf Animator's words.
    std::string serviceError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The Wolf Animator refused it without saying why.";
            case 400: return "The Wolf Animator could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "The grid did not accept your session - log out and back in, then try again.";
            case 404: return "The Wolf Animator is not open on this grid yet.";
            case 413: return "That was too long for the Wolf Animator.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 500: return "The Wolf Animator hit an error - try again in a moment.";
            case 503: return "The Wolf Animator is not answering - try again in a moment.";
            default:
                return transport.empty() ? llformat("The Wolf Animator answered HTTP %d.", status)
                                         : "Could not reach the Wolf Animator: " + transport;
        }
    }

    S32 intOr(const LLSD& v, S32 fallback) { return v.isUndefined() ? fallback : v.asInteger(); }

    // A thick line as two triangles (the UI pass has no wide-line guarantee).
    void thickLine(F32 x0, F32 y0, F32 x1, F32 y1, F32 w)
    {
        F32 dx = x1 - x0, dy = y1 - y0;
        const F32 len = sqrtf(dx * dx + dy * dy);
        if (len < 0.01f) return;
        const F32 nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
        gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x0 - nx, y0 - ny); gGL.vertex2f(x1 - nx, y1 - ny);
        gGL.vertex2f(x0 + nx, y0 + ny); gGL.vertex2f(x1 - nx, y1 - ny); gGL.vertex2f(x1 + nx, y1 + ny);
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfAnimPreview
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfAnimPreview::WolfAnimPreview(const Params& p)
    : LLView(p)
{
    mClock.reset();
}

// static
bool WolfAnimPreview::parse(const LLSD& pv, Figure& out)
{
    out = Figure();
    if (!pv.isMap() || !pv["joints"].isArray() || !pv["lines"].isArray() || !pv["frames"].isArray()) return false;
    const S32 nj = (S32)pv["joints"].size();
    const S32 nf = (S32)pv["frames"].size();
    if (nj <= 0 || nj > PREVIEW_MAX_JOINTS || nf <= 0 || nf > PREVIEW_MAX_FRAMES) return false;
    out.fps = (F32)pv["fps"].asReal();
    if (!(out.fps > 0.f) || out.fps > 120.f) out.fps = 12.f;
    out.joints = nj;
    for (S32 i = 0; i < nj; ++i)
    {
        const std::string n = pv["joints"][i].asString();
        const bool left = n.size() > 4 && n.compare(n.size() - 4, 4, "Left") == 0;
        const bool right = n.size() > 5 && n.compare(n.size() - 5, 5, "Right") == 0;
        out.side.push_back((S8)(left ? 1 : (right ? -1 : 0)));
        if (n == "mHead") out.head = i;
    }
    for (const LLSD& l : llsd::inArray(pv["lines"]))
    {
        if (!l.isArray() || l.size() != 2) return false;
        const S32 a = l[0].asInteger(), b = l[1].asInteger();
        if (a < 0 || a >= nj || b < 0 || b >= nj) return false;
        out.lines.emplace_back(a, b);
    }
    for (const LLSD& f : llsd::inArray(pv["frames"]))
    {
        if (!f.isArray() || (S32)f.size() != nj * 3) return false;
        std::vector<LLVector3> fr(nj);
        for (S32 j = 0; j < nj; ++j)
        {
            fr[j].set((F32)f[j * 3].asReal() * 0.01f, (F32)f[j * 3 + 1].asReal() * 0.01f, (F32)f[j * 3 + 2].asReal() * 0.01f);
        }
        out.frames.push_back(std::move(fr));
    }
    return true;
}

bool WolfAnimPreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    mLastX = x;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfAnimPreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
    }
    return true;
}

void WolfAnimPreview::onMouseCaptureLost()
{
    mDragging = false;
}

bool WolfAnimPreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mYaw += (F32)(x - mLastX) * 0.01f;
        mLastX = x;
    }
    return LLView::handleHover(x, y, mask);
}

void WolfAnimPreview::draw()
{
    const LLRect r = getLocalRect();
    LLLocalClipRect clip(r);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(r, GROUND, true);
    if (!mHas)
    {
        if (!mMessage.empty())
        {
            LLFontGL::getFontSansSerif()->renderUTF8(mMessage, 0, r.getCenterX(), r.getCenterY(), MID_COL, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
        LLView::draw();
        return;
    }
    const F32 dt = llmin(mClock.getElapsedTimeF32(), 0.1f);
    mClock.reset();
    if (mPlaying)
    {
        mTime += dt;
        if (!mDragging) mYaw += TURN_RAD_PER_S * dt;
    }
    if (mYaw > F_TWO_PI) mYaw -= F_TWO_PI;
    if (mYaw < 0.f) mYaw += F_TWO_PI;
    const size_t fi = (size_t)llfloor(mTime * mFigure.fps) % mFigure.frames.size();
    const std::vector<LLVector3>& fr = mFigure.frames[fi];
    // 2.1 m of height fills the picture; the floor near the bottom
    const F32 px_per_m = (F32)r.getHeight() * 0.86f / 2.1f;
    const F32 ox = (F32)r.getCenterX(), oy = (F32)r.mBottom + (F32)r.getHeight() * 0.06f;
    const F32 ca = cosf(mYaw), sa = sinf(mYaw);
    // a camera in front of the avatar turned round it by yaw: at yaw 0 the screen's right is the avatar's left
    auto px = [&](const LLVector3& p) { return ox + (p.mV[VX] * sa + p.mV[VY] * ca) * px_per_m; };
    auto py = [&](const LLVector3& p) { return oy + p.mV[VZ] * px_per_m; };
    gGL.begin(LLRender::TRIANGLES);
    gGL.color4fv(FLOOR.mV);
    thickLine((F32)r.mLeft + (F32)r.getWidth() * 0.1f, oy, (F32)r.mLeft + (F32)r.getWidth() * 0.9f, oy, 1.5f);
    const F32 w = llmax(3.f, px_per_m * 0.025f);
    for (const auto& l : mFigure.lines)
    {
        const S8 s = mFigure.side[l.first] != 0 ? mFigure.side[l.first] : mFigure.side[l.second];
        gGL.color4fv((s > 0 ? LEFT_COL : (s < 0 ? RIGHT_COL : MID_COL)).mV);
        thickLine(px(fr[l.first]), py(fr[l.first]), px(fr[l.second]), py(fr[l.second]), w);
    }
    if (mFigure.head >= 0)
    {
        const F32 hx = px(fr[mFigure.head]), hy = py(fr[mFigure.head]) + 0.09f * px_per_m, rad = 0.09f * px_per_m;
        gGL.color4fv(MID_COL.mV);
        constexpr S32 SEG = 20;
        for (S32 i = 0; i < SEG; ++i)
        {
            const F32 a0 = F_TWO_PI * (F32)i / (F32)SEG, a1 = F_TWO_PI * (F32)(i + 1) / (F32)SEG;
            gGL.vertex2f(hx, hy);
            gGL.vertex2f(hx + rad * cosf(a0), hy + rad * sinf(a0));
            gGL.vertex2f(hx + rad * cosf(a1), hy + rad * sinf(a1));
        }
    }
    gGL.end();
    LLView::draw();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterAnimator
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterAnimator::WolfFloaterAnimator(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterAnimator::~WolfFloaterAnimator()
{
    if (mPreview && gFocusMgr.getMouseCapture() == mPreview) gFocusMgr.setMouseCapture(nullptr);
}

bool WolfFloaterAnimator::postBuild()
{
    mRequest = getChild<LLTextEditor>("request_text");
    mCounter = getChild<LLTextBox>("request_counter");
    mExamples = getChild<LLComboBox>("examples");
    mQuote = getChild<LLTextEditor>("quote_text");
    mBalance = getChild<LLTextBox>("balance_text");
    mMakeBtn = getChild<LLButton>("make_btn");
    mAccess = getChild<LLTextBox>("access_text");
    mTitle = getChild<LLTextBox>("result_title");
    mResult = getChild<LLTextEditor>("result_text");
    mKey = getChild<LLTextBox>("preview_key");
    mPlayBtn = getChild<LLButton>("play_btn");
    mPlayOnMeBtn = getChild<LLButton>("play_on_me_btn");
    mFeedbackLabel = getChild<LLTextBox>("feedback_label");
    for (const char* r : RATINGS)
    {
        mChoiceBtns.push_back(getChild<LLButton>(std::string("choice_") + r));
    }
    mBoxLabel = getChild<LLTextBox>("box_label");
    mBoxText = getChild<LLTextEditor>("box_text");
    mSendBtn = getChild<LLButton>("send_btn");
    mStatus = getChild<LLTextBox>("status_text");
    mCancelBtn = getChild<LLButton>("cancel_btn");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mScroll = getChild<LLScrollContainer>("body_scroll");
    mNaturalHeight = getRect().getHeight();   // the XML's height (llfloater.cpp:3845-3852: postBuild runs before the saved rect is applied)

    LLPanel* holder = getChild<LLPanel>("preview_panel");
    WolfAnimPreview::Params pp;
    pp.name = "anim_preview";
    pp.rect = holder->getLocalRect();
    pp.follows.flags = FOLLOWS_ALL;
    pp.mouse_opaque = true;
    mPreview = LLUICtrlFactory::create<WolfAnimPreview>(pp);
    holder->addChild(mPreview);
    mPreview->setMessage(getString("no_result_yet"));

    mExamples->add(getString("examples_prompt"), LLSD(std::string()));
    mExamples->selectFirstItem();

    // Each lambda captures only `this`; the floater owns the controls, so they never call it after it is gone.
    mRequest->setKeystrokeCallback([this](LLTextEditor*) { onRequestEdited(); });
    mExamples->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExample(); });
    mMakeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMake(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancelJob(); });
    mBuyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuyCredits(); });
    mPlayBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlayPause(); });
    mPlayOnMeBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlayOnMe(); });
    for (size_t i = 0; i < mChoiceBtns.size(); ++i)
    {
        const std::string r = RATINGS[i];
        mChoiceBtns[i]->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { onChoice(r); });
    }
    mSendBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSendBox(); });

    clearResult();
    onRequestEdited();
    showQuote();
    setStatus(getString("start_hint"), false);
    updateCredits();
    updateControls();
    return true;
}

void WolfFloaterAnimator::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] as the Building Generator: a Wolf-only window opened off Wolf says so and closes.
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly(getString("feature_name"));
        closeFloater();
        return;
    }
    mOpenFailed = false;
    if (mReady)
    {
        // the balance is read again every time the window opens (Paul: never cache it)
        request("credits", LLSD::emptyMap());
        if (!mJob.empty()) request("status", LLSD().with("job", mJob));
    }
    updateControls();
}

void WolfFloaterAnimator::onClose(bool app_quitting)
{
    // an animation started from here never keeps playing once its window is gone
    stopOnMe();
    LLFloater::onClose(app_quitting);
}

void WolfFloaterAnimator::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterAnimator::draw()
{
    // draw() runs only while the window is open, so nothing below asks the service once it is closed.
    if (!mReady && !mBusy && !mOpenFailed && mRefused.empty() && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mBusy = true;
        setStatus(getString("opening"), false);
        request("open", LLSD::emptyMap());
    }
    if (!mJob.empty() && !mPolling && mPoll.getElapsedTimeF32() > POLL_S && (mJobState == "queued" || mJobState == "running"))
    {
        mPolling = true;
        request("status", LLSD().with("job", mJob));
    }
    WolfFloaterFit::fit(this, mNaturalHeight);
    LLFloater::draw();
}

void WolfFloaterAnimator::scrollToShow(const std::vector<LLView*>& views)
{
    // the views are the body panel's children, so their rects are in the scrolled panel's coordinates
    LLRect u;
    for (LLView* v : views)
    {
        if (!v || !v->getVisible()) continue;
        if (u.isEmpty()) u = v->getRect();
        else u.unionWith(v->getRect());
    }
    if (!u.isEmpty() && mScroll) mScroll->scrollToShowRect(u);
}

std::string WolfFloaterAnimator::requestText() const
{
    std::string text = mRequest->getText();
    LLStringUtil::trim(text);
    return text;
}

std::string WolfFloaterAnimator::credits(S32 n) const
{
    LLStringUtil::format_map_t a;
    a["[N]"] = llformat("%d", n);
    return getString(n == 1 ? "credit_one" : "credit_many", a);
}

void WolfFloaterAnimator::readCredits(const LLSD& reply)
{
    // Source: README "Window contract" open / credits / credits_short: balance, available, buyurl (read live)
    if (reply.has("balance")) mBalanceNow = intOr(reply["balance"], -1);
    if (reply.has("available")) mAvailable = intOr(reply["available"], -1);
    if (reply.has("buyurl") && !reply["buyurl"].asString().empty()) mBuyUrl = reply["buyurl"].asString();
}

void WolfFloaterAnimator::onOpenReply(const LLSD& reply)
{
    // Source: README "Window contract" open: examples, quote {typical, at_most, note}, balance, available, buyurl, access
    // {make, reason}, request_max, active, last
    readCredits(reply);
    const LLSD& q = reply["quote"];
    mQuoteTypical = intOr(q["typical"], -1);
    mQuoteAtMost = intOr(q["at_most"], -1);
    mPriceNote = q["note"].asString();
    mAccessMake = reply["access"]["make"].asBoolean();
    mAccessReason = reply["access"]["reason"].asString();
    mExamples->clearRows();
    mExamples->add(getString("examples_prompt"), LLSD(std::string()));
    for (const LLSD& e : llsd::inArray(reply["examples"]))
    {
        const std::string t = e.asString().substr(0, REQUEST_MAX_CHARS);
        if (!t.empty()) mExamples->add(t, LLSD(t));
    }
    mExamples->selectFirstItem();
    mReady = true;
    showQuote();
    setStatus(mAccessMake ? getString("start_hint") : std::string(), false);
    if (reply["active"].isMap()) showJob(reply["active"], false);
    else if (reply["last"].isMap()) showJob(reply["last"], false);
    updateCredits();
    updateControls();
}

void WolfFloaterAnimator::onRequestEdited()
{
    // 600 characters at most (the contract's): typing or pasting past it is cut back to the first 600
    // (wolffloaterbuildinggen.cpp onRequestEdited, as it is)
    LLWString w = mRequest->getWText();
    if ((S32)w.size() > REQUEST_MAX_CHARS)
    {
        w.resize(REQUEST_MAX_CHARS);
        mRequest->setText(wstring_to_utf8str(w));
        mRequest->endOfDoc();
    }
    LLStringUtil::format_map_t a;
    a["[N]"] = llformat("%d", (S32)w.size());
    a["[MAX]"] = llformat("%d", REQUEST_MAX_CHARS);
    mCounter->setText(getString("counter", a));
    mCounter->setColor(LLUIColorTable::instance().getColor((S32)w.size() >= REQUEST_MAX_CHARS ? "AlertCautionTextColor" : "LabelTextColor"));
    updateControls();
}

void WolfFloaterAnimator::onExample()
{
    const std::string t = mExamples->getValue().asString();
    if (t.empty() || !mRequest->getEnabled()) return;
    mRequest->setText(t);
    onRequestEdited();
    mRequest->setFocus(true);
}

void WolfFloaterAnimator::showQuote()
{
    if (mQuoteAtMost < 0)
    {
        mQuote->setText(getString(mReady ? "quote_missing" : "quote_waiting"));
        return;
    }
    LLStringUtil::format_map_t a;
    a["[N]"] = credits(mQuoteTypical);
    a["[M]"] = credits(mQuoteAtMost);
    std::string t = getString("quote_line", a);
    if (!mPriceNote.empty()) t += " " + mPriceNote;
    mQuote->setText(t);
}

void WolfFloaterAnimator::updateCredits()
{
    if (mAvailable < 0)
    {
        mBalance->setText(getString("balance_unknown"));
        mBalance->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
        return;
    }
    const bool shortOf = mShortOf || (mQuoteAtMost >= 0 && mAvailable < mQuoteAtMost);
    LLStringUtil::format_map_t a;
    a["[N]"] = llformat("%d", mAvailable);
    a["[B]"] = llformat("%d", mBalanceNow < 0 ? mAvailable : mBalanceNow);
    mBalance->setText(getString(shortOf ? "balance_short" : "balance_ok", a));
    mBalance->setColor(LLUIColorTable::instance().getColor(shortOf ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterAnimator::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool open = wolf && mReady && mRefused.empty();
    const bool working = !mJob.empty();
    const bool canMake = open && mAccessMake && !working;
    mRequest->setEnabled(canMake);
    mExamples->setEnabled(canMake);
    // the button stays pressable with an empty description: the press says what is missing (never a greyed mystery)
    mMakeBtn->setEnabled(canMake && !mBusy && mQuoteAtMost >= 0 && mAvailable >= 0 && mAvailable >= mQuoteAtMost);
    mBuyBtn->setVisible(open && mAvailable >= 0 && (mShortOf || (mQuoteAtMost >= 0 && mAvailable < mQuoteAtMost)));
    mCancelBtn->setVisible(working && mJobState == "queued");
    mCancelBtn->setEnabled(!mBusy);
    const bool limited = open && !mAccessMake && !mAccessReason.empty();
    mAccess->setText(limited ? mAccessReason : std::string());
    mAccess->setVisible(limited);

    const bool shown = mShown.isMap();
    mPlayBtn->setVisible(shown && mKey->getVisible());
    mPlayOnMeBtn->setVisible(shown && !mShown["item"].asString().empty());
    mPlayOnMeBtn->setLabel(getString(mPlayingAsset.notNull() ? "stop_on_me" : "play_on_me"));
    mPlayBtn->setLabel(getString(mPreview->playing() ? "pause" : "play"));
    const bool ask = shown && mCanRate && !working;
    mFeedbackLabel->setVisible(ask);
    for (size_t i = 0; i < mChoiceBtns.size(); ++i)
    {
        const bool retry = std::string(RATINGS[i]) == "retry";
        mChoiceBtns[i]->setVisible(ask && (!retry || mCanRetry));
        mChoiceBtns[i]->setEnabled(!mBusy);
    }
    const bool box = ask && mBox != Box::NONE;
    mBoxLabel->setVisible(box);
    mBoxText->setVisible(box);
    mSendBtn->setVisible(box);
    mSendBtn->setEnabled(!mBusy);
}

void WolfFloaterAnimator::onMake()
{
    if (mBusy || !mJob.empty() || !mAccessMake) return;
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        setStatus(getString("not_confirmed"), true);
        return;
    }
    if (requestText().empty())
    {
        setStatus(getString("request_needed"), true);
        mRequest->setFocus(true);
        return;
    }
    // Source: README make {request, at_most}: at_most = the quote on show; the service refuses with quote_changed otherwise
    mBusy = true;
    setStatus(getString("making_send"), false);
    updateControls();
    request("make", LLSD().with("request", requestText()).with("at_most", mQuoteAtMost));
}

void WolfFloaterAnimator::onCancelJob()
{
    if (mJob.empty() || mBusy || mJobState != "queued") return;
    mBusy = true;
    setStatus(getString("cancelling"), false);
    updateControls();
    request("cancel", LLSD().with("job", mJob));
}

void WolfFloaterAnimator::onBuyCredits()
{
    // As the Building Generator: the service's buyurl when it is a Wolf Territories page, else the AI credits shop WolfAI knows.
    const bool wolf_page = !mBuyUrl.empty() && mBuyUrl.find_first_of("\\\r\n\t ") == std::string::npos
        && (mBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuyUrl.rfind("https://wolf-grid.com/", 0) == 0);
    if (wolf_page) LLWeb::loadURLExternal(mBuyUrl);
    else WolfAI::instance().openBuyPage();
}

void WolfFloaterAnimator::onPlayPause()
{
    mPreview->setPlaying(!mPreview->playing());
    updateControls();
}

void WolfFloaterAnimator::onPlayOnMe()
{
    if (mPlayingAsset.notNull())
    {
        stopOnMe();
        setStatus(std::string(), false);
        updateControls();
        return;
    }
    // As Play in world from the inventory (llagentlistener.cpp playAnimation with "inworld": the item's asset, started by an
    // AgentAnimation request), so everyone around sees it.
    const LLViewerInventoryItem* item = gInventory.getItem(LLUUID(mShown["item"].asString()));
    if (!item || item->getInventoryType() != LLInventoryType::IT_ANIMATION || item->getAssetUUID().isNull())
    {
        LLInventoryModelBackgroundFetch::instance().scheduleItemFetch(LLUUID(mShown["item"].asString()), true);
        setStatus(getString("not_arrived"), true);
        return;
    }
    mPlayingAsset = item->getAssetUUID();
    gAgent.sendAnimationRequest(mPlayingAsset, ANIM_REQUEST_START);
    setStatus(getString("playing_on_me"), false);
    updateControls();
}

void WolfFloaterAnimator::stopOnMe()
{
    if (mPlayingAsset.isNull()) return;
    gAgent.sendAnimationRequest(mPlayingAsset, ANIM_REQUEST_STOP);
    mPlayingAsset.setNull();
}

void WolfFloaterAnimator::clearResult()
{
    stopOnMe();
    mShown = LLSD();
    mCanRate = mCanRetry = false;
    mBox = Box::NONE;
    mPreview->clearFigure();
    mPreview->setMessage(getString("no_result_yet"));
    mTitle->setText(std::string());
    mResult->setText(getString("no_result_text"));
    mKey->setVisible(false);
}

void WolfFloaterAnimator::showResult(const LLSD& o)
{
    // Source: README JOB when done: plan {title, summary, loop, bpm, beats, seconds, ...}, preview, item_name, charged, free,
    // feedback {rating, retry_job, can_retry}
    const LLSD& before = mShown;    // const: reading a key never turns an empty LLSD into a map
    if (before["job"].asString() != o["job"].asString()) stopOnMe();
    mShown = o;
    const LLSD& plan = o["plan"];
    // the delivered item into the inventory the viewer holds now (the service put it there after this login), so Play it
    // on my avatar and the inventory window find it without a relog
    const LLUUID item_id(o["item"].asString()), folder_id(o["folder"].asString());
    if (item_id.notNull()) LLInventoryModelBackgroundFetch::instance().scheduleItemFetch(item_id, true);
    if (folder_id.notNull()) LLInventoryModelBackgroundFetch::instance().scheduleFolderFetch(folder_id, true);
    WolfAnimPreview::Figure fig;
    if (WolfAnimPreview::parse(o["preview"], fig))
    {
        mPreview->setFigure(fig);
        mKey->setVisible(true);
    }
    else
    {
        mPreview->clearFigure();
        mPreview->setMessage(getString("preview_bad"));
        mKey->setVisible(false);
    }
    const std::string name = o["item_name"].asString().empty() ? plan["title"].asString() : o["item_name"].asString();
    mTitle->setText(plan["title"].asString());
    LLStringUtil::format_map_t a;
    a["[SECONDS]"] = llformat("%.1f", (F32)plan["seconds"].asReal());
    a["[BPM]"] = llformat("%d", (S32)ll_round((F32)plan["bpm"].asReal()));
    a["[NAME]"] = name;
    a["[CHARGED]"] = credits(o["charged"].asInteger());
    std::string t = plan["summary"].asString() + "\n\n";
    t += getString(plan["loop"].asBoolean() ? "facts_loop" : "facts_once", a) + "\n";
    t += getString(o["free"].asBoolean() ? "where_free" : "where_charged", a) + "\n\n";
    t += getString("how_to_play");
    mResult->setText(t);
    const LLSD& fb = o["feedback"];
    mCanRate = fb.isMap() && fb["rating"].asString().empty();
    mCanRetry = mCanRate && fb["can_retry"].asBoolean();
    mBox = Box::NONE;
}

void WolfFloaterAnimator::showJob(const LLSD& o, bool announce)
{
    // Source: README JOB: job, state (queued, running, done, failed, cancelled), progress, error, position, request, free
    const std::string state = o["state"].asString();
    LLStringUtil::format_map_t a;
    a["[AHEAD]"] = llformat("%d", o["position"].asInteger());
    a["[PROGRESS]"] = o["progress"].asString().empty() ? getString("progress_none") : o["progress"].asString();
    mPoll.reset();
    if (state == "queued" || state == "running")
    {
        mJob = o["job"].asString();
        mJobState = state;
        const std::string which = state == "queued" ? (o["position"].asInteger() > 0 ? "make_waiting" : "make_next") : "make_running";
        setStatus(getString(which, a), false);
    }
    else
    {
        mJob.clear();
        mJobState.clear();
        if (state == "done")
        {
            showResult(o);
            updateControls();     // the result's controls are visible before the scroll measures them
            scrollToShow({ mPlayOnMeBtn, mFeedbackLabel, mChoiceBtns[0], mChoiceBtns[3] });
            if (announce)
            {
                setStatus(getString("made"), false);
                WolfAI::instance().creditsUsed();
                request("credits", LLSD::emptyMap());
            }
        }
        else if (state == "cancelled")
        {
            if (announce) setStatus(getString("make_cancelled"), false);
        }
        else if (announce)
        {
            const std::string err = o["error"].asString();
            setStatus(err.empty() ? getString("make_failed") : err, true);
        }
    }
    updateCredits();
    updateControls();
}

void WolfFloaterAnimator::onChoice(const std::string& rating)
{
    if (mBusy || !mShown.isMap() || !mCanRate) return;
    if (rating == "great")
    {
        mBusy = true;
        request("rate", LLSD().with("job", mShown["job"].asString()).with("rating", "great"));
    }
    else if (rating == "ok")
    {
        mBox = Box::COMMENT;
        mBoxLabel->setText(getString("box_comment"));
        mBoxText->setText(std::string());
        mBoxText->setMaxTextLength(COMMENT_MAX_CHARS * 4);   // a byte limit (lltextbase.h): 4 bytes a character at most
        mSendBtn->setLabel(getString("send_comment"));
        mBoxText->setFocus(true);
        updateControls();
        scrollToShow({ mFeedbackLabel, mBoxText, mSendBtn });
    }
    else if (rating == "retry" && mCanRetry)
    {
        mBox = Box::RETRY;
        mBoxLabel->setText(getString("box_retry"));
        mBoxText->setMaxTextLength(REQUEST_MAX_CHARS * 4);
        mBoxText->setText(mShown["request"].asString());
        mSendBtn->setLabel(getString("send_retry"));
        mBoxText->setFocus(true);
        updateControls();
        scrollToShow({ mFeedbackLabel, mBoxText, mSendBtn });
    }
    else if (rating == "fix")
    {
        mBusy = true;
        request("rate", LLSD().with("job", mShown["job"].asString()).with("rating", "fix"));
    }
    updateControls();
}

void WolfFloaterAnimator::onSendBox()
{
    if (mBusy || !mShown.isMap() || mBox == Box::NONE) return;
    std::string text = mBoxText->getText();
    LLStringUtil::trim(text);
    if (mBox == Box::RETRY)
    {
        if (text.empty())
        {
            setStatus(getString("retry_empty"), true);
            mBoxText->setFocus(true);
            return;
        }
        mBusy = true;
        setStatus(getString("retry_send"), false);
        request("retry", LLSD().with("job", mShown["job"].asString()).with("request", text));
    }
    else
    {
        mBusy = true;
        request("rate", LLSD().with("job", mShown["job"].asString()).with("rating", "ok").with("comment", text));
    }
    updateControls();
}

void WolfFloaterAnimator::openFleaReport()
{
    // "Something is wrong": the Flea Report window with the facts filled in (the WolfStorm window does the same)
    WolfFloaterBugReport* f = LLFloaterReg::showTypedInstance<WolfFloaterBugReport>("wolf_bug_report", LLSD(), true);
    if (!f)
    {
        LLStringUtil::format_map_t a;
        a["[JOB]"] = mShown["job"].asString();
        setStatus(getString("flea_missing", a), true);
        return;
    }
    LLStringUtil::format_map_t a;
    a["[TITLE]"] = mShown["plan"]["title"].asString();
    a["[JOB]"] = mShown["job"].asString();
    a["[REQUEST]"] = mShown["request"].asString();
    a["[MADE]"] = mShown["created"].asString();
    a["[NAME]"] = mShown["item_name"].asString();
    f->prefill("inventory", getString("flea_subject", a), getString("flea_body", a));
    setStatus(getString("flea_opened"), false);
}

void WolfFloaterAnimator::request(const std::string& what, const LLSD& body)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfAnimator", [h, what, body]() { requestCoro(h, what, body); });
}

// static
void WolfFloaterAnimator::requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body)
{
    // Source: wolffloaterbuildinggen.cpp requestCoro - WolfCaves::serviceRequest (POST JSON, X-Wolf-Agent / X-Wolf-Session,
    // retries 0). The floater is looked up again through its handle when the reply comes: closed and destroyed in the
    // meantime, the reply is dropped.
    body["action"] = what;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(SERVICE_URL, &text, TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    const std::string error = ok ? std::string() : serviceError(status, reply, transport);
    if (WolfFloaterAnimator* f = dynamic_cast<WolfFloaterAnimator*>(h.get()))
    {
        f->onReply(what, body, ok, reply, error);
    }
}

void WolfFloaterAnimator::onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error)
{
    const std::string code = reply.isMap() ? reply["code"].asString() : std::string();
    if (what == "open")
    {
        mBusy = false;
        if (ok)
        {
            onOpenReply(reply);
        }
        else
        {
            if (code == "not_member") mRefused = error;
            else mOpenFailed = true;
            mPreview->setMessage(getString("closed"));
            setStatus(error + (mOpenFailed ? " " + getString("reopen") : std::string()), true);
        }
    }
    else if (what == "credits")
    {
        if (ok)
        {
            readCredits(reply);
            mShortOf = false;    // a fresh balance replaces the service's last "not enough"
        }
        else
        {
            setStatus(error, true);
        }
        updateCredits();
    }
    else if (what == "make" || what == "retry")
    {
        mBusy = false;
        if (ok)
        {
            clearResult();
            if (what == "retry")
            {
                mRequest->setText(body["request"].asString());   // what the try again was made from
                onRequestEdited();
            }
            showJob(reply, true);
        }
        else
        {
            readCredits(reply);   // credits_short carries balance, available and buyurl
            if (code == "credits_short") mShortOf = true;
            if (code == "quote_changed" && reply["quote"].isMap())
            {
                // the price moved: show the new one; the next press uses it
                mQuoteTypical = intOr(reply["quote"]["typical"], -1);
                mQuoteAtMost = intOr(reply["quote"]["at_most"], -1);
                showQuote();
            }
            if (code == "retry_used" || code == "retry_old" || code == "retry_none")
            {
                mCanRetry = false;
                mBox = Box::NONE;
            }
            setStatus(error, true);
            updateCredits();
        }
    }
    else if (what == "rate")
    {
        mBusy = false;
        const std::string rating = body["rating"].asString();
        if (ok || code == "rated")
        {
            mCanRate = false;
            mBox = Box::NONE;
            if (rating == "fix") openFleaReport();
            else setStatus(ok ? getString("thanks") : error, !ok);
        }
        else
        {
            setStatus(error, true);
        }
    }
    else if (what == "status" || what == "cancel")
    {
        if (what == "status") mPolling = false;
        else mBusy = false;
        mPoll.reset();
        // a reply about a job that is no longer the one followed is ignored
        if (body["job"].asString() == mJob && !mJob.empty())
        {
            if (ok) showJob(reply, true);
            else setStatus(error, true);
        }
    }
    updateControls();
}
