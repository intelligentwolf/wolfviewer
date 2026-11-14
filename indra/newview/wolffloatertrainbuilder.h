/**
 * @file wolffloatertrainbuilder.h
 * @brief WolfViewer: the Train Builder - choose a train, make it yours; the Wolf train yard builds it into your inventory in a
 *        train box that puts it on your own track.
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

#ifndef WOLF_FLOATERTRAINBUILDER_H
#define WOLF_FLOATERTRAINBUILDER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llview.h"

class LLButton;
class LLCheckBoxCtrl;
class LLColorSwatchCtrl;
class LLComboBox;
class LLLineEditor;
class LLSliderCtrl;
class LLTextBox;

/**
 * [TRAIN BUILDER 2026-11-14] The turntable: WolfPlanePreview (wolffloaterplanebuilder.cpp) for a short stretch of train -
 * the class's engine and the vehicles that show its choices, nose to tail along -X - from the yard's "preview"
 * ({v, t, groups}: region-terrain-work/train-generator-2026-11-14/generator/make_preview.py). A group is either one the
 * member colours ("main", "second", "carriage_lower", "carriage_upper") or a fixed colour "#rrggbb". Drag to turn it.
 */
class WolfTrainPreview : public LLView
{
public:
    // its own Params (never another widget's: lluictrlfactory.cpp registerWidget, memory viewer-install-must-pass-startup-check)
    struct Params : LLInitParam::Block<Params, LLView::Params> { Params() {} };
    explicit WolfTrainPreview(const Params& p);

    struct Tri { U32 a, b, c; U16 group; };
    struct Mesh
    {
        std::vector<LLVector3> verts;           // metres: +X forward, +Y left, Z up (trains.py "Vehicle frame")
        std::vector<Tri> tris;
        std::vector<std::string> groups;        // the names as the yard sent them
        LLVector3 centre;
        F32 radius = 1.f;
    };
    static std::shared_ptr<const Mesh> parse(const LLSD& preview);

    void setMesh(std::shared_ptr<const Mesh> mesh);
    void setColours(const std::map<std::string, LLColor4>& colours) { mColours = colours; mPalette.clear(); }
    void setMessage(const std::string& text) { mMessage = text; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    const LLColor4& colourOf(U16 group);

    std::shared_ptr<const Mesh> mMesh;
    std::map<std::string, LLColor4> mColours;
    std::vector<LLColor4> mPalette;            // per group of mMesh, built from mColours when either changes
    F32 mYaw = 0.5f;          // the middle of the swing (a drag moves it); 0 = side on, the train's +X to the right
    F32 mPhase = 0.f;         // where in its swing the picture is (draw)
    bool mDragging = false;
    S32 mLastX = 0;
    LLFrameTimer mTurn;
    std::string mMessage;
    std::vector<LLVector3> mProj;
    std::vector<std::pair<F32, U32>> mOrder;
};

/**
 * [TRAIN BUILDER 2026-11-14] Paul (13 Nov): "the same as boats and planes have a whole range of trains and a customiser to build
 * trains"; "any trains you like, steam, diesels, bullet trains, shunters". 14 Nov: residents' trains carry the member's OWN logo
 * and company lettering, never the grid's.
 * Persona: a Wolf Territories member, 35-65, who has laid a bit of track and wants a train of their own; never built a vehicle.
 * Story: "I pick a kind of train, choose how many coaches, give it my colours, name, number, company and logo, see it turning
 * and what it costs against my credits, press Build, and soon it is in my inventory with a box that puts it on my track."
 * Flow, as the Plane Builder's (turntable and figures left; right, top to bottom): 1. kind of train 2. carriages (how many,
 * which style) 3. make it yours (livery and colours, name, number, company, logo, who may drive, top speed) 4. the price
 * line by line against the credits you have (Buy credits... when short); Build bottom right; Cancel the order bottom left while
 * it waits. Every failure is a line in the caution colour, kept until the next action; a field's own problem is shown under it.
 * The yard (wolfstorm php/train_builder.php) checks the account (members only), re-checks the design and the words, holds the
 * price and charges it only once the train is in the inventory. Wolf Territories only: the menu items are hidden elsewhere,
 * and opened off Wolf the window says so and closes.
 */
class WolfFloaterTrainBuilder : public LLFloater
{
public:
    explicit WolfFloaterTrainBuilder(const LLSD& key);
    ~WolfFloaterTrainBuilder() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void setStatus(const std::string& text, bool error);
    void onOpenReply(const LLSD& reply);
    void onClassChanged();
    void onLiveryChanged();
    void onStyleChanged();
    void onColourChanged();
    void onDesignChanged();
    void onChooseLogo();
    void onLogoFilePicked(const std::vector<std::string>& filenames);
    void onRemoveLogo();
    void onBuild();
    void onCancelOrder();
    void showOrder(const LLSD& order);
    void updatePrice();
    void updateFigures();
    void updateControls();
    std::string textProblem(const std::string& text, bool required, S32 max_chars) const;
    std::string numberProblem() const;
    std::string logoProblem() const;
    LLSD design() const;
    const LLSD* currentClass() const;
    const LLSD* currentLivery() const;
    const LLSD* currentStyle() const;
    bool hasSecond() const;
    S32 carriages() const;

    void request(const std::string& what, const LLSD& body);
    static void requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body);
    void onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error);

    LLComboBox *mType = nullptr, *mLivery = nullptr, *mStyle = nullptr;
    LLSliderCtrl *mCarriages = nullptr, *mSpeed = nullptr;
    LLColorSwatchCtrl *mMain = nullptr, *mSecond = nullptr, *mLower = nullptr, *mUpper = nullptr;
    LLLineEditor *mName = nullptr, *mNumber = nullptr, *mCompany = nullptr;
    LLCheckBoxCtrl *mPublic = nullptr, *mLogoOk = nullptr;
    LLTextBox *mFigures = nullptr, *mCarriagesLabel = nullptr, *mNameProblem = nullptr, *mNumberProblem = nullptr,
              *mCompanyProblem = nullptr, *mLogoText = nullptr, *mPrice = nullptr, *mBalance = nullptr, *mStatus = nullptr;
    LLButton *mLogoBtn = nullptr, *mLogoRemoveBtn = nullptr, *mBuildBtn = nullptr, *mBuyBtn = nullptr, *mCancelBtn = nullptr;
    WolfTrainPreview* mPreview = nullptr;

    LLSD mCatalogue;
    LLSD mPriceRules;
    S32 mAvailable = -1;
    S32 mBalanceNow = -1;
    S32 mPriceNow = 0;
    bool mReady = false;
    bool mBusy = false;
    bool mLogoBusy = false;
    bool mSettingColours = false;            // the swatches are being set from a livery or style: not "custom"
    std::string mRefused;
    bool mOpenFailed = false;
    std::string mOrder;
    std::string mOrderState;
    LLFrameTimer mPoll;
    bool mPolling = false;
    std::string mPreviewClass;
    std::string mLogo;                        // the yard's re-encoded logo (base64 PNG), "" for none
    std::string mLogoName;                    // the file it came from, for the line under the button
    static std::map<std::string, std::shared_ptr<const WolfTrainPreview::Mesh>> sMeshes;
};

#endif // WOLF_FLOATERTRAINBUILDER_H
