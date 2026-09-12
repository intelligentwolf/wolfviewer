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

#include <string>

#include "lluuid.h"
#include "llsingleton.h"
#include "wolfweatherprofile.h"

/**
 * Two looping layers and an occasional thunder crack, exactly as the Weather tab's two Sound
 * Presets dropdowns describe:
 *   AMBIENCE (mSoundPreset)  the bed  — thunderstorms, steady rain, wind, a distant storm
 *   NEAR     (mSoundLayer)   what the precipitation sounds like where you are standing
 *
 * SYNTHESISED, NOT SHIPPED AS ASSETS, and for the same reason as the web viewer's
 * weather_sound.js: a sampled rain loop is a large asset that has to be uploaded to the grid,
 * kept in step between the two viewers, and goes silent the day it is missing. Filtered noise
 * IS rain, it costs one buffer, and it can be generated on the machine that plays it.
 *
 * HOW IT REACHES THE AUDIO ENGINE. The engine plays a sound by UUID, and before it asks the
 * asset server it looks for a decoded copy on disk:
 *   LLAudioData::LLAudioData  (llaudioengine.cpp:1920) — gAudiop->hasDecodedFile(uuid) sets
 *                                                        hasLocalData + hasDecodedData
 *   LLAudioEngine::hasDecodedFile (:999)               — <LL_PATH_FS_SOUND_CACHE>/<uuid>.dsf
 *   LLAudioData::load (:1934)                          — mBufferp->loadWAV(that path)
 * So each layer has a FIXED UUID of ours, and the first time it is wanted a plain 16-bit mono
 * WAV is written to that path. Nothing is ever requested from the grid's asset service, and a
 * cleared cache simply means it is written again.
 */
class WolfWeatherSound : public LLSingleton<WolfWeatherSound>
{
    LLSINGLETON(WolfWeatherSound);
    ~WolfWeatherSound();

public:
    /** The one entry point: wolfweather.cpp calls it after every precedence decision. */
    void apply(const WolfWeatherProfile& profile);
    /** Every frame: keeps the loops on the listener and schedules the thunder. */
    void idle();
    /** Silence, and let the sources go. */
    void stop();

    /** Sample rate and length of every generated loop. */
    static constexpr S32 SAMPLE_RATE = 22050;
    static constexpr S32 LOOP_SECS   = 6;
    /** The loop's last CROSSFADE_SECS are mixed into its first, so the seam is inaudible. */
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

private:
    static const Voice* findVoice(const Voice* table, const std::string& id);
    static const Voice* ambience(const std::string& id);
    static const Voice* nearLayer(const std::string& id);

    /** Make sure <sound cache>/<uuid>.dsf exists, writing the WAV if it does not. */
    static bool ensureSound(const Voice& v);
    static bool writeWav(const std::string& path, const Voice& v, bool oneShot);

    /** Start (or re-target) one looping layer. Returns the audio-source id, null on failure. */
    LLUUID startLoop(const Voice& v, F32 gain);
    void   setLoopGain(const LLUUID& source_id, F32 gain);
    void   stopLoop(LLUUID& source_id);

    void   maybeThunder();

    WolfWeatherProfile mProfile;
    bool   mPlaying = false;
    LLUUID mAmbSource;          ///< the audio-source id of the ambience loop
    LLUUID mNearSource;
    std::string mAmbId;         ///< which voice each source is currently playing
    std::string mNearId;
    F64    mNextThunder = 0.0;
};

#endif // WOLF_WEATHER_SOUND_H
