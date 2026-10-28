/**
 * @file wolflogscrub.h
 * @brief WolfViewer: take private details out of a viewer log, keep its tail, gzip it.
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

#ifndef WOLF_LOG_SCRUB_H
#define WOLF_LOG_SCRUB_H

#include "stdtypes.h"
#include <string>
#include <vector>

// [CRASH REPORTS 2026-10-28] Shared by the Flea Report (wolffloaterbugreport.cpp) and the automatic crash report
// (wolfcrashreport.cpp). The rules are ticketlib.php tkScrubLogText's; the website runs them again on what it receives.
namespace WolfLogScrub
{
    /// "1.4 MB" / "830 KB" / "12 bytes".
    std::string size_text(S64 n);

    /// Take out the given session ids (any case) and every capability link, password, token and key.
    std::string wolf_scrub_log(std::string text, const std::vector<std::string>& secrets);

    /// The last max_bytes of a file, from the start of a line, with a "[WolfViewer: only the last ..." note on top
    /// when it was cut (ticketlib.php tkPrepareLogFile looks for that note). False when the file cannot be read.
    bool read_tail(const std::string& path, std::string& out, S64 max_bytes);

    /// gzip (RFC 1952) of a string.
    bool gzip_string(const std::string& in, std::string& out);
}

#endif // WOLF_LOG_SCRUB_H
