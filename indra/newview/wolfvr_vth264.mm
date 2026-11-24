/**
 * @file wolfvr_vth264.mm
 * @brief [W111 VIDEO 2026-11-24] macOS's own H.264 encoder (VideoToolbox VTCompressionSession) as a wolfvr::Encoder,
 *        hardware or software. macOS only; plain C APIs (no Objective-C objects), compiled as Objective-C++ like
 *        the viewer's other macOS files. See wolfvr_osh264.h.
 *
 * Apple documentation followed (every call below names its page under https://developer.apple.com/documentation/):
 *   videotoolbox/vtcompressionsessioncreate(allocator:width:height:codectype:encoderspecification:imagebufferattributes:compresseddataallocator:outputcallback:refcon:compressionsessionout:)
 *   videotoolbox/vtcompressionsessionencodeframe(_:imagebuffer:presentationtimestamp:duration:frameproperties:sourceframerefcon:infoflagsout:)
 *   videotoolbox/vtcompressionsessioncompleteframes(_:untilpresentationtimestamp:)
 *   videotoolbox/vtcompressionoutputcallback
 *   videotoolbox/vtsessionsetproperty(_:key:value:)
 *   videotoolbox/kvtvideoencoderspecification_requirehardwareacceleratedvideoencoder
 *   videotoolbox/kvtvideoencoderspecification_enablehardwareacceleratedvideoencoder
 *   videotoolbox/kvtcompressionpropertykey_allowframereordering, _realtime, _profilelevel, _averagebitrate,
 *     _maxkeyframeinterval, _expectedframerate, _colorprimaries, _transferfunction, _ycbcrmatrix
 *   videotoolbox/kvtencodeframeoptionkey_forcekeyframe
 *   coremedia/cmvideoformatdescriptiongeth264parametersetatindex(_:parametersetindex:parametersetpointerout:parametersetsizeout:parametersetcountout:nalunitheaderlengthout:)
 *   coremedia/cmsamplebuffergetsampleattachmentsarray(_:createifnecessary:), coremedia/kcmsampleattachmentkey_notsync
 *   corevideo/cvpixelbufferpoolcreatepixelbuffer(_:_:_:), corevideo/cvpixelbufferlockbaseaddress(_:_:)
 * Every OSStatus / CVReturn is checked.
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

#include "wolfvr_osh264.h"
#include "wolfvr_yuv.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace wolfvr
{
namespace
{
    std::string osText(const char* what, OSStatus s)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s failed (OSStatus %d)", what, (int)s);
        return buf;
    }

    CFNumberRef cfInt(int32_t v)
    {
        return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
    }

    class VtH264Encoder : public Encoder
    {
    public:
        ~VtH264Encoder() override
        {
            if (mSession)
            {
                // VTCompressionSessionInvalidate before the last CFRelease (its page: "call it before releasing")
                VTCompressionSessionInvalidate(mSession);
                CFRelease(mSession);
            }
        }

        bool open(uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, bool hardware, std::string& error)
        {
            mW = w; mH = h; mFps = std::max<uint32_t>(1, fps);
            mName = hardware ? "VideoToolbox H.264 (hardware)" : "VideoToolbox H.264 (software)";
            if (w < 16 || h < 16 || (w & 1) || (h & 1))
            {
                error = mName + ": the picture size must be even and at least 16x16";
                return false;
            }
            // encoder specification: require the hardware encoder, or forbid it (software)
            CFMutableDictionaryRef spec = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (hardware)
            {
                CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                                     kCFBooleanTrue);
            }
            else
            {
                CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
                                     kCFBooleanFalse);
            }
            // source pixel buffers: NV12 video range ("420YpCbCr8BiPlanarVideoRange" = limited range), our size
            CFMutableDictionaryRef src = CFDictionaryCreateMutable(kCFAllocatorDefault, 3,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            CFNumberRef pf = cfInt((int32_t)kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
            CFNumberRef cw = cfInt((int32_t)w), ch = cfInt((int32_t)h);
            CFDictionarySetValue(src, kCVPixelBufferPixelFormatTypeKey, pf);
            CFDictionarySetValue(src, kCVPixelBufferWidthKey, cw);
            CFDictionarySetValue(src, kCVPixelBufferHeightKey, ch);
            CFRelease(pf); CFRelease(cw); CFRelease(ch);

            OSStatus s = VTCompressionSessionCreate(kCFAllocatorDefault, (int32_t)w, (int32_t)h, kCMVideoCodecType_H264,
                                                    spec, src, kCFAllocatorDefault, &VtH264Encoder::outputCallback,
                                                    this, &mSession);
            CFRelease(spec);
            CFRelease(src);
            if (s != noErr || !mSession)
            {
                mSession = nullptr;
                error = mName + ": " + osText("VTCompressionSessionCreate", s);
                return false;
            }
            // Required properties: real time, no frame reordering (no B-frames: one picture in, one out)
            if (!set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, error) ||
                !set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, error))
            {
                return false;
            }
            // Main profile, level chosen by the encoder; falls back to Baseline if refused
            if (!set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel, error) &&
                !set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel, error))
            {
                return false;
            }
            error.clear();
            CFNumberRef br = cfInt((int32_t)std::min<uint64_t>((uint64_t)kbps * 1000, 0x7fffffff));
            CFNumberRef gop = cfInt((int32_t)(mFps * 2));   // an IDR every 2 s, as the other encoders here
            CFNumberRef rate = cfInt((int32_t)mFps);
            const bool ok = set(kVTCompressionPropertyKey_AverageBitRate, br, error) &&
                            set(kVTCompressionPropertyKey_MaxKeyFrameInterval, gop, error) &&
                            set(kVTCompressionPropertyKey_ExpectedFrameRate, rate, error) &&
                            // the colours wolfvr_yuv.h converts with: BT.709 primaries, transfer and matrix; written
                            // into the SPS VUI by the encoder
                            set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, error) &&
                            set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2, error) &&
                            set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, error);
            CFRelease(br); CFRelease(gop); CFRelease(rate);
            if (!ok) return false;
            // VTCompressionSessionPrepareToEncodeFrames: lets the encoder allocate before the first picture
            s = VTCompressionSessionPrepareToEncodeFrames(mSession);
            if (s != noErr) { error = mName + ": " + osText("PrepareToEncodeFrames", s); return false; }
            return true;
        }

        bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) override
        {
            CVPixelBufferRef pb = nullptr;
            // the session's own pool (it exists because source attributes were given at creation)
            CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(mSession);
            CVReturn cr = pool ? CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pb)
                               : CVPixelBufferCreate(kCFAllocatorDefault, mW, mH,
                                                     kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pb);
            if (cr != kCVReturnSuccess || !pb) { error = mName + ": " + osText("CVPixelBuffer create", cr); return false; }
            cr = CVPixelBufferLockBaseAddress(pb, 0);
            if (cr != kCVReturnSuccess) { CVPixelBufferRelease(pb); error = mName + ": " + osText("CVPixelBufferLockBaseAddress", cr); return false; }
            uint8_t* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
            uint8_t* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
            const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
            const size_t cs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
            if (!y || !uv)
            {
                CVPixelBufferUnlockBaseAddress(pb, 0);
                CVPixelBufferRelease(pb);
                error = mName + ": the pixel buffer has no planes";
                return false;
            }
            const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
            rgbaToYuv420(rgba, mW, mH, y, ys, uv, nullptr, cs, true, threads);
            CVPixelBufferUnlockBaseAddress(pb, 0);
            // the same colours on the picture itself (CVBufferSetAttachment, kCVAttachmentMode_ShouldPropagate)
            CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                                  kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                                  kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                                  kCVAttachmentMode_ShouldPropagate);

            CFDictionaryRef props = nullptr;
            if (mForceKey)
            {
                const void* k[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
                const void* v[] = {kCFBooleanTrue};
                props = CFDictionaryCreate(kCFAllocatorDefault, k, v, 1, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
                mForceKey = false;
            }
            {
                std::lock_guard<std::mutex> lk(mMutex);
                mGot = false;
                mError.clear();
                mOut.clear();
            }
            const CMTime pts = CMTimeMake((int64_t)mFrame, (int32_t)mFps);
            ++mFrame;
            VTEncodeInfoFlags flags = 0;
            OSStatus s = VTCompressionSessionEncodeFrame(mSession, pb, pts, CMTimeMake(1, (int32_t)mFps), props,
                                                         nullptr, &flags);
            if (props) CFRelease(props);
            CVPixelBufferRelease(pb);
            if (s != noErr) { error = mName + ": " + osText("VTCompressionSessionEncodeFrame", s); return false; }
            // VTCompressionSessionCompleteFrames(session, kCMTimeInvalid): "forces the session to complete all
            // pending frames" - the output callback has run for this picture when it returns
            s = VTCompressionSessionCompleteFrames(mSession, kCMTimeInvalid);
            if (s != noErr) { error = mName + ": " + osText("VTCompressionSessionCompleteFrames", s); return false; }
            std::lock_guard<std::mutex> lk(mMutex);
            if (!mError.empty()) { error = mError; return false; }
            if (!mGot || mOut.empty()) { error = mName + ": the encoder dropped the picture"; return false; }
            out.data.swap(mOut);
            out.key = mKey;
            return true;
        }

        bool requestKeyFrame() override
        {
            mForceKey = true;   // kVTEncodeFrameOptionKey_ForceKeyFrame on the next picture
            return true;
        }

        bool setBitrate(uint32_t kbps) override
        {
            if (!mSession || !kbps) return false;
            CFNumberRef br = cfInt((int32_t)std::min<uint64_t>((uint64_t)kbps * 1000, 0x7fffffff));
            const OSStatus s = VTSessionSetProperty(mSession, kVTCompressionPropertyKey_AverageBitRate, br);
            CFRelease(br);
            return s == noErr;
        }

        std::string name() const override { return mName; }
        std::string codec() const override { return "h264"; }

    private:
        bool set(CFStringRef key, CFTypeRef value, std::string& error)
        {
            const OSStatus s = VTSessionSetProperty(mSession, key, value);
            if (s != noErr)
            {
                char k[96] = {0};
                CFStringGetCString(key, k, sizeof(k), kCFStringEncodingUTF8);
                error = mName + ": " + osText((std::string("VTSessionSetProperty ") + k).c_str(), s);
                return false;
            }
            return true;
        }

        // VTCompressionOutputCallback: may run on a VideoToolbox thread; CompleteFrames waits for it.
        static void outputCallback(void* refcon, void* /*frame_refcon*/, OSStatus status, VTEncodeInfoFlags info,
                                   CMSampleBufferRef sb)
        {
            VtH264Encoder* self = static_cast<VtH264Encoder*>(refcon);
            std::lock_guard<std::mutex> lk(self->mMutex);
            if (status != noErr) { self->mError = self->mName + ": " + osText("encode", status); return; }
            if (!sb || (info & kVTEncodeInfo_FrameDropped)) return;   // dropped: no output
            self->mGot = true;
            self->convert(sb);
        }

        // AVCC (each NAL unit preceded by its big-endian length, nal_unit_header_length bytes) -> Annex B, with
        // SPS and PPS from the format description in front of every sync (IDR) sample.
        void convert(CMSampleBufferRef sb)
        {
            bool key = true;
            // kCMSampleAttachmentKey_NotSync absent (or false) = a sync sample
            CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
            if (atts && CFArrayGetCount(atts) > 0)
            {
                CFDictionaryRef d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(atts, 0));
                CFBooleanRef notsync = nullptr;
                if (d && CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync,
                                                       reinterpret_cast<const void**>(&notsync)))
                {
                    key = !(notsync && CFBooleanGetValue(notsync));
                }
            }
            CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
            int nal_len = 4;
            static const uint8_t kStart[4] = {0, 0, 0, 1};
            if (fmt)
            {
                size_t count = 0;
                int hl = 0;
                const OSStatus s = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, &hl);
                if (s != noErr) { mError = mName + ": " + osText("GetH264ParameterSetAtIndex", s); return; }
                if (hl == 1 || hl == 2 || hl == 4) nal_len = hl;
                if (key)
                {
                    for (size_t i = 0; i < count; ++i)
                    {
                        const uint8_t* ps = nullptr;
                        size_t psz = 0;
                        const OSStatus s2 = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psz, nullptr, nullptr);
                        if (s2 != noErr || !ps) { mError = mName + ": " + osText("GetH264ParameterSetAtIndex", s2); return; }
                        mOut.insert(mOut.end(), kStart, kStart + 4);
                        mOut.insert(mOut.end(), ps, ps + psz);
                    }
                }
            }
            CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
            const size_t total = bb ? CMBlockBufferGetDataLength(bb) : 0;
            std::vector<uint8_t> avcc(total);
            if (total)
            {
                const OSStatus s = CMBlockBufferCopyDataBytes(bb, 0, total, avcc.data());
                if (s != noErr) { mError = mName + ": " + osText("CMBlockBufferCopyDataBytes", s); return; }
            }
            size_t i = 0;
            while (i + (size_t)nal_len <= total)
            {
                size_t n = 0;
                for (int k = 0; k < nal_len; ++k) n = (n << 8) | avcc[i + (size_t)k];
                i += (size_t)nal_len;
                if (n == 0 || i + n > total) { mError = mName + ": a malformed NAL unit length"; return; }
                mOut.insert(mOut.end(), kStart, kStart + 4);
                mOut.insert(mOut.end(), avcc.begin() + (std::ptrdiff_t)i, avcc.begin() + (std::ptrdiff_t)(i + n));
                i += n;
            }
            mKey = key && avcHasIdr(mOut);
        }

        VTCompressionSessionRef mSession = nullptr;
        std::string mName;
        uint32_t mW = 0, mH = 0, mFps = 30;
        uint64_t mFrame = 0;
        bool mForceKey = false;
        std::mutex mMutex;   // guards the output the callback fills
        std::vector<uint8_t> mOut;
        std::string mError;
        bool mGot = false, mKey = false;
    };
}

std::unique_ptr<Encoder> openVideoToolboxH264(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps,
                                              bool hardware, std::string& error)
{
    auto enc = std::make_unique<VtH264Encoder>();
    if (!enc->open(width, height, fps, bitrate_kbps, hardware, error)) return nullptr;
    return enc;
}
}
