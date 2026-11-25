/**
 * @file wolfrecorder.cpp
 * @brief WolfViewer: the built-in Video Recorder. See wolfrecorder.h.
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

#include "wolfrecorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fsyspath.h"
#include "llaudioengine.h"
#include "llaudioengine_openal.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "llui.h"
#include "llview.h"
#include "llwebrtc.h"
#include "llwindow.h"
#include "wolfrec_audioenc.h"
#include "wolfrec_mp4.h"
#include "wolfrecordprivacy.h"
#include "wolfvkbridge.h"
#include "wolfvrpipe_video.h"
#include "wolfvrscreen.h"
#include "wolfvrstream.h"

#if LL_OPENAL
#include "AL/al.h"
#include "AL/alc.h"
#endif

#if LL_WINDOWS
#include <shlobj.h>     // SHGetKnownFolderPath, FOLDERID_Videos
#include <knownfolders.h>
#endif

extern bool gSnapshot;
extern bool gCubeSnapshot;

namespace
{
    // ---------------------------------------------------------------------------------------------------------------
    // Constants
    constexpr U32 kRate = 48000;           // the sound track: 48 kHz stereo (the engine mirror's own rate,
                                           // llaudioengine_openal.h MIRROR_RATE, so the world needs no resampling)
    constexpr U32 kSlots = 4;              // pixel buffers in flight: a picture is collected 1-3 frames after its copy
    constexpr size_t kQueueMax = 6;        // pictures waiting for the encoder; more = drop (never stall the viewer)
    constexpr S64 kAudioLagUs = 100000;    // the mix runs 100 ms behind real time: every source has delivered by then
    constexpr F64 kWarnBeforeEnd = 30.0;   // the indicator and the window warn this long before the length limit
    constexpr U64 kDiskReserve = 200ull * 1024 * 1024;   // never fill the disk: stop with this much left (before: refuse)
    constexpr U64 kDiskStopAt = WolfRecorder::DISK_STOP_BYTES;
    constexpr F64 kNeverPlanSecs = 60.0;   // [W111 RECORDER UI 2026-11-24] "Never": room for at least a minute to start
    constexpr S64 kPreviewEveryUs = 250000;   // the window's preview picture: four a second

    using Clock = std::chrono::steady_clock;
    S64 nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

    // [W111 RECORDER UI 2026-11-24] The recording clock: the steady clock less the time spent paused, standing still
    // while paused. The pictures' times (capture) and the sound mix (the worker's mixUpTo) both run on it, so a pause
    // leaves no gap in either and they stay in step. Main thread writes, the worker reads.
    std::mutex gClockM;
    S64 gPausedTotalUs = 0;   // gClockM: time paused so far in this recording
    S64 gPauseStartUs = 0;    // gClockM: when the current pause began; 0 = not paused
    std::atomic<bool> gPaused{false};   // the sound taps and the microphone read this (no lock on the audio threads)
    // [W111 RECORDER UI 2026-11-24] The microphone is really open (the worker opened it; not when it could not be opened
    // or there is no sound encoder): only then do the window and the REC sign say "Microphone on".
    std::atomic<bool> gMicLive{false};
    // [W113 2026-11-25] the resident is muted in voice (WolfRecorder::setVoiceMicMuted, from LLWebRTCVoiceClient): the
    // microphone track then records silence (Mic::pump). Written on the main thread, read on the worker.
    std::atomic<bool> gVoiceMicMuted{false};
    S64 recNowUs()
    {
        std::lock_guard<std::mutex> lk(gClockM);
        return (gPauseStartUs ? gPauseStartUs : nowUs()) - gPausedTotalUs;
    }
    void setClockPaused(bool paused)   // main thread
    {
        std::lock_guard<std::mutex> lk(gClockM);
        if (paused && !gPauseStartUs)
        {
            gPauseStartUs = nowUs();
        }
        else if (!paused && gPauseStartUs)
        {
            gPausedTotalUs += nowUs() - gPauseStartUs;
            gPauseStartUs = 0;
        }
        gPaused = paused;
    }
    void resetClock()   // main thread, at the start of a recording
    {
        std::lock_guard<std::mutex> lk(gClockM);
        gPausedTotalUs = 0;
        gPauseStartUs = 0;
        gPaused = false;
    }

    // The loudest sample of each source since the window last asked (WolfRecorder::levels); worker writes, main reads.
    std::atomic<F32> gPeak[WolfRecorder::SOURCE_COUNT];
    void notePeak(int src, F32 v)
    {
        F32 cur = gPeak[src].load(std::memory_order_relaxed);
        while (v > cur && !gPeak[src].compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    // ---------------------------------------------------------------------------------------------------------------
    // One producer thread, one consumer thread (the same ring as wolfvraudio.cpp). Counts only grow.
    class SpscRing
    {
    public:
        explicit SpscRing(size_t capacity) : mBuf(capacity) {}
        size_t size() const { return mWrite.load(std::memory_order_acquire) - mRead.load(std::memory_order_acquire); }
        void push(const float* p, size_t n)   // producer: what does not fit is dropped (the consumer is far behind)
        {
            const size_t w = mWrite.load(std::memory_order_relaxed);
            const size_t room = mBuf.size() - (w - mRead.load(std::memory_order_acquire));
            n = std::min(n, room);
            for (size_t k = 0; k < n; ++k) mBuf[(w + k) % mBuf.size()] = p[k];
            mWrite.store(w + n, std::memory_order_release);
        }
        size_t pop(float* p, size_t n)        // consumer
        {
            const size_t r = mRead.load(std::memory_order_relaxed);
            n = std::min(n, mWrite.load(std::memory_order_acquire) - r);
            for (size_t k = 0; k < n; ++k) p[k] = mBuf[(r + k) % mBuf.size()];
            mRead.store(r + n, std::memory_order_release);
            return n;
        }
        void skip(size_t n)                   // consumer
        {
            const size_t r = mRead.load(std::memory_order_relaxed);
            n = std::min(n, mWrite.load(std::memory_order_acquire) - r);
            mRead.store(r + n, std::memory_order_release);
        }
        void clear() { skip(size()); }        // consumer side only
    private:
        std::vector<float> mBuf;
        std::atomic<size_t> mWrite{0};
        std::atomic<size_t> mRead{0};
    };

    // Linear resampling to 48 kHz, stereo interleaved; keeps its place between calls (one thread). The voice engine
    // plays at the audio device's rate (WebRTC's AudioDeviceBuffer), which need not be 48 kHz.
    struct ToRate
    {
        double pos = -1.0;
        float last[2] = {};
        void run(const float* in, size_t frames, U32 rate_in, std::vector<float>& out)
        {
            out.clear();
            if (!frames || !rate_in) return;
            if (rate_in == kRate)
            {
                out.assign(in, in + frames * 2);
                last[0] = in[(frames - 1) * 2];
                last[1] = in[(frames - 1) * 2 + 1];
                pos = -1.0;
                return;
            }
            const double step = (double)rate_in / (double)kRate;
            const double end = (double)frames - 1.0;
            while (pos < end)
            {
                const double fl = std::floor(pos);
                const long i = (long)fl;
                const float f = (float)(pos - fl);
                for (int c = 0; c < 2; ++c)
                {
                    const float a = i < 0 ? last[c] : in[(size_t)i * 2 + c];
                    const float b = in[(size_t)(i + 1) * 2 + c];
                    out.push_back(a + (b - a) * f);
                }
                pos += step;
            }
            pos -= (double)frames;
            last[0] = in[(frames - 1) * 2];
            last[1] = in[(frames - 1) * 2 + 1];
        }
    };

    // ---------------------------------------------------------------------------------------------------------------
    // Sound sources. Each one only writes while gSoundOn: the taps stay installed for the whole recording and a tap
    // being removed can still run for one block, so they and their rings are static (live for the whole session).
    std::atomic<bool> gSoundOn{false};
    SpscRing gWorldRing(kRate * 2 * 2);   // 2 s of stereo frames
    SpscRing gVoiceRing(kRate * 2 * 2);
    SpscRing gMicRing(kRate * 2 * 2);

#if LL_OPENAL
    // OpenAL's mixer thread (llaudioengine_openal.cpp onOutputPull): the engine's final mix, the speakers' block.
    void onWorldMix(const F32* stereo, U32 frames, U32 rate, void*)
    {
        if (!gSoundOn.load(std::memory_order_acquire) || gPaused.load(std::memory_order_acquire) || rate != kRate || !stereo) return;
        gWorldRing.push(stereo, (size_t)frames * 2);
    }
#endif

    // WebRTC's playout thread: the voices about to be played (llwebrtc.h setRecorderPlayoutTap).
    struct VoiceTap : public llwebrtc::LLWebRTCPlayoutTap
    {
        std::vector<float> stereo, out;
        ToRate rs;
        void onPlayoutAudio(const int16_t* s, size_t frames, size_t channels, uint32_t rate) override
        {
            if (!gSoundOn.load(std::memory_order_acquire) || gPaused.load(std::memory_order_acquire) || !s || !frames || !channels) return;
            stereo.resize(frames * 2);
            for (size_t i = 0; i < frames; ++i)
            {
                const float l = (float)s[i * channels] / 32768.f;
                const float r = channels > 1 ? (float)s[i * channels + 1] / 32768.f : l;
                stereo[i * 2] = l;
                stereo[i * 2 + 1] = r;
            }
            rs.run(stereo.data(), frames, rate, out);
            gVoiceRing.push(out.data(), out.size() & ~(size_t)1);
        }
    };
    VoiceTap sVoiceTap;

    // ---------------------------------------------------------------------------------------------------------------
    // Pictures: captured on the main thread, encoded on the worker.
    struct Frame
    {
        std::vector<U8> rgba;
        S64 t_us = 0;   // capture time (steady clock)
    };

    // Everything the main thread and the worker share.
    struct Shared
    {
        std::mutex m;
        std::condition_variable cv;
        std::deque<Frame> queue;          // m
        std::vector<std::vector<U8>> pool;   // m: spent picture buffers, reused
        bool stop_wanted = false;         // m
        // worker -> main (m)
        std::string error, notice, encoder_name, file;
        U64 bytes = 0;
        bool saved = false;
        bool worker_done = true;
        bool encoder_ready = false;
        std::deque<S64> written_us;       // write times of the pictures of the last two seconds (the real fps)
        F64 length = 0.0;                 // the saved video's length in seconds
        std::vector<U8> preview;          // [W111 RECORDER UI 2026-11-24] the window's preview picture (PREVIEW_WIDTH x
        U32 preview_serial = 0;           // PREVIEW_HEIGHT RGB, bottom row first) and its number (0 = none yet)
    };
    Shared gS;
    std::thread gWorker;

    // Main-thread state
    struct Session
    {
        WolfRecorder::State state = WolfRecorder::State::IDLE;
        S64 start_us = 0;
        F64 limit = 300.0;
        U32 width = 1280, height = 720, fps = 30, kbps = 6000;
        bool ui = false, world = false, voice = false, mic = false;
        bool warned = false;
        S64 next_due_us = 0;
        U32 dropped = 0;
        F64 last_disk_check = 0.0;
        U32 captured_frame = 0;           // LLFrameTimer frame of the last capture: one picture a frame
    };
    Session gSess;
    std::string gLastError, gLastNotice;  // main thread's copy for status() after the worker has ended
    std::atomic<F32> gLevelWorld{1.f}, gLevelVoice{1.f}, gLevelMic{1.f};

    // ---------------------------------------------------------------------------------------------------------------
    // GPU side: scaled copy + pixel buffer ring. Main thread (the GL thread) only.
    struct Gpu
    {
        GLuint tex = 0, fbo = 0;
        GLuint pbo[kSlots] = {};
        GLsync fence[kSlots] = {};
        S64 t_us[kSlots] = {};
        U64 order[kSlots] = {};
        U64 issues = 0;
        U32 next = 0;
        U32 w = 0, h = 0;
        S32 win_w = 0, win_h = 0;
        bool failed = false;
    };
    Gpu gGpu;

    void freeGpu()
    {
        for (U32 i = 0; i < kSlots; ++i)
        {
            if (gGpu.fence[i]) glDeleteSync(gGpu.fence[i]);
            gGpu.fence[i] = nullptr;
        }
        if (gGpu.pbo[0]) glDeleteBuffers((GLsizei)kSlots, gGpu.pbo);
        for (U32 i = 0; i < kSlots; ++i) gGpu.pbo[i] = 0;
        if (gGpu.fbo) glDeleteFramebuffers(1, &gGpu.fbo);
        if (gGpu.tex) glDeleteTextures(1, &gGpu.tex);
        gGpu.fbo = gGpu.tex = 0;
        gGpu.w = gGpu.h = 0;
        gGpu.win_w = gGpu.win_h = 0;
    }

    bool makeGpu(U32 w, U32 h)
    {
        freeGpu();
        GLint tex2d = 0, draw_fb = 0, pack = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        while (glGetError() != GL_NO_ERROR) {}
        glGenTextures(1, &gGpu.tex);
        glBindTexture(GL_TEXTURE_2D, gGpu.tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &gGpu.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gGpu.fbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gGpu.tex, 0);
        const GLenum fb_ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        glGenBuffers((GLsizei)kSlots, gGpu.pbo);
        const GLsizeiptr bytes = (GLsizeiptr)w * h * 4;
        for (U32 i = 0; i < kSlots; ++i)
        {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gGpu.pbo[i]);
            // GL 4.6 core 6.2 BufferData: STREAM_READ = written once by GL, read a few times by the application
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        const bool ok = fb_ok == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
        glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
        if (!ok)
        {
            freeGpu();
            return false;
        }
        gGpu.w = w;
        gGpu.h = h;
        return true;
    }

    // Hands a finished copy to the worker, or drops it when the worker is behind.
    void queuePicture(const void* p, size_t bytes, S64 t_us)
    {
        std::unique_lock<std::mutex> lk(gS.m);
        if (gS.queue.size() >= kQueueMax)
        {
            ++gSess.dropped;
            return;
        }
        Frame f;
        if (!gS.pool.empty())
        {
            f.rgba.swap(gS.pool.back());
            gS.pool.pop_back();
        }
        f.rgba.resize(bytes);
        std::memcpy(f.rgba.data(), p, bytes);
        f.t_us = t_us;
        gS.queue.push_back(std::move(f));
        lk.unlock();
        gS.cv.notify_one();
    }

    // Collects every finished copy, oldest first (pictures must stay in order); stops at the first one still busy.
    void collect()
    {
        if (!gGpu.fbo) return;
        GLint pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        for (;;)
        {
            int oldest = -1;
            for (U32 i = 0; i < kSlots; ++i)
                if (gGpu.fence[i] && (oldest < 0 || gGpu.order[i] < gGpu.order[oldest])) oldest = (int)i;
            if (oldest < 0) break;
            // GL 4.6 core 4.1.2 ClientWaitSync with a zero timeout: a poll, never a wait
            const GLenum r = glClientWaitSync(gGpu.fence[oldest], 0, 0);
            if (r == GL_TIMEOUT_EXPIRED) break;
            glDeleteSync(gGpu.fence[oldest]);
            gGpu.fence[oldest] = nullptr;
            if (r == GL_WAIT_FAILED) { ++gSess.dropped; continue; }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, gGpu.pbo[oldest]);
            const size_t bytes = (size_t)gGpu.w * gGpu.h * 4;
            if (const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT))
            {
                queuePicture(p, bytes, gGpu.t_us[oldest]);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            else
            {
                ++gSess.dropped;
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
    }

    // The fitted rectangle of the window inside the picture (letterbox / pillarbox), in picture pixels.
    void fitRect(S32 ww, S32 wh, U32 w, U32 h, S32& x0, S32& y0, S32& x1, S32& y1)
    {
        const F64 sw = (F64)w / ww, sh = (F64)h / wh;
        const F64 s = std::min(sw, sh);
        const S32 fw = (S32)std::lround(ww * s), fh = (S32)std::lround(wh * s);
        x0 = ((S32)w - fw) / 2;
        y0 = ((S32)h - fh) / 2;
        x1 = x0 + fw;
        y1 = y0 + fh;
    }

    // [RECORDING PRIVACY 2026-11-25] Ticket 1060. Shapes over the people who asked not to be recorded, drawn into the
    // picture texture (bound as the draw framebuffer) after the window was scaled into it and before it is read back.
    // Plain scissored clears (GL 4.6 core 17.4.3 Clear: only the scissor box is written, after the colour mask; no
    // shader, no blending, no vertex state to disturb), so a rounded shape is a stack of bands. The picture texture is
    // upside down against the window (capture step 1), so its row 0 is the top of the scene.
    void fillRect(S32 l, S32 t, S32 r, S32 b, const LLColor4& c)   // picture pixels, t < b (t = nearer the top)
    {
        if (r <= l || b <= t) return;
        glScissor(l, t, r - l, b - t);
        glClearColor(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    // A rectangle with rounded corners of `radius` (bands of a few pixels at the top and bottom, each inset to the curve)
    void fillRounded(S32 l, S32 t, S32 r, S32 b, S32 radius, const LLColor4& c)
    {
        radius = llmin(radius, (r - l) / 2, (b - t) / 2);
        if (radius < 2)
        {
            fillRect(l, t, r, b, c);
            return;
        }
        const S32 step = llmax(1, radius / 6);
        for (S32 dy = 0; dy < radius; dy += step)
        {
            // the curve's inset at the middle of this band: radius - sqrt(radius^2 - (radius - y)^2)
            const F32 y = (F32)radius - ((F32)dy + 0.5f * (F32)step);
            const S32 in = (S32)std::lround((F32)radius - std::sqrt(llmax(0.f, (F32)(radius * radius) - y * y)));
            const S32 h = llmin(step, radius - dy);
            fillRect(l + in, t + dy, r - in, t + dy + h, c);   // top band
            fillRect(l + in, b - dy - h, r - in, b - dy, c);   // bottom band
        }
        fillRect(l, t + radius, r, b - radius, c);
    }
    // One left-out person: a soft grey shape over the whole of them, with a lighter head-and-shoulders sign in it, so the
    // video shows that someone is there without showing who. Picture pixels, t < b.
    void drawLeftOut(S32 l, S32 t, S32 r, S32 b)
    {
        const S32 w = r - l, h = b - t;
        if (w <= 0 || h <= 0) return;
        static const LLColor4 kShape(0.42f, 0.43f, 0.45f, 1.f);
        static const LLColor4 kSign(0.60f, 0.61f, 0.63f, 1.f);
        fillRounded(l, t, r, b, llmin(w, h) / 6, kShape);
        if (w < 24 || h < 32) return;   // too small for the sign to read
        // the sign: a head and shoulders, centred, in proportion to the narrower side
        const S32 cx = l + w / 2;
        const S32 unit = llmin(w, h * 2 / 3);
        const S32 head = llmax(4, unit / 5);              // head half-width
        const S32 head_top = t + h / 6;
        fillRounded(cx - head, head_top, cx + head, head_top + 2 * head, head, kSign);
        const S32 sh_top = head_top + 2 * head + llmax(2, head / 3);
        const S32 sh_half = llmax(6, unit * 2 / 5);
        const S32 sh_bottom = llmin(b - h / 10, sh_top + 3 * head);
        fillRounded(cx - sh_half, sh_top, cx + sh_half, sh_bottom, head, kSign);
    }
    void drawPrivacyMasks(bool name_tags, S32 ww, S32 wh, S32 x0, S32 y0, S32 x1, S32 y1)
    {
        static std::vector<LLRectf> rects;   // main thread only; kept to avoid an allocation a picture
        WolfRecordPrivacy::recordingMasks(name_tags, rects);
        if (rects.empty()) return;
        GLint box[4];
        GLfloat cc[4];
        GLboolean mask[4];
        glGetIntegerv(GL_SCISSOR_BOX, box);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_SCISSOR_TEST);
        // window raw pixels (bottom-left origin) -> picture pixels: the same mapping as the blit (fitRect, flipped)
        const F32 sx = (F32)(x1 - x0) / (F32)ww, sy = (F32)(y1 - y0) / (F32)wh;
        for (const LLRectf& r : rects)
        {
            const S32 l = llclamp((S32)std::floor(x0 + r.mLeft * sx), x0, x1);
            const S32 rr = llclamp((S32)std::ceil(x0 + r.mRight * sx), x0, x1);
            const S32 t = llclamp((S32)std::floor(y1 - r.mTop * sy), y0, y1);      // the window's top = the picture's row y0
            const S32 b = llclamp((S32)std::ceil(y1 - r.mBottom * sy), y0, y1);
            drawLeftOut(l, t, rr, b);
        }
        glDisable(GL_SCISSOR_TEST);   // capture() turned it off for the copy and puts it back itself
        glScissor(box[0], box[1], box[2], box[3]);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
    }

    // One copy from `read_fbo` (0 = the back buffer) into the next pixel buffer. Main thread, inside render_ui.
    // [RECORDING PRIVACY 2026-11-25/] `name_tags`: the copied picture has the name tags in it (drawPrivacyMasks).
    void capture(GLuint read_fbo, bool name_tags)
    {
        const S32 ww = gViewerWindow->getWindowWidthRaw(), wh = gViewerWindow->getWindowHeightRaw();
        if (ww <= 0 || wh <= 0) return;
        if (gGpu.failed) return;
        if (gGpu.w != gSess.width || gGpu.h != gSess.height || !gGpu.fbo)
        {
            if (!makeGpu(gSess.width, gSess.height))
            {
                gGpu.failed = true;
                std::lock_guard<std::mutex> lk(gS.m);
                gS.error = "Your graphics card could not make the picture copies the recorder needs, so nothing could "
                           "be recorded.";
                gS.stop_wanted = true;
                gS.cv.notify_one();
                return;
            }
        }
        const S64 now = recNowUs();   // [W111 RECORDER UI 2026-11-24/] the recording clock (no paused time)
        const int slot = (int)gGpu.next;
        if (gGpu.fence[slot])
        {
            // Still busy kSlots pictures later: the GPU is behind; this picture is skipped rather than waited for.
            ++gSess.dropped;
            return;
        }
        gGpu.next = (gGpu.next + 1) % kSlots;

        GLint read_fb = 0, draw_fb = 0, pack = 0, read_buf = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        glGetIntegerv(GL_READ_BUFFER, &read_buf);
        const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);   // the pixels as they are on screen (as wolfvrscreen.cpp captureInto)
        if (scissor) glDisable(GL_SCISSOR_TEST);

        // 1. the window -> the picture texture, scaled and fitted, upside down so the read comes out top row first
        //    (GL 4.6 core 18.3.1 BlitFramebuffer: a destination rectangle with y0 > y1 flips; LINEAR for scaling)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        glReadBuffer(read_fbo ? GL_COLOR_ATTACHMENT0 : GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gGpu.fbo);
        if (ww != gGpu.win_w || wh != gGpu.win_h)
        {
            GLfloat cc[4];
            glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);   // the black bars
            glClearColor(cc[0], cc[1], cc[2], cc[3]);
            gGpu.win_w = ww;
            gGpu.win_h = wh;
        }
        S32 x0, y0, x1, y1;
        fitRect(ww, wh, gGpu.w, gGpu.h, x0, y0, x1, y1);
        glBlitFramebuffer(0, 0, ww, wh, x0, y1, x1, y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        // [RECORDING PRIVACY 2026-11-25/] 1b. the people who asked not to be recorded, left out of this picture only
        drawPrivacyMasks(name_tags, ww, wh, x0, y0, x1, y1);

        // 2. the picture -> its pixel buffer, without waiting (GL 4.6 core 18.2.2 ReadPixels into a bound
        //    PIXEL_PACK_BUFFER: the last argument is an offset and the call returns at once)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gGpu.fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, gGpu.pbo[slot]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, (GLsizei)gGpu.w, (GLsizei)gGpu.h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gGpu.fence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gGpu.t_us[slot] = now;
        gGpu.order[slot] = ++gGpu.issues;
        glFlush();   // start the copy now; it is collected on a later frame (collect, from the idle loop)

        glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_fb);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
        glReadBuffer((GLenum)read_buf);
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }

    // Is a picture due now (the chosen frame rate, one per drawn frame at most)?
    bool pictureDue()
    {
        if (gSess.state != WolfRecorder::State::RECORDING && gSess.state != WolfRecorder::State::STARTING) return false;
        if (gPaused.load()) return false;   // [W111 RECORDER UI 2026-11-24/] paused: no pictures
        if (gSnapshot || gCubeSnapshot || !gViewerWindow) return false;
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame == gSess.captured_frame) return false;   // VR stereo draws twice a frame
        const S64 now = nowUs();
        if (now < gSess.next_due_us) return false;
        const S64 step = 1000000 / (S64)gSess.fps;
        // keep the rhythm; after a slow frame start again from now rather than catch up with a burst
        gSess.next_due_us = (now - gSess.next_due_us > step) ? now + step : gSess.next_due_us + step;
        gSess.captured_frame = frame;
        return true;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Folders and files
    std::string videosFolder()
    {
#if LL_WINDOWS
        // Source: https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath -
        // FOLDERID_Videos (https://learn.microsoft.com/windows/win32/shell/knownfolderid), freed with CoTaskMemFree
        PWSTR path = nullptr;
        std::string out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path)) && path)
        {
            out = ll_convert_wide_to_string(std::wstring(path));
        }
        if (path) CoTaskMemFree(path);
        if (!out.empty()) return out;
        return gDirUtilp->getOSUserDir();
#elif LL_DARWIN
        // Source: Apple File System Programming Guide, "macOS Library Directory Details" / NSMoviesDirectory = ~/Movies
        const char* home = getenv("HOME");
        return std::string(home ? home : "") + "/Movies";
#else
        // Source: freedesktop.org xdg-user-dirs - $XDG_CONFIG_HOME/user-dirs.dirs (default ~/.config/user-dirs.dirs)
        // holds lines XDG_VIDEOS_DIR="$HOME/Videos" (a path relative to $HOME or absolute); default ~/Videos.
        const char* home_c = getenv("HOME");
        const std::string home = home_c ? home_c : "";
        const char* cfg = getenv("XDG_CONFIG_HOME");
        const std::string dirs = (cfg && *cfg ? std::string(cfg) : home + "/.config") + "/user-dirs.dirs";
        std::ifstream in(dirs);
        std::string line;
        while (std::getline(in, line))
        {
            const std::string key = "XDG_VIDEOS_DIR=\"";
            if (line.compare(0, key.size(), key) != 0) continue;
            std::string v = line.substr(key.size());
            const size_t q = v.find('"');
            if (q == std::string::npos) break;
            v.resize(q);
            if (v.compare(0, 5, "$HOME") == 0) v = home + v.substr(5);
            if (!v.empty() && v[0] == '/' && v != home && v != home + "/") return v;   // the spec: $HOME itself = unset
            break;
        }
        return home + "/Videos";
#endif
    }

    std::string sep() { return gDirUtilp->getDirDelimiter(); }

    // "WolfViewer 2026-11-24 06-30-12.mp4" (local time; no characters any file system refuses)
    std::string newFileName(const std::string& dir)
    {
        const std::time_t t = std::time(nullptr);
        std::tm tmv{};
#if LL_WINDOWS
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "WolfViewer %Y-%m-%d %H-%M-%S", &tmv);
        std::string base = buf, name = dir + sep() + base + ".mp4";
        std::error_code ec;
        for (int n = 2; std::filesystem::exists(fsyspath(name), ec) && n < 100; ++n)
        {
            name = dir + sep() + base + " (" + std::to_string(n) + ").mp4";
        }
        return name;
    }

    // Free bytes on the disk of `dir`; false when the system cannot say (the caller then neither refuses nor stops).
    bool freeBytes(const std::string& dir, U64& out)
    {
        std::error_code ec;
        const std::filesystem::space_info si = std::filesystem::space(fsyspath(dir), ec);
        out = ec ? 0 : (U64)si.available;
        return !ec;
    }

    std::string minutes(F64 secs)
    {
        const S32 m = (S32)(secs / 60.0);
        return std::to_string(m) + (m == 1 ? " minute" : " minutes");
    }

    std::string mb(U64 bytes) { return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB"; }

    // ---------------------------------------------------------------------------------------------------------------
    // Microphone: OpenAL capture, opened by the worker when the recording starts and closed when it ends - it is never
    // open at any other time (Paul, 24 Nov: "the mic must only ever record while the recorder is running").
#if LL_OPENAL
    struct Mic
    {
        ALCdevice* dev = nullptr;
        std::vector<int16_t> buf;
        bool open(std::string& error)
        {
            // Source: OpenAL 1.1 spec 6.4.1 alcCaptureOpenDevice(name NULL = the default input, frequency, format,
            // buffersize in sample frames); 6.4.2 alcCaptureStart. Mono 16-bit is the one format every backend takes.
            dev = alcCaptureOpenDevice(nullptr, kRate, AL_FORMAT_MONO16, (ALCsizei)(kRate / 2));
            if (!dev)
            {
                error = "Your microphone could not be opened. Check that one is plugged in and that WolfViewer is "
                        "allowed to use it.";
                return false;
            }
            alcCaptureStart(dev);
            return true;
        }
        // Moves what has been recorded into the ring (as stereo float). Any thread: ALC calls are thread-safe.
        void pump()
        {
            if (!dev) return;
            ALCint n = 0;
            // Source: OpenAL 1.1 spec 6.4.3 - ALC_CAPTURE_SAMPLES, the sample frames ready; alcCaptureSamples reads them
            alcGetIntegerv(dev, ALC_CAPTURE_SAMPLES, 1, &n);
            if (n <= 0) return;
            buf.resize((size_t)n);
            alcCaptureSamples(dev, buf.data(), n);
            // [W113 2026-11-25] Paul's rule: no microphone sound while the resident is muted in voice. The capture is
            // still read (the device's buffer must not fill) and the same number of samples goes into the ring, as
            // silence, so the sound keeps its length and stays in step with the pictures.
            std::vector<float> st((size_t)n * 2, 0.f);
            if (!gVoiceMicMuted.load(std::memory_order_acquire))
            {
                for (ALCint i = 0; i < n; ++i) st[(size_t)i * 2] = st[(size_t)i * 2 + 1] = (float)buf[(size_t)i] / 32768.f;
            }
            gMicRing.push(st.data(), st.size());
        }
        // [W111 RECORDER UI 2026-11-24] Paused: what was captured before the pause goes in, then the capture stops
        // (OpenAL 1.1 spec 6.4.2: alcCaptureStop halts capture, the device stays open; alcCaptureStart carries on).
        bool stopped = false;
        void setPaused(bool paused)
        {
            if (!dev || paused == stopped) return;
            if (paused)
            {
                pump();
                alcCaptureStop(dev);
            }
            else
            {
                alcCaptureStart(dev);
            }
            stopped = paused;
        }
        void close()
        {
            if (!dev) return;
            alcCaptureStop(dev);
            alcCaptureCloseDevice(dev);
            dev = nullptr;
        }
    };
#endif

    // ---------------------------------------------------------------------------------------------------------------
    // The worker: encoders, the mixer and the file.
    struct WorkerConfig
    {
        std::string path;
        U32 width, height, fps, kbps;
        bool world, voice, mic;
    };

    void setWorkerError(const std::string& e)
    {
        std::lock_guard<std::mutex> lk(gS.m);
        if (gS.error.empty()) gS.error = e;
        gS.stop_wanted = true;
    }

    // [W111 RECORDER UI 2026-11-24] The window's preview: a box-sampled PREVIEW_WIDTH x PREVIEW_HEIGHT copy of a picture
    // the worker already holds (RGBA, top row first - capture() flips it), as RGB bottom row first for a GL texture.
    // Four samples a pixel: ~150k reads, four times a second, on this thread - nothing more from the graphics card.
    void makePreview(const std::vector<U8>& rgba, U32 w, U32 h)
    {
        constexpr U32 PW = WolfRecorder::PREVIEW_WIDTH, PH = WolfRecorder::PREVIEW_HEIGHT;
        if (rgba.size() < (size_t)w * h * 4 || w < PW * 2 || h < PH * 2) return;
        std::vector<U8> out((size_t)PW * PH * 3);
        const U32 cw = w / PW, ch = h / PH;   // the source cell of one preview pixel
        for (U32 y = 0; y < PH; ++y)
        {
            const U32 sy = (PH - 1 - y) * h / PH;   // flip: the picture is top row first, the texture bottom row first
            for (U32 x = 0; x < PW; ++x)
            {
                const U32 sx = x * w / PW;
                U32 acc[3] = {};
                for (U32 k = 0; k < 4; ++k)
                {
                    const U32 px = sx + (k & 1) * cw / 2, py = sy + (k >> 1) * ch / 2;
                    const U8* p = &rgba[((size_t)py * w + px) * 4];
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                }
                U8* o = &out[((size_t)y * PW + x) * 3];
                o[0] = (U8)(acc[0] / 4);
                o[1] = (U8)(acc[1] / 4);
                o[2] = (U8)(acc[2] / 4);
            }
        }
        std::lock_guard<std::mutex> lk(gS.m);
        gS.preview.swap(out);
        ++gS.preview_serial;
    }

    void workerMain(WorkerConfig cfg)
    {
        std::string err, tried;
        // The video encoder: H.264 only (wolfvrpipe_video.h openEncoder "h264": the OS hardware encoder, its software
        // one, then OpenH264; never JPEG)
        std::unique_ptr<wolfvr::Encoder> venc = wolfvr::openEncoder("h264", cfg.width, cfg.height, cfg.fps, cfg.kbps, tried);
        if (!venc)
        {
            LL_WARNS("WolfRecorder") << "no H.264 encoder: " << tried << LL_ENDL;
            std::string msg = "No video encoder could be started on this computer, so nothing was recorded.";
            if (WolfVRStream::codecState() == 0)
            {
                msg += " Press \"Get the video codec\" to download the free H.264 codec from Cisco, then try again.";
            }
            setWorkerError(msg);
        }
        const bool any_sound = cfg.world || cfg.voice || cfg.mic;
        std::unique_ptr<wolfrec::AudioEncoder> aenc;
        if (venc && any_sound)
        {
            std::string atried;
            aenc = wolfrec::openAudioEncoder("auto", kRate, 2, 160000, atried);
            if (!aenc)
            {
                LL_WARNS("WolfRecorder") << "no sound encoder: " << atried << LL_ENDL;
                std::lock_guard<std::mutex> lk(gS.m);
                gS.notice = "The sound encoder could not be started, so this video has no sound.";
            }
#if LL_WINDOWS || LL_DARWIN
            else if (aenc->codec() == "opus")
            {
                // wolfrec_audioenc.h openAudioEncoder: the system AAC encoder failed and Opus stood in; Opus sound in
                // MP4 does not play in QuickTime before macOS 14 (Sonoma)
                LL_WARNS("WolfRecorder") << "system AAC encoder unavailable, sound in Opus: " << atried << LL_ENDL;
                std::lock_guard<std::mutex> lk(gS.m);
                gS.notice = "This computer's own sound encoder could not be used, so the sound is in Opus. It plays in "
                            "browsers, VLC and Windows, but not in QuickTime on Macs older than macOS 14.";
            }
#endif
        }
#if LL_OPENAL
        Mic mic;
        if (venc && aenc && cfg.mic && !mic.open(err))
        {
            std::lock_guard<std::mutex> lk(gS.m);
            gS.notice = err + " The video is recorded without it.";
            cfg.mic = false;
        }
        gMicLive = mic.dev != nullptr;
#else
        cfg.mic = false;
#endif
        wolfrec::Mp4Writer mp4;
        if (venc && !mp4.open(cfg.path, err))
        {
            setWorkerError("The video file could not be made: " + err);
            venc.reset();
        }
        if (venc)
        {
            mp4.setVideo(cfg.width, cfg.height);
            if (aenc) mp4.setAudio(*aenc);
            std::lock_guard<std::mutex> lk(gS.m);
            gS.encoder_name = venc->name() + (aenc ? ", " + aenc->name() : std::string());
            gS.encoder_ready = true;
            gS.file = cfg.path;
        }
        LL_INFOS("WolfRecorder") << "recording to '" << cfg.path << "' " << cfg.width << "x" << cfg.height << " at "
                                 << cfg.fps << " fps, " << cfg.kbps << " kbps, video " << (venc ? venc->name() : "none")
                                 << ", sound " << (aenc ? aenc->name() : "none") << " (world " << cfg.world << ", voice "
                                 << cfg.voice << ", mic " << cfg.mic << ")" << LL_ENDL;

        // Timeline: picture time 0 = the first picture's capture time; the sound is mixed from that same instant.
        S64 t0 = -1;
        S64 last_pts = 0, prev_pts = -1;
        S64 next_preview_us = 0;
        U64 mixed = 0;           // sound frames mixed so far
        bool trimmed = false;    // the sound that arrived before t0 has been dropped
        std::vector<float> mix, part;
        std::vector<wolfrec::AudioPacket> packets;
        wolfvr::EncodedFrame ef;
        bool failed = !venc;
        bool audio_dead = false;   // the sound encoder failed: no more sound (the encoder object lives on - the
                                   // writer reads its inputFrames() in finish(), wolfrec_mp4.h setAudio)

        auto mixUpTo = [&](S64 until_us) {
            if (!aenc || audio_dead || t0 < 0 || failed) return;
            const S64 target = (until_us - t0) * (S64)kRate / 1000000;
            if (!trimmed)
            {
                // drop what each source delivered before the first picture (it arrived since the taps went on)
                const size_t want = (size_t)std::max<S64>(0, (recNowUs() - t0) * (S64)kRate / 1000000) * 2;
                for (SpscRing* r : { &gWorldRing, &gVoiceRing, &gMicRing })
                {
                    const size_t have = r->size();
                    if (have > want) r->skip((have - want) & ~(size_t)1);
                }
                trimmed = true;
            }
            while ((S64)mixed < target)
            {
                const size_t n = (size_t)std::min<S64>(target - (S64)mixed, kRate / 50);   // 20 ms at a time
                mix.assign(n * 2, 0.f);
                struct Src { SpscRing* ring; bool on; F32 level; int id; };
                const Src srcs[] = { { &gWorldRing, cfg.world, gLevelWorld.load(), WolfRecorder::SOURCE_WORLD },
                                     { &gVoiceRing, cfg.voice, gLevelVoice.load(), WolfRecorder::SOURCE_VOICE },
                                     { &gMicRing, cfg.mic, gLevelMic.load(), WolfRecorder::SOURCE_MIC } };
                for (const Src& s : srcs)
                {
                    if (!s.on) { s.ring->clear(); continue; }
                    // A source on a slightly faster clock piles up: keep at most 300 ms beyond what is needed, so it
                    // never drifts out of step with the pictures (a short skip, rarely: clocks differ by ~0.01%).
                    const size_t have = s.ring->size();
                    const size_t limit = (n + kRate * 3 / 10) * 2;
                    if (have > limit) s.ring->skip((have - (n + kRate / 10) * 2) & ~(size_t)1);
                    part.resize(n * 2);
                    const size_t got = s.ring->pop(part.data(), n * 2);   // less = silence for the rest
                    F32 peak = 0.f;   // [W111 RECORDER UI 2026-11-24/] the window's level meter: as it goes into the mix
                    for (size_t k = 0; k < got; ++k)
                    {
                        const F32 v = part[k] * s.level;
                        mix[k] += v;
                        peak = std::max(peak, std::fabs(v));
                    }
                    notePeak(s.id, peak);
                }
                for (float& x : mix) x = std::max(-1.f, std::min(1.f, x));
                packets.clear();
                if (!aenc->encode(mix.data(), (uint32_t)n, packets, err))
                {
                    LL_WARNS("WolfRecorder") << "sound encoder: " << err << LL_ENDL;
                    std::lock_guard<std::mutex> lk(gS.m);
                    gS.notice = "The sound encoder stopped working; the rest of the video has no sound.";
                    audio_dead = true;
                    return;
                }
                for (const wolfrec::AudioPacket& p : packets)
                {
                    if (!mp4.writeAudio(p, err)) { failed = true; setWorkerError(err); return; }
                }
                mixed += n;
            }
        };

        for (;;)
        {
            Frame f;
            bool have = false, stop = false;
            {
                std::unique_lock<std::mutex> lk(gS.m);
                gS.cv.wait_for(lk, std::chrono::milliseconds(10), [] { return !gS.queue.empty() || gS.stop_wanted; });
                if (!gS.queue.empty())
                {
                    f = std::move(gS.queue.front());
                    gS.queue.pop_front();
                    have = true;
                }
                stop = gS.stop_wanted && gS.queue.empty();
            }
#if LL_OPENAL
            if (cfg.mic)
            {
                mic.setPaused(gPaused.load());   // [W111 RECORDER UI 2026-11-24/] no capture while paused
                if (!mic.stopped) mic.pump();
            }
#endif
            if (have)
            {
                if (!failed)
                {
                    if (t0 < 0) t0 = f.t_us;
                    if (nowUs() >= next_preview_us)   // [W111 RECORDER UI 2026-11-24/] the window's preview
                    {
                        next_preview_us = nowUs() + kPreviewEveryUs;
                        makePreview(f.rgba, cfg.width, cfg.height);
                    }
                    S64 pts = f.t_us - t0;
                    if (pts <= prev_pts) pts = prev_pts + 1;   // strictly rising
                    if (!venc->encode(f.rgba.data(), ef, err))
                    {
                        failed = true;
                        setWorkerError("The video encoder stopped working: " + err);
                    }
                    else if (!ef.data.empty())
                    {
                        if (!mp4.writeVideo(ef.data, pts, ef.key, err)) { failed = true; setWorkerError(err); }
                        else
                        {
                            prev_pts = last_pts = pts;
                            std::lock_guard<std::mutex> lk(gS.m);
                            gS.written_us.push_back(nowUs());
                            while (!gS.written_us.empty() && gS.written_us.front() < nowUs() - 2000000) gS.written_us.pop_front();
                            gS.bytes = mp4.bytesWritten();
                        }
                    }
                }
                std::lock_guard<std::mutex> lk(gS.m);
                gS.pool.push_back(std::move(f.rgba));
            }
            mixUpTo(recNowUs() - kAudioLagUs);
            if (stop) break;
        }

        // The end: the sound up to the end of the last picture (its duration = one frame), then the file's index.
        const S64 frame_us = 1000000 / (S64)cfg.fps;
        if (t0 >= 0 && !failed && aenc && !audio_dead)
        {
            // what the sources still hold for the last 100 ms arrives within that time: wait for it (bounded)
            const S64 end_us = t0 + last_pts + frame_us;
            // (the recording clock: stop() ends any pause first, so it runs)
            while (recNowUs() < end_us + kAudioLagUs / 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            mixUpTo(end_us);
            packets.clear();
            if (!audio_dead && aenc->flush(packets, err))
            {
                for (const wolfrec::AudioPacket& p : packets)
                    if (!mp4.writeAudio(p, err)) { setWorkerError(err); break; }
            }
        }
        gSoundOn = false;
#if LL_OPENAL
        mic.close();
        gMicLive = false;
#endif
        bool saved = false;
        if (venc && t0 >= 0)
        {
            saved = mp4.finish(frame_us, err);
            if (!saved) setWorkerError("The video could not be finished: " + err);
        }
        else if (venc)
        {
            mp4.finish(frame_us, err);
            std::error_code ec;
            std::filesystem::remove(fsyspath(cfg.path), ec);   // nothing was recorded: no empty file left behind
            setWorkerError("No pictures reached the recorder, so nothing was saved.");
        }
        {
            std::lock_guard<std::mutex> lk(gS.m);
            gS.saved = saved;
            gS.bytes = mp4.bytesWritten();
            gS.length = saved ? (F64)(last_pts + frame_us) / 1e6 : 0.0;
            gS.worker_done = true;
        }
        LL_INFOS("WolfRecorder") << (saved ? "saved '" : "not saved '") << cfg.path << "', " << mp4.bytesWritten()
                                 << " bytes, " << (last_pts / 1000) << " ms of pictures" << LL_ENDL;
    }

    // ---------------------------------------------------------------------------------------------------------------
    void setTaps(bool on)
    {
#if LL_OPENAL
        if (LLAudioEngine_OpenAL* al = dynamic_cast<LLAudioEngine_OpenAL*>(gAudiop))
        {
            al->setRecorderMixTap(on && gSess.world ? &onWorldMix : nullptr, nullptr);
        }
#endif
        if (llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface())
        {
            dev->setRecorderPlayoutTap(on && gSess.voice ? &sVoiceTap : nullptr);
        }
    }

    void joinWorker()
    {
        if (gWorker.joinable()) gWorker.join();
    }

    // Main thread, every frame while the recorder is busy.
    void idle(void*)
    {
        if (gSess.state == WolfRecorder::State::IDLE) return;
        collect();
        // the level sliders act at once, also during a recording
        static LLCachedControl<F32> lw(gSavedSettings, "WolfRecorderLevelWorld", 1.f);
        static LLCachedControl<F32> lv(gSavedSettings, "WolfRecorderLevelVoice", 1.f);
        static LLCachedControl<F32> lm(gSavedSettings, "WolfRecorderLevelMic", 1.f);
        gLevelWorld = llclamp((F32)lw, 0.f, 2.f);
        gLevelVoice = llclamp((F32)lv, 0.f, 2.f);
        gLevelMic = llclamp((F32)lm, 0.f, 2.f);
        const F64 now = LLFrameTimer::getElapsedSeconds();
        bool worker_done = false, stop_wanted = false;
        {
            std::lock_guard<std::mutex> lk(gS.m);
            worker_done = gS.worker_done;
            stop_wanted = gS.stop_wanted;
            if (gS.encoder_ready && gSess.state == WolfRecorder::State::STARTING) gSess.state = WolfRecorder::State::RECORDING;
        }
        if (gSess.state == WolfRecorder::State::RECORDING || gSess.state == WolfRecorder::State::STARTING)
        {
            if (stop_wanted)   // the worker failed: the main thread finishes the stop
            {
                WolfRecorder::stop();
            }
            else
            {
                const F64 secs = (F64)(recNowUs() - gSess.start_us) / 1e6;
                if (gSess.limit > 0.0 && secs >= gSess.limit)   // 0 = "Never" (the disk guard below still stops)
                {
                    gLastNotice = "The recording stopped at its length limit (" + minutes(gSess.limit) +
                                  "). You can change the limit in the Video Recorder window.";
                    WolfRecorder::stop();
                }
                else if (now - gSess.last_disk_check > 2.0)
                {
                    gSess.last_disk_check = now;
                    const std::string dir = WolfRecorder::folder();
                    U64 free_now = 0;
                    if (freeBytes(dir, free_now) && free_now < kDiskStopAt)
                    {
                        gLastNotice = "The recording stopped because the disk is nearly full. The video was saved up to "
                                      "that point.";
                        WolfRecorder::stop();
                    }
                    // voice can start (or restart) after the recording did: put the tap back once a second
                    if (gSess.voice) setTaps(true);
                }
            }
        }
        if (gSess.state == WolfRecorder::State::FINISHING && worker_done)
        {
            joinWorker();
            freeGpu();
            gGpu.failed = false;
            {
                std::lock_guard<std::mutex> lk(gS.m);
                if (!gS.error.empty()) gLastError = gS.error;
                if (!gS.notice.empty() && gLastNotice.empty()) gLastNotice = gS.notice;
                if (gS.saved && gLastError.empty() && gLastNotice.empty()) gLastNotice = "Saved.";
                gS.queue.clear();
            }
            gSess.state = WolfRecorder::State::IDLE;
        }
    }

    // [W111 RECORDER UI 2026-11-24] Views kept out of a whole-window video (holdBackFromVideo). Paul: "the recording
    // window should never be recorded". A view registers itself from its draw() and stays registered for the whole
    // recording, drawn after the picture copy every frame - also on frames where the UI itself is not redrawn (the
    // RenderUIBuffer debug setting: llviewerdisplay.cpp render_ui_2d redraws the UI into mUIScreen only when a view
    // is dirty, and lays that buffer down every frame). Handles, not pointers: a window can close meanwhile.
    std::vector<LLHandle<LLView>> gHeldViews;
    bool gDrawingHeld = false;

    // The whole UI is drawn again this frame (LLView::sIsRectDirty / sDirtyRect, screen coordinates = the root view's):
    // with RenderUIBuffer the cached UI must drop a held view when a whole-window recording starts (it was drawn into
    // the buffer before), and take it back when the recording ends. Without RenderUIBuffer the UI is drawn every frame
    // anyway and this changes nothing.
    void dirtyWholeUI()
    {
        if (LLView* root = LLUI::getInstance()->getRootView())
        {
            LLView::sDirtyRect = root->getLocalRect();
            LLView::sIsRectDirty = true;
        }
    }
    void releaseHeldViews()
    {
        if (gHeldViews.empty()) return;
        gHeldViews.clear();
        dirtyWholeUI();
    }

    // Each held view at its place on screen, as LLView::drawChildren would have drawn it (llview.cpp: LLUI::pushMatrix,
    // LLUI::translate to the view's rect) under the root view's UI scale (llviewerwindow.cpp LLViewerWindow::draw:
    // gUIProgram, scaleUI by the display scale). calcScreenRect is in the root view's coordinates, the root view being at
    // 0,0. LLUI's own matrix calls, not gGL's: they also move the text origin (llrender2dutils.cpp LLRender2D::translate
    // moves LLFontGL::sCurOrigin, which LLFontGL::render adds), else the text lands elsewhere than the shapes.
    void drawHeldViews(const std::vector<LLHandle<LLView>>& views)
    {
        if (views.empty()) return;
        gDrawingHeld = true;
        gUIProgram.bind();
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.pushMatrix();
        LLUI::pushMatrix();
        LLUI::loadIdentity();
        gGL.scaleUI(gViewerWindow->getDisplayScale().mV[VX], gViewerWindow->getDisplayScale().mV[VY], 1.f);
        for (const LLHandle<LLView>& h : views)
        {
            LLView* v = h.get();
            if (!v || !v->isInVisibleChain()) continue;   // closed, or hidden: nothing to draw
            const LLRect r = v->calcScreenRect();
            LLUI::pushMatrix();
            LLUI::translate((F32)r.mLeft, (F32)r.mBottom);
            v->draw();
            LLUI::popMatrix();
        }
        LLUI::popMatrix();
        gGL.popMatrix();
        gGL.flush();
        gUIProgram.unbind();
        gDrawingHeld = false;
    }

    bool gIdleAdded = false;
    void ensureIdle()
    {
        if (gIdleAdded) return;
        gIdleAdded = true;
        gIdleCallbacks.addFunction(idle, nullptr);
    }

    std::string timecode(F64 secs)   // [W111 RECORDER UI 2026-11-24] HH:MM:SS, as the window shows it
    {
        const S32 s = (S32)std::max(0.0, secs);
        return llformat("%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    }

    void drawIndicator()
    {
        const F64 secs = (F64)(recNowUs() - gSess.start_us) / 1e6;
        const bool paused = gPaused.load();
        std::string text = (paused ? "PAUSED  " : "REC  ") + timecode(secs);
        const bool near_end = gSess.limit > 0.0 && gSess.limit - secs <= kWarnBeforeEnd;
        if (near_end)
        {
            text += "  - stops at " + timecode(gSess.limit);
        }
        if (gMicLive.load() && !paused) text += "  |  Microphone on";   // paused: the capture is stopped (Mic::setPaused)
        if (gSess.voice && !paused) text += "  |  Voice recorded";
        const LLFontGL* font = LLFontGL::getFontSansSerifBold();
        const S32 W = gViewerWindow->getWindowWidthScaled(), H = gViewerWindow->getWindowHeightScaled();
        const S32 pad = 10, dot = 14;
        const S32 tw = font->getWidth(text), th = (S32)font->getLineHeight();
        const S32 right = W - 16, top = H - 40;
        const S32 left = right - (pad + dot + 8 + tw + pad), bottom = top - (th + 2 * pad);

        gUIProgram.bind();
        gGL.pushMatrix();
        gGL.pushUIMatrix();
        gGL.loadUIIdentity();
        gGL.scaleUI(gViewerWindow->getDisplayScale().mV[VX], gViewerWindow->getDisplayScale().mV[VY], 1.f);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        // dark backing (contrast for the white text over any scene), red dot blinking once a second
        gl_rect_2d(left, top, right, bottom, LLColor4(0.08f, 0.08f, 0.08f, 0.85f), true);
        if (near_end || paused) gl_rect_2d(left, top, right, bottom, LLColor4(1.f, 0.75f, 0.2f, 1.f), false);
        const F32 cx = (F32)(left + pad + dot / 2), cy = (F32)(bottom + (top - bottom) / 2);
        if (paused)
        {
            // two amber bars, steady: the pause sign
            const LLColor4 amber(1.f, 0.75f, 0.2f, 1.f);
            gl_rect_2d(left + pad + 1, (S32)cy + dot / 2, left + pad + 5, (S32)cy - dot / 2, amber, true);
            gl_rect_2d(left + pad + dot - 5, (S32)cy + dot / 2, left + pad + dot - 1, (S32)cy - dot / 2, amber, true);
        }
        else if (((S64)(secs * 2.0)) % 2 == 0)
        {
            gGL.color4f(0.9f, 0.12f, 0.12f, 1.f);
            gl_circle_2d(cx, cy, (F32)dot / 2.f, 16, true);
        }
        font->renderUTF8(text, 0, (F32)(left + pad + dot + 8), (F32)(bottom + pad), LLColor4::white, LLFontGL::LEFT,
                         LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        gGL.popUIMatrix();
        gGL.popMatrix();
        gGL.flush();
        gUIProgram.unbind();
    }
}

namespace WolfRecorder
{
// [W113 2026-11-25] see wolfrecorder.h
void setVoiceMicMuted(bool muted)
{
    gVoiceMicMuted.store(muted, std::memory_order_release);
}

bool worldSoundAvailable()
{
#if LL_OPENAL
    if (LLAudioEngine_OpenAL* al = dynamic_cast<LLAudioEngine_OpenAL*>(gAudiop)) return al->isMixMirrored();
#endif
    return false;
}

bool worldSoundNeedsRestart()
{
#if LL_OPENAL
    return gSavedSettings.getBOOL("WolfRecorderSoundWorld") && gAudiop && !LLAudioEngine_OpenAL::mirrorMixRequested();
#else
    return false;
#endif
}

std::string folder()
{
    std::string f = gSavedSettings.getString("WolfRecorderFolder");
    if (f.empty()) f = videosFolder() + sep() + "WolfViewer";
    return f;
}

void openFolder()
{
    const std::string f = folder();
    std::error_code ec;
    std::filesystem::create_directories(fsyspath(f), ec);
    if (gViewerWindow && gViewerWindow->getWindow()) gViewerWindow->getWindow()->openFile(f);
}

bool isRecording()
{
    return gSess.state == State::STARTING || gSess.state == State::RECORDING;
}

bool start(std::string& error)
{
    ensureIdle();
    if (gSess.state != State::IDLE)
    {
        error = gSess.state == State::FINISHING ? "The last video is still being saved; try again in a moment." : "";
        return false;
    }
    joinWorker();
    gLastError.clear();
    gLastNotice.clear();

    // settings, read once for the whole recording
    const S32 size = gSavedSettings.getS32("WolfRecorderSize");
    gSess.height = size >= 1080 ? 1080 : 720;
    gSess.width = size >= 1080 ? 1920 : 1280;
    gSess.fps = gSavedSettings.getS32("WolfRecorderFps") >= 60 ? 60 : 30;
    // H.264 at these sizes: ~6 Mbit/s for 720p30, ~10 Mbit/s for 1080p30 (YouTube's recommended upload rates for SDR:
    // https://support.google.com/youtube/answer/1722171 - 720p 5 Mbps, 1080p 8 Mbps; a little more for a 1-pass
    // real-time encoder), half as much again at 60 fps
    gSess.kbps = (gSess.height == 1080 ? 10000u : 6000u) * (gSess.fps == 60 ? 3u : 2u) / 2u;
    gSess.ui = gSavedSettings.getBOOL("WolfRecorderShowInterface");
    gSess.world = gSavedSettings.getBOOL("WolfRecorderSoundWorld") && worldSoundAvailable();
    gSess.voice = gSavedSettings.getBOOL("WolfRecorderSoundVoice");
#if LL_OPENAL
    gSess.mic = gSavedSettings.getBOOL("WolfRecorderSoundMic");
#else
    gSess.mic = false;
#endif
    gLevelWorld = llclamp(gSavedSettings.getF32("WolfRecorderLevelWorld"), 0.f, 2.f);
    gLevelVoice = llclamp(gSavedSettings.getF32("WolfRecorderLevelVoice"), 0.f, 2.f);
    gLevelMic = llclamp(gSavedSettings.getF32("WolfRecorderLevelMic"), 0.f, 2.f);
    // [W111 RECORDER UI 2026-11-24] 0 (or less) = "Never" (Paul: "i can imagine me running well over 5 minutes"); the
    // disk-space guard (idle) still stops a recording when the disk is nearly full
    const S32 max_minutes = gSavedSettings.getS32("WolfRecorderMaxMinutes");
    gSess.limit = max_minutes <= 0 ? 0.0 : 60.0 * llclamp(max_minutes, 1, 60);
    const F64 plan = gSess.limit > 0.0 ? gSess.limit : kNeverPlanSecs;   // what must fit on the disk to start

    // the folder, and room on its disk for a whole recording at its limit
    const std::string dir = folder();
    std::error_code ec;
    std::filesystem::create_directories(fsyspath(dir), ec);
    if (!std::filesystem::is_directory(fsyspath(dir), ec))
    {
        error = "The folder for videos could not be made: " + dir + ". Choose another folder.";
        gLastError = error;
        return false;
    }
    const U64 need = (U64)((F64)gSess.kbps * 1000.0 / 8.0 * plan * 1.1) + 192000 / 8 * (U64)plan + kDiskReserve;
    U64 have = 0;
    if (freeBytes(dir, have) && have < need)
    {
        error = gSess.limit > 0.0
            ? "There is not enough free space on the disk for a video of up to " + minutes(gSess.limit) + ": about " +
                  mb(need) + " is needed and " + mb(have) + " is free. Choose another folder or a shorter length limit."
            : "There is not enough free space on the disk to start a video: about " + mb(need) + " is needed and " +
                  mb(have) + " is free. Choose another folder or make some room on this disk.";
        gLastError = error;
        return false;
    }
    const std::string path = newFileName(dir);
    {
        // a quick test that the folder can be written to, so the resident hears about it now, not after Stop.
        // As a std::filesystem::path: MSVC's ofstream(const path&) is a template for that exact type only, so an
        // fsyspath converts to std::string and std::wstring equally and the call is ambiguous (C2668, W111 CI).
        const fsyspath probe_path(path);
        std::ofstream probe(static_cast<const std::filesystem::path&>(probe_path), std::ios::binary);
        if (!probe)
        {
            error = "WolfViewer is not allowed to save files in " + dir + ". Choose another folder.";
            gLastError = error;
            return false;
        }
        probe.close();
        std::filesystem::remove(fsyspath(path), ec);
    }
    WolfVRStream::codecState();   // makes sure a downloaded OpenH264 is known to the encoders (applyCodec)

    {
        std::lock_guard<std::mutex> lk(gS.m);
        gS.queue.clear();
        gS.stop_wanted = false;
        gS.error.clear();
        gS.notice.clear();
        gS.encoder_name.clear();
        gS.file = path;
        gS.bytes = 0;
        gS.saved = false;
        gS.worker_done = false;
        gS.encoder_ready = false;
        gS.written_us.clear();
        gS.length = 0.0;
    }
    resetClock();
    gMicLive = false;
    for (std::atomic<F32>& p : gPeak) p = 0.f;
    gWorldRing.clear();
    gVoiceRing.clear();
    gMicRing.clear();
    gSess.start_us = recNowUs();   // the recording clock (just reset: no paused time)
    gSess.next_due_us = nowUs();
    gSess.dropped = 0;
    gSess.warned = false;
    gSess.captured_frame = 0;
    gSess.last_disk_check = LLFrameTimer::getElapsedSeconds();
    gSess.state = State::STARTING;
    // [W111 RECORDER UI 2026-11-24/] the UI is drawn afresh this frame, before the first copy: a held view drops out of
    // the cached UI buffer (RenderUIBuffer) right away (dirtyWholeUI)
    if (gSess.ui) dirtyWholeUI();
    gSoundOn = gSess.world || gSess.voice || gSess.mic;
    setTaps(true);
    WorkerConfig cfg{ path, gSess.width, gSess.height, gSess.fps, gSess.kbps, gSess.world, gSess.voice, gSess.mic };
    gWorker = std::thread(workerMain, cfg);
    LL_INFOS("WolfRecorder") << "start: " << gSess.width << "x" << gSess.height << " " << gSess.fps << " fps, interface "
                             << gSess.ui << ", limit " << gSess.limit << " s" << LL_ENDL;
    return true;
}

void stop()
{
    if (!isRecording()) return;
    gSess.state = State::FINISHING;
    setTaps(false);
    setClockPaused(false);   // [W111 RECORDER UI 2026-11-24/] the clock runs again: the worker waits on it for the end
    releaseHeldViews();      // [W111 RECORDER UI 2026-11-24/] held views are drawn as usual again (and the UI buffer redrawn)
    // the pictures still in their pixel buffers: collect whatever has finished (the rest is a frame or two)
    collect();
    {
        std::lock_guard<std::mutex> lk(gS.m);
        gS.stop_wanted = true;
    }
    gS.cv.notify_one();
}

Report status()
{
    Report st;
    st.state = gSess.state;
    st.limit_seconds = gSess.limit;
    st.dropped = gSess.dropped;
    st.world = gSess.world;
    st.voice = gSess.voice;
    st.mic = gMicLive.load();   // [W111 RECORDER UI 2026-11-24/] really open, not only asked for
    if (isRecording()) st.seconds = (F64)(recNowUs() - gSess.start_us) / 1e6;
    st.paused = isRecording() && gPaused.load();
    std::lock_guard<std::mutex> lk(gS.m);
    st.length = gS.length;
    st.file = gS.file;
    st.encoder = gS.encoder_name;
    st.bytes = gS.bytes;
    st.saved = gS.saved && gSess.state == State::IDLE;
    const S64 now = nowUs();
    size_t n = 0;
    for (S64 t : gS.written_us) if (t >= now - 2000000) ++n;
    st.fps = (F32)n / 2.f;
    st.error = gSess.state == State::IDLE ? gLastError : gS.error;
    st.notice = gSess.state == State::IDLE ? gLastNotice : gS.notice;
    return st;
}

void captureWorld(bool name_tags_drawn)
{
    if (gSess.ui || !pictureDue()) return;
    capture(0, name_tags_drawn);   // the back buffer right after renderFinalize: the finished world, before any HUD or UI
}

void captureWholeWindowAndDrawIndicator()
{
    if (!isRecording() || gSnapshot || gCubeSnapshot || !gViewerWindow) return;
    if (gSess.ui && pictureDue())
    {
        // OpenGL: the back buffer as it is now. Vulkan: wolfvrscreen.cpp composeVkBigScreen builds the composite from
        // the back buffer as it is now (and Big Screen reuses that one in this frame). Either way, before the held views.
        capture(WolfVRScreen::wholeWindowFramebuffer(), true);   // [RECORDING PRIVACY 2026-11-25/] the tags are in it
    }
    // [W111 RECORDER UI 2026-11-24/] the recorder's own window, after the copy: never in the video
    gHeldViews.erase(std::remove_if(gHeldViews.begin(), gHeldViews.end(),
                                    [](const LLHandle<LLView>& h) { return h.get() == nullptr; }),
                     gHeldViews.end());
    drawHeldViews(gHeldViews);
    drawIndicator();
}

void pause()
{
    if (gSess.state != State::RECORDING || gPaused.load()) return;
    setClockPaused(true);
    LL_INFOS("WolfRecorder") << "paused at " << (F64)(recNowUs() - gSess.start_us) / 1e6 << " s" << LL_ENDL;
}

void resume()
{
    if (gSess.state != State::RECORDING || !gPaused.load()) return;
    setClockPaused(false);
    gSess.next_due_us = nowUs();   // a picture straight away
    LL_INFOS("WolfRecorder") << "resumed" << LL_ENDL;
}

bool isPaused()
{
    return isRecording() && gPaused.load();
}

void levels(F32 out[SOURCE_COUNT])
{
    const bool on[SOURCE_COUNT] = { gSess.world, gSess.voice, gSess.mic };
    for (int i = 0; i < SOURCE_COUNT; ++i)
    {
        const F32 v = gPeak[i].exchange(0.f);
        out[i] = (gSess.state == State::RECORDING && on[i]) ? v : 0.f;
    }
}

bool previewPicture(U32& serial, std::vector<U8>& rgb)
{
    std::lock_guard<std::mutex> lk(gS.m);
    if (gS.preview_serial == serial || gS.preview.empty()) return false;
    serial = gS.preview_serial;
    rgb = gS.preview;
    return true;
}

bool holdBackFromVideo(LLView* view)
{
    // the same test captureWholeWindowAndDrawIndicator makes before it draws the held views (a snapshot pass draws the
    // window as usual: it is not a video picture)
    if (gDrawingHeld || !view || !isRecording() || !gSess.ui || gSnapshot || gCubeSnapshot || !gViewerWindow)
    {
        return false;
    }
    const LLHandle<LLView> h = view->getHandle();
    if (std::find(gHeldViews.begin(), gHeldViews.end(), h) == gHeldViews.end()) gHeldViews.push_back(h);
    return true;
}

bool wantsWholeWindow()
{
    return isRecording() && gSess.ui;
}

void shutdown()
{
    if (isRecording()) stop();
    setTaps(false);
    {
        std::lock_guard<std::mutex> lk(gS.m);
        gS.stop_wanted = true;
    }
    gS.cv.notify_one();
    joinWorker();
    gSoundOn = false;
    resetClock();
    gHeldViews.clear();
    gSess.state = State::IDLE;
}
}
