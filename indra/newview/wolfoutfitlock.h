/**
 * @file wolfoutfitlock.h
 * @brief WolfViewer: Lock What I'm Wearing - nothing put on or taken off by the resident's own clicks while it is on.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#ifndef WOLF_OUTFITLOCK_H
#define WOLF_OUTFITLOCK_H

#include <string>

// [T964 2026-11-21] ticket 964, Jimmy Olsen: "the outfit has already a lock mode but for avoiding being deleted. but no protection
// to when u add something on by accident, it removes pretty much almost all for ur clothes or body ... maybe a way to get it
// protected? (so people would have choice to toggle it off when need actually add something else?)". Paul: yes, built Shadow's
// way. Firestorm's Protect (FSProtectedFolders, llinventorybridge.cpp LLFolderBridge::isProtected) only stops moving and
// deleting a folder; nothing stopped wearing.
//
// While it is on (per account: settings_per_account.xml WolfOutfitLocked), every wear, add, replace, take-off and detach the
// resident starts from the viewer's own windows and menus is refused, with one plain note that has an Unlock button. The gate
// (refuse) is at the top of LLAppearanceMgr's entry points (wear / replace / add / take off / remove, llappearancemgr.cpp) and in
// the three places that attach straight from the UI (rez_attachment: the inventory Wear / Attach action, dragging onto the
// avatar, the Attach To menu). NOT held, as Paul agreed: anything a script or another part of the viewer does - RLVa
// commands (rlvhelper.cpp RlvForceWear::done), the LEAP Appearance API (llappearancelistener.cpp), Weather Props' umbrella
// (wolfweatherprops.cpp), the Fishing rod (wolffishingoverlay.cpp), attaches from the region (they never come through these
// calls) and the outfit being rebuilt from the Current Outfit folder (updateAppearanceFromCOF is not gated). Those run inside a
// Bypass. A call that passed the gate keeps going through its later callbacks (they run inside a Bypass too): the decision
// is made once, when the resident acts.
namespace WolfOutfitLock
{
    bool locked();
    void setLocked(bool on);
    void toggle();

    /** True when the outfit is locked and this is not inside a Bypass: the caller must not go on. Shows the note (one at a
     *  time) and logs what was refused (`what`, a few words for the log, never shown). */
    bool refuse(const char* what);

    /** What refuse would answer, without the note: for a drag over the avatar, whose cursor says "locked" before the drop. */
    bool wouldRefuse();

    /** Calls made while one of these lives are not held (scripts, RLVa, viewer features, callbacks of a call that passed). */
    class Bypass
    {
    public:
        Bypass();
        ~Bypass();
        Bypass(const Bypass&) = delete;
        Bypass& operator=(const Bypass&) = delete;
    };
}

#endif // WOLF_OUTFITLOCK_H
