/**
 * @file wolffloaterimtranslate.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolffloaterimtranslate.h"

#include "fsfloaterim.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "lltextbox.h"
#include "lltranslate.h"
#include "lluicolortable.h"
#include "wolfvoicemessage.h"

WolfFloaterIMTranslate::WolfFloaterIMTranslate(const LLSD& key)
    : LLFloater(key)
    , mSessionID(key.asUUID())
{
}

bool WolfFloaterIMTranslate::postBuild()
{
    mAutoCB = getChild<LLCheckBoxCtrl>("auto_cb");
    mLangCombo = getChild<LLComboBox>("their_lang_combo");
    mMyLangText = getChild<LLTextBox>("my_lang_text");
    mStatusText = getChild<LLTextBox>("status_text");
    mTranslateLastBtn = getChild<LLButton>("translate_last_btn");
    mAutoCB->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAutoChanged(); });
    mLangCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onLanguageChanged(); });
    mTranslateLastBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTranslateLast(); });
    getChild<LLButton>("settings_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
    {
        // Source: fsfloaternearbychat.cpp translate_btn - the existing Translation settings window.
        LLFloaterReg::showInstance("prefs_translation");
    });
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void WolfFloaterIMTranslate::onOpen(const LLSD& key)
{
    mSessionID = key.asUUID();
    setStatus(LLStringUtil::null, false);
    refresh();
}

std::string WolfFloaterIMTranslate::otherName() const
{
    const std::string name = LLIMModel::instance().getName(mSessionID);
    return name.empty() ? std::string("them") : name;
}

// static
std::string WolfFloaterIMTranslate::languageName(const std::string& code)
{
    // Source: floater_translation_settings.xml wolf_their_language_combo - the same languages, in English.
    static const std::map<std::string, std::string> names = {
        { "en", "English" }, { "da", "Danish" }, { "de", "German" }, { "es", "Spanish" }, { "fr", "French" },
        { "it", "Italian" }, { "hu", "Hungarian" }, { "nl", "Dutch" }, { "pl", "Polish" }, { "pt", "Portuguese" },
        { "ru", "Russian" }, { "tr", "Turkish" }, { "uk", "Ukrainian" }, { "zh", "Chinese" }, { "ja", "Japanese" },
        { "ko", "Korean" } };
    auto it = names.find(code);
    return it != names.end() ? it->second : code;
}

void WolfFloaterIMTranslate::refresh()
{
    LLStringUtil::format_map_t args;
    args["[NAME]"] = otherName();
    getChild<LLTextBox>("heading_text")->setText(getString("heading", args));
    const bool configured = LLTranslate::isTranslationConfigured();
    mAutoCB->setValue(LLTranslate::isSessionTranslated(mSessionID));
    mAutoCB->setEnabled(configured);
    if (!mLangCombo->selectByValue(LLTranslate::getSessionLanguage(mSessionID)))
    {
        mLangCombo->selectByValue("auto");
    }
    mLangCombo->setEnabled(configured);
    mTranslateLastBtn->setEnabled(configured);
    LLStringUtil::format_map_t lang;
    lang["[LANG]"] = languageName(LLTranslate::getTranslateLanguage());
    mMyLangText->setText(getString("my_language", lang));
    if (!configured)
    {
        setStatus(getString("not_set_up"), true);
    }
}

void WolfFloaterIMTranslate::setStatus(const std::string& text, bool problem)
{
    mStatusText->setText(text.empty() ? std::string(" ") : text);
    mStatusText->setColor(LLUIColorTable::instance().getColor(problem ? "AlertCautionTextColor" : "LabelTextColor"));
}

void WolfFloaterIMTranslate::onAutoChanged()
{
    const bool on = mAutoCB->getValue().asBoolean();
    LLTranslate::setSessionTranslation(mSessionID, on, mLangCombo->getValue().asString());
    LLStringUtil::format_map_t args;
    args["[NAME]"] = otherName();
    setStatus(getString(on ? "on_status" : "off_status", args), false);
    if (FSFloaterIM* im = FSFloaterIM::findInstance(mSessionID))
    {
        im->updateTranslateButton();
    }
}

void WolfFloaterIMTranslate::onLanguageChanged()
{
    LLTranslate::setSessionTranslation(mSessionID, mAutoCB->getValue().asBoolean(), mLangCombo->getValue().asString());
}

void WolfFloaterIMTranslate::onTranslateLast()
{
    LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(mSessionID);
    std::string text, from;
    if (session)
    {
        // mMsgs has the newest message first (llimview.cpp LLIMSession::addMessage push_front). Their lines only: not mine,
        // not the system's (null id), not a voice message.
        for (const LLSD& msg : session->mMsgs)
        {
            const LLUUID from_id = msg["from_id"].asUUID();
            const std::string line = msg["message"].asString();
            if (from_id.isNull() || from_id == gAgentID || line.find("secondlife:///app/wolfvoicemsg/") != std::string::npos
                || WolfVoiceMessage::isVoiceMessageLine(line) || !LLTranslate::worthTranslating(line))
            {
                continue;
            }
            text = line;
            from = msg["from"].asString();
            break;
        }
    }
    LLStringUtil::format_map_t args;
    args["[NAME]"] = otherName();
    if (text.empty())
    {
        setStatus(getString("nothing_yet", args), true);
        return;
    }
    if (from.empty())
    {
        from = otherName();
    }
    setStatus(getString("working"), false);
    mTranslateLastBtn->setEnabled(false);
    LLHandle<LLFloater> h = getHandle();
    const LLUUID session_id = mSessionID;
    LLTranslate::translateMessage("", LLTranslate::getTranslateLanguage(), text,
        [h, session_id, text, from](std::string translation, std::string detected_lang)
        {
            translation = LLTranslate::removeNoTranslateTags(translation);
            LLStringUtil::trim(translation);
            const bool same = translation.empty() || LLStringUtil::compareInsensitive(translation, text) == 0;
            if (!same)
            {
                // A system line in the conversation, so it sits next to what it translates (never logged, never translated
                // again: llimview.cpp addMessage skips SYSTEM_FROM).
                WolfFloaterIMTranslate* f = static_cast<WolfFloaterIMTranslate*>(h.get());
                LLStringUtil::format_map_t line_args;
                line_args["[NAME]"] = from;
                line_args["[TEXT]"] = translation;
                std::string line = f ? f->getString("added_line", line_args) : ("Translation of " + from + "'s last message: " + translation);
                LLIMModel::instance().addMessage(session_id, SYSTEM_FROM, LLUUID::null, line, false);
            }
            if (WolfFloaterIMTranslate* f = static_cast<WolfFloaterIMTranslate*>(h.get()))
            {
                f->mTranslateLastBtn->setEnabled(true);
                f->setStatus(f->getString(same ? "same_language" : "done"), false);
            }
        },
        [h](int status, std::string err_msg)
        {
            LL_WARNS("Translate") << "Translate last message failed (" << status << "): " << err_msg << LL_ENDL;
            if (WolfFloaterIMTranslate* f = static_cast<WolfFloaterIMTranslate*>(h.get()))
            {
                LLStringUtil::format_map_t fail_args;
                fail_args["[REASON]"] = err_msg.empty() ? ("HTTP " + std::to_string(status)) : err_msg;
                f->mTranslateLastBtn->setEnabled(true);
                f->setStatus(f->getString("failed", fail_args), true);
            }
        });
}
