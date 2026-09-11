/**
 * @file wolfai.h
 * @brief AI assist — write/edit an LSL script, and generate a textured model into inventory.
 *
 * [AI 2026-09-11] Paul: "we're going to AI enable the viewer and browser … in the script editor
 * it will have an AI button … in the build menu there will be an AI option … it will generate
 * 3d models with textures … only admin users with a userlevel of above 230 will have access."
 *
 * WHERE THE WORK HAPPENS. Nothing here talks to an AI provider. The proxy holds the keys and
 * does the calling (rust_proxy/src/main.rs, the AI assist section); this is a thin client over
 * five routes on WolfGrid::PROXY_API_BASE:
 *   GET  /ai_available        may this account use the tools, and which are configured
 *   POST /ai_script           write or edit one LSL script (answers in seconds)
 *   POST /ai_mesh             start a model generation, answers a job id
 *   GET  /ai_mesh_status?job= progress
 *   GET  /ai_mesh_result?job= the model file's bytes
 * A generation runs for MINUTES, which is why it is a job plus a poll rather than one request.
 *
 * NOTHING HERE IS A SECURITY BOUNDARY. scriptReady()/meshReady() decide only whether to draw a
 * button. The proxy re-verifies the session and reads the account level from the grid on every
 * route, so a forged "yes" here buys a button that answers 403.
 *
 * THE MODEL REACHES INVENTORY THROUGH THE EXISTING UPLOADER. The bytes are written to a temp
 * file and handed to WolfMeshUpload, which already does Y-up to SL Z-up, the glTF V flip,
 * per-prim normalisation, >8 materials into a linkset, >65535 verts split, the texture uploads
 * and the TextureEntry. A second copy of that here would drift from the tested one.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid. Modified by IntelligentWolf Ltd, 2026.
 * $/LicenseInfo$
 */

#ifndef WOLF_AI_H
#define WOLF_AI_H

#include "llpanel.h"      // WolfPanelAI (the Build > AI tab) derives from LLPanel
#include "llsingleton.h"
#include "lluuid.h"
#include <functional>
#include <string>

class WolfAI : public LLSingleton<WolfAI>
{
    LLSINGLETON(WolfAI);

public:
    /** What /ai_available said. `mScript` and `mMesh` are separate because the two tools run on
     *  different providers and a server can hold one key and not the other. */
    struct Avail
    {
        bool        mAsked      = false;   // have we ever had an answer
        bool        mEnabled    = false;   // any provider configured
        bool        mAllowed    = false;   // this account's level is high enough
        bool        mScript     = false;   // allowed AND the script provider is configured
        bool        mMesh       = false;   // allowed AND the mesh provider is configured
        std::string mScriptModel;
        std::string mMeshModel;
        // [AI CREDITS 2026-09-11] What the resident has and what it costs. The balance is the
        // control now — the account-level gate is gone, so any verified grid resident may use
        // the tools and their credits are what limit them.
        S32         mBalance      = 0;
        S32         mScriptCost   = 1;
        S32         mMeshMin      = 40;   // headroom needed to START a model
        S32         mMeshTypical  = 30;   // what the user is TOLD it costs
        F64         mPencePerCredit = 0.0;
        std::string mBuyUrl;
    };

    const Avail& avail() const { return mAvail; }
    bool scriptReady() const { return mAvail.mScript; }
    bool meshReady() const { return mAvail.mMesh; }

    /** Ask the proxy, unless a recent answer is already held. Safe to call on every UI refresh. */
    void refresh(bool force = false);
    /** The panel's Update button: forget the cached answer and ask again right now. */
    void refreshNow() { refresh(true); }

    /** ok = true with the script source, or false with a message written to be shown. */
    typedef std::function<void(bool ok, const std::string& script_or_error)> script_fn;
    /** Write (`existing` empty) or edit one LSL script. Returns at once; the callback runs on
     *  the main thread exactly once. */
    void requestScript(const std::string& prompt, const std::string& existing, script_fn done);

    /** Progress line while a model is generated and uploaded. */
    typedef std::function<void(const std::string& status, bool is_error)> progress_fn;
    /** Final outcome of a generation, exactly once, on the main thread. */
    typedef std::function<void(bool ok, const std::string& message, const LLUUID& item_id)> done_fn;
    /** Generate a model and put it in inventory under `inv_name`. */
    void generateModel(const std::string& prompt, const std::string& inv_name,
                       progress_fn progress, done_fn done);

    /** True while a generation started by this viewer is still running. */
    bool generating() const { return mGenerating; }

    /** Caps the service also enforces; mirrored so the UI can stop a hopeless request early. */
    static const S32 MAX_PROMPT_CHARS = 4000;
    static const S32 MAX_SCRIPT_CHARS = 64000;

private:
    static void availCoro();
    static void scriptCoro(std::string prompt, std::string existing, script_fn done);
    static void meshCoro(std::string prompt, std::string inv_name, progress_fn progress, done_fn done);

    Avail mAvail;
    bool  mFetching   = false;
    bool  mGenerating = false;
    F64   mNextRefresh = 0.0;
};

/**
 * Build floater > AI (floater_tools.xml wolf_ai_panel).
 *
 * Paul: "in the build menu there will be an AI option that you click, it will generate 3d models
 * with textures … then it will upload it to the persons inventory." The panel is a description
 * box, a name, a button and a status line; the generation and the upload are WolfAI's.
 *
 * Modelled on WolfPanelTerrainPaint (wolfterrainpaint.h:332) and registered the same way, with
 * an LLPanelInjector, so the tab is XUI like every other tool tab.
 */
class WolfPanelAI : public LLPanel
{
public:
    WolfPanelAI();
    bool postBuild() override;
    void refresh() override;

private:
    void onBuild();
    void onUpdateCredits();
    void onBuyCredits();
    void setStatus(const std::string& msg, bool error);

    class LLTextEditor* mPrompt  = nullptr;
    class LLLineEditor* mName    = nullptr;
    class LLButton*     mBuild   = nullptr;
    class LLTextBox*    mNote    = nullptr;
    class LLTextBox*    mStatus  = nullptr;
    class LLTextBox*    mCredits = nullptr;
    class LLButton*     mUpdate  = nullptr;
    class LLButton*     mBuy     = nullptr;
};

#endif // WOLF_AI_H
