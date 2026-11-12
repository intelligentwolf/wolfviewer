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

    std::printf("\n%d checks, %d failed\n", sRun, sFailed);
    return sFailed == 0 ? 0 : 1;
}
