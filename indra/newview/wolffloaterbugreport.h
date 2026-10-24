/**
 * @file wolffloaterbugreport.h
 * @brief WolfViewer: Report a Problem - send the Wolf Territories support team a ticket, with the viewer's logs.
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

#ifndef WOLF_FLOATERBUGREPORT_H
#define WOLF_FLOATERBUGREPORT_H

#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLTextBox;
class LLTextEditor;

// [WOLF BUG REPORT 2026-10-24] Paul: "build in a bug reporting a little bug icon on the menu that links to tickets and
// allows them to upload their logs". Opened by the status bar's bug icon and Wolf World > Report a Problem...
// The report becomes a support ticket on wolf-grid.com (gridmanager/viewerbugreport.php, the same tickets staff answer
// in Grid Admin); "My tickets" opens the website's ticket page in the browser. With "Attach my viewer logs" (on by
// default) the current and previous session logs, and the details of the last crash if there was one this week, are
// cut to their last part, scrubbed of the session id and capability links, gzipped and sent with it. Wolf Territories
// only: the request carries this login's agent and session ids.
class WolfFloaterBugReport : public LLFloater
{
public:
    WolfFloaterBugReport(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // One file the report would send (findLogs) and what it is called in the ticket.
    struct LogFile
    {
        std::string mPath;
        std::string mSendAs;     // e.g. "WolfViewer.log" (".gz" is added when sent)
        std::string mWhat;       // "this session", "the session before", "the last crash (21 Oct)"
        S64 mSize = 0;
    };

private:
    enum class EStatus { INFO, OK, ERR };

    void findLogs();
    void onSend();
    void setStatus(const std::string& text, EStatus kind);
    void setSending(bool sending);
    void onSent(const LLSD& reply, const std::string& error);
    static void sendCoro(LLHandle<LLFloater> handle, LLSD fields, std::vector<std::pair<std::string, std::string>> parts);

    LLComboBox* mCategory = nullptr;
    LLLineEditor* mSubject = nullptr;
    LLTextEditor* mBody = nullptr;
    LLCheckBoxCtrl* mAttach = nullptr;
    LLTextBox* mLogsList = nullptr;
    LLTextEditor* mStatus = nullptr;
    LLButton* mSend = nullptr;
    LLButton* mOpenTicket = nullptr;
    std::vector<LogFile> mLogs;
    std::string mTicketUrl;
    bool mSending = false;
};

#endif // WOLF_FLOATERBUGREPORT_H
