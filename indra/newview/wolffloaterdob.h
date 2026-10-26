/**
 * @file wolffloaterdob.h
 * @brief WolfViewer: Your Date of Birth (Wolf World > Date of Birth..., and after login when the account has none).
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

#ifndef WOLF_FLOATERDOB_H
#define WOLF_FLOATERDOB_H

#include "llfloater.h"

class LLButton;
class LLComboBox;
class LLTextBox;
class LLTextEditor;

// [DOB IN THE VIEWER 2026-10-26] Paul: "any user that does not have their age set on our grid should have the DOB
// requested through the viewer with a note that giving an incorrect age could lock them out of the account".
// After a Wolf Territories login, WolfDob::idle asks gridmanager/viewerdob.php (X-Wolf-Agent + X-Wolf-Session, checked
// there with the grid's presence service) whether the account has a date of birth, and opens this window when it has
// none. "Not now" just closes it: the next login asks again, and nothing is ever blocked. Flow: day / month / year, then
// a check step ("You were born on 3 March 2001, so you're 25. Is that right?"), then saved. The rules are the website's
// (doblib.php): set once, then only support can change it; 14 and under closes the account (terms section 1).
// Wolf Territories residents only (the server refuses anyone else, hypergrid visitors included).
class WolfFloaterDob : public LLFloater
{
public:
    WolfFloaterDob(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    enum class EStep { LOADING, ENTER, CHECK, DONE };
    enum class EStatus { INFO, OK, ERR };

    void showStep(EStep step);
    void setStatus(const std::string& text, EStatus kind);
    std::string pickedDate() const;   // "YYYY-MM-DD", or "" when a part is not chosen
    void onContinue();
    void onSave();
    void send(const std::string& action, const std::string& dob);
    void onReply(const std::string& action, const LLSD& reply, const std::string& error);
    static void sendCoro(LLHandle<LLFloater> handle, std::string action, std::string dob);

    LLComboBox* mDay = nullptr;
    LLComboBox* mMonth = nullptr;
    LLComboBox* mYear = nullptr;
    LLTextBox* mCheckText = nullptr;
    LLTextEditor* mStatus = nullptr;
    LLButton* mNotNow = nullptr;
    LLButton* mContinue = nullptr;
    LLButton* mChange = nullptr;
    LLButton* mSave = nullptr;
    LLButton* mClose = nullptr;
    std::string mChecked;   // the date the check step shows, sent by "Yes, save it"
    bool mBusy = false;
};

namespace WolfDob
{
    /** After a Wolf Territories login: ask once per login whether the account has a date of birth, and open the
     *  window when it has none. From LLAppViewer idle. */
    void idle();
}

#endif // WOLF_FLOATERDOB_H
