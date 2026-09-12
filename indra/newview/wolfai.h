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
    F64 mNextRefresh = 0.;
    U64 mMutation = 0, mRevision = 0;
    std::map<LLUUID, BuildJob> mJobs;
    std::set<std::string> mTasks;
    std::map<std::string, std::string> mErrors;
};

class WolfPanelAI : public LLPanel
{
public:
    WolfPanelAI();
    bool postBuild() override;
    void refresh() override;
    void draw() override;
    void onVisibilityChange(bool visible) override;
private:
    // [2026-09-12] The wizard, matching WolfStorm: choose -> form -> (large) confirm -> working.
    enum class Step { Choose, Form, Confirm, Working };
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
