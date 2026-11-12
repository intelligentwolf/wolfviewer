/**
 * @file wolfupdate.cpp
 * @brief WolfViewer: find, fetch and install a newer release. See wolfupdate.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfupdate.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include <boost/json.hpp>
#include <curl/curl.h>
#include <openssl/evp.h>

#if LL_WINDOWS
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <climits>
#include <cstdlib>
extern char** environ;
#endif

#include "llapp.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "llhttpconstants.h"
#include "llnotificationsutil.h"
#include "llproxy.h"
#include "llpanel.h"
#include "llsdjson.h"
#include "lluri.h"
#include "llstartup.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llcheckboxctrl.h"
#include "llagent.h"
#include "wolfgrid.h"
#include "wolfterrainpaint.h"
#include "wolfupdatechannel.h"
#include "wolfupdatemanifest.h"
#include "wolfcrashreport.h"
#include "wolfvkbridge.h"

extern std::string gWindowTitle;   // llappviewer.cpp

namespace
{
    // ── what is trusted ──────────────────────────────────────────────────────────────────────
    const char* const MANIFEST_URL = "https://wolf-grid.com/downloads/wolfviewer/latest.json";
    const char* const SIGNATURE_URL = "https://wolf-grid.com/downloads/wolfviewer/latest.json.sig";
    /// [BETA CHANNEL 2026-11-12] The beta feed: the same format, signed with the same key and verified by the
    /// same function (wolfupdatemanifest.h accept), plus "channel": "beta" inside the signed bytes. Fetched only when
    /// "Get beta versions" is ticked (BETA_SETTING) and this copy is not a package-repository install. Like
    /// the two above, a constant: no setting, file or server reply can supply a feed URL.
    const char* const BETA_MANIFEST_URL = "https://wolf-grid.com/downloads/wolfviewer/beta/latest.json";
    const char* const BETA_SIGNATURE_URL = "https://wolf-grid.com/downloads/wolfviewer/beta/latest.json.sig";
    /// Every download must live under WolfUpdateChannel::DOWNLOAD_PREFIX (wolfupdatechannel.h): the website
    /// serves the installers (viewers.php reads them from /downloads/wolfviewer/), and a manifest naming
    /// anywhere else is refused.
    /// [BETA CHANNEL 2026-11-12] gridmanager/viewerseen.php: which version this resident last logged in with,
    /// for the beta gate report's "how many people ran it" (DESIGN.md decision 4).
    const char* const SEEN_URL = "https://www.wolf-grid.com/viewerseen.php";
    /// f=osv, NOT f=viewers: index.php routes "osv" to viewers.php (2026-09-12 fix).
    const char* const DOWNLOAD_PAGE_URL = "https://www.wolf-grid.com/index.php?f=osv";
    // The manifest signing key's public half and the signature check are in wolfupdatemanifest.h (MANIFEST_PUBKEY,
    // verifySignature), so the stand-alone harness runs the very code this file does.

    // ── settings ─────────────────────────────────────────────────────────────────────────────
    const char* const CHECK_SETTING = "WolfViewerCheckForUpdates";
    /// The newest release already told about, for the one-off notices (package manager, manual).
    const char* const TOLD_SETTING = "WolfViewerUpdateLastTold";
    /// "Skip this version": the newest release the resident declined for good.
    const char* const SKIPPED_SETTING = "WolfViewerUpdateSkipped";
    const char* const AUTO_DOWNLOAD_SETTING = "WolfViewerAutoDownloadUpdates";
    // [BETA CHANNEL 2026-11-12]
    /// Preferences > WolfViewer > Extras "Get beta versions". Read once per run, by the one check.
    const char* const BETA_SETTING = "WolfViewerGetBetaVersions";
    /// TOLD_SETTING / SKIPPED_SETTING for a beta offer, kept apart so that declining a beta never hides a
    /// release and declining a release never hides a beta (wolfupdatechannel.h choose).
    const char* const TOLD_BETA_SETTING = "WolfViewerUpdateLastToldBeta";
    const char* const SKIPPED_BETA_SETTING = "WolfViewerUpdateSkippedBeta";
    /// The release revision in the last release manifest that verified: this build is a beta while its own
    /// revision is above it (wolfupdatechannel.h isBetaBuild). Labels only, never an update decision.
    const char* const STABLE_SEEN_SETTING = "WolfViewerStableRevisionSeen";
    // [STAGED ROLLOUT 2026-11-12] wolfupdatechannel.h Rollout / rolloutBucket / sequenceAcceptable
    const char* const SEED_SETTING = "WolfViewerRolloutSeed";
    const char* const MANIFEST_REVISION_SETTING = "WolfViewerManifestRevisionSeen";
    const char* const MANIFEST_SEQUENCE_SETTING = "WolfViewerManifestSequenceSeen";
    const char* const ROLLOUT_SEEN_REVISION_SETTING = "WolfViewerRolloutFirstSeenRevision";
    const char* const ROLLOUT_SEEN_TIME_SETTING = "WolfViewerRolloutFirstSeenTime";
    // [BETA CRASH NOTICE 2026-11-12] wolfupdatechannel.h betaCrashNotice
    const char* const CRASH_SPOKEN_SETTING = "WolfViewerBetaCrashNoticeTime";
    const char* const CRASH_MUTED_SETTING = "WolfViewerBetaCrashNoticeMuted";
    const F32 CRASH_BAND_WAIT_S = 8.f;   // how long the crash band waits for the update check's answer
    /// [review 2026-11-12] The highest sequence verified on the beta feed (wolfupdatechannel.h feedSequenceAcceptable).
    const char* const BETA_SEQUENCE_SETTING = "WolfViewerBetaSequenceSeen";
    /// [review 2026-11-12] Preferences > Privacy "Tell Wolf Territories which WolfViewer version I use" (on by default):
    /// the once-per-login version record (seen_coro). Its own switch, not the update check's.
    const char* const REPORT_VERSION_SETTING = "WolfViewerReportVersion";
    /// What the title bar, the login screen and About add to the release name of a beta.
    const char* const BETA_SUFFIX = " (Beta)";

    enum class Install { SELF, PACKAGED, MANUAL };

    struct Offer
    {
        S32 mRevision = 0;
        std::string mName;      // the release name, "Bouncing Panda"
        std::string mUrl;
        std::string mSha256;    // lower-case hex
        U64 mSize = 0;
        bool mBeta = false;     // [BETA CHANNEL 2026-11-12] from the beta feed (its URL shape, its own skip/told)
    };

    /// The download's shared state: written by the worker thread, read by the coroutine.
    struct Job
    {
        std::string mUrl, mPath, mPartPath, mSha256, mCaFile, mUserAgent;
        U64 mSize = 0;
        std::atomic<U64> mReceived{ 0 };
        std::atomic<bool> mAbort{ false };
        std::atomic<bool> mDone{ false };
        bool mOk = false;            // read only after mDone
        std::string mError;          // read only after mDone
    };

    bool sChecked = false;
    Offer sOffer;
    std::string sReadyPath;          // the verified download, once there is one
    bool sBusy = false;              // a download or an install is under way
    std::mutex sThreadMutex;
    std::thread sThread;
    std::shared_ptr<Job> sJob;

    // ── the check's answer, kept for the run (ticket 532) ────────────────────────────────────
    bool sHaveOffer = false;         // sOffer is a newer, usable, not skipped release
    Install sKind = Install::SELF;   // how this copy can take it (install_kind)
    std::string sKindWhere;          // install_kind's folder, for the MANUAL wording
    bool sLoggedIn = false;          // STATE_STARTED reached: notices are in-world ones now
    bool sInWorldDone = false;       // the in-world notice for this run has been decided
    /// Update now was pressed on the login screen's band: from then on the download and the
    /// install report on the band (and in their own alerts on failure), not the in-world notices.
    bool sActedAtLogin = false;
    /// [BETA CHANNEL 2026-11-12] What the login screen shows for a build with no release name (fspanellogin.cpp
    /// passes it to releaseLabel), kept so refresh_channel_labels can write that text again.
    std::string sLoginFallback;
    // [STAGED ROLLOUT 2026-11-12] Help > Check for WolfViewer Updates
    bool sCheckRunning = false;      // a check coroutine is between its start and its answer
    bool sByHand = false;            // the check about to run was asked for by hand: not staged, skips forgotten, always answers
    bool sManualPending = false;     // asked by hand while a check was already under way: a by-hand check follows it at once
    bool sCheckAnswered = false;     // a check has run to its answer (or there will be none): the beta crash band waits for it
    // [BETA CRASH NOTICE 2026-11-12] the band "This beta version of WolfViewer closed unexpectedly last time"
    LLHandle<LLPanel> sCrashNotice;
    time_t sCrashNoticeTime = 0;     // the crash it is about (stored as spoken about once the resident answers)

    // ── the band on the login screen ─────────────────────────────────────────────────────────
    enum class Stage { OFFER, DOWNLOADING, INSTALLING, MAC_OPENED };
    LLHandle<LLPanel> sLoginPanel;   // FSPanelLogin; dies with it on a successful login
    LLHandle<LLPanel> sNotice;       // the band, a child of that panel
    Stage sStage = Stage::OFFER;
    U64 sNoticeReceived = 0;         // download progress shown on the band
    void notice_back_to_offer();     // after a failure: offer Update now again

    const char* platform_key()
    {
#if LL_WINDOWS
#  ifdef USE_AVX2_OPTIMIZATION
        return "windows64-avx2";   // the same flavour this copy is (llappviewer.cpp ~3565 same test)
#  else
        return "windows64";
#  endif
#elif LL_DARWIN
        return "macos64";
#else
        return "linux64";          // one tarball, both builds; wrapper.sh picks (2026-09-28)
#endif
    }

#if LL_WINDOWS
    const char* const PLATFORM_WORDS = "windows";   // panel_wolf_update_notice.xml body_*_windows; viewerseen.php platform
#elif LL_DARWIN
    const char* const PLATFORM_WORDS = "mac";
#else
    const char* const PLATFORM_WORDS = "linux";
#endif

    std::string hex(const unsigned char* p, size_t n)
    {
        static const char* d = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
        return s;
    }

    /// The SHA-256 of a whole file, or "" if it cannot be read.
    std::string file_sha256(const std::string& path, U64& size_out)
    {
        size_out = 0;
        LLFILE* f = LLFile::fopen(path, "rb");
        if (!f) return std::string();
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        std::vector<unsigned char> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
        {
            EVP_DigestUpdate(ctx, buf.data(), n);
            size_out += n;
        }
        const bool err = ferror(f) != 0;
        fclose(f);
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx, md, &len);
        EVP_MD_CTX_free(ctx);
        return err ? std::string() : hex(md, len);
    }

    std::string updates_dir()
    {
        return gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "updates";
    }

    /// The file name of the offer's download URL, in the URL shape of the feed it came from
    /// (wolfupdatechannel.h downloadFileName: the release's "w<digits>/" folder, the beta's "beta/w<digits>/").
    std::string offer_file_name(const Offer& o)
    {
        return WolfUpdateChannel::downloadFileName(o.mUrl, o.mBeta ? WolfUpdateChannel::Feed::BETA : WolfUpdateChannel::Feed::STABLE);
    }

    /// [BETA CHANNEL 2026-11-12] "Skip this version" / "told once" are kept per feed (see SKIPPED_BETA_SETTING).
    const char* skip_setting() { return sOffer.mBeta ? SKIPPED_BETA_SETTING : SKIPPED_SETTING; }
    const char* told_setting() { return sOffer.mBeta ? TOLD_BETA_SETTING : TOLD_SETTING; }

    /// Remove everything in the updates folder except `keep` (a full path; "" keeps nothing).
    void clean_updates(const std::string& keep)
    {
        const std::string dir = updates_dir();
        if (!LLFile::isdir(dir)) return;
        LLDirIterator it(dir, "*");
        std::string name;
        while (it.next(name))
        {
            const std::string path = dir + gDirUtilp->getDirDelimiter() + name;
            if (path != keep && LLFile::isfile(path)) LLFile::remove(path);
        }
    }

#if !LL_WINDOWS && !LL_DARWIN
    /// This copy's install folder: the parent of bin/, where the viewer binary is
    /// (lldir_linux.cpp mExecutableDir, from /proc/self/exe).
    std::string linux_install_dir()
    {
        char buf[PATH_MAX];
        const std::string up = gDirUtilp->getExecutableDir() + "/..";
        return realpath(up.c_str(), buf) ? std::string(buf) : std::string();
    }
#endif

    Install install_kind(std::string& where)
    {
#if LL_WINDOWS || LL_DARWIN
        where.clear();
        return Install::SELF;
#else
        where = linux_install_dir();
        if (where.empty()) return Install::MANUAL;
        if (LLFile::isfile(where + "/etc/wolfviewer-package")) return Install::PACKAGED;
        // Never rewrite a build tree or anything that is not an installed tarball.
        if (where.find("/build-linux-") != std::string::npos || !LLFile::isfile(where + "/wolfviewer")
            || !LLFile::isfile(where + "/etc/wolfviewer-update.sh"))
        {
            return Install::MANUAL;
        }
        const std::string parent = where.substr(0, where.find_last_of('/'));
        if (access(where.c_str(), W_OK) != 0 || access(parent.empty() ? "/" : parent.c_str(), W_OK) != 0)
        {
            return Install::MANUAL;
        }
        return Install::SELF;
#endif
    }

    void notify_failed(const std::string& why)
    {
        LL_WARNS("WolfUpdate") << "update failed: " << why << LL_ENDL;
        notice_back_to_offer();
        LLNotificationsUtil::add("WolfUpdateFailed", LLSD().with("REASON", why), LLSD(),
            [](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) == 1) LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
            });
    }

    LLSD release_args()
    {
        LLSD args;
        args["NAME"] = sOffer.mName.empty() ? llformat("w%d", sOffer.mRevision) : sOffer.mName;
        // [BETA CHANNEL 2026-11-12] every notice about a beta says so: "WolfViewer <name> (Beta) (w105) is available"
        if (sOffer.mBeta) args["NAME"] = args["NAME"].asString() + BETA_SUFFIX;
        args["LATEST"] = sOffer.mRevision;
        args["MINE"] = WolfUpdate::currentRevision();
        args["SIZE"] = llformat("%.0f", (F64)sOffer.mSize / (1024.0 * 1024.0));
        return args;
    }

    // ── the download, on its own thread ──────────────────────────────────────────────────────
    struct WriteCtx { LLFILE* mFile; EVP_MD_CTX* mHash; Job* mJob; };

    size_t on_write(char* data, size_t size, size_t nmemb, void* user)
    {
        WriteCtx* w = (WriteCtx*)user;
        const size_t n = size * nmemb;
        const U64 got = w->mJob->mReceived.load() + n;
        if (got > w->mJob->mSize) return 0;   // longer than the signed size: stop
        if (fwrite(data, 1, n, w->mFile) != n) return 0;
        EVP_DigestUpdate(w->mHash, data, n);
        w->mJob->mReceived.store(got);
        return n;
    }

    int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        const Job* job = (const Job*)user;
        return (job->mAbort.load() || LLApp::isExiting()) ? 1 : 0;
    }

    void run_job(std::shared_ptr<Job> job)
    {
        // A copy from an earlier run that already matches needs no download at all.
        U64 have = 0;
        if (LLFile::isfile(job->mPath) && file_sha256(job->mPath, have) == job->mSha256 && have == job->mSize)
        {
            job->mReceived = have;
            job->mOk = true;
            job->mDone = true;
            return;
        }
        LLFile::remove(job->mPath, ENOENT);
        LLFILE* f = LLFile::fopen(job->mPartPath, "wb");
        if (!f)
        {
            job->mError = "could not write to " + job->mPartPath;
            job->mDone = true;
            return;
        }
        EVP_MD_CTX* hash = EVP_MD_CTX_new();
        EVP_DigestInit_ex(hash, EVP_sha256(), nullptr);
        WriteCtx w{ f, hash, job.get() };
        CURL* h = curl_easy_init();
        CURLcode rc = CURLE_FAILED_INIT;
        if (h)
        {
            curl_easy_setopt(h, CURLOPT_URL, job->mUrl.c_str());
            curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write);
            curl_easy_setopt(h, CURLOPT_WRITEDATA, &w);
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, job.get());
            curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // under 1 KB/s for a minute: give up
            curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
            curl_easy_setopt(h, CURLOPT_USERAGENT, job->mUserAgent.c_str());
            // The viewer's own trust store and proxy, as LLCore uses them
            // (_httpoprequest.cpp:614-637: applyProxySettings, CURLOPT_CAINFO).
            if (!job->mCaFile.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, job->mCaFile.c_str());
            LLProxy::applyProxySettings(h);
            rc = curl_easy_perform(h);
            curl_easy_cleanup(h);
        }
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_DigestFinal_ex(hash, md, &len);
        EVP_MD_CTX_free(hash);
        const bool closed = fclose(f) == 0;
        const std::string got = hex(md, len);
        if (rc != CURLE_OK)
        {
            job->mError = job->mAbort.load() ? std::string("cancelled") : std::string(curl_easy_strerror(rc));
        }
        else if (!closed)
        {
            job->mError = "could not finish writing the download";
        }
        else if (job->mReceived.load() != job->mSize || got != job->mSha256)
        {
            job->mError = "the download did not match the published release (size or SHA-256)";
        }
        else if (LLFile::rename(job->mPartPath, job->mPath) != 0)
        {
            job->mError = "could not move the download into place";
        }
        else
        {
            job->mOk = true;
        }
        if (!job->mOk) LLFile::remove(job->mPartPath, ENOENT);
        job->mDone = true;
    }

#if !LL_WINDOWS
    /// Start a program with arguments; pid on success, -1 on failure. No shell is involved.
    pid_t spawn(const std::vector<std::string>& argv)
    {
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        pid_t pid = -1;
        if (posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return -1;
        return pid;
    }
#endif

    // ── what the resident sees ───────────────────────────────────────────────────────────────
    bool on_available(const LLSD& n, const LLSD& r)
    {
        const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
        if (opt == 0)
        {
            WolfUpdate::startDownload();
        }
        else if (opt == 2)
        {
            gSavedSettings.setS32(skip_setting(), sOffer.mRevision);
        }
        return false;
    }

    bool on_ready(const LLSD& n, const LLSD& r)
    {
        const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
        if (opt == 2)
        {
            gSavedSettings.setS32(skip_setting(), sOffer.mRevision);
        }
        return false;
    }
}

// static
S32 WolfUpdate::currentRevision()
{
    // The 4th version field is the release number: the CI workflow sets `revision: "N"` and
    // BuildVersion.cmake:14 reads it into LL_VIEWER_VERSION_BUILD. So w53 is build 53.
    return (S32)LLVersionInfo::instance().getBuild();
}

// static
bool WolfUpdate::isBetaBuild()
{
    return WolfUpdateChannel::isBetaBuild(currentRevision(), gSavedSettings.getS32(STABLE_SEEN_SETTING));
}

// static
std::string WolfUpdate::channelWord()
{
    if (gSavedSettings.getS32(STABLE_SEEN_SETTING) <= 0) return "unknown";
    return isBetaBuild() ? "beta" : "stable";
}

// static
std::string WolfUpdate::betaSuffix()
{
    return isBetaBuild() ? std::string(BETA_SUFFIX) : std::string();
}

// static
std::string WolfUpdate::releaseLabel(const std::string& unnamed_fallback)
{
    sLoginFallback = unnamed_fallback;
    const std::string release_name = LLVersionInfo::instance().getReleaseName();
    std::string label = (release_name.empty() ? unnamed_fallback : release_name) + betaSuffix();
#ifdef USE_AVX2_OPTIMIZATION
    label += " \xC2\xB7 AVX2";   // U+00B7 middle dot (fspanellogin.cpp, 2026-09-28: the same flag as About's SIMD field)
#endif
    return label;
}

namespace
{
    /// Preferences > WolfViewer > Extras: the sentence under "Get beta versions" for the box as it stands now.
    void refresh_beta_preference(LLHandle<LLPanel> handle)
    {
        LLPanel* panel = handle.get();
        if (!panel) return;
        LLCheckBoxCtrl* box = panel->findChild<LLCheckBoxCtrl>("wolf_get_beta_versions");
        LLTextBox* text = panel->findChild<LLTextBox>("wolf_get_beta_versions_text");
        if (!box || !text) return;   // a skin with its own panel file
        std::string where;
        std::string key = "wolf_beta_text_off";
        if (install_kind(where) == Install::PACKAGED)
        {
            box->setEnabled(false);
            key = "wolf_beta_text_packaged";
        }
        else if (box->get())
        {
            key = "wolf_beta_text_on";
        }
        else if (WolfUpdate::isBetaBuild())
        {
            key = "wolf_beta_text_off_on_beta";   // unticked while running a beta: say what happens (no downgrade)
        }
        text->setText(panel->getString(key));
    }

    /// [BETA CHANNEL 2026-11-12] Once per login to Wolf Territories: this resident's version, platform, renderer
    /// and channel to gridmanager/viewerseen.php (one row per resident), so the beta gate report can say how many
    /// people ran a build, not only how many crashed. Signed-in residents only, as wolffloaterdob.cpp post_dob:
    /// verified TLS, no redirects, this login's agent and session in the headers. A failure (or a server without
    /// the endpoint) is logged and nothing else: nobody asked for this.
    void seen_coro()
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfUpdateSeen", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t options = WolfGrid::makeVerifiedHttpOptions();
        options->setTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/x-www-form-urlencoded");
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
        const std::string body = "viewer=" + LLURI::escape(LLVersionInfo::instance().getChannelAndVersion())
            + "&platform=" + PLATFORM_WORDS
            + "&renderer=" + LLURI::escape(WolfVkBridge::reportFields()["renderer"].asString())
            + "&channel=" + WolfUpdate::channelWord()
            + "&beta_optin=" + (gSavedSettings.getBOOL(BETA_SETTING) ? "1" : "0");
        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());
        LLSD result = adapter->postRawAndSuspend(request, SEEN_URL, raw, options, headers);
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        LL_INFOS("WolfUpdate") << "version record: " << status.toString() << LL_ENDL;
    }
}

// static
void WolfUpdate::setupPreferences(LLPanel* extras_panel)
{
    // postBuild only: setCommitCallback CONNECTS (it does not replace), so this must run once per panel.
    if (!extras_panel) return;
    const LLHandle<LLPanel> handle = extras_panel->getHandle();
    if (LLCheckBoxCtrl* box = extras_panel->findChild<LLCheckBoxCtrl>("wolf_get_beta_versions"))
    {
        box->setCommitCallback([handle](LLUICtrl*, const LLSD&) { refresh_beta_preference(handle); });
    }
    refresh_beta_preference(handle);
}

// static
void WolfUpdate::refreshPreferences(LLPanel* extras_panel)
{
    if (extras_panel) refresh_beta_preference(extras_panel->getHandle());
}

// static
void WolfUpdate::checkOnce()
{
    if (sChecked) return;
    sChecked = true;
    if (!gSavedSettings.getBOOL(CHECK_SETTING)) return;
    LLCoros::instance().launch("WolfUpdate", []() { WolfUpdate::checkCoro(); });
}

// static
void WolfUpdate::checkByHand()
{
    // Help > Check for WolfViewer Updates. Works with WolfViewerCheckForUpdates off (the resident is asking now).
    if (sCheckRunning)
    {
        // A check is under way (most likely the automatic one, a second after the login screen appeared). A click
        // must always be answered: say so now, and run a by-hand check the moment that one ends (checkCoro's Running).
        if (!sManualPending) LLNotificationsUtil::add("WolfUpdateChecking");
        sManualPending = true;
        return;
    }
    sChecked = true;
    sByHand = true;
    LLCoros::instance().launch("WolfUpdate", []() { WolfUpdate::checkCoro(); });
}

// static
void WolfUpdate::shutdown()
{
    std::lock_guard<std::mutex> lock(sThreadMutex);
    if (sJob) sJob->mAbort = true;
    if (sThread.joinable()) sThread.join();
}

namespace
{
    /// GET a small file whole. False on any failure (logged, never shown: the resident did not
    /// ask for this check).
    bool fetch_small(const std::string& url, std::string& out)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfUpdate", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(30);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_USER_AGENT, LLVersionInfo::instance().getChannelAndVersion());
        LLSD result = adapter->getRawAndSuspend(request, url, opts, headers);
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(
            result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
        if (!status || !result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            LL_INFOS("WolfUpdate") << "update check: " << url << ": " << status.toString() << LL_ENDL;
            return false;
        }
        const LLSD::Binary& bytes = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    /// One feed's two files. False unless both arrived. What is done with them is wolfupdatemanifest.h decide().
    bool fetch_feed(const char* manifest_url, const char* signature_url, std::string& manifest, std::string& signature)
    {
        return fetch_small(manifest_url, manifest) && fetch_small(signature_url, signature);
    }

    /// [STAGED ROLLOUT 2026-11-12] This install's random seed for rolloutBucket: made once with the system's random
    /// source (not from the computer, the account or the avatar), kept in the viewer's settings, sent nowhere.
    U32 rollout_seed()
    {
        S32 seed = gSavedSettings.getS32(SEED_SETTING);
        if (seed <= 0)
        {
            // std::random_device is the system's random source in the libraries CI builds with (MSVC: the CRT's
            // rand_s / RtlGenRandom; libstdc++ and libc++: getrandom / /dev/urandom / arc4random). The standard does not
            // promise that, so two things no two computers share are mixed in: the clock to its finest tick and where
            // this run's stack happens to be. Even a random_device that returned the same number everywhere would then
            // still give installs different seeds.
            std::random_device rd;
            U32 mixed = (U32)rd();
            const U64 ticks = (U64)std::chrono::high_resolution_clock::now().time_since_epoch().count();
            mixed ^= WolfUpdateChannel::fmix32((U32)(ticks ^ (ticks >> 32)));
            mixed ^= WolfUpdateChannel::fmix32((U32)((uintptr_t)&rd >> 4) + (U32)((U64)(uintptr_t)&rd >> 32));
            seed = (S32)(WolfUpdateChannel::fmix32(mixed) & 0x7fffffffu);
            if (seed == 0) seed = 1;
            gSavedSettings.setS32(SEED_SETTING, seed);
        }
        return (U32)seed;
    }

    /// The Windows / Linux / macOS "it is downloaded, install it?" question (moved here unchanged from downloadCoro so
    /// that a check asked for by hand can show it again).
    void notify_ready()
    {
#if LL_DARWIN
        LLNotificationsUtil::add("WolfUpdateReadyMac", release_args(), LLSD(), [](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) WolfUpdate::startInstall();
            else on_ready(n, r);
        });
#else
        LLNotificationsUtil::add("WolfUpdateReady", release_args(), LLSD(), [](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0) WolfUpdate::startInstall();
            else on_ready(n, r);
        });
#endif
    }

    /// A check asked for by hand could not get an answer: say so (it must always say something).
    void notify_check_failed()
    {
        LLNotificationsUtil::add("WolfUpdateCheckFailed", LLSD(), LLSD(), [](const LLSD& n, const LLSD& r)
        {
            if (LLNotificationsUtil::getSelectedOption(n, r) == 1) LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
        });
    }

    /// [BETA CHANNEL 2026-11-12] The title bar and the login screen's release text follow isBetaBuild(), which
    /// can change when the run's check answers (a beta installed by hand on a new computer learns the release
    /// number then; a promoted beta stops being one). The window title is only re-set while the login screen
    /// waits: later it carries the avatar's name (llstartup.cpp, llappviewer.cpp handleLoginComplete), which
    /// is built from gWindowTitle, so correcting the string is enough.
    void refresh_channel_labels()
    {
        const bool beta = WolfUpdate::isBetaBuild();
        gDebugInfo["WolfReleaseChannel"] = WolfUpdate::channelWord();
        const std::string suffix = BETA_SUFFIX;
        const size_t at = gWindowTitle.find(suffix);
        bool changed = false;
        if (beta && at == std::string::npos)
        {
            // after the release name, or after the version when this build has no name (llappviewer.cpp initWindow)
            const std::string name = LLVersionInfo::instance().getReleaseName();
            const std::string anchor = name.empty() ? LLVersionInfo::instance().getChannelAndVersion() : name;
            const size_t a = gWindowTitle.find(anchor);
            if (a != std::string::npos)
            {
                gWindowTitle.insert(a + anchor.size(), suffix);
                LLStringUtil::truncate(gWindowTitle, 255);   // llappviewer.cpp initWindow's own limit for the title
                changed = true;
            }
        }
        else if (!beta && at != std::string::npos)
        {
            gWindowTitle.erase(at, suffix.size());
            changed = true;
        }
        if (changed && gViewerWindow && !sLoggedIn && LLStartUp::getStartupState() <= STATE_LOGIN_WAIT)
        {
            gViewerWindow->setTitle(gWindowTitle);
        }
        if (LLPanel* login = sLoginPanel.get())
        {
            if (LLTextBox* release_text = login->findChild<LLTextBox>("wolf_release_text"))
            {
                release_text->setText(WolfUpdate::releaseLabel(sLoginFallback));
            }
        }
    }

    /// The release manifest verified: remember its revision (STABLE_SEEN_SETTING) and put the labels right.
    void note_stable_revision(S32 revision)
    {
        // Only ever upwards (wolfupdatemanifest.h decide keeps the maximum: a replayed older release manifest must not
        // make a release build call itself a beta).
        if (revision <= gSavedSettings.getS32(STABLE_SEEN_SETTING)) return;
        const bool was_beta = WolfUpdate::isBetaBuild();
        gSavedSettings.setS32(STABLE_SEEN_SETTING, revision);
        LL_INFOS("WolfUpdate") << "the release is w" << revision << "; this build (w" << WolfUpdate::currentRevision()
                               << ") is " << WolfUpdate::channelWord() << LL_ENDL;
        if (was_beta != WolfUpdate::isBetaBuild()) refresh_channel_labels();
        else gDebugInfo["WolfReleaseChannel"] = WolfUpdate::channelWord();   // "unknown" -> "stable"
    }

    // ── the band on the login screen (ticket 532) ────────────────────────────────────────────

    /// Still on the login screen, before Log In: only then may Update now restart without asking
    /// again. STATE_LOGIN_WAIT is the login screen waiting for the resident (llstartup.h); a failed
    /// login goes back to it.
    bool at_login_screen()
    {
        return !sLoggedIn && LLStartUp::getStartupState() <= STATE_LOGIN_WAIT;
    }

    /// Put the band's words and buttons in step with sStage. Nothing to do without a band.
    void refresh_notice()
    {
        LLPanel* p = sNotice.get();
        if (!p) return;
        const LLSD r = release_args();
        LLStringUtil::format_map_t a;
        a["[NAME]"] = r["NAME"].asString();
        // [BETA CHANNEL 2026-11-12] title_ready_beta says "beta" itself, so it takes the name without "(Beta)"
        a["[BARE_NAME]"] = sOffer.mName.empty() ? llformat("w%d", sOffer.mRevision) : sOffer.mName;
        a["[SIZE]"] = r["SIZE"].asString();
        a["[DIR]"] = sKindWhere;
        a["[PERCENT]"] = llformat("%d", sOffer.mSize ? (S32)((sNoticeReceived * 100) / sOffer.mSize) : 0);
        const std::string platform = PLATFORM_WORDS;
        std::string title, body, button;
        bool not_now = false;
        switch (sStage)
        {
        case Stage::OFFER:
            // [BETA CHANNEL 2026-11-12] "A new WolfViewer beta is ready: <name>"
            title = p->getString(sOffer.mBeta ? "title_ready_beta" : "title_ready", a);
            switch (sKind)
            {
            case Install::SELF:     body = p->getString("body_self_" + platform, a); button = p->getString("button_update"); not_now = true; break;
            case Install::PACKAGED: body = p->getString("body_packaged", a); button = p->getString("button_ok"); break;
            case Install::MANUAL:   body = p->getString("body_manual", a); button = p->getString("button_download_page"); not_now = true; break;
            }
            break;
        case Stage::DOWNLOADING:
            title = p->getString("title_downloading", a);
            body = p->getString("body_downloading", a);
            break;
        case Stage::INSTALLING:
            title = p->getString("title_installing", a);
            body = p->getString("body_installing_" + platform, a);
            break;
        case Stage::MAC_OPENED:
            title = p->getString("title_mac_opened", a);
            body = p->getString("body_mac_opened", a);
            button = p->getString("button_quit");
            break;
        }
        p->getChild<LLTextBox>("notice_title")->setText(title);
        p->getChild<LLTextBox>("notice_body")->setText(body);
        LLButton* update = p->getChild<LLButton>("update_btn");
        update->setLabel(button);
        update->setVisible(!button.empty());
        p->getChild<LLButton>("not_now_btn")->setVisible(not_now);
    }

    void notice_back_to_offer()
    {
        if (!sActedAtLogin) return;
        sStage = Stage::OFFER;
        refresh_notice();
    }

    /// Install the verified download from the band. startInstall() refuses when nothing is
    /// downloaded, an install is under way, or there is unsaved ground paint (not before login).
    /// The install coroutine runs at once (LLCoros::launch dispatches), so it may already have
    /// moved the band on (macOS: MAC_OPENED) or back (a failure) by the time this returns.
    void install_from_notice()
    {
        sStage = Stage::INSTALLING;
        refresh_notice();
        if (!WolfUpdate::startInstall()) notice_back_to_offer();
    }

    /// "Not now" (Paul, 2026-11-05): nothing more about THIS release, on the login screen or
    /// in-world; the next release is offered as usual. That is exactly "Skip this version"
    /// (on_available / on_ready / the WolfUpdateManual callback store SKIPPED_SETTING, and
    /// checkCoro makes no offer at all for a release at or below it), so it stores the same.
    /// present_in_world() checks it too, for the rest of this run.
    void on_notice_not_now()
    {
        gSavedSettings.setS32(skip_setting(), sOffer.mRevision);
        // Hidden, not deleted: this runs inside the band's own button. It goes with the login panel.
        if (LLPanel* p = sNotice.get()) p->setVisible(false);
        LL_INFOS("WolfUpdate") << "login screen notice for w" << sOffer.mRevision << " closed (Not now)" << LL_ENDL;
    }

    void on_notice_button()
    {
        if (sStage == Stage::MAC_OPENED)
        {
            // The disk image is open; the old copy can only be replaced once this one has quit.
            LLAppViewer::instance()->requestQuit();
            return;
        }
        if (sStage != Stage::OFFER) return;   // the button is hidden while downloading or installing
        switch (sKind)
        {
        case Install::PACKAGED:
            on_notice_not_now();   // "OK": the system's updater has it, nothing for us to run
            return;
        case Install::MANUAL:
            LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
            return;
        case Install::SELF:
            break;
        }
        sActedAtLogin = true;
        LL_INFOS("WolfUpdate") << "Update now pressed on the login screen for w" << sOffer.mRevision << LL_ENDL;
        if (!sReadyPath.empty())
        {
            install_from_notice();
            return;
        }
        sStage = Stage::DOWNLOADING;
        sNoticeReceived = 0;
        refresh_notice();
        WolfUpdate::startDownload();
    }

    /// A band across the top of the login panel, directly under the menu strip, built from `xml`; nullptr when there
    /// is no login panel. (show_login_notice's own placement code, 2026-11-05, shared with the beta crash band.)
    LLPanel* add_login_band(const char* xml)
    {
        LLPanel* login = sLoginPanel.get();
        if (!login) return nullptr;
        // Built with no parent and added below, as llconversationview.cpp:214 builds its item panel.
        LLPanel* p = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(xml, nullptr, LLPanel::child_registry_t::instance());
        if (!p) return nullptr;
        // Directly under the menu strip: the top of the login page (login_html), which every login
        // panel has (panel_fs_login.xml and panel_fs_nui_login.xml, all skins, top="18"). Full width,
        // following the top, left and right edges, so a resized window keeps it in place.
        S32 top = login->getRect().getHeight();
        if (LLView* web = login->findChildView("login_html"))
        {
            LLRect r;
            web->localRectToOtherView(web->getLocalRect(), &r, login);
            top = r.mTop;
        }
        const S32 height = p->getRect().getHeight();
        p->reshape(login->getRect().getWidth(), height);
        p->setOrigin(0, top - height);
        login->addChild(p);   // in front of the login page, so it draws over it and gets the clicks
        return p;
    }

    // ── [BETA CRASH NOTICE 2026-11-12] "This beta version of WolfViewer closed unexpectedly last time" ──────────
    /// The resident has answered: that crash has been spoken about; the tick mutes the notice for this beta.
    void crash_notice_answered()
    {
        if (sCrashNoticeTime > 0) gSavedSettings.setU32(CRASH_SPOKEN_SETTING, (U32)sCrashNoticeTime);
        if (LLPanel* p = sCrashNotice.get())
        {
            if (p->getChild<LLCheckBoxCtrl>("mute_check")->get()) gSavedSettings.setS32(CRASH_MUTED_SETTING, WolfUpdate::currentRevision());
        }
    }

    void on_crash_keep()
    {
        crash_notice_answered();
        // Hidden, not deleted: this runs inside the band's own button. It goes with the login panel.
        if (LLPanel* p = sCrashNotice.get()) p->setVisible(false);
    }

    /// "Go back to the normal version": the ONE thing it does to the program is untick "Get beta versions". The release
    /// is an OLDER revision than this beta and the updater never installs downwards, by design and without exception
    /// (wolfupdatechannel.h choose), so the going back is the resident's own install from the download page, which this
    /// opens in their browser; the band then says what to do there.
    void on_crash_go_back()
    {
        crash_notice_answered();
        gSavedSettings.setBOOL(BETA_SETTING, false);
        LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
        LL_INFOS("WolfUpdate") << "beta crash notice: Get beta versions turned off, download page opened" << LL_ENDL;
        if (LLPanel* p = sCrashNotice.get())
        {
            p->getChild<LLTextBox>("notice_title")->setText(p->getString("title_going_back"));
            p->getChild<LLTextBox>("notice_body")->setText(p->getString("body_going_back"));
            p->getChild<LLButton>("go_back_btn")->setVisible(false);
            p->getChild<LLCheckBoxCtrl>("mute_check")->setVisible(false);
            p->getChild<LLButton>("keep_btn")->setLabel(p->getString("button_close"));
        }
    }

    /// From loginScreenShown, once per run: decide (wolfupdatechannel.h betaCrashNotice) and show the band.
    void beta_crash_notice_once()
    {
        static bool decided = false;
        if (decided || !sLoginPanel.get() || !at_login_screen()) return;
        decided = true;
        const bool is_beta = WolfUpdate::isBetaBuild();
        if (!is_beta) return;   // never on a release build (nor before any release manifest has been verified)
        LLAppViewer* app = LLAppViewer::instance();
        const bool second = !app || app->isSecondInstance();
        const time_t spoken = (time_t)gSavedSettings.getU32(CRASH_SPOKEN_SETTING);
        const time_t newest = second ? 0 : WolfCrashReport::newestCrashOfThisBuild(WolfUpdate::currentRevision(), spoken);
        // W104's Vulkan notices about a crash of the last run (wolfvkbridge.cpp rememberNotice): that crash has its
        // explanation, and WolfViewer has already acted on it; this band is not stacked on top.
        const std::string& vk = WolfVkBridge::pendingNoticeName();
        const bool vulkan_notice = vk == "WolfVulkanCrashedTwice" || vk == "WolfVulkanFellBackLastStart";
        switch (WolfUpdateChannel::betaCrashNotice(is_beta, second, (long long)newest, (long long)spoken,
                                                   gSavedSettings.getS32(CRASH_MUTED_SETTING), WolfUpdate::currentRevision(), vulkan_notice))
        {
        case WolfUpdateChannel::CrashNotice::NO:
            return;
        case WolfUpdateChannel::CrashNotice::MARK:
            gSavedSettings.setU32(CRASH_SPOKEN_SETTING, (U32)newest);
            LL_INFOS("WolfUpdate") << "beta crash notice not shown (" << (vulkan_notice ? "a Vulkan notice covers that crash" : "turned off for this beta") << ")" << LL_ENDL;
            return;
        case WolfUpdateChannel::CrashNotice::SHOW:
            break;
        }
        if (!sNotice.isDead()) return;   // the update band is already up: it is the better advice (not marked; may show next time)
        LLPanel* p = add_login_band("panel_wolf_beta_crash_notice.xml");
        if (!p) return;
        sCrashNotice = p->getHandle();
        sCrashNoticeTime = newest;
        p->getChild<LLTextBox>("notice_title")->setText(p->getString("title_crashed"));
        p->getChild<LLTextBox>("notice_body")->setText(p->getString(gSavedSettings.getBOOL("WolfCrashReportsAuto") ? "body_crashed_sent" : "body_crashed"));
        p->getChild<LLButton>("keep_btn")->setClickedCallback(boost::bind(&on_crash_keep));
        p->getChild<LLButton>("go_back_btn")->setClickedCallback(boost::bind(&on_crash_go_back));
        LL_INFOS("WolfUpdate") << "beta crash notice shown on the login screen" << LL_ENDL;
    }

    /// The band, on the login panel, once per release until Not now. Not used once logged in.
    void show_login_notice()
    {
        if (!sHaveOffer || !at_login_screen()) return;
        if (!sNotice.isDead())
        {
            refresh_notice();
            return;
        }
        LLPanel* login = sLoginPanel.get();
        if (!login) return;
        if (gSavedSettings.getS32(skip_setting()) >= sOffer.mRevision) return;   // Not now, this run

        LLPanel* p = add_login_band("panel_wolf_update_notice.xml");
        if (!p) return;
        // [BETA CRASH NOTICE 2026-11-12] A newer build is the better advice: the update band takes the crash band's
        // place. The crash is not marked as spoken about, so the crash band can return if this offer is declined.
        if (LLPanel* crash = sCrashNotice.get()) crash->setVisible(false);
        p->getChild<LLButton>("update_btn")->setClickedCallback(boost::bind(&on_notice_button));
        p->getChild<LLButton>("not_now_btn")->setClickedCallback(boost::bind(&on_notice_not_now));
        sNotice = p->getHandle();
        sStage = Stage::OFFER;

        // The band says what the one-off in-world notices for these two would have said.
        if (sKind != Install::SELF) gSavedSettings.setS32(told_setting(), sOffer.mRevision);
        refresh_notice();
        LL_INFOS("WolfUpdate") << "login screen notice shown for w" << sOffer.mRevision << LL_ENDL;
    }

    /// In-world, once per run: what the check has always done after login (2026-10-02), unless
    /// Update now was already pressed on the login screen.
    void present_in_world()
    {
        if (sInWorldDone || !sHaveOffer) return;
        sInWorldDone = true;
        if (sActedAtLogin) return;   // that download reports for itself (downloadCoro)
        // Not now on the login screen's band, after this run's check had already made the offer.
        if (gSavedSettings.getS32(skip_setting()) >= sOffer.mRevision) return;
        switch (sKind)
        {
        case Install::PACKAGED:
            // The system's updater has it; say so once per release.
            if (gSavedSettings.getS32(told_setting()) >= sOffer.mRevision) return;
            gSavedSettings.setS32(told_setting(), sOffer.mRevision);
            LLNotificationsUtil::add("WolfUpdatePackaged", release_args());
            return;
        case Install::MANUAL:
        {
            if (gSavedSettings.getS32(told_setting()) >= sOffer.mRevision) return;
            gSavedSettings.setS32(told_setting(), sOffer.mRevision);
            LLSD args = release_args();
            args["DIR"] = sKindWhere;
            LLNotificationsUtil::add("WolfUpdateManual", args, LLSD(), [](const LLSD& n, const LLSD& r)
            {
                const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
                if (opt == 0) LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
                else if (opt == 2) gSavedSettings.setS32(skip_setting(), sOffer.mRevision);
            });
            return;
        }
        case Install::SELF:
            break;
        }
        if (gSavedSettings.getBOOL(AUTO_DOWNLOAD_SETTING))
        {
            WolfUpdate::startDownload();
        }
        else
        {
            LLNotificationsUtil::add("WolfUpdateAvailable", release_args(), LLSD(), [](const LLSD& n, const LLSD& r) { on_available(n, r); });
        }
    }
}

// static
void WolfUpdate::loginScreenShown(LLPanel* login_panel)
{
    if (login_panel) sLoginPanel = login_panel->getHandle();
    checkOnce();
    show_login_notice();   // no-op until the check has answered; checkCoro calls it then
    // [BETA CRASH NOTICE 2026-11-12] The crash band is decided once the update check has answered (checkCoro's
    // Running), because that answer is what says whether this build is still a beta: straight after a promotion the
    // first start would otherwise call the release "this beta". With no check coming (turned off, or already
    // answered before this panel existed) it is decided now; if the check has not answered within CRASH_BAND_WAIT_S
    // (no network) it is decided on what was known before.
    if (sCheckAnswered || !gSavedSettings.getBOOL(CHECK_SETTING))
    {
        beta_crash_notice_once();
    }
    else
    {
        LLCoros::instance().launch("WolfUpdate crash band", []()
        {
            for (F32 waited = 0.f; waited < CRASH_BAND_WAIT_S && !sCheckAnswered; waited += 0.5f)
            {
                llcoro::suspendUntilTimeout(0.5f);
                if (LLApp::isExiting()) return;
            }
            beta_crash_notice_once();   // a no-op if the check's answer already decided it
        });
    }
}

// static
void WolfUpdate::loggedIn()
{
    sLoggedIn = true;
    checkOnce();           // the login screen may have been skipped (no connect box)
    present_in_world();    // no-op until the check has answered; checkCoro calls it then
    // [BETA CHANNEL 2026-11-12] the version record, once per run; its own switch in Preferences > Privacy [review]
    static bool seen_sent = false;
    // Sent only with BOTH switches on: someone who turned the update check off is not newly reported [review 2].
    if (!seen_sent && gSavedSettings.getBOOL(CHECK_SETTING) && gSavedSettings.getBOOL(REPORT_VERSION_SETTING) && WolfGrid::isWolfTerritories())
    {
        seen_sent = true;
        LLCoros::instance().launch("WolfUpdate seen", []() { seen_coro(); });
    }
}

// static
void WolfUpdate::checkCoro()
{
    using namespace WolfUpdateChannel;
    // [STAGED ROLLOUT 2026-11-12] one check at a time; asked by hand = not staged, earlier "Not now" forgotten, always answers
    struct Running
    {
        Running() { sCheckRunning = true; }
        ~Running()
        {
            sCheckRunning = false;
            sCheckAnswered = true;
            // Help > Check for WolfViewer Updates was chosen while this check was under way: it gets its own answer
            // now (a by-hand check: not staged, earlier "Not now" forgotten; pause, signature and "never downwards"
            // apply as always). The second pair of small fetches is the price of always answering a click.
            if (sManualPending)
            {
                sManualPending = false;
                WolfUpdate::checkByHand();
            }
            else
            {
                beta_crash_notice_once();   // the crash band waits for the check, so a build promoted since is not called a beta
            }
        }
    } running;
    const bool by_hand = sByHand;
    sByHand = false;

    // What this file does: fetch, read the settings, apply the answer. The thinking - signature, JSON, feed and
    // sequence rules, staged rollout, what to remember - is WolfUpdateManifest::decide (wolfupdatemanifest.h), which
    // tests/wolfupdatemanifest_harness.cpp drives with real signed manifests.
    WolfUpdateManifest::Inputs in;
    in.mPlatform = platform_key();
    in.mMine = currentRevision();
    in.mByHand = by_hand;
    // The release feed, exactly as W104 asked for it: the same two files, first, whatever the beta setting.
    in.mStableFetched = fetch_feed(MANIFEST_URL, SIGNATURE_URL, in.mStableManifest, in.mStableSignature);
    // [BETA CHANNEL 2026-11-12] The beta feed, only for a resident who ticked "Get beta versions", and never
    // for a copy from the Linux package repositories (they carry releases only; the Preferences box is greyed
    // out there, but the setting is shared with any other copy on this computer). Not asked for at all when the
    // release feed gave nothing: without a release answer nothing is acted on.
    in.mBetaOptIn = gSavedSettings.getBOOL(BETA_SETTING);
    if (in.mBetaOptIn && in.mStableFetched)
    {
        std::string where;
        in.mFollowBeta = install_kind(where) != Install::PACKAGED;
    }
    if (in.mFollowBeta)
    {
        in.mBetaFetched = fetch_feed(BETA_MANIFEST_URL, BETA_SIGNATURE_URL, in.mBetaManifest, in.mBetaSignature);
    }
    in.mSkippedStable = gSavedSettings.getS32(SKIPPED_SETTING);
    in.mSkippedBeta = gSavedSettings.getS32(SKIPPED_BETA_SETTING);
    in.mStableSeen = gSavedSettings.getS32(STABLE_SEEN_SETTING);
    in.mManifestRevisionSeen = gSavedSettings.getS32(MANIFEST_REVISION_SETTING);
    in.mManifestSequenceSeen = (long long)gSavedSettings.getU32(MANIFEST_SEQUENCE_SETTING);
    in.mBetaSequenceSeen = (long long)gSavedSettings.getU32(BETA_SEQUENCE_SETTING);
    in.mRolloutFirstSeenRevision = gSavedSettings.getS32(ROLLOUT_SEEN_REVISION_SETTING);
    in.mRolloutFirstSeenTime = (long long)gSavedSettings.getU32(ROLLOUT_SEEN_TIME_SETTING);
    in.mSeed = rollout_seed();
    in.mNow = (long long)time(nullptr);

    const WolfUpdateManifest::Outcome out = WolfUpdateManifest::decide(in);

    // What to remember.
    if (out.mManifestRevisionSeen != in.mManifestRevisionSeen || out.mManifestSequenceSeen != in.mManifestSequenceSeen)
    {
        gSavedSettings.setS32(MANIFEST_REVISION_SETTING, out.mManifestRevisionSeen);
        gSavedSettings.setU32(MANIFEST_SEQUENCE_SETTING, (U32)out.mManifestSequenceSeen);
    }
    if (out.mBetaSequenceSeen != in.mBetaSequenceSeen) gSavedSettings.setU32(BETA_SEQUENCE_SETTING, (U32)out.mBetaSequenceSeen);
    if (out.mRolloutFirstSeenRevision != in.mRolloutFirstSeenRevision || out.mRolloutFirstSeenTime != in.mRolloutFirstSeenTime)
    {
        gSavedSettings.setS32(ROLLOUT_SEEN_REVISION_SETTING, out.mRolloutFirstSeenRevision);
        gSavedSettings.setU32(ROLLOUT_SEEN_TIME_SETTING, (U32)out.mRolloutFirstSeenTime);
    }
    if (out.mStable.mVerified)
    {
        LL_INFOS("WolfUpdate") << "this build is w" << in.mMine << ", latest release is w" << out.mStable.mRevision
                               << " \"" << out.mStable.mName << "\" (" << platform_key() << ")" << LL_ENDL;
        note_stable_revision(out.mStableSeen);   // the labels follow the release (only ever upwards)
        if (out.mStable.mRollout.mPresent)
        {
            LL_INFOS("WolfUpdate") << "release rollout: " << out.mPercent << "% now" << (out.mStable.mRollout.mPaused ? " (paused)" : "")
                                   << "; this install's place is " << out.mBucket
                                   << ((by_hand || in.mBetaOptIn) ? " (not staged: asked by hand or beta versions on)" : "") << LL_ENDL;
        }
    }
    if (in.mFollowBeta)
    {
        if (out.mBeta.mVerified)
        {
            LL_INFOS("WolfUpdate") << "beta feed: w" << out.mBeta.mRevision << " \"" << out.mBeta.mName << "\"" << LL_ENDL;
        }
        else
        {
            LL_WARNS("WolfUpdate") << "update check: no usable beta/latest.json this time ("
                                   << (!in.mBetaFetched ? "not reached" : out.mBetaStale ? "older than one already seen" : "signature or channel")
                                   << "); the release answer stands, downloads are kept" << LL_ENDL;
        }
    }

    switch (out.mVerdict)
    {
    case Verdict::FAILED:
        // Not reached, not ours, or tampered with. Said in the log, never acted on.
        if (in.mStableFetched) LL_WARNS("WolfUpdate") << "update check: latest.json signature does not verify or it is unreadable; ignored" << LL_ENDL;
        if (by_hand) notify_check_failed();
        return;
    case Verdict::STALE:
        LL_WARNS("WolfUpdate") << "update check: latest.json for w" << out.mStable.mRevision << " has sequence " << out.mStable.mSequence
                               << ", older than the " << in.mManifestSequenceSeen << " already seen for that release; ignored" << LL_ENDL;
        // By hand: what is true here is that the server answered with an OLDER notice about a release this viewer
        // already knows more recently, so nothing is on offer from it. Not "check your internet connection".
        if (by_hand) LLNotificationsUtil::add(out.mStaleButCurrent ? "WolfUpdateNone" : "WolfUpdateNotOffered");
        return;
    case Verdict::REFUSED:
        LL_WARNS("WolfUpdate") << "update check: latest.json is not a release manifest (channel \"" << out.mStable.mChannel
                               << "\"); ignored" << LL_ENDL;
        if (by_hand) notify_check_failed();
        return;
    case Verdict::UP_TO_DATE:
        if (out.mCleanUpdates) clean_updates("");   // anything downloaded before is this build or older
        if (by_hand) LLNotificationsUtil::add("WolfUpdateNone");
        return;
    case Verdict::UNUSABLE:
        LL_WARNS("WolfUpdate") << "update check: latest.json entry for " << platform_key() << " is not usable" << LL_ENDL;
        if (by_hand) notify_check_failed();
        return;
    case Verdict::SKIPPED:
        return;
    case Verdict::PAUSED:
        LL_INFOS("WolfUpdate") << "w" << out.mStable.mRevision << " is paused: not offered" << LL_ENDL;
        if (by_hand) LLNotificationsUtil::add("WolfUpdatePaused");
        return;
    case Verdict::HELD:
        LL_INFOS("WolfUpdate") << "w" << out.mStable.mRevision << " is not yet offered to this install's share; Help > Check for "
                                  "WolfViewer Updates offers it at once" << LL_ENDL;
        return;
    case Verdict::OFFER:
        break;
    }
    const bool from_beta = out.mFeed == Feed::BETA;
    Offer o;
    o.mRevision = out.mOffer.mRevision;
    o.mName = out.mOffer.mName;
    o.mUrl = out.mOffer.mUrl;
    o.mSha256 = out.mOffer.mSha256;
    o.mSize = out.mOffer.mSize;
    o.mBeta = from_beta;
    if (by_hand && sHaveOffer && sOffer.mRevision == o.mRevision && (sBusy || !sReadyPath.empty()))
    {
        // Asked by hand while this very release is already downloading or downloaded: say where it stands.
        if (sBusy) LLNotificationsUtil::add("WolfUpdateDownloading", release_args());
        else notify_ready();
        return;
    }
    sOffer = o;
    sKind = install_kind(sKindWhere);
    sHaveOffer = true;
    if (by_hand)
    {
        // Asked by hand: an earlier "Not now" / "told once" for this release no longer applies, and the in-world
        // notice may be shown again this run.
        if (gSavedSettings.getS32(skip_setting()) >= o.mRevision) gSavedSettings.setS32(skip_setting(), o.mRevision - 1);
        if (gSavedSettings.getS32(told_setting()) >= o.mRevision) gSavedSettings.setS32(told_setting(), o.mRevision - 1);
        sInWorldDone = false;
        sActedAtLogin = false;
        if (LLPanel* band = sNotice.get()) band->setVisible(true);
    }
    LL_INFOS("WolfUpdate") << "offering w" << o.mRevision << (from_beta ? " from the beta feed" : " from the release feed") << LL_ENDL;
    // Where the resident is now decides how they are told: still on the login screen, the band;
    // already in-world (a quick or automatic login beat this answer), the in-world notices.
    if (sLoggedIn) present_in_world();
    else show_login_notice();
}

// static
void WolfUpdate::startDownload()
{
    if (sBusy) return;
    sBusy = true;
    LLCoros::instance().launch("WolfUpdate download", []() { WolfUpdate::downloadCoro(); });
}

// static
void WolfUpdate::downloadCoro()
{
    const std::string dir = updates_dir();
    if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
    auto job = std::make_shared<Job>();
    job->mUrl = sOffer.mUrl;
    job->mSha256 = sOffer.mSha256;
    job->mSize = sOffer.mSize;
    job->mPath = dir + gDirUtilp->getDirDelimiter() + llformat("w%d-", sOffer.mRevision) + offer_file_name(sOffer);
    job->mPartPath = job->mPath + ".part";
    job->mCaFile = gDirUtilp->getCAFile();
    job->mUserAgent = LLVersionInfo::instance().getChannelAndVersion();
    clean_updates(job->mPath);   // older downloads; a matching copy of this one is reused
    LL_INFOS("WolfUpdate") << "downloading w" << sOffer.mRevision << " (" << sOffer.mSize << " bytes) to " << job->mPath << LL_ENDL;
    // From the login screen's band the band shows the progress; in-world, the tip as before.
    if (!(sActedAtLogin && at_login_screen())) LLNotificationsUtil::add("WolfUpdateDownloading", release_args());
    {
        std::lock_guard<std::mutex> lock(sThreadMutex);
        if (sThread.joinable()) sThread.join();
        sJob = job;
        sThread = std::thread(run_job, job);
    }
    while (!job->mDone.load())
    {
        llcoro::suspendUntilTimeout(0.5f);
        if (LLApp::isExiting()) return;   // shutdown() joins the thread
        if (sActedAtLogin)
        {
            sNoticeReceived = job->mReceived.load();
            refresh_notice();
        }
    }
    {
        std::lock_guard<std::mutex> lock(sThreadMutex);
        if (sThread.joinable()) sThread.join();
        sJob.reset();
    }
    sBusy = false;
    if (!job->mOk)
    {
        notify_failed(job->mError);
        return;
    }
    sReadyPath = job->mPath;
    LL_INFOS("WolfUpdate") << "w" << sOffer.mRevision << " downloaded and verified: " << sReadyPath << LL_ENDL;
    if (sActedAtLogin)
    {
        // Asked for on the login screen and still there: install now, as Update now said.
        if (at_login_screen())
        {
            install_from_notice();
            return;
        }
        // Logged in while it downloaded: ask, as any in-world download does. The band (behind
        // the progress screen if a login is under way) offers Update now again.
        notice_back_to_offer();
    }
    notify_ready();
}

// static
bool WolfUpdate::startInstall()
{
    if (sBusy || sReadyPath.empty()) return false;
    // Unsaved ground paint lives only in memory (wolfterrainpaint.cpp hasUnsavedPaint); a restart
    // would lose it, and the restart here does not go through the quit confirmation.
    std::string paint_region;
    if (WolfTerrainPaint::instance().hasUnsavedPaint(paint_region))
    {
        LLNotificationsUtil::add("WolfUpdateUnsavedPaint", LLSD().with("REGION", paint_region));
        return false;
    }
    sBusy = true;
    LLCoros::instance().launch("WolfUpdate install", []() { WolfUpdate::installCoro(); });
    return true;
}

// static
void WolfUpdate::installCoro()
{
    const std::string path = sReadyPath;
#if LL_WINDOWS
    // The installer asks Windows for administrator rights itself (installer_template.nsi
    // RequestExecutionLevel admin), so ShellExecuteEx shows the permission prompt and returns once
    // it is answered. Then the viewer quits; the installer waits for its window to close
    // (CloseSecondLife), installs into the same folder and starts it again (.onInstSuccess).
    std::wstring file = ll_convert_string_to_wide(path);
    std::wstring params = L"/S /SKIP_DIALOGS /UPDATE";
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        const DWORD err = GetLastError();
        sBusy = false;
        notify_failed(err == ERROR_CANCELLED ? std::string("Windows permission to install was not given")
                                             : llformat("the installer could not be started (error %lu)", (unsigned long)err));
        return;
    }
    LL_INFOS("WolfUpdate") << "installer started; quitting so it can replace this copy" << LL_ENDL;
    LLAppViewer::instance()->requestQuit();
#elif LL_DARWIN
    // Open the disk image in Finder; dragging WolfViewer onto Applications replaces this copy.
    const pid_t pid = spawn({ "/usr/bin/open", path });
    sBusy = false;
    if (pid < 0) { notify_failed("the disk image could not be opened"); return; }
    int st = 0;
    waitpid(pid, &st, 0);   // `open` returns at once
    if (sActedAtLogin && sStage == Stage::INSTALLING)
    {
        sStage = Stage::MAC_OPENED;   // the band now says what to do in Finder, with Quit WolfViewer
        refresh_notice();
    }
#else
    std::string where;
    if (install_kind(where) != Install::SELF) { sBusy = false; notify_failed("this copy cannot update itself"); return; }
    if (!(sActedAtLogin && at_login_screen())) LLNotificationsUtil::add("WolfUpdateInstalling", release_args());   // else the band says it
    const pid_t pid = spawn({ "/bin/bash", where + "/etc/wolfviewer-update.sh", "apply", path, where, llformat("%d", (S32)getpid()) });
    if (pid < 0) { sBusy = false; notify_failed("the update helper could not be started"); return; }
    int st = 0;
    for (;;)
    {
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0) { st = -1; break; }
        llcoro::suspendUntilTimeout(0.25f);
    }
    sBusy = false;
    if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
    {
        notify_failed(llformat("the new version could not be put in place (helper exit %d); this copy is unchanged",
                               WIFEXITED(st) ? WEXITSTATUS(st) : -1));
        return;
    }
    // The helper has swapped the folders and is waiting for this process to exit before it
    // starts the new copy.
    LLFile::remove(path, ENOENT);
    LL_INFOS("WolfUpdate") << "new version in place at " << where << "; restarting" << LL_ENDL;
    LLAppViewer::instance()->requestQuit();
#endif
}
