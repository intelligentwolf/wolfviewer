/**
 * @file wolfupdatemanifest.h
 * @brief WolfViewer: accept a signed update manifest and decide what the update check does. No viewer headers.
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

#ifndef WOLF_UPDATE_MANIFEST_H
#define WOLF_UPDATE_MANIFEST_H

#include <string>

#include <boost/json.hpp>
#include <openssl/evp.h>

#include "wolfupdatechannel.h"

/**
 * [BETA CHANNEL review 2026-11-12] The independent review asked whether wolfupdate.cpp's glue behaves as the tested
 * header logic does. So the glue's thinking lives here: everything checkCoro does between "the bytes arrived" and
 * "tell the resident" - the signature check, the JSON, the feed rules, the beta feed's replay rule, what to
 * remember - is decide(), which uses only the standard library, boost::json and OpenSSL, and is driven with real
 * signed manifests by indra/newview/tests/wolfupdatemanifest_harness.cpp. What is left in wolfupdate.cpp is the two
 * HTTP GETs per feed, reading and writing the settings named below, and the notices.
 */
namespace WolfUpdateManifest
{
    using WolfUpdateChannel::Entry;
    using WolfUpdateChannel::Feed;
    using WolfUpdateChannel::Verdict;

    /// The manifest signing key's public half (Ed25519, 32 bytes). The private half is
    /// ~/wolfviewer-releases/keys/manifest-ed25519.pem, generated 2026-10-02. (Moved here from wolfupdate.cpp unchanged.)
    constexpr unsigned char MANIFEST_PUBKEY[32] = {
        0x65, 0x44, 0x7d, 0x1e, 0xae, 0xf9, 0xdf, 0xa5, 0x6b, 0x2a, 0xbc, 0xdc, 0x72, 0x65, 0x77, 0x65,
        0x7c, 0x52, 0x13, 0x40, 0x4b, 0x7f, 0x28, 0x1f, 0x09, 0x84, 0xc2, 0x32, 0x78, 0x7b, 0x2e, 0xe5 };

    /// Raw Ed25519 over the manifest's exact bytes. (wolfupdate.cpp verify_signature, moved here unchanged apart from
    /// taking the key as an argument, so the harness can also run it with a key made for the test.)
    inline bool verifySignature(const std::string& message, const std::string& signature, const unsigned char* pubkey32)
    {
        if (signature.size() != 64) return false;
        bool ok = false;
        EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pubkey32, 32);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (key && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1)
        {
            ok = EVP_DigestVerify(ctx, (const unsigned char*)signature.data(), signature.size(),
                                  (const unsigned char*)message.data(), message.size()) == 1;
        }
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(key);
        return ok;
    }

    inline long long jsonNumber(const boost::json::value* v)
    {
        if (!v) return 0;
        if (v->is_int64()) return v->as_int64();
        if (v->is_uint64()) return (long long)v->as_uint64();
        if (v->is_double()) return (long long)v->as_double();
        return 0;
    }

    /// A JSON number as an int, 0 when it is absent, not a number, or outside `lo`..`hi` (never a wrapped cast).
    inline int jsonInt(const boost::json::value* v, long long lo, long long hi, bool* in_range = nullptr)
    {
        const bool is_number = v && (v->is_int64() || v->is_uint64() || v->is_double());
        long long n = 0;
        bool ok = is_number;
        if (is_number && v->is_uint64() && v->as_uint64() > 0x7fffffffffffffffULL) ok = false;
        else if (is_number && v->is_double() && !(v->as_double() >= -9.0e18 && v->as_double() <= 9.0e18)) ok = false;
        else if (is_number) n = jsonNumber(v);
        if (ok && (n < lo || n > hi)) ok = false;
        if (in_range) *in_range = ok;
        return ok ? (int)n : 0;
    }

    inline std::string jsonString(const boost::json::value* v)
    {
        return v && v->is_string() ? std::string(v->as_string().c_str()) : std::string();
    }

    /// The manifest's entry for one platform. mVerified is set by the caller's signature check, not here: this returns
    /// mVerified == true only to say "it is a JSON object"; use accept().
    inline Entry parse(const std::string& manifest, const char* platform)
    {
        Entry e;
        boost::system::error_code ec;
        const boost::json::value v = boost::json::parse(manifest, ec);
        if (ec || !v.is_object()) return e;
        const boost::json::object& m = v.as_object();
        e.mRevision = jsonInt(m.if_contains("revision"), 0, 999999);   // out of range reads as 0: nothing newer
        e.mName = jsonString(m.if_contains("release_name"));
        e.mChannel = jsonString(m.if_contains("channel"));
        e.mSequence = jsonNumber(m.if_contains("sequence"));
        if (const boost::json::value* files = m.if_contains("files"))
        {
            if (files->is_object())
            {
                if (const boost::json::value* file = files->as_object().if_contains(platform))
                {
                    if (file->is_object())
                    {
                        const boost::json::object& f = file->as_object();
                        e.mUrl = jsonString(f.if_contains("url"));
                        e.mSha256 = jsonString(f.if_contains("sha256"));
                        const long long size = jsonNumber(f.if_contains("size"));
                        e.mSize = size > 0 ? (uint64_t)size : 0;
                        for (char& c : e.mSha256) { if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a'); }
                    }
                }
            }
        }
        e.mVerified = true;
        return e;
    }

    /// THE one way a manifest is accepted, for the release feed and the beta feed alike: its signature over its exact
    /// bytes, then its JSON. mVerified is false unless both hold.
    inline Entry accept(const std::string& manifest, const std::string& signature, const unsigned char* pubkey32, const char* platform)
    {
        if (!verifySignature(manifest, signature, pubkey32)) return Entry();
        return parse(manifest, platform);
    }

    /// What wolfupdate.cpp hands to decide(): the bytes it fetched and the settings it read.
    struct Inputs
    {
        const unsigned char* mPubkey = MANIFEST_PUBKEY;
        const char* mPlatform = "";
        int mMine = 0;                      // this build's revision
        bool mStableFetched = false;        // both release files arrived
        std::string mStableManifest, mStableSignature;
        bool mFollowBeta = false;           // "Get beta versions" ticked and not a package-repository install
        bool mBetaFetched = false;          // both beta files arrived
        std::string mBetaManifest, mBetaSignature;
        bool mByHand = false;               // Help > Check for WolfViewer Updates: an earlier "Not now" no longer applies
        int mSkippedStable = 0, mSkippedBeta = 0;         // WolfViewerUpdateSkipped / ...SkippedBeta
        int mStableSeen = 0;                              // WolfViewerStableRevisionSeen
        long long mBetaSequenceSeen = 0;                  // WolfViewerBetaSequenceSeen
    };

    /// What decide() hands back: the verdict, the offer, and what to remember. Nothing here touches a setting or a file.
    struct Outcome
    {
        Verdict mVerdict = Verdict::FAILED;
        Feed mFeed = Feed::STABLE;
        Entry mOffer;                       // the entry on offer when mVerdict is OFFER
        Entry mStable, mBeta;               // as accepted (for the log)
        bool mBetaFeedFailed = false;       // the beta feed was wanted and gave no usable answer this time
        bool mBetaStale = false;            // the beta manifest was an older one than already seen: not acted on
        bool mCleanUpdates = false;         // nothing newer, on a COMPLETE answer: old downloads may go
        int mStableSeen = 0;                // new values for the two memories (equal to the inputs when unchanged)
        long long mBetaSequenceSeen = 0;
    };

    inline Outcome decide(const Inputs& in)
    {
        using namespace WolfUpdateChannel;
        Outcome o;
        o.mStableSeen = in.mStableSeen;
        o.mBetaSequenceSeen = in.mBetaSequenceSeen;

        // The release feed first, as W104: no verified release manifest, no action at all.
        if (in.mStableFetched) o.mStable = accept(in.mStableManifest, in.mStableSignature, in.mPubkey, in.mPlatform);
        if (!o.mStable.mVerified)
        {
            o.mVerdict = Verdict::FAILED;
            return o;
        }
        if (channelMatches(o.mStable, Feed::STABLE))
        {
            // The release only ever moves up, so the label's memory keeps the highest release seen: a replayed older
            // release manifest cannot make a release build call itself a beta. (We never publish a lower release
            // number after a higher one as "the release": an undo re-serves the previous manifest for a while, and
            // builds of the withdrawn number are then, correctly, still not betas.)
            if (o.mStable.mRevision > o.mStableSeen) o.mStableSeen = o.mStable.mRevision;
        }

        // The beta feed. It never legitimately goes back in time: a manifest with a lower sequence than one already
        // verified is not acted on, whatever its revision (an old beta manifest replayed after a withdrawal is refused
        // by every viewer that saw the withdrawal). The release feed needs no such rule: it carries no state that a
        // replay of an older manifest for the same release could undo, and an older release offers nothing to anyone
        // at or above it (DESIGN.md section 20).
        if (in.mFollowBeta)
        {
            if (in.mBetaFetched) o.mBeta = accept(in.mBetaManifest, in.mBetaSignature, in.mPubkey, in.mPlatform);
            if (o.mBeta.mVerified && !channelMatches(o.mBeta, Feed::BETA)) o.mBeta.mVerified = false;
            if (o.mBeta.mVerified && !feedSequenceAcceptable(o.mBeta.mSequence, in.mBetaSequenceSeen))
            {
                o.mBeta.mVerified = false;
                o.mBetaStale = true;
            }
            if (o.mBeta.mVerified && o.mBeta.mSequence > in.mBetaSequenceSeen && o.mBeta.mSequence <= 0xffffffffLL)
            {
                o.mBetaSequenceSeen = o.mBeta.mSequence;
            }
            o.mBetaFeedFailed = !o.mBeta.mVerified;
        }

        const Choice c = choose(in.mMine, in.mByHand ? 0 : in.mSkippedStable, in.mByHand ? 0 : in.mSkippedBeta,
                                o.mStable, in.mFollowBeta ? &o.mBeta : nullptr);
        o.mVerdict = c.mVerdict;
        o.mFeed = c.mFeed;
        if (c.mVerdict == Verdict::OFFER) o.mOffer = c.mFeed == Feed::BETA ? o.mBeta : o.mStable;
        // Old downloads are cleared only on a complete answer: with the beta feed wanted but unanswered, a beta
        // installer already downloaded (and not yet installed) must survive the outage.
        o.mCleanUpdates = c.mVerdict == Verdict::UP_TO_DATE && !o.mBetaFeedFailed;
        return o;
    }
}

#endif // WOLF_UPDATE_MANIFEST_H
