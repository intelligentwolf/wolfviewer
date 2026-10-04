/**
 * @file wolffloaterdj.cpp
 * @brief WolfViewer: Wolf DJ / Talk Show floater. See wolffloaterdj.h and wolfdjaudio.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterdj.h"

#include "wolfdjcapture.h"
#include "wolfdjplayer.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llparcel.h"
#include "llsecapi.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llvieweraudio.h"
#include "llviewermedia.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "llrender.h"
#include "llui.h"
#include "roles_constants.h"

#include <boost/json.hpp>

using namespace WolfDJ;

namespace
{
    const char* STRIP_KEYS[CH_COUNT] = { "voice", "mic", "music_a", "music_b" };
    const char* MY_STREAM_PAGE = "https://www.wolf-grid.com/index.php?f=mystream";
    const char* PROTECTED_TYPE = "wolf_dj";

    // A show in progress: what to put back afterwards. Survives the floater being closed.
    struct Show
    {
        bool mLandSet = false;
        std::string mPrevMusicUrl;
        S32 mParcelLocalId = 0;
        LLUUID mRegionId;
        bool mMusicSilenced = false;
        bool mPrevStreamingMusic = true;
        std::string mListenUrl;
        std::string mStatusHost;
        int mStatusPort = 0;
        std::string mMount;
    };
    Show sShow;

    std::string password_id()
    {
        return gAgentID.asString();
    }

    // Put the land and the viewer's own parcel music back as they were.
    void end_show(std::string* note)
    {
        if (sShow.mLandSet)
        {
            LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            LLViewerRegion* region = gAgent.getRegion();
            if (parcel && region && region->getRegionID() == sShow.mRegionId && parcel->getLocalID() == sShow.mParcelLocalId)
            {
                parcel->setMusicURL(sShow.mPrevMusicUrl);
                LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate(parcel);
            }
            else if (note)
            {
                *note = "You have left the land you were playing on, so its music could not be put back to "
                        + (sShow.mPrevMusicUrl.empty() ? std::string("none") : sShow.mPrevMusicUrl) + ".";
            }
            sShow.mLandSet = false;
        }
        if (sShow.mMusicSilenced)
        {
            gSavedSettings.setBOOL("AudioStreamingMusic", sShow.mPrevStreamingMusic);
            if (sShow.mPrevStreamingMusic)
            {
                LLViewerAudio::getInstance()->startInternetStreamWithAutoFade(LLViewerMedia::getInstance()->getParcelAudioURL());
            }
            sShow.mMusicSilenced = false;
        }
    }
}

WolfFloaterDJ::WolfFloaterDJ(const LLSD& key) : LLFloater(key)
{
}

WolfFloaterDJ::~WolfFloaterDJ()
{
}

bool WolfFloaterDJ::postBuild()
{
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        const std::string k = STRIP_KEYS[ch];
        Strip& s = mStrips[ch];
        s.mFader = getChild<LLSliderCtrl>("fader_" + k);
        s.mEqHigh = getChild<LLSliderCtrl>("eq_high_" + k);
        s.mEqMid = getChild<LLSliderCtrl>("eq_mid_" + k);
        s.mEqLow = getChild<LLSliderCtrl>("eq_low_" + k);
        s.mMute = getChild<LLButton>("mute_" + k);
        s.mCue = getChild<LLButton>("cue_" + k);
        s.mMeter = getChild<LLView>("meter_" + k);
        s.mDb = getChild<LLTextBox>("db_" + k);
        WolfDJChannel* chan = &WolfDJMixer::instance().channel(ch);
        s.mFader->setCommitCallback([this, chan](LLUICtrl* c, const LLSD&) { chan->mFader = (F32)c->getValue().asReal(); saveLevels(); });
        s.mEqHigh->setCommitCallback([this, chan](LLUICtrl* c, const LLSD&) { chan->mEqHighDb = (F32)c->getValue().asReal(); saveLevels(); });
        s.mEqMid->setCommitCallback([this, chan](LLUICtrl* c, const LLSD&) { chan->mEqMidDb = (F32)c->getValue().asReal(); saveLevels(); });
        s.mEqLow->setCommitCallback([this, chan](LLUICtrl* c, const LLSD&) { chan->mEqLowDb = (F32)c->getValue().asReal(); saveLevels(); });
        s.mMute->setCommitCallback([this, chan](LLUICtrl* c, const LLSD&) { chan->mMute = c->getValue().asBoolean(); saveLevels(); });
        s.mCue->setCommitCallback([this, ch](LLUICtrl*, const LLSD&) { onCue(ch); });
    }
    mMaster.mFader = getChild<LLSliderCtrl>("fader_master");
    mMaster.mCue = getChild<LLButton>("cue_master");
    mMaster.mMeter = getChild<LLView>("meter_master");
    mMaster.mDb = getChild<LLTextBox>("db_master");
    mMaster.mFader->setCommitCallback([this](LLUICtrl* c, const LLSD&) { WolfDJMixer::instance().mMasterFader = (F32)c->getValue().asReal(); saveLevels(); });
    mMaster.mCue->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCue(CUE_MASTER); });

    getChild<LLCheckBoxCtrl>("talk_over")->setCommitCallback([this](LLUICtrl* c, const LLSD&)
    {
        WolfDJMixer::instance().mTalkOver = c->getValue().asBoolean();
        gSavedSettings.setBOOL("WolfDJTalkOver", c->getValue().asBoolean());
    });

    mSourceA = getChild<LLComboBox>("source_music_a");
    mSourceB = getChild<LLComboBox>("source_music_b");
    mSourceA->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSource(CH_MUSIC_A); });
    mSourceB->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSource(CH_MUSIC_B); });
    getChild<LLButton>("refresh_sources")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshApps(); });

    mUrl = getChild<LLLineEditor>("stream_url");
    mPassword = getChild<LLLineEditor>("stream_password");
    mStation = getChild<LLLineEditor>("station_name");
    mArtist = getChild<LLLineEditor>("now_artist");
    mTitle = getChild<LLLineEditor>("now_title");
    mLiveBtn = getChild<LLButton>("go_live");
    mStatus = getChild<LLTextBox>("status_text");
    mVoiceNote = getChild<LLTextBox>("voice_note");

    mUrl->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedPerAccountSettings.setString("WolfDJStreamURL", c->getValue().asString()); });
    mStation->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedPerAccountSettings.setString("WolfDJStationName", c->getValue().asString()); });
    mPassword->setCommitCallback([](LLUICtrl* c, const LLSD&)
    {
        if (gSecAPIHandler)
        {
            gSecAPIHandler->setProtectedData(PROTECTED_TYPE, password_id(), LLSD(c->getValue().asString()));
        }
    });
    getChild<LLButton>("get_stream")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLWeb::loadURLExternal(MY_STREAM_PAGE); });
    getChild<LLButton>("copy_url")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCopyUrl(); });
    getChild<LLButton>("send_title")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSendTitle(); });
    mLiveBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGoLive(); });
    getChild<LLButton>("open_playlist")->setCommitCallback([](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance("wolf_dj_playlist"); });
    getChild<LLCheckBoxCtrl>("play_on_land")->setCommitCallback([](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL("WolfDJPlayOnLand", c->getValue().asBoolean());
    });
    return true;
}

void WolfFloaterDJ::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    WolfDJMixer& mix = WolfDJMixer::instance();
    mix.startEngine();
    loadLevels();

    mUrl->setValue(gSavedPerAccountSettings.getString("WolfDJStreamURL"));
    std::string station = gSavedPerAccountSettings.getString("WolfDJStationName");
    if (station.empty())
    {
        station = gAgentUsername.empty() ? std::string("Wolf DJ") : gAgentUsername + " live";
    }
    mStation->setValue(station);
    if (gSecAPIHandler)
    {
        const LLSD pw = gSecAPIHandler->getProtectedData(PROTECTED_TYPE, password_id());
        mPassword->setValue(pw.isString() ? pw.asString() : std::string());
    }
    getChild<LLCheckBoxCtrl>("talk_over")->set(gSavedSettings.getBOOL("WolfDJTalkOver"));
    mix.mTalkOver = gSavedSettings.getBOOL("WolfDJTalkOver");
    getChild<LLCheckBoxCtrl>("play_on_land")->set(gSavedSettings.getBOOL("WolfDJPlayOnLand"));

    // The microphone is always a channel while the mixer is open (its strip has a mute).
    if (!mix.capture(CH_MIC))
    {
        std::string err;
        std::unique_ptr<WolfDJCaptureStream> mic = WolfDJCapture::openMic(&mix.channel(CH_MIC), err);
        if (mic)
        {
            mix.setCapture(CH_MIC, std::move(mic), "mic");
        }
        else
        {
            mStatus->setText("Microphone: " + err);
        }
    }
    refreshApps();
    mClock.reset();
}

void WolfFloaterDJ::onClose(bool app_quitting)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    if (app_quitting)
    {
        return;     // LLAppViewer::requestQuit already ended the show (WolfDJ quit hook)
    }
    if (!mix.isLiveRequested())
    {
        // Not on air: nothing should keep listening to the mic or to other programs.
        for (int ch = 0; ch < CH_COUNT; ++ch)
        {
            mix.setCapture(ch, nullptr, std::string());
        }
        mix.mCue = CUE_NONE;
        mix.stopEngineIfIdle();
    }
}

void WolfFloaterDJ::refreshApps()
{
    std::vector<WolfDJApp> apps;
    std::string why;
    const bool ok = WolfDJCapture::listApps(apps, why);
    WolfDJMixer& mix = WolfDJMixer::instance();
    for (LLComboBox* combo : { mSourceA, mSourceB })
    {
        const int ch = combo == mSourceA ? CH_MUSIC_A : CH_MUSIC_B;
        combo->removeall();
        combo->add("(nothing)", LLSD(std::string()));
        combo->add("Wolf DJ playlist", LLSD(std::string(WOLFDJ_PLAYLIST_ID)));
        for (const WolfDJApp& app : apps)
        {
            combo->add(app.mName, LLSD(app.mId));
        }
        const std::string& current = mix.captureId(ch);
        if (!current.empty() && !combo->setSelectedByValue(LLSD(current), true))
        {
            combo->add("(playing earlier program)", LLSD(current));
            combo->setSelectedByValue(LLSD(current), true);
        }
        else if (current.empty())
        {
            combo->setSelectedByValue(LLSD(std::string()), true);
        }
    }
    getChild<LLTextBox>("sources_note")->setText(ok ? (why.empty() ? std::string() : why) : why);
}

void WolfFloaterDJ::onSource(int ch)
{
    LLComboBox* combo = ch == CH_MUSIC_A ? mSourceA : mSourceB;
    const std::string id = combo->getValue().asString();
    WolfDJMixer& mix = WolfDJMixer::instance();
    if (id == mix.captureId(ch)) return;
    if (id.empty())
    {
        mix.setCapture(ch, nullptr, std::string());
        return;
    }
    if (id == WOLFDJ_PLAYLIST_ID)
    {
        // One channel plays the playlist: picking it here takes it off the other one.
        const int other = ch == CH_MUSIC_A ? CH_MUSIC_B : CH_MUSIC_A;
        if (mix.captureId(other) == WOLFDJ_PLAYLIST_ID)
        {
            mix.setCapture(other, nullptr, std::string());
            (other == CH_MUSIC_A ? mSourceA : mSourceB)->setSelectedByValue(LLSD(std::string()), true);
        }
        mix.setCapture(ch, wolfdj_open_playlist_stream(&mix.channel(ch)), id);
        return;
    }
    std::string err;
    std::unique_ptr<WolfDJCaptureStream> cap = WolfDJCapture::openApp(id, &mix.channel(ch), err);
    if (!cap)
    {
        mix.setCapture(ch, nullptr, std::string());
        combo->setSelectedByValue(LLSD(std::string()), true);
        LLSD args;
        args["MESSAGE"] = "Could not capture that program: " + err;
        LLNotificationsUtil::add("GenericAlert", args);
        return;
    }
    mix.setCapture(ch, std::move(cap), id);
}

void WolfFloaterDJ::onCue(int ch)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    mix.mCue = (mix.mCue.load() == ch) ? CUE_NONE : ch;
}

bool WolfFloaterDJ::parseStreamUrl(const std::string& url_in, WolfDJMixer::StreamConfig& cfg, std::string& err) const
{
    std::string url = url_in;
    LLStringUtil::trim(url);
    if (url.empty())
    {
        err = "Paste your stream address from My Radio Stream (it looks like http://cast.wolfterritories.org:8000/radio/your-name).";
        return false;
    }
    if (url.find("://") == std::string::npos)
    {
        url = "http://" + url;
    }
    LLURI uri(url);
    if (uri.scheme() != "http" || uri.hostName().empty())
    {
        err = "The stream address must start with http:// (the address listeners use).";
        return false;
    }
    cfg.mHost = uri.hostName();
    const std::string hp = uri.hostNameAndPort();
    const size_t colon = hp.rfind(':');
    cfg.mPort = colon == std::string::npos ? 80 : atoi(hp.c_str() + colon + 1);
    cfg.mMount = uri.path();
    if (cfg.mMount.size() < 2 || cfg.mMount[0] != '/')
    {
        err = "The stream address needs the mount, e.g. /radio/your-name.";
        return false;
    }
    return true;
}

bool WolfFloaterDJ::canSetLand(std::string& why) const
{
    LLViewerRegion* region = gAgent.getRegion();
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
    if (!region || !parcel)
    {
        why = "You are not on any land.";
        return false;
    }
    if (region->getOwner() != gAgentID)
    {
        why = "This is not your region, so the mixer does not change its music. Send the owner your stream address (Copy) to put on their land.";
        return false;
    }
    if (!LLViewerParcelMgr::isParcelModifiableByAgent(parcel, GP_LAND_CHANGE_MEDIA))
    {
        why = "You cannot change this parcel's music.";
        return false;
    }
    return true;
}

void WolfFloaterDJ::onGoLive()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    if (mix.isLiveRequested())
    {
        mix.goOffAir();
        std::string note;
        end_show(&note);
        mStatus->setText(note.empty() ? std::string("Off air.") : note);
        return;
    }

    WolfDJMixer::StreamConfig cfg;
    std::string err;
    if (!parseStreamUrl(mUrl->getValue().asString(), cfg, err))
    {
        mStatus->setText(err);
        return;
    }
    cfg.mPassword = mPassword->getValue().asString();
    if (cfg.mPassword.empty())
    {
        mStatus->setText(std::string("Enter your stream password (from My Radio Stream - it is shown once when you make or renew it)."));
        return;
    }
    cfg.mStationName = mStation->getValue().asString();
    cfg.mDescription = "Live from Wolf Territories with WolfViewer";
    gSavedPerAccountSettings.setString("WolfDJStreamURL", mUrl->getValue().asString());
    gSavedPerAccountSettings.setString("WolfDJStationName", cfg.mStationName);
    if (gSecAPIHandler)
    {
        gSecAPIHandler->setProtectedData(PROTECTED_TYPE, password_id(), LLSD(cfg.mPassword));
    }

    sShow = Show();
    sShow.mListenUrl = "http://" + cfg.mHost + ":" + std::to_string(cfg.mPort) + cfg.mMount;
    sShow.mStatusHost = cfg.mHost;
    sShow.mStatusPort = cfg.mPort;
    sShow.mMount = cfg.mMount;

    // The DJ must not hear their own stream 10 seconds late: silence this viewer's parcel music
    // for the show (optionallyStartMusic honours AudioStreamingMusic, so it stays off even when
    // the parcel's music changes to the stream below).
    sShow.mPrevStreamingMusic = gSavedSettings.getBOOL("AudioStreamingMusic");
    gSavedSettings.setBOOL("AudioStreamingMusic", false);
    LLViewerAudio::getInstance()->stopInternetStreamWithAutoFade();
    sShow.mMusicSilenced = true;

    std::string land_note;
    if (getChild<LLCheckBoxCtrl>("play_on_land")->get())
    {
        std::string why;
        if (canSetLand(why))
        {
            LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
            sShow.mPrevMusicUrl = parcel->getMusicURL();
            sShow.mParcelLocalId = parcel->getLocalID();
            sShow.mRegionId = gAgent.getRegion()->getRegionID();
            parcel->setMusicURL(sShow.mListenUrl);
            LLViewerParcelMgr::getInstance()->sendParcelPropertiesUpdate(parcel);
            sShow.mLandSet = true;
        }
        else
        {
            land_note = why;
        }
    }
    mix.setNowPlaying(mArtist->getValue().asString(), mTitle->getValue().asString());
    mix.goLive(cfg);
    mListeners = -1;
    mNextListeners = mClock.getElapsedTimeF32() + 10.f;
    getChild<LLTextBox>("land_note")->setText(land_note);
}

void WolfFloaterDJ::onSendTitle()
{
    WolfDJMixer::instance().setNowPlaying(mArtist->getValue().asString(), mTitle->getValue().asString());
}

void WolfFloaterDJ::onCopyUrl()
{
    WolfDJMixer::StreamConfig cfg;
    std::string err;
    if (!parseStreamUrl(mUrl->getValue().asString(), cfg, err))
    {
        mStatus->setText(err);
        return;
    }
    const std::string url = "http://" + cfg.mHost + ":" + std::to_string(cfg.mPort) + cfg.mMount;
    const LLWString w = utf8str_to_wstring(url);
    LLClipboard::instance().copyToClipboard(w, 0, (S32)w.size());
    mStatus->setText("Copied " + url + " - the region owner puts it in About Land > Sound.");
}

void WolfFloaterDJ::saveLevels()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    LLSD levels;
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        const WolfDJChannel& c = mix.channel(ch);
        LLSD s;
        s["fader"] = c.mFader.load();
        s["high"] = c.mEqHighDb.load();
        s["mid"] = c.mEqMidDb.load();
        s["low"] = c.mEqLowDb.load();
        s["mute"] = c.mMute.load();
        levels[STRIP_KEYS[ch]] = s;
    }
    levels["master"] = mix.mMasterFader.load();
    gSavedSettings.setLLSD("WolfDJLevels", levels);
}

void WolfFloaterDJ::loadLevels()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    const LLSD levels = gSavedSettings.getLLSD("WolfDJLevels");
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        WolfDJChannel& c = mix.channel(ch);
        const LLSD s = levels.has(STRIP_KEYS[ch]) ? levels[STRIP_KEYS[ch]] : LLSD();
        if (s.isMap())
        {
            c.mFader = (F32)s["fader"].asReal();
            c.mEqHighDb = (F32)s["high"].asReal();
            c.mEqMidDb = (F32)s["mid"].asReal();
            c.mEqLowDb = (F32)s["low"].asReal();
            c.mMute = s["mute"].asBoolean();
        }
        Strip& st = mStrips[ch];
        st.mFader->setValue(c.mFader.load());
        st.mEqHigh->setValue(c.mEqHighDb.load());
        st.mEqMid->setValue(c.mEqMidDb.load());
        st.mEqLow->setValue(c.mEqLowDb.load());
        st.mMute->setValue(c.mMute.load());
    }
    if (levels.has("master"))
    {
        mix.mMasterFader = (F32)levels["master"].asReal();
    }
    mMaster.mFader->setValue(mix.mMasterFader.load());
}

void WolfFloaterDJ::requestListeners()
{
    if (mListenersBusy || sShow.mStatusHost.empty()) return;
    mListenersBusy = true;
    const std::string url = "http://" + sShow.mStatusHost + ":" + std::to_string(sShow.mStatusPort) + "/status-json.xsl";
    const std::string mount = sShow.mMount;
    LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("WolfDJListeners", [handle, url, mount]()
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfDJ", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(10);
        opts->setRetries(0);
        LLSD result = adapter->getRawAndSuspend(request, url, opts);
        S32 listeners = -1;
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            // Icecast 2.4 status-json.xsl: {"icestats":{"source": {...} or [{...}, ...]}}, each
            // source with "listenurl" and "listeners".
            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value root = boost::json::parse(std::string(raw.begin(), raw.end()), ec);
            if (!ec && root.is_object())
            {
                const boost::json::object& top = root.as_object();
                const boost::json::value* stats = top.if_contains("icestats");
                const boost::json::value* sources = stats && stats->is_object() ? stats->as_object().if_contains("source") : nullptr;
                auto check = [&](const boost::json::value& v)
                {
                    if (!v.is_object()) return;
                    const boost::json::object& o = v.as_object();
                    const boost::json::value* lu = o.if_contains("listenurl");
                    const boost::json::value* ls = o.if_contains("listeners");
                    if (!lu || !lu->is_string() || !ls) return;
                    const std::string listen(lu->as_string().c_str());
                    if (listen.size() >= mount.size() && listen.compare(listen.size() - mount.size(), mount.size(), mount) == 0)
                    {
                        listeners = ls->is_int64() ? (S32)ls->as_int64() : (ls->is_uint64() ? (S32)ls->as_uint64() : listeners);
                    }
                };
                if (sources && sources->is_array())
                {
                    for (const boost::json::value& v : sources->as_array()) check(v);
                }
                else if (sources)
                {
                    check(*sources);
                }
            }
        }
        if (!handle.isDead())
        {
            WolfFloaterDJ* self = static_cast<WolfFloaterDJ*>(handle.get());
            self->mListeners = listeners;
            self->mListenersBusy = false;
        }
    });
}

void WolfFloaterDJ::updateStatus()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    const EState st = mix.state();
    const bool live = mix.isLiveRequested();
    mLiveBtn->setLabel(live ? std::string("Stop") : std::string("Go Live"));
    mLiveBtn->setToggleState(live);
    if (live)
    {
        std::string text;
        switch (st)
        {
        case ON_AIR:
        {
            const S32 secs = (S32)mix.onAirSeconds();
            text = llformat("ON AIR  %d:%02d:%02d", secs / 3600, (secs / 60) % 60, secs % 60);
            if (mListeners >= 0)
            {
                text += llformat("  ·  %d listener%s", mListeners, mListeners == 1 ? "" : "s");
            }
            break;
        }
        case CONNECTING: text = mix.statusMessage(); break;
        case RETRYING: text = mix.statusMessage(); break;
        case FAILED: text = mix.statusMessage(); break;
        default: text = "Starting..."; break;
        }
        mStatus->setText(text);
        if (st == ON_AIR && mClock.getElapsedTimeF32() > mNextListeners)
        {
            mNextListeners = mClock.getElapsedTimeF32() + 15.f;
            requestListeners();
        }
    }

    // In-world voice is only there while voice is connected.
    const bool voice_signal = mix.channel(CH_VOICE).mPrePeak.load() > 0.f;
    mVoiceNote->setText(voice_signal ? std::string() : std::string("Voice: what you hear in voice chat"));

    // Cue buttons show which one is on.
    const int cue = mix.mCue.load();
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        mStrips[ch].mCue->setToggleState(cue == ch);
    }
    mMaster.mCue->setToggleState(cue == CUE_MASTER);

    // A program that has gone (closed, or its sound stream ended).
    for (int ch : { (int)CH_MUSIC_A, (int)CH_MUSIC_B })
    {
        WolfDJCaptureStream* cap = mix.capture(ch);
        if (cap && !cap->ok())
        {
            mix.setCapture(ch, nullptr, std::string());
            (ch == CH_MUSIC_A ? mSourceA : mSourceB)->setSelectedByValue(LLSD(std::string()), true);
        }
    }
}

// LED meter: 12 segments from -48 dBFS to 0 dBFS (green to -12, amber to -3, red above), plus a
// clip LED that stays lit 2 s after a peak at full scale. Falls at 24 dB a second.
void WolfFloaterDJ::drawMeter(LLView* meter, float peak, float& shown_db, float& clip_until)
{
    static const float SEG_DB[12] = { -48.f, -42.f, -36.f, -30.f, -24.f, -18.f, -12.f, -9.f, -6.f, -3.f, -1.5f, -0.5f };
    const float now = mClock.getElapsedTimeF32();
    const float db = wolfdj_lin_to_db(peak);
    const float dt = llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.25f);
    shown_db = std::max(db, shown_db - 24.f * dt);
    if (peak >= 0.99f) clip_until = now + 2.f;

    const LLRect r = meter->getRect();
    const S32 segs = 13;    // 12 level LEDs + the clip LED on top
    const S32 gap = 2;
    const S32 seg_h = std::max(2, (r.getHeight() - gap * (segs - 1)) / segs);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    for (S32 i = 0; i < segs; ++i)
    {
        const S32 bottom = r.mBottom + i * (seg_h + gap);
        LLColor4 on, off;
        bool lit;
        if (i == segs - 1)
        {
            on = LLColor4(1.f, 0.1f, 0.1f, 1.f);
            lit = now < clip_until;
        }
        else
        {
            if (SEG_DB[i] < -12.f) on = LLColor4(0.15f, 0.9f, 0.25f, 1.f);
            else if (SEG_DB[i] < -3.f) on = LLColor4(1.f, 0.75f, 0.1f, 1.f);
            else on = LLColor4(1.f, 0.2f, 0.15f, 1.f);
            lit = shown_db >= SEG_DB[i];
        }
        off = LLColor4(on.mV[VRED] * 0.18f, on.mV[VGREEN] * 0.18f, on.mV[VBLUE] * 0.18f, 1.f);
        gl_rect_2d(r.mLeft, bottom + seg_h, r.mRight, bottom, lit ? on : off, true);
    }
}

void WolfFloaterDJ::draw()
{
    updateStatus();
    LLFloater::draw();
    if (isMinimized()) return;
    WolfDJMixer& mix = WolfDJMixer::instance();
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        Strip& s = mStrips[ch];
        const WolfDJChannel& c = mix.channel(ch);
        drawMeter(s.mMeter, c.mPeak.load(), s.mShownDb, s.mClipUntil);
        s.mDb->setText(c.mMute ? std::string("muted") : llformat("%+.0f dB", wolfdj_lin_to_db(wolfdj_fader_gain(c.mFader))));
    }
    drawMeter(mMaster.mMeter, mix.mMasterPeak.load(), mMaster.mShownDb, mMaster.mClipUntil);
    const float lim = mix.mLimiterDb.load();
    mMaster.mDb->setText(lim < -0.5f ? llformat("limit %.0f dB", lim) : llformat("%+.0f dB", wolfdj_lin_to_db(wolfdj_fader_gain(mix.mMasterFader))));
}

// static - LLAppViewer::requestQuit: end a show while the region can still hear us.
void wolfdj_on_app_quit()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    if (mix.isLiveRequested())
    {
        mix.goOffAir();
    }
    end_show(nullptr);
}
