/**
 * @file wolflogscrub.cpp
 * @brief WolfViewer: take private details out of a viewer log, keep its tail, gzip it. See wolflogscrub.h.
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

// [CRASH REPORTS 2026-10-28] Moved here unchanged from wolffloaterbugreport.cpp (the Flea Report, 2026-10-24) so the
// automatic crash report (wolfcrashreport.cpp) scrubs and packs its log with exactly the same code. The one change:
// read_tail takes its cap as an argument (the Flea Report passes its MAX_LOG_BYTES, 4 MB, as before).

#include "llviewerprecompiledheaders.h"

#include "wolflogscrub.h"

#include <regex>

#include "llfile.h"

#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif

namespace
{
    // Lines longer than this are scrubbed a piece at a time (std::regex recurses on long matches); the server
    // scrubs the whole file again in one go (ticketlib.php tkScrubLogText), so a piece edge cannot leak anything.
    const size_t SCRUB_CHUNK = 16 * 1024;

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
}

namespace WolfLogScrub
{
    std::string size_text(S64 n)
    {
        if (n >= 1024 * 1024) return llformat("%.1f MB", (F64)n / (1024.0 * 1024.0));
        if (n >= 1024) return llformat("%d KB", (S32)(n / 1024));
        return llformat("%d bytes", (S32)n);
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

    // The last max_bytes of a file, from the start of a line, with a note on top when it was cut.
    bool read_tail(const std::string& path, std::string& out, S64 max_bytes)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0) return false;
        const std::streamoff from = size > max_bytes ? size - max_bytes : 0;
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
                            size_text(max_bytes).c_str(), size_text((S64)size).c_str()) + data;
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
        zs.avail_in = (uInt)in.size();   // callers cap their input well under 4 GB (uInt)
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
}
