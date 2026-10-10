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
#include "wolfdjplayer.h"

#include "llbase64.h"
#include "llcallbacklist.h"
#include "llrand.h"
#include "llwebrtc.h"

#include "vorbis/codec.h"
#include "vorbis/vorbisenc.h"

#if LL_OPENAL
#include "AL/al.h"
#include "AL/alc.h"
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

// Same cookbook, LPF and HPF.
void WolfDJBiquad::setLowPass(float f0, float q)
{
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.f * q);
    const float a0 = 1.f + alpha;
    b0 = (1.f - cw) / 2.f / a0;
    b1 = (1.f - cw) / a0;
    b2 = (1.f - cw) / 2.f / a0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha) / a0;
}

void WolfDJBiquad::setHighPass(float f0, float q)
{
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.f * q);
    const float a0 = 1.f + alpha;
    b0 = (1.f + cw) / 2.f / a0;
    b1 = -(1.f + cw) / a0;
    b2 = (1.f + cw) / 2.f / a0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha) / a0;
}

// Source: RBJ cookbook, BPF "constant 0 dB peak gain": b0 = alpha, b1 = 0, b2 = -alpha,
// a0 = 1 + alpha, a1 = -2 cos(w0), a2 = 1 - alpha.
void WolfDJBiquad::setBandPass(float f0, float q)
{
    const float w0 = 2.f * PI_F * f0 / (float)RATE;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.f * q);
    const float a0 = 1.f + alpha;
    b0 = alpha / a0;
    b1 = 0.f;
    b2 = -alpha / a0;
    a1 = -2.f * cw / a0;
    a2 = (1.f - alpha) / a0;
}

// ---------------------------------------------------------------------------------------------
// Effects [WOLF DJ 2026-10-05] - Paul: "can we add some basic effects?"
// Reverb: the Schroeder/Moorer structure of Jezar's public-domain Freeverb (tuning.h): parallel
// lowpass-feedback combs then series allpasses, its 44.1 kHz delay lengths scaled to RATE, the
// right side 23 samples longer (stereospread), input gain 0.015 (fixedgain), allpass feedback
// 0.5, room 0.84 (offsetroom 0.7 + 0.5 x scaleroom 0.28), damping 0.2 (0.5 x scaledamp 0.4).
// The first four combs and first two allpasses: a basic room, not the full eight-comb tank.
// ---------------------------------------------------------------------------------------------

namespace
{
    constexpr size_t ECHO_FRAMES = RATE * 375 / 1000;
    constexpr int FV_COMB[4] = { 1116, 1188, 1277, 1356 };
    constexpr int FV_ALLPASS[2] = { 556, 441 };
    constexpr int FV_SPREAD = 23;
    constexpr float FV_FIXED_GAIN = 0.015f;
    constexpr float FV_ROOM = 0.84f;
    constexpr float FV_DAMP = 0.2f;
    constexpr float FV_ALLPASS_FEEDBACK = 0.5f;

    size_t fv_len(int at44k, int side)
    {
        return (size_t)((at44k + (side ? FV_SPREAD : 0)) * (double)RATE / 44100.0);
    }
}

void WolfDJEffect::reset(int fx)
{
    mFx = fx;
    mDelay.clear();
    mDelayPos = 0;
    for (int s = 0; s < 2; ++s)
    {
        for (Comb& c : mComb[s]) { c.mBuf.clear(); c.mPos = 0; c.mStore = 0.f; }
        for (AllPass& a : mAll[s]) { a.mBuf.clear(); a.mPos = 0; }
    }
    if (fx == FX_ECHO)
    {
        mDelay.assign(ECHO_FRAMES * 2, 0.f);
    }
    else if (fx == FX_REVERB)
    {
        for (int s = 0; s < 2; ++s)
        {
            for (int i = 0; i < 4; ++i) mComb[s][i].mBuf.assign(fv_len(FV_COMB[i], s), 0.f);
            for (int i = 0; i < 2; ++i) mAll[s][i].mBuf.assign(fv_len(FV_ALLPASS[i], s), 0.f);
        }
    }
    else if (fx == FX_RADIO)
    {
        mHighPass.setHighPass(300.f, 0.707f);
        mLowPass.setLowPass(3400.f, 0.707f);
        mHighPass.reset();
        mLowPass.reset();
    }
}

void WolfDJEffect::process(int fx, float amount, float* stereo, size_t frames)
{
    if (fx != mFx) reset(fx);
    if (fx == FX_NONE) return;
    amount = llclamp(amount, 0.f, 1.f);
    if (fx == FX_ECHO)
    {
        const float feedback = 0.25f + 0.4f * amount;    // more amount: louder and longer repeats
        for (size_t f = 0; f < frames; ++f)
        {
            for (int s = 0; s < 2; ++s)
            {
                float& d = mDelay[mDelayPos * 2 + s];
                const float x = stereo[f * 2 + s];
                const float echoed = d;
                d = x + echoed * feedback;
                stereo[f * 2 + s] = x + echoed * amount;
            }
            if (++mDelayPos == ECHO_FRAMES) mDelayPos = 0;
        }
    }
    else if (fx == FX_REVERB)
    {
        const float wet = amount * 3.f;     // Freeverb scalewet
        const float dry = 1.f - 0.5f * amount;
        for (size_t f = 0; f < frames; ++f)
        {
            const float in = (stereo[f * 2] + stereo[f * 2 + 1]) * FV_FIXED_GAIN;
            for (int s = 0; s < 2; ++s)
            {
                float out = 0.f;
                for (Comb& c : mComb[s])
                {
                    const float y = c.mBuf[c.mPos];
                    c.mStore = y * (1.f - FV_DAMP) + c.mStore * FV_DAMP;
                    c.mBuf[c.mPos] = in + c.mStore * FV_ROOM;
                    if (++c.mPos == c.mBuf.size()) c.mPos = 0;
                    out += y;
                }
                for (AllPass& a : mAll[s])
                {
                    const float b = a.mBuf[a.mPos];
                    a.mBuf[a.mPos] = out + b * FV_ALLPASS_FEEDBACK;
                    if (++a.mPos == a.mBuf.size()) a.mPos = 0;
                    out = b - out;
                }
                stereo[f * 2 + s] = stereo[f * 2 + s] * dry + out * wet;
            }
        }
    }
    else if (fx == FX_RADIO)
    {
        const float drive = 1.f + 4.f * amount;
        const float norm = 1.f / tanhf(drive);
        for (size_t i = 0; i < frames * 2; ++i)
        {
            const int side = (int)(i & 1);
            const float x = mLowPass.process(side, mHighPass.process(side, stereo[i]));
            stereo[i] = tanhf(x * drive) * norm;
        }
    }
}

// Source: the Behringer X32 / Midas M32 fader scale (X32 OSC protocol, documented by Patrick-Gilles
// Maillot): four straight dB runs - 0..0.0625 is -90..-60 dB, 0.0625..0.25 is -60..-30 dB,
// 0.25..0.5 is -30..-10 dB, 0.5..1 is -10..+10 dB - so 0 dB sits at 0.75, as on any desk's
// long-throw fader. The bottom stop is off (the desk's -inf).
float wolfdj_fader_db(float pos)
{
    pos = llclamp(pos, 0.f, 1.f);
    if (pos <= 0.001f) return -120.f;
    if (pos >= 0.5f) return pos * 40.f - 30.f;
    if (pos >= 0.25f) return pos * 80.f - 50.f;
    if (pos >= 0.0625f) return pos * 160.f - 70.f;
    return pos * 480.f - 90.f;
}

float wolfdj_fader_pos(float db)
{
    if (db <= -90.f) return 0.f;
    if (db < -60.f) return (db + 90.f) / 480.f;
    if (db < -30.f) return (db + 70.f) / 160.f;
    if (db < -10.f) return (db + 50.f) / 80.f;
    return llclamp((db + 30.f) / 40.f, 0.f, 1.f);
}

float wolfdj_fader_gain(float pos)
{
    const float db = wolfdj_fader_db(pos);
    return db <= -120.f ? 0.f : powf(10.f, db / 20.f);
}

// The law before 10-10 was pos^2 x 1.4125 (+3 dB at the top): the same gain on the new scale.
float wolfdj_old_fader_to_pos(float old_pos)
{
    old_pos = llclamp(old_pos, 0.f, 1.f);
    const float gain = old_pos * old_pos * 1.4125f;
    return gain <= 0.f ? 0.f : wolfdj_fader_pos(20.f * log10f(gain));
}

const float WolfDJ::SPECTRUM_HZ[WolfDJ::SPECTRUM_BANDS] = { 31.5f, 63.f, 125.f, 250.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 16000.f };

float wolfdj_lin_to_db(float lin)
{
    return lin > 1e-6f ? 20.f * log10f(lin) : -120.f;
}

// ---------------------------------------------------------------------------------------------
// Channel
// ---------------------------------------------------------------------------------------------

static constexpr size_t RING_FRAMES = RATE * 2;             // 2 s
static constexpr int CUE_BUFFERS = 12;                     // the DJ's own output queue, 20 ms each
static constexpr size_t MAX_LAG_FRAMES = RATE / 4;          // more than 250 ms queued...
static constexpr size_t TRIM_TO_FRAMES = RATE * 8 / 100;    // ...is trimmed back to 80 ms

WolfDJChannel::WolfDJChannel()
:   mMaxLag(MAX_LAG_FRAMES), mTrimTo(TRIM_TO_FRAMES)
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

void WolfDJChannel::setLagLimit(float seconds)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (seconds <= 0.f)
    {
        mMaxLag = MAX_LAG_FRAMES;
        mTrimTo = TRIM_TO_FRAMES;
        return;
    }
    mMaxLag = llclamp((size_t)(seconds * RATE), TRIM_TO_FRAMES, RING_FRAMES - RATE / 4);
    mTrimTo = mMaxLag * 2 / 3;
}

size_t WolfDJChannel::pull(float* out, size_t frames)
{
    std::lock_guard<std::mutex> lock(mMutex);
    if (mCount > mMaxLag)
    {
        const size_t drop = mCount - mTrimTo;
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
    mJingle.setLagLimit(PLAYER_RING_SECONDS);   // the jingles decode ahead too
    // [WOLF DJ 2026-10-10] Octave bands: Q = sqrt(2) is a one-octave bandwidth (RBJ cookbook,
    // 1/Q = 2 sinh(ln(2)/2 * BW * w0/sin(w0)), about sqrt(2) for BW = 1 well below Nyquist).
    for (int b = 0; b < SPECTRUM_BANDS; ++b)
    {
        mSpectrumFilters[b].setBandPass(SPECTRUM_HZ[b], 1.41421356f);
        mSpectrum[b] = 0.f;
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
    // [WOLF DJ 2026-10-10] The playlist decodes ahead (WolfDJPlayer::run, PLAYER_LEAD): give it
    // room so a slow moment on its thread is not trimmed away as lag. Live sources keep 250 ms.
    mChannels[ch].setLagLimit(mCaptureIds[ch] == WOLFDJ_PLAYLIST_ID ? PLAYER_RING_SECONDS : 0.f);
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
    mMasterPeakL = 0.f;
    mMasterPeakR = 0.f;
    mMasterPrePeak = 0.f;
    mJinglePeak = 0.f;
    mJinglePrePeak = 0.f;
    for (std::atomic<float>& b : mSpectrum) b = 0.f;
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
    // [WOLF DJ 2026-10-10] Window shut, nothing live, the playlist stopped: nothing left to mix.
    // Stopping removes this callback - safe from inside it (LLCallbackList::callFunctions has
    // already moved past it).
    if (!self->mWindowOpen && !self->mLiveRequested && !WolfDJPlayer::instance().isPlaying())
    {
        for (int ch = 0; ch < CH_COUNT; ++ch)
        {
            self->setCapture(ch, nullptr, std::string());
        }
        self->mCue = CUE_NONE;
        self->stopEngineIfIdle();
        return;
    }
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
        if (eq_on)
        {
            for (size_t i = 0; i < N * 2; ++i)
            {
                const int side = (int)(i & 1);
                mTmp[i] = ch.mHigh.process(side, ch.mMid.process(side, ch.mLow.process(side, mTmp[i])));
            }
        }
        // [WOLF DJ 2026-10-05] the channel's effect, after its EQ (so Cue hears it too). Called
        // for FX_NONE as well: that clears the last effect's tail, so turning it back on later
        // does not replay it.
        ch.mEffect.process(ch.mFx.load(), ch.mFxAmount.load(), mTmp.data(), N);
        float pre_peak = 0.f;
        for (size_t i = 0; i < N * 2; ++i)
        {
            pre_peak = std::max(pre_peak, fabsf(mTmp[i]));
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

    // [WOLF DJ 2026-10-05] Jingle pads: on air at their own level, and in the DJ's ears while
    // the jingle strip's MON is on (10-10).
    {
        const size_t got = mJingle.pull(mTmp.data(), N);
        float jpeak = 0.f, jpre = 0.f;
        if (got > 0)
        {
            const float gain = wolfdj_fader_gain(mJingleLevel);
            const bool mon = mJingleMonitor.load();
            if (mon && monitor_buf.empty()) monitor_buf.assign(N * 2, 0.f);
            for (size_t i = 0; i < N * 2; ++i)
            {
                const float y = mTmp[i] * gain;
                mBus[i] += y;
                if (mon) monitor_buf[i] += y;
                jpre = std::max(jpre, fabsf(mTmp[i]));
                jpeak = std::max(jpeak, fabsf(y));
            }
        }
        mJinglePeak = jpeak;
        mJinglePrePeak = jpre;
    }

    // Talk-over envelope: fast down, slow back up. 0.02 RMS = about -34 dBFS, i.e. speech.
    const float duck_target = (talk_over && mic_level > 0.02f) ? 0.3f : 1.f;
    mDucking = duck_target < 1.f;
    for (int ci = CH_MUSIC_A; ci < CH_COUNT; ++ci)
    {
        float& d = mChannels[ci].mDuck;
        d = duck_target < d ? std::max(duck_target, d - 0.25f) : std::min(duck_target, d + 0.04f);
    }

    // Master + limiter (no look-ahead: the gain change is spread over the tick, then hard clip).
    // [WOLF DJ 2026-10-10] A master fader per side (Paul: "stereo sliders for the main mix").
    const float master[2] = { wolfdj_fader_gain(mMasterFaderL), wolfdj_fader_gain(mMasterFaderR) };
    float peak = 0.f, pre_master = 0.f;
    for (size_t i = 0; i < N * 2; ++i)
    {
        pre_master = std::max(pre_master, fabsf(mBus[i]));
        mBus[i] *= master[i & 1];
        peak = std::max(peak, fabsf(mBus[i]));
    }
    mMasterPrePeak = pre_master;
    const float old_lim = mLimiter;
    float new_lim = std::min(1.f, mLimiter * 1.01f);
    if (peak * new_lim > 0.95f) new_lim = 0.95f / peak;
    float side_peak[2] = { 0.f, 0.f };
    for (size_t i = 0; i < N; ++i)
    {
        const float g = old_lim + (new_lim - old_lim) * (float)(i + 1) / (float)N;
        for (int s = 0; s < 2; ++s)
        {
            float& v = mBus[i * 2 + s];
            v = llclamp(v * g, -1.f, 1.f);
            side_peak[s] = std::max(side_peak[s], fabsf(v));
        }
    }
    mLimiter = new_lim;
    mLimiterDb = wolfdj_lin_to_db(new_lim);
    mMasterPeakL = side_peak[0];
    mMasterPeakR = side_peak[1];
    mMasterPeak = std::max(side_peak[0], side_peak[1]);

    // [WOLF DJ 2026-10-10] The spectrum display: each octave band's RMS over this tick, of the
    // mono sum of what goes on air.
    for (int b = 0; b < SPECTRUM_BANDS; ++b)
    {
        WolfDJBiquad& f = mSpectrumFilters[b];
        float sum_sq = 0.f;
        for (size_t i = 0; i < N; ++i)
        {
            const float y = f.process(0, 0.5f * (mBus[i * 2] + mBus[i * 2 + 1]));
            sum_sq += y * y;
        }
        mSpectrum[b] = sqrtf(sum_sq / (float)N);
    }
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
    // [WOLF DJ 2026-10-10] A channel's MON only counts while it has a source (MON stays on a
    // channel with nothing on it, and that must not hold the output open); the jingles' MON while
    // a pad plays.
    bool monitored = mJingleMonitor.load() && WolfDJJingles::instance().playing() >= 0;
    for (int ch = 0; ch < CH_COUNT; ++ch)
    {
        monitored = monitored || (mChannels[ch].mMonitor.load() && mCaptures[ch]);
    }
    const bool want = mRunning && (mCue.load() != CUE_NONE || monitored);
    if (!want)
    {
        closeCueOutput();
        std::lock_guard<std::mutex> lock(mCueMutex);
        mCueRing.clear();
        return;
    }

    // Our own device + context; every call below runs with it current and the viewer's put back.
    if (!mCueContext)
    {
        ALCdevice* dev = alcOpenDevice(NULL);
        if (!dev) return;
        ALCcontext* ctx = alcCreateContext(dev, NULL);
        if (!ctx)
        {
            alcCloseDevice(dev);
            return;
        }
        mCueDevice = dev;
        mCueContext = ctx;
    }
    ALCcontext* viewer_ctx = alcGetCurrentContext();
    alcMakeContextCurrent((ALCcontext*)mCueContext);
    struct Restore
    {
        ALCcontext* mCtx;
        ~Restore() { alcMakeContextCurrent(mCtx); }
    } restore{ viewer_ctx };

    // Unplugged (ALC_EXT_disconnect): start again on the default device next time.
    ALCdevice* dev = (ALCdevice*)mCueDevice;
    if (alcIsExtensionPresent(dev, "ALC_EXT_disconnect"))
    {
        ALCint connected = 1;
        alcGetIntegerv(dev, alcGetEnumValue(dev, "ALC_CONNECTED"), 1, &connected);
        if (!connected)
        {
            alcMakeContextCurrent(viewer_ctx);
            closeCueOutput();
            return;
        }
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
        // [WOLF DJ 2026-10-10] 12 x 20 ms: this is fed from the main thread, so the queue has to
        // ride out a slow viewer frame (a teleport, a region crossing) without the DJ's own
        // monitoring dropping out (Paul: "stop it from not playing"). It starts after 3 (60 ms).
        ALuint bufs[CUE_BUFFERS];
        alGenBuffers(CUE_BUFFERS, bufs);
        if (alGetError() != AL_NO_ERROR)
        {
            alDeleteSources(1, &src);
            return;
        }
        mCueSource = src;
        mCueFreeBuffers.assign(bufs, bufs + CUE_BUFFERS);
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

// Main thread. Releases the DJ output's source, buffers, context and device.
void WolfDJMixer::closeCueOutput()
{
#if LL_OPENAL
    if (!mCueContext) return;
    ALCcontext* viewer_ctx = alcGetCurrentContext();
    alcMakeContextCurrent((ALCcontext*)mCueContext);
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
    alcMakeContextCurrent(viewer_ctx == (ALCcontext*)mCueContext ? NULL : viewer_ctx);
    alcDestroyContext((ALCcontext*)mCueContext);
    alcCloseDevice((ALCdevice*)mCueDevice);
    mCueContext = nullptr;
    mCueDevice = nullptr;
#endif
}
