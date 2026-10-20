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
#include "llchatentry.h"
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
#include "llinventorymodel.h"
#include "lllayoutstack.h"
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
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerregion.h"
#include "llvoiceclient.h"
#include "llwindow.h"
#include "wolfgrid.h"

static LLDefaultChildRegistry::Register<WolfChatSidebar> r_wolf_chat_sidebar("wolf_chat_sidebar");

namespace
{
    // php/wolfchat.php on the WolfStorm web hosts - the host every WolfViewer call to a wolfstorm/php service uses
    // (wolfadventure.cpp API_URL, wolfcave.cpp WolfCaves::API_URL).
    const char* const API_URL = "https://wolfstorm.app/php/wolfchat.php";

    // How often the open window asks for news (seconds). Messages of the channel you look at, the channels with
    // who is in which voice room, the members online; the voice heartbeat matches php/wolfchat.php WCH_VOICE_FRESH.
    const F64 POLL_MESSAGES = 3.0;
    const F64 POLL_CHANNELS = 10.0;
    const F64 POLL_ONLINE = 30.0;
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
    class WolfChatHeartbeat : public LLEventTimer
    {
    public:
        WolfChatHeartbeat() : LLEventTimer(VOICE_HEARTBEAT) {}
        bool tick() override
        {
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
    };
    WolfChatHeartbeat* sHeartbeat = nullptr;

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
    LLCoros::instance().launch("WolfChat " + action, [text, done]()
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
        LLSD result = adapter->postRawAndSuspend(request, API_URL, raw, options, headers);
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
        done(true, std::string());
    });
}

void WolfChat::leaveRoom()
{
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
                text(selected ? fontBold() : font(), c.mName, (F32)(PAD + ICON + 8), cy, selected ? C_ACCENT : C_TEXT,
                     LLFontGL::LEFT, w - PAD * 3 - ICON);
            }
            else
            {
                text(fontBold(), "#", (F32)(PAD + 4), cy, C_DIM);
                const bool bold = selected || c.mUnread;
                text(bold ? fontBold() : font(), c.mName, (F32)(PAD + ICON + 8), cy, (selected || c.mUnread) ? C_TEXT : C_DIM,
                     LLFontGL::LEFT, w - PAD * 4 - ICON - 10);
                if (c.mUnread)
                {
                    // Unread: a dot AND the bold name (never colour alone).
                    gGL.color4fv(C_TEXT.mV);
                    gl_circle_2d((F32)(w - PAD - 6), cy, 4.f, 16, true);
                }
            }
            continue;
        }
        if (r.mHit == HIT_PERSON)
        {
            const Person& p = mChannels[r.mChannel].mPeople[r.mPerson];
            if (mHover == i) gl_rect_2d(PAD / 2, top, w - PAD / 2, bottom, C_ROW_HOVER, true);
            const S32 x0 = PAD + ICON + 8;
            // Speaking: a lime ring; the name stays the same colour so it is never the only sign.
            if (p.mSpeaking) gl_rect_2d(x0 - 6, top - 3, x0 - 3, bottom + 3, C_ACCENT, true);
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
    else if (r->mHit == HIT_CHANNEL) tip = mChannels[r->mChannel].mVoice ? "Join this voice room" : "Read this channel";
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

void WolfChatImage::draw()
{
    const S32 w = getRect().getWidth();
    const S32 h = getRect().getHeight();
    gl_rect_2d(0, h, w, 0, C_SKELETON, true);
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
        dw = std::max(1, (S32)(iw * s));
        dh = std::max(1, (S32)(ih * s));
    }
    mImage->draw((w - dw) / 2, (h - dh) / 2, dw, dh);
}

bool WolfChatImage::handleMouseDown(S32 x, S32 y, MASK mask)
{
    LLFloaterReg::showInstance("wolf_chat_image", LLSD(mImageId), true);
    return true;
}

bool WolfChatImage::handleHover(S32 x, S32 y, MASK mask)
{
    getWindow()->setCursor(UI_CURSOR_HAND);
    return true;
}

// =====================================================================================================================
// the window
// =====================================================================================================================

WolfFloaterChat::WolfFloaterChat(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterChat::~WolfFloaterChat()
{
    if (LLFloater* p = mTexPicker.get()) p->closeFloater();
}

bool WolfFloaterChat::postBuild()
{
    mGroupCombo = getChild<LLComboBox>("group_combo");
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
    mDeleteBtn = getChild<LLButton>("delete_btn");
    mMembersPanel = getChild<LLView>("members_column");

    mGroupCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { selectGroup(mGroupCombo->getValue().asUUID()); });
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
    mRenameBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRenameChannel(); });
    mDeleteBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onDeleteChannel(); });
    mOnlineList->setDoubleClickCallback([this]()
    {
        LLScrollListItem* item = mOnlineList->getFirstSelected();
        if (item) LLAvatarActions::showProfile(item->getValue().asUUID());
    });

    getChild<LLButton>("members_btn")->setToggleState(true);
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
        mPendingChannel = key["channel"].asUUID();
        const LLUUID g = key["group"].asUUID();
        if (mGroupCombo->getItemCount() > 0)
        {
            if (mGroupCombo->selectByValue(LLSD(g))) selectGroup(g);
            else setStatus("You are not in the group that link is for, so its channels cannot be shown.", true);
            return;
        }
        mGroup = g;     // picked once the groups are in (loadGroups)
    }
    if (mGroupCombo->getItemCount() == 0) loadGroups();
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
        self->mGroupCombo->removeall();
        for (const LLSD& g : llsd::inArray(reply["groups"]))
        {
            self->mGroupCombo->add(g["name"].asString(), LLSD(g["id"].asUUID()));
        }
        if (self->mGroupCombo->getItemCount() == 0)
        {
            self->mSidebar->setChannels({}, false);
            self->setStatus("You are not in any group yet. Join or make a group (Communicate > Groups), "
                            "then open Wolf Chat again to talk in its channels.", false);
            return;
        }
        LLUUID want = self->mGroup;
        if (want.isNull() || !self->mGroupCombo->selectByValue(LLSD(want)))
        {
            // Your active group first, as the viewer's own group chat does; else the first in the list.
            if (!self->mGroupCombo->selectByValue(LLSD(gAgent.getGroupID()))) self->mGroupCombo->selectFirstItem();
        }
        self->setStatus(std::string(), false);
        self->selectGroup(self->mGroupCombo->getValue().asUUID());
    });
}

void WolfFloaterChat::selectGroup(const LLUUID& group)
{
    if (group.isNull()) return;
    if (group != mGroup || mChannels.empty())
    {
        mGroup = group;
        mGroupName = mGroupCombo->getSelectedItemLabel();
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
        self->mCanManage = reply["can_manage"].asInteger() != 0;
        self->mChannels.clear();
        for (const LLSD& c : llsd::inArray(reply["channels"]))
        {
            WolfChatSidebar::Channel ch;
            ch.mId = c["id"].asUUID();
            ch.mName = c["name"].asString();
            ch.mVoice = c["kind"].asString() == "voice";
            if (!ch.mVoice) self->mLastId[ch.mId] = c["last_id"].asInteger();
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
            for (const WolfChatSidebar::Channel& c : self->mChannels)
            {
                if (!c.mVoice) { show = c.mId; break; }
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
    if (mChannel.isNull() || mView != VIEW_CHANNEL) return;
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
            Message msg;
            msg.mId = m["id"].asInteger();
            msg.mAgent = m["agent"].asUUID();
            msg.mName = m["name"].asString();
            msg.mText = m["text"].asString();
            msg.mImage = m["image"].asUUID();
            msg.mImageName = m["image_name"].asString();
            msg.mTime = m["time"].asInteger();
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
    if (mChannel.isNull() || mMessages.empty() || mBusyMessages) return;
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
        for (const LLSD& m : llsd::inArray(reply["messages"]))
        {
            Message msg;
            msg.mId = m["id"].asInteger();
            msg.mAgent = m["agent"].asUUID();
            msg.mName = m["name"].asString();
            msg.mText = m["text"].asString();
            msg.mImage = m["image"].asUUID();
            msg.mImageName = m["image_name"].asString();
            msg.mTime = m["time"].asInteger();
            older.push_back(msg);
        }
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
            self->mOnlineTitle->setText(LLStringExplicit("USERS ONLINE - could not load"));
            self->mOnlineTitle->setToolTip(error);
            return;
        }
        const LLUUID keep = self->mOnlineList->getSelectedValue().asUUID();
        self->mOnlineList->deleteAllItems();
        S32 n = 0;
        for (const LLSD& p : llsd::inArray(reply["online"]))
        {
            self->mOnlineList->addSimpleElement(p["name"].asString(), ADD_BOTTOM, LLSD(p["agent"].asUUID()));
            ++n;
        }
        if (keep.notNull()) self->mOnlineList->selectByValue(LLSD(keep));
        self->mOnlineTitle->setText(llformat("USERS ONLINE - %d", n));
        self->mOnlineTitle->setToolTip(std::string());
        if (n == 0) self->mOnlineList->setCommentText("No one else from this group is online.");
    });
}

// ---------------------------------------------------------------------------------------------------- showing

void WolfFloaterChat::showChannel(const LLUUID& channel)
{
    mChannel = channel;
    mView = VIEW_CHANNEL;
    ++mGeneration;
    mMessages.clear();
    mLog->clear();
    mBackBtn->setVisible(false);
    mOlderBtn->setVisible(false);
    if (channel.notNull()) loadMessages(false);
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
        else if (mChannel.notNull()) mLog->appendText("No messages yet. Say hello - everyone in the group can read this channel.", false, dim);
        return;
    }
    for (const Message& m : list) appendToLog(m);
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
    mLog->appendText("   " + clockText(m.mTime) + "  ", false, when);

    // Delete: your own message, or anyone's for a group moderator.
    const bool mine = m.mAgent == gAgentID;
    if (mView == VIEW_CHANNEL && (mine || mCanManage))
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

    // The words (links in them work: the log parses URLs and SLURLs).
    if (!m.mText.empty())
    {
        LLStyle::Params body;
        body.color(C_TEXT);
        body.readonly_color(C_TEXT);
        mLog->appendText(m.mText, true, body);
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
    else
    {
        for (const WolfChatSidebar::Channel& c : mChannels)
        {
            if (c.mId == mChannel) title = "# " + c.mName;
        }
    }
    mChannelTitle->setText(title);
    const bool channel_view = mView == VIEW_CHANNEL && mChannel.notNull();
    mRenameBtn->setVisible(channel_view && mCanManage);
    mDeleteBtn->setVisible(channel_view && mCanManage);
    const bool can_write = channel_view && mCanPost;
    mInput->setEnabled(can_write);
    mSendBtn->setEnabled(can_write);
    mImageBtn->setEnabled(can_write);
    mEmojiBtn->setEnabled(can_write);
    if (channel_view && !mCanPost)
        mInput->setLabel(LLStringExplicit("Your group role does not allow posting in this group's chat."));
    else
        mInput->setLabel(LLStringExplicit(mChannel.notNull() ? "Message in #" + title.substr(std::min<size_t>(2, title.size())) + "..." : std::string()));
}

void WolfFloaterChat::refreshSidebar()
{
    std::vector<WolfChatSidebar::Channel> shown = mChannels;
    const LLUUID room = WolfChat::roomChannel();
    for (WolfChatSidebar::Channel& c : shown)
    {
        if (!c.mVoice)
        {
            c.mUnread = c.mId != mChannel && mLastId[c.mId] > mSeen[c.mId];
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
            p.mSpeaking = LLVoiceClient::getInstance()->getIsSpeaking(id);
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
    const bool in = WolfChat::inRoom();
    std::string where;
    if (in)
    {
        where = "Voice connected: " + WolfChat::roomName();
        if (WolfChat::roomGroup() != mGroup) where += " (another group)";
    }
    else
    {
        where = "Not in a voice room. Click a voice channel to join it.";
    }
    mVoiceStatus->setText(where);
    mVoiceStatus->setColor(in ? C_OK : C_DIM);
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
    // Voice state changes without asking us (the toolbar's Speak, another call, a moderator): read it each frame.
    static S32 frame = 0;
    if (++frame % 10 == 0)
    {
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
    if (WolfChat::roomChannel() == channel)
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
    if (cargo_type == DAD_TEXTURE && mView == VIEW_CHANNEL && mChannel.notNull() && mCanPost)
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
        for (const LLSD& m : llsd::inArray(reply["messages"]))
        {
            Message msg;
            msg.mId = m["id"].asInteger();
            msg.mAgent = m["agent"].asUUID();
            msg.mName = m["name"].asString();
            msg.mText = m["text"].asString();
            msg.mImage = m["image"].asUUID();
            msg.mImageName = m["image_name"].asString();
            msg.mTime = m["time"].asInteger();
            msg.mChannel = m["channel"].asUUID();
            self->mResults.push_back(msg);
        }
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
        for (const LLSD& m : llsd::inArray(reply["messages"]))
        {
            Message msg;
            msg.mId = m["id"].asInteger();
            msg.mAgent = m["agent"].asUUID();
            msg.mName = m["name"].asString();
            msg.mText = m["text"].asString();
            msg.mImage = m["image"].asUUID();
            msg.mImageName = m["image_name"].asString();
            msg.mTime = m["time"].asInteger();
            msg.mChannel = m["channel"].asUUID();
            self->mResults.push_back(msg);
        }
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
        "- Pick a group at the top left. Every group you are in has channels.\n"
        "- # text channels: read and post. Pictures: the picture button, or drag a texture or snapshot from your "
        "inventory onto the window (it must be copy and transfer).\n"
        "- Voice channels: click one to join the voice room. Mic turns your microphone on and off; Deafen stops all "
        "voice; Leave Call takes you back to the voice where you stand; Invite People sends a link to the room.\n"
        "- Group moderators (owners, and roles with \"Moderate group chat\") can add (+), rename and delete channels "
        "and delete messages.\n"
        "- Users shows who in the group is online; double-click someone for their profile."));
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

void WolfFloaterChat::onRenameChannel()
{
    if (!mCanManage || mChannel.isNull()) return;
    LLHandle<LLFloater> h = getHandle();
    const LLUUID channel = mChannel;
    LLNotificationsUtil::add("WolfChatChannelName", LLSD().with("PROMPT", std::string("New name for this channel:")), LLSD(),
        [h, channel](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        std::string name = r["message"].asString();
        LLStringUtil::trim(name);
        if (name.empty()) return false;
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

void WolfFloaterChat::onDeleteChannel()
{
    if (!mCanManage || mChannel.isNull()) return;
    std::string name;
    for (const WolfChatSidebar::Channel& c : mChannels)
    {
        if (c.mId == mChannel) name = c.mName;
    }
    LLHandle<LLFloater> h = getHandle();
    const LLUUID channel = mChannel;
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Delete #" + name + " and every message in it? This cannot be undone."), LLSD(),
        [h, channel](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return false;
        LLSD b;
        b["channel"] = channel;
        WolfChat::api("channel_delete", b, [h](bool ok, const LLSD&, const std::string& error)
        {
            WolfFloaterChat* s = static_cast<WolfFloaterChat*>(h.get());
            if (!s) return;
            if (!ok) { s->setStatus("The channel was not deleted: " + error, true); return; }
            s->setStatus("Channel deleted.", false);
            s->mChannel.setNull();
            s->mBusyChannels = false;
            s->loadChannels();
        });
        return false;
    });
}

// =====================================================================================================================
// a picture, big
// =====================================================================================================================

WolfFloaterChatImage::WolfFloaterChatImage(const LLSD& key)
:   LLFloater(key),
    mImageId(key.asUUID())
{
}

bool WolfFloaterChatImage::postBuild()
{
    mImage = LLUI::getUIImageByID(mImageId, LLGLTexture::BOOST_PREVIEW);
    return true;
}

void WolfFloaterChatImage::draw()
{
    LLFloater::draw();
    if (mImage.isNull()) return;
    const LLRect r = getLocalRect();
    const S32 top = r.getHeight() - getHeaderHeight() - 4;
    const S32 w = r.getWidth() - 8, h = top - 4;
    if (w <= 0 || h <= 0) return;
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
        dw = std::max(1, (S32)(iw * s));
        dh = std::max(1, (S32)(ih * s));
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
