/**
 * @file wolfupdatechannel.h
 * @brief WolfViewer: which signed update feed's offer to take (release or beta). No viewer headers.
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

#ifndef WOLF_UPDATE_CHANNEL_H
#define WOLF_UPDATE_CHANNEL_H

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

/**
 * [BETA CHANNEL 2026-11-12] Paul: "a beta download and updater feed get beta versions".
 *
 * The decisions of the updater (wolfupdate.cpp checkCoro), kept free of viewer headers so that
 * indra/newview/tests/wolfupdatechannel_harness.cpp can run every case of the design's table
 * (region-terrain-work/beta-channel-2026-11-12/DESIGN.md section 7) without a viewer.
 *
 * TWO FEEDS, ONE SIGNATURE CHECK. Both manifests are verified by the same function against the same
 * compiled-in key before anything here sees them (wolfupdate.cpp verify_signature). What is added
 * for the beta feed is only on top of that:
 *  - a manifest belongs to ONE feed: the beta one says "channel": "beta" inside the signed bytes,
 *    the release one has no channel (or "stable"); each feed refuses the other's manifest;
 *  - a download URL belongs to ONE feed: the release's is the prefix, an optional "w<digits>/"
 *    folder and a file name (as W104); the beta's is the prefix, "beta/w<digits>/" and a file
 *    name. W104 and older refuse the beta shape (their folder test wants 'w' first), so a beta
 *    manifest copied over the release one on the web server moves nobody, old viewer or new.
 *
 * NEVER BACKWARDS. An offer is only ever a revision above this build's, on either feed.
 *
 * THE RELEASE FEED DECIDES AS W104 DID. With no beta entry (the box is not ticked, or this copy
 * came from a Linux package repository) choose() is W104's sequence: up to date, not usable,
 * skipped, offer.
 */
namespace WolfUpdateChannel
{
    enum class Feed { STABLE, BETA };

    /// Every download must live here: the website serves the installers from /downloads/wolfviewer/.
    constexpr const char* DOWNLOAD_PREFIX = "https://wolf-grid.com/downloads/wolfviewer/";

    /**
     * [STAGED ROLLOUT 2026-11-12] Paul: "i want a slower rollout". The release manifest's signed "rollout":
     *   "rollout": { "started": <UTC seconds>, "steps": [[<hours after started>, <percent>], ...], "paused": false }
     * A release is offered by itself only to installs whose bucket (rolloutBucket) is below the percent that applies
     * now (rolloutPercent). No "rollout" at all is everyone at once: today's manifests, and the emergency path.
     * W104 and older do not read the field and update at once; staging works for updates FROM a build that has this.
     */
    struct Rollout
    {
        bool mPresent = false;
        bool mPaused = false;
        long long mStarted = 0;                       // UTC seconds
        std::vector<std::pair<int, int>> mSteps;      // (hours after mStarted, percent)
        bool mMalformed = false;                      // the field was there but not of the shape above
    };

    /// One feed's manifest entry for this platform, after its signature verified and it parsed.
    struct Entry
    {
        bool mVerified = false;     // false: not fetched, not signed by us, or unreadable
        int mRevision = 0;
        std::string mChannel;       // the manifest's "channel", "" when it has none
        std::string mName;          // the release name, "Bouncing Panda"
        std::string mUrl;
        std::string mSha256;        // lower-case hex
        uint64_t mSize = 0;
        long long mSequence = 0;    // [STAGED ROLLOUT] the manifest's "sequence" (UTC seconds it was signed), 0 when it has none
        Rollout mRollout;           // [STAGED ROLLOUT] release feed only
    };

    /**
     * The percent of installs a release is offered to now. 0 when paused. 100 when there is no rollout, or it is
     * malformed or empty (a mistake of ours in a field only we can sign must not strand anyone). Otherwise the
     * percent of the latest step whose hour has come; before the first step's hour, 0; with a clock that is behind
     * `started`, the earliest step's percent.
     */
    inline int rolloutPercent(const Rollout& r, long long now)
    {
        if (!r.mPresent) return 100;
        if (r.mPaused) return 0;
        if (r.mMalformed || r.mSteps.empty() || r.mSteps.size() > 16) return 100;
        for (const auto& step : r.mSteps)
        {
            if (step.first < 0 || step.first > 24 * 365 || step.second < 0 || step.second > 100) return 100;
        }
        const long long elapsed = now - r.mStarted;
        int percent = 0;
        long long best_hour = -1;
        long long earliest_hour = -1;
        int earliest_percent = 0;
        for (const auto& step : r.mSteps)
        {
            if (earliest_hour < 0 || step.first < earliest_hour) { earliest_hour = step.first; earliest_percent = step.second; }
            if ((long long)step.first * 3600 <= elapsed && step.first > best_hour) { best_hour = step.first; percent = step.second; }
        }
        if (elapsed < 0) return earliest_percent;
        return percent;
    }

    /**
     * This install's place in the queue for one release, 0..99: FNV-1a (32-bit) over the install's random seed
     * (WolfViewerRolloutSeed, made once on this computer, derived from nothing, sent nowhere) and the revision, so
     * a different tenth of installs goes first each release. In the share when rolloutBucket() < rolloutPercent().
     */
    inline int rolloutBucket(uint32_t seed, int revision)
    {
        uint32_t h = 2166136261u;
        const uint32_t words[2] = { seed, (uint32_t)revision };
        for (uint32_t w : words)
        {
            for (int i = 0; i < 4; ++i)
            {
                h ^= (w >> (8 * i)) & 0xffu;
                h *= 16777619u;
            }
        }
        return (int)(h % 100u);
    }

    /**
     * May a verified release manifest be acted on, given the newest one this viewer has verified before
     * (WolfViewerManifestRevisionSeen / WolfViewerManifestSequenceSeen)? Only a manifest for the SAME revision with
     * a LOWER sequence is refused: that is an older state of the same release (a replay could otherwise undo a
     * pause). A higher revision is its own order; a lower revision is our own release undo and offers nothing to
     * anyone at or above it anyway.
     */
    inline bool sequenceAcceptable(int revision, long long sequence, int seen_revision, long long seen_sequence)
    {
        return !(revision == seen_revision && sequence < seen_sequence);
    }

    /// What the caller knows about this install and this check, for the staged rollout. See choose().
    struct Staging
    {
        long long mNow = 0;         // UTC seconds
        int mBucket = 0;            // rolloutBucket(seed, the release's revision)
        bool mBypass = false;       // "Get beta versions" is ticked, or the resident asked by hand: not staged
    };

    /// A plain file name only: the URL's last part, which becomes a path on disk.
    inline bool safeFileName(const std::string& name)
    {
        if (name.empty() || name.size() > 128 || name[0] == '.') return false;
        for (char c : name)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
                return false;
        }
        return true;
    }

    /// "w<digits>" and nothing else (publish_manifest_remote.sh / publish_beta.sh link each release there).
    inline bool releaseFolder(const std::string& folder)
    {
        return folder.size() >= 2 && folder[0] == 'w' && folder.find_first_not_of("0123456789", 1) == std::string::npos;
    }

    /// The file name of a download URL of this feed's shape, "" when the URL is not of that shape.
    inline std::string downloadFileName(const std::string& url, Feed feed)
    {
        if (url.rfind(DOWNLOAD_PREFIX, 0) != 0) return std::string();
        std::string rest = url.substr(std::strlen(DOWNLOAD_PREFIX));
        if (feed == Feed::BETA)
        {
            // prefix + "beta/" + "w<digits>/" + file name: the folder is not optional here
            if (rest.rfind("beta/", 0) != 0) return std::string();
            rest = rest.substr(5);
            const size_t slash = rest.find('/');
            if (slash == std::string::npos || !releaseFolder(rest.substr(0, slash))) return std::string();
            rest = rest.substr(slash + 1);
        }
        else
        {
            // W104's rule, unchanged: prefix + optional "w<digits>/" + file name
            const size_t slash = rest.find('/');
            if (slash != std::string::npos)
            {
                if (!releaseFolder(rest.substr(0, slash))) return std::string();
                rest = rest.substr(slash + 1);
            }
        }
        return safeFileName(rest) ? rest : std::string();
    }

    /// Is this manifest the one this feed publishes? (The signature says it is ours; this says which.)
    inline bool channelMatches(const Entry& e, Feed feed)
    {
        if (feed == Feed::BETA) return e.mChannel == "beta";
        return e.mChannel.empty() || e.mChannel == "stable";
    }

    /// W104's "is the entry usable" test, with the URL shape of the feed it came from.
    inline bool usable(const Entry& e, Feed feed)
    {
        return !downloadFileName(e.mUrl, feed).empty() && e.mSha256.size() == 64 && e.mSize != 0;
    }

    enum class Verdict
    {
        REFUSED,        // the release feed's manifest is not a release manifest: act on nothing
        UP_TO_DATE,     // nothing newer than this build
        UNUSABLE,       // a newer release, but its entry for this platform cannot be used
        SKIPPED,        // a newer release the resident said "Not now" / "Skip this version" to
        OFFER,
        PAUSED,         // [STAGED ROLLOUT] a newer release whose rollout we paused: offered to nobody, asked or not
        HELD            // [STAGED ROLLOUT] a newer release not yet offered to this install's share; ask again next start
    };

    struct Choice
    {
        Verdict mVerdict = Verdict::UP_TO_DATE;
        Feed mFeed = Feed::STABLE;     // which entry is on offer, when mVerdict is OFFER
    };

    /**
     * What to offer.
     * @param mine            this build's revision
     * @param skipped_stable  WolfViewerUpdateSkipped
     * @param skipped_beta    WolfViewerUpdateSkippedBeta
     * @param stable          the release feed's entry; the caller has checked mVerified
     * @param beta            the beta feed's entry, or nullptr when that feed is not followed
     * @param staging         [STAGED ROLLOUT] nullptr: the release's "rollout" is not looked at (W104's behaviour)
     *
     * The beta is taken only when it is above BOTH this build and the release (so a release that
     * has overtaken, or is, the beta always wins and a promoted beta is fetched from the release
     * feed), usable, and not declined. Declining a beta never hides a release, and the other way round.
     */
    inline Choice choose(int mine, int skipped_stable, int skipped_beta, const Entry& stable, const Entry* beta,
                         const Staging* staging = nullptr)
    {
        Choice c;
        if (!channelMatches(stable, Feed::STABLE))
        {
            c.mVerdict = Verdict::REFUSED;
            return c;
        }
        if (beta && beta->mVerified && channelMatches(*beta, Feed::BETA)
            && beta->mRevision > mine && beta->mRevision > stable.mRevision
            && usable(*beta, Feed::BETA) && skipped_beta < beta->mRevision)
        {
            c.mVerdict = Verdict::OFFER;
            c.mFeed = Feed::BETA;
            return c;
        }
        // From here: W104's checkCoro, in its order.
        if (stable.mRevision <= mine) c.mVerdict = Verdict::UP_TO_DATE;
        else if (!usable(stable, Feed::STABLE)) c.mVerdict = Verdict::UNUSABLE;
        else if (staging && stable.mRollout.mPresent && stable.mRollout.mPaused) c.mVerdict = Verdict::PAUSED;
        else if (skipped_stable >= stable.mRevision) c.mVerdict = Verdict::SKIPPED;
        else if (staging && !staging->mBypass && staging->mBucket >= rolloutPercent(stable.mRollout, staging->mNow)) c.mVerdict = Verdict::HELD;
        else c.mVerdict = Verdict::OFFER;
        return c;
    }

    /**
     * Is this build a beta? The word is not compiled in, because the beta that passes is promoted
     * as the very same files (DESIGN.md section 5): a build is a beta while its revision is above
     * the release in the last release manifest this viewer verified (WolfViewerStableRevisionSeen;
     * 0 = none verified yet, which is "not known", never "beta"). For display and report labels
     * only: it takes no part in choose().
     */
    inline bool isBetaBuild(int mine, int stable_seen)
    {
        return stable_seen > 0 && mine > stable_seen;
    }

    /**
     * [BETA CRASH NOTICE 2026-11-12] Paul: "if it crashes recommend them to take the normal version". Whether the
     * login screen should say "This beta version of WolfViewer closed unexpectedly last time".
     * @param is_beta           isBetaBuild()
     * @param second_instance   a second WolfViewer on this computer (the first one deals with crash folders)
     * @param newest_crash      time of the newest crash record left by THIS build (0 = none)
     * @param spoken_until      WolfViewerBetaCrashNoticeTime: the newest crash already spoken about
     * @param muted_revision    WolfViewerBetaCrashNoticeMuted: "Don't show this again for this beta" was ticked on that build
     * @param mine              this build's revision
     * @param other_notice      a Vulkan fall-back notice is being shown for this start (it explains the crash)
     * SHOW: show it. MARK: say nothing but count the crash as spoken about. NO: nothing.
     * There is deliberately no outcome that installs anything: going back to the release is the resident's own
     * download from the website, so choose() stays the only source of offers and never returns a lower revision.
     */
    enum class CrashNotice { NO, SHOW, MARK };
    inline CrashNotice betaCrashNotice(bool is_beta, bool second_instance, long long newest_crash, long long spoken_until,
                                       int muted_revision, int mine, bool other_notice)
    {
        if (!is_beta || second_instance || newest_crash <= 0 || newest_crash <= spoken_until) return CrashNotice::NO;
        if (muted_revision == mine || other_notice) return CrashNotice::MARK;
        return CrashNotice::SHOW;
    }
}

#endif // WOLF_UPDATE_CHANNEL_H
