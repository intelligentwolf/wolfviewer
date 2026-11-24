/**
 * @file wolfmediaaudio.h
 * @brief [W111 VIDEO 2026-11-24] Media plugin sound played in the viewer's audio engine.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_MEDIA_AUDIO_H
#define WOLF_MEDIA_AUDIO_H

namespace WolfMediaAudio
{
    /** Registers the viewer as the place media plugins send their sound (LLPluginClassMedia::setAudioHost), once the
     *  audio engine is up. Plugins made after this ask (setting WolfMediaAudioThroughViewer, engine support). */
    void registerHost();
}

#endif // WOLF_MEDIA_AUDIO_H
