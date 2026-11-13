/**
 * @file wolfmemorylog.cpp
 * @brief WolfViewer: the periodic memory line and the crash report's memory figures. See wolfmemorylog.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfmemorylog.h"

#include <cstring>               // memset

#include "llcharacter.h"         // LLCharacter::sInstances
#include "llframetimer.h"
#include "llgl.h"                // gGLManager.mVRAM
#include "llimage.h"             // LLImageFormatted::sGlobalFormattedMemory, LLImageRaw::sRawImageCount
#include "llimagegl.h"           // LLImageGL::getTextureBytesAllocated
#include "llvertexbuffer.h"      // LLVertexBuffer::getBytesAllocated
#include "llviewerobjectlist.h"  // gObjectList
#include "llviewertexture.h"     // LLViewerTexture::sFreeVRAMMegabytes, sDesiredDiscardBias
#include "llviewertexturelist.h" // gTextureList

#if LL_WINDOWS
#include "llwin32headers.h"
#include <psapi.h>               // GetProcessMemoryInfo, GetPerformanceInfo (psapi is linked: cmake/Linking.cmake)
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#if LL_DARWIN
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#endif

namespace
{
    // ---------------------------------------------------------------------------------------------------------------
    // Text into a fixed buffer: no allocation, so the crash handler can use it too.
    // ---------------------------------------------------------------------------------------------------------------
    struct Text
    {
        char* mBuf;
        size_t mCap;
        size_t mLen;
    };

    Text text_on(char* buf, size_t cap)
    {
        Text t = { buf, cap, 0 };
        if (cap > 0) buf[0] = '\0';
        return t;
    }

    void put(Text& t, const char* s)
    {
        while (*s && t.mLen + 1 < t.mCap) t.mBuf[t.mLen++] = *s++;
        if (t.mCap > 0) t.mBuf[t.mLen] = '\0';
    }

    void put_dec(Text& t, unsigned long long v)
    {
        char d[24];
        int n = 0;
        do
        {
            d[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        } while (v && n < 20);
        char s[24];
        int k = 0;
        while (n > 0) s[k++] = d[--n];
        s[k] = '\0';
        put(t, s);
    }

    void put_signed(Text& t, long long v)
    {
        if (v < 0)
        {
            put(t, "-");
            put_dec(t, static_cast<unsigned long long>(-(v + 1)) + 1ULL);
        }
        else
        {
            put_dec(t, static_cast<unsigned long long>(v));
        }
    }

    void put_mb(Text& t, unsigned long long bytes)
    {
        put_dec(t, bytes >> 20);
        put(t, " MB");
    }

    // Seconds since some fixed point (not the wall clock). GetTickCount64 / clock_gettime(CLOCK_MONOTONIC) are both
    // usable in a crashed process (clock_gettime is on POSIX's async-signal-safe list).
    unsigned long long monotonic_seconds()
    {
#if LL_WINDOWS
        return GetTickCount64() / 1000ULL;
#else
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
        return static_cast<unsigned long long>(ts.tv_sec);
#endif
    }

    // ---------------------------------------------------------------------------------------------------------------
    // One reading of the process and the computer. mResident / mPrivate / mAvail are what the 5-second check compares.
    // ---------------------------------------------------------------------------------------------------------------
    struct Reading
    {
        bool mProcessOk = false;
        bool mSystemOk = false;
        unsigned long long mResident = 0;   // Windows WorkingSetSize, Linux statm resident, macOS resident_size
        unsigned long long mPrivate = 0;    // Windows PagefileUsage (commit charge), Linux statm data, macOS phys_footprint
        unsigned long long mTotal = 0;      // physical memory
        unsigned long long mAvail = 0;      // Windows ullAvailPhys, Linux MemAvailable, macOS free + inactive pages
        // Windows: ullTotalPageFile (the commit limit for this process) and ullAvailPageFile (what it can still commit);
        // at a crash also GetPerformanceInfo's system-wide CommitTotal / CommitLimit / CommitPeak.
        // Linux: Committed_AS / CommitLimit as mSysCommit / mSysLimit.
        unsigned long long mCommitLimit = 0;
        unsigned long long mCommitAvail = 0;
        bool mSysOk = false;
        unsigned long long mSysCommit = 0;
        unsigned long long mSysLimit = 0;
        unsigned long long mSysPeak = 0;
        // Linux SwapTotal / SwapFree; macOS vm.swapusage xsu_total / xsu_used
        bool mSwapOk = false;
        unsigned long long mSwapTotal = 0;
        unsigned long long mSwapOther = 0;
    };

#if LL_LINUX
    // Page size for /proc/self/statm (counted in pages: proc_pid_statm(5)). Read once when the program loads, as
    // sysconf is not on the async-signal-safe list.
    const long sPageSize = sysconf(_SC_PAGESIZE);

    // A small /proc file into buf (NUL-terminated); false if it could not be read. open / read / close only.
    bool read_proc(const char* path, char* buf, size_t cap)
    {
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        size_t used = 0;
        while (used + 1 < cap)
        {
            const ssize_t n = read(fd, buf + used, cap - 1 - used);
            if (n <= 0) break;
            used += static_cast<size_t>(n);
        }
        close(fd);
        buf[used] = '\0';
        return used > 0;
    }

    // /proc/meminfo "Key:   12345 kB" (proc_meminfo(5)) -> bytes; false when the key is not there.
    bool meminfo_value(const char* text, const char* key, unsigned long long& bytes)
    {
        const char* line = text;
        while (*line)
        {
            const char* k = key;
            const char* p = line;
            while (*k && *p == *k) { ++k; ++p; }
            if (!*k && *p == ':')
            {
                ++p;
                while (*p == ' ' || *p == '\t') ++p;
                unsigned long long v = 0;
                bool any = false;
                while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<unsigned long long>(*p - '0'); ++p; any = true; }
                if (!any) return false;
                bytes = v * 1024ULL;   // every field we read is in kB (proc_meminfo(5))
                return true;
            }
            while (*line && *line != '\n') ++line;
            if (*line == '\n') ++line;
        }
        return false;
    }

    // The n-th (0-based) space-separated number of /proc/self/statm.
    unsigned long long statm_field(const char* text, int n)
    {
        const char* p = text;
        for (int i = 0; i < n; ++i)
        {
            while (*p >= '0' && *p <= '9') ++p;
            while (*p == ' ') ++p;
        }
        unsigned long long v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + static_cast<unsigned long long>(*p - '0'); ++p; }
        return v;
    }
#endif

#if LL_DARWIN
    // One send right to the host port for the whole run: mach_host_self() adds a user reference every call.
    const mach_port_t sHostPort = mach_host_self();
#endif

    void read_now(Reading& r, bool at_crash)
    {
#if LL_WINDOWS
        PROCESS_MEMORY_COUNTERS pmc;
        memset(&pmc, 0, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            // Source: learn.microsoft.com PROCESS_MEMORY_COUNTERS - WorkingSetSize "the current working set size, in
            // bytes"; PagefileUsage "the Commit Charge value in bytes for this process" (what LLMemory logs as
            // "allocated page size", llmemory.cpp updateMemoryInfo)
            r.mResident = pmc.WorkingSetSize;
            r.mPrivate = pmc.PagefileUsage;
            r.mProcessOk = true;
        }
        MEMORYSTATUSEX ms;
        memset(&ms, 0, sizeof(ms));
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms))
        {
            // Source: learn.microsoft.com MEMORYSTATUSEX - ullAvailPhys "can be immediately reused"; ullTotalPageFile
            // "the current committed memory limit for the system or the current process, whichever is smaller";
            // ullAvailPageFile "the maximum amount of memory the current process can commit"
            r.mTotal = ms.ullTotalPhys;
            r.mAvail = ms.ullAvailPhys;
            r.mCommitLimit = ms.ullTotalPageFile;
            r.mCommitAvail = ms.ullAvailPageFile;
            r.mSystemOk = true;
        }
        if (at_crash)
        {
            // Not every minute: SL-12122 (llsys.cpp loadStatsMap) measured GetPerformanceInfo at about 10 ms.
            PERFORMANCE_INFORMATION pi;
            memset(&pi, 0, sizeof(pi));
            pi.cb = sizeof(pi);
            if (GetPerformanceInfo(&pi, sizeof(pi)))
            {
                // Source: learn.microsoft.com PERFORMANCE_INFORMATION - CommitTotal / CommitLimit / CommitPeak are in
                // pages, PageSize "the size of a page, in bytes"
                r.mSysCommit = static_cast<unsigned long long>(pi.CommitTotal) * pi.PageSize;
                r.mSysLimit = static_cast<unsigned long long>(pi.CommitLimit) * pi.PageSize;
                r.mSysPeak = static_cast<unsigned long long>(pi.CommitPeak) * pi.PageSize;
                r.mSysOk = true;
            }
        }
#elif LL_LINUX
        (void)at_crash;
        char buf[4096];
        if (sPageSize > 0 && read_proc("/proc/self/statm", buf, sizeof(buf)))
        {
            // Source: proc_pid_statm(5) - in pages: (2) resident, (6) data + stack
            r.mResident = statm_field(buf, 1) * static_cast<unsigned long long>(sPageSize);
            r.mPrivate = statm_field(buf, 5) * static_cast<unsigned long long>(sPageSize);
            r.mProcessOk = true;
        }
        if (read_proc("/proc/meminfo", buf, sizeof(buf)))
        {
            // Source: proc_meminfo(5). MemAvailable (Linux 3.14+) as llsys.cpp getAvailableMemoryKB prefers it.
            r.mSystemOk = meminfo_value(buf, "MemTotal", r.mTotal)
                && (meminfo_value(buf, "MemAvailable", r.mAvail) || meminfo_value(buf, "MemFree", r.mAvail));
            r.mSysOk = meminfo_value(buf, "Committed_AS", r.mSysCommit) && meminfo_value(buf, "CommitLimit", r.mSysLimit);
            r.mSwapOk = meminfo_value(buf, "SwapTotal", r.mSwapTotal) && meminfo_value(buf, "SwapFree", r.mSwapOther);
        }
#elif LL_DARWIN
        (void)at_crash;
        task_vm_info_data_t vmi;
        memset(&vmi, 0, sizeof(vmi));
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vmi), &count) == KERN_SUCCESS)
        {
            // Source: xnu osfmk/mach/task_info.h struct task_vm_info - resident_size; phys_footprint was "added for
            // rev1", so it is used only when the kernel filled that far (count is in natural_t units, TASK_VM_INFO_COUNT)
            r.mResident = vmi.resident_size;
            const size_t filled = static_cast<size_t>(count) * sizeof(natural_t);
            r.mPrivate = filled >= offsetof(task_vm_info_data_t, phys_footprint) + sizeof(vmi.phys_footprint) ? vmi.phys_footprint : 0;
            r.mProcessOk = true;
        }
        vm_statistics64_data_t vm;
        memset(&vm, 0, sizeof(vm));
        mach_msg_type_number_t vcount = HOST_VM_INFO64_COUNT;
        int mib_mem[2] = { CTL_HW, HW_MEMSIZE };   // Source: xnu bsd/sys/sysctl.h HW_MEMSIZE "uint64_t: physical ram size"
        uint64_t memsize = 0;
        size_t len = sizeof(memsize);
        if (host_statistics64(sHostPort, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &vcount) == KERN_SUCCESS
            && sysctl(mib_mem, 2, &memsize, &len, nullptr, 0) == 0)
        {
            // free + inactive pages: what llsys.cpp LLMemoryInfo::getAvailableMemoryKB counts as available on macOS
            r.mAvail = (static_cast<unsigned long long>(vm.free_count) + vm.inactive_count) * vm_page_size;
            r.mTotal = memsize;
            r.mSystemOk = true;
        }
        int mib_swap[2] = { CTL_VM, VM_SWAPUSAGE };   // Source: xnu bsd/sys/sysctl.h VM_SWAPUSAGE, struct xsw_usage
        struct xsw_usage sw;
        memset(&sw, 0, sizeof(sw));
        len = sizeof(sw);
        if (sysctl(mib_swap, 2, &sw, &len, nullptr, 0) == 0)
        {
            r.mSwapTotal = sw.xsu_total;
            r.mSwapOther = sw.xsu_used;
            r.mSwapOk = true;
        }
#else
        (void)r;
        (void)at_crash;
#endif
    }

    void format_reading(Text& t, const Reading& r)
    {
        put(t, "process: ");
        if (r.mProcessOk)
        {
#if LL_WINDOWS
            put(t, "working set "); put_mb(t, r.mResident);
            put(t, ", committed "); put_mb(t, r.mPrivate);
#elif LL_DARWIN
            put(t, "resident "); put_mb(t, r.mResident);
            put(t, ", footprint "); put_mb(t, r.mPrivate);
#else
            put(t, "resident "); put_mb(t, r.mResident);
            put(t, ", data+stack "); put_mb(t, r.mPrivate);
#endif
        }
        else
        {
            put(t, "(not read)");
        }
        put(t, " | system: ");
        if (r.mSystemOk)
        {
            put(t, "available "); put_mb(t, r.mAvail);
            put(t, " of "); put_mb(t, r.mTotal);
        }
        else
        {
            put(t, "(not read)");
        }
#if LL_WINDOWS
        if (r.mSystemOk)
        {
            put(t, ", commit limit "); put_mb(t, r.mCommitLimit);
            put(t, ", this process can still commit "); put_mb(t, r.mCommitAvail);
        }
        if (r.mSysOk)
        {
            put(t, ", committed by all "); put_mb(t, r.mSysCommit);
            put(t, " of "); put_mb(t, r.mSysLimit);
            put(t, " (peak since boot "); put_mb(t, r.mSysPeak); put(t, ")");
        }
#else
        if (r.mSysOk)
        {
            put(t, ", committed "); put_mb(t, r.mSysCommit);
            put(t, " of limit "); put_mb(t, r.mSysLimit);
        }
        if (r.mSwapOk)
        {
#if LL_DARWIN
            put(t, ", swap used "); put_mb(t, r.mSwapOther);
#else
            put(t, ", swap free "); put_mb(t, r.mSwapOther);
#endif
            put(t, " of "); put_mb(t, r.mSwapTotal);
        }
#endif
    }

    // ---------------------------------------------------------------------------------------------------------------
    // The viewer's own figures from the last line, kept for the crash report (plain numbers: a torn read in a crash
    // costs at most one wrong figure).
    // ---------------------------------------------------------------------------------------------------------------
    struct ViewerFigures
    {
        bool mValid;
        unsigned long long mWhen;          // monotonic_seconds()
        unsigned long long mGLTextures;    // bytes, LLImageGL::getTextureBytesAllocated
        unsigned long long mVertexBuffers; // bytes, LLVertexBuffer::getBytesAllocated (the VBO pool's VRAM)
        long long mTextures;               // gTextureList.getNumImages
        long long mVRAM;                   // MB, gGLManager.mVRAM (detected or set)
        long long mBudgetFree;             // MB, LLViewerTexture::sFreeVRAMMegabytes (left under the texture target)
        long long mBiasHundredths;         // LLViewerTexture::sDesiredDiscardBias x 100
        long long mObjects;                // gObjectList.getNumObjects
        long long mAvatars;                // LLCharacter::sInstances
    };
    ViewerFigures sViewer = {};

    void put_bias(Text& t, long long hundredths)
    {
        if (hundredths < 0) hundredths = 0;
        put_dec(t, static_cast<unsigned long long>(hundredths / 100));
        put(t, ".");
        const long long frac = hundredths % 100;
        if (frac < 10) put(t, "0");
        put_dec(t, static_cast<unsigned long long>(frac));
    }

    void format_viewer(Text& t, const ViewerFigures& v)
    {
        put(t, "GL textures "); put_mb(t, v.mGLTextures);
        put(t, ", "); put_signed(t, v.mTextures); put(t, " textures");
        put(t, " | vertex buffers "); put_mb(t, v.mVertexBuffers);
        put(t, " | VRAM "); put_signed(t, v.mVRAM); put(t, " MB, texture budget free ");
        put_signed(t, v.mBudgetFree); put(t, " MB, discard bias "); put_bias(t, v.mBiasHundredths);
        put(t, " | objects "); put_signed(t, v.mObjects);
        put(t, " | avatars "); put_signed(t, v.mAvatars);
    }

    const unsigned long long GB = 1ULL << 30;

    bool moved(unsigned long long a, unsigned long long b)
    {
        return (a > b ? a - b : b - a) >= GB;
    }

    // The 5-second check: has the process or the computer moved by 1 GB since the last line?
    bool moved_a_lot(const Reading& last, const Reading& now)
    {
        if (last.mProcessOk && now.mProcessOk && (moved(last.mResident, now.mResident) || moved(last.mPrivate, now.mPrivate)))
        {
            return true;
        }
        if (last.mSystemOk && now.mSystemOk && moved(last.mAvail, now.mAvail))
        {
            return true;
        }
#if LL_WINDOWS
        // what decides a bad_alloc on Windows: the commit this process has left (796/799 and 838 ran out of it)
        if (last.mSystemOk && now.mSystemOk && moved(last.mCommitAvail, now.mCommitAvail))
        {
            return true;
        }
#endif
        return false;
    }

    const F32 CHECK_SECONDS = 5.f;
    const F32 LINE_SECONDS = 60.f;
    const F32 EARLY_MIN_SECONDS = 15.f;
}

void WolfMemoryLog::idle()
{
    static LLFrameTimer s_check;
    static LLFrameTimer s_since_line;
    static bool s_started = false;
    static Reading s_last;

    if (s_started && s_check.getElapsedTimeF32() < CHECK_SECONDS) return;
    s_check.reset();

    Reading now;
    read_now(now, false);
    const F32 since = s_since_line.getElapsedTimeF32();
    const bool due = !s_started || since >= LINE_SECONDS;
    const bool early = !due && since >= EARLY_MIN_SECONDS && moved_a_lot(s_last, now);
    if (!due && !early) return;
    s_started = true;
    s_since_line.reset();
    s_last = now;

    ViewerFigures v;
    v.mValid = true;
    v.mWhen = monotonic_seconds();
    v.mGLTextures = LLImageGL::getTextureBytesAllocated();
    v.mVertexBuffers = LLVertexBuffer::getBytesAllocated();
    v.mTextures = gTextureList.getNumImages();
    v.mVRAM = static_cast<long long>(gGLManager.mVRAM);
    v.mBudgetFree = static_cast<long long>(LLViewerTexture::sFreeVRAMMegabytes);
    v.mBiasHundredths = static_cast<long long>(LLViewerTexture::sDesiredDiscardBias * 100.f + 0.5f);
    v.mObjects = gObjectList.getNumObjects();
    v.mAvatars = static_cast<long long>(LLCharacter::sInstances.size());
    sViewer = v;

    char buf[1024];
    Text t = text_on(buf, sizeof(buf));
    format_reading(t, now);
    put(t, " | viewer: ");
    format_viewer(t, v);
    // kept from the 2026-10-02 breakdown line (llviewerdisplay.cpp): compressed image bytes held, decoded raw images alive
    put(t, " | compressed images "); put_mb(t, static_cast<unsigned long long>(llmax(LLImageFormatted::sGlobalFormattedMemory, 0)));
    put(t, " | raw images "); put_signed(t, LLImageRaw::sRawImageCount);
    LL_INFOS("WolfMemory") << buf << (early ? " (early: memory moved by 1 GB or more)" : "") << LL_ENDL;
}

size_t WolfMemoryLog::formatSystemNow(char* buf, size_t cap, bool at_crash)
{
    Text t = text_on(buf, cap);
    Reading r;
    read_now(r, at_crash);
    format_reading(t, r);
    return t.mLen;
}

size_t WolfMemoryLog::formatLastViewer(char* buf, size_t cap)
{
    Text t = text_on(buf, cap);
    const ViewerFigures v = sViewer;
    if (!v.mValid) return 0;
    const unsigned long long now = monotonic_seconds();
    format_viewer(t, v);
    put(t, " (logged ");
    put_dec(t, now >= v.mWhen ? now - v.mWhen : 0);
    put(t, " s before)");
    return t.mLen;
}
