/**
 * @file wolfweathersound.h
 * @brief WolfViewer: the sound of the weather — the Weather tab's Sound section, made real.
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

#ifndef WOLF_WEATHER_SOUND_H
#define WOLF_WEATHER_SOUND_H

#include <map>
#include <string>

#include "lluuid.h"
#include "llsingleton.h"
#include "wolfweatherprofile.h"

/**
 * The Weather tab's two Sound Presets dropdowns, and an occasional thunder crack:
 *   AMBIENCE (mSoundPreset)  the bed  — thunderstorms, steady rain, wind, a distant storm
 *   NEAR     (mSoundLayer)   what the precipitation sounds like where you are standing
 *
 * <WolfViewer 2026-10-26> RECORDED RAIN. Residents did not like the old rain: it was filtered
 * noise in a 6 s mono loop that clicked at every wrap (region-terrain-work/rain-sound/README.md
 * in the firestorm repo has the measurements). The rain is now real CC0 recordings shipped with
 * the viewer in app_settings/wolfweather/ (SOURCES.md there: who, where, which file, its SHA-1):
 *   - a bed of light, moderate and heavy rain, crossfaded by the intensity with equal power;
 *   - a near layer (patter at a window, rain on leaves, rain on a roof);
 *   - single drops landing round the listener in heavy rain;
 *   - under a roof (WolfWeather::indoors) the open loops give way to low-passed copies;
 *   - every level eases, so rain starts, stops and changes without a hard switch;
 *   - each loop starts at a random point and wanders +-1.5 dB, so no repeat can be heard.
 * Wind and the thunder cracks are still synthesised (they were not the complaint), and the
 * synthesised voices remain the fallback if a recording is missing.
 * Every number in the mix is in region-terrain-work/rain-sound/tools/rain_mix.py, which also
 * renders the previews, and wolfstorm js/world/weather_sound.js carries the same numbers.
 *
 * HOW IT REACHES THE AUDIO ENGINE. The engine plays a sound by UUID, and before it asks the
 * asset server it looks for a decoded copy on disk:
 *   LLAudioData::LLAudioData  (llaudioengine.cpp:1920) — gAudiop->hasDecodedFile(uuid) sets
 *                                                        hasLocalData + hasDecodedData
 *   LLAudioEngine::hasDecodedFile (:999)               — <LL_PATH_FS_SOUND_CACHE>/<uuid>.dsf
 *   LLAudioData::load (:1934)                          — mBufferp->loadWAV(that path)
 * So each sound has a FIXED UUID of ours, and a plain 16-bit WAV is written to that path:
 * generated for the synthesised voices, decoded from the shipped Ogg Vorbis for the
 * recordings. The engine's own Vorbis decoder is NOT used for the recordings: it writes a
 * mono 44.1 kHz header whatever the file is and fades the first and last 128 samples to zero
 * (llaudiodecodemgr.cpp:315-330 and finishDecode "FUDGECAKES"), a tick at every loop point.
 * Nothing is ever requested from the grid's asset service.
 */
class WolfWeatherSound : public LLSingleton<WolfWeatherSound>
{
    LLSINGLETON(WolfWeatherSound);
    ~WolfWeatherSound();

public:
    /** The one entry point: wolfweather.cpp calls it after every precedence decision. */
    void apply(const WolfWeatherProfile& profile);
    /** Every frame: levels, fades, the indoor muffle, drops. */
    void idle();
    /**
     * [LIGHTNING 2026-09-13] One thunder clap, now. WolfLightning calls this when the sound of a
     * strike reaches the listener (bolt first, clap distance/340 m/s later); the scheduling
     * that used to live here (maybeThunder) moved there so the bolt and the clap are one event.
     * Does nothing while the weather sound is off, so a silent storm still forks silently.
     */
    void crack(bool distant);
    /** Fade everything out (the sources go once they are silent). */
    void stop();

    /** Sample rate and length of every generated loop. */
    static constexpr S32 SAMPLE_RATE = 22050;
    static constexpr S32 LOOP_SECS   = 6;
    /** A generated loop's tail is folded into its head with equal power over this long. */
    static constexpr F32 CROSSFADE_SECS = 0.35f;

    /**
     * One generated sound: its fixed UUID and the filter that shapes the noise. Public because
     * the voice TABLES live in wolfweathersound.cpp's anonymous namespace, where the UUID
     * strings have one home and nothing else can reach them.
     */
    struct Voice
    {
        const char* mId;
        const char* mUuid;
        F32 mRate;        ///< resampling factor; < 1 is deeper, > 1 brighter
        F32 mLowHz;       ///< band low edge (0 = no high-pass)
        F32 mHighHz;      ///< band high edge (0 = no low-pass)
        F32 mGain;        ///< the layer's own level before volume and intensity
    };

    /** One shipped recording: the file in app_settings/wolfweather and the UUIDs it is played as. */
    struct Recording
    {
        const char* mFile;
        const char* mUuid;          ///< the recording as it is (stereo, 2D)
        const char* mMuffledUuid;   ///< its under-a-roof copy (mono, low-passed); nullptr = none
        bool        mLoop;          ///< a seamless loop (started at a random point) or a one-shot drop
    };

    /** <WolfViewer 2026-10-26/> Source: rain_mix.py — the same numbers in both viewers. */
    static F32 intensity(const WolfWeatherProfile& p);
    static void tierWeights(F32 intensity, F32 out[3]);
    static F32 bedLevel(F32 vol, F32 intensity);
    static F32 nearLevel(F32 vol, F32 intensity);
    static F32 dripRate(F32 intensity);

private:
    static const Voice* findVoice(const Voice* table, const std::string& id);
    static const Voice* ambience(const std::string& id);
    static const Voice* nearLayer(const std::string& id);

    /** Make sure <sound cache>/<uuid>.dsf exists, writing the WAV if it does not. */
    static bool ensureSound(const Voice& v);
    static bool writeWav(const std::string& path, const Voice& v, bool oneShot);

    /** A recording's state this session: decoded and written once, off the main thread. */
    enum class Prep { NONE, PENDING, READY, FAILED };
    Prep prepare(const Recording& r);
    void onPrepared(const std::string& file, bool ok);

    /** One playing loop, eased towards its target level. */
    struct Layer
    {
        LLUUID mSound;
        LLUUID mSource;
        bool   mStereo2D = true;     ///< recordings: 2D (stereo kept); synthesised: at the avatar
        F32    mGain = 0.f;
        F32    mTarget = 0.f;
        bool   mSeen = false;        ///< given a target this frame
        F32    mDriftDb = 0.f;
        F32    mDriftTarget = 0.f;
        F64    mDriftNext = 0.0;
        F64    mRetryAt = 0.0;       ///< a source the engine would not start is retried, not spun on
    };
    void setTarget(const std::string& key, const LLUUID& sound, bool stereo2d, F32 target);
    void updateLayer(Layer& l, F32 dt, F64 now);
    void stopSource(Layer& l);
    void maybeDrip(F64 now, F32 intensity, F32 indoors);

    WolfWeatherProfile mProfile;
    bool   mWant = false;            ///< apply() wants sound; false = everything fading out
    std::map<std::string, Layer> mLayers;
    std::map<std::string, Prep>  mPrep;
    F64    mLastIdle = 0.0;
    F64    mNextDrip = 0.0;
};

#endif // WOLF_WEATHER_SOUND_H
