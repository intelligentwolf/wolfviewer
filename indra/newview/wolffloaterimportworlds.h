/**
 * @file wolffloaterimportworlds.h
 * @brief WolfViewer: Build > Import worlds... - bring in a Minecraft build, a Roblox place or a MagicaVoxel model.
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

#ifndef WOLF_FLOATER_IMPORT_WORLDS_H
#define WOLF_FLOATER_IMPORT_WORLDS_H

#include <memory>
#include <string>

#include "llfloater.h"
#include "wolfimportflow.h"

class LLButton;
class LLCheckBoxCtrl;
class LLTextBox;
class LLTextEditor;

/**
 * [IMPORT WORLDS 2026-10-18] GAME_CARD_SPINE_SPEC.md "Change (Paul, 2026-10-18 ~15:20Z): Build -> Import worlds...". Three big
 * choices (Minecraft build, Roblox place, MagicaVoxel model), the cost on the one primary button, progress, the report and the
 * amber warnings, where the box went, and for a Roblox place "Also make it a game" then "Set up your game" (the Adventure
 * Studio on that game). The flow is the Studio's own (WolfImportFlow), kept while the window is closed so an import going on
 * is shown again when it reopens. Wolf Territories only.
 */
class WolfFloaterImportWorlds : public LLFloater
{
public:
    WolfFloaterImportWorlds(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refresh();
    void onPrimary();
    void onCancel();
    void onSetUp();

    /** One flow for this window, kept across opening and closing it. */
    static std::shared_ptr<WolfImportFlow>& flow();

    LLButton* mChoice[3] = { nullptr, nullptr, nullptr };
    LLButton* mPrimary = nullptr;
    LLButton* mCancel = nullptr;
    LLButton* mBuy = nullptr;
    LLCheckBoxCtrl* mMakeGame = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mError = nullptr;
    LLTextEditor* mReport = nullptr;
    std::string mShown;   // what the widgets show now (refresh only changes them when it changes)
};

#endif // WOLF_FLOATER_IMPORT_WORLDS_H
