/**
 * @file wolffloaterexperiences.cpp
 * @brief WolfViewer: Experiences - the welcome chooser. See wolffloaterexperiences.h.
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

// [WolfViewer 2026-10-14] Contract: wolfstorm/tools/EXPERIENCES_V1.md.

#include "llviewerprecompiledheaders.h"

#include "wolffloaterexperiences.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"         // [WolfViewer 2026-10-14] builder tools (WolfFloaterExperienceSubmit)
#include "lllineeditor.h"
#include "llparcel.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llspinctrl.h"
#include "lltexturectrl.h"
#include "llviewerparcelmgr.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "lliconctrl.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llscrollcontainer.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llregionhandle.h"     // [WolfViewer 2026-10-14] from_region_handle (Go teleports directly)
#include "llslurl.h"
#include "llworldmapmessage.h"
#include "lldate.h"             // [WolfViewer 2026-10-14] What's on: LLDate::now (the feed sent no "now")
#include "lluicolortable.h"     // [WolfViewer 2026-10-14] What's on: AccentColor for LIVE NOW
#include "llurlaction.h"        // [WolfViewer 2026-10-14] What's on: follow an off-grid event's link
#include "wolfgrid.h"
#include "llappviewer.h"        // [WolfViewer 2026-10-14] gDisconnected (upload_new_resource does nothing then)
#include "llagentbenefits.h"    // [WolfViewer 2026-10-14] builder tools "Upload a picture...": the upload cost
#include "llfloaterperms.h"     //   the default upload permissions
#include "llstatusbar.h"        //   can_afford_transaction
#include "llviewermenufile.h"   //   LLFilePickerReplyThread, upload_new_resource
#include <ctime>

namespace
{
    // Source: EXPERIENCES_V1.md "Data" - GET, public.
    const char* const EXPERIENCES_URL = "https://wolfstorm.app/php/experiences.php";
    // Source: EXPERIENCES_V1.md - the script sends Cache-Control max-age=300; the window asks again
    // when what it holds is older than that.
    const F64 FRESH_SECONDS = 300.0;
    // Source: EXPERIENCES_V1.md - at most 6 categories. [WolfViewer 2026-10-14] Source: EXPERIENCES_V1.md
    // "Builder tools" API GET: "The whole list may now hold up to 60 experiences (clients: raise their cap
    // to 60)" - the curated entries plus the approved community ones (was 24).
    const S32 MAX_EXPERIENCES = 60;
    const S32 MAX_CATEGORIES = 6;
    const char* const SETTING_NO_SHOW = "WolfExperiencesNoShowOnArrive";
    const char* const FLOATER_NAME = "wolf_experiences";
    const char* const SUBMIT_FLOATER_NAME = "wolf_experience_submit";   // [WolfViewer 2026-10-14] builder tools

    // Card geometry (panel_wolf_experience_card.xml holds the looks, these the places). Featured
    // cards are taller with a bigger picture (contract: "shown first, larger").
    const S32 CARD_W = 500;
    const S32 CARD_H = 108;
    const S32 CARD_H_FEATURED = 148;
    const S32 CARD_GAP = 8;
    const S32 PAD = 6;
    const S32 GO_W = 70;
    const S32 GO_H = 28;
    const S32 CHIP_H = 22;      // the chips' height in floater_wolf_experiences.xml
    const S32 CHIP_GAP = 4;
    const S32 CHIP_MARGIN = 12; // the window's side margin (the chips' left in the layout file)

    // ---- the shared list (main thread only: coroutines run on it) ----
    LLSD sReply;                  // the last good reply, cleaned (see clean())
    F64 sReplyAt = -1.0;          // when it arrived (LLFrameTimer total seconds); < 0 = never
    bool sFetching = false;
    std::string sError;           // the last fetch's failure in words; empty when it worked

    // ---- this login ----
    LLUUID sSession;              // the login the state below (and the list, see newAccount) belongs to
    bool sShownThisSession = false;   // opened this login, by itself or by hand
    bool sAutoDecided = false;    // the after-login decision has been made this login
    bool sTeleportedFirst = false;    // a teleport began before it was made: then it never opens by itself
    S32 sAutoTries = 0;           // fetches started by the auto-open this login
    F64 sNextAutoTry = 0.0;

    // A Go in flight: the notice to show once the teleport ends in its region.
    struct PendingArrival
    {
        bool mActive = false;
        bool mStarted = false;    // a teleport has begun since Go
        F64 mAt = 0.0;
        std::string mRegion, mTitle, mArrive;
    };
    PendingArrival sPending;

    // Source: wolffloaterwatchwolf.cpp:144-151 reply_of / wolfcave.cpp:96-103 json_to_llsd -
    // the raw body through boost::json into LLSD (llsdjson.h LlsdFromJson).
    LLSD reply_of(const LLSD& result)
    {
        if (!result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW)) return LLSD();
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(std::string(raw.begin(), raw.end()), ec);
        return ec ? LLSD() : LlsdFromJson(v);
    }

    bool sameRegion(const std::string& a, const std::string& b)
    {
        // OpenSim looks region names up without regard to case; so does this.
        return !a.empty() && LLStringUtil::compareInsensitive(a, b) == 0;
    }

    // The site validates the file (contract), but a viewer never trusts what it is sent: keep only
    // entries that can be shown and teleported to, at most 24, featured first (file order kept
    // otherwise), and at most 6 categories.
    LLSD clean(const LLSD& reply)
    {
        LLSD out = LLSD::emptyMap();
        // Source: EXPERIENCES_V1.md "newAccount" - present only when the identity sent with the GET
        // verified; true = this account was created within the last 7 days. Absent = not new.
        out["newAccount"] = reply.has("newAccount") && reply["newAccount"].isBoolean() && reply["newAccount"].asBoolean();
        out["categories"] = LLSD::emptyArray();
        for (const LLSD& c : llsd::inArray(reply["categories"]))
        {
            if (out["categories"].size() >= MAX_CATEGORIES) break;
            if (c.isMap() && !c["id"].asString().empty() && !c["title"].asString().empty())
            {
                out["categories"].append(c);
            }
        }
        LLSD featured = LLSD::emptyArray(), rest = LLSD::emptyArray();
        for (const LLSD& e : llsd::inArray(reply["experiences"]))
        {
            if (featured.size() + rest.size() >= MAX_EXPERIENCES) break;
            if (!e.isMap() || e["id"].asString().empty() || e["title"].asString().empty()
                || e["region"].asString().empty())
            {
                continue;
            }
            (e["featured"].asBoolean() ? featured : rest).append(e);
        }
        out["experiences"] = LLSD::emptyArray();
        for (const LLSD& e : llsd::inArray(featured)) out["experiences"].append(e);
        for (const LLSD& e : llsd::inArray(rest)) out["experiences"].append(e);
        return out;
    }

    bool haveReply() { return sReplyAt >= 0.0; }

    // Source: wolffloaterwatchwolf.cpp:243-264 fetchCoro - HttpCoroutineAdapter::getRawAndSuspend
    // with WolfGrid::makeVerifiedHttpOptions. The identity pair goes with it, as wolfcave.cpp:137-147
    // wolfRequest sends it, so the site can say whether this account is new (contract "newAccount");
    // the list itself is public. Callers only fetch when logged in to Wolf Territories
    // (WolfGrid::isWolfTerritories), so the session id never goes to another grid's site.
    void fetchCoro(LLUUID session)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfExperiences", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", session.asString());
        LLSD result = adapter->getRawAndSuspend(request, EXPERIENCES_URL, options, headers);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const LLSD reply = reply_of(result);
        sFetching = false;
        if (session != gAgentSessionID)
        {
            return;    // asked for an earlier login: its newAccount is not this one's
        }
        if (status && reply.isMap() && reply["success"].asBoolean())
        {
            sReply = clean(reply);
            sReplyAt = LLFrameTimer::getTotalSeconds();
            sError.clear();
        }
        else
        {
            // A broken file is served as HTTP 503 {"success":false,...} (contract); the raw adapter
            // keeps no body for an error status (wolfcave.cpp:184-186), so the words come from here.
            sError = "Experiences are not available right now";
            if (reply.isMap() && !reply["error"].asString().empty())
            {
                sError += " (" + reply["error"].asString() + ")";
            }
            else if (!status)
            {
                sError += " (" + status.toString() + ")";
            }
            sError += ".";
            LL_WARNS("WolfExperiences") << "fetch failed: " << status.toString() << LL_ENDL;
        }
        WolfFloaterExperiences::dataChanged();
    }

    void startFetch()
    {
        if (sFetching || !WolfGrid::isWolfTerritories()) return;
        sFetching = true;
        const LLUUID session = gAgentSessionID;
        LLCoros::instance().launch("WolfExperiencesFetch", [session]() { fetchCoro(session); });
    }

    // ---- [WolfViewer 2026-10-14] What's on: the events feed. Contract: wolfstorm/tools/EVENTS_V1.md ----

    // Source: EVENTS_V1.md "Feed" - GET, public, Cache-Control: public, max-age=120. Public, so no
    // identity goes with it.
    const char* const EVENTS_URL = "https://wolfstorm.app/php/events.php";
    const F64 EVENTS_FRESH_SECONDS = 120.0;
    // Source: EVENTS_V1.md "Feed" - "at most 30".
    const S32 MAX_EVENTS = 30;
    const F64 COUNTDOWN_SECONDS = 30.0;     // the "Starts in ..." lines are rewritten this often
    const S32 EVENT_CARD_H = 120;           // title, time line, two lines of description, place + Go
    const S32 EVENT_ICON = 32;              // icon_event.tga is 16 x 16: drawn at twice that, no bigger
    const char* const EVENT_ICON_NAME = "icon_event.tga";   // skins/default/textures/textures.xml:1097
    const char* const EVENTS_SUBTITLE = "What's on across Wolf Territories. Times are your own local time, not SLT.";

    LLSD sEvents = LLSD::emptyArray();  // the last good feed's events, cleaned (cleanEvents)
    F64 sEventsAt = -1.0;               // when it arrived (LLFrameTimer total seconds); < 0 = never
    S64 sEventsServerNow = 0;           // the feed's "now" (unix seconds UTC) at sEventsAt
    bool sEventsFetching = false;
    std::string sEventsError;

    bool haveEvents() { return sEventsAt >= 0.0; }

    // Now, in unix seconds UTC, on the feed's own clock: its "now" plus the time since it came. A wrong
    // clock on this computer then cannot move LIVE NOW or the countdowns.
    S64 eventsNow()
    {
        return sEventsServerNow + (S64)(LLFrameTimer::getTotalSeconds() - sEventsAt);
    }

    // Source: EVENTS_V1.md "Feed" - starts/ends are unix seconds UTC. Read as real: llsdjson.cpp:50-52
    // LlsdFromJson makes every JSON integer an LLSD from int64_t.
    S64 secondsOf(const LLSD& v) { return (S64)v.asReal(); }

    // The site checks the data (contract); a viewer still keeps only what it can show: a title, a start,
    // an end after it, not yet ended, at most 30, in the feed's order (soonest first).
    LLSD cleanEvents(const LLSD& reply, S64 now)
    {
        LLSD out = LLSD::emptyArray();
        for (const LLSD& e : llsd::inArray(reply["events"]))
        {
            if ((S32)out.size() >= MAX_EVENTS) break;
            if (!e.isMap() || e["id"].asString().empty() || e["title"].asString().empty()) continue;
            const S64 starts = secondsOf(e["starts"]), ends = secondsOf(e["ends"]);
            if (starts <= 0 || ends <= starts || ends <= now) continue;
            out.append(e);
        }
        return out;
    }

    // Source: fetchCoro above (wolffloaterwatchwolf.cpp:243-264) - the same raw GET, without the identity
    // pair: the feed is public (EVENTS_V1.md).
    void eventsFetchCoro(LLUUID session)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEvents", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        LLSD result = adapter->getRawAndSuspend(request, EVENTS_URL, options, headers);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        const LLSD reply = reply_of(result);
        sEventsFetching = false;
        if (session != gAgentSessionID)
        {
            return;    // asked for an earlier login (maybe on another grid): not this one's
        }
        if (status && reply.isMap() && reply["success"].asBoolean() && reply["events"].isArray())
        {
            const S64 feed_now = secondsOf(reply["now"]);
            sEventsServerNow = feed_now > 0 ? feed_now : (S64)LLDate::now().secondsSinceEpoch();
            sEventsAt = LLFrameTimer::getTotalSeconds();
            sEvents = cleanEvents(reply, sEventsServerNow);
            sEventsError.clear();
        }
        else
        {
            sEventsError = "What's on could not be loaded";
            if (reply.isMap() && !reply["error"].asString().empty())
            {
                sEventsError += " (" + reply["error"].asString() + ")";
            }
            else if (!status)
            {
                sEventsError += " (" + status.toString() + ")";
            }
            sEventsError += ".";
            LL_WARNS("WolfExperiences") << "events fetch failed: " << status.toString() << LL_ENDL;
        }
        WolfFloaterExperiences::eventsChanged();
    }

    void startEventsFetch()
    {
        if (sEventsFetching || !WolfGrid::isWolfTerritories()) return;
        sEventsFetching = true;
        const LLUUID session = gAgentSessionID;
        LLCoros::instance().launch("WolfEventsFetch", [session]() { eventsFetchCoro(session); });
    }

    // A time as this computer's clock shows it: "Today 20:00", "Tomorrow 9:30 PM", "Thu 20:00".
    // Source: llexperiencelog.cpp:87-88 - strftime over localtime(), the viewer's way to show local time.
    // localtime() per event, not LLStringOps::getLocalTimeOffset (fixed when it is set up,
    // llstring.cpp:1110-1123), so an event after a daylight-saving change shows its real local hour.
    // Source: fsfloaternearbychat.cpp:285 / llconversationlog.cpp:126 - the Use24HourClock setting;
    // llstring.h:161-168 LLStringOps::sWeekDayShortList / sAM / sPM, the viewer's own words for them.
    std::string localWhen(S64 when, S64 now)
    {
        time_t t = (time_t)when;
        time_t n = (time_t)now;
        const struct tm* p = localtime(&n);
        if (!p) return std::string();
        struct tm tomorrow = *p;
        const struct tm today = *p;
        p = localtime(&t);
        if (!p) return std::string();
        const struct tm at = *p;
        // Tomorrow's date: midday of the next day, normalised by mktime (tm_isdst -1 = let it decide).
        tomorrow.tm_mday += 1;
        tomorrow.tm_hour = 12;
        tomorrow.tm_min = tomorrow.tm_sec = 0;
        tomorrow.tm_isdst = -1;
        mktime(&tomorrow);

        std::string day;
        if (at.tm_year == today.tm_year && at.tm_yday == today.tm_yday)
        {
            day = "Today";
        }
        else if (at.tm_year == tomorrow.tm_year && at.tm_yday == tomorrow.tm_yday)
        {
            day = "Tomorrow";
        }
        else if (LLStringOps::sWeekDayShortList.size() == 7)
        {
            day = LLStringOps::sWeekDayShortList[at.tm_wday];
        }
        else
        {
            char buf[32];
            strftime(buf, sizeof(buf), "%a", &at);
            day = buf;
        }

        static LLCachedControl<bool> use_24h(gSavedSettings, "Use24HourClock", true);
        if (use_24h)
        {
            return day + llformat(" %02d:%02d", at.tm_hour, at.tm_min);
        }
        const S32 h12 = at.tm_hour % 12 == 0 ? 12 : at.tm_hour % 12;
        const std::string ampm = at.tm_hour < 12 ? (LLStringOps::sAM.empty() ? std::string("AM") : LLStringOps::sAM)
                                                 : (LLStringOps::sPM.empty() ? std::string("PM") : LLStringOps::sPM);
        return day + llformat(" %d:%02d ", h12, at.tm_min) + ampm;
    }

    // The card's time line. Source: EVENTS_V1.md "Clients show times in the viewer's local time with
    // 'LIVE NOW' / 'starts in 2 h 10 min' / weekday+time"; live = starts <= now < ends.
    std::string eventWhen(S64 starts, S64 ends, S64 now, bool& live)
    {
        live = false;
        if (now >= ends)
        {
            return "Ended";
        }
        if (now >= starts)
        {
            live = true;
            return "LIVE NOW  \xC2\xB7  started " + localWhen(starts, now) + " your time";
        }
        const S64 mins = (starts - now + 59) / 60;    // whole minutes, rounded up
        if (mins >= 12 * 60)
        {
            return localWhen(starts, now) + " your time";
        }
        std::string in;
        if (starts - now < 60)
        {
            in = "Starts in under a minute";
        }
        else if (mins < 60)
        {
            in = llformat("Starts in %d min", (S32)mins);
        }
        else if (mins % 60 == 0)
        {
            in = llformat("Starts in %d h", (S32)(mins / 60));
        }
        else
        {
            in = llformat("Starts in %d h %d min", (S32)(mins / 60), (S32)(mins % 60));
        }
        return in + "  \xC2\xB7  " + localWhen(starts, now) + " your time";
    }

    // What an event without a region on this grid can still do with its "link" (the original address,
    // EVENTS_V1.md). hop:// and secondlife:// go to the viewer's own SLURL dispatch (a hypergrid hop is
    // handled there, or refused with the viewer's own "can't teleport to that grid" notice); a web link
    // (e.g. https://opensimworld.com/hop/...) opens the way a clicked link in chat does. Anything else
    // (a bare "host:port:Region" text, other schemes) is not followed.
    enum EEventLink { LINK_NONE, LINK_SLURL, LINK_WEB };
    EEventLink eventLinkKind(const std::string& link)
    {
        std::string l = link;
        LLStringUtil::trim(l);
        LLStringUtil::toLower(l);
        if (l.rfind("hop://", 0) == 0 || l.rfind("secondlife://", 0) == 0) return LINK_SLURL;
        if (l.rfind("https://", 0) == 0 || l.rfind("http://", 0) == 0) return LINK_WEB;
        return LINK_NONE;
    }

    // A category without a picture of its own: an icon the skin already has, chosen by the words in
    // the category id. Display only - unknown ids get the Places icon.
    std::string categoryIcon(const std::string& category)
    {
        std::string c = category;
        LLStringUtil::toLower(c);
        auto has = [&c](const char* w) { return c.find(w) != std::string::npos; };
        if (has("fly") || has("ride") || has("travel") || has("drive")) return "Command_Destinations_Icon";
        if (has("roleplay") || has("game") || has("play")) return "Command_WolfRoleplay_Icon";
        if (has("build") || has("sandbox") || has("create")) return "Command_Build_Icon";
        if (has("explore") || has("tour") || has("sight")) return "Command_Map_Icon";
        if (has("social") || has("meet") || has("people") || has("event") || has("music")) return "Command_People_Icon";
        return "Command_Places_Icon";
    }

    // Lets Escape through to the floater. LLPanel::handleKeyHere answers Escape by dropping focus
    // and calling it handled (llpanel.cpp:278-282), and keys travel up the parents
    // (llview.cpp:1061-1065 LLView::handleKey), so a plain panel between a Go button and the floater
    // would swallow it. Used for the scrolled holder and each card.
    class PassEscapePanel : public LLPanel
    {
    public:
        PassEscapePanel() : LLPanel() {}
        bool handleKeyHere(KEY key, MASK mask) override
        {
            if (key == KEY_ESCAPE) return false;
            return LLPanel::handleKeyHere(key, mask);
        }
    };

    // Rect from the top-left of a parent of the given height (LLRect is bottom-up).
    LLRect topLeft(S32 parent_h, S32 left, S32 top, S32 w, S32 h)
    {
        return LLRect(left, parent_h - top, left + w, parent_h - top - h);
    }

    // ---- [WolfViewer 2026-10-14] builder tools: requests to php/experiences.php with identity ----

    // Source: wolfcave.cpp:137-163 wolfRequest - the same helper: verified host, the X-Wolf-Agent /
    // X-Wolf-Session pair (EXPERIENCES_V1.md "Builder tools": identity required for ?mine, ?review and
    // every POST), JSON body, and no automatic retry of a POST (llcorehttp retries 5 times by default and
    // counts a timeout as retryable - a submit sent twice would count twice against the daily limit).
    // Source: llcorehttp/httpoptions.h:104 HttpOptions::setRetries. Callers check
    // WolfGrid::isWolfTerritories() first, so the session id never goes to another grid's site.
    LLSD xpRequest(const std::string& url, const LLSD* body)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfExperienceTools", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(20);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        if (!body)
        {
            return adapter->getRawAndSuspend(request, url, options, headers);
        }
        options->setRetries(0);
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        const std::string text = boost::json::serialize(LlsdToJson(*body));
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(text.data(), text.size());
        return adapter->postRawAndSuspend(request, url, raw, options, headers);
    }

    // Source: wolfcave.cpp:166-178 replyOf - the HTTP status as a number (0 = no answer) and the JSON body.
    S32 xpReplyOf(const LLSD& result, LLSD& reply, std::string& transport)
    {
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        reply = reply_of(result);
        transport = st ? std::string() : st.toString();
        return st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
    }

    // The words for a failed request. User-facing refusals come as HTTP 200 + success:false with the words
    // in "error" (EXPERIENCES_V1.md "Builder tools" API) and are shown as they are. The raw adapter keeps no
    // body for an error status (wolfcave.cpp:180-183), so each status the contract names gets its own line:
    // 401 bad identity, 429 too many (3 pending or live, 10 a day: contract "Limits").
    std::string xpError(S32 status, const LLSD& reply, const std::string& transport)
    {
        if (reply.isMap() && !reply["error"].asString().empty()) return reply["error"].asString();
        switch (status)
        {
            case 200: return "The grid refused it without saying why.";
            case 401: return "The grid could not confirm you are signed in. Log out and back in, then try again.";
            case 403: return "The grid refused it: you do not have the rights for this.";
            case 429: return "Too many experiences: you may have 3 waiting or live at once, and send 10 a day. Try again later.";
            case 503: return "The experiences service is not answering. Try again in a moment.";
            case 0:   return "The experiences service could not be reached (" + transport + "). Try again in a moment.";
            default:  return llformat("The experiences service answered HTTP %d. Try again in a moment.", status);
        }
    }

    // A submission's region name. The contract's submit and public list call it "region"; the table column
    // is region_name (EXPERIENCES_V1.md "Storage"), so a ?mine / ?review row is read under either name.
    std::string regionOf(const LLSD& e)
    {
        return e.has("region") ? e["region"].asString() : e["region_name"].asString();
    }

    // [WolfViewer 2026-10-14] Go, shared by the chooser's cards and the admins' "Waiting for review": look
    // the region up by name and teleport to the entry's position at once; the arrival notice follows.
    // Source: llurldispatcher.cpp:490-497 teleport_via_slapp (sendNamedRegionRequest, teleport=true) and
    // :306-332 regionHandleCallback (from_region_handle(handle) + position, gAgent.teleportViaLocation).
    void goToExperience(const LLSD& e)
    {
        const std::string region = regionOf(e);
        if (region.empty()) return;
        // Region metres; the region clamps anything outside it (z 0 = the ground, contract).
        const S32 x = llmax(0, e["x"].asInteger()), y = llmax(0, e["y"].asInteger()), z = llmax(0, e["z"].asInteger());

        sPending = PendingArrival();
        // [WolfViewer 2026-10-14] Always watched now (was: only with an "arrive" line), so a What's on Go,
        // which has none, also gets "the region was not found" instead of nothing happening. The arrival
        // notice itself still shows only when there is an "arrive" line (idle).
        sPending.mActive = true;
        sPending.mAt = LLFrameTimer::getTotalSeconds();
        sPending.mRegion = region;
        sPending.mTitle = e["title"].asString();
        sPending.mArrive = e["arrive"].asString();

        const LLVector3d local((F64)x, (F64)y, (F64)z);
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(region,
            [local](U64 handle, const std::string&, const LLUUID&, bool teleport)
            {
                if (!teleport || handle == 0) return;
                gAgent.teleportViaLocation(from_region_handle(handle) + local);
            },
            LLSLURL(region, LLVector3((F32)x, (F32)y, (F32)z)).getSLURLString(),
            true);
    }

    // Words for a submission's status (EXPERIENCES_V1.md "Storage": pending / live / rejected / withdrawn).
    std::string statusWords(const std::string& status)
    {
        if (status == "pending") return "Waiting for review";
        if (status == "live") return "Live - shown in Wolf Experiences";
        if (status == "rejected") return "Not approved";
        if (status == "withdrawn") return "Withdrawn";
        return status;
    }

    // The title of a category id in the chooser's list (the id itself when it is not there).
    std::string categoryTitle(const std::string& id)
    {
        for (const LLSD& c : llsd::inArray(sReply["categories"]))
        {
            if (c["id"].asString() == id) return c["title"].asString();
        }
        return id;
    }
}

// ---------------------------------------------------------------------------------------------------
// Auto-open and the arrival notice
// ---------------------------------------------------------------------------------------------------

void WolfExperiences::idle()
{
    // Source: wolfinvisible.cpp WolfInvisible::idle - the same "logged in and in a region" test.
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion())
    {
        return;
    }
    // A new login in the same run (llstartup.cpp makes a new session id): start again, and forget
    // the list - its newAccount belonged to the previous login.
    if (gAgentSessionID != sSession)
    {
        sSession = gAgentSessionID;
        sReply = LLSD();
        sReplyAt = -1.0;
        sError.clear();
        sShownThisSession = false;
        sAutoDecided = false;
        sTeleportedFirst = false;
        sAutoTries = 0;
        sNextAutoTry = 0.0;
        sPending = PendingArrival();
        // [WolfViewer 2026-10-14] What's on: the feed is per login too (a new login may be another grid).
        sEvents = LLSD::emptyArray();
        sEventsAt = -1.0;
        sEventsServerNow = 0;
        sEventsError.clear();
    }
    WolfFloaterExperiences::tick();   // [WolfViewer 2026-10-14] What's on countdowns, every 30 s while shown
    const F64 now = LLFrameTimer::getTotalSeconds();
    const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;

    // After Go: wait for a teleport to start (the SLURL handler first asks "Teleport to ...?" and
    // looks the region up), then for it to end; in the card's region, show its "arrive" line.
    // Source: llagent.cpp:4840-4906 teleportCore sets TELEPORT_START, or TELEPORT_LOCAL for a
    // teleport inside the region the agent is in; both end at TELEPORT_NONE.
    if (sPending.mActive)
    {
        if (teleporting)
        {
            sPending.mStarted = true;
        }
        else if (sPending.mStarted)
        {
            if (!sPending.mArrive.empty() && sameRegion(sPending.mRegion, gAgent.getRegion()->getName()))
            {
                LLSD args;
                args["TITLE"] = sPending.mTitle;
                args["ARRIVE"] = sPending.mArrive;
                LLNotificationsUtil::add("WolfExperienceArrived", args);
            }
            sPending = PendingArrival();
        }
        else if (now - sPending.mAt > 20.0)
        {
            // [WolfViewer 2026-10-14] Go teleports at once (no "Teleport to ...?" any more), so no teleport
            // 20 s after Go means the grid did not find the region: say so instead of nothing happening.
            LLSD args;
            args["MESSAGE"] = "Could not take you to " + sPending.mTitle + ": the region " + sPending.mRegion
                            + " was not found or is not running right now. Try again in a little while.";
            LLNotificationsUtil::add("GenericAlert", args);
            sPending = PendingArrival();
        }
    }

    // [WolfViewer 2026-10-14] Paul: "when a user first logs in they should SHOW experiences when they
    // have just signed up" (EXPERIENCES_V1.md "When the chooser opens" 1): right after login, whatever
    // region they land in, when the site says newAccount - once per login, unless the person said not
    // to. Never over a teleport they started: one begun before the answer came settles it as "no".
    if (sAutoDecided || sShownThisSession)
    {
        return;
    }
    if (teleporting)
    {
        sTeleportedFirst = true;
        return;
    }
    static LLCachedControl<bool> no_show(gSavedSettings, SETTING_NO_SHOW, false);
    if (no_show)
    {
        sAutoDecided = true;
        return;
    }
    // Never on another grid: only once this region has itself said it is Wolf Territories
    // (wolfgrid.h isOnWolfTerritoriesConfirmed); until its features arrive, the next frames ask again.
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        return;
    }
    if (!haveReply())
    {
        // A failed fetch is tried again a minute later, three times a login at most.
        if (!sFetching && now >= sNextAutoTry && sAutoTries < 3)
        {
            ++sAutoTries;
            sNextAutoTry = now + 60.0;
            startFetch();
        }
        return;
    }
    sAutoDecided = true;
    if (sReply["newAccount"].asBoolean() && !sTeleportedFirst)
    {
        // Focus to it, so Tab reaches the cards and Escape closes it at once.
        LLFloaterReg::showInstance(FLOATER_NAME, LLSD(), true);
    }
}

// ---------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------

WolfFloaterExperiences::WolfFloaterExperiences(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterExperiences::postBuild()
{
    mScroll = getChild<LLScrollContainer>("cards_scroll");
    mStatus = getChild<LLTextBox>("status");
    mRetry = getChild<LLButton>("retry");
    mDontShow = getChild<LLCheckBoxCtrl>("dont_show");

    // The scrolled view, made here so Escape passes through it (PassEscapePanel).
    mCards = new PassEscapePanel();
    mCards->setRect(LLRect(0, 0, CARD_W, 0));
    mCards->setFollows(FOLLOWS_LEFT | FOLLOWS_TOP);
    mScroll->addChild(mCards);

    // Where the chip rows start and where the card list ends, as the layout file places them
    // (buildChips wraps the chips from here and stretches the list up to the last row).
    mChipLeft0 = getChild<LLButton>("cat_0")->getRect().mLeft;
    mChipTop0 = getChild<LLButton>("cat_0")->getRect().mTop;
    mScrollBottom = mScroll->getRect().mBottom;
    for (S32 i = 0; i <= MAX_CATEGORIES; ++i)
    {
        LLButton* chip = getChild<LLButton>(llformat("cat_%d", i));
        chip->setCommitCallback([this, i](LLUICtrl*, const LLSD&)
        {
            if (i < (S32)mChipIds.size()) mCategory = mChipIds[i];
            refreshView();
        });
        mChips.push_back(chip);
    }

    mDontShow->set(gSavedSettings.getBOOL(SETTING_NO_SHOW));
    mDontShow->setCommitCallback([](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL(SETTING_NO_SHOW, c->getValue().asBoolean());
    });
    mRetry->setCommitCallback([this](LLUICtrl*, const LLSD&) { load(true); });   // either section: load() asks for the one shown
    // [WolfViewer 2026-10-14] Builder tools: "Add your experience" opens the submit window (EXPERIENCES_V1.md
    // "In both viewers"); Wolf Territories only, like the list itself.
    mAddBtn = getChild<LLButton>("add_btn");
    mAddBtn->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance(SUBMIT_FLOATER_NAME, LLSD(), true); });
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });

    // [WolfViewer 2026-10-14] What's on (EVENTS_V1.md "Where it shows" 3): "Experiences" | "What's on" at the
    // top, the active one pressed. Plain buttons straight on the floater, so Tab reaches them and
    // Space / Enter press them, and Escape still closes (see PassEscapePanel).
    mSubtitle = getChild<LLTextBox>("subtitle");
    mSubtitleExperiences = mSubtitle->getText();
    mModeExperiences = getChild<LLButton>("mode_experiences");
    mModeWhatsOn = getChild<LLButton>("mode_whats_on");
    mModeExperiences->setCommitCallback([this](LLUICtrl*, const LLSD&) { showWhatsOn(false); });
    mModeWhatsOn->setCommitCallback([this](LLUICtrl*, const LLSD&) { showWhatsOn(true); });
    mModeExperiences->setToggleState(true);
    mModeWhatsOn->setToggleState(false);
    return true;
}

void WolfFloaterExperiences::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // Opened by hand before the after-login decision: that counts as this login's showing.
    sShownThisSession = true;
    mDontShow->set(gSavedSettings.getBOOL(SETTING_NO_SHOW));
    load(false);
}

bool WolfFloaterExperiences::handleKeyHere(KEY key, MASK mask)
{
    // Escape closes (Jakob's Law). Floaters do not do this by themselves: LLPanel::handleKeyHere
    // only drops focus on Escape (llpanel.cpp:278-282).
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        closeFloater();
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

// static
void WolfFloaterExperiences::dataChanged()
{
    WolfFloaterExperiences* self = LLFloaterReg::findTypedInstance<WolfFloaterExperiences>(FLOATER_NAME);
    if (self && !self->mWhatsOn)   // [WolfViewer 2026-10-14] What's on keeps its cards (and focus)
    {
        self->refreshView();
    }
    WolfFloaterExperienceSubmit::dataChanged();   // [WolfViewer 2026-10-14] its categories come from the list
}

void WolfFloaterExperiences::load(bool force)
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        refreshView();
        return;
    }
    if (mWhatsOn)
    {
        // [WolfViewer 2026-10-14] What's on: asked again when what is held is older than the feed's
        // max-age=120 (EVENTS_V1.md), each time the section opens; "Try again" always asks.
        const bool events_fresh = haveEvents() && LLFrameTimer::getTotalSeconds() - sEventsAt < EVENTS_FRESH_SECONDS;
        if (force || !events_fresh)
        {
            if (force)
            {
                sEventsError.clear();   // show "Finding what's on..." while it asks
            }
            startEventsFetch();
        }
        refreshView();
        return;
    }
    const bool fresh = haveReply() && LLFrameTimer::getTotalSeconds() - sReplyAt < FRESH_SECONDS;
    if (force || !fresh)
    {
        if (force)
        {
            sError.clear();   // "Try again": show "Finding experiences..." while it asks
        }
        startFetch();
    }
    refreshView();
}

void WolfFloaterExperiences::setStatus(const std::string& text, bool error, bool offer_retry)
{
    mStatus->setText(text);
    mStatus->setColor(error ? LLColor4(1.f, 0.45f, 0.4f, 1.f) : LLColor4(0.75f, 0.77f, 0.8f, 1.f));
    mStatus->setVisible(!text.empty());
    mRetry->setVisible(offer_retry);
}

void WolfFloaterExperiences::refreshView()
{
    mAddBtn->setVisible(WolfGrid::isOnWolfTerritories() && !mWhatsOn);   // [WolfViewer 2026-10-14] builder tools
    if (mWhatsOn)
    {
        // [WolfViewer 2026-10-14] What's on: no chips; the cards start under the subtitle.
        for (LLButton* chip : mChips) chip->setVisible(false);
        placeScroll(mChipTop0);
        if (!WolfGrid::isOnWolfTerritories())
        {
            clearCards();
            setStatus("What's on is for Wolf Territories only.", true, false);
            return;
        }
        if (!haveEvents())
        {
            clearCards();
            if (sEventsFetching || sEventsError.empty())
            {
                setStatus("Finding what's on\xE2\x80\xA6", false, false);
            }
            else
            {
                setStatus(sEventsError, true, true);
            }
            return;
        }
        // An older feed stays on show while a newer one is fetched; a failed refresh keeps it too.
        buildEventCards();
        return;
    }
    if (!WolfGrid::isOnWolfTerritories())
    {
        buildChips(LLSD::emptyArray());
        clearCards();
        setStatus("Experiences are on Wolf Territories only.", true, false);
        return;
    }
    if (!haveReply())
    {
        buildChips(LLSD::emptyArray());
        clearCards();
        if (sFetching || sError.empty())
        {
            setStatus("Finding experiences\xE2\x80\xA6", false, false);
        }
        else
        {
            setStatus(sError, true, true);
        }
        return;
    }
    // An older list stays on show while a newer one is fetched; a failed refresh keeps it too.
    buildChips(sReply["categories"]);
    buildCards();
}

void WolfFloaterExperiences::buildChips(const LLSD& categories)
{
    mChipIds.clear();
    mChipIds.push_back(std::string());
    for (const LLSD& c : llsd::inArray(categories))
    {
        mChipIds.push_back(c["id"].asString());
    }
    // A category that is no longer in the list: back to All.
    if (std::find(mChipIds.begin(), mChipIds.end(), mCategory) == mChipIds.end())
    {
        mCategory.clear();
    }
    // [WolfViewer 2026-10-14] Paul's screenshot: seven chips in one row ran far past the window's right
    // edge. They now wrap into rows inside the window, and the card list starts below the last row.
    // The chips' tooltips are gone: the label says what each does, and the "All" one hung over the first
    // card's picture.
    const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
    const S32 right_edge = getRect().getWidth() - CHIP_MARGIN;
    S32 left = mChipLeft0, top = mChipTop0, rows = 0;
    const bool any = categories.size() > 0;
    for (size_t i = 0; i < mChips.size(); ++i)
    {
        LLButton* chip = mChips[i];
        const bool used = any && i < mChipIds.size();
        chip->setVisible(used);
        if (!used) continue;
        const std::string label = i == 0 ? std::string("All") : categories[(S32)i - 1]["title"].asString();
        chip->setLabel(label);
        chip->setToolTip(std::string());
        const S32 w = llmax(48, font->getWidth(label) + 24);
        if (rows == 0) rows = 1;
        if (left > mChipLeft0 && left + w > right_edge)
        {
            left = mChipLeft0;
            top -= CHIP_H + CHIP_GAP;
            ++rows;
        }
        chip->setRect(LLRect(left, top, left + w, top - CHIP_H));
        left += w + CHIP_GAP;
        chip->setToggleState(mChipIds[i] == mCategory);
    }
    // The cards fill from just under the chips (or under the subtitle when there are none) to the
    // scroll's own bottom, which stays where the layout put it.
    placeScroll(rows ? mChipTop0 - rows * (CHIP_H + CHIP_GAP) - CHIP_GAP : mChipTop0);
}

// [WolfViewer 2026-10-14] The card list from `top` down to the scroll's own bottom, which stays where the
// layout put it.
void WolfFloaterExperiences::placeScroll(S32 top)
{
    LLRect sr = mScroll->getRect();
    mScroll->setRect(LLRect(sr.mLeft, top, sr.mRight, mScrollBottom));
}

// [WolfViewer 2026-10-14] Every card goes, and with it What's on's pointers to their time lines.
void WolfFloaterExperiences::clearCards()
{
    mEventTimes.clear();
    mCards->deleteAllChildren();
}

void WolfFloaterExperiences::buildCards()
{
    clearCards();

    std::vector<LLSD> shown;
    for (const LLSD& e : llsd::inArray(sReply["experiences"]))
    {
        if (mCategory.empty() || e["category"].asString() == mCategory) shown.push_back(e);
    }

    S32 total_h = 0;
    for (const LLSD& e : shown)
    {
        total_h += (e["featured"].asBoolean() ? CARD_H_FEATURED : CARD_H) + CARD_GAP;
    }
    mCards->setRect(LLRect(0, total_h, CARD_W, 0));

    S32 top = 0;
    for (const LLSD& e : shown)
    {
        const bool featured = e["featured"].asBoolean();
        const S32 h = featured ? CARD_H_FEATURED : CARD_H;
        const S32 pic_h = h - 2 * PAD;
        const S32 pic_w = pic_h * 4 / 3;    // parcel snapshots are 4:3
        const S32 text_left = PAD + pic_w + 10;
        // [WolfViewer 2026-10-14] Paul's screenshot: titles were cut short ("Fly Concorde over the
        // Territor..."). The title and blurb now use the card's whole width; Go sits bottom right,
        // beside the "In <region>" line.
        const S32 text_w = CARD_W - text_left - PAD;
        const S32 where_w = text_w - GO_W - 8;

        PassEscapePanel* card = new PassEscapePanel();
        card->buildFromFile("panel_wolf_experience_card.xml");
        card->setRect(topLeft(total_h, 0, top, CARD_W, h));
        mCards->addChild(card);   // added in order: Tab reaches the Go buttons top to bottom

        // Picture: the texture by uuid, as LLAvatarIconCtrl shows a profile picture
        // (lliconctrl.cpp:122-146 LLIconCtrl::setValue -> LLUI::getUIImageByID, a fetched texture).
        // A null uuid: the category's icon (contract).
        LLIconCtrl* pic = card->getChild<LLIconCtrl>("picture");
        const LLUUID image = e["image"].asUUID();
        if (image.notNull())
        {
            pic->setRect(topLeft(h, PAD, PAD, pic_w, pic_h));
            pic->setValue(image);
        }
        else
        {
            const S32 icon = llmin(pic_h, 64);
            pic->setRect(topLeft(h, PAD + (pic_w - icon) / 2, (h - icon) / 2, icon, icon));
            pic->setValue(categoryIcon(e["category"].asString()));
        }

        const std::string title = e["title"].asString();
        const std::string region = e["region"].asString();
        LLTextBox* title_text = card->getChild<LLTextBox>("title");
        title_text->setRect(topLeft(h, text_left, PAD, text_w, 18));
        title_text->setText(title);

        LLTextBox* blurb = card->getChild<LLTextBox>("blurb");
        blurb->setRect(topLeft(h, text_left, PAD + 20, text_w, h - 2 * PAD - 20 - GO_H - 4));
        blurb->setText(e["blurb"].asString());

        LLTextBox* where = card->getChild<LLTextBox>("where");
        where->setRect(topLeft(h, text_left, h - PAD - (GO_H + 16) / 2, where_w, 16));
        // [WolfViewer 2026-10-14] Source: EXPERIENCES_V1.md "Builder tools" - approved community entries
        // carry "community": true and "by": "<avatar name>"; their cards show a small "by <name>".
        std::string where_text = "In " + region;
        if (e["community"].asBoolean() && !e["by"].asString().empty())
        {
            where_text += "  \xC2\xB7  by " + e["by"].asString();
        }
        where->setText(where_text);

        LLButton* go = card->getChild<LLButton>("go");
        go->setRect(topLeft(h, CARD_W - PAD - GO_W, h - PAD - GO_H, GO_W, GO_H));
        go->setToolTip("Go to " + title + ": teleport to " + region);
        const std::string id = e["id"].asString();
        go->setCommitCallback([this, id](LLUICtrl*, const LLSD&) { onGo(id); });
        // Tabbing to a card below the fold brings it into view.
        LLHandle<LLFloater> fh = getHandle();
        go->setFocusReceivedCallback([fh, card](LLFocusableElement*)
        {
            if (WolfFloaterExperiences* self = static_cast<WolfFloaterExperiences*>(fh.get()))
            {
                self->mScroll->scrollToShowRect(card->getRect());
            }
        });

        top += h + CARD_GAP;
    }
    mScroll->goToTop();

    if (shown.empty())
    {
        setStatus(sReply["experiences"].size() == 0 ? std::string("There are no experiences to choose from yet.")
                                                   : std::string("Nothing in this category yet. Choose All to see everything."),
                  false, false);
    }
    else
    {
        setStatus(std::string(), false, false);
    }
}

// Go: look the region up by name and teleport to the card's position at once (see onGo for why there is
// no "Teleport to ...?"). The region's own rules (landing point, access) still decide where you land.
void WolfFloaterExperiences::onGo(const std::string& id)
{
    if (!WolfGrid::isOnWolfTerritories() || !haveReply())
    {
        return;
    }
    for (const LLSD& e : llsd::inArray(sReply["experiences"]))
    {
        if (e["id"].asString() != id) continue;
        // [WolfViewer 2026-10-14] Go teleports straight away: pressing Go IS the choice, and the stock SLURL
        // route asked "Teleport to ...?" a second time (llurldispatcher.cpp:442-450 TeleportViaSLAPP), an
        // extra click for a new resident. The same two steps that route takes after its OK are done in
        // goToExperience (shared with the admins' review list since the builder tools).
        // Source: llurldispatcher.cpp:490-497 teleport_via_slapp, :306-332 regionHandleCallback.
        goToExperience(e);
        closeFloater();
        return;
    }
}

// ---------------------------------------------------------------------------------------------------
// [WolfViewer 2026-10-14] What's on - the events section. Contract: wolfstorm/tools/EVENTS_V1.md.
//
// Who and what: a resident (or a visitor deciding to join) wants to know what is happening on the grid
// now or soon - a DJ set, a live singer, a talk - and to be there in one click when it starts.
// Flow: Wolf Experiences -> "What's on" -> cards soonest first (LIVE NOW / Starts in ... / day + time in
// their own time zone) -> Go -> teleport straight there (the same Go as an experience).
// ---------------------------------------------------------------------------------------------------

// static
void WolfFloaterExperiences::eventsChanged()
{
    WolfFloaterExperiences* self = LLFloaterReg::findTypedInstance<WolfFloaterExperiences>(FLOATER_NAME);
    if (self && self->mWhatsOn)
    {
        self->refreshView();
    }
}

// static
void WolfFloaterExperiences::tick()
{
    WolfFloaterExperiences* self = LLFloaterReg::findTypedInstance<WolfFloaterExperiences>(FLOATER_NAME);
    // Source: llfloater.h:282-284 isShown - visible and not minimised.
    if (!self || !self->mWhatsOn || !self->isShown() || self->mEventTimes.empty())
    {
        return;
    }
    if (LLFrameTimer::getTotalSeconds() - self->mLastTick >= COUNTDOWN_SECONDS)
    {
        self->updateEventTimes();
    }
}

void WolfFloaterExperiences::showWhatsOn(bool whats_on)
{
    mWhatsOn = whats_on;
    // Both are toggle buttons: a press flips the pressed one too, so both are set here.
    mModeExperiences->setToggleState(!whats_on);
    mModeWhatsOn->setToggleState(whats_on);
    mSubtitle->setText(whats_on ? std::string(EVENTS_SUBTITLE) : mSubtitleExperiences);
    load(false);
}

void WolfFloaterExperiences::buildEventCards()
{
    clearCards();
    const S32 count = (S32)sEvents.size();
    const S32 total_h = count * (EVENT_CARD_H + CARD_GAP);
    mCards->setRect(LLRect(0, total_h, CARD_W, 0));

    S32 top = 0;
    for (const LLSD& e : llsd::inArray(sEvents))
    {
        const S32 h = EVENT_CARD_H;
        const S32 pic_h = h - 2 * PAD;
        const S32 pic_w = pic_h * 4 / 3;    // the event picture's place, as an experience card's
        const S32 text_left = PAD + pic_w + 10;
        const S32 text_w = CARD_W - text_left - PAD;
        const S32 where_w = text_w - GO_W - 8;

        PassEscapePanel* card = new PassEscapePanel();
        card->buildFromFile("panel_wolf_experience_card.xml");
        card->setRect(topLeft(total_h, 0, top, CARD_W, h));
        mCards->addChild(card);   // in order: Tab reaches the Go buttons top to bottom

        // Picture: the event's texture as buildCards shows one (lliconctrl.cpp:122-146 LLIconCtrl::setValue
        // with a uuid); the null uuid (contract "<uuid or null-uuid>"): the viewer's events icon.
        LLIconCtrl* pic = card->getChild<LLIconCtrl>("picture");
        const LLUUID image = e["image"].asUUID();
        if (image.notNull())
        {
            pic->setRect(topLeft(h, PAD, PAD, pic_w, pic_h));
            pic->setValue(image);
        }
        else
        {
            pic->setRect(topLeft(h, PAD + (pic_w - EVENT_ICON) / 2, (h - EVENT_ICON) / 2, EVENT_ICON, EVENT_ICON));
            pic->setValue(std::string(EVENT_ICON_NAME));
        }

        const std::string title = e["title"].asString();
        LLTextBox* title_text = card->getChild<LLTextBox>("title");
        title_text->setRect(topLeft(h, text_left, PAD, text_w, 18));
        title_text->setText(title);

        // The time line; its words and colour are set by updateEventTimes (now and every 30 s).
        LLTextBox* when = card->getChild<LLTextBox>("when");
        when->setRect(topLeft(h, text_left, PAD + 20, text_w, 16));
        when->setVisible(true);
        mEventTimes.push_back({ when, secondsOf(e["starts"]), secondsOf(e["ends"]) });

        LLTextBox* blurb = card->getChild<LLTextBox>("blurb");
        blurb->setRect(topLeft(h, text_left, PAD + 38, text_w, h - 2 * PAD - 38 - GO_H - 4));
        blurb->setText(e["description"].asString());

        // Source: EVENTS_V1.md - region null = no region on this grid; then only "link".
        const std::string region = e["region"].isString() ? e["region"].asString() : std::string();
        const std::string link = e["link"].asString();
        const EEventLink kind = region.empty() ? eventLinkKind(link) : LINK_NONE;
        std::string place = e["where"].asString();
        if (place.empty()) place = region;
        std::string where_text = place.empty() ? std::string() : "At " + place;
        if (region.empty() && kind == LINK_NONE)
        {
            where_text += (where_text.empty() ? "" : "  \xC2\xB7  ") + std::string("This event is on another grid");
        }
        LLTextBox* where = card->getChild<LLTextBox>("where");
        where->setRect(topLeft(h, text_left, h - PAD - (GO_H + 16) / 2, where_w, 16));
        where->setText(where_text);

        LLButton* go = card->getChild<LLButton>("go");
        go->setRect(topLeft(h, CARD_W - PAD - GO_W, h - PAD - GO_H, GO_W, GO_H));
        if (!region.empty())
        {
            go->setToolTip("Go to " + title + ": teleport to " + region);
        }
        else if (kind == LINK_SLURL)
        {
            go->setToolTip("This event is not in a Wolf Territories region. Go follows its address: " + link);
        }
        else if (kind == LINK_WEB)
        {
            go->setLabel(std::string("Open"));
            go->setToolTip("This event is not in a Wolf Territories region. Open its page: " + link);
        }
        else
        {
            go->setEnabled(false);
            go->setToolTip(std::string("This event is on another grid: there is no address here to go to."));
        }
        const std::string id = e["id"].asString();
        go->setCommitCallback([this, id](LLUICtrl*, const LLSD&) { onEventGo(id); });
        LLHandle<LLFloater> fh = getHandle();
        go->setFocusReceivedCallback([fh, card](LLFocusableElement*)
        {
            if (WolfFloaterExperiences* self = static_cast<WolfFloaterExperiences*>(fh.get()))
            {
                self->mScroll->scrollToShowRect(card->getRect());
            }
        });

        top += h + CARD_GAP;
    }
    mScroll->goToTop();
    updateEventTimes();

    if (count == 0)
    {
        setStatus("Nothing on in the next 7 days \xE2\x80\x94 add an event at wolf-grid.com/whats-on", false, false);
    }
    else
    {
        setStatus(std::string(), false, false);
    }
}

void WolfFloaterExperiences::updateEventTimes()
{
    mLastTick = LLFrameTimer::getTotalSeconds();
    if (!haveEvents()) return;
    const S64 now = eventsNow();
    // Source: skins/default/colors.xml:33 AccentColor (LIVE NOW); :82 LtGray (the other lines).
    static LLUIColor accent = LLUIColorTable::instance().getColor("AccentColor");
    static LLUIColor normal = LLUIColorTable::instance().getColor("LtGray");
    for (const EventTime& t : mEventTimes)
    {
        bool live = false;
        t.mWhen->setText(eventWhen(t.mStarts, t.mEnds, now, live));
        t.mWhen->setColor(live ? accent : normal);
    }
}

void WolfFloaterExperiences::onEventGo(const std::string& id)
{
    if (!WolfGrid::isOnWolfTerritories() || !haveEvents())
    {
        return;
    }
    for (const LLSD& e : llsd::inArray(sEvents))
    {
        if (e["id"].asString() != id) continue;
        const std::string region = e["region"].isString() ? e["region"].asString() : std::string();
        if (!region.empty())
        {
            // The experiences' Go (goToExperience: llurldispatcher.cpp:490-497 / :306-332): straight there,
            // with "the region was not found" after 20 s when nothing happens. No "arrive" line for events.
            LLSD dest;
            dest["region"] = region;
            // Source: EVENTS_V1.md - x/y/z always sent, 128/128/0 when nothing better is known; the same
            // here should a number be missing.
            dest["x"] = e.has("x") ? e["x"] : LLSD(128);
            dest["y"] = e.has("y") ? e["y"] : LLSD(128);
            dest["z"] = e.has("z") ? e["z"] : LLSD(0);
            dest["title"] = e["title"];
            goToExperience(dest);
            closeFloater();
            return;
        }
        std::string link = e["link"].asString();
        LLStringUtil::trim(link);
        if (eventLinkKind(link) != LINK_NONE)
        {
            // Source: llurlaction.cpp:96-106 LLUrlAction::clickAction - what a clicked link in chat does: the
            // SLURL dispatch first (llappviewer.cpp:1132 -> llurldispatcher.cpp:539-550 dispatchFromTextEditor;
            // fsslurl.cpp:258-261/355-358 parse hop:// as a location, llurldispatcher.cpp:209-271 dispatchRegion
            // hops or refuses with CantTeleportToGrid), else the web browser (llappviewer.cpp:1129 LLWeb::loadURL).
            // Not trusted content: the address was typed by the event's owner on the website.
            LLUrlAction::clickAction(link, false);
        }
        return;
    }
}

// ---------------------------------------------------------------------------------------------------
// [WolfViewer 2026-10-14] Builder tools - "Add your experience", "My experiences", "Waiting for review".
// Source: wolfstorm/tools/EXPERIENCES_V1.md "Builder tools - Add your experience (v1.1)". The server
// decides everything that matters (land rights at the spot, the field rules, the limits, who is an
// admin); this window only asks and shows the answers in words.
// ---------------------------------------------------------------------------------------------------

namespace
{
    // Source: EXPERIENCES_V1.md "Storage" - region_name VARCHAR(64), review_note VARCHAR(200); "Data" -
    // title <= 40, blurb / arrive <= 140 (also the XML's max_length_chars).
    const S32 MAX_REGION_CHARS = 64;
    const std::string LOADING_CATEGORIES = "Loading the categories\xE2\x80\xA6";

    // Source: wolfstorm php/experiences.php:107-115 exp_text - a text must not start or end with spaces
    // (the submit "validates every field like the curated entries", contract), so send it trimmed.
    std::string trimmed(const LLLineEditor* e)
    {
        std::string s = e->getText();
        LLStringUtil::trim(s);
        return s;
    }

    F32 r2(F32 v) { return roundf(v * 100.f) / 100.f; }

    const LLColor4 STATUS_ERROR(1.f, 0.45f, 0.4f, 1.f);     // the chooser's error colour (setStatus)
    const LLColor4 STATUS_OK(0.75f, 0.77f, 0.8f, 1.f);

    // [WolfViewer 2026-10-14] "Upload a picture...": File > Upload > Image's own upload, told what happened.
    // Source: llfloaternamedesc.cpp:196-228 LLFloaterNameDesc::onBtnOK - an LLNewFileResourceUploadInfo
    // (encodes the file: llviewerassetupload.cpp:399-409 prepareUpload -> :411+ exportTempFile ->
    // LLViewerTextureList::createUploadFile) handed to upload_new_resource (llviewermenufile.cpp:1493-1559,
    // the NewFileAgentInventory capability; the region charges what it charges, OpenSim usually L$0).
    // FT_NONE puts it in the Textures folder (llviewerassetupload.cpp:292-323 assignDefaults: the folder
    // for the asset type). The upload coroutine (llviewerassetupload.cpp:896-1038 AssetInventoryUploadCoproc)
    // ends in finishUpload(result) on success - the server's reply carries the new texture's id as
    // "new_asset" (llviewerassetupload.cpp:184-259 LLResourceUploadInfo::finishUpload reads it there) - or in
    // HandleUploadError (:1042-1118), which shows the viewer's own notice and then calls failedUpload with
    // the reason. Both run on the main thread (a coprocedure coroutine).
    class WolfExperiencePictureUpload : public LLNewFileResourceUploadInfo
    {
    public:
        WolfExperiencePictureUpload(const std::string& filename, const std::string& name, S32 cost, LLHandle<LLFloater> floater)
            : LLNewFileResourceUploadInfo(filename, name, "Wolf Experiences picture", 0,
                                          LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                                          LLFloaterPerms::getNextOwnerPerms("Uploads"),
                                          LLFloaterPerms::getGroupPerms("Uploads"),
                                          LLFloaterPerms::getEveryonePerms("Uploads"),
                                          cost, LLUUID::null,
                                          false),   // stay in the builder's window: no inventory window pops up
              mFloater(floater)
        {
        }

        LLUUID finishUpload(LLSD& result) override
        {
            // The inventory item, made exactly as File > Upload makes it.
            const LLUUID item = LLNewFileResourceUploadInfo::finishUpload(result);
            const LLUUID asset = result["new_asset"].asUUID();
            if (WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(mFloater.get()))
            {
                // The coroutine also calls finishUpload when the grid gave no upload address
                // (llviewerassetupload.cpp:982-986): then there is no new texture.
                self->pictureUploaded(asset, asset.isNull() ? std::string("The grid did not send the new picture's id, so it was not used. Try again, or choose it from your Textures folder.") : std::string());
            }
            return item;
        }

        bool failedUpload(LLSD& result, std::string& reason) override
        {
            if (WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(mFloater.get()))
            {
                std::string why = reason;
                // A file that could not be read or converted: exportTempFile's result has a notification
                // label and, sometimes, no message (llviewerassetupload.cpp:561-570).
                if (result.has("label") && !result.has("message"))
                {
                    why = "The picture file could not be read or converted (" + result["label"].asString() + "). Try a PNG, JPEG, TGA or BMP image.";
                }
                self->pictureUploaded(LLUUID::null, why.empty() ? std::string("The picture could not be uploaded.") : "The picture could not be uploaded: " + why);
            }
            return false;   // the viewer carries on as for any failed upload (llviewerassetupload.cpp:1097-1118)
        }

    private:
        LLHandle<LLFloater> mFloater;
    };
}

WolfFloaterExperienceSubmit::WolfFloaterExperienceSubmit(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterExperienceSubmit::postBuild()
{
    mSecAdd = getChild<LLButton>("sec_add");
    mSecMine = getChild<LLButton>("sec_mine");
    mSecReview = getChild<LLButton>("sec_review");
    mSecAdd->setCommitCallback([this](LLUICtrl*, const LLSD&) { showSection(SECTION_ADD); });
    mSecMine->setCommitCallback([this](LLUICtrl*, const LLSD&) { showSection(SECTION_MINE); loadMine(); });
    mSecReview->setCommitCallback([this](LLUICtrl*, const LLSD&) { showSection(SECTION_REVIEW); loadReview(); });
    mSecReview->setVisible(false);   // shown only once the site answers GET ?review=1 (loadReview)

    mRegion = getChild<LLLineEditor>("add_region");
    mRegion->setMaxTextChars(MAX_REGION_CHARS);
    mX = getChild<LLSpinCtrl>("add_x");
    mY = getChild<LLSpinCtrl>("add_y");
    mZ = getChild<LLSpinCtrl>("add_z");
    mTitle = getChild<LLLineEditor>("add_title");
    mBlurb = getChild<LLLineEditor>("add_blurb");
    mArrive = getChild<LLLineEditor>("add_arrive");
    mCategory = getChild<LLComboBox>("add_category");
    mPicture = getChild<LLTextureCtrl>("add_picture");
    mUploadBtn = getChild<LLButton>("add_upload");   // [WolfViewer 2026-10-14]
    mUploadBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUploadPicture(); });
    mAddStatus = getChild<LLTextBox>("add_status");
    mSubmit = getChild<LLButton>("add_submit");
    getChild<LLButton>("add_here")->setCommitCallback([this](LLUICtrl*, const LLSD&) { useWhereIStand(); });
    mSubmit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSubmit(); });
    getChild<LLButton>("add_cancel")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });

    mMineList = getChild<LLScrollListCtrl>("mine_list");
    mMineDetail = getChild<LLTextBox>("mine_detail");
    mMineStatus = getChild<LLTextBox>("mine_status");
    mWithdraw = getChild<LLButton>("mine_withdraw");
    mMineList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMineSelect(); });
    mWithdraw->setCommitCallback([this](LLUICtrl*, const LLSD&) { onWithdraw(); });
    getChild<LLButton>("mine_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadMine(); });

    mReviewList = getChild<LLScrollListCtrl>("review_list");
    mReviewPicture = getChild<LLIconCtrl>("review_picture");
    mReviewDetail = getChild<LLTextBox>("review_detail");
    mReviewNote = getChild<LLLineEditor>("review_note");
    mReviewStatus = getChild<LLTextBox>("review_status");
    mReviewGo = getChild<LLButton>("review_go");
    mApprove = getChild<LLButton>("review_approve");
    mReject = getChild<LLButton>("review_reject");
    mReviewList->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReviewSelect(); });
    mApprove->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReview(true); });
    mReject->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReview(false); });
    mReviewGo->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        LLScrollListItem* item = mReviewList->getFirstSelected();
        if (!item || !WolfGrid::isOnWolfTerritories()) return;
        for (const LLSD& s : llsd::inArray(mReview))
        {
            if (s["id"].asString() == item->getValue().asString())
            {
                goToExperience(s);
                setStatus(mReviewStatus, "Taking you to " + regionOf(s) + "\xE2\x80\xA6", false);
                return;
            }
        }
    });
    getChild<LLButton>("review_refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadReview(); });

    showSection(SECTION_ADD);
    return true;
}

void WolfFloaterExperienceSubmit::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    if (!WolfGrid::isWolfTerritories())
    {
        showSection(SECTION_ADD);
        setStatus(mAddStatus, "Experiences are on Wolf Territories only.", true);
        return;
    }
    // A fresh form starts where the builder stands; a half-filled one is kept as it was.
    if (mTitle->getText().empty() && mBlurb->getText().empty() && mArrive->getText().empty())
    {
        useWhereIStand();
    }
    if (!haveReply())
    {
        startFetch();   // the categories come with the chooser's list
    }
    fillCategories();
    loadReview();       // shows "Waiting for review" only if the site says this is an admin
    showSection(mSection);
    if (mSection == SECTION_MINE) loadMine();
}

bool WolfFloaterExperienceSubmit::handleKeyHere(KEY key, MASK mask)
{
    // Escape closes, as the chooser does (llpanel.cpp:271-281: a plain floater only drops focus).
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        closeFloater();
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

// static
void WolfFloaterExperienceSubmit::dataChanged()
{
    if (WolfFloaterExperienceSubmit* self = LLFloaterReg::findTypedInstance<WolfFloaterExperienceSubmit>(SUBMIT_FLOATER_NAME))
    {
        self->fillCategories();
    }
}

void WolfFloaterExperienceSubmit::setStatus(LLTextBox* box, const std::string& text, bool error)
{
    box->setText(text);
    box->setColor(error ? STATUS_ERROR : STATUS_OK);
}

void WolfFloaterExperienceSubmit::showSection(ESection section)
{
    if (section == SECTION_REVIEW && !mAdmin) section = SECTION_ADD;
    mSection = section;
    // Each section's controls are named add_*, mine_* or review_* in floater_wolf_experience_submit.xml.
    for (LLView* child : *getChildList())
    {
        const std::string& n = child->getName();
        if (n.rfind("add_", 0) == 0) child->setVisible(section == SECTION_ADD);
        else if (n.rfind("mine_", 0) == 0) child->setVisible(section == SECTION_MINE);
        else if (n.rfind("review_", 0) == 0) child->setVisible(section == SECTION_REVIEW);
    }
    mSecAdd->setToggleState(section == SECTION_ADD);
    mSecMine->setToggleState(section == SECTION_MINE);
    mSecReview->setToggleState(section == SECTION_REVIEW);
    if (section == SECTION_REVIEW)
    {
        onReviewSelect();   // [WolfViewer 2026-10-14] the picture shows only for a selected entry that has one
    }
    updateButtons();
}

void WolfFloaterExperienceSubmit::fillCategories()
{
    // Source: EXPERIENCES_V1.md - category = one of categories[].id from the reply (<= 6).
    const std::string chosen = mCategory->getValue().asString();
    mCategory->removeall();
    for (const LLSD& c : llsd::inArray(sReply["categories"]))
    {
        mCategory->add(c["title"].asString(), LLSD(c["id"].asString()));
    }
    if (mCategory->getItemCount() > 0)
    {
        mCategory->setValue(LLSD(chosen));
        if (mCategory->getValue().asString() != chosen || chosen.empty()) mCategory->selectFirstItem();
        if (mAddStatus->getText() == LOADING_CATEGORIES) setStatus(mAddStatus, std::string(), false);
    }
    else if (!sError.empty() && !sFetching)
    {
        setStatus(mAddStatus, "The categories could not be loaded: " + sError, true);
    }
    else if (sFetching)
    {
        // Submit stays off until there is a category to choose; say why.
        setStatus(mAddStatus, LOADING_CATEGORIES, false);
    }
    updateButtons();
}

void WolfFloaterExperienceSubmit::useWhereIStand()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || !WolfGrid::isOnWolfTerritories())
    {
        setStatus(mAddStatus, "Stand in a Wolf Territories region to fill in where you are.", true);
        return;
    }
    // Source: llviewerregion.h:339 LLViewerRegion::getPosRegionFromGlobal; llagent.h:213 getPositionGlobal;
    // llviewerregion.h:263 getWidth (var regions are wider than 256 m).
    const LLVector3 pos = region->getPosRegionFromGlobal(gAgent.getPositionGlobal());
    const F32 width = region->getWidth();
    mX->setMaxValue(width);
    mY->setMaxValue(width);
    mRegion->setText(region->getName());
    mX->set(r2(llclamp(pos.mV[VX], 0.f, width)));
    mY->set(r2(llclamp(pos.mV[VY], 0.f, width)));
    mZ->set(r2(llmax(pos.mV[VZ], 0.f)));

    // The picture starts as this parcel's snapshot (contract), unless the builder already chose another.
    // Source: llviewerparcelmgr.h:163 getAgentParcel; llparcel.h:392 LLParcel::getSnapshotID;
    // lltexturectrl.h:201-204 set/getImageAssetID, setDefaultImageAssetID (the picker's "Default").
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    const LLUUID snapshot = parcel ? parcel->getSnapshotID() : LLUUID::null;
    if (mPicture->getImageAssetID() == mDefaultPicture)
    {
        mPicture->setImageAssetID(snapshot);
    }
    mPicture->setDefaultImageAssetID(snapshot);
    mDefaultPicture = snapshot;
    setStatus(mAddStatus, "Filled in from where you stand in " + region->getName() + ".", false);
}

// [WolfViewer 2026-10-14] Paul: "users should be able to upload their own texture for experiences".
// Flow: "Upload a picture..." -> the computer's own file window, images only -> the standard texture upload
// (encoded, charged, into Textures) -> the picture becomes the new texture, "Picture uploaded" here; or why not.
void WolfFloaterExperienceSubmit::onUploadPicture()
{
    if (mUploading) return;
    if (!WolfGrid::isOnWolfTerritories())
    {
        // The upload goes to the region underfoot: only a Wolf Territories one.
        setStatus(mAddStatus, "Stand in a Wolf Territories region to upload a picture.", true);
        return;
    }
    mUploading = true;
    updateButtons();
    setStatus(mAddStatus, "Choose a picture on your computer\xE2\x80\xA6", false);
    // Source: wolffloaterroleplay.cpp:1894-1915 onChooseFile / lltexturectrl.cpp:1253 LLFloaterTexturePicker::onBtnAdd -
    // the viewer's image file window (FFLOAD_IMAGE), answered on the main thread; an empty list = cancelled
    // (llviewermenufile.cpp:349-365 LLFilePickerReplyThread::notify -> the failure callback).
    LLHandle<LLFloater> h = getHandle();
    LLFilePickerReplyThread::startPicker(
        [h](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
            if (!self) return;
            if (files.empty() || files.front().empty())
            {
                self->mUploading = false;
                self->setStatus(self->mAddStatus, std::string(), false);
                self->updateButtons();
                return;
            }
            self->uploadPictureFile(files.front());
        },
        LLFilePicker::FFLOAD_IMAGE, false,
        [h](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
            if (!self) return;
            self->mUploading = false;
            self->setStatus(self->mAddStatus, std::string(), false);   // cancelled: nothing to say
            self->updateButtons();
        });
}

void WolfFloaterExperienceSubmit::uploadPictureFile(const std::string& filename)
{
    // Source: llviewermenufile.cpp:1498-1511 upload_new_resource - it does nothing when disconnected, and
    // without the NewFileAgentInventory capability it falls back to the old asset path, which never
    // reports the new texture back here.
    if (gDisconnected || gAgent.getRegionCapability("NewFileAgentInventory").empty())
    {
        pictureUploaded(LLUUID::null, "This region cannot take uploads right now. Try again in a moment, or from another region.");
        return;
    }
    // Source: llfloaternamedesc.cpp:150-167 getExpectedUploadCost (LLAgentBenefitsMgr findUploadCost for the
    // asset type) and :201-225 onBtnOK (can_afford_transaction, else "You need L$[COST]").
    LLAssetType::EType type = LLAssetType::AT_TEXTURE;
    S32 cost = 0;
    if (!LLAgentBenefitsMgr::current().findUploadCost(type, cost))
    {
        cost = 0;   // unknown: the region decides and charges (and says so) as for File > Upload
    }
    if (!can_afford_transaction(cost))
    {
        pictureUploaded(LLUUID::null, llformat("You need L$%d to upload a picture.", cost));
        return;
    }
    std::string name = gDirUtilp->getBaseFileName(filename, true);   // the file's name, without its extension
    if (name.empty()) name = "Experience picture";
    setStatus(mAddStatus, "Uploading " + gDirUtilp->getBaseFileName(filename) + "\xE2\x80\xA6", false);
    LLResourceUploadInfo::ptr_t info = std::make_shared<WolfExperiencePictureUpload>(filename, name, cost, getHandle());
    upload_new_resource(info);
}

void WolfFloaterExperienceSubmit::pictureUploaded(const LLUUID& asset_id, const std::string& error)
{
    mUploading = false;
    if (asset_id.isNull())
    {
        setStatus(mAddStatus, error, true);
    }
    else
    {
        // Source: lltexturectrl.h:201-204 setImageAssetID. The default (the parcel snapshot, the picker's
        // "Default") stays as it was, so "Use where I stand" keeps this upload (useWhereIStand only replaces
        // the default picture).
        mPicture->setImageAssetID(asset_id);
        setStatus(mAddStatus, "Picture uploaded. It is also in your Textures folder.", false);
    }
    updateButtons();
}

void WolfFloaterExperienceSubmit::updateButtons()
{
    const bool wolf = WolfGrid::isWolfTerritories();
    mSubmit->setEnabled(wolf && !mSubmitting && mCategory->getItemCount() > 0);
    mUploadBtn->setEnabled(wolf && !mUploading);   // [WolfViewer 2026-10-14]

    LLScrollListItem* mine = mMineList->getFirstSelected();
    bool can_withdraw = false;
    if (mine)
    {
        for (const LLSD& s : llsd::inArray(mMine))
        {
            if (s["id"].asString() != mine->getValue().asString()) continue;
            // Source: EXPERIENCES_V1.md - withdraw = "the submitter's own pending/live entry".
            const std::string st = s["status"].asString();
            can_withdraw = st == "pending" || st == "live";
        }
    }
    mWithdraw->setEnabled(wolf && can_withdraw && !mActionBusy);

    const bool picked = mReviewList->getFirstSelected() != nullptr;
    mReviewGo->setEnabled(wolf && picked);
    mApprove->setEnabled(wolf && picked && !mActionBusy);
    mReject->setEnabled(wolf && picked && !mActionBusy);
}

void WolfFloaterExperienceSubmit::onSubmit()
{
    if (!WolfGrid::isWolfTerritories() || mSubmitting) return;
    // Source: wolfstorm php/experiences.php:131-139 exp_check_entry - region, title, blurb and arrive must
    // each hold 1+ characters, category one of the list. Said here first so the builder is not sent away
    // to the server for an empty field; the server checks everything again.
    struct Need { LLLineEditor* field; const char* words; };
    const Need needs[] = {
        { mRegion, "Enter the region where people should arrive." },
        { mTitle, "Say what people will do, in a few words (for example \"Ride the coast train\")." },
        { mBlurb, "Describe the experience in a sentence." },
        { mArrive, "Say what to do first on arrival (for example \"Take a seat at gate 1\")." },
    };
    for (const Need& n : needs)
    {
        if (trimmed(n.field).empty())
        {
            setStatus(mAddStatus, n.words, true);
            n.field->setFocus(true);
            return;
        }
    }
    if (mCategory->getValue().asString().empty())
    {
        setStatus(mAddStatus, "Choose a category.", true);
        mCategory->setFocus(true);
        return;
    }

    // Source: EXPERIENCES_V1.md "Builder tools" API - POST {action:"submit", region, x, y, z, title, blurb,
    // arrive, category, image}. image: a lower-case uuid (php/experiences.php:122-124 exp_is_uuid; LLUUID
    // asString is lower case); the null uuid = draw the category's icon (contract "image").
    LLSD body;
    body["action"] = "submit";
    body["region"] = trimmed(mRegion);
    body["x"] = (F64)r2(mX->get());
    body["y"] = (F64)r2(mY->get());
    body["z"] = (F64)r2(mZ->get());
    body["title"] = trimmed(mTitle);
    body["blurb"] = trimmed(mBlurb);
    body["arrive"] = trimmed(mArrive);
    body["category"] = mCategory->getValue().asString();
    body["image"] = mPicture->getImageAssetID().asString();

    mSubmitting = true;
    updateButtons();
    setStatus(mAddStatus, "Sending\xE2\x80\xA6", false);
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfExperienceSubmit", [h, body]()
    {
        LLSD reply;
        std::string transport;
        const S32 status = xpReplyOf(xpRequest(EXPERIENCES_URL, &body), reply, transport);
        WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
        if (!self) return;
        const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
        self->submitDone(ok, ok ? std::string() : xpError(status, reply, transport));
    });
}

void WolfFloaterExperienceSubmit::submitDone(bool ok, const std::string& message)
{
    mSubmitting = false;
    if (!ok)
    {
        // The server's refusal, in its words, stays in the window (it does not fade).
        setStatus(mAddStatus, message, true);
        updateButtons();
        return;
    }
    const std::string title = trimmed(mTitle);
    mTitle->clear();
    mBlurb->clear();
    mArrive->clear();
    setStatus(mAddStatus, std::string(), false);
    showSection(SECTION_MINE);
    setStatus(mMineStatus, "\"" + title + "\" was sent. A grid admin will look at it; its status shows here.", false);
    loadMine();
}

void WolfFloaterExperienceSubmit::loadMine()
{
    if (mMineBusy || !WolfGrid::isWolfTerritories()) return;
    mMineBusy = true;
    if (mMine.size() == 0) mMineList->setCommentText("Loading your experiences\xE2\x80\xA6");
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfExperienceMine", [h]()
    {
        LLSD reply;
        std::string transport;
        // Source: EXPERIENCES_V1.md "Builder tools" API - GET ?mine=1 -> {success, submissions:[...]}.
        const S32 status = xpReplyOf(xpRequest(std::string(EXPERIENCES_URL) + "?mine=1", nullptr), reply, transport);
        WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
        if (!self) return;
        const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
        self->mineLoaded(ok, ok ? reply["submissions"] : LLSD(), ok ? std::string() : xpError(status, reply, transport));
    });
}

void WolfFloaterExperienceSubmit::mineLoaded(bool ok, const LLSD& submissions, const std::string& error)
{
    mMineBusy = false;
    if (!ok)
    {
        mMineList->setCommentText("Your experiences could not be loaded.");
        setStatus(mMineStatus, error, true);
        updateButtons();
        return;
    }
    mMine = LLSD::emptyArray();
    for (const LLSD& s : llsd::inArray(submissions))
    {
        if (s.isMap() && !s["id"].asString().empty()) mMine.append(s);
    }
    showMine();
}

void WolfFloaterExperienceSubmit::showMine()
{
    const LLSD selected = mMineList->getSelectedValue();
    mMineList->deleteAllItems();
    for (const LLSD& s : llsd::inArray(mMine))
    {
        LLSD row;
        row["value"] = s["id"].asString();
        row["columns"][0]["column"] = "title";
        row["columns"][0]["value"] = s["title"].asString();
        row["columns"][1]["column"] = "status";
        row["columns"][1]["value"] = statusWords(s["status"].asString());
        mMineList->addElement(row);
    }
    if (mMine.size() == 0)
    {
        mMineList->setCommentText("You have not added an experience yet. Use Add to send your first one.");
    }
    else if (selected.isDefined())
    {
        mMineList->setSelectedByValue(selected, true);
    }
    onMineSelect();
}

void WolfFloaterExperienceSubmit::onMineSelect()
{
    std::string text;
    if (LLScrollListItem* item = mMineList->getFirstSelected())
    {
        for (const LLSD& s : llsd::inArray(mMine))
        {
            if (s["id"].asString() != item->getValue().asString()) continue;
            text = s["title"].asString() + " - " + statusWords(s["status"].asString());
            const std::string region = regionOf(s);
            if (!region.empty())
            {
                text += llformat("\nIn %s at %d, %d, %d", region.c_str(), s["x"].asInteger(), s["y"].asInteger(), s["z"].asInteger());
            }
            // Source: EXPERIENCES_V1.md - "status (pending/live/rejected with the note)".
            if (!s["review_note"].asString().empty())
            {
                text += "\nThe admin's note: " + s["review_note"].asString();
            }
        }
    }
    else if (mMine.size() > 0)
    {
        text = "Choose one of your experiences to see its details or withdraw it.";
    }
    mMineDetail->setText(text);
    updateButtons();
}

void WolfFloaterExperienceSubmit::onWithdraw()
{
    LLScrollListItem* item = mMineList->getFirstSelected();
    if (!item || mActionBusy) return;
    const std::string id = item->getValue().asString();
    std::string title;
    for (const LLSD& s : llsd::inArray(mMine))
    {
        if (s["id"].asString() == id) title = s["title"].asString();
    }
    // Medium risk, not undoable from here (it would have to be sent and reviewed again): ask first.
    // Source: wolfcave.cpp:2106-2113 - GenericAlertYesCancel (notifications.xml:171-180), option 0 = Yes.
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Withdraw \"" + title + "\"? It will no longer be shown or reviewed. To show it again you would send it again."),
        LLSD(),
        [h, id](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
            WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
            if (!self || self->mActionBusy || !WolfGrid::isWolfTerritories()) return;
            self->mActionBusy = true;
            self->updateButtons();
            self->setStatus(self->mMineStatus, "Withdrawing\xE2\x80\xA6", false);
            // Source: EXPERIENCES_V1.md "Builder tools" API - POST {action:"withdraw", id}.
            LLSD body;
            body["action"] = "withdraw";
            body["id"] = id;
            LLCoros::instance().launch("WolfExperienceWithdraw", [h, body]()
            {
                LLSD reply;
                std::string transport;
                const S32 status = xpReplyOf(xpRequest(EXPERIENCES_URL, &body), reply, transport);
                WolfFloaterExperienceSubmit* me = static_cast<WolfFloaterExperienceSubmit*>(h.get());
                const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
                if (ok) startFetch();    // a live one leaves the chooser's list
                if (!me) return;
                me->actionDone(SECTION_MINE, ok, ok ? std::string("Withdrawn.") : xpError(status, reply, transport));
            });
        });
}

void WolfFloaterExperienceSubmit::loadReview()
{
    if (mReviewBusy || !WolfGrid::isWolfTerritories()) return;
    mReviewBusy = true;
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfExperienceReviewList", [h]()
    {
        LLSD reply;
        std::string transport;
        // Source: EXPERIENCES_V1.md "Builder tools" API - GET ?review=1 (identity + god level), the
        // pending submissions. Whether this is an admin is the server's answer, never this viewer's.
        const S32 status = xpReplyOf(xpRequest(std::string(EXPERIENCES_URL) + "?review=1", nullptr), reply, transport);
        WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
        if (!self) return;
        const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
        // No answer, or the service failing (5xx), says nothing about who this is: keep what was known.
        const bool unknown = !ok && (status == 0 || status >= 500);
        self->reviewLoaded(ok, ok ? reply["submissions"] : LLSD(), unknown ? xpError(status, reply, transport) : std::string());
    });
}

void WolfFloaterExperienceSubmit::reviewLoaded(bool ok, const LLSD& submissions, const std::string& error)
{
    mReviewBusy = false;
    if (!ok)
    {
        if (!error.empty())
        {
            // Could not ask: an admin keeps the list they had, with the reason.
            if (mAdmin) setStatus(mReviewStatus, error, true);
            return;
        }
        // Refused: not an admin (or no longer). The section goes away.
        mAdmin = false;
        mReview = LLSD::emptyArray();
        mSecReview->setVisible(false);
        if (mSection == SECTION_REVIEW) showSection(SECTION_ADD);
        return;
    }
    mAdmin = true;
    mSecReview->setVisible(true);
    mReview = LLSD::emptyArray();
    for (const LLSD& s : llsd::inArray(submissions))
    {
        if (s.isMap() && !s["id"].asString().empty()) mReview.append(s);
    }
    mSecReview->setLabel(llformat("Waiting for review (%d)", (S32)mReview.size()));
    showReview();
}

void WolfFloaterExperienceSubmit::showReview()
{
    const LLSD selected = mReviewList->getSelectedValue();
    mReviewList->deleteAllItems();
    for (const LLSD& s : llsd::inArray(mReview))
    {
        LLSD row;
        row["value"] = s["id"].asString();
        row["columns"][0]["column"] = "title";
        row["columns"][0]["value"] = s["title"].asString();
        row["columns"][1]["column"] = "by";
        row["columns"][1]["value"] = s["by"].asString();
        row["columns"][2]["column"] = "region";
        row["columns"][2]["value"] = regionOf(s);
        mReviewList->addElement(row);
    }
    if (mReview.size() == 0)
    {
        mReviewList->setCommentText("Nothing is waiting for review.");
    }
    else if (selected.isDefined())
    {
        mReviewList->setSelectedByValue(selected, true);
    }
    onReviewSelect();
}

void WolfFloaterExperienceSubmit::onReviewSelect()
{
    std::string text;
    LLUUID image;
    bool any = false;
    if (LLScrollListItem* item = mReviewList->getFirstSelected())
    {
        for (const LLSD& s : llsd::inArray(mReview))
        {
            if (s["id"].asString() != item->getValue().asString()) continue;
            any = true;
            image = s["image"].asUUID();
            text = s["title"].asString();
            if (!s["by"].asString().empty()) text += "  by " + s["by"].asString();
            text += "\nCategory: " + categoryTitle(s["category"].asString());
            text += llformat("\nIn %s at %d, %d, %d", regionOf(s).c_str(), s["x"].asInteger(), s["y"].asInteger(), s["z"].asInteger());
            text += "\n" + s["blurb"].asString();
            text += "\nWhen you arrive: " + s["arrive"].asString();
        }
    }
    else
    {
        text = mReview.size() > 0 ? std::string("Choose an experience to see it, visit it with Go, then approve or reject it.") : std::string();
    }
    // The picture as the chooser's cards show it (lliconctrl.cpp LLIconCtrl::setValue with a uuid); none
    // = the category icon there, nothing here.
    mReviewPicture->setVisible(mSection == SECTION_REVIEW && any && image.notNull());
    if (image.notNull()) mReviewPicture->setValue(image);
    mReviewDetail->setText(text);
    updateButtons();
}

void WolfFloaterExperienceSubmit::onReview(bool approve)
{
    LLScrollListItem* item = mReviewList->getFirstSelected();
    if (!item || mActionBusy || !WolfGrid::isWolfTerritories()) return;
    // Source: EXPERIENCES_V1.md "Builder tools" API - POST {action:"review", id, decision:"approve"|"reject",
    // note}; review_note VARCHAR(200) (the note field's max_length_chars).
    LLSD body;
    body["action"] = "review";
    body["id"] = item->getValue().asString();
    body["decision"] = approve ? "approve" : "reject";
    body["note"] = trimmed(mReviewNote);
    mActionBusy = true;
    updateButtons();
    setStatus(mReviewStatus, approve ? "Approving\xE2\x80\xA6" : "Rejecting\xE2\x80\xA6", false);
    LLHandle<LLFloater> h = getHandle();
    LLCoros::instance().launch("WolfExperienceReview", [h, body, approve]()
    {
        LLSD reply;
        std::string transport;
        const S32 status = xpReplyOf(xpRequest(EXPERIENCES_URL, &body), reply, transport);
        const bool ok = status == 200 && reply.isMap() && reply["success"].asBoolean();
        if (ok && approve) startFetch();    // the approved one joins the chooser's list
        WolfFloaterExperienceSubmit* self = static_cast<WolfFloaterExperienceSubmit*>(h.get());
        if (!self) return;
        self->actionDone(SECTION_REVIEW, ok,
            ok ? std::string(approve ? "Approved: it is now shown in Wolf Experiences." : "Rejected; the builder sees your note.")
               : xpError(status, reply, transport));
    });
}

void WolfFloaterExperienceSubmit::actionDone(ESection section, bool ok, const std::string& message)
{
    mActionBusy = false;
    if (section == SECTION_MINE)
    {
        setStatus(mMineStatus, message, !ok);
        if (ok) loadMine();
    }
    else
    {
        setStatus(mReviewStatus, message, !ok);
        if (ok)
        {
            mReviewNote->clear();
            mReviewList->deselectAllItems();
            loadReview();
        }
    }
    updateButtons();
}
