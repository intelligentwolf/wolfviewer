/**
 * @file wolfestateaccess.cpp
 * @brief [TICKET 544 2026-11-05] Region/Estate > Estate: "Wolf Territories: who can come in" (wolfestateaccess.h).
 *
 * [2026-11-05] Paul: no 18+ box ("a lot of this is covered by setting the region to adult content"); a one-line pointer
 * names the Adult Content setting instead (the Region tab's "Region type on wolf-grid.com..." button). Default off: every
 * box is unticked until an owner or manager ticks it, and nothing here turns a rule on by itself.
 * Who and why: Jimmy Olsen runs regions larger than 12x12 that other viewers crash on, and wants child avatars and
 * visitors kept out of some of them; older residents use this window too, so every box saves at once, says what it did,
 * and a failure stays on screen until the next action. Flow: open the Estate tab -> the estate's rules show -> tick or
 * untick (or set the height) -> "Saved" or a visible error.
 *
 * Source of the request and reply: wolfstorm php/wolf_estate_access.php "RESIDENT REQUESTS" (view / set). Source of the HTTP
 * pattern, the Wolf-only session rule, the status colours and the hidden chat: wolfpassingplanes.cpp (itself
 * wolfautoenvironment.cpp postCoro and wolfnpc.cpp requestShips). The nudge "estateaccess reload" on channel 75 is
 * EstateAccessPlugin.NUDGE_CHANNEL / NUDGE_TEXT (region-terrain-work/estate-access-t544/plugin/src/EstateAccessPlugin.cs).
 * Layout: the block goes in the access area where "Must be 18+", "Must have payment info on file" and "Must not be a
 * scripted agent" are (WolfSim ignores those three: EstateManagementModule.cs:123-124 sets DoDenyMinors / DoDenyAnonymous
 * from IgnoreEstateMinorAccessControl / IgnoreEstatePaymentAccessControl, true by default, and handleEstateChangeInfo reads
 * no bots bit, EstateManagementModule.cs:2120-2162), which are hidden on Wolf Territories; the controls under them move
 * down. The tab scrolls (llfloaterregioninfo.cpp postBuild, ticket 529's way), so a 1360x768 screen reaches everything.
 *
 * Copyright (C) 2026 IntelligentWolf Ltd.
 */

#include "llviewerprecompiledheaders.h"

#include "wolfestateaccess.h"

#include <map>

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
#include "llscrollcontainer.h"
#include "llsdjson.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "llui.h"
#include "llviewerregion.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "wolfgrid.h"

// Source: fsnearbychathub.cpp:107 (declared as wolfpassingplanes.cpp:56 declares it) - the viewer's own chat path, so RLV's
// channel rules apply; nothing is added to the resident's chat history
void send_chat_from_viewer(std::string utf8_out_text, EChatType type, S32 channel);

namespace
{
    // Source: wolfpassingplanes.cpp:59 API_URL's host; php/wolf_estate_access.php is its neighbour.
    const char* const API_URL = "https://wolfstorm.app/php/wolf_estate_access.php";
    // Source: EstateAccessPlugin.cs NUDGE_CHANNEL = 75, NUDGE_TEXT = "estateaccess reload"
    const S32 NUDGE_CHANNEL = 75;
    const char* const NUDGE_TEXT = "estateaccess reload";
    // Source: php/wolf_estate_access.php WEA_MIN_CM / WEA_MAX_CM (and AccessRules.MIN_HEIGHT_CM / MAX_HEIGHT_CM)
    const S32 MIN_CM = 50, MAX_CM = 250;
    // As wolfpassingplanes.cpp: read once per window open / region change, retry a failed read after RETRY_SEC, and wait up
    // to CONFIRM_WAIT_SEC for the region to confirm it is on Wolf Territories (the session is only sent then).
    const F64 REREAD_DEBOUNCE_SEC = 2.0;
    const F64 RETRY_SEC = 10.0;
    const F32 CONFIRM_WAIT_SEC = 15.f;
    // The height is saved this long after the last spinner change (each arrow click commits; the grid allows 20 a minute).
    const F32 HEIGHT_SETTLE_SEC = 1.5f;
    // How long a waiting height save waits for another save to finish (the HTTP timeout is 30 s) before it says it gave up.
    const F32 HEIGHT_WAIT_BUSY_SEC = 40.f;

    /** The on/off rules: the check box, its note, and the name wolf_estate_access.php uses. */
    struct Flag { const char* check; const char* note; const char* what; };
    const Flag FLAGS[] = {
        { "wolf_npcs_check",    "wolf_npcs_note",    "block_npcs" },
        { "wolf_animesh_check", "wolf_animesh_note", "block_animesh" },
        { "wolf_viewers_check", "wolf_viewers_note", "wolf_viewers_only" },
        { "wolf_hg_check",      "wolf_hg_note",      "block_hypergrid" },
    };

    /** Every Wolf control of the block (shown on Wolf Territories only). */
    const char* const WOLF_VIEWS[] = {
        "wolf_access_heading", "wolf_access_intro",
        "wolf_adults_pointer", "wolf_npcs_check", "wolf_npcs_note", "wolf_animesh_check", "wolf_animesh_note",
        "wolf_height_check", "wolf_height_spin", "wolf_height_unit", "wolf_height_note",
        "wolf_viewers_check", "wolf_viewers_note", "wolf_hg_check", "wolf_hg_note", "wolf_access_status",
    };
    /** The stock boxes the block replaces on Wolf Territories (inert on WolfSim, see the top of the file). */
    const char* const STOCK_HIDDEN[] = { "limit_age_verified", "limit_payment", "limit_bots" };
    /** The stock controls under them, moved down by the block, and the border round the access area. */
    const char* const STOCK_BELOW[] = { "parcel_access_override", "voice_chat_check", "allow_direct_teleport", "apply_btn",
                                        "message_estate_btn", "kick_user_from_estate_btn" };
    const char* const BORDER = "access_border";

    struct State
    {
        LLUUID mRegion;            // the region the state below belongs to
        bool mHave = false;        // a "view" reply is in mReply
        bool mBusy = false;        // a read or a save is on its way
        LLSD mReply;               // {success, estate, estate_name, rules: {...}, mayChange}
        std::string mStatus;
        bool mStatusError = false;
        bool mStatusFromRead = false;
        F64 mFetchedAt = -1.e9;
        F64 mFailedAt = 0.0;
        U64 mSerial = 0;           // bumped on every region change, so an old reply is dropped
        U64 mHeightSerial = 0;     // bumped on every spinner change: only the last one is saved
        bool mHeightPending = false;   // a spinner change waits to be saved: the spinner keeps what the owner set (review 11-05)
    };
    State sState;

    /** Where each stock control started, as its distance from the panel's top (it keeps that while the panel is resized:
     *  every control follows the top), so the block can be put in and taken out again. */
    struct Place { S32 fromTop; S32 left; S32 width; S32 height; };
    struct Origin
    {
        LLHandle<LLPanel> mPanel;
        std::map<std::string, Place> mPlaces;
    };
    Origin sOrigin;

    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        // Source: wolfpassingplanes.cpp json_to_llsd (itself wolfautoenvironment.cpp:68)
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

    std::string str(LLPanel* panel, const char* name) { return panel->getString(name); }

    const LLSD& rules() { return sState.mReply["rules"]; }
    bool have_rules() { return sState.mHave && sState.mReply["rules"].isMap(); }

    void place_from_top(LLPanel* panel, LLView* v, S32 from_top, S32 left, S32 width, S32 height)
    {
        LLRect r;
        r.setLeftTopAndSize(left, panel->getRect().getHeight() - from_top, width, height);
        v->setShape(r);
    }

    /** Puts the Wolf block in the access area (or takes it out) at the panel's current width; returns how far from the top
     *  the lowest control ends. */
    S32 stack(LLPanel* panel, bool wolf)
    {
        constexpr S32 RIGHT_MARGIN = 10, MIN_TEXT = 14, BOTTOM_MARGIN = 8;
        const S32 panel_width = panel->getRect().getWidth();
        auto orig = [](const char* name) -> const Place* {
            auto it = sOrigin.mPlaces.find(name);
            return it == sOrigin.mPlaces.end() ? nullptr : &it->second;
        };
        const Place* first = orig("limit_age_verified");
        const Place* below = orig("parcel_access_override");
        if (!first || !below) return 0;
        S32 top = first->fromTop;   // grows downwards from here
        auto put = [&](LLView* v, S32 height, S32 gap)
        {
            const LLRect r = v->getRect();
            place_from_top(panel, v, top, r.mLeft, r.getWidth(), height);
            top += height + gap;
        };
        auto text = [&](const char* name, S32 gap)
        {
            LLTextBox* t = panel->findChild<LLTextBox>(name);
            if (!t) return;
            // the width first: the wrapped height is measured at the width the box will have (wolfpassingplanes.cpp stack)
            LLRect r = t->getRect();
            r.mRight = r.mLeft + llmax(0, panel_width - RIGHT_MARGIN - r.mLeft);
            t->setShape(r);
            const bool empty = t->getText().empty();
            put(t, empty ? 0 : llmax(MIN_TEXT, t->getTextPixelHeight() + 2), empty ? 0 : gap);
        };
        auto check = [&](const char* name, S32 gap)
        {
            if (LLView* c = panel->findChildView(name)) put(c, c->getRect().getHeight(), gap);
        };
        if (wolf)
        {
            top += 2;
            text("wolf_access_heading", 1);
            text("wolf_access_intro", 4);
            text("wolf_adults_pointer", 4);   // where age gating lives: the Adult Content region type (no 18+ box here)
            for (const Flag& f : FLAGS)
            {
                if (std::string(f.what) == "wolf_viewers_only")
                {
                    // the height row goes before the viewer rule (the ticket's order: NPCs, animesh, height, viewer, HG)
                    LLView* hc = panel->findChildView("wolf_height_check");
                    LLView* spin = panel->findChildView("wolf_height_spin");
                    LLView* unit = panel->findChildView("wolf_height_unit");
                    if (hc && spin && unit)
                    {
                        const S32 row = top;
                        const LLRect sr = spin->getRect(), ur = unit->getRect();
                        place_from_top(panel, spin, row - 1, sr.mLeft, sr.getWidth(), sr.getHeight());
                        place_from_top(panel, unit, row + 1, ur.mLeft, ur.getWidth(), ur.getHeight());
                        put(hc, llmax(hc->getRect().getHeight(), sr.getHeight() - 1), 1);
                    }
                    text("wolf_height_note", 4);
                }
                check(f.check, 0);
                text(f.note, 4);
            }
            text("wolf_access_status", 6);
        }
        else
        {
            top = below->fromTop;   // nothing put in: everything where the XML has it
        }
        // the stock controls under the block: moved down by however far the block (or nothing) reaches past them
        const S32 shift = top - below->fromTop;
        S32 lowest = 0;
        for (const char* name : STOCK_BELOW)
        {
            const Place* p = orig(name);
            LLView* v = panel->findChildView(name);
            if (!p || !v) continue;
            place_from_top(panel, v, p->fromTop + shift, p->left, p->width, p->height);
            lowest = llmax(lowest, p->fromTop + shift + p->height);
        }
        if (const Place* b = orig(BORDER))
        {
            if (LLView* v = panel->findChildView(BORDER)) place_from_top(panel, v, b->fromTop, b->left, b->width, b->height + shift);
        }
        return lowest + BOTTOM_MARGIN;
    }

    /** [TICKET 529's way, wolfpassingplanes.cpp layout] The panel sits in a scroll container (llfloaterregioninfo.cpp
     *  postBuild): it is made as tall as its contents (never less than the view) and the rest scrolls. When the scroll bar
     *  shows, the panel is narrowed by its width and the block restacked at that width. The window is never grown here (the
     *  Region tab's layout owns any growth of this window). */
    void layout(LLPanel* panel, bool wolf)
    {
        LLScrollContainer* scroll = dynamic_cast<LLScrollContainer*>(panel->getParent());
        if (!scroll)
        {
            stack(panel, wolf);
            return;
        }
        LLFloater* floater = panel->getParentByType<LLFloater>();
        if (floater && floater->isMinimized()) return;   // a minimised window's panel is squeezed: leave it until restored
        const S32 border = scroll->getBorderWidth();
        const S32 view_width = scroll->getRect().getWidth() - 2 * border;
        const S32 view_height = scroll->getRect().getHeight() - 2 * border;
        auto fit = [panel, wolf](S32 width) -> S32
        {
            LLRect r = panel->getRect();
            if (r.getWidth() != width)
            {
                r.mRight = r.mLeft + width;
                panel->setShape(r);
            }
            return stack(panel, wolf);
        };
        S32 needed = fit(view_width);
        if (needed > view_height)
        {
            static LLUICachedControl<S32> scrollbar_size_control("UIScrollbarSize", 0);   // as llscrollcontainer.cpp
            const S32 scrollbar = scroll->getSize() == -1 ? (S32)scrollbar_size_control : scroll->getSize();
            needed = fit(view_width - scrollbar);
        }
        LLRect r = panel->getRect();
        r.setLeftTopAndSize(r.mLeft, r.mTop, r.getWidth(), llmax(view_height, needed));
        panel->setShape(r);   // every control follows the top
    }

    S32 height_cm(LLPanel* panel)
    {
        LLSpinCtrl* spin = panel->findChild<LLSpinCtrl>("wolf_height_spin");
        if (!spin) return 0;
        return llclamp((S32)ll_round(spin->getValueF32() * 100.f), MIN_CM, MAX_CM);
    }

    void write_controls(LLPanel* panel)
    {
        const bool may = have_rules() && sState.mReply["mayChange"].asBoolean();
        const bool usable = may && !sState.mBusy;
        for (const Flag& f : FLAGS)
        {
            if (LLCheckBoxCtrl* c = panel->findChild<LLCheckBoxCtrl>(f.check))
            {
                c->set(have_rules() && rules()[f.what].asBoolean());
                c->setEnabled(usable);
            }
            if (LLView* n = panel->findChildView(f.note)) n->setEnabled(true);
        }
        const S32 cm = have_rules() ? rules()["min_height_cm"].asInteger() : 0;
        if (LLCheckBoxCtrl* hc = panel->findChild<LLCheckBoxCtrl>("wolf_height_check"))
        {
            hc->set(cm > 0);
            hc->setEnabled(usable);
        }
        if (LLSpinCtrl* spin = panel->findChild<LLSpinCtrl>("wolf_height_spin"))
        {
            if (cm > 0 && !sState.mHeightPending) spin->set((F32)cm / 100.f);   // never over a change still to be saved
            spin->setEnabled(usable && cm > 0);
        }
        // setCtrlsEnabled greys every child for someone who may not manage the estate; the words must stay readable
        for (const char* name : { "wolf_access_heading", "wolf_access_intro", "wolf_adults_pointer", "wolf_height_unit", "wolf_height_note", "wolf_access_status" })
        {
            if (LLView* v = panel->findChildView(name)) v->setEnabled(true);
        }
        if (LLTextBox* status = panel->findChild<LLTextBox>("wolf_access_status"))
        {
            std::string msg = sState.mStatus;
            if (msg.empty() && sState.mHave && !may) msg = str(panel, "wolf_not_yours");
            status->setText(msg);
            // Same colours as wolfpassingplanes.cpp write_controls (itself wolfautoenvironment.cpp:185): a failure must be readable.
            status->setColor(sState.mStatusError ? LLColor4(0.88f, 0.33f, 0.28f, 1.f) : LLColor4(0.79f, 0.64f, 0.15f, 1.f));
        }
        layout(panel, true);   // the texts may have changed height
    }

    LLSD post_and_wait(const std::string& body, bool save, LLCore::HttpStatus& status)
    {
        // Source: wolfpassingplanes.cpp post_and_wait (wolfautoenvironment.cpp:608-631 postCoro): verified options, the
        // agent + session headers, a JSON body, a save never sent twice by the HTTP layer.
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfEstateAccess", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        if (save) options->setRetries(0);
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
        // Source: wolfpassingplanes.cpp why_failed - the grid's own words when it gave some
        if (reply.isMap() && reply.has("error")) return reply["error"].asString();
        if (!status) return status.toString();
        return "the service returned an unreadable reply";
    }

    void fetch_coro(LLHandle<LLPanel> handle, LLUUID region, U64 serial)
    {
        // The session goes only to Wolf Territories: wait (briefly) for this region to confirm it (wolfpassingplanes.cpp fetch_coro).
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
            set_status(str(panel, "wolf_read_failed") + " " + str(panel, "wolf_not_wolf"), true, true);
            write_controls(panel);
            return;
        }
        boost::json::object o;
        o["action"] = "view";
        o["region"] = region.asString();
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(boost::json::serialize(o), false, status);
        if (sState.mSerial != serial) return;   // moved on to another region meanwhile
        sState.mBusy = false;
        panel = handle.get();
        if (!status || !reply.isMap() || !reply["success"].asBoolean() || !reply["rules"].isMap())
        {
            sState.mFailedAt = LLFrameTimer::getTotalSeconds();
            if (!panel) return;
            set_status(str(panel, "wolf_read_failed") + " " + why_failed(status, reply), true, true);
            write_controls(panel);
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        sState.mFailedAt = 0.0;
        if (sState.mStatusFromRead) set_status(std::string(), false);   // a save's message stays until the next action
        if (panel) write_controls(panel);
    }

    /** One rule saved: {what, on} for a box, {what: "min_height", cm} for the height. */
    void save_coro(LLHandle<LLPanel> handle, LLUUID region, std::string body, U64 serial)
    {
        LLCore::HttpStatus status;
        const LLSD reply = post_and_wait(body, true, status);
        const bool ok = status && reply.isMap() && reply["success"].asBoolean() && reply["rules"].isMap();
        LLPanel* panel = handle.get();
        if (!ok)
        {
            // A change the owner asked for must not fail silently, even with the window closed (wolfpassingplanes.cpp
            // save_coro): the alert keeps the whole text, to read and copy, and stays until it is closed.
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Could not save the estate's entry rules: " + why_failed(status, reply)));
        }
        if (sState.mSerial != serial) return;
        sState.mBusy = false;
        if (!ok)
        {
            if (panel)
            {
                set_status(str(panel, "wolf_save_failed") + " " + why_failed(status, reply), true);
                write_controls(panel);   // the boxes go back to what the grid has
            }
            return;
        }
        sState.mReply = reply;
        sState.mHave = true;
        // Tell the region the change was made on, so it follows at once; the estate's other regions read it within 10 minutes.
        bool told = false;
        if (WolfGrid::isOnWolfTerritoriesConfirmed() && gAgent.getRegion() && gAgent.getRegion()->getRegionID() == region
            && !(rlv_handler_t::isEnabled() && !RlvActions::canSendChannel(NUDGE_CHANNEL)))
        {
            send_chat_from_viewer(NUDGE_TEXT, CHAT_TYPE_NORMAL, NUDGE_CHANNEL);
            told = true;
        }
        if (panel)
        {
            set_status(str(panel, told ? "wolf_saved" : "wolf_saved_later"), false);
            write_controls(panel);
        }
    }

    void start_save(LLPanel* panel, const std::string& body)
    {
        const LLUUID region = sState.mRegion;
        const U64 serial = sState.mSerial;
        LLHandle<LLPanel> handle = panel->getHandle();
        sState.mBusy = true;
        set_status(str(panel, "wolf_saving"), false);
        write_controls(panel);
        LLCoros::instance().launch("WolfEstateAccessSave", [handle, region, body, serial]() { save_coro(handle, region, body, serial); });
    }

    bool may_change() { return !sState.mBusy && have_rules() && sState.mReply["mayChange"].asBoolean(); }

    void on_flag(LLHandle<LLPanel> handle, size_t index)
    {
        LLPanel* panel = handle.get();
        if (!panel || index >= LL_ARRAY_SIZE(FLAGS)) return;
        LLCheckBoxCtrl* c = panel->findChild<LLCheckBoxCtrl>(FLAGS[index].check);
        if (!c || !may_change()) { write_controls(panel); return; }
        boost::json::object o;
        o["action"] = "set";
        o["region"] = sState.mRegion.asString();
        o["what"] = FLAGS[index].what;
        o["on"] = c->get();
        start_save(panel, boost::json::serialize(o));
    }

    void save_height(LLPanel* panel, S32 cm)
    {
        boost::json::object o;
        o["action"] = "set";
        o["region"] = sState.mRegion.asString();
        o["what"] = "min_height";
        o["cm"] = cm;
        start_save(panel, boost::json::serialize(o));
    }

    void on_height_check(LLHandle<LLPanel> handle)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLCheckBoxCtrl* hc = panel->findChild<LLCheckBoxCtrl>("wolf_height_check");
        if (!hc || !may_change()) { write_controls(panel); return; }
        ++sState.mHeightSerial;   // a spinner save still waiting is dropped: this one decides
        sState.mHeightPending = false;
        save_height(panel, hc->get() ? height_cm(panel) : 0);
    }

    void on_height_spin(LLHandle<LLPanel> handle)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLCheckBoxCtrl* hc = panel->findChild<LLCheckBoxCtrl>("wolf_height_check");
        if (!hc || !hc->get() || !have_rules() || !sState.mReply["mayChange"].asBoolean()) return;
        const U64 mine = ++sState.mHeightSerial;
        const U64 serial = sState.mSerial;
        sState.mHeightPending = true;
        LLCoros::instance().launch("WolfEstateAccessHeight", [handle, mine, serial]()
        {
            llcoro::suspendUntilTimeout(HEIGHT_SETTLE_SEC);
            // another save on its way: wait for it (review 11-05: never drop the owner's value without a word)
            LLFrameTimer waited;
            while (sState.mBusy && sState.mHeightSerial == mine && sState.mSerial == serial
                   && waited.getElapsedTimeF32() < HEIGHT_WAIT_BUSY_SEC)
            {
                llcoro::suspendUntilTimeout(0.5f);
            }
            if (sState.mHeightSerial != mine || sState.mSerial != serial) return;   // a newer change, or another region
            sState.mHeightPending = false;
            LLPanel* p = handle.get();
            if (!p) return;
            LLCheckBoxCtrl* c = p->findChild<LLCheckBoxCtrl>("wolf_height_check");
            if (!c || !c->get()) { write_controls(p); return; }
            if (!may_change())
            {
                set_status(str(p, "wolf_height_not_saved"), true);
                write_controls(p);
                return;
            }
            const S32 cm = height_cm(p);
            if (have_rules() && rules()["min_height_cm"].asInteger() == cm) { write_controls(p); return; }   // nothing changed
            save_height(p, cm);
        });
    }

    void record_origins(LLPanel* panel)
    {
        sOrigin.mPanel = panel->getHandle();
        sOrigin.mPlaces.clear();
        const S32 h = panel->getRect().getHeight();
        auto keep = [&](const char* name)
        {
            if (LLView* v = panel->findChildView(name))
            {
                const LLRect r = v->getRect();
                sOrigin.mPlaces[name] = Place{ h - r.mTop, r.mLeft, r.getWidth(), r.getHeight() };
            }
        };
        for (const char* name : STOCK_HIDDEN) keep(name);
        for (const char* name : STOCK_BELOW) keep(name);
        keep(BORDER);
    }
}

void WolfEstateAccess::postBuild(LLPanel* panel)
{
    record_origins(panel);
    LLHandle<LLPanel> handle = panel->getHandle();
    for (size_t i = 0; i < LL_ARRAY_SIZE(FLAGS); ++i)
    {
        if (LLCheckBoxCtrl* c = panel->findChild<LLCheckBoxCtrl>(FLAGS[i].check))
            c->setCommitCallback([handle, i](LLUICtrl*, const LLSD&) { on_flag(handle, i); });
    }
    if (LLCheckBoxCtrl* hc = panel->findChild<LLCheckBoxCtrl>("wolf_height_check"))
        hc->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_height_check(handle); });
    if (LLSpinCtrl* spin = panel->findChild<LLSpinCtrl>("wolf_height_spin"))
        spin->setCommitCallback([handle](LLUICtrl*, const LLSD&) { on_height_spin(handle); });
}

void WolfEstateAccess::sync(LLPanel* panel, LLViewerRegion* region, bool reread)
{
    if (!panel) return;
    if (sOrigin.mPanel.get() != panel) record_origins(panel);   // a rebuilt window: its own starting places
    // Wolf Territories' own: gone (not greyed) elsewhere, where the stock boxes come back (paul-wolf-features-wolf-grid-only).
    const bool wolf = WolfGrid::isOnWolfTerritories() && region;
    for (const char* name : WOLF_VIEWS)
    {
        if (LLView* v = panel->findChildView(name)) v->setVisible(wolf);
    }
    for (const char* name : STOCK_HIDDEN)
    {
        if (LLView* v = panel->findChildView(name)) v->setVisible(!wolf);
    }
    if (!wolf)
    {
        layout(panel, false);   // the stock tab exactly as it was
        return;
    }
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
        if (!sState.mHave) set_status(str(panel, "wolf_reading"), false, true);
        LLHandle<LLPanel> handle = panel->getHandle();
        const U64 serial = sState.mSerial;
        LLCoros::instance().launch("WolfEstateAccessRead", [handle, id, serial]() { fetch_coro(handle, id, serial); });
    }
    write_controls(panel);
}
