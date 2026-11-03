/**
 * @file wolfvr_openh264.cpp
 * @brief WolfViewer VR pipe: the direct OpenH264 encoder and the run-time library loader. See wolfvr_openh264.h.
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

#include "wolfvr_openh264.h"

// Cisco OpenH264 v2.6.0 API headers, vendored unmodified (openh264/SOURCES.md, BSD-2-Clause).
#include "openh264/codec_api.h"
#include "openh264/codec_ver.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wolfvr
{
// ---------------------------------------------------------------------------------------------------------------
// DynamicLibrary
// ---------------------------------------------------------------------------------------------------------------
#if defined(_WIN32)
namespace
{
    std::wstring utf8ToWide(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    std::string lastErrorText(DWORD code)
    {
        wchar_t* buf = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
        std::string out;
        if (n && buf)
        {
            const int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                out.resize((size_t)len);
                WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, out.data(), len, nullptr, nullptr);
            }
            LocalFree(buf);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.'))
        {
            out.pop_back();
        }
        return out.empty() ? "Windows error " + std::to_string(code) : out;
    }
}
#endif

DynamicLibrary::~DynamicLibrary()
{
    close();
}

bool DynamicLibrary::open(const std::string& path, std::string& error)
{
    close();
#if defined(_WIN32)
    const std::wstring w = utf8ToWide(path);
    if (w.empty()) { error = path + ": not a valid file name"; return false; }
    // LOAD_WITH_ALTERED_SEARCH_PATH: for a full path, the DLL's own folder is searched for what it needs, not the
    // viewer's (LoadLibraryExW, Microsoft "Dynamic-Link Library Search Order"). Cisco's DLL imports only KERNEL32.
    HMODULE h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { error = path + ": " + lastErrorText(GetLastError()); return false; }
    mHandle = reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        const char* e = dlerror();
        error = e ? std::string(e) : path + ": could not be opened";
        return false;
    }
    mHandle = h;
#endif
    return true;
}

void* DynamicLibrary::symbol(const char* name) const
{
    if (!mHandle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(mHandle), name));
#else
    return dlsym(mHandle, name);
#endif
}

void DynamicLibrary::close()
{
    if (!mHandle) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(mHandle));
#else
    dlclose(mHandle);
#endif
    mHandle = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The library: loaded once, kept until exit
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // Exported C functions (openh264/codec_api.h:545-586, declared extern "C" without a calling-convention macro).
    using CreateEncoderFn = int (*)(ISVCEncoder**);
    using DestroyEncoderFn = void (*)(ISVCEncoder*);
    using VersionExFn = void (*)(OpenH264Version*);

    struct Library
    {
        std::mutex mutex;
        std::string override_path;     // setOpenH264Path
        DynamicLibrary lib;
        bool loaded = false;
        std::string description;
        CreateEncoderFn create = nullptr;
        DestroyEncoderFn destroy = nullptr;
    };

    Library& library()
    {
        // Never destroyed: the library must stay loaded until the process ends (an encoder on the frame thread may
        // still be running while static objects are torn down at exit), as the GStreamer handles do.
        static Library* l = new Library;
        return *l;
    }

    // One candidate file: open it, find the three functions, check the version. The structures this file passes
    // (SEncParamExt, SSourcePicture, SFrameBSInfo) changed size in 2.6.0 (bPsnrY/U/V and rPsnr were added: diff of
    // codec/api/wels/codec_app_def.h v2.5.0..v2.6.0), and v2.7.0's three API headers are byte-identical to v2.6.0's,
    // so only 2.6.x and 2.7.x match the vendored headers. An older library would read our structures wrongly.
    bool tryLoad(Library& l, const std::string& path, std::string& error)
    {
        std::string err;
        if (!l.lib.open(path, err)) { error = err; return false; }
        auto create = reinterpret_cast<CreateEncoderFn>(l.lib.symbol("WelsCreateSVCEncoder"));
        auto destroy = reinterpret_cast<DestroyEncoderFn>(l.lib.symbol("WelsDestroySVCEncoder"));
        auto version = reinterpret_cast<VersionExFn>(l.lib.symbol("WelsGetCodecVersionEx"));
        if (!create || !destroy || !version)
        {
            error = path + ": not an OpenH264 library (WelsCreateSVCEncoder missing)";
            l.lib.close();
            return false;
        }
        OpenH264Version v{};
        version(&v);
        if (v.uMajor != OPENH264_MAJOR || (v.uMinor != OPENH264_MINOR && v.uMinor != OPENH264_MINOR + 1))
        {
            error = path + ": OpenH264 " + std::to_string(v.uMajor) + "." + std::to_string(v.uMinor) + "." +
                    std::to_string(v.uRevision) + " does not match this viewer (needs 2.6 or 2.7)";
            l.lib.close();
            return false;
        }
        l.create = create;
        l.destroy = destroy;
        l.loaded = true;
        l.description = "OpenH264 " + std::to_string(v.uMajor) + "." + std::to_string(v.uMinor) + "." +
                        std::to_string(v.uRevision) + " (" + path + ")";
        return true;
    }

    bool ensureLoaded(std::string& error)
    {
        Library& l = library();
        std::lock_guard<std::mutex> lk(l.mutex);
        if (l.loaded) return true;
        std::vector<std::string> candidates;
        if (!l.override_path.empty()) candidates.push_back(l.override_path);
#if defined(__linux__)
        // Paul: on Linux the distribution's library is the fallback after the downloaded one. libopenh264.so.8 is the
        // Makefile soname of 2.6.0 (Makefile SHAREDLIB_MAJORVERSION=8); a meson build of 2.6.0 names it .so.7
        // (meson.build major_version = '7'); .so is the development link. The version check decides.
        candidates.push_back("libopenh264.so.8");
        candidates.push_back("libopenh264.so.7");
        candidates.push_back("libopenh264.so");
#endif
        if (candidates.empty())
        {
            error = "the free video codec from Cisco is not downloaded yet";
            return false;
        }
        std::string all;
        for (const std::string& c : candidates)
        {
            std::string e;
            if (tryLoad(l, c, e)) return true;
            all += (all.empty() ? "" : "; ") + e;
        }
        error = "OpenH264 not loaded: " + all;
        return false;
    }
}

void setOpenH264Path(const std::string& path_utf8)
{
    Library& l = library();
    std::lock_guard<std::mutex> lk(l.mutex);
    l.override_path = path_utf8;
}

std::string openh264LoadedDescription()
{
    Library& l = library();
    std::lock_guard<std::mutex> lk(l.mutex);
    return l.loaded ? l.description : std::string();
}

// ---------------------------------------------------------------------------------------------------------------
// RGBA -> I420 (ITU-R BT.709, limited range), as the encoder input and as signalled in the SPS VUI below
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // ITU-R BT.709-6 item 3.2/3.3: Kr = 0.2126, Kb = 0.0722 (Kg = 1 - Kr - Kb); E'Cb = (E'B - E'Y) / 1.8556,
    // E'Cr = (E'R - E'Y) / 1.5748. Item 4.6 (8-bit): Y = 219 E'Y + 16, Cb/Cr = 224 E'C + 128. The 8-bit RGB input is
    // E' * 255. Coefficients in 16.16 fixed point.
    constexpr double kKr = 0.2126, kKb = 0.0722, kKg = 1.0 - kKr - kKb;
    constexpr int fix16(double v) { return (int)(v >= 0 ? v * 65536.0 + 0.5 : v * 65536.0 - 0.5); }
    constexpr double kYs = 219.0 / 255.0, kCs = 224.0 / 255.0;
    constexpr int kYR = fix16(kYs * kKr), kYG = fix16(kYs * kKg), kYB = fix16(kYs * kKb);
    constexpr int kUR = fix16(-kCs * kKr / 1.8556), kUG = fix16(-kCs * kKg / 1.8556), kUB = fix16(kCs * (1 - kKb) / 1.8556);
    constexpr int kVR = fix16(kCs * (1 - kKr) / 1.5748), kVG = fix16(-kCs * kKg / 1.5748), kVB = fix16(-kCs * kKb / 1.5748);

    inline uint8_t lumaOf(const uint8_t* p)
    {
        return (uint8_t)((kYR * p[0] + kYG * p[1] + kYB * p[2] + (16 << 16) + (1 << 15)) >> 16);
    }

    // Rows [cy0, cy1) of chroma (= luma rows 2*cy0 .. 2*cy1-1). Odd widths/heights repeat the last column/row.
    void convertBand(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t* Y, uint8_t* U, uint8_t* V, uint32_t cw,
                     uint32_t cy0, uint32_t cy1)
    {
        const size_t stride = (size_t)w * 4;
        for (uint32_t cy = cy0; cy < cy1; ++cy)
        {
            const uint32_t y0 = cy * 2, y1 = std::min(y0 + 1, h - 1);
            const uint8_t* r0 = rgba + y0 * stride;
            const uint8_t* r1 = rgba + y1 * stride;
            uint8_t* Y0 = Y + (size_t)y0 * w;
            uint8_t* Y1 = Y + (size_t)y1 * w;   // == Y0 for the last row of an odd height: same values written
            uint8_t* Ur = U + (size_t)cy * cw;
            uint8_t* Vr = V + (size_t)cy * cw;
            for (uint32_t cx = 0; cx < cw; ++cx)
            {
                const uint32_t x0 = cx * 2, x1 = std::min(x0 + 1, w - 1);
                const uint8_t* a = r0 + (size_t)x0 * 4;
                const uint8_t* b = r0 + (size_t)x1 * 4;
                const uint8_t* c = r1 + (size_t)x0 * 4;
                const uint8_t* d = r1 + (size_t)x1 * 4;
                Y0[x0] = lumaOf(a);
                Y0[x1] = lumaOf(b);
                Y1[x0] = lumaOf(c);
                Y1[x1] = lumaOf(d);
                // chroma of the 2x2 average: sums are 0..1020, so the shift is 16 + 2
                const int sr = a[0] + b[0] + c[0] + d[0];
                const int sg = a[1] + b[1] + c[1] + d[1];
                const int sb = a[2] + b[2] + c[2] + d[2];
                Ur[cx] = (uint8_t)std::clamp((kUR * sr + kUG * sg + kUB * sb + (128 << 18) + (1 << 17)) >> 18, 0, 255);
                Vr[cx] = (uint8_t)std::clamp((kVR * sr + kVG * sg + kVB * sb + (128 << 18) + (1 << 17)) >> 18, 0, 255);
            }
        }
    }

    // H.264 Table A-1 MaxMBPS / MaxFS per level, as OpenH264 holds them (codec/common/src/common_tables.cpp
    // g_ksLevelLimits, v2.6.0). OpenH264 picks the level from fMaxFrameRate, which it clamps to 60 (wels_const.h
    // MAX_FRAME_RATE); for 72 fps the level is computed here from the real rate and handed in (the encoder only ever
    // raises a level it is given: au_set.cpp WelsInitSps "uiLevelIdc < uiLevel").
    struct LevelLimit { ELevelIdc level; uint32_t max_mbps, max_fs; };
    const LevelLimit kLevels[] = {
        {LEVEL_3_0, 40500, 1620},   {LEVEL_3_1, 108000, 3600},  {LEVEL_3_2, 216000, 5120},
        {LEVEL_4_0, 245760, 8192},  {LEVEL_4_1, 245760, 8192},  {LEVEL_4_2, 522240, 8704},
        {LEVEL_5_0, 589824, 22080}, {LEVEL_5_1, 983040, 36864}, {LEVEL_5_2, 2073600, 36864},
    };

    ELevelIdc levelFor(uint32_t w, uint32_t h, uint32_t fps)
    {
        const uint64_t mbs = (uint64_t)((w + 15) / 16) * ((h + 15) / 16);
        for (const LevelLimit& l : kLevels)
        {
            if (mbs <= l.max_fs && mbs * fps <= l.max_mbps) return l.level;
        }
        return LEVEL_5_2;
    }

    void traceToString(void* ctx, int level, const char* text)
    {
        // Called by the encoder (also from its worker threads) for WELS_LOG_ERROR only (trace level set below).
        auto* sink = static_cast<std::pair<std::mutex, std::string>*>(ctx);
        if (!sink || !text) return;
        std::lock_guard<std::mutex> lk(sink->first);
        sink->second = text;
        while (!sink->second.empty() && (sink->second.back() == '\n' || sink->second.back() == '\r'))
        {
            sink->second.pop_back();
        }
        (void)level;
    }

    class OpenH264Encoder : public Encoder
    {
    public:
        ~OpenH264Encoder() override
        {
            if (mEnc)
            {
                mEnc->Uninitialize();
                library().destroy(mEnc);
            }
        }

        bool open(uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, const OpenH264Tuning& t, std::string& error)
        {
            Library& l = library();
            mW = w; mH = h; mFps = std::max<uint32_t>(1, fps);
            if (w < 16 || h < 16 || w > 8192 || h > 8192)
            {
                // EncodeFrameInternal refuses < 16 (welsEncoderExt.cpp:406); MAX_MBS_PER_FRAME caps the area
                error = "openh264: unsupported picture size " + std::to_string(w) + "x" + std::to_string(h);
                return false;
            }
            if (l.create(&mEnc) != 0 || !mEnc)
            {
                mEnc = nullptr;
                error = "openh264: WelsCreateSVCEncoder failed";
                return false;
            }
            // Errors only, into mLastError (ENCODER_OPTION_TRACE_* may be set before Initialize: welsEncoderExt.cpp:693)
            int trace_level = WELS_LOG_ERROR;
            mEnc->SetOption(ENCODER_OPTION_TRACE_LEVEL, &trace_level);
            WelsTraceCallback cb = &traceToString;
            mEnc->SetOption(ENCODER_OPTION_TRACE_CALLBACK, &cb);
            void* ctx = &mLastError;
            mEnc->SetOption(ENCODER_OPTION_TRACE_CALLBACK_CONTEXT, &ctx);

            SEncParamExt p;
            std::memset(&p, 0, sizeof p);
            mEnc->GetDefaultParams(&p);   // TagWelsSvcCodingParam::FillDefault (param_svc.h:131)
            const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
            // OpenH264 clips iMultipleThreadIdc to 1..MAX_THREADS_NUM = 4 (encoder_ext.cpp:2223, wels_const.h:69)
            const int threads = std::clamp(t.threads > 0 ? t.threads : (int)hw, 1, 4);
            const int slices = std::clamp(t.slices > 0 ? t.slices : threads, 1, 4);
            mConvertThreads = std::clamp(t.convert_threads > 0 ? t.convert_threads : threads, 1, 8);
            // The rate control budgets iTargetBitrate / fFrameRate per picture with fFrameRate clamped to 60
            // (param_svc.h:295 WELS_CLIP3 .. MAX_FRAME_RATE; ratectl.cpp:1358), so above 60 fps the target is scaled
            // down to make the real bitrate the one asked for.
            const uint32_t rc_fps = std::min<uint32_t>(mFps, 60);
            const int bps = (int)std::min<uint64_t>((uint64_t)kbps * 1000ULL * rc_fps / mFps, 0x7fffffffULL);

            p.iUsageType = CAMERA_VIDEO_REAL_TIME;
            p.iPicWidth = (int)w;
            p.iPicHeight = (int)h;
            p.iTargetBitrate = bps;
            p.iMaxBitrate = UNSPECIFIED_BIT_RATE;
            p.iRCMode = RC_BITRATE_MODE;
            p.fMaxFrameRate = (float)rc_fps;
            p.iTemporalLayerNum = 1;          // no hierarchical P: every picture is a reference, one out per one in
            p.iSpatialLayerNum = 1;
            p.iComplexityMode = (ECOMPLEXITY_MODE)std::clamp(t.complexity, 0, 2);
            p.uiIntraPeriod = mFps * 2;       // an IDR every 2 s, as the GStreamer chains (gop-size = fps * 2)
            p.iNumRefFrame = 1;
            p.eSpsPpsIdStrategy = CONSTANT_ID;
            p.bPrefixNalAddingCtrl = false;
            p.bEnableSSEI = false;
            p.bSimulcastAVC = false;
            p.iPaddingFlag = 0;
            p.iEntropyCodingModeFlag = 0;     // CAVLC: Constrained Baseline, the cheapest to encode and decode
            p.bEnableFrameSkip = false;       // never drop a picture: the pipe expects one access unit per encode
            p.bEnableLongTermReference = false;
            p.iMultipleThreadIdc = (unsigned short)threads;
            p.bUseLoadBalancing = true;
            p.iLoopFilterDisableIdc = 0;      // the encoder makes it 2 (not across slices) when threaded (encoder_ext.cpp:2052)
            p.bEnableDenoise = false;
            p.bEnableBackgroundDetection = true;
            p.bEnableAdaptiveQuant = true;
            p.bEnableSceneChangeDetect = false;   // a head turn must not cost an IDR's worth of bits mid-stream
            p.bEnableFrameCroppingFlag = true;
            SSpatialLayerConfig& L = p.sSpatialLayers[0];
            L.iVideoWidth = (int)w;
            L.iVideoHeight = (int)h;
            L.fFrameRate = (float)rc_fps;
            L.iSpatialBitrate = bps;
            L.iMaxSpatialBitrate = UNSPECIFIED_BIT_RATE;
            L.uiProfileIdc = PRO_BASELINE;
            L.uiLevelIdc = levelFor(w, h, mFps);
            L.sSliceArgument.uiSliceMode = slices > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE;
            L.sSliceArgument.uiSliceNum = (unsigned int)slices;
            // VUI: BT.709 limited range, matching the conversion above (codec_app_def.h EVideoFormatSPS VF_UNDEF,
            // EColorPrimaries CP_BT709, ETransferCharacteristics TRC_BT709, EColorMatrix CM_BT709)
            L.bVideoSignalTypePresent = true;
            L.uiVideoFormat = VF_UNDEF;
            L.bFullRange = false;
            L.bColorDescriptionPresent = true;
            L.uiColorPrimaries = CP_BT709;
            L.uiTransferCharacteristics = TRC_BT709;
            L.uiColorMatrix = CM_BT709;

            const int rv = mEnc->InitializeExt(&p);
            if (rv != cmResultSuccess)
            {
                error = "openh264: InitializeExt refused the settings (" + std::to_string(rv) + ")" + lastError();
                return false;
            }
            int fmt = videoFormatI420;
            mEnc->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);
            mThreads = threads;
            mSlices = slices;

            const size_t ysz = (size_t)w * h;
            mCw = (w + 1) / 2;
            mCh = (h + 1) / 2;
            mYuv.resize(ysz + 2 * (size_t)mCw * mCh);
            return true;
        }

        bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) override
        {
            uint8_t* Y = mYuv.data();
            uint8_t* U = Y + (size_t)mW * mH;
            uint8_t* V = U + (size_t)mCw * mCh;
            const uint32_t bands = std::min<uint32_t>((uint32_t)mConvertThreads, mCh);
            if (bands <= 1)
            {
                convertBand(rgba, mW, mH, Y, U, V, mCw, 0, mCh);
            }
            else
            {
                std::vector<std::thread> workers;
                workers.reserve(bands - 1);
                const uint32_t per = (mCh + bands - 1) / bands;
                for (uint32_t i = 1; i < bands; ++i)
                {
                    const uint32_t a = std::min(mCh, i * per), b = std::min(mCh, (i + 1) * per);
                    workers.emplace_back(convertBand, rgba, mW, mH, Y, U, V, mCw, a, b);
                }
                convertBand(rgba, mW, mH, Y, U, V, mCw, 0, std::min(mCh, per));
                for (std::thread& t : workers) t.join();
            }

            SSourcePicture pic;
            std::memset(&pic, 0, sizeof pic);
            pic.iColorFormat = videoFormatI420;
            pic.iPicWidth = (int)mW;
            pic.iPicHeight = (int)mH;
            pic.iStride[0] = (int)mW;
            pic.iStride[1] = pic.iStride[2] = (int)mCw;
            pic.pData[0] = Y;
            pic.pData[1] = U;
            pic.pData[2] = V;
            pic.uiTimeStamp = (long long)(mFrame * 1000ULL / mFps);   // milliseconds (codec_app_def.h SSourcePicture)
            ++mFrame;

            std::memset(&mInfo, 0, sizeof mInfo);
            const int rv = mEnc->EncodeFrame(&pic, &mInfo);
            if (rv != cmResultSuccess)
            {
                error = "openh264: EncodeFrame failed (" + std::to_string(rv) + ")" + lastError();
                return false;
            }
            if (mInfo.eFrameType == videoFrameTypeSkip || mInfo.eFrameType == videoFrameTypeInvalid)
            {
                // Frame skipping is off (bEnableFrameSkip = false), so this is not rate control; a skipped picture
                // would leave the page with nothing to decode, so it counts as a failure (the pipe reopens).
                error = "openh264: the encoder produced no picture";
                return false;
            }
            // The layers' NAL units already carry Annex B start codes; one access unit is all of them in order.
            size_t total = 0;
            for (int i = 0; i < mInfo.iLayerNum; ++i)
            {
                const SLayerBSInfo& li = mInfo.sLayerInfo[i];
                for (int n = 0; n < li.iNalCount; ++n) total += (size_t)li.pNalLengthInByte[n];
            }
            out.data.resize(total);
            size_t pos = 0;
            for (int i = 0; i < mInfo.iLayerNum; ++i)
            {
                const SLayerBSInfo& li = mInfo.sLayerInfo[i];
                size_t len = 0;
                for (int n = 0; n < li.iNalCount; ++n) len += (size_t)li.pNalLengthInByte[n];
                if (len) std::memcpy(out.data.data() + pos, li.pBsBuf, len);
                pos += len;
            }
            out.key = avcHasIdr(out.data);
            if (total == 0) { error = "openh264: empty picture"; return false; }
            return true;
        }

        bool requestKeyFrame() override
        {
            return mEnc && mEnc->ForceIntraFrame(true) == cmResultSuccess;
        }

        std::string name() const override
        {
            return "openh264 (Cisco, " + std::to_string(mThreads) + " threads, " + std::to_string(mSlices) + " slices)";
        }
        std::string codec() const override { return "h264"; }

    private:
        std::string lastError()
        {
            std::lock_guard<std::mutex> lk(mLastError.first);
            return mLastError.second.empty() ? std::string() : ": " + mLastError.second;
        }

        ISVCEncoder* mEnc = nullptr;
        uint32_t mW = 0, mH = 0, mFps = 60, mCw = 0, mCh = 0;
        int mThreads = 1, mSlices = 1, mConvertThreads = 1;
        uint64_t mFrame = 0;
        std::vector<uint8_t> mYuv;
        SFrameBSInfo mInfo;
        std::pair<std::mutex, std::string> mLastError;
    };
}

std::unique_ptr<Encoder> openOpenH264Encoder(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                                             const OpenH264Tuning& tuning, std::string& error)
{
    if (!ensureLoaded(error)) return nullptr;
    auto enc = std::make_unique<OpenH264Encoder>();
    if (!enc->open(width, height, fps, bitrate_kbps, tuning, error)) return nullptr;
    return enc;
}
}
