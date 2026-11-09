/**
 * @file wolffloatervulkan.h
 * @brief Wolf World > Vulkan Renderer: which renderer draws the window, the Vulkan device found, status, debugging.
 *
 * [VULKAN BACKEND 2026-11-08] floater_wolf_vulkan.xml; the settings are WolfRenderBackend, WolfVulkanValidation
 * (wolfvkbridge.h). Paul: "any settings for vulcan should be in a floater attached to wolf world menu".
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATER_VULKAN_H
#define WOLF_FLOATER_VULKAN_H

#include "llfloater.h"

class LLRadioGroup;
class LLTextBox;

class WolfFloaterVulkan : public LLFloater
{
public:
    WolfFloaterVulkan(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void onChoice();
    void refresh();

    LLRadioGroup* mBackend = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mRendererLine = nullptr;
    LLTextBox* mVulkanLine = nullptr;
    LLTextBox* mFrameLine = nullptr;
    F32 mNextRefresh = 0.f;
};

#endif // WOLF_FLOATER_VULKAN_H
