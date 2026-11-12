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
 * "tell the resident" - the signature check, the JSON, the feed and sequence rules, the staged rollout, what to
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
        if (const boost::json::value* r = m.if_contains("rollout"))
        {
            e.mRollout.mPresent = true;
            // "paused" is read on its own: a pause must win even when the rest of the field is missing or wrong.
            if (r->is_object())
            {
                const boost::json::value* paused = r->as_object().if_contains("paused");
                e.mRollout.mPaused = paused && paused->is_bool() && paused->as_bool();
                e.mRollout.mStarted = jsonNumber(r->as_object().if_contains("started"));
            }
            const boost::json::value* steps = r->is_object() ? r->as_object().if_contains("steps") : nullptr;
            if (!steps || !steps->is_array())
            {
                e.mRollout.mMalformed = true;
            }
            else
            {
                for (const boost::json::value& step : steps->as_array())
                {
                    bool hour_ok = false, percent_ok = false;
                    if (!step.is_array() || step.as_array().size() != 2) { e.mRollout.mMalformed = true; break; }
                    const int hour = jsonInt(&step.as_array()[0], 0, 24 * 365, &hour_ok);
                    const int percent = jsonInt(&step.as_array()[1], 0, 100, &percent_ok);
                    if (!hour_ok || !percent_ok || e.mRollout.mSteps.size() >= 16) { e.mRollout.mMalformed = true; break; }
                    e.mRollout.mSteps.emplace_back(hour, percent);
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
        bool mBetaOptIn = false;            // "Get beta versions" ticked (a package-repository install is still not staged)
        bool mByHand = false;               // Help > Check for WolfViewer Updates
        int mSkippedStable = 0, mSkippedBeta = 0;         // WolfViewerUpdateSkipped / ...SkippedBeta
        int mStableSeen = 0;                              // WolfViewerStableRevisionSeen
        int mManifestRevisionSeen = 0;                    // WolfViewerManifestRevisionSeen
        long long mManifestSequenceSeen = 0;              // WolfViewerManifestSequenceSeen
        long long mBetaSequenceSeen = 0;                  // WolfViewerBetaSequenceSeen
        int mRolloutFirstSeenRevision = 0;                // WolfViewerRolloutFirstSeenRevision
        long long mRolloutFirstSeenTime = 0;              // WolfViewerRolloutFirstSeenTime
        uint32_t mSeed = 0;                               // WolfViewerRolloutSeed
        long long mNow = 0;                               // UTC seconds
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
        int mStableSeen = 0;                // new values for the three memories (equal to the inputs when unchanged)
        int mManifestRevisionSeen = 0;
        long long mManifestSequenceSeen = 0;
        long long mBetaSequenceSeen = 0;
        int mRolloutFirstSeenRevision = 0;  // when this viewer first verified a manifest for the release (rolloutPercent's
        long long mRolloutFirstSeenTime = 0; // second clock); equal to the inputs when unchanged
        bool mStaleButCurrent = false;      // STALE, and this build is at or above the release that manifest names
        int mPercent = 100;                 // the release's rollout percent now (for the log)
        int mBucket = 0;                    // this install's place for this release (for the log)
    };

    inline Outcome decide(const Inputs& in)
    {
        using namespace WolfUpdateChannel;
        Outcome o;
        o.mStableSeen = in.mStableSeen;
        o.mManifestRevisionSeen = in.mManifestRevisionSeen;
        o.mManifestSequenceSeen = in.mManifestSequenceSeen;
        o.mBetaSequenceSeen = in.mBetaSequenceSeen;
        o.mRolloutFirstSeenRevision = in.mRolloutFirstSeenRevision;
        o.mRolloutFirstSeenTime = in.mRolloutFirstSeenTime;

        // The release feed first, as W104: no verified release manifest, no action at all.
        if (in.mStableFetched) o.mStable = accept(in.mStableManifest, in.mStableSignature, in.mPubkey, in.mPlatform);
        if (!o.mStable.mVerified)
        {
            o.mVerdict = Verdict::FAILED;
            return o;
        }
        // An older signed state of the SAME release is not acted on (a replay must not undo a pause).
        if (!sequenceAcceptable(o.mStable.mRevision, o.mStable.mSequence, in.mManifestRevisionSeen, in.mManifestSequenceSeen))
        {
            o.mVerdict = Verdict::STALE;
            o.mStaleButCurrent = in.mMine >= o.mStable.mRevision;
            return o;
        }
        if (channelMatches(o.mStable, Feed::STABLE))
        {
            // The release only ever moves up, so the label's memory keeps the highest release seen: a replayed older
            // release manifest cannot make a release build call itself a beta. (We never publish a lower release
            // number after a higher one as "the release": an undo re-serves the previous manifest for a while, and
            // builds of the withdrawn number are then, correctly, still not betas.)
            if (o.mStable.mRevision > o.mStableSeen) o.mStableSeen = o.mStable.mRevision;
            if (o.mStable.mSequence >= 0 && o.mStable.mSequence <= 0xffffffffLL
                && (o.mStable.mRevision > in.mManifestRevisionSeen
                    || (o.mStable.mRevision == in.mManifestRevisionSeen && o.mStable.mSequence > in.mManifestSequenceSeen)))
            {
                o.mManifestRevisionSeen = o.mStable.mRevision;
                o.mManifestSequenceSeen = o.mStable.mSequence;
            }
        }

        // The beta feed. It never legitimately goes back in time, so its rule is stricter than the release's: a
        // manifest with a lower sequence than one already verified is not acted on, whatever its revision (an old beta
        // manifest replayed after a withdrawal is refused by every viewer that saw the withdrawal).
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

        // The rollout's second clock: when this viewer first verified a manifest for THIS release. A new release
        // starts it; a wall clock that has gone back before it restarts it (so "since first seen" is never negative).
        if (channelMatches(o.mStable, Feed::STABLE))
        {
            if (o.mStable.mRevision != in.mRolloutFirstSeenRevision || in.mRolloutFirstSeenTime <= 0 || in.mNow < in.mRolloutFirstSeenTime)
            {
                o.mRolloutFirstSeenRevision = o.mStable.mRevision;
                o.mRolloutFirstSeenTime = in.mNow;
            }
        }

        Staging staging;
        staging.mNow = in.mNow;
        staging.mFirstSeen = o.mRolloutFirstSeenRevision == o.mStable.mRevision ? o.mRolloutFirstSeenTime : 0;
        staging.mBucket = rolloutBucket(in.mSeed, o.mStable.mRevision);
        staging.mBypass = in.mByHand || in.mBetaOptIn;
        o.mBucket = staging.mBucket;
        o.mPercent = rolloutPercent(o.mStable.mRollout, in.mNow, staging.mFirstSeen);

        const Choice c = choose(in.mMine, in.mByHand ? 0 : in.mSkippedStable, in.mByHand ? 0 : in.mSkippedBeta,
                                o.mStable, in.mFollowBeta ? &o.mBeta : nullptr, &staging);
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
