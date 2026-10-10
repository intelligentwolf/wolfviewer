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
#include "wolfdjdesk.h"
#include "wolfdjplayer.h"
#include "wolfgrid.h"

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
#include "llscrollcontainer.h"
#include "lllocalcliprect.h"
#include "llsecapi.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llweb.h"
#include "llrender.h"
#include "llui.h"
#include "lluicolortable.h"
#include "roles_constants.h"

#include <boost/json.hpp>

using namespace WolfDJ;

namespace
{
    const char* STRIP_KEYS[CH_COUNT] = { "voice", "mic", "music_a", "music_b", "music_c", "music_d" };
    const char* GRID_STREAM_API = "https://www.wolf-grid.com/gridstream.php";   // gridmanager/gridstream.php
    const char* PROTECTED_TYPE = "wolf_dj";

    // A show in progress: what to put back afterwards. Survives the floater being closed.
    struct Show
    {
        bool mLandSet = false;
        std::string mPrevMusicUrl;
        S32 mParcelLocalId = 0;
        LLUUID mRegionId;
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

    // Paul: "please save the users stream password". setProtectedData only changes the store in
    // memory (llsechandler_basic.cpp LLSecAPIBasicHandler::setProtectedData); syncProtectedMap
    // writes it to the encrypted file (_writeProtectedData), so it is there next session.
    // Paul: "add a check box Save Password?" - setting WolfDJSavePassword. Off: the stored copy
    // is removed and the typed password is used only this session.
    void save_password(const std::string& pw)
    {
        if (!gSecAPIHandler) return;
        if (gSavedSettings.getBOOL("WolfDJSavePassword") && !pw.empty())
        {
            gSecAPIHandler->setProtectedData(PROTECTED_TYPE, password_id(), LLSD(pw));
        }
        else
        {
            gSecAPIHandler->deleteProtectedData(PROTECTED_TYPE, password_id());
        }
        gSecAPIHandler->syncProtectedMap();
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
    // [WOLF DJ 2026-10-10] The desk draws and handles every strip (wolfdjdesk.cpp); it commits
    // when a move is finished, and the levels are saved then.
    mDesk = getChild<WolfDJDesk>("desk");
    mDesk->setCommitCallback([this](LLUICtrl*, const LLSD&) { saveLevels(); });

    // [WOLF DJ 2026-10-05] Jingle pads, each its own colour (WolfDJJingles::PAD_RGB).
    static_assert(WolfDJJingles::PAD_COUNT == 8, "mPads and the XML have 8 pads");
    for (int i = 0; i < (int)mPads.size(); ++i)
    {
        const float* rgb = WolfDJJingles::PAD_RGB[i];
        mPads[i] = getChild<LLButton>(llformat("jingle_%d", i));
        mPads[i]->setImageColor(LLUIColor(LLColor4(rgb[0], rgb[1], rgb[2], 1.f)));
        mPads[i]->setCommitCallback([i](LLUICtrl*, const LLSD&)
        {
            const int pad = i;
            if (WolfDJJingles::instance().pad(pad).empty())
            {
                // Empty: straight to where a song is put on it (a status line note was overwritten
                // every frame while on air - Paul 10-05).
                LLFloaterReg::showInstance("wolf_dj_playlist");
                return;
            }
            WolfDJJingles::instance().trigger(pad);
        });
    }
    getChild<LLButton>("jingle_stop")->setCommitCallback([](LLUICtrl*, const LLSD&) { WolfDJJingles::instance().stop(); });

    getChild<LLCheckBoxCtrl>("talk_over")->setCommitCallback([](LLUICtrl* c, const LLSD&)
    {
        WolfDJMixer::instance().mTalkOver = c->getValue().asBoolean();
        gSavedSettings.setBOOL("WolfDJTalkOver", c->getValue().asBoolean());
    });

    for (int ch = CH_MUSIC_A; ch < CH_COUNT; ++ch)
    {
        LLComboBox* combo = getChild<LLComboBox>(std::string("source_") + STRIP_KEYS[ch]);
        mSources[ch - CH_MUSIC_A] = combo;
        // On the desk, in the slot its strip leaves for it (the desk sits at the panel's origin).
        LLRect r = mDesk->sourceRect(ch);
        r.translate(mDesk->getRect().mLeft, mDesk->getRect().mBottom);
        combo->setShape(r);
        combo->setCommitCallback([this, ch](LLUICtrl*, const LLSD&) { onSource(ch); });
        // Paul: "a refresh button for music sources" - and the list refreshes itself every time
        // it is opened (LLComboBox::onButtonMouseDown calls prearrangeList before showing it).
        combo->setPrearrangeCallback([this, combo, ch](LLUICtrl*, const LLSD&)
        {
            std::vector<WolfDJApp> apps;
            std::string why;
            WolfDJCapture::listApps(apps, why);
            fillSources(combo, ch, apps);
        });
    }
    getChild<LLButton>("refresh_sources")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshApps(); });

    mUrl = getChild<LLLineEditor>("stream_url");
    mPassword = getChild<LLLineEditor>("stream_password");
    mStation = getChild<LLLineEditor>("station_name");
    mArtist = getChild<LLLineEditor>("now_artist");
    mTitle = getChild<LLLineEditor>("now_title");
    mLiveBtn = getChild<LLButton>("go_live");
    mStatus = getChild<LLTextBox>("status_text");

    mUrl->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedPerAccountSettings.setString("WolfDJStreamURL", c->getValue().asString()); });
    mStation->setCommitCallback([](LLUICtrl* c, const LLSD&) { gSavedPerAccountSettings.setString("WolfDJStationName", c->getValue().asString()); });
    mPassword->setCommitCallback([](LLUICtrl* c, const LLSD&) { save_password(c->getValue().asString()); });
    getChild<LLCheckBoxCtrl>("save_password")->setCommitCallback([this](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL("WolfDJSavePassword", c->getValue().asBoolean());
        save_password(mPassword->getValue().asString());
    });
    // Paul 10-05: "a button saying Grid Stream - click it and it just gets your grid stream if
    // you have a region, if not it reminds you to purchase a region with a link to the regions page".
    getChild<LLButton>("get_stream")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onGridStream(false); });
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
    mix.setWindowOpen(true);
    mix.startEngine();
    loadLevels();

    mUrl->setValue(gSavedPerAccountSettings.getString("WolfDJStreamURL"));
    std::string station = gSavedPerAccountSettings.getString("WolfDJStationName");
    if (station.empty())
    {
        station = gAgentUsername.empty() ? std::string("Wolf DJ") : gAgentUsername + " live";
    }
    mStation->setValue(station);
    getChild<LLCheckBoxCtrl>("save_password")->set(gSavedSettings.getBOOL("WolfDJSavePassword"));
    if (gSecAPIHandler && mPassword->getValue().asString().empty())
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
    // Paul: "music 1 should always default to wolf dj play[list]" - unless the DJ has put
    // something else there, or the playlist is already on another channel.
    if (mix.captureId(CH_MUSIC_A).empty())
    {
        bool elsewhere = false;
        for (int ch = CH_MUSIC_B; ch < CH_COUNT; ++ch)
        {
            elsewhere = elsewhere || mix.captureId(ch) == WOLFDJ_PLAYLIST_ID;
        }
        if (!elsewhere)
        {
            mix.setCapture(CH_MUSIC_A, wolfdj_open_playlist_stream(&mix.channel(CH_MUSIC_A)), WOLFDJ_PLAYLIST_ID);
        }
    }
    // [WOLF DJ 2026-10-10] MON is per channel now. A channel with no MON saved yet takes the old
    // playlist "Hear it myself" setting if it has the playlist.
    const LLSD saved = gSavedSettings.getLLSD("WolfDJLevels");
    for (int ch = CH_MUSIC_A; ch < CH_COUNT; ++ch)
    {
        const bool has_mon = saved.has(STRIP_KEYS[ch]) && saved[STRIP_KEYS[ch]].has("mon");
        if (!has_mon && mix.captureId(ch) == WOLFDJ_PLAYLIST_ID)
        {
            mix.channel(ch).mMonitor = gSavedSettings.getBOOL("WolfDJPlaylistMonitor");
        }
    }
    WolfDJPlayer::instance().loadSaved();
    refreshApps();
    mClock.reset();
}

void WolfFloaterDJ::onClose(bool app_quitting)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    mix.setWindowOpen(false);
    // The password field may still have focus (its commit fires on focus loss / Enter).
    save_password(mPassword->getValue().asString());
    if (app_quitting)
    {
        return;     // LLAppViewer::requestQuit already ended the show (WolfDJ quit hook)
    }
    if (!mix.isLiveRequested())
    {
        // Not on air: nothing should keep listening to the mic or to other programs. [10-10] A
        // playlist that is playing carries on (and the engine with it, so the DJ still hears it
        // through MON): closing the mixer used to silence it (Paul: "stop it from not playing").
        const bool playlist_on = WolfDJPlayer::instance().isPlaying();
        for (int ch = 0; ch < CH_COUNT; ++ch)
        {
            if (!(playlist_on && mix.captureId(ch) == WOLFDJ_PLAYLIST_ID))
            {
                mix.setCapture(ch, nullptr, std::string());
            }
        }
        mix.mCue = CUE_NONE;
        if (!playlist_on)
        {
            mix.stopEngineIfIdle();
        }
    }
}

void WolfFloaterDJ::refreshApps()
{
    std::vector<WolfDJApp> apps;
    std::string why;
    WolfDJCapture::listApps(apps, why);
    for (int ch = CH_MUSIC_A; ch < CH_COUNT; ++ch)
    {
        fillSources(sourceCombo(ch), ch, apps);
    }
    // Paul 10-05: "we need to tell people they need to play audio then hit refresh streams to get a
    // feed into it from another application" - a program only shows in the lists while it is
    // playing sound (wolfdjcapture listApps). A reason the lists cannot be made wins.
    getChild<LLTextBox>("sources_note")->setText(why.empty()
        ? std::string("Another program: start it playing first, press Refresh sources, then pick it on a Music channel.")
        : why);
}

void WolfFloaterDJ::fillSources(LLComboBox* combo, int ch, const std::vector<WolfDJApp>& apps)
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    {
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
}

void WolfFloaterDJ::onSource(int ch)
{
    LLComboBox* combo = sourceCombo(ch);
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
        // One channel plays the playlist: picking it here takes it off any other.
        for (int other = CH_MUSIC_A; other < CH_COUNT; ++other)
        {
            if (other != ch && mix.captureId(other) == WOLFDJ_PLAYLIST_ID)
            {
                mix.setCapture(other, nullptr, std::string());
                sourceCombo(other)->setSelectedByValue(LLSD(std::string()), true);
            }
        }
        mix.setCapture(ch, wolfdj_open_playlist_stream(&mix.channel(ch)), id);
        mix.channel(ch).mMonitor = true;    // the playlist is only heard through MON
        saveLevels();
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
    // Another program already plays through the DJ's speakers: MON would play it twice.
    mix.channel(ch).mMonitor = false;
    saveLevels();
}

bool WolfFloaterDJ::parseStreamUrl(const std::string& url_in, WolfDJMixer::StreamConfig& cfg, std::string& err) const
{
    std::string url = url_in;
    LLStringUtil::trim(url);
    if (url.empty())
    {
        err = "Press Grid Stream to fill in your stream, or paste its address (it looks like http://cast.wolfterritories.org:8000/radio/your-name).";
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
        mStatus->setText(std::string("Enter your stream password, or press Grid Stream to get it (it is shown once when it is made)."));
        return;
    }
    cfg.mStationName = mStation->getValue().asString();
    cfg.mDescription = "Live from Wolf Territories with WolfViewer";
    gSavedPerAccountSettings.setString("WolfDJStreamURL", mUrl->getValue().asString());
    gSavedPerAccountSettings.setString("WolfDJStationName", cfg.mStationName);
    save_password(cfg.mPassword);

    sShow = Show();
    sShow.mListenUrl = "http://" + cfg.mHost + ":" + std::to_string(cfg.mPort) + cfg.mMount;
    sShow.mStatusHost = cfg.mHost;
    sShow.mStatusPort = cfg.mPort;
    sShow.mMount = cfg.mMount;

    // The viewer's parcel music is left alone while live (Paul: "you dont need to do that leave it
    // on so they can test it") - the DJ can listen to their own stream as listeners hear it.

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
        s["fx"] = c.mFx.load();
        s["fx_amount"] = c.mFxAmount.load();
        s["mon"] = c.mMonitor.load();
        levels[STRIP_KEYS[ch]] = s;
    }
    // [WOLF DJ 2026-10-10] Fader positions on the desk scale (law 2, wolfdj_fader_db); a stereo
    // master; the jingle strip's MON.
    levels["law"] = 2;
    levels["master_l"] = mix.mMasterFaderL.load();
    levels["master_r"] = mix.mMasterFaderR.load();
    levels["master_link"] = mDesk->masterLinked();
    levels["jingle"] = mix.mJingleLevel.load();
    levels["jingle_mon"] = mix.mJingleMonitor.load();
    gSavedSettings.setLLSD("WolfDJLevels", levels);
}

void WolfFloaterDJ::loadLevels()
{
    WolfDJMixer& mix = WolfDJMixer::instance();
    const LLSD levels = gSavedSettings.getLLSD("WolfDJLevels");
    // Levels saved before the desk (10-10) were on the old fader law: the same gain, moved to
    // where it sits on the desk scale.
    const bool old_law = levels.isMap() && !levels.has("law");
    auto fader = [old_law](const LLSD& v) { const float f = llclamp((F32)v.asReal(), 0.f, 1.f); return old_law ? wolfdj_old_fader_to_pos(f) : f; };
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        WolfDJChannel& c = mix.channel(ch);
        const LLSD s = levels.has(STRIP_KEYS[ch]) ? levels[STRIP_KEYS[ch]] : LLSD();
        if (s.isMap())
        {
            c.mFader = fader(s["fader"]);
            c.mEqHighDb = llclamp((F32)s["high"].asReal(), EQ_MIN_DB, EQ_MAX_DB);
            c.mEqMidDb = llclamp((F32)s["mid"].asReal(), EQ_MIN_DB, EQ_MAX_DB);
            c.mEqLowDb = llclamp((F32)s["low"].asReal(), EQ_MIN_DB, EQ_MAX_DB);
            c.mMute = s["mute"].asBoolean();
            if (s.has("fx"))
            {
                c.mFx = llclamp(s["fx"].asInteger(), (S32)FX_NONE, (S32)FX_COUNT - 1);
                c.mFxAmount = llclamp((F32)s["fx_amount"].asReal(), 0.f, 1.f);
            }
            if (s.has("mon"))
            {
                c.mMonitor = s["mon"].asBoolean();
            }
        }
    }
    if (levels.has("jingle"))
    {
        mix.mJingleLevel = fader(levels["jingle"]);
    }
    if (levels.has("jingle_mon"))
    {
        mix.mJingleMonitor = levels["jingle_mon"].asBoolean();
    }
    if (levels.has("master_l"))
    {
        mix.mMasterFaderL = fader(levels["master_l"]);
        mix.mMasterFaderR = fader(levels["master_r"]);
        mDesk->setMasterLinked(levels["master_link"].asBoolean());
    }
    else if (levels.has("master"))
    {
        mix.mMasterFaderL = fader(levels["master"]);    // one master fader before 10-10
        mix.mMasterFaderR = mix.mMasterFaderL.load();
    }
}

void WolfFloaterDJ::onGridStream(bool new_password)
{
    if (!WolfGrid::isWolfTerritories())
    {
        // The request carries this login's agent and session ids: only ever to Wolf's own site.
        mStatus->setText(std::string("Grid Stream is for Wolf Territories. On this grid, paste your stream's address and password."));
        return;
    }
    if (mGridStreamBusy) return;
    mGridStreamBusy = true;
    mStatus->setText(std::string(new_password ? "Getting a new password for your grid stream..." : "Getting your grid stream..."));
    const LLHandle<LLFloater> handle = getHandle();
    const std::string body = new_password ? "action=newpassword" : "action=get";
    LLCoros::instance().launch("WolfDJGridStream", [handle, body]() { gridStreamCoro(handle, body); });
}

// static
void WolfFloaterDJ::gridStreamCoro(LLHandle<LLFloater> handle, std::string body)
{
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfDJGridStream", LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    // Same verified options and identity headers as Jimmy's weather (wolfautoenvironment.cpp postCoro).
    LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
    options->setTimeout(20);
    options->setRetries(0);     // a retried "newpassword" would make a second password
    LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
    headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
    headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
    headers->append("X-Wolf-Agent", gAgentID.asString());
    headers->append("X-Wolf-Session", gAgentSessionID.asString());
    LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
    raw->append(body.data(), body.size());
    LLSD result = adapter->postRawAndSuspend(request, GRID_STREAM_API, raw, options, headers);
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
        result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);

    // gridstream.php: {success, url, mount, created, password?, message?} or {success:false, error, noRegion?, regionsUrl?}
    bool success = false, no_region = false;
    std::string error, url, password, message, regions_url;
    if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
    {
        const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        boost::system::error_code ec;
        boost::json::value root = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
        if (!ec && root.is_object())
        {
            const boost::json::object& o = root.as_object();
            auto str = [&o](const char* k) {
                const boost::json::value* v = o.if_contains(k);
                return v && v->is_string() ? std::string(v->as_string().c_str()) : std::string();
            };
            auto flag = [&o](const char* k) {
                const boost::json::value* v = o.if_contains(k);
                return v && v->is_bool() && v->as_bool();
            };
            success = flag("success");
            no_region = flag("noRegion");
            error = str("error");
            url = str("url");
            password = str("password");
            message = str("message");
            regions_url = str("regionsUrl");
        }
    }

    WolfFloaterDJ* self = static_cast<WolfFloaterDJ*>(handle.get());
    if (self) self->mGridStreamBusy = false;

    if (!success && no_region)
    {
        const std::string link = regions_url.empty() ? std::string("https://www.wolf-grid.com/index.php?f=ns") : regions_url;
        if (self) self->mStatus->setText(std::string("You need a region of your own to get a grid stream."));
        LLNotificationsUtil::add("GenericAlertYesCancel",
            LLSD().with("MESSAGE", "You need a region of your own to get a grid stream - every region on Wolf Territories "
                                   "comes with its own free radio stream.\n\nOpen the regions page to get one?"),
            LLSD(), [link](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) == 0) LLWeb::loadURLExternal(link);
                return false;
            });
        return;
    }
    if (!success || url.empty())
    {
        const std::string why = !error.empty() ? error : !status ? status.toString() : std::string("the website gave an unreadable answer");
        if (self) self->mStatus->setText("Could not get your grid stream: " + why);
        else LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Could not get your grid stream: " + why));
        return;
    }

    gSavedPerAccountSettings.setString("WolfDJStreamURL", url);
    if (!password.empty())
    {
        save_password(password);
    }
    if (!self)
    {
        if (!password.empty() && !gSavedSettings.getBOOL("WolfDJSavePassword"))
        {
            // Not saved and nowhere to show it: say it, or it is lost (the site keeps only a hash).
            LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Your grid stream password is " + password + " - keep it safe."));
        }
        return;
    }
    self->mUrl->setValue(url);
    if (!password.empty())
    {
        self->mPassword->setValue(password);
        self->mStatus->setText((message.empty() ? std::string("Your grid stream is ready.") : message)
            + (gSavedSettings.getBOOL("WolfDJSavePassword") ? " Address and password filled in and saved." : " Address and password filled in."));
        return;
    }
    if (!self->mPassword->getValue().asString().empty())
    {
        self->mStatus->setText(std::string("Your grid stream address is filled in. Using your saved password - press Go Live."));
        return;
    }
    // The stream exists but this viewer has no copy of its password, and the site cannot hand
    // the old one back (it keeps only a hash): offer a new one, saying what that does.
    self->mStatus->setText(std::string("Your grid stream address is filled in. It needs its password."));
    const LLHandle<LLFloater> h = handle;
    LLNotificationsUtil::add("GenericAlertYesCancel",
        LLSD().with("MESSAGE", "Your grid stream already has a password, and it is only shown once, when it is made.\n\n"
                               "Get a new one now? Any other DJ software using the old password will need the new one."),
        LLSD(), [h](const LLSD& n, const LLSD& r)
        {
            WolfFloaterDJ* f = static_cast<WolfFloaterDJ*>(h.get());
            if (f && LLNotificationsUtil::getSelectedOption(n, r) == 0) f->onGridStream(true);
            return false;
        });
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
    // Paul: "go live should go red when you're live and changed to ON AIR". Amber while it
    // connects or reconnects; back to the skin's own button colour off air.
    static const LLUIColor normal = LLUIColorTable::instance().getColor("ButtonImageColor");
    if (live && st == ON_AIR)
    {
        mLiveBtn->setLabel(std::string("ON AIR"));
        mLiveBtn->setImageColor(LLUIColor(LLColor4(0.95f, 0.12f, 0.12f, 1.f)));
        mLiveBtn->setToolTip(std::string("You are live. Click to go off air."));
    }
    else if (live && (st == CONNECTING || st == RETRYING || st == OFF_AIR))
    {
        mLiveBtn->setLabel(std::string("Connecting..."));
        mLiveBtn->setImageColor(LLUIColor(LLColor4(1.f, 0.62f, 0.1f, 1.f)));
        mLiveBtn->setToolTip(std::string("Connecting to your stream. Click to cancel."));
    }
    else if (live)
    {
        mLiveBtn->setLabel(std::string("Stop"));
        mLiveBtn->setImageColor(normal);
        mLiveBtn->setToolTip(std::string("The stream server refused - see the message. Click to stop."));
    }
    else
    {
        mLiveBtn->setLabel(std::string("Go Live"));
        mLiveBtn->setImageColor(normal);
        mLiveBtn->setToolTip(std::string("Start broadcasting to your stream."));
    }
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

    // Paul: "if i'm on air ... my playlist songs playing i need to be able to see them" - the
    // Now playing fields follow the playlist as each song starts (the player sends the same
    // artist/title to the stream, wolfdjplayer.cpp player_idle).
    WolfDJPlayer& player = WolfDJPlayer::instance();
    if (player.output() && player.isPlaying() && player.trackSerial() != mShownTrack)
    {
        mShownTrack = player.trackSerial();
        std::string artist, title;
        player.nowPlaying(artist, title);
        mArtist->setValue(artist);
        mTitle->setValue(title);
    }

    // [WOLF DJ 2026-10-10] The playlist can put itself on a channel when play is pressed
    // (WolfDJPlayer::routeToMixer): keep the source lists showing what each channel really has.
    for (int ch = CH_MUSIC_A; ch < CH_COUNT; ++ch)
    {
        LLComboBox* combo = sourceCombo(ch);
        const std::string& id = mix.captureId(ch);
        if (combo->getValue().asString() != id && !combo->hasFocus())
        {
            if (!combo->setSelectedByValue(LLSD(id), true))
            {
                std::vector<WolfDJApp> apps;
                std::string why;
                WolfDJCapture::listApps(apps, why);
                fillSources(combo, ch, apps);
            }
        }
    }

    // A program that has gone (closed, or its sound stream ended).
    for (int ch = CH_MUSIC_A; ch < CH_COUNT; ++ch)
    {
        WolfDJCaptureStream* cap = mix.capture(ch);
        if (cap && !cap->ok())
        {
            mix.setCapture(ch, nullptr, std::string());
            sourceCombo(ch)->setSelectedByValue(LLSD(std::string()), true);
        }
    }
}

void WolfFloaterDJ::draw()
{
    updateStatus();
    updatePads();
    LLFloater::draw();
}

// [WOLF DJ 2026-10-05] Each pad shows its file's name (set from the Playlist window) and stays
// lit while it plays.
void WolfFloaterDJ::updatePads()
{
    WolfDJJingles& j = WolfDJJingles::instance();
    const int playing = j.playing();
    for (int i = 0; i < (int)mPads.size(); ++i)
    {
        const std::string path = j.pad(i);
        if (path != mPadShown[i])
        {
            mPadShown[i] = path;
            const std::string name = WolfDJJingles::padLabel(path);
            mPads[i]->setLabel(name.empty() ? llformat("%d", i + 1) : name);
            mPads[i]->setToolTip(name.empty()
                ? llformat("Jingle pad %d - empty. Put a song on it from the Playlist window.", i + 1)
                : llformat("Jingle pad %d: %s. Click to play it on air; click again to stop.", i + 1, name.c_str()));
        }
        mPads[i]->setToggleState(playing == i);
    }
    // A file that would not play: say so where it cannot be missed (it is not a toast that goes).
    const std::string err = j.takeError();
    if (!err.empty())
    {
        LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE", "Jingle pad: " + err));
    }
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
