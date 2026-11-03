/**
 * @file wolfvrpairing.cpp
 * @brief WolfViewer VR in the headset browser: the remembered headsets. See wolfvrpairing.h.
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

#include <boost/json.hpp>

#include "wolfvrpairing.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace json = boost::json;

namespace wolfvr
{
namespace
{
    const char kHex[] = "0123456789abcdef";

    std::string toHex(const unsigned char* p, size_t n)
    {
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) { s += kHex[p[i] >> 4]; s += kHex[p[i] & 15]; }
        return s;
    }

    bool isLowerHex(const std::string& s, size_t len)
    {
        if (s.size() != len) return false;
        for (char c : s)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    // Labels go into the PC's prompt and the floater list: printable ASCII only, short.
    std::string cleanLabel(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c >= 32 && c < 127) out += c;
            if (out.size() >= 60) break;
        }
        return out.empty() ? std::string("a web browser") : out;
    }
}

bool validDeviceId(const std::string& id) { return isLowerHex(id, 32); }

std::string randomHex32()
{
    unsigned char b[32];
    if (RAND_bytes(b, sizeof(b)) != 1) return std::string();
    return toHex(b, sizeof(b));
}

std::string sha256Hex(const std::string& data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr) != 1) return std::string();
    return toHex(md, len);
}

std::string deviceLabelFromUserAgent(const std::string& ua)
{
    // Only a friendly name for the question on the PC, never used to decide anything (any page can send any user
    // agent). Quest Browser user agents name the browser and the model ("...; Quest 3) ... OculusBrowser/..."), so
    // the longer model names are tried first; anything not recognised gets a plain description.
    auto has = [&](const char* s) { return ua.find(s) != std::string::npos; };
    if (has("OculusBrowser") || has("Quest"))
    {
        for (const char* q : {"Quest 3S", "Quest 3", "Quest Pro", "Quest 2"})
        {
            if (has(q)) return std::string("Meta ") + q + " browser";
        }
        return "Meta Quest browser";
    }
    if (has("PicoBrowser") || has("Pico")) return "Pico browser";
    if (has("Android")) return "Android browser";
    if (has("Firefox")) return "Firefox";
    if (has("Chrome")) return "Chrome-type browser";
    if (has("Safari")) return "Safari";
    return "a web browser";
}

DeviceStore::DeviceStore(std::string path) : mPath(std::move(path))
{
    load();
}

void DeviceStore::load()
{
    std::lock_guard<std::mutex> lk(mMutex);
    mDevices.clear();
    if (mPath.empty()) return;
    std::ifstream f(mPath, std::ios::binary);
    if (!f) return;   // no file yet: nobody allowed
    std::ostringstream ss;
    ss << f.rdbuf();
    boost::system::error_code ec;
    json::value v = json::parse(ss.str(), ec);
    if (ec || !v.is_object())
    {
        mError = "the list of allowed headsets could not be read; every headset will be asked again";
        return;
    }
    const json::object& o = v.as_object();
    auto it = o.find("devices");
    if (it == o.end() || !it->value().is_array()) return;
    for (const json::value& d : it->value().as_array())
    {
        if (!d.is_object()) continue;
        const json::object& e = d.as_object();
        auto s = [&](const char* k) {
            auto i = e.find(k);
            return (i != e.end() && i->value().is_string()) ? std::string(i->value().as_string()) : std::string();
        };
        auto n = [&](const char* k) -> int64_t {
            auto i = e.find(k);
            return (i != e.end() && i->value().is_int64()) ? i->value().as_int64() : 0;
        };
        KnownDevice k;
        k.id = s("id");
        k.label = cleanLabel(s("label"));
        k.secret_hash = s("hash");
        k.approved = n("approved");
        k.seen = n("seen");
        if (validDeviceId(k.id) && isLowerHex(k.secret_hash, 64)) mDevices[k.id] = k;
    }
}

void DeviceStore::save()
{
    if (mPath.empty()) return;
    json::array arr;
    for (const auto& [id, k] : mDevices)
    {
        json::object e;
        e["id"] = k.id;
        e["label"] = k.label;
        e["hash"] = k.secret_hash;
        e["approved"] = k.approved;
        e["seen"] = k.seen;
        arr.push_back(std::move(e));
    }
    json::object root;
    root["v"] = 1;
    root["devices"] = std::move(arr);
    // Write a new file next to it, then rename over the old one, so a crash never leaves half a list.
    const std::string tmp = mPath + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { mError = "could not save the list of allowed headsets"; return; }
#if defined(__linux__) || defined(__APPLE__)
        chmod(tmp.c_str(), 0600);   // the hashes are for this PC's user only
#endif
        f << json::serialize(root);
        if (!f.good()) { mError = "could not save the list of allowed headsets"; return; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, mPath, ec);
    if (ec)
    {
        // Windows will not rename over an existing file on every filesystem: remove it, then rename.
        std::filesystem::remove(mPath, ec);
        std::filesystem::rename(tmp, mPath, ec);
    }
    mError = ec ? "could not save the list of allowed headsets" : std::string();
}

bool DeviceStore::check(const std::string& id, const std::string& secret)
{
    if (!validDeviceId(id) || !isLowerHex(secret, 64)) return false;
    std::lock_guard<std::mutex> lk(mMutex);
    if (mDenied.count(id)) return false;
    auto it = mDevices.find(id);
    if (it == mDevices.end()) return false;
    const std::string h = sha256Hex(secret);
    if (h.size() != it->second.secret_hash.size() ||
        CRYPTO_memcmp(h.data(), it->second.secret_hash.data(), h.size()) != 0)
    {
        return false;
    }
    const int64_t now = (int64_t)time(nullptr);
    // Save "seen" at most once an hour: it is only for the floater's list.
    if (now - it->second.seen > 3600)
    {
        it->second.seen = now;
        save();
    }
    return true;
}

std::string DeviceStore::approve(const std::string& id, const std::string& label)
{
    if (!validDeviceId(id)) return std::string();
    const std::string secret = randomHex32();
    if (secret.empty()) return std::string();
    std::lock_guard<std::mutex> lk(mMutex);
    KnownDevice k;
    k.id = id;
    k.label = cleanLabel(label);
    k.secret_hash = sha256Hex(secret);
    k.approved = k.seen = (int64_t)time(nullptr);
    mDevices[id] = k;
    mDenied.erase(id);
    save();
    return secret;
}

void DeviceStore::deny(const std::string& id)
{
    std::lock_guard<std::mutex> lk(mMutex);
    mDenied.insert(id);
}

bool DeviceStore::denied(const std::string& id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return mDenied.count(id) != 0;
}

bool DeviceStore::knows(const std::string& id) const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return mDevices.count(id) != 0;
}

void DeviceStore::forgetAll()
{
    std::lock_guard<std::mutex> lk(mMutex);
    mDevices.clear();
    mDenied.clear();
    save();
}

size_t DeviceStore::count() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return mDevices.size();
}

std::vector<KnownDevice> DeviceStore::list() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    std::vector<KnownDevice> out;
    for (const auto& [id, k] : mDevices) out.push_back(k);
    return out;
}

std::string DeviceStore::lastError() const
{
    std::lock_guard<std::mutex> lk(mMutex);
    return mError;
}
}
