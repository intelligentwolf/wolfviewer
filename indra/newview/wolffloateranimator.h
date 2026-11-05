/**
 * @file wolffloateranimator.h
 * @brief WolfViewer: the Wolf Animator - describe a movement in words; the Wolf Animator makes it into an avatar animation
 *        and puts it in your inventory.
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

#ifndef WOLF_FLOATERANIMATOR_H
#define WOLF_FLOATERANIMATOR_H

#include <string>
#include <utility>
#include <vector>

#include "llfloater.h"
#include "llframetimer.h"
#include "llview.h"

class LLButton;
class LLComboBox;
class LLScrollContainer;
class LLTextBox;
class LLTextEditor;

/**
 * [WOLF ANIMATOR 2026-11-05] The moving stick figure of a delivered animation. Source:
 * region-terrain-work/wolf-animator-2026-11-05/README.md "Window contract" preview {fps, joints, lines, frames, loop} -
 * frames = per frame, x, y, z centimetres per joint (X forward, Y left, Z up, the floor at 0), lines = pairs of joint
 * indexes (wanim/check.py preview). Drawn as thick lines, the camera turning slowly round it; drag to turn it; the left
 * side gold, the right blue (the WolfStorm window's colours), said in words under it.
 */
class WolfAnimPreview : public LLView
{
public:
    // its own Params (never another widget's: lluictrlfactory.cpp registerWidget, memory viewer-install-must-pass-startup-check)
    struct Params : LLInitParam::Block<Params, LLView::Params> { Params() {} };
    explicit WolfAnimPreview(const Params& p);

    struct Figure
    {
        F32 fps = 12.f;
        S32 joints = 0;
        std::vector<S8> side;                       // +1 left, -1 right, 0 middle, per joint
        S32 head = -1;
        std::vector<std::pair<S32, S32>> lines;
        std::vector<std::vector<LLVector3>> frames; // metres
    };
    // false when the data breaks any rule of the contract (the window then says the preview could not be drawn)
    static bool parse(const LLSD& preview, Figure& out);

    void setFigure(const Figure& f) { mFigure = f; mHas = !f.frames.empty(); mClock.reset(); }
    void clearFigure() { mHas = false; mFigure = Figure(); }
    void setMessage(const std::string& text) { mMessage = text; }
    void setPlaying(bool on) { mPlaying = on; }
    bool playing() const { return mPlaying; }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    void onMouseCaptureLost() override;

private:
    Figure mFigure;
    bool mHas = false;
    bool mPlaying = true;
    F32 mYaw = 0.6f;
    F32 mTime = 0.f;
    bool mDragging = false;
    S32 mLastX = 0;
    LLFrameTimer mClock;
    std::string mMessage;
};

/**
 * [WOLF ANIMATOR 2026-11-05] Ticket 547 (Jimmy Olsen, approved by Paul): "a tool one could use AI credits to create animations
 * for avatar, like 'dancing samba, loop mode with hands in the air'". Paul 2026-11-05: "i'll test it in wolfviewer".
 * Persona: a Wolf Territories resident, often older or less confident with computers, who wants their avatar to dance or
 * wave and has never made an animation.
 * Story: "I say what my avatar should do in my own words (or pick an example), see what it costs, press Make my animation,
 * and a minute later it is in my inventory, with a moving picture of it; then I tell them how it went."
 * Flow, top to bottom: 1. the description (600 characters, counter under it) and an example list 2. what it costs and the
 * credits you have 3. once made: the moving figure on the left, its name, what it does and where it is on the right 4. How
 * was it? Great / OK / Please try again (free, once: their words in the box) / Something is wrong (the Flea Report window,
 * filled in); the box and Send for OK's comment or the try again. 1-4 scroll (body_scroll) when the screen is short
 * (ticket 529's rule, wolffloaterfit.h); the footer is always on show: the status line (progress, or a failure in the
 * caution colour, kept until the next action), Cancel bottom left while it waits, Buy credits... when short and Make my
 * animation bottom right. When an animation arrives the body scrolls to it and How was it?.
 * Price (README "Price and costs"): what the AI costs plus 10%, at least 1 credit, never more than the quote, charged only once
 * it is delivered; a failure or refusal charges nothing; the free try again charges nothing. The service
 * (wolfstorm php/animator.php) checks the account, holds the quote and charges on delivery. Wolf Territories only: the menu
 * items are gone elsewhere, and opened off Wolf the window says so and closes. Transport, gate and error words are the
 * Building Generator's (wolffloaterbuildinggen.cpp).
 */
class WolfFloaterAnimator : public LLFloater
{
public:
    explicit WolfFloaterAnimator(const LLSD& key);
    ~WolfFloaterAnimator() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    enum class Box { NONE, COMMENT, RETRY };

    void setStatus(const std::string& text, bool error);
    void onOpenReply(const LLSD& reply);
    void readCredits(const LLSD& reply);
    void onRequestEdited();
    void onExample();
    void onMake();
    void onCancelJob();
    void onBuyCredits();
    void onPlayPause();
    void onPlayOnMe();
    void stopOnMe();
    void onChoice(const std::string& rating);
    void onSendBox();
    void showJob(const LLSD& job, bool announce);
    void showResult(const LLSD& job);
    void clearResult();
    void showQuote();
    void showFeedback();
    void openFleaReport();
    void scrollToShow(const std::vector<LLView*>& views);
    std::string credits(S32 n) const;
    void updateCredits();
    void updateControls();
    std::string requestText() const;

    void request(const std::string& what, const LLSD& body);
    static void requestCoro(LLHandle<LLFloater> h, std::string what, LLSD body);
    void onReply(const std::string& what, const LLSD& body, bool ok, const LLSD& reply, const std::string& error);

    LLTextEditor *mRequest = nullptr, *mQuote = nullptr, *mResult = nullptr, *mBoxText = nullptr;
    LLComboBox* mExamples = nullptr;
    LLTextBox *mCounter = nullptr, *mAccess = nullptr, *mTitle = nullptr, *mBalance = nullptr, *mStatus = nullptr,
              *mFeedbackLabel = nullptr, *mBoxLabel = nullptr, *mKey = nullptr;
    LLButton *mMakeBtn = nullptr, *mBuyBtn = nullptr, *mCancelBtn = nullptr, *mPlayBtn = nullptr, *mSendBtn = nullptr,
             *mPlayOnMeBtn = nullptr;
    std::vector<LLButton*> mChoiceBtns;      // great, ok, retry, fix
    WolfAnimPreview* mPreview = nullptr;
    // [TICKET 529 PATTERN] the body scrolls; the window is kept inside the screen (wolffloaterfit.h)
    LLScrollContainer* mScroll = nullptr;
    S32 mNaturalHeight = 0;

    std::string mBuyUrl;
    S32 mAvailable = -1;
    S32 mBalanceNow = -1;
    bool mAccessMake = false;
    std::string mAccessReason;
    bool mReady = false;
    bool mBusy = false;                      // open / make / cancel / rate / retry in flight
    bool mOpenFailed = false;
    std::string mRefused;
    bool mShortOf = false;
    S32 mQuoteTypical = -1, mQuoteAtMost = -1;
    std::string mPriceNote;

    // the job being followed, polled every 5 s while it is queued or running
    std::string mJob, mJobState;
    LLFrameTimer mPoll;
    bool mPolling = false;

    // the delivered job on show, and what "How was it?" may still offer
    LLSD mShown;
    bool mCanRate = false, mCanRetry = false;
    Box mBox = Box::NONE;
    // "Play it on my avatar": the delivered item's animation started in-world (as Play in world from the inventory), and
    // stopped again by the same button or when the window closes
    LLUUID mPlayingAsset;
};

#endif // WOLF_FLOATERANIMATOR_H
