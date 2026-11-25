/**
 * @file wolfrecorder.h
 * @brief WolfViewer: the built-in Video Recorder - the 3D view (or the whole window) and the viewer's sound to an MP4.
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

#ifndef WOLF_RECORDER_H
#define WOLF_RECORDER_H

#include "stdtypes.h"
#include <string>
#include <vector>

class LLView;

// [W111 VIDEO 2026-11-24] Ticket 1033 (Blkchina Foggarty: "a way to do short videos"); Paul: "yes definitly".
//
// Who and why: a resident wants a short clip of what they see - dancing at a club with the music, a build, a ride - to
// share, with as few steps as possible (Record, Stop, Open folder).
//
// How it works, all three platforms alike:
//  - Pictures: once a frame, at most at the chosen frame rate, the finished picture is scaled on the GPU into a fixed
//    16:9 picture (1280x720 or 1920x1080, the window fitted inside) and read back through a ring of pixel buffers with
//    fences - asynchronously, never waiting on the GPU. A picture whose copy is still busy when its slot comes round is
//    dropped, not waited for. World only: right after renderFinalize (llviewerdisplay.cpp render_ui, the VR capture
//    point). Whole window: at the end of render_ui, after the UI and before the recording indicator, which is drawn
//    after the copy so it is never in the video.
//  - Sound, mixed into one track: the world (the OpenAL engine's own mix, through its mirror route - the media
//    plugins' sound plays through it too), voice chat (the WebRTC playout tap) and the microphone (OpenAL capture,
//    opened only while recording), each with its own level.
//  - One worker thread encodes (H.264 video, AAC or Opus sound) and writes the MP4; a bounded queue between the main
//    thread and it drops pictures when it falls behind, and the real frame rate is shown.
namespace WolfRecorder
{
    enum class State { IDLE, STARTING, RECORDING, FINISHING };

    // [W111 RECORDER UI 2026-11-24] the sound sources, in the order of levels() and of the window's rows
    enum Source { SOURCE_WORLD = 0, SOURCE_VOICE = 1, SOURCE_MIC = 2, SOURCE_COUNT = 3 };
    // the preview picture: 16:9, RGB, bottom row first (as a GL texture takes it)
    constexpr U32 PREVIEW_WIDTH = 256;
    constexpr U32 PREVIEW_HEIGHT = 144;
    // a recording stops by itself when the disk's free space falls under this (the window counts the time left to it)
    constexpr U64 DISK_STOP_BYTES = 100ull * 1024 * 1024;

    struct Report
    {
        State       state = State::IDLE;
        F64         seconds = 0.0;          // recorded so far (paused time not counted)
        F64         limit_seconds = 0.0;    // stops by itself here; 0 = never (only the disk-space guard)
        bool        paused = false;         // RECORDING, paused: no pictures and no sound are taken
        F64         length = 0.0;           // the last saved video's length in seconds (with saved)
        F32         fps = 0.f;              // pictures a second really written (last two seconds)
        U32         dropped = 0;            // pictures that could not be kept up with
        U64         bytes = 0;              // file size so far
        bool        world = false, voice = false, mic = false;   // sound being recorded
        std::string file;                   // the file being written, or the last one saved
        std::string encoder;                // "H.264 (Media Foundation)", ...
        std::string error;                  // last failure in plain words (empty: none); stays until the next start
        std::string notice;                 // last plain-words note (saved, stopped at the limit, ...)
        bool        saved = false;          // the last recording finished and its file is playable
    };

    /** Main thread. Starts recording with the current settings; false with `error` in plain words when it cannot. */
    bool start(std::string& error);
    /** Main thread. Stops; the worker finishes the file in the background (state FINISHING, then IDLE). */
    void stop();
    bool isRecording();      // STARTING or RECORDING (paused or not)
    Report status();

    /** [W111 RECORDER UI 2026-11-24] Main thread. Pause and carry on: while paused no picture is taken, the world and
     *  voice sound are not kept and the microphone capture is stopped; the video simply has no paused stretch (the
     *  pictures' times and the sound both run on a clock that stands still while paused). Only while RECORDING. */
    void pause();
    void resume();
    bool isPaused();

    /** Main thread. The loudest sample of each source since the last call (0..1+, after its level slider), then
     *  reset; all 0 when not recording or the source is off. */
    void levels(F32 out[SOURCE_COUNT]);

    /** Main thread. A small copy of a recent picture (PREVIEW_WIDTH x PREVIEW_HEIGHT RGB, bottom row first), made by
     *  the encoder thread from the pictures it already has (no extra copy from the graphics card). True with the
     *  picture when it is newer than `serial` (which is then updated). The last one stays after the recording. */
    bool previewPicture(U32& serial, std::vector<U8>& rgb);

    /** Main thread, from a view's draw(): true when a whole-window recording runs and this view must be kept out of
     *  the video - the view then returns without drawing. From then until the recording ends the recorder draws it
     *  itself, right after the picture copy, every frame (as it does the recording indicator) - also when the UI is
     *  cached (RenderUIBuffer). For the recorder's own window (Paul: "the recording window should never be recorded"). */
    bool holdBackFromVideo(LLView* view);

    /** True when the world sound can be recorded this session (the sound engine's mirror route is running). */
    bool worldSoundAvailable();
    /** True when WolfRecorderSoundWorld asks for the world sound but the route did not start with the viewer: the
     *  window says it takes effect after a restart. */
    bool worldSoundNeedsRestart();
    /** The folder videos go to (WolfRecorderFolder, or the computer's Videos/Movies folder + "/WolfViewer"). UTF-8. */
    std::string folder();
    /** Opens the folder in the computer's file manager. */
    void openFolder();

    /** llviewerdisplay.cpp render_ui, right after renderFinalize: the finished world (world-only recordings). */
    void captureWorld();
    /** llviewerdisplay.cpp render_ui, after the UI: the whole window (recordings with the interface), then the
     *  recording indicator over it (never in the video). */
    void captureWholeWindowAndDrawIndicator();
    /** wolfvrscreen.cpp keepWorldForBigScreen: Vulkan mode needs the world kept for the whole-window composite. */
    bool wantsWholeWindow();

    /** Before the sound engine and voice shut down (LLAppViewer::cleanup): finishes any recording and waits. */
    void shutdown();

    /** [W113 2026-11-25] Any thread (LLWebRTCVoiceClient::setMuteMic). The resident's own microphone mute in voice
     *  (mute button, push-to-talk not held, mic disabled). While muted the recorder's microphone track records
     *  silence - the same number of samples at the same times, so sound and pictures stay in step (Paul's rule: no
     *  WolfViewer tap gets microphone sound while the resident is muted). The voice client passes false while voice
     *  is off (no voice mute to obey then: the recorder's microphone records as before); false until it first says. */
    void setVoiceMicMuted(bool muted);
}

#endif // WOLF_RECORDER_H
