/**
 * @file wolfdjcapture_linux.cpp
 * @brief WolfViewer Wolf DJ capture on Linux: the PulseAudio client API (served by PulseAudio
 *        itself or by PipeWire's pipewire-pulse), loaded at run time so the viewer still starts
 *        on a system without libpulse.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfdjcapture.h"
#include "wolfdjaudio.h"

#include <pulse/pulseaudio.h>
#include <pulse/thread-mainloop.h>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>

// [WOLF DJ 2026-10-04] Same approach as media_plugins/cef/linux_volume_catcher.cpp (libpulse
// loaded at run time, symbols looked up by name) - but every prototype here is taken from the
// real header with decltype, so none is retyped by hand.
//   One program:        record the monitor of the sink it plays to, narrowed to its sink input
//                       (pulse/stream.h pa_stream_set_monitor_stream, "monitor only a very
//                       specific sink input of the sink ... before pa_stream_connect_record").
//   Everything you hear: "@DEFAULT_MONITOR@", the default sink's monitor.
//   Microphone:         the default source (device NULL).

namespace
{
#define WDJ_PA_SYMS(X) \
    X(pa_threaded_mainloop_new) X(pa_threaded_mainloop_start) X(pa_threaded_mainloop_lock) \
    X(pa_threaded_mainloop_unlock) X(pa_threaded_mainloop_wait) X(pa_threaded_mainloop_signal) \
    X(pa_threaded_mainloop_get_api) \
    X(pa_context_new) X(pa_context_connect) X(pa_context_get_state) X(pa_context_set_state_callback) \
    X(pa_context_get_sink_input_info_list) X(pa_context_get_sink_info_by_index) \
    X(pa_operation_get_state) X(pa_operation_unref) \
    X(pa_stream_new) X(pa_stream_set_read_callback) X(pa_stream_set_state_callback) \
    X(pa_stream_set_monitor_stream) X(pa_stream_connect_record) X(pa_stream_peek) X(pa_stream_drop) \
    X(pa_stream_disconnect) X(pa_stream_unref) X(pa_stream_get_state) \
    X(pa_proplist_gets)

#define WDJ_PA_DECL(name) decltype(&::name) p_##name = nullptr;
    WDJ_PA_SYMS(WDJ_PA_DECL)
#undef WDJ_PA_DECL

    struct Pulse
    {
        std::mutex mInitMutex;
        bool mTried = false;
        bool mOk = false;
        std::string mWhy;
        pa_threaded_mainloop* mLoop = nullptr;
        pa_context* mCtx = nullptr;

        static void onContextState(pa_context*, void* ud)
        {
            Pulse* self = static_cast<Pulse*>(ud);
            p_pa_threaded_mainloop_signal(self->mLoop, 0);
        }

        // One connection for the life of the viewer (as the volume catcher keeps its own).
        bool init()
        {
            std::lock_guard<std::mutex> lock(mInitMutex);
            if (mTried) return mOk;
            mTried = true;
            void* dso = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!dso)
            {
                mWhy = "PulseAudio / PipeWire (libpulse.so.0) is not available on this system.";
                return false;
            }
#define WDJ_PA_LOAD(name) p_##name = reinterpret_cast<decltype(&::name)>(dlsym(dso, #name)); \
            if (!p_##name) { mWhy = std::string("libpulse is missing ") + #name + "."; return false; }
            WDJ_PA_SYMS(WDJ_PA_LOAD)
#undef WDJ_PA_LOAD
            mLoop = p_pa_threaded_mainloop_new();
            if (!mLoop)
            {
                mWhy = "Could not start the PulseAudio client.";
                return false;
            }
            mCtx = p_pa_context_new(p_pa_threaded_mainloop_get_api(mLoop), "WolfViewer Wolf DJ");
            p_pa_context_set_state_callback(mCtx, &Pulse::onContextState, this);
            p_pa_threaded_mainloop_lock(mLoop);
            if (p_pa_threaded_mainloop_start(mLoop) < 0 || p_pa_context_connect(mCtx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
            {
                p_pa_threaded_mainloop_unlock(mLoop);
                mWhy = "Could not connect to the sound server.";
                return false;
            }
            while (true)
            {
                const pa_context_state_t st = p_pa_context_get_state(mCtx);
                if (st == PA_CONTEXT_READY) break;
                if (!PA_CONTEXT_IS_GOOD(st))
                {
                    p_pa_threaded_mainloop_unlock(mLoop);
                    mWhy = "The sound server refused the connection.";
                    return false;
                }
                p_pa_threaded_mainloop_wait(mLoop);
            }
            p_pa_threaded_mainloop_unlock(mLoop);
            mOk = true;
            return true;
        }

        // Mainloop lock held. Waits for an operation (its callback signals the loop).
        void wait(pa_operation* op)
        {
            if (!op) return;
            while (p_pa_operation_get_state(op) == PA_OPERATION_RUNNING)
            {
                p_pa_threaded_mainloop_wait(mLoop);
            }
            p_pa_operation_unref(op);
        }
    };

    Pulse& pulse()
    {
        static Pulse sPulse;
        return sPulse;
    }

    struct SinkInputList
    {
        std::vector<WolfDJApp>* mApps;
        pid_t mSelf;
    };

    void onSinkInput(pa_context*, const pa_sink_input_info* info, int eol, void* ud)
    {
        if (eol)
        {
            p_pa_threaded_mainloop_signal(pulse().mLoop, 0);
            return;
        }
        SinkInputList* list = static_cast<SinkInputList*>(ud);
        const char* pid = p_pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID);
        if (pid && atoi(pid) == (int)list->mSelf) return;   // the viewer itself (its voice, its sounds)
        const char* app = p_pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
        const char* media = p_pa_proplist_gets(info->proplist, PA_PROP_MEDIA_NAME);
        std::string name = app && *app ? app : (info->name ? info->name : "Program");
        if (media && *media && name != media)
        {
            std::string m = media;
            if (m.size() > 48) m = m.substr(0, 45) + "...";
            name += " - " + m;
        }
        list->mApps->push_back({ "si:" + std::to_string(info->index) + ":" + std::to_string(info->sink), name });
    }

    struct SinkMonitor
    {
        std::string mName;
    };

    void onSinkInfo(pa_context*, const pa_sink_info* info, int eol, void* ud)
    {
        if (eol)
        {
            p_pa_threaded_mainloop_signal(pulse().mLoop, 0);
            return;
        }
        if (info && info->monitor_source_name)
        {
            static_cast<SinkMonitor*>(ud)->mName = info->monitor_source_name;
        }
    }

    class PulseStream : public WolfDJCaptureStream
    {
    public:
        PulseStream(WolfDJChannel* ch) : mChannel(ch) {}

        ~PulseStream() override
        {
            Pulse& pa = pulse();
            p_pa_threaded_mainloop_lock(pa.mLoop);
            if (mStream)
            {
                p_pa_stream_set_read_callback(mStream, NULL, NULL);
                p_pa_stream_set_state_callback(mStream, NULL, NULL);
                p_pa_stream_disconnect(mStream);
                p_pa_stream_unref(mStream);
                mStream = nullptr;
            }
            p_pa_threaded_mainloop_unlock(pa.mLoop);
            mChannel->clear();
        }

        bool ok() const override { return !mFailed; }
        std::string error() const override { return mFailed ? "The sound source stopped." : std::string(); }

        // monitor_idx = PA_INVALID_INDEX for a plain source.
        bool start(const char* device, uint32_t monitor_idx, std::string& err)
        {
            Pulse& pa = pulse();
            p_pa_threaded_mainloop_lock(pa.mLoop);
            pa_sample_spec spec;
            spec.format = PA_SAMPLE_FLOAT32LE;
            spec.rate = WolfDJ::RATE;
            spec.channels = 2;
            mStream = p_pa_stream_new(pa.mCtx, "Wolf DJ capture", &spec, NULL);
            if (!mStream)
            {
                p_pa_threaded_mainloop_unlock(pa.mLoop);
                err = "Could not create a capture stream.";
                return false;
            }
            p_pa_stream_set_read_callback(mStream, &PulseStream::onRead, this);
            p_pa_stream_set_state_callback(mStream, &PulseStream::onState, this);
            if (monitor_idx != PA_INVALID_INDEX && p_pa_stream_set_monitor_stream(mStream, monitor_idx) < 0)
            {
                p_pa_threaded_mainloop_unlock(pa.mLoop);
                err = "That program can no longer be captured.";
                return false;
            }
            pa_buffer_attr attr;
            attr.maxlength = (uint32_t)-1;
            attr.tlength = (uint32_t)-1;
            attr.prebuf = (uint32_t)-1;
            attr.minreq = (uint32_t)-1;
            attr.fragsize = WolfDJ::RATE / 50 * 2 * sizeof(float);   // 20 ms
            const int r = p_pa_stream_connect_record(mStream, device, &attr, PA_STREAM_ADJUST_LATENCY);
            p_pa_threaded_mainloop_unlock(pa.mLoop);
            if (r < 0)
            {
                err = "Could not start capturing.";
                return false;
            }
            return true;
        }

    private:
        static void onRead(pa_stream* s, size_t, void* ud)
        {
            PulseStream* self = static_cast<PulseStream*>(ud);
            const void* data = nullptr;
            size_t bytes = 0;
            while (p_pa_stream_peek(s, &data, &bytes) == 0 && bytes > 0)
            {
                if (data)   // NULL with bytes > 0 is a hole: skip it
                {
                    self->mChannel->pushFloat(static_cast<const float*>(data), bytes / (2 * sizeof(float)), 2, WolfDJ::RATE);
                }
                p_pa_stream_drop(s);
            }
        }

        static void onState(pa_stream* s, void* ud)
        {
            const pa_stream_state_t st = p_pa_stream_get_state(s);
            if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED)
            {
                static_cast<PulseStream*>(ud)->mFailed = true;
            }
        }

        WolfDJChannel* mChannel;
        pa_stream* mStream = nullptr;
        std::atomic<bool> mFailed{ false };
    };
}

bool WolfDJCapture::listApps(std::vector<WolfDJApp>& apps, std::string& why)
{
    apps.clear();
    Pulse& pa = pulse();
    if (!pa.init())
    {
        why = pa.mWhy;
        return false;
    }
    apps.push_back({ WOLFDJ_EVERYTHING_ID, "Everything you hear" });
    SinkInputList list{ &apps, getpid() };
    p_pa_threaded_mainloop_lock(pa.mLoop);
    pa.wait(p_pa_context_get_sink_input_info_list(pa.mCtx, &onSinkInput, &list));
    p_pa_threaded_mainloop_unlock(pa.mLoop);
    return true;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openMic(WolfDJChannel* ch, std::string& err)
{
    Pulse& pa = pulse();
    if (!pa.init())
    {
        err = pa.mWhy;
        return nullptr;
    }
    auto s = std::make_unique<PulseStream>(ch);
    if (!s->start(NULL, PA_INVALID_INDEX, err)) return nullptr;
    return s;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openApp(const std::string& id, WolfDJChannel* ch, std::string& err)
{
    Pulse& pa = pulse();
    if (!pa.init())
    {
        err = pa.mWhy;
        return nullptr;
    }
    auto s = std::make_unique<PulseStream>(ch);
    if (id == WOLFDJ_EVERYTHING_ID)
    {
        if (!s->start("@DEFAULT_MONITOR@", PA_INVALID_INDEX, err)) return nullptr;
        return s;
    }
    // "si:<sink input>:<sink>"
    unsigned si = 0, sink = 0;
    if (sscanf(id.c_str(), "si:%u:%u", &si, &sink) != 2)
    {
        err = "Unknown program.";
        return nullptr;
    }
    SinkMonitor mon;
    p_pa_threaded_mainloop_lock(pa.mLoop);
    pa.wait(p_pa_context_get_sink_info_by_index(pa.mCtx, sink, &onSinkInfo, &mon));
    p_pa_threaded_mainloop_unlock(pa.mLoop);
    if (mon.mName.empty())
    {
        err = "That program's sound output has gone.";
        return nullptr;
    }
    if (!s->start(mon.mName.c_str(), si, err)) return nullptr;
    return s;
}
