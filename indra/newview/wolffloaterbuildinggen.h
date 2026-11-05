/**
 * @file wolffloaterbuildinggen.h
 * @brief WolfViewer: the Building Generator - describe a building in words; the Wolf yard plans it, shows it, and builds it
 *        into your inventory.
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

#ifndef WOLF_FLOATERBUILDINGGEN_H
#define WOLF_FLOATERBUILDINGGEN_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llview.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLScrollContainer;
class LLTextBox;
class LLTextEditor;

/**
 * [BUILDING GENERATOR 2026-11-01] The plan's turntable. Copied from WolfPlanePreview (wolffloaterplanebuilder.cpp), with the
 * Building Generator's preview format instead of the plane's: region-terrain-work/building-generator-2026-11-01/README.md
 * "Viewer contract" preview {v, t, palette, size_m} - v in centimetres (X east, Y north, Z up, origin the centre of the
 * footprint on the ground), t = 4 ints per triangle a,b,c,colour, colour indexing palette [[r,g,b], ...], at most 30,000
 * triangles. Flat-shaded with the painter's algorithm, as the plane's (no depth buffer in the UI pass). Drag to turn it.
 */
class WolfBuildingPreview : public LLView
{
public:
    // its own Params (never another widget's: lluictrlfactory.cpp registerWidget, memory viewer-install-must-pass-startup-check)
    struct Params : LLInitParam::Block<Params, LLView::Params> { Params() {} };
    explicit WolfBuildingPreview(const Params& p);

    struct Tri { U32 a, b, c; U16 colour; };
    struct Mesh
    {
        std::vector<LLVector3> verts;           // metres: X east, Y north, Z up
        std::vector<Tri> tris;
        std::vector<LLColor4> palette;
        LLVector3 centre;
        F32 radius = 1.f;
    };
    // nullptr when the data breaks any rule of the contract (then the window says the preview could not be drawn)
    static std::shared_ptr<const Mesh> parse(const LLSD& preview);

    void setMesh(std::shared_ptr<const Mesh> mesh) { mMesh = std::move(mesh); }
    bool hasMesh() const { return (bool)mMesh; }
    void setMessage(const std::string& text) { mMessage = text; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    std::shared_ptr<const Mesh> mMesh;
    F32 mYaw = 0.6f;
    bool mDragging = false;
    S32 mLastX = 0;
    LLFrameTimer mTurn;
    std::string mMessage;
    std::vector<LLVector3> mProj;
    std::vector<std::pair<F32, U32>> mOrder;
};

/**
 * [BUILDING GENERATOR 2026-11-01] Paul: "anyone can design a building 'please make me a huge castle' 'please make me a row of
 * shops'", "make it for medieval times? or the second world war france", "i could find the house/building builder under the
 * build menu??".
 * Persona: a Wolf Territories member who wants a building for their land and has never built one.
 * Story: "I say what I want in my own words, pick a period or let it decide, see what designing it costs, press Plan it, look
 * at the turning picture and its land impact, and press Build it to have it put in my inventory."
 * Flow (preview and the plan's words on the left; on the right, top to bottom): 1. the request 2. the style (its description
 * scrolls in a fixed box) 3. the options and the design cost 4. the price to build it line by line, with the yard's
 * price note, against the credits you have; Plan it and Build it bottom right (always on show: the body above scrolls when the screen is short, wolffloaterfit.h), Buy credits... beside them when short, Cancel bottom left
 * while a job waits. The status line above the buttons: progress, or a failure in the caution colour, kept until the next
 * action. Nothing is ever charged without a press of Plan it or Build it, each showing its figure first.
 * Price (README "Price", Paul 2026-11-01): every order costs what the AI costs to design it plus 10%, at least 1 credit,
 * quoted before Plan it and never more than the quote; building and delivery cost nothing (no AI runs in it) and nothing is
 * charged for land or size. The yard (wolfstorm php/building_gen.php) checks the account, holds the quote and charges the
 * actual when the plan is ready. Wolf Territories only: the menu items are gone elsewhere, and opened off Wolf the window says
 * so and closes.
 */
class WolfFloaterBuildingGen : public LLFloater
{
public:
    explicit WolfFloaterBuildingGen(const LLSD& key);
    ~WolfFloaterBuildingGen() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void setStatus(const std::string& text, bool error);
    void onOpenReply(const LLSD& reply);
    void readCredits(const LLSD& reply);
    void onRequestEdited();
    void onStyleChanged();
    void onPlan();
    void onBuild();
    void onCancelJob();
    void onBuyCredits();
    void showJob(const LLSD& job);
    void showPlan(const LLSD& job, bool fromLastTime);
    void clearPlan();
    void showQuote();
    void showPriceIdle();
    std::string credits(S32 n) const;
    void updateCredits();
    void updateControls();
    std::string requestText() const;
    std::string styleId() const;
    std::string quoteKey() const;
    LLSD options() const;

    void request(const std::string& what, const LLSD& body);
    static void requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body);
    void onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error);

    LLTextEditor *mRequest = nullptr, *mSources = nullptr, *mPlanText = nullptr, *mPrice = nullptr, *mStyleInfo = nullptr,
                 *mQuote = nullptr;
    LLComboBox* mStyle = nullptr;
    std::vector<LLCheckBoxCtrl*> mOptionBoxes;
    std::vector<std::string> mOptionIds;     // the catalogue option each box stands for ("" = box not used)
    LLTextBox *mCounter = nullptr, *mAccess = nullptr, *mPlanTitle = nullptr,
              *mBudget = nullptr, *mSourcesLabel = nullptr, *mBalance = nullptr, *mStatus = nullptr;
    LLButton *mPlanBtn = nullptr, *mBuildBtn = nullptr, *mBuyBtn = nullptr, *mCancelBtn = nullptr;
    WolfBuildingPreview* mPreview = nullptr;
    // [TICKET 529 PATTERN 2026-11-05] the body scrolls; the window is kept inside the screen (wolffloaterfit.h)
    LLScrollContainer* mScroll = nullptr;
    S32 mNaturalHeight = 0;

    LLSD mCatalogue;
    std::string mBuyUrl;
    S32 mAvailable = -1;
    S32 mBalanceNow = -1;
    bool mAccessPlan = false, mAccessBuild = false;
    std::string mAccessReason;
    bool mReady = false;
    bool mBusy = false;                      // open / plan / build / cancel in flight
    bool mOpenFailed = false;
    std::string mRefused;
    bool mShortOf = false;                   // the yard said credits_short since the last change

    // the quote: what was asked for (request text + style), and what the member is shown
    LLFrameTimer mQuoteTimer;
    bool mQuoteDue = false;
    bool mQuoting = false;
    bool mPlanAfterQuote = false;
    std::string mQuoteFor;
    S32 mQuoteTypical = -1, mQuoteAtMost = -1;
    bool mQuoteResearch = false;
    // the yard's own price sentence (open price_rules.note, then quote.note), shown in 4. Price to build it
    std::string mPriceNote;

    // the job being followed (plan or build), polled every 5 s while it is queued or running
    std::string mJob, mJobKind, mJobState;
    LLFrameTimer mPoll;
    bool mPolling = false;

    // the finished plan on show (the plan JOB's uuid is what build sends)
    std::string mPlanId;
    S32 mPlanPrice = -1;
};

#endif // WOLF_FLOATERBUILDINGGEN_H
