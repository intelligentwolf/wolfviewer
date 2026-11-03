/**
 * @file wolfvrpipe.h
 * @brief WolfViewer VR in the headset browser, stage 1: the LAN pipe (HTTPS + WebSocket server, test pattern,
 *        video encoder, pose channel, latency numbers). No viewer headers: the self-test builds it on its own.
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

#ifndef WOLF_VRPIPE_H
#define WOLF_VRPIPE_H

// [VR PIPE 2026-11-03] Paul: "I only have Linux, I don't want to mess around, so make it work". The resident opens an
// address (or scans a QR code) in the headset's own browser, presses VR, and the page sends the head and controller
// poses here; this side renders (stage 1: a moving test pattern), encodes and streams the picture back; the page
// reprojects each picture to the newest head pose. Paul's hard rule: the picture and the poses stay on the LOCAL
// network. The server binds the LAN address only (never 0.0.0.0) plus a plain-HTTP loopback listener for
// "adb reverse" over USB, and only one headset is connected at a time.
// [VR SIMPLE 2026-11-03] No code, no pairing (Paul: "get rid of that code", then "don't make it difficult"): while VR
// is on, a headset on the home network connects straight in; the source address must be a home-network or loopback
// one. The LAN port answers BOTH plain HTTP and HTTPS: plain HTTP only redirects to https://<ip>:<port>/, so the
// resident types just "192.168.1.9:4333" (browsers open a typed address over http://). The first byte tells them
// apart: a TLS connection starts with a handshake record, 0x16 (RFC 8446 5.1 ContentType handshake(22)); an HTTP
// request starts with a letter of its method.
//
// Wire format (all little-endian), documented once here and mirrored in app_settings/wolfvr/wolfvr.js:
//   socket: /ws?d=<device id> (the page's own random id, so a reloaded page takes over its own old connection)
//   viewer -> page: {"t":"welcome"} first (then the page sends hello); a second, different headset gets
//     {"t":"busy"} and close 4009. Other closes: 4000 the same headset opened the page again, 4003 not on this home
//     network, 1001 the stream was switched off
//   page -> viewer, text JSON (allowed headset only):
//     {"t":"hello","webcodecs":bool,"ua":"..."}            once after connecting
//     {"t":"ping","ct":ms}                                  clock sync; answered with {"t":"pong","ct":ms,"st":ms}
//     {"t":"pose","s":seq,"ct":ms,"p":[x,y,z],"q":[x,y,z,w],"c":[...]}   every XR frame (display rate)
//     {"t":"stats",...}                                     once a second: what the headset measured
//     {"t":"key"}                                           decoder lost its place: send a key frame
//     {"t":"in","e":"move"|"down"|"up","u":0..1,"v":0..1,"b":0|2}   big screen: the pointer on the picture
//     {"t":"mode","m":"screen"|"world"}                    the in-VR switch between Big Screen and 3D
//     {"t":"in","e":"stick","x":-1..1,"y":-1..1} / {"t":"in","e":"turn","d":1|-1} / {"t":"in","e":"lift","d":1|0|-1}
//     {"t":"in","e":"wheel","d":clicks} / {"t":"in","e":"kd"|"ku","k":"<DOM key>","m":mods} / {"t":"in","e":"text","s":".."}
//   viewer -> page, text JSON:
//     {"t":"config","codec":"h264"|"jpeg","codecString":"avc1.xxxxxx","w":,"h":,"fov":deg,"fps":,"encoder":"...",
//      "mode":"test"|"screen"}
//     {"t":"pong",...}, {"t":"stats",...} (the viewer's own numbers, for the in-headset readout)
//   viewer -> page, binary: a 72-byte FrameHeader (see wolfvrpipe.cpp kHeaderSize) followed by one H.264 access
//     unit (Annex B) or one JPEG.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wolfvr
{
    enum class LogLevel { INFO, WARN };
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    // One LAN address the server can bind. Picked by pickLanAddress; the floater lists them all.
    struct LanAddress
    {
        std::string ip;          // dotted IPv4
        std::string reason;      // why it was chosen / rejected, for the floater and the log
    };

    /** The IPv4 address of the interface that carries the default route, if it is a private (RFC 1918) or
     *  link-local address. Empty when the machine is not on a private LAN (then the server refuses to start:
     *  a public address would put the stream on the internet). Portable: asks the OS routing table by
     *  connecting a UDP socket (no packet is sent). */
    LanAddress pickLanAddress();
    bool isPrivateIPv4(const std::string& ip);

    /** A self-signed EC P-256 certificate for `ip` (subjectAltName IP:ip and DNS:localhost), valid 365 days.
     *  Returns false with `error` set on failure. Stage-1 fallback only; see VR_PIPE_STAGE1.md for the
     *  per-install real certificate design. */
    bool makeSelfSignedCert(const std::string& ip, std::string& cert_pem, std::string& key_pem, std::string& error);
    /** True when cert_pem is a parseable certificate that names `ip` and has at least 7 days left. */
    bool certCoversIp(const std::string& cert_pem, const std::string& ip);

    /** The head orientation a picture is drawn for (WebXR convention, see wolfvrpipe_video.h Quat). */
    struct HeadPose
    {
        bool valid = false;
        uint32_t seq = 0;          // the page's pose number
        double client_ms = -1;     // the page's clock when it was taken
        float q[4] = {0, 0, 0, 1}; // x, y, z, w
    };

    /** [VR BIG SCREEN 2026-11-03] Mouse and keyboard from the headset page, for the viewer window. Positions are in
     *  the streamed picture, 0..1 from its top-left corner. */
    struct InputEvent
    {
        // STICK: the left thumbstick now (x right +, y back +, -1..1; WolfStorm input_manager.js _vrLocomotionFlags
        // axes), TURN: one snap turn (wheel = +1 left / -1 right), LIFT: jump/fly up or down held (wheel = +1/0/-1)
        enum class Type { MOVE, DOWN, UP, WHEEL, KEY_DOWN, KEY_UP, TEXT, STICK, TURN, LIFT };
        Type type = Type::MOVE;
        float u = 0, v = 0;
        float x = 0, y = 0;        // STICK
        int button = 0;            // 0 left, 2 right (DOM MouseEvent.button numbering)
        int wheel = 0;             // clicks, positive = towards the user (scroll down)
        uint32_t mods = 0;         // 1 shift, 2 control, 4 alt
        std::string key;           // DOM KeyboardEvent.key for KEY_DOWN / KEY_UP ("Enter", "a", ...)
        std::string text;          // TEXT: characters typed (UTF-8)
    };

    struct Config
    {
        std::string bind_ip;              // LAN address for HTTPS (pickLanAddress)
        uint16_t    port = 4333;          // the same port is used for HTTPS on the LAN and HTTP on 127.0.0.1
        bool        loopback_http = true; // http://localhost:port for "adb reverse tcp:port tcp:port"
        std::string cert_pem, key_pem;
        std::string web_root;             // app_settings/wolfvr
        // A headset connected (true) or went (false), for a notice that needs no click. Network thread; must not block.
        std::function<void(const std::string& label, bool connected)> on_headset;
        uint32_t    width = 1280, height = 1280;
        uint32_t    fps = 72;
        uint32_t    bitrate_kbps = 25000;
        float       fov_deg = 100.f;      // horizontal and vertical field of view of the streamed picture
        std::string encoder = "auto";     // auto | nvenc | va | x264 | openh264 | jpeg
        // [VR STAGE 2 2026-11-03] true: the pictures come from submitFrame (WolfViewer's own world view), not the
        // test pattern. The frame thread then encodes each newest submitted picture as it arrives.
        bool        external_frames = false;
        // [VR BIG SCREEN 2026-11-03] what the page shows: "test" (stage 1 pattern, reprojected), "screen" (the viewer
        // window on a big curved screen, controllers as the mouse), "world" (VR 3D Mode: the world seen from the head
        // pose, reprojected like the test pattern). Sent to the page in "config".
        std::string mode = "test";
        bool        world_allowed = true;   // the page may ask for "world" (sent in "config" as "world")
        LogFn       log;
    };

    // Snapshot of the numbers, viewer side and what the headset reported. -1 = not measured yet.
    struct Stats
    {
        bool   running = false;
        bool   client_connected = false;
        std::string client_ua;
        std::string encoder;               // element actually in use, e.g. "nvh264enc" or "jpeg (libjpeg)"
        std::string codec;                 // "h264" | "jpeg"
        double send_fps = -1;              // frames sent per second
        double kbps = -1;                  // payload bitrate sent
        double render_ms = -1;             // test pattern draw time (avg)
        double encode_ms = -1;             // encode time (avg) and 95th percentile
        double encode_ms_p95 = -1;
        double pose_hz = -1;               // poses received per second
        double pose_latency_ms = -1;       // pose stamped in the headset -> received here (clock-synced; avg)
        double rtt_ms = -1;                // ping round trip (median of the last 16)
        uint64_t frames_sent = 0;
        uint64_t frames_skipped = 0;       // not encoded because the socket still held the previous one
        // reported by the headset page (JSON "stats"):
        double hs_display_fps = -1;        // XR frames per second
        double hs_new_frame_fps = -1;      // decoded pictures shown per second
        double hs_decode_ms = -1;
        double hs_pose_to_photon_ms = -1;  // headset clock: pose stamped -> the picture rendered for it drawn
        double hs_pose_to_photon_p95 = -1;
        double hs_viewer_to_photon_ms = -1;// viewer capture -> drawn in the headset (clock-synced)
        std::string last_error;
    };

    class Service
    {
    public:
        Service();
        ~Service();
        Service(const Service&) = delete;
        Service& operator=(const Service&) = delete;

        /** Starts the listeners and the frame thread. False with `error` set when it cannot (port in use,
         *  bad certificate, no web root). Calling start while running restarts. */
        bool start(const Config& cfg, std::string& error);
        void stop();
        bool running() const;
        Stats stats() const;
        /** The picture size in use (Config width / height); 0 when stopped. */
        uint32_t frameWidth() const;
        uint32_t frameHeight() const;
        /** The port actually listening (the configured one, or the next free one up to +7). 0 when stopped. */
        uint16_t port() const;
        /** https://<ip>:<port>/ */
        std::string lanUrl() const;
        /** <ip>:<port> - what the resident types in the headset browser (plain http is redirected to https). */
        std::string shortAddress() const;
        /** http://localhost:<port>/ (USB / adb reverse) */
        std::string usbUrl() const;
        /** [VR STAGE 2] The newest head pose from the connected, allowed headset (valid=false when there is none).
         *  Any thread. */
        HeadPose headPose() const;
        /** [VR STAGE 2] True when an allowed headset is connected and wants pictures (so the caller only renders
         *  for it when it is useful). Any thread. */
        bool wantsFrames() const;
        /** [VR STAGE 2] One picture for the headset (external_frames): width*height*4 RGBA from Config, top row
         *  first, drawn for `pose`. Only the newest is kept: one not yet encoded when the next arrives is dropped
         *  (and counted as skipped), never half a stream. `capture_ms` = how long the caller took to make it. */
        void submitFrame(std::vector<uint8_t>&& rgba, const HeadPose& pose, double capture_ms);
        /** [VR BIG SCREEN] The mouse and keyboard events the allowed headset sent since the last call (at most 256
         *  are kept; older ones are dropped). Any thread. */
        std::vector<InputEvent> takeInput();
        /** [VR 3D 2026-11-03] Switch what is streamed without dropping the headset: the frame thread opens a new
         *  encoder at the new size and sends a new "config". Any thread. */
        void setMode(const std::string& mode, uint32_t width, uint32_t height, bool external_frames);
        /** [VR 3D] The mode the headset page asked for ("screen" / "world"), once; empty when none. Any thread. */
        std::string takeModeRequest();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif // WOLF_VRPIPE_H
