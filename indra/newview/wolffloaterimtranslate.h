/**
 * @file wolffloaterimtranslate.h
 * @brief WolfViewer: one conversation's translator, opened by the globe button in an IM window (ticket 631).
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

#ifndef WOLF_FLOATERIMTRANSLATE_H
#define WOLF_FLOATERIMTRANSLATE_H

#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLTextBox;

// [IM TRANSLATOR 2026-11-10] Keyed by the IM session id (LLFloaterReg "wolf_im_translate"). The setting lives in LLTranslate
// (isSessionTranslated / setSessionTranslation, per account); this window only shows and changes it, and translates the
// other person's newest line on request. Layout and the flow: floater_wolf_im_translate.xml.
class WolfFloaterIMTranslate : public LLFloater
{
public:
    explicit WolfFloaterIMTranslate(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void refresh() override;
    void onAutoChanged();
    void onLanguageChanged();
    void onTranslateLast();
    void setStatus(const std::string& text, bool problem);
    std::string otherName() const;
    static std::string languageName(const std::string& code);

    LLUUID          mSessionID;
    LLCheckBoxCtrl* mAutoCB = nullptr;
    LLComboBox*     mLangCombo = nullptr;
    LLTextBox*      mMyLangText = nullptr;
    LLTextBox*      mStatusText = nullptr;
    LLButton*       mTranslateLastBtn = nullptr;
};

#endif // WOLF_FLOATERIMTRANSLATE_H
