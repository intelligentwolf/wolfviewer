/**
 * @file wolffloatervulkan.cpp
 * @brief Wolf World > Vulkan Renderer (see wolffloatervulkan.h).
 *
 * [VULKAN BACKEND 2026-11-08]
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

#include "llviewerprecompiledheaders.h"
#include "wolffloatervulkan.h"
#include "wolfvkbridge.h"
#include "llradiogroup.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llnotificationsutil.h"
#include "llframetimer.h"

WolfFloaterVulkan::WolfFloaterVulkan(const LLSD& key) : LLFloater(key) {}

bool WolfFloaterVulkan::postBuild()
{
    mBackend = getChild<LLRadioGroup>("backend");
    mStatus = getChild<LLTextBox>("status");
    mRendererLine = getChild<LLTextBox>("renderer_line");
    mVulkanLine = getChild<LLTextBox>("vulkan_line");
    mFrameLine = getChild<LLTextBox>("frame_line");
    mBackend->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChoice(); });
    return true;
}

void WolfFloaterVulkan::onOpen(const LLSD& key)
{
    mBackend->setValue(gSavedSettings.getString("WolfRenderBackend") == "vulkan" ? "vulkan" : "opengl");
    // the Vulkan choice is offered only where a Vulkan device was found; the status line says why otherwise
    const LLSD fields = WolfVkBridge::reportFields();
    mBackend->setIndexEnabled(1, fields["vulkan_available"].asBoolean());
    refresh();
}

void WolfFloaterVulkan::onChoice()
{
    // [V BUTTON 2026-11-08] the choice itself lives in WolfVkBridge::chooseBackend, shared with the navigation bar's V button
    WolfVkBridge::chooseBackend(mBackend->getValue().asString());
    refresh();
}

void WolfFloaterVulkan::refresh()
{
    mStatus->setText(WolfVkBridge::statusLine());
    mRendererLine->setText(getString("renderer_prefix") + WolfVkBridge::rendererLine());
    const LLSD fields = WolfVkBridge::reportFields();
    if (fields["vulkan_available"].asBoolean())
    {
        mVulkanLine->setText(getString("vulkan_prefix") + WolfVkBridge::vulkanLine());
    }
    else
    {
        mVulkanLine->setText(getString("vulkan_none"));
    }
    if (WolfVkBridge::isActive())
    {
        LLStringUtil::format_map_t args;
        args["[DRAWS]"] = llformat("%u", WolfVkBridge::sceneDrawsLastFrame());
        args["[MS]"] = llformat("%.1f", WolfVkBridge::presentMillis());
        args["[HANDOVER]"] = WolfVkBridge::handOverLine();
        args["[GBUFFER]"] = WolfVkBridge::gbufferShared() ? llformat("%u", WolfVkBridge::gbufferDrawsLastFrame()) : "0";
        std::string line = getString("frame_active", args);
        if (gSavedSettings.getBOOL("WolfVulkanValidation"))
        {
            LLStringUtil::format_map_t vargs;
            vargs["[COUNT]"] = llformat("%u", WolfVkBridge::validationMessageCount());
            line += " " + getString("frame_validation", vargs);
        }
        mFrameLine->setText(line);
    }
    else
    {
        mFrameLine->setText(getString("frame_idle"));
    }
}

void WolfFloaterVulkan::draw()
{
    const F32 now = static_cast<F32>(LLFrameTimer::getElapsedSeconds());   // [CI 2026-11-09] MSVC C4244 (double -> F32) is an error here (/WX), run 42603913776
    if (now >= mNextRefresh)
    {
        mNextRefresh = now + 1.f;   // the frame numbers once a second; nothing to poll otherwise
        refresh();
    }
    LLFloater::draw();
}
