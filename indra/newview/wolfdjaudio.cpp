/**
 * @file wolfdjaudio.cpp
 * @brief WolfViewer Wolf DJ mixer engine: channels, EQ, talk-over, limiter, cue, Ogg Vorbis
 *        encoder and the Icecast source client. See wolfdjaudio.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfdjaudio.h"
#include "wolfdjcapture.h"

#include "llbase64.h"
#include "llcallbacklist.h"
#include "llrand.h"
#include "llwebrtc.h"

#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"

#if LL_OPENAL
#include "AL/al.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <ws2tcpip.h>
typedef SOCKET wdj_socket_t;
static const wdj_socket_t WDJ_BAD_SOCKET = INVALID_SOCKET;
static void wdj_close(wdj_socket_t s) { closesocket(s); }
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int wdj_socket_t;
static const wdj_socket_t WDJ_BAD_SOCKET = -1;
static void wdj_close(wdj_socket_t s) { ::close(s); }
#endif

using namespace WolfDJ;

static constexpr float PI_F = 3.14159265358979f;

// ---------------------------------------------------------------------------------------------
// EQ - Source: Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
// coefficients" (lowShelf/highShelf with shelf slope S = 1, peakingEQ), normalised by a0.
// ---------------------------------------------------------------------------------------------

void WolfDJBiquad::setLowShelf(float f0, float db)
{
    const float A = powf(10.f, db / 40.f);
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / 2.f * sqrtf(2.f);   // S = 1: sqrt((A + 1/A)(1/S - 1) + 2) = sqrt(2)
    const float sa = 2.f * sqrtf(A) * alpha;
    const float a0 = (A + 1.f) + (A - 1.f) * cw + sa;
    b0 = A * ((A + 1.f) - (A - 1.f) * cw + sa) / a0;
    b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cw) / a0;
    b2 = A * ((A + 1.f) - (A - 1.f) * cw - sa) / a0;
    a1 = -2.f * ((A - 1.f) + (A + 1.f) * cw) / a0;
    a2 = ((A + 1.f) + (A - 1.f) * cw - sa) / a0;
}

void WolfDJBiquad::setPeaking(float f0, float q, float db)
{
    const float A = powf(10.f, db / 40.f);
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.f * q);
    const float a0 = 1.f + alpha / A;
    b0 = (1.f + alpha * A) / a0;
    b1 = -2.f * cw / a0;
    b2 = (1.f - alpha * A) / a0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha / A) / a0;
}

void WolfDJBiquad::setHighShelf(float f0, float db)
{
    const float A = powf(10.f, db / 40.f);
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / 2.f * sqrtf(2.f);
    const float sa = 2.f * sqrtf(A) * alpha;
    const float a0 = (A + 1.f) - (A - 1.f) * cw + sa;
    b0 = A * ((A + 1.f) + (A - 1.f) * cw + sa) / a0;
    b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cw) / a0;
    b2 = A * ((A + 1.f) + (A - 1.f) * cw - sa) / a0;
    a1 = 2.f * ((A - 1.f) - (A + 1.f) * cw) / a0;
    a2 = ((A + 1.f) - (A - 1.f) * cw - sa) / a0;
}

float wolfdj_fader_gain(float pos)
{
    pos = llclamp(pos, 0.f, 1.f);
    return pos * pos * 1.4125f;     // 1.4125 = +3 dB at the top; 0.84 = 0 dB
}

float wolfdj_lin_to_db(float lin)
{
    return lin > 1e-6f ? 20.f * log10f(lin) : -120.f;
}

// ---------------------------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------------------------

static constexpr size_t RING_FRAMES = RATE * 2;             // 2 s
static constexpr size_t MAX_LAG_FRAMES = RATE / 4;          // more than 250 ms queued...
static constexpr size_t TRIM_TO_FRAMES = RATE * 8 / 100;    // ...is trimmed back to 80 ms

WolfDJChannel::WolfDJChannel()
{
    mRing.assign(RING_FRAMES * 2, 0.f);
    mLow.setLowShelf(100.f, 0.f);
    mMid.setPeaking(1000.f, 0.7f, 0.f);
    mHigh.setHighShelf(8000.f, 0.f);
}

void WolfDJChannel::pushS16(const int16_t* samples, size_t frames, size_t channels, unsigned rate)
{
    if (!samples || frames == 0 || channels == 0) return;
    std::lock_guard<std::mutex> lock(mMutex);
    mScratch.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const float l = samples[i * channels] / 32768.f;
        const float r = channels > 1 ? samples[i * channels + 1] / 32768.f : l;
        mScratch[i * 2] = l;
        mScratch[i * 2 + 1] = r;
    }
    pushStereo(mScratch.data(), frames, rate);
}

void WolfDJChannel::pushFloat(const float* samples, size_t frames, size_t channels, unsigned rate)
{
    if (!samples || frames == 0 || channels == 0) return;
    std::lock_guard<std::mutex> lock(mMutex);
    mScratch.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const float l = samples[i * channels];
        const float r = channels > 1 ? samples[i * channels + 1] : l;
        mScratch[i * 2] = l;
        mScratch[i * 2 + 1] = r;
    }
    pushStereo(mScratch.data(), frames, rate);
}

// mMutex held. Linear-interpolation resampler to RATE, then into the ring (oldest dropped when full).
void WolfDJChannel::pushStereo(const float* in, size_t frames, unsigned rate)
{
    mActive = true;
    if (rate == 0) return;
    const float* src = in;
    size_t n = frames;
    if (rate != RATE)
    {
        if (rate != mInRate)
        {
            mInRate = rate;
            mPos = 1.0;
        }
        // Position p: 0 = the previous block's last frame, k = in[k - 1].
        const double step = (double)rate / (double)RATE;
        mResampled.clear();
        while (true)
        {
            const size_t idx = (size_t)mPos;
            if (idx >= frames) break;
            const float t = (float)(mPos - (double)idx);
            const float l0 = idx == 0 ? mPrevL : in[(idx - 1) * 2];
            const float r0 = idx == 0 ? mPrevR : in[(idx - 1) * 2 + 1];
            const float l1 = in[idx * 2], r1 = in[idx * 2 + 1];
            mResampled.push_back(l0 + (l1 - l0) * t);
            mResampled.push_back(r0 + (r1 - r0) * t);
            mPos += step;
        }
        mPos -= (double)frames;
        mPrevL = in[(frames - 1) * 2];
        mPrevR = in[(frames - 1) * 2 + 1];
        src = mResampled.data();
        n = mResampled.size() / 2;
    }
    else
    {
        mInRate = rate;
    }
    for (size_t i = 0; i < n; ++i)
    {
        size_t at;
        if (mCount == RING_FRAMES)
        {
            at = mHead;                             // overwrite the oldest
            mHead = (mHead + 1) % RING_FRAMES;
        }
        else
        {
            at = (mHead + mCount) % RING_FRAMES;
            ++mCount;
        }
        mRing[at * 2] = src[i * 2];
        mRing[at * 2 + 1] = src[i * 2 + 1];
    }
}

size_t WolfDJChannel::pull(float* out, size_t frames)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mCount > MAX_LAG_FRAMES)
    {
        const size_t drop = mCount - TRIM_TO_FRAMES;
        mHead = (mHead + drop) % RING_FRAMES;
        mCount -= drop;
    }
    const size_t n = std::min(frames, mCount);
    for (size_t i = 0; i < n; ++i)
    {
        const size_t at = (mHead + i) % RING_FRAMES;
        out[i * 2] = mRing[at * 2];
        out[i * 2 + 1] = mRing[at * 2 + 1];
    }
    mHead = (mHead + n) % RING_FRAMES;
    mCount -= n;
    std::fill(out + n * 2, out + frames * 2, 0.f);
    return n;
}

void WolfDJChannel::clear()
{
    std::lock_guard<std::mutex> lock(mMutex);
    mHead = mCount = 0;
    mPos = 1.0;
    mPrevL = mPrevR = 0.f;
    mActive = false;
}

// ---------------------------------------------------------------------------------------------
// Ogg Vorbis encoder. Source: llaudio/llvorbisencode.cpp encode_vorbis_file (the libvorbis
// encoder_example sequence): vorbis_encode_init_vbr, headerout + flush, analysis_buffer/wrote,
// blockout -> analysis -> bitrate_addblock -> flushpacket -> packetin -> pageout.
// ---------------------------------------------------------------------------------------------

class WolfDJEncoder
{
public:
    ~WolfDJEncoder() { clear(); }

    bool begin(const std::string& artist, const std::string& title, std::string& out)
    {
        clear();
        vorbis_info_init(&mVi);
        // 0.4 = about 128 kbit/s stereo at 48 kHz (oggenc -q4).
        if (vorbis_encode_init_vbr(&mVi, 2, RATE, 0.4f))
        {
            vorbis_info_clear(&mVi);
            return false;
        }
        vorbis_comment_init(&mVc);
        vorbis_comment_add_tag(&mVc, "ENCODER", "WolfViewer Wolf DJ");
        if (!artist.empty()) vorbis_comment_add_tag(&mVc, "ARTIST", artist.c_str());
        if (!title.empty()) vorbis_comment_add_tag(&mVc, "TITLE", title.c_str());
        vorbis_analysis_init(&mVd, &mVi);
        vorbis_block_init(&mVd, &mVb);
        ogg_stream_init(&mOs, (int)ll_rand());
        ogg_packet header, header_comm, header_code;
        vorbis_analysis_headerout(&mVd, &mVc, &header, &header_comm, &header_code);
        ogg_stream_packetin(&mOs, &header);
        ogg_stream_packetin(&mOs, &header_comm);
        ogg_stream_packetin(&mOs, &header_code);
        ogg_page og;
        while (ogg_stream_flush(&mOs, &og) != 0)
        {
            out.append((const char*)og.header, og.header_len);
            out.append((const char*)og.body, og.body_len);
        }
        mActive = true;
        return true;
    }

    void encode(const float* stereo, size_t frames, std::string& out)
    {
        if (!mActive || frames == 0) return;
        float** buffer = vorbis_analysis_buffer(&mVd, (int)frames);
        for (size_t i = 0; i < frames; ++i)
        {
            buffer[0][i] = stereo[i * 2];
            buffer[1][i] = stereo[i * 2 + 1];
        }
        vorbis_analysis_wrote(&mVd, (int)frames);
        drain(out);
    }

    // End this logical stream (EOS page) so a new one can follow (chained Ogg).
    void end(std::string& out)
    {
        if (!mActive) return;
        vorbis_analysis_wrote(&mVd, 0);
        drain(out);
        clear();
    }

    bool active() const { return mActive; }

private:
    void drain(std::string& out)
    {
        ogg_packet op;
        ogg_page og;
        while (vorbis_analysis_blockout(&mVd, &mVb) == 1)
        {
            vorbis_analysis(&mVb, NULL);
            vorbis_bitrate_addblock(&mVb);
            while (vorbis_bitrate_flushpacket(&mVd, &op))
            {
                ogg_stream_packetin(&mOs, &op);
                while (ogg_stream_pageout(&mOs, &og) != 0)
                {
                    out.append((const char*)og.header, og.header_len);
                    out.append((const char*)og.body, og.body_len);
                }
            }
        }
    }

    void clear()
    {
        if (!mActive) return;
        ogg_stream_clear(&mOs);
        vorbis_block_clear(&mVb);
        vorbis_dsp_clear(&mVd);
        vorbis_comment_clear(&mVc);
        vorbis_info_clear(&mVi);
        mActive = false;
    }

    bool mActive = false;
    ogg_stream_state mOs;
    vorbis_info mVi;
    vorbis_comment mVc;
    vorbis_dsp_state mVd;
    vorbis_block mVb;
};

// ---------------------------------------------------------------------------------------------
// Icecast source client
// ---------------------------------------------------------------------------------------------

namespace
{
    std::string header_safe(std::string s)
    {
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '\r' || c == '\n'; }), s.end());
        return s;
    }

#if LL_WINDOWS
    struct WinsockInit
    {
        WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
        ~WinsockInit() { WSACleanup(); }
    };
#endif

    void set_blocking(wdj_socket_t s, bool blocking)
    {
#if LL_WINDOWS
        u_long nb = blocking ? 0 : 1;
        ioctlsocket(s, FIONBIO, &nb);
#else
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
    }

    // Waits for the socket to be readable (want_write false) or writable. False on timeout/error.
    bool wait_socket(wdj_socket_t s, bool want_write, int timeout_ms)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int r = select((int)s + 1, want_write ? NULL : &fds, want_write ? &fds : NULL, NULL, &tv);
        return r > 0;
    }

    bool send_all(wdj_socket_t s, const char* data, size_t len)
    {
        while (len > 0)
        {
            if (!wait_socket(s, true, 15000)) return false;
#if LL_WINDOWS
            const int n = ::send(s, data, (int)std::min(len, (size_t)65536), 0);
#elif LL_LINUX
            const ssize_t n = ::send(s, data, len, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(s, data, len, 0);
#endif
            if (n <= 0) return false;
            data += n;
            len -= (size_t)n;
        }
        return true;
    }
}

class WolfDJSender
{
public:
    explicit WolfDJSender(const WolfDJMixer::StreamConfig& cfg) : mCfg(cfg)
    {
        mThread = std::thread([this]() { run(); });
    }

    ~WolfDJSender()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStop = true;
        }
        mCond.notify_all();
        if (mThread.joinable()) mThread.join();
    }

    void push(std::string&& data, bool bos)
    {
        if (data.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQueued += data.size();
            mQueue.push_back({ std::move(data), bos });
        }
        mCond.notify_one();
    }

    // After every (re)connection the mixer must start a fresh Ogg stream: Icecast needs the
    // Vorbis headers at the start of each source connection.
    bool takeRestartRequest() { return mWantRestart.exchange(false); }

    EState state() const { return (EState)mState.load(); }
    std::string message() const
    {
        std::lock_guard<std::mutex> lock(mMsgMutex);
        return mMessage;
    }
    double onAirSeconds() const
    {
        if (state() != ON_AIR) return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mOnAirSince).count();
    }

    // Lets the last pages (the EOS) go out before the connection is closed.
    void drain(int max_ms)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
        while (std::chrono::steady_clock::now() < until && state() == ON_AIR)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mQueue.empty()) return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

private:
    struct Chunk
    {
        std::string mData;
        bool mBos;
    };

    void setState(EState s, const std::string& msg)
    {
        {
            std::lock_guard<std::mutex> lock(mMsgMutex);
            mMessage = msg;
        }
        mState = (int)s;
    }

    // Interruptible sleep. False when stopping.
    bool pause(int ms)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        return !mCond.wait_for(lock, std::chrono::milliseconds(ms), [this]() { return mStop; });
    }

    // Connects and sends the SOURCE request. On failure sets err; fatal = do not retry.
    wdj_socket_t connectSource(std::string& err, bool& fatal)
    {
        fatal = false;
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = NULL;
        const std::string port = std::to_string(mCfg.mPort);
        if (getaddrinfo(mCfg.mHost.c_str(), port.c_str(), &hints, &res) != 0 || !res)
        {
            err = "Cannot find the stream server " + mCfg.mHost + ".";
            return WDJ_BAD_SOCKET;
        }
        wdj_socket_t s = WDJ_BAD_SOCKET;
        for (addrinfo* ai = res; ai; ai = ai->ai_next)
        {
            s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == WDJ_BAD_SOCKET) continue;
#if LL_DARWIN
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            set_blocking(s, false);
            ::connect(s, ai->ai_addr, (int)ai->ai_addrlen);
            bool ok = wait_socket(s, true, 10000);
            if (ok)
            {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                ok = (soerr == 0);
            }
            if (ok) break;
            wdj_close(s);
            s = WDJ_BAD_SOCKET;
        }
        freeaddrinfo(res);
        if (s == WDJ_BAD_SOCKET)
        {
            err = "Cannot connect to the stream server " + mCfg.mHost + ":" + port + ".";
            return WDJ_BAD_SOCKET;
        }
        set_blocking(s, true);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

        // Source: icecast-2.4.4 connection.c _handle_connection (SOURCE or PUT -> source request),
        // client_check_source_auth (Basic auth; on /radio/* the URL auth asks GridManager
        // streamauth.php, which checks only the password - "the mount already says whose stream it is").
        const std::string cred = "source:" + mCfg.mPassword;
        std::string req = "SOURCE " + header_safe(mCfg.mMount) + " HTTP/1.0\r\n";
        req += "Authorization: Basic " + LLBase64::encode((const U8*)cred.data(), cred.size()) + "\r\n";
        req += "User-Agent: WolfViewer-WolfDJ/1.0\r\n";
        req += "Content-Type: application/ogg\r\n";
        req += "Ice-Name: " + header_safe(mCfg.mStationName) + "\r\n";
        req += "Ice-Description: " + header_safe(mCfg.mDescription) + "\r\n";
        req += "Ice-Public: 0\r\n";
        req += "\r\n";
        if (!send_all(s, req.data(), req.size()))
        {
            wdj_close(s);
            err = "The stream server closed the connection.";
            return WDJ_BAD_SOCKET;
        }

        // Read the answer: icecast-2.4.4 source_startup sends "HTTP/1.0 200 OK\r\n\r\n".
        std::string resp;
        char buf[1024];
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (resp.find("\r\n\r\n") == std::string::npos && resp.size() < 8192)
        {
            if (std::chrono::steady_clock::now() > until || !wait_socket(s, false, 1000))
            {
                if (std::chrono::steady_clock::now() > until) break;
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (mStop) break;
                }
                continue;
            }
            const int n = (int)::recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, n);
        }
        int code = 0;
        const size_t sp = resp.find(' ');
        if (resp.compare(0, 5, "HTTP/") == 0 && sp != std::string::npos)
        {
            code = atoi(resp.c_str() + sp + 1);
        }
        if (code == 200)
        {
            return s;
        }
        wdj_close(s);
        const size_t eol = resp.find("\r\n");
        const std::string status = resp.substr(0, eol == std::string::npos ? resp.size() : eol);
        if (code == 401)
        {
            fatal = true;
            err = "The stream password was not accepted for " + mCfg.mMount + ". Check it on My Radio Stream (you need to own a region).";
        }
        else if (code == 403)
        {
            fatal = true;
            const size_t body = resp.find("\r\n\r\n");
            std::string why = body == std::string::npos ? status : resp.substr(body + 4);
            if (why.size() > 200) why.resize(200);
            err = "The stream server refused " + mCfg.mMount + ": " + why;
        }
        else
        {
            err = status.empty() ? std::string("No answer from the stream server.") : "The stream server answered: " + status;
        }
        return WDJ_BAD_SOCKET;
    }

    void run()
    {
#if LL_WINDOWS
        WinsockInit winsock;
#endif
        int backoff_ms = 2000;
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStop) break;
            }
            setState(CONNECTING, "Connecting to " + mCfg.mHost + "...");
            std::string err;
            bool fatal = false;
            wdj_socket_t s = connectSource(err, fatal);
            if (s == WDJ_BAD_SOCKET)
            {
                if (fatal)
                {
                    setState(FAILED, err);
                    while (pause(1000)) {}
                    break;
                }
                setState(RETRYING, err + " Trying again...");
                if (!pause(backoff_ms)) break;
                backoff_ms = std::min(backoff_ms * 2, 30000);
                continue;
            }
            backoff_ms = 2000;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mQueue.clear();
                mQueued = 0;
            }
            bool await_bos = true;
            mWantRestart = true;
            mOnAirSince = std::chrono::steady_clock::now();
            setState(ON_AIR, "");
            bool broken = false;
            while (!broken)
            {
                Chunk c;
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCond.wait_for(lock, std::chrono::seconds(1), [this]() { return mStop || !mQueue.empty(); });
                    if (mStop) break;
                    if (mQueue.empty()) continue;
                    if (mQueued > 4 * 1024 * 1024)
                    {
                        // The upload cannot keep up (about 4 minutes behind): start over.
                        mQueue.clear();
                        mQueued = 0;
                        broken = true;
                        err = "Your upload could not keep up with the stream.";
                        break;
                    }
                    c = std::move(mQueue.front());
                    mQueue.pop_front();
                    mQueued -= c.mData.size();
                }
                if (await_bos && !c.mBos) continue;     // pages of the stream before this connection
                await_bos = false;
                if (!send_all(s, c.mData.data(), c.mData.size()))
                {
                    broken = true;
                    err = "The connection to the stream server dropped.";
                }
            }
            wdj_close(s);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStop) break;
            }
            setState(RETRYING, err + " Reconnecting...");
            if (!pause(backoff_ms)) break;
        }
        setState(OFF_AIR, "");
    }

    WolfDJMixer::StreamConfig mCfg;
    std::thread mThread;
    mutable std::mutex mMutex;
    std::condition_variable mCond;
    std::deque<Chunk> mQueue;
    size_t mQueued = 0;
    bool mStop = false;
    std::atomic<bool> mWantRestart{ false };
    std::atomic<int> mState{ OFF_AIR };
    mutable std::mutex mMsgMutex;
    std::string mMessage;
    std::chrono::steady_clock::time_point mOnAirSince;
};

// ---------------------------------------------------------------------------------------------
// In-world voice: the llwebrtc playout tap (llwebrtc.cpp LLWebRTCAudioTransport::NeedMorePlayData)
// ---------------------------------------------------------------------------------------------

namespace
{
    struct VoiceTap : public llwebrtc::LLWebRTCPlayoutTap
    {
        void onPlayoutAudio(const int16_t* samples, size_t frames, size_t channels, uint32_t sample_rate) override
        {
            WolfDJMixer::instance().channel(CH_VOICE).pushS16(samples, frames, channels, sample_rate);
        }
    };
    VoiceTap sVoiceTap;     // static: outlives the voice engine, so the tap is never left dangling
}

// ---------------------------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------------------------

WolfDJMixer& WolfDJMixer::instance()
{
    static WolfDJMixer sInstance;
    return sInstance;
}

WolfDJMixer::WolfDJMixer()
{
    mBus.assign(TICK_FRAMES * 2, 0.f);
    mTmp.assign(TICK_FRAMES * 2, 0.f);
    for (int ci = CH_MUSIC_A; ci < CH_COUNT; ++ci)
    {
        mChannels[ci].mFader = 0.7f;
    }
}

WolfDJMixer::~WolfDJMixer()
{
    for (auto& cap : mCaptures) cap.reset();
    mRunning = false;
    if (mThread.joinable()) mThread.join();
}

void WolfDJMixer::setCapture(int ch, std::unique_ptr<WolfDJCaptureStream> cap, const std::string& id)
{
    mCaptures[ch].reset();          // its destructor stops the stream before the next one starts
    mChannels[ch].clear();
    mCaptures[ch] = std::move(cap);
    mCaptureIds[ch] = mCaptures[ch] ? id : std::string();
}

void WolfDJMixer::shutdown()
{
    goOffAir();
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        setCapture(ch, nullptr, std::string());
    }
    stopEngineIfIdle();
}

void WolfDJMixer::startEngine()
{
    if (mRunning) return;
    mRunning = true;
    mThread = std::thread([this]() { run(); });
    gIdleCallbacks.addFunction(&WolfDJMixer::onIdle, this);
    attachVoiceTap(true);
}

void WolfDJMixer::stopEngineIfIdle()
{
    if (!mRunning || mLiveRequested) return;
    mRunning = false;
    if (mThread.joinable()) mThread.join();
    gIdleCallbacks.deleteFunction(&WolfDJMixer::onIdle, this);
    attachVoiceTap(false);
    mCue = CUE_NONE;
    idleCue();      // releases the OpenAL source
    for (WolfDJChannel& ch : mChannels)
    {
        ch.mPeak = 0.f;
        ch.mPrePeak = 0.f;
    }
    mMasterPeak = 0.f;
}

void WolfDJMixer::attachVoiceTap(bool attach)
{
    if (llwebrtc::LLWebRTCDeviceInterface* dev = llwebrtc::getDeviceInterface())
    {
        dev->setPlayoutTap(attach ? &sVoiceTap : nullptr);
    }
}

// static - main thread
void WolfDJMixer::onIdle(void* data)
{
    WolfDJMixer* self = static_cast<WolfDJMixer*>(data);
    // The voice engine can be torn down and rebuilt (voice off/on, a device change): put the
    // tap back now and then. It is one atomic store.
    self->mVoiceTapSeconds += 1.f / 30.f;
    if (self->mVoiceTapSeconds > 2.f)
    {
        self->mVoiceTapSeconds = 0.f;
        self->attachVoiceTap(true);
    }
    self->idleCue();
}

void WolfDJMixer::run()
{
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (mRunning)
    {
        next += std::chrono::milliseconds(TICK_FRAMES * 1000 / RATE);
        mixTick();
        std::this_thread::sleep_until(next);
        if (clock::now() - next > std::chrono::milliseconds(200))
        {
            next = clock::now();    // the machine stalled: do not try to catch up
        }
    }
}

void WolfDJMixer::mixTick()
{
    const size_t N = TICK_FRAMES;
    std::fill(mBus.begin(), mBus.end(), 0.f);
    const int cue = mCue.load();
    std::vector<float> cue_buf;
    std::vector<float> monitor_buf;     // channels heard through the DJ's own output (the playlist)
    float mic_level = 0.f;
    const bool talk_over = mTalkOver.load();

    for (int ci = 0; ci < CH_COUNT; ++ci)
    {
        WolfDJChannel& ch = mChannels[ci];
        const size_t got = ch.pull(mTmp.data(), N);
        if (got == 0)
        {
            ch.mPeak = 0.f;
            ch.mPrePeak = 0.f;
            if (cue == ci) cue_buf.assign(N * 2, 0.f);
            continue;
        }

        // EQ: 100 Hz low shelf, 1 kHz peak, 8 kHz high shelf.
        const float lo = ch.mEqLowDb, mi = ch.mEqMidDb, hi = ch.mEqHighDb;
        if (lo != ch.mLastLow) { ch.mLow.setLowShelf(100.f, lo); ch.mLastLow = lo; }
        if (mi != ch.mLastMid) { ch.mMid.setPeaking(1000.f, 0.7f, mi); ch.mLastMid = mi; }
        if (hi != ch.mLastHigh) { ch.mHigh.setHighShelf(8000.f, hi); ch.mLastHigh = hi; }
        const bool eq_on = lo != 0.f || mi != 0.f || hi != 0.f;
        float pre_peak = 0.f;
        for (size_t i = 0; i < N * 2; ++i)
        {
            float x = mTmp[i];
            if (eq_on)
            {
                const int side = (int)(i & 1);
                x = ch.mHigh.process(side, ch.mMid.process(side, ch.mLow.process(side, x)));
                mTmp[i] = x;
            }
            pre_peak = std::max(pre_peak, fabsf(x));
        }
        ch.mPrePeak = pre_peak;
        if (cue == ci) cue_buf.assign(mTmp.begin(), mTmp.end());

        // Talk-over: the music channels dip while the mic is in use (decided below, applied
        // with this tick's smoothed gain).
        float gain = ch.mMute ? 0.f : wolfdj_fader_gain(ch.mFader);
        if (ci >= CH_MUSIC_A)
        {
            gain *= ch.mDuck;
        }
        float post_peak = 0.f, sum_sq = 0.f;
        for (size_t i = 0; i < N * 2; ++i)
        {
            const float y = mTmp[i] * gain;
            mBus[i] += y;
            post_peak = std::max(post_peak, fabsf(y));
            sum_sq += y * y;
        }
        if (ch.mMonitor)
        {
            if (monitor_buf.empty()) monitor_buf.assign(N * 2, 0.f);
            for (size_t i = 0; i < N * 2; ++i) monitor_buf[i] += mTmp[i] * gain;
        }
        ch.mPeak = post_peak;
        if (ci == CH_MIC)
        {
            mic_level = sqrtf(sum_sq / (float)(N * 2));
        }
    }

    // Talk-over envelope: fast down, slow back up. 0.02 RMS = about -34 dBFS, i.e. speech.
    const float duck_target = (talk_over && mic_level > 0.02f) ? 0.3f : 1.f;
    for (int ci = CH_MUSIC_A; ci < CH_COUNT; ++ci)
    {
        float& d = mChannels[ci].mDuck;
        d = duck_target < d ? std::max(duck_target, d - 0.25f) : std::min(duck_target, d + 0.04f);
    }

    // Master + limiter (no look-ahead: the gain change is spread over the tick, then hard clip).
    const float master = wolfdj_fader_gain(mMasterFader);
    float peak = 0.f;
    for (size_t i = 0; i < N * 2; ++i)
    {
        mBus[i] *= master;
        peak = std::max(peak, fabsf(mBus[i]));
    }
    const float old_lim = mLimiter;
    float new_lim = std::min(1.f, mLimiter * 1.01f);
    if (peak * new_lim > 0.95f) new_lim = 0.95f / peak;
    float master_peak = 0.f;
    for (size_t i = 0; i < N; ++i)
    {
        const float g = old_lim + (new_lim - old_lim) * (float)(i + 1) / (float)N;
        for (int s = 0; s < 2; ++s)
        {
            float& v = mBus[i * 2 + s];
            v = llclamp(v * g, -1.f, 1.f);
            master_peak = std::max(master_peak, fabsf(v));
        }
    }
    mLimiter = new_lim;
    mLimiterDb = wolfdj_lin_to_db(new_lim);
    mMasterPeak = master_peak;
    if (cue == CUE_MASTER) cue_buf.assign(mBus.begin(), mBus.end());

    // Local output: the cue when one is on, otherwise the monitored channels (the playlist).
    if (cue == CUE_NONE && !monitor_buf.empty())
    {
        cue_buf.swap(monitor_buf);
    }
    if (!cue_buf.empty())
    {
        std::lock_guard<std::mutex> lock(mCueMutex);
        for (float v : cue_buf)
        {
            mCueRing.push_back((int16_t)llclamp(v * 32767.f, -32768.f, 32767.f));
        }
        const size_t cap = RATE * 2 / 2;    // 0.5 s stereo
        while (mCueRing.size() > cap) mCueRing.pop_front();
    }

    // On air.
    std::lock_guard<std::mutex> lock(mTitleMutex);
    if (mLiveRequested && mSender)
    {
        std::string out;
        const bool restart = mSender->takeRestartRequest();
        if (restart || !mEncoder->active())
        {
            // A new connection: a fresh Ogg stream whose headers carry the current title.
            mEncoder = std::make_unique<WolfDJEncoder>();
            if (mTitlePending)
            {
                mArtist = mPendingArtist;
                mTitle = mPendingTitle;
                mTitlePending = false;
            }
            if (mEncoder->begin(mArtist, mTitle, out))
            {
                mSender->push(std::move(out), true);
                out.clear();
            }
        }
        else if (mTitlePending)
        {
            mArtist = mPendingArtist;
            mTitle = mPendingTitle;
            mTitlePending = false;
            mEncoder->end(out);
            mSender->push(std::move(out), false);
            out.clear();
            if (mEncoder->begin(mArtist, mTitle, out))
            {
                mSender->push(std::move(out), true);
                out.clear();
            }
        }
        if (mEncoder->active())
        {
            mEncoder->encode(mBus.data(), N, out);
            if (!out.empty()) mSender->push(std::move(out), false);
        }
    }
}

void WolfDJMixer::goLive(const StreamConfig& cfg)
{
    goOffAir();
    std::lock_guard<std::mutex> lock(mTitleMutex);
    mEncoder = std::make_unique<WolfDJEncoder>();
    mSender = std::make_unique<WolfDJSender>(cfg);
    mLiveRequested = true;
    startEngine();
}

void WolfDJMixer::goOffAir()
{
    std::unique_ptr<WolfDJSender> sender;
    {
        std::lock_guard<std::mutex> lock(mTitleMutex);
        mLiveRequested = false;
        if (mSender && mEncoder && mEncoder->active())
        {
            std::string out;
            mEncoder->end(out);     // EOS, so listeners' players end cleanly
            mSender->push(std::move(out), false);
        }
        sender = std::move(mSender);
        mEncoder.reset();
    }
    if (sender)
    {
        sender->drain(1500);
        sender.reset();     // joins the sender thread
    }
}

EState WolfDJMixer::state() const
{
    std::lock_guard<std::mutex> lock(mTitleMutex);
    return mSender ? mSender->state() : OFF_AIR;
}

std::string WolfDJMixer::statusMessage() const
{
    std::lock_guard<std::mutex> lock(mTitleMutex);
    return mSender ? mSender->message() : std::string();
}

double WolfDJMixer::onAirSeconds() const
{
    std::lock_guard<std::mutex> lock(mTitleMutex);
    return mSender ? mSender->onAirSeconds() : 0.0;
}

void WolfDJMixer::setNowPlaying(const std::string& artist, const std::string& title)
{
    std::lock_guard<std::mutex> lock(mTitleMutex);
    mPendingArtist = artist;
    mPendingTitle = title;
    mTitlePending = true;
}

// ---------------------------------------------------------------------------------------------
// Cue output - main thread. One OpenAL source on the viewer's own context (LLAudioEngine_OpenAL),
// fed 20 ms 16-bit stereo buffers from the cue ring.
// ---------------------------------------------------------------------------------------------

void WolfDJMixer::idleCue()
{
#if LL_OPENAL
    bool monitored = false;
    for (const WolfDJChannel& ch : mChannels)
    {
        monitored = monitored || ch.mMonitor.load();
    }
    const bool want = mRunning && (mCue.load() != CUE_NONE || monitored);
    if (!want)
    {
        if (mCueSource)
        {
            ALuint src = mCueSource;
            alSourceStop(src);
            ALint queued = 0;
            alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
            while (queued-- > 0)
            {
                ALuint b = 0;
                alSourceUnqueueBuffers(src, 1, &b);
                mCueFreeBuffers.push_back(b);
            }
            for (unsigned b : mCueFreeBuffers)
            {
                ALuint ab = b;
                alDeleteBuffers(1, &ab);
            }
            mCueFreeBuffers.clear();
            alDeleteSources(1, &src);
            mCueSource = 0;
            mCueStarted = false;
        }
        std::lock_guard<std::mutex> lock(mCueMutex);
        mCueRing.clear();
        return;
    }
    if (!mCueSource)
    {
        alGetError();
        ALuint src = 0;
        alGenSources(1, &src);
        if (alGetError() != AL_NO_ERROR || src == 0) return;
        alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(src, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcef(src, AL_GAIN, 1.f);
        ALuint bufs[6];
        alGenBuffers(6, bufs);
        if (alGetError() != AL_NO_ERROR)
        {
            alDeleteSources(1, &src);
            return;
        }
        mCueSource = src;
        mCueFreeBuffers.assign(bufs, bufs + 6);
        mCueStarted = false;
    }
    ALuint src = mCueSource;
    ALint processed = 0;
    alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0)
    {
        ALuint b = 0;
        alSourceUnqueueBuffers(src, 1, &b);
        mCueFreeBuffers.push_back(b);
    }
    std::vector<int16_t> chunk;
    const size_t chunk_samples = TICK_FRAMES * 2;
    while (!mCueFreeBuffers.empty())
    {
        {
            std::lock_guard<std::mutex> lock(mCueMutex);
            if (mCueRing.size() < chunk_samples) break;
            chunk.assign(mCueRing.begin(), mCueRing.begin() + chunk_samples);
            mCueRing.erase(mCueRing.begin(), mCueRing.begin() + chunk_samples);
        }
        ALuint b = mCueFreeBuffers.back();
        mCueFreeBuffers.pop_back();
        alBufferData(b, AL_FORMAT_STEREO16, chunk.data(), (ALsizei)(chunk.size() * sizeof(int16_t)), RATE);
        alSourceQueueBuffers(src, 1, &b);
    }
    ALint queued = 0, playing = 0;
    alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(src, AL_SOURCE_STATE, &playing);
    if (playing != AL_PLAYING && queued >= 3)
    {
        alSourcePlay(src);      // also restarts after an underrun
        mCueStarted = true;
    }
#endif
}
