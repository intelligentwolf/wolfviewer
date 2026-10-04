/**
 * @file wolfdjcapture_mac.mm
 * @brief WolfViewer Wolf DJ capture on macOS: the microphone through an AudioQueue input
 *        (AudioToolbox), other programs' sound through ScreenCaptureKit (macOS 13 or later).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfdjcapture.h"
#include "wolfdjaudio.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <vector>

// [WOLF DJ 2026-10-04] ScreenCaptureKit (developer.apple.com): SCShareableContent
// getShareableContentExcludingDesktopWindows:onScreenWindowsOnly:completionHandler: (displays,
// applications - SCRunningApplication applicationName/bundleIdentifier/processID);
// SCContentFilter initWithDisplay:includingApplications:exceptingWindows: (one program) or
// initWithDisplay:excludingWindows: (everything); SCStreamConfiguration capturesAudio, sampleRate,
// channelCount, excludesCurrentProcessAudio (the viewer's own voice and sounds are never taken
// twice), width/height/minimumFrameInterval kept tiny because only the audio is wanted;
// SCStream addStreamOutput:type:SCStreamOutputTypeAudio. Guarded for macOS 13 (audio capture);
// the framework is weak-linked (CMakeLists.txt). The first use asks for Screen & System Audio
// Recording permission. This file is manual retain/release, like the viewer's other .mm files.

API_AVAILABLE(macos(13.0))
@interface WolfDJSCKOutput : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    WolfDJChannel* mChannel;
    std::atomic<bool>* mFailed;
}
@end

@implementation WolfDJSCKOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sb)) return;
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
    const AudioStreamBasicDescription* asbd = fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : NULL;
    if (!asbd || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel != 32) return;
    const CMItemCount frames = CMSampleBufferGetNumSamples(sb);
    if (frames <= 0) return;
    struct
    {
        AudioBufferList list;
        AudioBuffer extra[7];
    } abl;
    CMBlockBufferRef block = NULL;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, NULL, &abl.list, sizeof(abl), NULL, NULL,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr)
    {
        return;
    }
    const unsigned rate = (unsigned)asbd->mSampleRate;
    if (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)
    {
        const UInt32 chans = abl.list.mNumberBuffers;
        std::vector<float> inter((size_t)frames * chans);
        for (UInt32 c = 0; c < chans; ++c)
        {
            const float* src = (const float*)abl.list.mBuffers[c].mData;
            if (!src) continue;
            for (CMItemCount i = 0; i < frames; ++i) inter[(size_t)i * chans + c] = src[i];
        }
        mChannel->pushFloat(inter.data(), (size_t)frames, chans, rate);
    }
    else if (abl.list.mNumberBuffers > 0 && abl.list.mBuffers[0].mData)
    {
        mChannel->pushFloat((const float*)abl.list.mBuffers[0].mData, (size_t)frames, asbd->mChannelsPerFrame, rate);
    }
    if (block) CFRelease(block);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    if (mFailed) *mFailed = true;
}

@end

namespace
{
    // Waits for an asynchronous ScreenCaptureKit call (its handlers run off the main thread).
    bool wait_for(dispatch_semaphore_t sem, int seconds)
    {
        return dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)seconds * NSEC_PER_SEC)) == 0;
    }

    API_AVAILABLE(macos(13.0))
    SCShareableContent* shareable_content(std::string& err)
    {
        __block SCShareableContent* result = nil;
        __block NSString* why = nil;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
            completionHandler:^(SCShareableContent* content, NSError* error) {
                result = [content retain];
                if (error) why = [[error localizedDescription] retain];
                dispatch_semaphore_signal(sem);
            }];
        const bool done = wait_for(sem, 5);
        dispatch_release(sem);
        if (!done || !result)
        {
            err = "macOS did not allow capturing other programs. Allow WolfViewer in System Settings > Privacy & Security > Screen & System Audio Recording, then try again.";
            if (why)
            {
                err += std::string(" (") + [why UTF8String] + ")";
                [why release];
            }
            return nil;
        }
        if (why) [why release];
        return [result autorelease];
    }

    class SCKStream : public WolfDJCaptureStream
    {
    public:
        SCKStream(WolfDJChannel* ch) : mChannel(ch) {}

        ~SCKStream() override
        {
            if (@available(macOS 13.0, *))
            {
                if (mStream)
                {
                    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                    [(SCStream*)mStream stopCaptureWithCompletionHandler:^(NSError* error) { dispatch_semaphore_signal(sem); }];
                    wait_for(sem, 3);
                    dispatch_release(sem);
                    [(SCStream*)mStream release];
                }
                if (mOutput) [(WolfDJSCKOutput*)mOutput release];
                if (mQueue) dispatch_release(mQueue);
            }
            mChannel->clear();
        }

        bool ok() const override { return !mFailed; }
        std::string error() const override { return mFailed ? std::string("The sound source stopped.") : std::string(); }

        // pid 0 = everything you hear.
        bool start(pid_t pid, std::string& err) API_AVAILABLE(macos(13.0))
        {
            @autoreleasepool
            {
                SCShareableContent* content = shareable_content(err);
                if (!content) return false;
                SCDisplay* display = content.displays.firstObject;
                if (!display)
                {
                    err = "No display to capture from.";
                    return false;
                }
                SCContentFilter* filter = nil;
                if (pid == 0)
                {
                    filter = [[[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]] autorelease];
                }
                else
                {
                    SCRunningApplication* app = nil;
                    for (SCRunningApplication* a in content.applications)
                    {
                        if (a.processID == pid) { app = a; break; }
                    }
                    if (!app)
                    {
                        err = "That program is no longer running.";
                        return false;
                    }
                    filter = [[[SCContentFilter alloc] initWithDisplay:display includingApplications:@[app] exceptingWindows:@[]] autorelease];
                }
                SCStreamConfiguration* cfg = [[[SCStreamConfiguration alloc] init] autorelease];
                cfg.capturesAudio = YES;
                cfg.sampleRate = WolfDJ::RATE;
                cfg.channelCount = 2;
                cfg.excludesCurrentProcessAudio = YES;
                cfg.width = 2;
                cfg.height = 2;
                cfg.minimumFrameInterval = CMTimeMake(1, 1);

                WolfDJSCKOutput* out = [[WolfDJSCKOutput alloc] init];
                out->mChannel = mChannel;
                out->mFailed = &mFailed;
                mOutput = out;
                SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:out];
                mStream = stream;
                mQueue = dispatch_queue_create("WolfDJ.capture", DISPATCH_QUEUE_SERIAL);
                NSError* add_err = nil;
                if (![stream addStreamOutput:out type:SCStreamOutputTypeAudio sampleHandlerQueue:mQueue error:&add_err])
                {
                    err = "Could not capture that program's sound.";
                    return false;
                }
                __block NSString* why = nil;
                dispatch_semaphore_t sem = dispatch_semaphore_create(0);
                [stream startCaptureWithCompletionHandler:^(NSError* error) {
                    if (error) why = [[error localizedDescription] retain];
                    dispatch_semaphore_signal(sem);
                }];
                const bool done = wait_for(sem, 5);
                dispatch_release(sem);
                if (!done || why)
                {
                    err = "macOS would not start the capture. Allow WolfViewer in System Settings > Privacy & Security > Screen & System Audio Recording.";
                    if (why)
                    {
                        err += std::string(" (") + [why UTF8String] + ")";
                        [why release];
                    }
                    return false;
                }
                return true;
            }
        }

    private:
        WolfDJChannel* mChannel;
        void* mStream = nullptr;    // SCStream*, retained
        void* mOutput = nullptr;    // WolfDJSCKOutput*, retained
        dispatch_queue_t mQueue = nullptr;
        std::atomic<bool> mFailed{ false };
    };

    // ---- Microphone: AudioQueue input, float32 interleaved stereo at 48 kHz ----
    class MicStream : public WolfDJCaptureStream
    {
    public:
        MicStream(WolfDJChannel* ch) : mChannel(ch) {}

        ~MicStream() override
        {
            if (mQueue)
            {
                AudioQueueStop(mQueue, true);
                AudioQueueDispose(mQueue, true);
            }
            mChannel->clear();
        }

        bool ok() const override { return !mFailed; }
        std::string error() const override { return mFailed ? std::string("The microphone stopped.") : std::string(); }

        bool start(std::string& err)
        {
            AudioStreamBasicDescription fmt = {};
            fmt.mSampleRate = WolfDJ::RATE;
            fmt.mFormatID = kAudioFormatLinearPCM;
            fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            fmt.mChannelsPerFrame = 2;
            fmt.mBitsPerChannel = 32;
            fmt.mBytesPerFrame = 8;
            fmt.mFramesPerPacket = 1;
            fmt.mBytesPerPacket = 8;
            if (AudioQueueNewInput(&fmt, &MicStream::onInput, this, NULL, NULL, 0, &mQueue) != noErr)
            {
                mQueue = NULL;
                err = "Could not open the microphone (check System Settings > Privacy & Security > Microphone).";
                return false;
            }
            const UInt32 bytes = WolfDJ::RATE / 50 * 8;     // 20 ms
            for (int i = 0; i < 3; ++i)
            {
                AudioQueueBufferRef buf = NULL;
                if (AudioQueueAllocateBuffer(mQueue, bytes, &buf) == noErr)
                {
                    AudioQueueEnqueueBuffer(mQueue, buf, 0, NULL);
                }
            }
            if (AudioQueueStart(mQueue, NULL) != noErr)
            {
                err = "Could not start the microphone.";
                return false;
            }
            return true;
        }

    private:
        static void onInput(void* ud, AudioQueueRef q, AudioQueueBufferRef buf, const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription*)
        {
            MicStream* self = static_cast<MicStream*>(ud);
            if (buf->mAudioDataByteSize > 0)
            {
                self->mChannel->pushFloat((const float*)buf->mAudioData, buf->mAudioDataByteSize / 8, 2, WolfDJ::RATE);
            }
            AudioQueueEnqueueBuffer(q, buf, 0, NULL);
        }

        WolfDJChannel* mChannel;
        AudioQueueRef mQueue = NULL;
        std::atomic<bool> mFailed{ false };
    };
}

bool WolfDJCapture::listApps(std::vector<WolfDJApp>& apps, std::string& why)
{
    apps.clear();
    if (@available(macOS 13.0, *))
    {
        apps.push_back({ WOLFDJ_EVERYTHING_ID, "Everything you hear" });
        @autoreleasepool
        {
            std::string err;
            SCShareableContent* content = shareable_content(err);
            if (!content)
            {
                why = err;
                return true;
            }
            const pid_t self = getpid();
            NSArray* sorted = [content.applications sortedArrayUsingComparator:^NSComparisonResult(SCRunningApplication* a, SCRunningApplication* b) {
                return [a.applicationName localizedCaseInsensitiveCompare:b.applicationName];
            }];
            for (SCRunningApplication* a in sorted)
            {
                if (a.processID == self || a.applicationName.length == 0) continue;
                apps.push_back({ "pid:" + std::to_string((int)a.processID), [a.applicationName UTF8String] });
            }
        }
        return true;
    }
    why = "Capturing other programs needs macOS 13 or later. The microphone and the playlist still work.";
    return false;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openMic(WolfDJChannel* ch, std::string& err)
{
    auto s = std::make_unique<MicStream>(ch);
    if (!s->start(err)) return nullptr;
    return s;
}

std::unique_ptr<WolfDJCaptureStream> WolfDJCapture::openApp(const std::string& id, WolfDJChannel* ch, std::string& err)
{
    if (@available(macOS 13.0, *))
    {
        int pid = 0;
        if (id != WOLFDJ_EVERYTHING_ID && (sscanf(id.c_str(), "pid:%d", &pid) != 1 || pid <= 0))
        {
            err = "Unknown program.";
            return nullptr;
        }
        auto s = std::make_unique<SCKStream>(ch);
        if (!s->start((pid_t)pid, err)) return nullptr;
        return s;
    }
    err = "Capturing other programs needs macOS 13 or later.";
    return nullptr;
}
