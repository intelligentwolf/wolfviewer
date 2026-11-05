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
#include "llstartup.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llweb.h"
#include "wolfterrainpaint.h"

namespace
{
    // ── what is trusted ──────────────────────────────────────────────────────────────────────
    const char* const MANIFEST_URL = "https://wolf-grid.com/downloads/wolfviewer/latest.json";
    const char* const SIGNATURE_URL = "https://wolf-grid.com/downloads/wolfviewer/latest.json.sig";
    /// Every download must live here: the website serves the installers (viewers.php reads them
    /// from /downloads/wolfviewer/), and a manifest naming anywhere else is refused.
    const char* const DOWNLOAD_PREFIX = "https://wolf-grid.com/downloads/wolfviewer/";
    /// f=osv, NOT f=viewers: index.php routes "osv" to viewers.php (2026-09-12 fix).
    const char* const DOWNLOAD_PAGE_URL = "https://www.wolf-grid.com/index.php?f=osv";
    /// The manifest signing key's public half (Ed25519, 32 bytes). The private half is
    /// ~/wolfviewer-releases/keys/manifest-ed25519.pem, generated 2026-10-02.
    const unsigned char MANIFEST_PUBKEY[32] = {
        0x65, 0x44, 0x7d, 0x1e, 0xae, 0xf9, 0xdf, 0xa5, 0x6b, 0x2a, 0xbc, 0xdc, 0x72, 0x65, 0x77, 0x65,
        0x7c, 0x52, 0x13, 0x40, 0x4b, 0x7f, 0x28, 0x1f, 0x09, 0x84, 0xc2, 0x32, 0x78, 0x7b, 0x2e, 0xe5 };

    // ── settings ─────────────────────────────────────────────────────────────────────────────
    const char* const CHECK_SETTING = "WolfViewerCheckForUpdates";
    /// The newest release already told about, for the one-off notices (package manager, manual).
    const char* const TOLD_SETTING = "WolfViewerUpdateLastTold";
    /// "Skip this version": the newest release the resident declined for good.
    const char* const SKIPPED_SETTING = "WolfViewerUpdateSkipped";
    const char* const AUTO_DOWNLOAD_SETTING = "WolfViewerAutoDownloadUpdates";

    enum class Install { SELF, PACKAGED, MANUAL };

    struct Offer
    {
        S32 mRevision = 0;
        std::string mName;      // the release name, "Bouncing Panda"
        std::string mUrl;
        std::string mSha256;    // lower-case hex
        U64 mSize = 0;
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

    std::string hex(const unsigned char* p, size_t n)
    {
        static const char* d = "0123456789abcdef";
        std::string s;
        s.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
        return s;
    }

    bool verify_signature(const std::string& message, const std::string& signature)
    {
        if (signature.size() != 64) return false;
        bool ok = false;
        EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, MANIFEST_PUBKEY, sizeof(MANIFEST_PUBKEY));
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

    /// The file name of a download URL: DOWNLOAD_PREFIX, then optionally the release's own
    /// folder "w<digits>/" (publish_downloads.sh links each release there, so a URL in a signed
    /// manifest never changes under it while the next release is being published), then a plain
    /// file name. "" when the URL is not of that shape.
    std::string download_file_name(const std::string& url);

    /// A plain file name only: the URL's last part, which becomes a path on disk.
    bool safe_file_name(const std::string& name)
    {
        if (name.empty() || name.size() > 128 || name[0] == '.') return false;
        for (char c : name)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
                return false;
        }
        return true;
    }

    std::string download_file_name(const std::string& url)
    {
        if (url.rfind(DOWNLOAD_PREFIX, 0) != 0) return std::string();
        std::string rest = url.substr(strlen(DOWNLOAD_PREFIX));
        const size_t slash = rest.find('/');
        if (slash != std::string::npos)
        {
            const std::string folder = rest.substr(0, slash);
            if (folder.size() < 2 || folder[0] != 'w'
                || folder.find_first_not_of("0123456789", 1) != std::string::npos)
            {
                return std::string();
            }
            rest = rest.substr(slash + 1);
        }
        return safe_file_name(rest) ? rest : std::string();
    }

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
            gSavedSettings.setS32(SKIPPED_SETTING, sOffer.mRevision);
        }
        return false;
    }

    bool on_ready(const LLSD& n, const LLSD& r)
    {
        const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
        if (opt == 2)
        {
            gSavedSettings.setS32(SKIPPED_SETTING, sOffer.mRevision);
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
void WolfUpdate::checkOnce()
{
    if (sChecked) return;
    sChecked = true;
    if (!gSavedSettings.getBOOL(CHECK_SETTING)) return;
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

    // ── the band on the login screen (ticket 532) ────────────────────────────────────────────
#if LL_WINDOWS
    const char* const PLATFORM_WORDS = "windows";   // panel_wolf_update_notice.xml body_*_windows
#elif LL_DARWIN
    const char* const PLATFORM_WORDS = "mac";
#else
    const char* const PLATFORM_WORDS = "linux";
#endif

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
        a["[SIZE]"] = r["SIZE"].asString();
        a["[DIR]"] = sKindWhere;
        a["[PERCENT]"] = llformat("%d", sOffer.mSize ? (S32)((sNoticeReceived * 100) / sOffer.mSize) : 0);
        const std::string platform = PLATFORM_WORDS;
        std::string title, body, button;
        bool not_now = false;
        switch (sStage)
        {
        case Stage::OFFER:
            title = p->getString("title_ready", a);
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
        gSavedSettings.setS32(SKIPPED_SETTING, sOffer.mRevision);
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
        if (gSavedSettings.getS32(SKIPPED_SETTING) >= sOffer.mRevision) return;   // Not now, this run

        // Built with no parent and added below, as llconversationview.cpp:214 builds its item panel.
        LLPanel* p = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
            "panel_wolf_update_notice.xml", nullptr, LLPanel::child_registry_t::instance());
        if (!p) return;
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
        p->getChild<LLButton>("update_btn")->setClickedCallback(boost::bind(&on_notice_button));
        p->getChild<LLButton>("not_now_btn")->setClickedCallback(boost::bind(&on_notice_not_now));
        login->addChild(p);   // in front of the login page, so it draws over it and gets the clicks
        sNotice = p->getHandle();
        sStage = Stage::OFFER;

        // The band says what the one-off in-world notices for these two would have said.
        if (sKind != Install::SELF) gSavedSettings.setS32(TOLD_SETTING, sOffer.mRevision);
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
        if (gSavedSettings.getS32(SKIPPED_SETTING) >= sOffer.mRevision) return;
        switch (sKind)
        {
        case Install::PACKAGED:
            // The system's updater has it; say so once per release.
            if (gSavedSettings.getS32(TOLD_SETTING) >= sOffer.mRevision) return;
            gSavedSettings.setS32(TOLD_SETTING, sOffer.mRevision);
            LLNotificationsUtil::add("WolfUpdatePackaged", release_args());
            return;
        case Install::MANUAL:
        {
            if (gSavedSettings.getS32(TOLD_SETTING) >= sOffer.mRevision) return;
            gSavedSettings.setS32(TOLD_SETTING, sOffer.mRevision);
            LLSD args = release_args();
            args["DIR"] = sKindWhere;
            LLNotificationsUtil::add("WolfUpdateManual", args, LLSD(), [](const LLSD& n, const LLSD& r)
            {
                const S32 opt = LLNotificationsUtil::getSelectedOption(n, r);
                if (opt == 0) LLWeb::loadURLExternal(DOWNLOAD_PAGE_URL);
                else if (opt == 2) gSavedSettings.setS32(SKIPPED_SETTING, sOffer.mRevision);
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
}

// static
void WolfUpdate::loggedIn()
{
    sLoggedIn = true;
    checkOnce();           // the login screen may have been skipped (no connect box)
    present_in_world();    // no-op until the check has answered; checkCoro calls it then
}

// static
void WolfUpdate::checkCoro()
{
    std::string manifest, signature;
    if (!fetch_small(MANIFEST_URL, manifest) || !fetch_small(SIGNATURE_URL, signature)) return;
    if (!verify_signature(manifest, signature))
    {
        // Not ours, or tampered with. Said in the log, never acted on.
        LL_WARNS("WolfUpdate") << "update check: latest.json signature does not verify; ignored" << LL_ENDL;
        return;
    }
    boost::system::error_code ec;
    const boost::json::value v = boost::json::parse(manifest, ec);
    if (ec) { LL_WARNS("WolfUpdate") << "update check: unreadable latest.json" << LL_ENDL; return; }
    const LLSD m = LlsdFromJson(v);
    const LLSD file = m["files"][platform_key()];
    Offer o;
    o.mRevision = m["revision"].asInteger();
    o.mName = m["release_name"].asString();
    o.mUrl = file["url"].asString();
    o.mSha256 = file["sha256"].asString();
    o.mSize = (U64)file["size"].asReal();
    LLStringUtil::toLower(o.mSha256);
    const S32 mine = currentRevision();
    LL_INFOS("WolfUpdate") << "this build is w" << mine << ", latest release is w" << o.mRevision
                           << " \"" << o.mName << "\" (" << platform_key() << ")" << LL_ENDL;
    if (o.mRevision <= mine)
    {
        clean_updates("");   // anything downloaded before is this build or older
        return;
    }
    if (download_file_name(o.mUrl).empty() || o.mSha256.size() != 64 || o.mSize == 0)
    {
        LL_WARNS("WolfUpdate") << "update check: latest.json entry for " << platform_key() << " is not usable" << LL_ENDL;
        return;
    }
    if (gSavedSettings.getS32(SKIPPED_SETTING) >= o.mRevision) return;
    sOffer = o;
    sKind = install_kind(sKindWhere);
    sHaveOffer = true;
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
    job->mPath = dir + gDirUtilp->getDirDelimiter() + llformat("w%d-", sOffer.mRevision) + download_file_name(sOffer.mUrl);
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
