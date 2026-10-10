/**
 * @file wolfdjplayer.cpp
 * @brief WolfViewer Wolf DJ playlist player and its decoders. See wolfdjplayer.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfdjplayer.h"

#include "wolfdjaudio.h"
#include "wolfdjcapture.h"

#include "llcallbacklist.h"
#include "llsdutil.h"
#include "llfile.h"
#include "llviewercontrol.h"
#include "llstring.h"

#define OV_EXCLUDE_STATIC_CALLBACKS
#include "vorbis/vorbisfile.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <chrono>
#include <cstring>

namespace
{
    std::string lower_ext(const std::string& path)
    {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of("/\\");
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
        std::string ext = path.substr(dot + 1);
        LLStringUtil::toLower(ext);
        return ext;
    }

    std::string base_name(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? name : name.substr(0, dot);
    }

    // "Artist - Title.mp3" when a file has no tags.
    void titles_from_name(const std::string& path, std::string& artist, std::string& title)
    {
        const std::string name = base_name(path);
        const size_t dash = name.find(" - ");
        if (dash != std::string::npos)
        {
            artist = name.substr(0, dash);
            title = name.substr(dash + 3);
        }
        else
        {
            title = name;
        }
    }

    // [WOLF DJ 2026-10-10] A track is read into memory first and decoded from there (Paul:
    // "pre-load it", "stop it from not playing"): a slow disk, a sleeping drive or a network
    // folder can then hold up only the load - before the song starts, or while the song before
    // it is still playing - never the music on air. A very big file (over STREAM_ABOVE: a long
    // uncompressed mix) is not held in memory but streamed from the disk as it plays (Paul: "the dj
    // player should not refuse files but it should stream them").
    typedef std::shared_ptr<const std::vector<unsigned char>> bytes_t;
    constexpr long long STREAM_ABOVE = 64ll * 1024 * 1024;

    /** What a decoder reads: the file's bytes, or (no bytes) the file itself, streamed. */
    struct Source
    {
        bytes_t mBytes;
        std::string mStreamPath;
        long long mSize = 0;
        bool ok() const { return mBytes || !mStreamPath.empty(); }
    };

    Source load_file(const std::string& path, std::string& err)
    {
        Source src;
        llstat st;
        if (LLFile::stat(path, &st) != 0)
        {
            err = "Could not open " + base_name(path) + ".";
            return src;
        }
        if ((long long)st.st_size > STREAM_ABOVE)
        {
            src.mStreamPath = path;
            src.mSize = (long long)st.st_size;
            return src;
        }
        LLFILE* f = LLFile::fopen(path, "rb");
        if (!f)
        {
            err = "Could not open " + base_name(path) + ".";
            return src;
        }
        auto data = std::make_shared<std::vector<unsigned char>>();
        data->reserve((size_t)st.st_size);
        unsigned char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        {
            data->insert(data->end(), buf, buf + n);
        }
        const bool bad = ferror(f) != 0;
        fclose(f);
        if (bad || data->empty())
        {
            err = "Could not read " + base_name(path) + ".";
            return src;
        }
        src.mBytes = data;
        src.mSize = (long long)data->size();
        return src;
    }

    /** A read position in a loaded file, or in a file streamed from the disk, for the decoders'
     *  callbacks. Seeks are absolute (64-bit on every platform). */
    struct Reader
    {
        const unsigned char* mData = nullptr;
        LLFILE* mFile = nullptr;
        unsigned long long mSize = 0, mPos = 0;

        ~Reader() { if (mFile) fclose(mFile); }

        size_t read(void* out, size_t bytes)
        {
            const size_t n = (size_t)std::min<unsigned long long>(bytes, mSize - mPos);
            if (n == 0) return 0;
            size_t got = n;
            if (mFile) got = fread(out, 1, n, mFile);
            else memcpy(out, mData + mPos, n);
            mPos += got;
            return got;
        }
        int seek(long long offset, int whence)
        {
            const long long base = whence == SEEK_SET ? 0 : (whence == SEEK_CUR ? (long long)mPos : (long long)mSize);
            const long long to = base + offset;
            if (to < 0 || to > (long long)mSize) return -1;
            if (mFile)
            {
#if LL_WINDOWS
                if (_fseeki64(mFile, to, SEEK_SET) != 0) return -1;
#else
                if (fseeko(mFile, (off_t)to, SEEK_SET) != 0) return -1;
#endif
            }
            mPos = (unsigned long long)to;
            return 0;
        }
        unsigned long long left() const { return mSize - mPos; }
    };

    class Decoder
    {
    public:
        virtual ~Decoder() {}
        // Interleaved float frames; 0 at the end.
        virtual size_t read(float* out, size_t frames) = 0;
        unsigned mRate = 0, mChannels = 0;
        double mDuration = 0.0;
        std::string mArtist, mTitle;
    protected:
        bytes_t mBytes;     // kept alive while the decoder reads it
        Reader mIn;
        bool setSource(const Source& src)
        {
            if (src.mBytes)
            {
                mBytes = src.mBytes;
                mIn.mData = mBytes->data();
                mIn.mSize = mBytes->size();
                return mIn.mSize > 0;
            }
            mIn.mFile = LLFile::fopen(src.mStreamPath, "rb");
            mIn.mSize = (unsigned long long)src.mSize;
            return mIn.mFile != nullptr && mIn.mSize > 0;
        }
    };

    // ---- Ogg Vorbis: libvorbisfile ----
    class VorbisDecoder : public Decoder
    {
    public:
        bool open(const Source& src)
        {
            if (!setSource(src)) return false;
            ov_callbacks cb;
            cb.read_func = [](void* ptr, size_t size, size_t n, void* ud) -> size_t
            {
                return size ? static_cast<Reader*>(ud)->read(ptr, size * n) / size : 0;
            };
            cb.seek_func = [](void* ud, ogg_int64_t off, int whence) -> int { return static_cast<Reader*>(ud)->seek((long long)off, whence); };
            cb.close_func = nullptr;
            cb.tell_func = [](void* ud) -> long { return (long)static_cast<Reader*>(ud)->mPos; };
            if (ov_open_callbacks(&mIn, &mVf, NULL, 0, cb) != 0) return false;
            mOpen = true;
            vorbis_info* vi = ov_info(&mVf, -1);
            if (!vi) return false;
            mRate = (unsigned)vi->rate;
            mChannels = (unsigned)vi->channels;
            mDuration = ov_time_total(&mVf, -1);
            if (vorbis_comment* vc = ov_comment(&mVf, -1))
            {
                for (int i = 0; i < vc->comments; ++i)
                {
                    const std::string c(vc->user_comments[i], vc->comment_lengths[i]);
                    const size_t eq = c.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = c.substr(0, eq);
                    LLStringUtil::toUpper(key);
                    if (key == "ARTIST") mArtist = c.substr(eq + 1);
                    else if (key == "TITLE") mTitle = c.substr(eq + 1);
                }
            }
            return mRate > 0 && mChannels > 0;
        }
        ~VorbisDecoder() override { if (mOpen) ov_clear(&mVf); }
        size_t read(float* out, size_t frames) override
        {
            size_t done = 0;
            while (done < frames)
            {
                float** pcm = nullptr;
                int section = 0;
                const long n = ov_read_float(&mVf, &pcm, (int)(frames - done), &section);
                if (n <= 0) break;
                const unsigned ch = (unsigned)ov_info(&mVf, section)->channels;
                for (long i = 0; i < n; ++i)
                {
                    for (unsigned c = 0; c < mChannels; ++c)
                    {
                        out[(done + i) * mChannels + c] = pcm[std::min(c, ch - 1)][i];
                    }
                }
                done += (size_t)n;
            }
            return done;
        }
    private:
        OggVorbis_File mVf;
        bool mOpen = false;
    };

    // ---- ID3 tags for MP3 (id3.org ID3v2.3.0 / ID3v2.4.0 frame layout, ID3v1 128-byte TAG) ----
    std::string id3_text(const unsigned char* p, size_t len)
    {
        if (len < 1) return std::string();
        const unsigned enc = p[0];
        ++p;
        --len;
        std::string out;
        if (enc == 0 || enc == 3)   // ISO-8859-1 / UTF-8
        {
            std::string raw((const char*)p, strnlen((const char*)p, len));
            if (enc == 3) return raw;
            for (unsigned char c : raw)     // Latin-1 to UTF-8
            {
                if (c < 0x80) out += (char)c;
                else { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
            }
            return out;
        }
        // UTF-16 with BOM (1) or big-endian (2)
        bool be = enc == 2;
        if (enc == 1 && len >= 2)
        {
            be = (p[0] == 0xFE && p[1] == 0xFF);
            p += 2;
            len -= 2;
        }
        llutf16string w;
        for (size_t i = 0; i + 1 < len; i += 2)
        {
            const U16 ch = be ? (U16)((p[i] << 8) | p[i + 1]) : (U16)((p[i + 1] << 8) | p[i]);
            if (ch == 0) break;
            w.push_back(ch);
        }
        return utf16str_to_utf8str(w);
    }

    void parse_id3v2(const unsigned char* d, size_t size, std::string& artist, std::string& title)
    {
        if (size < 10 || memcmp(d, "ID3", 3) != 0) return;
        const unsigned ver = d[3];
        const unsigned flags = d[5];
        size_t pos = 10;
        if (flags & 0x40)   // extended header
        {
            if (size < 14) return;
            const size_t ext = ver >= 4
                ? (size_t)((d[10] & 0x7F) << 21 | (d[11] & 0x7F) << 14 | (d[12] & 0x7F) << 7 | (d[13] & 0x7F))
                : (size_t)(d[10] << 24 | d[11] << 16 | d[12] << 8 | d[13]) + 4;
            pos += ext;
        }
        while (pos + 10 <= size && d[pos] != 0)
        {
            const unsigned char* f = d + pos;
            const size_t fsize = ver >= 4
                ? (size_t)((f[4] & 0x7F) << 21 | (f[5] & 0x7F) << 14 | (f[6] & 0x7F) << 7 | (f[7] & 0x7F))
                : (size_t)(f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
            if (fsize == 0 || pos + 10 + fsize > size) break;
            if (memcmp(f, "TIT2", 4) == 0) title = id3_text(f + 10, fsize);
            else if (memcmp(f, "TPE1", 4) == 0) artist = id3_text(f + 10, fsize);
            pos += 10 + fsize;
        }
    }

    // ---- MP3: dr_mp3 ----
    class Mp3Decoder : public Decoder
    {
    public:
        // known_length >= 0: the length is already known (the loader or the scan counted it),
        // so the pass over the frames is not made again when the song starts.
        bool open(const Source& src, double known_length)
        {
            if (!setSource(src)) return false;
            auto onMeta = [](void* ud, const drmp3_metadata* m)
            {
                Mp3Decoder* self = static_cast<Mp3Decoder*>(ud);
                const unsigned char* d = static_cast<const unsigned char*>(m->pRawData);
                if (!d) return;
                if (m->type == DRMP3_METADATA_TYPE_ID3V2)
                {
                    parse_id3v2(d, m->rawDataSize, self->mArtist, self->mTitle);
                }
                else if (m->type == DRMP3_METADATA_TYPE_ID3V1 && m->rawDataSize >= 128 && memcmp(d, "TAG", 3) == 0 && self->mTitle.empty())
                {
                    self->mTitle = std::string((const char*)d + 3, strnlen((const char*)d + 3, 30));
                    self->mArtist = std::string((const char*)d + 33, strnlen((const char*)d + 33, 30));
                    LLStringUtil::trim(self->mTitle);
                    LLStringUtil::trim(self->mArtist);
                }
            };
            // Callbacks for memory and disk alike (Reader): dr_mp3 buffers its own reads.
            auto onRead = [](void* ud, void* out, size_t n) -> size_t { return static_cast<Mp3Decoder*>(ud)->mIn.read(out, n); };
            auto onSeek = [](void* ud, int off, drmp3_seek_origin o) -> drmp3_bool32
            {
                const int whence = o == DRMP3_SEEK_SET ? SEEK_SET : (o == DRMP3_SEEK_CUR ? SEEK_CUR : SEEK_END);
                return static_cast<Mp3Decoder*>(ud)->mIn.seek(off, whence) == 0;
            };
            auto onTell = [](void* ud, drmp3_int64* cur) -> drmp3_bool32 { *cur = (drmp3_int64)static_cast<Mp3Decoder*>(ud)->mIn.mPos; return DRMP3_TRUE; };
            if (!drmp3_init(&mMp3, onRead, onSeek, onTell, onMeta, this, NULL)) return false;
            mOpen = true;
            mRate = mMp3.sampleRate;
            mChannels = mMp3.channels;
            // [WOLF DJ 2026-10-10] Paul: "i need to see how long a track is as it's playing" - an
            // MP3 had no length before. Source: dr_mp3.h drmp3_get_pcm_frame_count - the Xing/LAME
            // frame count when the file has one, otherwise a pass over the frame headers that
            // seeks back to the start (drmp3_get_mp3_and_pcm_frame_count).
            if (known_length >= 0.0)
            {
                mDuration = known_length;
            }
            else if (mRate > 0)
            {
                mDuration = (double)drmp3_get_pcm_frame_count(&mMp3) / (double)mRate;
            }
            return mRate > 0 && mChannels > 0;
        }
        ~Mp3Decoder() override { if (mOpen) drmp3_uninit(&mMp3); }
        size_t read(float* out, size_t frames) override
        {
            return (size_t)drmp3_read_pcm_frames_f32(&mMp3, frames, out);
        }
    private:
        drmp3 mMp3;
        bool mOpen = false;
    };

    // ---- FLAC: dr_flac ----
    class FlacDecoder : public Decoder
    {
    public:
        bool open(const Source& src)
        {
            if (!setSource(src)) return false;
            auto onMeta = [](void* ud, drflac_metadata* m)
            {
                if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
                FlacDecoder* self = static_cast<FlacDecoder*>(ud);
                drflac_vorbis_comment_iterator it;
                drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
                drflac_uint32 len = 0;
                while (const char* c = drflac_next_vorbis_comment(&it, &len))
                {
                    const std::string s(c, len);
                    const size_t eq = s.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = s.substr(0, eq);
                    LLStringUtil::toUpper(key);
                    if (key == "ARTIST") self->mArtist = s.substr(eq + 1);
                    else if (key == "TITLE") self->mTitle = s.substr(eq + 1);
                }
            };
            auto onRead = [](void* ud, void* out, size_t n) -> size_t { return static_cast<FlacDecoder*>(ud)->mIn.read(out, n); };
            auto onSeek = [](void* ud, int off, drflac_seek_origin o) -> drflac_bool32
            {
                const int whence = o == DRFLAC_SEEK_SET ? SEEK_SET : (o == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END);
                return static_cast<FlacDecoder*>(ud)->mIn.seek(off, whence) == 0;
            };
            auto onTell = [](void* ud, drflac_int64* cur) -> drflac_bool32 { *cur = (drflac_int64)static_cast<FlacDecoder*>(ud)->mIn.mPos; return DRFLAC_TRUE; };
            mFlac = drflac_open_with_metadata(onRead, onSeek, onTell, onMeta, this, NULL);
            if (!mFlac) return false;
            mRate = mFlac->sampleRate;
            mChannels = mFlac->channels;
            if (mRate > 0) mDuration = (double)mFlac->totalPCMFrameCount / (double)mRate;
            return mRate > 0 && mChannels > 0;
        }
        ~FlacDecoder() override { if (mFlac) drflac_close(mFlac); }
        size_t read(float* out, size_t frames) override
        {
            return (size_t)drflac_read_pcm_frames_f32(mFlac, frames, out);
        }
    private:
        drflac* mFlac = nullptr;
    };

    // ---- WAV: RIFF/WAVE, PCM 8/16/24/32-bit and 32-bit float (and WAVE_FORMAT_EXTENSIBLE) ----
    class WavDecoder : public Decoder
    {
    public:
        bool open(const Source& src)
        {
            if (!setSource(src)) return false;
            unsigned char h[12];
            if (mIn.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
            bool have_fmt = false;
            while (true)
            {
                unsigned char ch[8];
                if (mIn.read(ch, 8) != 8) return false;
                const U32 size = (U32)ch[4] | (U32)ch[5] << 8 | (U32)ch[6] << 16 | (U32)ch[7] << 24;
                if (memcmp(ch, "fmt ", 4) == 0)
                {
                    // Checked before the buffer is made: a broken file can claim 4 GB here.
                    if (size < 16 || size > mIn.left()) return false;
                    std::vector<unsigned char> f(size);
                    if (mIn.read(f.data(), size) != size) return false;
                    U16 tag = (U16)(f[0] | f[1] << 8);
                    mChannels = (U16)(f[2] | f[3] << 8);
                    mRate = (U32)f[4] | (U32)f[5] << 8 | (U32)f[6] << 16 | (U32)f[7] << 24;
                    mBits = (U16)(f[14] | f[15] << 8);
                    if (tag == 0xFFFE && size >= 26) tag = (U16)(f[24] | f[25] << 8);   // SubFormat GUID's first two bytes
                    mFloat = (tag == 3);
                    if (tag != 1 && tag != 3) return false;
                    if (size & 1) mIn.seek(1, SEEK_CUR);
                    have_fmt = true;
                }
                else if (memcmp(ch, "data", 4) == 0)
                {
                    if (!have_fmt || mChannels == 0 || mRate == 0) return false;
                    // A file cut short (or a streaming writer's 0xFFFFFFFF) plays what is there.
                    mBytesLeft = (U32)std::min<unsigned long long>(size, mIn.left());
                    const unsigned bpf = mChannels * (mBits / 8);
                    if (bpf > 0) mDuration = (double)(mBytesLeft / bpf) / (double)mRate;
                    return (mBits == 8 || mBits == 16 || mBits == 24 || mBits == 32);
                }
                else if (mIn.seek((long long)size + (size & 1), SEEK_CUR) != 0)
                {
                    return false;
                }
            }
        }
        size_t read(float* out, size_t frames) override
        {
            const unsigned bps = mBits / 8;
            const unsigned bpf = bps * mChannels;
            const size_t want = std::min((size_t)(mBytesLeft / bpf), frames);
            mRaw.resize(want * bpf);
            const size_t got = want ? mIn.read(mRaw.data(), want * bpf) / bpf : 0;
            mBytesLeft -= (U32)(got * bpf);
            for (size_t i = 0; i < got * mChannels; ++i)
            {
                const unsigned char* s = mRaw.data() + i * bps;
                float v = 0.f;
                if (mBits == 8) v = ((int)s[0] - 128) / 128.f;
                else if (mBits == 16) v = (S16)(s[0] | s[1] << 8) / 32768.f;
                else if (mBits == 24) v = (S32)((U32)s[0] << 8 | (U32)s[1] << 16 | (U32)s[2] << 24) / 2147483648.f;
                else if (mFloat) { float f; memcpy(&f, s, 4); v = f; }
                else v = (S32)((U32)s[0] | (U32)s[1] << 8 | (U32)s[2] << 16 | (U32)s[3] << 24) / 2147483648.f;
                out[i] = v;
            }
            return got;
        }
    private:
        unsigned mBits = 0;
        bool mFloat = false;
        U32 mBytesLeft = 0;
        std::vector<unsigned char> mRaw;
    };

    // A decoder for a loaded (or streamed) file. known_length: see Mp3Decoder::open.
    std::unique_ptr<Decoder> open_decoder(const std::string& path, const Source& src, std::string& err, double known_length = -1.0)
    {
        const std::string ext = lower_ext(path);
        std::unique_ptr<Decoder> d;
        bool ok = false;
        if (src.ok())
        {
            if (ext == "ogg" || ext == "oga") { auto v = std::make_unique<VorbisDecoder>(); ok = v->open(src); d = std::move(v); }
            else if (ext == "mp3") { auto v = std::make_unique<Mp3Decoder>(); ok = v->open(src, known_length); d = std::move(v); }
            else if (ext == "flac") { auto v = std::make_unique<FlacDecoder>(); ok = v->open(src); d = std::move(v); }
            else if (ext == "wav") { auto v = std::make_unique<WavDecoder>(); ok = v->open(src); d = std::move(v); }
        }
        // [SECURITY 2026-10-10] sane audio only: a crafted header with thousands of channels or a 1 Hz
        // rate would make the mixer's buffers enormous.
        if (ok && (d->mChannels < 1 || d->mChannels > 8 || d->mRate < 8000 || d->mRate > 384000)) ok = false;
        if (!ok)
        {
            err = "Could not play " + base_name(path) + " (not a playable Ogg, MP3, FLAC or WAV file).";
            return nullptr;
        }
        if (d->mTitle.empty())
        {
            std::string artist, title;
            titles_from_name(path, artist, title);
            d->mTitle = title;
            if (d->mArtist.empty()) d->mArtist = artist;
        }
        return d;
    }

    // Load from disk (or open to stream), then a decoder (the jingle pads, the length scan, and a
    // song that was not preloaded). These run on the DJ's own threads, where nothing else would
    // catch a failure: out of memory, or a decoder library's own exception, is a song that will
    // not play, not the viewer closing.
    std::unique_ptr<Decoder> open_decoder(const std::string& path, std::string& err, double known_length = -1.0)
    {
        try
        {
            const Source src = load_file(path, err);
            return src.ok() ? open_decoder(path, src, err, known_length) : nullptr;
        }
        catch (const std::exception& e)
        {
            err = "Could not play " + base_name(path) + " (" + e.what() + ").";
            return nullptr;
        }
    }

    std::unique_ptr<Decoder> open_decoder_safe(const std::string& path, const bytes_t& bytes, std::string& err, double known_length = -1.0)
    {
        try
        {
            Source src;
            src.mBytes = bytes;
            src.mSize = bytes ? (long long)bytes->size() : 0;
            return open_decoder(path, src, err, known_length);
        }
        catch (const std::exception& e)
        {
            err = "Could not play " + base_name(path) + " (" + e.what() + ").";
            return nullptr;
        }
    }

    // The playlist as a mixer source: picking it on a channel points the player at that channel.
    class PlayerStream : public WolfDJCaptureStream
    {
    public:
        // Whether the DJ hears it is the channel strip's MON button (WolfDJChannel::mMonitor).
        explicit PlayerStream(WolfDJChannel* ch) : mChannel(ch)
        {
            WolfDJPlayer::instance().setOutput(ch);
        }
        ~PlayerStream() override
        {
            if (WolfDJPlayer::instance().output() == mChannel) WolfDJPlayer::instance().setOutput(nullptr);
        }
        bool ok() const override { return true; }
        std::string error() const override { return std::string(); }
    private:
        WolfDJChannel* mChannel;
    };
}

// Main thread: when a new song starts on air, send its artist and title to the stream
// (setting WolfDJPlaylistTitles, the playlist floater's checkbox).
static void player_idle(void*)
{
    static int last_serial = 0;
    WolfDJPlayer& player = WolfDJPlayer::instance();
    const int serial = player.trackSerial();
    if (serial == last_serial) return;
    last_serial = serial;
    if (player.output() && gSavedSettings.getBOOL("WolfDJPlaylistTitles"))
    {
        std::string artist, title;
        player.nowPlaying(artist, title);
        WolfDJMixer::instance().setNowPlaying(artist, title);
    }
}

std::unique_ptr<WolfDJCaptureStream> wolfdj_open_playlist_stream(WolfDJChannel* ch)
{
    return std::make_unique<PlayerStream>(ch);
}

bool WolfDJPlayer::isAudioFile(const std::string& path)
{
    const std::string ext = lower_ext(path);
    return ext == "ogg" || ext == "oga" || ext == "mp3" || ext == "flac" || ext == "wav";
}

// ---------------------------------------------------------------------------------------------
// Jingle pads [WOLF DJ 2026-10-05]
// ---------------------------------------------------------------------------------------------

const float WolfDJJingles::PAD_RGB[PAD_COUNT][3] = {
    { 0.90f, 0.22f, 0.22f },    // red
    { 0.95f, 0.55f, 0.12f },    // orange
    { 0.92f, 0.80f, 0.15f },    // yellow
    { 0.25f, 0.75f, 0.30f },    // green
    { 0.15f, 0.70f, 0.75f },    // teal
    { 0.25f, 0.45f, 0.90f },    // blue
    { 0.60f, 0.35f, 0.85f },    // purple
    { 0.90f, 0.35f, 0.65f },    // pink
};

WolfDJJingles& WolfDJJingles::instance()
{
    static WolfDJJingles sInstance;
    return sInstance;
}

WolfDJJingles::WolfDJJingles()
{
    mPads.assign(PAD_COUNT, std::string());
    const LLSD saved = gSavedSettings.getLLSD("WolfDJJingles");
    for (int i = 0; i < PAD_COUNT && saved.isArray() && i < (int)saved.size(); ++i)
    {
        mPads[i] = saved[i].asString();
    }
}

WolfDJJingles::~WolfDJJingles()
{
    shutdown();
}

void WolfDJJingles::shutdown()
{
    mRunning = false;
    if (mThread.joinable()) mThread.join();
}

std::string WolfDJJingles::pad(int pad) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return pad >= 0 && pad < PAD_COUNT ? mPads[pad] : std::string();
}

// static
std::string WolfDJJingles::padLabel(const std::string& path)
{
    return path.empty() ? std::string() : base_name(path);
}

void WolfDJJingles::setPad(int pad, const std::string& path)
{
    if (pad < 0 || pad >= PAD_COUNT) return;
    LLSD arr = LLSD::emptyArray();
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mPads[pad] = path;
        for (const std::string& p : mPads) arr.append(p);
    }
    gSavedSettings.setLLSD("WolfDJJingles", arr);
    LL_INFOS("WolfDJ") << "jingle pad " << (pad + 1) << " set to " << path << LL_ENDL;
}

void WolfDJJingles::trigger(int pad)
{
    if (pad < 0 || pad >= PAD_COUNT) return;
    const std::string path = this->pad(pad);
    LL_INFOS("WolfDJ") << "jingle pad " << (pad + 1) << " pressed (" << (path.empty() ? std::string("empty") : path)
                       << "), playing now: " << (mPlaying.load() + 1) << LL_ENDL;
    if (path.empty()) return;
    mRequest = pad;     // from the start, even if it is the one playing
    if (!mRunning)
    {
        if (mThread.joinable()) mThread.join();
        mRunning = true;
        mThread = std::thread([this]() { run(); });
    }
}

void WolfDJJingles::stop()
{
    mRequest = -2;
}

std::string WolfDJJingles::takeError()
{
    std::lock_guard<std::mutex> lock(mMutex);
    std::string e;
    e.swap(mError);
    return e;
}

void WolfDJJingles::run()
{
    using clock = std::chrono::steady_clock;
    std::unique_ptr<Decoder> dec;
    std::vector<float> buf;
    double pushed = 0.0;
    clock::time_point base = clock::now();
    while (mRunning)
    {
        const int req = mRequest.exchange(-1);
        if (req == -2)
        {
            dec.reset();
            mPlaying = -1;
        }
        else if (req >= 0)
        {
            std::string err;
            const std::string path = pad(req);
            dec = open_decoder(path, err);
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!dec) mError = err;
            }
            mPlaying = dec ? req : -1;
            pushed = 0.0;
            base = clock::now();
            if (dec)
                LL_INFOS("WolfDJ") << "jingle pad " << (req + 1) << " playing " << path << " ("
                                   << dec->mDuration << " s, " << dec->mRate << " Hz, " << dec->mChannels << " ch)" << LL_ENDL;
            else
                LL_WARNS("WolfDJ") << "jingle pad " << (req + 1) << ": " << err << LL_ENDL;
        }
        if (!dec)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // Real-time pace, PLAYER_LEAD ahead (as WolfDJPlayer::run).
        const double elapsed = std::chrono::duration<double>(clock::now() - base).count();
        if (pushed - elapsed > WolfDJ::PLAYER_LEAD)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const size_t frames = 1024;
        buf.resize(frames * dec->mChannels);
        const size_t got = dec->read(buf.data(), frames);
        if (got == 0)
        {
            LL_INFOS("WolfDJ") << "jingle pad " << (mPlaying.load() + 1) << " finished after " << pushed << " s" << LL_ENDL;
            dec.reset();
            mPlaying = -1;
            continue;
        }
        WolfDJMixer::instance().jingleChannel().pushFloat(buf.data(), got, dec->mChannels, dec->mRate);
        pushed += (double)got / (double)dec->mRate;
    }
}

// ---------------------------------------------------------------------------------------------

WolfDJPlayer& WolfDJPlayer::instance()
{
    static WolfDJPlayer sInstance;
    return sInstance;
}

WolfDJPlayer::WolfDJPlayer()
{
}

WolfDJPlayer::~WolfDJPlayer()
{
    shutdown();
}

void WolfDJPlayer::shutdown()
{
    mScanStop = true;       // first: the scan may be part way through a slow file
    {
        // Under the lock, so the loader cannot miss this between its check and its wait.
        std::lock_guard<std::mutex> lock(mMutex);
        mRunning = false;
    }
    mLoadCv.notify_all();
    if (mThread.joinable()) mThread.join();
    if (mLoader.joinable()) mLoader.join();
    if (mScanner.joinable()) mScanner.join();
    gIdleCallbacks.deleteFunction(&player_idle, nullptr);
}

void WolfDJPlayer::setOutput(WolfDJChannel* ch)
{
    mOutput = ch;
}

bool WolfDJPlayer::routeToMixer(std::string& why)
{
    if (mOutput.load()) return true;
    WolfDJMixer& mix = WolfDJMixer::instance();
    int pick = -1;
    for (int ch = WolfDJ::CH_MUSIC_A; ch < WolfDJ::CH_COUNT && pick < 0; ++ch)
    {
        if (mix.captureId(ch).empty()) pick = ch;
    }
    if (pick < 0)
    {
        why = "Every music channel has a program on it: pick \"Wolf DJ playlist\" on one of them in the Wolf DJ mixer.";
        return false;
    }
    mix.startEngine();
    mix.setCapture(pick, wolfdj_open_playlist_stream(&mix.channel(pick)), WOLFDJ_PLAYLIST_ID);
    mix.channel(pick).mMonitor = true;      // the playlist is heard only through MON
    LL_INFOS("WolfDJ") << "playlist put on music channel " << (pick - WolfDJ::CH_MUSIC_A + 1) << " to play" << LL_ENDL;
    return true;
}

void WolfDJPlayer::setFiles(const std::vector<std::string>& files)
{
    bool start_scan = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        // Keep the playing track playing: find it in the new list.
        const int cur = mCurrent.load();
        std::string playing = (cur >= 0 && cur < (int)mFiles.size()) ? mFiles[cur] : std::string();
        mFiles = files;
        int idx = -1;
        for (size_t i = 0; i < mFiles.size(); ++i)
        {
            if (!playing.empty() && mFiles[i] == playing) { idx = (int)i; break; }
        }
        mCurrent = idx;
        start_scan = !mScanning;
        if (start_scan) mScanning = true;
    }
    // The order may have changed: load whatever now follows the playing song.
    if (mRunning && mCurrent.load() >= 0) preload(mCurrent.load() + 1);
    if (start_scan)
    {
        if (mScanner.joinable()) mScanner.join();   // it has said it is finished
        mScanStop = false;
        mScanner = std::thread([this]() { scanLengths(); });
    }
}

void WolfDJPlayer::loadSaved()
{
    if (mLoadedSaved) return;
    mLoadedSaved = true;
    if (!files().empty()) return;
    std::vector<std::string> list;
    const LLSD saved = gSavedSettings.getLLSD("WolfDJPlaylist");
    if (saved.isArray())
    {
        for (const LLSD& f : llsd::inArray(saved))
        {
            if (f.isString()) list.push_back(f.asString());
        }
    }
    setFiles(list);
}

std::vector<std::string> WolfDJPlayer::files() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mFiles;
}

void WolfDJPlayer::play(int index)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index < 0 || index >= (int)mFiles.size()) return;
    }
    std::string why;
    if (!routeToMixer(why))
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mError = why;
    }
    mRequest = index;
    mPlaying = true;
    mPaused = false;
    if (!mRunning)
    {
        if (mThread.joinable()) mThread.join();
        if (mLoader.joinable()) mLoader.join();
        mRunning = true;
        mThread = std::thread([this]() { run(); });
        mLoader = std::thread([this]() { loaderLoop(); });
        gIdleCallbacks.addFunction(&player_idle, nullptr);
    }
}

void WolfDJPlayer::togglePause()
{
    if (!mPlaying)
    {
        play(mCurrent.load() >= 0 ? mCurrent.load() : 0);
        return;
    }
    if (mPaused)
    {
        std::string why;
        if (!routeToMixer(why))
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mError = why;
        }
    }
    mPaused = !mPaused;
}

void WolfDJPlayer::stop()
{
    mPlaying = false;
    mPaused = false;
    mRequest = -2;      // close the file
}

void WolfDJPlayer::next()
{
    play(mCurrent.load() + 1);
}

void WolfDJPlayer::previous()
{
    const int cur = mCurrent.load();
    play(mPosition.load() > 3.0 || cur <= 0 ? std::max(cur, 0) : cur - 1);
}

void WolfDJPlayer::nowPlaying(std::string& artist, std::string& title) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    artist = mArtist;
    title = mTitle;
}

std::string WolfDJPlayer::lastError() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mError;
}

std::string WolfDJPlayer::preloadedNext() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return (mNextBytes || mNextStream) ? mNextLabel : std::string();
}

double WolfDJPlayer::knownDuration(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mLengths.find(path);
    return it == mLengths.end() ? -1.0 : it->second;
}

// Ask the loader for the song at index (out of range: nothing). Never waits.
void WolfDJPlayer::preload(int index)
{
    std::lock_guard<std::mutex> lock(mMutex);
    const std::string want = (index >= 0 && index < (int)mFiles.size()) ? mFiles[index] : std::string();
    if (want == mLoadWant) return;
    mLoadWant = want;
    if (mNextPath != want)
    {
        mNextPath.clear();
        mNextBytes.reset();
        mNextLabel.clear();
        mNextStream = false;
    }
    mLoadCv.notify_all();
}

// The loader thread: reads the wanted song into memory, so the player can start it without
// touching the disk.
void WolfDJPlayer::loaderLoop()
{
    std::unique_lock<std::mutex> lock(mMutex);
    while (mRunning)
    {
        mLoadCv.wait(lock, [this]() { return !mRunning || (!mLoadWant.empty() && mLoadWant != mNextPath); });
        if (!mRunning) break;
        const std::string path = mLoadWant;
        lock.unlock();
        std::string err;
        Source src;
        try
        {
            src = load_file(path, err);
        }
        catch (const std::exception& e)     // out of memory: the player will say so
        {
            err = e.what();
        }
        const std::shared_ptr<const std::vector<unsigned char>> bytes = src.mBytes;
        const bool stream = !bytes && !src.mStreamPath.empty();     // too big to hold: streamed when it plays
        std::string label;
        if (src.ok())
        {
            std::string e2;
            std::unique_ptr<Decoder> d;
            try { d = open_decoder(path, src, e2); } catch (const std::exception&) {}
            if (d)     // tags and length
            {
                label = d->mArtist.empty() ? d->mTitle : d->mArtist + " - " + d->mTitle;
                std::lock_guard<std::mutex> l2(mMutex);
                mLengths[path] = d->mDuration;
                ++mLengthsVersion;
            }
        }
        lock.lock();
        if (mLoadWant == path)
        {
            // A song that would not load is left to the player, which skips it and says why.
            mNextPath = path;
            mNextBytes = bytes;
            mNextStream = stream && !label.empty();
            mNextLabel = (bytes || mNextStream) ? label : std::string();
            if (!bytes && !mNextStream) mLoadWant.clear();      // do not try it again and again
            LL_INFOS("WolfDJ") << "preloaded " << path << (bytes ? llformat(" (%u bytes)", (unsigned)bytes->size())
                                                           : mNextStream ? std::string(" (big: streamed from the disk)") : std::string(": ") + err) << LL_ENDL;
        }
    }
}

// Song lengths for the playlist window, one file at a time, newest list first.
void WolfDJPlayer::scanLengths()
{
    while (!mScanStop)
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const std::string& f : mFiles)
            {
                if (mLengths.find(f) == mLengths.end()) { path = f; break; }
            }
            if (path.empty())
            {
                mScanning = false;
                return;
            }
        }
        std::string err;
        double len = 0.0;
        if (std::unique_ptr<Decoder> d = open_decoder(path, err)) len = d->mDuration;
        std::lock_guard<std::mutex> lock(mMutex);
        mLengths[path] = len;       // 0: could not be read; not tried again
        ++mLengthsVersion;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    mScanning = false;
}

void WolfDJPlayer::run()
{
    using clock = std::chrono::steady_clock;
    std::unique_ptr<Decoder> dec;
    std::vector<float> buf;
    double pushed = 0.0;                // seconds of the current track handed to the mixer
    clock::time_point base = clock::now();
    bool was_paused = false;
    int auto_next = -1;                 // the song the player itself moved on to at a song's end

    bool carry_on = false;              // the next start follows the last song without a break

    auto start_track = [&](int idx) -> bool
    {
        std::string path;
        std::shared_ptr<const std::vector<unsigned char>> bytes;
        double known = -1.0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (idx < 0 || idx >= (int)mFiles.size()) return false;
            path = mFiles[idx];
            if (mNextBytes && mNextPath == path) bytes = mNextBytes;    // preloaded: no disk now
            auto len = mLengths.find(path);
            if (len != mLengths.end() && len->second > 0.0) known = len->second;
        }
        std::string err;
        dec = bytes ? open_decoder_safe(path, bytes, err, known) : open_decoder(path, err, known);
        mCurrent = idx;
        if (carry_on)
        {
            // Straight on from the last song: its last PLAYER_LEAD is still queued, so this one
            // is timed from where that ends, not from now - otherwise every change of song would
            // queue another lead's worth until the channel trimmed it away mid-song.
            base += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(pushed));
        }
        else
        {
            base = clock::now();
        }
        pushed = 0.0;
        mPosition = 0.0;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (!dec)
            {
                mError = err;
                mDuration = 0.0;
                return false;
            }
            if (mOutput.load()) mError.clear();     // else keep "every music channel is in use"
            mArtist = dec->mArtist;
            mTitle = dec->mTitle;
            mDuration = dec->mDuration;
            mLengths[path] = dec->mDuration;
            ++mLengthsVersion;
            ++mTrackSerial;
        }
        LL_INFOS("WolfDJ") << "playing " << path << " (" << dec->mDuration << " s, " << (bytes ? "preloaded" : "loaded now") << ")" << LL_ENDL;
        preload(idx + 1);
        return true;
    };

    while (mRunning)
    {
        const int req = mRequest.exchange(-1);
        if (req == -2)
        {
            dec.reset();
            mPosition = 0.0;
        }
        else if (req >= 0)
        {
            // Skip files that will not open, to the end of the list at most.
            const int n = (int)files().size();
            int idx = req;
            carry_on = req == auto_next;
            while (mPlaying && !start_track(idx))
            {
                if (++idx >= n)
                {
                    mPlaying = false;
                    dec.reset();
                }
            }
            carry_on = false;
            auto_next = -1;
        }

        WolfDJChannel* out = mOutput.load();
        const bool paused = mPaused.load() || !mPlaying.load() || !out;
        if (!dec || paused)
        {
            was_paused = paused;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (was_paused)
        {
            // Resume where we were: the clock restarts from the current position.
            base = clock::now() - std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(pushed));
            was_paused = false;
        }

        // Real-time pace, PLAYER_LEAD ahead of the clock (the channel holds PLAYER_RING_SECONDS).
        const double elapsed = std::chrono::duration<double>(clock::now() - base).count();
        if (pushed - elapsed > WolfDJ::PLAYER_LEAD)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (elapsed - pushed > 0.5)
        {
            // This thread was held up (the machine stalled): carry on from here rather than
            // pushing a burst the channel would only trim away.
            base = clock::now() - std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(pushed));
        }
        const size_t frames = 1024;
        buf.resize(frames * dec->mChannels);
        const size_t got = dec->read(buf.data(), frames);
        if (got == 0)
        {
            // End of the track: straight into the next (preloaded), or the end of the list.
            const int nxt = mCurrent.load() + 1;
            if (nxt < (int)files().size())
            {
                // Unless the DJ asks for something else first (compare_exchange: their request wins).
                int none = -1;
                if (mRequest.compare_exchange_strong(none, nxt)) auto_next = nxt;
            }
            else
            {
                dec.reset();
                mPlaying = false;
                mPosition = 0.0;
            }
            continue;
        }
        out->pushFloat(buf.data(), got, dec->mChannels, dec->mRate);
        pushed += (double)got / (double)dec->mRate;
        mPosition = pushed;
    }
}
