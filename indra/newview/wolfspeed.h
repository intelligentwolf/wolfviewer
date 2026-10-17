/**
 * @file wolfspeed.h
 * @brief WolfViewer: Xenon's WolfSpeed - walk, run and fly faster, as fast as the region allows
 *        (WolfSim WolfSpeedModule).
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

#ifndef WOLF_SPEED_H
#define WOLF_SPEED_H

#include <string>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llsingleton.h"
#include "lluuid.h"

// [WOLFSPEED 2026-10-17] Paul: "add a WolfSpeed feature to the viewers, a floater that allows users to go at super
// speed". Named "Xenon's WolfSpeed" after the resident Xenon Darrow (Paul, 2026-10-17); identifiers stay WolfSpeed.
//
// Who and what: a resident on Wolf Territories wants to cross a big region fast - pick a speed in one click, see the
// speed they really get (and why, when the region caps or forbids it), and switch it off in one click.
//
// The split (WolfSim OptionalModules/World/WolfSpeed/WolfSpeedModule.cs has the wire format): the REGION applies the
// speed (ScenePresence.WolfSpeed, the multiplication osSetSpeed's SpeedModifier is part of), caps it, resets it on a
// teleport, on leaving the region and on sitting down, and stops a fast avatar at the edge of a parcel it may not
// enter. The viewer only asks ("hello", "set <x>", "region <0|1> <cap>") and shows what the region answers. After a
// crossing (the hello's answer says via=cross) it asks the new region for the same speed; after a teleport or login
// it does not (the speed goes off). Wolf Territories only, inert everywhere else.
class WolfSpeed : public LLSingleton<WolfSpeed>
{
    LLSINGLETON(WolfSpeed);
    ~WolfSpeed() override;

public:
    // The hard ceiling: ScenePresence.WolfSpeedCeiling (WolfSim ScenePresence.cs) - the dial's scale.
    static constexpr F32 CEILING = 6.f;

    enum ELevel { M_NONE = 0, M_OK, M_INFO, M_WARN, M_ERROR };
    struct Message
    {
        ELevel mLevel = M_NONE;
        std::string mWord;         // the level in a word (never colour alone): ON, LIMIT, OFF HERE, ...
        std::string mText;         // plain English
    };

    /** The region answered "hello" (it has the module) and its limits are known. */
    bool available() const;
    /** The person may change the region's settings (estate owner / manager, grid god). */
    bool canEditRegion() const { return available() && mEdit; }

    F32 speed() const { return mSpeed; }           // the region's WolfSpeed for us now
    F32 inForce() const { return mForce; }         // 1 while a script or game sets the speed (paused)
    F32 maxHere() const { return mMax; }           // the most allowed here now (1 = switched off)
    F32 gridMax() const { return mGrid; }
    F32 regionCap() const { return mRMax; }        // 0 = none below the grid's
    bool regionAllows() const { return mAllow; }
    bool paused() const { return mPaused; }
    bool waiting() const { return mSentAt > 0.0; } // a request is on its way
    F32 shownSpeed() const { return mQueued > 0.f ? mQueued : mSpeed; }

    const Message& message() const { return mMessage; }
    /** The window is being opened for a message (say): it does not take the keyboard. */
    bool openingQuietly() const { return mQuietOpen; }
    void dismissMessage() { mMessage = Message(); }

    /** The person picked a speed (1 = off). Sent at once, or coalesced with the next pick (SEND_EVERY_S). */
    void request(F32 x);
    /** Region owners: allow it here or not, and the cap (0 = the grid's maximum). */
    void requestRegion(bool allow, F32 cap);

    /** The region's "WolfSpeed" GenericMessage. */
    void receive(const LLUUID& invoice, const std::vector<std::string>& strings);

    /** Every frame, from LLAppViewer's world update (after WolfGame): the Wolf gate, the hello, queued picks. */
    void idle();

private:
    void forget();
    void sendSet(F32 x);
    void say(ELevel level, const std::string& word, const std::string& text, bool open_window);
    void gateToolbar(bool wolf);

    LLUUID mRegion;                // the region we are in (a change forgets the old region's answers)
    LLUUID mModuleRegion;          // the region that answered "hello"
    F64 mHelloAt = 0.0;
    F64 mHelloFirstAt = 0.0;
    bool mHelloSent = false;

    // the region's answer
    F32 mSpeed = 1.f, mForce = 1.f, mMax = 1.f, mGrid = 1.f, mRMax = 0.f;
    bool mAllow = false, mEdit = false, mPaused = false;

    F32 mWanted = 1.f;             // what the person picked (asked again after a crossing)
    F32 mQueued = 0.f;             // a pick waiting for SEND_EVERY_S (0 = none)
    F64 mLastSendAt = -1e9;
    F64 mSentAt = 0.0;             // a set on its way (cleared by the answer, or after a while)

    Message mMessage;
    bool mQuietOpen = false;
    S32 mOnWolf = -1;              // last gate state (-1 unknown)
    F64 mLastGate = 0.0;
};

// The window: drawn in code in the style of the flight deck and the car dashboard - a big speed dial, the presets,
// Off, what is in force and why, and (for region owners) the region's own switch and cap. Three looks to choose from
// (THEME: WolfSpeedTheme).
class WolfFloaterSpeed : public LLFloater
{
public:
    WolfFloaterSpeed(const LLSD& key);
    ~WolfFloaterSpeed() override = default;

    bool postBuild() override;
    void draw() override;
    void onOpen(const LLSD& key) override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    enum EHit { H_NONE = 0, H_DIAL, H_WALK, H_RUN, H_FAST, H_SUPER, H_OFF, H_THEME, H_DISMISS,
                H_REGION_ALLOW, H_CAP_DOWN, H_CAP_UP, H_REGION_SAVE, H_COUNT };
    EHit hitAt(S32 x, S32 y) const;
    void press(EHit h);
    F32 dialValueAt(S32 x, S32 y) const;
    F32 presetValue(EHit h) const;

    void drawDial(F32 cx, F32 cy, F32 r);
    void drawButton(EHit h, const std::string& label, const std::string& sub, bool lit, bool enabled);

    LLRect mHit[H_COUNT];          // where each control was drawn (draw), for the mouse
    F32 mDialCX = 0.f, mDialCY = 0.f, mDialR = 0.f;
    bool mDragging = false;
    F32 mDragValue = 1.f;
    EHit mHover = H_NONE;
    EHit mPressed = H_NONE;

    // the region owner's pending choice (sent with SAVE)
    bool mEditAllow = true;
    F32 mEditCap = 0.f;
    bool mEditDirty = false;

    F32 mNeedle = 1.f;             // eased towards the speed in force
    LLFrameTimer mFrameTimer;
};

#endif // WOLF_SPEED_H
