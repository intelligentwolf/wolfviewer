/**
 * @file wolfai.h
 * @brief Authenticated AI scripts, small models, and durable architectural builds.
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid. Modified by IntelligentWolf Ltd, 2026.
 * $/LicenseInfo$
 */
#ifndef WOLF_AI_H
#define WOLF_AI_H
#include "llpanel.h"
#include "llframetimer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class WolfAI : public LLSingleton<WolfAI>
{
    LLSINGLETON(WolfAI);
public:
    // Source: main.rs ai_identify; never replace credentials after suspension.
    struct Session
    {
        LLUUID agent, session;
        static Session capture();
        bool current() const;
        bool operator==(const Session& rhs) const { return agent == rhs.agent && session == rhs.session; }
    };
    // Source: build_jobs.rs Quote and build_service.rs public_job.
    struct BuildQuote
    {
        S32 maxCredits = 0, maxComponents = 0, maxExtent = 0;
        bool valid = false;
    };
    struct BuildJob
    {
        LLUUID id, item;
        std::string name, state, detail;
        S32 completed = 0, total = 0, cost = 0, previews = 0;
        bool paid = false;
        BuildQuote quote;
        LLSD data;
        bool active() const;
        bool collectable() const;
        bool resumable() const;
    };
    struct Avail
    {
        bool mAsked = false, mEnabled = false, mAllowed = false;
        bool mScript = false, mMesh = false, mBuild = false, mBalanceKnown = false;
        std::string mScriptModel, mMeshModel, mBuyUrl, mError;
        S32 mBalance = 0, mScriptCost = 0, mMeshMin = 0, mMeshTypical = 0;
        F64 mPencePerCredit = 0.;
        BuildQuote mStructure, mSettlement;
    };
    const Avail& avail() const { return mAvail; }
    bool scriptReady() const { return mAvail.mScript; }
    bool meshReady() const { return mAvail.mMesh; }
    bool buildReady() const { return mAvail.mBuild; }
    void refresh(bool force = false);
    void refreshNow() { refresh(true); }
    // [WOLF AI CREDITS 2026-10-24] ask again on the next idle unless the last ask is under min_age seconds old
    // (LLViewerWindow::handleFocus: back from buying credits in the browser).
    void refreshSoon(F64 min_age);
    // [WOLF AI CREDITS 2026-10-24] a Wolf service reply showed credits being spent (WolfAdventure::api): ask now, and once
    // more after the brain's billing run has taken them (wolfadventure_worker.php WAW_BILLING_SEC = 60).
    void creditsUsed();
    // [WOLF AI CREDITS 2026-10-24] the Wolf Credits store in the browser (status bar "AI n" click); the balance is read
    // again when the viewer gets focus back (refreshSoon).
    void openBuyPage();
    void syncSession();
    typedef std::function<void(bool, const std::string&)> script_fn;
    typedef std::function<void(const std::string&, bool)> progress_fn;
    typedef std::function<void(bool, const std::string&, const LLUUID&)> done_fn;
    void requestScript(const std::string& prompt, const std::string& existing, script_fn done);
    void generateModel(const std::string& prompt, const std::string& inv_name, progress_fn progress, done_fn done);
    bool generating() const { return mGenerating; }
    static const S32 MAX_PROMPT_CHARS = 4000;
    static const S32 MAX_SCRIPT_CHARS = 64000;

    typedef std::function<void(bool, const std::string&)> build_fn;
    typedef std::function<void(bool, const LLSD::Binary&, const std::string&)> bytes_fn;
    void listBuilds(build_fn done, std::function<bool()> valid = {});
    void getBuild(const LLUUID& id, build_fn done, std::function<bool()> valid = {});
    void startBuild(const std::string& prompt, const std::string& size, S32 maximum, build_fn done);
    void resumeBuild(const BuildJob& job, build_fn done);
    void collectBuild(const BuildJob& job, build_fn done);
    void getBuildPreview(const LLUUID& id, S32 view, bytes_fn done);
    void getBuildGrounding(const LLUUID& id, bytes_fn done);
    const std::map<LLUUID, BuildJob>& jobs() const { return mJobs; }
    const BuildJob* job(const LLUUID& id) const;
    bool pending(const std::string& key) const { return mTasks.count(key) != 0; }
    U64 revision() const { return mRevision; }
    void error(const std::string& key, const std::string& message);
    std::string errors() const;
    // Task/result state survives panel close and is reset only by a different login session.
    std::string mDraft, mDraftName, mDraftMode = "structure", mMeshStatus;
    LLUUID mSelected, mMeshItem, mLastStarted;
private:
    static void availCoro(Session auth);
    static void scriptCoro(Session auth, std::string prompt, std::string existing, script_fn done);
    static void meshCoro(Session auth, std::string prompt, std::string inv_name, progress_fn progress, done_fn done);
    static bool parseBuildJob(const LLSD& data, BuildJob& job, std::string& error);
    void mergeJob(const BuildJob& job);
    void buildRequest(const std::string& path, LLSD body, const std::string& task, build_fn done);
    void privateBytes(const std::string& path, const std::string& mime, bytes_fn done);
    Session mSession;
    Avail mAvail;
    bool mFetching = false, mGenerating = false;
    F64 mNextRefresh = 0., mLastAsk = -1.e9, mBillingCheck = 0.;   // mBillingCheck: creditsUsed()'s second ask (0 = none)
    U64 mMutation = 0, mRevision = 0;
    std::map<LLUUID, BuildJob> mJobs;
    std::set<std::string> mTasks;
    std::map<std::string, std::string> mErrors;
};

class WolfAIForge;
/**
 * [FORGE LOOK 2026-10-20] Paul: "our standard object ai generator just make them look as nice as the new avatar and
 * clothing generator but not the same". "The Forge": a smithy at night painted behind the controls by WolfAIForge
 * (wolfai.cpp; the technique of the radio and WolfAvatars, the primitives in wolfpaint.h) - see floater_wolf_ai.xml for
 * what is drawn where. Look only: the steps, calls, prices and words are as they were.
 * Who and why: a resident with no 3D skills wants to describe an object in words and find it in their Objects folder a
 * few minutes later, knowing the cost before anything is charged.
 */
class WolfPanelAI : public LLPanel
{
    friend class WolfAIForge;   // the view behind the controls that paints the forge
public:
    WolfPanelAI();
    bool postBuild() override;
    void refresh() override;
    void draw() override;
    void onVisibilityChange(bool visible) override;
private:
    // [2026-09-12] The wizard, matching WolfStorm: choose -> form -> (large) confirm -> working.
    // [WOLF 3D TEXT 2026-10-26] and the 3D Text tool: choose -> words (with the live preview beside them) -> delivered
    // (TextForm, TextDone; region-terrain-work/wolf-text3d/PROTOCOL.md "UI shape" and "Changes 2026-10-26 (round 2)": the
    // preview is on the form, so the separate preview step is gone).
    enum class Step { Choose, Form, Confirm, Working, TextForm, TextDone };
    bool textStep() const { return mStep == Step::TextForm || mStep == Step::TextDone; }
    /** The resident has gone on to the small model / large build steps: a late 3D Text reply fills its screens without
     *  taking them away from what they are doing (the sign is in their inventory either way). */
    bool textElsewhere() const { return mStep == Step::Form || mStep == Step::Confirm || mStep == Step::Working; }
    void setStep(Step step);
    void onChoose(const std::string& mode);
    void onBack();
    void onConfirmStart();
    void onWorkingDone();
    void autoCollect(const WolfAI::BuildJob& job);
    void onBuild();
    void onUpdateCredits();
    void onBuyCredits();
    void onMode();
    void onSelect();
    void onReload();
    void onResume();
    void onCollect();
    void onView(S32 direction);
    void renderJobs();
    void loadPresentation();
    void invalidateReads();
    void saveDraft();
    void setStatus(const std::string& message, bool error);
    bool accepts(const WolfAI::Session& auth, U64 generation) const;
    /** [FORGE LOOK] the error box shows only while there is an error (its red plate would otherwise sit there empty). */
    void showErrors(const std::string& errors);
    // [FORGE LOOK] painted by WolfAIForge::draw() under the widgets (panel-local coordinates)
    void drawForge();
    void drawForgeHero(F32 t) const;
    void drawForgeEmbers() const;
    void drawForgeStep(F32 t);
    void drawCrucible(const LLRect& area, F32 t) const;
    void drawForgeText(F32 t);

    // ---- [WOLF 3D TEXT 2026-10-26] the 3D Text tool (PROTOCOL.md: text_options, text_preview, text_make; free, no AI) ----
    // Who and why: a resident wants their words standing in-world as a solid sign - plain, bevelled, neon or a hillside
    // sign - and to find it in their inventory a minute later, without paying or learning to build.
    void textChoose();
    void textAskOptions();
    void textApplyOptions(const LLSD& options);
    void textLoadDefaults(const std::string& style);
    void textSyncControls();
    /** The SPEC from the form, checked as PROTOCOL.md says the server checks it; false when it is not, with `first` the
     *  first field to put right and `why` its message. `show`: write each message by its field (a submit, or messages
     *  already showing); otherwise the live preview checks quietly (GOV.UK: validate on submit, not while typing). */
    bool textSpec(LLSD& spec, class LLUICtrl*& first, std::string& why, bool show);
    bool textFieldErrorsShown() const;
    /** The form, after a press of Make or Update preview, failed its checks: the summary in the error box, the page that
     *  holds the first bad field shown, and that field focused. */
    void textSubmitFailed(class LLUICtrl* first);
    void textMake();
    /** Ask text_preview for the form as it is now (loud: the Update preview button, which shows field messages). */
    void textPreview(bool loud);
    /** The form changed: a new preview TEXT_PREVIEW_DELAY after the last change (textTick, from draw). */
    void textQueuePreview();
    void textTick();
    /** The quiet line under the picture and the picture's own state (dimmed, "Updating..."), from the flags below. */
    void textSyncStatus();
    void textShowPage(bool lights);
    void textShowNight(bool night);
    void textShowPreview(const LLSD& reply);
    void textShowMade(const LLSD& reply);
    void textFieldError(const char* field, const std::string& message);
    std::string textNumbers(const LLSD& stats, const std::string& size_separator) const;
    LLSD mTextOptions;                  // text_options' reply, or the protocol's fallback until it comes
    std::string mTextStyle;             // the style whose defaults the form last took
    bool mTextOptionsLive = false, mTextOptionsAsking = false, mTextEdited = false;
    bool mTextMaking = false;           // a text_make is on its way (Make is disabled until it answers)
    bool mTextPreviewing = false;       // a text_preview is on its way (one at a time; a change meanwhile waits for it)
    bool mTextStale = false;            // the picture no longer shows the form as it is
    bool mTextSummary = false;          // the error box holds the "settings need changing" summary of a failed submit
    F64 mTextDirtyAt = 0.;              // when the form last changed with a preview still to ask for (0: none waiting)
    F64 mTextPausedUntil = 0.;          // the server's "too many previews - wait N s": no preview asked before this
    std::string mTextHint;              // the quiet line: why the preview waits, or why it could not be drawn
    // a reply is used only when it answers the latest call of its kind in this session (a new session bumps both)
    U64 mTextMakeSerial = 0, mTextPreviewSerial = 0;
    LLPanel *mStepText = nullptr, *mStepTextDone = nullptr, *mTextPageLetters = nullptr, *mTextPageLights = nullptr;
    class LLTextEditor *mTextWords = nullptr, *mTextReport = nullptr;
    class LLComboBox *mTextStyleBox = nullptr, *mTextFont = nullptr, *mTextLayout = nullptr, *mTextLight = nullptr;
    class LLSpinCtrl *mTextHeight = nullptr, *mTextDepth = nullptr, *mTextRadius = nullptr;
    class LLSliderCtrl *mTextGlow = nullptr, *mTextIntensity = nullptr;
    class LLCheckBoxCtrl *mTextBevel = nullptr, *mTextFullbright = nullptr;
    class LLColorSwatchCtrl *mTextColour = nullptr, *mTextLightColour = nullptr;
    class LLTextureCtrl* mTextTexture = nullptr;
    class LLLineEditor* mTextName = nullptr;
    class LLButton *mTextPreviewBtn = nullptr, *mTextMakeBtn = nullptr, *mTextDay = nullptr, *mTextNight = nullptr;
    class LLButton *mTextTabLetters = nullptr, *mTextTabLights = nullptr;
    class LLTextBox *mTextStatus = nullptr, *mTextNumbers = nullptr;
    class WolfAIPreview* mTextPicture = nullptr;

    WolfAIForge* mForge = nullptr;
    class LLTextEditor *mPrompt = nullptr, *mError = nullptr, *mResult = nullptr, *mSources = nullptr;
    class LLLineEditor* mName = nullptr;
    class LLButton *mBuild = nullptr, *mCollect = nullptr, *mResume = nullptr;
    class LLTextBox *mNote = nullptr, *mStatus = nullptr, *mCredits = nullptr;
    class LLComboBox* mMode = nullptr;
    class LLScrollListCtrl* mJobs = nullptr;
    LLPanel *mStepChoose = nullptr, *mStepForm = nullptr, *mStepConfirm = nullptr, *mStepWorking = nullptr;
    class LLTextBox *mConfirmText = nullptr, *mWorkingTitle = nullptr, *mWorkingNote = nullptr;
    Step mStep = Step::Choose;
    std::set<LLUUID> mAutoCollected;   // jobs this panel has already tried to collect on its own
    class WolfAIPreview* mPreview = nullptr;
    class WolfAIGrounding* mGrounding = nullptr;
    WolfAI::Session mSession;
    LLFrameTimer mRefreshTimer;
    U64 mReadGeneration = 0, mPresentationGeneration = 0, mShownRevision = ~U64(0);
    bool mListPending = false, mPollPending = false, mNeedList = true, mBuilt = false;
    F64 mNextPoll = 0.;
    LLUUID mPresentationJob;
    S32 mView = 0, mPresentationCount = -1;
};
#endif
