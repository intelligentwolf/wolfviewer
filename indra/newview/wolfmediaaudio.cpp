/**
 * @file wolfmediaaudio.cpp
 * @brief [W111 VIDEO 2026-11-24] Media plugin sound played in the viewer's audio engine.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "wolfmediaaudio.h"

#include "llaudioengine.h"
#include "llpluginclassmedia.h"
#include "llviewercontrol.h"
#include "wolfpluginaudio.h"

// [W111 VIDEO 2026-11-24] Ticket 1033 (video recorder), Paul's decision "B": parcel music, sound and video on prims and
// web pages play through the viewer's OpenAL engine rather than each media plugin's own audio output, so they are in
// the engine's mix (the recorder takes that mix: llaudioengine_openal.h setMirrorMix / setMixTap).
//   plugin (libVLC / GStreamer / CEF callbacks) -> MediaPluginBase::wolfAudioPush -> shared-memory ring
//   (llplugin/wolfpluginaudio.h) -> this feed -> LLAudioEngine PCM stream (llaudio/llaudiopcm_openal.h, its own
//   feeder thread) -> an OpenAL source in the engine's context.
// The volume each medium is played at is still worked out where it always was (LLViewerMediaImpl::updateVolume,
// LLStreamingAudio_MediaPlugins::setGain, audio_update_volume) and reaches LLPluginClassMedia::setVolume, which now
// hands it to the engine instead of the plugin applying it.
namespace
{
    class RingFeed : public LLAudioPCMFeed
    {
    public:
        explicit RingFeed(const std::shared_ptr<WolfPluginAudioRing>& ring) : mRing(ring) {}

        // The engine's feeder thread lets go of the feed when the stream ends.
        ~RingFeed() override
        {
            if (const U32 dropped = mRing->takeDropped())
            {
                LL_WARNS("MediaSound") << "A media plugin's sound ring overflowed: " << dropped
                                       << " frames were not heard" << LL_ENDL;
            }
        }

        bool read(Chunk& out) override
        {
            if (!mRing->read(mChunk))
            {
                return false;
            }
            static_assert((U32)LLAudioPCMFeed::KIND_DATA == (U32)WolfPluginAudio::KIND_DATA
                          && (U32)LLAudioPCMFeed::KIND_FLUSH == (U32)WolfPluginAudio::KIND_FLUSH
                          && (U32)LLAudioPCMFeed::KIND_PAUSE == (U32)WolfPluginAudio::KIND_PAUSE
                          && (U32)LLAudioPCMFeed::KIND_RESUME == (U32)WolfPluginAudio::KIND_RESUME,
                          "the two kind lists must match");
            static_assert(WolfPluginAudio::CHANNELS == 2, "LLAudioPCMFeed chunks are stereo");
            out.mKind = mChunk.mKind;
            out.mRate = mChunk.mRate;
            out.mDueUs = mChunk.mDueUs;
            out.mSamples.swap(mChunk.mSamples);     // no copy; mChunk gets the old vector back for the next read
            return true;
        }

        S64 nowUs() const override
        {
            return WolfPluginAudio::nowUs();
        }

    private:
        std::shared_ptr<WolfPluginAudioRing> mRing;
        WolfPluginAudio::Chunk               mChunk;
    };

    class Host : public LLPluginClassMediaAudioHost
    {
    public:
        bool wantAudioThroughViewer() override
        {
            static LLCachedControl<bool> through_viewer(gSavedSettings, "WolfMediaAudioThroughViewer", true);
            return through_viewer() && gAudiop && gAudiop->supportsPCMStreams();
        }

        U32 openAudio(const std::shared_ptr<WolfPluginAudioRing>& ring) override
        {
            return gAudiop && ring ? gAudiop->createPCMStream(std::make_shared<RingFeed>(ring)) : 0;
        }

        void setAudioGain(U32 handle, F32 gain) override
        {
            if (gAudiop)
            {
                gAudiop->setPCMStreamGain(handle, gain);
            }
        }

        void closeAudio(U32 handle) override
        {
            if (gAudiop)
            {
                gAudiop->destroyPCMStream(handle);
            }
        }
    };
}

void WolfMediaAudio::registerHost()
{
    static Host host;
    LLPluginClassMedia::setAudioHost(&host);
}
