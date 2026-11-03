/**
 * @file wolfvrpipe_selftest.cpp
 * @brief Stand-alone tests and a test server for the VR pipe (wolfvrpipe*.cpp), built by
 *        scripts/wolfvr/build_selftest.sh against the same packaged Boost/OpenSSL/libjpeg as the viewer.
 *
 *   wolfvrpipe_selftest selftest                 unit checks + encoder benchmark; exit code 0 = all passed
 *   wolfvrpipe_selftest bench264 [threads] [slices] [complexity] [convert_threads]
 *                                                OpenH264 direct at the VR sizes: encode avg/p95 ms, CPU, bitrate
 *   WOLFVR_FETCH_TEST=1 wolfvrpipe_selftest selftest   also downloads Cisco's OpenH264 into a temporary folder and
 *                                                encodes with it (needs the internet)
 *   wolfvrpipe_selftest serve <web_root> [secs] [encoder]
 *                                                runs the real server on the LAN address, prints the address
 *                                                large; any headset on the home network connects (one at a
 *                                                time). WOLFVR_STATS=1: one JSON line of numbers a second (for
 *                                                the browser tests). WOLFVR_STATE_DIR: where the certificate is
 *                                                kept (default ~/.config/wolf/wolfvr-test), so the headset
 *                                                browser's certificate warning comes once only.
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

#include "../wolfvrpipe.h"
#include "../wolfvrdevice.h"
#include "../wolfvrpipe_video.h"
#include "../wolfvr_openh264.h"
#include "../wolfvrcodecfetch.h"

#include <boost/asio.hpp>
#include "../qrcodegen/qrcodegen.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <mutex>
#include <sstream>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
#include <condition_variable>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace wolfvr;

static int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++gFailures; } \
                         else { std::printf("ok   %s\n", #cond); } } while (0)

static bool readAllText(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static double msNow()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static void benchEncoder(const std::string& which, uint32_t w, uint32_t h)
{
    std::string tried;
    auto enc = openEncoder(which, w, h, 72, 25000, tried);
    if (!enc || (which != "jpeg" && enc->codec() != "h264"))
    {
        std::printf("bench %-9s not available (%s)\n", which.c_str(), tried.c_str());
        return;
    }
    std::vector<uint8_t> px;
    double total = 0, worst = 0, draw = 0;
    size_t bytes = 0;
    int keys = 0;
    std::string codec_string;
    const int N = 216;   // 3 s at 72 fps
    for (int i = 0; i < N; ++i)
    {
        const float a = i * 0.02f;   // turning head: yaw sweep
        const Quat q{0.f, std::sin(a / 2), 0.f, std::cos(a / 2)};
        const double t0 = msNow();
        drawTestPattern(px, w, h, 100.f, q, i, t0);
        const double t1 = msNow();
        EncodedFrame f;
        std::string err;
        if (!enc->encode(px.data(), f, err)) { std::printf("bench %s: encode failed: %s\n", which.c_str(), err.c_str()); ++gFailures; return; }
        const double t2 = msNow();
        if (i == 0 && enc->codec() == "h264") codec_string = avcCodecString(f.data);
        if (i == 0) CHECK(f.key);
        keys += f.key;
        draw += t1 - t0;
        total += t2 - t1;
        worst = std::max(worst, t2 - t1);
        bytes += f.data.size();
    }
    std::printf("bench %-9s %-16s %ux%u draw %.2f ms/frame, encode avg %.2f ms max %.2f ms, %.1f Mbit/s at 72 fps, "
                "%d key frames, %s\n", which.c_str(), enc->name().c_str(), w, h, draw / N, total / N, worst,
                bytes * 8.0 / (N / 72.0) / 1e6, keys, codec_string.c_str());
}

// CPU seconds used by this process so far (user + system), for the encoder benchmark. POSIX getrusage(2).
static double cpuSeconds()
{
#if defined(__linux__) || defined(__APPLE__)
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return (double)u.ru_utime.tv_sec + (double)u.ru_utime.tv_usec / 1e6 + (double)u.ru_stime.tv_sec +
           (double)u.ru_stime.tv_usec / 1e6;
#else
    return 0;
#endif
}

// [VR OPENH264 2026-11-03] One OpenH264 configuration at one size: N pictures of the turning test card, timed one by
// one (the drawing is not counted). CPU% = CPU time spent inside encode / wall time of encode (cores busy while it
// runs); "load at fps" = CPU time per picture * fps (how much of one core the stream keeps busy at that rate).
// WOLFVR_BENCH_CONTENT=detail: fine detail over the test card that pans 4 pixels a picture, much harder to encode
// than the card's flat walls (closer to a textured world turning past the camera).
static void addDetail(std::vector<uint8_t>& px, uint32_t w, uint32_t h, int frame)
{
    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t* p = &px[(size_t)y * w * 4];
        for (uint32_t x = 0; x < w; ++x, p += 4)
        {
            uint32_t v = (x + (uint32_t)frame * 4u) * 2654435761u ^ (y * 40503u);
            v ^= v >> 13;
            const int n = (int)(v & 63) - 32;
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::clamp((int)p[c] + n, 0, 255);
        }
    }
}

static void bench264One(const char* label, uint32_t w, uint32_t h, uint32_t fps, uint32_t kbps, const OpenH264Tuning& t)
{
    const bool detail = std::getenv("WOLFVR_BENCH_CONTENT") && std::string(std::getenv("WOLFVR_BENCH_CONTENT")) == "detail";
    std::string err;
    auto enc = openOpenH264Encoder(w, h, fps, kbps, t, err);
    if (!enc) { std::printf("bench264 %-22s not available: %s\n", label, err.c_str()); ++gFailures; return; }
    const int N = (int)fps * 5;
    std::vector<uint8_t> px;
    std::vector<double> ms;
    double cpu = 0, wall = 0;
    size_t bytes = 0;
    int keys = 0;
    std::string codec_string;
    for (int i = 0; i < N; ++i)
    {
        const float a = (float)i * 0.02f;
        const Quat q{0.f, std::sin(a / 2), 0.f, std::cos(a / 2)};
        drawTestPattern(px, w, h, 100.f, q, (uint64_t)i, (double)i);
        if (detail) addDetail(px, w, h, i);
        EncodedFrame f;
        const double c0 = cpuSeconds(), t0 = msNow();
        if (!enc->encode(px.data(), f, err)) { std::printf("bench264 %s: encode failed: %s\n", label, err.c_str()); ++gFailures; return; }
        const double t1 = msNow(), c1 = cpuSeconds();
        if (i == 0) { codec_string = avcCodecString(f.data); CHECK(f.key); CHECK(!codec_string.empty()); }
        ms.push_back(t1 - t0);
        wall += (t1 - t0) / 1000.0;
        cpu += c1 - c0;
        bytes += f.data.size();
        keys += f.key ? 1 : 0;
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    double avg = 0;
    for (double v : ms) avg += v;
    avg /= (double)N;
    const double p95 = sorted[(size_t)((double)N * 0.95)];
    const double mbps = (double)bytes * 8.0 / ((double)N / (double)fps) / 1e6;
    std::printf("bench264 %-22s %ux%u@%u target %5.1f Mbit/s | encode avg %6.2f ms p95 %6.2f ms max %6.2f | "
                "CPU %4.0f%% while encoding, load at %u fps %4.0f%% of a core | real %5.1f Mbit/s | %d keys | %s | %s\n",
                label, w, h, fps, kbps / 1000.0, avg, p95, sorted.back(), wall > 0 ? 100.0 * cpu / wall : 0.0, fps,
                100.0 * cpu / (double)N * fps, mbps, keys, codec_string.c_str(), enc->name().c_str());
    std::fflush(stdout);
}

static int bench264(int argc, char** argv)
{
    OpenH264Tuning t;
    if (argc > 2) t.threads = std::atoi(argv[2]);
    if (argc > 3) t.slices = std::atoi(argv[3]);
    if (argc > 4) t.complexity = std::atoi(argv[4]);
    if (argc > 5) t.convert_threads = std::atoi(argv[5]);
    if (const char* p = std::getenv("WOLFVR_OPENH264")) setOpenH264Path(p);
    std::printf("cores %u, threads %d (0 = auto), slices %d, complexity %d, convert threads %d\n",
                std::thread::hardware_concurrency(), t.threads, t.slices, t.complexity, t.convert_threads);
    bench264One("Big Screen", 1920, 1080, 60, 25000, t);
    bench264One("Quest 3 stereo", 2304, 1152, 60, 40000, t);
    bench264One("Quest 2 stereo", 2048, 1024, 72, 30000, t);
    bench264One("fallback 1792x896", 1792, 896, 72, 25000, t);
    bench264One("fallback 1536x768", 1536, 768, 72, 20000, t);
    // What a reopen costs (the frame thread reopens the encoder for every key frame the page asks for):
    // openEncoder("openh264") = init + a probe picture + init again.
    {
        std::string tried;
        const double t0 = msNow();
        auto enc = openEncoder("openh264", 2304, 1152, 60, 40000, tried);
        std::printf("openEncoder(\"openh264\") 2304x1152 took %.1f ms%s\n", msNow() - t0, enc ? "" : (" FAILED " + tried).c_str());
    }
    std::printf("%s\n", openh264LoadedDescription().c_str());
    return gFailures ? 1 : 0;
}

// [VR OPENH264 2026-11-03] The licence text in the code is Cisco's file byte for byte; the codec download refuses
// what is not Cisco's; WOLFVR_FETCH_TEST=1 also downloads the real file into a temporary folder and encodes with it.
static void codecFetchTests()
{
    const char* root = std::getenv("WOLFVR_SOURCE_DIR");   // indra/newview, for the vendored licence file
    std::string lic;
    if (root && readAllText(std::string(root) + "/openh264/BINARY_LICENSE.txt", lic))
    {
        CHECK(lic == openh264BinaryLicenseText());
    }
    else
    {
        std::printf("     (WOLFVR_SOURCE_DIR not set: licence text not compared)\n");
    }
    CHECK(std::string(openh264AttributionText()) == "OpenH264 Video Codec provided by Cisco Systems, Inc.");
    CHECK(openh264Available());

    const std::filesystem::path tmp = std::filesystem::temp_directory_path() /
                                      ("wolfvr-codec-" + std::to_string((unsigned long long)msNow()));
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);
    const std::string dir = tmp.string();
    CHECK(!openh264Installed(dir));
    CHECK(openh264File(dir).rfind(dir, 0) == 0);
    // a wrong file under the right name is not "installed"
    std::ofstream(openh264File(dir), std::ios::binary) << "not the codec";
    CHECK(!openh264Installed(dir));
    std::filesystem::remove(openh264File(dir), ec);

    if (std::getenv("WOLFVR_FETCH_TEST"))
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false, ok = false;
        std::string msg;
        const double t0 = msNow();
        fetchOpenH264Async(dir, [&](bool o, std::string s) {
            std::lock_guard<std::mutex> lk(m);
            ok = o; msg = std::move(s); done = true;
            cv.notify_all();
        });
        // a second request while the first runs is refused at once
        bool second_done = false, second_ok = true;
        fetchOpenH264Async(dir, [&](bool o, std::string) { second_ok = o; second_done = true; });
        CHECK(second_done && !second_ok);
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(150), [&] { return done; });
        }
        std::printf("     fetch: %s (%.0f ms) %s\n", ok ? "ok" : "FAILED", msNow() - t0, msg.c_str());
        CHECK(ok);
        CHECK(openh264Installed(dir));
        std::string again;
        CHECK(fetchOpenH264(dir, again));   // already there: no second download
        CHECK(again.find("already") != std::string::npos);
        setOpenH264Path(openh264File(dir));
        std::string err;
        OpenH264Tuning t;
        auto enc = openOpenH264Encoder(640, 360, 30, 2000, t, err);
        CHECK(enc != nullptr);
        if (enc)
        {
            std::vector<uint8_t> px;
            drawTestPattern(px, 640, 360, 100.f, Quat{}, 0, 0);
            EncodedFrame f;
            CHECK(enc->encode(px.data(), f, err));
            CHECK(f.key);
            std::printf("     encoded with %s: %s\n", openh264LoadedDescription().c_str(), avcCodecString(f.data).c_str());
            CHECK(openh264LoadedDescription().find(openh264File(dir)) != std::string::npos);
        }
    }
    std::filesystem::remove_all(tmp, ec);
}

static int selftest()
{
    // LAN address rules
    CHECK(isPrivateIPv4("192.168.1.9"));
    CHECK(isPrivateIPv4("10.0.0.1"));
    CHECK(isPrivateIPv4("172.16.5.5"));
    CHECK(isPrivateIPv4("172.31.255.255"));
    CHECK(!isPrivateIPv4("172.32.0.1"));
    CHECK(!isPrivateIPv4("172.15.0.1"));
    CHECK(!isPrivateIPv4("100.98.231.62"));   // Tailscale CGNAT: not a LAN
    CHECK(!isPrivateIPv4("8.8.8.8"));
    CHECK(!isPrivateIPv4("127.0.0.1"));
    CHECK(!isPrivateIPv4("not an ip"));
    const LanAddress lan = pickLanAddress();
    std::printf("LAN address: '%s' (%s)\n", lan.ip.c_str(), lan.reason.c_str());

    // [VR SIMPLE 2026-11-03] the page id and the browser name (wolfvrdevice.h)
    {
        CHECK(validDeviceId("0123456789abcdef0123456789abcdef"));
        CHECK(!validDeviceId("0123456789ABCDEF0123456789ABCDEF"));   // upper case is not what the page makes
        CHECK(!validDeviceId("0123"));
        CHECK(!validDeviceId("0123456789abcdef0123456789abcdeg"));
        CHECK(deviceLabelFromUserAgent("Mozilla/5.0 (X11; Linux x86_64; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) "
                                       "OculusBrowser/40.0 Chrome/132.0 VR Safari/537.36") == "Meta Quest 3 browser");
        CHECK(deviceLabelFromUserAgent("Mozilla/5.0 (X11; Linux x86_64; Quest 3S) OculusBrowser/40.0") ==
              "Meta Quest 3S browser");
        CHECK(deviceLabelFromUserAgent("") == "a web browser");
    }

    // certificate
    std::string cert, key, err;
    CHECK(makeSelfSignedCert("192.168.1.9", cert, key, err));
    CHECK(cert.find("BEGIN CERTIFICATE") != std::string::npos);
    CHECK(key.find("PRIVATE KEY") != std::string::npos);
    CHECK(certCoversIp(cert, "192.168.1.9"));
    CHECK(!certCoversIp(cert, "192.168.1.10"));
    CHECK(!certCoversIp("garbage", "192.168.1.9"));

    // H.264 helpers: an SPS for High profile (100), constraints 0, level 4.2 (42 = 0x2A), then an IDR slice header.
    const std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0xF0,              // AUD
                                     0, 0, 0, 1, 0x67, 100, 0x00, 0x2A, 0xAC, // SPS
                                     0, 0, 1, 0x68, 0xEE,                   // PPS
                                     0, 0, 1, 0x65, 0x88, 0x84};            // IDR
    CHECK(avcCodecString(au) == "avc1.64002A");
    CHECK(avcHasIdr(au));
    const std::vector<uint8_t> p = {0, 0, 0, 1, 0x41, 0x9A};                // non-IDR slice
    CHECK(avcCodecString(p).empty());
    CHECK(!avcHasIdr(p));

    // start refusals
    {
        Service s;
        Config c;
        c.bind_ip = "8.8.8.8";
        CHECK(!s.start(c, err));
        std::printf("     refused public address: %s\n", err.c_str());
    }

    // busy port: the next one is used; all eight busy: a clear refusal naming the holder (never 4333: Paul tests there)
    if (!lan.ip.empty())
    {
        Config c;
        c.bind_ip = lan.ip;
        c.port = 4360;
        c.web_root = std::getenv("WOLFVR_WEBROOT") ? std::getenv("WOLFVR_WEBROOT") : "";
        CHECK(makeSelfSignedCert(c.bind_ip, c.cert_pem, c.key_pem, err));
        std::vector<std::unique_ptr<Service>> held;
        for (int k = 0; k < 8; ++k)
        {
            auto s = std::make_unique<Service>();
            const bool ok = s->start(c, err);
            CHECK(ok);
            if (ok) CHECK(s->port() == 4360 + k);
            held.push_back(std::move(s));
        }
        Service extra;
        CHECK(!extra.start(c, err));
        std::printf("     all busy: %s\n", err.c_str());
        CHECK(err.find("wolfvrpipe_self") != std::string::npos);

        // [VR PAIRING 2026-11-03] plain HTTP on the LAN port gets only a redirect to the HTTPS page, built from our
        // own address whatever Host the request names
        if (!held.empty() && held[0]->running())
        {
            namespace net = boost::asio;
            net::io_context io;
            net::ip::tcp::socket sock(io);
            boost::system::error_code ec;
            sock.connect({net::ip::make_address(lan.ip), 4360}, ec);
            CHECK(!ec);
            const std::string rq = "GET /ws?d=x HTTP/1.1\r\nHost: evil.example\r\nConnection: close\r\n\r\n";
            net::write(sock, net::buffer(rq), ec);
            std::string reply;
            char buf[2048];
            for (;;)
            {
                const size_t n = sock.read_some(net::buffer(buf), ec);
                reply.append(buf, n);
                if (ec) break;
            }
            CHECK(reply.rfind("HTTP/1.1 302", 0) == 0);
            CHECK(reply.find("Location: https://" + lan.ip + ":4360/\r\n") != std::string::npos);
            CHECK(reply.find("evil.example") == std::string::npos);
        }
    }

    // [VR OPENH264] the codec download (before the encoders, so a downloaded library is the one they load)
    codecFetchTests();

    // [VR OPENH264] an explicit key frame without reopening: OpenH264 and JPEG can, GStreamer chains cannot
    {
        std::string tried;
        auto enc = openEncoder("openh264", 320, 240, 30, 1000, tried);
        if (enc)
        {
            std::vector<uint8_t> px;
            EncodedFrame f;
            std::string err;
            for (int i = 0; i < 3; ++i) { drawTestPattern(px, 320, 240, 100.f, Quat{}, (uint64_t)i, 0); enc->encode(px.data(), f, err); }
            CHECK(!f.key);
            CHECK(enc->requestKeyFrame());
            drawTestPattern(px, 320, 240, 100.f, Quat{}, 3, 0);
            CHECK(enc->encode(px.data(), f, err));
            CHECK(f.key);
            CHECK(!avcCodecString(f.data).empty());   // the IDR carries the SPS again
        }
        else
        {
            std::printf("     openh264 not available: %s\n", tried.c_str());
        }
    }

    // encoders
    for (const char* e : {"nvenc", "va", "x264", "openh264", "jpeg"}) benchEncoder(e, 1280, 1280);
    std::printf("%s (%d failures)\n", gFailures ? "SELFTEST FAILED" : "SELFTEST PASSED", gFailures);
    return gFailures ? 1 : 0;
}

// The address as a QR code in the terminal (two modules per character row, dark on light, 4-module quiet zone),
// so the headset camera can read it instead of typing 70 characters with a controller.
static void printQr(const std::string& text)
{
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int n = qr.getSize(), q = 4;
    for (int y = -q; y < n + q; y += 2)
    {
        std::string line;
        for (int x = -q; x < n + q; ++x)
        {
            const bool top = qr.getModule(x, y), bottom = qr.getModule(x, y + 1);   // false outside the symbol
            line += top && bottom ? " " : top ? "\u2584" : bottom ? "\u2580" : "\u2588";
        }
        std::printf("%s\n", line.c_str());
    }
}

// The address in big block letters (5 rows), so it can be read from across the room with the headset on the
// forehead. Digits, '.' and ':' only; anything else is left out.
static void printBig(const std::string& text)
{
    static const char* kGlyphs[12][5] = {
        {"###", "# #", "# #", "# #", "###"}, {" # ", "## ", " # ", " # ", "###"}, {"###", "  #", "###", "#  ", "###"},
        {"###", "  #", "###", "  #", "###"}, {"# #", "# #", "###", "  #", "  #"}, {"###", "#  ", "###", "  #", "###"},
        {"###", "#  ", "###", "# #", "###"}, {"###", "  #", "  #", "  #", "  #"}, {"###", "# #", "###", "# #", "###"},
        {"###", "# #", "###", "  #", "###"}, {"   ", "   ", "   ", "   ", " # "}, {"   ", " # ", "   ", " # ", "   "}};
    for (int row = 0; row < 5; ++row)
    {
        std::string line = "   ";
        for (char ch : text)
        {
            const int g = ch >= '0' && ch <= '9' ? ch - '0' : ch == '.' ? 10 : ch == ':' ? 11 : -1;
            if (g < 0) continue;
            for (const char* p = kGlyphs[g][row]; *p; ++p) line += *p == '#' ? "\u2588" : " ";
            line += " ";
        }
        std::printf("%s\n", line.c_str());
    }
}

static bool readAll(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static int serve(int argc, char** argv)
{
    Config c;
    c.web_root = argv[2];
    const int secs = argc > 3 ? std::atoi(argv[3]) : 60;
    c.encoder = argc > 4 ? argv[4] : "auto";
    if (const char* port = std::getenv("WOLFVR_PORT")) c.port = (uint16_t)std::atoi(port);
    const bool stats_lines = std::getenv("WOLFVR_STATS") != nullptr;
    // [VR BIG SCREEN 2026-11-03] WOLFVR_MODE=screen: stand in for the viewer window - a 1280x720 "desktop" with a
    // marker where the headset's pointer is, made here at 60 a second - and print every pointer/key event (IN ...).
    const bool big = std::getenv("WOLFVR_MODE") && std::string(std::getenv("WOLFVR_MODE")) == "screen";
    if (big)
    {
        c.mode = "screen";
        c.external_frames = true;
        c.width = 1280;
        c.height = 720;
        c.fps = 60;
    }
    const LanAddress lan = pickLanAddress();
    c.bind_ip = lan.ip;
    if (c.bind_ip.empty()) { std::printf("No home network found: %s\n", lan.reason.c_str()); return 1; }

    // The certificate is kept between runs, as WolfViewer keeps it in its user settings.
    std::string dir;
    if (const char* d = std::getenv("WOLFVR_STATE_DIR")) dir = d;
    else if (const char* h = std::getenv("HOME")) dir = std::string(h) + "/.config/wolf/wolfvr-test";
    else dir = ".";
    std::error_code fec;
    std::filesystem::create_directories(dir, fec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, fec);
    const std::string cert_path = dir + "/cert.pem", key_path = dir + "/key.pem";
    std::string err;
    if (!(readAll(cert_path, c.cert_pem) && readAll(key_path, c.key_pem) && certCoversIp(c.cert_pem, c.bind_ip)))
    {
        if (!makeSelfSignedCert(c.bind_ip, c.cert_pem, c.key_pem, err)) { std::printf("cert: %s\n", err.c_str()); return 1; }
        {
            std::ofstream k(key_path, std::ios::binary | std::ios::trunc);
            std::filesystem::permissions(key_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, fec);
            k << c.key_pem;
        }
        std::ofstream(cert_path, std::ios::binary | std::ios::trunc) << c.cert_pem;
    }

    // static: the stand-in frame thread (WOLFVR_MODE=screen) and the server threads use them
    static std::mutex out_mutex;
    c.log = [](LogLevel, const std::string& s) {
        std::lock_guard<std::mutex> lk(out_mutex);
        std::printf("LOG %s\n", s.c_str());
        std::fflush(stdout);
    };
    c.on_headset = [](const std::string& label, bool connected) {
        std::lock_guard<std::mutex> lk(out_mutex);
        std::printf("%s %s\n", connected ? "HEADSET CONNECTED" : "HEADSET DISCONNECTED", label.c_str());
        std::fflush(stdout);
    };
    static Service s;
    if (!s.start(c, err)) { std::printf("start failed: %s\n", err.c_str()); return 1; }
    {
        std::lock_guard<std::mutex> lk(out_mutex);
        std::printf("\nWolfViewer VR test server is on. In the headset's browser, type:\n\n");
        printBig(s.shortAddress());
        std::printf("\n   %s\n\n", s.shortAddress().c_str());
        std::printf("Same Wi-Fi as this PC. The first time, the browser warns the connection is not private:\n"
                    "choose Advanced, then Proceed. Bookmark the page for next time.\n"
                    "Help: https://wolf-grid.com/vr   Ctrl+C stops.\n\n");
        if (stats_lines) std::printf("LAN_URL %s\nUSB_URL %s\n", s.lanUrl().c_str(), s.usbUrl().c_str());
        if (std::getenv("WOLFVR_QR")) printQr(s.lanUrl());
        std::fflush(stdout);
    }
    static std::atomic<bool> frames_run{true};
    std::thread frames;
    if (big)
    {
        frames = std::thread([] {
            std::vector<uint8_t> px;
            float pu = 0.5f, pv = 0.5f;
            uint64_t n = 0;
            bool world = false;
            while (frames_run)
            {
                for (const InputEvent& e : s.takeInput())
                {
                    static const char* kNames[] = {"move", "down", "up", "wheel", "keydown", "keyup", "text", "stick", "turn", "lift"};
                    if (e.type != InputEvent::Type::MOVE)
                    {
                        std::lock_guard<std::mutex> lk(out_mutex);
                        std::printf("IN %s u=%.3f v=%.3f b=%d wheel=%d mods=%u x=%.2f y=%.2f key=%s text=%s\n", kNames[(int)e.type],
                                    e.u, e.v, e.button, e.wheel, e.mods, e.x, e.y, e.key.c_str(), e.text.c_str());
                        std::fflush(stdout);
                    }
                    pu = e.u;
                    pv = e.v;
                }
                // [VR 3D] the page's in-VR switch: world = the test pattern drawn for the head pose (as WolfViewer's
                // 3D mode draws the world for it), at 1440x1440; screen = the stand-in desktop again
                const std::string req = s.takeModeRequest();
                if (!req.empty() && (req == "world") != world)
                {
                    world = req == "world";
                    s.setMode(req, world ? 1440 : 1280, world ? 1440 : 720, true);
                    std::lock_guard<std::mutex> lk(out_mutex);
                    std::printf("MODE %s\n", req.c_str());
                    std::fflush(stdout);
                }
                if (world)
                {
                    const HeadPose hp = s.headPose();
                    const Quat q{hp.q[0], hp.q[1], hp.q[2], hp.q[3]};
                    drawTestPattern(px, 1440, 1440, 100.f, q, n++, 0);
                    s.submitFrame(std::move(px), hp, 2.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    continue;
                }
                const uint32_t W = 1280, H = 720;
                px.resize((size_t)W * H * 4);
                for (uint32_t y = 0; y < H; ++y)
                {
                    for (uint32_t x = 0; x < W; ++x)
                    {
                        uint8_t* p = &px[((size_t)y * W + x) * 4];
                        const bool grid = x % 80 == 0 || y % 80 == 0;
                        const bool top = y < 40;                          // a "menu bar", so up is obvious
                        const bool mark = std::abs((int)x - (int)(pu * W)) < 10 && std::abs((int)y - (int)(pv * H)) < 10;
                        const bool bar = y >= H - 20 && x < (n * 8) % W;   // moves each frame
                        p[0] = mark ? 255 : top ? 40 : grid ? 90 : 30;
                        p[1] = mark ? 40 : top ? 90 : grid ? 90 : bar ? 200 : 40;
                        p[2] = mark ? 40 : top ? 160 : grid ? 90 : 60;
                        p[3] = 255;
                    }
                }
                s.submitFrame(std::move(px), HeadPose(), 2.0);
                ++n;
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
    }
    bool was_connected = false;
    for (int i = 0; i < secs; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const Stats st = s.stats();
        std::lock_guard<std::mutex> lk(out_mutex);
        if (stats_lines)
        {
            std::printf("STATS {\"conn\":%d,\"enc\":\"%s\",\"fps\":%.1f,\"kbps\":%.0f,\"renderMs\":%.2f,\"encodeMs\":%.2f,"
                        "\"encodeP95\":%.2f,\"poseHz\":%.1f,\"poseLatMs\":%.2f,\"rttMs\":%.2f,\"sent\":%llu,\"skipped\":%llu,"
                        "\"hsFps\":%.1f,\"hsNewFps\":%.1f,\"hsDecodeMs\":%.2f,\"hsP2P\":%.1f,\"hsP2P95\":%.1f,\"hsV2P\":%.1f,"
                        "\"err\":\"%s\"}\n",
                        st.client_connected, st.encoder.c_str(), st.send_fps, st.kbps, st.render_ms, st.encode_ms,
                        st.encode_ms_p95, st.pose_hz, st.pose_latency_ms, st.rtt_ms,
                        (unsigned long long)st.frames_sent, (unsigned long long)st.frames_skipped, st.hs_display_fps,
                        st.hs_new_frame_fps, st.hs_decode_ms, st.hs_pose_to_photon_ms, st.hs_pose_to_photon_p95,
                        st.hs_viewer_to_photon_ms, st.last_error.c_str());
        }
        else if (st.client_connected && (!was_connected || i % 10 == 0))
        {
            // A short line every 10 s for a person: what reaches the headset and how quickly.
            std::printf("Headset: %.0f fps shown, %.0f new pictures/s, head move to picture %.0f ms, %s %.1f Mbit/s\n",
                        st.hs_display_fps, st.hs_new_frame_fps, st.hs_pose_to_photon_ms, st.encoder.c_str(),
                        st.kbps / 1000.0);
        }
        else if (!st.client_connected && was_connected)
        {
            std::printf("Headset disconnected. Still on: %s\n", s.shortAddress().c_str());
        }
        was_connected = st.client_connected;
        std::fflush(stdout);
    }
    frames_run = false;
    if (frames.joinable()) frames.join();
    s.stop();
    std::printf("STOPPED\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) return selftest();
    if (argc >= 2 && std::strcmp(argv[1], "bench264") == 0) return bench264(argc, argv);
    if (argc >= 3 && std::strcmp(argv[1], "serve") == 0) return serve(argc, argv);
    std::fprintf(stderr, "usage: %s selftest | bench264 [threads] [slices] [complexity] [convert_threads] | serve <web_root> [secs] [encoder]\n",
                 argv[0]);
    return 2;
}
