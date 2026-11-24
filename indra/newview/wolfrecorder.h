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

    struct Report
    {
        State       state = State::IDLE;
        F64         seconds = 0.0;          // recorded so far
        F64         limit_seconds = 0.0;    // stops by itself here
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
    bool isRecording();      // STARTING or RECORDING
    Report status();

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
}

#endif // WOLF_RECORDER_H
