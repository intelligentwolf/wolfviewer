/**
 * @file wolfvrpipe_video.cpp
 * @brief WolfViewer VR pipe, stage 1: test pattern and encoders. See wolfvrpipe_video.h.
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

#include "wolfvrpipe_video.h"
#include "wolfvr_openh264.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <csetjmp>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <type_traits>

extern "C" {
#include "jpeglib/jpeglib.h"
#include "jpeglib/jerror.h"
}

#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace wolfvr
{
// ---------------------------------------------------------------------------------------------------------------
// Test pattern
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // 5x7 glyphs, one byte per row, bit 4 = leftmost column. Only the characters the overlay prints.
    struct Glyph { char c; uint8_t rows[7]; };
    const Glyph kGlyphs[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
        {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
        {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
        {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    };

    const Glyph* findGlyph(char c)
    {
        for (const Glyph& g : kGlyphs)
        {
            if (g.c == c) return &g;
        }
        return nullptr;
    }

    void fillRect(std::vector<uint8_t>& px, uint32_t w, uint32_t h, int x0, int y0, int rw, int rh,
                  uint8_t r, uint8_t g, uint8_t b)
    {
        const int x1 = std::min<int>(x0 + rw, (int)w), y1 = std::min<int>(y0 + rh, (int)h);
        for (int y = std::max(0, y0); y < y1; ++y)
        {
            uint8_t* p = &px[((size_t)y * w + (size_t)std::max(0, x0)) * 4];
            for (int x = std::max(0, x0); x < x1; ++x, p += 4)
            {
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }

    void drawText(std::vector<uint8_t>& px, uint32_t w, uint32_t h, int x, int y, int scale, const std::string& s)
    {
        // A dark box behind the text so it reads on every wall colour.
        const int tw = (int)s.size() * 6 * scale + scale, th = 9 * scale;
        fillRect(px, w, h, x - scale, y - scale, tw + scale, th, 0, 0, 0);
        for (char c : s)
        {
            if (const Glyph* g = findGlyph(c))
            {
                for (int row = 0; row < 7; ++row)
                {
                    for (int col = 0; col < 5; ++col)
                    {
                        if (g->rows[row] & (0x10 >> col))
                        {
                            fillRect(px, w, h, x + col * scale, y + row * scale, scale, scale, 255, 255, 255);
                        }
                    }
                }
            }
            x += 6 * scale;
        }
    }

    // Rotation matrix columns of unit quaternion q (WebXR: x right, y up, -z forward).
    void quatToMatrix(const Quat& q, float m[3][3])
    {
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        // m[col][row]
        m[0][0] = 1 - 2 * (yy + zz); m[0][1] = 2 * (xy + wz);     m[0][2] = 2 * (xz - wy);
        m[1][0] = 2 * (xy - wz);     m[1][1] = 1 - 2 * (xx + zz); m[1][2] = 2 * (yz + wx);
        m[2][0] = 2 * (xz + wy);     m[2][1] = 2 * (yz - wx);     m[2][2] = 1 - 2 * (xx + yy);
    }
}

void drawTestPattern(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, float fov_deg, const Quat& q,
                     uint64_t frame, double viewer_ms)
{
    rgba.resize((size_t)width * height * 4);
    float m[3][3];
    quatToMatrix(q, m);
    const float t = std::tan(fov_deg * 0.5f * 3.14159265f / 180.f);

    // Wall colours: -Z front (blue), +Z back (red), +X right (green), -X left (purple), +Y ceiling, -Y floor.
    static const uint8_t kWall[6][3] = {
        {150, 40, 40}, {40, 70, 160},    // +Z, -Z
        {40, 130, 60}, {110, 50, 140},   // +X, -X
        {120, 120, 120}, {110, 80, 50},  // +Y, -Y
    };
    const float kGrid = 4.f;          // grid lines per wall half-width
    const float kLine = 0.03f;        // line half width in grid cells

    // Shaded at half resolution (one sample per 2x2 block) on four threads: this is a test card, and it has to cost
    // a few milliseconds, not the ~50 ms a full-resolution single-threaded pass measured (selftest, 1280x1280).
    auto shadeRows = [&](uint32_t y_begin, uint32_t y_end) {
        const float step = 4.f * t / (float)width;    // two pixels per sample
        for (uint32_t py = y_begin; py < y_end; py += 2)
        {
            const float cy = (1.f - 2.f * ((float)py + 1.f) / (float)height) * t;
            // dir = cx * col0 + cy * col1 - col2 ; accumulate along the row
            const float bx = cy * m[1][0] - m[2][0], by = cy * m[1][1] - m[2][1], bz = cy * m[1][2] - m[2][2];
            uint8_t* row0 = &rgba[(size_t)py * width * 4];
            uint8_t* row1 = py + 1 < height ? row0 + (size_t)width * 4 : row0;
            float cx = -t + 0.5f * step;
            for (uint32_t px = 0; px < width; px += 2, cx += step)
            {
                const float dx = bx + cx * m[0][0], dy = by + cx * m[0][1], dz = bz + cx * m[0][2];
                const float ax = std::fabs(dx), ay = std::fabs(dy), az = std::fabs(dz);
                int face;
                float u, v, major;
                if (az >= ax && az >= ay) { face = dz > 0 ? 0 : 1; major = az; u = dx; v = dy; }
                else if (ax >= ay)        { face = dx > 0 ? 2 : 3; major = ax; u = dz; v = dy; }
                else                      { face = dy > 0 ? 4 : 5; major = ay; u = dx; v = dz; }
                const float inv = kGrid / major;
                u *= inv;
                v *= inv;
                const float flu = std::floor(u), flv = std::floor(v);
                const float fu = std::min(u - flu, flu + 1.f - u), fv = std::min(v - flv, flv + 1.f - v);
                const uint8_t* c = kWall[face];
                uint8_t r, g, b;
                if (fu < kLine || fv < kLine)
                {
                    r = g = b = 235;
                }
                else if ((std::fabs(u) < 0.25f && fv < 0.12f) || (std::fabs(v) < 0.25f && fu < 0.12f))
                {
                    r = 255; g = 220; b = 0;   // a yellow cross at each wall's centre
                }
                else
                {
                    // checker shade inside the cells so motion is easy to see
                    const bool odd = ((int)flu + (int)flv) & 1;
                    r = odd ? c[0] : (uint8_t)(c[0] * 3 / 4);
                    g = odd ? c[1] : (uint8_t)(c[1] * 3 / 4);
                    b = odd ? c[2] : (uint8_t)(c[2] * 3 / 4);
                }
                const uint32_t n = px + 1 < width ? 2 : 1;
                for (uint32_t k = 0; k < n; ++k)
                {
                    uint8_t* p0 = row0 + (size_t)(px + k) * 4;
                    uint8_t* p1 = row1 + (size_t)(px + k) * 4;
                    p0[0] = p1[0] = r; p0[1] = p1[1] = g; p0[2] = p1[2] = b; p0[3] = p1[3] = 255;
                }
            }
        }
    };
    constexpr uint32_t kBands = 4;
    std::thread workers[kBands - 1];
    const uint32_t band = ((height / kBands) + 1) & ~1u;   // even, so every band starts on a sample row
    for (uint32_t i = 0; i + 1 < kBands; ++i)
    {
        const uint32_t y0 = std::min(height, i * band), y1 = std::min(height, (i + 1) * band);
        workers[i] = std::thread(shadeRows, y0, y1);
    }
    shadeRows(std::min(height, (kBands - 1) * band), height);
    for (std::thread& w : workers) w.join();

    // Screen-space overlay.
    const int scale = std::max(2, (int)width / 256);
    // forward = -col2; yaw 0 = looking down -Z, positive = turned right; pitch positive = looking up
    const float yaw = std::atan2(-m[2][0], m[2][2]) * 180.f / 3.14159265f;
    const float pitch = std::asin(std::clamp(-m[2][1], -1.f, 1.f)) * 180.f / 3.14159265f;
    char line[96];
    std::snprintf(line, sizeof(line), "WOLF VR PIPE  FRAME %llu", (unsigned long long)frame);
    drawText(rgba, width, height, 4 * scale, 4 * scale, scale, line);
    std::snprintf(line, sizeof(line), "T %.1f", viewer_ms);
    drawText(rgba, width, height, 4 * scale, 14 * scale, scale, line);
    std::snprintf(line, sizeof(line), "YAW %.1f PITCH %.1f", yaw, pitch);
    drawText(rgba, width, height, 4 * scale, 24 * scale, scale, line);
    // One step per frame across the bottom: a dropped or doubled frame is a visible jump or pause.
    const int steps = 60, barw = (int)width / steps;
    fillRect(rgba, width, height, 0, (int)height - 6 * scale, (int)width, 6 * scale, 0, 0, 0);
    fillRect(rgba, width, height, (int)(frame % steps) * barw, (int)height - 6 * scale, barw, 6 * scale,
             255, 255, 255);
}

// ---------------------------------------------------------------------------------------------------------------
// H.264 helpers
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    // Calls fn(nal_type, payload_ptr, payload_len) for each NAL unit in an Annex B buffer.
    template <typename Fn>
    void forEachNal(const std::vector<uint8_t>& b, Fn fn)
    {
        size_t i = 0;
        const size_t n = b.size();
        auto findStart = [&](size_t from, size_t& sc_len) -> size_t {
            for (size_t k = from; k + 3 <= n; ++k)
            {
                if (b[k] == 0 && b[k + 1] == 0)
                {
                    if (b[k + 2] == 1) { sc_len = 3; return k; }
                    if (k + 4 <= n && b[k + 2] == 0 && b[k + 3] == 1) { sc_len = 4; return k; }
                }
            }
            sc_len = 0;
            return n;
        };
        size_t sc = 0;
        i = findStart(0, sc);
        while (i < n)
        {
            const size_t start = i + sc;
            size_t sc2 = 0;
            const size_t next = findStart(start, sc2);
            if (start < n)
            {
                fn(b[start] & 0x1F, &b[start], next - start);
            }
            i = next;
            sc = sc2;
        }
    }
}

std::string avcCodecString(const std::vector<uint8_t>& au)
{
    std::string out;
    forEachNal(au, [&](int type, const uint8_t* p, size_t len) {
        if (out.empty() && type == 7 && len >= 4)
        {
            char s[16];
            std::snprintf(s, sizeof(s), "avc1.%02X%02X%02X", p[1], p[2], p[3]);
            out = s;
        }
    });
    return out;
}

bool avcHasIdr(const std::vector<uint8_t>& au)
{
    bool idr = false;
    forEachNal(au, [&](int type, const uint8_t*, size_t) { if (type == 5) idr = true; });
    return idr;
}

// ---------------------------------------------------------------------------------------------------------------
// JPEG encoder (libjpeg-turbo, already linked by llimage)
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    struct JpegErr
    {
        jpeg_error_mgr pub;
        jmp_buf jump;
        char msg[JMSG_LENGTH_MAX];
    };

    void jpegErrorExit(j_common_ptr ci)
    {
        // libjpeg's contract: error_exit must not return (jerror.h). Same setjmp/longjmp shape as llimagejpeg.cpp.
        JpegErr* e = reinterpret_cast<JpegErr*>(ci->err);
        (*ci->err->format_message)(ci, e->msg);
        longjmp(e->jump, 1);
    }

    // Only trivially destructible locals between setjmp and longjmp; the result is copied out by the caller.
    bool jpegCompress(const uint8_t* rgba, uint32_t w, uint32_t h, unsigned char** mem, unsigned long* mem_size,
                      char* msg)
    {
        jpeg_compress_struct cinfo;
        JpegErr jerr;
        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = jpegErrorExit;
        if (setjmp(jerr.jump))
        {
            jpeg_destroy_compress(&cinfo);
            std::memcpy(msg, jerr.msg, JMSG_LENGTH_MAX);
            return false;
        }
        jpeg_create_compress(&cinfo);
        jpeg_mem_dest(&cinfo, mem, mem_size);
        cinfo.image_width = w;
        cinfo.image_height = h;
        cinfo.input_components = 4;
        cinfo.in_color_space = JCS_EXT_RGBA;   // libjpeg-turbo extension (jpeglib.h)
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, 80, TRUE);
        cinfo.dct_method = JDCT_IFAST;
        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height)
        {
            JSAMPROW row = const_cast<JSAMPROW>(rgba + (size_t)cinfo.next_scanline * w * 4);
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        return true;
    }

    class JpegEncoder : public Encoder
    {
    public:
        JpegEncoder(uint32_t w, uint32_t h) : mW(w), mH(h) {}
        bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) override
        {
            unsigned char* mem = nullptr;
            unsigned long mem_size = 0;
            char msg[JMSG_LENGTH_MAX] = {0};
            const bool ok = jpegCompress(rgba, mW, mH, &mem, &mem_size, msg);
            if (ok) out.data.assign(mem, mem + mem_size);
            if (mem) free(mem);   // jpeg_mem_dest buffers are malloc'd by libjpeg
            if (!ok) { error = std::string("libjpeg: ") + msg; return false; }
            out.key = true;
            return true;
        }
        bool requestKeyFrame() override { return true; }   // every JPEG is a whole picture
        std::string name() const override { return "jpeg (libjpeg)"; }
        std::string codec() const override { return "jpeg"; }
    private:
        uint32_t mW, mH;
    };
}

// ---------------------------------------------------------------------------------------------------------------
// GStreamer H.264 encoders, loaded at run time (Linux). No GStreamer headers: the viewer's packaged headers are
// older than the system library (no gst_app_sink_try_pull_sample), so only opaque pointers and functions whose
// signatures were read from /usr/include/gstreamer-1.0 (1.28) are used. Values:
//   GstState NULL = 1, PLAYING = 4                 (gst/gstelement.h:188-191)
//   GstStateChangeReturn FAILURE = 0               (gst/gstelement.h:215)
//   GstFlowReturn OK = 0                           (gst/gstpad.h:169)
//   struct GError { GQuark domain; gint code; gchar* message; }   (glib/gerror.h:45-50)
// ---------------------------------------------------------------------------------------------------------------
#if defined(__linux__)
namespace
{
    struct GErrorView { uint32_t domain; int code; char* message; };
    using gptr = void*;
    using GstClockTime = uint64_t;
    constexpr GstClockTime kGstSecond = 1000000000ULL;   // GST_SECOND (gst/gstclock.h)
    constexpr int kGstStateNull = 1, kGstStatePlaying = 4, kGstStateChangeFailure = 0, kGstFlowOk = 0;

    struct GstApi
    {
        bool loaded = false;
        std::string error;
        int  (*init_check)(int*, char***, GErrorView**) = nullptr;
        gptr (*parse_launch)(const char*, GErrorView**) = nullptr;
        gptr (*bin_get_by_name)(gptr, const char*) = nullptr;
        int  (*element_set_state)(gptr, int) = nullptr;
        int  (*element_get_state)(gptr, int*, int*, GstClockTime) = nullptr;
        void (*object_unref)(gptr) = nullptr;
        void (*mini_object_unref)(gptr) = nullptr;
        gptr (*buffer_new_allocate)(gptr, size_t, gptr) = nullptr;
        size_t (*buffer_fill)(gptr, size_t, const void*, size_t) = nullptr;
        size_t (*buffer_extract)(gptr, size_t, void*, size_t) = nullptr;
        size_t (*buffer_get_size)(gptr) = nullptr;
        gptr (*sample_get_buffer)(gptr) = nullptr;
        gptr (*element_factory_find)(const char*) = nullptr;
        int  (*app_src_push_buffer)(gptr, gptr) = nullptr;
        gptr (*app_sink_try_pull_sample)(gptr, GstClockTime) = nullptr;
        void (*error_free)(GErrorView*) = nullptr;
    };

    GstApi& gst()
    {
        static GstApi api;
        static bool tried = false;
        if (tried) return api;
        tried = true;
        void* core = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        void* app = core ? dlopen("libgstapp-1.0.so.0", RTLD_NOW | RTLD_GLOBAL) : nullptr;
        void* glib = core ? dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL) : nullptr;
        if (!core || !app || !glib)
        {
            api.error = "GStreamer 1.0 is not installed (libgstreamer-1.0.so.0 / libgstapp-1.0.so.0)";
            return api;
        }
        bool ok = true;
        auto sym = [&](void* lib, const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            if (!fn) { ok = false; api.error = std::string("GStreamer has no ") + name; }
        };
        sym(core, "gst_init_check", api.init_check);
        sym(core, "gst_parse_launch", api.parse_launch);
        sym(core, "gst_bin_get_by_name", api.bin_get_by_name);
        sym(core, "gst_element_set_state", api.element_set_state);
        sym(core, "gst_element_get_state", api.element_get_state);
        sym(core, "gst_object_unref", api.object_unref);
        sym(core, "gst_mini_object_unref", api.mini_object_unref);
        sym(core, "gst_buffer_new_allocate", api.buffer_new_allocate);
        sym(core, "gst_buffer_fill", api.buffer_fill);
        sym(core, "gst_buffer_extract", api.buffer_extract);
        sym(core, "gst_buffer_get_size", api.buffer_get_size);
        sym(core, "gst_sample_get_buffer", api.sample_get_buffer);
        sym(core, "gst_element_factory_find", api.element_factory_find);
        sym(app, "gst_app_src_push_buffer", api.app_src_push_buffer);
        sym(app, "gst_app_sink_try_pull_sample", api.app_sink_try_pull_sample);
        sym(glib, "g_error_free", api.error_free);
        if (!ok) return api;
        GErrorView* err = nullptr;
        if (!api.init_check(nullptr, nullptr, &err))
        {
            api.error = std::string("gst_init failed: ") + (err && err->message ? err->message : "?");
            if (err) api.error_free(err);
            return api;
        }
        api.loaded = true;
        return api;
    }

    bool haveElement(const char* name)
    {
        GstApi& g = gst();
        if (!g.loaded) return false;
        gptr f = g.element_factory_find(name);
        if (!f) return false;
        g.object_unref(f);
        return true;
    }

    class GstH264Encoder : public Encoder
    {
    public:
        ~GstH264Encoder() override { close(); }

        bool open(const std::string& element, const std::string& chain, uint32_t w, uint32_t h, uint32_t fps,
                  std::string& error)
        {
            GstApi& g = gst();
            mElement = element;
            mW = w; mH = h;
            char caps[160];
            std::snprintf(caps, sizeof(caps), "video/x-raw,format=RGBA,width=%u,height=%u,framerate=%u/1", w, h, fps);
            const std::string desc = std::string("appsrc name=src is-live=true do-timestamp=true format=time caps=") +
                caps + " ! " + chain +
                " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au"
                " ! appsink name=sink sync=false max-buffers=4";
            GErrorView* err = nullptr;
            mPipe = g.parse_launch(desc.c_str(), &err);
            if (err)
            {
                error = element + ": " + (err->message ? err->message : "parse error");
                g.error_free(err);
                close();
                return false;
            }
            if (!mPipe) { error = element + ": no pipeline"; return false; }
            mSrc = g.bin_get_by_name(mPipe, "src");
            mSink = g.bin_get_by_name(mPipe, "sink");
            if (!mSrc || !mSink) { error = element + ": appsrc/appsink missing"; close(); return false; }
            if (g.element_set_state(mPipe, kGstStatePlaying) == kGstStateChangeFailure)
            {
                error = element + ": could not start (driver or GPU refused)";
                close();
                return false;
            }
            return true;
        }

        void close()
        {
            GstApi& g = gst();
            if (!g.loaded) return;
            if (mPipe) g.element_set_state(mPipe, kGstStateNull);
            if (mSrc) g.object_unref(mSrc);
            if (mSink) g.object_unref(mSink);
            if (mPipe) g.object_unref(mPipe);
            mSrc = mSink = mPipe = nullptr;
        }

        bool encode(const uint8_t* rgba, EncodedFrame& out, std::string& error) override
        {
            GstApi& g = gst();
            const size_t size = (size_t)mW * mH * 4;
            gptr buf = g.buffer_new_allocate(nullptr, size, nullptr);
            if (!buf) { error = "out of memory"; return false; }
            g.buffer_fill(buf, 0, rgba, size);
            if (g.app_src_push_buffer(mSrc, buf) != kGstFlowOk)   // takes ownership of buf
            {
                error = mElement + ": push refused";
                return false;
            }
            // The first picture includes encoder start-up (NVENC session, VA context), so it may take far longer.
            const GstClockTime timeout = mFirst ? 3 * kGstSecond : kGstSecond / 4;
            mFirst = false;
            gptr sample = g.app_sink_try_pull_sample(mSink, timeout);
            if (!sample)
            {
                error = mElement + ": no output within " + (timeout > kGstSecond ? "3 s" : "250 ms");
                return false;
            }
            gptr obuf = g.sample_get_buffer(sample);   // borrowed from the sample
            const size_t n = obuf ? g.buffer_get_size(obuf) : 0;
            out.data.resize(n);
            if (n) g.buffer_extract(obuf, 0, out.data.data(), n);
            g.mini_object_unref(sample);
            out.key = avcHasIdr(out.data);
            return n > 0;
        }
        std::string name() const override { return mElement; }
        std::string codec() const override { return "h264"; }

    private:
        std::string mElement;
        uint32_t mW = 0, mH = 0;
        bool mFirst = true;
        gptr mPipe = nullptr, mSrc = nullptr, mSink = nullptr;
    };

    // Encoder chains. Every property name and enum nick was read with gst-inspect-1.0 1.28 (nvh264enc, x264enc)
    // or from the GStreamer va plugin documentation (vah264enc: rate-control cbr, key-int-max, b-frames,
    // target-usage 1-7, NV12 input only). GOP = 2 s; the stream restarts the encoder when a key frame is needed.
    // [VR OPENH264 2026-11-03] OpenH264 is no longer a GStreamer chain: wolfvr_openh264.cpp drives the library
    // directly, the same way on every platform.
    struct Chain { const char* family; const char* element; const char* fmt; };
    const Chain kChains[] = {
        {"nvenc", "nvh264enc",
         "nvh264enc preset=p1 tune=ultra-low-latency zerolatency=true rc-mode=cbr bitrate=%u gop-size=%u"},
        {"nvenc", "nvautogpuh264enc",
         "nvautogpuh264enc preset=p1 tune=ultra-low-latency zerolatency=true rc-mode=cbr bitrate=%u gop-size=%u"},
        {"va", "vah264enc",
         "videoconvert n-threads=4 ! video/x-raw,format=NV12 ! vah264enc rate-control=cbr bitrate=%u key-int-max=%u "
         "b-frames=0 target-usage=7"},
        {"va", "vah264lpenc",
         "videoconvert n-threads=4 ! video/x-raw,format=NV12 ! vah264lpenc rate-control=cbr bitrate=%u key-int-max=%u "
         "b-frames=0 target-usage=7"},
        {"x264", "x264enc",
         "videoconvert n-threads=4 ! video/x-raw,format=I420 ! x264enc tune=zerolatency speed-preset=ultrafast "
         "bitrate=%u key-int-max=%u bframes=0 threads=4 sliced-threads=true byte-stream=true"},
    };
}
#endif // __linux__

std::unique_ptr<Encoder> openEncoder(const std::string& which, uint32_t width, uint32_t height, uint32_t fps,
                                     uint32_t bitrate_kbps, std::string& tried)
{
    tried.clear();
    const bool any = which.empty() || which == "auto";
    // A grey picture to prove an encoder really produces H.264 (with an SPS) before the stream is handed to it.
    std::vector<uint8_t> probe;
#if defined(__linux__)
    if (any || which == "nvenc" || which == "va" || which == "x264")
    {
        GstApi& g = gst();
        if (!g.loaded)
        {
            tried += g.error + "; ";
        }
        else
        {
            probe.assign((size_t)width * height * 4, 128);
            for (const Chain& c : kChains)
            {
                if (!any && which != c.family) continue;
                if (!haveElement(c.element)) { tried += std::string(c.element) + ": not installed; "; continue; }
                char chain[400];
                std::snprintf(chain, sizeof(chain), c.fmt, bitrate_kbps, fps * 2);
                auto enc = std::make_unique<GstH264Encoder>();
                std::string err;
                EncodedFrame test;
                if (enc->open(c.element, chain, width, height, fps, err) && enc->encode(probe.data(), test, err) &&
                    !avcCodecString(test.data).empty())
                {
                    // The probe picture used up the IDR; reopen so the stream starts with one.
                    enc = std::make_unique<GstH264Encoder>();
                    if (enc->open(c.element, chain, width, height, fps, err))
                    {
                        return enc;
                    }
                }
                tried += (err.empty() ? std::string(c.element) + ": no SPS in output" : err) + "; ";
            }
        }
    }
#endif
    // [VR OPENH264 2026-11-03] Cisco's OpenH264, driven directly: the H.264 encoder on Windows and macOS, and the
    // fallback after the GStreamer ones on Linux.
    if (any || which == "openh264")
    {
        const OpenH264Tuning tuning;
        std::string err;
        auto enc = openOpenH264Encoder(width, height, fps, bitrate_kbps, tuning, err);
        if (enc)
        {
            if (probe.empty()) probe.assign((size_t)width * height * 4, 128);
            EncodedFrame test;
            if (enc->encode(probe.data(), test, err) && !avcCodecString(test.data).empty())
            {
                // The probe used up the IDR and fed the rate control a grey picture: start again from a fresh one.
                enc = openOpenH264Encoder(width, height, fps, bitrate_kbps, tuning, err);
                if (enc) return enc;
            }
            else if (err.empty())
            {
                err = "openh264: no SPS in output";
            }
        }
        tried += err + "; ";
    }
    if (any || which == "jpeg")
    {
        return std::make_unique<JpegEncoder>(width, height);
    }
    tried += "no encoder matched '" + which + "'";
    return nullptr;
}
}
