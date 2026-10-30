/**
 * @file wolffloaterinvisallow.h
 * @brief WolfViewer: Invisible Mode - the people who can still see you online.
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

#ifndef WOLF_FLOATERINVISALLOW_H
#define WOLF_FLOATERINVISALLOW_H

#include "llfloater.h"

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

// [SELECTIVE INVISIBLE MODE 2026-10-30] Paul: "i want an option where the user can allow certain users to see the person
// is online even if they are in invisible mode". Wolf World > "Invisible Mode: People Who Can See Me...".
// Who and why: a resident who uses Invisible Mode wants a partner or a few close friends to still see them online,
// and sets that up in under a minute.
// Flow: open -> the list loads -> pick a friend (or type a name) -> Add; select someone -> Remove. Every outcome stays in
// the status box (errors in the error colour until the next action).
// The list lives on the grid (grid.wolf_invisible_allow, wolfstorm.app/php/wolf_invisible_allow.php, X-Wolf-Agent +
// X-Wolf-Session checked there with the grid's presence service); the regions read it from there themselves
// (WolfSim WolfInvisibleModule). After each change this viewer tells its region GenericMessage "wolfinvisibleallow"
// "reload", which only makes the region read the list again - the region never takes the list from the viewer.
class WolfFloaterInvisAllow : public LLFloater
{
public:
    WolfFloaterInvisAllow(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    enum class EStatus { INFO, OK, ERR };

    void setStatus(const std::string& text, EStatus kind);
    void setBusy(bool busy);
    void fillFriends();
    void updateButtons();
    void onAddName();
    void onAddFriend();
    void onRemove();
    void send(const LLSD& body, const std::string& doing);
    void onReply(const LLSD& body, const LLSD& reply, const std::string& error);
    static void sendCoro(LLHandle<LLFloater> handle, LLSD body);

    LLScrollListCtrl* mList = nullptr;
    LLLineEditor* mName = nullptr;
    LLComboBox* mFriends = nullptr;
    LLButton* mAddName = nullptr;
    LLButton* mAddFriend = nullptr;
    LLButton* mRemove = nullptr;
    LLTextBox* mCount = nullptr;
    LLTextEditor* mStatus = nullptr;
    bool mBusy = false;
    S32 mMax = 100;
};

#endif // WOLF_FLOATERINVISALLOW_H
