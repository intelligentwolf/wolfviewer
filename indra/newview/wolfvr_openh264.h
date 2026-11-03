/**
 * @file wolfvr_openh264.h
 * @brief WolfViewer VR pipe: H.264 with Cisco's OpenH264, loaded at run time (no link-time dependency), plus the
 *        tiny portable shared-library loader it uses. Internal to wolfvrpipe_video.cpp and the self-test.
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

#ifndef WOLF_VR_OPENH264_H
#define WOLF_VR_OPENH264_H

#include "wolfvrpipe_video.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wolfvr
{
    // [VR OPENH264 2026-11-03] Paul: Windows and macOS have no GStreamer, so they stream H.264 with Cisco's OpenH264.
    // Cisco pays the MPEG LA royalties only for its OWN prebuilt binary, downloaded to the resident's computer
    // (openh264/BINARY_LICENSE.txt), so the library is never shipped with the viewer: wolfvrcodecfetch.h downloads
    // it on first use, and this file opens it with dlopen / LoadLibraryW.

    /** A shared library opened at run time (dlopen on Linux/macOS, LoadLibraryExW on Windows). Paths are UTF-8. */
    class DynamicLibrary
    {
    public:
        DynamicLibrary() = default;
        ~DynamicLibrary();
        DynamicLibrary(const DynamicLibrary&) = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        /** False with `error` set (the system's own words) when the library cannot be opened. */
        bool open(const std::string& path, std::string& error);
        /** The address of `name`, or nullptr. */
        void* symbol(const char* name) const;
        void close();
        bool isOpen() const { return mHandle != nullptr; }

    private:
        void* mHandle = nullptr;
    };

    /** The OpenH264 library file the viewer downloaded from Cisco (wolfvr::openh264File in wolfvrcodecfetch.h, after
     *  wolfvr::openh264Installed said yes), or empty for none. Tried before the system library (Linux only). Any
     *  thread; takes effect the next time an encoder is opened, unless a library is already loaded (a loaded library
     *  stays loaded until the viewer exits: encoders made from it may still be running). */
    void setOpenH264Path(const std::string& path_utf8);

    /** "OpenH264 2.6.0 (<file>)" once a library is loaded, empty before. */
    std::string openh264LoadedDescription();

    /** Encoder settings that the self-test bench varies; openEncoder uses the defaults. */
    struct OpenH264Tuning
    {
        int threads = 0;            // 0 = this computer's cores, at most 4 (OpenH264 MAX_THREADS_NUM)
        int slices = 0;             // 0 = one per thread
        int complexity = 0;         // ECOMPLEXITY_MODE: 0 LOW, 1 MEDIUM, 2 HIGH
        int convert_threads = 0;    // RGBA -> I420 bands; 0 = same as threads
    };

    /** Opens the OpenH264 encoder directly (no GStreamer). False/nullptr with `error` set when the library is missing
     *  or refuses the settings. One RGBA picture in (top row first), one Annex B access unit out, no B-frames. */
    std::unique_ptr<Encoder> openOpenH264Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                                                 const OpenH264Tuning& tuning, std::string& error);
}

#endif // WOLF_VR_OPENH264_H
