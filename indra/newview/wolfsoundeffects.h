/**
 * @file wolfsoundeffects.h
 * [SOUND EFFECTS 2026-11-06] Paul: "Wolf World -> Sound Effects allows the user to select weather sounds and teleport
 * sounds and volumes because i'm tired of all the 'its too loud its wrong' etc" (after ticket 568, Luke Koda: "would like
 * to be able to choose a [teleport] sound").
 *
 * One place that says how loud each group of Wolf sounds plays for this resident: a switch and a volume per group, on
 * top of the viewer's own Ambient / UI volumes (which still apply). The sound code asks level(group) wherever it sets a
 * gain; the Sound Effects window (wolffloatersoundeffects.cpp) changes the settings.
 *
 * The teleport sound: UISndTeleportOut stays the setting that holds it (as in Firestorm, so Preferences > Sounds still
 * shows it). WolfViewer ships a few teleport sounds of its own (app_settings/wolfteleport/, made for WolfViewer, see
 * SOURCES.md there), each played under a fixed UUID once decoded into the sound cache, the same way as the weather's
 * recordings (wolfweathersound.h Recording). A resident's own sound is any sound asset from their inventory.
 */
#ifndef WOLF_SOUND_EFFECTS_H
#define WOLF_SOUND_EFFECTS_H

#include "lluuid.h"
#include <string>
#include <vector>

namespace WolfSoundEffects
{
    enum Group { WEATHER, THUNDER, BIRDS, NATURE, SEA_LIFE, BUBBLES, TELEPORT, GROUP_COUNT };

    /** 0 when the group is switched off here, else its volume (0 to 1). */
    float level(Group group);

    /** The settings behind a group: its switch (BOOL) and its volume (F32, 0 to 1). */
    const char* switchSetting(Group group);
    const char* volumeSetting(Group group);

    /** A teleport sound WolfViewer ships, or the standard one. */
    struct TeleportSound
    {
        const char* mKey;      ///< the window's list value and the strings.xml-free label key
        const char* mLabel;    ///< what the window shows
        const char* mFile;     ///< in app_settings/wolfteleport/; nullptr = an asset on the grid (the standard sound)
        const char* mUuid;     ///< what UISndTeleportOut holds when it is chosen
    };
    const std::vector<TeleportSound>& teleportSounds();
    /** The shipped sound with this UUID, or nullptr (the standard sound counts as shipped). */
    const TeleportSound* findTeleportSound(const LLUUID& id);
    /** The Firestorm default of UISndTeleportOut (settings.xml). */
    const LLUUID& standardTeleportSound();

    /** Make a shipped sound playable (decode it into the sound cache, off the main thread the first time).
     *  True once it is ready; anything that is not one of ours is always "ready". */
    bool prepare(const LLUUID& id);

    /** Plays the teleport sound now, at the teleport volume, if it is switched on. Replaces
     *  make_ui_sound("UISndTeleportOut") at the three places a teleport starts. */
    void playTeleport();
    /** Plays a sound once as a sample in the window (preview), at the given volume. */
    void preview(const LLUUID& id, float volume);
}

#endif
