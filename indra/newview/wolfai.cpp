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

    /// Source: wolfterrainpaint.cpp:74 json_to_llsd — the same decode every Wolf route uses.
    LLSD json_to_llsd(const LLSD::Binary& bytes)
    {
        std::string text(bytes.begin(), bytes.end());
        boost::system::error_code ec;
        boost::json::value v = boost::json::parse(text, ec);
        if (ec) return LLSD();
        return LlsdFromJson(v);
    }

    /// The pair every Wolf proxy route authenticates with (main.rs ai_identify: agent + session,
    /// checked against the grid's presence service).
    void add_wolf_headers(LLCore::HttpHeaders::ptr_t& headers)
    {
        headers->append("X-Wolf-Agent", gAgentID.asString());
        headers->append("X-Wolf-Session", gAgentSessionID.asString());
    }

    /// Inside a coroutine: GET a Wolf route and decode its JSON reply.
    /// Source: wolfterrainpaint.cpp fetchCoro — same adapter, same result unpacking.
    S32 get_json(const std::string& url, LLSD& out, std::string& error, S32 timeout = 30)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_ACCEPT, "application/json");
        add_wolf_headers(headers);

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
    S32 get_bytes(const std::string& url, LLSD::Binary& out, std::string& filename,
                  std::string& error, S32 timeout = 300)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        add_wolf_headers(headers);

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
        filename.clear();
        if (http_results.has("headers") && http_results["headers"].has("x-wolf-filename"))
        {
            filename = http_results["headers"]["x-wolf-filename"].asString();
        }
        error = status ? std::string() : status.toString();
        return status.getType();
    }

    /// Inside a coroutine: POST JSON to a Wolf route. Source: wolfmeshupload.cpp:1796 postJson.
    S32 post_json(const std::string& url, const std::string& body, LLSD& out,
                  std::string& error, S32 timeout = 240)
    {
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
            std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>("WolfAI", LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
        LLCore::HttpOptions::ptr_t opts = std::make_shared<LLCore::HttpOptions>();
        opts->setTimeout(timeout);
        LLCore::HttpHeaders::ptr_t headers = std::make_shared<LLCore::HttpHeaders>();
        headers->append(HTTP_OUT_HEADER_CONTENT_TYPE, "application/json");
        add_wolf_headers(headers);

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
}

WolfAI::WolfAI() {}

void WolfAI::refresh(bool force)
{
    if (mFetching) return;
    if (!force && mAvail.mAsked && LLFrameTimer::getElapsedSeconds() < mNextRefresh) return;
    if (!WolfGrid::isWolfTerritories()) { mAvail = Avail(); mAvail.mAsked = true; return; }
    if (gAgentID.isNull() || gAgentSessionID.isNull()) return;   // not logged in yet
    mFetching = true;
    mNextRefresh = LLFrameTimer::getElapsedSeconds() + AVAIL_CACHE_SECS;
    LLCoros::instance().launch("WolfAI avail", []() { WolfAI::availCoro(); });
}

void WolfAI::availCoro()
{
    LLSD reply;
    std::string transport;
    const S32 code = get_json(ai_url("/ai_available"), reply, transport, 20);

    WolfAI& self = WolfAI::instance();
    self.mFetching = false;
    Avail a;
    a.mAsked = true;
    if (code == 200 && reply.isMap() && reply["success"].asBoolean())
    {
        a.mEnabled   = reply["enabled"].asBoolean();
        a.mAllowed   = reply["allowed"].asBoolean();
        a.mScript    = reply["script"].asBoolean();
        a.mMesh      = reply["mesh"].asBoolean();
        a.mScriptModel = reply["script_model"].asString();
        a.mMeshModel   = reply["mesh_model"].asString();
        // [AI CREDITS 2026-09-11] Balance and prices, so the panel can say what a model costs
        // and what the resident holds without a second round trip.
        a.mBalance       = reply["balance"].asInteger();
        a.mScriptCost    = reply.has("script_cost") ? reply["script_cost"].asInteger() : 1;
        a.mMeshMin       = reply.has("mesh_min_balance") ? reply["mesh_min_balance"].asInteger() : 40;
        a.mPencePerCredit = reply["pence_per_credit"].asReal();
        a.mBuyUrl        = reply["buy_url"].asString();
    }
    else
    {
        // A 401/403, a proxy that predates the feature, or no network. All mean "no AI here";
        // none is worth a dialog, because most accounts are legitimately not administrators.
        LL_DEBUGS("WolfAI") << "availability: HTTP " << code << " " << transport << LL_ENDL;
    }
    self.mAvail = a;
    LL_INFOS("WolfAI") << "AI assist: enabled=" << a.mEnabled << " allowed=" << a.mAllowed
                       << " script=" << a.mScript << " mesh=" << a.mMesh
                       << " balance=" << a.mBalance << LL_ENDL;
}

void WolfAI::requestScript(const std::string& prompt, const std::string& existing, script_fn done)
{
    if (!done) return;
    if (!WolfGrid::isWolfTerritories())
    {
        done(false, "The AI tools are a Wolf Territories feature.");
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
        [prompt, existing, done]() { WolfAI::scriptCoro(prompt, existing, done); });
}

void WolfAI::scriptCoro(std::string prompt, std::string existing, script_fn done)
{
    LLSD body;
    body["prompt"] = prompt;
    body["script"] = existing;
    const std::string text = boost::json::serialize(LlsdToJson(body));

    LLSD reply;
    std::string transport;
    const S32 code = post_json(ai_url("/ai_script"), text, reply, transport, 240);

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
    if (!done) return;
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
        [prompt, inv_name, progress, done]() { WolfAI::meshCoro(prompt, inv_name, progress, done); });
}

void WolfAI::meshCoro(std::string prompt, std::string inv_name, progress_fn progress, done_fn done)
{
    // Every exit from here must clear mGenerating and call `done` exactly once, or the button
    // stays disabled for the rest of the session.
    auto finish = [done](bool ok, const std::string& msg, const LLUUID& id)
    {
        WolfAI::instance().mGenerating = false;
        done(ok, msg, id);
    };
    auto say = [progress](const std::string& s, bool err)
    {
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
        const S32 code = post_json(ai_url("/ai_mesh"), text, reply, transport, 60);
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
        const S32 code = get_json(ai_url("/ai_mesh_status?job=" + LLURI::escape(job)), reply, transport, 30);
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
        const S32 code = get_bytes(ai_url("/ai_mesh_result?job=" + LLURI::escape(job)),
                                   bytes, filename, transport, 300);
        if (code != 200 || bytes.empty())
        {
            LLSD nothing;
            finish(false, reply_error(nothing, transport, code), LLUUID::null);
            return;
        }
    }
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

    say("Uploading to your inventory…", false);
    // upload() is itself a coroutine launcher and calls `done` exactly once on the main thread,
    // so its outcome IS this operation's outcome — no second completion path to keep in step.
    WolfMeshUpload::upload(model, opts,
        [progress](const std::string& s, bool e) { if (progress) progress(s, e); },
        [finish](bool ok, const std::string& msg, const LLUUID& item)
        {
            finish(ok, msg, item);
        });
}

// ═══════════════════════ WolfPanelAI — Build floater > AI ═══════════════════════════════════
// Registered as XUI class "wolf_panel_ai", the same route WolfPanelTerrainPaint takes
// (wolfterrainpaint.cpp:65). The tab is removed off-grid in LLFloaterTools::postBuild.

static LLPanelInjector<WolfPanelAI> t_wolf_panel_ai("wolf_panel_ai");

WolfPanelAI::WolfPanelAI() : LLPanel() {}

bool WolfPanelAI::postBuild()
{
    mPrompt = getChild<LLTextEditor>("ai_prompt");
    mName   = getChild<LLLineEditor>("ai_name");
    mBuild  = getChild<LLButton>("ai_build");
    mNote   = getChild<LLTextBox>("ai_note");
    mStatus = getChild<LLTextBox>("ai_status");
    mCredits = getChild<LLTextBox>("ai_credits");
    mUpdate  = getChild<LLButton>("ai_update");
    mBuy     = getChild<LLButton>("ai_buy");
    mBuild->setCommitCallback(boost::bind(&WolfPanelAI::onBuild, this));
    // [AI CREDITS 2026-09-11] Paul's "update button": ask the service again right now.
    mUpdate->setCommitCallback(boost::bind(&WolfPanelAI::onUpdateCredits, this));
    mBuy->setCommitCallback(boost::bind(&WolfPanelAI::onBuyCredits, this));
    if (mPrompt) mPrompt->setMaxTextLength(WolfAI::MAX_PROMPT_CHARS);
    refresh();
    return true;
}

void WolfPanelAI::refresh()
{
    WolfAI& ai = WolfAI::instance();
    ai.refresh();
    const WolfAI::Avail& a = ai.avail();

    // Three distinct states, each said plainly rather than a dead button with no explanation.
    std::string note;
    bool usable = false;
    if (!a.mEnabled)
    {
        note = getString("str_not_configured");
    }
    else if (!a.mAllowed)
    {
        // [AI CREDITS 2026-09-11] The account-level gate is gone; this is only reached on a
        // grid the service does not serve.
        note = getString("str_not_wolf");
    }
    else if (!a.mMesh)
    {
        note = getString("str_no_mesh_provider");
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[MODEL]"] = a.mMeshModel.length() ? a.mMeshModel : std::string("AI");
        args["[NEED]"]  = llformat("%d", a.mMeshMin);
        // Paul: "nice to put the price on the form." The rate comes from the store's own price
        // list, so the figure shown is one the store would honour.
        args["[APPROX]"] = a.mPencePerCredit > 0.0
            ? llformat(" (about £%.2f)", (a.mMeshMin * a.mPencePerCredit) / 100.0)
            : std::string();
        note = getString("str_ready", args);
        usable = true;
    }
    if (mNote) mNote->setText(note);

    // The credit line, and the warning when there is not enough for a model.
    if (mCredits)
    {
        LLStringUtil::format_map_t cargs;
        cargs["[BALANCE]"] = llformat("%d", a.mBalance);
        mCredits->setText(getString("str_credits", cargs));
    }
    const bool short_of_credits = a.mAllowed && a.mMesh && a.mBalance < a.mMeshMin;
    if (short_of_credits)
    {
        LLStringUtil::format_map_t sargs;
        sargs["[NEED]"]    = llformat("%d", a.mMeshMin);
        sargs["[BALANCE]"] = llformat("%d", a.mBalance);
        setStatus(getString("str_low_credits", sargs), true);
    }
    if (mBuy) mBuy->setVisible(a.mAllowed && !a.mBuyUrl.empty());

    const bool busy = ai.generating();
    // Deliberately still ENABLED when short of credits: the service refuses and says so with a
    // link, which is more useful than a dead button with no explanation.
    if (mBuild)  mBuild->setEnabled(usable && !busy);
    if (mPrompt) mPrompt->setEnabled(usable && !busy);
    if (mName)   mName->setEnabled(usable && !busy);
    if (mUpdate) mUpdate->setEnabled(!busy);
}

void WolfPanelAI::onUpdateCredits()
{
    // Force, because the whole point of the button is to see a purchase that has happened since
    // the cached answer.
    WolfAI::instance().refreshNow();
    setStatus(getString("str_updating"), false);
}

void WolfPanelAI::onBuyCredits()
{
    const std::string url = WolfAI::instance().avail().mBuyUrl;
    if (url.empty()) return;
    // The store is a web page; the viewer's own browser handling decides internal or external.
    LLWeb::loadURL(url);
}

void WolfPanelAI::setStatus(const std::string& msg, bool error)
{
    if (!mStatus) return;
    mStatus->setText(msg);
    // Red for a failure, the same signal the paint panel uses.
    mStatus->setColor(error ? LLColor4(0.9f, 0.45f, 0.45f, 1.f)
                            : LLUIColorTable::instance().getColor("LabelTextColor").get());
}

void WolfPanelAI::onBuild()
{
    if (!mPrompt) return;
    std::string prompt = mPrompt->getText();
    LLStringUtil::trim(prompt);
    if (prompt.empty())
    {
        setStatus(getString("str_need_prompt"), true);
        mPrompt->setFocus(true);
        return;
    }
    std::string name = mName ? mName->getText() : std::string();
    LLStringUtil::trim(name);
    if (name.empty()) name = "AI model";

    setStatus(getString("str_starting"), false);
    if (mBuild) mBuild->setEnabled(false);

    // The callbacks capture a handle, not `this`: a generation runs for minutes and the build
    // floater can be closed in that time. WolfPanelTerrainPaint has the same hazard and the
    // same answer.
    LLHandle<LLPanel> handle = getHandle();
    WolfAI::instance().generateModel(prompt, name,
        [handle](const std::string& status, bool is_error)
        {
            if (WolfPanelAI* self = dynamic_cast<WolfPanelAI*>(handle.get()))
            {
                self->setStatus(status, is_error);
            }
        },
        [handle, name](bool ok, const std::string& message, const LLUUID& item)
        {
            WolfPanelAI* self = dynamic_cast<WolfPanelAI*>(handle.get());
            if (self)
            {
                self->setStatus(ok ? llformat("\"%s\" is in your Objects folder.", name.c_str())
                                   : message,
                                !ok);
                // The charge has landed by now, so re-read rather than show a stale balance.
                WolfAI::instance().refreshNow();
                self->refresh();
            }
            // Tell the user even if they closed the floater — they asked for this minutes ago
            // and a silent finish would look like a failure.
            if (!self || ok)
            {
                LLNotificationsUtil::add("GenericAlertOK", LLSD().with("MESSAGE",
                    ok ? llformat("The AI model \"%s\" is now in your Objects folder.", name.c_str())
                       : "The AI model could not be built: " + message));
            }
            if (item.notNull())
            {
                LL_INFOS("WolfAI") << "AI model uploaded as item " << item << LL_ENDL;
            }
        });
}
