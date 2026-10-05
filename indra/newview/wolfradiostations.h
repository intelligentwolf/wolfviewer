/**
 * @file wolfradiostations.h
 * @brief WolfViewer: the internet radio stations behind About Land > Sound and the Radio
 *        Stations floater, and the land's music URL helpers they share.
 *
 * [2026-10-05] Paul: "replace this, 10 preset URL with button to set on the land another button
 * that says Radio Stations you click on and up comes a floater that looks like a radio with a
 * genre choice ... a set of stations ... say 10 for each genre". The lists come from
 * wolfstorm.app/php/radio_stations.php (built daily from Radio Browser, radio-browser.info):
 * 10 featured Wolf Territories stations (WTGR first) and 12 genres of 10 stations each.
 * The user's own 10 presets are the setting WolfMusicPresets.
 */

#ifndef WOLF_RADIO_STATIONS_H
#define WOLF_RADIO_STATIONS_H

#include "llsd.h"
#include "llsingleton.h"
#include <boost/signals2.hpp>

class WolfRadioStations : public LLSingleton<WolfRadioStations>
{
    LLSINGLETON_EMPTY_CTOR(WolfRadioStations);

public:
    static constexpr S32 PRESET_COUNT = 10;

    typedef boost::signals2::signal<void()> changed_signal_t;

    /// Start loading the lists if they are not loaded or loading (once per session; a failed
    /// load may be tried again).
    void load();
    bool isLoaded() const { return mLoaded; }
    bool isLoading() const { return mLoading; }
    const std::string& lastError() const { return mLastError; }

    /// [{name, url, bitrate, country, homepage, genre}] — the 10 Wolf Territories stations.
    const LLSD& featured() const { return mFeatured; }
    /// [{id, name, stations: [...]}]
    const LLSD& genres() const { return mGenres; }

    /// Fired on the main loop when a load finishes (either way).
    boost::signals2::connection onChanged(const changed_signal_t::slot_type& cb) { return mChanged.connect(cb); }

    // ---- the user's own presets (setting WolfMusicPresets, an array of PRESET_COUNT strings) ----
    static std::string preset(S32 index);
    static void setPreset(S32 index, const std::string& url);
    /// Put the URL in the first empty slot. False when every slot is taken (or it is already there).
    static bool addPreset(const std::string& url, bool* already_there = nullptr);

    // ---- the land's music ----
    /// The parcel About Land is showing, if the agent may change its media; else null.
    static class LLParcel* editableParcel();
    /// Set the About Land parcel's music URL (as the Sound tab does). False when not allowed.
    static bool setLandMusic(const std::string& url);
    /// Play a stream for this viewer only (a preview); "" stops it and gives back the land's music.
    static void preview(const std::string& url);
    static const std::string& previewing() { return sPreviewURL; }

private:
    static void loadCoro();

    bool mLoaded = false;
    bool mLoading = false;
    std::string mLastError;
    LLSD mFeatured;
    LLSD mGenres;
    changed_signal_t mChanged;
    static std::string sPreviewURL;
};

#endif // WOLF_RADIO_STATIONS_H
