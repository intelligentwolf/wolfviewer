/**
 * @file wolfcoreloader.cpp
 * @brief Dynamic runtime loader for the optional WolfCore acceleration library (Windows, macOS, Linux).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * Copyright (C) 2026, IntelligentWolf Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "wolfcoreloader.h"

#include <mutex>
#include <vector>

#if LL_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#include <climits>
#include <cstdlib>
#include <unistd.h>
#endif

#if LL_DARWIN
#include <mach-o/dyld.h>
#endif

namespace
{
    std::once_flag sInitFlag;
    const WolfCoreApi* sApi = nullptr;
    std::string sLoadedPath;
#if LL_WINDOWS
    HMODULE sHandle = nullptr;
#else
    void* sHandle = nullptr;
#endif

#if LL_WINDOWS
    std::wstring executableDirW()
    {
        std::vector<wchar_t> buf(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0)
            {
                return std::wstring();
            }
            if (len < buf.size() - 1)
            {
                std::wstring full(buf.data(), len);
                const size_t slash = full.find_last_of(L"\\/");
                return (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
            }
            if (buf.size() >= 32768)
            {
                return std::wstring();
            }
            buf.resize(buf.size() * 2, L'\0');
        }
    }

    std::string narrowUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        const int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (need <= 0) return std::string();
        std::string out(static_cast<size_t>(need), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
        return out;
    }
#elif LL_DARWIN
    std::string darwinFrameworksLibPath()
    {
        char raw[PATH_MAX];
        uint32_t size = sizeof(raw);
        if (_NSGetExecutablePath(raw, &size) != 0)
        {
            return std::string();
        }
        char real[PATH_MAX];
        if (!realpath(raw, real))
        {
            return std::string();
        }
        std::string path(real);
        // .../WolfViewer.app/Contents/MacOS/WolfViewer -> .../WolfViewer.app/Contents
        for (int up = 0; up < 2; ++up)
        {
            const size_t slash = path.find_last_of('/');
            if (slash == std::string::npos || slash == 0)
            {
                return std::string();
            }
            path.resize(slash);
        }
        return path + "/Frameworks/libwolfcore.dylib";
    }
#else
    std::vector<std::string> linuxCandidatePaths()
    {
        std::vector<std::string> out;
        char buf[PATH_MAX];
        const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
        {
            return out;
        }
        buf[len] = '\0';
        std::string exe(buf, static_cast<size_t>(len));
        const size_t slash = exe.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
        {
            return out;
        }
        const std::string exe_dir = exe.substr(0, slash);
        // Packaged Linux layout: <root>/bin/do-not-directly-run-wolfviewer-bin and <root>/lib/libwolfcore.so
        const size_t parent_slash = exe_dir.find_last_of('/');
        if (parent_slash != std::string::npos && parent_slash > 0)
        {
            out.push_back(exe_dir.substr(0, parent_slash) + "/lib/libwolfcore.so");
        }
        out.push_back(exe_dir + "/libwolfcore.so");
        return out;
    }
#endif

    void initOnce()
    {
        PFN_wolfcore_get_api entry = nullptr;
        std::string chosen_path;

#if LL_WINDOWS
        // OWASP DLL-search safety: load strictly by absolute path next to the viewer executable,
        // never searching the current working directory or PATH.
        const std::wstring dir = executableDirW();
        if (!dir.empty())
        {
            const std::wstring dll_path = dir + L"\\wolfcore.dll";
            const DWORD attrs = GetFileAttributesW(dll_path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                sHandle = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (sHandle)
                {
                    chosen_path = narrowUtf8(dll_path);
                    entry = reinterpret_cast<PFN_wolfcore_get_api>(GetProcAddress(sHandle, "wolfcore_get_api"));
                }
            }
        }
#elif LL_DARWIN
        const std::string dylib_path = darwinFrameworksLibPath();
        if (!dylib_path.empty() && access(dylib_path.c_str(), R_OK) == 0)
        {
            sHandle = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (sHandle)
            {
                chosen_path = dylib_path;
                entry = reinterpret_cast<PFN_wolfcore_get_api>(dlsym(sHandle, "wolfcore_get_api"));
            }
        }
#else
        for (const std::string& candidate : linuxCandidatePaths())
        {
            if (access(candidate.c_str(), R_OK) != 0)
            {
                continue;
            }
            sHandle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (sHandle)
            {
                chosen_path = candidate;
                entry = reinterpret_cast<PFN_wolfcore_get_api>(dlsym(sHandle, "wolfcore_get_api"));
                break;
            }
        }
#endif

        if (!sHandle)
        {
            LL_INFOS("WolfCore") << "Optional WolfCore library not present; using open-source LGPL fallback paths" << LL_ENDL;
            return;
        }

        const WolfCoreApi* table = entry ? entry(WOLFCORE_ABI_VERSION) : nullptr;
        if (!table || table->abi_version != WOLFCORE_ABI_VERSION || table->struct_size < sizeof(WolfCoreApi)
            || !table->pack_uniform_member || !table->spv_collect_used_ids || !table->plan_frame_pacing)
        {
            LL_WARNS("WolfCore") << "WolfCore library at " << chosen_path
                                 << " did not match ABI v" << WOLFCORE_ABI_VERSION
                                 << "; falling back to open-source paths" << LL_ENDL;
#if LL_WINDOWS
            FreeLibrary(sHandle);
#else
            dlclose(sHandle);
#endif
            sHandle = nullptr;
            return;
        }

        sApi = table;
        sLoadedPath = chosen_path;
        LL_INFOS("WolfCore") << "WolfCore v" << (table->version_string ? table->version_string : "?")
                             << " (ABI v" << table->abi_version << ") loaded from " << sLoadedPath << LL_ENDL;
    }
} // namespace

namespace WolfCoreLoader
{
    const WolfCoreApi* api()
    {
        std::call_once(sInitFlag, initOnce);
        return sApi;
    }

    const std::string& loadedPath()
    {
        std::call_once(sInitFlag, initOnce);
        return sLoadedPath;
    }
} // namespace WolfCoreLoader
