/**
 * @file wolfdjplayer.h
 * @brief WolfViewer Wolf DJ playlist player: plays audio files from the DJ's computer into a
 *        mixer channel (pick "Wolf DJ playlist" as the source of Music 1 or Music 2).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_DJPLAYER_H
#define WOLF_DJPLAYER_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WolfDJChannel;

// [WOLF DJ 2026-10-04] Paul: "can we also have audio playlists ... on the left the files and
// folders and then on the right the files ... it could open as another floater i'm concerned
// about screen space". Ogg Vorbis (libvorbisfile, already linked), MP3 and FLAC (dr_mp3.h /
// dr_flac.h by David Reid, public domain or MIT-0, vendored in newview) and WAV.
#define WOLFDJ_PLAYLIST_ID "playlist"

namespace WolfDJ
{
    // [WOLF DJ 2026-10-10] How far ahead of the clock the playlist and the jingles decode, and the
    // room their mixer channel gives them (WolfDJChannel::setLagLimit) - more than the lead, so a
    // late wake-up of the player's thread is not trimmed away. Live sources keep 250 ms.
    constexpr double PLAYER_LEAD = 0.25;
    constexpr float PLAYER_RING_SECONDS = 0.6f;
}

class WolfDJPlayer
{
public:
    static WolfDJPlayer& instance();

    // True for the file types this player can decode (by extension).
    static bool isAudioFile(const std::string& path);

    // Main thread.
    void setOutput(WolfDJChannel* ch);          // the channel the playlist plays on; null = none
    WolfDJChannel* output() const { return mOutput.load(); }
    void setFiles(const std::vector<std::string>& files);
    std::vector<std::string> files() const;
    // The playlist saved last time (setting WolfDJPlaylist), once per session, so the desk's
    // play button works before the playlist window has ever been opened.
    void loadSaved();
    void play(int index);
    void togglePause();
    void stop();
    void next();
    void previous();

    // [WOLF DJ 2026-10-10] Paul: "stop it from not playing". Pressing play while the playlist is
    // on no mixer channel used to sit at 0:00 in silence; now it is put on Music 1 (or the first
    // free music channel) first. False, with the reason, when every music channel is in use.
    bool routeToMixer(std::string& why);

    int current() const { return mCurrent.load(); }
    bool isPlaying() const { return mPlaying.load(); }
    bool isPaused() const { return mPaused.load(); }
    double position() const { return mPosition.load(); }
    double duration() const { return mDuration.load(); }
    // Increments whenever a new track starts (the floater sends its title to the stream then).
    int trackSerial() const { return mTrackSerial.load(); }
    void nowPlaying(std::string& artist, std::string& title) const;
    std::string lastError() const;

    // [WOLF DJ 2026-10-10] The song after this one once it is loaded into memory ("" until then).
    std::string preloadedNext() const;
    // A file's length in seconds, from a scan in the background: < 0 while not known yet.
    double knownDuration(const std::string& path) const;
    int lengthsVersion() const { return mLengthsVersion.load(); }   // changes when a length is learnt

    void shutdown();
    ~WolfDJPlayer();

private:
    WolfDJPlayer();
    void run();
    void preload(int index);                    // any thread: ask the loader for a song
    void loaderLoop();                          // the loader's thread
    void scanLengths();                         // the length scan's thread

    std::thread mThread;
    std::atomic<bool> mRunning{ false };
    bool mLoadedSaved = false;
    std::atomic<WolfDJChannel*> mOutput{ nullptr };
    mutable std::mutex mMutex;                  // mFiles, mArtist, mTitle, mError, mNext*, mLengths
    std::vector<std::string> mFiles;
    std::string mArtist, mTitle, mError;
    std::atomic<int> mCurrent{ -1 };
    std::atomic<int> mRequest{ -1 };            // a track to start (set by the main thread)
    std::atomic<bool> mPlaying{ false };
    std::atomic<bool> mPaused{ false };
    std::atomic<double> mPosition{ 0.0 };
    std::atomic<double> mDuration{ 0.0 };
    std::atomic<int> mTrackSerial{ 0 };

    // The next song, read into memory by a loader thread while this one plays.
    std::thread mLoader;
    std::condition_variable mLoadCv;
    std::string mLoadWant;                      // the song the loader should have ready
    std::string mNextPath;                      // what mNextBytes holds ("" none)
    std::shared_ptr<const std::vector<unsigned char>> mNextBytes;
    std::string mNextLabel;                     // its name for the desk, once loaded
    bool mNextStream = false;                   // it is too big to hold: opened (and streamed) when it plays

    // Song lengths for the playlist window.
    std::thread mScanner;
    std::atomic<bool> mScanning{ false };
    std::atomic<bool> mScanStop{ false };
    std::map<std::string, double> mLengths;
    std::atomic<int> mLengthsVersion{ 0 };
};

// [WOLF DJ 2026-10-05] Jingle pads. Paul: "a jingle set of buttons in different colours you can
// apply files to in the playlist folder". Each pad holds one audio file (set from the playlist
// window, saved in WolfDJJingles); pressing it plays the file into the mixer's jingle channel
// (WolfDJMixer::jingleChannel) over whatever else is on air. One at a time: any press starts
// that pad from the beginning (Paul 10-05: "clicking a jingle button only works once" - a
// second press used to stop it); stop() ends it.
class WolfDJJingles
{
public:
    static constexpr int PAD_COUNT = 8;
    static const float PAD_RGB[PAD_COUNT][3];   // each pad's colour, the same in both windows
    static WolfDJJingles& instance();

    std::string pad(int pad) const;                     // the file, "" for an empty pad
    static std::string padLabel(const std::string& path); // the file's name, for a button
    void setPad(int pad, const std::string& path);      // main thread; "" clears; saved
    void trigger(int pad);                              // main thread: play it from the start
    void stop();
    int playing() const { return mPlaying.load(); }     // the pad playing, -1 none
    // The last file that would not play, once: "" when there is nothing new to report.
    std::string takeError();
    void shutdown();
    ~WolfDJJingles();

private:
    WolfDJJingles();
    void run();

    std::thread mThread;
    std::atomic<bool> mRunning{ false };
    std::atomic<int> mRequest{ -1 };            // a pad to start, -2 stop, -1 nothing
    std::atomic<int> mPlaying{ -1 };
    mutable std::mutex mMutex;                  // mPads, mError
    std::vector<std::string> mPads;
    std::string mError;
};

class WolfDJCaptureStream;
// The playlist as a mixer source (WolfFloaterDJ: the "Wolf DJ playlist" entry of a music channel).
std::unique_ptr<WolfDJCaptureStream> wolfdj_open_playlist_stream(WolfDJChannel* ch);

#endif // WOLF_DJPLAYER_H
