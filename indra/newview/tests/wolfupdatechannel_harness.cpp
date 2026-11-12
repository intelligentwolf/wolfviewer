/**
 * @file wolfupdatechannel_harness.cpp
 * @brief Stand-alone test of wolfupdatechannel.h (the updater's release / beta decisions).
 *
 * Not part of the CMake test set (it needs no viewer library). Build and run:
 *   g++ -std=c++17 -Wall -Wextra -Wconversion -I indra/newview \
 *       indra/newview/tests/wolfupdatechannel_harness.cpp -o ~/.cache/betachannel/wuc && ~/.cache/betachannel/wuc
 * Exit status 0 only when every case passes. Cases follow DESIGN.md section 7
 * (firestorm repo, region-terrain-work/beta-channel-2026-11-12).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#include "wolfupdatechannel.h"

#include <cstdio>

using namespace WolfUpdateChannel;

namespace
{
    int sFailed = 0, sRun = 0;

    const char* const SHA = "bf152acdd44cbdf4254a272d51ad8e619f4a2bd14f8fa7a3ac65b394b43dd9b3";
    const char* const FILE_NAME = "WolfViewer-7.2.4-linux64-OpenSim.tar.xz";

    Entry stable(int rev)
    {
        Entry e;
        e.mVerified = true;
        e.mRevision = rev;
        e.mName = "Release";
        e.mUrl = std::string(DOWNLOAD_PREFIX) + "w" + std::to_string(rev) + "/" + FILE_NAME;
        e.mSha256 = SHA;
        e.mSize = 274339244;
        return e;
    }

    Entry beta(int rev)
    {
        Entry e = stable(rev);
        e.mChannel = "beta";
        e.mName = "Beta";
        e.mUrl = std::string(DOWNLOAD_PREFIX) + "beta/w" + std::to_string(rev) + "/" + FILE_NAME;
        return e;
    }

    const char* word(Verdict v)
    {
        switch (v)
        {
        case Verdict::REFUSED: return "REFUSED";
        case Verdict::UP_TO_DATE: return "UP_TO_DATE";
        case Verdict::UNUSABLE: return "UNUSABLE";
        case Verdict::SKIPPED: return "SKIPPED";
        case Verdict::OFFER: return "OFFER";
        case Verdict::PAUSED: return "PAUSED";
        case Verdict::HELD: return "HELD";
        case Verdict::FAILED: return "FAILED";
        case Verdict::STALE: return "STALE";
        }
        return "?";
    }

    void expect(const char* what, const Choice& got, Verdict verdict, Feed feed = Feed::STABLE)
    {
        ++sRun;
        const bool ok = got.mVerdict == verdict && (verdict != Verdict::OFFER || got.mFeed == feed);
        if (!ok) ++sFailed;
        std::printf("%s  %-66s %s%s\n", ok ? "ok  " : "FAIL", what, word(got.mVerdict),
                    got.mVerdict == Verdict::OFFER ? (got.mFeed == Feed::BETA ? " beta" : " stable") : "");
    }

    void expect_true(const char* what, bool got)
    {
        ++sRun;
        if (!got) ++sFailed;
        std::printf("%s  %s\n", got ? "ok  " : "FAIL", what);
    }
}

int main()
{
    // ── the box is not ticked: beta == nullptr, W104's behaviour ─────────────────────────────
    { Entry s = stable(104); expect("off: on the release", choose(104, 0, 0, s, nullptr), Verdict::UP_TO_DATE); }
    { Entry s = stable(104); expect("off: new release", choose(103, 0, 0, s, nullptr), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(104); expect("off: release skipped", choose(103, 104, 0, s, nullptr), Verdict::SKIPPED); }
    { Entry s = stable(104); expect("off: an older skip does not hide a newer release", choose(102, 103, 0, s, nullptr), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(104); expect("off: running a beta (105), release 104: stay", choose(105, 0, 0, s, nullptr), Verdict::UP_TO_DATE); }
    { Entry s = stable(106); expect("off: running a beta (105), release 106: offered", choose(105, 0, 0, s, nullptr), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(104); s.mSha256 = "abc"; expect("off: bad sha in the entry", choose(103, 0, 0, s, nullptr), Verdict::UNUSABLE); }
    { Entry s = stable(104); s.mSize = 0; expect("off: no size", choose(103, 0, 0, s, nullptr), Verdict::UNUSABLE); }
    { Entry s = stable(104); s.mUrl = "https://evil.example/w104/x.exe"; expect("off: URL elsewhere", choose(103, 0, 0, s, nullptr), Verdict::UNUSABLE); }
    { Entry s = stable(104); s.mChannel = "stable"; expect("off: channel \"stable\" is a release manifest", choose(103, 0, 0, s, nullptr), Verdict::OFFER, Feed::STABLE); }
    // a declined beta (skipped_beta) never hides a release
    { Entry s = stable(105); expect("off: beta 106 was declined earlier, release 105 still offered", choose(104, 0, 106, s, nullptr), Verdict::OFFER, Feed::STABLE); }

    // ── attacks on a viewer that did not opt in ──────────────────────────────────────────────
    { Entry s = beta(105); expect("off: BETA manifest served as the release feed: refused", choose(104, 0, 0, s, nullptr), Verdict::REFUSED); }
    { Entry s = beta(105); s.mChannel.clear(); expect("off: beta URLs under a channel-less manifest: unusable", choose(104, 0, 0, s, nullptr), Verdict::UNUSABLE); }
    { Entry s = stable(105); s.mChannel = "nightly"; expect("off: unknown channel on the release feed: refused", choose(104, 0, 0, s, nullptr), Verdict::REFUSED); }

    // ── the box is ticked ────────────────────────────────────────────────────────────────────
    { Entry s = stable(104), b = beta(105); expect("on: beta newer", choose(104, 0, 0, s, &b), Verdict::OFFER, Feed::BETA); }
    { Entry s = stable(104), b = beta(105); expect("on: on the beta", choose(105, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(106); expect("on: next beta", choose(105, 0, 0, s, &b), Verdict::OFFER, Feed::BETA); }
    { Entry s = stable(106), b = beta(106); expect("on: promoted, I run it: nothing", choose(106, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(106), b = beta(106); expect("on: promoted, I run older: from the RELEASE feed", choose(104, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(106), b = beta(106); expect("on: promoted, I ran beta 105: from the RELEASE feed", choose(105, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(107), b = beta(106); expect("on: release overtakes my beta", choose(105, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(107), b = beta(106); expect("on: release overtakes the beta I run", choose(106, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(104), b = beta(103); expect("on: stale beta feed: never backwards", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(104); expect("on: beta feed equals my build", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(105); expect("on: running 512 (local build): nothing", choose(512, 0, 0, s, &b), Verdict::UP_TO_DATE); }

    // withdrawn beta: revision 0, no files
    { Entry s = stable(104); Entry b; b.mVerified = true; b.mChannel = "beta"; expect("on: beta withdrawn", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(105); Entry b; b.mVerified = true; b.mChannel = "beta"; expect("on: beta withdrawn, release newer", choose(104, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }

    // beta feed failed (not fetched / signature bad): the release answer stands
    { Entry s = stable(104); Entry b = beta(105); b.mVerified = false; expect("on: beta not verified: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(105); Entry b = beta(106); b.mVerified = false; expect("on: beta not verified, release newer", choose(104, 0, 0, s, &b), Verdict::OFFER, Feed::STABLE); }

    // manifests in the wrong place
    { Entry s = stable(104), b = stable(105); expect("on: RELEASE-shaped manifest served as the beta feed: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = stable(105); b.mChannel = "beta"; expect("on: beta manifest with release-shaped URLs: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(105); b.mUrl = "https://wolf-grid.com/downloads/wolfviewer/beta/../w105/x.exe"; expect("on: beta URL with ..: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(105); b.mUrl = "http://wolf-grid.com/downloads/wolfviewer/beta/w105/x.exe"; expect("on: beta URL over http: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(105); b.mSha256 = "00"; expect("on: beta entry without a sha: ignored", choose(104, 0, 0, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = beta(105), b = beta(106); expect("on: beta manifest as the release feed: refused outright", choose(104, 0, 0, s, &b), Verdict::REFUSED); }

    // declining
    { Entry s = stable(104), b = beta(105); expect("on: beta 105 declined", choose(104, 0, 105, s, &b), Verdict::UP_TO_DATE); }
    { Entry s = stable(104), b = beta(106); expect("on: 105 declined, 106 offered", choose(104, 0, 105, s, &b), Verdict::OFFER, Feed::BETA); }
    { Entry s = stable(105), b = beta(106); expect("on: beta 106 declined: release 105 still offered", choose(104, 0, 106, s, &b), Verdict::OFFER, Feed::STABLE); }
    { Entry s = stable(105), b = beta(106); expect("on: release 105 declined: beta 106 still offered", choose(104, 105, 0, s, &b), Verdict::OFFER, Feed::BETA); }
    { Entry s = stable(105), b = beta(106); expect("on: both declined", choose(104, 105, 106, s, &b), Verdict::SKIPPED); }

    // ── URL shapes ───────────────────────────────────────────────────────────────────────────
    const std::string p = DOWNLOAD_PREFIX;
    expect_true("url: release, folder", downloadFileName(p + "w104/" + FILE_NAME, Feed::STABLE) == FILE_NAME);
    expect_true("url: release, no folder (W104 accepted it)", downloadFileName(p + FILE_NAME, Feed::STABLE) == FILE_NAME);
    expect_true("url: release feed refuses beta/", downloadFileName(p + "beta/w105/" + FILE_NAME, Feed::STABLE).empty());
    expect_true("url: release feed refuses test/", downloadFileName(p + "test/" + FILE_NAME, Feed::STABLE).empty());
    expect_true("url: release feed refuses w104x/", downloadFileName(p + "w104x/" + FILE_NAME, Feed::STABLE).empty());
    expect_true("url: beta, folder", downloadFileName(p + "beta/w105/" + FILE_NAME, Feed::BETA) == FILE_NAME);
    expect_true("url: beta feed refuses a release URL", downloadFileName(p + "w105/" + FILE_NAME, Feed::BETA).empty());
    expect_true("url: beta feed refuses no folder", downloadFileName(p + "beta/" + FILE_NAME, Feed::BETA).empty());
    expect_true("url: beta feed refuses the bare name", downloadFileName(p + FILE_NAME, Feed::BETA).empty());
    expect_true("url: beta feed refuses beta/w105/sub/", downloadFileName(p + "beta/w105/sub/" + FILE_NAME, Feed::BETA).empty());
    expect_true("url: beta feed refuses beta/beta/w105/", downloadFileName(p + "beta/beta/w105/" + FILE_NAME, Feed::BETA).empty());
    expect_true("url: beta feed refuses beta/w/", downloadFileName(p + "beta/w/" + FILE_NAME, Feed::BETA).empty());
    expect_true("url: query string refused", downloadFileName(p + "w104/" + FILE_NAME + "?x=1", Feed::STABLE).empty());
    expect_true("url: dot file refused", downloadFileName(p + "w104/.htaccess", Feed::STABLE).empty());
    expect_true("url: other host refused", downloadFileName(std::string("https://wolf-grid.com.evil.example/downloads/wolfviewer/w104/") + FILE_NAME, Feed::STABLE).empty());
    expect_true("url: empty refused", downloadFileName("", Feed::BETA).empty());

    // ── the Beta label ───────────────────────────────────────────────────────────────────────
    expect_true("label: 105 with release 104 seen is a beta", isBetaBuild(105, 104));
    expect_true("label: 104 with release 104 seen is not", !isBetaBuild(104, 104));
    expect_true("label: promoted (106 seen) is not", !isBetaBuild(106, 106));
    expect_true("label: older than the release is not", !isBetaBuild(103, 104));
    expect_true("label: nothing verified yet is not known, so not a beta", !isBetaBuild(105, 0));


    // ── [STAGED ROLLOUT] percent by time ─────────────────────────────────────────────────────
    {
        const long long T = 1794480000;
        Rollout r; r.mPresent = true; r.mStarted = T; r.mSteps = { { 0, 10 }, { 24, 50 }, { 48, 100 } };
        expect_true("rollout: at the start 10%", rolloutPercent(r, T) == 10);
        expect_true("rollout: 23 h 59 min still 10%", rolloutPercent(r, T + 24 * 3600 - 60) == 10);
        expect_true("rollout: 24 h is 50%", rolloutPercent(r, T + 24 * 3600) == 50);
        expect_true("rollout: 48 h is 100%", rolloutPercent(r, T + 48 * 3600) == 100);
        expect_true("rollout: a year later still 100%", rolloutPercent(r, T + 365LL * 86400) == 100);
        expect_true("rollout: clock behind the start: the first step (10%)", rolloutPercent(r, T - 86400) == 10);
        Rollout shuffled = r; shuffled.mSteps = { { 48, 100 }, { 0, 10 }, { 24, 50 } };
        expect_true("rollout: steps out of hour order are not a plan we sign: everyone", rolloutPercent(shuffled, T + 30 * 3600) == 100);
        Rollout paused = r; paused.mPaused = true;
        expect_true("rollout: paused is 0% whatever the time", rolloutPercent(paused, T + 100 * 3600) == 0);
        Rollout none;
        expect_true("rollout: no field is everyone", rolloutPercent(none, T) == 100);
        Rollout empty = r; empty.mSteps.clear();
        expect_true("rollout: no steps is everyone", rolloutPercent(empty, T) == 100);
        Rollout bad = r; bad.mSteps = { { 0, 150 } };
        expect_true("rollout: a percent over 100 is treated as everyone", rolloutPercent(bad, T) == 100);
        Rollout mal = r; mal.mMalformed = true;
        expect_true("rollout: a malformed field is everyone", rolloutPercent(mal, T) == 100);
        Rollout late = r; late.mSteps = { { 6, 25 } };
        expect_true("rollout: before a delayed first step, nobody", rolloutPercent(late, T + 3600) == 0);
        Rollout fixed = r; fixed.mSteps = { { 0, 100 } };
        expect_true("rollout: 'everyone now' is 100% at once", rolloutPercent(fixed, T) == 100);

        // buckets: in range, stable, spread, and different per release
        int counts[100] = { 0 }; bool in_range = true; int moved = 0;
        for (uint32_t seed = 1; seed <= 20000; ++seed)
        {
            const int b = rolloutBucket(seed * 2654435761u, 105);
            if (b < 0 || b > 99) in_range = false; else ++counts[b];
            if (b != rolloutBucket(seed * 2654435761u, 106)) ++moved;
        }
        int lo = 1 << 30, hi = 0, first_ten = 0;
        for (int i = 0; i < 100; ++i) { if (counts[i] < lo) lo = counts[i]; if (counts[i] > hi) hi = counts[i]; if (i < 10) first_ten += counts[i]; }
        expect_true("bucket: always 0..99", in_range);
        expect_true("bucket: the same seed and release give the same bucket", rolloutBucket(12345u, 105) == rolloutBucket(12345u, 105));
        expect_true("bucket: 20,000 installs spread evenly (each bucket 140..260)", lo >= 140 && hi <= 260);
        expect_true("bucket: about a tenth are below 10 (1,800..2,200)", first_ten >= 1800 && first_ten <= 2200);
        expect_true("bucket: a different group goes first next release (over 95% change bucket)", moved > 19000);

        // [review 2] plans we would not sign are "everyone"; a pause wins over anything; the second clock
        Rollout down = r; down.mSteps = { { 0, 50 }, { 24, 10 } };
        expect_true("rollout: a plan whose percent goes DOWN is not valid (treated as everyone)", !rolloutPlanValid(down) && rolloutPercent(down, T) == 100);
        Rollout same_hour = r; same_hour.mSteps = { { 0, 10 }, { 0, 50 } };
        expect_true("rollout: a plan with a repeated hour is not valid", !rolloutPlanValid(same_hour));
        expect_true("rollout: hours out of order are not valid (the tools sign them in order)", !rolloutPlanValid(shuffled));
        Rollout paused_bad = bad; paused_bad.mPaused = true;
        Rollout paused_mal = mal; paused_mal.mPaused = true;
        Rollout paused_empty = empty; paused_empty.mPaused = true;
        expect_true("rollout: PAUSED wins over a bad plan, a malformed field and no steps", rolloutPercent(paused_bad, T) == 0 && rolloutPercent(paused_mal, T) == 0 && rolloutPercent(paused_empty, T) == 0);
        // a wall clock a year behind the signed start: by the first clock alone it would sit at 10% for ever
        const long long behind = T - 365LL * 86400;
        expect_true("clock behind, first seen just now: the first step (10%)", rolloutPercent(r, behind, behind) == 10);
        expect_true("clock behind, 25 hours after this viewer first saw the release: 50%", rolloutPercent(r, behind + 25 * 3600, behind) == 50);
        expect_true("clock behind, 49 hours after first seen: everyone", rolloutPercent(r, behind + 49 * 3600, behind) == 100);
        expect_true("clock right, first seen a day late: the signed start still decides (the larger elapsed time wins)", rolloutPercent(r, T + 49 * 3600, T + 24 * 3600) == 100);
        expect_true("first seen unknown and clock behind: the first step", rolloutPercent(r, behind) == 10);

        // [review 2] bucket quality: uniform within a release, and INDEPENDENT between consecutive releases.
        // Seeds as the viewer makes them (31 random bits; here a 64-bit LCG's high bits), 1,000,000 installs.
        {
            const int N = 1000000;
            static int one[100]; static int joint[100][100]; static int diff[100];
            for (int i = 0; i < 100; ++i) { one[i] = 0; diff[i] = 0; for (int j = 0; j < 100; ++j) joint[i][j] = 0; }
            unsigned long long x = 0x9E3779B97F4A7C15ULL; int same = 0;
            for (int i = 0; i < N; ++i)
            {
                x = x * 6364136223846793005ULL + 1442695040888963407ULL;
                const uint32_t seed = (uint32_t)(x >> 33);
                const int a = rolloutBucket(seed, 105), b = rolloutBucket(seed, 106);
                ++one[a]; ++joint[a][b]; ++diff[(b - a + 100) % 100];
                if (a == b) ++same;
            }
            double chi_one = 0, chi_joint = 0, chi_diff = 0;
            for (int i = 0; i < 100; ++i)
            {
                chi_one += (one[i] - N / 100.0) * (one[i] - N / 100.0) / (N / 100.0);
                chi_diff += (diff[i] - N / 100.0) * (diff[i] - N / 100.0) / (N / 100.0);
                for (int j = 0; j < 100; ++j) chi_joint += (joint[i][j] - N / 10000.0) * (joint[i][j] - N / 10000.0) / (N / 10000.0);
            }
            // of the installs in the first tenth for 105, how many are in the first tenth for 106 (independent: 10%)
            int first = 0, both = 0;
            for (int a = 0; a < 10; ++a) for (int b = 0; b < 100; ++b) { first += joint[a][b]; if (b < 10) both += joint[a][b]; }
            std::printf("      per release: chi-square %.1f (99 degrees of freedom; 0.1%% point 148.2)\n", chi_one);
            std::printf("      105 against 106, 100 x 100 cells: chi-square %.1f (9,999 d.o.f.; mean 9,999, 0.1%% point about 10,440)\n", chi_joint);
            std::printf("      shift between releases (b - a mod 100): chi-square %.1f (99 d.o.f.; 0.1%% point 148.2)\n", chi_diff);
            std::printf("      first tenth of 105 that is also first tenth of 106: %.2f%% (independent = 10%%); same place in both: %.2f%% (independent = 1%%)\n",
                        100.0 * both / first, 100.0 * same / N);
            expect_true("bucket: uniform within a release (chi-square below the 0.1% point)", chi_one < 148.2);
            expect_true("bucket: consecutive releases are independent (joint chi-square below the 0.1% point)", chi_joint < 10440.0);
            expect_true("bucket: the shift from one release to the next is uniform (no fixed offsets)", chi_diff < 148.2);
            expect_true("bucket: being first this time does not make you first next time (9..11%)", 100.0 * both / first > 9.0 && 100.0 * both / first < 11.0);
            // sequential seeds too (the worst case for a weak mixer)
            int seq[100] = { 0 }; int seq_diff[100] = { 0 };
            for (uint32_t sd = 1; sd <= 200000; ++sd) { const int a = rolloutBucket(sd, 105), b = rolloutBucket(sd, 106); ++seq[a]; ++seq_diff[(b - a + 100) % 100]; }
            double chi_seq = 0, chi_seq_diff = 0;
            for (int i = 0; i < 100; ++i) { chi_seq += (seq[i] - 2000.0) * (seq[i] - 2000.0) / 2000.0; chi_seq_diff += (seq_diff[i] - 2000.0) * (seq_diff[i] - 2000.0) / 2000.0; }
            std::printf("      sequential seeds 1..200,000: chi-square %.1f within a release, %.1f for the shift (99 d.o.f.)\n", chi_seq, chi_seq_diff);
            expect_true("bucket: sequential seeds are spread and shifted uniformly too", chi_seq < 148.2 && chi_seq_diff < 148.2);
        }

        // choose() with staging
        Entry s = stable(106); s.mRollout = r; s.mSequence = T;
        Staging in; in.mNow = T; in.mBucket = 9;
        Staging out; out.mNow = T; out.mBucket = 10;
        expect("staged: bucket 9 at 10% is offered", choose(105, 0, 0, s, nullptr, &in), Verdict::OFFER, Feed::STABLE);
        expect("staged: bucket 10 at 10% is held", choose(105, 0, 0, s, nullptr, &out), Verdict::HELD);
        Staging day2 = out; day2.mNow = T + 24 * 3600;
        expect("staged: bucket 10 a day later (50%) is offered", choose(105, 0, 0, s, nullptr, &day2), Verdict::OFFER, Feed::STABLE);
        Staging last; last.mNow = T + 48 * 3600; last.mBucket = 99;
        expect("staged: bucket 99 after two days is offered", choose(105, 0, 0, s, nullptr, &last), Verdict::OFFER, Feed::STABLE);
        Staging hand = out; hand.mBypass = true;
        expect("staged: asked by hand / beta box ticked: offered at once", choose(105, 0, 0, s, nullptr, &hand), Verdict::OFFER, Feed::STABLE);
        expect("staged: W104-style caller (no staging) is offered at once", choose(105, 0, 0, s, nullptr), Verdict::OFFER, Feed::STABLE);
        expect("staged: already on it: nothing", choose(106, 0, 0, s, nullptr, &out), Verdict::UP_TO_DATE);
        Entry p = s; p.mRollout.mPaused = true;
        expect("paused: in the share: not offered", choose(105, 0, 0, p, nullptr, &in), Verdict::PAUSED);
        expect("paused: asked by hand: still not offered", choose(105, 0, 0, p, nullptr, &hand), Verdict::PAUSED);
        expect("paused: already on it: nothing to say", choose(106, 0, 0, p, nullptr, &in), Verdict::UP_TO_DATE);
        Entry b107 = beta(107);
        expect("paused release, newer beta, box ticked: the beta is still offered", choose(105, 0, 0, p, &b107, &hand), Verdict::OFFER, Feed::BETA);
        Entry plain = stable(106);
        expect("no rollout field (emergency / old tooling): everyone at once", choose(105, 0, 0, plain, nullptr, &out), Verdict::OFFER, Feed::STABLE);
        expect("staged: a skipped release stays skipped", choose(105, 106, 0, s, nullptr, &in), Verdict::SKIPPED);
        Entry promoted_beta = beta(106);
        expect("staged: beta tester on 105, 106 promoted at 10%: offered at once from the release feed", choose(105, 0, 0, s, &promoted_beta, &hand), Verdict::OFFER, Feed::STABLE);

        // sequence memory (replay of an older state of the same release)
        expect_true("sequence: first manifest ever", sequenceAcceptable(106, T, 0, 0));
        expect_true("sequence: the same manifest again", sequenceAcceptable(106, T, 106, T));
        expect_true("sequence: a newer state of the same release", sequenceAcceptable(106, T + 60, 106, T));
        expect_true("sequence: an OLDER state of the same release is refused (replay cannot undo a pause)", !sequenceAcceptable(106, T, 106, T + 60));
        expect_true("sequence: an unsequenced manifest for the same release after a staged one is refused", !sequenceAcceptable(106, 0, 106, T));
        expect_true("sequence: a higher release is accepted whatever its sequence", sequenceAcceptable(107, 0, 106, T));
        expect_true("sequence: a lower release (our own undo) is accepted", sequenceAcceptable(105, 0, 106, T));
    }

    // ── [BETA CRASH NOTICE] ──────────────────────────────────────────────────────────────────
    expect_true("crash notice: beta, new crash: shown", betaCrashNotice(true, false, 2000, 1000, 0, 105, false) == CrashNotice::SHOW);
    expect_true("crash notice: release build: never", betaCrashNotice(false, false, 2000, 1000, 0, 104, false) == CrashNotice::NO);
    expect_true("crash notice: no crash: nothing", betaCrashNotice(true, false, 0, 0, 0, 105, false) == CrashNotice::NO);
    expect_true("crash notice: that crash was already spoken about: nothing", betaCrashNotice(true, false, 2000, 2000, 0, 105, false) == CrashNotice::NO);
    expect_true("crash notice: a later crash: shown again", betaCrashNotice(true, false, 3000, 2000, 0, 105, false) == CrashNotice::SHOW);
    expect_true("crash notice: 'don't show again for this beta': marked, not shown", betaCrashNotice(true, false, 3000, 2000, 105, 105, false) == CrashNotice::MARK);
    expect_true("crash notice: muted on the previous beta does not mute this one", betaCrashNotice(true, false, 3000, 2000, 105, 106, false) == CrashNotice::SHOW);
    expect_true("crash notice: a Vulkan fall-back notice is showing: marked, not stacked", betaCrashNotice(true, false, 3000, 2000, 0, 105, true) == CrashNotice::MARK);
    expect_true("crash notice: second instance: nothing", betaCrashNotice(true, true, 3000, 2000, 0, 105, false) == CrashNotice::NO);

    // ── never downwards: no input makes choose() offer a revision at or below the running one ──
    {
        bool never_down = true; int tried = 0;
        for (int mine = 100; mine <= 110; ++mine)
            for (int srev = 100; srev <= 110; ++srev)
                for (int brev = 0; brev <= 110; brev += (brev == 0 ? 100 : 1))
                    for (int flags = 0; flags < 16; ++flags)
                    {
                        Entry s = stable(srev); Entry b = beta(brev);
                        if (flags & 1) { s.mRollout.mPresent = true; s.mRollout.mStarted = 1000; s.mRollout.mSteps = { { 0, 10 } }; }
                        if (flags & 2) s.mRollout.mPaused = s.mRollout.mPresent;
                        Staging st; st.mNow = 2000; st.mBucket = (flags & 4) ? 5 : 50; st.mBypass = (flags & 8) != 0;
                        const Choice c = choose(mine, 0, 0, s, (flags & 8) ? &b : nullptr, &st);
                        ++tried;
                        if (c.mVerdict == Verdict::OFFER && (c.mFeed == Feed::BETA ? b.mRevision : s.mRevision) <= mine) never_down = false;
                    }
        std::printf("      (%d combinations)\n", tried);
        expect_true("never downwards: every offer is above the running revision (there is no downgrade path, crash or not)", never_down);
    }

    std::printf("\n%d checks, %d failed\n", sRun, sFailed);
    return sFailed == 0 ? 0 : 1;
}
