/**
 * @file wolfsoundeffects.cpp
 * [SOUND EFFECTS 2026-11-06] See wolfsoundeffects.h.
 */
#include "llviewerprecompiledheaders.h"

#include "wolfsoundeffects.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llviewercontrol.h"
#include "wolfweathersound.h"

namespace
{
    // Source: settings.xml - the groups' switches. The four that existed before (birds, nature, dolphins and whales,
    // bubbles) are the same settings their own code already reads, so turning one off here is turning it off there.
    // PlayModeUISndTeleportOut is Firestorm's own teleport sound switch (llui.cpp find_ui_sound reads "PlayMode" + name).
    const char* const SWITCHES[WolfSoundEffects::GROUP_COUNT] = {
        "WolfSoundWeather", "WolfSoundThunder", "WolfViewerBirdsSound", "WolfViewerNatureSounds",
        "WolfViewerSeaLifeSound", "WolfViewerSwimBubblesSound", "PlayModeUISndTeleportOut",
    };
    const char* const VOLUMES[WolfSoundEffects::GROUP_COUNT] = {
        "WolfSoundWeatherVolume", "WolfSoundThunderVolume", "WolfSoundBirdsVolume", "WolfSoundNatureVolume",
        "WolfSoundSeaLifeVolume", "WolfSoundBubblesVolume", "WolfSoundTeleportVolume",
    };

    // Source: settings.xml UISndTeleportOut, its Value (Firestorm's default teleport sound, an asset on every grid).
    const LLUUID STANDARD_TP("d7a9a565-a013-2a69-797d-5332baa1a947");

    // The UUIDs are WolfViewer's own (made 2026-11-06), never an asset on any grid: the sound is written into the sound
    // cache under them (wolfweathersound.h Recording), and the audio engine plays it from there.
    const std::vector<WolfSoundEffects::TeleportSound>& tp_table()
    {
        static const std::vector<WolfSoundEffects::TeleportSound> t = {
            { "standard", "Standard",      nullptr,               "d7a9a565-a013-2a69-797d-5332baa1a947" },
            { "shimmer",  "Soft shimmer",  "wolf_tp_shimmer.ogg", "6b0ddda2-bfbb-49db-8a81-01e53ebe9d51" },
            { "whoosh",   "Gentle whoosh", "wolf_tp_whoosh.ogg",  "02a3dea9-22ae-4e8e-a11d-3f46908b6a5a" },
            { "chime",    "Crystal chime", "wolf_tp_chime.ogg",   "ee0d9c15-7e32-4e18-b050-4e6b68533d5d" },
            { "warp",     "Warp",          "wolf_tp_warp.ogg",    "9e777e16-3ddb-47e9-b88d-9c2a12aa3bdb" },
            { "sparkle",  "Magic sparkle", "wolf_tp_sparkle.ogg", "f6636eb8-f718-45b9-8055-c92a00e5aa1c" },
            { "pop",      "Quiet pop",     "wolf_tp_pop.ogg",     "0a1d8481-7456-4ef8-aa7c-37c5e39f22d5" },
        };
        return t;
    }

    // One Recording per shipped sound, for WolfWeatherSound::recordingReady (a one-shot: no loop, no muffled copy).
    const WolfWeatherSound::Recording* recording_for(const WolfSoundEffects::TeleportSound& s)
    {
        static std::vector<WolfWeatherSound::Recording> recs;
        if (recs.empty())
        {
            for (const auto& t : tp_table())
            {
                WolfWeatherSound::Recording r = { t.mFile, t.mUuid, nullptr, false };
                r.mDir = "wolfteleport";
                recs.push_back(r);
            }
        }
        for (size_t i = 0; i < tp_table().size(); ++i)
        {
            if (&tp_table()[i] == &s) return s.mFile ? &recs[i] : nullptr;
        }
        return nullptr;
    }

    void play(const LLUUID& id, float gain)
    {
        if (!gAudiop || id.isNull() || gain <= 0.f) return;
        // Source: llappviewer.cpp ui_audio_callback - a UI sound is the agent's own, AUDIO_TYPE_UI (the UI volume).
        gAudiop->triggerSound(id, gAgent.getID(), llclamp(gain, 0.f, 1.f), LLAudioEngine::AUDIO_TYPE_UI);
    }
}

namespace WolfSoundEffects
{
    const char* switchSetting(Group g) { return (g >= 0 && g < GROUP_COUNT) ? SWITCHES[g] : ""; }
    const char* volumeSetting(Group g) { return (g >= 0 && g < GROUP_COUNT) ? VOLUMES[g] : ""; }

    float level(Group g)
    {
        if (g < 0 || g >= GROUP_COUNT) return 1.f;
        // Read every call (a handful a second at most), not cached per group: the window changes them live.
        if (!gSavedSettings.getBOOL(SWITCHES[g])) return 0.f;
        return llclamp(gSavedSettings.getF32(VOLUMES[g]), 0.f, 1.f);
    }

    const std::vector<TeleportSound>& teleportSounds() { return tp_table(); }

    const TeleportSound* findTeleportSound(const LLUUID& id)
    {
        for (const auto& t : tp_table())
        {
            if (LLUUID(t.mUuid) == id) return &t;
        }
        return nullptr;
    }

    const LLUUID& standardTeleportSound() { return STANDARD_TP; }

    bool prepare(const LLUUID& id)
    {
        const TeleportSound* s = findTeleportSound(id);
        if (!s || !s->mFile) return true;              // an asset: the audio engine fetches it itself
        const WolfWeatherSound::Recording* r = recording_for(*s);
        return r && WolfWeatherSound::instance().recordingReady(*r);
    }

    void playTeleport()
    {
        // The same two tests llui.cpp find_ui_sound makes for "UISndTeleportOut": its PlayMode switch, and a null UUID
        // meaning silent - plus the volume set in the Sound Effects window.
        const float gain = level(TELEPORT);
        if (gain <= 0.f) return;
        const LLUUID id(gSavedSettings.getString("UISndTeleportOut"));
        if (id.isNull()) return;
        if (!prepare(id))
        {
            // Still being decoded (the first teleport after a new choice, before the window warmed it): the standard
            // sound this once rather than silence.
            LL_INFOS("WolfSoundEffects") << "teleport sound " << id << " not ready yet; the standard sound plays this time" << LL_ENDL;
            play(STANDARD_TP, gain);
            return;
        }
        play(id, gain);
    }

    void preview(const LLUUID& id, float volume)
    {
        play(id, volume);
    }
}
