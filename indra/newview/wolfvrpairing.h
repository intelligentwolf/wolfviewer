/**
 * @file wolfvrpairing.h
 * @brief WolfViewer VR in the headset browser: which headsets may connect (Allow / Deny, remembered).
 *        No viewer headers: the self-test builds it on its own.
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

#ifndef WOLF_VRPAIRING_H
#define WOLF_VRPAIRING_H

// [VR PAIRING 2026-11-03] Paul, after trying the 12-letter code on his Quest 3: "you need to make it much easier to
// connect, get rid of that code, it's a job and a half to type all that in on a VR viewer". So: no code. The first time
// a headset's page connects, the PC asks "A headset wants to connect: Allow / Deny". An allowed headset is remembered:
//   - the page keeps a random device id (32 hex characters) in the headset browser's localStorage and sends it with
//     the socket (/ws?d=<id>);
//   - on Allow the PC makes a 256-bit secret and hands it to the page as an HttpOnly, Secure, SameSite=Strict cookie
//     (through a one-time /claim ticket, so the page's script never sees the secret);
//   - this store keeps id -> SHA-256 of the secret (never the secret itself), a label and two dates, in a JSON file
//     in the viewer's user settings folder (mode 0600).
// Next time the cookie and the id match and the headset goes straight in. Deny blocks that device id until the stream
// is switched off. "Forget all headsets" empties the file.

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wolfvr
{
    /** 32 lower-case hex characters (what wolfvr.js makes with crypto.getRandomValues). */
    bool validDeviceId(const std::string& id);
    /** 64 hex characters from the OpenSSL CSPRNG (32 bytes); empty if the CSPRNG failed. */
    std::string randomHex32();
    /** Lower-case hex SHA-256. */
    std::string sha256Hex(const std::string& data);
    /** A short, plain name for the prompt from the browser's user agent ("Meta Quest browser", ...). */
    std::string deviceLabelFromUserAgent(const std::string& ua);

    struct KnownDevice
    {
        std::string id;
        std::string label;
        std::string secret_hash;   // sha256Hex(secret)
        int64_t approved = 0;      // unix seconds
        int64_t seen = 0;
    };

    class DeviceStore
    {
    public:
        /** `path` empty = kept in memory only. Reads the file at once (a missing file is an empty list). */
        explicit DeviceStore(std::string path = std::string());

        /** True when `id` was allowed and `secret` is the one it was given. Updates "seen". Constant-time compare. */
        bool check(const std::string& id, const std::string& secret);
        /** Allows `id` (replacing any earlier secret) and returns the new secret, or empty when it cannot. */
        std::string approve(const std::string& id, const std::string& label);
        /** Deny: refuse this id until the store is made again (the stream is switched off). Not saved. */
        void deny(const std::string& id);
        bool denied(const std::string& id) const;
        /** True when `id` was allowed at some time (whatever secret it shows now). */
        bool knows(const std::string& id) const;
        /** Forget every allowed headset (and the denials); saves at once. */
        void forgetAll();
        size_t count() const;
        std::vector<KnownDevice> list() const;
        /** Why the last load or save failed (empty when fine). */
        std::string lastError() const;

    private:
        void load();
        void save();   // caller holds mMutex

        std::string mPath;
        mutable std::mutex mMutex;
        std::map<std::string, KnownDevice> mDevices;
        std::set<std::string> mDenied;
        std::string mError;
    };
}

#endif // WOLF_VRPAIRING_H
