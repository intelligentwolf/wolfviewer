/**
 * @file wolffloatershipbuilder.cpp
 * @brief WolfViewer: Ship Builder - design a sailing ship and have the shipyard build her. See wolffloatershipbuilder.h.
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

#include "wolffloatershipbuilder.h"

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
#include "llradiogroup.h"
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

std::map<std::string, std::shared_ptr<const WolfShipPreview::Mesh>> WolfFloaterShipBuilder::sMeshes;

namespace
{
    // The shipyard: wolfstorm php/ship_builder.php (on wolfstorm.app, where the other Wolf services are:
    // wolfadventure.cpp API_URL, wolfcave.cpp API_URL).
    const char* SHIPYARD_URL = "https://wolfstorm.app/php/ship_builder.php";
    // Seconds a request may take. A preview is up to about 1.2 MB (the 74-gun ship, make_preview_meshes.py output).
    constexpr F32 TIMEOUT_S = 60.f, PREVIEW_TIMEOUT_S = 120.f;
    // How often an order still waiting or being built is asked about while the window is open (a build takes about
    // 20 s to 2 minutes; nothing is asked while the window is closed - the shipyard offers the folder in-world when
    // she is delivered, inventory_notify_lib.php wid_delivered).
    constexpr F32 POLL_S = 5.f;
    // The turntable: one turn in 24 s, seen from 12 degrees above.
    constexpr F32 TURN_RAD_PER_S = F_TWO_PI / 24.f;
    constexpr F32 ELEVATION_RAD = 12.f * DEG_TO_RAD;
    constexpr F32 LENGTH_MAX_SHARE = 1.25f;   // shipbuilder_class.py LENGTH_MAX 125 %: she fills the picture at the most

    // Colour groups (make_preview_meshes.py GROUPS) and the fixed colours: the generator's own texture colours
    // (pirate-ship/generator/textures.py: tex_deck (150,116,78), tex_copper (176,98,52), tex_iron (30,30,32), tex_gilt
    // (206,160,62), tex_jolly (12,12,14), tex_inner_red (128,46,34), tex_wood_dark (92,60,36) for a timber figurehead).
    enum Group : U8 { G_HULL, G_TRIM, G_SAIL, G_WOOD, G_COPPER, G_IRON, G_GILT, G_FLAG, G_RED, G_FIGUREHEAD, G_COUNT };
    const char* GROUP_NAMES[G_COUNT] = { "hull", "trim", "sail", "wood", "copper", "iron", "gilt", "flag", "red", "figurehead" };
    LLColor4 rgb(S32 r, S32 g, S32 b) { return LLColor4((F32)r / 255.f, (F32)g / 255.f, (F32)b / 255.f, 1.f); }
    const LLColor4 C_WOOD = rgb(150, 116, 78), C_COPPER = rgb(176, 98, 52), C_IRON = rgb(30, 30, 32),
                   C_GILT = rgb(206, 160, 62), C_FLAG = rgb(12, 12, 14), C_RED = rgb(128, 46, 34),
                   C_TIMBER = rgb(92, 60, 36);
    // HMS Wolfy's own livery, the starting colours (textures.py LIVERY hull, strake, sail)
    const LLColor4 DEFAULT_HULL = rgb(34, 32, 31), DEFAULT_TRIM = rgb(178, 128, 52), DEFAULT_SAIL = rgb(226, 214, 186);
    // the turntable's sky, and the words drawn on it
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

    std::string metres(F64 v) { return llformat("%.1f m", v); }

    /** The words for a refused or failed request (the shipyard's own words when it gave them). The viewer's raw adapter
     *  drops the body of a 4xx (wolfcave.cpp replyOf), so ship_builder.php answers what a person should read as HTTP
     *  200 + success:false; the statuses left are these (ship_builder.php header, grid_rights.php wt_authenticate). */
    std::string shipyardError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && reply.has("error") && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The shipyard refused it without saying why.";
            case 400: return "The shipyard could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "The grid did not accept your session - log out and back in, then try again.";
            case 404: return "The shipyard is not open on this grid yet.";
            case 413: return "The request was too large for the shipyard.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 500: return "The shipyard hit an error - try again in a moment.";
            case 503: return "The shipyard is not answering - try again in a moment.";
            default:
                return transport.empty() ? llformat("The shipyard answered HTTP %d.", status)
                                         : "Could not reach the shipyard: " + transport;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// WolfShipPreview
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfShipPreview::WolfShipPreview(const Params& p)
    : LLView(p), mHull(DEFAULT_HULL), mTrim(DEFAULT_TRIM), mSail(DEFAULT_SAIL)
{
    mTurn.reset();
}

// static
std::shared_ptr<const WolfShipPreview::Mesh> WolfShipPreview::parse(const LLSD& pv)
{
    if (!pv.isMap() || !pv["v"].isArray() || !pv["t"].isArray() || !pv["groups"].isArray()) return nullptr;
    auto m = std::make_shared<Mesh>();
    // the file's group names -> ours (an unknown one is left out, so a newer file cannot paint the wrong colour)
    std::vector<S32> gmap;
    for (const LLSD& g : llsd::inArray(pv["groups"]))
    {
        S32 k = -1;
        for (S32 i = 0; i < G_COUNT; ++i)
        {
            if (g.asString() == GROUP_NAMES[i]) k = i;
        }
        gmap.push_back(k);
    }
    const LLSD& v = pv["v"];
    const S32 nv = v.size() / 3;
    if (nv <= 0 || v.size() % 3) return nullptr;
    m->verts.reserve(nv);
    LLVector3 lo(F32_MAX, F32_MAX, F32_MAX), hi(-F32_MAX, -F32_MAX, -F32_MAX);
    for (S32 i = 0; i < nv; ++i)
    {
        // centimetres (make_preview_meshes.py writes round(x * 100))
        LLVector3 p((F32)v[i * 3].asInteger() / 100.f, (F32)v[i * 3 + 1].asInteger() / 100.f, (F32)v[i * 3 + 2].asInteger() / 100.f);
        lo.set(llmin(lo.mV[0], p.mV[0]), llmin(lo.mV[1], p.mV[1]), llmin(lo.mV[2], p.mV[2]));
        hi.set(llmax(hi.mV[0], p.mV[0]), llmax(hi.mV[1], p.mV[1]), llmax(hi.mV[2], p.mV[2]));
        m->verts.push_back(p);
    }
    const LLSD& t = pv["t"];
    if (t.size() % 5) return nullptr;
    const S32 nt = t.size() / 5;
    m->tris.reserve(nt);
    for (S32 i = 0; i < nt; ++i)
    {
        const S32 a = t[i * 5].asInteger(), b = t[i * 5 + 1].asInteger(), c = t[i * 5 + 2].asInteger();
        const S32 g = t[i * 5 + 3].asInteger(), gun = t[i * 5 + 4].asInteger();
        if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) return nullptr;
        if (g < 0 || g >= (S32)gmap.size() || gmap[g] < 0) continue;
        m->tris.push_back(Tri{ (U32)a, (U32)b, (U32)c, (U8)gmap[g], (S16)llclamp(gun, -1, 32767) });
    }
    for (const LLSD& n : llsd::inArray(pv["guns"])) m->gunNames.push_back(n.asString());
    m->centre = (lo + hi) * 0.5f;
    const LLVector3 ext = hi - lo;
    // the picture is turned about Z: what must fit is her length or beam across, and her height up
    m->radius = llmax(0.5f * llmax(ext.mV[0], ext.mV[1]), 0.5f * ext.mV[2], 1.f);
    return m;
}

void WolfShipPreview::setMesh(std::shared_ptr<const Mesh> mesh)
{
    mMesh = std::move(mesh);
    mGunShown.assign(mMesh ? mMesh->gunNames.size() : 0, true);
}

void WolfShipPreview::setLook(const LLColor4& hull, const LLColor4& trim, const LLColor4& sail, const std::set<std::string>& guns,
                              const std::string& figurehead, F32 scale)
{
    mHull = hull;
    mTrim = trim;
    mSail = sail;
    mFigurehead = figurehead;
    mScale = scale;
    if (mMesh)
    {
        for (size_t i = 0; i < mMesh->gunNames.size(); ++i) mGunShown[i] = guns.count(mMesh->gunNames[i]) > 0;
    }
}

bool WolfShipPreview::handleMouseDown(S32 x, S32 y, MASK mask)
{
    mDragging = true;
    mLastX = x;
    gFocusMgr.setMouseCapture(this);
    return true;
}

bool WolfShipPreview::handleMouseUp(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mDragging = false;
        gFocusMgr.setMouseCapture(nullptr);
        mTurn.reset();
    }
    return true;
}

void WolfShipPreview::onMouseCaptureLost()
{
    mDragging = false;
    mTurn.reset();
}

bool WolfShipPreview::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragging)
    {
        mYaw += (F32)(x - mLastX) * 0.01f;
        mLastX = x;
    }
    return LLView::handleHover(x, y, mask);
}

void WolfShipPreview::draw()
{
    const LLRect r = getLocalRect();
    LLLocalClipRect clip(r);
    // the sky: a plain gradient, lighter at the horizon
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
            LLFontGL::getFontSansSerif()->renderUTF8(mMessage, 0, (F32)r.getCenterX(), (F32)r.getCenterY(), INK,
                                                    LLFontGL::HCENTER, LLFontGL::VCENTER);
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
    // She fills the picture at the longest length (LENGTH_MAX_SHARE) and is drawn smaller at less.
    const F32 px_per_m = 0.47f * (F32)llmin(r.getWidth(), r.getHeight()) / (m.radius * LENGTH_MAX_SHARE) * mScale;
    const F32 cx = (F32)r.getCenterX(), cy = (F32)r.getCenterY();
    const F32 ca = cosf(mYaw), sa = sinf(mYaw), ce = cosf(ELEVATION_RAD), se = sinf(ELEVATION_RAD);
    mProj.resize(m.verts.size());
    for (size_t i = 0; i < m.verts.size(); ++i)
    {
        const LLVector3 p = m.verts[i] - m.centre;
        // turn about Z, then tilt towards the viewer, who looks along +Y': X' across the screen, Z' up, Y' depth
        const F32 x1 = p.mV[0] * ca - p.mV[1] * sa;
        const F32 y1 = p.mV[0] * sa + p.mV[1] * ca;
        const F32 y2 = y1 * ce - p.mV[2] * se;
        const F32 z2 = y1 * se + p.mV[2] * ce;
        mProj[i].set(cx + x1 * px_per_m, cy + z2 * px_per_m, y2);
    }
    // far first (painter's algorithm); solid groups drop the triangles that face away
    mOrder.clear();
    mOrder.reserve(m.tris.size());
    for (U32 i = 0; i < (U32)m.tris.size(); ++i)
    {
        const Tri& t = m.tris[i];
        if (t.gun >= 0 && (size_t)t.gun < mGunShown.size() && !mGunShown[t.gun]) continue;
        if (t.group == G_FIGUREHEAD && mFigurehead == "none") continue;
        const LLVector3 &a = mProj[t.a], &b = mProj[t.b], &c = mProj[t.c];
        if (t.group != G_SAIL && t.group != G_FLAG)
        {
            // screen y is up here: a triangle wound clockwise on screen faces away (the generator's faces are wound
            // counter-clockwise seen from outside, glTF's rule)
            const F32 cross = (b.mV[0] - a.mV[0]) * (c.mV[1] - a.mV[1]) - (c.mV[0] - a.mV[0]) * (b.mV[1] - a.mV[1]);
            if (cross < 0.f) continue;
        }
        mOrder.emplace_back(a.mV[2] + b.mV[2] + c.mV[2], i);
    }
    std::sort(mOrder.begin(), mOrder.end(), [](const std::pair<F32, U32>& l, const std::pair<F32, U32>& rr) { return l.first > rr.first; });
    // light from the upper left, in front: flat shading 0.55 .. 1.0 of the colour by the face's angle to it
    LLVector3 light(-0.4f, -0.6f, 0.7f);
    light.normalize();
    gGL.begin(LLRender::TRIANGLES);
    for (const auto& o : mOrder)
    {
        const Tri& t = m.tris[o.second];
        LLColor4 col;
        switch (t.group)
        {
            case G_HULL: col = mHull; break;
            case G_TRIM: col = mTrim; break;
            case G_SAIL: col = mSail; break;
            case G_WOOD: col = C_WOOD; break;
            case G_COPPER: col = C_COPPER; break;
            case G_IRON: col = C_IRON; break;
            case G_GILT: col = C_GILT; break;
            case G_FLAG: col = C_FLAG; break;
            case G_RED: col = C_RED; break;
            case G_FIGUREHEAD: col = mFigurehead == "timber" ? C_TIMBER : C_GILT; break;
            default: continue;
        }
        const LLVector3 &a = mProj[t.a], &b = mProj[t.b], &c = mProj[t.c];
        // the face normal in the view (screen x, screen y up, depth away from the viewer)
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
// WolfFloaterShipBuilder
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════

WolfFloaterShipBuilder::WolfFloaterShipBuilder(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterShipBuilder::~WolfFloaterShipBuilder()
{
    if (mPreview && gFocusMgr.getMouseCapture() == mPreview) gFocusMgr.setMouseCapture(nullptr);
}

bool WolfFloaterShipBuilder::postBuild()
{
    mHullType = getChild<LLComboBox>("hull_type");
    mLength = getChild<LLSliderCtrl>("length");
    mGuns = getChild<LLRadioGroup>("guns");
    mFigurehead = getChild<LLComboBox>("figurehead");
    mHullColour = getChild<LLColorSwatchCtrl>("hull_colour");
    mTrimColour = getChild<LLColorSwatchCtrl>("trim_colour");
    mSailColour = getChild<LLColorSwatchCtrl>("sail_colour");
    mName = getChild<LLLineEditor>("ship_name");
    mFigures = getChild<LLTextBox>("figures_text");
    mNameProblem = getChild<LLTextBox>("name_problem");
    mPrice = getChild<LLTextBox>("price_text");
    mBalance = getChild<LLTextBox>("balance_text");
    mStatus = getChild<LLTextBox>("status_text");
    mBuildBtn = getChild<LLButton>("build_btn");
    mBuyBtn = getChild<LLButton>("buy_btn");
    mCancelBtn = getChild<LLButton>("cancel_btn");

    LLPanel* holder = getChild<LLPanel>("preview_panel");
    WolfShipPreview::Params pp;
    pp.name = "ship_preview";
    pp.rect = holder->getLocalRect();
    pp.follows.flags = FOLLOWS_ALL;
    pp.mouse_opaque = true;
    mPreview = LLUICtrlFactory::create<WolfShipPreview>(pp);
    holder->addChild(mPreview);
    mPreview->setMessage(getString("loading"));

    mGuns->setValue("full");
    mHullColour->set(DEFAULT_HULL);
    mTrimColour->set(DEFAULT_TRIM);
    mSailColour->set(DEFAULT_SAIL);

    mHullType->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClassChanged(); });
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mLength, mGuns, mFigurehead, mHullColour, mTrimColour, mSailColour })
    {
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDesignChanged(); });
    }
    mName->setKeystrokeCallback([this](LLLineEditor*, void*) { onDesignChanged(); }, nullptr);
    mBuildBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    mBuyBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfAI::instance().openBuyPage(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCancelOrder(); });
    setStatus(std::string(), false);
    updateControls();
    return true;
}

void WolfFloaterShipBuilder::onOpen(const LLSD& key)
{
    // [WOLF GRID GATE] (wolffloaterseatunnels.cpp): a Wolf-only window opened off Wolf says so and closes.
    if (!WolfGrid::isOnWolfTerritories())
    {
        WolfGrid::noteWolfOnly(getString("feature_name"));
        closeFloater();
        return;
    }
    mOpenFailed = false;     // a failed first ask is tried again (draw) - never in a loop, only on opening
    if (mReady)
    {
        // the balance is read again every time the window opens (Paul: never cache it), and an order is looked at
        request("credits", LLSD::emptyMap());
        if (!mOrder.empty()) request("status", LLSD().with("job", mOrder));
    }
    // otherwise draw() asks the shipyard as soon as this region is confirmed as Wolf's (isOnWolfTerritoriesConfirmed)
    updateControls();
}

void WolfFloaterShipBuilder::setStatus(const std::string& text, bool error)
{
    if (!mStatus) return;
    mStatus->setText(text);
    mStatus->setColor(LLUIColorTable::instance().getColor(error ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterShipBuilder::draw()
{
    // The first ask waits until this region's features say it is Wolf's own (wolfgrid.h: use the confirmed check before
    // sending anything with the session).
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

const LLSD* WolfFloaterShipBuilder::currentClass() const
{
    // an element of mCatalogue itself (never of a copy), so the pointer lives as long as the catalogue
    const std::string id = mHullType->getValue().asString();
    const LLSD& classes = mCatalogue["classes"];
    for (LLSD::array_const_iterator it = classes.beginArray(); it != classes.endArray(); ++it)
    {
        if ((*it)["id"].asString() == id) return &(*it);
    }
    return nullptr;
}

std::string WolfFloaterShipBuilder::nameProblem() const
{
    // the shipyard's own rules (shipbuilder_lib.php sb_check_design, shipbuilder_class.py clean_name): 1 to name_max
    // letters of the Latin alphabets, digits, space and ' - . & ; it checks them again
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    if (name.empty()) return getString("name_needed");
    const LLWString w = utf8str_to_wstring(name);
    const S32 max = mCatalogue.has("name_max") ? mCatalogue["name_max"].asInteger() : 24;
    if ((S32)w.size() > max) return llformat(getString("name_long").c_str(), max);
    bool any = false;
    for (llwchar ch : w)
    {
        // Basic Latin, Latin-1 letters (not x and divide), Latin Extended-A/B and Additional (U+1E00-1EFF)
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

LLSD WolfFloaterShipBuilder::design() const
{
    std::string name = mName->getText();
    LLStringUtil::trim(name);
    LLSD d;
    d["class"] = mHullType->getValue().asString();
    d["length_pct"] = (S32)std::lround(mLength->getValueF32());
    d["guns"] = mGuns->getValue().asString();
    d["figurehead"] = mFigurehead->getValue().asString();
    d["hull"] = colourOf(mHullColour->get());
    d["trim"] = colourOf(mTrimColour->get());
    d["sail"] = colourOf(mSailColour->get());
    d["name"] = name;
    return d;
}

void WolfFloaterShipBuilder::onOpenReply(const LLSD& reply)
{
    mCatalogue = reply["catalogue"];
    mPriceRules = reply["price_rules"];
    mBalanceNow = reply["balance"].isUndefined() ? -1 : reply["balance"].asInteger();
    mAvailable = reply["available"].isUndefined() ? -1 : reply["available"].asInteger();
    mHullType->removeall();
    for (const LLSD& c : llsd::inArray(mCatalogue["classes"]))
    {
        const S32 guns = c["guns"]["full"].size();
        mHullType->add(llformat("%s - %d guns", c["label"].asString().c_str(), guns), c["id"]);
    }
    if (mCatalogue["classes"].size() > 0) mHullType->selectFirstItem();
    const LLSD& lp = mCatalogue["length_pct"];
    if (lp.size() == 3)
    {
        mLength->setMinValue((F32)lp[0].asInteger());
        mLength->setMaxValue((F32)lp[1].asInteger());
        mLength->setIncrement((F32)lp[2].asInteger());
    }
    mName->setMaxTextChars(mCatalogue.has("name_max") ? mCatalogue["name_max"].asInteger() : 24);
    mReady = true;
    setStatus(std::string(), false);
    onClassChanged();
    if (reply["active"].isMap()) showOrder(reply["active"]);
}

void WolfFloaterShipBuilder::onClassChanged()
{
    const LLSD* c = currentClass();
    if (!c) return;
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

void WolfFloaterShipBuilder::onDesignChanged()
{
    const LLSD* c = currentClass();
    if (c)
    {
        const LLSD d = design();
        std::set<std::string> guns;
        for (const LLSD& g : llsd::inArray((*c)["guns"][d["guns"].asString()])) guns.insert(g.asString());
        const F32 k = (F32)d["length_pct"].asInteger() / 100.f;
        mPreview->setLook(mHullColour->get(), mTrimColour->get(), mSailColour->get(), guns, d["figurehead"].asString(), k);
        // her figures at the chosen length (catalogue: the class's own at 100 %), and what she carries
        const S32 full = (*c)["guns"]["full"].size(), kept = (S32)guns.size();
        const S32 prims = (*c)["prims"].asInteger() - (full - kept) - (d["figurehead"].asString() == "none" ? 1 : 0);
        std::string decks;
        for (const LLSD& dk : llsd::inArray((*c)["decks"]))
        {
            if (!decks.empty()) decks += ", ";
            decks += dk.asString();
        }
        LLStringUtil::format_map_t a;
        a["[TITLE]"] = (*c)["title"].asString();
        a["[DECK]"] = metres((*c)["length_on_deck_m"].asReal() * k);
        a["[OVERALL]"] = metres((*c)["length_overall_m"].asReal() * k);
        a["[BEAM]"] = metres((*c)["beam_m"].asReal() * k);
        a["[DRAUGHT]"] = metres((*c)["draught_m"].asReal() * k);
        a["[HEIGHT]"] = metres((*c)["height_m"].asReal() * k);
        a["[DECKS]"] = decks;
        a["[MASTS]"] = llformat("%d", (*c)["masts"].asInteger());
        a["[SAILS]"] = llformat("%d", (*c)["sails"].asInteger());
        a["[GUNS]"] = kept == full ? llformat("%d", kept) : llformat("%d of %d", kept, full);
        a["[PRIMS]"] = llformat("%d", prims);
        mFigures->setText(getString("figures", a));
    }
    const std::string problem = mReady ? nameProblem() : std::string();
    mNameProblem->setText(problem);
    updatePrice();
    updateControls();
}

void WolfFloaterShipBuilder::updatePrice()
{
    const LLSD* c = currentClass();
    if (!c || !mPriceRules.isMap())
    {
        mPrice->setText(std::string());
        mPriceNow = 0;
        return;
    }
    // shipbuilder_lib.php sb_price, the same sums (the shipyard's figure is the one charged): HULL = base + per_metre x her
    // length on deck; then each calibre, heaviest first: count x per_damage_point x that gun's damage, rounded
    const LLSD d = design();
    const F64 length = (*c)["length_on_deck_m"].asReal() * d["length_pct"].asInteger() / 100.0;
    const S32 hull = (S32)std::lround(mPriceRules["base"].asReal() + mPriceRules["per_metre"].asReal() * length);
    std::string text = llformat("%s %d", getString("price_hull").c_str(), hull);
    S32 total = hull;
    std::vector<std::pair<S32, S32>> by;     // pounder, count
    const LLSD& gp = (*c)["gun_pounders"][d["guns"].asString()];
    for (LLSD::map_const_iterator it = gp.beginMap(); it != gp.endMap(); ++it)
    {
        by.emplace_back(atoi(it->first.c_str()), it->second.asInteger());
    }
    std::sort(by.begin(), by.end(), [](const std::pair<S32, S32>& l, const std::pair<S32, S32>& r) { return l.first > r.first; });
    for (const auto& pc : by)
    {
        const std::string key = llformat("%d", pc.first);
        const F64 dmg = mPriceRules["gun_damage"].has(key) ? mPriceRules["gun_damage"][key].asReal() : mPriceRules["round_shot_damage"].asReal();
        const S32 cr = (S32)std::lround(pc.second * mPriceRules["per_damage_point"].asReal() * dmg);
        text += llformat(" + %d \xC3\x97 %d-pounder%s %d", pc.second, pc.first, pc.second == 1 ? "" : "s", cr);
        total += cr;
    }
    text += llformat(" = %d %s", total, getString("credits_word").c_str());
    mPrice->setText(text);
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

void WolfFloaterShipBuilder::updateControls()
{
    const bool wolf = WolfGrid::isOnWolfTerritories();
    const bool open = wolf && mReady && mRefused.empty();
    const bool ordering = !mOrder.empty();
    for (LLUICtrl* c : std::initializer_list<LLUICtrl*>{ mHullType, mLength, mGuns, mFigurehead, mHullColour, mTrimColour, mSailColour, mName })
    {
        c->setEnabled(open && !ordering);
    }
    const bool shortOf = open && mAvailable >= 0 && mAvailable < mPriceNow;
    mBuildBtn->setEnabled(open && !mBusy && !ordering && !shortOf && mAvailable >= 0 && nameProblem().empty());
    mBuyBtn->setVisible(shortOf);
    mCancelBtn->setVisible(ordering && mOrderState == "queued");
}

void WolfFloaterShipBuilder::onBuild()
{
    if (mBusy || !mOrder.empty() || !nameProblem().empty()) return;
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

void WolfFloaterShipBuilder::onCancelOrder()
{
    if (mOrder.empty() || mBusy) return;
    request("cancel", LLSD().with("job", mOrder));
}

void WolfFloaterShipBuilder::showOrder(const LLSD& o)
{
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
            // the status bar's "AI n" and this window's balance: read again now and after billing (wolfai.cpp creditsUsed)
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

void WolfFloaterShipBuilder::request(const std::string& what, const LLSD& body)
{
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfShipBuilder", [h, what, body]() { requestCoro(h, what, body); });
}

// static
void WolfFloaterShipBuilder::requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body)
{
    body["action"] = what;
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLSD result = WolfCaves::serviceRequest(SHIPYARD_URL, &text, what == "preview" ? PREVIEW_TIMEOUT_S : TIMEOUT_S);
    LLSD reply;
    std::string transport;
    const S32 status = WolfCaves::serviceReply(result, reply, transport);
    const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
    const std::string error = ok ? std::string() : shipyardError(status, reply, transport);
    if (WolfFloaterShipBuilder* f = dynamic_cast<WolfFloaterShipBuilder*>(h.get()))
    {
        f->onReply(what, body, ok, reply, error);
    }
}

void WolfFloaterShipBuilder::onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error)
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
            // a member check refusal stays (nothing else to do here); anything else is asked again when the window is
            // reopened (never in a loop from draw)
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
        std::shared_ptr<const WolfShipPreview::Mesh> mesh = ok ? WolfShipPreview::parse(reply["preview"]) : nullptr;
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
                mPreviewClass.clear();      // choosing the hull type again asks again
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
        else setStatus(error, true);     // the order is kept: the next look tries again
    }
    updateControls();
}
