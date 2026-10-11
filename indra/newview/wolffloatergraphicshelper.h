/**
 * @file wolffloatergraphicshelper.h
 * @brief WolfViewer: Wolf World > Graphics Helper - what is wrong with the picture, switches to try
 *        each graphics feature off and on, and a diagnostic file (with the logs) to send us.
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

#ifndef WOLF_FLOATERGRAPHICSHELPER_H
#define WOLF_FLOATERGRAPHICSHELPER_H

#include "llfloater.h"

class LLCheckBoxCtrl;
class LLTextBox;
class LLTextEditor;

// [GRAPHICS HELPER 2026-10-11] Paul: "create a graphics helper on wolf world menu to help users
// diagnose graphics problems and have a download diagnostic button so they can download it and send
// it to us which will include their logs", then "build into the graphics diagnostic buttons that can
// turn things off and on (toggles) so the user can test different options". Prompted by Timmy (Mac,
// w63/w64: builds very dark where Firestorm on the same Mac is fine).
//
// One window: the computer in a few lines; what looks wrong (the shader fallback this session and in
// the last one's log, shader errors in the logs, sky light off, a photo effect left on, exposure,
// gamma, an unsupported card, little graphics memory); a switch per feature, applied at once, with
// "Put everything back" returning every one to how it was when the window was first opened this
// session; and "Save diagnostic file..." - a plain text file of all that plus WolfViewer.log and
// WolfViewer.old, saved where the user picks.
class WolfFloaterGraphicsHelper : public LLFloater
{
public:
    WolfFloaterGraphicsHelper(const LLSD& key);
    ~WolfFloaterGraphicsHelper() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void onToggle(size_t index);
    void onPutBack();
    void onSave();
    void refreshToggles();
    void refreshProblems();
    void setStatus(const std::string& text, bool error);
    static void saveTo(LLHandle<LLFloater> handle, const std::string& filename);

    std::vector<LLCheckBoxCtrl*> mChecks;   // one per entry of the switch table (the .cpp)
    LLTextBox*    mSystem = nullptr;
    LLTextEditor* mProblems = nullptr;
    LLTextBox*    mStatus = nullptr;
    std::vector<boost::signals2::connection> mConns;
};

#endif // WOLF_FLOATERGRAPHICSHELPER_H
