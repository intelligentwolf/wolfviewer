/**
 * @file wolffloaterbugreport.cpp
 * @brief WolfViewer: Report a Problem. See wolffloaterbugreport.h.
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

#include "wolffloaterbugreport.h"

#include <ctime>
#include <regex>

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "indra_constants.h"   // APP_NAME
#include "llgl.h"
#include "lllineeditor.h"
#include "llsdjson.h"
#include "llsys.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llversioninfo.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "workqueue.h"
#include "wolfgrid.h"

#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

namespace
{
    // gridmanager/viewerbugreport.php on the grid website; the ticket pages are gridmanager/tickets.php
    // (index.php?f=tk, and &ticket=<id> for one ticket - ticketlib.php tkTicketUrl).
    const char* REPORT_API = "https://www.wolf-grid.com/viewerbugreport.php";
    const char* TICKETS_URL = "https://www.wolf-grid.com/index.php?f=tk";

    // The keep-the-tail cap for each log before it is compressed. viewerbugreport.php accepts up to
    // TK_LOG_MAX_FILE_BYTES (4 MB) per compressed file and 16 MB once opened (TK_LOG_MAX_RAW_BYTES); 4 MB of
    // text compresses to well under 1 MB, so four of them stay inside its 8 MB total.
    const S64 MAX_LOG_BYTES = 4 * 1024 * 1024;
    // A crash older than this is not "the problem you are reporting".
    const time_t CRASH_MAX_AGE = 7 * 24 * 60 * 60;
    // Lines longer than this are scrubbed a piece at a time (std::regex recurses on long matches); the server
    // scrubs the whole file again in one go (ticketlib.php tkScrubLogText), so a piece edge cannot leak anything.
    const size_t SCRUB_CHUNK = 16 * 1024;

    // ticketlib.php tkCategories(), in the website's order: the same keys, so the ticket lands in the same queue
    // filter as one raised on the website.
    const char* const CATEGORIES[][2] = {
        { "region", "Region / sim problem" },
        { "land", "Land, rentals & billing" },
        { "account", "Account & login" },
        { "payment", "Payments & currency" },
        { "marketplace", "Marketplace" },
        { "inventory", "Inventory & assets" },
        { "scripting", "Scripting problem" },
        { "teleport", "Teleporting & map problem" },
        { "abuse", "Abuse report" },
        { "other", "Something else" },
    };

    std::string size_text(S64 n)
    {
        if (n >= 1024 * 1024) return llformat("%.1f MB", (F64)n / (1024.0 * 1024.0));
        if (n >= 1024) return llformat("%d KB", (S32)(n / 1024));
        return llformat("%d bytes", (S32)n);
    }

    std::string date_text(time_t t)
    {
        struct tm g;
#if LL_WINDOWS
        localtime_s(&g, &t);
#else
        localtime_r(&t, &g);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%d %b %H:%M", &g);
        return buf;
    }

    std::string lower(std::string s)
    {
        LLStringUtil::toLower(s);
        return s;
    }

    void replace_all(std::string& s, const std::string& from, const std::string& to)
    {
        if (from.empty()) return;
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        {
            s.replace(at, from.size(), to);
        }
    }

    // The rules of ticketlib.php tkScrubLogText, one line (or piece of a line) at a time. Each regex only runs
    // when the line holds the word it needs, which keeps a 4 MB log to a fraction of a second.
    std::string scrub_piece(const std::string& in)
    {
        static const std::regex cap_url(
            "\\b(https?://[^/\\s\"'<>]+)/[^\\s\"'<>]*?([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|[0-9a-f]{32})[^\\s\"'<>]*",
            std::regex::ECMAScript | std::regex::icase);
        static const std::regex query_secret(
            "([?&;](password|passwd|pass|pwd|token|access_token|auth|key|apikey|api_key|secret|sig|signature|session|session_id|secure_session_id)=)[^&\\s\"'<>]+",
            std::regex::ECMAScript | std::regex::icase);
        static const std::regex llsd_secret(
            "(<key>\\s*(passwd|password|secure_session_id|session_id|mfa_hash|token|auth_token|seed_capability)\\s*</key>\\s*<(\\w+)>)[^<]*(</\\3>)",
            std::regex::ECMAScript | std::regex::icase);
        static const std::regex bearer("\\bBearer\\s+[A-Za-z0-9._~+/=-]{8,}", std::regex::ECMAScript);
        static const std::regex md5_pass("\\$1\\$[0-9a-fA-F]{32}", std::regex::ECMAScript);
        static const std::regex key_value(
            "\\b(password|passwd|secret|token|api[_-]?key|authorization)([\"']?\\s*[:=]\\s*[\"']?)[^\\s\"'<>,;]+",
            std::regex::ECMAScript | std::regex::icase);

        const std::string l = lower(in);
        std::string s = in;
        if (l.find("http") != std::string::npos) s = std::regex_replace(s, cap_url, "$1/[capability removed]");
        if (l.find('=') != std::string::npos) s = std::regex_replace(s, query_secret, "$1[removed]");
        if (l.find("<key>") != std::string::npos) s = std::regex_replace(s, llsd_secret, "$1[removed]$4");
        if (l.find("bearer") != std::string::npos) s = std::regex_replace(s, bearer, "Bearer [removed]");
        if (l.find("$1$") != std::string::npos) s = std::regex_replace(s, md5_pass, "[password hash removed]");
        if (l.find("pass") != std::string::npos || l.find("secret") != std::string::npos || l.find("token") != std::string::npos
            || l.find("key") != std::string::npos || l.find("authoriz") != std::string::npos)
        {
            s = std::regex_replace(s, key_value, "$1$2[removed]");
        }
        return s;
    }

    // Take out the session ids and every capability link, password, token and key the log may hold. What a
    // WolfViewer log really contains (read 2026-10-24): capability URLs ("Requesting seed from
    // http://regionsN.wolfterritories.org:port/CAPS/<uuid>0000/", the event queue ".../CE/<uuid>", every cap
    // ".../<uuid>"), which are bearer secrets for this session; IM session ids (kept: not secrets); no passwords.
    // The other rules are for what Firestorm can log with debug settings on (an LLSD login dump: <key>passwd</key>).
    std::string wolf_scrub_log(std::string text, const std::vector<std::string>& secrets)
    {
        for (const std::string& secret : secrets)
        {
            if (secret.size() < 16) continue;
            std::string up = secret;
            LLStringUtil::toUpper(up);
            replace_all(text, lower(secret), "[session id removed]");
            replace_all(text, up, "[session id removed]");
        }
        static const std::regex sensitive_key(
            "<key>\\s*(passwd|password|secure_session_id|session_id|mfa_hash|token|auth_token|seed_capability)\\s*</key>\\s*$",
            std::regex::ECMAScript | std::regex::icase);
        static const std::regex value_line("^(\\s*<(\\w+)>)[^<]*(</\\2>)", std::regex::ECMAScript);

        std::string out;
        out.reserve(text.size());
        bool pending_value = false;   // the line before named a secret LLSD key and left its value to this line
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find('\n', start);
            const size_t stop = end == std::string::npos ? text.size() : end + 1;
            std::string line = text.substr(start, stop - start);
            if (pending_value)
            {
                line = std::regex_replace(line, value_line, "$1[removed]$3", std::regex_constants::format_first_only);
                pending_value = false;
            }
            if (line.size() <= SCRUB_CHUNK)
            {
                if (line.find("<key>") != std::string::npos || line.find("<KEY>") != std::string::npos)
                {
                    pending_value = std::regex_search(line, sensitive_key);
                }
                out += scrub_piece(line);
            }
            else
            {
                for (size_t at = 0; at < line.size(); at += SCRUB_CHUNK)
                {
                    out += scrub_piece(line.substr(at, SCRUB_CHUNK));
                }
            }
            start = stop;
        }
        return out;
    }

    // The last MAX_LOG_BYTES of a file, from the start of a line, with a note on top when it was cut.
    bool read_tail(const std::string& path, std::string& out)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) return false;
        const std::streamoff from = size > MAX_LOG_BYTES ? size - MAX_LOG_BYTES : 0;
        in.seekg(from, std::ios::beg);
        std::string data((size_t)(size - from), '\0');
        in.read(&data[0], (std::streamsize)data.size());
        data.resize((size_t)in.gcount());
        if (from > 0)
        {
            const size_t nl = data.find('\n');
            if (nl != std::string::npos) data.erase(0, nl + 1);
            // ticketlib.php tkPrepareLogFile looks for "[WolfViewer: only the last" to mark the file "last part only".
            data = llformat("[WolfViewer: only the last %s of this %s log were sent]\n",
                            size_text(MAX_LOG_BYTES).c_str(), size_text((S64)size).c_str()) + data;
        }
        out.swap(data);
        return true;
    }

    // gzip (RFC 1952: windowBits 15 + 16) of a string.
    bool gzip_string(const std::string& in, std::string& out)
    {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
        zs.next_in = (Bytef*)in.data();
        zs.avail_in = (uInt)in.size();   // at most MAX_LOG_BYTES plus a note line
        out.clear();
        char buf[65536];
        int rc = Z_OK;
        do
        {
            zs.next_out = (Bytef*)buf;
            zs.avail_out = (uInt)sizeof(buf);
            rc = deflate(&zs, Z_FINISH);
            if (rc == Z_STREAM_ERROR) break;
            out.append(buf, sizeof(buf) - zs.avail_out);
        } while (rc != Z_STREAM_END);
        deflateEnd(&zs);
        return rc == Z_STREAM_END;
    }

    // Runs on the "General" work queue: read, scrub and compress every log. Returns [file name, gzip bytes] pairs.
    std::vector<std::pair<std::string, std::string>> prepare_logs(const std::vector<WolfFloaterBugReport::LogFile>& logs,
                                                                  const std::vector<std::string>& secrets)
    {
        std::vector<std::pair<std::string, std::string>> parts;
        for (const WolfFloaterBugReport::LogFile& f : logs)
        {
            std::string text, gz;
            if (!read_tail(f.mPath, text)) continue;
            if (!gzip_string(wolf_scrub_log(text, secrets), gz)) continue;
            parts.emplace_back(f.mSendAs + ".gz", gz);
        }
        return parts;
    }

    std::string json_string(const LLSD& v, const char* key)
    {
        return v.isMap() && v.has(key) ? v[key].asString() : std::string();
    }
}

WolfFloaterBugReport::WolfFloaterBugReport(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterBugReport::postBuild()
{
    mCategory = getChild<LLComboBox>("category");
    mSubject = getChild<LLLineEditor>("subject");
    mBody = getChild<LLTextEditor>("body");
    mAttach = getChild<LLCheckBoxCtrl>("attach_logs");
    mLogsList = getChild<LLTextBox>("logs_list");
    mStatus = getChild<LLTextEditor>("status");
    mSend = getChild<LLButton>("send");
    mOpenTicket = getChild<LLButton>("open_ticket");

    mCategory->add("Choose one...", LLSD(std::string()));
    for (const auto& c : CATEGORIES)
    {
        mCategory->add(c[1], LLSD(std::string(c[0])));
    }
    mCategory->selectFirstItem();
    mBody->setLabel(std::string("What you were doing, what you expected, and what happened instead."));

    mSend->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    getChild<LLButton>("my_tickets")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(TICKETS_URL); });
    mOpenTicket->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (!mTicketUrl.empty()) LLWeb::loadURLExternal(mTicketUrl);
    });
    mAttach->setCommitCallback([this](LLUICtrl*, const LLSD&) { findLogs(); });
    return true;
}

void WolfFloaterBugReport::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    findLogs();
    if (!mSending && !WolfGrid::isWolfTerritories())   // Paul 10-24: "only wolf residents" (a Wolf login, wherever they are)
    {
        setStatus("Report a Problem is for Wolf Territories residents. To report a problem on another grid, "
                  "use that grid's own support.", EStatus::ERR);
    }
}

// Exactly which files would be sent, with their sizes, so the resident knows what they are handing over.
void WolfFloaterBugReport::findLogs()
{
    mLogs.clear();
    // llappviewer.cpp initLoggingAndGetLastDuration: this session logs to APP_NAME + ".log"; the session before is
    // renamed to APP_NAME + ".old" at start-up.
    const struct { const char* ext; const char* what; } sessions[] = { { ".log", "this session" }, { ".old", "the session before" } };
    for (const auto& s : sessions)
    {
        LogFile f;
        f.mPath = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + s.ext);
        f.mSendAs = APP_NAME + s.ext;
        f.mWhat = s.what;
        llstat st;
        if (LLFile::stat(f.mPath, &st) == 0 && st.st_size > 0)
        {
            f.mSize = (S64)st.st_size;
            mLogs.push_back(f);
        }
    }

    // The last crash: each run writes static_debug_info.log into its own "dump-<uuid>" folder in the logs folder
    // (llappviewer.cpp setDebugFileNames(LL_PATH_DUMP), lldir.cpp getDumpDir) and a clean exit deletes it
    // (LLAppViewer::removeDumpDir), so a folder left behind is a run that crashed or was killed. On Windows the
    // crash handler also copies the log there (llappviewerwin32.cpp FS::LogfileOut). The minidump (.dmp) is NOT
    // sent: it is a copy of the viewer's memory, which holds the session and cannot be scrubbed.
    const std::string logdir = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "");
    const std::string this_run = gDirUtilp->dumpDirExists() ? gDirUtilp->getBaseFileName(gDirUtilp->getDumpDir()) : std::string();
    std::string newest;
    time_t newest_time = 0;
    {
        LLDirIterator it(logdir, "dump-*");
        std::string name;
        while (it.next(name))
        {
            const std::string dir = logdir + name;
            llstat st;
            if (name == this_run || !LLFile::isdir(dir) || LLFile::stat(dir + gDirUtilp->getDirDelimiter() + "static_debug_info.log", &st) != 0)
            {
                continue;
            }
            if (st.st_mtime > newest_time)
            {
                newest_time = st.st_mtime;
                newest = dir;
            }
        }
    }
    if (!newest.empty() && time(nullptr) - newest_time <= CRASH_MAX_AGE)
    {
        const std::string when = date_text(newest_time);
        for (const std::string& file : { std::string("static_debug_info.log"), APP_NAME + ".log" })
        {
            LogFile f;
            f.mPath = newest + gDirUtilp->getDirDelimiter() + file;
            f.mSendAs = "crash-" + file;
            f.mWhat = "the crash on " + when;
            llstat st;
            if (LLFile::stat(f.mPath, &st) == 0 && st.st_size > 0)
            {
                f.mSize = (S64)st.st_size;
                mLogs.push_back(f);
            }
        }
    }

    std::string t;
    if (mLogs.empty())
    {
        t = "No viewer logs were found, so only your message will be sent.";
    }
    else
    {
        t = mAttach->get() ? "These will be sent:\n" : "Not attached (tick the box to send them):\n";
        for (const LogFile& f : mLogs)
        {
            t += f.mSendAs + " - " + f.mWhat + ", " + size_text(f.mSize)
                 + (f.mSize > MAX_LOG_BYTES ? " (the last " + size_text(MAX_LOG_BYTES) + " only)" : std::string()) + "\n";
        }
        t += "Session ids and capability links are taken out first. Only the support team can open them.";
    }
    mLogsList->setText(t);
}

void WolfFloaterBugReport::setStatus(const std::string& text, EStatus kind)
{
    // Light red / light green on the dark floater, and the words say it too ("Not sent", "Sent"), never colour alone.
    static const LLUIColor err(LLColor4(1.f, 0.6f, 0.6f, 1.f));
    static const LLUIColor ok(LLColor4(0.79f, 1.f, 0.79f, 1.f));
    static const LLUIColor info(LLColor4::white);
    const LLUIColor& c = kind == EStatus::ERR ? err : kind == EStatus::OK ? ok : info;
    mStatus->setReadOnlyColor(c);
    mStatus->setColor(c);
    mStatus->setText(text);
    mStatus->setVisible(!text.empty());
}

void WolfFloaterBugReport::setSending(bool sending)
{
    mSending = sending;
    mSend->setEnabled(!sending);
    mSend->setLabel(sending ? std::string("Sending...") : std::string("Send"));
}

void WolfFloaterBugReport::onSend()
{
    if (mSending) return;
    // Checked on Send, not while typing; the box below says what is missing and the cursor goes to it.
    const std::string category = mCategory->getSelectedValue().asString();
    std::string subject = mSubject->getText();
    std::string body = mBody->getText();
    LLStringUtil::trim(subject);
    LLStringUtil::trim(body);
    if (category.empty())
    {
        setStatus("Not sent: please choose what the problem is about.", EStatus::ERR);
        mCategory->setFocus(true);
        return;
    }
    if (subject.empty())
    {
        setStatus("Not sent: please give the report a short subject.", EStatus::ERR);
        mSubject->setFocus(true);
        return;
    }
    if (body.empty())
    {
        setStatus("Not sent: please say what happened, so we know what to look at.", EStatus::ERR);
        mBody->setFocus(true);
        return;
    }
    // The request carries this login's agent and session ids: only a Wolf Territories login, only to Wolf's own site.
    if (!WolfGrid::isWolfTerritories())
    {
        setStatus("Not sent: Report a Problem is for Wolf Territories residents.", EStatus::ERR);
        return;
    }

    LLSD fields;
    fields["category"] = category;
    fields["subject"] = subject;
    fields["body"] = body;
    fields["viewer"] = LLVersionInfo::instance().getChannelAndVersion();
    fields["os"] = LLOSInfo::instance().getOSString();
    fields["gpu"] = gGLManager.getRawGLString();
    if (LLViewerRegion* region = gAgent.getRegion())
    {
        fields["region"] = region->getName();
        fields["regionid"] = region->getRegionID().asString();
        const LLVector3 pos = gAgent.getPositionAgent();
        fields["position"] = llformat("%d, %d, %d", (S32)ll_round(pos.mV[VX]), (S32)ll_round(pos.mV[VY]), (S32)ll_round(pos.mV[VZ]));
    }

    mTicketUrl.clear();
    mOpenTicket->setVisible(false);
    setSending(true);
    const LLHandle<LLFloater> handle = getHandle();
    if (!mAttach->get() || mLogs.empty())
    {
        setStatus("Sending your report...", EStatus::INFO);
        LLCoros::instance().launch("WolfBugReport", [handle, fields]() { sendCoro(handle, fields, {}); });
        return;
    }

    setStatus("Preparing your logs (taking out private details and compressing them)...", EStatus::INFO);
    const std::vector<LogFile> logs = mLogs;
    const std::vector<std::string> secrets = { gAgentSessionID.asString(), gAgent.getSecureSessionID().asString() };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    auto launch = [handle, fields](std::vector<std::pair<std::string, std::string>> parts)
    {
        if (WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get()))
        {
            self->setStatus("Sending your report and logs...", EStatus::INFO);
        }
        LLCoros::instance().launch("WolfBugReport", [handle, fields, parts]() { sendCoro(handle, fields, parts); });
    };
    if (!main_queue || !general_queue
        || !main_queue->postTo(general_queue, [logs, secrets]() { return prepare_logs(logs, secrets); }, launch))
    {
        // No worker thread (should not happen once logged in): do it here; a short pause beats sending no logs.
        launch(prepare_logs(logs, secrets));
    }
}

// static
void WolfFloaterBugReport::sendCoro(LLHandle<LLFloater> handle, LLSD fields, std::vector<std::pair<std::string, std::string>> parts)
{
    // multipart/form-data (RFC 7578), CRLF line ends. The boundary is random so no log line can contain it.
    LLUUID rnd;
    rnd.generate();
    const std::string boundary = "----WolfBugReport" + rnd.asString();
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    auto put = [&raw](const std::string& s) { raw->append(s.data(), s.size()); };
    for (LLSD::map_const_iterator it = fields.beginMap(); it != fields.endMap(); ++it)
    {
        put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + it->first + "\"\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\n" + it->second.asString() + "\r\n");
    }
    for (const auto& p : parts)
    {
        put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"logs[]\"; filename=\"" + p.first + "\"\r\n"
            "Content-Type: application/gzip\r\n\r\n");
        put(p.second);
        put("\r\n");
    }
    put("--" + boundary + "--\r\n");

    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfBugReport", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(180);
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "multipart/form-data; boundary=" + boundary);
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLSD result = adapter->postRawAndSuspend(request, REPORT_API, raw, options, headers);

    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    LLSD reply;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
        if (!ec) reply = LlsdFromJson(v);
    }
    std::string error;
    if (!reply.isMap() || !reply["success"].asBoolean())
    {
        error = json_string(reply, "error");
        if (error.empty())
        {
            error = "the website did not answer properly (" + status.toString() + "). Please try again in a moment, "
                    "or raise a ticket on wolf-grid.com.";
        }
        LL_WARNS("WolfBugReport") << "report not sent: " << status.toString() << " " << error << LL_ENDL;
    }
    WolfFloaterBugReport* self = static_cast<WolfFloaterBugReport*>(handle.get());
    if (!self) return;
    self->onSent(reply, error);
}

void WolfFloaterBugReport::onSent(const LLSD& reply, const std::string& error)
{
    setSending(false);
    if (!error.empty())
    {
        // Stays until the next Send; the text can be selected and copied (a read-only text editor).
        setStatus("Not sent: " + error, EStatus::ERR);
        return;
    }
    const S32 ticket = reply["ticket"].asInteger();
    mTicketUrl = json_string(reply, "url");
    std::string t = llformat("Sent. Your ticket number is #%d. We will reply on wolf-grid.com and by email.", ticket);
    const S32 stored = reply["logsStored"].asInteger();
    if (stored > 0) t += llformat(" %d log file%s attached.", stored, stored == 1 ? "" : "s");
    const LLSD& refused = reply["logsRefused"];
    if (refused.isArray() && refused.size() > 0)
    {
        t += " Some logs were not kept:";
        for (const LLSD& r : llsd::inArray(refused)) t += " " + r.asString();
    }
    if (!mTicketUrl.empty()) t += "\n" + mTicketUrl;
    setStatus(t, EStatus::OK);
    mOpenTicket->setVisible(!mTicketUrl.empty());
    // A second click on Send must not raise the same ticket twice: the form is emptied (Send then asks for a subject).
    mSubject->setText(LLStringUtil::null);
    mBody->setText(LLStringUtil::null);
    mCategory->selectFirstItem();
}
