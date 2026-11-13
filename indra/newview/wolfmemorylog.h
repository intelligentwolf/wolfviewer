/**
 * @file wolfmemorylog.h
 * @brief WolfViewer: a small memory line in the log once a minute (sooner when memory moves a lot), and the same
 *        figures for the crash report.
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

#ifndef WOLF_MEMORY_LOG_H
#define WOLF_MEMORY_LOG_H

#include <cstddef>

/**
 * [W106 MEMLOG 2026-11-13] Crash tickets 796, 799 and 838 (W103, Windows): two residents' viewers ran out of memory -
 * the log's last line was LLUserWarningMsg::showOutOfMemory's "request for memory failed" box, and 799's exception
 * record decodes (against the W103 CI build's own files) to a thrown std::bad_alloc ("bad allocation") from operator new
 * in a std::vector<float>::resize. 838's viewer had committed 27.7 GB on a 16 GB computer, 796/799's 15 GB, while their
 * working sets were 5.4 GB and 1.3 GB. What the logs could not say: whether the computer had run out of commit (RAM +
 * page file), how close it was, or the memory state at the moment of the crash.
 *
 * THE LOG LINE (idle, from llviewerdisplay.cpp display_stats; replaces the 2026-10-02 "WolfMemory" breakdown line,
 * which it keeps): once a minute, and early (at most every 15 s) when the process's resident or committed memory, or
 * the computer's available memory, has moved by 1 GB or more since the last line. A reading every 5 s decides that:
 * two system calls on Windows / macOS, two small /proc files on Linux. One line a minute is about 25 KB an hour of a
 * log that starts afresh each run.
 *
 * AT THE CRASH (wolfcrashreport.cpp): formatSystemNow writes the same process / system figures into crash_stack.txt
 * and formatLastViewer the viewer's own figures from the last line (textures, VRAM budget, objects), with their age.
 * Both are meant for a crashed process: fixed buffers, no allocation, no locks. On Linux only calls POSIX lists as
 * async-signal-safe (open / read / close, clock_gettime); on macOS the Mach and sysctl queries are not on that list and
 * are best effort; on Windows a healthy writer thread calls them.
 */
namespace WolfMemoryLog
{
    /// From display_stats every frame (main thread). Takes a reading every 5 s; writes the line when one is due. In a
    /// second copy of WolfViewer that reading also decides the once-per-start low-memory notice
    /// (WolfSecondCopyLowMemory, wolfmemorylog.cpp second_copy_notice).
    void idle();

    /// The process and system memory now, as one line of text without a newline ("process: ... | system: ...").
    /// at_crash: also the system-wide commit figures that take too long to read every minute (Windows
    /// GetPerformanceInfo, SL-12122). Crash-safe. Returns the length written (buf is always NUL-terminated).
    size_t formatSystemNow(char* buf, size_t cap, bool at_crash);

    /// The viewer's own figures from the last log line and how many seconds ago it was written; 0 (empty) when no
    /// line has been written yet. Crash-safe.
    size_t formatLastViewer(char* buf, size_t cap);

    /// [W106 MEMLOG 2026-11-13] For the second-copy notice: whether the copy that was already open when this one
    /// started still holds its exec marker lock, tested again now (this run's isSecondInstance() is decided once at
    /// start). Defined in llappviewer.cpp beside processMarkerFiles, which owns the marker's name and rules.
    bool anotherCopyStillOpen();
}

#endif // WOLF_MEMORY_LOG_H
