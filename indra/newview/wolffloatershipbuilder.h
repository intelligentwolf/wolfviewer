/**
 * @file wolffloatershipbuilder.h
 * @brief WolfViewer: Wolf World > Build & Land > Ship Builder... - design a sailing ship and have the shipyard build her.
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

#ifndef WOLF_FLOATERSHIPBUILDER_H
#define WOLF_FLOATERSHIPBUILDER_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llview.h"

class LLButton;
class LLColorSwatchCtrl;
class LLComboBox;
class LLLineEditor;
class LLRadioGroup;
class LLSliderCtrl;
class LLTextBox;

/**
 * [SHIP BUILDER 2026-10-29] The turntable: the hull type's low-detail mesh (the shipyard's preview_<class>.json, made by
 * region-terrain-work/ship-builder-2026-10-29/worker/make_preview_meshes.py), turning slowly, painted in the member's own
 * hull / trim / sail colours. Drag to turn her by hand. Drawn flat-shaded with the painter's algorithm (far triangles
 * first), solid parts with their back faces culled; there is no depth buffer in the UI pass.
 */
class WolfShipPreview : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params> {};
    explicit WolfShipPreview(const Params& p);

    struct Tri { U32 a, b, c; U8 group; S16 gun; };     // group: the colour group (Group in the .cpp)
    /** One hull type's preview mesh, parsed once and kept for the session (a few hundred kB each). */
    struct Mesh
    {
        std::vector<LLVector3> verts;           // model metres: bow +X, port +Y, Z up (the generator's frame)
        std::vector<Tri> tris;
        std::vector<std::string> gunNames;      // the class's gun prims, by index (Tri::gun)
        LLVector3 centre;
        F32 radius = 1.f;                       // half her largest extent at 100 %
    };
    /** A preview_<class>.json reply ({v, t, guns, groups}) -> a mesh, or null if it cannot be read. */
    static std::shared_ptr<const Mesh> parse(const LLSD& preview);

    void setMesh(std::shared_ptr<const Mesh> mesh);
    bool hasMesh() const { return (bool)mMesh; }
    /** The member's choices: colours (0..1), the guns kept (prim names), the figurehead ("gilt", "timber", "none"),
     *  her length as a share of the class's own (0.9 .. 1.25). */
    void setLook(const LLColor4& hull, const LLColor4& trim, const LLColor4& sail, const std::set<std::string>& guns,
                 const std::string& figurehead, F32 scale);
    void setMessage(const std::string& text) { mMessage = text; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    std::shared_ptr<const Mesh> mMesh;
    std::vector<bool> mGunShown;
    LLColor4 mHull, mTrim, mSail;
    std::string mFigurehead = "gilt";
    F32 mScale = 1.f;
    F32 mYaw = 0.6f;                        // radians
    bool mDragging = false;
    S32 mLastX = 0;
    LLFrameTimer mTurn;
    std::string mMessage;
    // per-frame scratch, kept to avoid allocating every frame
    std::vector<LLVector3> mProj;
    std::vector<std::pair<F32, U32>> mOrder;
};

/**
 * [SHIP BUILDER 2026-10-29] Paul: "in our build thing in wolfviewer could we make a ship builder?" ... "okay anyone can use
 * it but only wolf grid members"; then "each ship build must cost AI credits" and "ships should cost more if they have
 * more powerful cannons".
 * Persona: a Wolf Territories member who wants a pirate ship of their own and has never built one.
 * Story: "I pick a hull, make her mine - size, guns, colours, figurehead, her name on the stern - see what she costs
 * against my credits, press Build, and a minute or two later she is in my inventory, ready to sail."
 * Flow (top to bottom on the right, the turntable on the left): 1. hull type (her figures under the picture) 2. length
 * and guns 3. colours, figurehead, name 4. the price, line by line, against the credits you have (Buy credits... when
 * short) 5. Build (primary, bottom right) 6. the order's progress, then where she is. Every failure is a line in the
 * caution colour in the window, kept until the next action.
 * The shipyard (wolfstorm php/ship_builder.php) checks the account (Wolf Territories members only - a hypergrid visitor is
 * refused), re-checks the design, holds the price, builds her with the pirate-ship generator and charges her price only
 * once she is in the inventory (nothing is charged when building fails). Wolf Territories only: the menu item is gone
 * elsewhere, and opened off Wolf the window says so and closes.
 */
class WolfFloaterShipBuilder : public LLFloater
{
public:
    explicit WolfFloaterShipBuilder(const LLSD& key);
    ~WolfFloaterShipBuilder() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    /** Confirmations quietly, failures in the caution colour, kept until the next action. */
    void setStatus(const std::string& text, bool error);
    void onOpenReply(const LLSD& reply);
    void onClassChanged();
    void onDesignChanged();
    void onBuild();
    void onCancelOrder();
    void showOrder(const LLSD& order);
    void updatePrice();
    void updateControls();
    /** Inline check of the name (shipbuilder_lib.php sb_check_design rules); empty when it is fine. */
    std::string nameProblem() const;
    LLSD design() const;
    const LLSD* currentClass() const;

    void request(const std::string& what, const LLSD& body);
    static void requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body);
    void onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error);

    LLComboBox *mHullType = nullptr, *mFigurehead = nullptr;
    LLSliderCtrl* mLength = nullptr;
    LLRadioGroup* mGuns = nullptr;
    LLColorSwatchCtrl *mHullColour = nullptr, *mTrimColour = nullptr, *mSailColour = nullptr;
    LLLineEditor* mName = nullptr;
    LLTextBox *mFigures = nullptr, *mNameProblem = nullptr, *mPrice = nullptr, *mBalance = nullptr, *mStatus = nullptr;
    LLButton *mBuildBtn = nullptr, *mBuyBtn = nullptr, *mCancelBtn = nullptr;
    WolfShipPreview* mPreview = nullptr;

    LLSD mCatalogue;                         // {classes: [...], length_pct, figureheads, name_max}
    LLSD mPriceRules;                        // shipbuilder_lib.php SB_PRICE
    S32 mAvailable = -1;                     // credits the member may spend now (live, from the shipyard); -1 unknown
    S32 mBalanceNow = -1;
    S32 mPriceNow = 0;
    bool mReady = false;                     // the shipyard answered "open"
    bool mBusy = false;                      // a build request is in flight
    std::string mRefused;                    // the shipyard refused the account (not a member): everything stays off
    bool mOpenFailed = false;                // "open" failed for another reason: asked again when the window is reopened
    std::string mOrder;                      // the member's order still waiting or being built
    std::string mOrderState;
    LLFrameTimer mPoll;
    bool mPolling = false;
    std::string mPreviewClass;               // the class the turntable holds or is fetching
    static std::map<std::string, std::shared_ptr<const WolfShipPreview::Mesh>> sMeshes;   // by class, for the session
};

#endif // WOLF_FLOATERSHIPBUILDER_H
