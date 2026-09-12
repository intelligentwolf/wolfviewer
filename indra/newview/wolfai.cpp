/**
 * @file wolfai.cpp
 * @brief AI assist client — see wolfai.h for what this is and why it lives behind the proxy.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid. Modified by IntelligentWolf Ltd, 2026.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolfai.h"

#include <boost/json.hpp>
#include <fstream>
#include <limits>
#include <png.h>
#include "llbase64.h"
#include "llcombobox.h"
#include "llscrolllistctrl.h"
#include "llimagepng.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llviewertexture.h"
#include "llmediactrl.h"
#include "llpluginclassmedia.h"
#include "llrender.h"
#include "llgl.h"
#include "llstyle.h"
#include "llviewercontrol.h"

#include "llagent.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lleventcoro.h"   // llcoro::suspendUntilTimeout (lleventcoro.h:85)
#include "llfile.h"        // llofstream (llfile.h:289), LLFile::remove
#include "llframetimer.h"
#include "llhttpconstants.h"
#include "llbutton.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lluictrlfactory.h"   // LLPanelInjector
#include "llweb.h"            // the Buy credits button opens the store
#include "lluicolortable.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "lluri.h"         // LLURI::escape (lluri.h:144)
#include "wolfgrid.h"
#include "wolfmeshupload.h"

namespace
{
    /// How long an availability answer is trusted. Short enough that a level change takes effect
    /// without a restart; long enough that opening a floater is not a round trip every time.
    const F64 AVAIL_CACHE_SECS = 120.0;
    /// Between status polls of a running generation.
    const F32 POLL_SECS = 4.f;
    /// Give up watching a generation after this. The proxy's own ceiling is 900 s; this is a
    /// little longer so the service's message wins rather than ours.
    const F64 WATCH_LIMIT_SECS = 1000.0;

    std::string ai_url(const std::string& path)
    {
        return std::string(WolfGrid::PROXY_API_BASE) + path;
    }

    // Source: llsd.h Integer is S32; LlsdFromJson narrows int64. Reject overflow before conversion.
    bool json_fits(const boost::json::value& value)
    {
        if (value.is_int64()) return value.as_int64() >= std::numeric_limits<S32>::min() && value.as_int64() <= std::numeric_limits<S32>::max();
        if (value.is_uint64()) return value.as_uint64() <= U64(std::numeric_limits<S32>::max());
        if (value.is_array()) for (const auto& v : value.as_array()) { if (!json_fits(v)) return false; }
        if (value.is_object()) for (const auto& v : value.as_object()) { if (!json_fits(v.value())) return false; }
        return true;
    }

    /// Source: wolfterrainpaint.cpp:74 json_to_llsd — the same decode every Wolf route uses.
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec || !json_fits(v)) return LLSD();
        return LlsdFromJson(v);
    }

    /// The pair every Wolf proxy route authenticates with (main.rs ai_identify: agent + session,
    /// checked against the grid's presence service).
    void add_wolf_headers(LLCore::HttpHeaders::ptr_t& headers, const WolfAI::Session& auth)
    {
        headers->append("X-Wolf-Agent", auth.agent.asString());
        headers->append("X-Wolf-Session", auth.session.asString());
    }

    /// Inside a coroutine: GET a Wolf route and decode its JSON reply.
    /// Source: wolfterrainpaint.cpp fetchCoro — same adapter, same result unpacking.
    S32 get_json(const WolfAI::Session& auth, const std::string& url, LLSD& out, std::string& error, S32 timeout = 30)
    {
        if (!auth.current()) { error = "Your session changed. Reopen the AI window."; return 0; }
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        // Source: HttpOptions defaults allow redirects/retries and omit response headers.
        opts->setWantHeaders(true);
        opts->setFollowRedirects(false);
        opts->setRetries(0);
        opts->setSSLVerifyPeer(true);
        opts->setSSLVerifyHost(true);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        add_wolf_headers(headers, auth);

        LLSD result = adapter->getRawAndSuspend(request, url, opts, headers);
        LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
        out = LLSD();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            out = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /// Inside a coroutine: GET a Wolf route and keep the raw BYTES (the generated model).
    S32 get_bytes(const WolfAI::Session& auth, const std::string& url, LLSD::Binary& out, std::string& filename,
                  std::string& error, S32 timeout = 300, LLSD* metadata = nullptr)
    {
        if (!auth.current()) { error = "Your session changed. Reopen the AI window."; return 0; }
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        // Source: HttpOptions defaults allow redirects/retries and omit response headers.
        opts->setWantHeaders(true);
        opts->setFollowRedirects(false);
        opts->setRetries(0);
        opts->setSSLVerifyPeer(true);
        opts->setSSLVerifyHost(true);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        add_wolf_headers(headers, auth);

        LLSD result = adapter->getRawAndSuspend(request, url, opts, headers);
        LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
        out.clear();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            out = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        }
        // The proxy names the file in a header so the extension survives — the uploader
        // dispatches on it (main.rs, the /ai_mesh_result route).
        if (metadata) *metadata = http_results["headers"];
        filename.clear();
        if (http_results.has("headers") && http_results["headers"].has("x-wolf-filename"))
        {
            filename = http_results["headers"]["x-wolf-filename"].asString();
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /// Inside a coroutine: POST JSON to a Wolf route. Source: wolfmeshupload.cpp:1796 postJson.
    S32 post_json(const WolfAI::Session& auth, const std::string& url, const std::string& body, LLSD& out,
                  std::string& error, S32 timeout = 240)
    {
        if (!auth.current()) { error = "Your session changed. Reopen the AI window."; return 0; }
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        // Source: HttpOptions defaults allow redirects/retries and omit response headers.
        opts->setWantHeaders(true);
        opts->setFollowRedirects(false);
        opts->setRetries(0);
        opts->setSSLVerifyPeer(true);
        opts->setSSLVerifyHost(true);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        add_wolf_headers(headers, auth);

        LLCore::BufferArray::ptr_t raw(new LLCore::BufferArray());
        raw->append(body.data(), body.size());

        LLSD result = adapter->postRawAndSuspend(request, url, raw, opts, headers);
        LLSD http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
        out = LLSD();
        if (result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW))
        {
            out = json_to_llsd(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary());
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /// The service's own wording when it sent one, else something honest about the transport.
    /// The service writes its errors to be shown to a user; a status line is a poor substitute.
    std::string reply_error(const LLSD& reply, const std::string& transport, S32 code)
    {
        if (reply.isMap() && reply.has("error") && reply["error"].asString().length())
        {
            return reply["error"].asString();
        }
        if (transport.length()) return transport;
        return llformat("the AI service returned HTTP %d", code);
    }
    // Source: build_jobs.rs Quote; reject absent/coerced/negative numeric boundary values.
    bool nonnegative(const LLSD& value) { return value.isInteger() && value.asInteger() >= 0; }
    WolfAI::BuildQuote parse_quote(const LLSD& value)
    {
        WolfAI::BuildQuote q;
        q.valid = value.isMap() && nonnegative(value["max_credits"]) && value["max_credits"].asInteger() > 0
            && nonnegative(value["max_components"]) && value["max_components"].asInteger() > 0
            && nonnegative(value["max_extent_m"]) && value["max_extent_m"].asInteger() > 0;
        if (q.valid) { q.maxCredits = value["max_credits"].asInteger(); q.maxComponents = value["max_components"].asInteger(); q.maxExtent = value["max_extent_m"].asInteger(); }
        return q;
    }
    // Source: lluri.h scheme/authority accessors; LLMediaCtrl dispatches custom schemes.
    bool safe_link(const std::string& url)
    {
        if (url.find_first_of("\\\r\n\t ") != std::string::npos) return false;
        LLURI uri(url);
        return uri.scheme() == "https" && !uri.hostName().empty() && uri.userName().empty()
            && uri.authority().find('@') == std::string::npos;
    }

}

WolfAI::WolfAI() {}

void WolfAI::refresh(bool force)
{
    syncSession();
    if (mFetching) return;
    if (!force && mAvail.mAsked && LLFrameTimer::getElapsedSeconds() < mNextRefresh) return;
    if (!WolfGrid::isWolfTerritories()) { mAvail = Avail(); mAvail.mAsked = true; return; }
    if (gAgentID.isNull() || gAgentSessionID.isNull()) return;   // not logged in yet
    mFetching = true;
    mNextRefresh = LLFrameTimer::getElapsedSeconds() + AVAIL_CACHE_SECS;
    const Session auth = mSession;
    LLCoros::instance().launch("WolfAI avail", [auth]() { WolfAI::availCoro(auth); });
}

void WolfAI::availCoro(Session auth)
{
    LLSD reply;
    std::string transport;
    const S32 code = get_json(auth, ai_url("/ai_available"), reply, transport, 20);

    WolfAI& self = WolfAI::instance();
    if (!auth.current()) return;
    self.mFetching = false;
    Avail a;
    a.mAsked = true;
    if (code == 200 && reply.isMap() && reply["success"].asBoolean())
    {
        a.mEnabled   = reply["enabled"].asBoolean();
        a.mAllowed   = reply["allowed"].asBoolean();
        a.mScript    = reply["script"].asBoolean();
        a.mMesh      = reply["mesh"].asBoolean();
        a.mBuild = reply["build"].asBoolean();
        a.mStructure = parse_quote(reply["build_quotes"]["structure"]);
        a.mSettlement = parse_quote(reply["build_quotes"]["settlement"]);
        a.mBalanceKnown = nonnegative(reply["balance"]);
        a.mScriptModel = reply["script_model"].asString();
        a.mMeshModel   = reply["mesh_model"].asString();
        // [AI CREDITS 2026-09-11] Balance and prices, so the panel can say what a model costs
        // and what the resident holds without a second round trip.
        a.mBalance       = reply["balance"].asInteger();
        a.mScriptCost    = reply["script_cost"].asInteger();
        a.mMeshMin       = reply["mesh_min_balance"].asInteger();
        // Source: rust_proxy/src/main.rs AI_MESH_TYPICAL_COST. The fallback tracks that constant
        // so a failed /ai_available never quotes a model as cheaper than it is.
        a.mMeshTypical   = reply["mesh_typical_cost"].asInteger();
        a.mPencePerCredit = reply["pence_per_credit"].asReal();
        a.mBuyUrl        = reply["buy_url"].asString();
        a.mMesh = a.mMesh && reply["mesh"].isBoolean() && a.mBalanceKnown
            && nonnegative(reply["mesh_min_balance"]) && nonnegative(reply["mesh_typical_cost"]);
        a.mBuild = a.mBuild && reply["build"].isBoolean() && a.mBalanceKnown;
        if (!a.mBalanceKnown) a.mError = "Credit balance unavailable. Use Update to retry.";
    }
    else
    {
        a.mError = "Credit service unavailable. Use Update to retry.";
        LL_DEBUGS("WolfAI") << "availability: HTTP " << code << " " << transport << LL_ENDL;
    }
    self.mAvail = a;
    self.error("credits", a.mError);
    ++self.mRevision;
    LL_INFOS("WolfAI") << "AI assist: enabled=" << a.mEnabled << " allowed=" << a.mAllowed
                       << " script=" << a.mScript << " mesh=" << a.mMesh
                       << " balance=" << a.mBalance << LL_ENDL;
}

void WolfAI::requestScript(const std::string& prompt, const std::string& existing, script_fn done)
{
    syncSession();
    if (!done) return;
    if (!WolfGrid::isWolfTerritories())
    {
        done(false, "These tools are only available on Wolf Territories Grid.");
        return;
    }
    if ((S32)prompt.length() > MAX_PROMPT_CHARS)
    {
        done(false, llformat("That request is too long (%d characters max).", MAX_PROMPT_CHARS));
        return;
    }
    if ((S32)existing.length() > MAX_SCRIPT_CHARS)
    {
        done(false, llformat("That script is too long to send (%d characters max).", MAX_SCRIPT_CHARS));
        return;
    }
    LLCoros::instance().launch("WolfAI script",
        [auth = mSession, prompt, existing, done]() { WolfAI::scriptCoro(auth, prompt, existing, done); });
}

void WolfAI::scriptCoro(Session auth, std::string prompt, std::string existing, script_fn done)
{
    LLSD body;
    body["prompt"] = prompt;
    body["script"] = existing;
    const std::string text = boost::json::serialize(LlsdToJson(body));

    LLSD reply;
    std::string transport;
    const S32 code = post_json(auth, ai_url("/ai_script"), text, reply, transport, 240);

    if (!auth.current()) { done(false, "Your session changed. Reopen the AI window."); return; }
    if (code == 200 && reply.isMap() && reply["success"].asBoolean())
    {
        const std::string script = reply["script"].asString();
        if (script.empty())
        {
            done(false, "The AI returned an empty script.");
            return;
        }
        LL_INFOS("WolfAI") << "script: " << reply["tokens_in"].asInteger() << " in / "
                           << reply["tokens_out"].asInteger() << " out tokens, "
                           << reply["cost_cents"].asInteger() << "c ("
                           << reply["model"].asString() << ")" << LL_ENDL;
        done(true, script);
        return;
    }
    done(false, reply_error(reply, transport, code));
}

void WolfAI::generateModel(const std::string& prompt, const std::string& inv_name,
                           progress_fn progress, done_fn done)
{
    syncSession();
    if (!done) return;
    if (!WolfGrid::isWolfTerritories()) { done(false, "These tools are only available on Wolf Territories Grid.", LLUUID::null); return; }
    if (mGenerating)
    {
        done(false, "A model is already being generated.", LLUUID::null);
        return;
    }
    if (!WolfMeshUpload::isAvailable())
    {
        // Said plainly: the generation would succeed and then have nowhere to go, because the
        // texture uploads need a region that accepts them.
        done(false, "Model upload is not available here, so there would be nowhere to put it.", LLUUID::null);
        return;
    }
    if ((S32)prompt.length() > MAX_PROMPT_CHARS)
    {
        done(false, llformat("That description is too long (%d characters max).", MAX_PROMPT_CHARS), LLUUID::null);
        return;
    }
    mGenerating = true;
    LLCoros::instance().launch("WolfAI mesh",
        [auth = mSession, prompt, inv_name, progress, done]() { WolfAI::meshCoro(auth, prompt, inv_name, progress, done); });
}

void WolfAI::meshCoro(Session auth, std::string prompt, std::string inv_name, progress_fn progress, done_fn done)
{
    // Every exit from here must clear mGenerating and call `done` exactly once, or the button
    // stays disabled for the rest of the session.
    auto finish = [auth, done](bool ok, const std::string& msg, const LLUUID& id)
    {
        if (!auth.current()) { done(false, "Your session changed. Reopen the AI window.", LLUUID::null); return; }
        WolfAI& ai = WolfAI::instance();
        ai.mGenerating = false;
        ai.mMeshItem = id;
        ai.mMeshStatus = ok ? "Model delivered to your Objects folder." : msg;
        ai.error("mesh", ok ? "" : msg);
        ++ai.mRevision;
        if (ok) ai.refreshNow();
        done(ok, msg, id);
    };
    auto say = [auth, progress](const std::string& s, bool err)
    {
        if (!auth.current()) return;
        WolfAI::instance().mMeshStatus = s;
        if (progress) progress(s, err);
    };

    say("Asking the AI to build it…", false);

    // ── start the job ────────────────────────────────────────────────────────────────────
    std::string job;
    {
        LLSD body;
        body["prompt"] = prompt;
        const std::string text = boost::json::serialize(LlsdToJson(body));
        LLSD reply;
        std::string transport;
        const S32 code = post_json(auth, ai_url("/ai_mesh"), text, reply, transport, 60);
        if (!auth.current()) { finish(false, "Your session changed. Reopen the AI window.", LLUUID::null); return; }
        if (code != 200 || !reply.isMap() || !reply["success"].asBoolean())
        {
            finish(false, reply_error(reply, transport, code), LLUUID::null);
            return;
        }
        job = reply["job"].asString();
        if (job.empty())
        {
            finish(false, "The AI service did not start a job.", LLUUID::null);
            return;
        }
    }

    // ── wait for it ──────────────────────────────────────────────────────────────────────
    // A poll, not one long request: a generation runs for minutes, past any sane HTTP timeout.
    const F64 started = LLFrameTimer::getElapsedSeconds();
    for (;;)
    {
        llcoro::suspendUntilTimeout(POLL_SECS);
        LLSD reply;
        std::string transport;
        const S32 code = get_json(auth, ai_url("/ai_mesh_status?job=" + LLURI::escape(job)), reply, transport, 30);
        if (!auth.current()) { finish(false, "Your session changed. Reopen the AI window.", LLUUID::null); return; }
        if (code != 200 || !reply.isMap() || !reply["success"].asBoolean())
        {
            finish(false, reply_error(reply, transport, code), LLUUID::null);
            return;
        }
        const std::string state = reply["state"].asString();
        const std::string detail = reply["detail"].asString();
        if (state == "done") break;
        if (state == "failed")
        {
            finish(false, detail.length() ? detail : "The model could not be generated.", LLUUID::null);
            return;
        }
        say(llformat("%s (%ds)", detail.c_str(), reply["elapsed"].asInteger()), false);
        if (LLFrameTimer::getElapsedSeconds() - started > WATCH_LIMIT_SECS)
        {
            finish(false, "The model is taking too long; giving up watching it.", LLUUID::null);
            return;
        }
    }

    // ── collect it ───────────────────────────────────────────────────────────────────────
    say("Collecting the model…", false);
    LLSD::Binary bytes;
    std::string filename;
    {
        std::string transport;
        const S32 code = get_bytes(auth, ai_url("/ai_mesh_result?job=" + LLURI::escape(job)),
                                   bytes, filename, transport, 300);
        if (code != 200 || bytes.empty())
        {
            LLSD nothing;
            finish(false, reply_error(nothing, transport, code), LLUUID::null);
            return;
        }
    }
    if (!auth.current()) { finish(false, "Your session changed. Reopen the AI window.", LLUUID::null); return; }
    if (filename.empty()) filename = "model.glb";

    // ── hand it to the uploader the viewer already has ───────────────────────────────────
    // Through a temp FILE because WolfMeshUpload::Model::load takes a path and dispatches on
    // the extension (wolfmeshupload.h:100). That keeps the generated model on exactly the path
    // a hand-picked file takes, including the bounds-checked glTF reader.
    std::string ext = gDirUtilp->getExtension(filename);
    if (ext.empty()) ext = "glb";
    const std::string temp_path = gDirUtilp->getTempFilename() + "." + ext;
    {
        llofstream out(temp_path.c_str(), std::ios::out | std::ios::binary);
        if (!out.good())
        {
            finish(false, "Could not write the model to a temporary file.", LLUUID::null);
            return;
        }
        out.write((const char*)bytes.data(), bytes.size());
        out.close();
    }

    say(llformat("Reading the model (%d KB)…", (S32)(bytes.size() / 1024)), false);

    WolfMeshUpload::model_ptr_t model = std::make_shared<WolfMeshUpload::Model>();
    std::string err;
    if (!model->load(temp_path, err) || !model->finalise(err))
    {
        LLFile::remove(temp_path);
        finish(false, err.length() ? err : "The generated model could not be read.", LLUUID::null);
        return;
    }
    LLFile::remove(temp_path);

    WolfMeshUpload::Options opts;
    opts.mName = inv_name.length() ? inv_name : std::string("AI model");
    opts.mDescription = "Generated by AI: " + prompt.substr(0, 180);
    opts.mIncludeTextures = true;
    // Source: WolfMeshUpload::uploadCoro: guard every suspension-to-write transition.
    opts.mSessionBound = true;
    opts.mAgentId = auth.agent;
    opts.mSessionId = auth.session;

    say("Uploading to your inventory…", false);
    // upload() is itself a coroutine launcher and calls `done` exactly once on the main thread,
    // so its outcome IS this operation's outcome — no second completion path to keep in step.
    WolfMeshUpload::upload(model, opts,
        [say](const std::string& s, bool e) { say(s, e); },
        [finish](bool ok, const std::string& msg, const LLUUID& item)
        {
            finish(ok, msg, item);
        });
}

// Source: llagentdata.h; task state is independent of the panel's presentation lifetime.
WolfAI::Session WolfAI::Session::capture() { return {gAgentID, gAgentSessionID}; }
bool WolfAI::Session::current() const { return WolfGrid::isWolfTerritories() && agent.notNull() && session.notNull() && *this == capture(); }
void WolfAI::syncSession()
{
    const Session auth = Session::capture();
    if (mSession == auth) return;
    mSession = auth;
    mAvail = Avail(); mFetching = false; mGenerating = false; mNextRefresh = 0.;
    mJobs.clear(); mTasks.clear(); mErrors.clear();
    mDraft.clear(); mDraftName.clear(); mDraftMode = "structure";
    mSelected.setNull(); mLastStarted.setNull(); mMeshItem.setNull(); mMeshStatus.clear();
    ++mMutation; ++mRevision;
}
// Source: build_jobs.rs JobState and build_service.rs collect_inner/resume.
bool WolfAI::BuildJob::active() const
{
    return state == "researching" || state == "planning" || state == "generating" || state == "reviewing"
        || state == "charging" || state == "delivering";
}
bool WolfAI::BuildJob::collectable() const
{
    return state == "ready" || state == "charging" || state == "paid" || state == "delivery_pending";
}
bool WolfAI::BuildJob::resumable() const { return !paid && (state == "interrupted" || state == "failed"); }
const WolfAI::BuildJob* WolfAI::job(const LLUUID& id) const
{
    auto it = mJobs.find(id); return it == mJobs.end() ? nullptr : &it->second;
}
void WolfAI::error(const std::string& key, const std::string& message)
{
    if (message.empty()) mErrors.erase(key); else mErrors[key] = message;
    ++mRevision;
}
std::string WolfAI::errors() const
{
    std::string text;
    for (bool actions : {true, false}) for (const auto& entry : mErrors)
    {
        const bool action = entry.first == "action" || entry.first == "start" || entry.first == "mesh"
            || entry.first.find("collect:") == 0 || entry.first.find("resume:") == 0;
        if (action != actions) continue;
        if (!text.empty()) text += "\n\n";
        text += entry.second;
    }
    return text;
}
// Source: build_service.rs public_job; LLSD coercion must never manufacture actionable values.
bool WolfAI::parseBuildJob(const LLSD& data, BuildJob& out, std::string& error)
{
    BuildJob j;
    j.quote = parse_quote(data["quote"]);
    const std::set<std::string> states = {"researching", "planning", "generating", "reviewing", "ready", "charging", "paid", "delivering", "delivery_pending", "delivered", "interrupted", "failed"};
    bool valid = data.isMap() && data["id"].isString() && LLUUID::validate(data["id"].asString())
        && data["name"].isString() && data["prompt"].isString() && data["detail"].isString()
        && data["state"].isString() && states.count(data["state"].asString()) && data["paid"].isBoolean()
        && j.quote.valid && data["sources"].isArray();
    for (const char* field : {"completed_components", "total_components", "cost_credits", "preview_count"}) valid = valid && nonnegative(data[field]);
    valid = valid && (data["balance"].isUndefined() || nonnegative(data["balance"]));
    for (const LLSD& source : llsd::inArray(data["sources"]))
        valid = valid && source.isMap() && source["title"].isString() && source["url"].isString();
    for (const char* field : {"site", "result"}) valid = valid && (data[field].isUndefined() || data[field].isMap());
    if (data["site"].isMap())
        for (const char* field : {"description", "style"}) valid = valid && data["site"][field].isString();
    if (data["result"].isMap())
    {
        valid = valid && data["result"]["description"].isString();
        for (const char* field : {"parts", "visual_triangles", "physics_triangles"}) valid = valid && nonnegative(data["result"][field]);
    }
    if (valid)
    {
        j.id = LLUUID(data["id"].asString()); j.name = data["name"].asString(); j.state = data["state"].asString(); j.detail = data["detail"].asString();
        j.paid = data["paid"].asBoolean(); j.cost = data["cost_credits"].asInteger();
        j.completed = data["completed_components"].asInteger(); j.total = data["total_components"].asInteger(); j.previews = data["preview_count"].asInteger();
        valid = j.id.notNull() && j.cost <= j.quote.maxCredits && j.completed <= j.total;
        if (j.state == "delivered")
        {
            valid = valid && j.paid && data["item_id"].isString() && LLUUID::validate(data["item_id"].asString());
            if (valid) { j.item = LLUUID(data["item_id"].asString()); valid = j.item.notNull(); }
        }
        else valid = valid && data["item_id"].isUndefined();
    }
    if (!valid) { error = "The build service returned an invalid saved build. Reload to retry."; return false; }
    j.data = data; out = j; return true;
}
void WolfAI::mergeJob(const BuildJob& incoming)
{
    // Source: build_service.rs public_job reveals item only after delivery. Reads cannot undo it.
    const BuildJob* previous = job(incoming.id);
    if (previous && previous->state == "delivered") return;
    mJobs[incoming.id] = incoming;
    ++mRevision;
}
void WolfAI::listBuilds(build_fn done, std::function<bool()> valid)
{
    syncSession(); const Session auth = mSession; const U64 mutation = mMutation;
    LLCoros::instance().launch("WolfAI saved builds", [auth, mutation, done, valid]()
    {
        LLSD reply; std::string message;
        const S32 code = get_json(auth, ai_url("/ai_build/jobs"), reply, message);
        if (!auth.current()) { done(false, "Your session changed. Reopen the AI window."); return; }
        bool ok = code == 200 && reply["success"].isBoolean() && reply["success"].asBoolean() && reply["jobs"].isArray();
        std::vector<BuildJob> parsed;
        if (ok) for (const LLSD& data : llsd::inArray(reply["jobs"]))
        {
            BuildJob j; if (!parseBuildJob(data, j, message)) { ok = false; break; } parsed.push_back(j);
        }
        else message = reply_error(reply, message, code);
        WolfAI& ai = WolfAI::instance();
        if ((valid && !valid()) || mutation != ai.mMutation) { done(true, ""); return; }
        if (ok) for (const auto& j : parsed) ai.mergeJob(j);
        ai.error("list", ok ? "" : "Saved builds: " + message);
        done(ok, message);
    });
}
void WolfAI::getBuild(const LLUUID& id, build_fn done, std::function<bool()> valid)
{
    syncSession(); const Session auth = mSession; const U64 mutation = mMutation;
    LLCoros::instance().launch("WolfAI build status", [auth, mutation, id, done, valid]()
    {
        LLSD reply; std::string message; BuildJob j;
        const S32 code = get_json(auth, ai_url("/ai_build/status?id=" + LLURI::escape(id.asString())), reply, message);
        if (!auth.current()) { done(false, "Your session changed. Reopen the AI window."); return; }
        bool ok = code == 200 && reply["success"].isBoolean() && reply["success"].asBoolean();
        if (ok) ok = parseBuildJob(reply["job"], j, message) && j.id == id;
        else message = reply_error(reply, message, code);
        if (!ok && message.empty()) message = "The service returned a different saved build.";
        WolfAI& ai = WolfAI::instance();
        if ((valid && !valid()) || mutation != ai.mMutation) { done(true, ""); return; }
        if (ok) ai.mergeJob(j);
        ai.error("poll:" + id.asString(), ok ? "" : "Build " + id.asString() + ": " + message);
        done(ok, message);
    });
}
// Source: build_http.rs Start/Resume/Collect and public_job response envelope.
void WolfAI::buildRequest(const std::string& path, LLSD body, const std::string& task, build_fn done)
{
    syncSession();
    if (!WolfGrid::isWolfTerritories()) { done(false, "These tools are only available on Wolf Territories Grid."); return; }
    if (pending(task)) { done(false, "This action is already running."); return; }
    mTasks.insert(task); error(task, ""); ++mMutation;
    const Session auth = mSession;
    LLCoros::instance().launch("WolfAI build action", [auth, path, body, task, done]()
    {
        LLSD reply; std::string message; BuildJob j;
        const S32 code = post_json(auth, ai_url(path), boost::json::serialize(LlsdToJson(body)), reply, message);
        if (!auth.current()) { done(false, "Your session changed. Reopen the AI window."); return; }
        WolfAI& ai = WolfAI::instance();
        ai.mTasks.erase(task); ++ai.mMutation;
        bool ok = code == 200 && reply["success"].isBoolean() && reply["success"].asBoolean();
        if (ok) ok = parseBuildJob(reply["job"], j, message);
        else message = reply_error(reply, message, code);
        if (ok && body.has("job_id") && j.id.asString() != body["job_id"].asString()) { ok = false; message = "The service returned a different saved build."; }
        if (ok)
        {
            ai.mergeJob(j);
            if (task == "start") ai.mLastStarted = j.id;
            if (path == "/ai_build/collect" && j.state != "delivered")
            { ok = false; message = j.detail.empty() ? "Delivery is not yet confirmed. Reload or retry Collect." : j.detail; }
        }
        const std::string context = body.has("job_id") ? "Build " + body["job_id"].asString() + ": " : "Generate: ";
        ai.error(task, ok ? "" : context + message);
        // Publish server delivery to the panel BEFORE fallible inventory-cache work.
        done(ok, message);
        if (ok && j.state == "delivered")
        {
            ai.refreshNow();
            const LLUUID folder(body["folder_id"].asString());
            const std::string refreshKey = "inventory:" + j.id.asString();
            LLViewerInventoryCategory* cat = gInventory.getCategory(folder);
            if (cat && auth.current())
            {
                cat->setVersion(LLViewerInventoryCategory::VERSION_UNKNOWN);
                gInventory.fetchDescendentsOf(folder);
                gInventory.notifyObservers();
            }
            // Source: LLInventoryModel::fetchDescendentsOf only queues a fetch; verify the item
            // separately, so a lost inventory reply cannot erase delivery or start another upload.
            const F64 until = LLFrameTimer::getElapsedSeconds() + 30.;
            while (auth.current() && !gInventory.getItem(j.item) && LLFrameTimer::getElapsedSeconds() < until)
                llcoro::suspendUntilTimeout(1.f);
            if (!auth.current()) return;
            ai.error(refreshKey, gInventory.getItem(j.item) ? "" : "Build " + j.id.asString() + " is delivered (item " + j.item.asString() + "). Inventory has not refreshed. Open Inventory, or use Retry inventory.");
        }
    });
}
void WolfAI::startBuild(const std::string& prompt, const std::string& size, S32 maximum, build_fn done)
{
    if (prompt.empty() || prompt.size() > size_t(MAX_PROMPT_CHARS) || (size != "structure" && size != "settlement"))
    { error("start", "Enter a valid building description and mode."); done(false, "Enter a valid building description and mode."); return; }
    LLSD body; body["prompt"] = prompt; body["size"] = size; body["max_credits"] = maximum;
    buildRequest("/ai_build", body, "start", done);
}
void WolfAI::resumeBuild(const BuildJob& j, build_fn done)
{
    LLSD body; body["job_id"] = j.id.asString(); body["max_credits"] = j.quote.maxCredits;
    buildRequest("/ai_build/resume", body, "resume:" + j.id.asString(), done);
}
void WolfAI::collectBuild(const BuildJob& j, build_fn done)
{
    // Source: llinventorymodel.cpp findUserDefinedCategoryUUIDForType; build_service collect_inner.
    const LLUUID folder = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    if (folder.isNull() || !gInventory.getCategory(folder) || !gInventory.isObjectDescendentOf(folder, gInventory.getRootFolderID()))
    {
        const std::string message = "Open Inventory so your Objects folder can load, then retry Collect.";
        error("collect:" + j.id.asString(), "Build " + j.id.asString() + ": " + message); done(false, message); return;
    }
    LLSD body; body["job_id"] = j.id.asString(); body["folder_id"] = folder.asString();
    buildRequest("/ai_build/collect", body, "collect:" + j.id.asString(), done);
}
void WolfAI::privateBytes(const std::string& path, const std::string& mime, bytes_fn done)
{
    syncSession(); const Session auth = mSession;
    LLCoros::instance().launch("WolfAI private presentation", [auth, path, mime, done]()
    {
        LLSD::Binary bytes; LLSD headers; std::string filename, message;
        const S32 code = get_bytes(auth, ai_url(path), bytes, filename, message, 30, &headers);
        if (!auth.current()) { done(false, {}, "Your session changed. Reopen the AI window."); return; }
        std::string type = headers["content-type"].asString(); type = type.substr(0, type.find(';')); LLStringUtil::trim(type); LLStringUtil::toLower(type);
        std::string cache = headers["cache-control"].asString(); LLStringUtil::toLower(cache);
        const bool ok = code == 200 && type == mime && headers["x-content-type-options"].asString() == "nosniff"
            && cache.find("private") != std::string::npos && cache.find("no-store") != std::string::npos;
        done(ok, ok ? bytes : LLSD::Binary(), ok ? "" : "Private preview or research card could not be loaded. Select the build again to retry.");
    });
}
void WolfAI::getBuildPreview(const LLUUID& id, S32 view, bytes_fn done)
{
    privateBytes("/ai_build/preview?id=" + LLURI::escape(id.asString()) + "&view=" + std::to_string(view), "image/png", done);
}
void WolfAI::getBuildGrounding(const LLUUID& id, bytes_fn done)
{
    privateBytes("/ai_build/grounding?id=" + LLURI::escape(id.asString()), "text/html", done);
}
// Source: LLFloaterAuction::draw. A child draws inside its scroll parent's clipping scope.
// Source: LLImageFormatted::copyData is protected and copies without transferring ownership.
class WolfMemoryPNG : public LLImagePNG
{
public:
    bool copyBytes(const LLSD::Binary& bytes) { return copyData(const_cast<U8*>(bytes.data()), S32(bytes.size())); }
};
class WolfAIPreview : public LLPanel
{
public:
    explicit WolfAIPreview(const LLPanel::Params& p) : LLPanel(p) {}
    LLPointer<LLViewerTexture> texture;
    void draw() override
    {
        LLPanel::draw();
        if (texture.isNull()) return;
        const F32 scale = llmin(F32(getRect().getWidth()) / texture->getWidth(), F32(getRect().getHeight()) / texture->getHeight());
        const S32 width = ll_round(texture->getWidth() * scale), height = ll_round(texture->getHeight() * scale);
        LLGLSUIDefault ui;
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gl_draw_scaled_image((getRect().getWidth() - width) / 2, (getRect().getHeight() - height) / 2, width, height, texture);
    }
};
// Source: LLMediaCtrl::handleMediaEvent dispatches viewer schemes BEFORE LLWeb; intercept here.
// This child never forwards untrusted navigation/auth/file/geometry events to that dispatcher.
class WolfAIGrounding : public LLMediaCtrl
{
public:
    explicit WolfAIGrounding(const LLMediaCtrl::Params& p) : LLMediaCtrl(p) { setAllowFileDownload(false); }
    std::string failure;
    bool handleRightMouseDown(S32, S32, MASK) override { return true; }
    bool handleRightMouseUp(S32, S32, MASK) override { return true; }
    void handleMediaEvent(LLPluginClassMedia* plugin, EMediaEvent event) override
    {
        if (event == MEDIA_EVENT_CLICK_LINK_HREF || event == MEDIA_EVENT_CLICK_LINK_NOFOLLOW)
        {
            const std::string url = plugin->getClickURL();
            if (safe_link(url)) LLWeb::loadURLExternal(url);
            else failure = "This search suggestion link was blocked. Use the source links below.";
            return;
        }
        if (event == MEDIA_EVENT_PLUGIN_FAILED || event == MEDIA_EVENT_PLUGIN_FAILED_LAUNCH)
        { failure = "Search suggestion card is unavailable in this viewer build. Source links remain available."; return; }
        if (event == MEDIA_EVENT_CONTENT_UPDATED || event == MEDIA_EVENT_SIZE_CHANGED || event == MEDIA_EVENT_CURSOR_CHANGED || event == MEDIA_EVENT_NAVIGATE_COMPLETE)
            LLMediaCtrl::handleMediaEvent(plugin, event);
    }
};

static LLPanelInjector<WolfPanelAI> t_wolf_panel_ai("wolf_panel_ai");
WolfPanelAI::WolfPanelAI() : LLPanel() {}
// Source: original WolfPanelAI::postBuild and LLUICtrlFactory::create; reuse one copy of every control.
bool WolfPanelAI::postBuild()
{
    mPrompt = getChild<LLTextEditor>("ai_prompt"); mName = getChild<LLLineEditor>("ai_name");
    mBuild = getChild<LLButton>("ai_build"); mCollect = getChild<LLButton>("ai_collect"); mResume = getChild<LLButton>("ai_resume");
    mNote = getChild<LLTextBox>("ai_note"); mStatus = getChild<LLTextBox>("ai_status"); mCredits = getChild<LLTextBox>("ai_credits");
    mError = getChild<LLTextEditor>("ai_error"); mResult = getChild<LLTextEditor>("ai_job_result"); mSources = getChild<LLTextEditor>("ai_sources");
    mMode = getChild<LLComboBox>("ai_mode"); mJobs = getChild<LLScrollListCtrl>("ai_jobs");
    mStepChoose = getChild<LLPanel>("ai_step_choose"); mStepForm = getChild<LLPanel>("ai_step_form");
    mStepConfirm = getChild<LLPanel>("ai_step_confirm"); mStepWorking = getChild<LLPanel>("ai_step_working");
    mConfirmText = getChild<LLTextBox>("ai_confirm_text"); mWorkingTitle = getChild<LLTextBox>("ai_working_title");
    mWorkingNote = getChild<LLTextBox>("ai_working_note");
    getChild<LLButton>("ai_choose_small")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChoose("mesh"); });
    getChild<LLButton>("ai_choose_large")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChoose("structure"); });
    getChild<LLButton>("ai_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBack(); });
    getChild<LLButton>("ai_confirm_start")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onConfirmStart(); });
    getChild<LLButton>("ai_confirm_back")->setCommitCallback([this](LLUICtrl*, const LLSD&) { setStep(Step::Form); });
    getChild<LLButton>("ai_working_done")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onWorkingDone(); });
    LLPanel* preview = getChild<LLPanel>("ai_preview");
    LLPanel::Params pp; pp.name = "ai_preview_image"; pp.rect = preview->getLocalRect(); pp.follows.flags = FOLLOWS_ALL;
    mPreview = LLUICtrlFactory::create<WolfAIPreview>(pp); preview->addChild(mPreview);
    LLPanel* grounding = getChild<LLPanel>("ai_grounding");
    LLMediaCtrl::Params mp; mp.name = "ai_grounding_browser"; mp.rect = grounding->getLocalRect(); mp.follows.flags = FOLLOWS_ALL;
    mp.trusted_content = false; mp.hide_loading = false;
    mGrounding = LLUICtrlFactory::create<WolfAIGrounding>(mp); grounding->addChild(mGrounding);
    mBuild->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuild(); });
    getChild<LLButton>("ai_update")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpdateCredits(); });
    getChild<LLButton>("ai_buy")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBuyCredits(); });
    getChild<LLButton>("ai_jobs_reload")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReload(); });
    mMode->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMode(); });
    mJobs->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSelect(); });
    mCollect->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCollect(); });
    mResume->setCommitCallback([this](LLUICtrl*, const LLSD&) { onResume(); });
    getChild<LLButton>("ai_view_prev")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onView(-1); });
    getChild<LLButton>("ai_view_next")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onView(1); });
    mPrompt->setMaxTextLength(WolfAI::MAX_PROMPT_CHARS);
    mPrompt->setKeystrokeCallback([this](LLTextEditor*) { saveDraft(); });
    mBuilt = true;
    WolfAI& ai = WolfAI::instance(); ai.syncSession(); mSession = WolfAI::Session::capture();
    mPrompt->setText(ai.mDraft); mName->setText(ai.mDraftName); mMode->setValue(ai.mDraftMode);
    // A job already running (small model on WolfAI, large build on the grid) reopens on its status.
    const auto* selected = ai.job(ai.mSelected);
    setStep(ai.generating() || (selected && (selected->active() || selected->collectable())) ? Step::Working : Step::Choose);
    refresh();
    return true;
}
// [2026-09-12] One step visible at a time. Reopening the floater while something runs comes back
// to Working (postBuild / onVisibilityChange), so "you can close this window" is true.
void WolfPanelAI::setStep(Step step)
{
    mStep = step;
    mStepChoose->setVisible(step == Step::Choose); mStepForm->setVisible(step == Step::Form);
    mStepConfirm->setVisible(step == Step::Confirm); mStepWorking->setVisible(step == Step::Working);
    if (mBuilt) refresh();
}
void WolfPanelAI::onChoose(const std::string& mode)
{
    WolfAI& ai = WolfAI::instance();
    // A fresh choice starts clean: an old error from the other generator must not follow it.
    ai.error("action", ""); ai.error("mesh", "");
    ai.mDraftMode = mode; mMode->setValue(mode); saveDraft(); invalidateReads();
    setStep(Step::Form);
}
void WolfPanelAI::onBack() { saveDraft(); setStep(Step::Choose); }
void WolfPanelAI::onWorkingDone()
{
    // Only the screen changes. A running job stays where it is and refresh() brings this screen back.
    WolfAI::instance().error("action", "");
    setStep(Step::Choose);
}
void WolfPanelAI::saveDraft()
{
    if (!mBuilt || !mSession.current()) return;
    WolfAI& ai = WolfAI::instance(); ai.mDraft = mPrompt->getText(); ai.mDraftName = mName->getText(); ai.mDraftMode = mMode->getValue().asString();
}
void WolfPanelAI::invalidateReads()
{
    ++mReadGeneration; ++mPresentationGeneration;
    mListPending = false; mPollPending = false; mNextPoll = 0.;
    mPresentationJob.setNull(); mPresentationCount = -1;
    if (mPreview) mPreview->texture = nullptr;
    if (mGrounding) { mGrounding->unloadMediaSource(); mGrounding->failure.clear(); }
}
bool WolfPanelAI::accepts(const WolfAI::Session& auth, U64 generation) const
{
    return auth.current() && mSession == auth && generation == mReadGeneration && isInVisibleChain();
}
// Source: LLView::onVisibilityChange recursively notifies visible children when the floater closes.
void WolfPanelAI::onVisibilityChange(bool visible)
{
    saveDraft(); invalidateReads(); mNeedList = true;
    if (visible && mBuilt)
    {
        WolfAI& ai = WolfAI::instance(); const auto* selected = ai.job(ai.mSelected);
        if (ai.generating() || (selected && (selected->active() || selected->collectable()))) setStep(Step::Working);
    }
    LLPanel::onVisibilityChange(visible);
}
void WolfPanelAI::draw()
{
    if (mRefreshTimer.getElapsedTimeF32() >= 1.f)
    { mRefreshTimer.reset(); refresh(); }
    LLPanel::draw();
}
void WolfPanelAI::refresh()
{
    if (!mBuilt) return;
    WolfAI& ai = WolfAI::instance(); ai.syncSession();
    const auto auth = WolfAI::Session::capture();
    if (!(mSession == auth))
    {
        invalidateReads(); mSession = auth; mNeedList = true;
        mPrompt->setText(ai.mDraft); mName->setText(ai.mDraftName); mMode->setValue(ai.mDraftMode);
        mShownRevision = ~U64(0); mView = 0;
    }
    const bool wolf = WolfGrid::isWolfTerritories();
    if (!wolf)
    {
        ai.error("grid", "These tools are only available on Wolf Territories Grid.");
        mBuild->setEnabled(false); mCollect->setEnabled(false); mResume->setEnabled(false);
        mError->setText(ai.errors()); return;
    }
    ai.refresh();
    const auto& a = ai.avail();
    const bool mesh = mMode->getValue().asString() == "mesh";
    const auto& quote = mMode->getValue().asString() == "settlement" ? a.mSettlement : a.mStructure;
    mCredits->setText(a.mBalanceKnown ? llformat("Wolf Credits: %d", a.mBalance) : "Wolf Credits: unavailable");
    getChild<LLButton>("ai_buy")->setEnabled(safe_link(a.mBuyUrl));
    getChild<LLButton>("ai_jobs_reload")->setEnabled(!mListPending);
    std::string note;
    if (mesh)
    {
        note = a.mMesh ? llformat("Describe a small object. About %d credits; %d required to start. The generated model is uploaded to Objects. You can close this window while it runs.", a.mMeshTypical, a.mMeshMin)
            : "Small-object generation is unavailable. Your saved buildings remain accessible.";
        mBuild->setLabel("Generate small object"); mStatus->setText(ai.mMeshStatus);
    }
    else
    {
        note = quote.valid ? llformat("Up to %d credits · %d components · %d m maximum extent.\n", quote.maxCredits, quote.maxComponents, quote.maxExtent) : "Price unavailable. Use Update to retry.\n";
        note += "Finished, unfurnished architecture. Actual cost, up to this maximum, is charged after the completed build passes review. Failed generation is not charged. Saved results can be collected later.";
        mBuild->setLabel(quote.valid ? llformat("Build · up to %d credits", quote.maxCredits) : "Build · price unavailable");
        mStatus->setText(std::string(ai.pending("start") ? "Starting your saved build…" : ""));
    }
    mNote->setText(note); mName->setVisible(mesh); getChildView("ai_name_lbl")->setVisible(mesh);
    // Small model: the generator is fixed, so the size choice is hidden; large: Building/Settlement.
    mMode->setVisible(!mesh); getChildView("ai_mode_lbl")->setVisible(!mesh);
    for (const char* name : {"ai_saved_title", "ai_jobs_reload", "ai_jobs"}) getChildView(name)->setVisible(!mesh);
    if (mesh) mBuild->setLabel("Build small model"); else mBuild->setLabel(quote.valid ? llformat("Continue · up to %d credits", quote.maxCredits) : "Continue · price unavailable");
    // The confirm screen states the charge in the resident's numbers.
    {
        const std::string kind = mMode->getValue().asString() == "settlement" ? "settlement" : "building";
        const std::string balance = a.mBalanceKnown ? llformat("%d", a.mBalance) : "an unknown balance";
        getChild<LLTextBox>("ai_confirm_title")->setText("Before your " + kind + " is started");
        mConfirmText->setText(llformat(
            "You will be charged up to %d Wolf Credits for this %s — the actual AI cost, taken when it is ready. You have %s.\n\n"
            "It is designed in stages and then reviewed. Revisions use that allowance. When the allowance is used up, the %s is finished with what has been designed and the reviewer's remaining notes are kept with it — it is never abandoned part way.\n\n"
            "When it is ready it is delivered to your Objects folder and charged automatically. You can close this window while it builds.",
            quote.maxCredits, kind.c_str(), balance.c_str(), kind.c_str()));
        getChild<LLButton>("ai_confirm_start")->setLabel(quote.valid ? llformat("Start the %s · up to %d credits", kind.c_str(), quote.maxCredits) : "Price unavailable");
        getChild<LLButton>("ai_confirm_start")->setEnabled(auth.current() && a.mBuild && quote.valid && !ai.pending("start"));
    }
    // The working screen: title, note and the "another" button follow the generator.
    {
        const auto* sel = ai.job(ai.mSelected);
        const bool small = mesh || (ai.generating() && !sel);
        std::string who = small ? (ai.mDraftName.empty() ? std::string("Your model") : ai.mDraftName) : (sel ? sel->name : std::string("Your building"));
        mWorkingTitle->setText((small ? "Building “" : "Designing “") + who + "”");
        mWorkingNote->setText(std::string(small
            ? "You can close this window. The model keeps building and is delivered to your Objects folder shortly."
            : "You can close this window. The building keeps being designed on the grid. When it is ready it is put into your Objects folder and charged automatically the next time this viewer sees it."));
        getChild<LLButton>("ai_working_done")->setLabel(small ? "Make another" : "Start another");
        getChildView("ai_scroll")->setVisible(!small);
        if (mStep != Step::Working && ai.generating()) setStep(Step::Working);
    }
    mBuild->setEnabled(auth.current() && (mesh ? a.mMesh && !ai.generating() : a.mBuild && quote.valid && !ai.pending("start")));
    mPrompt->setEnabled(!(mesh ? ai.generating() : ai.pending("start"))); mName->setEnabled(!ai.generating());
    if (mShownRevision != ai.revision()) { renderJobs(); mShownRevision = ai.revision(); }
    if (mGrounding && !mGrounding->failure.empty())
    {
        ai.error("grounding:" + ai.mSelected.asString(), mGrounding->failure); mGrounding->failure.clear();
    }
    const std::string errors = ai.errors();
    if (mError->getText() != errors) mError->setText(errors);
    if (!isInVisibleChain() || !auth.current()) return;
    if (mNeedList && !mListPending) { onReload(); return; }
    const auto* selected = ai.job(ai.mSelected);
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (selected && selected->active() && !mPollPending && now >= mNextPoll)
    {
        mPollPending = true; mNextPoll = now + POLL_SECS;
        const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
        auto valid = [handle, auth, generation]() { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); return p && p->accepts(auth, generation); };
        ai.getBuild(selected->id, [handle, valid](bool, const std::string&)
        { if (valid()) { auto* p = static_cast<WolfPanelAI*>(handle.get()); p->mPollPending = false; p->refresh(); } }, valid);
    }
    if (selected) autoCollect(*selected);
    loadPresentation();
}
// [2026-09-12] Paul: "when the large model is completed it should be automatically put into the
// users inventory". Collection is what charges, at the maximum accepted on the confirm screen.
// Once per job per panel; the Collect button stays as the fallback. Waits silently while the
// Objects folder is not loaded rather than raising the same error every poll.
void WolfPanelAI::autoCollect(const WolfAI::BuildJob& job)
{
    WolfAI& ai = WolfAI::instance();
    if (!job.collectable() || mAutoCollected.count(job.id) || ai.pending("collect:" + job.id.asString())) return;
    const LLUUID folder = gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
    if (folder.isNull() || !gInventory.getCategory(folder)) return;
    mAutoCollected.insert(job.id);
    onCollect();
}
void WolfPanelAI::renderJobs()
{
    WolfAI& ai = WolfAI::instance();
    mJobs->deleteAllItems();
    for (const auto& entry : ai.jobs())
    {
        const auto& j = entry.second;
        LLSD row; row["id"] = j.id;
        row["columns"][0]["column"] = "name"; row["columns"][0]["value"] = j.name;
        row["columns"][1]["column"] = "state"; row["columns"][1]["value"] = j.state;
        mJobs->addElement(row);
    }
    if (ai.mSelected.isNull() && !ai.jobs().empty()) ai.mSelected = ai.jobs().begin()->first;
    mJobs->selectByID(ai.mSelected);
    const auto* j = ai.job(ai.mSelected);
    const bool busy = j && (ai.pending("collect:" + j->id.asString()) || ai.pending("resume:" + j->id.asString()));
    mCollect->setVisible(j && (j->collectable() || j->state == "delivered"));
    mCollect->setLabel(j && j->state == "delivered" ? "Retry inventory" : "Collect");
    mCollect->setEnabled(j && !busy); mResume->setVisible(j && j->resumable()); mResume->setEnabled(j && !busy);
    for (const char* name : {"ai_job_name","ai_job_status","ai_job_cost","ai_job_result","ai_sources"}) getChildView(name)->setVisible(j != nullptr);
    getChildView("ai_preview")->setVisible(j && j->previews > 0);
    getChildView("ai_view_controls")->setVisible(j && j->previews > 0);
    getChildView("ai_grounding_note")->setVisible(j != nullptr);
    if (!j)
    {
        getChild<LLTextBox>("ai_saved_title")->setText(std::string("Saved buildings — none loaded"));
        getChildView("ai_grounding")->setVisible(false); return;
    }
    getChild<LLTextBox>("ai_saved_title")->setText(std::string("Saved buildings"));
    getChild<LLTextBox>("ai_job_name")->setText(j->name);
    getChild<LLTextBox>("ai_job_status")->setText(j->detail + llformat("\n%s · %d / %d components", j->state.c_str(), j->completed, j->total));
    getChild<LLTextBox>("ai_job_cost")->setText(llformat("%s: %d credits · saved maximum %d", j->paid ? "Charged" : "Cost so far", j->cost, j->quote.maxCredits));
    std::string result = j->detail + "\n\n" + j->data["prompt"].asString();
    if (j->item.notNull()) result = "Delivered to inventory. Item: " + j->item.asString() + "\n\n" + result;
    const LLSD& site = j->data["site"];
    if (site.isMap()) result += "\n\n" + site["description"].asString() + "\nStyle: " + site["style"].asString();
    const LLSD& model = j->data["result"];
    if (model.isMap()) result += "\n\n" + model["description"].asString() + llformat("\n%d parts · %d visual triangles · %d physics triangles", model["parts"].asInteger(), model["visual_triangles"].asInteger(), model["physics_triangles"].asInteger());
    mResult->setText(result); mSources->setText(std::string(""));
    for (const LLSD& source : llsd::inArray(j->data["sources"]))
    {
        const std::string url = source["url"].asString(); if (!safe_link(url)) continue;
        LLStyle::Params style; style.is_link = true; style.link_href = url;
        style.color = LLUIColorTable::instance().getColor("HTMLLinkColor"); style.readonly_color = style.color;
        mSources->appendText(source["title"].asString().empty() ? url : source["title"].asString(), !mSources->getText().empty(), style);
    }
}
void WolfPanelAI::onReload()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    if (mListPending) return;
    mNeedList = false; mListPending = true;
    const auto auth = mSession; const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
    auto valid = [handle, auth, generation]() { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); return p && p->accepts(auth, generation); };
    WolfAI::instance().listBuilds([handle, valid](bool, const std::string&)
    { if (valid()) { auto* p = static_cast<WolfPanelAI*>(handle.get()); p->mListPending = false; p->mNextPoll = 0.; p->refresh(); } }, valid);
}
void WolfPanelAI::onMode() { saveDraft(); invalidateReads(); refresh(); }
void WolfPanelAI::onSelect()
{
    WolfAI& ai = WolfAI::instance();
    if (auto* row = mJobs->getFirstSelected()) ai.mSelected = row->getUUID();
    invalidateReads(); mView = 0; mShownRevision = ~U64(0);
    if (ai.job(ai.mSelected)) setStep(Step::Working); else refresh();
}
void WolfPanelAI::onView(S32 direction)
{
    const auto* j = WolfAI::instance().job(WolfAI::instance().mSelected);
    if (!j || !j->previews) return;
    mView = llclamp(mView + direction, 0, j->previews - 1);
    ++mPresentationGeneration; mPresentationCount = -1; mPreview->texture = nullptr; loadPresentation();
}
void WolfPanelAI::onUpdateCredits() { WolfAI::instance().refreshNow(); refresh(); }
void WolfPanelAI::onBuyCredits()
{
    const std::string url = WolfAI::instance().avail().mBuyUrl;
    if (safe_link(url)) LLWeb::loadURLExternal(url); else setStatus("The credit store link is unavailable. Use Update to retry.", true);
}
void WolfPanelAI::setStatus(const std::string& message, bool failure)
{
    if (failure) WolfAI::instance().error("action", message); else mStatus->setText(message);
    mError->setText(WolfAI::instance().errors());
}
void WolfPanelAI::onBuild()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    saveDraft();
    std::string prompt = mPrompt->getText(); LLStringUtil::trim(prompt);
    if (prompt.empty()) { setStatus("Describe what you want to build.", true); mPrompt->setFocus(true); return; }
    invalidateReads();
    WolfAI& ai = WolfAI::instance(); ai.error("action", "");
    const auto auth = mSession; const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
    auto completed = [handle, auth, generation](bool, const std::string&)
    { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); if (p && p->accepts(auth, generation)) { p->mShownRevision = ~U64(0); p->refresh(); } };
    if (mMode->getValue().asString() == "mesh")
    {
        ai.error("mesh", "");
        ai.generateModel(prompt, mName->getText(),
            [handle, auth, generation](const std::string&, bool)
            { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); if (p && p->accepts(auth, generation)) p->refresh(); },
            [completed](bool ok, const std::string& message, const LLUUID&) { completed(ok, message); });
        setStep(Step::Working);
        return;
    }
    // Large project: nothing is started from the form. The confirm screen states the charge and
    // asks; only its Start button spends anything (onConfirmStart).
    const auto& q = mMode->getValue().asString() == "settlement" ? ai.avail().mSettlement : ai.avail().mStructure;
    if (!q.valid) { setStatus("Price unavailable. Use Update before building.", true); return; }
    setStep(Step::Confirm);
}
void WolfPanelAI::onConfirmStart()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    saveDraft();
    std::string prompt = mPrompt->getText(); LLStringUtil::trim(prompt);
    if (prompt.empty()) { setStatus("Describe what you want to build.", true); setStep(Step::Form); return; }
    invalidateReads();
    WolfAI& ai = WolfAI::instance(); ai.error("action", "");
    const auto auth = mSession; const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
    const std::string mode = mMode->getValue().asString() == "settlement" ? "settlement" : "structure";
    const auto& q = mode == "settlement" ? ai.avail().mSettlement : ai.avail().mStructure;
    if (!q.valid) { setStatus("Price unavailable. Use Update before building.", true); return; }
    ai.startBuild(prompt, mode, q.maxCredits, [handle, auth, generation](bool ok, const std::string& message)
    {
        auto* p = dynamic_cast<WolfPanelAI*>(handle.get());
        if (!p || !p->accepts(auth, generation)) return;
        if (ok) { WolfAI::instance().mSelected = WolfAI::instance().mLastStarted; p->setStep(Step::Working); }
        else { p->setStep(Step::Form); }
        p->mShownRevision = ~U64(0); p->refresh();
    });
    setStep(Step::Working);
}
void WolfPanelAI::onResume()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    const auto* j = WolfAI::instance().job(WolfAI::instance().mSelected); if (!j || !j->resumable()) return;
    invalidateReads(); const auto auth = mSession; const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
    WolfAI::instance().resumeBuild(*j, [handle, auth, generation](bool, const std::string&)
    { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); if (p && p->accepts(auth, generation)) p->refresh(); }); refresh();
}
void WolfPanelAI::onCollect()
{
    if (!WolfGrid::isWolfTerritories()) { setStatus("These tools are only available on Wolf Territories Grid.", true); return; }
    const auto* j = WolfAI::instance().job(WolfAI::instance().mSelected); if (!j || (!j->collectable() && j->state != "delivered")) return;
    invalidateReads(); const auto auth = mSession; const U64 generation = mReadGeneration; LLHandle<LLPanel> handle = getHandle();
    WolfAI::instance().collectBuild(*j, [handle, auth, generation](bool, const std::string&)
    { auto* p = dynamic_cast<WolfPanelAI*>(handle.get()); if (p && p->accepts(auth, generation)) p->refresh(); }); refresh();
}
// Source: LLImagePNG updateData/decode, LLFloaterImagePreview image-area policy, and build_http
// private PNG/grounding routes. Browser documents contain no authentication material.
void WolfPanelAI::loadPresentation()
{
    WolfAI& ai = WolfAI::instance(); const auto* j = ai.job(ai.mSelected);
    if (!j || (mPresentationJob == j->id && mPresentationCount == j->previews)) return;
    mPresentationJob = j->id; mPresentationCount = j->previews;
    mView = j->previews ? llclamp(mView, 0, j->previews - 1) : 0;
    mPreview->texture = nullptr; mGrounding->unloadMediaSource(); mGrounding->failure.clear();
    getChildView("ai_grounding")->setVisible(false);
    getChild<LLTextBox>("ai_grounding_note")->setText(std::string("Loading search suggestions…"));
    getChild<LLTextBox>("ai_view_label")->setText(llformat("View %d / %d", mView + 1, j->previews));
    getChild<LLButton>("ai_view_prev")->setEnabled(mView > 0);
    getChild<LLButton>("ai_view_next")->setEnabled(mView + 1 < j->previews);
    const auto auth = mSession; const U64 generation = ++mPresentationGeneration;
    const LLUUID id = j->id; const S32 view = mView; LLHandle<LLPanel> handle = getHandle();
    auto panel = [handle, auth, generation, id, view]() -> WolfPanelAI*
    {
        auto* p = dynamic_cast<WolfPanelAI*>(handle.get());
        return p && auth.current() && p->mSession == auth && p->isInVisibleChain() && generation == p->mPresentationGeneration
            && id == WolfAI::instance().mSelected && view == p->mView ? p : nullptr;
    };
    if (j->previews > 0) ai.getBuildPreview(id, view, [panel, id](bool ok, const LLSD::Binary& bytes, const std::string& message)
    {
        auto* p = panel(); if (!p) return;
        std::string failure = message;
        LLPointer<WolfMemoryPNG> png = new WolfMemoryPNG;
        LLPointer<LLImageRaw> raw = new LLImageRaw;
        // Source: LLPngWrapper truncates dimensions to U16 before native decode. libpng's
        // full-width header check prevents oversized dimensions wrapping into a small allocation.
        png_image header{}; header.version = PNG_IMAGE_VERSION;
        ok = ok && bytes.size() >= 8 && bytes.size() <= size_t(std::numeric_limits<S32>::max())
            && png_image_begin_read_from_memory(&header, bytes.data(), bytes.size());
        if (ok) ok = header.width > 0 && header.height > 0
            && header.width <= std::numeric_limits<U16>::max() && header.height <= std::numeric_limits<U16>::max()
            && U64(header.width) * header.height <= U64(8096) * 8096;
        png_image_free(&header);
        if (ok) ok = png->copyBytes(bytes) && png->updateData()
            && (png->getComponents() == 3 || png->getComponents() == 4) && png->decode(raw, 0.f);
        if (ok) p->mPreview->texture = LLViewerTextureManager::getLocalTexture(raw.get(), false);
        else { p->mPreview->texture = nullptr; if (failure.empty()) failure = "The private preview is not a supported PNG. Select the build again to retry."; }
        WolfAI::instance().error("preview:" + id.asString(), ok ? "" : "Build " + id.asString() + ": " + failure);
        p->refresh();
    });
    ai.getBuildGrounding(id, [panel, id](bool ok, const LLSD::Binary& bytes, const std::string& message)
    {
        auto* p = panel(); if (!p) return;
        const std::string key = "grounding:" + id.asString();
        if (!ok) { WolfAI::instance().error(key, "Build " + id.asString() + ": " + message); p->getChild<LLTextBox>("ai_grounding_note")->setText(std::string("Search suggestions unavailable. Source links remain available.")); p->refresh(); return; }
        if (bytes.empty())
        {
            p->getChild<LLTextBox>("ai_grounding_note")->setText(std::string("No search suggestion card was returned. Research sources are listed below."));
            WolfAI::instance().error(key, ""); p->refresh(); return;
        }
        // Source: build_http.rs grounding CSP and browser contract. Base64 prevents markup
        // escaping the frame attribute; the real iframe sandbox prohibits scripts and same-origin.
        const std::string child = "<!doctype html><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src https: data:; base-uri 'none'; form-action 'none'\">" + std::string(bytes.begin(), bytes.end());
        const std::string encoded = LLBase64::encode(reinterpret_cast<const U8*>(child.data()), child.size());
        const std::string outer = "<!doctype html><meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; frame-src data:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'\"><style>html,body,iframe{margin:0;border:0;width:100%;height:100%}</style><iframe sandbox=\"allow-popups\" referrerpolicy=\"no-referrer\" src=\"data:text/html;charset=utf-8;base64," + encoded + "\"></iframe>";
        if (!p->mGrounding->ensureMediaSourceExists())
        {
            const std::string failure = "Search suggestion card is unavailable in this viewer build. Source links remain available.";
            WolfAI::instance().error(key, failure); p->getChild<LLTextBox>("ai_grounding_note")->setText(failure); p->refresh(); return;
        }
        p->mGrounding->navigateTo("data:text/html;charset=utf-8;base64," + LLBase64::encode(reinterpret_cast<const U8*>(outer.data()), outer.size()), "text/html", false);
        p->getChildView("ai_grounding")->setVisible(true);
        p->getChild<LLTextBox>("ai_grounding_note")->setText(std::string("Search suggestions · source links below"));
        WolfAI::instance().error(key, ""); p->refresh();
    });
}
