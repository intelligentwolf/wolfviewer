/**
 * @file wolfborder.h
 * @brief WolfViewer: border warnings - what the region ahead will do to you and your vehicle, before you get there,
 *        and the hop across a region that refuses it (WolfSim WolfBorderModule).
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

#ifndef WOLF_BORDER_H
#define WOLF_BORDER_H

#include <map>
#include <string>
#include <vector>

#include "llsingleton.h"
#include "lluictrl.h"
#include "lluuid.h"
#include "v3dmath.h"

// [BORDER GUARD 2026-10-16] Paul: "If someone has disabled their scripts on the region, the vehicles crash. Likewise,
// if they don't allow entry, we need a way of warning the user", then "can't we transport the person to the other
// side of the region?" ... "with their viewer".
//
// Who and what: a driver, pilot or skipper (or someone walking) heading for a region border wants to know a few
// seconds before it what the region ahead will do - refuse them, or stop their vehicle's scripts - and what will
// happen instead, without taking their eyes off the way ahead.
//
// The split (WolfSim OptionalModules/World/WolfBorder/WolfBorderModule.cs has the wire format):
//  - On a Wolf region with the module (it answered "hello"): every 2 s while heading for a border the viewer asks
//    "look"; the region answers what is ahead (refused / scripts off / no entry), the plan (go, over = this region
//    carries an aircraft or boat over it, hop, stop, walk) and where a hop comes out. At the border the viewer asks
//    "hop" - the region alone decides and checks everything and moves the vehicle (TeleportObject). A held vehicle
//    ("event=held") is told why.
//  - Anywhere else (another grid, a region without the module): what stock OpenSim tells the viewer - a neighbour's
//    RegionHandshake flags (REGION_FLAGS_SKIP_SCRIPTS, llregionflags.h), the refusal alerts of a walker's crossing
//    (EntityTransferModule GetDestination's "Access Denied" texts), and a vehicle that came to a stop at the edge.
//    A walker refused there is offered an ordinary teleport to the far side; a vehicle is warned and stops.
class WolfBorder : public LLSingleton<WolfBorder>
{
    LLSINGLETON(WolfBorder);
    ~WolfBorder() override;

public:
    enum ELevel { L_NONE = 0, L_INFO, L_CAUTION, L_WARNING };

    struct Banner
    {
        ELevel mLevel = L_NONE;
        std::string mTitle;        // "AHEAD: Hobbit's Haven"
        std::string mReason;       // why
        std::string mOutcome;      // what will happen
        F32 mDist = -1.f;          // metres to the border, < 0 = not shown
        bool mOfferTeleport = false;
        std::string mTeleportLabel;
    };

    /** The banner to draw now (WolfBorderBanner::draw). */
    const Banner& banner() const { return mBanner; }

    /** A teleport to the far side is on offer (a walker refused) - the banner's button and Wolf World's item. */
    bool canTeleportAcross() const { return mBanner.mOfferTeleport && mHasOffer; }
    void teleportAcross();

    /** llviewermessage.cpp process_alert_core: every alert, before anything else sees it; never swallows one. */
    void onAlert(const std::string& message);

    /** The region's "WolfBorder" GenericMessage. */
    void receive(const LLUUID& invoice, const std::vector<std::string>& strings);

    static void onIdle(void*);

private:
    void idle();
    void sendLook(bool force);
    void sendHop(const std::string& why);
    void setBanner(const Banner& b, const std::string& announceKey, F32 dist);
    void setEvent(const Banner& b, F64 seconds);
    bool rebuild(S32 kind, F64 a, F64 b, bool hop, bool seated);
    void clearBanner();
    void announce(const Banner& b);

    // the server's last answer (a Wolf region with the module)
    struct Look
    {
        F64 mAt = 0.0;
        std::string mState, mPlan, mAhead, mWhy, mTo;
        F64 mRX = 0, mRY = 0, mRW = 0, mRH = 0;
        LLVector3d mTarget;        // tx ty tz: the far side (a walker's teleport offer)
        bool mHasTarget = false;
    };

    LLUUID mModuleRegion;          // the region that answered "hello" (has the module)
    LLUUID mHelloRegion;           // the region "hello" was last sent to
    F64 mHelloAt = 0.0;
    F64 mLookSentAt = 0.0;
    F64 mHopSentAt = 0.0;
    Look mLook;

    // a held vehicle / a hop / a refusal shown for a while after it happened
    F64 mEventUntil = 0.0;
    F64 mEventSince = 0.0;
    Banner mEventBanner;
    std::string mEventKey;         // its announce key, made once by setEvent (not each frame it is shown)

    // The banner idle() shows from the region's look or what the viewer knows itself, built only when what it says
    // changes (rebuild), not every frame; the distance is set apart (setBanner).
    struct Built
    {
        S32 mKind = 0;             // 0 none, 1 the region's look, 2 refused lately, 3 scripts off ahead
        F64 mA = 0.0, mB = 0.0;    // what it was built from: the look's time / the region's south-west corner
        bool mHop = false, mSeated = false;
        Banner mBanner;
        std::string mKey;          // the announce key
    };
    Built mBuilt;

    // off-grid: regions that refused us lately (the alert) - origin + size, until when
    struct Refused { LLVector3d mMin, mMax; std::string mName; F64 mUntil = 0.0; };
    std::vector<Refused> mRefused;

    // off-grid: a vehicle stopped at the edge
    F64 mStoppedSince = 0.0;
    LLVector3d mLastPos;
    bool mWasApproaching = false;

    LLVector3d mTeleportOffer;     // the far side, global metres (mHasOffer)
    bool mHasOffer = false;
    bool mOfferFromLook = false;   // the offer is the region's target (checked again on the click), not the viewer's own
    Banner mBanner;
    std::string mAnnounced;        // the last banner told to chat / the voice, so it is said once
    std::map<std::string, F64> mAnnouncedAt;   // ... and the same words not again within ANNOUNCE_AGAIN_S
    LLUUID mLastRegion;            // a region change forgets the old region's answers
};

// The banner, drawn over the world under the floaters (main_view.xml wolf_border_banner), in the style of the
// flight deck's alerts: a dark panel, a coloured bar and a word for the level (never colour alone), large text.
class WolfBorderBanner : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };

    WolfBorderBanner(const Params& p);
    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

private:
    LLRect mPanel;                 // last drawn, for clicks
    LLRect mButton;                // the teleport button, empty when none
    S32 mDistUnits = -2;           // the distance as last shown (distanceUnits) ...
    std::string mDistText;         // ... and its text, made again only when it changes
};

#endif // WOLF_BORDER_H
