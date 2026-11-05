/**
 * @file wolffloaterskylevels.h
 * @brief WolfViewer: Sky Land window - list, add and remove a region's sky levels and shape the ground where you stand
 *        (ticket 541).
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
#ifndef WOLF_FLOATERSKYLEVELS_H
#define WOLF_FLOATERSKYLEVELS_H

// [WOLF SKY LEVELS 2026-11-05] The same window as WolfStorm's js/ui/floaters/floater_wolf_sky_levels.js: the same sections,
// words and rules. Persona: a region owner, maybe older, who wants an island in the sky over their region without learning a
// tool. Story: "I add a level at 200 m, fly up, press Make ground here, and walk on it." Flow: Add a level (estate owner and
// managers only) -> fly up -> Make ground here -> Raise / Lower / Flatten / Smooth -> Undo my last change if it went wrong.
// Every change says what happened on the status line; a refusal stays there, in the caution colour, until the next action.

#include "llfloater.h"

#include <string>

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLViewerRegion;
class WolfTerrainLevels;

class WolfFloaterSkyLevels : public LLFloater
{
public:
    explicit WolfFloaterSkyLevels(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    WolfTerrainLevels* sky() const;
    void refreshUI();
    void say(const std::string& text, bool failure);
    void act(const LLSD& body, const std::string& done_text);
    void onAdd();
    void onRemoveLevel();
    void onShape(const std::string& tool);
    void onRemoveHere();
    bool here(LLVector3& pos) const;
    S32 chosenLevel() const;

    LLTextBox* mStatus = nullptr;
    LLTextBox* mNone = nullptr;
    LLTextBox* mLimits = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLButton* mRemoveLevel = nullptr;
    LLLineEditor* mName = nullptr;
    LLSpinCtrl* mBase = nullptr;
    LLButton* mAdd = nullptr;
    LLComboBox* mLevel = nullptr;
    LLComboBox* mSize = nullptr;
    U32 mSeenChange = 0;
    LLViewerRegion* mSeenRegion = nullptr;
    bool mBusy = false;
};

#endif // WOLF_FLOATERSKYLEVELS_H
