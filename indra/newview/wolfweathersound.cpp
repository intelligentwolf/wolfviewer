/**
 * @file wolfweathersound.cpp
 * @brief WolfViewer: the weather's sound — recorded rain, synthesised wind and thunder.
 *        Mirror of wolfstorm/js/world/weather_sound.js.
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

#include "llviewerprecompiledheaders.h"

#include "wolfweathersound.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "vorbis/vorbisfile.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "lldir.h"
#include "llfile.h"
#include "llrand.h"
#include "llframetimer.h"
#include "llviewercamera.h"
#include "workqueue.h"
#include "wolfweather.h"

namespace
{
    /// The gap between cracks, in seconds — irregular, and rarer for a distant storm.
    // [LIGHTNING 2026-09-13] The strike cadence moved to wolflightning.h (THUNDER_*/DISTANT_*):
    // the bolt is scheduled there and the clap follows it.

    // <WolfViewer 2026-10-26> THE MIX. Source: region-terrain-work/rain-sound/tools/rain_mix.py
    // (firestorm repo), which renders the previews; wolfstorm js/world/weather_sound.js the same.
    constexpr F32 BED_GAIN     = 0.9f;    // the bed at full intensity and volume 100
    constexpr F32 NEAR_GAIN    = 0.55f;   // the near layer, under the bed
    constexpr F32 MUFFLED_GAIN = 0.7f;    // the low-passed copies when under a roof
    constexpr F32 DISTANT_GAIN = 0.6f;    // the "distant" ambience: the muffled bed only
    constexpr F32 DRIP_RATE    = 0.7f;    // drops a second at full intensity
    constexpr F32 MUFFLE_HZ    = 1100.f;  // two cascaded RBJ low-passes
    constexpr S32 MUFFLE_DECIM = 3;       // 44.1 kHz -> 14.7 kHz; the low-pass is the anti-alias
    constexpr F32 TAU_UP       = 1.2f;    // seconds; rain starts softly ...
    constexpr F32 TAU_DOWN     = 1.5f;    // ... and stops softly
    constexpr F32 DRIFT_DB     = 1.5f;    // each loop wanders this far either side
    constexpr F32 DRIFT_TAU    = 3.0f;
    constexpr F32 SILENT       = 0.0005f; // below this a fading layer lets its source go
    // Source: LLAudioEngine::triggerSound (llaudioengine.cpp:808) — a gain under 2*FLT_EPSILON
    // is "do not load silent sounds" and creates no source, so a fade-in starts from this.
    constexpr F32 START_GAIN   = 0.001f;

    std::string soundPath(const char* uuid_str)
    {
        // Source: LLAudioEngine::hasDecodedFile (llaudioengine.cpp:999) and LLAudioData::load
        // (:1967) — both build exactly this path, so this must match them or the engine will
        // never find what we wrote.
        return gDirUtilp->getExpandedFilename(LL_PATH_FS_SOUND_CACHE, std::string(uuid_str)) + ".dsf";
    }

    void put_u32(std::vector<U8>& out, U32 v)
    {
        out.push_back((U8)(v & 0xff));         out.push_back((U8)((v >> 8) & 0xff));
        out.push_back((U8)((v >> 16) & 0xff)); out.push_back((U8)((v >> 24) & 0xff));
    }
    void put_u16(std::vector<U8>& out, U16 v)
    {
        out.push_back((U8)(v & 0xff)); out.push_back((U8)((v >> 8) & 0xff));
    }
    void put_tag(std::vector<U8>& out, const char* tag)
    {
        for (int i = 0; i < 4; ++i) out.push_back((U8)tag[i]);
    }

    /**
     * A 16-bit PCM WAV of interleaved float samples (-1..1). Written to a temporary name and
     * renamed, so a half-written file can never be loaded and a failed write leaves nothing.
     */
    bool write_pcm_wav(const std::string& path, const std::vector<F32>& samples, U16 channels, U32 rate)
    {
        const U32 data_bytes = (U32)samples.size() * 2;
        std::vector<U8> wav;
        wav.reserve(samples.size() * 2 + 64);
        put_tag(wav, "RIFF");  put_u32(wav, 36 + data_bytes);  put_tag(wav, "WAVE");
        put_tag(wav, "fmt ");  put_u32(wav, 16);
        put_u16(wav, 1);                              // PCM
        put_u16(wav, channels);
        put_u32(wav, rate);
        put_u32(wav, rate * channels * 2);            // byte rate
        put_u16(wav, (U16)(channels * 2));            // block align
        put_u16(wav, 16);                             // bits per sample
        put_tag(wav, "data");  put_u32(wav, data_bytes);
        for (F32 f : samples)
        {
            const S32 s = llclamp((S32)llround(f * 32767.f), -32768, 32767);
            put_u16(wav, (U16)(S16)s);
        }
        const std::string tmp = path + ".tmp";
        LLFILE* fp = LLFile::fopen(tmp, "wb");
        if (!fp) return false;
        const size_t wrote = fwrite(wav.data(), 1, wav.size(), fp);
        fclose(fp);
        if (wrote != wav.size())
        {
            LLFile::remove(tmp);
            return false;
        }
        LLFile::remove(path, ENOENT);
        if (LLFile::rename(tmp, path) != 0)
        {
            LLFile::remove(tmp);
            return false;
        }
        return true;
    }

    /** Ogg Vorbis from memory: the whole file is read first, so no FILE* crosses into libvorbis. */
    struct MemReader
    {
        const std::vector<U8>* mData;
        size_t mPos = 0;
    };

    bool decode_ogg(const std::string& path, std::vector<F32>& out, S32& channels, S32& rate)
    {
        std::vector<U8> data;
        {
            LLFILE* fp = LLFile::fopen(path, "rb");
            if (!fp) return false;
            U8 chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) data.insert(data.end(), chunk, chunk + n);
            fclose(fp);
        }
        if (data.empty()) return false;
        MemReader rd{ &data, 0 };
        ov_callbacks cb;
        cb.read_func = [](void* ptr, size_t size, size_t nmemb, void* ud) -> size_t
        {
            MemReader* r = static_cast<MemReader*>(ud);
            if (!size) return 0;
            const size_t want = size * nmemb;
            const size_t left = r->mData->size() - r->mPos;
            const size_t take = want < left ? want : left;
            memcpy(ptr, r->mData->data() + r->mPos, take);
            r->mPos += take;
            return take / size;
        };
        cb.seek_func = [](void* ud, ogg_int64_t off, int whence) -> int
        {
            MemReader* r = static_cast<MemReader*>(ud);
            const long long size = (long long)r->mData->size();
            long long pos = (whence == SEEK_SET) ? off : (whence == SEEK_CUR) ? (long long)r->mPos + off : size + off;
            if (pos < 0 || pos > size) return -1;
            r->mPos = (size_t)pos;
            return 0;
        };
        cb.close_func = nullptr;
        cb.tell_func = [](void* ud) -> long { return (long)static_cast<MemReader*>(ud)->mPos; };
        OggVorbis_File vf;
        if (ov_open_callbacks(&rd, &vf, NULL, 0, cb) != 0) return false;
        vorbis_info* vi = ov_info(&vf, -1);
        if (!vi || vi->channels < 1 || vi->channels > 2 || vi->rate <= 0)
        {
            ov_clear(&vf);
            return false;
        }
        channels = vi->channels;
        rate = (S32)vi->rate;
        const ogg_int64_t total = ov_pcm_total(&vf, -1);
        if (total > 0) out.reserve((size_t)total * channels);
        for (;;)
        {
            float** pcm = nullptr;
            int section = 0;
            const long n = ov_read_float(&vf, &pcm, 4096, &section);
            if (n == 0) break;
            if (n < 0)
            {
                ov_clear(&vf);
                return false;           // a corrupt stream: the fallback voice plays instead
            }
            for (long i = 0; i < n; ++i)
            {
                for (S32 c = 0; c < channels; ++c) out.push_back(pcm[c][i]);
            }
        }
        ov_clear(&vf);
        return !out.empty();
    }

    /** RBJ Audio EQ Cookbook low-pass (Q = 1/sqrt 2). Same coefficients as rain_mix.py rbj_lowpass. */
    struct Biquad
    {
        F32 b0, b1, b2, a1, a2;
        F32 z1 = 0.f, z2 = 0.f;
        Biquad(F32 hz, F32 rate)
        {
            const F32 w = 2.f * F_PI * hz / rate;
            const F32 c = cosf(w), s = sinf(w), al = s / (2.f * 0.70710678f);
            const F32 a0 = 1.f + al;
            b0 = (1.f - c) / 2.f / a0; b1 = (1.f - c) / a0; b2 = (1.f - c) / 2.f / a0;
            a1 = -2.f * c / a0;        a2 = (1.f - al) / a0;
        }
        F32 run(F32 x)       // transposed direct form II
        {
            const F32 y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /**
     * The decoded recording -> its WAV (rotated by `rotate` of its length, so this session's
     * copy starts somewhere new) and, if wanted, the muffled copy: mono, two low-passes run
     * twice round the loop (the second pass starts in the state the first ended in, so the
     * seam stays continuous), every MUFFLE_DECIM-th sample. Source: rain_mix.py muffle().
     * Runs on the "General" worker: touches nothing but its arguments and the files.
     */
    bool prepare_recording(const std::string& in, const std::string& out, const std::string& out_muffled, F32 rotate)
    {
        std::vector<F32> pcm;
        S32 ch = 0, rate = 0;
        if (!decode_ogg(in, pcm, ch, rate)) return false;
        const size_t frames = pcm.size() / (size_t)ch;
        const size_t off = (size_t)(llclamp(rotate, 0.f, 0.999f) * (F32)frames);
        if (off > 0)
        {
            std::vector<F32> rot(pcm.size());
            for (size_t i = 0; i < frames; ++i)
            {
                const size_t src = (i + off) % frames;
                for (S32 c = 0; c < ch; ++c) rot[i * ch + c] = pcm[src * ch + c];
            }
            pcm.swap(rot);
        }
        if (!write_pcm_wav(out, pcm, (U16)ch, (U32)rate)) return false;
        if (out_muffled.empty()) return true;

        Biquad f1(MUFFLE_HZ, (F32)rate), f2(MUFFLE_HZ, (F32)rate);
        std::vector<F32> muf;
        muf.reserve(frames / MUFFLE_DECIM + 1);
        for (S32 pass = 0; pass < 2; ++pass)
        {
            for (size_t i = 0; i < frames; ++i)
            {
                F32 m = 0.f;
                for (S32 c = 0; c < ch; ++c) m += pcm[i * ch + c];
                const F32 y = f2.run(f1.run(m / (F32)ch));
                if (pass == 1 && (i % MUFFLE_DECIM) == 0 && i + MUFFLE_DECIM <= frames) muf.push_back(y);
            }
        }
        return write_pcm_wav(out_muffled, muf, 1, (U32)(rate / MUFFLE_DECIM));
    }
}

// The tables live here rather than in the header so the UUID strings have one home.
namespace
{
    // <WolfViewer 2026-10-26> The synthesised voices got NEW UUIDs: their loops are now folded
    // with equal power instead of jumping back at the wrap (the old WAVs, still in residents'
    // sound caches under the old UUIDs, clicked every 6 s). They are the wind, and the fallback
    // for any recording that is missing. The cracks are unchanged, so they keep theirs.
    const WolfWeatherSound::Voice AMBIENCE[] = {
        // id         uuid                                     rate  lowHz  highHz  gain
        { "thunder",  "0e13861a-faac-49e1-8ac9-0de5cf2910b5",  0.70f,   0.f,  900.f, 0.55f },
        { "steady",   "5ebc5e91-84ed-42b0-ae7e-8e60e976d327",  1.00f,   0.f, 2200.f, 0.50f },
        { "wind",     "0b6b71cb-9347-413f-bc89-0ac96ac77d41",  0.45f, 160.f,  700.f, 0.60f },
        { "distant",  "0f43a70a-06c9-42a0-a33a-2082da966f6e",  0.60f,   0.f,  550.f, 0.35f },
        { nullptr,    nullptr,                                 0.f,     0.f,    0.f, 0.f   },
    };
    // NOT "NEAR": <windef.h> defines NEAR (and FAR) as legacy near/far pointer keywords, so on
    // MSVC "Voice NEAR[]" preprocesses to "Voice []" and is then parsed as a structured binding
    // declaration (error C3694). It built fine on Linux and macOS and broke only on Windows.
    const WolfWeatherSound::Voice NEAR_LAYERS[] = {
        { "soft",     "3e3ad2b3-676a-4bb8-b5fb-5259740f2b4d",  1.40f, 1800.f,    0.f, 0.30f },
        { "heavy",    "81f143b9-16c5-440b-a14a-a021e1737b9d",  1.80f, 1200.f,    0.f, 0.50f },
        { "leaves",   "059d988f-36be-4353-b8c4-28c911dcedbf",  2.20f, 2200.f, 4600.f, 0.35f },
        { "water",    "e9769679-d63c-4f28-8c58-e03246078455",  1.10f,  500.f, 1400.f, 0.40f },
        { nullptr,    nullptr,                                 0.f,     0.f,    0.f, 0.f   },
    };
    const WolfWeatherSound::Voice CRACK_NEAR =
        { "crack",    "3419684f-4b78-47f2-a20b-c0f8060531a3",  0.35f,   0.f,  700.f, 0.90f };
    const WolfWeatherSound::Voice CRACK_FAR =
        { "crackfar", "5012bda1-e308-4cd8-8423-4c83ef8b8987",  0.30f,   0.f,  220.f, 0.45f };

    // <WolfViewer 2026-10-26> The recordings, app_settings/wolfweather/ (SOURCES.md there).
    // Light, moderate, heavy — the order tierWeights() returns.
    const WolfWeatherSound::Recording BEDS[3] = {
        { "wolf_rain_bed_light.ogg",    "2c4ef65d-11b8-4ec0-b0c9-ab81dee34d18", "8a412f47-3a99-4cd0-bed9-871ca4ced266", true },
        { "wolf_rain_bed_moderate.ogg", "23c0bdb2-7af6-4109-9a5c-a667b009bfa7", "88c15549-e32e-45f5-bfc1-70449506484a", true },
        { "wolf_rain_bed_heavy.ogg",    "c4303e06-5164-40fb-ad3f-c39fb1d84bf7", "bdb9dc08-819d-4cb6-8c3b-01d275b92a59", true },
    };
    const WolfWeatherSound::Recording NEAR_SOFT   =
        { "wolf_rain_near_soft.ogg",    "4a045332-15e0-4801-b3fc-6ecc6a372041", "da6ebfc1-4352-4acb-91cb-3b5b7219fa49", true };
    const WolfWeatherSound::Recording NEAR_LEAVES =
        { "wolf_rain_near_leaves.ogg",  "2803dfcb-c797-41d9-a515-3c5f47e7ebde", "09f5b60e-a409-48a5-b43a-0a329032a221", true };
    const WolfWeatherSound::Recording NEAR_ROOF   =
        { "wolf_rain_near_roof.ogg",    "19cf401f-fa7e-4dd7-89e5-42488aff6100", "97168dd0-c27b-4323-a091-6d4536f327d0", true };
    const WolfWeatherSound::Recording DRIPS[] = {
        { "wolf_rain_drip_1.ogg", "2c91c4f6-edd4-4e10-8f9b-196d70377b1b", nullptr, false },
        { "wolf_rain_drip_2.ogg", "be6352a0-1e4d-463a-acb5-999137bc724e", nullptr, false },
        { "wolf_rain_drip_3.ogg", "63d7ac2b-016d-444a-8788-004ebfa1e003", nullptr, false },
        { "wolf_rain_drip_4.ogg", "d736ac81-07be-4767-ab01-0d6ed86c2973", nullptr, false },
        { "wolf_rain_drip_5.ogg", "4b588031-1125-49df-b36b-687d16e8758f", nullptr, false },
        { "wolf_rain_drip_6.ogg", "9e301c19-560c-4ec5-9914-43f004150f91", nullptr, false },
        { "wolf_rain_drip_7.ogg", "73d9795d-8eab-400d-9d91-3a03c2cc3d0e", nullptr, false },
        { "wolf_rain_drip_8.ogg", "0a9335e2-ddac-48e5-bf6d-242d51ebe822", nullptr, false },
        { "wolf_rain_drip_9.ogg", "7eb65872-6eee-4b28-8eee-69314414171e", nullptr, false },
    };
    constexpr S32 DRIP_COUNT = (S32)(sizeof(DRIPS) / sizeof(DRIPS[0]));

    /** The near dropdown -> its recording. "water": no CC0 recording of rain on water was found,
     *  so it shares the soft patter (region-terrain-work/rain-sound/README.md). */
    const WolfWeatherSound::Recording* near_recording(const std::string& id)
    {
        if (id == "soft" || id == "water") return &NEAR_SOFT;
        if (id == "leaves") return &NEAR_LEAVES;
        if (id == "heavy") return &NEAR_ROOF;
        return nullptr;
    }

    bool is_rain_bed(const std::string& preset)
    {
        return preset == "steady" || preset == "thunder" || preset == "distant";
    }
}

WolfWeatherSound::WolfWeatherSound() {}
WolfWeatherSound::~WolfWeatherSound() {}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the mix — Source: rain_mix.py
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
F32 WolfWeatherSound::intensity(const WolfWeatherProfile& p)
{
    // Unchanged from the first weather sound: heavier weather is louder where you are standing.
    return llmin(1.6f, ((F32)p.mLevel / 2.f) * ((F32)p.mDensity / 100.f));
}

// static
void WolfWeatherSound::tierWeights(F32 i, F32 out[3])
{
    out[0] = out[1] = out[2] = 0.f;
    if (i <= 0.5f)      { out[0] = 1.f; }
    else if (i < 1.0f)  { const F32 t = (i - 0.5f) / 0.5f; out[0] = cosf(t * F_PI_BY_TWO); out[1] = sinf(t * F_PI_BY_TWO); }
    else if (i < 1.5f)  { const F32 t = (i - 1.0f) / 0.5f; out[1] = cosf(t * F_PI_BY_TWO); out[2] = sinf(t * F_PI_BY_TWO); }
    else                { out[2] = 1.f; }
}

// static
F32 WolfWeatherSound::bedLevel(F32 vol, F32 i)
{
    const F32 db = -6.f + 6.f * llclamp((i - 0.1f) / 1.5f, 0.f, 1.f);
    return vol * BED_GAIN * powf(10.f, db / 20.f);
}

// static
F32 WolfWeatherSound::nearLevel(F32 vol, F32 i)
{
    return vol * NEAR_GAIN * llclamp(i / 1.2f, 0.15f, 1.f);
}

// static
F32 WolfWeatherSound::dripRate(F32 i)
{
    return DRIP_RATE * llclamp((i - 0.9f) / 0.7f, 0.f, 1.f);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the voice tables
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
const WolfWeatherSound::Voice* WolfWeatherSound::findVoice(const Voice* table,
                                                           const std::string& id)
{
    if (id.empty() || id == "none") return nullptr;
    for (const Voice* v = table; v->mId; ++v)
    {
        if (id == v->mId) return v;
    }
    return nullptr;
}

// static
const WolfWeatherSound::Voice* WolfWeatherSound::ambience(const std::string& id)
{
    return findVoice(AMBIENCE, id);
}

// static
const WolfWeatherSound::Voice* WolfWeatherSound::nearLayer(const std::string& id)
{
    return findVoice(NEAR_LAYERS, id);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// generating the synthesised sounds (wind, thunder, fallbacks)
// ═══════════════════════════════════════════════════════════════════════════════════════════

// static
bool WolfWeatherSound::ensureSound(const Voice& v)
{
    const std::string path = soundPath(v.mUuid);
    if (gDirUtilp->fileExists(path)) return true;
    const bool one_shot = (v.mId && (std::string(v.mId) == "crack" || std::string(v.mId) == "crackfar"));
    return writeWav(path, v, one_shot);
}

/**
 * Write one synthesised layer as a 16-bit mono WAV.
 *
 * The signal is brown-ish noise (white noise integrated a little, which is a hush rather than a
 * hiss) put through a one-pole low-pass and/or high-pass at the voice's band edges. A loop is
 * generated CROSSFADE_SECS longer than it plays and that extra tail is folded into its head with
 * equal power, so the last sample runs straight on into the first (<WolfViewer 2026-10-26/> it
 * used to jump back to the start of the tail, a click every 6 s). A one-shot (thunder) gets a
 * fast attack and a long exponential decay instead, because a fading-IN rumble sounds like a
 * passing lorry rather than a strike.
 */
// static
bool WolfWeatherSound::writeWav(const std::string& path, const Voice& v, bool oneShot)
{
    const S32 rate = SAMPLE_RATE;
    const S32 n = (S32)(rate * (oneShot ? 3 : LOOP_SECS));
    const S32 fade = oneShot ? 0 : (S32)(rate * CROSSFADE_SECS);
    const S32 total = n + fade;
    std::vector<F32> buf((size_t)total, 0.f);

    // Brown-ish noise at the voice's own rate: a lower rate is generated more slowly and so
    // comes out deeper, which is the cheap equivalent of resampling.
    F32 last = 0.f;
    F32 held = 0.f;
    F32 phase = 0.f;
    const F32 step = v.mRate > 0.f ? v.mRate : 1.f;
    for (S32 i = 0; i < total; ++i)
    {
        phase += step;
        while (phase >= 1.f)
        {
            phase -= 1.f;
            const F32 white = ll_frand(2.f) - 1.f;
            last = (last + 0.02f * white) / 1.02f;
            held = last * 3.5f;
        }
        buf[(size_t)i] = held;
    }

    // One-pole filters. y += (x - y) * k, with k from the cutoff — the standard RC form, and
    // enough shaping for noise: a steeper filter would not be audible through rain.
    if (v.mHighHz > 0.f)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * v.mHighHz / (F32)rate);
        F32 y = 0.f;
        for (S32 i = 0; i < total; ++i) { y += (buf[(size_t)i] - y) * k; buf[(size_t)i] = y; }
    }
    if (v.mLowHz > 0.f)
    {
        const F32 k = 1.f - expf(-2.f * F_PI * v.mLowHz / (F32)rate);
        F32 y = 0.f;
        for (S32 i = 0; i < total; ++i)
        {
            y += (buf[(size_t)i] - y) * k;
            buf[(size_t)i] = buf[(size_t)i] - y;      // high-pass = signal minus its low-pass
        }
    }

    if (oneShot)
    {
        // A crack, then a rolling decay.
        const F32 attack = (F32)rate * 0.04f;
        for (S32 i = 0; i < n; ++i)
        {
            const F32 t = (F32)i;
            const F32 env = (t < attack) ? (t / attack)
                                         : expf(-(t - attack) / ((F32)n * 0.32f));
            buf[(size_t)i] *= env;
        }
    }
    else
    {
        // Equal power: the head fades in (sin) over the tail that followed sample n-1 (cos).
        for (S32 i = 0; i < fade; ++i)
        {
            const F32 t = ((F32)i / (F32)fade) * F_PI_BY_TWO;
            buf[(size_t)i] = buf[(size_t)i] * sinf(t) + buf[(size_t)(n + i)] * cosf(t);
        }
    }
    buf.resize((size_t)n);

    // Normalise to a known peak so a layer's own mGain means the same thing for every voice.
    F32 peak = 0.f;
    for (S32 i = 0; i < n; ++i) peak = llmax(peak, fabsf(buf[(size_t)i]));
    const F32 scale = (peak > 0.0001f) ? (0.85f / peak) : 0.f;
    for (F32& s : buf) s *= scale;

    if (!write_pcm_wav(path, buf, 1, (U32)rate))
    {
        LL_WARNS("WolfWeather") << "could not write weather sound " << path << LL_ENDL;
        return false;
    }
    LL_INFOS("WolfWeather") << "generated weather sound " << v.mId << " -> " << path << LL_ENDL;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// the recordings
// ═══════════════════════════════════════════════════════════════════════════════════════════

/**
 * Decode and write a recording once per session, on the "General" worker (a minute of stereo
 * Vorbis is tens of milliseconds — not a hitch to take on the main thread when the rain
 * starts). Written fresh every session rather than reused from the cache, so each session's
 * copy starts at a new random point. Until it is READY that layer is silent; it fades in.
 */
WolfWeatherSound::Prep WolfWeatherSound::prepare(const Recording& r)
{
    const std::string file = r.mFile;
    auto it = mPrep.find(file);
    if (it != mPrep.end()) return it->second;

    const std::string in = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "wolfweather", file);
    if (!LLFile::isfile(in))
    {
        LL_WARNS("WolfWeather") << "weather recording missing: " << in << " (the synthesised voice plays instead)" << LL_ENDL;
        mPrep[file] = Prep::FAILED;
        return Prep::FAILED;
    }
    const std::string out = soundPath(r.mUuid);
    const std::string out_muffled = r.mMuffledUuid ? soundPath(r.mMuffledUuid) : std::string();
    const F32 rotate = r.mLoop ? ll_frand() : 0.f;
    mPrep[file] = Prep::PENDING;

    auto work = [in, out, out_muffled, rotate]() -> bool { return prepare_recording(in, out, out_muffled, rotate); };
    auto done = [file](bool ok)
    {
        if (WolfWeatherSound::instanceExists()) WolfWeatherSound::instance().onPrepared(file, ok);
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue || !main_queue->postTo(general_queue, work, done))
    {
        done(work());      // no worker (before login, after shutdown began): do it here
    }
    return mPrep[file];
}

void WolfWeatherSound::onPrepared(const std::string& file, bool ok)
{
    mPrep[file] = ok ? Prep::READY : Prep::FAILED;
    if (ok) LL_INFOS("WolfWeather") << "weather recording ready: " << file << LL_ENDL;
    else    LL_WARNS("WolfWeather") << "weather recording could not be decoded: " << file << " (the synthesised voice plays instead)" << LL_ENDL;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
// playing
// ═══════════════════════════════════════════════════════════════════════════════════════════

void WolfWeatherSound::setTarget(const std::string& key, const LLUUID& sound, bool stereo2d, F32 target)
{
    // Nothing to start: do not make (and next frame erase) an entry for a silent copy.
    if (target <= 0.f && mLayers.find(key) == mLayers.end()) return;
    Layer& l = mLayers[key];
    if (l.mSound != sound)
    {
        stopSource(l);      // a different sound under the same key (a synthesised fallback)
        l.mSound = sound;
    }
    l.mStereo2D = stereo2d;
    l.mTarget = llmax(0.f, target);
    l.mSeen = true;
}

void WolfWeatherSound::stopSource(Layer& l)
{
    if (gAudiop && l.mSource.notNull())
    {
        if (LLAudioSource* src = gAudiop->findAudioSource(l.mSource))
        {
            src->setLoop(false);
            src->setGain(0.f);
            src->play(LLUUID::null);      // llaudiosourcevo.cpp:207 — the "stop this" call
        }
    }
    l.mSource.setNull();
}

void WolfWeatherSound::updateLayer(Layer& l, F32 dt, F64 now)
{
    const F32 tau = (l.mTarget > l.mGain) ? TAU_UP : TAU_DOWN;
    l.mGain += (l.mTarget - l.mGain) * (1.f - expf(-dt / tau));
    if (l.mTarget <= 0.f && l.mGain < SILENT)
    {
        l.mGain = 0.f;
        stopSource(l);
        return;
    }
    if (l.mStereo2D)
    {
        if (now >= l.mDriftNext)
        {
            l.mDriftTarget = ll_frand(2.f * DRIFT_DB) - DRIFT_DB;
            l.mDriftNext = now + 4.0 + ll_frand(5.f);
        }
        l.mDriftDb += (l.mDriftTarget - l.mDriftDb) * (1.f - expf(-dt / DRIFT_TAU));
    }
    const F32 gain = llclamp(l.mGain * powf(10.f, l.mDriftDb / 20.f), 0.f, 1.f);

    LLAudioSource* src = l.mSource.notNull() ? gAudiop->findAudioSource(l.mSource) : nullptr;
    if (l.mSource.notNull() && !src)
    {
        // The engine reclaimed it (buffer pressure, a channel taken): start it again shortly.
        l.mSource.setNull();
        l.mRetryAt = now + 0.5;
    }
    if (!src)
    {
        if (now < l.mRetryAt || l.mSound.isNull()) return;
        const LLUUID source_id = LLUUID::generateNewID();
        // Source: fsfloaterassetblacklist.cpp:312 — trigger with our own audio-source id so the
        // source can be found again and kept (looped, re-levelled, stopped). A zero position is
        // the engine's 2D "preview and UI" path (llaudioengine.cpp:826 setForcedPriority): no
        // distance fade and no panning, so a stereo recording plays as the stereo it is.
        // The synthesised (mono) voices keep the old placement at the avatar.
        gAudiop->triggerSound(l.mSound, gAgentID, llmax(gain, START_GAIN), LLAudioEngine::AUDIO_TYPE_AMBIENT,
                              l.mStereo2D ? LLVector3d::zero : gAgent.getPositionGlobal(), LLUUID::null, source_id);
        src = gAudiop->findAudioSource(source_id);
        if (!src)
        {
            l.mRetryAt = now + 2.0;       // muted, or no channel: do not try every frame
            return;
        }
        src->setLoop(true);
        l.mSource = source_id;
    }
    src->setGain(llmax(gain, START_GAIN));
    if (!l.mStereo2D) src->setPositionGlobal(gAgent.getPositionGlobal());
}

void WolfWeatherSound::maybeDrip(F64 now, F32 i, F32 indoors)
{
    const F32 rate = dripRate(i);
    if (rate <= 0.f || indoors >= 0.99f)
    {
        mNextDrip = 0.0;
        return;
    }
    // A Poisson process: exponential gaps, so the drops never fall into a rhythm.
    auto gap = [rate]() { return (F64)(-logf(llmax(1e-4f, 1.f - ll_frand())) / rate); };
    if (mNextDrip <= 0.0)
    {
        for (S32 d = 0; d < DRIP_COUNT; ++d) prepare(DRIPS[d]);
        mNextDrip = now + gap();
        return;
    }
    if (now < mNextDrip) return;
    mNextDrip = now + gap();
    const Recording& d = DRIPS[ll_rand(DRIP_COUNT)];
    if (prepare(d) != Prep::READY) return;
    const F32 vol = (F32)mProfile.mVolume / 100.f;
    const F32 gain = llclamp(vol * (0.10f + ll_frand(0.20f)) * (1.f - indoors), 0.f, 1.f);
    // 1.5-5 m round the camera, a little above it: off the leaves, a gutter, a ledge.
    const F32 dist = 1.5f + ll_frand(3.5f);
    const F32 ang = ll_frand(F_TWO_PI);
    LLVector3 at = LLViewerCamera::getInstance()->getOrigin();
    at += LLVector3(cosf(ang) * dist, sinf(ang) * dist, 0.5f + ll_frand(2.0f));
    gAudiop->triggerSound(LLUUID(d.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPosGlobalFromAgent(at));
}

void WolfWeatherSound::apply(const WolfWeatherProfile& profile)
{
    mProfile = profile;
    mWant = profile.mSound
         && profile.mVolume > 0
         && profile.mKind != WolfWeatherProfile::CLEAR
         && (profile.mSoundPreset != "none" || profile.mSoundLayer != "none");
    // The levels follow in idle(), every frame, so the indoor muffle is continuous too.
}

void WolfWeatherSound::stop()
{
    mWant = false;          // idle() fades every layer out and lets the sources go
}

void WolfWeatherSound::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = (mLastIdle > 0.0) ? (F32)llclamp(now - mLastIdle, 0.0, 0.25) : 0.f;
    mLastIdle = now;
    if (!gAudiop) return;
    if (!mWant && mLayers.empty()) return;

    for (auto& kv : mLayers) kv.second.mSeen = false;

    if (mWant)
    {
        const WolfWeatherProfile& p = mProfile;
        const F32 vol = (F32)p.mVolume / 100.f;
        const F32 i = intensity(p);
        const F32 k = llclamp(WolfWeather::indoors(), 0.f, 1.f);

        // ── the ambience bed ─────────────────────────────────────────────────────────────────
        if (is_rain_bed(p.mSoundPreset))
        {
            F32 w[3];
            tierWeights(i, w);
            const F32 bed = bedLevel(vol, i);
            const bool distant = (p.mSoundPreset == "distant");
            bool fallback = false;
            for (S32 t = 0; t < 3; ++t)
            {
                if (w[t] <= 0.f) continue;              // not playing: fades out if it was
                const Prep st = prepare(BEDS[t]);
                if (st == Prep::FAILED) { fallback = true; continue; }
                if (st != Prep::READY) continue;
                setTarget(std::string("bed:") + BEDS[t].mFile, LLUUID(BEDS[t].mUuid), true,
                          distant ? 0.f : bed * w[t] * (1.f - k));
                setTarget(std::string("mbed:") + BEDS[t].mFile, LLUUID(BEDS[t].mMuffledUuid), true,
                          bed * w[t] * (distant ? DISTANT_GAIN : k * MUFFLED_GAIN));
            }
            if (fallback)
            {
                if (const Voice* v = ambience(p.mSoundPreset))
                {
                    if (ensureSound(*v))
                    {
                        setTarget(std::string("amb:") + v->mId, LLUUID(v->mUuid), false,
                                  vol * v->mGain * (0.6f + i * 0.4f) * (1.f - 0.5f * k));
                    }
                }
            }
        }
        else if (const Voice* v = ambience(p.mSoundPreset))       // the wind
        {
            if (ensureSound(*v))
            {
                setTarget(std::string("amb:") + v->mId, LLUUID(v->mUuid), false,
                          vol * v->mGain * (0.6f + i * 0.4f) * (1.f - 0.5f * k));
            }
        }

        // ── the near layer ───────────────────────────────────────────────────────────────────
        if (const Recording* r = near_recording(p.mSoundLayer))
        {
            const Prep st = prepare(*r);
            if (st == Prep::READY)
            {
                const F32 nl = nearLevel(vol, i);
                setTarget(std::string("near:") + r->mFile, LLUUID(r->mUuid), true, nl * (1.f - k));
                setTarget(std::string("mnear:") + r->mFile, LLUUID(r->mMuffledUuid), true, nl * k * MUFFLED_GAIN);
            }
            else if (st == Prep::FAILED)
            {
                if (const Voice* v = nearLayer(p.mSoundLayer))
                {
                    if (ensureSound(*v))
                    {
                        setTarget(std::string("near:") + v->mId, LLUUID(v->mUuid), false,
                                  llmin(1.f, vol * v->mGain * i) * (1.f - k));
                    }
                }
            }
        }

        // ── single drops, heavy rain only ────────────────────────────────────────────────────
        if (p.mKind == WolfWeatherProfile::RAIN && (p.mSoundPreset == "steady" || p.mSoundPreset == "thunder"))
        {
            maybeDrip(now, i, k);
        }
        else
        {
            mNextDrip = 0.0;
        }
    }
    else
    {
        mNextDrip = 0.0;
    }

    // Every layer eases to its target (0 for anything not set this frame) and goes once silent.
    for (auto it = mLayers.begin(); it != mLayers.end(); )
    {
        Layer& l = it->second;
        if (!l.mSeen) l.mTarget = 0.f;
        updateLayer(l, dt, now);
        if (l.mTarget <= 0.f && l.mGain <= 0.f && l.mSource.isNull()) it = mLayers.erase(it);
        else ++it;
    }
}

// [LIGHTNING 2026-09-13] Called by WolfLightning when the sound of a strike arrives. The gate
// is the one apply() used before playing anything: the weather sound must be on.
void WolfWeatherSound::crack(bool distant)
{
    if (!mWant || !gAudiop) return;
    if (mProfile.mSoundPreset != "thunder" && mProfile.mSoundPreset != "distant") return;
    const Voice& v = distant ? CRACK_FAR : CRACK_NEAR;
    if (!ensureSound(v)) return;
    const F32 gain = llclamp((F32)mProfile.mVolume / 100.f * v.mGain, 0.f, 1.f);
    gAudiop->triggerSound(LLUUID(v.mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT,
                          gAgent.getPositionGlobal());
}
