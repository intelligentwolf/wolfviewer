/**
 * @file wolffloateraimode.h
 * @brief WolfViewer: Wolf World > AI Mode: Personality and Voice... - the minimum AI Mode needs (wolfaimode.h).
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

#ifndef WOLF_FLOATERAIMODE_H
#define WOLF_FLOATERAIMODE_H

#include "llfloater.h"
#include "llsd.h"

// [AI MODE 2026-11-05] Who and why: a resident (often older, sometimes with poor sight - memory paul-simple-for-older-disabled-
// users) turning AI Mode on for the first time gives their avatar a personality and a voice, and nothing else. Flow, top to
// bottom: what AI Mode does and what it costs / pick a ready personality or write your own / pick a voice / Save and turn on.
// Large text, no time limits, no codes. Opened by the menu the first time (key turn_on = true: saving turns AI Mode on) and
// from Wolf World > AI Mode: Personality and Voice... to change them later.
class WolfFloaterAIMode : public LLFloater
{
public:
    WolfFloaterAIMode(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void load();
    void onPreset();
    void onSave();
    void setStatus(const std::string& text, bool error);

    LLSD mPresets;
    bool mTurnOn = false;
    bool mBusy = false;
};

#endif // WOLF_FLOATERAIMODE_H
