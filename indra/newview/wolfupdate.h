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
 *
 * WHEN (ticket 532, 2026-11-05). The one check starts when the login screen is built, so a newer
 * release is shown there, BEFORE logging in, as a band across the top of the screen
 * (panel_wolf_update_notice.xml): "A new WolfViewer is ready: <name>", Update now, Not now. Update
 * now there downloads and installs straight away (nothing in-world can be lost yet). Not now is
 * "Skip this version": nothing more about that release, on the login screen or in-world; the next
 * release is offered as usual. The answer is kept for the run, so logging in does not ask the
 * server again: once in-world, the notices above follow from the same answer.
 */
class LLPanel;

class WolfUpdate
{
public:
    /// Start the one check for this run. Safe to call more than once; only the first does work.
    static void checkOnce();

    /// From the login panel's constructor (fspanellogin.cpp): start the check if it has not started
    /// and put the band on this panel once a newer release is known.
    static void loginScreenShown(LLPanel* login_panel);

    /// From STATE_STARTED (llstartup.cpp): the in-world notices, from the check already made.
    static void loggedIn();

    /// The release this build is, taken from the viewer's own version (LL_VIEWER_VERSION_BUILD).
    static S32 currentRevision();

    // ── [BETA CHANNEL 2026-11-12] "Get beta versions" (wolfupdatechannel.h; DESIGN.md in the firestorm repo,
    //    region-terrain-work/beta-channel-2026-11-12) ────────────────────────────────────────────────────────
    // With the box ticked (WolfViewerGetBetaVersions, Preferences > WolfViewer > Extras) the one check also reads
    // .../downloads/wolfviewer/beta/latest.json, signed with the same key and verified by the same code, and offers
    // whichever of the two is the higher revision (the release when they are equal). Unticked, it asks for and acts
    // on exactly what W104 did. A beta is promoted to the release as the very same files, so "Beta" is not compiled
    // in: a build is a beta while its revision is above the release in the last release manifest that verified.

    /// Is this build a beta (see above)? False until a release manifest has ever been verified on this computer.
    static bool isBetaBuild();
    /// "beta", "stable" or "unknown" (no release manifest verified yet): crash reports, Flea Reports, the version record.
    static std::string channelWord();
    /// " (Beta)" for a beta, else "": the window title (llappviewer.cpp) and About's RELEASE_NAME.
    static std::string betaSuffix();
    /// The login screen's release text: the release name (or `unnamed_fallback` for a build with none), " (Beta)"
    /// for a beta, " · AVX2" on the AVX2 build. Written again by the check when the answer changes it.
    static std::string releaseLabel(const std::string& unnamed_fallback);
    /// [STAGED ROLLOUT 2026-11-12] Help > Check for WolfViewer Updates: check now. A release that is being offered to
    /// a share of installs first (the manifest's signed "rollout") is offered at once to someone who asks, an earlier
    /// "Not now" is forgotten, and there is always an answer (an offer, "you have the newest", "paused", "could not check").
    static void checkByHand();
    /// From FSPanelPrefs::postBuild: the "Get beta versions" box and the sentence under it (which says what
    /// unticking does while a beta is running, and why the box is greyed out on a package-repository install).
    static void setupPreferences(LLPanel* extras_panel);
    /// From FSPanelPrefs::onOpen: the sentence only (setupPreferences connects the box's callback, once).
    static void refreshPreferences(LLPanel* extras_panel);

    /// From LLAppViewer::cleanup(): stop a download in progress and wait for its thread.
    static void shutdown();

    /// "Update now": fetch and verify the release on offer (a no-op while one is under way).
    static void startDownload();
    /// "Restart and update" / "Open installer": install the verified download. False when it
    /// did not start (nothing downloaded, already under way, or unsaved ground paint).
    static bool startInstall();

private:
    static void checkCoro();
    static void downloadCoro();
    static void installCoro();
};

#endif // WOLF_UPDATE_H
