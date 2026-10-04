/**
 * @file wolffloaterdj.h
 * @brief WolfViewer: Wolf DJ / Talk Show - the mini mixer floater (wolfdjaudio.cpp is the engine).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATERDJ_H
#define WOLF_FLOATERDJ_H

#include "llfloater.h"
#include "llframetimer.h"
#include "wolfdjaudio.h"

#include <array>
#include <vector>

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollContainer;
class LLSliderCtrl;
class LLTextBox;
struct WolfDJApp;

// [WOLF DJ 2026-10-04] Paul's rules: a Wolf user who OWNS A REGION can DJ (the GridManager rule
// for /radio/ streams - streamauth.php refuses anyone else); they broadcast to their own stream
// from any region; on their OWN region the mixer may set the parcel's music to it; anywhere else
// the region owner sets it (the floater gives them the address to send).
class WolfFloaterDJ : public LLFloater
{
public:
    WolfFloaterDJ(const LLSD& key);
    ~WolfFloaterDJ() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    struct Strip
    {
        LLSliderCtrl* mFader = nullptr;
        LLSliderCtrl* mEqHigh = nullptr;
        LLSliderCtrl* mEqMid = nullptr;
        LLSliderCtrl* mEqLow = nullptr;
        LLButton* mMute = nullptr;
        LLButton* mCue = nullptr;
        LLView* mMeter = nullptr;
        LLTextBox* mDb = nullptr;
        float mShownDb = -60.f;
        float mClipUntil = 0.f;
    };

    void refreshApps();
    void fillSources(LLComboBox* combo, int ch, const std::vector<WolfDJApp>& apps);
    void onSource(int ch);
    void onGoLive();
    void onSendTitle();
    void onCopyUrl();
    void onCue(int ch);
    void saveLevels();
    void loadLevels();
    void updateStatus();
    void requestListeners();
    void drawMeter(LLView* meter, float peak, float& shown_db, float& clip_until);
    bool parseStreamUrl(const std::string& url, WolfDJMixer::StreamConfig& cfg, std::string& err) const;
    bool canSetLand(std::string& why) const;

    std::array<Strip, WolfDJ::CH_COUNT> mStrips;
    Strip mMaster;
    // Music 1..4 source pickers (channels CH_MUSIC_A..CH_MUSIC_D).
    std::array<LLComboBox*, WolfDJ::CH_COUNT - WolfDJ::CH_MUSIC_A> mSources{};
    LLComboBox* sourceCombo(int ch) const { return mSources[ch - WolfDJ::CH_MUSIC_A]; }
    LLLineEditor* mUrl = nullptr;
    LLLineEditor* mPassword = nullptr;
    LLLineEditor* mStation = nullptr;
    LLLineEditor* mArtist = nullptr;
    LLLineEditor* mTitle = nullptr;
    LLButton* mLiveBtn = nullptr;
    LLTextBox* mStatus = nullptr;
    LLTextBox* mVoiceNote = nullptr;
    LLScrollContainer* mStripsScroll = nullptr;    // the channel strips scroll when the window is small
    LLFrameTimer mClock;
    F32 mNextListeners = 0.f;
    S32 mListeners = -1;
    bool mListenersBusy = false;
};

// LLAppViewer::requestQuit: go off air and put the land's music back while still connected.
void wolfdj_on_app_quit();

#endif // WOLF_FLOATERDJ_H
