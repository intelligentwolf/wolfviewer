/**
 * @file wolfmapoverlays.cpp
 * @brief WolfViewer: images region owners lay over the World Map (Wolf Territories only).
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
#include <algorithm>
#include <cmath>

#include "wolfmapoverlays.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llhttpconstants.h"
#include "llinventory.h"
#include "llnotificationsutil.h"
#include "llpermissions.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdjson.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llworldmap.h"
#include "llworldmapview.h"
#include "wolfgrid.h"

// Source: wolfstorm/js/world/map_overlays.js MapOverlays.API — one host owns the data for every viewer.
const char* WolfMapOverlays::API_URL = "https://wolfstorm.app/php/map_overlays.php";

namespace
{
    // Source: wolfstorm/php/map_overlays.php OVL_MIN_SIZE / OVL_RIGHTS_MAX; map_overlays.js
    // REFRESH_MS / HANDLE_PX / MAP_IMAGES_GRID_MESSAGE.
    constexpr F64 MIN_M = 2.0;
    constexpr size_t RIGHTS_MAX = 64;
    constexpr F64 REFRESH_SECONDS = 120.0;
    constexpr F64 REFETCH_MIN_SECONDS = 0.5;   // while panning: at most one fetch per half second
    constexpr F32 HANDLE_PX = 5.f;              // half-size of a corner handle
    const char* const MAP_IMAGES_GRID_MESSAGE = "Sorry, this function is only available on Wolf Territories Grid.";

    // Source: wolfterrainpaint.cpp json_to_llsd — the service answers JSON, not LLSD.
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    void notify_error(const std::string& msg)
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", msg));
    }

    void notify_tip(const std::string& msg)
    {
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", msg));
    }

    /** Clamp a rectangle (global metres) into a region box, keeping at least MIN_M a side. */
    void clamp_rect(F64& x, F64& y, F64& w, F64& h, F64 bx, F64 by, F64 bw, F64 bh)
    {
        w = llmax(MIN_M, llmin(w, bw));
        h = llmax(MIN_M, llmin(h, bh));
        x = llmax(bx, llmin(x, bx + bw - w));
        y = llmax(by, llmin(y, by + bh - h));
    }

    /** Text on a black box, so it reads over any map tile (Paul 09-25: white text over the map
     *  is difficult to read). (x, y) is the text's top-left; returns the box height. */
    S32 draw_boxed_text(const LLFontGL* font, const std::string& text, S32 x, S32 y)
    {
        const S32 pad = 4;
        const S32 w = font->getWidth(text);
        const S32 h = (S32)font->getLineHeight();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gl_rect_2d(x - pad, y + pad, x + w + pad, y - h - pad, LLColor4(0.f, 0.f, 0.f, 0.85f), true);
        font->renderUTF8(text, 0, (F32)x, (F32)y, LLColor4::white,
                         LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        return h + 2 * pad;
    }

    std::string display_name(const WolfMapOverlays::Overlay& o)
    {
        return o.mName.empty() ? std::string("(unnamed image)") : o.mName;
    }
}

WolfMapOverlays::WolfMapOverlays() {}
WolfMapOverlays::~WolfMapOverlays() {}

bool WolfMapOverlays::active() const
{
    // Always drawn on the grid — Paul 09-25: "make sure everyone can always see map textures".
    return WolfGrid::isWolfTerritories();
}

bool WolfMapOverlays::editMode() const
{
    static LLCachedControl<bool> edit(gSavedSettings, "WolfMapEditImages", false);
    return edit && active();
}

WolfMapOverlays::Overlay* WolfMapOverlays::find(S32 id)
{
    for (Overlay& o : mOverlays)
    {
        if (o.mId == id) return &o;
    }
    return nullptr;
}

// ── geometry ──────────────────────────────────────────────────────────────────────────

void WolfMapOverlays::viewRect(LLWorldMapView& view, const Overlay& o, F32& left, F32& bottom, F32& right, F32& top)
{
    const LLVector3 sw = view.globalPosToView(LLVector3d(o.mX, o.mY, 0.0));
    const LLVector3 ne = view.globalPosToView(LLVector3d(o.mX + o.mW, o.mY + o.mH, 0.0));
    left = sw.mV[VX];
    bottom = sw.mV[VY];
    right = ne.mV[VX];
    top = ne.mV[VY];
}

S32 WolfMapOverlays::hitOverlay(const LLVector3d& g) const
{
    for (S32 i = (S32)mOverlays.size() - 1; i >= 0; --i)
    {
        const Overlay& o = mOverlays[i];
        if (!canEdit(o)) continue;
        if (g.mdV[VX] >= o.mX && g.mdV[VX] <= o.mX + o.mW && g.mdV[VY] >= o.mY && g.mdV[VY] <= o.mY + o.mH) return i;
    }
    return -1;
}

S32 WolfMapOverlays::hitCorner(LLWorldMapView& view, S32 x, S32 y)
{
    Overlay* o = find(mSelectedId);
    if (!o || !canEdit(*o)) return 0;
    const LLVector3d g = view.viewPosToGlobal(x, y);
    // The handle is HANDLE_PX view pixels either side of the corner, measured in metres on the
    // untilted map (viewPosToGlobal undoes the tilt) — Source: llworldmapview.cpp viewPosToGlobal.
    const F64 tol = (F64)(HANDLE_PX + 2.f) * REGION_WIDTH_METERS / llmax(0.001f, view.getScale());
    const F64 cx[4] = { o->mX, o->mX + o->mW, o->mX, o->mX + o->mW };
    const F64 cy[4] = { o->mY, o->mY, o->mY + o->mH, o->mY + o->mH };
    for (S32 i = 0; i < 4; ++i)
    {
        if (fabs(g.mdV[VX] - cx[i]) <= tol && fabs(g.mdV[VY] - cy[i]) <= tol) return i + 1;   // 1 SW, 2 SE, 3 NW, 4 NE
    }
    return 0;
}

// ── drawing ───────────────────────────────────────────────────────────────────────────

void WolfMapOverlays::draw(LLWorldMapView& view)
{
    if (!active()) return;

    if (mOwns < 0) askOwner();
    const bool edit = editMode();
    if (edit != mLastEditMode)
    {
        mLastEditMode = edit;
        if (edit) askRights();
        else { mSelectedId = 0; mOp = Op(); }
    }

    ensureFetched(view);

    const F32 width = (F32)view.getRect().getWidth();
    const F32 height = (F32)view.getRect().getHeight();
    LLGLSUIDefault gls_ui;
    for (Overlay& o : mOverlays)
    {
        F32 left, bottom, right, top;
        viewRect(view, o, left, bottom, right, top);
        // Generous cull: under the map's tilt more than the flat view rect can be on screen.
        if (right < -width || left > 2.f * width || top < -height || bottom > 2.f * height) continue;
        if (right - left < 1.f || top - bottom < 1.f) continue;

        if (o.mImage.isNull())
        {
            // Source: llworldmap.cpp:103 — how the map fetches its own images.
            o.mImage = LLViewerTextureManager::getFetchedTexture(o.mTexture, FTT_DEFAULT, MIPMAP_TRUE,
                                                                 LLGLTexture::BOOST_MAP, LLViewerTexture::LOD_TEXTURE);
            o.mImage->setAddressMode(LLTexUnit::TAM_CLAMP);
        }
        // Source: llworldmapview.cpp land-for-sale overlay — tell the fetcher the size we need.
        const S32 draw_w = llclamp(ll_round((right - left) * LLUI::getScaleFactor().mV[VX]), 1, 1024);
        const S32 draw_h = llclamp(ll_round((top - bottom) * LLUI::getScaleFactor().mV[VY]), 1, 1024);
        o.mImage->setKnownDrawSize(draw_w, draw_h);

        if (o.mImage->hasGLTexture())
        {
            // Source: llworldmapview.cpp land-for-sale overlay quad (texcoord 0,1 at the top left).
            gGL.getTexUnit(0)->bind(o.mImage.get());
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            gGL.begin(LLRender::TRIANGLES);
            {
                gGL.texCoord2f(0.f, 1.f);
                gGL.vertex3f(left, top, -0.5f);
                gGL.texCoord2f(0.f, 0.f);
                gGL.vertex3f(left, bottom, -0.5f);
                gGL.texCoord2f(1.f, 0.f);
                gGL.vertex3f(right, bottom, -0.5f);

                gGL.texCoord2f(0.f, 1.f);
                gGL.vertex3f(left, top, -0.5f);
                gGL.texCoord2f(1.f, 0.f);
                gGL.vertex3f(right, bottom, -0.5f);
                gGL.texCoord2f(1.f, 1.f);
                gGL.vertex3f(right, top, -0.5f);
            }
            gGL.end();
        }
        else
        {
            // Still downloading: a faint box where it will be.
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gl_rect_2d(ll_round(left), ll_round(top), ll_round(right), ll_round(bottom), LLColor4(1.f, 1.f, 1.f, 0.12f), true);
        }

        if (edit && canEdit(o)) drawEditFrame(view, o, left, bottom, right, top);
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

void WolfMapOverlays::drawEditFrame(LLWorldMapView& view, const Overlay& o, F32 left, F32 bottom, F32 right, F32 top)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    const bool selected = (o.mId == mSelectedId);
    const LLColor4 frame = selected ? LLColor4(1.f, 0.82f, 0.29f, 1.f) : LLColor4(1.f, 1.f, 1.f, 0.85f);
    const S32 l = ll_round(left), b = ll_round(bottom), r = ll_round(right), t = ll_round(top);
    gl_rect_2d(l, t, r, b, frame, false);
    if (!selected) return;

    gl_rect_2d(l + 1, t - 1, r - 1, b + 1, frame, false);
    const S32 h = (S32)HANDLE_PX;
    const S32 cx[4] = { l, r, l, r };
    const S32 cy[4] = { b, b, t, t };
    for (S32 i = 0; i < 4; ++i)
    {
        gl_rect_2d(cx[i] - h, cy[i] + h, cx[i] + h, cy[i] - h, frame, true);
        gl_rect_2d(cx[i] - h, cy[i] + h, cx[i] + h, cy[i] - h, LLColor4(0.13f, 0.13f, 0.13f, 1.f), false);
    }
    const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
    draw_boxed_text(small, display_name(o), l + 4, t + h + 6 + (S32)small->getLineHeight());
}

void WolfMapOverlays::drawControls(LLWorldMapView& view)
{
    mDeleteRect = LLRect();
    if (!active() || mOwns != 1)
    {
        mButtonRect = LLRect();
        return;
    }
    const bool edit = editMode();
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    const std::string label = edit ? "Done editing map images" : "Edit my map images";
    const S32 h = 22;
    const S32 w = font->getWidth(label) + 20;
    const S32 left = 10;
    const S32 top = view.getRect().getHeight() - 10;
    mButtonRect.setLeftTopAndSize(left, top, w, h);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(mButtonRect, edit ? LLColor4(0.55f, 0.42f, 0.08f, 0.9f) : LLColor4(0.f, 0.f, 0.f, 0.7f), true);
    gl_rect_2d(mButtonRect, LLColor4(1.f, 1.f, 1.f, 0.8f), false);
    font->renderUTF8(label, 0, (F32)(left + 10), (F32)(top - h / 2), LLColor4::white,
                     LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);

    // "Delete image" beside it while one of the owner's images is selected (the Delete key does
    // the same, but only once the map has keyboard focus).
    const Overlay* sel = find(mSelectedId);
    if (edit && sel && canEdit(*sel))
    {
        const std::string del = "Delete \"" + display_name(*sel) + "\"";
        const S32 dw = llmin(font->getWidth(del) + 20, 320);
        mDeleteRect.setLeftTopAndSize(mButtonRect.mRight + 8, top, dw, h);
        gl_rect_2d(mDeleteRect, LLColor4(0.55f, 0.1f, 0.1f, 0.9f), true);
        gl_rect_2d(mDeleteRect, LLColor4(1.f, 1.f, 1.f, 0.8f), false);
        font->renderUTF8(del, 0, (F32)(mDeleteRect.mLeft + 10), (F32)(top - h / 2), LLColor4::white,
                         LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                         S32_MAX, dw - 20, nullptr, true);
    }
    if (edit)
    {
        const LLFontGL* small = LLFontGL::getFontSansSerifSmall();
        S32 y = top - h - 8;
        y -= draw_boxed_text(small, "Drag a texture from inventory onto your region.", left + 4, y);
        draw_boxed_text(small, "Drag an image to move it, a corner to stretch it (Shift keeps its shape), Delete removes it.", left + 4, y);
    }
}

bool WolfMapOverlays::clickControls(S32 x, S32 y)
{
    if (!active() || mOwns != 1) return false;
    if (mDeleteRect.notEmpty() && mDeleteRect.pointInRect(x, y))
    {
        confirmDelete();
        return true;
    }
    if (!mButtonRect.pointInRect(x, y)) return false;
    gSavedSettings.setBOOL("WolfMapEditImages", !editMode());
    return true;
}

// ── mouse ─────────────────────────────────────────────────────────────────────────────

bool WolfMapOverlays::mouseDown(LLWorldMapView& view, S32 x, S32 y, MASK mask)
{
    if (!editMode()) return false;
    const LLVector3d g = view.viewPosToGlobal(x, y);
    const S32 corner = hitCorner(view, x, y);
    Overlay* o = nullptr;
    if (corner) o = find(mSelectedId);
    else
    {
        const S32 i = hitOverlay(g);
        if (i >= 0) o = &mOverlays[i];
    }
    if (!o)
    {
        mSelectedId = 0;
        return false;
    }
    mSelectedId = o->mId;
    if (mSaving.count(o->mId))
    {
        notify_tip("Still saving the last change to that image...");
        return true;
    }
    mOp = Op();
    mOp.mId = o->mId;
    mOp.mOrigX = o->mX; mOp.mOrigY = o->mY; mOp.mOrigW = o->mW; mOp.mOrigH = o->mH;
    mOp.mGrab = g;
    if (corner)
    {
        // The opposite corner stays put. Corners: 1 SW, 2 SE, 3 NW, 4 NE.
        mOp.mKind = Op::RESIZE;
        mOp.mAnchorX = (corner == 1 || corner == 3) ? o->mX + o->mW : o->mX;
        mOp.mAnchorY = (corner == 1 || corner == 2) ? o->mY + o->mH : o->mY;
    }
    else
    {
        mOp.mKind = Op::MOVE;
    }
    return true;
}

void WolfMapOverlays::mouseMove(LLWorldMapView& view, S32 x, S32 y, MASK mask)
{
    if (mOp.mKind == Op::NONE) return;
    Overlay* o = find(mOp.mId);
    if (!o) { mOp = Op(); return; }
    const LLVector3d g = view.viewPosToGlobal(x, y);
    F64 nx, ny, nw, nh;
    if (mOp.mKind == Op::MOVE)
    {
        nx = mOp.mOrigX + (g.mdV[VX] - mOp.mGrab.mdV[VX]);
        ny = mOp.mOrigY + (g.mdV[VY] - mOp.mGrab.mdV[VY]);
        nw = mOp.mOrigW;
        nh = mOp.mOrigH;
    }
    else
    {
        // The moving corner follows the cursor, held inside the region.
        F64 cx = llclamp(g.mdV[VX], o->mRegionX, o->mRegionX + o->mRegionW);
        F64 cy = llclamp(g.mdV[VY], o->mRegionY, o->mRegionY + o->mRegionH);
        if ((mask & MASK_SHIFT) && mOp.mOrigH > 0.0)
        {
            // Keep the starting shape: the longer pull decides the size.
            const F64 aspect = mOp.mOrigW / mOp.mOrigH;
            const F64 dx = cx - mOp.mAnchorX, dy = cy - mOp.mAnchorY;
            if (fabs(dx) / aspect > fabs(dy)) cy = mOp.mAnchorY + (dy < 0.0 ? -1.0 : 1.0) * fabs(dx) / aspect;
            else cx = mOp.mAnchorX + (dx < 0.0 ? -1.0 : 1.0) * fabs(dy) * aspect;
        }
        nx = llmin(mOp.mAnchorX, cx);
        ny = llmin(mOp.mAnchorY, cy);
        nw = fabs(cx - mOp.mAnchorX);
        nh = fabs(cy - mOp.mAnchorY);
    }
    clamp_rect(nx, ny, nw, nh, o->mRegionX, o->mRegionY, o->mRegionW, o->mRegionH);
    o->mX = nx; o->mY = ny; o->mW = nw; o->mH = nh;
    mOp.mMoved = true;
}

void WolfMapOverlays::mouseUp()
{
    const Op op = mOp;
    mOp = Op();
    if (op.mKind == Op::NONE || !op.mMoved) return;
    Overlay* o = find(op.mId);
    if (!o) return;
    const S32 id = o->mId, version = o->mVersion;
    const F64 x = o->mX, y = o->mY, w = o->mW, h = o->mH;
    const F64 ox = op.mOrigX, oy = op.mOrigY, ow = op.mOrigW, oh = op.mOrigH;
    mSaving.insert(id);
    LLCoros::instance().launch("WolfMapOverlays update", [id, version, x, y, w, h, ox, oy, ow, oh]()
    {
        WolfMapOverlays::instance().updateCoro(id, version, x, y, w, h, ox, oy, ow, oh);
    });
}

bool WolfMapOverlays::hoverCursor(LLWorldMapView& view, S32 x, S32 y)
{
    if (active() && mOwns == 1 && (mButtonRect.pointInRect(x, y) || (mDeleteRect.notEmpty() && mDeleteRect.pointInRect(x, y))))
    {
        gViewerWindow->setCursor(UI_CURSOR_HAND);
        return true;
    }
    if (!editMode()) return false;
    const S32 corner = hitCorner(view, x, y);
    if (corner)
    {
        // SW / NE sit on the "/" diagonal, SE / NW on the "\" one.
        gViewerWindow->setCursor((corner == 1 || corner == 4) ? UI_CURSOR_SIZENESW : UI_CURSOR_SIZENWSE);
        return true;
    }
    if (hitOverlay(view.viewPosToGlobal(x, y)) >= 0)
    {
        gViewerWindow->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return false;
}

bool WolfMapOverlays::hitEditable(LLWorldMapView& view, S32 x, S32 y)
{
    if (active() && mOwns == 1 && (mButtonRect.pointInRect(x, y) || (mDeleteRect.notEmpty() && mDeleteRect.pointInRect(x, y)))) return true;
    if (!editMode()) return false;
    return hitOverlay(view.viewPosToGlobal(x, y)) >= 0;
}

std::string WolfMapOverlays::toolTipAt(LLWorldMapView& view, S32 x, S32 y)
{
    if (!active()) return std::string();
    const LLVector3d g = view.viewPosToGlobal(x, y);
    for (auto it = mOverlays.rbegin(); it != mOverlays.rend(); ++it)
    {
        if (g.mdV[VX] >= it->mX && g.mdV[VX] <= it->mX + it->mW && g.mdV[VY] >= it->mY && g.mdV[VY] <= it->mY + it->mH)
        {
            return display_name(*it);
        }
    }
    return std::string();
}

// ── keys ──────────────────────────────────────────────────────────────────────────────

bool WolfMapOverlays::handleKey(KEY key, MASK mask)
{
    if ((key != KEY_DELETE && key != KEY_BACKSPACE) || mask != MASK_NONE) return false;
    if (!editMode()) return false;
    Overlay* o = find(mSelectedId);
    if (!o || !canEdit(*o)) return false;
    confirmDelete();
    return true;
}

void WolfMapOverlays::confirmDelete()
{
    Overlay* o = find(mSelectedId);
    if (!o || !canEdit(*o)) return;
    const S32 id = o->mId;
    const std::string name = display_name(*o);
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Remove \"" + name + "\" from the map? Everyone will stop seeing it. The texture stays in your inventory."),
        LLSD(),
        [id, name](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                LLCoros::instance().launch("WolfMapOverlays delete", [id, name]()
                {
                    WolfMapOverlays::instance().deleteCoro(id, name);
                });
            }
            return false;
        });
}

// ── drop from inventory ───────────────────────────────────────────────────────────────

bool WolfMapOverlays::dragAndDrop(LLWorldMapView& view, S32 x, S32 y, bool drop, EDragAndDropType cargo_type,
                                  void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    // Source: lltexturectrl.cpp LLTextureCtrl::handleDragAndDrop — DAD_TEXTURE cargo is the
    // LLInventoryItem being dragged.
    if (cargo_type != DAD_TEXTURE) return false;
    *accept = ACCEPT_NO;
    if (!WolfGrid::isWolfTerritories())
    {
        tooltip_msg = MAP_IMAGES_GRID_MESSAGE;
        if (drop) notify_error(MAP_IMAGES_GRID_MESSAGE);
        return true;
    }
    LLInventoryItem* item = (LLInventoryItem*)cargo_data;
    if (!item) return true;
    if (mOwns == 0)
    {
        tooltip_msg = "You can only place images on your own regions.";
        if (drop) notify_error(tooltip_msg);
        return true;
    }
    // Source: lltexturectrl.cpp LLTextureCtrl::allowDrop — the viewer's own permission checks.
    // The map shows the image to everyone, so it must be the agent's to copy AND give away.
    const LLPermissions& perm = item->getPermissions();
    if (!perm.allowCopyBy(gAgent.getID()) || !perm.allowOperationBy(PERM_TRANSFER, gAgent.getID()))
    {
        tooltip_msg = "You need copy and transfer rights on this image to show it on the map.";
        return true;
    }
    *accept = ACCEPT_YES_COPY_SINGLE;
    tooltip_msg = "Place this image on the map (on a region you own)";
    if (!drop) return true;
    LL_INFOS("WolfMapOverlays") << "texture \"" << item->getName() << "\" dropped on the map" << LL_ENDL;

    // Centred where it was dropped, a quarter of the view wide, the image's own shape when the
    // viewer already knows it, and fitted into the region under the drop point.
    const LLVector3d g = view.viewPosToGlobal(x, y);
    const F64 view_m = (F64)view.getRect().getWidth() * REGION_WIDTH_METERS / llmax(0.001f, view.getScale());
    F64 w = llmax(MIN_M, view_m / 4.0);
    F64 h = w;
    LLViewerFetchedTexture* tex = LLViewerTextureManager::getFetchedTexture(item->getAssetUUID(), FTT_DEFAULT, MIPMAP_TRUE,
                                                                             LLGLTexture::BOOST_MAP, LLViewerTexture::LOD_TEXTURE);
    if (tex && tex->getFullWidth() > 0 && tex->getFullHeight() > 0)
    {
        h = w * (F64)tex->getFullHeight() / (F64)tex->getFullWidth();
    }
    F64 rx = g.mdV[VX] - w / 2.0, ry = g.mdV[VY] - h / 2.0;
    if (LLSimInfo* info = LLWorldMap::getInstance()->simInfoFromPosGlobal(g))
    {
        const LLVector3d origin = info->getGlobalOrigin();
        const F64 bw = info->getSizeX() ? (F64)info->getSizeX() : REGION_WIDTH_METERS;
        const F64 bh = info->getSizeY() ? (F64)info->getSizeY() : REGION_WIDTH_METERS;
        const F64 fit = llmin(1.0, llmin(bw / w, bh / h));
        w *= fit; h *= fit;
        rx = g.mdV[VX] - w / 2.0; ry = g.mdV[VY] - h / 2.0;
        clamp_rect(rx, ry, w, h, origin.mdV[VX], origin.mdV[VY], bw, bh);
    }
    const LLUUID item_id = item->getUUID();
    const std::string item_name = item->getName();
    notify_tip("Placing \"" + item_name + "\" on the map...");
    LLCoros::instance().launch("WolfMapOverlays create", [item_id, item_name, rx, ry, w, h]()
    {
        WolfMapOverlays::instance().createCoro(item_id, item_name, rx, ry, w, h);
    });
    return true;
}

// ── service ───────────────────────────────────────────────────────────────────────────

bool WolfMapOverlays::parseOverlay(const LLSD& in, Overlay& out)
{
    if (!in.isMap() || !in.has("id")) return false;
    out.mId = in["id"].asInteger();
    out.mRegion.set(in["region"].asString(), false);
    out.mTexture.set(in["texture"].asString(), false);
    out.mName = in["name"].asString();
    out.mRegionName = in["regionName"].asString();
    out.mX = in["x"].asReal();
    out.mY = in["y"].asReal();
    out.mW = in["w"].asReal();
    out.mH = in["h"].asReal();
    out.mRegionX = in["regionX"].asReal();
    out.mRegionY = in["regionY"].asReal();
    out.mRegionW = in["regionW"].asReal();
    out.mRegionH = in["regionH"].asReal();
    out.mVersion = in["version"].asInteger();
    return out.mId > 0 && out.mTexture.notNull() && out.mW > 0.0 && out.mH > 0.0;
}

void WolfMapOverlays::storeOverlay(const Overlay& in)
{
    Overlay o = in;
    for (Overlay& have : mOverlays)
    {
        if (have.mId == o.mId)
        {
            if (have.mTexture == o.mTexture) o.mImage = have.mImage;
            have = o;
            return;
        }
    }
    auto pos = std::lower_bound(mOverlays.begin(), mOverlays.end(), o.mId,
                                [](const Overlay& a, S32 id) { return a.mId < id; });
    mOverlays.insert(pos, o);
}

void WolfMapOverlays::ensureFetched(LLWorldMapView& view)
{
    if (mFetching) return;
    const F64 now = LLTimer::getElapsedSeconds();
    if (now - mFetchedAt < REFETCH_MIN_SECONDS) return;

    // The visible map in global metres: all four corners, because the tilt widens the far side.
    const S32 w = view.getRect().getWidth(), h = view.getRect().getHeight();
    const LLVector3d c[4] = { view.viewPosToGlobal(0, 0), view.viewPosToGlobal(w, 0),
                              view.viewPosToGlobal(0, h), view.viewPosToGlobal(w, h) };
    F64 x0 = c[0].mdV[VX], x1 = x0, y0 = c[0].mdV[VY], y1 = y0;
    for (const LLVector3d& p : c)
    {
        x0 = llmin(x0, p.mdV[VX]); x1 = llmax(x1, p.mdV[VX]);
        y0 = llmin(y0, p.mdV[VY]); y1 = llmax(y1, p.mdV[VY]);
    }
    const bool inside = mHaveBox && x0 >= mBoxX0 && x1 <= mBoxX1 && y0 >= mBoxY0 && y1 <= mBoxY1;
    if (inside && now - mFetchedAt < REFRESH_SECONDS) return;

    // One view's width / height of margin on every side, so a short pan needs no fetch.
    const F64 mx = x1 - x0, my = y1 - y0;
    const F64 bx0 = llmax(0.0, x0 - mx), by0 = llmax(0.0, y0 - my), bx1 = x1 + mx, by1 = y1 + my;
    mFetching = true;
    LLCoros::instance().launch("WolfMapOverlays fetch", [bx0, by0, bx1, by1]()
    {
        WolfMapOverlays::instance().fetchCoro(bx0, by0, bx1, by1);
    });
}

// Source: wolfterrainpaint.cpp fetchCoro — the same adapter shape (getRawAndSuspend, JSON body).
void WolfMapOverlays::fetchCoro(F64 x0, F64 y0, F64 x1, F64 y1)
{
    if (!WolfGrid::isWolfTerritories()) { mFetching = false; return; }
    const std::string url = std::string(API_URL) + llformat("?x0=%.0f&y0=%.0f&x1=%.0f&y1=%.0f", x0, y0, x1, y1);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfMapOverlays", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(20);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    const U32 gen = mWriteGen;

    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    // The box is recorded whatever happened: a failed read is retried after REFRESH_SECONDS,
    // not every frame.
    mFetching = false;
    mHaveBox = true;
    mBoxX0 = x0; mBoxY0 = y0; mBoxX1 = x1; mBoxY1 = y1;
    mFetchedAt = LLTimer::getElapsedSeconds();
    if (!status)
    {
        LL_WARNS("WolfMapOverlays") << "fetch failed: " << status.toString() << LL_ENDL;
        return;
    }
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    if (!reply.isMap() || !reply["success"].asBoolean() || !reply["overlays"].isArray())
    {
        LL_WARNS("WolfMapOverlays") << "fetch: unexpected reply" << LL_ENDL;
        return;
    }
    if (gen != mWriteGen)
    {
        // One of our own writes landed while this read was out: it may predate it. Read again
        // (ensureFetched asks once REFETCH_MIN_SECONDS has passed).
        mHaveBox = false;
        return;
    }

    std::vector<Overlay> next;
    for (const LLSD& r : llsd::inArray(reply["overlays"]))
    {
        Overlay o;
        if (!parseOverlay(r, o)) continue;
        // An image being dragged or saved keeps the local copy until that finishes.
        if ((mOp.mKind != Op::NONE && mOp.mId == o.mId) || mSaving.count(o.mId)) continue;
        if (const Overlay* have = find(o.mId))
        {
            if (have->mTexture == o.mTexture) o.mImage = have->mImage;
        }
        next.push_back(o);
    }
    for (const Overlay& have : mOverlays)
    {
        if ((mOp.mKind != Op::NONE && mOp.mId == have.mId) || mSaving.count(have.mId)) next.push_back(have);
    }
    std::sort(next.begin(), next.end(), [](const Overlay& a, const Overlay& b) { return a.mId < b.mId; });
    mOverlays.swap(next);
    LL_INFOS("WolfMapOverlays") << "fetched " << mOverlays.size() << " map image(s) for box "
                                << llformat("%.0f,%.0f-%.0f,%.0f", x0, y0, x1, y1) << LL_ENDL;
    if (mSelectedId && !find(mSelectedId)) mSelectedId = 0;
    if (editMode()) askRights();
}

void WolfMapOverlays::askRights()
{
    std::vector<LLUUID> want;
    for (const Overlay& o : mOverlays)
    {
        if (mAskedRights.count(o.mRegion) || std::find(want.begin(), want.end(), o.mRegion) != want.end()) continue;
        want.push_back(o.mRegion);
        if (want.size() >= RIGHTS_MAX) break;
    }
    if (want.empty()) return;
    for (const LLUUID& u : want) mAskedRights.insert(u);
    LLCoros::instance().launch("WolfMapOverlays rights", [want]()
    {
        WolfMapOverlays::instance().rightsCoro(want);
    });
}

void WolfMapOverlays::askOwner()
{
    if (mAskingOwner || LLTimer::getElapsedSeconds() < mOwnerRetryAt) return;
    mAskingOwner = true;
    LLCoros::instance().launch("WolfMapOverlays owner", []()
    {
        WolfMapOverlays::instance().ownerCoro();
    });
}

// Once per session: does the agent own any region (and so get the editing button)?
void WolfMapOverlays::ownerCoro()
{
    LLSD body;
    body["action"] = "owner";
    S32 http_status = 0;
    LLSD reply = post(body, http_status);
    mAskingOwner = false;
    if (!reply["success"].asBoolean())
    {
        // Unknown: ask again in a minute. The images still show either way.
        mOwnerRetryAt = LLTimer::getElapsedSeconds() + 60.0;
        LL_WARNS("WolfMapOverlays") << "owner check failed: " << reply["error"].asString() << LL_ENDL;
        return;
    }
    mOwns = reply["owns"].asBoolean() ? 1 : 0;
    LL_INFOS("WolfMapOverlays") << "owns a region: " << (mOwns ? "yes" : "no") << LL_ENDL;
}

void WolfMapOverlays::rightsCoro(std::vector<LLUUID> regions)
{
    LLSD body;
    body["action"] = "rights";
    LLSD list = LLSD::emptyArray();
    for (const LLUUID& u : regions) list.append(u.asString());
    body["regions"] = list;
    S32 http_status = 0;
    LLSD reply = post(body, http_status);
    if (!reply["success"].asBoolean())
    {
        for (const LLUUID& u : regions) mAskedRights.erase(u);
        notify_error("Could not check which map images you can edit: " + reply["error"].asString());
        return;
    }
    for (const LLSD& u : llsd::inArray(reply["editable"]))
    {
        mEditable.insert(LLUUID(u.asString()));
    }
}

// Source: wolfterrainpaint.cpp saveCoro — agent + session ids the service verifies against the
// grid's presence service; no secret is carried by this (public) viewer.
LLSD WolfMapOverlays::post(const LLSD& body, S32& http_status)
{
    http_status = 0;
    if (!WolfGrid::isWolfTerritories())
    {
        return LLSD().with("success", false).with("error", MAP_IMAGES_GRID_MESSAGE);
    }
    const std::string text = boost::json::serialize(LlsdToJson(body));
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfMapOverlays", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(text.data(), text.size());

    LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
    if (status.isHttpStatus()) http_status = status.getType();
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
    }
    const std::string action = body["action"].asString();
    if (reply.isMap() && reply["success"].asBoolean() && (action == "create" || action == "update" || action == "delete"))
    {
        ++mWriteGen;
    }
    if (!reply.isMap())
    {
        reply = LLSD().with("success", false).with("error", status ? std::string("unexpected reply") : status.toString());
    }
    else if (!status && !reply.has("error"))
    {
        reply["success"] = false;
        reply["error"] = status.toString();
    }
    return reply;
}

void WolfMapOverlays::createCoro(LLUUID item, std::string item_name, F64 x, F64 y, F64 w, F64 h)
{
    LLSD body;
    body["action"] = "create";
    body["item"] = item.asString();
    body["x"] = x; body["y"] = y; body["w"] = w; body["h"] = h;
    S32 http_status = 0;
    LLSD reply = post(body, http_status);
    Overlay o;
    if (!reply["success"].asBoolean() || !parseOverlay(reply["overlay"], o))
    {
        LL_WARNS("WolfMapOverlays") << "create refused (HTTP " << http_status << "): " << reply["error"].asString() << LL_ENDL;
        notify_error("Could not place \"" + item_name + "\": " + reply["error"].asString());
        return;
    }
    storeOverlay(o);
    mOwns = 1;
    mEditable.insert(o.mRegion);
    mAskedRights.insert(o.mRegion);
    mSelectedId = o.mId;
    // Leave the new image ready to move and stretch.
    gSavedSettings.setBOOL("WolfMapEditImages", true);
    notify_tip("Placed \"" + display_name(o) + "\" on " + o.mRegionName + ". Drag it to move it, drag a corner to stretch it.");
}

void WolfMapOverlays::updateCoro(S32 id, S32 version, F64 x, F64 y, F64 w, F64 h, F64 ox, F64 oy, F64 ow, F64 oh)
{
    LLSD body;
    body["action"] = "update";
    body["id"] = id;
    body["version"] = version;
    body["x"] = x; body["y"] = y; body["w"] = w; body["h"] = h;
    S32 http_status = 0;
    LLSD reply = post(body, http_status);
    mSaving.erase(id);
    Overlay o;
    if (reply["success"].asBoolean() && parseOverlay(reply["overlay"], o))
    {
        storeOverlay(o);
        return;
    }
    std::string name = "map image";
    if (parseOverlay(reply["overlay"], o))
    {
        storeOverlay(o);            // 409: the server's copy
        name = display_name(o);
    }
    else if (Overlay* have = find(id))
    {
        name = display_name(*have);
        if (http_status == 404)
        {
            mOverlays.erase(std::remove_if(mOverlays.begin(), mOverlays.end(), [id](const Overlay& e) { return e.mId == id; }), mOverlays.end());
            if (mSelectedId == id) mSelectedId = 0;
        }
        else
        {
            have->mX = ox; have->mY = oy; have->mW = ow; have->mH = oh;
        }
    }
    notify_error("Could not save \"" + name + "\": " + reply["error"].asString());
}

void WolfMapOverlays::deleteCoro(S32 id, std::string name)
{
    LLSD body;
    body["action"] = "delete";
    body["id"] = id;
    S32 http_status = 0;
    LLSD reply = post(body, http_status);
    const bool ok = reply["success"].asBoolean();
    if (ok || http_status == 404)
    {
        mOverlays.erase(std::remove_if(mOverlays.begin(), mOverlays.end(), [id](const Overlay& e) { return e.mId == id; }), mOverlays.end());
        if (mSelectedId == id) mSelectedId = 0;
    }
    if (ok) notify_tip("Removed \"" + name + "\" from the map.");
    else notify_error("Could not delete \"" + name + "\": " + reply["error"].asString());
}
