/**
 * @file wolfpassingplanes.cpp
 * @brief [PASSING PLANES 2026-11-01] Region/Estate > Region: "Passing planes over this region" (wolfpassingplanes.h).
 *
 * Who and why: an estate owner or manager of a themed region (a wild west town) wants the tour planes gone, or an airfield
 * owner wants them, in one click, and to see that it took. Flow: open the Region tab -> the state and its reason show ->
 * tick or untick -> "Saved" or a visible error that stays until the next action.
 *
 * Source of the request and reply: wolfstorm php/wolf_ambient.php "RESIDENT REQUESTS" (view / set). Source of the HTTP
 * pattern (verified options, agent + session headers, no retry on a save, json reply): wolfautoenvironment.cpp postCoro.
 * Source of the hidden chat: wolfnpc.cpp requestShips (send_chat_from_viewer, CHAT_TYPE_NORMAL, the RLV channel check).
 * Source of the "/74 traffic on|off" command: region-terrain-work/aircraft-dogfight-2026-11-01/c-wolf-air/src/AirTraffic.cs
 * Command() and AirDrones.cs CHANNEL = 74.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfpassingplanes.h"

#include <boost/json.hpp>

#include "llagent.h"
#include "llcheckboxctrl.h"
#include "llchat.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llsdjson.h"
#include "lltextbox.h"
#include "llviewerregion.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "wolfgrid.h"
#include "wolfregionweather.h"   // [BIRDS 2026-11-02/] setBirdsLocal

// Source: fsnearbychathub.cpp:107 (declared as wolfnpc.cpp:201 declares it) - the viewer's own chat path, so RLV's channel
// rules apply; nothing is added to the resident's chat history
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);

namespace
{
    // Source: wolfautoenvironment.cpp:47 API_URL - the same host; php/wolf_ambient.php is its neighbour.
    const char* const API_URL = "https://wolfstorm.app/php/wolf_ambient.php";
    // Source: c-wolf-air/src/AirDrones.cs:29 CHANNEL = 74; AirPlugin.cs OnEvent hands channel 74 chat to AirTraffic.Command.
    const S32 AIR_CHANNEL = 74;
    // Read once per window open / region change (review 2026-11-01: no polling while the window stays open). Several
    // refreshFromRegion calls in one open count as one; a failed read may be tried again after RETRY_SEC.
    const F64 REREAD_DEBOUNCE_SEC = 2.0;
    const F64 RETRY_SEC = 10.0;
    // How long a read waits for the region to confirm it is on Wolf Territories (the session is only sent then).
    const F32 CONFIRM_WAIT_SEC = 15.f;

    struct State
    {
        LLUUID mRegion;            // the region the state below belongs to
        bool mHave = false;        // a "view" reply is in mReply
        bool mBusy = false;        // a read or a save is on its way
        LLSD mReply;               // {success, ambient (undefined = nobody has chosen), default, on, cats, mayChange}
        std::string mStatus;       // the status line
        bool mStatusError = false;
        bool mStatusFromRead = false;  // the status line is the read's own ("Reading..." or a read failure)
        F64 mFetchedAt = -1.e9;
        F64 mFailedAt = 0.0;       // when the last read failed (0: it did not)
        U64 mSerial = 0;           // bumped on every region change, so an old reply is dropped
    };
    State sState;

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        // Source: wolfautoenvironment.cpp:68 json_to_llsd
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    void set_status(const std::string& msg, bool error, bool from_read = false)
    {
        sState.mStatus = msg;
        sState.mStatusError = error;
        sState.mStatusFromRead = from_read;
    }

    std::string str(LLPanel* panel, const char* name)
    {
        return panel->getString(name);
    }

    /** The state line: on or off, and whether that is the default (and why) or someone's choice. */
    std::string state_line(LLPanel* panel)
    {
        if (!sState.mHave) return std::string();
        const LLSD& r = sState.mReply;
        const bool chosen = r["ambient"].isBoolean();
        const bool on = r["on"].asBoolean();
        if (chosen) return str(panel, on ? "planes_on_chosen" : "planes_off_chosen");
        return str(panel, on ? "planes_on_default" : "planes_off_default");
    }

    /** [BIRDS 2026-11-02] The same for "Birds over this region": the reply's `birds` block (wolf_ambient.php view/set),
     *  absent from an older grid, which leaves the line empty and the box unticked and greyed. */
    bool have_birds()
    {
        return sState.mHave && sState.mReply["birds"].isMap() && sState.mReply["birds"]["on"].isBoolean();
    }

    std::string birds_line(LLPanel* panel)
    {
        if (!have_birds()) return std::string();
        const LLSD& b = sState.mReply["birds"];
        const bool on = b["on"].asBoolean();
        if (b["ambient"].isBoolean()) return str(panel, on ? "birds_on_chosen" : "birds_off_chosen");
        // [BIRDS 2026-11-02] wolf_ambient_lib.php wa_birds_default: Parks / Marinas on, other categories off, none = sea
        if (!b["default"].asBoolean() && b["seaCoast"].asBoolean()) return str(panel, "birds_sea_default");
        return str(panel, on ? "birds_on_default" : "birds_off_default");
    }

    void write_controls(LLPanel* panel)
    {
        LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>("wolf_planes_check");
        LLTextBox* state = panel->findChild<LLTextBox>("wolf_planes_state");
        LLTextBox* status = panel->findChild<LLTextBox>("wolf_planes_status");
        LLView* note = panel->findChildView("wolf_planes_note");
        if (!check || !state || !status || !note) return;
        const bool may = sState.mHave && sState.mReply["mayChange"].asBoolean();
        check->set(sState.mHave && sState.mReply["on"].asBoolean());
        check->setEnabled(may && !sState.mBusy);
        // [BIRDS 2026-11-02] the birds box and its line, from the same reply
        if (LLCheckBoxCtrl* birds = panel->findChild<LLCheckBoxCtrl>("wolf_birds_check"))
        {
            birds->set(have_birds() && sState.mReply["birds"]["on"].asBoolean());
            birds->setEnabled(may && !sState.mBusy && have_birds());
        }
        if (LLTextBox* birds_state = panel->findChild<LLTextBox>("wolf_birds_state"))
        {
            birds_state->setEnabled(true);
            birds_state->setText(birds_line(panel));
        }
        // setCtrlsEnabled greys every child for someone who may not manage the estate; these lines must stay readable.
        state->setEnabled(true);
        note->setEnabled(true);
        status->setEnabled(true);
        state->setText(state_line(panel));
        std::string msg = sState.mStatus;
        if (msg.empty() && sState.mHave && !may) msg = str(panel, "planes_not_yours");
        status->setText(msg);
        // Same colours as the Automatic Environment tab (wolfautoenvironment.cpp:185 setStatus): a failure must be readable.
        status->setColor(sState.mStatusError ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
    }

    LLSD post_and_wait(const std::string& body, bool save, LLCore::HttpStatus& status)
    {
        // Source: wolfautoenvironment.cpp:608-631 postCoro (verified options, the agent + session headers, a JSON body).
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfPassingPlanes", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        if (save) options->setRetries(0);   // a save is never sent twice by the HTTP layer (wolfautoenvironment.cpp:615)
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
        status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
            reply = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        return reply;
    }

    std::string why_failed(const LLCore::HttpStatus& status, const LLSD& reply)
    {
        // Source: wolfautoenvironment.cpp:633-634 - the grid's own words when it gave some
        if (reply.isMap() && reply.has("error")) return reply["error"].asString();
        if (!status) return status.toString();
        return "the service returned an unreadable reply";
    }

    std::string request_body(const char* action, const LLUUID& region, int ambient, bool birds = false)
    {
        boost::json::object o;
        o["action"] = action;
        o["region"] = region.asString();
        if (birds) o["what"] = "birds";   // [BIRDS 2026-11-02/] wolf_ambient.php set {what: "birds"}
        if (ambient >= 0) o["ambient"] = ambient == 1;
        return boost::json::serialize(o);
    }

    void fetch_coro(LLHandle<LLPanel> handle, LLUUID region, U64 serial)
    {
        // The session goes only to Wolf Territories: wait (briefly) for this region to confirm it.
        LLFrameTimer waited;
        while (!WolfGrid::isOnWolfTerritoriesConfirmed() && waited.getElapsedTimeF32() < CONFIRM_WAIT_SEC)
        {
            llcoro::suspendUntilTimeout(0.5f);
            if (sState.mSerial != serial) return;
        }
        LLPanel* panel = handle.get();
        if (sState.mSerial != serial) return;
        if (!WolfGrid::isOnWolfTerritoriesConfirmed() || !gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region)
        {
            sState.mBusy = false;
            sState.mFailedAt = LLFrameTimer::getTotalSeconds();
            if (!panel) return;
            set_status(str(panel, "planes_read_failed") + " " + str(panel, "planes_not_wolf"), true, true);
            write_controls(panel);
            return;
        }
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(request_body("view", region, -1), false, status);
        if (sState.mSerial != serial) return;   // moved on to another region meanwhile
        sState.mBusy = false;
        panel = handle.get();
        if (!status || !reply.isMap() || !reply["success"].asBoolean())
        {
            sState.mFailedAt = LLFrameTimer::getTotalSeconds();
            if (!panel) return;
            set_status(str(panel, "planes_read_failed") + " " + why_failed(status, reply), true, true);
            write_controls(panel);
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        sState.mFailedAt = 0.0;
        // a save's message (or failure) stays until the next action; only the read's own line goes
        if (sState.mStatusFromRead) set_status(std::string(), false);
        if (panel) write_controls(panel);
    }

    void save_coro(LLHandle<LLPanel> handle, LLUUID region, bool on, bool birds, U64 serial)
    {
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(request_body("set", region, on ? 1 : 0, birds), true, status);
        const bool ok = status && reply.isMap() && reply["success"].asBoolean();
        LLPanel* panel = handle.get();
        if (!ok)
        {
            // A change the owner asked for must not fail silently, even with the window closed (wolfautoenvironment.cpp:640).
            // [BIRDS 2026-11-02] Always, not only with the window closed: the status line under the two boxes is 44 high and
            // a long refusal would be cut off there; the alert keeps the whole text, to read and copy.
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                std::string(birds ? "Could not save birds: " : "Could not save passing planes: ") + why_failed(status, reply)));
        }
        if (sState.mSerial != serial) return;
        sState.mBusy = false;
        if (!ok)
        {
            if (panel)
            {
                set_status(str(panel, birds ? "birds_save_failed" : "planes_save_failed") + " " + why_failed(status, reply), true);
                write_controls(panel);   // the check box goes back to what the grid has
            }
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        if (birds)
        {
            // [BIRDS 2026-11-02] No plugin to tell: the viewers draw the birds from weather.php, which every viewer here
            // reads within the minute. This viewer shows the choice now.
            const bool saved_on = have_birds() && sState.mReply["birds"]["on"].asBoolean();
            if (WolfRegionWeather::instanceExists()) WolfRegionWeather::instance().setBirdsLocal(region, saved_on);
            if (panel)
            {
                set_status(str(panel, saved_on ? "birds_saved_on" : "birds_saved_off"), false);
                write_controls(panel);
            }
            return;
        }
        // Tell the running wolf-air now (it reads the record only every 30 minutes): its own "/74 traffic on|off", which
        // checks the same estate rule and answers with a short alert. Only on the region the change was for.
        bool told = false;
        if (WolfGrid::isOnWolfTerritoriesConfirmed() && gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region
            && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(AIR_CHANNEL)))
        {
            send_chat_from_viewer(on ? "traffic on" : "traffic off", CHAT_TYPE_NORMAL, AIR_CHANNEL);
            told = true;
        }
        if (panel)
        {
            std::string msg = str(panel, on ? "planes_saved_on" : "planes_saved_off");
            if (!told) msg += " " + str(panel, "planes_later");
            set_status(msg, false);
            write_controls(panel);
        }
    }

    void on_commit(LLHandle<LLPanel> handle, bool birds)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>(birds ? "wolf_birds_check" : "wolf_planes_check");
        if (!check) return;
        if (sState.mBusy || !sState.mHave || !sState.mReply["mayChange"].asBoolean() || (birds && !have_birds()))
        {
            write_controls(panel);
            return;
        }
        const bool on = check->get();
        const LLUUID region = sState.mRegion;
        const U64 serial = sState.mSerial;
        sState.mBusy = true;
        set_status(str(panel, "planes_saving"), false);
        write_controls(panel);
        LLCoros::instance().launch("WolfPassingPlanesSave", [handle, region, on, birds, serial]() { save_coro(handle, region, on, birds, serial); });
    }
}

void WolfPassingPlanes::postBuild(LLPanel* panel)
{
    LLHandle<LLPanel> handle = panel->getHandle();
    if (LLCheckBoxCtrl* check = panel->findChild<LLCheckBoxCtrl>("wolf_planes_check"))
    {
        check->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, false); });
    }
    if (LLCheckBoxCtrl* birds = panel->findChild<LLCheckBoxCtrl>("wolf_birds_check"))   // [BIRDS 2026-11-02/]
    {
        birds->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_commit(handle, true); });
    }
}

void WolfPassingPlanes::sync(LLPanel* panel, LLViewerRegion* region, bool reread)
{
    if (!panel) return;
    // Wolf Territories' own: gone (not greyed) elsewhere, as the Region tab's Adult Content link (processRegionInfo).
    const bool wolf = WolfGrid::isOnWolfTerritories() && region;
    for (const char* name : { "wolf_planes_check", "wolf_planes_state", "wolf_planes_note", "wolf_planes_status",
                              "wolf_birds_check", "wolf_birds_state" })
    {
        if (LLView* v = panel->findChildView(name)) v->setVisible(wolf);
    }
    if (!wolf) return;
    const LLUUID id = region->getRegionID();
    const F64 now = LLFrameTimer::getTotalSeconds();
    if (id != sState.mRegion)
    {
        sState = State();
        sState.mRegion = id;
        sState.mSerial = (U64)LLFrameTimer::getTotalTime();   // differs from every earlier region's
    }
    const bool first = !sState.mHave && sState.mFailedAt == 0.0 && sState.mFetchedAt < 0.0;
    const bool again = reread && now - sState.mFetchedAt >= REREAD_DEBOUNCE_SEC;
    const bool retry = !sState.mHave && sState.mFailedAt > 0.0 && now - sState.mFailedAt >= RETRY_SEC;
    if (!sState.mBusy && (first || again || retry))
    {
        sState.mBusy = true;
        sState.mFetchedAt = now;
        if (!sState.mHave) set_status(str(panel, "planes_reading"), false, true);
        LLHandle<LLPanel> handle = panel->getHandle();
        const U64 serial = sState.mSerial;
        LLCoros::instance().launch("WolfPassingPlanesRead", [handle, id, serial]() { fetch_coro(handle, id, serial); });
    }
    write_controls(panel);
}
