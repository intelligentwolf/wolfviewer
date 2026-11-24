/**
 * @file wolfrec_audioenc_at.mm
 * @brief [W111 VIDEO 2026-11-24] The recorder's AAC-LC encoder on macOS: Apple's AudioToolbox AudioConverter.
 *        macOS only; plain C APIs, compiled as Objective-C++ like the viewer's other macOS files.
 *        See wolfrec_audioenc.h.
 *
 * Apple documentation followed (https://developer.apple.com/documentation/audiotoolbox/...):
 *   audioconverternew(_:_:_:), audioconverterfillcomplexbuffer(_:_:_:_:_:_:),
 *   audioconvertercomplexinputdataproc, audioconvertersetproperty(_:_:_:_:), audioconvertergetproperty(_:_:_:_:),
 *   kaudioconverterprimeinfo, audioconverterprimeinfo, kaudioconverterencodebitrate,
 *   kaudioconvertercompressionmagiccookie, kaudioconverterpropertymaximumoutputpacketsize,
 *   kaudioconvertercurrentoutputstreamdescription, audioconverterdispose(_:)
 *   Technical Note TN2258 "AAC Audio - Encoder Delay and Synchronization" (the encoder's priming is the leading
 *   frames to trim; Apple's AAC encoder primes 2112 samples).
 * FFmpeg's libavcodec/audiotoolboxenc.c (LGPL) drives the same converter the same way and was read as a cross-check:
 * its priming is AudioConverterPrimeInfo.leadingFrames (avctx->initial_padding), as here.
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

#include "wolfrec_audioenc.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wolfrec
{
namespace
{
    // The input proc's "no more input just now" answer: any non-zero status stops AudioConverterFillComplexBuffer,
    // which returns it with the packets made so far (audioconvertercomplexinputdataproc; FFmpeg returns 1 the same way)
    constexpr OSStatus kNeedMore = 1;

    std::string osText(const char* what, OSStatus s)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s failed (OSStatus %d)", what, (int)s);
        return buf;
    }

    // ISO/IEC 14496-1 7.2.6.5/7.2.6.6/7.2.6.7: the magic cookie is an ES_Descriptor (tag 0x03) holding a
    // DecoderConfigDescriptor (0x04) holding the DecoderSpecificInfo (0x05) = the AudioSpecificConfig. Sizes in the
    // 8.3.3 expandable form (7 bits a byte, top bit = more).
    bool ascFromCookie(const std::vector<uint8_t>& c, std::vector<uint8_t>& asc)
    {
        size_t i = 0;
        auto readLen = [&](size_t& len) -> bool {
            len = 0;
            for (int k = 0; k < 4; ++k)
            {
                if (i >= c.size()) return false;
                const uint8_t b = c[i++];
                len = (len << 7) | (b & 0x7F);
                if (!(b & 0x80)) return true;
            }
            return true;
        };
        while (i < c.size())
        {
            const uint8_t tag = c[i++];
            size_t len = 0;
            if (!readLen(len)) return false;
            if (tag == 0x03)
            {
                // ES_ID(16), flags(8): streamDependenceFlag 0x80 (+16 bits), URL_Flag 0x40 (+length + URL),
                // OCRstreamFlag 0x20 (+16 bits)
                if (i + 3 > c.size()) return false;
                const uint8_t flags = c[i + 2];
                i += 3;
                if (flags & 0x80) i += 2;
                if (flags & 0x40) { if (i >= c.size()) return false; i += 1 + c[i]; }
                if (flags & 0x20) i += 2;
            }
            else if (tag == 0x04)
            {
                i += 13;   // objectTypeIndication(8), streamType/upStream/reserved(8), bufferSizeDB(24), max(32), avg(32)
            }
            else if (tag == 0x05)
            {
                if (i + len > c.size() || len == 0) return false;
                asc.assign(c.begin() + (std::ptrdiff_t)i, c.begin() + (std::ptrdiff_t)(i + len));
                return true;
            }
            else
            {
                i += len;
            }
        }
        return false;
    }

    class AtAacEncoder : public AudioEncoder
    {
    public:
        ~AtAacEncoder() override
        {
            if (mConv) AudioConverterDispose(mConv);
        }

        bool open(uint32_t rate, uint32_t ch, uint32_t bitrate, std::string& error)
        {
            if (ch < 1 || ch > 2) { error = "AudioToolbox AAC: mono or stereo only"; return false; }
            mRate = rate; mCh = ch;
            // input: interleaved 32-bit float, packed (one frame per packet)
            AudioStreamBasicDescription in = {};
            in.mSampleRate = rate;
            in.mFormatID = kAudioFormatLinearPCM;
            in.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            in.mBytesPerPacket = 4 * ch;
            in.mFramesPerPacket = 1;
            in.mBytesPerFrame = 4 * ch;
            in.mChannelsPerFrame = ch;
            in.mBitsPerChannel = 32;
            // output: MPEG-4 AAC (AAC-LC), the converter fills in the rest
            AudioStreamBasicDescription out = {};
            out.mSampleRate = rate;
            out.mFormatID = kAudioFormatMPEG4AAC;
            out.mChannelsPerFrame = ch;
            OSStatus s = AudioConverterNew(&in, &out, &mConv);
            if (s != noErr || !mConv) { mConv = nullptr; error = "AudioToolbox AAC: " + osText("AudioConverterNew", s); return false; }

            UInt32 br = bitrate;
            s = AudioConverterSetProperty(mConv, kAudioConverterEncodeBitRate, sizeof(br), &br);
            if (s != noErr) { error = "AudioToolbox AAC: " + osText("kAudioConverterEncodeBitRate", s); return false; }
            UInt32 size = sizeof(br);
            if (AudioConverterGetProperty(mConv, kAudioConverterEncodeBitRate, &size, &br) == noErr) mBitrate = br;
            else mBitrate = bitrate;

            // the frames per packet the converter chose (1024 for AAC-LC)
            AudioStreamBasicDescription cur = {};
            size = sizeof(cur);
            s = AudioConverterGetProperty(mConv, kAudioConverterCurrentOutputStreamDescription, &size, &cur);
            if (s != noErr) { error = "AudioToolbox AAC: " + osText("CurrentOutputStreamDescription", s); return false; }
            if (cur.mFramesPerPacket != 1024)
            {
                error = "AudioToolbox AAC: " + std::to_string(cur.mFramesPerPacket) + " frames per packet (1024 expected)";
                return false;
            }
            size = sizeof(mMaxPacket);
            s = AudioConverterGetProperty(mConv, kAudioConverterPropertyMaximumOutputPacketSize, &size, &mMaxPacket);
            if (s != noErr || mMaxPacket == 0) { error = "AudioToolbox AAC: " + osText("MaximumOutputPacketSize", s); return false; }

            // TN2258 / FFmpeg: the encoder's priming = AudioConverterPrimeInfo.leadingFrames. Zero is refused (the
            // A/V sync would be off by the real delay), and the recorder falls back to Opus.
            AudioConverterPrimeInfo prime = {};
            size = sizeof(prime);
            s = AudioConverterGetProperty(mConv, kAudioConverterPrimeInfo, &size, &prime);
            if (s != noErr || prime.leadingFrames == 0)
            {
                error = "AudioToolbox AAC: the encoder did not report its priming (" + osText("kAudioConverterPrimeInfo", s) + ")";
                return false;
            }
            mPriming = prime.leadingFrames;

            // AudioSpecificConfig from the magic cookie (an ES_Descriptor)
            UInt32 csize = 0;
            s = AudioConverterGetPropertyInfo(mConv, kAudioConverterCompressionMagicCookie, &csize, nullptr);
            std::vector<uint8_t> cookie(csize);
            if (s == noErr && csize)
            {
                s = AudioConverterGetProperty(mConv, kAudioConverterCompressionMagicCookie, &csize, cookie.data());
                cookie.resize(csize);
            }
            if (s != noErr || cookie.empty() || !ascFromCookie(cookie, mAsc))
            {
                // no usable cookie: the AudioSpecificConfig of AAC-LC at this rate (ISO/IEC 14496-3 1.6.2.1)
                mAsc = aacLcAudioSpecificConfig(rate, ch);
                if (mAsc.empty()) { error = "AudioToolbox AAC: no AudioSpecificConfig for " + std::to_string(rate) + " Hz"; return false; }
            }
            return true;
        }

        bool encode(const float* in, uint32_t frames, std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) { error = "AudioToolbox AAC: encode after flush"; return false; }
            mInput += frames;
            mFifo.insert(mFifo.end(), in, in + (size_t)frames * mCh);
            return pull(out, error);
        }

        bool flush(std::vector<AudioPacket>& out, std::string& error) override
        {
            if (mFlushed) return true;
            mFlushed = true;
            mEof = true;   // the input proc now answers "end of stream" when the queue is empty: the converter
                           // codes what it holds (its trailing frames) and stops
            return pull(out, error);
        }

        std::string codec() const override { return "aac"; }
        std::string name() const override { return "AudioToolbox AAC"; }
        std::vector<uint8_t> decoderConfig() const override { return mAsc; }
        uint32_t primingFrames() const override { return mPriming; }
        uint64_t inputFrames() const override { return mInput; }
        uint32_t sampleRate() const override { return mRate; }
        uint32_t channels() const override { return mCh; }
        uint32_t bitrate() const override { return mBitrate; }

    private:
        static OSStatus inputProc(AudioConverterRef, UInt32* io_packets, AudioBufferList* data,
                                  AudioStreamPacketDescription** desc, void* user)
        {
            AtAacEncoder* self = static_cast<AtAacEncoder*>(user);
            if (desc) *desc = nullptr;   // linear PCM input has no packet descriptions
            const size_t have = self->mFifo.size() / self->mCh;
            if (have == 0)
            {
                *io_packets = 0;
                data->mBuffers[0].mData = nullptr;
                data->mBuffers[0].mDataByteSize = 0;
                return self->mEof ? noErr : kNeedMore;   // 0 packets + noErr = end of stream
            }
            // The converter may use the buffer until it calls again: hand over the queue, kept in mGiven
            const size_t n = std::min<size_t>(have, *io_packets);
            self->mGiven.assign(self->mFifo.begin(), self->mFifo.begin() + (std::ptrdiff_t)(n * self->mCh));
            self->mFifo.erase(self->mFifo.begin(), self->mFifo.begin() + (std::ptrdiff_t)(n * self->mCh));
            data->mNumberBuffers = 1;
            data->mBuffers[0].mNumberChannels = self->mCh;
            data->mBuffers[0].mData = self->mGiven.data();
            data->mBuffers[0].mDataByteSize = (UInt32)(n * self->mCh * sizeof(float));
            *io_packets = (UInt32)n;
            return noErr;
        }

        bool pull(std::vector<AudioPacket>& out, std::string& error)
        {
            for (;;)
            {
                // enough queued for a packet, or the end: ask for one packet
                if (!mEof && mFifo.size() / mCh < 1024) return true;
                std::vector<uint8_t> buf(mMaxPacket);
                AudioBufferList abl;
                abl.mNumberBuffers = 1;
                abl.mBuffers[0].mNumberChannels = mCh;
                abl.mBuffers[0].mDataByteSize = mMaxPacket;
                abl.mBuffers[0].mData = buf.data();
                AudioStreamPacketDescription pd = {};
                UInt32 packets = 1;
                const OSStatus s = AudioConverterFillComplexBuffer(mConv, &AtAacEncoder::inputProc, this, &packets,
                                                                   &abl, &pd);
                if (s != noErr && s != kNeedMore) { error = "AudioToolbox AAC: " + osText("AudioConverterFillComplexBuffer", s); return false; }
                if (packets == 0) return true;   // needs more input, or (at the end) nothing more to code
                AudioPacket p;
                const UInt32 bytes = pd.mDataByteSize ? pd.mDataByteSize : abl.mBuffers[0].mDataByteSize;
                const UInt32 start = (UInt32)pd.mStartOffset;
                if ((size_t)start + bytes > buf.size()) { error = "AudioToolbox AAC: packet outside its buffer"; return false; }
                p.data.assign(buf.begin() + start, buf.begin() + start + bytes);
                p.frames = pd.mVariableFramesInPacket ? pd.mVariableFramesInPacket : 1024;
                out.push_back(std::move(p));
                if (s == kNeedMore && !mEof) return true;
            }
        }

        AudioConverterRef mConv = nullptr;
        std::vector<float> mFifo, mGiven;
        std::vector<uint8_t> mAsc;
        UInt32 mMaxPacket = 0;
        uint32_t mRate = 48000, mCh = 2, mBitrate = 0, mPriming = 0;
        uint64_t mInput = 0;
        bool mFlushed = false, mEof = false;
    };
}

std::unique_ptr<AudioEncoder> openAudioToolboxAac(uint32_t sample_rate, uint32_t channels, uint32_t bitrate_bps,
                                                  std::string& error)
{
    auto enc = std::make_unique<AtAacEncoder>();
    if (!enc->open(sample_rate, channels, bitrate_bps, error)) return nullptr;
    return enc;
}
}
