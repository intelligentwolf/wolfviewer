/**
 * @file wolfcrashreport.h
 * @brief WolfViewer: when the viewer crashes, write what support needs; next time, send it as a staff ticket.
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

#ifndef WOLF_CRASH_REPORT_H
#define WOLF_CRASH_REPORT_H

/**
 * [CRASH REPORTS 2026-10-28] Paul: "windows crash reports -> tickets".
 *
 * WHY. WolfViewer is built without BugSplat (wolfviewer.yml sets no BUGSPLAT_DB, so cmake/bugsplat.cmake leaves
 * USE_BUGSPLAT off), and llappviewerwin32.cpp turns Windows Error Reporting off for the viewer. Its own minidump and
 * log-copy code (bugsplatSendLog) is BugSplat-only, so a crash left nothing but static_debug_info.log in the run's
 * dump folder: no stack, no dump, nothing sent.
 *
 * AT THE CRASH (installHandler, not in BugSplat builds). Into this run's dump folder (LL_PATH_DUMP, "dump-<uuid>" in
 * the logs folder, which LLAppViewer::removeDumpDir deletes on a clean exit, so a folder left behind is a run that
 * did not end cleanly):
 *  - crash_stack.txt: the exception or signal, the faulting address and up to 64 frames, each as
 *    "<module file name>+0x<offset>" (no folder names, no memory contents);
 *  - Windows only, WolfViewer.dmp: a small minidump (MiniDumpNormal | WithThreadInfo | WithUnloadedModules: the
 *    threads' stacks and the module list, no heap), written by a thread started at start-up for the purpose;
 *  - a copy of WolfViewer.log as it stood (the last 8 MB on Linux and macOS), so the next start-up's ".old" rename
 *    cannot lose it. Not for a second instance, which has no log of its own.
 *
 * NEXT TIME (sendPendingOnce, once logged in to Wolf Territories). Up to MAX_PER_RUN folders from the last 7 days
 * that hold crash files and no crash_report.txt are packed (the log's last 4 MB scrubbed with the Flea Report's own
 * rules, wolflogscrub.cpp; a whitelist of static_debug_info.log's fields, never its file paths or login name) and
 * posted to wolf-grid.com/viewercrashreport.php, which raises or adds to a STAFF-ONLY ticket. crash_report.txt then
 * records the result so a crash is sent once. A small tip says it happened. Preferences > Privacy > General
 * "Send crash reports to Wolf Territories support automatically" (WolfCrashReportsAuto, default on) turns it off;
 * crashes found while it is off are marked and never sent later.
 */
namespace WolfCrashReport
{
    /// From LLAppViewer::init, before anything else can crash it: get ready to write the crash files into this
    /// run's dump folder. Does nothing in a BugSplat build (BugSplat writes its own).
    void installHandler(bool second_instance);

#if LL_WINDOWS
    /// From LLAppViewerWin32::reportCrashToBugsplat, i.e. the structured-exception filters of LLThread, LLCoros and
    /// the work queues, while the faulting stack is still there. Writes the crash files and returns true (the
    /// filter then lets the exception carry on to end the process, as it does in a BugSplat build); returns false
    /// for a C++ exception (left to its own handling, as before) or when the handler is not installed.
    bool writeForException(void* exception_pointers);
#endif

    /// From llstartup.cpp at STATE_STARTED: send any crash left by an earlier run. Only the first call does anything.
    void sendPendingOnce();
}

#endif // WOLF_CRASH_REPORT_H
