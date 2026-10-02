/**
 * @file wolfupdate.h
 * @brief WolfViewer: find, fetch and install a newer release.
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
 * Once per run: is there a newer WolfViewer, and if so, get it onto this machine with one click.
 *
 * WHY. Residents complained that updating meant downloading and re-installing by hand (Paul,
 * 2026-10-02). Until then this only offered to open the download page.
 *
 * WHAT IS TRUSTED. https://wolf-grid.com/downloads/wolfviewer/latest.json names the release and,
 * per platform, the download's URL, size and SHA-256. It is signed (latest.json.sig: a raw
 * Ed25519 signature over the file's exact bytes) with a key whose public half is compiled in
 * (wolfupdate.cpp), so a changed file on the web server cannot point viewers at anything else,
 * and a download must match the signed SHA-256 before anything runs. Both files are written by
 * ~/wolfviewer-releases/publish_downloads.sh.
 *
 * WHAT HAPPENS, by how this copy was installed:
 *  - Windows: the signed NSIS installer runs silently into the same folder (/S /SKIP_DIALOGS
 *    /UPDATE — one Windows permission prompt) and starts the viewer again.
 *  - Linux, installed from the tarball into a folder the user can write: the new tarball is
 *    unpacked beside this one and swapped in by etc/wolfviewer-update.sh; the old folder is kept
 *    as <dir>-<timestamp>, as install.sh keeps them.
 *  - Linux, installed from the Wolf Territories apt / dnf / pacman repository (the package ships
 *    etc/wolfviewer-package): nothing is downloaded; the system's updater installs it, and the
 *    resident is told once.
 *  - Linux, a folder the user cannot write: told, with the download page.
 *  - macOS: the disk image is downloaded, checked and opened; dragging WolfViewer onto
 *    Applications replaces the old copy.
 */
class WolfUpdate
{
public:
    /// Start the one check for this run. Safe to call more than once; only the first does work.
    static void checkOnce();

    /// The release this build is, taken from the viewer's own version (LL_VIEWER_VERSION_BUILD).
    static S32 currentRevision();

    /// From LLAppViewer::cleanup(): stop a download in progress and wait for its thread.
    static void shutdown();

    /// "Update now": fetch and verify the release on offer (a no-op while one is under way).
    static void startDownload();
    /// "Restart and update" / "Open installer": install the verified download.
    static void startInstall();

private:
    static void checkCoro();
    static void downloadCoro();
    static void installCoro();
};

#endif // WOLF_UPDATE_H
