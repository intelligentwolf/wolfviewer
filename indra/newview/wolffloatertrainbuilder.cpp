/**
 * @file wolffloatertrainbuilder.cpp
 * @brief WolfViewer: the Train Builder. See wolffloatertrainbuilder.h and region-terrain-work/train-generator-2026-11-14/
 *        DESIGN.md / README.md ("Viewer contract"). Copied from wolffloaterplanebuilder.cpp (2026-11-01), its transport exactly.
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

#include "wolffloatertrainbuilder.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <boost/json.hpp>

#include "llbase64.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llfocusmgr.h"
#include "llimage.h"
#include "lllineeditor.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewermenufile.h"
#include "wolfai.h"
#include "wolfcave.h"
#include "wolfgrid.h"

std::map<std::string, std::shared_ptr<const WolfTrainPreview::Mesh>> WolfFloaterTrainBuilder::sMeshes;

namespace
{
    // Source: train-generator-2026-11-14/README.md "Endpoint: https://wolfstorm.app/php/train_builder.php" (the plane's host).
    const char* YARD_URL = "https://wolfstorm.app/php/train_builder.php";
    // Source: wolffloaterplanebuilder.cpp TIMEOUT_S / PREVIEW_TIMEOUT_S (a 60 s timeout, or 120 s for preview).
    constexpr F32 TIMEOUT_S = 60.f, PREVIEW_TIMEOUT_S = 120.f;
    // Source: wolffloaterplanebuilder.cpp POLL_S ("polls status every 5 s, only while open and queued or running").
    constexpr F32 POLL_S = 5.f;
    constexpr F32 TURN_RAD_PER_S = F_TWO_PI / 30.f;
    constexpr F32 ELEVATION_RAD = 15.f * DEG_TO_RAD;    // a train is long and low: seen from a little above
    // Source: tb_classes.py TRAIN_NAME_CHARS 24, COMPANY_CHARS 32; trainbuilder_lib.php TB_NAME_MAX / TB_COMPANY_MAX
    constexpr S32 TRAIN_NAME_CHARS = 24, COMPANY_CHARS = 32;
    // Source: tb_classes.py NUMBER_RE "^[A-Z0-9 \-]{0,8}$" (lower case is sent as capitals)
    constexpr S32 NUMBER_CHARS = 8;
    // Source: trainbuilder_lib.php TB_LOGO_IN_MAX (1048576 bytes of the picture uploaded)
    constexpr S64 LOGO_MAX_BYTES = 1048576;
    // Source: trainbuilder_lib.php TB_LOGO_IN_MAX: PNG or JPEG only (it checks the first bytes)
    LLColor4 rgb(S32 r, S32 g, S32 b) { return LLColor4((F32)r / 255.f, (F32)g / 255.f, (F32)b / 255.f, 1.f); }
    const LLColor4 C_OTHER = rgb(128, 128, 128);
    const LLColor4 SKY_TOP = rgb(150, 182, 210), SKY_BOTTOM = rgb(214, 226, 236), INK = rgb(40, 48, 60);
    // the starting colours before the catalogue arrives (the Castle's deep green: tb_classes.py)
    const LLColor4 DEFAULT_MAIN = rgb(22, 84, 52), DEFAULT_SECOND = rgb(226, 212, 176);

    S32 to255(F32 v) { return llclamp((S32)std::lround(v * 255.f), 0, 255); }
    LLSD colourOf(const LLColor4& c)
    {
        LLSD a = LLSD::emptyArray();
        a.append(to255(c.mV[VRED]));
        a.append(to255(c.mV[VGREEN]));
        a.append(to255(c.mV[VBLUE]));
        return a;
    }
    LLColor4 colourFrom(const LLSD& a, const LLColor4& fallback)
    {
        if (!a.isArray() || a.size() != 3) return fallback;
        return rgb(llclamp(a[0].asInteger(), 0, 255), llclamp(a[1].asInteger(), 0, 255), llclamp(a[2].asInteger(), 0, 255));
    }
    bool sameColour(const LLColor4& a, const LLColor4& b)
    {
        return to255(a.mV[VRED]) == to255(b.mV[VRED]) && to255(a.mV[VGREEN]) == to255(b.mV[VGREEN]) && to255(a.mV[VBLUE]) == to255(b.mV[VBLUE]);
    }
    // "#rrggbb" (make_preview.py writes every fixed group so); anything else is grey, never a wrong livery colour
    bool hexColour(const std::string& s, LLColor4& out)
    {
        if (s.size() != 7 || s[0] != '#') return false;
        S32 v[3];
        for (S32 k = 0; k < 3; ++k)
        {
            S32 n = 0;
            for (S32 j = 0; j < 2; ++j)
            {
                const char ch = s[1 + k * 2 + j];
                const S32 d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
                if (d < 0) return false;
                n = n * 16 + d;
            }
            v[k] = n;
        }
        out = rgb(v[0], v[1], v[2]);
        return true;
    }

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
            case 404: return "The Train Builder yard is not open on this grid yet.";
            case 413: return "The request was too large for the yard.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 500: return "The yard hit an error - try again in a moment.";
            case 503: return "The yard is not answering - try again in a moment.";
            default:
                return transport.empty() ? llformat("The yard answered HTTP %d.", status)
                                         : "Could not reach the yard: " + transport;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfTrainPreview (copied from WolfPlanePreview, wolffloaterplanebuilder.cpp)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfTrainPreview::WolfTrainPreview(const Params& p)
    : LLView(p)
{
    mTurn.reset();
}

// static
std::shared_ptr<const WolfTrainPreview::Mesh> WolfTrainPreview::parse(const LLSD& pv)
{
    // Source: make_preview.py preview(): v = x,y,z centimetres (ints), t = 4 ints a triangle a,b,c,group, groups = names
    // (the plane's format: wolffloaterplanebuilder.cpp WolfPlanePreview::parse)
    if (!pv.isMap() || !pv["v"].isArray() || !pv["t"].isArray() || !pv["groups"].isArray()) return nullptr;
    auto m = std::make_shared<Mesh>();
    for (const LLSD& g : llsd::inArray(pv["groups"])) m->groups.push_back(g.asString());
    if (m->groups.size() > 65535) return nullptr;
    const LLSD& v = pv["v"];
    const S32 nv = (S32)(v.size() / 3);   // LLSD::size() is size_t (MSVC C4267 is an error)
    if (nv <= 0 || v.size() % 3) return nullptr;
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
    const S32 ng = (S32)m->groups.size();
    m->tris.reserve(nt);
    for (S32 i = 0; i < nt; ++i)
    {
        const S32 a = t[i * 4].asInteger(), b = t[i * 4 + 1].asInteger(), c = t[i * 4 + 2].asInteger();
        const S32 g = t[i * 4 + 3].asInteger();
        if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv || g < 0 || g >= ng) return nullptr;
        m->tris.push_back(Tri{ (U32)a, (U32)b, (U32)c, (U16)g });
    }
    m->centre = (lo + hi) * 0.5f;
    const LLVector3 ext = hi - lo;
    m->radius = llmax(0.5f * llmax(ext.mV[0], ext.mV[1]), 0.5f * ext.mV[2], 1.f);
    return m;
}

void WolfTrainPreview::setMesh(std::shared_ptr<const Mesh> mesh)
{
    mMesh = std::move(mesh);
    mPalette.clear();
}

const LLColor4& WolfTrainPreview::colourOf(U16 group)
{
    if (mPalette.empty() && mMesh)
    {
        mPalette.reserve(mMesh->groups.size());
        for (const std::string& g : mMesh->groups)
        {
            LLColor4 c = C_OTHER;
            auto it = mColours.find(g);
            if (it != mColours.end()) c = it->second;
            else hexColour(g, c);
            mPalette.push_back(c);
        }
    }
    return group < mPalette.size() ? mPalette[group] : C_OTHER;
}

bool WolfTrainPreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    mLastX = x;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfTrainPreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
        mTurn.reset();
    }
    return true;
}

void WolfTrainPreview::onMouseCaptureLost()
{
    mDragging = false;
    mTurn.reset();
}

bool WolfTrainPreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mYaw += (F32)(x - mLastX) * 0.01f;
        mLastX = x;
    }
    return LLView::handleHover(x, y, mask);
}

void WolfTrainPreview::draw()
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
    const Mesh& m = *mMesh;
    const F32 px_per_m = 0.47f * (F32)llmin(r.getWidth(), r.getHeight()) / m.radius * 1.6f;   // a long train: fill the width
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
    const F32 ca = cosf(mYaw), sa = sinf(mYaw), ce = cosf(ELEVATION_RAD), se = sinf(ELEVATION_RAD);
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
    mOrder.clear();
    mOrder.reserve(m.tris.size());
    for (U32 i = 0; i < (U32)m.tris.size(); ++i)
    {
        const Tri& t = m.tris[i];
        const LLVector3 &a = mProj[t.a], &b = mProj[t.b], &c = mProj[t.c];
        // as the plane's: a face wound clockwise on screen faces away
        const F32 cross = (b.mV[0] - a.mV[0]) * (c.mV[1] - a.mV[1]) - (c.mV[0] - a.mV[0]) * (b.mV[1] - a.mV[1]);
        if (cross < 0.f) continue;
        mOrder.emplace_back(a.mV[2] + b.mV[2] + c.mV[2], i);
    }
    std::sort(mOrder.begin(), mOrder.end(), [](const std::pair<F32, U32>& l, const std::pair<F32, U32>& rr) { return l.first > rr.first; });
    LLVector3 light(-0.4f, -0.6f, 0.7f);
    light.normalize();
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& o : mOrder)
    {
        const Tri& t = m.tris[o.second];
        const LLColor4& col = colourOf(t.group);
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
// WolfFloaterTrainBuilder
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterTrainBuilder::WolfFloaterTrainBuilder(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterTrainBuilder::~WolfFloaterTrainBuilder()
{
    if (mPreview && gFocusMgr.getMouseCapture() == mPreview) gFocusMgr.setMouseCapture(nullptr);
}

bool WolfFloaterTrainBuilder::postBuild()
{
    mType = getChild<LLComboBox>("train_type");
    mCarriages = getChild<LLSliderCtrl>("carriages");
    mCarriagesLabel = getChild<LLTextBox>("carriages_label");
    mStyle = getChild<LLComboBox>("carriage_style");
    mLower = getChild<LLColorSwatchCtrl>("coach_lower");
    mUpper = getChild<LLColorSwatchCtrl>("coach_upper");
    mLivery = getChild<LLComboBox>("livery");
    mMain = getChild<LLColorSwatchCtrl>("main_colour");
    mSecond = getChild<LLColorSwatchCtrl>("second_colour");
    mName = getChild<LLLineEditor>("train_name");
    mNumber = getChild<LLLineEditor>("train_number");
    mCompany = getChild<LLLineEditor>("company");
    mLogoBtn = getChild<LLButton>("logo_btn");
    mLogoRemoveBtn = getChild<LLButton>("logo_remove_btn");
    mLogoText = getChild<LLTextBox>("logo_text");
    mLogoOk = getChild<LLCheckBoxCtrl>("logo_ok");
    mPublic = getChild<LLCheckBoxCtrl>("public_check");
    mSpeed = getChild<LLSliderCtrl>("speed");
    mFigures = getChild<LLTextBox>("figures_text");
    mNameProblem = getChild<LLTextBox>("name_problem");
    mNumberProblem = getChild<LLTextBox>("number_problem");
    mCompanyProblem = getChild<LLTextBox>("company_problem");
    mPrice = getChild<LLTextBox>("price_text");
    mBalance = getChild<LLTextBox>("balance_text");
    mStatus = getChild<LLTextBox>("status_text");
    mBuildBtn = getChild<LLButton>("build_btn");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mCancelBtn = getChild<LLButton>("cancel_btn");

    LLPanel* holder = getChild<LLPanel>("preview_panel");
    WolfTrainPreview::Params pp;
    pp.name = "train_preview";
    pp.rect = holder->getLocalRect();
    pp.follows.flags = FOLLOWS_ALL;
    pp.mouse_opaque = true;
    mPreview = LLUICtrlFactory::create<WolfTrainPreview>(pp);
    holder->addChild(mPreview);
    mPreview->setMessage(getString("loading"));

    mMain->set(DEFAULT_MAIN);
    mSecond->set(DEFAULT_SECOND);
    mLower->set(DEFAULT_SECOND);
    mUpper->set(DEFAULT_SECOND);

    mType->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClassChanged(); });
    mLivery->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLiveryChanged(); });
    mStyle->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStyleChanged(); });
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mMain, mSecond, mLower, mUpper })
    {
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { onColourChanged(); });
    }
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mCarriages, mSpeed, mPublic, mLogoOk })
    {
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDesignChanged(); });
    }
    for (LLLineEditor* e : { mName, mNumber, mCompany })
    {
        e->setKeystrokeCallback([this](LLLineEditor*, void*) { onDesignChanged(); }, nullptr);
    }
    mLogoBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChooseLogo(); });
    mLogoRemoveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onRemoveLogo(); });
    mBuildBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mBuyBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfAI::instance().openBuyPage(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancelOrder(); });
    mLogoText->setText(getString("logo_none"));
    setStatus(std::string(), false);
    updateControls();
    return true;
}

void WolfFloaterTrainBuilder::onOpen(const LLSD& key)
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
        if (!mOrder.empty()) request("status", LLSD().with("job", mOrder));
    }
    updateControls();
}

void WolfFloaterTrainBuilder::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterTrainBuilder::draw()
{
    if (!mReady && !mBusy && !mOpenFailed && mRefused.empty() && WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        mBusy = true;
        setStatus(getString("opening"), false);
        request("open", LLSD::emptyMap());
    }
    if (!mOrder.empty() && !mPolling && mPoll.getElapsedTimeF32() > POLL_S && (mOrderState == "queued" || mOrderState == "running"))
    {
        mPolling = true;
        request("status", LLSD().with("job", mOrder));
    }
    LLFloater::draw();
}

const LLSD* WolfFloaterTrainBuilder::currentClass() const
{
    const std::string id = mType->getValue().asString();
    const LLSD& classes = mCatalogue["classes"];
    for (LLSD::array_const_iterator it = classes.beginArray(); it != classes.endArray(); ++it)
    {
        if ((*it)["id"].asString() == id) return &(*it);
    }
    return nullptr;
}

const LLSD* WolfFloaterTrainBuilder::currentLivery() const
{
    const LLSD* c = currentClass();
    if (!c) return nullptr;
    const std::string id = mLivery->getValue().asString();
    const LLSD& lv = (*c)["liveries"];
    for (LLSD::array_const_iterator it = lv.beginArray(); it != lv.endArray(); ++it)
    {
        if ((*it)["id"].asString() == id) return &(*it);
    }
    return nullptr;
}

const LLSD* WolfFloaterTrainBuilder::currentStyle() const
{
    const LLSD* c = currentClass();
    if (!c) return nullptr;
    const std::string id = mStyle->getValue().asString();
    const LLSD& st = (*c)["styles"];
    for (LLSD::array_const_iterator it = st.beginArray(); it != st.endArray(); ++it)
    {
        if ((*it)["id"].asString() == id) return &(*it);
    }
    return nullptr;
}

bool WolfFloaterTrainBuilder::hasSecond() const
{
    // Source: make_preview.py catalogue(): "groups" lists the class's colour groups ("main", and "second" when it has one)
    const LLSD* c = currentClass();
    return c && (*c)["groups"].size() > 1;
}

S32 WolfFloaterTrainBuilder::carriages() const
{
    return (S32)std::lround(mCarriages->getValueF32());
}

std::string WolfFloaterTrainBuilder::textProblem(const std::string& raw, bool required, S32 max_chars) const
{
    // Source: tb_classes.py NAME_RE / trainbuilder_lib.php TB_NAME_RE - Latin letters, digits, space and ' - . &, at least one
    // letter or digit (the planes' rule, wolffloaterplanebuilder.cpp nameProblem)
    std::string text = raw;
    LLStringUtil::trim(text);
    if (text.empty()) return required ? getString("name_needed") : std::string();
    const LLWString w = utf8str_to_wstring(text);
    if ((S32)w.size() > max_chars) return llformat(getString("text_long").c_str(), max_chars);
    bool any = false;
    for (llwchar ch : w)
    {
        const bool latin = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= 0xC0 && ch <= 0x24F && ch != 0xD7 && ch != 0xF7)
                           || (ch >= 0x1E00 && ch <= 0x1EFF);
        const bool digit = ch >= '0' && ch <= '9';
        if (latin || digit) { any = true; continue; }
        if (ch == ' ' || ch == '\'' || ch == '-' || ch == '.' || ch == '&') continue;
        return getString("name_letters");
    }
    if (!any) return getString("name_letters");
    return std::string();
}

std::string WolfFloaterTrainBuilder::numberProblem() const
{
    // Source: tb_classes.py NUMBER_RE "^[A-Z0-9 \-]{0,8}$" (lower case is taken as capitals)
    std::string n = mNumber->getText();
    LLStringUtil::trim(n);
    if ((S32)n.size() > NUMBER_CHARS) return llformat(getString("number_long").c_str(), NUMBER_CHARS);
    for (char ch : n)
    {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-';
        if (!ok) return getString("number_chars");
    }
    return std::string();
}

std::string WolfFloaterTrainBuilder::logoProblem() const
{
    // Source: trainbuilder_lib.php tb_check_design: a logo needs "logo_ok" (the member confirms it is theirs to use)
    if (!mLogo.empty() && !mLogoOk->get()) return getString("logo_confirm");
    return std::string();
}

LLSD WolfFloaterTrainBuilder::design() const
{
    // Source: DESIGN.md s2 "the DESIGN sent by the window"; tb_classes.py check()
    LLSD d;
    const LLSD* c = currentClass();
    d["class"] = mType->getValue().asString();
    const std::string livery = mLivery->getValue().asString();
    d["livery"] = livery.empty() ? std::string("custom") : livery;
    d["main"] = colourOf(mMain->get());
    d["second"] = colourOf(mSecond->get());
    d["carriages"] = carriages();
    if (c && (*c)["styles"].size() > 0)
    {
        const std::string st = mStyle->getValue().asString();
        d["carriage_style"] = st;
        if (st == "custom")
        {
            d["carriage_lower"] = colourOf(mLower->get());
            d["carriage_upper"] = colourOf(mUpper->get());
        }
    }
    std::string name = mName->getText(), number = mNumber->getText(), company = mCompany->getText();
    LLStringUtil::trim(name);
    LLStringUtil::trim(number);
    LLStringUtil::trim(company);
    LLStringUtil::toUpper(number);
    d["name"] = name;
    d["number"] = number;
    d["company"] = company;
    d["speed"] = (S32)std::lround(mSpeed->getValueF32());
    d["public"] = mPublic->get();
    d["logo"] = mLogo;
    d["logo_ok"] = !mLogo.empty() && mLogoOk->get();
    return d;
}

void WolfFloaterTrainBuilder::onOpenReply(const LLSD& reply)
{
    mCatalogue = reply["catalogue"];
    mPriceRules = reply["price_rules"];
    mBalanceNow = reply["balance"].isUndefined() ? -1 : reply["balance"].asInteger();
    mAvailable = reply["available"].isUndefined() ? -1 : reply["available"].asInteger();
    mType->removeall();
    for (const LLSD& c : llsd::inArray(mCatalogue["classes"]))
    {
        mType->add(c["title"].asString(), c["id"]);
    }
    if (mCatalogue["classes"].size() > 0) mType->selectFirstItem();
    mReady = true;
    setStatus(std::string(), false);
    onClassChanged();
    if (reply["active"].isMap()) showOrder(reply["active"]);
}

void WolfFloaterTrainBuilder::onClassChanged()
{
    const LLSD* c = currentClass();
    if (!c) return;
    // its liveries (our own colour pairs), then the member's own colours
    mLivery->removeall();
    for (const LLSD& lv : llsd::inArray((*c)["liveries"])) mLivery->add(lv["title"].asString(), lv["id"]);
    mLivery->add(getString("custom_livery"), LLSD("custom"));
    mLivery->selectFirstItem();
    // carriages: the class's range (make_preview.py catalogue "carriages" {min, max, default})
    const LLSD& cr = (*c)["carriages"];
    mCarriages->setMinValue((F32)cr["min"].asInteger());
    mCarriages->setMaxValue((F32)llmax(cr["max"].asInteger(), cr["min"].asInteger()));
    mCarriages->setValue(LLSD((F64)cr["default"].asInteger()));
    const std::string word = (*c)["carriage_word"].asString();
    mCarriagesLabel->setText(word.empty() ? getString("carriages_fixed") : llformat(getString("carriages_how_many").c_str(), word.c_str()));
    // styles: the class's, plus "Your own colours" for coaches (catalogue "custom_style")
    mStyle->removeall();
    for (const LLSD& st : llsd::inArray((*c)["styles"])) mStyle->add(st["title"].asString(), st["id"]);
    if ((*c)["custom_style"].asBoolean()) mStyle->add(getString("custom_livery"), LLSD("custom"));
    if (!(*c)["style_default"].asString().empty()) mStyle->setValue((*c)["style_default"]);
    else mStyle->selectFirstItem();
    // top speed: up to the class's (wolf-rail train.start takes 0-30 m/s)
    mSpeed->setMinValue(3.f);
    mSpeed->setMaxValue((F32)(*c)["max_speed"].asInteger());
    mSpeed->setValue(LLSD((F64)(*c)["speed_default"].asInteger()));
    // the class's own name and number as the suggestion (a member's own name is kept)
    mName->setLabel((*c)["name_default"].asString());
    mNumber->setLabel((*c)["number_default"].asString());
    onLiveryChanged();
    onStyleChanged();

    const std::string id = (*c)["id"].asString();
    auto it = sMeshes.find(id);
    if (it != sMeshes.end())
    {
        mPreview->setMesh(it->second);
        mPreviewClass = id;
    }
    else if (mPreviewClass != id)
    {
        mPreviewClass = id;
        mPreview->setMesh(nullptr);
        mPreview->setMessage(getString("loading"));
        request("preview", LLSD().with("class", id));
    }
    onDesignChanged();
}

void WolfFloaterTrainBuilder::onLiveryChanged()
{
    if (const LLSD* lv = currentLivery())
    {
        mSettingColours = true;
        mMain->set(colourFrom((*lv)["main"], DEFAULT_MAIN));
        if ((*lv).has("second")) mSecond->set(colourFrom((*lv)["second"], DEFAULT_SECOND));
        mSettingColours = false;
    }
    onDesignChanged();
}

void WolfFloaterTrainBuilder::onStyleChanged()
{
    if (const LLSD* st = currentStyle(); st && (*st).has("lower"))
    {
        mSettingColours = true;
        mLower->set(colourFrom((*st)["lower"], DEFAULT_SECOND));
        mUpper->set(colourFrom((*st)["upper"], DEFAULT_SECOND));
        mSettingColours = false;
    }
    onDesignChanged();
}

void WolfFloaterTrainBuilder::onColourChanged()
{
    // a colour changed by hand turns the livery (or the coach style) into the member's own, unless it still matches it
    if (!mSettingColours)
    {
        const LLSD* lv = currentLivery();
        if (lv && !(sameColour(mMain->get(), colourFrom((*lv)["main"], DEFAULT_MAIN))
                    && (!hasSecond() || sameColour(mSecond->get(), colourFrom((*lv)["second"], DEFAULT_SECOND)))))
        {
            mLivery->setValue(LLSD("custom"));
        }
        const LLSD* c = currentClass();
        const LLSD* st = currentStyle();
        if (c && (*c)["custom_style"].asBoolean() && st && (*st).has("lower")
            && !(sameColour(mLower->get(), colourFrom((*st)["lower"], DEFAULT_SECOND)) && sameColour(mUpper->get(), colourFrom((*st)["upper"], DEFAULT_SECOND))))
        {
            mStyle->setValue(LLSD("custom"));
        }
    }
    onDesignChanged();
}

void WolfFloaterTrainBuilder::updateFigures()
{
    const LLSD* c = currentClass();
    if (!c) return;
    // Source: make_preview.py catalogue(): length = fixed_len + carriages x carriage_len + gap x (vehicles - 1); seats the
    // same way; vehicles = carriages + vehicles_extra
    const S32 n = carriages();
    const S32 vehicles = n + (*c)["vehicles_extra"].asInteger();
    const F64 len = (*c)["fixed_len"].asReal() + n * (*c)["carriage_len"].asReal() + (*c)["gap_m"].asReal() * llmax(0, vehicles - 1);
    const S32 seats = (*c)["fixed_seats"].asInteger() + n * (*c)["carriage_seats"].asInteger();
    const S32 speed = (S32)std::lround(mSpeed->getValueF32());
    LLStringUtil::format_map_t a;
    a["[TITLE]"] = (*c)["title"].asString();
    a["[ERA]"] = (*c)["era_text"].asString();
    a["[LEN]"] = llformat("%.0f", len);
    a["[VEHICLES]"] = llformat("%d", vehicles);
    a["[SEATS]"] = llformat("%d", seats);
    a["[SPEED]"] = llformat("%d", speed);
    a["[KMH]"] = llformat("%d", (S32)std::lround(speed * 3.6));
    a["[ABOUT]"] = (*c)["desc"].asString();
    mFigures->setText(getString(seats > 0 ? "figures" : "figures_no_seats", a));
}

void WolfFloaterTrainBuilder::onDesignChanged()
{
    if (currentClass())
    {
        std::map<std::string, LLColor4> colours;
        colours["main"] = mMain->get();
        colours["second"] = hasSecond() ? mSecond->get() : mMain->get();
        colours["carriage_lower"] = mLower->get();
        colours["carriage_upper"] = mUpper->get();
        mPreview->setColours(colours);
        updateFigures();
    }
    mNameProblem->setText(mReady ? textProblem(mName->getText(), true, TRAIN_NAME_CHARS) : std::string());
    mNumberProblem->setText(mReady ? numberProblem() : std::string());
    mCompanyProblem->setText(mReady ? textProblem(mCompany->getText(), false, COMPANY_CHARS) : std::string());
    updatePrice();
    updateControls();
}

void WolfFloaterTrainBuilder::updatePrice()
{
    const LLSD* c = currentClass();
    if (!c || !mPriceRules.isMap())
    {
        mPrice->setText(std::string());
        mPriceNow = 0;
        return;
    }
    // Source: trainbuilder_lib.php tb_price: base + per_vehicle x vehicles (the yard's figure is the one charged)
    const S32 vehicles = carriages() + (*c)["vehicles_extra"].asInteger();
    const S32 base = mPriceRules["base"].asInteger();
    const S32 each = mPriceRules["per_vehicle"].asInteger() * vehicles;
    const S32 total = base + each;
    LLStringUtil::format_map_t p;
    p["[BASE]"] = llformat("%d", base);
    p["[N]"] = llformat("%d", vehicles);
    p["[EACH]"] = llformat("%d", each);
    p["[TOTAL]"] = llformat("%d", total);
    mPrice->setText(getString(vehicles == 1 ? "price_one" : "price", p));
    mPriceNow = total;
    if (mAvailable < 0)
    {
        mBalance->setText(getString("balance_unknown"));
        mBalance->setColor(LLUIColorTable::instance().getColor("LabelTextColor"));
    }
    else
    {
        LLStringUtil::format_map_t a;
        a["[N]"] = llformat("%d", mAvailable);
        const bool shortOf = mAvailable < total;
        mBalance->setText(getString(shortOf ? "balance_short" : "balance_ok", a));
        mBalance->setColor(LLUIColorTable::instance().getColor(shortOf ? "AlertCautionTextColor" : "LabelTextColor"));
    }
}

void WolfFloaterTrainBuilder::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool open = wolf && mReady && mRefused.empty();
    const bool ordering = !mOrder.empty();
    const bool edit = open && !ordering;
    const LLSD* c = currentClass();
    for (LLUICtrl* u : std::initializer_list<LLUICtrl*>{ mType, mLivery, mMain, mName, mNumber, mCompany, mPublic, mSpeed })
    {
        u->setEnabled(edit);
    }
    mSecond->setEnabled(edit && hasSecond());
    mCarriages->setEnabled(edit && c && (*c)["carriages"]["max"].asInteger() > (*c)["carriages"]["min"].asInteger());
    const bool styles = c && (*c)["styles"].size() > 0;
    mStyle->setEnabled(edit && styles);
    const bool coachColours = styles && (*c)["custom_style"].asBoolean();
    mLower->setVisible(coachColours);
    mUpper->setVisible(coachColours);
    mLower->setEnabled(edit && coachColours);
    mUpper->setEnabled(edit && coachColours);
    mLogoBtn->setEnabled(edit && !mLogoBusy);
    mLogoRemoveBtn->setVisible(!mLogo.empty());
    mLogoRemoveBtn->setEnabled(edit && !mLogoBusy);
    mLogoOk->setVisible(!mLogo.empty());
    mLogoOk->setEnabled(edit);
    const bool shortOf = open && mAvailable >= 0 && mAvailable < mPriceNow;
    mBuildBtn->setEnabled(open && !mBusy && !mLogoBusy && !ordering && !shortOf && mAvailable >= 0
                          && textProblem(mName->getText(), true, TRAIN_NAME_CHARS).empty() && numberProblem().empty()
                          && textProblem(mCompany->getText(), false, COMPANY_CHARS).empty() && logoProblem().empty());
    mBuyBtn->setVisible(shortOf);
    mCancelBtn->setVisible(ordering && mOrderState == "queued");
}

void WolfFloaterTrainBuilder::onChooseLogo()
{
    if (mLogoBusy || !mOrder.empty()) return;
    // The image picker as File > Upload > Image uses it (llviewermenufile.cpp: startPicker(..., FFLOAD_IMAGE, false); as
    // wolffloaterbugreport.cpp onAddPicture). The callback comes back on the main thread; the floater may have closed.
    const LLHandle<LLFloater> handle = getHandle();
    LLFilePickerReplyThread::startPicker(
        [handle](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (WolfFloaterTrainBuilder* self = dynamic_cast<WolfFloaterTrainBuilder*>(handle.get()))
            {
                self->onLogoFilePicked(filenames);
            }
        },
        LLFilePicker::FFLOAD_IMAGE, false);
}

void WolfFloaterTrainBuilder::onLogoFilePicked(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty() || mLogoBusy) return;   // the picker was cancelled
    const std::string& path = filenames[0];
    const std::string name = gDirUtilp->getBaseFileName(path, false);
    // FFLOAD_IMAGE offers tga, bmp, jpg, jpeg and png (llfilepicker.cpp); the yard takes PNG and JPEG (trainbuilder_lib.php
    // tb_logo_normalise). llimage.cpp getCodecFromExtension maps the lower-case extension (lldir.cpp getExtension).
    const EImageCodec codec = LLImageBase::getCodecFromExtension(gDirUtilp->getExtension(path));
    if (codec != IMG_CODEC_PNG && codec != IMG_CODEC_JPEG)
    {
        setStatus(llformat(getString("logo_type").c_str(), name.c_str()), true);
        return;
    }
    llstat st;
    if (LLFile::stat(path, &st) != 0 || st.st_size <= 0)
    {
        setStatus(llformat(getString("logo_unreadable").c_str(), name.c_str()), true);
        return;
    }
    if ((S64)st.st_size > LOGO_MAX_BYTES)
    {
        setStatus(llformat(getString("logo_too_big").c_str(), name.c_str(), (S32)(LOGO_MAX_BYTES / 1024)), true);
        return;
    }
    llifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
        setStatus(llformat(getString("logo_unreadable").c_str(), name.c_str()), true);
        return;
    }
    if (bytes.empty())
    {
        setStatus(llformat(getString("logo_unreadable").c_str(), name.c_str()), true);
        return;
    }
    mLogoBusy = true;
    mLogoName = name;
    setStatus(getString("logo_sending"), false);
    updateControls();
    request("logo", LLSD().with("image", LLBase64::encode(reinterpret_cast<const U8*>(bytes.data()), bytes.size())));
}

void WolfFloaterTrainBuilder::onRemoveLogo()
{
    mLogo.clear();
    mLogoName.clear();
    mLogoOk->set(false);
    mLogoText->setText(getString("logo_none"));
    setStatus(getString("logo_removed"), false);
    onDesignChanged();
}

void WolfFloaterTrainBuilder::onBuild()
{
    if (mBusy || !mOrder.empty()) return;
    if (!textProblem(mName->getText(), true, TRAIN_NAME_CHARS).empty() || !numberProblem().empty()
        || !textProblem(mCompany->getText(), false, COMPANY_CHARS).empty())
    {
        return;
    }
    if (const std::string lp = logoProblem(); !lp.empty())
    {
        setStatus(lp, true);
        return;
    }
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        setStatus(getString("not_confirmed"), true);
        return;
    }
    mBusy = true;
    setStatus(getString("ordering"), false);
    updateControls();
    request("build", LLSD().with("design", design()));
}

void WolfFloaterTrainBuilder::onCancelOrder()
{
    if (mOrder.empty() || mBusy) return;
    request("cancel", LLSD().with("job", mOrder));
}

void WolfFloaterTrainBuilder::showOrder(const LLSD& o)
{
    // Source: trainbuilder_lib.php tb_job_out (the plane's ORDER)
    const std::string state = o["state"].asString();
    LLStringUtil::format_map_t a;
    a["[NAME]"] = o["name"].asString();
    a["[PRICE]"] = llformat("%d", o["price"].asInteger());
    a["[AHEAD]"] = llformat("%d", o["position"].asInteger());
    a["[PROGRESS]"] = o["progress"].asString();
    a["[FOLDER]"] = o["folder_name"].asString();
    mPoll.reset();
    if (state == "queued" || state == "running")
    {
        mOrder = o["job"].asString();
        mOrderState = state;
        setStatus(getString(state == "queued" ? (o["position"].asInteger() > 0 ? "order_waiting" : "order_next") : "order_building", a), false);
    }
    else
    {
        mOrder.clear();
        mOrderState.clear();
        if (state == "done")
        {
            setStatus(getString(o["charged"].asBoolean() ? "order_done" : "order_done_unbilled", a), false);
            WolfAI::instance().creditsUsed();
            request("credits", LLSD::emptyMap());
        }
        else if (state == "cancelled")
        {
            setStatus(getString("order_cancelled"), false);
        }
        else
        {
            setStatus(o["error"].asString().empty() ? getString("order_failed") : o["error"].asString(), true);
        }
    }
    updateControls();
}

void WolfFloaterTrainBuilder::request(const std::string& what, const LLSD& body)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfTrainBuilder", [h, what, body]() { requestCoro(h, what, body); });
}

// static
void WolfFloaterTrainBuilder::requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body)
{
    // Source: wolffloaterplanebuilder.cpp requestCoro - WolfCaves::serviceRequest (POST JSON, X-Wolf-Agent / X-Wolf-Session,
    // retries 0), its transport exactly
    body["action"] = what;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(YARD_URL, &text, what == "preview" ? PREVIEW_TIMEOUT_S : TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    const std::string error = ok ? std::string() : yardError(status, reply, transport);
    if (WolfFloaterTrainBuilder* f = dynamic_cast<WolfFloaterTrainBuilder*>(h.get()))
    {
        f->onReply(what, body, ok, reply, error);
    }
}

void WolfFloaterTrainBuilder::onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error)
{
    if (what == "open")
    {
        mBusy = false;
        if (ok)
        {
            onOpenReply(reply);
        }
        else
        {
            if (reply["code"].asString() == "not_member") mRefused = error;
            else mOpenFailed = true;
            mPreview->setMessage(getString("closed"));
            setStatus(error + (mOpenFailed ? " " + getString("reopen") : std::string()), true);
        }
    }
    else if (what == "credits")
    {
        if (ok)
        {
            mBalanceNow = reply["balance"].isUndefined() ? -1 : reply["balance"].asInteger();
            mAvailable = reply["available"].isUndefined() ? -1 : reply["available"].asInteger();
        }
        else
        {
            setStatus(error, true);
        }
        updatePrice();
    }
    else if (what == "preview")
    {
        const std::string id = body["class"].asString();
        std::shared_ptr<const WolfTrainPreview::Mesh> mesh = ok ? WolfTrainPreview::parse(reply["preview"]) : nullptr;
        if (mesh) sMeshes[id] = mesh;
        if (id == mPreviewClass)
        {
            if (mesh)
            {
                mPreview->setMesh(mesh);
                onDesignChanged();
            }
            else
            {
                mPreview->setMessage(getString("no_preview"));
                setStatus(ok ? getString("no_preview") : error, true);
                mPreviewClass.clear();
            }
        }
    }
    else if (what == "logo")
    {
        mLogoBusy = false;
        if (ok && !reply["logo"].asString().empty())
        {
            mLogo = reply["logo"].asString();
            mLogoOk->set(false);
            LLStringUtil::format_map_t a;
            a["[FILE]"] = mLogoName;
            a["[W]"] = llformat("%d", reply["w"].asInteger());
            a["[H]"] = llformat("%d", reply["h"].asInteger());
            mLogoText->setText(getString("logo_ready", a));
            setStatus(getString("logo_tick"), false);
        }
        else
        {
            setStatus(ok ? getString("logo_failed") : error, true);
        }
        onDesignChanged();
    }
    else if (what == "build")
    {
        mBusy = false;
        if (ok)
        {
            showOrder(reply);
        }
        else
        {
            if (reply.has("available"))
            {
                mAvailable = reply["available"].asInteger();
                mBalanceNow = reply["balance"].asInteger();
            }
            setStatus(error, true);
            updatePrice();
        }
    }
    else if (what == "status" || what == "cancel")
    {
        if (what == "status") mPolling = false;
        mPoll.reset();
        if (ok) showOrder(reply);
        else setStatus(error, true);
    }
    updateControls();
}
