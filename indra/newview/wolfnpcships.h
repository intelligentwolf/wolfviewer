/**
 * @file wolfnpcships.h
 * @brief WolfViewer: the WolfNPC window's NPC ships tab (the region's wolf-naval pirate and peaceful ships).
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

#ifndef WOLF_NPC_SHIPS_H
#define WOLF_NPC_SHIPS_H

#include <functional>
#include <memory>
#include <string>

#include "llsd.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLPanel;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;

/**
 * [NPC SHIPS 2026-11-01] Paul: "we need to add npc ships to our wolfnpc options because people should be able to set this
 * up on their own regions". The contract: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md §30 (ops hello, get,
 * set, haven_add, haven_remove, clear; the state of §30.4). Requests go through WolfNPC::requestShips (chat on channel 73);
 * the region's wolf-naval plugin answers ("WolfNaval"). Only the region owner or an estate manager gets more than hello.
 * Read when the tab is shown, on Refresh and after each change - never on a timer (paul-think-of-scale).
 */
class WolfNPCShips
{
public:
    typedef std::function<void(const std::string& text, const std::string& yes, std::function<void()> go)> ask_fn;
    typedef std::function<void(const std::string& text)> say_fn;

    WolfNPCShips(LLPanel* panel, ask_fn ask, say_fn error, say_fn done);

    /** The tab is shown: hello (once a region), then the state. */
    void onShown();
    /** Another region: everything known is the old region's. */
    void onRegionChanged();

private:
    void sendHello();
    void requestState();
    /** A `ships` reply (§30.4): the switch, the numbers, the chart line, the havens and the ships. */
    void applyState(const LLSD& reply);
    void showNote(const std::string& text, bool problem);
    void fillControls();
    void fillHavens();
    void fillShips();
    void updateTotals();
    void onSwitch();
    void onSave();
    void onHavenAdd();
    void onHavenRemove();
    void onClear();
    /** Send `op`; a `ships` reply refreshes the tab; `done_words` on success, the region's words on a failure. */
    void send(const std::string& op, const LLSD& fields, const std::string& done_words, std::function<void(bool ok)> after = nullptr);

    LLPanel* mPanel;
    ask_fn mAsk;
    say_fn mError, mDone;
    /** Replies come back after the window (and this) may be gone: each callback holds a weak_ptr to this. */
    std::shared_ptr<int> mLife = std::make_shared<int>(0);
    U32 mSerial = 0;          // bumped on a region change: an older reply is dropped
    bool mHelloDone = false;  // hello answered on this region
    bool mReading = false;    // a hello or get is on its way: no second one
    bool mHolder = false;
    bool mDirty = false;      // the numbers, hunters or kinds were changed and not saved yet
    S32 mMax = 12;
    LLSD mHello, mState;

    LLTextBox* mNote = nullptr;
    LLPanel* mBody = nullptr;
    LLCheckBoxCtrl* mOn = nullptr;
    LLTextBox* mChart = nullptr;
    LLSpinCtrl* mPirates = nullptr;
    LLSpinCtrl* mTraders = nullptr;
    LLTextBox* mTotal = nullptr;
    LLCheckBoxCtrl* mHunters = nullptr;
    LLCheckBoxCtrl* mAmbient = nullptr;   // §30.4 cfg.ambient: passing traffic (§31)
    LLTextBox* mHuntersNote = nullptr;
    LLButton* mMore = nullptr;
    LLPanel* mClasses = nullptr;
    LLCheckBoxCtrl* mClass[4] = { nullptr, nullptr, nullptr, nullptr };
    std::string mClassId[4];
    LLLineEditor* mHavenName = nullptr;
    LLComboBox* mHavenSize = nullptr;
    LLScrollListCtrl* mHavens = nullptr;
    LLScrollListCtrl* mShips = nullptr;
    LLTextBox* mAwake = nullptr;
};

#endif // WOLF_NPC_SHIPS_H
