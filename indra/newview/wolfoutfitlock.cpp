/**
 * @file wolfoutfitlock.cpp
 * @brief WolfViewer: Lock What I'm Wearing (ticket 964). See wolfoutfitlock.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfoutfitlock.h"

#include "llnotifications.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"

namespace
{
    // Calls on the main thread only (the UI and LLAppearanceMgr): a plain depth, not thread-local.
    S32 sBypassDepth = 0;
    // The note now showing, so a multi-item wear (or a burst of drags) shows one note, not one per item.
    LLUUID sNoteId;

    // Source: settings_per_account.xml WolfOutfitLocked (Boolean, default 0): the resident's own choice, kept per account
    const char* const SETTING = "WolfOutfitLocked";
}

bool WolfOutfitLock::locked()
{
    return gSavedPerAccountSettings.controlExists(SETTING) && gSavedPerAccountSettings.getBOOL(SETTING);
}

void WolfOutfitLock::setLocked(bool on)
{
    if (!gSavedPerAccountSettings.controlExists(SETTING)) return;
    gSavedPerAccountSettings.setBOOL(SETTING, on);
    LL_INFOS("WolfOutfitLock") << (on ? "outfit locked" : "outfit unlocked") << LL_ENDL;
}

void WolfOutfitLock::toggle()
{
    setLocked(!locked());
}

bool WolfOutfitLock::wouldRefuse()
{
    return sBypassDepth == 0 && locked();
}

bool WolfOutfitLock::refuse(const char* what)
{
    if (!wouldRefuse())
    {
        return false;
    }
    LL_INFOS("WolfOutfitLock") << "refused while locked: " << (what ? what : "?") << LL_ENDL;
    // one note at a time (LLNotifications::find: null once it has been answered or closed)
    if (sNoteId.notNull() && LLNotifications::instance().find(sNoteId))
    {
        return true;
    }
    // notifications.xml WolfOutfitLocked: an alert that stays until it is answered (a refusal must be read, the canon), with
    // Unlock (option 0) and Keep it locked (option 1)
    LLNotificationPtr n = LLNotificationsUtil::add("WolfOutfitLocked", LLSD(), LLSD(), [](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            WolfOutfitLock::setLocked(false);
        }
    });
    sNoteId = n ? n->getID() : LLUUID::null;
    return true;
}

WolfOutfitLock::Bypass::Bypass()
{
    ++sBypassDepth;
}

WolfOutfitLock::Bypass::~Bypass()
{
    --sBypassDepth;
}
