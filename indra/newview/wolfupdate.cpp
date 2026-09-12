/**
 * @file wolfupdate.cpp
 * @brief WolfViewer: tell the user when a newer release exists.
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

#include "wolfupdate.h"

#include <boost/json.hpp>

#include "llcorehttputil.h"
#include "llcoros.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llweb.h"

bool WolfUpdate::sChecked = false;

namespace
{
    // GitHub's own "latest release" for the viewer's repository. The tag is the single source of
    // truth for what has shipped — it is created by the release itself, so there is no separate
    // file to remember to update and no way for this to disagree with what people can download.
    const char* const LATEST_RELEASE_URL =
        "https://api.github.com/repos/intelligentwolf/wolfviewer/releases/latest";

    // Where a person actually gets it. NOT the GitHub release page: the website serves the four
    // installers (viewers.php reads them from /downloads/wolfviewer/) and is the page everything
    // else — Discord, the login screen — already points at.
    // f=osv, NOT f=viewers. index.php has no "viewers" case (it routes "osv" to viewers.php),
    // so f=viewers renders the site's default page — HTTP 200, and not one download link on it.
    // Someone who answered "yes, open the download page" got a page with nothing to download.
    const char* const DOWNLOAD_PAGE_URL = "https://www.wolf-grid.com/index.php?f=osv";

    /// The setting that turns the check off entirely.
    const char* const CHECK_SETTING = "WolfViewerCheckForUpdates";
    /// The newest revision the user has already been told about, so a decline is not re-asked
    /// every single launch. Per revision, so the NEXT release still gets one prompt.
    const char* const TOLD_SETTING = "WolfViewerUpdateLastTold";

    bool onUpdatePrompt(const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
        }
        return false;
    }
}

// static
S32 WolfUpdate::currentRevision()
{
    // The 4th version field is the release number: the CI workflow sets `revision: "N"` and
    // BuildVersion.cmake:14 reads it into LL_VIEWER_VERSION_BUILD. So w26 is build 26.
    return (S32)LLVersionInfo::instance().getBuild();
}

// static
S32 WolfUpdate::revisionFromTag(const std::string& tag)
{
    // Tags are "v7.2.4-w26". Anything else is not one of ours and is ignored rather than guessed
    // at — a malformed tag must not be read as revision 0 and trigger a downgrade prompt.
    const size_t w = tag.rfind("-w");
    if (w == std::string::npos || w + 2 >= tag.size()) return -1;
    const std::string digits = tag.substr(w + 2);
    if (digits.empty()) return -1;
    for (char c : digits)
    {
        if (c < '0' || c > '9') return -1;
    }
    return (S32)atoi(digits.c_str());
}

// static
void WolfUpdate::checkOnce()
{
    if (sChecked) return;
    sChecked = true;
    if (!gSavedSettings.getBOOL(CHECK_SETTING)) return;
    LLCoros::instance().launch("WolfUpdate", []() { WolfUpdate::checkCoro(); });
}

// static
void WolfUpdate::checkCoro()
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfUpdate", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
    opts->setTimeout(30);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/vnd.github+json");
    // GitHub refuses requests with no user agent.
    headers->append(HTTP_OUT_HEADER_USER_AGENT, LLVersionInfo::instance().getChannelAndVersion());

    LLSD result = adapter->getRawAndSuspend(request, LATEST_RELEASE_URL, opts, headers);
    LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
    if (!status)
    {
        // Offline, rate limited, or GitHub having a bad day. Never a dialog: the user did not ask
        // for this check and a failed one is not their problem.
        LL_INFOS("WolfUpdate") << "update check failed: " << status.toString() << LL_ENDL;
        return;
    }
    if (!result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        LL_INFOS("WolfUpdate") << "update check: empty reply" << LL_ENDL;
        return;
    }

    std::string tag;
    {
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        const std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) { LL_INFOS("WolfUpdate") << "update check: unreadable reply" << LL_ENDL; return; }
        const LLSD sd = LlsdFromJson(v);
        tag = sd.has("tag_name") ? sd["tag_name"].asString() : std::string();
    }

    const S32 latest = revisionFromTag(tag);
    const S32 mine = currentRevision();
    LL_INFOS("WolfUpdate") << "this build is w" << mine << ", latest release is "
                           << (tag.empty() ? "unknown" : tag) << LL_ENDL;
    if (latest <= 0 || latest <= mine) return;

    // Asked once per release, not once per launch: someone who says no should not be nagged
    // every time they start the viewer, but the NEXT release still gets to ask.
    if (gSavedSettings.getS32(TOLD_SETTING) >= latest) return;
    gSavedSettings.setS32(TOLD_SETTING, latest);

    const std::string msg = llformat(
        "WolfViewer w%d is available — you are running w%d.\n\n"
        "Open the download page?", latest, mine);
    LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", msg),
                             LLSD(), boost::bind(onUpdatePrompt, _1, _2));
}
