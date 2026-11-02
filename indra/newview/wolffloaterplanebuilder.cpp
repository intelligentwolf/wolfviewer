/**
 * @file wolffloaterplanebuilder.cpp
 * @brief WolfViewer: the Plane Builder. See wolffloaterplanebuilder.h and
 *        region-terrain-work/aircraft-dogfight-2026-11-01/f-plane-builder/README.md ("Viewer contract").
 *        Copied from wolffloatershipbuilder.cpp (2026-10-29), its transport exactly.
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

#include "wolffloaterplanebuilder.h"

#include <algorithm>
#include <cmath>

#include <boost/json.hpp>

#include "llbutton.h"
#include "llcolorswatch.h"
#include "llcombobox.h"
#include "llcoros.h"
#include "llfocusmgr.h"
#include "lllineeditor.h"
#include "lllocalcliprect.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "wolfai.h"
#include "wolfcave.h"
#include "wolfgrid.h"

std::map<std::string, std::shared_ptr<const WolfPlanePreview::Mesh>> WolfFloaterPlaneBuilder::sMeshes;

namespace
{
    // Source: f-plane-builder/README.md "Endpoint: https://wolfstorm.app/php/plane_builder.php".
    const char* YARD_URL = "https://wolfstorm.app/php/plane_builder.php";
    // Source: f-plane-builder/README.md "a 60 s timeout, or 120 s for preview" (as wolffloatershipbuilder.cpp).
    constexpr F32 TIMEOUT_S = 60.f, PREVIEW_TIMEOUT_S = 120.f;
    // Source: f-plane-builder/README.md "The window polls status every 5 s, and only while it is open and the order is
    // queued or running".
    constexpr F32 POLL_S = 5.f;
    constexpr F32 TURN_RAD_PER_S = F_TWO_PI / 24.f;
    constexpr F32 ELEVATION_RAD = 18.f * DEG_TO_RAD;    // a little higher than the ship's 12: a plane is seen from above
    // Source: f-plane-builder/README.md "size_pct is 90 to 110 in steps of 5": it fills the picture at the largest
    constexpr F32 SIZE_MAX_SHARE = 1.10f;
    // Source: f-plane-builder/README.md DESIGN "letters is 0 to 6 characters of A-Z, 0-9 and -"
    constexpr S32 LETTERS_MAX = 6;

    // The preview's colour groups. The member's own: upper, lower, spinner. The rest are fixed colours; a group this viewer
    // does not know is drawn in a neutral grey (so a newer yard's mesh still shows its shape, never a wrong livery colour).
    // Source: f-plane-builder/README.md preview "groups = ["upper","lower","spinner","prop","dark","metal","glass"]"; the rest are
    // names a later yard may add.
    enum Group : U8 { G_UPPER, G_LOWER, G_SPINNER, G_PROP, G_METAL, G_DARK, G_GLASS, G_WOOD, G_FABRIC, G_RED, G_WHITE, G_BLUE, G_YELLOW, G_OTHER, G_COUNT };
    const char* GROUP_NAMES[G_OTHER] = { "upper", "lower", "spinner", "prop", "metal", "dark", "glass", "wood", "fabric", "red", "white", "blue", "yellow" };
    LLColor4 rgb(S32 r, S32 g, S32 b) { return LLColor4((F32)r / 255.f, (F32)g / 255.f, (F32)b / 255.f, 1.f); }
    const LLColor4 C_METAL = rgb(150, 152, 156), C_DARK = rgb(28, 28, 30), C_GLASS = rgb(150, 182, 205), C_WOOD = rgb(132, 92, 52),
                   C_FABRIC = rgb(196, 182, 150), C_RED = rgb(170, 30, 30), C_WHITE = rgb(232, 232, 228), C_BLUE = rgb(30, 50, 120),
                   C_YELLOW = rgb(222, 180, 40), C_OTHER = rgb(128, 128, 128);
    // The starting colours before the yard's liveries arrive: RAF Dark Green over Sky (a Battle of Britain fighter's scheme;
    // the yard's own livery list replaces these as soon as it answers).
    const LLColor4 DEFAULT_UPPER = rgb(70, 76, 50), DEFAULT_LOWER = rgb(190, 196, 168), DEFAULT_SPINNER = rgb(190, 196, 168);
    const LLColor4 SKY_TOP = rgb(150, 182, 210), SKY_BOTTOM = rgb(214, 226, 236), INK = rgb(40, 48, 60);

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

    std::string metres(F64 v) { return llformat("%.1f m", v); }

    // As wolffloatershipbuilder.cpp shipyardError: the yard's own words when it gave them.
    std::string yardError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The yard refused it without saying why.";
            case 400: return "The yard could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "The grid did not accept your session - log out and back in, then try again.";
            case 404: return "The Plane Builder yard is not open on this grid yet.";
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
// WolfPlanePreview (copied from WolfShipPreview, wolffloatershipbuilder.cpp)
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfPlanePreview::WolfPlanePreview(const Params& p)
    : LLView(p), mUpper(DEFAULT_UPPER), mLower(DEFAULT_LOWER), mSpinner(DEFAULT_SPINNER)
{
    mTurn.reset();
}

// static
std::shared_ptr<const WolfPlanePreview::Mesh> WolfPlanePreview::parse(const LLSD& pv)
{
    // Source: f-plane-builder/README.md preview: v = x,y,z centimetres (ints; X nose, Y left wing, Z up); t = 4 ints per triangle
    // a,b,c,group ("the ship's has 5"); groups = names. Otherwise as WolfShipPreview::parse.
    if (!pv.isMap() || !pv["v"].isArray() || !pv["t"].isArray() || !pv["groups"].isArray()) return nullptr;
    auto m = std::make_shared<Mesh>();
    std::vector<U8> gmap;
    for (const LLSD& g : llsd::inArray(pv["groups"]))
    {
        U8 k = G_OTHER;
        for (S32 i = 0; i < G_OTHER; ++i)
        {
            if (g.asString() == GROUP_NAMES[i]) k = (U8)i;
        }
        gmap.push_back(k);
    }
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
    m->tris.reserve(nt);
    for (S32 i = 0; i < nt; ++i)
    {
        const S32 a = t[i * 4].asInteger(), b = t[i * 4 + 1].asInteger(), c = t[i * 4 + 2].asInteger();
        const S32 g = t[i * 4 + 3].asInteger();
        if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) return nullptr;
        const U8 grp = (g >= 0 && g < (S32)gmap.size()) ? gmap[g] : (U8)G_OTHER;
        m->tris.push_back(Tri{ (U32)a, (U32)b, (U32)c, grp });
    }
    m->centre = (lo + hi) * 0.5f;
    const LLVector3 ext = hi - lo;
    m->radius = llmax(0.5f * llmax(ext.mV[0], ext.mV[1]), 0.5f * ext.mV[2], 1.f);
    return m;
}

void WolfPlanePreview::setLook(const LLColor4& upper, const LLColor4& lower, const LLColor4& spinner, F32 scale)
{
    mUpper = upper;
    mLower = lower;
    mSpinner = spinner;
    mScale = scale;
}

bool WolfPlanePreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    mLastX = x;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfPlanePreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
        mTurn.reset();
    }
    return true;
}

void WolfPlanePreview::onMouseCaptureLost()
{
    mDragging = false;
    mTurn.reset();
}

bool WolfPlanePreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mYaw += (F32)(x - mLastX) * 0.01f;
        mLastX = x;
    }
    return LLView::handleHover(x, y, mask);
}

void WolfPlanePreview::draw()
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
    const F32 px_per_m = 0.47f * (F32)llmin(r.getWidth(), r.getHeight()) / (m.radius * SIZE_MAX_SHARE) * mScale;
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
        // As the ship's: a face wound clockwise on screen faces away. Thin surfaces drawn single-sided in the mesh (the
        // fabric of a biplane's wings) are kept from both sides.
        if (t.group != G_FABRIC && t.group != G_GLASS && t.group != G_PROP)
        {
            const F32 cross = (b.mV[0] - a.mV[0]) * (c.mV[1] - a.mV[1]) - (c.mV[0] - a.mV[0]) * (b.mV[1] - a.mV[1]);
            if (cross < 0.f) continue;
        }
        mOrder.emplace_back(a.mV[2] + b.mV[2] + c.mV[2], i);
    }
    std::sort(mOrder.begin(), mOrder.end(), [](const std::pair<F32, U32>& l, const std::pair<F32, U32>& rr) { return l.first > rr.first; });
    LLVector3 light(-0.4f, -0.6f, 0.7f);
    light.normalize();
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& o : mOrder)
    {
        const Tri& t = m.tris[o.second];
        LLColor4 col;
        switch (t.group)
        {
            case G_UPPER: col = mUpper; break;
            case G_LOWER: col = mLower; break;
            case G_SPINNER: col = mSpinner; break;
            case G_PROP: col = C_DARK; break;          // the blades: painted black, as the period's were
            case G_METAL: col = C_METAL; break;
            case G_DARK: col = C_DARK; break;
            case G_GLASS: col = C_GLASS; break;
            case G_WOOD: col = C_WOOD; break;
            case G_FABRIC: col = C_FABRIC; break;
            case G_RED: col = C_RED; break;
            case G_WHITE: col = C_WHITE; break;
            case G_BLUE: col = C_BLUE; break;
            case G_YELLOW: col = C_YELLOW; break;
            default: col = C_OTHER; break;
        }
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
// WolfFloaterPlaneBuilder
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterPlaneBuilder::WolfFloaterPlaneBuilder(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterPlaneBuilder::~WolfFloaterPlaneBuilder()
{
    if (mPreview && gFocusMgr.getMouseCapture() == mPreview) gFocusMgr.setMouseCapture(nullptr);
}

bool WolfFloaterPlaneBuilder::postBuild()
{
    mType = getChild<LLComboBox>("plane_type");
    mSize = getChild<LLSliderCtrl>("size");
    mLivery = getChild<LLComboBox>("livery");
    mUpper = getChild<LLColorSwatchCtrl>("upper_colour");
    mLower = getChild<LLColorSwatchCtrl>("lower_colour");
    mSpinner = getChild<LLColorSwatchCtrl>("spinner_colour");
    mLetters = getChild<LLLineEditor>("letters");
    mName = getChild<LLLineEditor>("plane_name");
    mSizeLabel = getChild<LLTextBox>("size_label");
    mFixedNote = getChild<LLTextBox>("fixed_note");
    mFigures = getChild<LLTextBox>("figures_text");
    mLettersProblem = getChild<LLTextBox>("letters_problem");
    mNameProblem = getChild<LLTextBox>("name_problem");
    mPrice = getChild<LLTextBox>("price_text");
    mBalance = getChild<LLTextBox>("balance_text");
    mStatus = getChild<LLTextBox>("status_text");
    mBuildBtn = getChild<LLButton>("build_btn");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mCancelBtn = getChild<LLButton>("cancel_btn");

    LLPanel* holder = getChild<LLPanel>("preview_panel");
    WolfPlanePreview::Params pp;
    pp.name = "plane_preview";
    pp.rect = holder->getLocalRect();
    pp.follows.flags = FOLLOWS_ALL;
    pp.mouse_opaque = true;
    mPreview = LLUICtrlFactory::create<WolfPlanePreview>(pp);
    holder->addChild(mPreview);
    mPreview->setMessage(getString("loading"));

    mUpper->set(DEFAULT_UPPER);
    mLower->set(DEFAULT_LOWER);
    mSpinner->set(DEFAULT_SPINNER);

    mType->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClassChanged(); });
    mLivery->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLiveryChanged(); });
    mSize->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDesignChanged(); });
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mUpper, mLower, mSpinner })
    {
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { onColourChanged(); });
    }
    mName->setKeystrokeCallback([this](LLLineEditor*, void*) { onDesignChanged(); }, nullptr);
    mLetters->setKeystrokeCallback([this](LLLineEditor*, void*) { onDesignChanged(); }, nullptr);
    mBuildBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mBuyBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfAI::instance().openBuyPage(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancelOrder(); });
    setStatus(std::string(), false);
    updateControls();
    return true;
}

void WolfFloaterPlaneBuilder::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] as the Ship Builder: a Wolf-only window opened off Wolf says so and closes.
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

void WolfFloaterPlaneBuilder::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterPlaneBuilder::draw()
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

const LLSD* WolfFloaterPlaneBuilder::currentClass() const
{
    const std::string id = mType->getValue().asString();
    const LLSD& classes = mCatalogue["classes"];
    for (LLSD::array_const_iterator it = classes.beginArray(); it != classes.endArray(); ++it)
    {
        if ((*it)["id"].asString() == id) return &(*it);
    }
    return nullptr;
}

const LLSD* WolfFloaterPlaneBuilder::currentLivery() const
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

bool WolfFloaterPlaneBuilder::currentFixed() const
{
    // Source: f-plane-builder/README.md CLASS "fixed_livery (bool; absent = false)". LLSD::asBoolean() of an absent key is false.
    const LLSD* c = currentClass();
    return c && (*c)["fixed_livery"].asBoolean();
}

std::string WolfFloaterPlaneBuilder::nameProblem() const
{
    // Source: f-plane-builder/README.md "name is 1 to 24 characters of Latin letters, digits, space and ' - . &. It must
    // contain at least one letter or digit" - the same rule as the Ship Builder's (shipbuilder_lib.php sb_check_design)
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    if (name.empty()) return getString("name_needed");
    const LLWString w = utf8str_to_wstring(name);
    const S32 max = 24;
    if ((S32)w.size() > max) return llformat(getString("name_long").c_str(), max);
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

std::string WolfFloaterPlaneBuilder::lettersProblem() const
{
    // Source: f-plane-builder/README.md "letters is 0 to 6 characters of A-Z, 0-9 and -" (lower case is taken as capitals)
    if (currentFixed()) return std::string();   // the field is empty and greyed, and "" is sent
    std::string l = mLetters->getText();
    LLStringUtil::trim(l);
    if ((S32)l.size() > LETTERS_MAX) return llformat(getString("letters_long").c_str(), LETTERS_MAX);
    for (char ch : l)
    {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) return getString("letters_chars");
    }
    return std::string();
}

LLSD WolfFloaterPlaneBuilder::design() const
{
    // Source: f-plane-builder/README.md DESIGN
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    std::string letters = mLetters->getText();
    LLStringUtil::trim(letters);
    LLStringUtil::toUpper(letters);
    LLSD d;
    d["class"] = mType->getValue().asString();
    d["size_pct"] = (S32)std::lround(mSize->getValueF32());
    const std::string livery = mLivery->getValue().asString();
    d["livery"] = livery.empty() ? std::string("custom") : livery;
    d["upper"] = colourOf(mUpper->get());
    d["lower"] = colourOf(mLower->get());
    d["spinner"] = colourOf(mSpinner->get());
    d["letters"] = letters;
    // Source: f-plane-builder/README.md "For the viewer job": a fixed_livery class is sent with "size_pct": 100,
    // "livery": "<that entry's id>", "letters": "" (planebuilder_lib.php pb_check_design refuses anything else).
    if (const LLSD* c = currentClass(); c && (*c)["fixed_livery"].asBoolean())
    {
        d["size_pct"] = 100;
        d["livery"] = (*c)["liveries"].size() > 0 ? (*c)["liveries"][0]["id"].asString() : std::string();
        d["letters"] = std::string();
    }
    d["name"] = name;
    return d;
}

void WolfFloaterPlaneBuilder::onOpenReply(const LLSD& reply)
{
    mCatalogue = reply["catalogue"];
    mPriceRules = reply["price_rules"];
    mBalanceNow = reply["balance"].isUndefined() ? -1 : reply["balance"].asInteger();
    mAvailable = reply["available"].isUndefined() ? -1 : reply["available"].asInteger();
    mType->removeall();
    for (const LLSD& c : llsd::inArray(mCatalogue["classes"]))
    {
        // an unarmed type (the airliners: gun_text "") is listed by its title alone
        const std::string guns = c["gun_text"].asString();
        mType->add(guns.empty() ? c["title"].asString() : llformat("%s - %s", c["title"].asString().c_str(), guns.c_str()), c["id"]);
    }
    if (mCatalogue["classes"].size() > 0) mType->selectFirstItem();
    mReady = true;
    setStatus(std::string(), false);
    onClassChanged();
    if (reply["active"].isMap()) showOrder(reply["active"]);
}

void WolfFloaterPlaneBuilder::onClassChanged()
{
    const LLSD* c = currentClass();
    if (!c) return;
    // its period liveries, then the member's own colours; a fixed-livery class has its one livery only
    const bool fixed = (*c)["fixed_livery"].asBoolean();
    mLivery->removeall();
    for (const LLSD& lv : llsd::inArray((*c)["liveries"]))
    {
        mLivery->add(lv["title"].asString(), lv["id"]);
        if (fixed) break;
    }
    if (!fixed) mLivery->add(getString("custom_livery"), LLSD("custom"));
    mLivery->selectFirstItem();
    mLetters->setLabel((*c)["letters_hint"].asString());
    applyFixed(*c);
    onLiveryChanged();

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

void WolfFloaterPlaneBuilder::applyFixed(const LLSD& cls)
{
    // Source: f-plane-builder/README.md "For the viewer job": fixed_livery = size fixed at 100 %, its one livery, no custom
    // colours, the squadron letters empty and disabled; fixed_note shown beside the greyed options. The member's own size and
    // letters are put back when they choose a type that is not fixed. The greying itself is in updateControls.
    const bool fixed = cls["fixed_livery"].asBoolean();
    if (fixed && !mFixedShown)
    {
        mKeptSize = mSize->getValueF32();
        mKeptLetters = mLetters->getText();
    }
    if (fixed)
    {
        mSize->setValue(LLSD(100.0));
        mLetters->setText(LLStringUtil::null);
    }
    else if (mFixedShown)
    {
        mSize->setValue(LLSD((F64)mKeptSize));
        mLetters->setText(mKeptLetters);
    }
    mFixedShown = fixed;
    const std::string note = cls["fixed_note"].asString();
    mFixedNote->setText(fixed ? (note.empty() ? getString("fixed_note_default") : note) : std::string());
    mFixedNote->setVisible(fixed);
    mSizeLabel->setText(getString(fixed ? "size_label_fixed" : "size_label"));
}

void WolfFloaterPlaneBuilder::onLiveryChanged()
{
    if (const LLSD* lv = currentLivery())
    {
        mSettingLivery = true;
        mUpper->set(colourFrom((*lv)["upper"], DEFAULT_UPPER));
        mLower->set(colourFrom((*lv)["lower"], DEFAULT_LOWER));
        mSpinner->set(colourFrom((*lv)["spinner"], DEFAULT_SPINNER));
        mSettingLivery = false;
    }
    onDesignChanged();
}

void WolfFloaterPlaneBuilder::onColourChanged()
{
    // a colour changed by hand turns the livery into the member's own ("Your own colours"), unless it still matches it
    if (!mSettingLivery)
    {
        const LLSD* lv = currentLivery();
        if (lv && !(sameColour(mUpper->get(), colourFrom((*lv)["upper"], DEFAULT_UPPER))
                    && sameColour(mLower->get(), colourFrom((*lv)["lower"], DEFAULT_LOWER))
                    && sameColour(mSpinner->get(), colourFrom((*lv)["spinner"], DEFAULT_SPINNER))))
        {
            mLivery->setValue(LLSD("custom"));
        }
    }
    onDesignChanged();
}

void WolfFloaterPlaneBuilder::onDesignChanged()
{
    const LLSD* c = currentClass();
    if (c)
    {
        const LLSD d = design();
        const F32 k = (F32)d["size_pct"].asInteger() / 100.f;
        mPreview->setLook(mUpper->get(), mLower->get(), mSpinner->get(), k);
        LLStringUtil::format_map_t a;
        a["[TITLE]"] = (*c)["title"].asString();
        a["[SPAN]"] = metres((*c)["span_m"].asReal() * k);
        a["[LENGTH]"] = metres((*c)["length_m"].asReal() * k);
        a["[GUNS]"] = (*c)["gun_text"].asString().empty() ? getString("no_guns") : (*c)["gun_text"].asString();
        const std::string era = (*c)["era"].asString();
        a["[ERA]"] = (*c)["era_text"].asString().empty()
            ? getString(era == "ww1" ? "era_ww1" : era == "ww2early" ? "era_ww2early" : era == "ww2late" ? "era_ww2late" : "era_other")
            : (*c)["era_text"].asString();   // the yard's own words (f-plane-builder/README.md CLASS era_text)
        // [PLANE DESC 2026-11-02] Source: f-plane-builder/README.md CLASS "desc" (optional; the yard's own sentence about it).
        // Without one, the fighter's sentence - unless the class is "fixed_livery" or has "guns" 0 (Concorde, the 747-400),
        // which carry passengers, are unarmed and never join a dogfight. LLSD of an absent key: asString() "", asBoolean() false.
        std::string desc = (*c)["desc"].asString();
        LLStringUtil::trim(desc);
        const bool unarmed = (*c)["fixed_livery"].asBoolean() || (*c)["guns"].asInteger() == 0;
        a["[ABOUT]"] = desc.empty() ? getString(unarmed ? "about_unarmed" : "about_fighter") : desc;
        mFigures->setText(getString("figures", a));
        std::string tip;
        if (!desc.empty() && mFigures->getTextPixelHeight() > mFigures->getRect().getHeight())
        {
            // Too long for the box (figures_text clips a partial line: lltextbase.cpp mClipPartial): cut it at a word with
            // "desc_cut" until it fits, and give the whole of it as the box's tool tip. The box holds about 400 characters, so
            // the cutting starts at the last word break within DESC_CUT_START bytes (a space is ASCII: never inside a UTF-8 letter).
            constexpr size_t DESC_CUT_START = 600;
            tip = desc;
            std::string cut = desc.substr(0, desc.find_last_of(" \t\r\n", DESC_CUT_START));
            LLStringUtil::format_map_t t;
            do
            {
                LLStringUtil::trimTail(cut);
                t["[TEXT]"] = cut;
                a["[ABOUT]"] = getString("desc_cut", t);
                mFigures->setText(getString("figures", a));
                if (mFigures->getTextPixelHeight() <= mFigures->getRect().getHeight()) break;
                const size_t brk = cut.find_last_of(" \t\r\n");
                cut.erase(brk == std::string::npos ? 0 : brk);
            } while (!cut.empty());
            if (cut.empty())    // not even its first word fits beside the figures
            {
                t["[TEXT]"] = std::string();
                a["[ABOUT]"] = getString("desc_cut", t);
                mFigures->setText(getString("figures", a));
            }
        }
        mFigures->setToolTip(tip);
    }
    mNameProblem->setText(mReady ? nameProblem() : std::string());
    mLettersProblem->setText(mReady ? lettersProblem() : std::string());
    updatePrice();
    updateControls();
}

void WolfFloaterPlaneBuilder::updatePrice()
{
    const LLSD* c = currentClass();
    if (!c || !mPriceRules.isMap())
    {
        mPrice->setText(std::string());
        mPriceNow = 0;
        return;
    }
    // Source: f-plane-builder/README.md "price = base + round(per_span_metre × span_m × size_pct / 100) + per_gun × guns"
    // (the yard's figure is the one charged)
    const LLSD d = design();
    const S32 base = mPriceRules["base"].asInteger();
    const S32 airframe = (S32)std::lround(mPriceRules["per_span_metre"].asReal() * (*c)["span_m"].asReal() * d["size_pct"].asInteger() / 100.0);
    const S32 guns = (*c)["guns"].asInteger();
    const S32 armament = mPriceRules["per_gun"].asInteger() * guns;
    const S32 total = base + airframe + armament;
    mPrice->setText(llformat("%s %d + %s %d + %d %s %d = %d %s", getString("price_base").c_str(), base,
                             getString("price_airframe").c_str(), airframe, guns, getString(guns == 1 ? "price_gun" : "price_guns").c_str(),
                             armament, total, getString("credits_word").c_str()));
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

void WolfFloaterPlaneBuilder::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool open = wolf && mReady && mRefused.empty();
    const bool ordering = !mOrder.empty();
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mType, mName })
    {
        c->setEnabled(open && !ordering);
    }
    // a fixed-livery class (the airliners): size, livery, colours and letters stay greyed (applyFixed set them)
    const bool fixed = currentFixed();
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mSize, mLivery, mUpper, mLower, mSpinner, mLetters })
    {
        c->setEnabled(open && !ordering && !fixed);
    }
    const bool shortOf = open && mAvailable >= 0 && mAvailable < mPriceNow;
    mBuildBtn->setEnabled(open && !mBusy && !ordering && !shortOf && mAvailable >= 0 && nameProblem().empty() && lettersProblem().empty());
    mBuyBtn->setVisible(shortOf);
    mCancelBtn->setVisible(ordering && mOrderState == "queued");
}

void WolfFloaterPlaneBuilder::onBuild()
{
    if (mBusy || !mOrder.empty() || !nameProblem().empty() || !lettersProblem().empty()) return;
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

void WolfFloaterPlaneBuilder::onCancelOrder()
{
    if (mOrder.empty() || mBusy) return;
    request("cancel", LLSD().with("job", mOrder));
}

void WolfFloaterPlaneBuilder::showOrder(const LLSD& o)
{
    // Source: f-plane-builder/README.md ORDER
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

void WolfFloaterPlaneBuilder::request(const std::string& what, const LLSD& body)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfPlaneBuilder", [h, what, body]() { requestCoro(h, what, body); });
}

// static
void WolfFloaterPlaneBuilder::requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body)
{
    // Source: wolffloatershipbuilder.cpp requestCoro - WolfCaves::serviceRequest (POST JSON, X-Wolf-Agent / X-Wolf-Session,
    // retries 0), f-plane-builder/README.md "Copy wolffloatershipbuilder.cpp's transport exactly"
    body["action"] = what;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(YARD_URL, &text, what == "preview" ? PREVIEW_TIMEOUT_S : TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    const std::string error = ok ? std::string() : yardError(status, reply, transport);
    if (WolfFloaterPlaneBuilder* f = dynamic_cast<WolfFloaterPlaneBuilder*>(h.get()))
    {
        f->onReply(what, body, ok, reply, error);
    }
}

void WolfFloaterPlaneBuilder::onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error)
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
        std::shared_ptr<const WolfPlanePreview::Mesh> mesh = ok ? WolfPlanePreview::parse(reply["preview"]) : nullptr;
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
