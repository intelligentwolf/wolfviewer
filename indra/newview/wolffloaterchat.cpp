/**
 * @file wolffloaterchat.cpp
 * @brief WolfViewer Wolf Chat: a group's text and voice channels in one window (2026-10-20).
 *
 * See wolffloaterchat.h. The service is php/wolfchat.php (WolfStorm web hosts); every request carries
 * the session pair (X-Wolf-Agent / X-Wolf-Session) as WolfAdventure::api does, and is only sent while the
 * region underfoot has confirmed it is Wolf Territories (WolfGrid::isOnWolfTerritoriesConfirmed).
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterchat.h"

#include <algorithm>
#include <ctime>

#include <boost/json.hpp>

#include "llagent.h"
#include "llavataractions.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llradiogroup.h"
#include "llscrollcontainer.h"
#include "llchatentry.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "llcommandhandler.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventtimer.h"
#include "llfloateravatarpicker.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llgltexture.h"
#include "llhttpconstants.h"
#include "llimview.h"
#include "llmutelist.h"
#include "exogroupmutelist.h"
#include "rlvactions.h"
#include "llinventorymodel.h"
#include "llkeyboard.h"
#include "lllayoutstack.h"
#include "llmenugl.h"
#include "lllineeditor.h"
#include "lllocalcliprect.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltexturectrl.h"
#include "lltooltip.h"
#include "llui.h"
#include "lluictrlfactory.h"
#include "llurlaction.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llviewermenu.h"
#include "llviewertexture.h"
#include "llvoiceclient.h"
#include "llweb.h"
#include "llwindow.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfChatSidebar> r_wolf_chat_sidebar("wolf_chat_sidebar");
static LLDefaultChildRegistry::Register<WolfChatCards> r_wolf_chat_cards("wolf_chat_cards");          // [2026-10-21]
static LLDefaultChildRegistry::Register<WolfChatGifGrid> r_wolf_chat_gif_grid("wolf_chat_gif_grid");  // [2026-10-21]
static LLDefaultChildRegistry::Register<WolfChatServerRail> r_wolf_chat_server_rail("wolf_chat_server_rail");  // [2026-10-21]

namespace
{
    // php/wolfchat.php on the WolfStorm web hosts - the host every WolfViewer call to a wolfstorm/php service uses
    // (wolfadventure.cpp API_URL, wolfcave.cpp WolfCaves::API_URL).
    const char* const API_URL = "https://wolfstorm.app/php/wolfchat.php";
    // [2026-10-21] The Discord bridge's viewer side (php/wolfbridge.php: link_code / links / unlink), same host and the
    // same session headers as wolfchat.php.
    const char* const BRIDGE_URL = "https://wolfstorm.app/php/wolfbridge.php";
    // Wolf-Bot's invite (the bridge's Discord application; given with the contract 2026-10-21).
    const char* const DISCORD_BOT_INVITE = "https://discord.com/oauth2/authorize?client_id=1151805124855992403&scope=bot+applications.commands&permissions=536988672";

    // How often the open window asks for news (seconds). Messages of the channel you look at, the channels with
    // who is in which voice room, the members online; the voice heartbeat matches php/wolfchat.php WCH_VOICE_FRESH.
    const F64 POLL_MESSAGES = 3.0;
    const F64 POLL_CHANNELS = 10.0;
    const F64 POLL_ONLINE = 30.0;
    const F64 POLL_SERVERS = 30.0;      // [2026-10-21] the rail's unread / @ badges
    const F32 VOICE_HEARTBEAT = 20.f;

    // Sidebar geometry (pixels).
    const S32 PAD = 8;
    const S32 HEAD_H = 26;
    const S32 ROW_H = 24;
    const S32 PERSON_H = 20;
    const S32 ICON = 14;

    // The Wolf look of wolffloaterdjshows.cpp: a dark panel, every text colour at least 4.5:1 against it.
    const LLColor4 C_BG(0.075f, 0.082f, 0.098f, 1.f);
    const LLColor4 C_ROW_HOVER(0.13f, 0.14f, 0.17f, 1.f);
    const LLColor4 C_ROW_SELECTED(0.18f, 0.20f, 0.24f, 1.f);
    const LLColor4 C_TEXT(0.93f, 0.94f, 0.96f, 1.f);
    const LLColor4 C_DIM(0.66f, 0.69f, 0.74f, 1.f);
    const LLColor4 C_ACCENT(0.64f, 0.90f, 0.21f, 1.f);      // the Wolf lime of the website
    const LLColor4 C_SKELETON(0.16f, 0.17f, 0.20f, 1.f);
    const LLColor4 C_ERROR(1.f, 0.56f, 0.52f, 1.f);
    const LLColor4 C_OK(0.64f, 0.90f, 0.21f, 1.f);
    const LLColor4 C_NAME(0.55f, 0.80f, 1.f, 1.f);
    // [2026-10-21] Badges: white text on these is above 4.5:1 (relative luminance 0.13 and 0.08 against 1.0).
    const LLColor4 C_BADGE_MENTION(0.75f, 0.18f, 0.18f, 1.f);
    const LLColor4 C_BADGE_UNREAD(0.30f, 0.32f, 0.36f, 1.f);
    const LLColor4 C_CARD(0.11f, 0.12f, 0.145f, 1.f);
    const LLColor4 C_FOCUS(0.64f, 0.90f, 0.21f, 1.f);       // keyboard focus ring: the Wolf lime
    const LLColor4 C_DISCORD(0.72f, 0.75f, 1.f, 1.f);       // [2026-10-21] names of people writing from Discord (and the word "(Discord)")

    // [2026-10-21] The background check for new messages while Wolf Chat is closed: how often, and the longest wait
    // after failures (doubling from the first).
    const F32 NOTIFY_POLL = 30.f;
    const F64 NOTIFY_BACKOFF_MAX = 600.0;

    const LLFontGL* font() { return LLFontGL::getFontSansSerif(); }
    const LLFontGL* fontSmall() { return LLFontGL::getFontSansSerifSmall(); }
    const LLFontGL* fontBold() { return LLFontGL::getFontSansSerifBold(); }

    void text(const LLFontGL* f, const std::string& s, F32 x, F32 y, const LLColor4& c,
              LLFontGL::HAlign h = LLFontGL::LEFT, S32 max_px = S32_MAX)
    {
        f->renderUTF8(s, 0, x, y, c, h, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW, S32_MAX, max_px, nullptr, true);
    }

    void icon(const std::string& name, S32 x, S32 y, S32 size, const LLColor4& c)
    {
        LLUIImagePtr img = LLUI::getUIImage(name);
        if (img) img->draw(x, y, size, size, c);
    }

    /** The words for a refused request whose body did not come through (wolfadventure.cpp statusWords). */
    std::string statusWords(S32 status, const std::string& transport)
    {
        switch (status)
        {
            case 400: return "Wolf Chat could not read the request - update WolfViewer and try again.";
            case 401: return "The grid could not confirm you are signed in - log out and back in, then try again.";
            case 403: return "Wolf Chat refused it: your group role does not allow this (if it should, log out and back in).";
            case 404: return "That is gone (deleted by someone else), or Wolf Chat is not on this grid yet.";
            case 409: return "That clashes with something already there.";
            case 429: return "Too many requests - wait a moment and try again.";
            case 503: return "Wolf Chat is busy or down - try again in a moment.";
            default:  return transport.empty() ? llformat("Wolf Chat answered HTTP %d.", status) : "Wolf Chat could not be reached: " + transport;
        }
    }

    std::string clockText(S32 unix_seconds)
    {
        if (unix_seconds <= 0) return std::string();
        std::time_t t = (std::time_t)unix_seconds;
        std::time_t now = std::time(nullptr);
        std::tm when = *std::localtime(&t);
        std::tm today = *std::localtime(&now);
        char buf[64];
        if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday)
            std::strftime(buf, sizeof(buf), "Today at %H:%M", &when);
        else
            std::strftime(buf, sizeof(buf), "%d %b %Y %H:%M", &when);
        return buf;
    }

    std::string myName()
    {
        LLAvatarName av;
        if (LLAvatarNameCache::get(gAgentID, &av)) return av.getCompleteName();
        return std::string();
    }

    // ---- the voice room ------------------------------------------------------------------------------------------
    WolfChatVoiceChannel* sRoom = nullptr;      // the room joined last
    // [review] Every room object ever made this session, by channel - never deleted: FSFloaterVoiceControls and
    // LLSpeakerMgr keep raw pointers to the channel they last saw (fsfloatervoicecontrols.cpp sCurrentVoiceChannel),
    // and LLVoiceChannel registers itself by session id (llvoicechannel.cpp sVoiceChannelMap), so a room joined again
    // is the same object with fresh credentials.
    std::map<LLUUID, WolfChatVoiceChannel*> sRooms;
    LLUUID sRoomGroup;
    LLUUID sRoomChannel;
    std::string sRoomName;
    bool sRoomTold = false;                     // the service has been told we are in it

    /** Tells php/wolfchat.php we are in the room (with mic / deafen) every VOICE_HEARTBEAT seconds while we are,
     *  and that we left when we are not any more - whatever ended it (Leave, another call, a teleport). */
    // [2026-10-21] Paul: "it thought i was in the room but when i did my settings it disconnected me". The room follows
    // the REAL voice state: the channel's own state (llvoicechannel.h:40-49 EState; STATE_CONNECTED is set when the voice
    // client reports STATUS_JOINED, llvoicechannel.cpp:174-178) and whether it is still the current channel
    // (llvoicechannel.h:100 getCurrentVoiceChannel). Opening voice device settings suspends the call
    // (llpanelvoicedevicesettings.cpp:342 LLVoiceChannel::suspend) and closing them resumes it, which - stock Firestorm -
    // re-activates the same group channel (llvoicechannel.cpp:366-395 resume: STATE_READY then activate) and so starts a
    // fresh ad-hoc connection (llvoicechannel.cpp:457 setNonSpatialChannel -> llvoicewebrtc.cpp:1852-1862
    // startAdHocSession, which leaves the current session first). Paul's log 10-21 06:17:40: that reconnection never
    // came back. So: while you want to be in a room, a room that is not connected within ROOM_CONNECT_WAIT seconds
    // (not counting a suspension) is joined again once with fresh credentials; if that fails too, it is left and the
    // window says so. A call that takes over (another channel becomes current) leaves the room.
    const F32 ROOM_WATCH_PERIOD = 2.f;
    const F64 ROOM_CONNECT_WAIT = 20.0;
    bool sWantRoom = false;                 // you joined a room and have not left it
    bool sRejoinTried = false;              // the one automatic rejoin for this drop
    bool sRejoining = false;
    F64 sNotConnectedSince = 0.0;
    std::string sRoomNote;                  // what the window says about the room when it is not connected

    void roomWatch()
    {
        if (!sRoom || !sWantRoom || sRejoining) return;
        if (LLVoiceChannel::isSuspended())  // llvoicechannel.h:106 - settings open: wait, do not count
        {
            sNotConnectedSince = 0.0;
            return;
        }
        if (LLVoiceChannel::getCurrentVoiceChannel() != sRoom)
        {
            sWantRoom = false;
            sRoomNote = "You left " + sRoomName + ": another voice call or channel took over.";
            return;                          // the heartbeat tells the service (inRoom() is false now)
        }
        if (sRoom->getState() == LLVoiceChannel::STATE_CONNECTED)
        {
            sNotConnectedSince = 0.0;
            sRejoinTried = false;
            sRoomNote.clear();
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (sNotConnectedSince <= 0.0)
        {
            sNotConnectedSince = now;
            return;
        }
        if (now - sNotConnectedSince < ROOM_CONNECT_WAIT) return;
        sNotConnectedSince = 0.0;
        const LLUUID group = sRoomGroup, channel = sRoomChannel;
        const std::string name = sRoomName;
        if (sRejoinTried)
        {
            WolfChat::leaveRoom();
            sRoomNote = "Voice dropped - click " + name + " to rejoin.";
            return;
        }
        sRejoinTried = true;
        sRejoining = true;
        sRoomNote = "Voice dropped - rejoining " + name + "...";
        LL_INFOS("WolfChat") << "Voice room " << name << " not connected for " << ROOM_CONNECT_WAIT << " s - rejoining once" << LL_ENDL;
        WolfChat::joinRoom(group, channel, name, [name](bool ok, const std::string& error)
        {
            sRejoining = false;
            if (!ok)
            {
                sWantRoom = false;
                sRoomNote = "Voice dropped - click " + name + " to rejoin. (" + error + ")";
            }
        });
    }

    class WolfChatHeartbeat : public LLEventTimer
    {
    public:
        WolfChatHeartbeat() : LLEventTimer(ROOM_WATCH_PERIOD) {}
        bool tick() override
        {
            roomWatch();
            // The service hears from us every VOICE_HEARTBEAT seconds (php/wolfchat.php WCH_VOICE_FRESH).
            mSince += ROOM_WATCH_PERIOD;
            if (mSince < VOICE_HEARTBEAT && !(sRoomTold && !WolfChat::inRoom())) return false;
            mSince = 0.f;
            if (WolfChat::inRoom())
            {
                LLSD b;
                b["channel"] = sRoomChannel;
                b["muted"] = WolfChat::micOn() ? 0 : 1;
                b["deafened"] = WolfChat::deafened() ? 1 : 0;
                WolfChat::api("voice_here", b, nullptr);
                sRoomTold = true;
            }
            else if (sRoomTold)
            {
                WolfChat::api("voice_leave", LLSD::emptyMap(), nullptr);
                sRoomTold = false;
            }
            return false;   // keep ticking
        }
    private:
        F32 mSince = 0.f;
    };
    WolfChatHeartbeat* sHeartbeat = nullptr;

    /** "99+" past 99 (php/wolfchat.php counts unread up to WCH_UNREAD_CAP = 100). */
    std::string countText(S32 n)
    {
        return n > 99 ? std::string("99+") : llformat("%d", n);
    }

    /** A rounded-looking pill with white text, right-aligned at `right`; returns its left edge. */
    S32 badge(const std::string& label, S32 right, F32 cy, const LLColor4& bg)
    {
        const S32 w = fontSmall()->getWidth(label) + 10;
        const S32 h = 16;
        const S32 bottom = (S32)cy - h / 2;
        gl_rect_2d(right - w, bottom + h, right, bottom, bg, true);
        text(fontSmall(), label, (F32)(right - w / 2), cy, LLColor4::white, LLFontGL::HCENTER);
        return right - w;
    }

    /** [2026-10-21] Asks php/wolfchat.php every NOTIFY_POLL seconds for new messages the bells let through and shows
     *  them as notification toasts ("WolfChatMessage"); Open shows the channel. Runs only for an account that has
     *  used Wolf Chat (WolfChatUsed) and only on Wolf Territories. Starts from "now" each login, so old messages
     *  never pop up. */
    class WolfChatNotifier : public LLEventTimer
    {
    public:
        WolfChatNotifier() : LLEventTimer(NOTIFY_POLL) {}
        bool tick() override
        {
            if (mBusy || LLStartUp::getStartupState() < STATE_STARTED) return false;
            if (!gSavedPerAccountSettings.getBOOL("WolfChatUsed") || !WolfGrid::isOnWolfTerritoriesConfirmed()) return false;
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now < mNextTry) return false;
            mBusy = true;
            LLSD b;
            b["since"] = mSince;
            WolfChat::api("notify", b, [this](bool ok, const LLSD& reply, const std::string& error)
            {
                mBusy = false;
                if (!ok)
                {
                    // Quietly slower (a toast every 30 s about a background check would be worse than none); the
                    // window itself shows any failure when it is opened.
                    mBackoff = mBackoff <= 0.0 ? (F64)NOTIFY_POLL : std::min(mBackoff * 2.0, NOTIFY_BACKOFF_MAX);
                    mNextTry = LLFrameTimer::getElapsedSeconds() + mBackoff;
                    LL_INFOS("WolfChat") << "New-message check failed (next in " << mBackoff << " s): " << error << LL_ENDL;
                    return;
                }
                mBackoff = 0.0;
                mNextTry = 0.0;
                const bool first = mSince == 0;
                mSince = std::max(mSince, reply["next"].asInteger());
                if (first) return;
                show(reply["notes"]);
            });
            return false;
        }

    private:
        void show(const LLSD& notes)
        {
            // One toast per channel: the message, or how many and the newest.
            const LLUUID shown = shownChannel();
            std::vector<LLUUID> order;
            std::map<LLUUID, std::vector<LLSD> > by_channel;
            for (const LLSD& n : llsd::inArray(notes))
            {
                const LLUUID channel = n["channel"].asUUID();
                if (channel.isNull() || channel == shown) continue;
                if (!by_channel.count(channel)) order.push_back(channel);
                by_channel[channel].push_back(n);
            }
            for (const LLUUID& channel : order)
            {
                const std::vector<LLSD>& list = by_channel[channel];
                const LLSD& newest = list.back();
                LLSD args;
                args["FROM"] = newest["name"].asString().empty() ? std::string("someone") : newest["name"].asString();
                args["WHERE"] = "#" + newest["channel_name"].asString() + " (" + newest["group_name"].asString() + ")";
                args["TEXT"] = newest["text"].asString();
                args["COUNT"] = llformat("%d", (S32)list.size());
                LLSD payload;
                payload["group"] = newest["group"];
                payload["channel"] = channel;
                LLNotificationsUtil::add(list.size() == 1 ? "WolfChatMessage" : "WolfChatMessages", args, payload,
                    [](const LLSD& n, const LLSD& r)
                    {
                        if (LLNotificationsUtil::getSelectedOption(n, r) == 0)
                            WolfChat::openChannel(n["payload"]["group"].asUUID(), n["payload"]["channel"].asUUID());
                        return false;
                    });
            }
        }

        /** The channel an open, visible Wolf Chat window shows (its messages are being read already). */
        static LLUUID shownChannel()
        {
            WolfFloaterChat* f = LLFloaterReg::findTypedInstance<WolfFloaterChat>("wolf_chat");
            return f ? f->shownChannel() : LLUUID::null;
        }

        S32 mSince = 0;
        bool mBusy = false;
        F64 mBackoff = 0.0;
        F64 mNextTry = 0.0;
    };
    WolfChatNotifier* sNotifier = nullptr;

    /** "wolf channel call": the region gives a group member the credential for this room
     *  (WebRtcVoiceRegionModule HandleWolfChannelCall), or 403. The answer is the shape of a "call" answer
     *  (llvoicechannel.cpp:687 result["voice_credentials"]). */
    void requestRoomCredentials(LLUUID group, LLUUID channel, std::function<void(bool, const LLSD&, const std::string&)> done)
    {
        LLViewerRegion* region = gAgent.getRegion();
        const std::string url = region ? region->getCapability("ChatSessionRequest") : std::string();
        if (url.empty())
        {
            done(false, LLSD(), "This region has no voice service, so the room cannot be joined here.");
            return;
        }
        LLCoros::instance().launch("WolfChat room", [url, group, channel, done]()
        {
            LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
                std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfChatRoom", LLCore::HttpRequest::DEFAULT_POLICY_ID);
            LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
            LLSD body;
            body["method"] = "wolf channel call";
            body["session-id"] = channel;
            body["group-id"] = group;
            LLSD result = adapter->postAndSuspend(request, url, body);
            LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
            LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
            if (!st)
            {
                if (st == LLCore::HttpStatus(HTTP_FORBIDDEN))
                    done(false, LLSD(), "You cannot talk in this group's voice rooms: your group role does not allow voice, "
                                        "or this region does not have Wolf voice rooms yet.");
                else
                    done(false, LLSD(), "The region did not let you into the room (" + st.toString() + "). Try again in a moment.");
                return;
            }
            const LLSD creds = result["voice_credentials"];
            if (!creds.isMap() || creds["channel_uri"].asString().empty())
            {
                done(false, LLSD(), "The region answered without a room. Try again in a moment.");
                return;
            }
            done(true, creds, std::string());
        });
    }
}

// =====================================================================================================================
// the service
// =====================================================================================================================

void WolfChat::api(const std::string& action, const LLSD& body, api_callback_t done)
{
    apiTo(API_URL, action, body, done);
}

void WolfChat::bridgeApi(const std::string& action, const LLSD& body, api_callback_t done)
{
    apiTo(BRIDGE_URL, action, body, done);
}

// static
void WolfChat::apiTo(const char* url, const std::string& action, const LLSD& body, api_callback_t done)
{
    // Wolf Territories only: the request carries the session pair, so it goes only while the region underfoot has
    // said it is Wolf (the WolfAdventure::api gate, wolfadventure.cpp).
    if (!WolfGrid::isOnWolfTerritories())
    {
        if (done) done(false, LLSD(), "Wolf Chat works on Wolf Territories only.");
        return;
    }
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        if (done) done(false, LLSD(), "Moving between regions - Wolf Chat tries again in a moment.");
        return;
    }
    LLSD req = body.isMap() ? body : LLSD::emptyMap();
    req["action"] = action;
    const std::string text = boost::json::serialize(LlsdToJson(req));
    const std::string target = url;
    LLCoros::instance().launch("WolfChat " + action, [text, done, target]()
    {
        // Source: wolfadventure.cpp WolfAdventure::api - the session pair on a host-verified, non-redirecting POST,
        // never retried.
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfChat", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        options->setRetries(0);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(text.data(), text.size());
        LLSD result = adapter->postRawAndSuspend(request, target, raw, options, headers);
        LLSD http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus st = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        LLSD reply;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(std::string(bytes.begin(), bytes.end()), ec);
            if (!ec) reply = LlsdFromJson(v);
        }
        // A refusal's JSON arrives in the error body (llcorehttputil.cpp httpStatus["error_body"], as WolfAdventure::api).
        if (!reply.isMap() && http.has("error_body"))
        {
            boost::system::error_code eec;
            boost::json::value ev = boost::json::parse(http["error_body"].asString(), eec);
            if (!eec && ev.is_object()) reply = LlsdFromJson(ev);
        }
        const S32 status = st.isHttpStatus() ? (S32)st.getType() : (st ? 200 : 0);
        std::string error;
        bool ok = false;
        if (status == 200 && reply.isMap())
        {
            ok = reply["success"].asBoolean();
            if (!ok) error = reply["error"].asString();
            if (!ok && error.empty()) error = "Wolf Chat refused it without saying why.";
        }
        else if (reply.isMap() && reply.has("error"))
        {
            error = reply["error"].asString();
        }
        else
        {
            error = statusWords(status, st ? std::string() : st.toString());
        }
        if (!ok && !error.empty())
        {
            // The service writes lower-case sentence pieces for scripts and windows alike; start it like a sentence.
            error[0] = (char)toupper((unsigned char)error[0]);
            if (error.back() != '.' && error.back() != '!' && error.back() != '?') error += ".";
        }
        if (done) done(ok, reply, error);
    });
}

void WolfChat::openChannel(const LLUUID& group, const LLUUID& channel)
{
    LLSD key;
    key["group"] = group;
    key["channel"] = channel;
    LLFloaterReg::showInstance("wolf_chat", key, true);
}

void WolfChat::openGroupChat(const LLUUID& group)
{
    // The key asks the window to add the group to the rail (php/wolfchat.php server_add) and to show #group-chat,
    // whose channel id is the group id (the group IM session id, llimview.cpp:2595-2598 computeSessionID).
    LLSD key;
    key["group"] = group;
    key["channel"] = group;
    key["add"] = true;
    LLFloaterReg::showInstance("wolf_chat", key, true);
}

bool WolfChat::isWolfGroup(const std::string& group_name)
{
    // Source: OpenSim HGGroupsService.cs:63-87 CreateGroupProxy - a group from another grid is kept (and so named to the
    // viewer) as name + " @ " + new Uri(serviceLocation).Authority, i.e. "host:port" of that grid's groups service.
    const size_t at = group_name.rfind(" @ ");
    if (at != std::string::npos)
    {
        const std::string authority = group_name.substr(at + 3);
        if (!authority.empty() && authority.find(' ') == std::string::npos) return WolfGrid::isWolfHost(authority);
    }
    // No "@ host": a group of the login grid (wolfgrid.h:68 isWolfTerritories = logged in to Wolf).
    return WolfGrid::isWolfTerritories();
}

void WolfChat::noteGroupChatElsewhere(const LLUUID& group, const std::string& group_name)
{
    static std::set<LLUUID> told;
    if (!told.insert(group).second) return;
    // Why Wolf Chat cannot be used off Wolf Territories - neither case is a choice the viewer can get round:
    //  - a Wolf account that hypergrids out is logged OUT of Wolf's presence service as it leaves
    //    (HGEntityTransferModule.cs:258-293 CreateAgent "logout = success" at :293, then :246-253 AgentHasMovedAway ->
    //    PresenceService.LogoutAgent), and php/wolfchat.php signs people in by that presence session
    //    (grid_rights.php wt_authenticate -> wt_require_presence);
    //  - an account of another grid has no Wolf Territories session at all.
    LLSD args;
    args["GROUP"] = group_name;
    args["WHY"] = WolfGrid::isWolfTerritories()
        ? std::string("Wolf Chat signs you in with your Wolf Territories session, and visiting another grid ends it until you come back")
        : std::string("Wolf Chat is for Wolf Territories accounts on Wolf Territories");
    LLNotificationsUtil::add("WolfChatGroupChatElsewhere", args);
}

bool WolfChat::inRoom()
{
    return sRoom && LLVoiceChannel::getCurrentVoiceChannel() == sRoom && sRoom->callStarted();
}

LLUUID WolfChat::roomChannel() { return inRoom() ? sRoomChannel : LLUUID::null; }
LLUUID WolfChat::roomGroup() { return inRoom() ? sRoomGroup : LLUUID::null; }
std::string WolfChat::roomName() { return inRoom() ? sRoomName : std::string(); }

void WolfChat::joinRoom(const LLUUID& group, const LLUUID& channel, const std::string& name,
                        std::function<void(bool ok, const std::string& error)> done)
{
    if (!WolfGrid::isOnWolfTerritoriesConfirmed())
    {
        done(false, "Voice rooms work on Wolf Territories only (or you are moving between regions - try again in a moment).");
        return;
    }
    if (!LLVoiceClient::getInstance()->voiceEnabled())
    {
        done(false, "Voice is turned off in your preferences (Sound & Media > Voice). Turn it on to join a room.");
        return;
    }
    requestRoomCredentials(group, channel, [group, channel, name, done](bool ok, const LLSD& creds, const std::string& error)
    {
        if (!ok)
        {
            done(false, error);
            return;
        }
        leaveRoom();
        WolfChatVoiceChannel*& room = sRooms[channel];
        if (!room) room = new WolfChatVoiceChannel(channel, group, name);
        sRoom = room;
        sRoomGroup = group;
        sRoomChannel = channel;
        sRoomName = name;
        // Fresh credentials: back to NO_CHANNEL_INFO (resetChannelInfo), then setChannelInfo (-> READY,
        // llvoicechannel.cpp LLVoiceChannelGroup::setChannelInfo), so activate() starts the call at once instead of
        // asking again (LLVoiceChannel::activate).
        sRoom->resetChannelInfo();
        sRoom->setChannelInfo(creds);
        sRoom->activate();
        if (!sHeartbeat) sHeartbeat = new WolfChatHeartbeat();
        LLSD b;
        b["channel"] = channel;
        b["muted"] = micOn() ? 0 : 1;
        b["deafened"] = deafened() ? 1 : 0;
        api("voice_here", b, nullptr);
        sRoomTold = true;
        sWantRoom = true;               // [2026-10-21] watched from now on (roomWatch)
        sNotConnectedSince = 0.0;
        sRoomNote.clear();
        done(true, std::string());
    });
}

bool WolfChat::roomConnected()
{
    return inRoom() && sRoom->getState() == LLVoiceChannel::STATE_CONNECTED;
}

std::string WolfChat::roomNote()
{
    return sRoomNote;
}

void WolfChat::leaveRoom()
{
    sWantRoom = false;      // [2026-10-21] left on purpose (or by roomWatch): no automatic rejoin
    if (sRoom && LLVoiceChannel::getCurrentVoiceChannel() == sRoom)
    {
        sRoom->deactivate();    // back to the voice of the place you stand (LLVoiceChannel::deactivate)
    }
    if (sRoomTold)
    {
        api("voice_leave", LLSD::emptyMap(), nullptr);
        sRoomTold = false;
    }
}

// [2026-10-21] -------------------------------------------------------------------------------------------------------

WolfChat::GifSheet WolfChat::GifSheet::fromLLSD(const LLSD& gif)
{
    // Source: php/wolfchat.php wch_gif_out - the names of the "gif" object.
    GifSheet g;
    if (!gif.isMap()) return g;
    g.mId = gif["id"].asString();
    g.mAsset = gif["asset"].asUUID();
    g.mTitle = gif["title"].asString();
    g.mFrames = gif["frames"].asInteger();
    g.mCols = gif["cols"].asInteger();
    g.mRows = gif["rows"].asInteger();
    g.mCellW = gif["cw"].asInteger();
    g.mCellH = gif["ch"].asInteger();
    g.mSheetW = gif["sw"].asInteger();
    g.mSheetH = gif["sh"].asInteger();
    g.mDelayMs = gif["delay"].asInteger();
    return g;
}

bool WolfChat::GifSheet::valid() const
{
    // The bounds php/wolfchat.php keeps (WCH_GIF_SHEET_MAX 1024); anything else is not drawn.
    return mAsset.notNull() && mFrames >= 1 && mCols >= 1 && mRows >= 1 && mFrames <= mCols * mRows
        && mCellW >= 1 && mCellH >= 1 && mSheetW >= mCols * mCellW && mSheetH >= mRows * mCellH
        && mSheetW <= 1024 && mSheetH <= 1024 && mDelayMs >= 20;
}

LLRectf WolfChat::GifSheet::frameUV(S32 frame) const
{
    const S32 f = llclamp(frame, 0, mFrames - 1);
    const S32 col = f % mCols;
    const S32 row = f / mCols;
    const F32 sw = (F32)mSheetW;
    const F32 sh = (F32)mSheetH;
    // Half a texel in from each side, so a neighbouring frame never bleeds into the edge.
    const F32 left = ((F32)(col * mCellW) + 0.5f) / sw;
    const F32 right = ((F32)((col + 1) * mCellW) - 0.5f) / sw;
    const F32 top = 1.f - ((F32)(row * mCellH) + 0.5f) / sh;
    const F32 bottom = 1.f - ((F32)((row + 1) * mCellH) - 0.5f) / sh;
    return LLRectf(left, top, right, bottom);
}

S32 WolfChat::GifSheet::frameNow() const
{
    if (mFrames <= 1 || mDelayMs <= 0) return 0;
    const U64 ms = LLFrameTimer::getTotalTime() / 1000;     // microseconds since the epoch
    return (S32)((ms / (U64)mDelayMs) % (U64)mFrames);
}

void WolfChat::markUsed()
{
    gSavedPerAccountSettings.setBOOL("WolfChatUsed", true);
}

void WolfChat::startNotifier()
{
    if (!sNotifier) sNotifier = new WolfChatNotifier();
}

void WolfChat::openLink(const std::string& url)
{
    // php/wolfchat.php wch_card_fields lets only https, http and secondlife:// links in; checked again here.
    std::string lower = url;
    LLStringUtil::toLower(lower);
    if (lower.rfind("secondlife:", 0) == 0)
        LLUrlAction::clickAction(url, false);   // as a link clicked in chat: untrusted, so only click-allowed handlers
    else if (lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0)
        LLWeb::loadURLInternal(url);
}

bool WolfChat::micOn()
{
    return LLVoiceClient::getInstance()->getUserPTTState();
}

void WolfChat::setMic(bool on)
{
    // The viewer's own Speak toggle (LLVoiceClient::setUserPTTState), so the toolbar's Speak button agrees.
    LLVoiceClient::getInstance()->setUserPTTState(on);
}

bool WolfChat::deafened()
{
    // The viewer's own voice mute (llvieweraudio.cpp audio_update_volume: MuteVoice -> voice volume and mic gain 0),
    // the same switch as the volume panel's.
    return gSavedSettings.getBOOL("MuteVoice");
}

void WolfChat::setDeafened(bool deaf)
{
    gSavedSettings.setBOOL("MuteVoice", deaf);
}

// =====================================================================================================================
// the room as a voice channel
// =====================================================================================================================

WolfChatVoiceChannel::WolfChatVoiceChannel(const LLUUID& channel, const LLUUID& group, const std::string& name)
:   LLVoiceChannelGroup(channel, name, false),
    mGroup(group)
{
}

void WolfChatVoiceChannel::requestChannelInfo()
{
    // Asked again after a lost connection (LLVoiceChannelGroup::handleError ERROR_NOT_AVAILABLE retries): the same
    // "wolf channel call" the join made, never the stock "call" (which the region answers for groups and conferences).
    const LLUUID channel = getSessionID();
    requestRoomCredentials(mGroup, channel, [channel](bool ok, const LLSD& creds, const std::string& error)
    {
        LLVoiceChannel* vc = LLVoiceChannel::getChannelByID(channel);
        if (!vc) return;
        if (!ok)
        {
            LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", "Wolf Chat voice room: " + error));
            vc->deactivate();
            return;
        }
        vc->setChannelInfo(creds);
    });
}

// =====================================================================================================================
// the sidebar
// =====================================================================================================================

WolfChatSidebar::WolfChatSidebar(const Params& p)
:   LLUICtrl(p)
{
}

void WolfChatSidebar::setChannels(const std::vector<Channel>& channels, bool can_manage)
{
    mChannels = channels;
    mCanManage = can_manage;
    mLoading = false;
    layout();
}

void WolfChatSidebar::layout()
{
    mRows.clear();
    S32 y = 0;
    for (S32 pass = 0; pass < 2; ++pass)
    {
        const bool voice = pass == 1;
        Row head;
        head.mHeader = true;
        head.mVoiceHeader = voice;
        head.mHit = mCanManage ? (voice ? HIT_ADD_VOICE : HIT_ADD_TEXT) : HIT_NONE;
        head.mTop = y;
        head.mHeight = HEAD_H;
        mRows.push_back(head);
        y += HEAD_H;
        for (S32 i = 0; i < (S32)mChannels.size(); ++i)
        {
            const Channel& c = mChannels[i];
            if (c.mVoice != voice) continue;
            Row r;
            r.mHit = HIT_CHANNEL;
            r.mId = c.mId;
            r.mChannel = i;
            r.mTop = y;
            r.mHeight = ROW_H;
            mRows.push_back(r);
            y += ROW_H;
            if (!voice) continue;
            for (S32 k = 0; k < (S32)c.mPeople.size(); ++k)
            {
                Row pr;
                pr.mHit = HIT_PERSON;
                pr.mId = c.mPeople[k].mId;
                pr.mChannel = i;
                pr.mPerson = k;
                pr.mTop = y;
                pr.mHeight = PERSON_H;
                mRows.push_back(pr);
                y += PERSON_H;
            }
        }
        y += 6;
    }
}

const WolfChatSidebar::Row* WolfChatSidebar::rowAt(S32 x, S32 y) const
{
    const S32 content_y = (getRect().getHeight() - y) + mScroll;
    for (const Row& r : mRows)
    {
        if (content_y >= r.mTop && content_y < r.mTop + r.mHeight) return &r;
    }
    return nullptr;
}

void WolfChatSidebar::draw()
{
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    gl_rect_2d(0, h, w, 0, C_BG, true);
    LLLocalClipRect clip(LLRect(0, h, w, 0));

    if (mLoading)
    {
        // Skeleton rows the shape of the real ones while the channels load.
        text(fontSmall(), "TEXT CHANNELS", (F32)PAD, (F32)(h - HEAD_H / 2), C_DIM);
        for (S32 i = 0; i < 3; ++i)
        {
            const S32 t = h - HEAD_H - i * ROW_H;
            gl_rect_2d(PAD + 4, t - 8, PAD + 120 - i * 20, t - 16, C_SKELETON, true);
        }
        return;
    }

    for (S32 i = 0; i < (S32)mRows.size(); ++i)
    {
        const Row& r = mRows[i];
        const S32 top = h - r.mTop + mScroll;
        const S32 bottom = top - r.mHeight;
        if (bottom > h || top < 0) continue;
        const F32 cy = (F32)(top - r.mHeight / 2);
        if (r.mHeader)
        {
            text(fontSmall(), r.mVoiceHeader ? "VOICE CHANNELS" : "TEXT CHANNELS", (F32)PAD, cy, C_DIM);
            if (mCanManage)
            {
                const bool hover = mHover == i;
                text(fontBold(), "+", (F32)(w - PAD - 4), cy, hover ? C_ACCENT : C_DIM, LLFontGL::HCENTER);
            }
            continue;
        }
        if (r.mHit == HIT_CHANNEL)
        {
            const Channel& c = mChannels[r.mChannel];
            const bool selected = (!c.mVoice && c.mId == mSelected) || (c.mVoice && c.mId == mJoined);
            if (selected) gl_rect_2d(PAD / 2, top, w - PAD / 2, bottom, C_ROW_SELECTED, true);
            else if (mHover == i) gl_rect_2d(PAD / 2, top, w - PAD / 2, bottom, C_ROW_HOVER, true);
            if (c.mVoice)
            {
                icon("Command_Voice_Icon", PAD + 2, bottom + (ROW_H - ICON) / 2, ICON, selected ? C_ACCENT : C_DIM);
                if (c.mRestricted) icon("Lock", w - PAD - ICON - 2, bottom + (ROW_H - ICON) / 2, ICON, C_DIM);
                text(selected ? fontBold() : font(), c.mName, (F32)(PAD + ICON + 8), cy, selected ? C_ACCENT : C_TEXT,
                     LLFontGL::LEFT, w - PAD * 3 - ICON);
            }
            else
            {
                text(fontBold(), "#", (F32)(PAD + 4), cy, C_DIM);
                const bool bold = selected || c.mUnread;
                // [2026-10-21] Badges at the right: "@n" (mentions of you) and the unread count, or the dot when the
                // count is not known (only the window's own tally); a muted channel says "muted". Each is words or a
                // number, never colour alone; the name is bold while unread as before.
                S32 right = w - PAD - 2;
                if (c.mRestricted)
                {
                    // [2026-10-21] Owners: only some roles can see it (the tooltip says so too).
                    icon("Lock", right - ICON, (S32)cy - ICON / 2, ICON, C_DIM);
                    right -= ICON + 4;
                }
                if (c.mBridged)
                {
                    // [2026-10-21] Bridged to Discord (everyone sees it); a stopped bridge in red with a "!" (not colour alone).
                    icon("Command_Discord_Icon", right - ICON, (S32)cy - ICON / 2, ICON, c.mBridgeBroken ? C_ERROR : C_DIM);
                    right -= ICON + 2;
                    if (c.mBridgeBroken)
                    {
                        text(fontBold(), "!", (F32)(right - 3), cy, C_ERROR, LLFontGL::HCENTER);
                        right -= 8;
                    }
                    right -= 2;
                }
                if (c.mMentions > 0) right = badge("@" + countText(c.mMentions), right, cy, C_BADGE_MENTION) - 4;
                if (c.mUnreadCount > 0 && !c.mMuted) right = badge(countText(c.mUnreadCount), right, cy, C_BADGE_UNREAD) - 4;
                else if (c.mUnread && c.mUnreadCount == 0 && !c.mMuted)
                {
                    gGL.color4fv(C_TEXT.mV);
                    gl_circle_2d((F32)(right - 4), cy, 4.f, 16, true);
                    right -= 12;
                }
                if (c.mMuted)
                {
                    const S32 mw = fontSmall()->getWidth(std::string("muted"));
                    text(fontSmall(), "muted", (F32)(right - mw), cy, C_DIM);
                    right -= mw + 4;
                }
                text(bold ? fontBold() : font(), c.mName, (F32)(PAD + ICON + 8), cy, (selected || c.mUnread) ? C_TEXT : C_DIM,
                     LLFontGL::LEFT, std::max(16, right - (PAD + ICON + 8) - 4));
            }
            continue;
        }
        if (r.mHit == HIT_PERSON)
        {
            const Person& p = mChannels[r.mChannel].mPeople[r.mPerson];
            if (mHover == i) gl_rect_2d(PAD / 2, top, w - PAD / 2, bottom, C_ROW_HOVER, true);
            const S32 x0 = PAD + ICON + 8;
            // [2026-10-21] Speaking (Paul: "the indicator is not showing any activity"): Discord's ring - a filled lime
            // dot inside a ring while the person talks, a small hollow dot when silent (shape, not only colour), and a
            // level bar beside it that follows their voice power.
            const F32 dot_x = (F32)(PAD + ICON / 2 + 2);
            if (p.mSpeaking)
            {
                gGL.color4fv(C_ACCENT.mV);
                gl_circle_2d(dot_x, cy, 4.f, 16, true);
                gl_circle_2d(dot_x, cy, 7.f, 20, false);
            }
            else
            {
                gGL.color4fv(C_DIM.mV);
                gl_circle_2d(dot_x, cy, 3.f, 12, false);
            }
            if (p.mLevel > 0.f)
            {
                const S32 bar = (S32)((F32)(PERSON_H - 6) * llclamp(p.mLevel, 0.f, 1.f));
                gl_rect_2d(x0 - 5, bottom + 3 + bar, x0 - 3, bottom + 3, C_ACCENT, true);
            }
            text(font(), p.mName.empty() ? std::string("(loading name)") : p.mName, (F32)x0, cy,
                 p.mMutedByMe ? C_DIM : C_TEXT, LLFontGL::LEFT, w - x0 - PAD - 2 * ICON - 8);
            // Mic, then speaker: struck-through when off.
            const S32 mic_x = w - PAD - 2 * ICON - 4;
            const S32 spk_x = w - PAD - ICON;
            const S32 iy = bottom + (PERSON_H - ICON) / 2;
            icon(p.mMicOff ? "VoicePTT_Off" : "VoicePTT_On", mic_x, iy, ICON, p.mMicOff ? C_ERROR : C_DIM);
            icon(p.mDeafened ? "Audio_Off" : "Audio_Press", spk_x, iy, ICON, p.mDeafened ? C_ERROR : C_DIM);
            continue;
        }
    }
}

bool WolfChatSidebar::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const Row* r = rowAt(x, y);
    if (!r || !mClick) return true;
    if (r->mHeader)
    {
        // The "+" is the right-hand end of the header.
        if (mCanManage && x >= getRect().getWidth() - PAD - 18) mClick(r->mHit, LLUUID::null);
        return true;
    }
    mClick(r->mHit, r->mId);
    return true;
}

bool WolfChatSidebar::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    // [2026-10-21] Owners: the channel's menu (Rename / Who can see / Delete). Never for #group-chat.
    const Row* r = rowAt(x, y);
    if (!r || r->mHit != HIT_CHANNEL || !mCanManage || !mMenu) return true;
    if (mChannels[r->mChannel].mGroupChat) return true;
    setFocus(true);
    mMenu(r->mId, x, y);
    return true;
}

bool WolfChatSidebar::handleHover(S32 x, S32 y, MASK mask)
{
    const Row* r = rowAt(x, y);
    mHover = r ? (S32)(r - &mRows[0]) : -1;
    const bool clickable = r && (r->mHit == HIT_CHANNEL || r->mHit == HIT_PERSON
                                 || (r->mHeader && mCanManage && x >= getRect().getWidth() - PAD - 18));
    getWindow()->setCursor(clickable ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

void WolfChatSidebar::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool WolfChatSidebar::handleToolTip(S32 x, S32 y, MASK mask)
{
    const Row* r = rowAt(x, y);
    if (!r) return false;
    std::string tip;
    if (r->mHeader && mCanManage) tip = r->mVoiceHeader ? "Make a voice channel" : "Make a text channel";
    else if (r->mHit == HIT_CHANNEL)
    {
        const Channel& c = mChannels[r->mChannel];
        if (c.mVoice) tip = std::string("Join this voice room") + (c.mRestricted ? " (only some roles can see it)" : "")
                            + (mCanManage ? " - right-click to rename, delete or choose who can see it" : "");
        else if (c.mGroupChat)
        {
            tip = "This group's own group chat - the same chat everyone in the group sees in any viewer";
            if (c.mUnread) tip += " - new messages";
        }
        else
        {
            tip = "Read this channel";
            if (c.mRestricted) tip += " (only some roles can see it)";
            if (c.mBridged) tip += c.mBridgeBroken ? " - its Discord bridge has stopped" : " - bridged to Discord: visible outside the grid";
            if (mCanManage) tip += " - right-click to rename, delete, choose who can see it or bridge it to Discord";
            if (c.mUnreadCount > 0) tip += " - " + countText(c.mUnreadCount) + " unread";
            if (c.mMentions > 0) tip += ", " + countText(c.mMentions) + " mentioning you";
            if (c.mMuted) tip += " (notifications: Nothing)";
        }
    }
    else if (r->mHit == HIT_PERSON)
    {
        const Person& p = mChannels[r->mChannel].mPeople[r->mPerson];
        tip = p.mName + (p.mSpeaking ? " - talking" : "") + (p.mMicOff ? " - mic off" : " - mic on")
              + (p.mDeafened ? " - deafened" : "") + (p.mMutedByMe ? " - muted by you" : "") + ". Click for their profile.";
    }
    if (tip.empty()) return false;
    LLToolTipMgr::instance().show(tip);
    return true;
}

bool WolfChatSidebar::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    S32 content = 0;
    if (!mRows.empty()) content = mRows.back().mTop + mRows.back().mHeight + 6;
    mScroll = llclamp(mScroll + clicks * ROW_H, 0, std::max(0, content - getRect().getHeight()));
    return true;
}

// =====================================================================================================================
// a posted picture
// =====================================================================================================================

WolfChatImage::WolfChatImage(const Params& p)
:   LLUICtrl(p),
    mImageId(p.image_id),
    mImageName(p.image_name)
{
    mImage = LLUI::getUIImageByID(mImageId, LLGLTexture::BOOST_PREVIEW);
    setToolTip((mImageName.empty() ? std::string("A picture") : mImageName) + " - click to see it big");
}

WolfChatImage::~WolfChatImage()
{
    // BOOST_PREVIEW pins the texture (NO_DELETE); released the way LLTextureCtrl does (lltexturectrl.cpp:1936-1939).
    if (mSheet.notNull()) mSheet->forceActive();
}

void WolfChatImage::setGif(const WolfChat::GifSheet& gif, bool animate)
{
    if (!gif.valid()) return;
    mGif = gif;
    mAnimate = animate;
    mImage = nullptr;
    // Source: lltexturectrl.cpp:2507-2508 - fetched at BOOST_NONE, then raised to BOOST_PREVIEW.
    mSheet = LLViewerTextureManager::getFetchedTexture(gif.mAsset, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    if (mSheet.notNull()) mSheet->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
    const std::string what = gif.mTitle.empty() ? std::string("A GIF") : gif.mTitle;
    setToolTip(what + (animate ? " (GIF) - click to see it big" : " (GIF, shown still here) - click to see it play"));
}

void WolfChatImage::draw()
{
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    gl_rect_2d(0, h, w, 0, C_SKELETON, true);
    if (mGif.valid() && mSheet.notNull())
    {
        // The whole sheet at full size, so no frame is drawn from a blurred mip (lltexturectrl.cpp:2542 addTextureStats).
        mSheet->addTextureStats((F32)(mGif.mSheetW * mGif.mSheetH));
        if (!mSheet->hasGLTexture()) return;     // still arriving: the grey box is the loading state
        const F32 s = std::min((F32)w / (F32)mGif.mCellW, (F32)h / (F32)mGif.mCellH);
        const S32 dw = std::max(1, (S32)((F32)mGif.mCellW * s));
        const S32 dh = std::max(1, (S32)((F32)mGif.mCellH * s));
        const S32 frame = mAnimate ? mGif.frameNow() : 0;
        gl_draw_scaled_image((w - dw) / 2, (h - dh) / 2, dw, dh, mSheet, UI_VERTEX_COLOR, mGif.frameUV(frame));
        // "GIF" in the corner: says what it is without relying on the movement.
        const S32 lw = fontSmall()->getWidth(std::string("GIF")) + 8;
        gl_rect_2d((w - dw) / 2, (h + dh) / 2, (w - dw) / 2 + lw, (h + dh) / 2 - 16, LLColor4(0.f, 0.f, 0.f, 0.7f), true);
        text(fontSmall(), "GIF", (F32)((w - dw) / 2 + lw / 2), (F32)((h + dh) / 2 - 8), LLColor4::white, LLFontGL::HCENTER);
        return;
    }
    if (mImage.isNull()) return;
    // Fit, keeping the picture's shape, once its size is known.
    S32 iw = 0, ih = 0;
    if (LLGLTexture* gl = dynamic_cast<LLGLTexture*>(mImage->getImage().get()))
    {
        iw = gl->getFullWidth();
        ih = gl->getFullHeight();
    }
    S32 dw = w, dh = h;
    if (iw > 0 && ih > 0)
    {
        const F32 s = std::min((F32)w / (F32)iw, (F32)h / (F32)ih);
        dw = std::max(1, (S32)((F32)iw * s));
        dh = std::max(1, (S32)((F32)ih * s));
    }
    mImage->draw((w - dw) / 2, (h - dh) / 2, dw, dh);
}

bool WolfChatImage::handleMouseDown(S32 x, S32 y, MASK mask)
{
    if (mGif.valid())
    {
        // [2026-10-21] The big window plays the GIF; its key carries the sheet (php/wolfchat.php wch_gif_out names).
        LLSD key;
        key["asset"] = mGif.mAsset;
        key["id"] = mGif.mId;
        key["title"] = mGif.mTitle;
        key["frames"] = mGif.mFrames;
        key["cols"] = mGif.mCols;
        key["rows"] = mGif.mRows;
        key["cw"] = mGif.mCellW;
        key["ch"] = mGif.mCellH;
        key["sw"] = mGif.mSheetW;
        key["sh"] = mGif.mSheetH;
        key["delay"] = mGif.mDelayMs;
        LLFloaterReg::showInstance("wolf_chat_image", key, true);
        return true;
    }
    LLFloaterReg::showInstance("wolf_chat_image", LLSD(mImageId), true);
    return true;
}

bool WolfChatImage::handleHover(S32 x, S32 y, MASK mask)
{
    getWindow()->setCursor(UI_CURSOR_HAND);
    return true;
}

// =====================================================================================================================
// [2026-10-21] the server rail (Paul: "groups = servers")
// =====================================================================================================================

namespace
{
    const S32 RAIL_TILE = 44;
    const S32 RAIL_GAP = 8;
    const S32 RAIL_TOP = 6;

    /** Tiles for groups without an insignia: white initials on each of these is above 4.5:1 (relative luminance
     *  0.10 - 0.14 against white's 1.0). The colour follows the group id, so a group keeps its colour. */
    const LLColor4 RAIL_COLOURS[] = {
        LLColor4(0.20f, 0.36f, 0.62f, 1.f), LLColor4(0.55f, 0.22f, 0.45f, 1.f), LLColor4(0.18f, 0.45f, 0.30f, 1.f),
        LLColor4(0.60f, 0.30f, 0.12f, 1.f), LLColor4(0.35f, 0.30f, 0.60f, 1.f), LLColor4(0.15f, 0.45f, 0.50f, 1.f) };

    /** Up to two initials, upper case ("Wolf's Den" -> "WD"). */
    std::string initials(const std::string& name)
    {
        LLWString w = utf8str_to_wstring(name);
        LLWString out;
        bool start = true;
        for (llwchar c : w)
        {
            const bool letter = LLStringOps::isAlnum(c);
            if (letter && start && out.size() < 2) out += c;
            start = !letter && c != '\'';
        }
        LLWStringUtil::toUpper(out);
        return out.empty() ? std::string("?") : wstring_to_utf8str(out);
    }
}

WolfChatServerRail::WolfChatServerRail(const Params& p)
:   LLUICtrl(p)
{
}

void WolfChatServerRail::setServers(const std::vector<Server>& servers)
{
    mServers = servers;
    mIcons.clear();
    for (const Server& sv : mServers)
    {
        mIcons.push_back(sv.mInsignia.notNull() ? LLUI::getUIImageByID(sv.mInsignia, LLGLTexture::BOOST_PREVIEW) : LLUIImagePtr());
    }
    mFocus = llclamp(mFocus, 0, (S32)mServers.size());
    mHover = -1;
    mLoading = false;
}

LLRect WolfChatServerRail::tileRect(S32 i) const
{
    const S32 left = (getRect().getWidth() - RAIL_TILE) / 2;
    const S32 top = getRect().getHeight() - RAIL_TOP - i * (RAIL_TILE + RAIL_GAP) + mScroll;
    return LLRect(left, top, left + RAIL_TILE, top - RAIL_TILE);
}

S32 WolfChatServerRail::tileAt(S32 x, S32 y) const
{
    for (S32 i = 0; i <= (S32)mServers.size(); ++i)
    {
        LLRect r = tileRect(i);
        r.mLeft = 0;                        // the whole row answers, not just the tile
        r.mRight = getRect().getWidth();
        if (r.pointInRect(x, y)) return i;
    }
    return -1;
}

void WolfChatServerRail::draw()
{
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    gl_rect_2d(0, h, w, 0, C_BG, true);
    LLLocalClipRect clip(LLRect(0, h, w, 0));
    LLUIImagePtr rounded = LLUI::getUIImage("Rounded_Square");      // textures.xml:770
    if (mLoading)
    {
        // Skeleton tiles while the rail loads.
        for (S32 i = 0; i < 3; ++i)
        {
            const LLRect r = tileRect(i);
            if (rounded) rounded->draw(r.mLeft, r.mBottom, RAIL_TILE, RAIL_TILE, C_SKELETON);
        }
        return;
    }
    for (S32 i = 0; i <= (S32)mServers.size(); ++i)
    {
        const LLRect r = tileRect(i);
        if (r.mTop < 0 || r.mBottom > h) continue;
        const bool plus = i == (S32)mServers.size();
        const bool selected = !plus && mServers[i].mId == mSelected;
        if (plus)
        {
            if (rounded) rounded->draw(r.mLeft, r.mBottom, RAIL_TILE, RAIL_TILE, mHover == i ? C_ROW_SELECTED : C_CARD);
            text(fontBold(), "+", (F32)r.getCenterX(), (F32)r.getCenterY(), C_ACCENT, LLFontGL::HCENTER);
        }
        else
        {
            const Server& sv = mServers[i];
            // Left edge marker, Discord's: a tall bar for the open group, a short one for unread (shape, not colour).
            if (selected) gl_rect_2d(0, r.getCenterY() + 16, 3, r.getCenterY() - 16, C_TEXT, true);
            else if (sv.mUnread > 0) gl_rect_2d(0, r.getCenterY() + 4, 3, r.getCenterY() - 4, C_TEXT, true);
            if (mIcons[i].notNull())
            {
                mIcons[i]->draw(r.mLeft, r.mBottom, RAIL_TILE, RAIL_TILE);
            }
            else
            {
                const LLColor4& c = RAIL_COLOURS[sv.mId.getCRC32() % (sizeof(RAIL_COLOURS) / sizeof(RAIL_COLOURS[0]))];
                if (rounded) rounded->draw(r.mLeft, r.mBottom, RAIL_TILE, RAIL_TILE, c);
                text(fontBold(), initials(sv.mName), (F32)r.getCenterX(), (F32)r.getCenterY(), LLColor4::white, LLFontGL::HCENTER);
            }
            if (selected || mHover == i) gl_rect_2d(r.mLeft - 1, r.mTop + 1, r.mRight + 1, r.mBottom - 1, selected ? C_TEXT : C_DIM, false);
            if (sv.mMentions > 0)
            {
                // The red @ count at the bottom right of the tile.
                badge("@" + countText(sv.mMentions), r.mRight + 4, (F32)(r.mBottom + 6), C_BADGE_MENTION);
            }
        }
        if (hasFocus() && mFocus == i)
        {
            gl_rect_2d(r.mLeft - 3, r.mTop + 3, r.mRight + 3, r.mBottom - 3, C_FOCUS, false);
            gl_rect_2d(r.mLeft - 2, r.mTop + 2, r.mRight + 2, r.mBottom - 2, C_FOCUS, false);
        }
    }
}

void WolfChatServerRail::activate(S32 i)
{
    if (i == (S32)mServers.size()) { if (mAdd) mAdd(); }
    else if (i >= 0 && i < (S32)mServers.size() && mSelect) mSelect(LLUUID(mServers[i].mId));
}

bool WolfChatServerRail::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const S32 i = tileAt(x, y);
    if (i < 0) return true;
    mFocus = i;
    activate(i);
    return true;
}

bool WolfChatServerRail::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 i = tileAt(x, y);
    if (i < 0 || i >= (S32)mServers.size()) return true;
    setFocus(true);
    mFocus = i;
    if (mMenu) mMenu(LLUUID(mServers[i].mId), x, y);
    return true;
}

bool WolfChatServerRail::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = tileAt(x, y);
    getWindow()->setCursor(mHover >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

void WolfChatServerRail::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool WolfChatServerRail::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 i = tileAt(x, y);
    if (i < 0) return false;
    if (i == (S32)mServers.size())
    {
        LLToolTipMgr::instance().show("Add a group - choose which of your groups to chat in here");
        return true;
    }
    const Server& sv = mServers[i];
    std::string tip = sv.mName;
    if (sv.mUnread > 0) tip += " - " + countText(sv.mUnread) + " unread";
    if (sv.mMentions > 0) tip += std::string(sv.mUnread > 0 ? ", " : " - ") + countText(sv.mMentions) + " mentioning you";
    tip += "\nRight-click (or Alt+Up / Alt+Down / Delete) to move it or remove it from Wolf Chat.";
    LLToolTipMgr::instance().show(tip);
    return true;
}

bool WolfChatServerRail::handleKeyHere(KEY key, MASK mask)
{
    const S32 n = (S32)mServers.size();
    if (mask == MASK_ALT && (key == KEY_UP || key == KEY_DOWN))
    {
        if (mFocus < n && mMove)
        {
            const LLUUID id = mServers[mFocus].mId;
            const bool up = key == KEY_UP;
            mFocus = llclamp(mFocus + (up ? -1 : 1), 0, n - 1);     // the focus moves with the group
            mMove(id, up);
        }
        return true;
    }
    if (mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_UP:   mFocus = std::max(0, mFocus - 1); return true;
    case KEY_DOWN: mFocus = std::min(n, mFocus + 1); return true;
    case KEY_RETURN: activate(mFocus); return true;
    case KEY_DELETE:
        if (mFocus < n && mRemove) mRemove(LLUUID(mServers[mFocus].mId));
        return true;
    default:
        return false;
    }
}

bool WolfChatServerRail::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    const S32 content = RAIL_TOP + ((S32)mServers.size() + 1) * (RAIL_TILE + RAIL_GAP);
    mScroll = llclamp(mScroll + clicks * (RAIL_TILE + RAIL_GAP), 0, std::max(0, content - getRect().getHeight()));
    return true;
}

// =====================================================================================================================
// [2026-10-21] Add groups to Wolf Chat
// =====================================================================================================================

WolfFloaterChatAddGroup::WolfFloaterChatAddGroup(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterChatAddGroup::postBuild()
{
    mList = getChild<LLScrollListCtrl>("group_list");
    mStatus = getChild<LLTextBox>("add_status");
    mAddBtn = getChild<LLButton>("add_btn");
    mAddBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { add(); });
    getChild<LLButton>("cancel_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    mList->setDoubleClickCallback([this]() { add(); });
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { mAddBtn->setEnabled(!mBusy && mList->getFirstSelected() != nullptr); });
    return true;
}

void WolfFloaterChatAddGroup::onOpen(const LLSD& key)
{
    load();
}

void WolfFloaterChatAddGroup::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    mStatus->setColor(error ? C_ERROR : C_DIM);
}

void WolfFloaterChatAddGroup::load()
{
    mList->deleteAllItems();
    mList->setCommentText(std::string("Loading your groups..."));
    mAddBtn->setEnabled(false);
    setStatus(std::string(), false);
    LLHandle<LLFloater> h = getHandle();
    WolfChat::api("groups", LLSD::emptyMap(), [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChatAddGroup* self = static_cast<WolfFloaterChatAddGroup*>(h.get());
        if (!self) return;
        if (!ok)
        {
            self->mList->setCommentText(std::string("Your groups could not be loaded."));
            self->setStatus(error + " Close this and press + again to retry.", true);
            return;
        }
        S32 all = 0, shown = 0;
        for (const LLSD& g : llsd::inArray(reply["groups"]))
        {
            ++all;
            if (g["added"].asInteger() != 0) continue;
            // [2026-10-21] Paul's screenshot: rows were there (selectable) but no names showed. Built as the
            // members list builds its rows (a named cell in the "name" column, SansSerif, C_TEXT) instead of
            // addSimpleElement (llscrolllistctrl.cpp:3729-3745: unnamed cell, emoji font).
            LLScrollListItem::Params rp;
            rp.value(LLSD(g["id"].asUUID()));
            LLScrollListCell::Params cp;
            cp.column("name");
            cp.value(g["name"].asString());
            cp.color(C_TEXT);
            cp.font(font());
            rp.columns.add(cp);
            self->mList->addRow(rp);
            ++shown;
        }
        if (all == 0)
            self->mList->setCommentText(std::string("You are not in any group yet. Join or make one (Communicate > Groups), then add it here."));
        else if (shown == 0)
            self->mList->setCommentText(std::string("All your groups are in Wolf Chat already."));
        else
            self->mList->selectFirstItem();
        self->mAddBtn->setEnabled(shown > 0);
    });
}

void WolfFloaterChatAddGroup::add()
{
    if (mBusy) return;
    std::vector<LLScrollListItem*> picked = mList->getAllSelected();
    if (picked.empty())
    {
        setStatus("Choose at least one group first.", true);
        return;
    }
    LLSD groups = LLSD::emptyArray();
    for (LLScrollListItem* it : picked) groups.append(it->getValue());
    mBusy = true;
    mAddBtn->setEnabled(false);
    setStatus(llformat("Adding %d %s...", (S32)picked.size(), picked.size() == 1 ? "group" : "groups"), false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["groups"] = groups;
    WolfChat::api("server_add", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChatAddGroup* self = static_cast<WolfFloaterChatAddGroup*>(h.get());
        if (!self) return;
        self->mBusy = false;
        self->mAddBtn->setEnabled(true);
        if (!ok)
        {
            self->setStatus("Nothing was added: " + error, true);
            return;
        }
        if (self->mAdded) self->mAdded(reply);
        self->closeFloater();
    });
}

// =====================================================================================================================
// [2026-10-21] the grid's cards above the message box
// =====================================================================================================================

namespace
{
    const S32 CARD_GAP = 8;
    const S32 CARD_ICON = 28;
    const S32 CARD_CLOSE = 18;
}

WolfChatCards::WolfChatCards(const Params& p)
:   LLUICtrl(p)
{
}

void WolfChatCards::setCards(const std::vector<Card>& cards)
{
    mCards = cards;
    mIcons.clear();
    for (const Card& c : mCards)
    {
        mIcons.push_back(c.mIcon.notNull() ? LLUI::getUIImageByID(c.mIcon, LLGLTexture::BOOST_PREVIEW) : LLUIImagePtr());
    }
    mFocus = llclamp(mFocus, 0, std::max(0, (S32)mCards.size() - 1));
    mHover = -1;
}

LLRect WolfChatCards::cardRect(S32 i) const
{
    const S32 n = std::max(1, (S32)mCards.size());
    const S32 w = (getRect().getWidth() - CARD_GAP * (n - 1)) / n;
    const S32 left = i * (w + CARD_GAP);
    return LLRect(left, getRect().getHeight(), left + w, 0);
}

LLRect WolfChatCards::closeRect(S32 i) const
{
    const LLRect r = cardRect(i);
    return LLRect(r.mRight - CARD_CLOSE - 4, r.mTop - 4, r.mRight - 4, r.mTop - 4 - CARD_CLOSE);
}

S32 WolfChatCards::cardAt(S32 x, S32 y) const
{
    for (S32 i = 0; i < (S32)mCards.size(); ++i)
    {
        if (cardRect(i).pointInRect(x, y)) return i;
    }
    return -1;
}

void WolfChatCards::draw()
{
    for (S32 i = 0; i < (S32)mCards.size(); ++i)
    {
        const Card& c = mCards[i];
        const LLRect r = cardRect(i);
        gl_rect_2d(r, mHover == i ? C_ROW_SELECTED : C_CARD, true);
        if (hasFocus() && mFocus == i) gl_rect_2d(r.mLeft, r.mTop, r.mRight, r.mBottom, C_FOCUS, false);
        // The icon: the card's texture, or the Wolf lime square with the card's first letter.
        const S32 ix = r.mLeft + 8;
        const S32 iy = r.mBottom + (r.getHeight() - CARD_ICON) / 2;
        if (mIcons[i].notNull()) mIcons[i]->draw(ix, iy, CARD_ICON, CARD_ICON);
        else
        {
            gl_rect_2d(ix, iy + CARD_ICON, ix + CARD_ICON, iy, C_ACCENT, true);
            text(fontBold(), wstring_to_utf8str(utf8str_to_wstring(c.mTitle).substr(0, 1)), (F32)(ix + CARD_ICON / 2), (F32)(iy + CARD_ICON / 2), C_BG, LLFontGL::HCENTER);
        }
        const S32 tx = ix + CARD_ICON + 8;
        S32 right = r.mRight - CARD_CLOSE - 10;
        if (!c.mTag.empty())
        {
            // The tag says what kind of card it is ("AD", "SPONSORED"): words, not colour alone.
            right = badge(c.mTag, right, (F32)(r.mTop - 14), C_BADGE_UNREAD) - 6;
        }
        text(fontBold(), c.mTitle, (F32)tx, (F32)(r.mTop - 14), C_TEXT, LLFontGL::LEFT, std::max(10, right - tx));
        text(fontSmall(), c.mSubtitle, (F32)tx, (F32)(r.mBottom + 12), C_DIM, LLFontGL::LEFT, std::max(10, r.mRight - 8 - tx));
        const LLRect x = closeRect(i);
        const bool hot = mHover == i && mHoverClose;
        text(fontBold(), "x", (F32)x.getCenterX(), (F32)x.getCenterY(), hot ? C_TEXT : C_DIM, LLFontGL::HCENTER);
    }
}

bool WolfChatCards::handleMouseDown(S32 x, S32 y, MASK mask)
{
    const S32 i = cardAt(x, y);
    if (i < 0) return false;
    setFocus(true);
    mFocus = i;
    const Card c = mCards[i];      // a copy: the callbacks may replace the cards
    if (closeRect(i).pointInRect(x, y)) { if (mClose) mClose(c); }
    else if (mOpen) mOpen(c);
    return true;
}

bool WolfChatCards::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = cardAt(x, y);
    mHoverClose = mHover >= 0 && closeRect(mHover).pointInRect(x, y);
    getWindow()->setCursor(mHover >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

void WolfChatCards::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool WolfChatCards::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 i = cardAt(x, y);
    if (i < 0) return false;
    const Card& c = mCards[i];
    if (closeRect(i).pointInRect(x, y)) LLToolTipMgr::instance().show("Close this card (it stays closed for you)");
    else LLToolTipMgr::instance().show(c.mTitle + (c.mSubtitle.empty() ? std::string() : " - " + c.mSubtitle)
                                       + (c.mTag.empty() ? std::string() : " [" + c.mTag + "]") + "\nOpens " + c.mLink);
    return true;
}

bool WolfChatCards::handleKeyHere(KEY key, MASK mask)
{
    if (mCards.empty() || mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_LEFT:  mFocus = std::max(0, mFocus - 1); return true;
    case KEY_RIGHT: mFocus = std::min((S32)mCards.size() - 1, mFocus + 1); return true;
    case KEY_RETURN:
        if (mOpen) mOpen(Card(mCards[mFocus]));
        return true;
    case KEY_DELETE:
        if (mClose) mClose(Card(mCards[mFocus]));
        return true;
    default:
        return false;
    }
}

// =====================================================================================================================
// [2026-10-21] the GIF picker
// =====================================================================================================================

namespace
{
    const S32 GIF_COLS = 4;
    const S32 GIF_ROWS = 3;
    const S32 GIF_GAP = 8;
}

WolfChatGifGrid::WolfChatGifGrid(const Params& p)
:   LLUICtrl(p)
{
}

WolfChatGifGrid::~WolfChatGifGrid()
{
    release();
}

void WolfChatGifGrid::release()
{
    // BOOST_PREVIEW pins a texture; released as wolfalbumart.cpp setArt does.
    for (LLPointer<LLViewerFetchedTexture>& t : mStills)
    {
        if (t.notNull()) t->forceActive();
    }
    mStills.clear();
}

void WolfChatGifGrid::setResults(const std::vector<Result>& results)
{
    release();
    mResults = results;
    if ((S32)mResults.size() > GIF_COLS * GIF_ROWS) mResults.resize(GIF_COLS * GIF_ROWS);
    for (const Result& r : mResults)
    {
        // Source: wolfalbumart.cpp setArt - FTT_MAP_TILE fetches the URL as it is and decodes by its extension
        // (.jpg, lltexturefetch.cpp:1876-1878 getCodecFromExtension), as the world map's tiles.
        LLPointer<LLViewerFetchedTexture> t = LLViewerTextureManager::getFetchedTextureFromUrl(r.mStillUrl, FTT_MAP_TILE, true, LLGLTexture::BOOST_NONE);
        if (t.notNull()) t->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
        mStills.push_back(t);
    }
    mFocus = 0;
    mHover = -1;
    mLoading = false;
}

LLRect WolfChatGifGrid::cellRect(S32 i) const
{
    const S32 cw = (getRect().getWidth() - GIF_GAP * (GIF_COLS - 1)) / GIF_COLS;
    const S32 ch = (getRect().getHeight() - GIF_GAP * (GIF_ROWS - 1)) / GIF_ROWS;
    const S32 col = i % GIF_COLS;
    const S32 row = i / GIF_COLS;
    const S32 left = col * (cw + GIF_GAP);
    const S32 top = getRect().getHeight() - row * (ch + GIF_GAP);
    return LLRect(left, top, left + cw, top - ch);
}

S32 WolfChatGifGrid::cellAt(S32 x, S32 y) const
{
    for (S32 i = 0; i < (S32)mResults.size(); ++i)
    {
        if (cellRect(i).pointInRect(x, y)) return i;
    }
    return -1;
}

void WolfChatGifGrid::draw()
{
    gl_rect_2d(0, getRect().getHeight(), getRect().getWidth(), 0, C_BG, true);
    if (mLoading)
    {
        // Skeleton cells the shape of the results while they load.
        for (S32 i = 0; i < GIF_COLS * GIF_ROWS; ++i) gl_rect_2d(cellRect(i), C_SKELETON, true);
        return;
    }
    for (S32 i = 0; i < (S32)mResults.size(); ++i)
    {
        const LLRect r = cellRect(i);
        gl_rect_2d(r, C_SKELETON, true);
        LLViewerFetchedTexture* t = i < (S32)mStills.size() ? mStills[i].get() : nullptr;
        if (t)
        {
            t->addTextureStats((F32)(r.getWidth() * r.getHeight()));
            if (t->hasGLTexture())
            {
                // The stills are square (php/wolfchat.php WCH_STILL_SIDE); fit the cell.
                const S32 side = std::min(r.getWidth(), r.getHeight());
                gl_draw_scaled_image(r.mLeft + (r.getWidth() - side) / 2, r.mBottom + (r.getHeight() - side) / 2, side, side, t);
            }
        }
        if (mHover == i) gl_rect_2d(r, C_ACCENT, false);
        if (hasFocus() && mFocus == i)
        {
            gl_rect_2d(r, C_FOCUS, false);
            gl_rect_2d(r.mLeft + 1, r.mTop - 1, r.mRight - 1, r.mBottom + 1, C_FOCUS, false);
        }
    }
}

bool WolfChatGifGrid::handleMouseDown(S32 x, S32 y, MASK mask)
{
    setFocus(true);
    const S32 i = cellAt(x, y);
    if (i < 0) return true;
    mFocus = i;
    if (mPick) mPick(Result(mResults[i]));
    return true;
}

bool WolfChatGifGrid::handleHover(S32 x, S32 y, MASK mask)
{
    mHover = cellAt(x, y);
    getWindow()->setCursor(mHover >= 0 ? UI_CURSOR_HAND : UI_CURSOR_ARROW);
    return true;
}

void WolfChatGifGrid::onMouseLeave(S32 x, S32 y, MASK mask)
{
    mHover = -1;
    LLUICtrl::onMouseLeave(x, y, mask);
}

bool WolfChatGifGrid::handleToolTip(S32 x, S32 y, MASK mask)
{
    const S32 i = cellAt(x, y);
    if (i < 0) return false;
    LLToolTipMgr::instance().show((mResults[i].mTitle.empty() ? std::string("A GIF") : mResults[i].mTitle) + " - click to post it");
    return true;
}

bool WolfChatGifGrid::handleKeyHere(KEY key, MASK mask)
{
    const S32 n = (S32)mResults.size();
    if (n == 0 || mask != MASK_NONE) return false;
    switch (key)
    {
    case KEY_LEFT:  mFocus = std::max(0, mFocus - 1); return true;
    case KEY_RIGHT: mFocus = std::min(n - 1, mFocus + 1); return true;
    case KEY_UP:    if (mFocus >= GIF_COLS) mFocus -= GIF_COLS; return true;
    case KEY_DOWN:  if (mFocus + GIF_COLS < n) mFocus += GIF_COLS; return true;
    case KEY_RETURN:
        if (mPick) mPick(Result(mResults[mFocus]));
        return true;
    default:
        return false;
    }
}

WolfFloaterChatGif::WolfFloaterChatGif(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterChatGif::postBuild()
{
    mSearch = getChild<LLLineEditor>("gif_search_editor");
    mGrid = getChild<WolfChatGifGrid>("gif_grid");
    mTitle = getChild<LLTextBox>("results_title");
    mStatus = getChild<LLTextBox>("gif_status");
    mPrevBtn = getChild<LLButton>("prev_btn");
    mNextBtn = getChild<LLButton>("next_btn");
    mSearch->setCommitOnFocusLost(false);
    mSearch->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        std::string q = mSearch->getText();
        LLStringUtil::trim(q);
        mQuery = q;
        search(0);
    });
    getChild<LLButton>("gif_search_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&)
    {
        std::string q = mSearch->getText();
        LLStringUtil::trim(q);
        mQuery = q;
        search(0);
    });
    mPrevBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { search(std::max(0, mOffset - mPage)); });
    mNextBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { search(mOffset + mPage); });
    mGrid->setPickCallback([this](const WolfChatGifGrid::Result& r)
    {
        if (mPick) mPick(r.mId, r.mTitle);
        closeFloater();
    });
    return true;
}

void WolfFloaterChatGif::onOpen(const LLSD& key)
{
    mSearch->setFocus(true);
    if (!mBusy) search(mOffset);
}

void WolfFloaterChatGif::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    mStatus->setColor(error ? C_ERROR : C_DIM);
}

void WolfFloaterChatGif::search(S32 offset)
{
    if (mBusy) return;
    mBusy = true;
    const S32 gen = ++mGeneration;
    mGrid->setLoading(true);
    mPrevBtn->setEnabled(false);
    mNextBtn->setEnabled(false);
    mTitle->setText(mQuery.empty() ? std::string("Trending GIFs") : "GIFs for \"" + mQuery + "\"");
    setStatus("Looking...", false);
    LLSD b;
    b["q"] = mQuery;
    b["offset"] = offset;
    LLHandle<LLFloater> h = getHandle();
    WolfChat::api("gif_search", b, [h, gen, offset](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChatGif* self = static_cast<WolfFloaterChatGif*>(h.get());
        if (!self || gen != self->mGeneration) return;
        self->mBusy = false;
        if (!ok)
        {
            self->mGrid->setResults({});
            self->mPrevBtn->setEnabled(self->mOffset > 0);
            self->setStatus("GIFs could not be loaded: " + error, true);
            return;
        }
        std::vector<WolfChatGifGrid::Result> results;
        for (const LLSD& r : llsd::inArray(reply["results"]))
        {
            WolfChatGifGrid::Result res;
            res.mId = r["id"].asString();
            res.mTitle = r["title"].asString();
            res.mStillUrl = r["still"].asString();
            if (!res.mId.empty() && !res.mStillUrl.empty()) results.push_back(res);
        }
        self->mOffset = offset;
        if (reply["page"].asInteger() > 0) self->mPage = reply["page"].asInteger();
        self->mMore = reply["more"].asInteger() != 0;
        self->mGrid->setResults(results);
        self->mPrevBtn->setEnabled(self->mOffset > 0);
        self->mNextBtn->setEnabled(self->mMore);
        if (results.empty())
            self->setStatus(self->mQuery.empty() ? std::string("No GIFs to show right now.")
                                                 : "No GIFs found for \"" + self->mQuery + "\". Try other words.", false);
        else
            self->setStatus("Click a GIF (or use the arrow keys and Enter) to post it in the channel.", false);
    });
}

// =====================================================================================================================
// the window
// =====================================================================================================================

WolfFloaterChat::WolfFloaterChat(const LLSD& key)
:   LLFloater(key)
{
}

// static
WolfFloaterChat::Message WolfFloaterChat::messageFromLLSD(const LLSD& m)
{
    // Source: php/wolfchat.php wch_message_rows - the names of a message.
    Message msg;
    msg.mId = m["id"].asInteger();
    msg.mAgent = m["agent"].asUUID();
    msg.mName = m["name"].asString();
    msg.mText = m["text"].asString();
    msg.mImage = m["image"].asUUID();
    msg.mImageName = m["image_name"].asString();
    msg.mGif = WolfChat::GifSheet::fromLLSD(m["gif"]);
    msg.mPinned = m["pinned"].asInteger() != 0;
    msg.mTime = m["time"].asInteger();
    msg.mChannel = m["channel"].asUUID();
    return msg;
}

LLUUID WolfFloaterChat::shownChannel() const
{
    if (!isInVisibleChain() || isMinimized() || mView != VIEW_CHANNEL || mHistory) return LLUUID::null;
    return mChannel;
}

WolfFloaterChat::~WolfFloaterChat()
{
    if (LLFloater* p = mTexPicker.get()) p->closeFloater();
    if (LLFloater* p = mGifPicker.get()) p->closeFloater();
    if (LLFloater* p = mAddGroup.get()) p->closeFloater();
    mIMNewMsg.disconnect();
    mIMNoUnread.disconnect();
    if (mGroupChatShown && LLIMModel::getInstance()->getActiveSessionID() == mGroup) LLIMModel::getInstance()->resetActiveSessionID();
    // The rail's menu, as llchiclet.cpp:109-118 releases its context menu.
    if (LLView* menu = mServerMenu.get())
    {
        menu->die();
        mServerMenu.markDead();
    }
    if (LLView* menu = mChannelMenu.get())
    {
        menu->die();
        mChannelMenu.markDead();
    }
}

bool WolfFloaterChat::postBuild()
{
    mRail = getChild<WolfChatServerRail>("server_rail");
    mServerName = getChild<LLTextBox>("server_name");
    mEmptyState = getChild<LLView>("empty_state");
    mSidebar = getChild<WolfChatSidebar>("sidebar");
    mChannelTitle = getChild<LLTextBox>("channel_title");
    mStatus = getChild<LLTextBox>("status_text");
    mLog = getChild<LLTextEditor>("message_log");
    mInput = getChild<LLChatEntry>("message_input");
    mSearchEditor = getChild<LLLineEditor>("search_editor");
    mOnlineList = getChild<LLScrollListCtrl>("online_list");
    mOnlineTitle = getChild<LLTextBox>("online_title");
    mMeName = getChild<LLTextBox>("me_name");
    mVoiceStatus = getChild<LLTextBox>("voice_status");
    mSendBtn = getChild<LLButton>("send_btn");
    mImageBtn = getChild<LLButton>("image_btn");
    mEmojiBtn = getChild<LLButton>("emoji_btn");
    mMicBtn = getChild<LLButton>("mic_btn");
    mDeafenBtn = getChild<LLButton>("deafen_btn");
    mLeaveBtn = getChild<LLButton>("leave_call_btn");
    mInviteBtn = getChild<LLButton>("invite_btn");
    mOlderBtn = getChild<LLButton>("older_btn");
    mBackBtn = getChild<LLButton>("back_btn");
    mRenameBtn = getChild<LLButton>("rename_btn");
    mBridgeText = getChild<LLTextBox>("bridge_notice");     // [2026-10-21]
    mRelinkBtn = getChild<LLButton>("relink_btn");
    mRelinkBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBridge(mChannel); });
    mDeleteBtn = getChild<LLButton>("delete_btn");
    mMembersPanel = getChild<LLView>("members_column");
    mNotifyBtn = getChild<LLButton>("notify_btn");
    mPinsBtn = getChild<LLButton>("pins_btn");
    mGifBtn = getChild<LLButton>("gif_btn");
    mCards = getChild<WolfChatCards>("cards");

    // [2026-10-21] The server rail replaces the group combo box.
    mRail->setSelectCallback([this](const LLUUID& g) { selectGroup(g); });
    mRail->setAddCallback([this]() { onAddGroup(); });
    mRail->setMenuCallback([this](const LLUUID& g, S32 x, S32 y) { showServerMenu(g, x, y); });
    mRail->setMoveCallback([this](const LLUUID& g, bool up) { moveServer(g, up); });
    mRail->setRemoveCallback([this](const LLUUID& g) { removeServer(g); });
    getChild<LLButton>("add_group_big_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onAddGroup(); });
    // [2026-10-21] #group-chat follows the viewer's IM model: new messages (llimview.h:222 addNewMsgCallback, raised by
    // LLIMModel::addMessage llimview.cpp:2021) and "read elsewhere" (llimview.h:223 addNoUnreadMsgsCallback).
    LLHandle<LLFloater> him = getHandle();
    mIMNewMsg = LLIMModel::instance().addNewMsgCallback([him](const LLSD& msg)
    {
        if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(him.get())) self->onIMMessage(msg);
    });
    // [2026-10-21 CRASH FIX] "read" only refreshes the badges. It used to go through onIMMessage -> appendGroupChat ->
    // getMessages(..., hasFocus()), which marks read = sendNoUnreadMessages (llimview.cpp:1822-1825) -> this signal
    // again: endless recursion, stack overflow (Paul's crash 07:09 BST, core backtrace).
    mIMNoUnread = LLIMModel::instance().addNoUnreadMsgsCallback([him](const LLSD& msg)
    {
        if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(him.get())) self->onIMNoUnread(msg);
    });
    mSidebar->setClickCallback([this](WolfChatSidebar::EHit hit, const LLUUID& id) { onSidebarClick(hit, id); });
    mInput->setCommitOnFocusLost(false);
    mInput->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    mSendBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onSend(); });
    mImageBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPickImage(); });
    mEmojiBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onEmoji(); });
    mSearchEditor->setCommitOnFocusLost(false);
    mSearchEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSearch(); });
    getChild<LLButton>("mentions_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onMentions(); });
    getChild<LLButton>("members_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onToggleMembers(); });
    getChild<LLButton>("help_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onHelp(); });
    getChild<LLButton>("settings_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onVoiceSettings(); });
    mMicBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onMic(); });
    mDeafenBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeafen(); });
    mLeaveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLeaveCall(); });
    mInviteBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onInvitePeople(); });
    mOlderBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { loadOlder(); });
    mBackBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBackToChannel(); });
    mRenameBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRenameChannel(mChannel); });
    mDeleteBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeleteChannel(mChannel); });
    mSidebar->setMenuCallback([this](const LLUUID& c, S32 x, S32 y) { showChannelMenu(c, x, y); });     // [2026-10-21]
    mNotifyBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onNotifySetting(); });
    mPinsBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { loadPins(); });
    mGifBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPickGif(); });
    LLHandle<LLFloater> hc = getHandle();
    mCards->setOpenCallback([](const WolfChatCards::Card& c) { WolfChat::openLink(c.mLink); });
    mCards->setCloseCallback([hc](const WolfChatCards::Card& c)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(hc.get());
        if (!self) return;
        // Gone at once (it is only a card); the service remembers it for this account and answers the cards left.
        LLSD left = LLSD::emptyArray();
        for (const LLSD& x : llsd::inArray(self->mCardData))
        {
            if (x["id"].asInteger() != c.mId) left.append(x);
        }
        LLSD b;
        b["id"] = c.mId;
        WolfChat::api("card_close", b, [hc](bool ok, const LLSD& reply, const std::string& error)
        {
            WolfFloaterChat* s2 = static_cast<WolfFloaterChat*>(hc.get());
            if (!s2) return;
            if (!ok)
            {
                s2->setStatus("The card could not be closed for good (it may come back next time): " + error, true);
                return;
            }
            s2->setCards(reply["cards"]);
        });
        self->setCards(left);
    });
    mOnlineList->setDoubleClickCallback([this]()
    {
        LLScrollListItem* item = mOnlineList->getFirstSelected();
        if (item && item->getValue().asUUID().notNull()) LLAvatarActions::showProfile(item->getValue().asUUID());   // not the headings
    });

    getChild<LLButton>("members_btn")->setToggleState(true);
    mGifBtn->setVisible(false);         // shown when the grid says it has GIFs (setGifsAvailable)
    mCards->setVisible(false);
    mMeName->setText(myName());
    updateHeader();
    updateVoiceBar();
    return true;
}

void WolfFloaterChat::onOpen(const LLSD& key)
{
    if (!WolfGrid::isOnWolfTerritories())
    {
        // Wolf-only: the menu entry is hidden off Wolf; a link from elsewhere says why instead of opening empty.
        WolfGrid::noteWolfOnly("Wolf Chat");
        closeFloater();
        return;
    }
    if (key.isMap() && key.has("group"))
    {
        // A "wolfchat" link: its group opens even when it is not in the rail (the service refuses it if you are not
        // in the group, and the window says so).
        mPendingChannel = key["channel"].asUUID();
        const LLUUID g = key["group"].asUUID();
        if (key["add"].asBoolean())
        {
            // [2026-10-21] From a group chat button (WolfChat::openGroupChat): the group joins the rail if it is not in it.
            LLHandle<LLFloater> h = getHandle();
            LLSD b;
            b["groups"] = LLSD::emptyArray();
            b["groups"].append(g);
            WolfChat::api("server_add", b, [h](bool ok, const LLSD& reply, const std::string& error)
            {
                WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
                if (!self) return;
                if (!ok) { self->setStatus("This group could not be added to Wolf Chat: " + error, true); return; }
                if (self->mServersLoaded) self->setServers(reply["servers"]);
            });
        }
        if (mServersLoaded)
        {
            showEmpty(false);
            selectGroup(g);
            return;
        }
        mGroup = g;     // opened once the rail is in (setServers)
    }
    if (!mServersLoaded)
    {
        loadGroups();
        loadServers();
    }
}

void WolfFloaterChat::setStatus(const std::string& msg, bool error)
{
    // Failures stay until the next action works (they are not a toast that fades).
    mStatus->setText(msg);
    mStatus->setColor(error ? C_ERROR : C_DIM);
    if (error) LL_INFOS("WolfChat") << msg << LL_ENDL;
}

// ---------------------------------------------------------------------------------------------------- loading

void WolfFloaterChat::loadGroups()
{
    setStatus("Loading your groups...", false);
    LLHandle<LLFloater> h = getHandle();
    WolfChat::api("groups", LLSD::emptyMap(), [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok)
        {
            self->setStatus("Your groups could not be loaded: " + error + " Close and reopen Wolf Chat to try again.", true);
            return;
        }
        WolfChat::markUsed();          // [2026-10-21] the bells' background check may run for this account now
        self->setGifsAvailable(reply["gifs"].asInteger() != 0);
        self->setCards(reply["cards"]);
        // [2026-10-21] Group names, for a group opened from a link that is not in the rail.
        for (const LLSD& g : llsd::inArray(reply["groups"])) self->mGroupNames[g["id"].asUUID()] = g["name"].asString();
        if (self->mGroup.notNull()) self->mServerName->setText(self->serverName(self->mGroup));
        if (self->mStatus->getText() == "Loading your groups...") self->setStatus(std::string(), false);
    });
}

// ---------------------------------------------------------------------------------------------------- [2026-10-21]
// the server rail

std::string WolfFloaterChat::serverName(const LLUUID& group) const
{
    for (const WolfChatServerRail::Server& sv : mServers)
    {
        if (sv.mId == group) return sv.mName;
    }
    auto it = mGroupNames.find(group);
    return it != mGroupNames.end() ? it->second : std::string();
}

void WolfFloaterChat::loadServers()
{
    if (mBusyServers) return;
    mBusyServers = true;
    mNextServers = LLFrameTimer::getElapsedSeconds() + POLL_SERVERS;
    LLHandle<LLFloater> h = getHandle();
    WolfChat::api("servers_list", LLSD::emptyMap(), [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyServers = false;
        if (!ok)
        {
            if (!self->mServersLoaded)
            {
                self->mRail->setServers({});
                self->setStatus("Your Wolf Chat groups could not be loaded: " + error + " Wolf Chat tries again in 30 seconds.", true);
            }
            return;
        }
        self->setServers(reply["servers"]);
    });
}

void WolfFloaterChat::setServers(const LLSD& servers)
{
    // Source: php/wolfchat.php wch_servers - the names of a server.
    const bool first = !mServersLoaded;
    mServersLoaded = true;
    mServers.clear();
    for (const LLSD& sv : llsd::inArray(servers))
    {
        WolfChatServerRail::Server s;
        s.mId = sv["id"].asUUID();
        s.mName = sv["name"].asString();
        s.mInsignia = sv["insignia"].asUUID();
        s.mUnread = sv["unread"].asInteger();
        s.mMentions = sv["mentions"].asInteger();
        if (s.mId.notNull()) mServers.push_back(s);
    }
    // The open group has nothing unread here (reading it marks it read).
    for (WolfChatServerRail::Server& sv : mServers)
    {
        if (sv.mId == mGroup) { sv.mUnread = 0; sv.mMentions = 0; }
    }
    pushRail();
    mRail->setSelected(mGroup);
    if (mServers.empty() && (first || mGroup.isNull()) && !(first && mGroup.notNull()))
    {
        // No groups yet: say what this is, and offer the one thing to do (no group is added for you).
        showEmpty(true);
        return;
    }
    if (first)
    {
        showEmpty(false);
        // A link's group, else your active group if it is in the rail (as the viewer's group chat), else the first.
        LLUUID want = mGroup;
        if (want.isNull())
        {
            want = mServers.front().mId;
            for (const WolfChatServerRail::Server& sv : mServers)
            {
                if (sv.mId == gAgent.getGroupID()) want = sv.mId;
            }
        }
        mGroup.setNull();       // so selectGroup loads it
        selectGroup(want);
    }
}

void WolfFloaterChat::showEmpty(bool empty)
{
    mEmptyState->setVisible(empty);
    if (!empty) return;
    mGroup.setNull();
    mGroupName.clear();
    mChannels.clear();
    mChannel.setNull();
    mMessages.clear();
    ++mGeneration;
    mLog->clear();
    mSidebar->setChannels({}, false);
    mOnlineList->deleteAllItems();
    mOnlineTitle->setText(LLStringExplicit("MEMBERS"));
    mServerName->setText(LLStringUtil::null);
    mRail->setSelected(LLUUID::null);
    updateHeader();
    setStatus(std::string(), false);
}

void WolfFloaterChat::onAddGroup()
{
    WolfFloaterChatAddGroup* f = LLFloaterReg::getTypedInstance<WolfFloaterChatAddGroup>("wolf_chat_add_group");
    if (!f) return;
    LLHandle<LLFloater> h = getHandle();
    f->setAddedCallback([h](const LLSD& reply)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->setServers(reply["servers"]);
        const LLSD& added = reply["added"];
        const S32 n = added.isArray() ? (S32)added.size() : 0;
        if (n > 0)
        {
            self->showEmpty(false);
            self->selectGroup(added[0].asUUID());     // the first one added opens
        }
        self->setStatus(n == 0 ? std::string("Those groups were in Wolf Chat already.")
                               : llformat("Added %d %s to Wolf Chat.", n, n == 1 ? "group" : "groups"), false);
    });
    mAddGroup = f->getHandle();
    addDependentFloater(mAddGroup);
    f->openFloater();
    f->setFocus(true);
}

void WolfFloaterChat::showServerMenu(const LLUUID& group, S32 x, S32 y)
{
    mMenuServer = group;
    LLContextMenu* menu = static_cast<LLContextMenu*>(mServerMenu.get());
    if (!menu)
    {
        // Source: llchiclet.cpp:252-275 LLIMWellChiclet::createMenu - a context menu from XML with scoped commit and
        // enable callbacks; shown as llchiclet.cpp:182-196 does.
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        registrar.add("WolfChatServer.Action", [this](LLUICtrl*, const LLSD& param)
        {
            const std::string what = param.asString();
            if (what == "up") moveServer(mMenuServer, true);
            else if (what == "down") moveServer(mMenuServer, false);
            else if (what == "remove") removeServer(mMenuServer);
        });
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar enable_registrar;
        enable_registrar.add("WolfChatServer.Enable", [this](LLUICtrl*, const LLSD& param)
        {
            S32 at = -1;
            for (S32 i = 0; i < (S32)mServers.size(); ++i)
            {
                if (mServers[i].mId == mMenuServer) at = i;
            }
            if (param.asString() == "up") return at > 0;
            if (param.asString() == "down") return at >= 0 && at < (S32)mServers.size() - 1;
            return at >= 0;
        });
        menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>("menu_wolf_chat_server.xml",
                    LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
        if (!menu) return;
        mServerMenu = menu->getHandle();
    }
    menu->show(x, y);
    LLMenuGL::showPopup(mRail, menu, x, y);
}

void WolfFloaterChat::moveServer(const LLUUID& group, bool up)
{
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["group"] = group;
    b["dir"] = up ? "up" : "down";
    WolfChat::api("server_move", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok) { self->setStatus("The group was not moved: " + error, true); return; }
        self->setServers(reply["servers"]);
    });
}

void WolfFloaterChat::removeServer(const LLUUID& group)
{
    // Low risk and easy to undo (+ adds it back), so no "are you sure?" - the status line says how to undo it.
    const std::string name = serverName(group);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["group"] = group;
    WolfChat::api("server_remove", b, [h, group, name](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok) { self->setStatus("The group was not removed: " + error, true); return; }
        const bool was_open = self->mGroup == group;
        self->setServers(reply["servers"]);
        if (was_open)
        {
            if (self->mServers.empty()) self->showEmpty(true);
            else self->selectGroup(self->mServers.front().mId);
        }
        self->setStatus("Removed " + name + " from Wolf Chat. You are still in the group; + adds it back.", false);
    });
}

void WolfFloaterChat::selectGroup(const LLUUID& group)
{
    if (group.isNull()) return;
    if (group != mGroup || mChannels.empty())
    {
        mGroup = group;
        mGroupName = serverName(group);
        mServerName->setText(mGroupName);
        mRail->setSelected(group);
        mEmptyState->setVisible(false);
        mBridges.clear();               // [2026-10-21] this group's Discord links load next
        mNextBridges = 0.0;
        mChannels.clear();
        mChannel.setNull();
        mMessages.clear();
        mSeen.clear();
        mLastId.clear();
        mView = VIEW_CHANNEL;
        ++mGeneration;
        mSidebar->setLoading(true);
        mLog->clear();
        mOnlineList->deleteAllItems();
    }
    loadChannels();
    loadOnline();
    updateHeader();
}

void WolfFloaterChat::loadChannels()
{
    if (mGroup.isNull() || mBusyChannels) return;
    mBusyChannels = true;
    mNextChannels = LLFrameTimer::getElapsedSeconds() + POLL_CHANNELS;
    LLHandle<LLFloater> h = getHandle();
    const S32 gen = mGeneration;
    LLSD b;
    b["group"] = mGroup;
    WolfChat::api("channels", b, [h, gen](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyChannels = false;
        if (gen != self->mGeneration) { self->mNextChannels = 0.0; return; }     // [review] the new group: ask now
        if (!ok)
        {
            self->setStatus("The channels could not be loaded: " + error, true);
            return;
        }
        self->mCanPost = reply["can_post"].asInteger() != 0;
        self->mCanManage = reply["can_manage"].asInteger() != 0;          // [2026-10-21] the group's owners
        self->mCanModerate = reply["can_moderate"].asInteger() != 0;      // ModerateChat (messages, pins)
        self->mChannels.clear();
        {
            // [2026-10-21] Paul: "every chat in the new interface should automatically have a #group-chat channel linked
            // to the standard group chat". Always first; its id is the group id = the group IM session id.
            WolfChatSidebar::Channel gc;
            gc.mId = self->mGroup;
            gc.mName = "group-chat";
            gc.mGroupChat = true;
            self->mChannels.push_back(gc);
        }
        for (const LLSD& c : llsd::inArray(reply["channels"]))
        {
            WolfChatSidebar::Channel ch;
            ch.mId = c["id"].asUUID();
            ch.mName = c["name"].asString();
            ch.mVoice = c["kind"].asString() == "voice";
            ch.mRestricted = c["roles"].isArray() && c["roles"].size() > 0;     // owners get "roles" (php channels)
            if (!ch.mVoice)
            {
                self->mLastId[ch.mId] = c["last_id"].asInteger();
                // [2026-10-21] php/wolfchat.php channels: unread / mentions since you last read it, and its bell.
                ch.mUnreadCount = c["unread"].asInteger();
                ch.mMentions = c["mentions"].asInteger();
                self->mNotifyOf[ch.mId] = c["notify"].asString();
                ch.mMuted = c["notify"].asString() == "none";
            }
            for (const LLSD& p : llsd::inArray(c["voice"]))
            {
                WolfChatSidebar::Person person;
                person.mId = p["agent"].asUUID();
                person.mName = p["name"].asString();
                person.mMicOff = p["muted"].asInteger() != 0;
                person.mDeafened = p["deafened"].asInteger() != 0;
                ch.mPeople.push_back(person);
            }
            self->mChannels.push_back(ch);
        }
        // The channel to show: a link's, the one shown, else the first text channel.
        LLUUID show = self->mChannel;
        if (self->mPendingChannel.notNull())
        {
            for (const WolfChatSidebar::Channel& c : self->mChannels)
            {
                if (c.mId != self->mPendingChannel) continue;
                if (c.mVoice) self->joinVoice(c.mId);
                else show = c.mId;
            }
            self->mPendingChannel.setNull();
        }
        bool still_there = false;
        for (const WolfChatSidebar::Channel& c : self->mChannels) still_there = still_there || (!c.mVoice && c.mId == show);
        if (!still_there)
        {
            show.setNull();
            // The first Wolf Chat text channel; #group-chat only when asked for (it joins the group IM session).
            for (const WolfChatSidebar::Channel& c : self->mChannels)
            {
                if (!c.mVoice && !c.mGroupChat) { show = c.mId; break; }
            }
            if (self->mChannel.notNull()) self->setStatus("The channel you were reading was deleted.", false);
        }
        if (show != self->mChannel) self->showChannel(show);
        self->refreshSidebar();
        self->updateHeader();
    });
}

void WolfFloaterChat::loadMessages(bool newer_only)
{
    if (mChannel.isNull() || mView != VIEW_CHANNEL || mHistory || mGroupChatShown) return;    // [2026-10-21] not while showing a pin's place; #group-chat reads the IM model
    if (mBusyMessages)
    {
        mNextMessages = 0.0;    // [review] asked while an older answer is out: ask again as soon as it is in
        return;
    }
    if (mMessages.empty()) newer_only = false;     // nothing shown yet: the newest page (and Load older)
    mBusyMessages = true;
    mNextMessages = LLFrameTimer::getElapsedSeconds() + POLL_MESSAGES;
    LLHandle<LLFloater> h = getHandle();
    const S32 gen = mGeneration;
    LLSD b;
    b["channel"] = mChannel;
    if (newer_only && !mMessages.empty())
    {
        b["after"] = mMessages.back().mId;
        b["oldest"] = mMessages.front().mId;
    }
    WolfChat::api("messages", b, [h, gen, newer_only](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyMessages = false;
        if (gen != self->mGeneration) { self->mNextMessages = 0.0; return; }     // [review] the new channel: ask now
        if (!ok)
        {
            self->setStatus("New messages could not be loaded: " + error + " Wolf Chat keeps trying.", true);
            return;
        }
        bool rebuild = !newer_only;
        if (!newer_only) self->mMessages.clear();
        // Deleted since we showed them: taken out.
        for (const LLSD& id : llsd::inArray(reply["removed"]))
        {
            const S32 gone = id.asInteger();
            auto it = std::remove_if(self->mMessages.begin(), self->mMessages.end(), [gone](const Message& m) { return m.mId == gone; });
            if (it != self->mMessages.end())
            {
                self->mMessages.erase(it, self->mMessages.end());
                rebuild = true;
            }
        }
        std::vector<Message> added;
        for (const LLSD& m : llsd::inArray(reply["messages"]))
        {
            Message msg = messageFromLLSD(m);
            if (!self->mMessages.empty() && msg.mId <= self->mMessages.back().mId) continue;
            self->mMessages.push_back(msg);
            added.push_back(msg);
        }
        if (rebuild) self->rebuildLog();
        else for (const Message& m : added) self->appendToLog(m);
        if (!self->mMessages.empty())
        {
            self->mSeen[self->mChannel] = self->mMessages.back().mId;
            self->mLastId[self->mChannel] = std::max(self->mLastId[self->mChannel], self->mMessages.back().mId);
        }
        if (!newer_only)
        {
            self->mOlderBtn->setVisible(self->mView == VIEW_CHANNEL && (S32)self->mMessages.size() >= 50);
            if (self->mStatus->getText().find("could not be loaded") != std::string::npos) self->setStatus(std::string(), false);
        }
        self->refreshSidebar();
    });
}

void WolfFloaterChat::loadOlder()
{
    if (mChannel.isNull() || mMessages.empty() || mBusyMessages || mHistory || mGroupChatShown) return;
    mBusyMessages = true;
    LLHandle<LLFloater> h = getHandle();
    const S32 gen = mGeneration;
    LLSD b;
    b["channel"] = mChannel;
    b["before"] = mMessages.front().mId;
    WolfChat::api("messages", b, [h, gen](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyMessages = false;
        if (gen != self->mGeneration) return;
        if (!ok)
        {
            self->setStatus("Older messages could not be loaded: " + error, true);
            return;
        }
        std::vector<Message> older;
        for (const LLSD& m : llsd::inArray(reply["messages"])) older.push_back(messageFromLLSD(m));
        if (older.empty())
        {
            self->mOlderBtn->setVisible(false);
            self->setStatus("That is the start of this channel.", false);
            return;
        }
        self->mMessages.insert(self->mMessages.begin(), older.begin(), older.end());
        self->rebuildLog();
        self->mLog->setCursorPos(0);    // stay with the older messages just added
    });
}

void WolfFloaterChat::loadOnline()
{
    if (mGroup.isNull() || mBusyOnline || !mMembersPanel->getVisible()) return;
    mBusyOnline = true;
    mNextOnline = LLFrameTimer::getElapsedSeconds() + POLL_ONLINE;
    LLHandle<LLFloater> h = getHandle();
    const S32 gen = mGeneration;
    LLSD b;
    b["group"] = mGroup;
    WolfChat::api("online", b, [h, gen](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyOnline = false;
        if (gen != self->mGeneration) { self->mNextOnline = 0.0; return; }
        if (!ok)
        {
            self->mOnlineTitle->setText(LLStringExplicit("MEMBERS - could not load"));
            self->mOnlineTitle->setToolTip(error);
            return;
        }
        // [2026-10-21] Paul: "on the right i have my online and offline people". ONLINE - n, then OFFLINE - n (the
        // first 200, then "and N more"), as Discord's member list; the headings are rows that cannot be picked
        // (LLScrollListItem::Params enabled, llscrolllistitem.h:55), offline names dimmed and the word says it too.
        const LLUUID keep = self->mOnlineList->getSelectedValue().asUUID();
        self->mOnlineList->deleteAllItems();
        auto row = [self](const std::string& label, const LLSD& value, bool enabled, const LLColor4& colour, const LLFontGL* f)
        {
            LLScrollListItem::Params rp;
            rp.value(value);
            rp.enabled(enabled);
            LLScrollListCell::Params cp;
            cp.column("name");
            cp.value(label);
            cp.color(colour);
            cp.font(f);
            rp.columns.add(cp);
            self->mOnlineList->addRow(rp);
        };
        const S32 on = (S32)reply["online"].size();
        const S32 more = reply["offline_more"].asInteger();
        const S32 off = (S32)reply["offline"].size() + more;
        row(llformat("ONLINE - %d", on), LLSD(), false, C_DIM, fontSmall());
        for (const LLSD& p : llsd::inArray(reply["online"])) row(p["name"].asString(), LLSD(p["agent"].asUUID()), true, C_TEXT, font());
        row(llformat("OFFLINE - %d", off), LLSD(), false, C_DIM, fontSmall());
        for (const LLSD& p : llsd::inArray(reply["offline"])) row(p["name"].asString(), LLSD(p["agent"].asUUID()), true, C_DIM, font());
        if (more > 0) row(llformat("and %d more offline", more), LLSD(), false, C_DIM, fontSmall());
        if (keep.notNull()) self->mOnlineList->selectByValue(LLSD(keep));
        self->mOnlineTitle->setText(llformat("MEMBERS - %d", reply["members"].asInteger()));
        self->mOnlineTitle->setToolTip(std::string());
    });
}

// ---------------------------------------------------------------------------------------------------- showing

void WolfFloaterChat::showChannel(const LLUUID& channel)
{
    mChannel = channel;
    mView = VIEW_CHANNEL;
    mHistory = false;
    mHighlight = 0;
    ++mGeneration;
    mMessages.clear();
    mLog->clear();
    mBackBtn->setLabel(LLStringExplicit("Back to the channel"));
    mBackBtn->setVisible(false);
    mOlderBtn->setVisible(false);
    // [2026-10-21] #group-chat (its id is the group id) is the group IM session, not a Wolf Chat channel.
    const bool was_group_chat = mGroupChatShown;
    mGroupChatShown = channel.notNull() && channel == mGroup;
    if (was_group_chat && !mGroupChatShown && LLIMModel::getInstance()->getActiveSessionID() == mGroup)
        LLIMModel::getInstance()->resetActiveSessionID();
    if (mGroupChatShown) showGroupChat();
    else if (channel.notNull()) loadMessages(false);
    refreshSidebar();
    updateHeader();
}

void WolfFloaterChat::rebuildLog()
{
    mLog->clear();
    const std::vector<Message>& list = mView == VIEW_CHANNEL ? mMessages : mResults;
    if (list.empty())
    {
        LLStyle::Params dim;
        dim.color(C_DIM);
        dim.readonly_color(C_DIM);
        if (mView == VIEW_SEARCH) mLog->appendText("Nothing in this group's channels matches that.", false, dim);
        else if (mView == VIEW_MENTIONS) mLog->appendText("No one has mentioned you by name in this group yet.", false, dim);
        else if (mView == VIEW_PINS) mLog->appendText("Nothing is pinned in this channel. Group moderators pin messages "
                                                      "with the Pin button next to a message.", false, dim);
        else if (mGroupChatShown) mLog->appendText("No group chat messages yet in this session.", false, dim);
        else if (mChannel.notNull()) mLog->appendText("No messages yet. Say hello - everyone in the group can read this channel.", false, dim);
        return;
    }
    S32 jump_to = -1;
    for (const Message& m : list)
    {
        if (mHistory && m.mId == mHighlight) jump_to = mLog->getLength();
        appendToLog(m);
    }
    if (jump_to >= 0) mLog->setCursorPos(jump_to);     // [2026-10-21] scroll to the pinned message jumped to
}

void WolfFloaterChat::appendToLog(const Message& m)
{
    if (mView == VIEW_CHANNEL && mMessages.size() == 1 && mLog->getLength() > 0) mLog->clear();   // the "No messages yet" line
    const bool first = mLog->getLength() == 0;

    // Name and time.
    LLStyle::Params name;
    name.font(fontBold());
    name.color(m.mAgent == gAgentID ? C_ACCENT : C_NAME);
    name.readonly_color(m.mAgent == gAgentID ? C_ACCENT : C_NAME);
    std::string who = m.mName.empty() ? std::string("(someone)") : m.mName;
    // [2026-10-21] Written in Discord (the bridge names them "<Name> (Discord)"): their own tint, and the word says it.
    const std::string discord_tail = " (Discord)";
    if (who.size() > discord_tail.size() && who.compare(who.size() - discord_tail.size(), discord_tail.size(), discord_tail) == 0)
    {
        name.color(C_DISCORD);
        name.readonly_color(C_DISCORD);
    }
    if (m.mPinned) who = "[pinned] " + who;          // [2026-10-21] in words, not only a symbol
    if (mHistory && m.mId == mHighlight)
    {
        name.color(C_ACCENT);
        name.readonly_color(C_ACCENT);
        who = ">> " + who;
    }
    if (mView != VIEW_CHANNEL)
    {
        for (const WolfChatSidebar::Channel& c : mChannels)
        {
            if (c.mId == m.mChannel) who += "  in #" + c.mName;
        }
    }
    mLog->appendText(who, !first, name);
    LLStyle::Params when;
    when.font(fontSmall());
    when.color(C_DIM);
    when.readonly_color(C_DIM);
    mLog->appendText("   " + (m.mTimeText.empty() ? clockText(m.mTime) : m.mTimeText) + "  ", false, when);

    // Delete: your own message, or anyone's for a group moderator.
    const bool mine = m.mAgent == gAgentID;
    if (mView == VIEW_CHANNEL && !mGroupChatShown && (mine || mCanModerate))
    {
        LLButton::Params bp;
        bp.name("delete_message");
        bp.label("Delete");
        bp.font(fontSmall());
        bp.rect(LLRect(0, 16, 46, 0));
        bp.tool_tip(mine ? "Delete your message" : "Delete this message (group moderator)");
        LLButton* del = LLUICtrlFactory::create<LLButton>(bp);
        const S32 id = m.mId;
        LLHandle<LLFloater> h = getHandle();
        del->setClickedCallback([h, id, mine](LLUICtrl*, const LLSD&)
        {
            if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get())) self->deleteMessage(id, mine);
        });
        LLInlineViewSegment::Params ip;
        ip.view = del;
        ip.force_newline = false;
        ip.left_pad = 4;
        ip.right_pad = 0;
        ip.top_pad = 0;
        ip.bottom_pad = 0;
        mLog->appendWidget(ip, " ", false);
    }

    // [2026-10-21] Pin / Unpin (group moderators) in the channel; Jump in the pinned list.
    if ((mView == VIEW_CHANNEL && mCanModerate && !mGroupChatShown) || mView == VIEW_PINS)
    {
        const bool jump = mView == VIEW_PINS;
        LLButton::Params bp;
        bp.name(jump ? "jump_message" : "pin_message");
        bp.label(jump ? "Jump" : (m.mPinned ? "Unpin" : "Pin"));
        bp.font(fontSmall());
        bp.rect(LLRect(0, 16, 46, 0));
        bp.tool_tip(jump ? "Show this message where it is in the channel"
                         : (m.mPinned ? "Take this message off the channel's pinned list (group moderator)"
                                      : "Pin this message to the channel so everyone can find it (group moderator)"));
        LLButton* btn = LLUICtrlFactory::create<LLButton>(bp);
        const S32 id = m.mId;
        const bool pinned = m.mPinned;
        const LLUUID channel = mChannel;
        LLHandle<LLFloater> h = getHandle();
        btn->setClickedCallback([h, id, pinned, jump, channel](LLUICtrl*, const LLSD&)
        {
            WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
            if (!self) return;
            if (jump) self->showAround(channel, id);
            else self->setPinned(id, !pinned);
        });
        LLInlineViewSegment::Params ip;
        ip.view = btn;
        ip.force_newline = false;
        ip.left_pad = 4;
        ip.right_pad = 0;
        ip.top_pad = 0;
        ip.bottom_pad = 0;
        mLog->appendWidget(ip, " ", false);
    }

    // The words (links in them work: the log parses URLs and SLURLs).
    if (!m.mText.empty())
    {
        LLStyle::Params body;
        body.color(C_TEXT);
        body.readonly_color(C_TEXT);
        mLog->appendText(m.mText, true, body);
    }

    // [2026-10-21] The GIF: playing in the channel, its first frame in search, mentions and pins.
    if (m.mGif.valid())
    {
        WolfChatImage::Params p;
        p.name("posted_gif");
        const S32 gh = 128;
        const S32 gw = std::min(256, std::max(64, gh * m.mGif.mCellW / std::max(1, m.mGif.mCellH)));
        p.rect(LLRect(0, gh, gw, 0));
        WolfChatImage* img = LLUICtrlFactory::create<WolfChatImage>(p);
        img->setGif(m.mGif, mView == VIEW_CHANNEL);
        LLInlineViewSegment::Params ip;
        ip.view = img;
        ip.force_newline = true;
        ip.left_pad = 0;
        ip.right_pad = 0;
        ip.top_pad = 4;
        ip.bottom_pad = 4;
        mLog->appendWidget(ip, "\n", false);
    }

    // The picture.
    if (m.mImage.notNull())
    {
        WolfChatImage::Params p;
        p.name("posted_image");
        p.rect(LLRect(0, 180, 240, 0));
        p.image_id(m.mImage);
        p.image_name(m.mImageName);
        WolfChatImage* img = LLUICtrlFactory::create<WolfChatImage>(p);
        LLInlineViewSegment::Params ip;
        ip.view = img;
        ip.force_newline = true;
        ip.left_pad = 0;
        ip.right_pad = 0;
        ip.top_pad = 4;
        ip.bottom_pad = 4;
        mLog->appendWidget(ip, "\n", false);
    }
}

void WolfFloaterChat::updateHeader()
{
    std::string title = "Wolf Chat";
    if (mView == VIEW_SEARCH) title = "Search results";
    else if (mView == VIEW_MENTIONS) title = "@ Mentions of you";
    else if (mView == VIEW_PINS)
    {
        title = "Pinned";
        for (const WolfChatSidebar::Channel& c : mChannels)
        {
            if (c.mId == mChannel) title = "Pinned in # " + c.mName;
        }
    }
    else
    {
        for (const WolfChatSidebar::Channel& c : mChannels)
        {
            if (c.mId == mChannel) title = "# " + c.mName;
        }
    }
    mChannelTitle->setText(title);
    const bool channel_view = mView == VIEW_CHANNEL && mChannel.notNull();
    // [2026-10-21] #group-chat is the standard group chat: no rename / delete / pins / bell / pictures / GIFs / search
    // here (the status line says why when it opens).
    const bool gc = mGroupChatShown;
    mRenameBtn->setVisible(channel_view && mCanManage && !gc);
    mDeleteBtn->setVisible(channel_view && mCanManage && !gc);
    mNotifyBtn->setVisible(!gc);
    mPinsBtn->setVisible(!gc);
    mImageBtn->setVisible(!gc);
    mGifBtn->setVisible(mGifsAvailable && !gc);
    mSearchEditor->setVisible(!gc);
    const bool can_write = channel_view && mCanPost;
    mInput->setEnabled(can_write);
    mSendBtn->setEnabled(can_write);
    mImageBtn->setEnabled(can_write);
    mEmojiBtn->setEnabled(can_write);
    mGifBtn->setEnabled(can_write);
    // [2026-10-21] The bell names its setting (words, not an icon alone); Pins needs a channel.
    const bool has_channel = mChannel.notNull();
    std::string bell = mNotifyOf.count(mChannel) ? mNotifyOf[mChannel] : std::string();
    mNotifyBtn->setEnabled(has_channel);
    mNotifyBtn->setLabel(LLStringExplicit(bell == "all" ? "Notify: all" : bell == "none" ? "Notify: off" : "Notify: @"));
    mNotifyBtn->setToolTip(std::string("Notifications for this channel while Wolf Chat is closed - now: ")
                           + (bell == "all" ? "all messages" : bell == "none" ? "nothing" : "only messages that mention you")
                           + ". Click to change.");
    mPinsBtn->setEnabled(has_channel);
    mPinsBtn->setToggleState(mView == VIEW_PINS);
    // [2026-10-21] A bridged channel says so for everyone; a stopped bridge says why and owners get "Link again".
    auto bl = (mView == VIEW_CHANNEL && !gc) ? mBridges.find(mChannel) : mBridges.end();
    if (bl != mBridges.end())
    {
        const bool broken = bl->second.mState == "broken";
        const std::string where = bl->second.mDiscordChannel.empty() ? std::string()
            : " (#" + bl->second.mDiscordChannel + (bl->second.mGuild.empty() ? std::string() : " in " + bl->second.mGuild) + ")";
        const std::string line = broken ? "Discord bridge stopped: " + bl->second.mReason
                                        : (mBridgeNotice.empty() ? std::string("Bridged to Discord: messages in this channel are visible outside the grid.") : mBridgeNotice);
        mBridgeText->setText(line);
        mBridgeText->setToolTip(line + where);
        mBridgeText->setColor(broken ? C_ERROR : C_DISCORD);
        mBridgeText->setVisible(true);
        mRelinkBtn->setVisible(broken && mCanManage);
    }
    else
    {
        mBridgeText->setVisible(false);
        mRelinkBtn->setVisible(false);
    }
    if (channel_view && !mCanPost)
        mInput->setLabel(LLStringExplicit("Your group role does not allow posting in this group's chat."));
    else if (gc)
        mInput->setLabel(LLStringExplicit("Message the group chat (everyone in the group sees it)..."));
    else
        mInput->setLabel(LLStringExplicit(mChannel.notNull() ? "Message in #" + title.substr(std::min<size_t>(2, title.size())) + "..." : std::string()));
}

void WolfFloaterChat::refreshSidebar()
{
    std::vector<WolfChatSidebar::Channel> shown = mChannels;
    const LLUUID room = WolfChat::roomChannel();
    for (WolfChatSidebar::Channel& c : shown)
    {
        if (c.mGroupChat)
        {
            // [2026-10-21] Unread from the IM model (llimview.h:312 getNumUnread; -1 = no session).
            c.mUnread = !(mGroupChatShown && hasFocus()) && LLIMModel::instance().getNumUnread(c.mId) > 0;
            continue;
        }
        if (!c.mVoice)
        {
            // [2026-10-21] The channel being read has nothing unread (php/wolfchat.php marks it read as it is shown).
            const bool shown = c.mId == mChannel && mView == VIEW_CHANNEL && !mHistory;
            if (shown) { c.mUnreadCount = 0; c.mMentions = 0; }
            c.mUnread = !shown && c.mId != mChannel && (c.mUnreadCount > 0 || mLastId[c.mId] > mSeen[c.mId]);
            auto bl = mBridges.find(c.mId);       // [2026-10-21] php/wolfbridge.php links
            c.mBridged = bl != mBridges.end();
            c.mBridgeBroken = c.mBridged && bl->second.mState == "broken";
            continue;
        }
        if (c.mId != room) continue;
        // The room you are in: who is there and their state comes from the voice connection itself, not the
        // service's 20 s heartbeat (LLVoiceClient participant list of the current channel).
        std::set<LLUUID> ids;
        LLVoiceClient::getInstance()->getParticipantList(ids);
        std::map<LLUUID, WolfChatSidebar::Person> reported;
        for (const WolfChatSidebar::Person& p : c.mPeople) reported[p.mId] = p;
        c.mPeople.clear();
        for (const LLUUID& id : ids)
        {
            WolfChatSidebar::Person p;
            p.mId = id;
            LLAvatarName av;
            if (LLAvatarNameCache::get(id, &av)) p.mName = av.getCompleteName();
            else if (reported.count(id)) p.mName = reported[id].mName;
            // Source: llvoicewebrtc.cpp:2179-2190 getIsSpeaking / :2207-2223 getCurrentPower - the current session's
            // participants (the room while you are in it): remote ones from the voice server's "v"/"p" updates
            // (:3884-3889), your own from your microphone level (:2324-2332 predUpdateOwnVolume).
            p.mSpeaking = LLVoiceClient::getInstance()->getIsSpeaking(id);
            p.mLevel = LLVoiceClient::getInstance()->getCurrentPower(id);
            p.mMutedByMe = LLVoiceClient::getInstance()->getOnMuteList(id);
            if (id == gAgentID)
            {
                p.mMicOff = !WolfChat::micOn();
                p.mDeafened = WolfChat::deafened();
            }
            else
            {
                p.mMicOff = LLVoiceClient::getInstance()->getIsModeratorMuted(id) || (reported.count(id) && reported[id].mMicOff);
                p.mDeafened = reported.count(id) && reported[id].mDeafened;
            }
            c.mPeople.push_back(p);
        }
    }
    mSidebar->setSelected(mView == VIEW_CHANNEL ? mChannel : LLUUID::null);
    mSidebar->setJoined(room);
    mSidebar->setChannels(shown, mCanManage);
}

void WolfFloaterChat::updateVoiceBar()
{
    // [2026-10-21] The real state: connected, connecting (or reconnecting), or why not (WolfChat::roomNote).
    const bool in = WolfChat::inRoom();
    const bool connected = WolfChat::roomConnected();
    const std::string note = WolfChat::roomNote();
    std::string where;
    LLColor4 colour = C_DIM;
    if (connected)
    {
        where = "Voice connected: " + WolfChat::roomName();
        if (WolfChat::roomGroup() != mGroup) where += " (another group)";
        colour = C_OK;
    }
    else if (!note.empty())
    {
        where = note;
        colour = C_ERROR;
    }
    else if (in)
    {
        where = LLVoiceChannel::isSuspended() ? "Voice paused while voice settings are open: " + WolfChat::roomName()
                                               : "Connecting to " + WolfChat::roomName() + "...";
    }
    else
    {
        where = "Not in a voice room. Click a voice channel to join it.";
    }
    mVoiceStatus->setText(where);
    mVoiceStatus->setColor(colour);
    mLeaveBtn->setEnabled(in);
    mInviteBtn->setEnabled(in);
    const bool mic = WolfChat::micOn();
    mMicBtn->setToggleState(mic);
    mMicBtn->setLabel(mic ? LLStringExplicit("Mic on") : LLStringExplicit("Mic off"));
    const bool deaf = WolfChat::deafened();
    mDeafenBtn->setToggleState(deaf);
    mDeafenBtn->setLabel(deaf ? LLStringExplicit("Deafened") : LLStringExplicit("Deafen"));
}

void WolfFloaterChat::draw()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (isInVisibleChain() && mGroup.notNull() && !isMinimized())
    {
        if (now >= mNextMessages) loadMessages(true);
        if (now >= mNextChannels) loadChannels();
        if (now >= mNextOnline) loadOnline();
    }
    if (isInVisibleChain() && mServersLoaded && !isMinimized() && now >= mNextServers) loadServers();   // [2026-10-21] badges
    if (isInVisibleChain() && mGroup.notNull() && !isMinimized() && now >= mNextBridges) loadBridges();  // [2026-10-21] Discord
    // [2026-10-21] #group-chat: what was typed before the session was ready goes once it is (llimview.h:165
    // mSessionInitialized, set by sessionInitReplyReceived llimview.cpp:1175-1177).
    if (!mGroupChatQueue.empty() && mGroup.notNull())
    {
        LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(mGroup);
        if (session && session->mSessionInitialized)
        {
            std::vector<std::string> queued;
            queued.swap(mGroupChatQueue);
            for (const std::string& t : queued) LLIMModel::sendMessage(t, mGroup, mGroup, IM_SESSION_SEND);
        }
    }
    // Voice state changes without asking us (the toolbar's Speak, another call, a moderator, settings): read it often.
    // [2026-10-21] Every 0.15 s (time, not frames) and the room's rows only while in a room - the speaking rings.
    if (now >= mNextVoiceUi)
    {
        mNextVoiceUi = now + 0.15;
        updateVoiceBar();
        if (WolfChat::inRoom()) refreshSidebar();
    }
    LLFloater::draw();
}

// ---------------------------------------------------------------------------------------------------- doing

void WolfFloaterChat::onSidebarClick(WolfChatSidebar::EHit hit, const LLUUID& id)
{
    switch (hit)
    {
    case WolfChatSidebar::HIT_ADD_TEXT: askNewChannel(false); return;
    case WolfChatSidebar::HIT_ADD_VOICE: askNewChannel(true); return;
    case WolfChatSidebar::HIT_PERSON: LLAvatarActions::showProfile(id); return;
    case WolfChatSidebar::HIT_CHANNEL:
        for (const WolfChatSidebar::Channel& c : mChannels)
        {
            if (c.mId != id) continue;
            if (c.mVoice) joinVoice(id);
            else showChannel(id);
        }
        return;
    default:
        return;
    }
}

void WolfFloaterChat::joinVoice(const LLUUID& channel)
{
    // [2026-10-21] Only when the connection is really up; a room you seem to be in but is not connected is joined again.
    if (WolfChat::roomChannel() == channel && WolfChat::roomConnected())
    {
        setStatus("You are already in this voice room.", false);
        return;
    }
    std::string name;
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == channel) name = c.mName;
    }
    const std::string label = name + " (" + mGroupName + ")";
    setStatus("Joining " + name + "...", false);
    LLHandle<LLFloater> h = getHandle();
    WolfChat::joinRoom(mGroup, channel, label, [h, name](bool ok, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok)
        {
            self->setStatus("Could not join " + name + ": " + error, true);
            return;
        }
        self->setStatus("You joined " + name + ". Your mic is " + (WolfChat::micOn() ? "on" : "off - press Mic to talk") + ".", false);
        self->mNextChannels = 0.0;     // show yourself in the room at once
        self->updateVoiceBar();
    });
}

void WolfFloaterChat::onSend()
{
    if (mBusyPost) return;
    if (mChannel.isNull() || mView != VIEW_CHANNEL)
    {
        setStatus("Pick a text channel on the left first.", true);
        return;
    }
    std::string msg = mInput->getText();
    LLStringUtil::trim(msg);
    if (msg.empty()) return;
    if (mGroupChatShown)
    {
        sendGroupChat(msg);
        return;
    }
    mBusyPost = true;
    mSendBtn->setEnabled(false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannel;
    b["text"] = msg;
    WolfChat::api("post", b, [h, msg](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyPost = false;
        self->updateHeader();
        if (!ok)
        {
            // The words stay in the box so nothing is lost.
            self->setStatus("Your message was not posted: " + error, true);
            return;
        }
        if (self->mInput->getText() == msg || LLStringUtil::null == self->mInput->getText()) self->mInput->setText(LLStringUtil::null);
        self->setStatus(std::string(), false);
        self->mNextMessages = 0.0;
    });
}

void WolfFloaterChat::onEmoji()
{
    // The viewer's emoji picker attached to the message box (fsfloaterim.cpp onEmojiPickerToggleBtnClicked).
    mInput->setFocus(true);
    mInput->showEmojiHelper();
}

void WolfFloaterChat::onPickImage()
{
    // Source: wolffloateradventurestudioedit.cpp chooseTexture (itself llpanelprofile.cpp onShowTexturePicker): the
    // viewer's texture picker owned by this window. Its commit gives the inventory item when the picture was picked
    // from inventory (lltexturectrl.cpp commitCallback PICKER_INVENTORY), which is what php/wolfchat.php checks.
    if (LLFloater* old = mTexPicker.get()) old->closeFloater();
    LLFloaterTexturePicker* fp = new LLFloaterTexturePicker(this, LLUUID::null, LLUUID::null, LLUUID::null, false, false,
                                                            "CHOOSE A PICTURE TO POST", PERM_NONE, PERM_NONE, false, NULL, PICK_TEXTURE);
    mTexPicker = fp->getHandle();
    LLHandle<LLFloater> h = getHandle();
    fp->setOnFloaterCommitCallback([h](LLTextureCtrl::ETexturePickOp op, LLPickerSource source, const LLUUID& asset_id,
                                       const LLUUID& inventory_id, const LLUUID&)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self || op != LLTextureCtrl::TEXTURE_SELECT) return;
        if (inventory_id.isNull())
        {
            self->setStatus("Pick the picture on the Inventory tab of the picker (a texture or snapshot you own).", true);
            return;
        }
        self->postImage(inventory_id);
    });
    fp->setLocalTextureEnabled(false);
    fp->setBakeTextureEnabled(false);
    fp->setCanApply(false, true, false);
    addDependentFloater(mTexPicker);
    fp->openFloater();
    fp->setFocus(true);
}

bool WolfFloaterChat::handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type,
                                        void* cargo_data, EAcceptance* accept, std::string& tooltip_msg)
{
    // A texture or snapshot dragged from inventory onto the window is posted to the channel shown.
    if (cargo_type == DAD_TEXTURE && mView == VIEW_CHANNEL && mChannel.notNull() && mCanPost && !mGroupChatShown)
    {
        LLInventoryItem* item = (LLInventoryItem*)cargo_data;
        *accept = item ? ACCEPT_YES_COPY_SINGLE : ACCEPT_NO;
        if (item) tooltip_msg = "Post this picture in the channel";
        if (drop && item) postImage(item->getUUID());
        return true;
    }
    return LLFloater::handleDragAndDrop(x, y, mask, drop, cargo_type, cargo_data, accept, tooltip_msg);
}

void WolfFloaterChat::postImage(const LLUUID& item_id)
{
    if (mChannel.isNull() || mView != VIEW_CHANNEL)
    {
        setStatus("Pick a text channel on the left first, then the picture.", true);
        return;
    }
    // Said here at once when the viewer can see it; php/wolfchat.php checks the same from the inventory database.
    if (LLViewerInventoryItem* item = gInventory.getItem(item_id))
    {
        const LLPermissions& perm = item->getPermissions();
        if (item->getType() != LLAssetType::AT_TEXTURE)
        {
            setStatus("Only textures and snapshots can be posted.", true);
            return;
        }
        if (!perm.allowCopyBy(gAgentID) || !perm.allowOperationBy(PERM_TRANSFER, gAgentID))
        {
            setStatus("\"" + item->getName() + "\" is not copy and transfer, so it cannot be shared.", true);
            return;
        }
    }
    std::string words = mInput->getText();
    LLStringUtil::trim(words);
    mBusyPost = true;
    setStatus("Posting the picture...", false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannel;
    b["item"] = item_id;
    b["text"] = words;      // what is typed goes with the picture
    WolfChat::api("post", b, [h, words](bool ok, const LLSD&, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyPost = false;
        if (!ok)
        {
            self->setStatus("The picture was not posted: " + error, true);
            return;
        }
        if (!words.empty()) self->mInput->setText(LLStringUtil::null);
        self->setStatus(std::string(), false);
        self->mNextMessages = 0.0;
    });
}

void WolfFloaterChat::deleteMessage(S32 id, bool mine)
{
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", mine ? std::string("Delete your message? Everyone in the group stops seeing it.")
                                    : std::string("Delete this message? Everyone in the group stops seeing it.")),
        LLSD(), [h, id](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        LLSD b;
        b["id"] = id;
        WolfChat::api("delete_message", b, [h, id](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
            if (!self) return;
            if (!ok)
            {
                self->setStatus("The message was not deleted: " + error, true);
                return;
            }
            auto it = std::remove_if(self->mMessages.begin(), self->mMessages.end(), [id](const Message& m) { return m.mId == id; });
            self->mMessages.erase(it, self->mMessages.end());
            self->rebuildLog();
            self->setStatus("Message deleted.", false);
        });
        return false;
    });
}

void WolfFloaterChat::onSearch()
{
    std::string q = mSearchEditor->getText();
    LLStringUtil::trim(q);
    if (q.empty()) { onBackToChannel(); return; }
    if (q.size() < 3)
    {
        setStatus("Type at least 3 letters to search.", true);
        return;
    }
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["group"] = mGroup;
    b["q"] = q;
    setStatus("Searching...", false);
    WolfChat::api("search", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok) { self->setStatus("Search failed: " + error, true); return; }
        self->mResults.clear();
        for (const LLSD& m : llsd::inArray(reply["messages"])) self->mResults.push_back(messageFromLLSD(m));
        self->mView = VIEW_SEARCH;
        self->mBackBtn->setVisible(true);
        self->mOlderBtn->setVisible(false);
        self->rebuildLog();
        self->updateHeader();
        self->refreshSidebar();
        self->setStatus(llformat("%d found (newest first, at most 30).", (S32)self->mResults.size()), false);
    });
}

void WolfFloaterChat::onMentions()
{
    if (mGroup.isNull()) return;
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["group"] = mGroup;
    WolfChat::api("mentions", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok) { self->setStatus("Mentions could not be loaded: " + error, true); return; }
        self->mResults.clear();
        for (const LLSD& m : llsd::inArray(reply["messages"])) self->mResults.push_back(messageFromLLSD(m));
        self->mView = VIEW_MENTIONS;
        self->mBackBtn->setVisible(true);
        self->mOlderBtn->setVisible(false);
        self->rebuildLog();
        self->updateHeader();
        self->refreshSidebar();
        self->setStatus(std::string(), false);
    });
}

void WolfFloaterChat::onBackToChannel()
{
    mSearchEditor->setText(LLStringUtil::null);
    const LLUUID c = mChannel;
    showChannel(c);
}

void WolfFloaterChat::onToggleMembers()
{
    const bool show = !mMembersPanel->getVisible();
    mMembersPanel->setVisible(show);
    getChild<LLButton>("members_btn")->setToggleState(show);
    if (show) mNextOnline = 0.0;
}

void WolfFloaterChat::onHelp()
{
    LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
        "Wolf Chat is your group's own chat, like Discord.\n\n"
        "- Your groups are the tiles down the far left, like Discord servers. + adds groups (only the ones you "
        "choose); click a tile to open it. Right-click a tile (or Alt+Up / Alt+Down / Delete) to move it or remove "
        "it from Wolf Chat - you stay in the group. Only groups in Wolf Chat can notify you.\n"
        "- # text channels: read and post. Pictures: the picture button, or drag a texture or snapshot from your "
        "inventory onto the window (it must be copy and transfer).\n"
        "- Voice channels: click one to join the voice room. Mic turns your microphone on and off; Deafen stops all "
        "voice; Leave Call takes you back to the voice where you stand; Invite People sends a link to the room.\n"
        "- The group's owners add channels (+), and right-click a channel to rename it, delete it or choose who can "
        "see it (everyone, or only some roles - a lock marks those). #group-chat is always there for everyone.\n"
        "- Group moderators (roles with \"Moderate group chat\") delete messages and pin them.\n"
        "- Notify (the bell): what Wolf Chat tells you about from this channel while its window is closed - all "
        "messages, only messages that mention you (the start), or nothing. Numbers on the left are unread messages; "
        "@ numbers mention you.\n"
        "- Pins: the channel's pinned messages; Jump shows one where it is.\n"
        "- GIF: search GIPHY and post a GIF (it plays in the channel; click it to see it big, click again to pause).\n"
        "- Users shows the group's members, online first then offline; double-click someone for their profile."));
}

void WolfFloaterChat::onMic()
{
    WolfChat::setMic(!WolfChat::micOn());
    updateVoiceBar();
}

void WolfFloaterChat::onDeafen()
{
    WolfChat::setDeafened(!WolfChat::deafened());
    updateVoiceBar();
}

void WolfFloaterChat::onVoiceSettings()
{
    LLFloaterReg::showInstance("preferences", LLSD().with("tab", "audio"));
}

void WolfFloaterChat::onLeaveCall()
{
    WolfChat::leaveRoom();
    setStatus("You left the voice room.", false);
    mNextChannels = 0.0;
    updateVoiceBar();
}

void WolfFloaterChat::onInvitePeople()
{
    if (!WolfChat::inRoom())
    {
        setStatus("Join a voice room first, then invite people to it.", true);
        return;
    }
    const LLUUID group = WolfChat::roomGroup();
    const LLUUID channel = WolfChat::roomChannel();
    const std::string room = WolfChat::roomName();
    LLHandle<LLFloater> h = getHandle();
    LLFloaterAvatarPicker* picker = LLFloaterAvatarPicker::show(
        [h, group, channel, room](const uuid_vec_t& ids, const std::vector<LLAvatarName>& names)
        {
            // A private IM to each with a link that opens the room (the "wolfchat" handler below).
            const std::string link = "secondlife:///app/wolfchat/" + group.asString() + "/" + channel.asString();
            const std::string text = "Join me in the Wolf Chat voice room " + room + ": " + link;
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const std::string name = i < names.size() ? names[i].getCompleteName() : std::string();
                const LLUUID session = gIMMgr->addSession(name, IM_NOTHING_SPECIAL, ids[i]);
                if (session.notNull()) LLIMModel::sendMessage(text, session, ids[i], IM_NOTHING_SPECIAL);
            }
            if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get()))
            {
                self->setStatus(llformat("Invited %d %s. They need to be in the group to join.", (S32)ids.size(),
                                         ids.size() == 1 ? "person" : "people"), false);
            }
        }, true, true, true);
    if (picker) addDependentFloater(picker);
}

void WolfFloaterChat::askNewChannel(bool voice)
{
    if (!mCanManage) return;
    LLHandle<LLFloater> h = getHandle();
    const LLUUID group = mGroup;
    LLNotificationsUtil::add("WolfChatChannelName",
        LLSD().with("PROMPT", voice ? std::string("Name the new voice channel:") : std::string("Name the new text channel:")),
        LLSD(), [h, group, voice](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        std::string name = r["message"].asString();
        LLStringUtil::trim(name);
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return false;
        if (name.empty())
        {
            self->setStatus("The channel was not made: give it a name.", true);
            return false;
        }
        LLSD b;
        b["group"] = group;
        b["kind"] = voice ? "voice" : "text";
        b["name"] = name;
        WolfChat::api("channel_create", b, [h, voice](bool ok, const LLSD& reply, const std::string& error)
        {
            WolfFloaterChat* s = static_cast<WolfFloaterChat*>(h.get());
            if (!s) return;
            if (!ok) { s->setStatus("The channel was not made: " + error, true); return; }
            s->setStatus("Made " + std::string(voice ? "voice channel " : "#") + reply["channel"]["name"].asString() + ".", false);
            if (!voice) s->mPendingChannel = reply["channel"]["id"].asUUID();
            s->mBusyChannels = false;
            s->loadChannels();
        });
        return false;
    });
}

std::string WolfFloaterChat::channelName(const LLUUID& channel) const
{
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == channel) return c.mName;
    }
    return std::string();
}

void WolfFloaterChat::onRenameChannel(const LLUUID& channel)
{
    // [2026-10-21] Paul: "i can't rename channels" - only the header's Rename existed, for the text channel shown; voice
    // channels could not be renamed at all and an empty name was dropped silently. Now any channel (owners' right-click
    // menu or the header button), and an empty name says so.
    if (!mCanManage || channel.isNull() || channel == mGroup) return;
    std::string what = "this channel";
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == channel) what = c.mVoice ? "the voice channel " + c.mName : "#" + c.mName;
    }
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("WolfChatChannelName", LLSD().with("PROMPT", "New name for " + what + ":"), LLSD(),
        [h, channel](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        std::string name = r["message"].asString();
        LLStringUtil::trim(name);
        if (name.empty())
        {
            if (WolfFloaterChat* s = static_cast<WolfFloaterChat*>(h.get())) s->setStatus("The channel was not renamed: give it a name.", true);
            return false;
        }
        LLSD b;
        b["channel"] = channel;
        b["name"] = name;
        WolfChat::api("channel_rename", b, [h](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* s = static_cast<WolfFloaterChat*>(h.get());
            if (!s) return;
            if (!ok) { s->setStatus("The channel was not renamed: " + error, true); return; }
            s->setStatus("Channel renamed.", false);
            s->mBusyChannels = false;
            s->loadChannels();
        });
        return false;
    });
}

void WolfFloaterChat::onDeleteChannel(const LLUUID& channel)
{
    if (!mCanManage || channel.isNull() || channel == mGroup) return;
    const std::string name = channelName(channel);
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Delete #" + name + " and every message in it? This cannot be undone."), LLSD(),
        [h, channel](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        LLSD b;
        b["channel"] = channel;
        WolfChat::api("channel_delete", b, [h, channel](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* s = static_cast<WolfFloaterChat*>(h.get());
            if (!s) return;
            if (!ok) { s->setStatus("The channel was not deleted: " + error, true); return; }
            s->setStatus("Channel deleted.", false);
            if (s->mChannel == channel) s->mChannel.setNull();
            s->mBusyChannels = false;
            s->loadChannels();
        });
        return false;
    });
}

// ---------------------------------------------------------------------------------------------------- [2026-10-21]

// ---------------------------------------------------------------------------------------------------- [2026-10-21]
// #group-chat: the standard group IM session inside Wolf Chat

void WolfFloaterChat::pushRail()
{
    // The rail shows Wolf Chat's own unread / @ counts plus the group IM session's unread (llimview.h:312
    // getNumUnread, -1 when there is no session), so a group chat message marks its server too.
    std::vector<WolfChatServerRail::Server> shown = mServers;
    for (WolfChatServerRail::Server& sv : shown)
    {
        const S32 im = LLIMModel::instance().getNumUnread(sv.mId);
        if (im > 0 && !(sv.mId == mGroup && mGroupChatShown && hasFocus())) sv.mUnread += im;
    }
    mRail->setServers(shown);
}

bool WolfFloaterChat::joinGroupChat(std::string& why)
{
    // As LLGroupActions::startIM (llgroupactions.cpp:639-680): RLVa may forbid it; you must be in the group (group
    // data); opening the chat unmutes Firestorm's per-group chat mute (exoGroupMuteList::remove, :657); then the group
    // session (gIMMgr->addSession IM_SESSION_GROUP_START, :659-662) - which also joins it on the server.
    if (!RlvActions::canStartIM(mGroup))
    {
        why = "Group chat is blocked here by RLV restrictions.";
        return false;
    }
    if (LLMuteList::getInstance()->isMuted(mGroup))     // llmutelist.h:118 - the group itself is blocked
    {
        why = "You have blocked this group (People > Blocked), so its group chat is not shown.";
        return false;
    }
    LLGroupData data;
    if (!gAgent.getGroupData(mGroup, data))
    {
        why = "You are not in this group any more, so its group chat cannot be opened.";
        return false;
    }
    if (exoGroupMuteList::instance().isMuted(mGroup)) exoGroupMuteList::instance().remove(mGroup);
    if (!gIMMgr->hasSession(mGroup))
    {
        if (gIMMgr->addSession(data.mName, IM_SESSION_GROUP_START, mGroup).isNull())
        {
            why = "The group chat session could not be started - try again in a moment.";
            return false;
        }
    }
    return true;
}

void WolfFloaterChat::showGroupChat()
{
    mMessages.clear();
    mIMIndex = -1;
    std::string why;
    if (!joinGroupChat(why))
    {
        mLog->clear();
        setStatus(why, true);
        return;
    }
    if (hasFocus()) LLIMModel::getInstance()->setActiveSessionID(mGroup);   // no toast for what is on screen (llimview.cpp:530-534)
    appendGroupChat(true);
    setStatus("#group-chat is this group's standard group chat: everyone in the group sees it, in any viewer. "
              "Pictures, GIFs, pins, search and the bell are for the other channels.", false);
}

void WolfFloaterChat::appendGroupChat(bool rebuild)
{
    if (!mGroupChatShown || mInGroupAppend) return;
    // Belt and braces for the 07:09 crash: getMessages may raise IM signals that lead back here.
    mInGroupAppend = true;
    struct Reset { bool& b; ~Reset() { b = false; } } reset{ mInGroupAppend };
    // Source: fsfloaterim.cpp:1557-1590 FSFloaterIM::updateMessages - newest first from the model, from the index after
    // the last one shown; marked read only while this window has focus (as FSFloaterIM passes hasFocus()).
    std::list<LLSD> list;
    LLIMModel::instance().getMessages(mGroup, list, mIMIndex + 1, hasFocus());
    std::vector<Message> added;
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        const LLSD& msg = *it;
        Message m;
        m.mId = msg["index"].asInteger();
        m.mAgent = msg["from_id"].asUUID();
        m.mName = msg["from"].asString();
        m.mText = msg["message"].asString();
        m.mTimeText = msg["time"].asString();
        m.mChannel = mGroup;
        mIMIndex = std::max(mIMIndex, m.mId);
        if (m.mAgent.notNull() && LLMuteList::getInstance()->isMuted(m.mAgent)) continue;     // people you blocked
        added.push_back(m);
    }
    if (rebuild)
    {
        mMessages = added;
        rebuildLog();
    }
    else
    {
        for (const Message& m : added)
        {
            mMessages.push_back(m);
            appendToLog(m);
        }
    }
    refreshSidebar();
    pushRail();
}

void WolfFloaterChat::sendGroupChat(const std::string& text)
{
    if (!mCanPost)
    {
        setStatus("Your group role does not allow posting in this group's chat.", true);
        return;
    }
    if (!RlvActions::canSendIM(mGroup))
    {
        setStatus("Sending to this group chat is blocked by RLV restrictions.", true);
        return;
    }
    std::string why;
    if (!joinGroupChat(why))
    {
        setStatus("Not sent: " + why, true);
        return;
    }
    // Source: groupchatlistener.cpp:101 - LLIMModel::sendMessage(text, session id, group id, IM_SESSION_SEND); any
    // dialog but IM_NOTHING_SPECIAL goes out as IM_SESSION_SEND (llimview.cpp:2226-2232). The grid sends it back
    // to everyone in the session, us included, so it appears through the new-message signal (no local echo for
    // groups, llimview.cpp:2363-2369 echoes only IM_NOTHING_SPECIAL).
    LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(mGroup);
    if (session && session->mSessionInitialized)
        LLIMModel::sendMessage(text, mGroup, mGroup, IM_SESSION_SEND);
    else
        mGroupChatQueue.push_back(text);   // sent from draw() once the session is ready (fsfloaterim.cpp:623-630)
    mInput->setText(LLStringUtil::null);
    setStatus(std::string(), false);
}

void WolfFloaterChat::onIMNoUnread(const LLSD& msg)
{
    // Source: llimview.cpp:1850-1866 sendNoUnreadMessages - the session's unread count went to 0; only the badges change.
    const LLUUID session = msg["session_id"].asUUID();
    if (session.isNull()) return;
    bool ours = session == mGroup;
    for (const WolfChatServerRail::Server& sv : mServers) ours = ours || sv.mId == session;
    if (!ours) return;
    refreshSidebar();
    pushRail();
}

void WolfFloaterChat::onIMMessage(const LLSD& msg)
{
    const LLUUID session = msg["session_id"].asUUID();
    if (session.isNull()) return;
    if (session == mGroup && mGroupChatShown) appendGroupChat(false);
    else
    {
        bool ours = session == mGroup;
        for (const WolfChatServerRail::Server& sv : mServers) ours = ours || sv.mId == session;
        if (!ours) return;
        refreshSidebar();
        pushRail();
    }
}

void WolfFloaterChat::onFocusReceived()
{
    if (mGroupChatShown && mGroup.notNull() && LLIMModel::instance().findIMSession(mGroup))
    {
        LLIMModel::getInstance()->setActiveSessionID(mGroup);     // as fsfloaterim.cpp:216-218
        LLIMModel::getInstance()->sendNoUnreadMessages(mGroup);   // read now (llimview.h:250)
    }
    LLFloater::onFocusReceived();
}

void WolfFloaterChat::onFocusLost()
{
    if (mGroupChatShown && LLIMModel::getInstance()->getActiveSessionID() == mGroup)
        LLIMModel::getInstance()->resetActiveSessionID();         // as fsfloaterim.cpp:205-207
    LLFloater::onFocusLost();
}

// ---------------------------------------------------------------------------------------------------- [2026-10-21]
// owners: the channel menu and "Who can see this channel"

void WolfFloaterChat::showChannelMenu(const LLUUID& channel, S32 x, S32 y)
{
    if (!mCanManage || channel.isNull() || channel == mGroup) return;     // #group-chat has no menu
    mMenuChannel = channel;
    LLContextMenu* menu = static_cast<LLContextMenu*>(mChannelMenu.get());
    if (!menu)
    {
        // Source: llchiclet.cpp:252-275 LLIMWellChiclet::createMenu, as showServerMenu.
        LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
        registrar.add("WolfChatChannel.Action", [this](LLUICtrl*, const LLSD& param)
        {
            const std::string what = param.asString();
            if (what == "rename") onRenameChannel(mMenuChannel);
            else if (what == "roles") onChannelRoles(mMenuChannel);
            else if (what == "delete") onDeleteChannel(mMenuChannel);
            else if (what == "bridge") onBridge(mMenuChannel);
            else if (what == "unbridge") onUnbridge(mMenuChannel);
        });
        // [2026-10-21] The Discord items only for text channels: "Bridge" when not linked (or the link stopped), "Unlink"
        // when linked.
        LLUICtrl::EnableCallbackRegistry::ScopedRegistrar visible_registrar;
        visible_registrar.add("WolfChatChannel.Visible", [this](LLUICtrl*, const LLSD& param)
        {
            bool text_channel = false;
            for (const WolfChatSidebar::Channel& c : mChannels)
            {
                if (c.mId == mMenuChannel) text_channel = !c.mVoice && !c.mGroupChat;
            }
            if (!text_channel) return false;
            auto bl = mBridges.find(mMenuChannel);
            if (param.asString() == "bridge") return bl == mBridges.end() || bl->second.mState == "broken";
            return bl != mBridges.end();
        });
        menu = LLUICtrlFactory::getInstance()->createFromFile<LLContextMenu>("menu_wolf_chat_channel.xml",
                    LLMenuGL::sMenuContainer, LLViewerMenuHolderGL::child_registry_t::instance());
        if (!menu) return;
        mChannelMenu = menu->getHandle();
    }
    menu->show(x, y);
    LLMenuGL::showPopup(mSidebar, menu, x, y);
}

void WolfFloaterChat::onChannelRoles(const LLUUID& channel)
{
    if (!mCanManage || channel.isNull() || channel == mGroup) return;
    WolfFloaterChatChannelRoles* f = LLFloaterReg::getTypedInstance<WolfFloaterChatChannelRoles>("wolf_chat_channel_roles");
    if (!f) return;
    LLHandle<LLFloater> h = getHandle();
    std::string name = channelName(channel);
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == channel && !c.mVoice) name = "#" + name;
    }
    f->setup(channel, name, [h]()
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->setStatus("Saved who can see that channel.", false);
        self->mBusyChannels = false;
        self->loadChannels();
    });
    addDependentFloater(f->getHandle());
    f->openFloater();
    f->setFocus(true);
}

WolfFloaterChatChannelRoles::WolfFloaterChatChannelRoles(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterChatChannelRoles::postBuild()
{
    mWho = getChild<LLRadioGroup>("who");
    mRolesPanel = getChild<LLView>("roles_panel");
    mStatus = getChild<LLTextBox>("roles_status");
    mSaveBtn = getChild<LLButton>("save_btn");
    mWho->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateEnabled(); });
    mSaveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { save(); });
    getChild<LLButton>("cancel_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void WolfFloaterChatChannelRoles::setup(const LLUUID& channel, const std::string& name, std::function<void()> saved)
{
    mChannelId = channel;
    mChannelName = name;
    mSaved = saved;
}

void WolfFloaterChatChannelRoles::onOpen(const LLSD& key)
{
    getChild<LLTextBox>("roles_title")->setText("Who can see " + mChannelName + "?");
    load();
}

void WolfFloaterChatChannelRoles::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    mStatus->setColor(error ? C_ERROR : C_DIM);
}

void WolfFloaterChatChannelRoles::updateEnabled()
{
    // The role boxes are for "Only members with these roles"; the Owners box is always ticked and never changes.
    const bool some = mWho->getValue().asString() == "roles";
    for (auto& b : mBoxes) b.second->setEnabled(some && !mOwnerRole.count(b.first));
    mSaveBtn->setEnabled(!mBusy);
}

void WolfFloaterChatChannelRoles::load()
{
    for (auto& b : mBoxes)
    {
        mRolesPanel->removeChild(b.second);
        delete b.second;
    }
    mBoxes.clear();
    mOwnerRole.clear();
    mBusy = true;
    updateEnabled();
    setStatus("Loading the group's roles...", false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannelId;
    WolfChat::api("channel_roles", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChatChannelRoles* self = static_cast<WolfFloaterChatChannelRoles*>(h.get());
        if (!self) return;
        self->mBusy = false;
        if (!ok)
        {
            self->setStatus("The roles could not be loaded: " + error, true);
            self->updateEnabled();
            return;
        }
        std::set<LLUUID> allowed;
        for (const LLSD& r : llsd::inArray(reply["allowed"])) allowed.insert(r.asUUID());
        // One checkbox per role, top to bottom (Source: php/wolfchat.php channel_roles "roles": id, name, owners).
        const S32 row_h = 22;
        const S32 n = (S32)reply["roles"].size();
        const S32 width = self->mRolesPanel->getRect().getWidth();
        const S32 height = std::max(self->mRolesPanel->getRect().getHeight(), n * row_h + 4);
        self->mRolesPanel->reshape(width, height);
        S32 i = 0;
        for (const LLSD& r : llsd::inArray(reply["roles"]))
        {
            const bool owners = r["owners"].asInteger() != 0;
            const LLUUID id = r["id"].asUUID();
            LLCheckBoxCtrl::Params cp;
            cp.name("role_" + r["id"].asString());
            cp.label(r["name"].asString() + (owners ? " (always)" : ""));
            cp.rect(LLRect(4, height - i * row_h, width - 8, height - (i + 1) * row_h));
            cp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
            cp.tool_tip(owners ? std::string("Owners always see every channel.") : "Members with the " + r["name"].asString() + " role");
            LLCheckBoxCtrl* box = LLUICtrlFactory::create<LLCheckBoxCtrl>(cp);
            box->set(owners || allowed.count(id) > 0);
            self->mRolesPanel->addChild(box);
            self->mBoxes.push_back(std::make_pair(id, box));
            if (owners) self->mOwnerRole.insert(id);
            ++i;
        }
        self->mWho->setValue(LLSD(allowed.empty() ? "everyone" : "roles"));
        self->setStatus(std::string(), false);
        self->updateEnabled();
    });
}

void WolfFloaterChatChannelRoles::save()
{
    if (mBusy) return;
    LLSD roles = LLSD::emptyArray();
    if (mWho->getValue().asString() == "roles")
    {
        for (auto& b : mBoxes)
        {
            if (b.second->get() && !mOwnerRole.count(b.first)) roles.append(b.first);
        }
        if (roles.size() == 0)
        {
            setStatus("Tick at least one role, or choose \"Everyone in the group\".", true);
            return;
        }
    }
    mBusy = true;
    updateEnabled();
    setStatus("Saving...", false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannelId;
    b["roles"] = roles;
    WolfChat::api("channel_roles", b, [h](bool ok, const LLSD&, const std::string& error)
    {
        WolfFloaterChatChannelRoles* self = static_cast<WolfFloaterChatChannelRoles*>(h.get());
        if (!self) return;
        self->mBusy = false;
        self->updateEnabled();
        if (!ok)
        {
            self->setStatus("Not saved: " + error, true);
            return;
        }
        if (self->mSaved) self->mSaved();
        self->closeFloater();
    });
}

// ---------------------------------------------------------------------------------------------------- [2026-10-21]
// the Discord bridge (php/wolfbridge.php: links, link_code, unlink)

void WolfFloaterChat::loadBridges()
{
    if (mGroup.isNull() || mBusyBridges) return;
    mBusyBridges = true;
    mNextBridges = LLFrameTimer::getElapsedSeconds() + 60.0;    // links change rarely; after a change it is asked at once
    LLHandle<LLFloater> h = getHandle();
    const LLUUID group = mGroup;
    LLSD b;
    b["group"] = group;
    WolfChat::bridgeApi("links", b, [h, group](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyBridges = false;
        if (group != self->mGroup) { self->mNextBridges = 0.0; return; }
        if (!ok)
        {
            // In the background: no badge rather than a stale one; the bridge's own actions say what went wrong.
            LL_INFOS("WolfChat") << "Discord links could not be loaded: " << error << LL_ENDL;
            return;
        }
        // Source: php/wolfbridge.php links (contract 2026-10-21): links[{channel, state, reason, guild_name,
        // discord_channel_name, ...}], notice.
        self->mBridges.clear();
        for (const LLSD& l : llsd::inArray(reply["links"]))
        {
            BridgeLink link;
            link.mState = l["state"].asString();
            link.mReason = l["reason"].asString();
            link.mGuild = l["guild_name"].asString();
            link.mDiscordChannel = l["discord_channel_name"].asString();
            const LLUUID channel = l["channel"].asUUID();
            if (channel.notNull()) self->mBridges[channel] = link;
        }
        self->mBridgeNotice = reply["notice"].asString();
        self->refreshSidebar();
        self->updateHeader();
    });
}

void WolfFloaterChat::onBridge(const LLUUID& channel)
{
    if (!mCanManage || channel.isNull() || channel == mGroup) return;
    bool restricted = false, text_channel = false;
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId != channel) continue;
        restricted = c.mRestricted;
        text_channel = !c.mVoice;
    }
    if (!text_channel)
    {
        setStatus("Only text channels can be bridged to Discord.", true);
        return;
    }
    WolfFloaterChatBridge* f = LLFloaterReg::getTypedInstance<WolfFloaterChatBridge>("wolf_chat_bridge");
    if (!f) return;
    LLHandle<LLFloater> h = getHandle();
    f->setup(channel, "#" + channelName(channel), restricted, [h]()
    {
        if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get())) self->mNextBridges = 0.0;
    });
    addDependentFloater(f->getHandle());
    f->openFloater();
    f->setFocus(true);
}

void WolfFloaterChat::onUnbridge(const LLUUID& channel)
{
    if (!mCanManage || channel.isNull()) return;
    const std::string name = channelName(channel);
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Stop bridging #" + name + " to Discord? Messages stop going between the two. You can link it again later."),
        LLSD(), [h, channel, name](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        LLSD b;
        b["channel"] = channel;
        WolfChat::bridgeApi("unlink", b, [h, name](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
            if (!self) return;
            if (!ok) { self->setStatus("#" + name + " was not unlinked: " + error, true); return; }
            self->setStatus("#" + name + " is no longer bridged to Discord.", false);
            self->mNextBridges = 0.0;
        });
        return false;
    });
}

WolfFloaterChatBridge::WolfFloaterChatBridge(const LLSD& key)
:   LLFloater(key)
{
}

bool WolfFloaterChatBridge::postBuild()
{
    mCode = getChild<LLLineEditor>("bridge_code");
    mCommand = getChild<LLLineEditor>("bridge_command");
    mCountdown = getChild<LLTextBox>("bridge_countdown");
    mStatus = getChild<LLTextBox>("bridge_status");
    auto copy = [this](LLLineEditor* from, const std::string& what)
    {
        const std::string t = from->getText();
        if (t.empty()) { setStatus("There is nothing to copy yet.", true); return; }
        // Source: llclipboard.h:64 copyToClipboard(const LLWString&, pos, len).
        const LLWString w = utf8str_to_wstring(t);
        if (LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size())) setStatus(what + " copied.", false);
        else setStatus("Copying did not work - select the text and press Ctrl+C.", true);
    };
    getChild<LLButton>("copy_code_btn")->setClickedCallback([this, copy](LLUICtrl*, const LLSD&) { copy(mCode, "The code"); });
    getChild<LLButton>("copy_command_btn")->setClickedCallback([this, copy](LLUICtrl*, const LLSD&) { copy(mCommand, "The command"); });
    getChild<LLButton>("invite_btn")->setClickedCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURL(DISCORD_BOT_INVITE); });
    getChild<LLButton>("new_code_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { requestCode(); });
    getChild<LLButton>("close_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    getChild<LLTextBox>("invite_link")->setText(std::string(DISCORD_BOT_INVITE));
    return true;
}

void WolfFloaterChatBridge::setup(const LLUUID& channel, const std::string& name, bool restricted, std::function<void()> made)
{
    mChannelId = channel;
    mChannelName = name;
    mRestricted = restricted;
    mMade = made;
}

void WolfFloaterChatBridge::onOpen(const LLSD& key)
{
    getChild<LLTextBox>("bridge_title")->setText("Bridge " + mChannelName + " to Discord");
    // A channel only some roles can see would be readable by everyone in the Discord channel: say so before linking.
    getChild<LLView>("restricted_warning")->setVisible(mRestricted);
    requestCode();
}

void WolfFloaterChatBridge::setStatus(const std::string& msg, bool error)
{
    mStatus->setText(msg);
    mStatus->setColor(error ? C_ERROR : C_DIM);
}

void WolfFloaterChatBridge::requestCode()
{
    if (mBusy || mChannelId.isNull()) return;
    mBusy = true;
    mExpires = 0.0;
    mCode->setText(LLStringUtil::null);
    mCommand->setText(LLStringUtil::null);
    setStatus("Making a link code...", false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannelId;
    WolfChat::bridgeApi("link_code", b, [h](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChatBridge* self = static_cast<WolfFloaterChatBridge*>(h.get());
        if (!self) return;
        self->mBusy = false;
        if (!ok)
        {
            self->setStatus("No code: " + error, true);
            return;
        }
        // Source: php/wolfbridge.php link_code: {code, command, expires_in (600)}.
        self->mCode->setText(reply["code"].asString());
        self->mCommand->setText(reply["command"].asString());
        self->mExpires = LLFrameTimer::getElapsedSeconds() + (F64)std::max(0, reply["expires_in"].asInteger());
        self->setStatus("Do steps 1-3 below. This window can stay open while you do.", false);
        if (self->mMade) self->mMade();
    });
}

void WolfFloaterChatBridge::draw()
{
    if (mExpires > 0.0)
    {
        const S32 left = (S32)(mExpires - LLFrameTimer::getElapsedSeconds());
        if (left > 0)
        {
            mCountdown->setText(llformat("The code works for %d:%02d more.", left / 60, left % 60));
            mCountdown->setColor(C_DIM);
        }
        else
        {
            mCountdown->setText(LLStringExplicit("This code has expired. Press New code."));
            mCountdown->setColor(C_ERROR);
        }
    }
    else
    {
        mCountdown->setText(LLStringUtil::null);
    }
    LLFloater::draw();
}

void WolfFloaterChat::loadPins()
{
    if (mChannel.isNull()) return;
    if (mView == VIEW_PINS)
    {
        onBackToChannel();      // Pins again = back (the button is a toggle)
        return;
    }
    LLHandle<LLFloater> h = getHandle();
    const LLUUID channel = mChannel;
    LLSD b;
    b["channel"] = channel;
    setStatus("Loading the pinned messages...", false);
    WolfChat::api("pins", b, [h, channel](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self || self->mChannel != channel) return;
        if (!ok)
        {
            self->setStatus("The pinned messages could not be loaded: " + error, true);
            self->updateHeader();
            return;
        }
        self->mResults.clear();
        for (const LLSD& m : llsd::inArray(reply["messages"])) self->mResults.push_back(messageFromLLSD(m));
        self->mView = VIEW_PINS;
        self->mBackBtn->setLabel(LLStringExplicit("Back to the channel"));
        self->mBackBtn->setVisible(true);
        self->mOlderBtn->setVisible(false);
        self->rebuildLog();
        self->updateHeader();
        self->refreshSidebar();
        self->setStatus(self->mResults.empty() ? std::string() : llformat("%d pinned (newest pin first).", (S32)self->mResults.size()), false);
    });
}

void WolfFloaterChat::showAround(const LLUUID& channel, S32 message_id)
{
    // The messages around the pinned one (php/wolfchat.php messages "around"); new messages are not added while
    // this is shown - "Back to the newest" returns to the live channel.
    mChannel = channel;
    mView = VIEW_CHANNEL;
    mHistory = true;
    mHighlight = message_id;
    ++mGeneration;
    mMessages.clear();
    mLog->clear();
    mOlderBtn->setVisible(false);
    mBackBtn->setLabel(LLStringExplicit("Back to the newest"));
    mBackBtn->setVisible(true);
    updateHeader();
    refreshSidebar();
    setStatus("Finding the message...", false);
    LLHandle<LLFloater> h = getHandle();
    const S32 gen = mGeneration;
    LLSD b;
    b["channel"] = channel;
    b["around"] = message_id;
    WolfChat::api("messages", b, [h, gen](bool ok, const LLSD& reply, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self || gen != self->mGeneration) return;
        if (!ok)
        {
            self->setStatus("That message could not be shown: " + error, true);
            return;
        }
        self->mMessages.clear();
        for (const LLSD& m : llsd::inArray(reply["messages"])) self->mMessages.push_back(messageFromLLSD(m));
        self->rebuildLog();
        self->setStatus("Showing the pinned message (marked >>) where it is. \"Back to the newest\" returns to the channel.", false);
    });
}

void WolfFloaterChat::setPinned(S32 id, bool pin)
{
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["id"] = id;
    WolfChat::api(pin ? "pin" : "unpin", b, [h, id, pin](bool ok, const LLSD&, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        if (!ok)
        {
            self->setStatus(std::string(pin ? "The message was not pinned: " : "The message was not unpinned: ") + error, true);
            return;
        }
        for (Message& m : self->mMessages)
        {
            if (m.mId == id) m.mPinned = pin;
        }
        self->rebuildLog();
        self->setStatus(pin ? "Pinned. Everyone in the group finds it under Pins." : "Unpinned.", false);
    });
}

void WolfFloaterChat::onNotifySetting()
{
    if (mChannel.isNull()) return;
    std::string name;
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == mChannel) name = c.mName;
    }
    const std::string now = mNotifyOf.count(mChannel) ? mNotifyOf[mChannel] : std::string("mentions");
    LLSD args;
    args["CHANNEL"] = name;
    args["GROUP"] = mGroupName;
    args["NOW"] = now == "all" ? "All messages" : now == "none" ? "Nothing" : "Only @mentions";
    LLHandle<LLFloater> h = getHandle();
    const LLUUID channel = mChannel;
    LLNotificationsUtil::add("WolfChatNotifySetting", args, LLSD(), [h, channel](const LLSD& n, const LLSD& r)
    {
        // notifications.xml WolfChatNotifySetting: 0 all, 1 mentions, 2 none, 3 cancel.
        const S32 option = LLNotificationsUtil::getSelectedOption(n, r);
        if (option < 0 || option > 2) return false;
        const std::string kind = option == 0 ? "all" : option == 1 ? "mentions" : "none";
        LLSD b;
        b["channel"] = channel;
        b["notify"] = kind;
        WolfChat::api("notify_set", b, [h, channel, kind](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
            if (!self) return;
            if (!ok)
            {
                self->setStatus("The notification setting was not changed: " + error, true);
                return;
            }
            self->mNotifyOf[channel] = kind;
            for (WolfChatSidebar::Channel& c : self->mChannels)
            {
                if (c.mId == channel) c.mMuted = kind == "none";
            }
            self->updateHeader();
            self->refreshSidebar();
            self->setStatus(kind == "all" ? "You will be told about every new message here while Wolf Chat is closed."
                            : kind == "none" ? "You will not be told about new messages here."
                                             : "You will be told only about messages here that mention you.", false);
        });
        return false;
    });
}

void WolfFloaterChat::onPickGif()
{
    if (!mGifsAvailable) return;
    if (mChannel.isNull() || mView != VIEW_CHANNEL)
    {
        setStatus("Pick a text channel on the left first, then the GIF.", true);
        return;
    }
    WolfFloaterChatGif* picker = LLFloaterReg::getTypedInstance<WolfFloaterChatGif>("wolf_chat_gif");
    if (!picker) return;
    LLHandle<LLFloater> h = getHandle();
    picker->setPickCallback([h](const std::string& id, const std::string& title)
    {
        if (WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get())) self->postGif(id, title);
    });
    mGifPicker = picker->getHandle();
    addDependentFloater(mGifPicker);
    picker->openFloater();
}

void WolfFloaterChat::postGif(const std::string& id, const std::string& title)
{
    if (mChannel.isNull() || mView != VIEW_CHANNEL)
    {
        setStatus("Pick a text channel on the left first, then the GIF.", true);
        return;
    }
    if (mBusyPost) return;
    std::string words = mInput->getText();
    LLStringUtil::trim(words);
    mBusyPost = true;
    // The first time anyone posts a GIF, the grid makes it into a texture (a few seconds).
    setStatus("Posting the GIF...", false);
    LLHandle<LLFloater> h = getHandle();
    LLSD b;
    b["channel"] = mChannel;
    b["gif"] = id;
    b["text"] = words;      // what is typed goes with the GIF
    WolfChat::api("gif_post", b, [h, words](bool ok, const LLSD&, const std::string& error)
    {
        WolfFloaterChat* self = static_cast<WolfFloaterChat*>(h.get());
        if (!self) return;
        self->mBusyPost = false;
        if (!ok)
        {
            self->setStatus("The GIF was not posted: " + error, true);
            return;
        }
        if (!words.empty()) self->mInput->setText(LLStringUtil::null);
        self->setStatus(std::string(), false);
        self->mNextMessages = 0.0;
    });
}

void WolfFloaterChat::setCards(const LLSD& cards)
{
    mCardData = cards.isArray() ? cards : LLSD::emptyArray();
    std::vector<WolfChatCards::Card> list;
    for (const LLSD& c : llsd::inArray(mCardData))
    {
        WolfChatCards::Card card;
        card.mId = c["id"].asInteger();
        card.mTitle = c["title"].asString();
        card.mSubtitle = c["subtitle"].asString();
        card.mIcon = c["icon"].asUUID();
        card.mLink = c["link"].asString();
        card.mTag = c["tag"].asString();
        if (card.mId > 0 && !card.mTitle.empty()) list.push_back(card);
    }
    mCards->setCards(list);
    setCardsShown(!list.empty());
}

void WolfFloaterChat::setCardsShown(bool shown)
{
    // The cards take a strip above the status line; the message log gives up that height while they are there.
    if (shown == mCardsShown) return;
    mCardsShown = shown;
    mCards->setVisible(shown);
    const S32 strip = mCards->getRect().getHeight() + 4;
    LLRect r = mLog->getRect();
    r.mBottom += shown ? strip : -strip;
    mLog->setShape(r);
}

void WolfFloaterChat::setGifsAvailable(bool available)
{
    // No key / tools on the grid = no GIF button at all (never a button that cannot work). The message box moves
    // right to make room for it.
    if (available == mGifsAvailable) return;
    mGifsAvailable = available;
    mGifBtn->setVisible(available);
    const S32 room = mGifBtn->getRect().getWidth() + 4;
    LLRect r = mInput->getRect();
    r.mLeft += available ? room : -room;
    mInput->setShape(r);
}

// =====================================================================================================================
// a picture, big
// =====================================================================================================================

WolfFloaterChatImage::WolfFloaterChatImage(const LLSD& key)
:   LLFloater(key),
    mImageId(key.isMap() ? key["asset"].asUUID() : key.asUUID()),
    mGif(WolfChat::GifSheet::fromLLSD(key))       // [2026-10-21] a GIF's key is its sheet (WolfChatImage::handleMouseDown)
{
}

WolfFloaterChatImage::~WolfFloaterChatImage()
{
    if (mSheet.notNull()) mSheet->forceActive();
}

bool WolfFloaterChatImage::postBuild()
{
    if (mGif.valid())
    {
        mSheet = LLViewerTextureManager::getFetchedTexture(mGif.mAsset, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
        if (mSheet.notNull()) mSheet->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
        setTitle(mGif.mTitle.empty() ? std::string("GIF") : "GIF - " + mGif.mTitle);
        setToolTip(std::string("Click to pause or play the GIF."));
        return true;
    }
    mImage = LLUI::getUIImageByID(mImageId, LLGLTexture::BOOST_PREVIEW);
    return true;
}

bool WolfFloaterChatImage::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // [2026-10-21] A moving picture can be stopped (WCAG 2.2.2): click pauses on the frame shown, click again plays.
    if (mGif.valid() && y < getRect().getHeight() - getHeaderHeight())
    {
        mPaused = !mPaused;
        mPausedFrame = mGif.frameNow();
        return true;
    }
    return LLFloater::handleMouseDown(x, y, mask);
}

void WolfFloaterChatImage::draw()
{
    LLFloater::draw();
    const LLRect r = getLocalRect();
    const S32 top = r.getHeight() - getHeaderHeight() - 4;
    const S32 w = r.getWidth() - 8, h = top - 4;
    if (w <= 0 || h <= 0) return;
    if (mGif.valid() && mSheet.notNull())
    {
        mSheet->addTextureStats((F32)(mGif.mSheetW * mGif.mSheetH));
        if (!mSheet->hasGLTexture()) return;
        const F32 s = std::min((F32)w / (F32)mGif.mCellW, (F32)h / (F32)mGif.mCellH);
        const S32 dw = std::max(1, (S32)((F32)mGif.mCellW * s));
        const S32 dh = std::max(1, (S32)((F32)mGif.mCellH * s));
        gl_draw_scaled_image(4 + (w - dw) / 2, 4 + (h - dh) / 2, dw, dh, mSheet, UI_VERTEX_COLOR,
                             mGif.frameUV(mPaused ? mPausedFrame : mGif.frameNow()));
        if (mPaused) text(fontBold(), "Paused - click to play", (F32)(4 + w / 2), 16.f, C_TEXT, LLFontGL::HCENTER);
        return;
    }
    if (mImage.isNull()) return;
    S32 iw = 0, ih = 0;
    if (LLGLTexture* gl = dynamic_cast<LLGLTexture*>(mImage->getImage().get()))
    {
        iw = gl->getFullWidth();
        ih = gl->getFullHeight();
    }
    S32 dw = w, dh = h;
    if (iw > 0 && ih > 0)
    {
        const F32 s = std::min((F32)w / (F32)iw, (F32)h / (F32)ih);
        dw = std::max(1, (S32)((F32)iw * s));
        dh = std::max(1, (S32)((F32)ih * s));
    }
    mImage->draw(4 + (w - dw) / 2, 4 + (h - dh) / 2, dw, dh);
}

// =====================================================================================================================
// secondlife:///app/wolfchat/<group>/<channel> - the link Invite People sends
// =====================================================================================================================

class WolfChatHandler : public LLCommandHandler
{
public:
    // A link in chat: opening a window is harmless, but only when clicked (never from a web page by itself).
    WolfChatHandler() : LLCommandHandler("wolfchat", UNTRUSTED_CLICK_ONLY) {}

    bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web) override
    {
        if (LLStartUp::getStartupState() < STATE_STARTED) return true;
        if (params.size() < 1) return false;
        LLUUID group, channel;
        if (!group.set(params[0].asString(), false)) return false;
        if (params.size() >= 2) channel.set(params[1].asString(), false);
        if (!WolfGrid::isOnWolfTerritories())
        {
            WolfGrid::noteWolfOnly("Wolf Chat");
            return true;
        }
        WolfChat::openChannel(group, channel);
        return true;
    }
};
WolfChatHandler gWolfChatHandler;
