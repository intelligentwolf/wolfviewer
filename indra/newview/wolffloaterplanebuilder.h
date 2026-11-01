/**
 * @file wolffloaterplanebuilder.h
 * @brief WolfViewer: the Plane Builder - design a fighter from six real aircraft; the Wolf yard builds it into your inventory.
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

#ifndef WOLF_FLOATERPLANEBUILDER_H
#define WOLF_FLOATERPLANEBUILDER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llview.h"

class LLButton;
class LLColorSwatchCtrl;
class LLComboBox;
class LLLineEditor;
class LLSliderCtrl;
class LLTextBox;

/**
 * [PLANE BUILDER 2026-11-01] The turntable, copied from WolfShipPreview (wolffloatershipbuilder.cpp, 2026-10-29): the type's
 * low-detail mesh from the yard's "preview" ({v, t, groups}, the ship preview's shape: region-terrain-work/aircraft-dogfight-
 * 2026-11-01/f-plane-builder/README.md), turning slowly, painted in the member's upper / lower / spinner colours. Drag to turn it.
 * Flat-shaded with the painter's algorithm, as the ship's (no depth buffer in the UI pass).
 */
class WolfPlanePreview : public LLView
{
public:
    // its own Params (never another widget's: lluictrlfactory.cpp registerWidget, memory viewer-install-must-pass-startup-check)
    struct Params : LLInitParam::Block<Params, LLView::Params> { Params() {} };
    explicit WolfPlanePreview(const Params& p);

    struct Tri { U32 a, b, c; U8 group; };
    struct Mesh
    {
        std::vector<LLVector3> verts;           // model metres: nose +X, left wing +Y, Z up (DESIGN.md s14)
        std::vector<Tri> tris;
        LLVector3 centre;
        F32 radius = 1.f;
    };
    static std::shared_ptr<const Mesh> parse(const LLSD& preview);

    void setMesh(std::shared_ptr<const Mesh> mesh) { mMesh = std::move(mesh); }
    bool hasMesh() const { return (bool)mMesh; }
    void setLook(const LLColor4& upper, const LLColor4& lower, const LLColor4& spinner, F32 scale);
    void setMessage(const std::string& text) { mMessage = text; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    std::shared_ptr<const Mesh> mMesh;
    LLColor4 mUpper, mLower, mSpinner;
    F32 mScale = 1.f;
    F32 mYaw = 0.6f;
    bool mDragging = false;
    S32 mLastX = 0;
    LLFrameTimer mTurn;
    std::string mMessage;
    std::vector<LLVector3> mProj;
    std::vector<std::pair<F32, U32>> mOrder;
};

/**
 * [PLANE BUILDER 2026-11-01] Paul: "a plane generator like our boat generator".
 * Persona: a Wolf Territories member who wants a fighter of their own for the dogfights and has never built one.
 * Story: "I pick a type, make it mine - size, a period livery or my own colours, my squadron letters, its name - see what it
 * costs against my credits, press Build, and a minute or two later it is in my inventory, ready to fly and fight."
 * Flow, as the Ship Builder's (turntable left; right, top to bottom): 1. aircraft type (its figures under the picture)
 * 2. size 3. livery, colours, squadron letters, name 4. the price line by line against the credits you have (Buy credits...
 * when short) 5. Build (primary, bottom right) 6. the order's progress, then where it is. Every failure is a line in the
 * caution colour, kept until the next action; a field's own problem is shown under that field.
 * The yard (wolfstorm php/plane_builder.php) checks the account (members only), re-checks the design, holds the price and
 * charges it only once the plane is in the inventory. Wolf Territories only: the menu items are gone elsewhere, and opened off
 * Wolf the window says so and closes.
 */
class WolfFloaterPlaneBuilder : public LLFloater
{
public:
    explicit WolfFloaterPlaneBuilder(const LLSD& key);
    ~WolfFloaterPlaneBuilder() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void setStatus(const std::string& text, bool error);
    void onOpenReply(const LLSD& reply);
    void onClassChanged();
    void onLiveryChanged();
    void onColourChanged();
    void onDesignChanged();
    void onBuild();
    void onCancelOrder();
    void showOrder(const LLSD& order);
    void updatePrice();
    void updateControls();
    std::string nameProblem() const;
    std::string lettersProblem() const;
    LLSD design() const;
    const LLSD* currentClass() const;
    const LLSD* currentLivery() const;

    void request(const std::string& what, const LLSD& body);
    static void requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body);
    void onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error);

    LLComboBox *mType = nullptr, *mLivery = nullptr;
    LLSliderCtrl* mSize = nullptr;
    LLColorSwatchCtrl *mUpper = nullptr, *mLower = nullptr, *mSpinner = nullptr;
    LLLineEditor *mLetters = nullptr, *mName = nullptr;
    LLTextBox *mFigures = nullptr, *mLettersProblem = nullptr, *mNameProblem = nullptr, *mPrice = nullptr, *mBalance = nullptr, *mStatus = nullptr;
    LLButton *mBuildBtn = nullptr, *mBuyBtn = nullptr, *mCancelBtn = nullptr;
    WolfPlanePreview* mPreview = nullptr;

    LLSD mCatalogue;
    LLSD mPriceRules;
    S32 mAvailable = -1;
    S32 mBalanceNow = -1;
    S32 mPriceNow = 0;
    bool mReady = false;
    bool mBusy = false;
    bool mSettingLivery = false;             // the swatches are being set from a livery: not "custom"
    std::string mRefused;
    bool mOpenFailed = false;
    std::string mOrder;
    std::string mOrderState;
    LLFrameTimer mPoll;
    bool mPolling = false;
    std::string mPreviewClass;
    static std::map<std::string, std::shared_ptr<const WolfPlanePreview::Mesh>> sMeshes;
};

#endif // WOLF_FLOATERPLANEBUILDER_H
