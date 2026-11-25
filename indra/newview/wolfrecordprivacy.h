/**
 * @file wolfrecordprivacy.h
 * @brief WolfViewer: "Don't include me in other people's recordings" - each resident's own choice, and the Video
 *        Recorder leaving out the people who made it.
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

#ifndef WOLF_RECORD_PRIVACY_H
#define WOLF_RECORD_PRIVACY_H

#include "llrect.h"
#include "lluuid.h"
#include <string>
#include <vector>

// [RECORDING PRIVACY 2026-11-25] Ticket 1060 (Luke Koda: "I want button or setting that allows or disallows videos of
// me... That should be up to each person."); Paul approved.
//
// Who and why: a resident who does not want to appear in other people's videos ticks one box, once, and every WolfViewer
// video recorder around them leaves them out (WolfStorm has no video recorder). Someone recording sees everyone as usual on their screen;
// only the video shows a soft grey shape where that person is (so it is clear someone is there), and their name tag is
// covered too.
//
// Where the choice lives: on the grid website (gridmanager/recordingprivacy.php, table grid.recording_optout), so it
// reaches everyone else's viewer:
//  - the resident's own choice: action=status / action=set with this login's X-Wolf-Agent / X-Wolf-Session (as
//    viewerdob.php), Wolf Territories residents only. Set from Preferences > Privacy, the Video Recorder window and the
//    website's account page.
//  - other people's: action=lookup, up to 100 avatar ids a request, anonymous (a hypergrid visitor recording on a Wolf
//    region asks too; the answer says only which of the ids have asked not to be recorded).
// Scale: nothing is asked unless the Video Recorder window is open or a recording runs, and then only on a Wolf region:
// one batched request for the avatars not yet known (at most one request in flight, at least 2 s apart), each answer
// kept for 5 minutes. 2000 residents with the window shut cost nothing.
namespace WolfRecordPrivacy
{
    /** LLAppViewer idle. Looks up the avatars around while the recorder needs them (keepLookingUp / a recording). */
    void idle();
    /** The Video Recorder window is open (called from its draw): look up the avatars around for the next few seconds,
     *  so their answers are in before Record is pressed. */
    void keepLookingUp();

    /** The opted-out avatars around (the last scan, at most a second old), not counting this viewer's own. */
    S32 nearbyCount();
    /** True when lookups have been failing for a while (the recorder window says so). */
    bool lookupTrouble();

    /** Main thread, from WolfRecorder's picture copy. The window rectangles (raw pixels, bottom-left origin, as
     *  LLViewerWindow::getWindowRectRaw) to blank in this recorded picture: each opted-out avatar the world drew this
     *  frame (not this viewer's own), and, when `name_tags` (the picture holds them), its name tag. */
    void recordingMasks(bool name_tags, std::vector<LLRectf>& out);

    // This resident's own choice (Wolf Territories logins only)
    enum class Own { UNAVAILABLE, LOADING, KNOWN, SAVING };
    /** Asks the website once per login when not known yet; returns the state now. */
    Own ownState();
    bool ownOptedOut();
    /** The last failure in plain words (empty: none). */
    std::string ownError();
    /** Saves the choice on the website (state SAVING, then KNOWN; on failure the old choice stands and ownError says
     *  why). */
    void setOwn(bool opted_out);
}

#endif // WOLF_RECORD_PRIVACY_H
