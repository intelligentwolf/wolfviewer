/**
 * @file wolfvrpipe.cpp
 * @brief WolfViewer VR in the headset browser, stage 1: LAN HTTPS/WebSocket server, frame thread and numbers.
 *        See wolfvrpipe.h for the design and the wire format.
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

// Boost.Asio first: on Windows it must see winsock2.h before anything pulls in windows.h. No viewer headers here.
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>

#include "wolfvrpipe.h"
#include "wolfvrdevice.h"
#include "wolfvrpipe_video.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
namespace json = boost::json;
using tcp = boost::asio::ip::tcp;

namespace wolfvr
{
// ---------------------------------------------------------------------------------------------------------------
// LAN address, certificate
// ---------------------------------------------------------------------------------------------------------------
bool isPrivateIPv4(const std::string& ip)
{
    boost::system::error_code ec;
    const auto a = net::ip::make_address_v4(ip, ec);
    if (ec) return false;
    const uint32_t v = a.to_uint();
    // RFC 1918 private ranges and RFC 3927 link-local. Carrier-grade NAT (100.64/10, Tailscale) is NOT a LAN.
    return (v >> 24) == 10 || (v >> 20) == ((172u << 4) | 1u) || (v >> 16) == ((192u << 8) | 168u) ||
           (v >> 16) == ((169u << 8) | 254u);
}

LanAddress pickLanAddress()
{
    LanAddress out;
    try
    {
        // Ask the routing table which source address the default route uses: connect() on a UDP socket only selects
        // a route, it sends nothing. 192.0.2.1 is TEST-NET-1 (RFC 5737), never a real host.
        net::io_context ioc;
        net::ip::udp::socket s(ioc);
        s.connect(net::ip::udp::endpoint(net::ip::make_address_v4("192.0.2.1"), 9));
        const std::string ip = s.local_endpoint().address().to_string();
        if (isPrivateIPv4(ip))
        {
            out.ip = ip;
            out.reason = "the network card your PC uses for the internet";
        }
        else
        {
            out.reason = ip + " is not a home-network address, so the headset stream will not start on it";
        }
    }
    catch (const std::exception& e)
    {
        out.reason = std::string("no network found (") + e.what() + ")";
    }
    return out;
}

namespace
{
    std::string bioToString(BIO* bio)
    {
        char* data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        return std::string(data, (size_t)std::max(0L, len));
    }
}

bool makeSelfSignedCert(const std::string& ip, std::string& cert_pem, std::string& key_pem, std::string& error)
{
    EVP_PKEY* pkey = EVP_EC_gen("P-256");
    if (!pkey) { error = "could not make a key"; return false; }
    X509* x = X509_new();
    bool ok = x != nullptr;
    if (ok)
    {
        X509_set_version(x, 2);   // v3
        unsigned char serial[16];
        RAND_bytes(serial, sizeof(serial));
        serial[0] &= 0x7F;
        BIGNUM* bn = BN_bin2bn(serial, sizeof(serial), nullptr);
        BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
        BN_free(bn);
        X509_gmtime_adj(X509_getm_notBefore(x), -3600);
        X509_gmtime_adj(X509_getm_notAfter(x), 365L * 24 * 3600);
        X509_set_pubkey(x, pkey);
        X509_NAME* name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char*)"WolfViewer (this PC)", -1, -1, 0);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)ip.c_str(), -1, -1, 0);
        X509_set_issuer_name(x, name);
        X509V3_CTX ctx;
        X509V3_set_ctx_nodb(&ctx);
        X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
        const std::string san = "IP:" + ip + ",DNS:localhost";
        const std::pair<int, std::string> exts[] = {
            {NID_subject_alt_name, san},
            {NID_basic_constraints, "critical,CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature"},
            {NID_ext_key_usage, "serverAuth"},
        };
        for (const auto& e : exts)
        {
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, e.first, e.second.c_str());
            if (!ext) { ok = false; break; }
            X509_add_ext(x, ext, -1);
            X509_EXTENSION_free(ext);
        }
        ok = ok && X509_sign(x, pkey, EVP_sha256()) > 0;
    }
    if (ok)
    {
        BIO* cb = BIO_new(BIO_s_mem());
        BIO* kb = BIO_new(BIO_s_mem());
        ok = PEM_write_bio_X509(cb, x) == 1 && PEM_write_bio_PrivateKey(kb, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        if (ok) { cert_pem = bioToString(cb); key_pem = bioToString(kb); }
        BIO_free(cb);
        BIO_free(kb);
    }
    if (!ok) error = "could not make the certificate";
    X509_free(x);
    EVP_PKEY_free(pkey);
    return ok;
}

bool certCoversIp(const std::string& cert_pem, const std::string& ip)
{
    if (cert_pem.empty()) return false;
    BIO* b = BIO_new_mem_buf(cert_pem.data(), (int)cert_pem.size());
    X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
    BIO_free(b);
    if (!x) return false;
    bool ok = X509_check_ip_asc(x, ip.c_str(), 0) == 1;
    if (ok)
    {
        // at least 7 days left
        time_t soon = time(nullptr) + 7 * 24 * 3600;
        ok = X509_cmp_time(X509_get0_notAfter(x), &soon) > 0;
    }
    X509_free(x);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// Service internals
// ---------------------------------------------------------------------------------------------------------------
namespace
{
    double nowMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    constexpr uint32_t kMagic = 0x31525657;   // "WVR1" little-endian
    constexpr size_t kHeaderSize = 72;        // keep in step with wolfvr.js parseHeader()

    template <typename T> void put(std::string& b, size_t off, T v) { std::memcpy(&b[off], &v, sizeof(T)); }

    struct Rolling
    {
        std::deque<double> v;
        size_t cap;
        explicit Rolling(size_t c = 120) : cap(c) {}
        void add(double x) { v.push_back(x); if (v.size() > cap) v.pop_front(); }
        double avg() const { if (v.empty()) return -1; double s = 0; for (double x : v) s += x; return s / v.size(); }
        double pct(double p) const
        {
            if (v.empty()) return -1;
            std::vector<double> s(v.begin(), v.end());
            std::sort(s.begin(), s.end());
            return s[std::min(s.size() - 1, (size_t)(p * (s.size() - 1) + 0.5))];
        }
    };

    struct Pose
    {
        uint32_t seq = 0;
        double client_ms = -1;   // page clock when stamped
        Quat q;
        bool valid = false;
    };

    std::string contentType(const std::string& path)
    {
        auto ends = [&](const char* s) {
            const size_t n = std::strlen(s);
            return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
        };
        if (ends(".html")) return "text/html; charset=utf-8";
        if (ends(".js") || ends(".mjs")) return "text/javascript; charset=utf-8";
        if (ends(".css")) return "text/css; charset=utf-8";
        if (ends(".svg")) return "image/svg+xml";
        if (ends(".png")) return "image/png";
        if (ends(".json")) return "application/json";
        if (ends(".woff2")) return "font/woff2";
        if (ends(".txt") || ends(".md")) return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    bool safeRelPath(const std::string& p)
    {
        if (p.empty() || p.size() > 200) return false;
        for (char c : p)
        {
            if (!(std::isalnum((unsigned char)c) || c == '/' || c == '.' || c == '_' || c == '-')) return false;
        }
        // no "..", no hidden files, no empty components
        std::stringstream ss(p);
        std::string part;
        while (std::getline(ss, part, '/'))
        {
            if (part.empty() || part[0] == '.') return false;
        }
        return true;
    }

    bool readFile(const std::string& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    std::string queryParam(std::string_view target, std::string_view key)
    {
        const size_t q = target.find('?');
        if (q == std::string_view::npos) return std::string();
        std::string_view rest = target.substr(q + 1);
        while (!rest.empty())
        {
            const size_t amp = rest.find('&');
            std::string_view kv = rest.substr(0, amp);
            const size_t eq = kv.find('=');
            if (eq != std::string_view::npos && kv.substr(0, eq) == key) return std::string(kv.substr(eq + 1));
            if (amp == std::string_view::npos) break;
            rest = rest.substr(amp + 1);
        }
        return std::string();
    }

    // Base class so the frame thread can talk to a TLS or a plain (USB/loopback) socket alike.
    struct Client : std::enable_shared_from_this<Client>
    {
        virtual ~Client() = default;
        virtual void send(std::shared_ptr<const std::string> msg, bool binary) = 0;   // call on the io thread
        virtual void close(uint16_t code, const char* reason) = 0;                      // call on the io thread
        std::atomic<int> pending_binary{0};   // frames queued or being written
        std::atomic<bool> webcodecs{false};
        std::atomic<bool> hello{false};
        std::string ua;                       // io thread only until hello is set
        std::string device;                   // the page's own id (localStorage), so a reload can take over
        uint64_t generation = 0;
    };
}

namespace
{
    constexpr uint16_t kPortTries = 8;   // the port and the next seven (4333-4340)

    // Who is listening on TCP `port`, for the "all ports in use" message: "name (pid N)". Linux only, and only
    // processes of this user are visible; elsewhere (or when unreadable) empty.
    std::string portOwner(uint16_t port)
    {
#if defined(__linux__)
        std::vector<std::string> inodes;
        for (const char* table : {"/proc/net/tcp", "/proc/net/tcp6"})
        {
            std::ifstream f(table);
            std::string line;
            std::getline(f, line);   // header
            while (std::getline(f, line))
            {
                // sl local_address rem_address st ... inode   (proc(5): local "HEXIP:HEXPORT", st 0A = LISTEN)
                std::istringstream ss(line);
                std::string sl, local, remote, st, txrx, trtm, retr, uid, timeout, inode;
                ss >> sl >> local >> remote >> st >> txrx >> trtm >> retr >> uid >> timeout >> inode;
                const size_t colon = local.rfind(':');
                if (colon == std::string::npos || st != "0A") continue;
                if (std::strtoul(local.c_str() + colon + 1, nullptr, 16) == port) inodes.push_back(inode);
            }
        }
        if (inodes.empty()) return std::string();
        std::error_code ec;
        for (const auto& proc : std::filesystem::directory_iterator("/proc", ec))
        {
            const std::string pid = proc.path().filename().string();
            if (pid.empty() || !std::isdigit((unsigned char)pid[0])) continue;
            std::error_code fec;
            for (const auto& fd : std::filesystem::directory_iterator(proc.path() / "fd", fec))
            {
                std::error_code lec;
                const std::string target = std::filesystem::read_symlink(fd.path(), lec).string();
                for (const std::string& ino : inodes)
                {
                    if (target == "socket:[" + ino + "]")
                    {
                        std::ifstream c(proc.path() / "comm");
                        std::string comm;
                        std::getline(c, comm);
                        return comm + " (pid " + pid + ")";
                    }
                }
            }
        }
        return "another program";
#else
        (void)port;
        return std::string();
#endif
    }
}

struct Service::Impl
{
    Config cfg;
    net::io_context ioc{1};
    std::unique_ptr<net::ssl::context> ssl;
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::thread net_thread, frame_thread;
    std::atomic<bool> stopping{false};
    std::atomic<int> http_conns{0};

    // the one connected headset; written on the io thread, read by the frame thread under client_mutex
    std::mutex client_mutex;
    std::shared_ptr<Client> client;
    uint64_t next_generation = 1;
    std::atomic<bool> key_requested{false};

    std::mutex pose_mutex;
    Pose pose;

    // [VR STAGE 2] pictures from submitFrame (external_frames)
    std::mutex ext_mutex;
    std::condition_variable ext_cv;
    std::vector<uint8_t> ext_rgba;
    HeadPose ext_pose;
    double ext_capture_ms = 0;
    bool ext_ready = false;

    // [VR BIG SCREEN] input from the page
    std::mutex input_mutex;
    std::deque<InputEvent> input;

    // [VR 3D] mode switches. cfg.mode / width / height / external_frames belong to the frame thread once it runs;
    // other threads read the size and external flag through these atomics.
    std::atomic<uint32_t> cur_w{0}, cur_h{0};
    std::atomic<bool> cur_external{false};
    std::mutex mode_mutex;
    bool mode_pending = false;
    std::string pending_mode;
    uint32_t pending_w = 0, pending_h = 0;
    bool pending_external = false;
    std::string mode_request;   // from the page

    // clock sync from the page: server_ms - client_ms
    std::atomic<double> clock_offset{0};
    std::atomic<bool> clock_synced{false};

    mutable std::mutex stats_mutex;
    Stats st;
    Rolling encode_ms{240}, render_ms{240}, pose_lat{480}, rtt{16};
    uint64_t poses_window = 0, frames_window = 0, bytes_window = 0;
    double window_start = 0, last_log = 0;

    void log(LogLevel l, const std::string& s) { if (cfg.log) cfg.log(l, s); }

    // ---- HTTP ----
    template <typename Stream>
    net::awaitable<void> serveHttp(Stream& stream, bool tls);
    template <typename Stream>
    http::message_generator respond(const http::request<http::string_body>& req, bool tls);
    template <typename Stream>
    net::awaitable<void> runWebSocket(Stream stream, http::request<http::string_body> req, std::string ip);
    net::awaitable<void> redirectToHttps(beast::tcp_stream& stream);
    net::awaitable<void> listen(tcp::acceptor& acc, bool tls);
    void onText(const std::shared_ptr<Client>& c, const std::string& text);
    void adoptClient(const std::shared_ptr<Client>& c, const std::string& label);
    std::string client_label;   // io thread: the connected headset's name, for on_headset
    void dropClient(const std::shared_ptr<Client>& c);

    // ---- frames ----
    void frameLoop();
    void sendToClient(const std::shared_ptr<Client>& c, std::string msg, bool binary);
};

namespace
{
    template <typename WsStream>
    struct WsClient : Client
    {
        WsStream ws;
        std::deque<std::pair<std::shared_ptr<const std::string>, bool>> queue;
        bool writing = false;
        explicit WsClient(WsStream&& s) : ws(std::move(s)) {}

        void send(std::shared_ptr<const std::string> msg, bool binary) override
        {
            queue.emplace_back(std::move(msg), binary);
            if (!writing) writeNext();
        }
        void writeNext()
        {
            if (queue.empty()) { writing = false; return; }
            writing = true;
            auto self = std::static_pointer_cast<WsClient>(shared_from_this());
            const bool binary = queue.front().second;
            ws.binary(binary);
            ws.async_write(net::buffer(*queue.front().first),
                           [self, binary](beast::error_code ec, size_t) {
                               if (binary) self->pending_binary--;
                               self->queue.pop_front();
                               if (ec) { self->writing = false; self->queue.clear(); return; }
                               self->writeNext();
                           });
        }
        void close(uint16_t code, const char* reason) override
        {
            auto self = std::static_pointer_cast<WsClient>(shared_from_this());
            websocket::close_reason cr(code);   // rfc6455.hpp:184; the (close_code, string_view) form takes only the enum
            cr.reason = reason;
            ws.async_close(cr, [self](beast::error_code) {});
        }
    };

    template <typename Body>
    void secureHeaders(http::response<Body>& res, bool html)
    {
        res.set(http::field::server, "WolfViewer");
        res.set("X-Content-Type-Options", "nosniff");
        res.set("Referrer-Policy", "no-referrer");
        res.set(http::field::cache_control, "no-store");
        if (html)
        {
            res.set("Content-Security-Policy",
                    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; "
                    "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
            res.set("Permissions-Policy", "xr-spatial-tracking=(self)");
        }
    }
}

template <typename Stream>
http::message_generator Service::Impl::respond(const http::request<http::string_body>& req, bool tls)
{
    auto textReply = [&](http::status s, const std::string& body) {
        http::response<http::string_body> res{s, req.version()};
        res.set(http::field::content_type, "text/html; charset=utf-8");
        secureHeaders(res, true);
        res.body() = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                     "<title>WolfViewer VR</title><body><p>" + body + "</p></body>";   // no inline style: the CSP forbids it
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return http::message_generator(std::move(res));
    };
    if (req.method() != http::verb::get && req.method() != http::verb::head)
    {
        return textReply(http::status::method_not_allowed, "Not allowed.");
    }
    const std::string_view target = req.target();
    std::string path(target.substr(0, target.find('?')));
    if (path == "/favicon.ico")
    {
        http::response<http::empty_body> res{http::status::no_content, req.version()};
        secureHeaders(res, false);
        res.keep_alive(req.keep_alive());
        return http::message_generator(std::move(res));
    }
    if (path == "/") path = "/index.html";
    const std::string rel = path.substr(1);
    if (!safeRelPath(rel))
    {
        return textReply(http::status::not_found, "Not found.");
    }
    const std::string file = cfg.web_root + "/" + rel;
    std::string body;
    bool br = false;
    const std::string_view ae = req[http::field::accept_encoding];
    if (ae.find("br") != std::string_view::npos && readFile(file + ".br", body))
    {
        br = true;
    }
    else if (!readFile(file, body))
    {
        return textReply(http::status::not_found, "Not found.");
    }
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::content_type, contentType(rel));
    if (br) res.set(http::field::content_encoding, "br");
    res.set(http::field::vary, "Accept-Encoding");
    secureHeaders(res, rel == "index.html");
    res.keep_alive(req.keep_alive());
    if (req.method() == http::verb::head) { res.content_length(body.size()); }
    else { res.body() = std::move(body); res.prepare_payload(); }
    (void)tls;
    return http::message_generator(std::move(res));
}

template <typename Stream>
net::awaitable<void> Service::Impl::serveHttp(Stream& stream, bool tls)
{
    beast::flat_buffer buffer;
    for (;;)
    {
        http::request_parser<http::string_body> parser;
        parser.header_limit(8192);
        parser.body_limit(1024);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
        co_await http::async_read(stream, buffer, parser, net::use_awaitable);
        http::request<http::string_body> req = parser.release();

        if (websocket::is_upgrade(req))
        {
            const std::string_view target = req.target();
            const bool path_ok = target.substr(0, target.find('?')) == "/ws";
            const bool id_ok = validDeviceId(queryParam(target, "d"));
            // The socket only talks to our own page: Origin must be this very host.
            const std::string origin(req[http::field::origin]);
            const std::string host(req[http::field::host]);
            const bool origin_ok = origin == std::string(tls ? "https://" : "http://") + host;
            if (!path_ok || !id_ok || !origin_ok)
            {
                log(LogLevel::WARN, std::string("VR pipe: refused a headset connection (") +
                    (!path_ok ? "path" : !id_ok ? "no device id" : "origin") + ")");
                http::response<http::string_body> res{http::status::forbidden, req.version()};
                secureHeaders(res, false);
                res.keep_alive(false);
                res.prepare_payload();
                co_await beast::async_write(stream, http::message_generator(std::move(res)), net::use_awaitable);
                co_return;
            }
            beast::get_lowest_layer(stream).expires_never();
            boost::system::error_code rec;
            const auto peer = beast::get_lowest_layer(stream).socket().remote_endpoint(rec);
            co_await runWebSocket(std::move(stream), std::move(req), rec ? std::string("?") : peer.address().to_string());
            co_return;
        }

        const bool keep = req.keep_alive();
        co_await beast::async_write(stream, respond<Stream>(req, tls), net::use_awaitable);
        if (!keep) co_return;
    }
}

template <typename Stream>
net::awaitable<void> Service::Impl::runWebSocket(Stream stream, http::request<http::string_body> req, std::string ip)
{
    using WsT = websocket::stream<Stream>;
    auto c = std::make_shared<WsClient<WsT>>(WsT(std::move(stream)));
    c->ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    c->ws.read_message_max(64 * 1024);
    c->ws.auto_fragment(false);
    const std::string device = queryParam(req.target(), "d");
    const std::string ua(req[http::field::user_agent]);
    co_await c->ws.async_accept(req, net::use_awaitable);

    // [VR SIMPLE 2026-11-03] Paul: "don't make it difficult". While VR is on, any headset on the home network comes
    // straight in: no question, no pairing. The server only listens on the LAN address and loopback, and refuses a
    // connection whose source is not a home-network (RFC 1918 / link-local) or loopback address. One headset at a
    // time: a second one is told someone is already connected, unless it is the same headset again (the page was
    // reloaded), which takes over its own old connection.
    bool allowed = ip == "127.0.0.1" || isPrivateIPv4(ip);
    if (!allowed)
    {
        log(LogLevel::WARN, "VR pipe: refused a connection from " + ip + " (not a home-network address)");
        c->close(4003, "not on this home network");
    }
    else
    {
        c->device = device;
        std::shared_ptr<Client> cur;
        {
            std::lock_guard<std::mutex> lk(client_mutex);
            cur = client;
        }
        if (cur && cur->device != device)
        {
            allowed = false;
            log(LogLevel::INFO, "VR pipe: a second headset (" + ip + ") was told someone is already connected");
            c->send(std::make_shared<const std::string>("{\"t\":\"busy\"}"), false);
            c->close(4009, "someone is already connected");
        }
        else
        {
            adoptClient(c, deviceLabelFromUserAgent(ua));
            c->send(std::make_shared<const std::string>("{\"t\":\"welcome\"}"), false);
        }
    }
    try
    {
        beast::flat_buffer buf;
        for (;;)
        {
            buf.clear();
            co_await c->ws.async_read(buf, net::use_awaitable);
            // A headset still waiting for Allow is read (to notice it leaving) but never listened to.
            if (allowed && c->ws.got_text())
            {
                onText(c, beast::buffers_to_string(buf.data()));
            }
        }
    }
    catch (const std::exception&)
    {
    }
    if (allowed) dropClient(c);
}

net::awaitable<void> Service::Impl::redirectToHttps(beast::tcp_stream& stream)
{
    // Plain HTTP on the LAN port: answer every request with a redirect to the HTTPS page and nothing else. The
    // target is built from our own address and port, never from the request's Host header.
    beast::flat_buffer buffer;
    http::request_parser<http::empty_body> parser;
    parser.header_limit(8192);
    stream.expires_after(std::chrono::seconds(15));
    co_await http::async_read(stream, buffer, parser, net::use_awaitable);
    http::response<http::string_body> res{http::status::found, parser.get().version()};
    const std::string url = "https://" + cfg.bind_ip + ":" + std::to_string(cfg.port) + "/";
    res.set(http::field::location, url);
    res.set(http::field::content_type, "text/html; charset=utf-8");
    secureHeaders(res, false);
    res.body() = "<!doctype html><meta charset=utf-8><title>WolfViewer VR</title><p><a href=\"" + url + "\">" + url +
                 "</a></p>";
    res.keep_alive(false);
    res.prepare_payload();
    co_await beast::async_write(stream, http::message_generator(std::move(res)), net::use_awaitable);
}

void Service::Impl::adoptClient(const std::shared_ptr<Client>& c, const std::string& label)
{
    client_label = label;
    if (cfg.on_headset) cfg.on_headset(label, true);
    std::shared_ptr<Client> old;
    {
        std::lock_guard<std::mutex> lk(client_mutex);
        old = client;
        c->generation = next_generation++;
        client = c;
    }
    clock_synced = false;
    if (old)
    {
        old->close(4000, "another headset took over");
    }
    std::lock_guard<std::mutex> lk(stats_mutex);
    st.client_connected = true;
    log(LogLevel::INFO, "VR pipe: headset connected");
}

void Service::Impl::dropClient(const std::shared_ptr<Client>& c)
{
    bool was = false;
    {
        std::lock_guard<std::mutex> lk(client_mutex);
        if (client == c) { client.reset(); was = true; }
    }
    if (was)
    {
        std::lock_guard<std::mutex> lk(stats_mutex);
        st.client_connected = false;
        log(LogLevel::INFO, "VR pipe: headset disconnected");
        if (cfg.on_headset) cfg.on_headset(client_label, false);
    }
}

void Service::Impl::onText(const std::shared_ptr<Client>& c, const std::string& text)
{
    const double recv_ms = nowMs();
    boost::system::error_code ec;
    json::value v = json::parse(text, ec);
    if (ec || !v.is_object()) return;
    const json::object& o = v.as_object();
    auto str = [&](const char* k) -> std::string {
        auto it = o.find(k);
        return (it != o.end() && it->value().is_string()) ? std::string(it->value().as_string()) : std::string();
    };
    auto num = [&](const char* k, double def = -1) -> double {
        auto it = o.find(k);
        if (it == o.end()) return def;
        const json::value& x = it->value();
        const double d = x.is_double() ? x.as_double() : x.is_int64() ? (double)x.as_int64()
                         : x.is_uint64() ? (double)x.as_uint64() : def;
        return std::isfinite(d) ? d : def;
    };
    const std::string t = str("t");
    if (t == "pose")
    {
        auto it = o.find("q");
        if (it == o.end() || !it->value().is_array() || it->value().as_array().size() != 4) return;
        const json::array& a = it->value().as_array();
        double q[4];
        for (int i = 0; i < 4; ++i)
        {
            const json::value& x = a[i];
            q[i] = x.is_double() ? x.as_double() : x.is_int64() ? (double)x.as_int64() : 0.0;
        }
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (!(n > 0.5 && n < 1.5)) return;
        Pose p;
        p.seq = (uint32_t)num("s", 0);
        p.client_ms = num("ct");
        p.q = Quat{(float)(q[0] / n), (float)(q[1] / n), (float)(q[2] / n), (float)(q[3] / n)};
        p.valid = true;
        {
            std::lock_guard<std::mutex> lk(pose_mutex);
            pose = p;
        }
        std::lock_guard<std::mutex> lk(stats_mutex);
        poses_window++;
        if (clock_synced && p.client_ms >= 0)
        {
            const double lat = recv_ms - (p.client_ms + clock_offset.load());
            if (lat > -50 && lat < 5000) pose_lat.add(lat);
        }
    }
    else if (t == "ping")
    {
        json::object r;
        r["t"] = "pong";
        r["ct"] = num("ct");
        r["st"] = nowMs();
        c->send(std::make_shared<const std::string>(json::serialize(r)), false);
    }
    else if (t == "sync")
    {
        // The page's best estimate (minimum round trip of the last pings): server clock minus page clock.
        const double off = num("off", NAN), r = num("rtt", NAN);
        if (std::isfinite(off) && std::isfinite(r))
        {
            clock_offset = off;
            clock_synced = true;
            std::lock_guard<std::mutex> lk(stats_mutex);
            rtt.add(r);
        }
    }
    else if (t == "hello")
    {
        auto it = o.find("webcodecs");
        c->webcodecs = it != o.end() && it->value().is_bool() && it->value().as_bool();
        c->ua = str("ua").substr(0, 200);
        {
            std::lock_guard<std::mutex> lk(stats_mutex);
            st.client_ua = c->ua;
        }
        c->hello = true;
        key_requested = true;   // (re)start the encoder for this page
    }
    else if (t == "key")
    {
        key_requested = true;
    }
    else if (t == "mode")
    {
        const std::string m = str("m");
        if (m != "screen" && m != "world") return;
        std::lock_guard<std::mutex> lk(mode_mutex);
        mode_request = m;
    }
    else if (t == "in")
    {
        // [VR BIG SCREEN] Checked here, at the edge: bounded numbers, short strings, a bounded queue.
        InputEvent e;
        const std::string ev = str("e");
        auto unit = [&](const char* k) { const double d = num(k, 0); return (float)std::clamp(d, 0.0, 1.0); };
        e.u = unit("u");
        e.v = unit("v");
        e.mods = (uint32_t)std::clamp(num("m", 0), 0.0, 7.0);
        if (ev == "move" || ev == "down" || ev == "up")
        {
            e.type = ev == "move" ? InputEvent::Type::MOVE : ev == "down" ? InputEvent::Type::DOWN : InputEvent::Type::UP;
            e.button = num("b", 0) == 2 ? 2 : 0;
        }
        else if (ev == "wheel")
        {
            e.type = InputEvent::Type::WHEEL;
            e.wheel = (int)std::clamp(num("d", 0), -10.0, 10.0);
            if (!e.wheel) return;
        }
        else if (ev == "stick")
        {
            e.type = InputEvent::Type::STICK;
            e.x = (float)std::clamp(num("x", 0), -1.0, 1.0);
            e.y = (float)std::clamp(num("y", 0), -1.0, 1.0);
        }
        else if (ev == "turn" || ev == "lift")
        {
            e.type = ev == "turn" ? InputEvent::Type::TURN : InputEvent::Type::LIFT;
            const double d = num("d", 0);
            e.wheel = d > 0.5 ? 1 : d < -0.5 ? -1 : 0;
            if (ev == "turn" && !e.wheel) return;
        }
        else if (ev == "kd" || ev == "ku")
        {
            e.type = ev == "kd" ? InputEvent::Type::KEY_DOWN : InputEvent::Type::KEY_UP;
            e.key = str("k");
            if (e.key.empty() || e.key.size() > 16) return;
        }
        else if (ev == "text")
        {
            e.type = InputEvent::Type::TEXT;
            e.text = str("s");
            if (e.text.empty() || e.text.size() > 64) return;
        }
        else
        {
            return;
        }
        std::lock_guard<std::mutex> lk(input_mutex);
        input.push_back(std::move(e));
        while (input.size() > 256) input.pop_front();
    }
    else if (t == "log")
    {
        // There is no console in a headset: the page reports its first frame error here (wolfvr.js).
        log(LogLevel::WARN, "VR pipe: headset page: " + str("msg").substr(0, 300) + " | " + str("stack").substr(0, 600));
    }
    else if (t == "stats")
    {
        std::lock_guard<std::mutex> lk(stats_mutex);
        st.hs_display_fps = num("fps");
        st.hs_new_frame_fps = num("newFps");
        st.hs_decode_ms = num("decodeMs");
        st.hs_pose_to_photon_ms = num("p2p");
        st.hs_pose_to_photon_p95 = num("p2p95");
        st.hs_viewer_to_photon_ms = num("v2p");
    }
}

net::awaitable<void> Service::Impl::listen(tcp::acceptor& acc, bool tls)
{
    auto exec = co_await net::this_coro::executor;
    for (;;)
    {
        tcp::socket sock = co_await acc.async_accept(net::use_awaitable);
        if (stopping) co_return;
        if (http_conns >= 16)   // a LAN neighbour must not be able to exhaust the viewer
        {
            boost::system::error_code ec;
            sock.close(ec);
            continue;
        }
        sock.set_option(tcp::no_delay(true));
        http_conns++;
        net::co_spawn(exec, [this, s = std::move(sock), tls]() mutable -> net::awaitable<void> {
            try
            {
                if (tls)
                {
                    // [VR PAIRING 2026-11-03] One port, two protocols: TLS starts with a handshake record (0x16),
                    // anything else is plain HTTP and only gets the redirect. Wait at most 15 s for the first byte.
                    using namespace net::experimental::awaitable_operators;
                    net::steady_timer timer(co_await net::this_coro::executor, std::chrono::seconds(15));
                    auto first = co_await (s.async_wait(tcp::socket::wait_read, net::use_awaitable) ||
                                           timer.async_wait(net::use_awaitable));
                    unsigned char b = 0;
                    boost::system::error_code pec;
                    const size_t n = first.index() == 0 ? s.receive(net::buffer(&b, 1), tcp::socket::message_peek, pec) : 0;
                    if (pec || n != 1)
                    {
                        // silent for 15 s, or closed before saying anything
                    }
                    else if (b != 0x16)
                    {
                        beast::tcp_stream plain(std::move(s));
                        co_await redirectToHttps(plain);
                    }
                    else
                    {
                        beast::ssl_stream<beast::tcp_stream> stream(beast::tcp_stream(std::move(s)), *ssl);
                        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(15));
                        co_await stream.async_handshake(net::ssl::stream_base::server, net::use_awaitable);
                        co_await serveHttp(stream, true);
                    }
                }
                else
                {
                    beast::tcp_stream stream(std::move(s));
                    co_await serveHttp(stream, false);
                }
            }
            catch (const std::exception&)
            {
                // closed, timed out, or the browser refused our certificate - all normal
            }
            http_conns--;
        }, net::detached);
    }
}

void Service::Impl::sendToClient(const std::shared_ptr<Client>& c, std::string msg, bool binary)
{
    if (binary) c->pending_binary++;
    auto m = std::make_shared<const std::string>(std::move(msg));
    net::post(ioc, [c, m, binary] { c->send(m, binary); });
}

void Service::Impl::frameLoop()
{
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000LL / std::max<uint32_t>(1, cfg.fps));
    auto next = clock::now();
    std::vector<uint8_t> rgba;
    std::unique_ptr<Encoder> enc;
    uint64_t enc_generation = 0;
    bool config_sent = false;
    uint64_t frame = 0;
    window_start = last_log = nowMs();

    while (!stopping)
    {
        {
            std::lock_guard<std::mutex> lk(mode_mutex);
            if (mode_pending)
            {
                mode_pending = false;
                cfg.mode = pending_mode;
                cfg.width = pending_w;
                cfg.height = pending_h;
                cfg.external_frames = pending_external;
                cur_w = cfg.width;
                cur_h = cfg.height;
                cur_external = cfg.external_frames;
                enc.reset();   // a new size needs a new encoder, a key frame and a new config for the page
                std::lock_guard<std::mutex> elk(ext_mutex);
                ext_ready = false;
                log(LogLevel::INFO, "VR pipe: now streaming " + cfg.mode + " " + std::to_string(cfg.width) + "x" +
                    std::to_string(cfg.height));
            }
        }
        std::shared_ptr<Client> c;
        {
            std::lock_guard<std::mutex> lk(client_mutex);
            c = client;
        }
        if (!c || !c->hello)
        {
            if (enc) { enc.reset(); enc_generation = 0; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next = clock::now();
            continue;
        }

        // A new page, or the page lost its decoder: a fresh encoder starts with a key frame and a new config.
        // exchange() first: a hello sets key_requested AND changes the generation, and that must open ONE encoder.
        const bool key_asked = key_requested.exchange(false);
        if (!enc || enc_generation != c->generation || key_asked)
        {
            enc.reset();
            std::string tried;
            enc = openEncoder(c->webcodecs ? cfg.encoder : std::string("jpeg"), cfg.width, cfg.height, cfg.fps,
                              cfg.bitrate_kbps, tried);
            enc_generation = c->generation;
            config_sent = false;
            if (!enc)
            {
                std::lock_guard<std::mutex> lk(stats_mutex);
                st.last_error = "no video encoder: " + tried;
                log(LogLevel::WARN, "VR pipe: " + st.last_error);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            log(LogLevel::INFO, "VR pipe: encoder " + enc->name() + (tried.empty() ? "" : " (skipped: " + tried + ")"));
            std::lock_guard<std::mutex> lk(stats_mutex);
            st.encoder = enc->name();
            st.codec = enc->codec();
        }

        Pose p;
        double t0 = 0, t1 = 0;
        if (cfg.external_frames)
        {
            // [VR STAGE 2] Wait (briefly, so stop and a new headset are noticed) for the viewer's next picture.
            std::unique_lock<std::mutex> lk(ext_mutex);
            if (!ext_cv.wait_for(lk, std::chrono::milliseconds(100), [this] { return ext_ready || stopping.load(); }) ||
                !ext_ready)
            {
                continue;
            }
            rgba.swap(ext_rgba);
            ext_ready = false;
            p.seq = ext_pose.seq;
            p.client_ms = ext_pose.valid ? ext_pose.client_ms : -1;
            p.q = Quat{ext_pose.q[0], ext_pose.q[1], ext_pose.q[2], ext_pose.q[3]};
            p.valid = ext_pose.valid;
            t1 = nowMs();
            t0 = t1 - ext_capture_ms;
            lk.unlock();
            if (c->pending_binary.load() >= 2)
            {
                std::lock_guard<std::mutex> slk(stats_mutex);
                st.frames_skipped++;
                continue;
            }
        }
        else
        {
            next += period;
            const auto now = clock::now();
            if (next < now - period * 2) next = now;   // fell far behind: do not try to catch up in a burst
            std::this_thread::sleep_until(next);

            // Back-pressure before encoding, never after: dropping an encoded P-frame would break the decoder.
            if (c->pending_binary.load() >= 2)
            {
                std::lock_guard<std::mutex> lk(stats_mutex);
                st.frames_skipped++;
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(pose_mutex);
                p = pose;
            }
            t0 = nowMs();
            drawTestPattern(rgba, cfg.width, cfg.height, cfg.fov_deg, p.q, frame, t0);
            t1 = nowMs();
        }
        EncodedFrame ef;
        std::string err;
        if (!enc->encode(rgba.data(), ef, err))
        {
            log(LogLevel::WARN, "VR pipe: encode failed: " + err);
            std::lock_guard<std::mutex> lk(stats_mutex);
            st.last_error = err;
            enc.reset();   // reopen on the next pass
            continue;
        }
        const double t2 = nowMs();

        if (!config_sent)
        {
            json::object cfgmsg;
            cfgmsg["t"] = "config";
            cfgmsg["codec"] = enc->codec();
            cfgmsg["codecString"] = enc->codec() == "h264" ? avcCodecString(ef.data) : std::string();
            cfgmsg["encoder"] = enc->name();
            cfgmsg["w"] = cfg.width;
            cfgmsg["h"] = cfg.height;
            cfgmsg["fov"] = cfg.fov_deg;
            cfgmsg["fps"] = cfg.fps;
            cfgmsg["mode"] = cfg.mode;
            cfgmsg["world"] = cfg.world_allowed;
            if (enc->codec() == "h264" && !ef.key)
            {
                // A decoder can only start on a key frame; this one is not, so ask again.
                key_requested = true;
                continue;
            }
            sendToClient(c, json::serialize(cfgmsg), false);
            config_sent = true;
        }

        std::string msg(kHeaderSize + ef.data.size(), '\0');
        put<uint32_t>(msg, 0, kMagic);
        put<uint32_t>(msg, 4, (uint32_t)kHeaderSize);
        put<uint64_t>(msg, 8, frame);
        put<double>(msg, 16, t0);
        put<double>(msg, 24, p.valid ? p.client_ms : -1.0);
        put<uint32_t>(msg, 32, p.seq);
        put<float>(msg, 36, p.q.x);
        put<float>(msg, 40, p.q.y);
        put<float>(msg, 44, p.q.z);
        put<float>(msg, 48, p.q.w);
        put<float>(msg, 52, cfg.fov_deg);
        put<float>(msg, 56, (float)(t1 - t0));
        put<float>(msg, 60, (float)(t2 - t1));
        msg[64] = ef.key ? 1 : 0;
        msg[65] = enc->codec() == "h264" ? 0 : 1;
        put<uint32_t>(msg, 68, (uint32_t)ef.data.size());
        std::memcpy(&msg[kHeaderSize], ef.data.data(), ef.data.size());
        const size_t bytes = ef.data.size();
        sendToClient(c, std::move(msg), true);
        frame++;

        // numbers
        std::string periodic;
        {
            std::lock_guard<std::mutex> lk(stats_mutex);
            render_ms.add(t1 - t0);
            encode_ms.add(t2 - t1);
            st.frames_sent++;
            frames_window++;
            bytes_window += bytes;
            const double now_ms = nowMs();
            const double dt = now_ms - window_start;
            if (dt >= 1000)
            {
                st.send_fps = frames_window * 1000.0 / dt;
                st.kbps = bytes_window * 8.0 / dt;   // bits per ms = kbit/s
                st.pose_hz = poses_window * 1000.0 / dt;
                st.render_ms = render_ms.avg();
                st.encode_ms = encode_ms.avg();
                st.encode_ms_p95 = encode_ms.pct(0.95);
                st.pose_latency_ms = pose_lat.avg();
                st.rtt_ms = rtt.pct(0.5);
                frames_window = bytes_window = poses_window = 0;
                window_start = now_ms;
                json::object s;
                s["t"] = "stats";
                s["fps"] = st.send_fps;
                s["kbps"] = st.kbps;
                s["renderMs"] = st.render_ms;
                s["encodeMs"] = st.encode_ms;
                s["encodeP95"] = st.encode_ms_p95;
                s["poseHz"] = st.pose_hz;
                s["poseLatMs"] = st.pose_latency_ms;
                s["skipped"] = st.frames_skipped;
                s["encoder"] = st.encoder;
                periodic = json::serialize(s);
                if (now_ms - last_log >= 10000)
                {
                    last_log = now_ms;
                    char line[512];
                    std::snprintf(line, sizeof(line),
                                  "VR pipe numbers: %s %ux%u send %.1f fps %.0f kbit/s render %.2f ms encode %.2f ms "
                                  "(p95 %.2f) poses %.1f/s pose->viewer %.1f ms rtt %.1f ms skipped %llu | headset: "
                                  "display %.1f fps new pictures %.1f fps decode %.2f ms pose->photon %.1f ms (p95 "
                                  "%.1f) viewer->photon %.1f ms",
                                  st.encoder.c_str(), cfg.width, cfg.height, st.send_fps, st.kbps, st.render_ms,
                                  st.encode_ms, st.encode_ms_p95, st.pose_hz, st.pose_latency_ms, st.rtt_ms,
                                  (unsigned long long)st.frames_skipped, st.hs_display_fps, st.hs_new_frame_fps,
                                  st.hs_decode_ms, st.hs_pose_to_photon_ms, st.hs_pose_to_photon_p95,
                                  st.hs_viewer_to_photon_ms);
                    log(LogLevel::INFO, line);
                }
            }
        }
        if (!periodic.empty()) sendToClient(c, std::move(periodic), false);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------------------------------------------
Service::Service() = default;
Service::~Service() { stop(); }

bool Service::running() const { return mImpl != nullptr; }

bool Service::start(const Config& cfg, std::string& error)
{
    stop();
    if (!isPrivateIPv4(cfg.bind_ip)) { error = cfg.bind_ip + " is not a home-network address"; return false; }
    if (cfg.width < 64 || cfg.height < 64 || cfg.width > 4096 || cfg.height > 4096 || cfg.fps < 1 || cfg.fps > 120)
    {
        error = "picture size or frame rate out of range";
        return false;
    }
    {
        std::string probe;
        if (!readFile(cfg.web_root + "/index.html", probe))
        {
            error = "the headset page is missing (" + cfg.web_root + "/index.html)";
            return false;
        }
    }
    auto impl = std::make_unique<Impl>();
    impl->cfg = cfg;
    impl->cur_w = cfg.width;
    impl->cur_h = cfg.height;
    impl->cur_external = cfg.external_frames;

    try
    {
        impl->ssl = std::make_unique<net::ssl::context>(net::ssl::context::tls_server);
        impl->ssl->set_options(net::ssl::context::default_workarounds | net::ssl::context::no_sslv2 |
                               net::ssl::context::no_sslv3 | net::ssl::context::no_tlsv1 |
                               net::ssl::context::no_tlsv1_1 | net::ssl::context::single_dh_use);
        impl->ssl->use_certificate_chain(net::buffer(cfg.cert_pem));
        impl->ssl->use_private_key(net::buffer(cfg.key_pem), net::ssl::context::pem);

    }
    catch (const std::exception& e)
    {
        error = std::string("bad certificate or key (") + e.what() + ")";
        return false;
    }
    // [2026-11-03] Paul hit "port in use" on 4333 (one of our own test servers). Try the port, then the next seven, and
    // name the program holding it if all are taken. Both listeners (LAN HTTPS and loopback HTTP) use the SAME port so
    // the USB address stays port-for-port with "adb reverse".
    std::vector<std::unique_ptr<tcp::acceptor>> accs;
    uint16_t used = 0;
    std::string busy;
    for (uint16_t p = cfg.port; p < cfg.port + kPortTries && p >= cfg.port; ++p)
    {
        accs.clear();
        boost::system::error_code ec;
        auto open = [&](const std::string& ip) {
            auto acc = std::make_unique<tcp::acceptor>(impl->ioc);
            const tcp::endpoint ep(net::ip::make_address(ip), p);
            acc->open(ep.protocol(), ec);
            if (!ec) acc->set_option(net::socket_base::reuse_address(true), ec);
            if (!ec) acc->bind(ep, ec);
            if (!ec) acc->listen(8, ec);
            if (!ec) accs.push_back(std::move(acc));
        };
        open(cfg.bind_ip);
        if (!ec && cfg.loopback_http) open("127.0.0.1");
        if (!ec) { used = p; break; }
        const std::string owner = portOwner(p);
        busy += (busy.empty() ? "" : ", ") + std::to_string(p) + (owner.empty() ? "" : " (" + owner + ")") +
                (ec == net::error::address_in_use ? "" : " [" + ec.message() + "]");
    }
    if (!used)
    {
        error = "ports " + std::to_string(cfg.port) + "-" + std::to_string(cfg.port + kPortTries - 1) +
                " are all in use: " + busy;
        return false;
    }
    if (used != cfg.port)
    {
        impl->log(LogLevel::WARN, "VR pipe: port " + std::to_string(cfg.port) + " is in use (" + busy + "), using " +
                  std::to_string(used));
    }
    impl->cfg.port = used;
    for (size_t k = 0; k < accs.size(); ++k)
    {
        impl->acceptors.push_back(std::move(accs[k]));
        const bool tls = k == 0;   // [0] = LAN HTTPS, [1] = loopback HTTP
        net::co_spawn(impl->ioc, [i = impl.get(), a = impl->acceptors.back().get(), tls]() -> net::awaitable<void> {
            try { co_await i->listen(*a, tls); } catch (const std::exception&) {}
        }, net::detached);
    }
    {
        std::lock_guard<std::mutex> lk(impl->stats_mutex);
        impl->st.running = true;
    }
    Impl* i = impl.get();
    impl->net_thread = std::thread([i] {
        for (;;)
        {
            try { i->ioc.run(); break; }
            catch (const std::exception& e) { i->log(LogLevel::WARN, std::string("VR pipe: network: ") + e.what()); }
        }
    });
    impl->frame_thread = std::thread([i] { i->frameLoop(); });
    mImpl = std::move(impl);
    const std::string port = std::to_string(mImpl->cfg.port);
    mImpl->log(LogLevel::INFO, "VR pipe: listening on https://" + cfg.bind_ip + ":" + port +
               (cfg.loopback_http ? " and http://127.0.0.1:" + port : std::string()));
    return true;
}

void Service::stop()
{
    if (!mImpl) return;
    Impl* i = mImpl.get();
    i->stopping = true;
    i->ext_cv.notify_all();
    if (i->frame_thread.joinable()) i->frame_thread.join();
    net::post(i->ioc, [i] {
        boost::system::error_code ec;
        for (auto& a : i->acceptors) a->close(ec);
        std::shared_ptr<Client> c;
        {
            std::lock_guard<std::mutex> lk(i->client_mutex);
            c = i->client;
            i->client.reset();
        }
        if (c) c->close(1001, "WolfViewer stopped the headset stream");
    });
    // Give the close frame a moment to leave, then stop everything still open.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    i->ioc.stop();
    if (i->net_thread.joinable()) i->net_thread.join();
    i->log(LogLevel::INFO, "VR pipe: stopped");
    mImpl.reset();
}

Stats Service::stats() const
{
    if (!mImpl) return Stats();
    std::lock_guard<std::mutex> lk(mImpl->stats_mutex);
    return mImpl->st;
}

uint32_t Service::frameWidth() const { return mImpl ? mImpl->cur_w.load() : 0; }
uint32_t Service::frameHeight() const { return mImpl ? mImpl->cur_h.load() : 0; }

void Service::setMode(const std::string& mode, uint32_t width, uint32_t height, bool external_frames)
{
    if (!mImpl || width < 64 || height < 64 || width > 4096 || height > 4096) return;
    std::lock_guard<std::mutex> lk(mImpl->mode_mutex);
    mImpl->mode_pending = true;
    mImpl->pending_mode = mode;
    mImpl->pending_w = width;
    mImpl->pending_h = height;
    mImpl->pending_external = external_frames;
    // Pictures of the new size are accepted at once; the frame thread switches before it takes the next one.
    mImpl->cur_w = width;
    mImpl->cur_h = height;
    mImpl->cur_external = external_frames;
}

std::string Service::takeModeRequest()
{
    if (!mImpl) return std::string();
    std::lock_guard<std::mutex> lk(mImpl->mode_mutex);
    std::string m;
    m.swap(mImpl->mode_request);
    return m;
}

uint16_t Service::port() const
{
    return mImpl ? mImpl->cfg.port : 0;
}

std::string Service::lanUrl() const
{
    if (!mImpl) return std::string();
    return "https://" + mImpl->cfg.bind_ip + ":" + std::to_string(mImpl->cfg.port) + "/";
}

std::string Service::shortAddress() const
{
    if (!mImpl) return std::string();
    return mImpl->cfg.bind_ip + ":" + std::to_string(mImpl->cfg.port);
}

HeadPose Service::headPose() const
{
    HeadPose h;
    if (!mImpl) return h;
    std::lock_guard<std::mutex> lk(mImpl->pose_mutex);
    const Pose& p = mImpl->pose;
    h.valid = p.valid;
    h.seq = p.seq;
    h.client_ms = p.client_ms;
    h.q[0] = p.q.x; h.q[1] = p.q.y; h.q[2] = p.q.z; h.q[3] = p.q.w;
    return h;
}

std::vector<InputEvent> Service::takeInput()
{
    std::vector<InputEvent> out;
    if (!mImpl) return out;
    std::lock_guard<std::mutex> lk(mImpl->input_mutex);
    out.assign(std::make_move_iterator(mImpl->input.begin()), std::make_move_iterator(mImpl->input.end()));
    mImpl->input.clear();
    return out;
}

bool Service::wantsFrames() const
{
    if (!mImpl) return false;
    std::lock_guard<std::mutex> lk(mImpl->client_mutex);
    return mImpl->client && mImpl->client->hello;
}

void Service::submitFrame(std::vector<uint8_t>&& rgba, const HeadPose& pose, double capture_ms)
{
    if (!mImpl || !mImpl->cur_external) return;
    if (rgba.size() != (size_t)mImpl->cur_w * mImpl->cur_h * 4) return;
    Impl* i = mImpl.get();
    {
        std::lock_guard<std::mutex> lk(i->ext_mutex);
        if (i->ext_ready)
        {
            std::lock_guard<std::mutex> slk(i->stats_mutex);
            i->st.frames_skipped++;   // the previous one was never encoded
        }
        i->ext_rgba.swap(rgba);
        i->ext_pose = pose;
        i->ext_capture_ms = capture_ms;
        i->ext_ready = true;
    }
    i->ext_cv.notify_one();
}

std::string Service::usbUrl() const
{
    if (!mImpl || !mImpl->cfg.loopback_http) return std::string();
    return "http://localhost:" + std::to_string(mImpl->cfg.port) + "/";
}
}
