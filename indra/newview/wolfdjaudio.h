/**
 * @file wolfdjaudio.h
 * @brief WolfViewer Wolf DJ: a mini mixer that sends in-world voice, the microphone and music
 *        from other programs to the DJ's own radio stream (cast.wolfterritories.org /radio/<slug>).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_DJAUDIO_H
#define WOLF_DJAUDIO_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// [WOLF DJ 2026-10-04] Paul: "dj's stream in world and right now they have to use mixx or
// something what about if you're logged in ... we have a mini mixer and you can stream any of
// your applications directly to your stream and set the land stream if you have permissions",
// "have a talk show mode so that we can pick up voice in world and transmit it on to the stream
// thats really really what i need", "faders as well with eq levels", "level indicator leds so
// they can see if it is too high and a button to just listen to one channel through the output".
//
// The server side already exists (gridmanager regionstreamlib.php / streamauth.php): every
// region owner has an Icecast 2.4.4 mount /radio/<slug> on cast.wolfterritories.org, checked by
// URL auth on every source connect. This is a source client like Mixxx:
//   capture (wolfdjcapture_*.cpp, the voice playout tap in llwebrtc) -> WolfDJChannel rings
//   -> WolfDJMixer thread every 20 ms (EQ, fader, talk-over, limiter, meters, cue)
//   -> Ogg Vorbis (libvorbisenc, as llvorbisencode.cpp) -> WolfDJSender thread -> Icecast.
// Icecast facts relied on (icecast-2.4.4): connection.c _handle_connection accepts SOURCE and
// PUT; connection_complete_source answers "HTTP/1.0 200 OK"; no chunked transfer; format.c
// format_get_type maps "application/ogg" to the Ogg handler; format_ogg.c has no set_tag, so a
// song title is sent in-band as a new chained Ogg stream whose Vorbis comments carry TITLE and
// ARTIST (update_comments -> stats "title"/"artist").

namespace WolfDJ
{
    constexpr unsigned RATE = 48000;            // the mix, the encoder and every channel ring
    constexpr unsigned TICK_FRAMES = 960;       // 20 ms per mixer tick
    constexpr float EQ_MIN_DB = -24.f;          // near-kill
    constexpr float EQ_MAX_DB = 12.f;

    enum EChannel
    {
        CH_VOICE = 0,   // in-world voice: everyone you hear (llwebrtc playout tap)
        CH_MIC,         // your microphone
        CH_MUSIC_A,     // another program, everything you hear, or the playlist
        CH_MUSIC_B,
        CH_MUSIC_C,     // [WOLF DJ 2026-10-04] Paul: "can we have more channels"
        CH_MUSIC_D,
        CH_COUNT
    };
    // [WOLF DJ 2026-10-05] Paul: "can we add some basic effects?" - one per channel, after its EQ.
    enum EFx
    {
        FX_NONE = 0,
        FX_ECHO,        // 375 ms delay with feedback
        FX_REVERB,      // Freeverb-style room
        FX_RADIO,       // telephone band (300 Hz - 3.4 kHz) with drive
        FX_COUNT
    };
    constexpr int CUE_NONE = -1;
    constexpr int CUE_MASTER = CH_COUNT;        // cue the stream mix

    // [WOLF DJ 2026-10-10] The desk's spectrum display: octave bands of what is on air, centred
    // on the ISO 266 preferred octave frequencies 31.5 Hz .. 16 kHz.
    constexpr int SPECTRUM_BANDS = 10;
    extern const float SPECTRUM_HZ[SPECTRUM_BANDS];

    enum EState
    {
        OFF_AIR = 0,
        CONNECTING,
        ON_AIR,
        RETRYING,       // dropped or refused for a reason that may pass: tries again
        FAILED          // wrong password / not your stream: does not try again
    };
}

// One RBJ biquad (Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter
// coefficients"), transposed direct form II, one state per stereo side.
struct WolfDJBiquad
{
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1[2] = { 0.f, 0.f }, z2[2] = { 0.f, 0.f };

    void setLowShelf(float f0, float db);
    void setPeaking(float f0, float q, float db);
    void setHighShelf(float f0, float db);
    void setLowPass(float f0, float q);     // [WOLF DJ 2026-10-05] the radio effect
    void setHighPass(float f0, float q);
    void setBandPass(float f0, float q);    // [WOLF DJ 2026-10-10] the spectrum display
    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.f; }
    float process(int side, float x)
    {
        const float y = b0 * x + z1[side];
        z1[side] = b1 * x - a1 * y + z2[side];
        z2[side] = b2 * x - a2 * y;
        return y;
    }
};

// [WOLF DJ 2026-10-05] A channel's effect. Mixer thread only. Changing the effect clears its
// state, so a new echo or room does not start with the tail of the last one.
class WolfDJEffect
{
public:
    // stereo: frames x 2 interleaved floats, processed in place. amount 0..1.
    void process(int fx, float amount, float* stereo, size_t frames);

private:
    void reset(int fx);

    struct Comb
    {
        std::vector<float> mBuf;
        size_t mPos = 0;
        float mStore = 0.f;
    };
    struct AllPass
    {
        std::vector<float> mBuf;
        size_t mPos = 0;
    };

    int mFx = -1;
    std::vector<float> mDelay;          // echo, stereo frames
    size_t mDelayPos = 0;
    Comb mComb[2][4];                   // reverb, per side
    AllPass mAll[2][2];
    WolfDJBiquad mHighPass, mLowPass;   // radio
};

// A mixer channel: capture threads push audio in (any rate, any channel count), the mixer
// thread pulls 48 kHz stereo out. The controls are atomics the floater writes.
class WolfDJChannel
{
public:
    WolfDJChannel();

    // Producer side - any thread. Interleaved samples.
    void pushFloat(const float* samples, size_t frames, size_t channels, unsigned rate);
    void pushS16(const int16_t* samples, size_t frames, size_t channels, unsigned rate);
    void clear();

    // Consumer side - mixer thread only. Fills frames x 2 floats; returns frames actually had.
    size_t pull(float* out, size_t frames);

    // [WOLF DJ 2026-10-10] How much may wait in the ring before the oldest is dropped. Live
    // sources (mic, voice, programs) keep the default 250 ms so they stay close to real time; a
    // file player that decodes ahead (the playlist, the jingles) gets more, so a moment's delay
    // on its thread is not a gap on air (Paul: "stop it from not playing").
    void setLagLimit(float seconds);

    std::atomic<float> mFader{ 0.75f };         // 0..1, see faderGain()
    std::atomic<bool>  mMute{ false };
    std::atomic<float> mEqLowDb{ 0.f }, mEqMidDb{ 0.f }, mEqHighDb{ 0.f };
    std::atomic<float> mPeak{ 0.f };            // post-fader peak of the last tick, 0..1+
    std::atomic<float> mPrePeak{ 0.f };         // pre-fader (what cue hears)
    std::atomic<bool>  mActive{ false };        // a source is attached and delivering
    // Also play this channel (post-fader) through the DJ's own output: the strip's MON button
    // (Paul 10-10: "have a MON button on each channel so i can turn off monitoring"). On by
    // default for the playlist, whose music exists only inside the viewer (Paul 10-04: "dj
    // playlist i cant hear the music it needs to monitor back"). Cue, when on, replaces it.
    std::atomic<bool>  mMonitor{ false };

    std::atomic<int>   mFx{ WolfDJ::FX_NONE };  // [WOLF DJ 2026-10-05] WolfDJ::EFx
    std::atomic<float> mFxAmount{ 0.35f };      // 0..1

    // Mixer-thread EQ and effect state.
    WolfDJEffect mEffect;
    WolfDJBiquad mLow, mMid, mHigh;
    float mLastLow = 0.f, mLastMid = 0.f, mLastHigh = 0.f;
    float mDuck = 1.f;                          // talk-over gain, smoothed

private:
    void pushStereo(const float* stereo, size_t frames, unsigned rate);

    std::mutex mMutex;
    std::vector<float> mRing;                   // stereo frames
    size_t mHead = 0, mCount = 0;               // in frames
    size_t mMaxLag, mTrimTo;                    // in frames, see setLagLimit
    // Linear resampler: position since the last input frame of the previous block.
    unsigned mInRate = 0;
    double mPos = 1.0;
    float mPrevL = 0.f, mPrevR = 0.f;
    std::vector<float> mScratch, mResampled;
};

// [WOLF DJ 2026-10-10] Fader position (0..1) to dB and back: a mixing desk's long-throw scale,
// +10 dB at the top, 0 dB three quarters of the way up, -60 dB near the bottom, off at the
// bottom stop (Paul: "make the volume controls into proper mixing desk sliders"). See the .cpp.
float wolfdj_fader_db(float pos);               // -120 (off) at the bottom stop
float wolfdj_fader_pos(float db);
float wolfdj_fader_gain(float pos);             // linear
float wolfdj_lin_to_db(float lin);
// The fader law before 10-10 (a squared law, +3 dB at the top), for saved levels.
float wolfdj_old_fader_to_pos(float old_pos);

class WolfDJCaptureStream;
class WolfDJEncoder;
class WolfDJSender;

class WolfDJMixer
{
public:
    static WolfDJMixer& instance();

    WolfDJChannel& channel(int ch) { return mChannels[ch]; }

    // The mixing thread runs while the floater is open or while on air (meters need it either way).
    void startEngine();
    void stopEngineIfIdle();
    bool engineRunning() const { return mRunning.load(); }

    struct StreamConfig
    {
        std::string mHost;
        int         mPort = 8000;
        std::string mMount;         // "/radio/<slug>"
        std::string mPassword;
        std::string mStationName;
        std::string mDescription;
    };
    void goLive(const StreamConfig& cfg);
    void goOffAir();
    WolfDJ::EState state() const;
    std::string statusMessage() const;          // last error or connection note
    double onAirSeconds() const;                // since the current connection went on air
    bool isLiveRequested() const { return mLiveRequested.load(); }

    // Song title for listeners (a new chained Ogg stream with TITLE/ARTIST comments).
    void setNowPlaying(const std::string& artist, const std::string& title);

    // [WOLF DJ 2026-10-10] Paul: "we need stereo sliders for the main mix" - a fader per side.
    std::atomic<float> mMasterFaderL{ 0.75f };
    std::atomic<float> mMasterFaderR{ 0.75f };
    std::atomic<bool>  mTalkOver{ true };       // the mic dips the music channels
    std::atomic<bool>  mDucking{ false };       // [WOLF DJ 2026-10-10] talk-over is dipping them now
    std::atomic<int>   mCue{ WolfDJ::CUE_NONE };
    std::atomic<float> mMasterPeak{ 0.f };      // the louder side, post-limiter
    std::atomic<float> mMasterPeakL{ 0.f };     // [WOLF DJ 2026-10-10] each side, post-limiter
    std::atomic<float> mMasterPeakR{ 0.f };
    std::atomic<float> mLimiterDb{ 0.f };       // gain reduction now, for the floater

    // [WOLF DJ 2026-10-05] Jingle pads (WolfDJJingles, wolfdjplayer.cpp) play into this: not a
    // strip, just a level. Always heard by the DJ too (like the playlist), never ducked.
    WolfDJChannel& jingleChannel() { return mJingle; }
    std::atomic<float> mJingleLevel{ 0.75f };   // fader position 0..1 (wolfdj_fader_gain)
    std::atomic<float> mJinglePeak{ 0.f };      // post-fader
    std::atomic<float> mJinglePrePeak{ 0.f };   // [WOLF DJ 2026-10-10] pre-fader, for the desk
    std::atomic<bool>  mJingleMonitor{ true };  // [WOLF DJ 2026-10-10] the jingle strip's MON

    // [WOLF DJ 2026-10-10] The on-air spectrum (RMS per band, linear), for the desk's display.
    std::array<std::atomic<float>, WolfDJ::SPECTRUM_BANDS> mSpectrum;
    std::atomic<float> mMasterPrePeak{ 0.f };   // the mix before the master fader

    // Cue output (main thread, OpenAL): the cued channel or the master, pre-fader.
    void idleCue();

    // Capture streams feeding the channels (main thread only). The mixer owns them so a show
    // carries on when the floater is closed. Null cap closes the channel's source.
    void setCapture(int ch, std::unique_ptr<WolfDJCaptureStream> cap, const std::string& id);
    WolfDJCaptureStream* capture(int ch) const { return mCaptures[ch].get(); }
    const std::string& captureId(int ch) const { return mCaptureIds[ch]; }

    // Viewer quitting: off air, sources closed, engine stopped (LLAppViewer::cleanup).
    void shutdown();

    // [WOLF DJ 2026-10-10] The Wolf DJ window is open. With it shut, a playlist left playing
    // keeps the engine going (WolfFloaterDJ::onClose); once that stops and nothing is live, the
    // engine stops itself (onIdle).
    void setWindowOpen(bool open) { mWindowOpen = open; }

    ~WolfDJMixer();

private:
    WolfDJMixer();
    void run();
    void mixTick();
    void attachVoiceTap(bool attach);
    static void onIdle(void*);

    std::array<WolfDJChannel, WolfDJ::CH_COUNT> mChannels;
    WolfDJChannel mJingle;              // [WOLF DJ 2026-10-05] the jingle pads
    // After mChannels, so they are destroyed first (they push into the channels).
    std::array<std::unique_ptr<WolfDJCaptureStream>, WolfDJ::CH_COUNT> mCaptures;
    std::array<std::string, WolfDJ::CH_COUNT> mCaptureIds;
    std::thread mThread;
    std::atomic<bool> mRunning{ false };
    std::atomic<bool> mLiveRequested{ false };
    bool mWindowOpen = false;           // main thread

    // Mixer-thread state.
    std::vector<float> mBus, mTmp;
    float mLimiter = 1.f;
    std::array<WolfDJBiquad, WolfDJ::SPECTRUM_BANDS> mSpectrumFilters;
    std::unique_ptr<WolfDJEncoder> mEncoder;
    std::unique_ptr<WolfDJSender> mSender;

    mutable std::mutex mTitleMutex;     // also guards mEncoder / mSender
    std::string mPendingArtist, mPendingTitle, mArtist, mTitle;
    bool mTitlePending = false;

    // Cue ring (mixer thread -> main thread).
    std::mutex mCueMutex;
    std::deque<int16_t> mCueRing;
    // The DJ's own OpenAL device and context: the viewer's listener gain is 0 whenever it mutes
    // (MuteAudio, or MuteWhenMinimized when its window loses focus - llvieweraudio.cpp
    // audio_update_volume -> LLAudioEngine::setMuted -> LLAudioEngine_OpenAL::setInternalGain),
    // and the DJ must hear the playlist and the cue while working in other programs.
    void* mCueDevice = nullptr;                 // ALCdevice*
    void* mCueContext = nullptr;                // ALCcontext*
    void closeCueOutput();
    unsigned mCueSource = 0;                    // OpenAL source, 0 = none
    std::vector<unsigned> mCueFreeBuffers;
    bool mCueStarted = false;
    float mVoiceTapSeconds = 0.f;
};

#endif // WOLF_DJAUDIO_H
