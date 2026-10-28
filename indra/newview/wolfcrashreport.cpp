/**
 * @file wolfcrashreport.cpp
 * @brief WolfViewer: crash files at the crash, a staff ticket on the next run. See wolfcrashreport.h.
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

#include "wolfcrashreport.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include <boost/json.hpp>

#include "indra_constants.h"   // APP_NAME
#include "llagent.h"
#include "llappviewer.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llgl.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llsdserialize.h"
#include "llsys.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"   // gSavedSettings
#include "workqueue.h"
#include "wolfgrid.h"
#include "wolflogscrub.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#include <dbghelp.h>     // MiniDumpWriteDump (dbghelp is linked for every Windows target: cmake/Linking.cmake)
#include <intrin.h>      // _ReturnAddress
#include <signal.h>
#else
#include <dlfcn.h>       // dladdr
#include <execinfo.h>    // backtrace
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if LL_LINUX
#include <ucontext.h>
#endif
#endif

// The files the crash handler writes into the run's dump folder, and the one the sender writes when it is done.
// APP_NAME + ".log" / ".dmp" are the names bugsplatSendLog used (llappviewerwin32.cpp FS::LogfileOut / FS::DumpFile),
// and the name the Flea Report already looks for in a crash folder (wolffloaterbugreport.cpp findLogs).
static const char* const STACK_FILE = "crash_stack.txt";
static const char* const DONE_FILE = "crash_report.txt";

// ===================================================================================================================
// At the crash. Everything below this line up to "Next time" runs in a crashed process: fixed buffers prepared at
// start-up, plain system calls, no allocation, no locks of ours, no logging.
// ===================================================================================================================

namespace
{
    const int MAX_FRAMES = 64;
}

#if LL_WINDOWS

namespace
{
    const size_t PATH_CHARS = 1024;
    wchar_t sStackPath[PATH_CHARS] = {};
    wchar_t sDumpPath[PATH_CHARS] = {};
    wchar_t sLogIn[PATH_CHARS] = {};
    wchar_t sLogOut[PATH_CHARS] = {};
    bool sCopyLog = false;

    HANDLE sRequest = nullptr;          // auto-reset: a crash wants writing
    HANDLE sDone = nullptr;             // manual-reset: the files are written
    DWORD sWriterThreadId = 0;
    EXCEPTION_POINTERS* sPointers = nullptr;
    DWORD sCrashThreadId = 0;
    volatile LONG sClaimed = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER sPrevFilter = nullptr;

    // Long enough for a minidump of a big process on a slow disk; short enough that a stuck writer (a crash inside
    // the loader, say) cannot keep a dead viewer on screen for ever.
    const DWORD WRITE_WAIT_MS = 60000;
    // Not a Windows code (customer bit 0x20000000 set): "abort() was called", for the SIGABRT path.
    const DWORD WOLF_ABORT_CODE = 0xE0574C46;
    // The MSVC C++ exception code, as llthread.cpp / llcoros.cpp / workqueue.cpp STATUS_MSC_EXCEPTION.
    const DWORD MSC_CPP_EXCEPTION = 0xE06D7363;

    bool copy_path(wchar_t* dst, size_t cap, const std::string& utf8)
    {
        const std::wstring w = ll_convert_string_to_wide(utf8);
        if (w.empty() || w.size() + 1 > cap) return false;
        std::copy(w.begin(), w.end(), dst);
        dst[w.size()] = L'\0';
        return true;
    }

    // Not "TextOut": wingdi.h defines that name as a macro (TextOutW).
    struct CrashText
    {
        HANDLE mFile;
        size_t mUsed;
        char mBuf[4096];
    };

    void out_flush(CrashText& o)
    {
        if (o.mUsed > 0)
        {
            DWORD written = 0;
            WriteFile(o.mFile, o.mBuf, static_cast<DWORD>(o.mUsed), &written, nullptr);
            o.mUsed = 0;
        }
    }

    void out_str(CrashText& o, const char* s)
    {
        for (; *s; ++s)
        {
            if (o.mUsed == sizeof(o.mBuf)) out_flush(o);
            o.mBuf[o.mUsed++] = *s;
        }
    }

    void out_hex(CrashText& o, unsigned long long v)
    {
        char t[20];
        int n = 0;
        do
        {
            const unsigned digit = static_cast<unsigned>(v & 0xF);
            t[n++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            v >>= 4;
        } while (v && n < 16);
        char s[20];
        int k = 0;
        s[k++] = '0';
        s[k++] = 'x';
        while (n > 0) s[k++] = t[--n];
        s[k] = '\0';
        out_str(o, s);
    }

    void out_dec(CrashText& o, unsigned long long v)
    {
        char t[24];
        int n = 0;
        do
        {
            t[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        } while (v && n < 20);
        char s[24];
        int k = 0;
        while (n > 0) s[k++] = t[--n];
        s[k] = '\0';
        out_str(o, s);
    }

    // "<module file name>+0x<offset>": the file name only, never its folder (which holds the Windows user name).
    void out_location(CrashText& o, DWORD64 pc)
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(pc)), &mod) && mod)
        {
            wchar_t wpath[1024] = {};
            const DWORD len = GetModuleFileNameW(mod, wpath, 1024);
            const wchar_t* base = wpath;
            for (DWORD i = 0; i < len && i < 1024; ++i)
            {
                if (wpath[i] == L'\\' || wpath[i] == L'/') base = wpath + i + 1;
            }
            char name[1024] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, base, -1, name, static_cast<int>(sizeof(name)), nullptr, nullptr);
            if (n > 1)
            {
                out_str(o, name);
                out_str(o, "+");
                out_hex(o, pc - static_cast<DWORD64>(reinterpret_cast<ULONG_PTR>(mod)));
                return;
            }
        }
        out_str(o, "?+");
        out_hex(o, pc);
    }

#if defined(_M_X64)
    // Walk the crashed thread's stack from its exception context with the x64 unwind tables (RtlLookupFunctionEntry
    // / RtlVirtualUnwind, kernel32): no symbols, no allocation. A damaged stack ends the walk (the __except), it does
    // not crash the writer. No C++ objects here, so __try is allowed (C2712).
    int walk_stack(CONTEXT* ctx, DWORD64* pcs, int max)
    {
        int n = 0;
        __try
        {
            while (n < max && ctx->Rip != 0)
            {
                pcs[n++] = ctx->Rip;
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &image_base, nullptr);
                if (!fn)
                {
                    // A leaf function has no unwind data: its return address is on top of the stack.
                    ctx->Rip = *reinterpret_cast<const DWORD64*>(static_cast<ULONG_PTR>(ctx->Rsp));
                    ctx->Rsp += 8;
                }
                else
                {
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    // 0 = UNW_FLAG_NHANDLER: unwind only, call no handler.
                    RtlVirtualUnwind(0, image_base, ctx->Rip, fn, ctx, &handler_data, &establisher, nullptr);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return n;
    }
#endif

    void write_stack_file()
    {
        HANDLE f = CreateFileW(sStackPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        CrashText o;
        o.mFile = f;
        o.mUsed = 0;
        out_str(o, "WolfViewer crash stack\nformat: 1\nplatform: windows\n");
        const EXCEPTION_RECORD* rec = sPointers ? sPointers->ExceptionRecord : nullptr;
        if (rec)
        {
            out_str(o, "exception: ");
            out_hex(o, static_cast<unsigned long long>(rec->ExceptionCode));
            out_str(o, rec->ExceptionCode == WOLF_ABORT_CODE ? " (abort)\n" : "\n");
            out_str(o, "address: ");
            out_location(o, static_cast<DWORD64>(reinterpret_cast<ULONG_PTR>(rec->ExceptionAddress)));
            out_str(o, "\n");
            if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            {
                // ExceptionInformation[0]: 0 read, 1 write, 8 execute (DEP); [1] the address it tried to use.
                const ULONG_PTR kind = rec->ExceptionInformation[0];
                out_str(o, kind == 0 ? "access: read of " : kind == 1 ? "access: write to " : kind == 8 ? "access: execute at " : "access: ");
                out_hex(o, static_cast<unsigned long long>(rec->ExceptionInformation[1]));
                out_str(o, "\n");
            }
        }
        out_str(o, "thread: ");
        out_dec(o, sCrashThreadId);
        out_str(o, "\n");
#if defined(_M_X64)
        if (sPointers && sPointers->ContextRecord)
        {
            CONTEXT ctx = *sPointers->ContextRecord;
            DWORD64 pcs[MAX_FRAMES];
            const int n = walk_stack(&ctx, pcs, MAX_FRAMES);
            for (int i = 0; i < n; ++i)
            {
                out_str(o, "frame ");
                out_dec(o, static_cast<unsigned long long>(i));
                out_str(o, ": ");
                out_location(o, pcs[i]);
                out_str(o, "\n");
            }
        }
#endif
        out_flush(o);
        CloseHandle(f);
    }

    void write_minidump()
    {
        HANDLE f = CreateFileW(sDumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = sCrashThreadId;
        mei.ExceptionPointers = sPointers;
        mei.ClientPointers = FALSE;   // the pointers are in this process
        // Thread stacks, thread times and the module lists; no heap, no data segments (llwindebug.cpp, which is not
        // built for release, used WithDataSegs | WithIndirectlyReferencedMemory: far bigger and far more personal).
        const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, sPointers ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }

    // Started at start-up and parked: writing from a healthy thread works after a stack overflow, and MiniDumpWriteDump
    // is meant to be called from a thread other than the one that crashed.
    DWORD WINAPI crash_writer(LPVOID)
    {
        if (WaitForSingleObject(sRequest, INFINITE) != WAIT_OBJECT_0) return 0;
        write_stack_file();
        write_minidump();
        if (sCopyLog)
        {
            CopyFileW(sLogIn, sLogOut, FALSE);   // as bugsplatSendLog copies FS::LogfileIn to FS::LogfileOut
        }
        SetEvent(sDone);
        return 0;
    }

    void write_crash(EXCEPTION_POINTERS* ep)
    {
        if (!sRequest || !sDone || GetCurrentThreadId() == sWriterThreadId) return;
        if (InterlockedCompareExchange(&sClaimed, 1, 0) != 0)
        {
            // Another thread is already writing its crash: wait, so this one does not end the process under it.
            WaitForSingleObject(sDone, WRITE_WAIT_MS);
            return;
        }
        sPointers = ep;
        sCrashThreadId = GetCurrentThreadId();
        if (SetEvent(sRequest))
        {
            WaitForSingleObject(sDone, WRITE_WAIT_MS);
        }
    }

    LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep)
    {
        write_crash(ep);
        return sPrevFilter ? sPrevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    // abort() (an assert, a CRT check) raises SIGABRT before it ends the process; there is no exception, so the
    // context is this handler's own and the walk starts here.
    void __cdecl on_sigabrt(int)
    {
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        EXCEPTION_RECORD rec;
        memset(&rec, 0, sizeof(rec));
        rec.ExceptionCode = WOLF_ABORT_CODE;
        rec.ExceptionAddress = _ReturnAddress();
        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = &rec;
        ep.ContextRecord = &ctx;
        write_crash(&ep);
    }

    bool install_platform(const std::string& stack, const std::string& dump, const std::string& log_in,
                          const std::string& log_out, bool copy_log)
    {
        if (!copy_path(sStackPath, PATH_CHARS, stack) || !copy_path(sDumpPath, PATH_CHARS, dump)) return false;
        sCopyLog = copy_log && copy_path(sLogIn, PATH_CHARS, log_in) && copy_path(sLogOut, PATH_CHARS, log_out);
        sRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        sDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!sRequest || !sDone) return false;
        HANDLE t = CreateThread(nullptr, 0, crash_writer, nullptr, 0, &sWriterThreadId);
        if (!t)
        {
            CloseHandle(sRequest);
            CloseHandle(sDone);
            sRequest = sDone = nullptr;
            return false;
        }
        CloseHandle(t);   // it runs on; the handle is not needed
        sPrevFilter = SetUnhandledExceptionFilter(unhandled_filter);
        signal(SIGABRT, on_sigabrt);
        return true;
    }
}

bool WolfCrashReport::writeForException(void* exception_pointers)
{
    EXCEPTION_POINTERS* ep = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (!ep || !ep->ExceptionRecord || !sRequest) return false;
    if (ep->ExceptionRecord->ExceptionCode == MSC_CPP_EXCEPTION) return false;
    if (GetCurrentThreadId() == sWriterThreadId) return false;
    write_crash(ep);
    return true;
}

#else // ! LL_WINDOWS: Linux and macOS, from LLApp's fatal-signal handler (llapp.cpp default_unix_signal_handler)

namespace
{
    char sStackPath[2048] = {};
    char sLogIn[2048] = {};
    char sLogOut[2048] = {};
    bool sCopyLog = false;
    volatile sig_atomic_t sWritten = 0;
    const off_t LOG_COPY_MAX = 8 * 1024 * 1024;

    bool copy_path(char* dst, size_t cap, const std::string& s)
    {
        if (s.empty() || s.size() + 1 > cap) return false;
        memcpy(dst, s.c_str(), s.size() + 1);
        return true;
    }

    struct CrashText
    {
        int mFd;
        size_t mUsed;
        char mBuf[4096];
    };

    void write_all(int fd, const char* p, size_t n)
    {
        while (n > 0)
        {
            const ssize_t w = write(fd, p, n);
            if (w <= 0) return;
            p += w;
            n -= static_cast<size_t>(w);
        }
    }

    void out_flush(CrashText& o)
    {
        write_all(o.mFd, o.mBuf, o.mUsed);
        o.mUsed = 0;
    }

    void out_str(CrashText& o, const char* s)
    {
        for (; *s; ++s)
        {
            if (o.mUsed == sizeof(o.mBuf)) out_flush(o);
            o.mBuf[o.mUsed++] = *s;
        }
    }

    void out_hex(CrashText& o, unsigned long long v)
    {
        char t[20];
        int n = 0;
        do
        {
            const unsigned digit = static_cast<unsigned>(v & 0xF);
            t[n++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            v >>= 4;
        } while (v && n < 16);
        char s[20];
        int k = 0;
        s[k++] = '0';
        s[k++] = 'x';
        while (n > 0) s[k++] = t[--n];
        s[k] = '\0';
        out_str(o, s);
    }

    void out_dec(CrashText& o, unsigned long long v)
    {
        char t[24];
        int n = 0;
        do
        {
            t[n++] = static_cast<char>('0' + static_cast<int>(v % 10));
            v /= 10;
        } while (v && n < 20);
        char s[24];
        int k = 0;
        while (n > 0) s[k++] = t[--n];
        s[k] = '\0';
        out_str(o, s);
    }

    // "<file name>+0x<offset>", the file name only (its folder holds the home folder's name).
    void out_location(CrashText& o, const void* pc)
    {
        Dl_info info;
        if (pc && dladdr(pc, &info) && info.dli_fname && info.dli_fbase)
        {
            const char* base = info.dli_fname;
            for (const char* p = info.dli_fname; *p; ++p)
            {
                if (*p == '/') base = p + 1;
            }
            out_str(o, *base ? base : "?");
            out_str(o, "+");
            out_hex(o, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(info.dli_fbase)));
            return;
        }
        out_str(o, "?+");
        out_hex(o, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc)));
    }

    const char* signal_name(int signum)
    {
        switch (signum)
        {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGQUIT: return "SIGQUIT";
        default: return "signal";
        }
    }

    // The faulting instruction, from the signal's machine context. Linux only; on macOS the frames name the place.
    const void* faulting_pc(void* ucontext)
    {
        if (!ucontext) return nullptr;
#if LL_LINUX && defined(__x86_64__)
        return reinterpret_cast<const void*>(static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
#elif LL_LINUX && defined(__aarch64__)
        return reinterpret_cast<const void*>(static_cast<ucontext_t*>(ucontext)->uc_mcontext.pc);
#else
        return nullptr;
#endif
    }

    void copy_log_tail()
    {
        const int in = open(sLogIn, O_RDONLY | O_CLOEXEC);
        if (in < 0) return;
        const int out = open(sLogOut, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (out >= 0)
        {
            const off_t size = lseek(in, 0, SEEK_END);
            lseek(in, size > LOG_COPY_MAX ? size - LOG_COPY_MAX : 0, SEEK_SET);
            static char buf[65536];
            ssize_t n;
            while ((n = read(in, buf, sizeof(buf))) > 0)
            {
                write_all(out, buf, static_cast<size_t>(n));
            }
            close(out);
        }
        close(in);
    }

    void on_fatal_signal(int signum, void* siginfo, void* ucontext)
    {
        if (sWritten) return;
        sWritten = 1;
        const int fd = open(sStackPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0)
        {
            CrashText o;
            o.mFd = fd;
            o.mUsed = 0;
#if LL_DARWIN
            out_str(o, "WolfViewer crash stack\nformat: 1\nplatform: mac\n");
#else
            out_str(o, "WolfViewer crash stack\nformat: 1\nplatform: linux\n");
#endif
            out_str(o, "signal: ");
            out_dec(o, static_cast<unsigned long long>(signum));
            out_str(o, " (");
            out_str(o, signal_name(signum));
            out_str(o, ")\n");
            if (siginfo && (signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE))
            {
                out_str(o, "fault at: ");
                out_hex(o, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(static_cast<siginfo_t*>(siginfo)->si_addr)));
                out_str(o, "\n");
            }
            if (const void* pc = faulting_pc(ucontext))
            {
                out_str(o, "address: ");
                out_location(o, pc);
                out_str(o, "\n");
            }
            // backtrace() from inside the handler: the first frames are this handler and LLApp's, then the signal
            // trampoline, then the code that crashed. The website's signature starts at the "address:" frame.
            void* frames[MAX_FRAMES];
            const int n = backtrace(frames, MAX_FRAMES);
            for (int i = 0; i < n; ++i)
            {
                out_str(o, "frame ");
                out_dec(o, static_cast<unsigned long long>(i));
                out_str(o, ": ");
                out_location(o, frames[i]);
                out_str(o, "\n");
            }
            out_flush(o);
            close(fd);
        }
        if (sCopyLog) copy_log_tail();
    }

    bool install_platform(const std::string& stack, const std::string& /*dump*/, const std::string& log_in,
                          const std::string& log_out, bool copy_log)
    {
        if (!copy_path(sStackPath, sizeof(sStackPath), stack)) return false;
        sCopyLog = copy_log && copy_path(sLogIn, sizeof(sLogIn), log_in) && copy_path(sLogOut, sizeof(sLogOut), log_out);
        // The first backtrace() call loads libgcc's unwinder (dlopen, malloc); do it now, not in a crashed process.
        void* warm[2];
        backtrace(warm, 2);
        LLApp::setFatalSignalHook(on_fatal_signal);
        return true;
    }
}

#endif // ! LL_WINDOWS

void WolfCrashReport::installHandler(bool second_instance)
{
#if defined(LL_BUGSPLAT)
    (void)second_instance;   // BugSplat writes and sends its own (llappviewerwin32.cpp bugsplatSendLog)
    (void)&install_platform;   // built but not used: no unused-function warning
#else
    // The run's dump folder: llappviewer.cpp's constructor points the debug files at LL_PATH_DUMP (setDebugFileNames),
    // which made it (lldir.cpp getDumpDir). The log: initLoggingAndGetLastDuration logs to LL_PATH_LOGS APP_NAME.log.
    const std::string dir = gDirUtilp->getDumpDir();
    const std::string sep = gDirUtilp->getDirDelimiter();
    const bool ok = install_platform(dir + sep + STACK_FILE, dir + sep + APP_NAME + ".dmp",
                                     gDirUtilp->getExpandedFilename(LL_PATH_LOGS, APP_NAME + ".log"), dir + sep + APP_NAME + ".log",
                                     !second_instance);
    if (ok)
    {
        LL_INFOS("WolfCrashReport") << "Crash files for this run go to " << dir << LL_ENDL;
    }
    else
    {
        LL_WARNS("WolfCrashReport") << "Crash handler not installed (dump folder " << dir << ")" << LL_ENDL;
    }
#endif
}

// ===================================================================================================================
// Next time: find, pack and send.
// ===================================================================================================================

namespace
{
    const char* CRASH_API = "https://www.wolf-grid.com/viewercrashreport.php";
    // Matches viewercrashreport.php: CR_MAX_TEXT_GZ for each text file, CR_MAX_DUMP_GZ / CR_MAX_DUMP_RAW for the dump.
    const S64 MAX_LOG_BYTES = 4 * 1024 * 1024;          // the log's tail, before scrubbing, as the Flea Report
    const S64 MAX_STACK_BYTES = 64 * 1024;
    const S64 MAX_DUMP_RAW = 32 * 1024 * 1024;
    const size_t MAX_DUMP_GZ = 8 * 1024 * 1024;
    const time_t SEND_MAX_AGE = 7 * 24 * 60 * 60;      // older crashes are not sent (Flea Report CRASH_MAX_AGE)
    const time_t KEEP_FOLDER_AGE = 30 * 24 * 60 * 60;  // crash folders on this computer are deleted after this
    const size_t MAX_PER_RUN = 3;

    struct Part
    {
        std::string mField;
        std::string mFileName;
        std::string mType;
        std::string mData;
    };

    struct Package
    {
        std::string mDir;
        std::vector<std::pair<std::string, std::string>> mFields;
        std::vector<Part> mParts;
    };

    struct Found
    {
        std::string mDir;
        std::string mCrashId;
        time_t mWhen = 0;
    };

    time_t file_time(const std::string& path)
    {
        llstat st;
        return LLFile::stat(path, &st) == 0 ? st.st_mtime : 0;
    }

    void write_done(const std::string& dir, const std::string& text)
    {
        llofstream out((dir + gDirUtilp->getDirDelimiter() + DONE_FILE).c_str());
        if (out.is_open()) out << text << "\n";
    }

    std::string utc_text(time_t t)
    {
        struct tm g;
#if LL_WINDOWS
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &g);
        return buf;
    }

    // Crash folders of earlier runs. Deletes the ones older than KEEP_FOLDER_AGE (no eternal logs on the resident's
    // computer either); returns those from the last SEND_MAX_AGE that hold crash files and have not been dealt with.
    std::vector<Found> find_crashes()
    {
        std::vector<Found> found;
        const std::string logdir = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "");
        const std::string sep = gDirUtilp->getDirDelimiter();
        const std::string this_run = gDirUtilp->dumpDirExists() ? gDirUtilp->getBaseFileName(gDirUtilp->getDumpDir()) : std::string();
        const time_t now = time(nullptr);
        std::vector<std::string> names;
        {
            LLDirIterator it(logdir, "dump-*");
            std::string name;
            while (it.next(name)) names.push_back(name);
        }
        for (const std::string& name : names)
        {
            // Only folders named as lldir.cpp getDumpDir names them: "dump-" + a UUID (36 characters).
            if (name == this_run || name.size() != 41 || name.compare(0, 5, "dump-") != 0) continue;
            const std::string id = name.substr(5);
            const std::string dir = logdir + name;
            if (!LLUUID::validate(id) || !LLFile::isdir(dir)) continue;
            const time_t stack_time = file_time(dir + sep + STACK_FILE);
            const time_t dump_time = file_time(dir + sep + APP_NAME + ".dmp");
            const time_t newest = std::max({ stack_time, dump_time, file_time(dir + sep + "static_debug_info.log"), file_time(dir) });
            if (newest > 0 && now - newest > KEEP_FOLDER_AGE)
            {
                LL_INFOS("WolfCrashReport") << "Deleting old crash folder " << name << LL_ENDL;
                gDirUtilp->deleteDirAndContents(dir);
                continue;
            }
            const time_t when = std::max(stack_time, dump_time);
            if (when == 0 || now - when > SEND_MAX_AGE || file_time(dir + sep + DONE_FILE) != 0) continue;
            Found f;
            f.mDir = dir;
            f.mCrashId = id;
            f.mWhen = when;
            found.push_back(f);
        }
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mWhen > b.mWhen; });
        return found;
    }

    bool read_file(const std::string& path, S64 max_bytes, std::string& out)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return false;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size <= 0 || size > max_bytes) return false;
        in.seekg(0, std::ios::beg);
        out.assign(static_cast<size_t>(size), '\0');
        in.read(&out[0], static_cast<std::streamsize>(out.size()));
        out.resize(static_cast<size_t>(in.gcount()));
        return !out.empty();
    }

    std::string one_line(std::string s, size_t max)
    {
        for (char& c : s)
        {
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        }
        LLStringUtil::trim(s);
        if (s.size() > max) s.resize(max);
        return s;
    }

    // The fields of static_debug_info.log (llappviewer.cpp writeDebugInfo / initCrashReporting gDebugInfo) that help
    // with a crash. Not sent: AgentID and LoginName (the website knows who sent it), SettingsFilename, ViewerExePath
    // and CurrentPath (folders with the computer's user name), ParcelMediaURL / ParcelMusicURL, SLLog.
    std::string debug_info_text(const LLSD& info, const std::vector<std::string>& secrets, std::vector<std::pair<std::string, std::string>>& fields)
    {
        static const char* const KEYS[][2] = {
            { "ClientInfo", "Name" }, { "ClientInfo", "Version" }, { "ClientInfo", "Platform" }, { "ClientInfo", "AddressSize" },
            { "ClientInfo", "Flavor" }, { "CPUInfo", "CPUString" }, { "RAMInfo", "Physical" }, { "RAMInfo", "Allocated" },
            { "OSInfo", "" }, { "GraphicsCard", "" }, { "GridName", "" }, { "StartupState", "" }, { "CurrentRegion", "" },
            { "CurrentSimHost", "" }, { "MainloopTimeoutState", "" }, { "FirstLogin", "" }, { "FirstRunThisInstall", "" },
            { "FoundOtherInstanceAtStartup", "" }, { "FatalMessage", "" },
        };
        std::string text = "WolfViewer crash: from static_debug_info.log\n";
        for (const auto& k : KEYS)
        {
            if (!info.has(k[0])) continue;
            const LLSD& v = k[1][0] ? info[k[0]][k[1]] : info[k[0]];
            if (v.isUndefined() || v.isMap() || v.isArray()) continue;
            const std::string name = k[1][0] ? std::string(k[0]) + "." + k[1] : std::string(k[0]);
            text += name + ": " + one_line(v.asString(), 2000) + "\n";
        }
        text = WolfLogScrub::wolf_scrub_log(text, secrets);
        if (info.has("ClientInfo"))
        {
            fields.emplace_back("crashed_viewer", one_line(info["ClientInfo"]["Name"].asString() + " " + info["ClientInfo"]["Version"].asString(), 120));
        }
        if (info.has("CurrentRegion")) fields.emplace_back("crashed_region", one_line(info["CurrentRegion"].asString(), 128));
        if (info.has("StartupState")) fields.emplace_back("startup_state", one_line(info["StartupState"].asString(), 64));
        if (info.has("GraphicsCard")) fields.emplace_back("crashed_gpu", one_line(info["GraphicsCard"].asString(), 200));
        if (info.has("FatalMessage"))
        {
            fields.emplace_back("fatal", one_line(WolfLogScrub::wolf_scrub_log(info["FatalMessage"].asString(), secrets), 500));
        }
        return text;
    }

    // Runs on the "General" work queue: read and pack every crash folder found.
    std::vector<Package> prepare(const std::vector<Found>& crashes, const std::vector<std::string>& secrets,
                                 const std::vector<std::pair<std::string, std::string>>& common)
    {
        std::vector<Package> packages;
        const std::string sep = gDirUtilp->getDirDelimiter();
        for (const Found& c : crashes)
        {
            Package p;
            p.mDir = c.mDir;
            p.mFields = common;
            p.mFields.emplace_back("crashid", c.mCrashId);
            p.mFields.emplace_back("crashtime", llformat("%lld", static_cast<long long>(c.mWhen)));

            std::string text, gz;
            if (read_file(c.mDir + sep + STACK_FILE, MAX_STACK_BYTES, text) && WolfLogScrub::gzip_string(text, gz))
            {
                p.mParts.push_back({ "stack", "crash-stack.txt.gz", "application/gzip", gz });
            }

            std::string raw;
            if (read_file(c.mDir + sep + "static_debug_info.log", 1024 * 1024, raw))
            {
                LLSD info;
                std::istringstream in(raw);
                if (LLSDSerialize::fromXML(info, in, false) > 0 && info.isMap())
                {
                    const std::string info_text = debug_info_text(info, secrets, p.mFields);
                    if (WolfLogScrub::gzip_string(info_text, gz)) p.mParts.push_back({ "info", "crash-info.txt.gz", "application/gzip", gz });
                }
            }

            // The log as the crash left it (copied by the handler). WolfViewer.old is not used: after more than one
            // run it may be a different session's.
            if (WolfLogScrub::read_tail(c.mDir + sep + APP_NAME + ".log", text, MAX_LOG_BYTES)
                && WolfLogScrub::gzip_string(WolfLogScrub::wolf_scrub_log(text, secrets), gz))
            {
                p.mParts.push_back({ "log", "crash-" + APP_NAME + ".log.gz", "application/gzip", gz });
            }

            // The minidump, only if it really is one ("MDMP", minidumpapiset.h MINIDUMP_SIGNATURE) and fits.
            std::string dmp;
            if (read_file(c.mDir + sep + APP_NAME + ".dmp", MAX_DUMP_RAW, dmp))
            {
                if (dmp.size() >= 4 && dmp.compare(0, 4, "MDMP") == 0 && WolfLogScrub::gzip_string(dmp, gz) && gz.size() <= MAX_DUMP_GZ)
                {
                    p.mParts.push_back({ "dump", APP_NAME + ".dmp.gz", "application/gzip", gz });
                }
                else
                {
                    p.mFields.emplace_back("dump_skipped", llformat("%d bytes, not a minidump or too large compressed", static_cast<S32>(dmp.size())));
                }
            }
            if (!p.mParts.empty()) packages.push_back(p);
        }
        return packages;
    }

    // multipart/form-data (RFC 7578), as wolffloaterbugreport.cpp sendCoro. Returns the HTTP status (0 = no answer)
    // and the JSON reply.
    S32 post_package(const Package& p, LLSD& reply)
    {
        LLUUID rnd;
        rnd.generate();
        const std::string boundary = "----WolfCrashReport" + rnd.asString();
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        auto put = [&raw](const std::string& s) { raw->append(s.data(), s.size()); };
        for (const auto& f : p.mFields)
        {
            put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + f.first + "\"\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\n" + f.second + "\r\n");
        }
        for (const Part& part : p.mParts)
        {
            put("--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + part.mField + "\"; filename=\"" + part.mFileName + "\"\r\n"
                "Content-Type: " + part.mType + "\r\n\r\n");
            put(part.mData);
            put("\r\n");
        }
        put("--" + boundary + "--\r\n");

        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfCrashReport", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(180);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "multipart/form-data; boundary=" + boundary);
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        LLSD result = adapter->postRawAndSuspend(request, CRASH_API, raw, options, headers);

        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        reply = LLSD();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            const LLSD::Binary& bin = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            boost::system::error_code ec;
            boost::json::value v = boost::json::parse(std::string(bin.begin(), bin.end()), ec);
            if (!ec) reply = LlsdFromJson(v);
        }
        if (status) return 200;
        return status.isHttpStatus() ? static_cast<S32>(status.getType()) : 0;
    }

    void send_coro(std::vector<Package> packages)
    {
        S32 sent = 0;
        for (const Package& p : packages)
        {
            if (LLApp::isExiting()) return;
            LLSD reply;
            const S32 http = post_package(p, reply);
            const bool ok = reply.isMap() && reply["success"].asBoolean();
            if (ok)
            {
                // A repeat (the reply to an earlier send was lost) is fine: the website keeps one per crash id.
                write_done(p.mDir, "sent " + utc_text(time(nullptr)) + (reply["duplicate"].asBoolean() ? " (already received)" : ""));
                if (!reply["duplicate"].asBoolean()) ++sent;
                LL_INFOS("WolfCrashReport") << "Crash report sent from " << gDirUtilp->getBaseFileName(p.mDir) << LL_ENDL;
            }
            else if (http >= 400 && http < 500 && http != 401 && http != 403 && http != 408 && http != 429)
            {
                // The website will never take this one (too big, damaged): record it so it is not tried every run.
                const std::string why = reply.isMap() ? reply["error"].asString() : std::string();
                write_done(p.mDir, "refused " + utc_text(time(nullptr)) + " (HTTP " + std::to_string(http) + "): " + why);
                LL_WARNS("WolfCrashReport") << "Crash report refused (HTTP " << http << "): " << why << LL_ENDL;
            }
            else
            {
                // No answer, a server error, a rate limit or a session problem: left for a later run (up to 7 days).
                LL_WARNS("WolfCrashReport") << "Crash report not sent now (HTTP " << http << "); will try again next time" << LL_ENDL;
                break;
            }
        }
        if (sent > 0)
        {
            LLNotificationsUtil::add("WolfCrashReportSent");
        }
    }
}

void WolfCrashReport::sendPendingOnce()
{
    static bool done = false;
    if (done) return;
    done = true;

    LLAppViewer* app = LLAppViewer::instance();
    if (!app || app->isSecondInstance()) return;   // the first instance deals with the folders

    std::vector<Found> crashes = find_crashes();   // also deletes folders older than 30 days
    if (crashes.empty()) return;

    if (!gSavedSettings.getBOOL("WolfCrashReportsAuto"))
    {
        // Turned off: these are never sent, not even if it is turned on again later.
        for (const Found& c : crashes) write_done(c.mDir, "not sent: automatic crash reports were off");
        LL_INFOS("WolfCrashReport") << crashes.size() << " crash(es) not sent: WolfCrashReportsAuto is off" << LL_ENDL;
        return;
    }
    // The request carries this login's agent and session ids: only to Wolf's own site, only from a Wolf login. A
    // crash found on another grid stays for the next Wolf Territories login.
    if (!WolfGrid::isWolfTerritories()) return;

    if (crashes.size() > MAX_PER_RUN) crashes.resize(MAX_PER_RUN);   // the newest; the rest wait for later runs

    std::vector<std::pair<std::string, std::string>> common;
    common.emplace_back("viewer", LLVersionInfo::instance().getChannelAndVersion());
    common.emplace_back("os", LLOSInfo::instance().getOSString());
    common.emplace_back("gpu", gGLManager.getRawGLString());
#if LL_WINDOWS
    common.emplace_back("platform", "windows");
#elif LL_DARWIN
    common.emplace_back("platform", "mac");
#else
    common.emplace_back("platform", "linux");
#endif
    const std::vector<std::string> secrets = { gAgentSessionID.asString(), gAgent.getSecureSessionID().asString() };

    auto launch = [](std::vector<Package> packages)
    {
        if (packages.empty()) return;
        LLCoros::instance().launch("WolfCrashReport", [packages]() { send_coro(packages); });
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue
        || !main_queue->postTo(general_queue, [crashes, secrets, common]() { return prepare(crashes, secrets, common); }, launch))
    {
        launch(prepare(crashes, secrets, common));
    }
}
