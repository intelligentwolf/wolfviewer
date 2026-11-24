/**
 * @file llwebrtc_opus.cpp
 * @brief [W111 VIDEO 2026-11-24] The video recorder's Opus encoder: libopus (inside libwebrtc) behind the plain
 *        LLWebRTCOpusEncoder interface of llwebrtc.h.
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

#include "llwebrtc.h"

// libopus as packaged with libwebrtc (packages/include/webrtc is on ll::webrtc's include path; the same header
// and the opus_encoder_* symbols are in the linux64, windows64 and darwin64 webrtc m144.7559.06.20 packages).
#include "third_party/opus/src/include/opus.h"

namespace llwebrtc
{
namespace
{
    class OpusEncoderImpl : public LLWebRTCOpusEncoder
    {
      public:
        explicit OpusEncoderImpl(OpusEncoder* enc) : mEnc(enc)
        {
            // OPUS_GET_LOOKAHEAD (opus_defines.h:481-494): samples of delay at the encoder's rate, to be skipped from
            // the decoder's output - "Applications needing delay compensation should call this CTL".
            opus_int32 la = 0;
            if (opus_encoder_ctl(mEnc, OPUS_GET_LOOKAHEAD(&la)) == OPUS_OK) mLookahead = (int)la;
        }
        ~OpusEncoderImpl() override { opus_encoder_destroy(mEnc); }   // opus.h:351

        int encode(const float* interleaved, int frames, unsigned char* out, int max_bytes) override
        {
            if (!interleaved || !out || frames <= 0 || max_bytes <= 0) return OPUS_BAD_ARG;
            // opus.h:340 opus_encode_float: returns the packet length in bytes or a negative error code
            return (int)opus_encode_float(mEnc, interleaved, frames, out, (opus_int32)max_bytes);
        }
        int lookahead() const override { return mLookahead; }

      private:
        OpusEncoder* mEnc;
        int mLookahead = -1;   // -1 when the CTL failed: the caller refuses the encoder (no pre-skip, no sync)
    };
}

LLWebRTCOpusEncoder* newOpusEncoder(int sample_rate, int channels, int bitrate_bps, int* error_code)
{
    int err = OPUS_OK;
    // opus.h:208 opus_encoder_create(Fs, channels, application, error); OPUS_APPLICATION_AUDIO = music/mixed sound
    // (opus_defines.h:213), what a recording of the world (sounds, music, voice together) is.
    OpusEncoder* enc = opus_encoder_create((opus_int32)sample_rate, channels, OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK)
    {
        if (enc) opus_encoder_destroy(enc);
        if (error_code) *error_code = err != OPUS_OK ? err : OPUS_ALLOC_FAIL;
        return nullptr;
    }
    // OPUS_SET_BITRATE (opus_defines.h:289): bits per second; VBR stays on (the default)
    err = opus_encoder_ctl(enc, OPUS_SET_BITRATE((opus_int32)bitrate_bps));
    if (err != OPUS_OK)
    {
        opus_encoder_destroy(enc);
        if (error_code) *error_code = err;
        return nullptr;
    }
    OpusEncoderImpl* impl = new OpusEncoderImpl(enc);
    if (impl->lookahead() < 0)
    {
        delete impl;
        if (error_code) *error_code = OPUS_INTERNAL_ERROR;
        return nullptr;
    }
    if (error_code) *error_code = OPUS_OK;
    return impl;
}

void freeOpusEncoder(LLWebRTCOpusEncoder* encoder)
{
    delete static_cast<OpusEncoderImpl*>(encoder);
}

const char* opusErrorText(int error_code)
{
    return opus_strerror(error_code);   // opus_defines.h:813, static strings
}

}  // namespace llwebrtc
