/**
 * @file wolfupdatemanifest_harness.cpp
 * @brief Stand-alone test of wolfupdatemanifest.h: the update check's real acceptance and decision code
 *        (signature, JSON, feed rules, the beta feed's replay rule, what is remembered), driven with manifests
 *        signed here by a key made for the run.
 *
 * Not part of the CMake test set. Build and run from the repository root (boost comes from the build's packages):
 *   g++ -std=c++17 -Wall -Wextra -I indra/newview -I build-linux-x86_64/packages/include \
 *       indra/newview/tests/wolfupdatemanifest_harness.cpp -lcrypto -o ~/.cache/betachannel/wum && ~/.cache/betachannel/wum
 * Optional: two more arguments, a release manifest and its .sig as served (e.g. a copy of the live latest.json):
 * they must verify with the PRODUCTION key compiled into the viewer, and a changed byte must not.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#include <boost/json/src.hpp>   // header-only build of boost::json for this one file

#include "wolfupdatemanifest.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace WolfUpdateManifest;

namespace
{
    int sRun = 0, sFailed = 0;
    EVP_PKEY* sKey = nullptr;
    unsigned char sPub[32];

    void check(const char* what, bool ok)
    {
        ++sRun;
        if (!ok) ++sFailed;
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    }

    std::string sign(const std::string& m, EVP_PKEY* key = nullptr)
    {
        unsigned char sig[64]; size_t n = sizeof(sig);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key ? key : sKey);
        EVP_DigestSign(ctx, sig, &n, (const unsigned char*)m.data(), m.size());
        EVP_MD_CTX_free(ctx);
        return std::string((const char*)sig, n);
    }

    const char* const SHA = "bf152acdd44cbdf4254a272d51ad8e619f4a2bd14f8fa7a3ac65b394b43dd9b3";

    /// A manifest as the release tools write it (sorted keys do not matter to the viewer).
    std::string manifest(int rev, bool beta, long long sequence, bool with_files = true)
    {
        std::ostringstream s;
        const std::string folder = std::string(beta ? "beta/" : "") + "w" + std::to_string(rev);
        s << "{\n  \"schema\": 1,\n";
        if (beta) s << "  \"channel\": \"beta\",\n";
        if (sequence > 0) s << "  \"sequence\": " << sequence << ",\n";
        s << "  \"revision\": " << rev << ",\n  \"release_name\": \"Name " << rev << "\",\n  \"files\": {";
        if (with_files)
        {
            s << "\n    \"linux64\": {\"url\": \"https://wolf-grid.com/downloads/wolfviewer/" << folder
              << "/WolfViewer-7.2.4-linux64-OpenSim.tar.xz\", \"size\": 274339244, \"sha256\": \"" << SHA << "\"}\n  ";
        }
        s << "}\n}\n";
        return s.str();
    }

    Inputs base(int mine)
    {
        Inputs in;
        in.mPubkey = sPub;
        in.mPlatform = "linux64";
        in.mMine = mine;
        return in;
    }

    void stable(Inputs& in, const std::string& m) { in.mStableFetched = true; in.mStableManifest = m; in.mStableSignature = sign(m); }
    void beta(Inputs& in, const std::string& m) { in.mFollowBeta = true; in.mBetaFetched = true; in.mBetaManifest = m; in.mBetaSignature = sign(m); }

    std::string slurp(const char* path)
    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    }
}

int main(int argc, char** argv)
{
    sKey = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    size_t publen = sizeof(sPub);
    if (!sKey || EVP_PKEY_get_raw_public_key(sKey, sPub, &publen) != 1 || publen != 32) { std::puts("no test key"); return 2; }
    EVP_PKEY* other = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
    const long long T = 1794480000;

    // ── acceptance: signature over the exact bytes, then the JSON ────────────────────────────
    {
        const std::string m = manifest(105, false, 0);
        check("accept: a signed manifest", accept(m, sign(m), sPub, "linux64").mVerified);
        std::string changed = m; changed[changed.find("105")] = '9';
        check("accept: one changed byte is refused", !accept(changed, sign(m), sPub, "linux64").mVerified);
        check("accept: signed by another key is refused", !accept(m, sign(m, other), sPub, "linux64").mVerified);
        check("accept: the production key does not accept the test key's signature", !accept(m, sign(m), MANIFEST_PUBKEY, "linux64").mVerified);
        check("accept: a short signature is refused", !accept(m, sign(m).substr(0, 63), sPub, "linux64").mVerified);
        check("accept: an empty signature is refused", !accept(m, "", sPub, "linux64").mVerified);
        const std::string junk = "not json";
        check("accept: signed text that is not JSON is refused", !accept(junk, sign(junk), sPub, "linux64").mVerified);
        const std::string arr = "[1,2]";
        check("accept: signed JSON that is not an object is refused", !accept(arr, sign(arr), sPub, "linux64").mVerified);
        const Entry e = accept(m, sign(m), sPub, "linux64");
        check("parse: revision, name, url, sha, size", e.mRevision == 105 && e.mName == "Name 105" && e.mSha256 == SHA && e.mSize == 274339244
              && e.mUrl == "https://wolf-grid.com/downloads/wolfviewer/w105/WolfViewer-7.2.4-linux64-OpenSim.tar.xz" && e.mChannel.empty() && e.mSequence == 0);
        const Entry w = accept(m, sign(m), sPub, "windows64");
        check("parse: a platform the manifest does not have gives an unusable entry", w.mVerified && w.mUrl.empty() && w.mSize == 0);
        const std::string r = manifest(106, false, T);
        check("parse: sequence", accept(r, sign(r), sPub, "linux64").mSequence == T);
        std::string old = manifest(106, false, 0); old.replace(old.find("\"schema\": 1"), 11, "\"schema\": 1, \"rollout\": {\"paused\": true}");
        const Entry oe = accept(old, sign(old), sPub, "linux64");
        check("parse: a \"rollout\" field (from the abandoned staged-rollout design) is ignored: a normal release", oe.mVerified && oe.mRevision == 106);
    }

    // ── decide(): the release feed, W104's behaviour ─────────────────────────────────────────
    { Inputs in = base(104); const Outcome o = decide(in); check("decide: release feed not reached: FAILED, nothing remembered", o.mVerdict == Verdict::FAILED && o.mStableSeen == 0 && !o.mCleanUpdates); }
    { Inputs in = base(104); stable(in, manifest(105, false, 0)); in.mStableSignature[0] ^= 1; const Outcome o = decide(in); check("decide: bad release signature: FAILED", o.mVerdict == Verdict::FAILED && o.mStableSeen == 0); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); const Outcome o = decide(in); check("decide: on the release: UP_TO_DATE, old downloads cleared, release 104 remembered", o.mVerdict == Verdict::UP_TO_DATE && o.mCleanUpdates && o.mStableSeen == 104); }
    { Inputs in = base(104); stable(in, manifest(105, false, 0)); const Outcome o = decide(in); check("decide: new release: OFFER from the release feed", o.mVerdict == Verdict::OFFER && o.mFeed == Feed::STABLE && o.mOffer.mRevision == 105 && o.mOffer.mSha256 == SHA); }
    { Inputs in = base(104); stable(in, manifest(105, false, 0)); in.mSkippedStable = 105; check("decide: skipped", decide(in).mVerdict == Verdict::SKIPPED); }
    { Inputs in = base(104); stable(in, manifest(105, false, 0)); in.mSkippedStable = 105; in.mByHand = true; check("decide: asked by hand forgets the skip", decide(in).mVerdict == Verdict::OFFER); }
    { Inputs in = base(104); stable(in, manifest(105, true, T)); const Outcome o = decide(in); check("decide: a BETA manifest served as the release feed: REFUSED, and not remembered as the release", o.mVerdict == Verdict::REFUSED && o.mStableSeen == 0); }

    // ── the label's memory only goes up ──────────────────────────────────────────────────────
    { Inputs in = base(106); in.mStableSeen = 106; stable(in, manifest(104, false, 0)); const Outcome o = decide(in); check("decide: a replayed older release manifest does not lower the release remembered (no false Beta label)", o.mStableSeen == 106 && o.mVerdict == Verdict::UP_TO_DATE); }

    // ── the beta feed ────────────────────────────────────────────────────────────────────────
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(105, true, T)); const Outcome o = decide(in); check("decide: box ticked, beta newer: OFFER from the beta feed; its sequence remembered", o.mVerdict == Verdict::OFFER && o.mFeed == Feed::BETA && o.mOffer.mRevision == 105 && o.mBetaSequenceSeen == T); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(105, true, T)); in.mFollowBeta = false; const Outcome o = decide(in); check("decide: beta bytes present but the feed not followed: ignored", o.mVerdict == Verdict::UP_TO_DATE); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(105, true, T)); in.mBetaSignature[5] ^= 1; const Outcome o = decide(in); check("decide: bad beta signature: release answer stands, downloads KEPT", o.mVerdict == Verdict::UP_TO_DATE && o.mBetaFeedFailed && !o.mCleanUpdates); }
    { Inputs in = base(105); stable(in, manifest(104, false, 0)); in.mFollowBeta = true; const Outcome o = decide(in); check("decide: beta feed not reached while on a beta: downloads KEPT", o.mVerdict == Verdict::UP_TO_DATE && !o.mCleanUpdates); }
    { Inputs in = base(105); stable(in, manifest(104, false, 0)); beta(in, manifest(105, true, T)); const Outcome o = decide(in); check("decide: on the beta, both feeds answered: downloads cleared", o.mVerdict == Verdict::UP_TO_DATE && o.mCleanUpdates); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(105, false, T)); const Outcome o = decide(in); check("decide: a RELEASE manifest served as the beta feed: ignored", o.mVerdict == Verdict::UP_TO_DATE && o.mBetaFeedFailed); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(0, true, T + 500, false)); const Outcome o = decide(in); check("decide: withdrawn beta feed: nothing offered; its sequence remembered; a complete answer", o.mVerdict == Verdict::UP_TO_DATE && o.mBetaSequenceSeen == T + 500 && o.mCleanUpdates); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(105, true, T)); in.mBetaSequenceSeen = T + 500; const Outcome o = decide(in); check("decide: the withdrawn beta's old manifest REPLAYED after the withdrawal was seen: not acted on", o.mVerdict == Verdict::UP_TO_DATE && o.mBetaStale && !o.mCleanUpdates && o.mBetaSequenceSeen == T + 500); }
    { Inputs in = base(104); stable(in, manifest(104, false, 0)); beta(in, manifest(106, true, T + 900)); in.mBetaSequenceSeen = T + 500; const Outcome o = decide(in); check("decide: the next beta after a withdrawal is offered", o.mVerdict == Verdict::OFFER && o.mOffer.mRevision == 106 && o.mBetaSequenceSeen == T + 900); }

    // ── the release feed carries no replay memory (DESIGN.md section 20) ─────────────────────
    { Inputs in = base(105); stable(in, manifest(106, false, T + 60)); Outcome o = decide(in); stable(in, manifest(106, false, 0)); const Outcome again = decide(in);
      check("decide: the same release with or without a sequence gives the same offer (no stale state on the release feed)", o.mVerdict == Verdict::OFFER && again.mVerdict == Verdict::OFFER && again.mOffer.mRevision == 106); }
    { Inputs in = base(106); stable(in, manifest(105, false, 0)); check("decide: a replayed OLDER release manifest offers nothing to a viewer above it", decide(in).mVerdict == Verdict::UP_TO_DATE); }
    { Inputs in = base(104); in.mByHand = true; in.mSkippedStable = 105; stable(in, manifest(105, false, 0)); check("decide: by hand, a declined release is offered again", decide(in).mVerdict == Verdict::OFFER); }
    { Inputs in = base(104); std::string m = manifest(106, false, 0); m.replace(m.find("\"revision\": 106"), 15, "\"revision\": 99999999999999999999"); stable(in, m); const Outcome o = decide(in); check("decide: a huge revision is read as 0 (nothing newer), never a wrapped number", o.mStable.mRevision == 0 && o.mVerdict == Verdict::UP_TO_DATE); }

    // ── optional: a real served manifest against the production key ──────────────────────────
    if (argc == 3)
    {
        const std::string m = slurp(argv[1]), s = slurp(argv[2]);
        const Entry e = accept(m, s, MANIFEST_PUBKEY, "linux64");
        check("LIVE: the served release manifest verifies with the key compiled into the viewer", e.mVerified && e.mRevision > 0);
        std::printf("      (revision %d \"%s\", sequence %lld)\n", e.mRevision, e.mName.c_str(), e.mSequence);
        std::string changed = m; if (!changed.empty()) changed[changed.size() / 2] ^= 1;
        check("LIVE: with one bit changed it does not", !accept(changed, s, MANIFEST_PUBKEY, "linux64").mVerified);
        Inputs in; in.mPlatform = "linux64"; in.mMine = e.mRevision; in.mStableFetched = true; in.mStableManifest = m; in.mStableSignature = s;
        check("LIVE: a viewer on that revision is up to date", decide(in).mVerdict == Verdict::UP_TO_DATE);
        in.mMine = e.mRevision - 1;
        check("LIVE: a viewer one revision behind is offered it", decide(in).mVerdict == Verdict::OFFER);
    }

    EVP_PKEY_free(sKey); EVP_PKEY_free(other);
    std::printf("\n%d checks, %d failed\n", sRun, sFailed);
    return sFailed == 0 ? 0 : 1;
}
