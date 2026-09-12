/**
 * @file wolfupdate.h
 * @brief WolfViewer: tell the user when a newer release exists.
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

#ifndef WOLF_UPDATE_H
#define WOLF_UPDATE_H

#include "stdtypes.h"   // S32
#include <string>

/**
 * Checks once per run whether a newer WolfViewer has been released, and offers to open the
 * download page.
 *
 * WHY THIS EXISTS. Releases are cut by hand and announced on Discord and the website, so anyone
 * not reading either simply never finds out — we have had residents report bugs against builds
 * several releases old, and diagnosed problems that were already fixed. The viewer knows its own
 * revision; it may as well say.
 *
 * WHAT IT DELIBERATELY DOES NOT DO: download or install anything. An installer that replaces a
 * running viewer is a category of risk (partial writes, permissions, a half-updated install) far
 * beyond the problem being solved, and every platform wants it done differently. This opens the
 * download page and lets the user decide.
 */
class WolfUpdate
{
public:
    /// Start the one check for this run. Safe to call more than once; only the first does work.
    static void checkOnce();

    /// The release this build is, taken from the viewer's own version (LL_VIEWER_VERSION_BUILD).
    static S32 currentRevision();

private:
    static void checkCoro();
    /// "v7.2.4-w26" -> 26. Returns -1 when the tag is not one of ours.
    static S32 revisionFromTag(const std::string& tag);
    static bool sChecked;
};

#endif // WOLF_UPDATE_H
