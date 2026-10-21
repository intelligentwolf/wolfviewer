/**
 * @file wolfsplash.cpp
 * @brief WolfViewer's start-up splash picture (see wolfsplash.h for why it exists).
 *
 * Drawn in software so Windows, Linux and macOS show the same thing with nothing but the viewer's own libraries:
 * libjpeg for the photograph (as llimage/llimagejpeg.cpp), FreeType for the type (as llrender/llfontfreetype.cpp).
 * The photograph is skins/default/textures/wolfgame/wolfgame_background.jpg, the howling wolf the game screens use;
 * the faces are the ones the viewer ships in fonts/ (Cinzel for the name - the WolfAvatars / Wolf Radio title face -
 * DroidSerif-Italic and DejaVuSans).
 */

#include "linden_common.h"
#include "wolfsplash.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <algorithm>

extern "C" {
#ifdef LL_USESYSTEMLIBS
# include <jpeglib.h>
# include <jerror.h>
#else
# include "jpeglib/jpeglib.h"
# include "jpeglib/jerror.h"
#endif
}

#if !defined(_WIN32)
# include <spawn.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <fcntl.h>
# include <unistd.h>
# include <errno.h>
# include <signal.h>
# include <thread>
extern char** environ;
#endif

#include <ft2build.h>
#include FT_FREETYPE_H

namespace
{
    WolfSplashConfig sConfig;
    bool sHasConfig = false;

    // ---- colours (straight RGB 0xRRGGBB) ----
    const uint32_t GOLD_TOP    = 0xF4DC97;
    const uint32_t GOLD_BOTTOM = 0xB8862F;
    const uint32_t GOLD        = 0xD9B45A;
    const uint32_t CREAM       = 0xF0E7D3;
    const uint32_t MUTED       = 0xA9A291;

    inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    inline float smooth(float e0, float e1, float x) { float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3.f - 2.f * t); }

    // dst is premultiplied BGRA (0xAARRGGBB in a uint32 on a little-endian machine: bytes B, G, R, A).
    inline void blend(uint32_t& dst, uint32_t rgb, float a)
    {
        if (a <= 0.f) return;
        a = clamp01(a);
        const float ia = 1.f - a;
        const float sr = float((rgb >> 16) & 255) * a, sg = float((rgb >> 8) & 255) * a, sb = float(rgb & 255) * a;
        const float dr = float((dst >> 16) & 255), dg = float((dst >> 8) & 255), db = float(dst & 255), da = float(dst >> 24);
        const uint32_t r = uint32_t(sr + dr * ia + 0.5f), g = uint32_t(sg + dg * ia + 0.5f), b = uint32_t(sb + db * ia + 0.5f);
        const uint32_t al = uint32_t(255.f * a + da * ia + 0.5f);
        dst = (std::min(al, 255u) << 24) | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }

    inline uint32_t lerpRGB(uint32_t a, uint32_t b, float t)
    {
        t = clamp01(t);
        auto ch = [&](int s) { return uint32_t(float((a >> s) & 255) * (1.f - t) + float((b >> s) & 255) * t + 0.5f); };
        return (ch(16) << 16) | (ch(8) << 8) | ch(0);
    }

    // signed distance from p to a rounded rectangle (centre c, half size hx/hy, corner radius r)
    inline float sdRoundRect(float px, float py, float cx, float cy, float hx, float hy, float r)
    {
        const float qx = std::fabs(px - cx) - (hx - r), qy = std::fabs(py - cy) - (hy - r);
        const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
        return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
    }

    // ---- JPEG (libjpeg's own decode loop, as llimagejpeg.cpp LLImageJPEG::decode) ----
    struct JpegError { jpeg_error_mgr pub; jmp_buf jump; };
    void jpegErrorExit(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegError*>(cinfo->err)->jump, 1); }

    bool loadJpeg(const std::string& path, std::vector<uint8_t>& rgb, int& w, int& h)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        jpeg_decompress_struct cinfo;
        JpegError jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpegErrorExit;
        if (setjmp(jerr.jump))
        {
            jpeg_destroy_decompress(&cinfo);
            fclose(f);
            return false;
        }
        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, f);
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        w = int(cinfo.output_width);
        h = int(cinfo.output_height);
        rgb.assign(size_t(w) * h * 3, 0);
        while (cinfo.output_scanline < cinfo.output_height)
        {
            JSAMPROW row = &rgb[size_t(cinfo.output_scanline) * w * 3];
            jpeg_read_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        fclose(f);
        return w > 0 && h > 0;
    }

    // UTF-8 to code points (malformed bytes are skipped)
    std::vector<uint32_t> utf8Decode(const std::string& s)
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < s.size();)
        {
            const unsigned char c = (unsigned char)s[i];
            uint32_t cp = 0;
            int n = 0;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
            else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
            else { ++i; continue; }
            if (i + n > s.size()) break;
            bool ok = true;
            for (int k = 1; k < n; ++k)
            {
                const unsigned char cc = (unsigned char)s[i + k];
                if ((cc >> 6) != 2) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            i += ok ? n : 1;
            if (ok) out.push_back(cp);
        }
        return out;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// config

void WolfSplash::setConfig(const WolfSplashConfig& cfg) { sConfig = cfg; sHasConfig = true; }
const WolfSplashConfig& WolfSplash::getConfig() { return sConfig; }
bool WolfSplash::hasConfig() { return sHasConfig; }

std::vector<std::string> WolfSplash::toArgs(const WolfSplashConfig& cfg)
{
    return { cfg.art_path, cfg.title_font, cfg.italic_font, cfg.ui_font, cfg.title, cfg.subtitle, cfg.version, cfg.status,
             std::to_string(cfg.has_hint ? 1 : 0), std::to_string(cfg.hint_x), std::to_string(cfg.hint_y) };
}

bool WolfSplash::fromArgs(int argc, char** argv, int first, WolfSplashConfig& cfg)
{
    if (argc - first < 11) return false;
    char** a = argv + first;
    cfg.art_path = a[0]; cfg.title_font = a[1]; cfg.italic_font = a[2]; cfg.ui_font = a[3];
    cfg.title = a[4]; cfg.subtitle = a[5]; cfg.version = a[6]; cfg.status = a[7];
    cfg.has_hint = atoi(a[8]) != 0; cfg.hint_x = atoi(a[9]); cfg.hint_y = atoi(a[10]);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// renderer

struct WolfSplashRenderer::Face
{
    FT_Face face = nullptr;
};

WolfSplashRenderer::WolfSplashRenderer()
{
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) == 0) mFT = lib;
}

WolfSplashRenderer::~WolfSplashRenderer()
{
    for (Face* f : { mTitle, mItalic, mUI })
    {
        if (f && f->face) FT_Done_Face(f->face);
        delete f;
    }
    if (mFT) FT_Done_FreeType((FT_Library)mFT);
}

WolfSplashRenderer::Face* WolfSplashRenderer::loadFace(const std::string& path)
{
    if (!mFT || path.empty()) return nullptr;
    FT_Face face = nullptr;
    if (FT_New_Face((FT_Library)mFT, path.c_str(), 0, &face) != 0) return nullptr;
    Face* f = new Face;
    f->face = face;
    return f;
}

void WolfSplashRenderer::drawText(std::vector<uint32_t>& dst, Face* face, float px, float x, float baseline,
                                  const std::string& utf8, uint32_t top_rgb, uint32_t bottom_rgb, float alpha,
                                  float spacing, bool shadow)
{
    if (!face || !face->face || utf8.empty()) return;
    FT_Face ft = face->face;
    FT_Set_Pixel_Sizes(ft, 0, FT_UInt(std::max(1.f, px) + 0.5f));
    const float top = baseline - px * 0.78f;   // cap height, near enough for a two-colour gradient
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass)
    {
        const float off = pass == 0 ? std::max(1.f, mScale) : 0.f;
        float pen = x;
        for (uint32_t cp : utf8Decode(utf8))
        {
            if (FT_Load_Char(ft, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) continue;
            const FT_GlyphSlot g = ft->glyph;
            const FT_Bitmap& bm = g->bitmap;
            const int gx = int(std::floor(pen + off)) + g->bitmap_left;
            const int gy = int(std::floor(baseline + off)) - g->bitmap_top;
            for (unsigned int row = 0; row < bm.rows; ++row)
            {
                const int y = gy + int(row);
                if (y < 0 || y >= mH) continue;
                const uint32_t rgb = pass == 0 ? 0x000000 : lerpRGB(top_rgb, bottom_rgb, (float(y) - top) / std::max(1.f, baseline - top));
                for (unsigned int col = 0; col < bm.width; ++col)
                {
                    const int xx = gx + int(col);
                    if (xx < 0 || xx >= mW) continue;
                    const float cov = float(bm.buffer[int(row) * bm.pitch + int(col)]) / 255.f;
                    if (cov <= 0.f) continue;
                    blend(dst[size_t(y) * mW + xx], rgb, cov * alpha * (pass == 0 ? 0.55f : 1.f));
                }
            }
            pen += float(g->advance.x) / 64.f + spacing;
        }
    }
}

void WolfSplashRenderer::init(const WolfSplashConfig& cfg, int w, int h, float scale, bool rounded)
{
    mW = std::max(1, w);
    mH = std::max(1, h);
    mScale = scale > 0.f ? scale : 1.f;
    mRounded = rounded;
    mStatus = cfg.status;
    const float s = mScale;
    const float W = float(mW), H = float(mH);

    if (!mTitle) mTitle = loadFace(cfg.title_font);
    if (!mItalic) mItalic = loadFace(cfg.italic_font);
    if (!mUI) mUI = loadFace(cfg.ui_font);

    // ---- the photograph, cover-fitted, nudged so the wolf sits right of centre and the left stays dark for type ----
    mStatic.assign(size_t(mW) * mH, 0xFF0B0E12);
    std::vector<uint8_t> rgb;
    int iw = 0, ih = 0;
    if (loadJpeg(cfg.art_path, rgb, iw, ih))
    {
        // cover, 6% closer, and the whole photo slid 24% of the width to the right so the wolf's head sits clear of
        // the name (preview renders); left of the photo its own dark forest edge fades into the background
        const float k = std::max(W / iw, H / ih) * 1.06f;
        const float xoff = 0.24f * W;
        const float sx0 = 0.f;
        const float sy0 = std::max(0.f, (ih - H / k) * 0.30f);
        for (int y = 0; y < mH; ++y)
        {
            const float fy = std::min(float(ih - 1), sy0 + (y + 0.5f) / k - 0.5f);
            const int y0 = std::max(0, int(fy)), y1 = std::min(ih - 1, y0 + 1);
            const float ty = fy - y0;
            for (int x = 0; x < mW; ++x)
            {
                const float raw = sx0 + (x - xoff + 0.5f) / k - 0.5f;
                const float fade = raw < 0.f ? clamp01(1.f + raw / (0.20f * W / k)) : 1.f;
                const float fx = std::min(float(iw - 1), std::max(0.f, raw));
                const int x0 = std::max(0, int(fx)), x1 = std::min(iw - 1, x0 + 1);
                const float tx = fx - x0;
                float c[3];
                for (int ch = 0; ch < 3; ++ch)
                {
                    const float a = rgb[(size_t(y0) * iw + x0) * 3 + ch], b = rgb[(size_t(y0) * iw + x1) * 3 + ch];
                    const float cc = rgb[(size_t(y1) * iw + x0) * 3 + ch], d = rgb[(size_t(y1) * iw + x1) * 3 + ch];
                    c[ch] = (a * (1 - tx) + b * tx) * (1 - ty) + (cc * (1 - tx) + d * tx) * ty;
                }
                // a cool night grade: a touch darker, blue lifted in the shadows
                const float r = c[0] * 0.80f, g = c[1] * 0.82f, b = std::min(255.f, c[2] * 0.86f + 6.f);
                uint32_t& px = mStatic[size_t(y) * mW + x];
                blend(px, (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b), fade);
            }
        }
    }

    // ---- shading: dark from the left for the name, from the bottom for the status, a soft vignette ----
    for (int y = 0; y < mH; ++y)
    {
        for (int x = 0; x < mW; ++x)
        {
            const float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
            float a = 0.88f * (1.f - smooth(0.f, 0.62f, u));
            a = 1.f - (1.f - a) * (1.f - 0.70f * smooth(0.62f, 1.f, v));
            const float dx = u - 0.55f, dy = v - 0.45f;
            a = 1.f - (1.f - a) * (1.f - 0.45f * smooth(0.30f, 0.85f, std::sqrt(dx * dx + dy * dy)));
            blend(mStatic[size_t(y) * mW + x], 0x05070A, a);
        }
    }

    // ---- a gold hairline inside the edge ----
    const float radius = rounded ? 16.f * s : 0.f;
    {
        const float inset = 10.f * s, half_w = 0.5f * std::max(1.f, s);
        const float cx = W * 0.5f, cy = H * 0.5f, hx = W * 0.5f - inset, hy = H * 0.5f - inset;
        const float r = std::max(0.f, radius - inset * 0.6f) + 4.f * s;
        for (int y = 0; y < mH; ++y)
            for (int x = 0; x < mW; ++x)
            {
                const float d = sdRoundRect(x + 0.5f, y + 0.5f, cx, cy, hx, hy, r);
                const float cov = clamp01(half_w + 0.5f - std::fabs(d));
                if (cov > 0.f) blend(mStatic[size_t(y) * mW + x], GOLD, 0.42f * cov);
            }
    }

    // ---- type ----
    const float L = 36.f * s;
    drawText(mStatic, mTitle, 11.f * s, L, 46.f * s, "WOLF TERRITORIES", GOLD, GOLD, 0.80f, 3.2f * s, false);
    drawText(mStatic, mTitle, 42.f * s, L - 2.f * s, 168.f * s, cfg.title.empty() ? std::string("WOLFVIEWER") : cfg.title,
             GOLD_TOP, GOLD_BOTTOM, 1.f, 4.f * s, true);
    // ornament: a fine rule with a diamond, under the name
    {
        const float y = 186.f * s, x0 = L, x1 = L + 150.f * s, dx = L + 75.f * s, dr = 4.f * s;
        for (int yy = int(y - dr - 1); yy <= int(y + dr + 1); ++yy)
        {
            if (yy < 0 || yy >= mH) continue;
            for (int xx = int(x0); xx <= int(x1); ++xx)
            {
                if (xx < 0 || xx >= mW) continue;
                const float px = xx + 0.5f, py = yy + 0.5f;
                const float edge = std::min(smooth(x0, x0 + 30.f * s, px), 1.f - smooth(x1 - 30.f * s, x1, px));
                const float line = clamp01(0.5f * std::max(1.f, s) + 0.5f - std::fabs(py - y)) * edge;
                const float diamond = clamp01(dr - (std::fabs(px - dx) + std::fabs(py - y)) + 0.5f);
                const float a = std::max(line * 0.75f, diamond);
                if (a > 0.f) blend(mStatic[size_t(yy) * mW + xx], GOLD, a);
            }
        }
    }
    drawText(mStatic, mItalic, 24.f * s, L, 222.f * s, cfg.subtitle.empty() ? std::string("Local build") : cfg.subtitle,
             CREAM, CREAM, 1.f, 0.4f * s, true);
    drawText(mStatic, mUI, 11.5f * s, L, 246.f * s, cfg.version, MUTED, MUTED, 0.9f, 0.6f * s, false);

    // ---- the window's rounded corners (coverage, applied to every frame) ----
    mMask.clear();
    if (rounded)
    {
        mMask.assign(size_t(mW) * mH, 255);
        for (int y = 0; y < mH; ++y)
            for (int x = 0; x < mW; ++x)
            {
                const float d = sdRoundRect(x + 0.5f, y + 0.5f, W * 0.5f, H * 0.5f, W * 0.5f, H * 0.5f, radius);
                mMask[size_t(y) * mW + x] = uint8_t(255.f * clamp01(0.5f - d));
            }
    }
}

void WolfSplashRenderer::setStatus(const std::string& status)
{
    mStatus = status;
}

void WolfSplashRenderer::render(uint32_t* out, double t)
{
    if (!out || mStatic.empty()) return;
    std::vector<uint32_t> frame(mStatic);
    const float s = mScale, W = float(mW), H = float(mH);
    const float L = 36.f * s;

    drawText(frame, mUI, 13.f * s, L, H - 44.f * s, mStatus, CREAM, CREAM, 0.92f, 0.3f * s, false);

    // ---- progress: an honest "working" bar (no made-up percentage): a slim gold track with a light running along it ----
    {
        const float x0 = L, x1 = W - L, yc = H - 29.f * s, half = 1.5f * s;
        const float seg = (x1 - x0) * 0.30f;
        const float phase = float(std::fmod(t / 1.9, 1.0));
        const float e = phase * phase * (3.f - 2.f * phase);
        const float c = x0 - seg + e * ((x1 - x0) + 2.f * seg);
        for (int y = int(yc - half - 2.f * s); y <= int(yc + half + 2.f * s); ++y)
        {
            if (y < 0 || y >= mH) continue;
            for (int x = int(x0); x <= int(x1); ++x)
            {
                if (x < 0 || x >= mW) continue;
                const float px = x + 0.5f, py = y + 0.5f;
                const float cov = clamp01(half + 0.5f - std::fabs(py - yc));
                const float ends = std::min(clamp01((px - x0) / (2.f * s)), clamp01((x1 - px) / (2.f * s)));
                uint32_t& d = frame[size_t(y) * mW + x];
                if (cov > 0.f) blend(d, GOLD, 0.22f * cov * ends);
                const float g = std::exp(-((px - c) * (px - c)) / (2.f * (seg * 0.28f) * (seg * 0.28f)));
                if (cov > 0.f && g > 0.002f) blend(d, lerpRGB(GOLD, GOLD_TOP, g), 0.95f * g * cov * ends);
                // a soft glow around the light
                const float glow = g * clamp01(1.f - std::fabs(py - yc) / (3.5f * s)) * 0.25f;
                if (cov <= 0.f && glow > 0.002f) blend(d, GOLD_TOP, glow * ends);
            }
        }
    }

    if (mMask.empty())
    {
        std::copy(frame.begin(), frame.end(), out);
        return;
    }
    for (size_t i = 0; i < frame.size(); ++i)
    {
        const uint32_t p = frame[i];
        const uint32_t m = mMask[i];
        if (m == 255) { out[i] = p; continue; }
        auto sc = [&](int sh) { return ((((p >> sh) & 255) * m + 127) / 255) << sh; };
        out[i] = sc(24) | sc(16) | sc(8) | sc(0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the helper process (Linux, macOS)

#if !defined(_WIN32)
namespace
{
    bool splashSend(int fd, const std::string& line)
    {
        if (fd < 0) return false;
        const std::string msg = line + "\n";
        size_t off = 0;
        while (off < msg.size())
        {
#ifdef MSG_NOSIGNAL
            const ssize_t n = send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
#else
            const ssize_t n = send(fd, msg.data() + off, msg.size() - off, 0);   // macOS: SO_NOSIGPIPE set in start()
#endif
            if (n <= 0) return false;
            off += size_t(n);
        }
        return true;
    }
}

bool WolfSplashChild::start(const std::string& exe_path)
{
    if (mFd >= 0 || exe_path.empty() || getenv("WOLF_NO_SPLASH")) return false;
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
    // our end must not leak into other children (plugins, voice): they would keep the helper's stdin open
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    // the viewer never blocks on the splash: a helper that stopped reading only costs it its status lines
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    std::vector<std::string> args = { exe_path, "--wolf-splash" };
    for (const std::string& a : WolfSplash::toArgs(WolfSplash::getConfig())) args.push_back(a);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);   // the helper reads its end on stdin (dup2 clears CLOEXEC)
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exe_path.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);
    if (rc != 0)
    {
        close(sv[0]);
        return false;
    }
    mFd = sv[0];
    mPid = pid;
    return true;
}

void WolfSplashChild::status(const std::string& text)
{
    std::string one(text);
    for (char& c : one) if (c == '\n' || c == '\r') c = ' ';
    splashSend(mFd, "S " + one);
}

void WolfSplashChild::stop()
{
    if (mFd < 0) return;
    splashSend(mFd, "F");   // fades out over 280 ms by itself
    close(mFd);
    mFd = -1;
    const pid_t pid = mPid;
    mPid = -1;
    if (pid > 0)
    {
        std::thread([pid]() { waitpid(pid, nullptr, 0); }).detach();   // no zombie left behind
    }
}

bool WolfSplashChild::readCommands(std::string& buffer, std::string& status, bool& status_changed, bool& fade)
{
    static bool nonblocking = false;
    if (!nonblocking)
    {
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
        nonblocking = true;
    }
    bool alive = true;
    char buf[1024];
    for (;;)
    {
        const ssize_t n = read(0, buf, sizeof(buf));
        if (n > 0) { buffer.append(buf, size_t(n)); continue; }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) alive = false;
        break;
    }
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos)
    {
        const std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        if (line.size() >= 2 && line[0] == 'S' && line[1] == ' ') { status = line.substr(2); status_changed = true; }
        else if (line == "F") fade = true;
    }
    if (!alive) fade = true;   // the viewer is gone
    return alive;
}
#endif
