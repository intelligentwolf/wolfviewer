/**
 * @file llfloaterwolfavatars.h
 * @brief WolfViewer: Wolf World > WolfAvatars... - describe a character, pick one of four pictures, and a wearable rigged
 *        avatar arrives in your inventory.
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

#ifndef LL_FLOATER_WOLF_AVATARS_H
#define LL_FLOATER_WOLF_AVATARS_H

#include <functional>
#include <string>
#include <vector>

#include "llfloater.h"
#include "llpointer.h"
#include "llrect.h"
#include "llsd.h"

class LLButton;
class LLComboBox;
class LLFontGL;
class LLLineEditor;
class LLPanel;
class LLTextBox;
class LLTextEditor;
class LLViewerTexture;
class WolfAvatarPicture;
class WolfAvatarsBackdrop;

/**
 * [WOLFAVATARS 2026-10-20] Paul: the feature is WolfAvatars and it ships in the release "Character Colin".
 * Source for every call, field and word: firestorm/region-terrain-work/character-builder/WOLFAVATARS_API.md (Flow 1-6 and
 * "Screen (both viewers)") and the brain wolfstorm/php/wolfavatars_lib.php. The calls go through the Adventure Studio's own
 * helper, WolfAdventure::api (wolfadventure.cpp: wolfadventure_api.php with X-Wolf-Agent + X-Wolf-Session).
 *
 * One window, one step shown at a time:
 *   describe  -> char_quote                    (person / animal / clothing, name, words, height; people: dressed / base
 *                                               body; clothing: the garment; a base body or clothing: woman / man)
 *   price     -> char_start {confirm}          (Back drops the quoted job: job_cancel)
 *   working   -> job {job} every 3 s           (queued / running: the progress words and bar)
 *   pictures  -> char_picture n = 0..3, char_pick {n}, char_more   (asking)
 *   done      -> the report lines, Open my inventory
 * failed / cancelled go back to describe with the words kept and the reason in red. On open, jobs {kind:"character"} and the
 * newest unfinished job (queued, running or asking) is taken up again. Wolf Territories only.
 *
 * [2026-10-20] Paul: "the window is a bit basic! make it much more exciting like our radio, mixer etc ... lets make a fuss
 * about this unique feature". "The Summoning Circle": a moonlit wolf den drawn in code behind the controls, the way the radio
 * is (wolffloaterradio.cpp WolfRadioBackdrop: an LLView behind every widget that paints with gGL triangles and scales with the
 * window). A night sky with drifting stars, a big moon, mountains and pines, a wolf howling on a ridge, the engraved
 * "WolfAvatars" title plate, five step stones (Describe - Pictures - Pick - Build - Done), Person / Animal as two big drawn
 * tiles, a rune ring whose glowing arc is the progress bar, the four pictures in gilt frames beside the circle with a beam to
 * the chosen one, and a light burst behind the finished avatar's picture. Every control is still an XUI widget laid over the
 * drawing (layoutWidgets(), on every resize), so the keyboard, focus and tooltips work as before. No image files.
 */
class LLFloaterWolfAvatars : public LLFloater
{
    friend class WolfAvatarsBackdrop;   // the view behind the controls that paints the den
public:
    LLFloaterWolfAvatars(const LLSD& key);

    /** [WOLFNPC 2026-10-24] Told when a person or animal (not clothing) is finished here (job, characterKind, name): the
     *  WolfNPC window's "Create a new one" waits for it, to rez it as an NPC. One listener; nullptr removes it. */
    typedef std::function<void(const std::string& job, const std::string& kind, const std::string& name)> character_done_fn;
    static void setCharacterDoneListener(character_done_fn fn);
    ~LLFloaterWolfAvatars() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    enum EStep { STEP_LOADING = 0, STEP_DESCRIBE, STEP_PRICE, STEP_WORKING, STEP_PICTURES, STEP_DONE };

    // ---- the drawn den ----
    // Where everything sits, in floater-local pixels (the step panels are laid over the whole floater, so their own
    // coordinates are the floater's); worked out by layoutWidgets() on every resize and step change.
    struct Layout
    {
        LLRect client;          // below the title bar
        LLRect banner;          // the sky: title plate, moon, ridge with the wolf, mountains
        LLRect title_plate;     // "WolfAvatars" / "wolf-grid.com"
        F32 moon_x = 0.f, moon_y = 0.f, moon_r = 0.f;
        F32 ridge_x = 0.f;      // where the howling wolf sits on its ridge
        LLRect stones;          // the step stones band, on the ground under the mountains
        LLRect content;         // the step's own area
        LLRect footer;          // the button row
        LLRect error_plate;     // the plate the error line sits on (drawn while there is an error)
        LLRect stop_plate;      // "Stop making this character?" (drawn while it is asked)
        // describe
        LLRect tile[4];         // Person, Furry - standing, Furry - four legs, Clothing (EKind order; KINDS below)
        LLRect clothes[2];      // Dressed as described, Base body (people only; empty otherwise)
        const LLFontGL* clothes_font = nullptr;   // the font both clothes labels fit in
        // [CLOTHING 2026-10-20] the garment chips, two rows of four (Clothing only; empty otherwise)
        LLRect garment[8];
        const LLFontGL* garment_font = nullptr;
        // [FIGURE 2026-10-20] Woman / Man (a base body, or clothing; empty otherwise)
        LLRect figure[2];
        const LLFontGL* figure_font = nullptr;
        LLRect slate;           // the panel the fields sit on
        LLRect form_error_plate;
        // circle (price, working, loading, pictures)
        F32 ring_x = 0.f, ring_y = 0.f, ring_r = 0.f;
        LLRect words_plate;     // the stage words under the ring
        LLRect plaque;          // the price plaque
        // pictures (floater-local, the frames' outer edges)
        LLRect pic[4];
        LLRect pic_error_plate;
        // done
        LLRect done_picture;
        LLRect done_slate;
    };
    void layoutWidgets();
    void fitButton(LLButton* b, const std::vector<std::string>& samples, bool display) const;

    // Painted by WolfAvatarsBackdrop::draw() under the widgets (floater-local coordinates).
    void drawDen();
    void drawSky(F32 t) const;
    void drawTitle() const;
    void drawStones() const;
    void drawDescribe(F32 t) const;
    /** The choice chips on the slate: Dressed / Base, Woman / Man, the garments (drawChip for each). */
    void drawChips() const;
    /** The rune ring: pct >= 0 lights that much of it (the progress bar) and shows the number; spinning turns an arc while
     *  there is no number; strength 0..1 is how brightly it glows; figure puts the chosen kind's silhouette in the middle. */
    void drawCircle(F32 cx, F32 cy, F32 r, F32 t, S32 pct, bool spinning, F32 strength, bool figure) const;
    void drawFigure(F32 cx, F32 cy, F32 size, const LLColor4& colour) const;
    void drawPictures(F32 t) const;
    void drawDone(F32 t) const;
    void drawButtons(F32 t) const;
    void drawPlates() const;
    S32 currentStone() const;   // 0..4 (Describe .. Done), -1 while it is not known yet

    // [CLOTHING 2026-10-20] what is made: char_quote kind "person" | "animal" | "clothing" (wolfavatars_lib.php WCH_KINDS)
    // [FURRY 2026-10-21] Paul: "i want to be able to specify FURRY standing or FURRY quad": Furry - standing is a person
    // (kind "person" + furry: true); Furry - four legs is the animal. The value is the tile's place, top to bottom.
    enum EKind { KIND_PERSON = 0, KIND_FURRY, KIND_ANIMAL, KIND_CLOTHING };
    static const S32 KINDS = 4;
    static_assert(sizeof(Layout::tile) / sizeof(LLRect) == 4, "one tile per kind");
    static const S32 GARMENTS = 8;      // WOLFAVATARS_API.md "Garment types": top, dress, trousers, skirt, shoes, hat, cape, gown
    bool isPerson() const { return mKind == KIND_PERSON || mKind == KIND_FURRY; }   // made as a person (two legs)
    bool isFurry() const { return mKind == KIND_FURRY; }
    /** Dressed / Base body: a plain person only (a base body is the human WolfAvatars body; a furry is always dressed). */
    bool basesShown() const { return mKind == KIND_PERSON; }
    bool isClothing() const { return mKind == KIND_CLOTHING; }
    /** Woman / Man shows (and figure is sent) for a base body or for clothing (WOLFAVATARS_API.md "Figure"). */
    bool figureShown() const { return isClothing() || (basesShown() && mBase); }
    void setKind(EKind kind);
    void setClothes(bool base);
    void setGarment(S32 n);
    void setFigure(bool male);
    /** The choices under the description follow the kind: Dressed / Base (people), Garment (clothing), Woman / Man. */
    void refreshClothes();

    void showStep(EStep step);
    void setError(const std::string& error, const LLSD& reply = LLSD());
    void clearError();
    void setBusy(bool busy);

    void onKindChanged();
    void onGetPrice();
    void onBack();
    void onStart();
    void onAskStop();
    void onStopAnswer(bool stop);
    void onPick();
    void onMore();
    void onOpenInventory();
    void onMakeAnother();
    void onBuyCredits();
    void onPictureClicked(S32 n);

    void resume();
    void loadAvatars();                 // [MADE FOR 2026-10-20] jobs -> avatars: the "This clothing is for" list
    void takeAvatars(const LLSD& reply);
    void fillForCombo();
    void poll();
    /** A job {job} reply (or the job a resume found): the step and every widget follow its state. */
    void applyJob(const LLSD& job);
    void loadPictures(S32 count, S32 round);
    void loadPicture(S32 n);
    void askMorePrice();
    void refreshPictures();

    LLHandle<LLFloater> handle() const { return getHandle(); }

    EStep mStep = STEP_LOADING;
    bool mBusy = false;                 // a call this window started (not a poll) is out: its buttons are greyed
    std::string mJob;                   // the job shown ("" = none)
    LLSD mJobLast;                      // the last job {job} reply
    U32 mSerial = 0;                    // bumped when the shown job changes: a late reply for another job is dropped
    bool mPollOut = false;
    F64 mNextPoll = 0.0;
    bool mConfirmStop = false;          // the "Stop making it?" row is up

    // the quote shown on the price step (char_quote reply quote{credits, pictures, build})
    S32 mQuoteCredits = 0, mQuotePictures = 0, mQuoteBuild = 0;

    // the pictures of the current round (char_picture), kept while the job and round stay the same
    static const S32 PICTURES = 4;
    std::string mPicJob;
    S32 mPicRound = -1;
    S32 mPicCount = 0;
    S32 mPicked = -1;                   // the picture selected in the grid (not yet sent)
    S32 mMoreCredits = -1;              // char_more's price for 4 more pictures (-1 = not known)
    std::string mMoreFor;               // job + round the price is for
    std::string mBuyUrl;                // a credits_short refusal's buyurl ("" = no Buy credits link)

    EKind mKind = KIND_PERSON;          // Person (two legs), Animal (four legs) or Clothing: the three drawn tiles
    S32 mGarment = 0;                   // [CLOTHING 2026-10-20] index into GARMENT_IDS (llfloaterwolfavatars.cpp), Top first
    bool mMale = false;                 // [FIGURE 2026-10-20] figure "male" (true) or "female" (false, the default)
    // [MADE FOR 2026-10-20] clothing: "" not chosen yet, "female" / "male" a standard body, or the job of one of the
    // resident's avatars (jobs: avatars [{job, name}], wolfavatars_bodies); mAvatarsRead false until that list is read
    std::string mForWho;
    std::vector<std::pair<std::string, std::string>> mAvatars;
    bool mAvatarsRead = false;
    U32 mAvatarsSerial = 0;
    // [BASE BODY 2026-10-20] Paul: "make sure we can make avatars without clothing so people can add clothing later".
    // char_quote clothes "base" (true) or "dressed" (false, the default); people only (wolfavatars_lib.php wa_api_char_quote)
    bool mBase = false;
    S32 mWorkPct = -1;                  // the % in the job's progress words (-1: none, the ring turns instead)
    std::string mWorkPhase;             // the job's phase while it is made ("pictures" / "build", "" = not known yet)

    Layout mLayout;
    WolfAvatarsBackdrop* mBackdrop = nullptr;

    LLButton* mPersonBtn = nullptr;
    LLButton* mAnimalBtn = nullptr;
    LLButton* mFurryBtn = nullptr;      // [FURRY 2026-10-21]
    LLButton* mDressedBtn = nullptr;
    LLButton* mBaseBtn = nullptr;
    LLTextBox* mClothesHint = nullptr;
    LLButton* mClothingBtn = nullptr;
    LLButton* mGarmentBtn[GARMENTS] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    LLTextBox* mGarmentLabel = nullptr;
    LLTextBox* mGarmentHint = nullptr;
    LLTextBox* mFigureLabel = nullptr;
    LLButton* mWomanBtn = nullptr;
    LLButton* mManBtn = nullptr;
    LLComboBox* mForCombo = nullptr;    // [MADE FOR 2026-10-20] "This clothing is for" (clothing only)
    // the XUI's own words for a person or an animal, put back when Clothing is let go of
    std::string mIntroText, mPromptLabelText, mPromptTip, mTipsText, mDoneTitleText;
    LLTextBox* mLoadingText = nullptr;
    LLTextBox* mIntro = nullptr;
    LLTextBox* mKindLabel = nullptr;
    LLTextBox* mNameLabel = nullptr;
    LLTextBox* mPromptLabel = nullptr;
    LLTextBox* mHeightLabel = nullptr;
    LLTextBox* mTips = nullptr;
    LLTextBox* mPriceTitle = nullptr;
    LLTextBox* mPriceHow = nullptr;
    LLTextBox* mWorkNote = nullptr;
    LLTextBox* mDoneTitle = nullptr;
    LLTextBox* mDoneNote = nullptr;
    LLTextBox* mStopText = nullptr;
    LLButton* mStopYes = nullptr;
    LLButton* mStopNo = nullptr;
    LLPanel* mPicSlot[4] = { nullptr, nullptr, nullptr, nullptr };
    LLLineEditor* mName = nullptr;
    LLTextEditor* mPrompt = nullptr;
    LLLineEditor* mHeight = nullptr;
    LLTextBox* mHeightNote = nullptr;
    LLTextBox* mFormError = nullptr;
    LLButton* mPriceBtn = nullptr;

    LLTextBox* mPriceText = nullptr;
    LLButton* mBackBtn = nullptr;
    LLButton* mStartBtn = nullptr;

    LLTextBox* mWorkTitle = nullptr;
    LLTextBox* mWorkWords = nullptr;
    LLButton* mWorkStop = nullptr;

    LLTextBox* mPicTitle = nullptr;
    LLTextBox* mPicError = nullptr;
    WolfAvatarPicture* mPic[PICTURES] = { nullptr, nullptr, nullptr, nullptr };
    // [BIG PICTURE 2026-10-20] Paul: "when i get an image can i click on it to make it bigger" - "sometimes they are a bit
    // small": a clicked picture is shown large over the four (the four frames' area), with Pick and Back to all four
    WolfAvatarPicture* mPicBig = nullptr;
    LLButton* mZoomBackBtn = nullptr;
    bool mZoom = false;
    LLButton* mPickBtn = nullptr;
    LLButton* mMoreBtn = nullptr;
    LLButton* mPicStop = nullptr;

    LLTextEditor* mReport = nullptr;
    LLButton* mInvBtn = nullptr;
    LLButton* mAnotherBtn = nullptr;

    LLPanel* mPanels[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    LLTextBox* mError = nullptr;
    LLButton* mBuyBtn = nullptr;
    LLPanel* mStopPanel = nullptr;
};

#endif // LL_FLOATER_WOLF_AVATARS_H
