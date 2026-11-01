/**
 * @file wolffloaterbuildinggen.cpp
 * @brief WolfViewer: the Building Generator. See wolffloaterbuildinggen.h and
 *        region-terrain-work/building-generator-2026-11-01/README.md ("Viewer contract").
 *        Its transport, credits handling, error words and polling are the Plane Builder's (wolffloaterplanebuilder.cpp).
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

#include "wolffloaterbuildinggen.h"

#include <algorithm>
#include <cmath>

#include <boost/json.hpp>

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcoros.h"
#include "llfocusmgr.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llweb.h"
#include "wolfai.h"
#include "wolfcave.h"
#include "wolfgrid.h"

namespace
{
    // Source: building-generator-2026-11-01/README.md "Endpoint https://wolfstorm.app/php/building_gen.php".
    const char* YARD_URL = "https://wolfstorm.app/php/building_gen.php";
    // Source: building-generator-2026-11-01/README.md "retries 0, timeout 60 s" (the Plane Builder's transport).
    constexpr F32 TIMEOUT_S = 60.f;
    // Source: building-generator-2026-11-01/README.md "Polling: the window polls status every 5 s, only while it is open and
    // the job is queued or running".
    constexpr F32 POLL_S = 5.f;
    // The quote is asked for once the request and style have been still this long (the brief: "debounced ~1 s").
    constexpr F32 QUOTE_SETTLE_S = 1.f;
    // Source: building-generator-2026-11-01/README.md "a text box for the request (600 characters)".
    constexpr S32 REQUEST_MAX_CHARS = 600;
    // Source: building-generator-2026-11-01/README.md preview "At most 30,000 triangles".
    constexpr S32 PREVIEW_MAX_TRIS = 30000;
    // A mesh of 30,000 triangles needs at most 90,000 corners; more than that is not a preview of the contract.
    constexpr S32 PREVIEW_MAX_VERTS = PREVIEW_MAX_TRIS * 3;
    // Source: building-generator-2026-11-01/README.md "The styles list ... first entry "Let the generator decide" = auto".
    const char* STYLE_AUTO = "auto";
    // Source: building-generator-2026-11-01/README.md OPTIONS (four keys); the window has a box for each.
    constexpr size_t OPTION_BOXES = 4;

    constexpr F32 TURN_RAD_PER_S = F_TWO_PI / 30.f;     // a little slower than the plane's 24 s: a building is studied
    constexpr F32 ELEVATION_RAD = 24.f * DEG_TO_RAD;    // higher than the plane's 18: the roofs and the layout show

    LLColor4 rgb(S32 r, S32 g, S32 b) { return LLColor4((F32)r / 255.f, (F32)g / 255.f, (F32)b / 255.f, 1.f); }
    // As WolfPlanePreview's sky and ink (wolffloaterplanebuilder.cpp).
    const LLColor4 SKY_TOP = rgb(150, 182, 210), SKY_BOTTOM = rgb(214, 226, 236), INK = rgb(40, 48, 60);

    // As wolffloaterplanebuilder.cpp yardError: the yard's own words when it gave them.
    std::string yardError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The yard refused it without saying why.";
            case 400: return "The yard could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "The grid did not accept your session - log out and back in, then try again.";
            case 404: return "The Building Generator yard is not open on this grid yet.";
            case 413: return "The request was too large for the yard.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 500: return "The yard hit an error - try again in a moment.";
            case 503: return "The yard is not answering - try again in a moment.";
            default:
                return transport.empty() ? llformat("The yard answered HTTP %d.", status)
                                         : "Could not reach the yard: " + transport;
        }
    }

    S32 intOr(const LLSD& v, S32 fallback) { return v.isUndefined() ? fallback : v.asInteger(); }

    // A web address the sources list may show as a link: http(s) only, no spaces or control characters (so it can never be a
    // secondlife:/// command or break the line).
    bool webAddress(const std::string& url)
    {
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) return false;
        for (char ch : url)
        {
            if ((unsigned char)ch <= ' ' || ch == 0x7f) return false;
        }
        return true;
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfBuildingPreview (copied from WolfPlanePreview, wolffloaterplanebuilder.cpp)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfBuildingPreview::WolfBuildingPreview(const Params& p)
    : LLView(p)
{
    mTurn.reset();
}

// static
std::shared_ptr<const WolfBuildingPreview::Mesh> WolfBuildingPreview::parse(const LLSD& pv)
{
    // Source: building-generator-2026-11-01/README.md preview: "v = x,y,z centimetres (ints; X east, Y north, Z up, origin =
    // the centre of the footprint on the ground); t = 4 ints per triangle a,b,c,colour where colour indexes palette
    // ([[r,g,b], ...], 0..255) ... At most 30,000 triangles." Checked as WolfPlanePreview::parse checks the plane's: lengths a
    // multiple of 3 / 4, every index in range; anything else and there is no picture rather than a wrong one.
    if (!pv.isMap() || !pv["v"].isArray() || !pv["t"].isArray() || !pv["palette"].isArray()) return nullptr;
    auto m = std::make_shared<Mesh>();

    const LLSD& pal = pv["palette"];
    const S32 np = (S32)pal.size();          // LLSD::size() is size_t (MSVC C4267 is an error)
    if (np <= 0 || np > 65535) return nullptr;
    m->palette.reserve(np);
    for (S32 i = 0; i < np; ++i)
    {
        const LLSD& c = pal[i];
        if (!c.isArray() || c.size() != 3) return nullptr;
        const S32 r = c[0].asInteger(), g = c[1].asInteger(), b = c[2].asInteger();
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return nullptr;
        m->palette.push_back(rgb(r, g, b));
    }

    const LLSD& v = pv["v"];
    if (v.size() % 3) return nullptr;
    const S32 nv = (S32)(v.size() / 3);
    if (nv <= 0 || nv > PREVIEW_MAX_VERTS) return nullptr;
    m->verts.reserve(nv);
    LLVector3 lo(F32_MAX, F32_MAX, F32_MAX), hi(-F32_MAX, -F32_MAX, -F32_MAX);
    for (S32 i = 0; i < nv; ++i)
    {
        LLVector3 p((F32)v[i * 3].asInteger() / 100.f, (F32)v[i * 3 + 1].asInteger() / 100.f, (F32)v[i * 3 + 2].asInteger() / 100.f);
        lo.set(llmin(lo.mV[0], p.mV[0]), llmin(lo.mV[1], p.mV[1]), llmin(lo.mV[2], p.mV[2]));
        hi.set(llmax(hi.mV[0], p.mV[0]), llmax(hi.mV[1], p.mV[1]), llmax(hi.mV[2], p.mV[2]));
        m->verts.push_back(p);
    }

    const LLSD& t = pv["t"];
    if (t.size() % 4) return nullptr;
    const S32 nt = (S32)(t.size() / 4);
    if (nt <= 0 || nt > PREVIEW_MAX_TRIS) return nullptr;
    m->tris.reserve(nt);
    for (S32 i = 0; i < nt; ++i)
    {
        const S32 a = t[i * 4].asInteger(), b = t[i * 4 + 1].asInteger(), c = t[i * 4 + 2].asInteger();
        const S32 k = t[i * 4 + 3].asInteger();
        if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) return nullptr;
        if (k < 0 || k >= np) return nullptr;
        m->tris.push_back(Tri{ (U32)a, (U32)b, (U32)c, (U16)k });
    }
    m->centre = (lo + hi) * 0.5f;
    const LLVector3 ext = hi - lo;
    // It turns about Z, so the footprint's diagonal is what has to fit across the picture.
    m->radius = llmax(0.5f * sqrtf(ext.mV[0] * ext.mV[0] + ext.mV[1] * ext.mV[1]), 0.5f * ext.mV[2], 1.f);
    return m;
}

bool WolfBuildingPreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    mLastX = x;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfBuildingPreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
        mTurn.reset();
    }
    return true;
}

void WolfBuildingPreview::onMouseCaptureLost()
{
    mDragging = false;
    mTurn.reset();
}

bool WolfBuildingPreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mYaw += (F32)(x - mLastX) * 0.01f;
        mLastX = x;
    }
    return LLView::handleHover(x, y, mask);
}

void WolfBuildingPreview::draw()
{
    const LLRect r = getLocalRect();
    LLLocalClipRect clip(r);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.begin(LLRender::TRIANGLES);
    gGL.color4fv(SKY_TOP.mV);    gGL.vertex2i(r.mLeft, r.mTop);
    gGL.color4fv(SKY_BOTTOM.mV); gGL.vertex2i(r.mLeft, r.mBottom);
    gGL.color4fv(SKY_BOTTOM.mV); gGL.vertex2i(r.mRight, r.mBottom);
    gGL.color4fv(SKY_TOP.mV);    gGL.vertex2i(r.mLeft, r.mTop);
    gGL.color4fv(SKY_BOTTOM.mV); gGL.vertex2i(r.mRight, r.mBottom);
    gGL.color4fv(SKY_TOP.mV);    gGL.vertex2i(r.mRight, r.mTop);
    gGL.end();

    if (!mMesh)
    {
        if (!mMessage.empty())
        {
            LLFontGL::getFontSansSerif()->renderUTF8(mMessage, 0, r.getCenterX(), r.getCenterY(), INK, LLFontGL::HCENTER, LLFontGL::VCENTER);
        }
        LLView::draw();
        return;
    }
    if (!mDragging)
    {
        mYaw += TURN_RAD_PER_S * llmin(mTurn.getElapsedTimeF32(), 0.1f);
        mTurn.reset();
        if (mYaw > F_TWO_PI) mYaw -= F_TWO_PI;
    }
    if (mYaw < 0.f) mYaw += F_TWO_PI;
    const Mesh& m = *mMesh;
    const F32 ce = cosf(ELEVATION_RAD), se = sinf(ELEVATION_RAD);
    // the turned mesh reaches radius * (cos e + sin e) up the picture at most; keep it inside with a margin
    const F32 px_per_m = 0.47f * (F32)llmin(r.getWidth(), r.getHeight()) / (m.radius * (ce + se));
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
    const F32 ca = cosf(mYaw), sa = sinf(mYaw);
    mProj.resize(m.verts.size());
    for (size_t i = 0; i < m.verts.size(); ++i)
    {
        const LLVector3 p = m.verts[i] - m.centre;
        const F32 x1 = p.mV[0] * ca - p.mV[1] * sa;
        const F32 y1 = p.mV[0] * sa + p.mV[1] * ca;
        const F32 y2 = y1 * ce - p.mV[2] * se;
        const F32 z2 = y1 * se + p.mV[2] * ce;
        mProj[i].set(cx + x1 * px_per_m, cy + z2 * px_per_m, y2);
    }
    // Every face is drawn, back to front. Unlike the plane's, no face is culled by its winding: the contract does not say which
    // way the generator winds its triangles, and the painter's order covers the far sides anyway.
    mOrder.clear();
    mOrder.reserve(m.tris.size());
    for (U32 i = 0; i < (U32)m.tris.size(); ++i)
    {
        const Tri& t = m.tris[i];
        mOrder.emplace_back(mProj[t.a].mV[2] + mProj[t.b].mV[2] + mProj[t.c].mV[2], i);
    }
    std::sort(mOrder.begin(), mOrder.end(), [](const std::pair<F32, U32>& l, const std::pair<F32, U32>& rr) { return l.first > rr.first; });
    LLVector3 light(-0.4f, -0.6f, 0.7f);
    light.normalize();
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& o : mOrder)
    {
        const Tri& t = m.tris[o.second];
        const LLColor4& col = m.palette[t.colour];
        const LLVector3 &a = mProj[t.a], &b = mProj[t.b], &c = mProj[t.c];
        LLVector3 n = (b - a) % (c - a);
        n.normalize();
        const F32 shade = 0.55f + 0.45f * fabsf(n * light);
        gGL.color4f(col.mV[VRED] * shade, col.mV[VGREEN] * shade, col.mV[VBLUE] * shade, 1.f);
        gGL.vertex2f(a.mV[0], a.mV[1]);
        gGL.vertex2f(b.mV[0], b.mV[1]);
        gGL.vertex2f(c.mV[0], c.mV[1]);
    }
    gGL.end();
    LLView::draw();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfFloaterBuildingGen
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterBuildingGen::WolfFloaterBuildingGen(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterBuildingGen::~WolfFloaterBuildingGen()
{
    if (mPreview && gFocusMgr.getMouseCapture() == mPreview) gFocusMgr.setMouseCapture(nullptr);
}

bool WolfFloaterBuildingGen::postBuild()
{
    mRequest = getChild<LLTextEditor>("request_text");
    mCounter = getChild<LLTextBox>("request_counter");
    mStyle = getChild<LLComboBox>("style");
    mStyleInfo = getChild<LLTextBox>("style_info");
    for (size_t i = 0; i < OPTION_BOXES; ++i)
    {
        mOptionBoxes.push_back(getChild<LLCheckBoxCtrl>(llformat("option_%d", (S32)i + 1)));
    }
    mOptionIds.assign(OPTION_BOXES, std::string());
    mQuote = getChild<LLTextBox>("quote_text");
    mAccess = getChild<LLTextBox>("access_text");
    mPlanTitle = getChild<LLTextBox>("plan_title");
    mPlanText = getChild<LLTextEditor>("plan_text");
    mBudget = getChild<LLTextBox>("budget_text");
    mSourcesLabel = getChild<LLTextBox>("sources_label");
    mSources = getChild<LLTextEditor>("sources_text");
    mPrice = getChild<LLTextEditor>("price_text");
    mBalance = getChild<LLTextBox>("balance_text");
    mStatus = getChild<LLTextBox>("status_text");
    mPlanBtn = getChild<LLButton>("plan_btn");
    mBuildBtn = getChild<LLButton>("build_btn");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mCancelBtn = getChild<LLButton>("cancel_btn");

    LLPanel* holder = getChild<LLPanel>("preview_panel");
    WolfBuildingPreview::Params pp;
    pp.name = "building_preview";
    pp.rect = holder->getLocalRect();
    pp.follows.flags = FOLLOWS_ALL;
    pp.mouse_opaque = true;
    mPreview = LLUICtrlFactory::create<WolfBuildingPreview>(pp);
    holder->addChild(mPreview);
    mPreview->setMessage(getString("no_plan_yet"));

    mStyle->add(getString("style_auto"), LLSD(STYLE_AUTO));
    mStyle->selectFirstItem();

    // Each lambda captures only `this`; the floater owns the controls, so they never call it after it is gone.
    mRequest->setKeystrokeCallback([this](LLTextEditor*) { onRequestEdited(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStyleChanged(); });
    mPlanBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPlan(); });
    mBuildBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mBuyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuyCredits(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancelJob(); });

    clearPlan();
    onRequestEdited();
    setStatus(std::string(), false);
    updateControls();
    return true;
}

void WolfFloaterBuildingGen::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] as the Plane Builder: a Wolf-only window opened off Wolf says so and closes.
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

void WolfFloaterBuildingGen::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterBuildingGen::draw()
{
    // draw() runs only while the window is open, so nothing below asks the yard once it is closed.
    if (!mReady && !mBusy && !mOpenFailed && mRefused.empty() && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mBusy = true;
        setStatus(getString("opening"), false);
        request("open", LLSD::emptyMap());
    }
    if (mReady && mAccessPlan && mQuoteDue && !mQuoting && mQuoteTimer.getElapsedTimeF32() > QUOTE_SETTLE_S)
    {
        mQuoteDue = false;
        if (!requestText().empty() && WolfGrid::isOnWolfTerritoriesConfirmed())
        {
            mQuoting = true;
            request("quote", LLSD().with("request", requestText()).with("style", styleId()));
        }
    }
    if (!mJob.empty() && !mPolling && mPoll.getElapsedTimeF32() > POLL_S && (mJobState == "queued" || mJobState == "running"))
    {
        mPolling = true;
        request("status", LLSD().with("job", mJob));
    }
    LLFloater::draw();
}

std::string WolfFloaterBuildingGen::requestText() const
{
    std::string text = mRequest->getText();
    LLStringUtil::trim(text);
    return text;
}

std::string WolfFloaterBuildingGen::styleId() const
{
    const std::string id = mStyle->getValue().asString();
    return id.empty() ? std::string(STYLE_AUTO) : id;
}

std::string WolfFloaterBuildingGen::quoteKey() const
{
    return styleId() + "\n" + requestText();
}

LLSD WolfFloaterBuildingGen::options() const
{
    // Source: building-generator-2026-11-01/README.md OPTIONS {"war_damage": bool, "interiors": bool, "props": bool,
    // "night_lights": bool}: one key per catalogue option, from its box.
    LLSD o = LLSD::emptyMap();
    for (size_t i = 0; i < OPTION_BOXES; ++i)
    {
        if (!mOptionIds[i].empty()) o[mOptionIds[i]] = mOptionBoxes[i]->get();
    }
    return o;
}

void WolfFloaterBuildingGen::readCredits(const LLSD& reply)
{
    // Source: building-generator-2026-11-01/README.md open / credits / credits_short: balance, available, buyurl (read live)
    if (reply.has("balance")) mBalanceNow = intOr(reply["balance"], -1);
    if (reply.has("available")) mAvailable = intOr(reply["available"], -1);
    if (reply.has("buyurl") && !reply["buyurl"].asString().empty()) mBuyUrl = reply["buyurl"].asString();
}

void WolfFloaterBuildingGen::onOpenReply(const LLSD& reply)
{
    // Source: building-generator-2026-11-01/README.md "open": catalogue, price_rules, balance, available, buyurl, access,
    // active, last_plan
    mCatalogue = reply["catalogue"];
    readCredits(reply);
    const LLSD& access = reply["access"];
    mAccessPlan = access["plan"].asBoolean();
    mAccessBuild = access["build"].asBoolean();
    mAccessReason = access["reason"].asString();

    // the styles: "Let the generator decide" first, then the catalogue's, each with its period and place as its tooltip
    mStyle->removeall();
    mStyle->add(getString("style_auto"), LLSD(STYLE_AUTO));
    for (const LLSD& s : llsd::inArray(mCatalogue["styles"]))
    {
        const std::string id = s["id"].asString();
        if (id.empty() || id == STYLE_AUTO) continue;
        std::string tip = s["period"].asString();
        if (!s["place"].asString().empty()) tip += (tip.empty() ? "" : ", ") + s["place"].asString();
        LLSD row;
        row["value"] = id;
        row["columns"][0]["value"] = s["title"].asString().empty() ? id : s["title"].asString();
        if (!tip.empty()) row["columns"][0]["tool_tip"] = tip;
        mStyle->addElement(row);
    }
    mStyle->selectFirstItem();

    // the four options, labelled and set as the catalogue says
    mOptionIds.assign(OPTION_BOXES, std::string());
    size_t n = 0;
    for (const LLSD& o : llsd::inArray(mCatalogue["options"]))
    {
        if (n >= OPTION_BOXES) break;
        const std::string id = o["id"].asString();
        if (id.empty()) continue;
        mOptionIds[n] = id;
        mOptionBoxes[n]->setLabel(o["title"].asString().empty() ? id : o["title"].asString());
        mOptionBoxes[n]->set(o["default"].asBoolean());
        ++n;
    }
    for (size_t i = 0; i < OPTION_BOXES; ++i)
    {
        mOptionBoxes[i]->setVisible(!mOptionIds[i].empty());
    }

    mReady = true;
    setStatus(std::string(), false);
    onStyleChanged();

    const LLSD& active = reply["active"];
    const LLSD& last = reply["last_plan"];
    if (last.isMap() && last["plan"].isMap())
    {
        showPlan(last, true);
    }
    if (active.isMap())
    {
        showJob(active);
    }
    else if (mPlanId.empty())
    {
        setStatus(getString(mAccessPlan ? "start_hint" : "plan_not_allowed"), !mAccessPlan);
    }
    updateCredits();
    updateControls();
}

void WolfFloaterBuildingGen::onRequestEdited()
{
    // 600 characters at most (the contract's): typing or pasting past it is cut back to the first 600
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
    // the quote waits until the words have been still for a second
    // (a Plan it press still waiting for its quote stays pending: the reply sees the change and says so)
    mQuoteDue = true;
    mQuoteTimer.reset();
    showQuote();
    updateCredits();
    updateControls();
}

void WolfFloaterBuildingGen::onStyleChanged()
{
    const std::string id = styleId();
    std::string info = getString("style_auto_info");
    std::string tip;
    for (const LLSD& s : llsd::inArray(mCatalogue["styles"]))
    {
        if (s["id"].asString() != id) continue;
        tip = s["period"].asString();
        if (!s["place"].asString().empty()) tip += (tip.empty() ? "" : ", ") + s["place"].asString();
        info = s["summary"].asString().empty() ? tip : s["summary"].asString();
        break;
    }
    mStyleInfo->setText(info);
    mStyle->setToolTip(tip.empty() ? getString("style_tip") : tip);
    // (a Plan it press still waiting for its quote stays pending: the reply sees the change and says so)
    mQuoteDue = true;
    mQuoteTimer.reset();
    showQuote();
    updateCredits();
    updateControls();
}

void WolfFloaterBuildingGen::showQuote()
{
    // Source: building-generator-2026-11-01/README.md "the quote ("Planning costs about N, at most M credits")"; research true
    // means a web search may run
    if (mReady && !mAccessPlan)
    {
        mQuote->setText(std::string());     // no quote is asked for: access.reason says why (access_text)
        return;
    }
    if (requestText().empty())
    {
        mQuote->setText(getString("quote_empty"));
        return;
    }
    if (mQuoteFor != quoteKey() || mQuoteAtMost < 0)
    {
        mQuote->setText(getString("quote_working"));
        return;
    }
    LLStringUtil::format_map_t a;
    a["[N]"] = llformat("%d", mQuoteTypical);
    a["[M]"] = llformat("%d", mQuoteAtMost);
    std::string text = getString("quote_line", a);
    if (mQuoteResearch) text += " " + getString("quote_research");
    mQuote->setText(text);
}

void WolfFloaterBuildingGen::clearPlan()
{
    mPlanId.clear();
    mPlanPrice = -1;
    mPreview->setMesh(nullptr);
    mPreview->setMessage(getString("no_plan_yet"));
    mPlanTitle->setText(std::string());
    mPlanText->setText(getString("no_plan_text"));
    mBudget->setText(std::string());
    mSourcesLabel->setVisible(false);
    mSources->setVisible(false);
    mSources->setText(std::string());
    mPrice->setText(std::string());
}

void WolfFloaterBuildingGen::showPlan(const LLSD& o, bool fromLastTime)
{
    // Source: building-generator-2026-11-01/README.md PLAN JOB when done: plan {title, summary, style {id, title, researched,
    // sources [{title, url}]}, layout, size, buildings [{name, type, storeys, w_m, d_m, h_m}], options, warnings}, budget
    // {objects, prims, land_impact, triangles, textures, texture_mb, scripts, lights}, preview, price, lines [{label, credits}],
    // ai_charged
    const LLSD& plan = o["plan"];
    mPlanId = o["job"].asString();
    mPlanPrice = intOr(o["price"], -1);

    std::shared_ptr<const WolfBuildingPreview::Mesh> mesh = WolfBuildingPreview::parse(o["preview"]);
    mPreview->setMesh(mesh);
    if (!mesh) mPreview->setMessage(getString("preview_bad"));

    mPlanTitle->setText(plan["title"].asString());
    std::string text = plan["summary"].asString();
    const LLSD& style = plan["style"];
    if (!style["title"].asString().empty())
    {
        LLStringUtil::format_map_t a;
        a["[STYLE]"] = style["title"].asString();
        text += (text.empty() ? "" : "\n") + getString(style["researched"].asBoolean() ? "plan_style_researched" : "plan_style", a);
    }
    for (const LLSD& b : llsd::inArray(plan["buildings"]))
    {
        LLStringUtil::format_map_t a;
        a["[NAME]"] = b["name"].asString();
        a["[TYPE]"] = b["type"].asString();
        a["[STOREYS]"] = llformat("%d", b["storeys"].asInteger());
        a["[SIZE]"] = llformat("%.0f x %.0f x %.0f m", b["w_m"].asReal(), b["d_m"].asReal(), b["h_m"].asReal());
        text += "\n" + getString(b["storeys"].asInteger() == 1 ? "building_line_one" : "building_line", a);
    }
    for (const LLSD& w : llsd::inArray(plan["warnings"]))
    {
        if (!w.asString().empty()) text += "\n" + getString("warning_prefix") + " " + w.asString();
    }
    mPlanText->setText(text);

    const LLSD& budget = o["budget"];
    if (budget.isMap())
    {
        // Source: README "the budget ("Land impact 312 - 14 objects - 41 textures, about 52 MB")"
        LLStringUtil::format_map_t a;
        a["[LI]"] = llformat("%d", budget["land_impact"].asInteger());
        a["[OBJECTS]"] = llformat("%d", budget["objects"].asInteger());
        a["[TEXTURES]"] = llformat("%d", budget["textures"].asInteger());
        a["[MB]"] = llformat("%ld", std::lround(budget["texture_mb"].asReal()));
        mBudget->setText(getString("budget_line", a));
    }
    else
    {
        mBudget->setText(std::string());
    }

    // the researched style's sources: links when they are plain web addresses, otherwise their titles only
    std::string sources;
    for (const LLSD& s : llsd::inArray(style["sources"]))
    {
        const std::string title = s["title"].asString();
        const std::string url = s["url"].asString();
        std::string line = title;
        if (webAddress(url)) line += (line.empty() ? "" : " - ") + url;
        if (!line.empty()) sources += (sources.empty() ? "" : "\n") + line;
    }
    mSourcesLabel->setVisible(!sources.empty());
    mSources->setVisible(!sources.empty());
    mSources->setText(sources);

    // the build price, line by line, then the total
    std::string price;
    for (const LLSD& l : llsd::inArray(o["lines"]))
    {
        price += llformat("%s: %d %s\n", l["label"].asString().c_str(), l["credits"].asInteger(), getString("credits_word").c_str());
    }
    if (mPlanPrice >= 0)
    {
        LLStringUtil::format_map_t a;
        a["[N]"] = llformat("%d", mPlanPrice);
        price += getString("price_total", a);
    }
    mPrice->setText(price);

    LLStringUtil::format_map_t a;
    a["[TITLE]"] = plan["title"].asString();
    a["[AI]"] = llformat("%d", o["ai_charged"].asInteger());
    a["[PRICE]"] = llformat("%d", llmax(mPlanPrice, 0));
    setStatus(getString(fromLastTime ? "plan_last_time" : "plan_ready", a), false);
    mShortOf = false;
    updateCredits();
    updateControls();
}

void WolfFloaterBuildingGen::updateCredits()
{
    if (mAvailable < 0)
    {
        mBalance->setText(getString("balance_unknown"));
        mBalance->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
        return;
    }
    // what the next press would cost: the build price once a plan is on show, otherwise the planning step's most
    const S32 need = !mPlanId.empty() ? mPlanPrice : (mQuoteFor == quoteKey() ? mQuoteAtMost : -1);
    const bool shortOf = mShortOf || (need >= 0 && mAvailable < need);
    LLStringUtil::format_map_t a;
    a["[N]"] = llformat("%d", mAvailable);
    a["[B]"] = llformat("%d", mBalanceNow < 0 ? mAvailable : mBalanceNow);
    mBalance->setText(getString(shortOf ? "balance_short" : "balance_ok", a));
    mBalance->setColor(LLUIColorTable::instance().getColor(shortOf ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterBuildingGen::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool open = wolf && mReady && mRefused.empty();
    const bool working = !mJob.empty();
    const bool canPlan = open && mAccessPlan && !working;
    mRequest->setEnabled(canPlan);
    mStyle->setEnabled(canPlan);
    for (LLCheckBoxCtrl* c : mOptionBoxes)
    {
        c->setEnabled(canPlan);
    }
    const bool quoted = mQuoteFor == quoteKey() && mQuoteAtMost >= 0;
    mPlanBtn->setEnabled(canPlan && !mBusy && !mQuoting && quoted && !requestText().empty() && mAvailable >= 0
                         && mAvailable >= mQuoteAtMost);
    mBuildBtn->setEnabled(open && mAccessBuild && !working && !mBusy && !mPlanId.empty() && mPlanPrice >= 0 && mAvailable >= 0
                          && mAvailable >= mPlanPrice);
    const S32 need = !mPlanId.empty() ? mPlanPrice : (quoted ? mQuoteAtMost : -1);
    mBuyBtn->setVisible(open && mAvailable >= 0 && (mShortOf || (need >= 0 && mAvailable < need)));
    mCancelBtn->setVisible(working && mJobState == "queued");
    mCancelBtn->setEnabled(!mBusy);

    // access.reason stays on show while planning or building is not allowed (the controls are greyed, never hidden)
    const bool limited = open && (!mAccessPlan || !mAccessBuild) && !mAccessReason.empty();
    mAccess->setText(limited ? mAccessReason : std::string());
    mAccess->setVisible(limited);
}

void WolfFloaterBuildingGen::onPlan()
{
    if (mBusy || mQuoting || !mJob.empty() || !mAccessPlan) return;
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        setStatus(getString("not_confirmed"), true);
        return;
    }
    if (requestText().empty())
    {
        setStatus(getString("request_needed"), true);
        return;
    }
    // the quote is asked again on the press; the plan goes only if it still says what the member was shown
    mPlanAfterQuote = true;
    mQuoting = true;
    mQuoteDue = false;
    setStatus(getString("checking_cost"), false);
    updateControls();
    request("quote", LLSD().with("request", requestText()).with("style", styleId()));
}

void WolfFloaterBuildingGen::onBuild()
{
    if (mBusy || !mJob.empty() || mPlanId.empty() || !mAccessBuild) return;
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        setStatus(getString("not_confirmed"), true);
        return;
    }
    // Source: building-generator-2026-11-01/README.md "build {"plan": uuid}: a BUILD JOB (queued); the build price is held now
    // and charged on delivery". The price is on show beside the button, and this runs only on the member's own press.
    mBusy = true;
    LLStringUtil::format_map_t a;
    a["[PRICE]"] = llformat("%d", llmax(mPlanPrice, 0));
    setStatus(getString("ordering", a), false);
    updateControls();
    request("build", LLSD().with("plan", mPlanId));
}

void WolfFloaterBuildingGen::onCancelJob()
{
    if (mJob.empty() || mBusy || mJobState != "queued") return;
    mBusy = true;
    setStatus(getString("cancelling"), false);
    updateControls();
    request("cancel", LLSD().with("job", mJob));
}

void WolfFloaterBuildingGen::onBuyCredits()
{
    // The yard's buyurl when it is a Wolf Territories page (the same check as WolfAI::openBuyPage, wolfai.cpp), else the AI
    // credits shop WolfAI knows.
    const bool wolf_page = !mBuyUrl.empty() && mBuyUrl.find_first_of("\\\r\n\t ") == std::string::npos
        && (mBuyUrl.rfind("https://www.wolf-grid.com/", 0) == 0 || mBuyUrl.rfind("https://wolf-grid.com/", 0) == 0);
    if (wolf_page) LLWeb::loadURLExternal(mBuyUrl);
    else WolfAI::instance().openBuyPage();
}

void WolfFloaterBuildingGen::showJob(const LLSD& o)
{
    // Source: building-generator-2026-11-01/README.md JOB: job, kind (plan | build), state (queued, running, done, failed,
    // cancelled), progress, error, position, request; BUILD JOB done: folder_name, charged, cost {credits}
    const std::string state = o["state"].asString();
    const std::string kind = o["kind"].asString();
    LLStringUtil::format_map_t a;
    a["[AHEAD]"] = llformat("%d", o["position"].asInteger());
    a["[PROGRESS]"] = o["progress"].asString().empty() ? getString("progress_none") : o["progress"].asString();
    a["[PRICE]"] = llformat("%d", o["price"].asInteger());
    a["[FOLDER]"] = o["folder_name"].asString();
    mPoll.reset();
    if (state == "queued" || state == "running")
    {
        mJob = o["job"].asString();
        mJobKind = kind;
        mJobState = state;
        if (kind == "build" && mPlanId.empty()) mPlanId = o["plan"].asString();
        const std::string prefix = kind == "build" ? "build_" : "plan_";
        const std::string which = state == "queued" ? (o["position"].asInteger() > 0 ? "waiting" : "next") : "running";
        setStatus(getString(prefix + which, a), false);
    }
    else
    {
        mJob.clear();
        mJobKind.clear();
        mJobState.clear();
        if (state == "done" && kind == "plan")
        {
            showPlan(o, false);
            WolfAI::instance().creditsUsed();
            request("credits", LLSD::emptyMap());
        }
        else if (state == "done")
        {
            // charged: true once the credits are taken (or the figure itself); cost {credits}: what it came to
            const LLSD& charged = o["charged"];
            S32 figure = intOr(o["cost"]["credits"], o["price"].asInteger());
            bool billed = true;
            if (charged.type() == LLSD::TypeBoolean) billed = charged.asBoolean();
            else if (charged.type() == LLSD::TypeInteger || charged.type() == LLSD::TypeReal) figure = charged.asInteger();
            a["[CHARGED]"] = llformat("%d", figure);
            setStatus(getString(billed ? "build_done" : "build_done_unbilled", a), false);
            // a building is built once: its plan is no longer on offer
            mPlanId.clear();
            mPlanPrice = -1;
            mPrice->setText(std::string());
            WolfAI::instance().creditsUsed();
            request("credits", LLSD::emptyMap());
        }
        else if (state == "cancelled")
        {
            setStatus(getString(kind == "build" ? "build_cancelled" : "plan_cancelled"), false);
        }
        else
        {
            const std::string err = o["error"].asString();
            setStatus(err.empty() ? getString(kind == "build" ? "build_failed" : "plan_failed") : err, true);
        }
    }
    updateCredits();
    updateControls();
}

void WolfFloaterBuildingGen::request(const std::string& what, const LLSD& body)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfBuildingGen", [h, what, body]() { requestCoro(h, what, body); });
}

// static
void WolfFloaterBuildingGen::requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body)
{
    // Source: wolffloaterplanebuilder.cpp requestCoro - WolfCaves::serviceRequest (POST JSON, X-Wolf-Agent / X-Wolf-Session,
    // retries 0), building-generator-2026-11-01/README.md "Transport exactly as the Plane Builder". The floater is looked up
    // again through its handle when the reply comes: closed and destroyed in the meantime, the reply is dropped.
    body["action"] = what;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(YARD_URL, &text, TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    const std::string error = ok ? std::string() : yardError(status, reply, transport);
    if (WolfFloaterBuildingGen* f = dynamic_cast<WolfFloaterBuildingGen*>(h.get()))
    {
        f->onReply(what, body, ok, reply, error);
    }
}

void WolfFloaterBuildingGen::onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error)
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
            mShortOf = false;    // a fresh balance replaces the yard's last "not enough"
        }
        else
        {
            setStatus(error, true);
        }
        updateCredits();
    }
    else if (what == "quote")
    {
        mQuoting = false;
        const bool planNow = mPlanAfterQuote;
        mPlanAfterQuote = false;
        const std::string key = body["style"].asString() + "\n" + body["request"].asString();
        if (key != quoteKey())
        {
            // the words or the style changed while it was asked: a new quote is due, this one is not shown
            mQuoteDue = true;
            mQuoteTimer.reset();
            if (planNow) setStatus(getString("changed_while_checking"), true);
        }
        else if (!ok)
        {
            mQuoteFor.clear();
            mQuoteAtMost = -1;
            setStatus(error, true);
            showQuote();
        }
        else
        {
            // Source: README quote {typical, at_most, research}
            const LLSD& q = reply["quote"];
            const S32 shown = (mQuoteFor == key) ? mQuoteAtMost : -1;
            mQuoteFor = key;
            mQuoteTypical = intOr(q["typical"], -1);
            mQuoteAtMost = intOr(q["at_most"], -1);
            mQuoteResearch = q["research"].asBoolean();
            showQuote();
            if (planNow)
            {
                if (mQuoteAtMost < 0)
                {
                    setStatus(getString("quote_missing"), true);
                }
                else if (shown != mQuoteAtMost)
                {
                    // never plan at a figure the member has not seen: show it and wait for another press
                    LLStringUtil::format_map_t a;
                    a["[M]"] = llformat("%d", mQuoteAtMost);
                    setStatus(getString("quote_moved", a), true);
                }
                else if (mAvailable >= 0 && mAvailable < mQuoteAtMost)
                {
                    setStatus(getString("plan_short"), true);
                }
                else
                {
                    // Source: README plan {"request", "style", "options", "at_most"}: at_most = the quote the member saw
                    mBusy = true;
                    setStatus(getString("planning_send"), false);
                    LLSD p;
                    p["request"] = body["request"];
                    p["style"] = body["style"];
                    p["options"] = options();
                    p["at_most"] = mQuoteAtMost;
                    request("plan", p);
                }
            }
        }
        updateCredits();
    }
    else if (what == "plan" || what == "build")
    {
        mBusy = false;
        if (ok)
        {
            showJob(reply);
        }
        else
        {
            readCredits(reply);   // credits_short carries balance, available and buyurl
            if (code == "credits_short") mShortOf = true;
            if (code == "quote_changed")
            {
                // the yard's figure moved: ask again so the new one is on show before another press
                mQuoteFor.clear();
                mQuoteAtMost = -1;
                mQuoteDue = true;
                mQuoteTimer.reset();
                showQuote();
            }
            if (code == "plan_expired") clearPlan();
            setStatus(error, true);
            updateCredits();
        }
    }
    else if (what == "status" || what == "cancel")
    {
        if (what == "status") mPolling = false;
        else mBusy = false;
        mPoll.reset();
        // a reply about a job that is no longer the one followed (one finished and another started) is ignored
        if (body["job"].asString() == mJob && !mJob.empty())
        {
            if (ok) showJob(reply);
            else setStatus(error, true);
        }
    }
    updateControls();
}
