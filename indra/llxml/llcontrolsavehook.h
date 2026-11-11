/**
 * @file llcontrolsavehook.h
 * @brief [WolfViewer 2026-11-11] A hook called at the start of every LLControlGroup::saveToFile.
 *
 * Set by the Vulkan renderer in a SECOND viewer instance (wolfvkbridge.cpp, decideBackend): the settings file is the
 * first instance's too, so before any save the second instance re-reads the Vulkan state keys from the file, and the
 * file keeps exactly what the first instance wrote. Empty (no hook) otherwise.
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

#ifndef LL_LLCONTROLSAVEHOOK_H
#define LL_LLCONTROLSAVEHOOK_H

#include <functional>
#include <string>

class LLControlGroup;

extern std::function<void(LLControlGroup& group, const std::string& filename)> gWolfControlBeforeSave;

#endif // LL_LLCONTROLSAVEHOOK_H
