/**
 * @file wolfvrcodecfetch.cpp
 * @brief WolfViewer VR pipe: first-use download of Cisco's OpenH264 binary. See wolfvrcodecfetch.h.
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

// Boost.Asio first: on Windows it must see winsock2.h before anything pulls in windows.h (bzlib.h includes it).
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "wolfvrcodecfetch.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

// bzip2 1.0.8, vendored unmodified (bzip2/SOURCES.md); it declares itself extern "C". On Windows it includes
// windows.h, which Boost.Asio has already brought in above.
#include "bzip2/bzlib.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

namespace wolfvr
{
namespace
{
    // Cisco OpenH264 v2.6.0 prebuilt binaries. File names: the v2.6.0 RELEASES file ("Binaries" section,
    // https://raw.githubusercontent.com/cisco/openh264/v2.6.0/RELEASES) and the GitHub release v2.6.0 body, which
    // list the same URLs. Downloaded 2026-11-03 from http://ciscobinary.openh264.org/<name>.bz2; every unpacked file
    // matched the MD5 in Cisco's <name>.signed.md5.txt (quoted below), then its SHA-256 and the SHA-256 of the .bz2
    // as served were computed with sha256sum. Both are pinned: the .bz2 is checked before it is unpacked, the
    // unpacked file before it is written.
    struct CiscoBinary
    {
        const char* name;          // the unpacked file name, also its name on disk
        uint64_t bz2_size;
        const char* bz2_sha256;
        uint64_t size;
        const char* sha256;
    };

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
    // MD5 ce1282f5845f56761954282e4992730d (openh264-2.6.0-win64.dll.signed.md5.txt)
    const CiscoBinary kBinary = {"openh264-2.6.0-win64.dll", 452053,
                                 "dab5f2a872777f9a58b69bfa9fbcf20d9f82f2d6ec91383fd70bff49bd34ac9f", 978520,
                                 "2076cb5675ec6c1a4c70e7a2a322552f547b6eeed649d6dfcd9e02a543b24691"};
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    // MD5 9f4d683a9af470059f31b0b7a13e7c1a (openh264-2.6.0-win-arm64.dll.signed.md5.txt)
    const CiscoBinary kBinary = {"openh264-2.6.0-win-arm64.dll", 378234,
                                 "6b57d3ecd06bd80b5f3ee1abbb31d48798ca6d89ef1bc98cf2e6d1ebddf5cf45", 810584,
                                 "fb75103938f4f47d119b983e06334df41a803bc72fb5c46e3623f6fea5782732"};
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    // MD5 dc5ee5d08b6a7290ff254ded561e14df (libopenh264-2.6.0-mac-arm64.dylib.signed.md5.txt). Note the 2.6.0 Mac
    // names have no ".8" (RELEASES v2.6.0), unlike Linux.
    const CiscoBinary kBinary = {"libopenh264-2.6.0-mac-arm64.dylib", 482124,
                                 "6db362ee5abdab572311aeadb96d3f44b0617d9a4a4b9f4db4cb5ac4d968da71", 1207136,
                                 "052e98bfcf7a9167d22f3bbb3f5988ef79065591f36af8b52924b22b13624551"};
#elif defined(__APPLE__) && defined(__x86_64__)
    // MD5 ea0ca009c3cb5f5ab8906de3993c1484 (libopenh264-2.6.0-mac-x64.dylib.signed.md5.txt)
    const CiscoBinary kBinary = {"libopenh264-2.6.0-mac-x64.dylib", 529614,
                                 "38b2ed6d1d45b6a3e408c734173f2d67ab44a10d0e154ff3489b89877cd60e7e", 1381104,
                                 "e3dc8bc01fe69363f61fd3c02fd27798537a585eadd38cd808f303d1ee505a19"};
#elif defined(__linux__) && defined(__x86_64__)
    // MD5 1859c0aaf825429cbf36f1f496c5e08c (libopenh264-2.6.0-linux64.8.so.signed.md5.txt)
    const CiscoBinary kBinary = {"libopenh264-2.6.0-linux64.8.so", 634264,
                                 "27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b", 1731128,
                                 "2f0cde7c6a6abcf5cae76942894ea42897fa677bce4ed6c91a24dd1b041d5f04"};
#elif defined(__linux__) && defined(__aarch64__)
    // MD5 9394e36085a540e34fc6e5d16929f151 (libopenh264-2.6.0-linux-arm64.8.so.signed.md5.txt)
    const CiscoBinary kBinary = {"libopenh264-2.6.0-linux-arm64.8.so", 619371,
                                 "a78aea7970150f46bcd3bb7994c9e6dd90bd7a9ea785920f5a73f6964e3fcda7", 1492120,
                                 "12e7b33623667cdab0e575170c147b1b36eadb77d0d2aa7ceb5afd3e58902140"};
#else
#define WOLFVR_NO_CISCO_BINARY 1
#endif

    constexpr const char* kHost = "ciscobinary.openh264.org";   // RELEASES "Binaries": http://ciscobinary.openh264.org/
    constexpr uint64_t kMaxDownload = 2u << 20;    // the largest .bz2 above is 634264 bytes
    constexpr uint64_t kMaxUnpacked = 4u << 20;    // the largest library above is 1731128 bytes

    std::filesystem::path pathFromUtf8(const std::string& s)
    {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    }

    std::string pathToUtf8(const std::filesystem::path& p)
    {
        const auto u = p.u8string();   // std::u8string in C++20
        return std::string(reinterpret_cast<const char*>(u.data()), u.size());
    }

    std::string sha256Hex(const uint8_t* data, size_t len)
    {
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        if (EVP_Digest(data, len, md, &md_len, EVP_sha256(), nullptr) != 1) return std::string();
        static const char* kHex = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < md_len; ++i)
        {
            out += kHex[md[i] >> 4];
            out += kHex[md[i] & 15];
        }
        return out;
    }

    bool readFile(const std::filesystem::path& p, uint64_t max, std::vector<uint8_t>& out)
    {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(p, ec);
        if (ec || size > max) return false;
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.resize((size_t)size);
        if (size) f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)size);
        return (bool)f;
    }

    // One HTTP/1.1 GET with a deadline over the whole exchange (DNS included). Body capped at kMaxDownload.
    bool httpGet(const std::string& target, std::vector<uint8_t>& body, std::string& error)
    {
        // Declared before the io_context so they outlive it: on a timeout the io_context is destroyed with the
        // request still pending.
        std::exception_ptr failure;
        bool finished = false;
        unsigned status = 0;
        net::io_context ioc;
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            auto ex = co_await net::this_coro::executor;
            tcp::resolver resolver(ex);
            beast::tcp_stream stream(ex);
            const auto endpoints = co_await resolver.async_resolve(kHost, "80", net::use_awaitable);
            stream.expires_after(std::chrono::seconds(20));
            co_await stream.async_connect(endpoints, net::use_awaitable);
            http::request<http::empty_body> req{http::verb::get, target, 11};
            req.set(http::field::host, kHost);
            req.set(http::field::user_agent, "WolfViewer");
            req.set(http::field::connection, "close");
            stream.expires_after(std::chrono::seconds(20));
            co_await http::async_write(stream, req, net::use_awaitable);
            beast::flat_buffer buffer;
            http::response_parser<http::vector_body<uint8_t>> parser;
            parser.body_limit(kMaxDownload);
            stream.expires_after(std::chrono::seconds(90));
            co_await http::async_read(stream, buffer, parser, net::use_awaitable);
            status = parser.get().result_int();
            body = std::move(parser.get().body());
            boost::system::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }, [&](std::exception_ptr e) { failure = e; finished = true; });
        ioc.run_for(std::chrono::seconds(120));
        if (!finished)
        {
            error = "Cisco's download server did not answer in time";
            return false;
        }
        if (failure)
        {
            try { std::rethrow_exception(failure); }
            catch (const boost::system::system_error& e)
            {
                error = e.code() == http::error::body_limit ? std::string("the download was far bigger than the codec")
                                                            : "could not reach Cisco's download server (" +
                                                                  e.code().message() + ")";
            }
            catch (const std::exception& e) { error = std::string("download failed (") + e.what() + ")"; }
            return false;
        }
        if (status != 200)
        {
            error = "Cisco's download server answered " + std::to_string(status) + " instead of the file";
            return false;
        }
        return true;
    }

    // bzip2 stream -> bytes, refusing anything that unpacks to more than kMaxUnpacked (bzlib.h, manual 3.3).
    bool bunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& error)
    {
        bz_stream s;
        std::memset(&s, 0, sizeof s);
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) { error = "could not start unpacking"; return false; }
        out.assign(256 * 1024, 0);
        size_t used = 0;
        s.next_in = const_cast<char*>(reinterpret_cast<const char*>(in.data()));
        s.avail_in = (unsigned int)in.size();
        int rc = BZ_OK;
        while (rc == BZ_OK)
        {
            if (used == out.size())
            {
                if (out.size() >= kMaxUnpacked) { rc = BZ_MEM_ERROR; break; }
                out.resize(std::min<size_t>(out.size() * 2, (size_t)kMaxUnpacked));
            }
            s.next_out = reinterpret_cast<char*>(out.data() + used);
            s.avail_out = (unsigned int)(out.size() - used);
            rc = BZ2_bzDecompress(&s);
            used = out.size() - s.avail_out;
            if (rc == BZ_OK && s.avail_in == 0 && s.avail_out != 0) { rc = BZ_UNEXPECTED_EOF; break; }
        }
        BZ2_bzDecompressEnd(&s);
        if (rc != BZ_STREAM_END)
        {
            error = rc == BZ_MEM_ERROR ? "the codec unpacked to far more than expected"
                                       : "the download could not be unpacked (bzip2 error " + std::to_string(rc) + ")";
            return false;
        }
        out.resize(used);
        return true;
    }

    // Writes to a temporary name in the same folder, then renames it over the final name, so a half-written file
    // never has the library's name.
    bool writeAtomically(const std::filesystem::path& dir, const std::string& name, const std::vector<uint8_t>& data,
                         std::string& error)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) { error = "could not make the folder " + pathToUtf8(dir) + " (" + ec.message() + ")"; return false; }
        unsigned char rnd[8];
        if (RAND_bytes(rnd, sizeof rnd) != 1) { error = "no random numbers for a temporary file name"; return false; }
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, ".part-%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2],
                      rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
        const std::filesystem::path tmp = dir / pathFromUtf8(name + suffix);
        const std::filesystem::path final_path = dir / pathFromUtf8(name);
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (f) f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
            f.flush();
            if (!f)
            {
                f.close();
                std::filesystem::remove(tmp, ec);
                error = "could not write in " + pathToUtf8(dir) + " (is the disk full?)";
                return false;
            }
        }
        // Owner read/write/execute (POSIX; on Windows only the read-only attribute is affected, and stays off).
        std::filesystem::permissions(tmp,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                         std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::rename(tmp, final_path, ec);   // replaces an existing file (MoveFileExW REPLACE_EXISTING)
        if (ec)
        {
            std::error_code rec;
            std::filesystem::remove(tmp, rec);
            error = "could not put the codec in place in " + pathToUtf8(dir) + " (" + ec.message() +
                    "; if the VR stream is running, restart the viewer and try again)";
            return false;
        }
        return true;
    }

    std::atomic<bool> gFetching{false};
}

bool openh264Available()
{
#if defined(WOLFVR_NO_CISCO_BINARY)
    return false;
#else
    return true;
#endif
}

std::string openh264File(const std::string& dir)
{
#if defined(WOLFVR_NO_CISCO_BINARY)
    (void)dir;
    return std::string();
#else
    return pathToUtf8(pathFromUtf8(dir) / pathFromUtf8(kBinary.name));
#endif
}

bool openh264Installed(const std::string& dir)
{
#if defined(WOLFVR_NO_CISCO_BINARY)
    (void)dir;
    return false;
#else
    std::vector<uint8_t> data;
    if (!readFile(pathFromUtf8(openh264File(dir)), kMaxUnpacked, data)) return false;
    return data.size() == kBinary.size && sha256Hex(data.data(), data.size()) == kBinary.sha256;
#endif
}

bool fetchOpenH264(const std::string& dir, std::string& message)
{
#if defined(WOLFVR_NO_CISCO_BINARY)
    (void)dir;
    message = "Cisco does not publish the OpenH264 video codec for this kind of computer.";
    return false;
#else
    if (dir.empty()) { message = "No folder was given for the video codec."; return false; }
    if (openh264Installed(dir))
    {
        message = "The OpenH264 video codec from Cisco is already installed.";
        return true;
    }
    std::vector<uint8_t> packed, lib;
    std::string err;
    if (!httpGet(std::string("/") + kBinary.name + ".bz2", packed, err))
    {
        message = "The video codec could not be downloaded: " + err + ". Check the internet connection and try again.";
        return false;
    }
    if (packed.size() != kBinary.bz2_size || sha256Hex(packed.data(), packed.size()) != kBinary.bz2_sha256)
    {
        message = "The download was not Cisco's codec file (its checksum is wrong), so it was not used. "
                  "Try again later.";
        return false;
    }
    if (!bunzip(packed, lib, err))
    {
        message = "The video codec could not be unpacked: " + err + ".";
        return false;
    }
    if (lib.size() != kBinary.size || sha256Hex(lib.data(), lib.size()) != kBinary.sha256)
    {
        message = "The unpacked codec is not Cisco's file (its checksum is wrong), so it was not used.";
        return false;
    }
    if (!writeAtomically(pathFromUtf8(dir), kBinary.name, lib, err))
    {
        message = "The video codec could not be saved: " + err + ".";
        return false;
    }
    message = std::string("The OpenH264 video codec from Cisco is installed (") + kBinary.name + ").";
    return true;
#endif
}

void fetchOpenH264Async(const std::string& dir, std::function<void(bool ok, std::string message)> done)
{
    bool expected = false;
    if (!gFetching.compare_exchange_strong(expected, true))
    {
        if (done) done(false, "The video codec is already downloading.");
        return;
    }
    std::thread([dir, done = std::move(done)]() {
        std::string message;
        bool ok = false;
        try
        {
            ok = fetchOpenH264(dir, message);
        }
        catch (const std::exception& e)
        {
            message = std::string("The video codec could not be installed (") + e.what() + ").";
        }
        gFetching = false;
        if (done) done(ok, message);
    }).detach();
}

const char* openh264AttributionText()
{
    // BINARY_LICENSE.txt condition 3, verbatim.
    return "OpenH264 Video Codec provided by Cisco Systems, Inc.";
}

const char* openh264BinaryLicenseText()
{
    // openh264/BINARY_LICENSE.txt (sha256 bd9f363c5ea11ef723d0304cddacb5273c43c0e1194097c7a045d05273635418), byte for
    // byte; the non-ASCII characters (U+2019, U+201C, U+201D, U+00A9) are written as their UTF-8 bytes. The
    // self-test compares this text with the file.
    return
        "-------------------------------------------------------\n"
        "About The Cisco-Provided Binary of OpenH264 Video Codec\n"
        "-------------------------------------------------------\n"
        "\n"
        "Cisco provides this program under the terms of the BSD license.  \n"
        "\n"
        "Additionally, this binary is licensed under Cisco\xE2""\x80""\x99""s AVC/H.264 Patent Portfolio License from MPEG LA, at no cost to you, provided that the requirements and conditions shown below in the AVC/H.264 Patent Portfolio sections are met.  \n"
        "\n"
        "As with all AVC/H.264 codecs, you may also obtain your own patent license from MPEG LA or from the individual patent owners, or proceed at your own risk.  Your rights from Cisco under the BSD license are not affected by this choice.  \n"
        "\n"
        "For more information on the OpenH264 binary licensing, please see the OpenH264 FAQ found at http://www.openh264.org/faq.html#binary \n"
        "\n"
        "A corresponding source code to this binary program is available under the same BSD terms, which can be found at http://www.openh264.org\n"
        "\n"
        "-----------\n"
        "BSD License\n"
        "-----------\n"
        "\n"
        "Copyright \xC2""\xA9"" 2014 Cisco Systems, Inc.\n"
        "\n"
        "All rights reserved.\n"
        "\n"
        "Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:\n"
        "\n"
        "1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.\n"
        "\n"
        "2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.\n"
        "\n"
        "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \xE2""\x80""\x9C""AS IS\xE2""\x80""\x9D"" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
        "\n"
        "-----------------------------------------\n"
        "AVC/H.264 Patent Portfolio License Notice\n"
        "-----------------------------------------\n"
        "\n"
        "The binary form of this Software is distributed by Cisco under the AVC/H.264 Patent Portfolio License from MPEG LA, and is subject to the following requirements, which may or may not be applicable to your use of this software: \n"
        "\n"
        "THIS PRODUCT IS LICENSED UNDER THE AVC PATENT PORTFOLIO LICENSE FOR THE PERSONAL USE OF A CONSUMER OR OTHER USES IN WHICH IT DOES NOT RECEIVE REMUNERATION TO (i) ENCODE VIDEO IN COMPLIANCE WITH THE AVC STANDARD (\xE2""\x80""\x9C""AVC VIDEO\xE2""\x80""\x9D"") AND/OR (ii) DECODE AVC VIDEO THAT WAS ENCODED BY A CONSUMER ENGAGED IN A PERSONAL ACTIVITY AND/OR WAS OBTAINED FROM A VIDEO PROVIDER LICENSED TO PROVIDE AVC VIDEO.  NO LICENSE IS GRANTED OR SHALL BE IMPLIED FOR ANY OTHER USE.  ADDITIONAL INFORMATION MAY BE OBTAINED FROM MPEG LA, L.L.C. SEE HTTP://WWW.MPEGLA.COM\n"
        "\n"
        "Accordingly, please be advised that content providers and broadcasters using AVC/H.264 in their service may be required to obtain a separate use license from MPEG LA, referred to as \"(b) sublicenses\" in the SUMMARY OF AVC/H.264 LICENSE TERMS from MPEG LA found at http://www.openh264.org/mpegla\n"
        "\n"
        "---------------------------------------------\n"
        "AVC/H.264 Patent Portfolio License Conditions\n"
        "---------------------------------------------\n"
        "\n"
        "In addition, the Cisco-provided binary of this Software is licensed under Cisco's license from MPEG LA only if the following conditions are met:\n"
        "\n"
        "1. The Cisco-provided binary is separately downloaded to an end user\xE2""\x80""\x99""s device, and not integrated into or combined with third party software prior to being downloaded to the end user\xE2""\x80""\x99""s device;\n"
        "\n"
        "2. The end user must have the ability to control (e.g., to enable, disable, or re-enable) the use of the Cisco-provided binary;\n"
        "\n"
        "3. Third party software, in the location where end users can control the use of the Cisco-provided binary, must display the following text:\n"
        "\n"
        "       \"OpenH264 Video Codec provided by Cisco Systems, Inc.\"\n"
        "\n"
        "4.  Any third-party software that makes use of the Cisco-provided binary must reproduce all of the above text, as well as this last condition, in the EULA and/or in another location where licensing information is to be presented to the end user.  \n"
        " \n"
        "\n"
        "\n"
        "                          v1.0\n";
}
}
