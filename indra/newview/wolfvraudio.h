/**
 * @file wolfvraudio.h
 * @brief WolfViewer: the viewer's sound to the VR headset, and the headset's microphone into voice chat.
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

#ifndef WOLF_VRAUDIO_H
#define WOLF_VRAUDIO_H

#include <string>

// [VR AUDIO 2026-11-04] Paul: "don't forget audio, what do we do with it? move it to the headset?"
// While a headset page plays the sound ({"t":"audio","on":true}), the viewer's whole mix goes to it as 10 ms
// blocks of 48 kHz stereo 16-bit PCM (WolfVRStream::submitAudio):
//   - the sound engine's final mix (world sounds, UI sounds, wind, gestures, Wolf DJ / speech sources): OpenAL
//     loopback mirror, llaudioengine_openal.h setMirrorMix (only when WolfVRStreamEnabled was on at start);
//   - voice chat: the WebRTC playout tap (llwebrtc.h setVRPlayoutTap), mixed into the engine's blocks.
// WolfVRAudioOut "headset" silences the PC speakers for both (engine: setSpeakersMuted; voice: setPlayoutSilenced)
// and gives them back the moment the headset stops wanting sound; "both" plays on both; "pc" sends nothing.
// WolfVRHeadsetMic: the headset's microphone replaces the PC's for voice chat (llwebrtc.h LLWebRTCExternalMic).
namespace WolfVRAudio
{
    /** Main thread, every frame (after WolfVRStream's own pump): applies the settings and the headset's state. */
    void pump();
    /** Removes every tap (engine, voice, microphone) and gives the PC speakers back. Before the sound engine and
     *  voice shut down (LLAppViewer::cleanup). */
    void shutdown();
    /** A plain sentence when the headset cannot get all of the sound right now (empty when all is well). */
    std::string problem();
    /** For the readout: true while sound blocks are going to the headset; whether the microphone is in use. */
    bool sending();
    bool micInUse();
}

#endif // WOLF_VRAUDIO_H
