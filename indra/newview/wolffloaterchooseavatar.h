/**
 * @file wolffloaterchooseavatar.h
 * @brief WolfViewer: Choose an Avatar - become one of the grid's starter looks (people, animals, sci-fi).
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

#ifndef WOLF_FLOATERCHOOSEAVATAR_H
#define WOLF_FLOATERCHOOSEAVATAR_H

#include "llfloater.h"
#include "llpointer.h"
#include "lluuid.h"

class LLButton;
class LLPanel;
class LLTextBox;
class LLTextEditor;
class LLViewerTexture;
class WolfStarterCard;
class WolfChooseAvatarBackdrop;

// [STARTER AVATARS 2026-10-24] Paul: "for new user sign ups we offer two possible avatar choices - can we increase that to
// 10 now using our ai generator: animals, people, scifi - then add the choice to the viewer and wolfstorm: pick an avatar
// like second life has". The looks come from gridmanager/starteravatarsapi.php (the same list the sign-up page shows).
// Choosing one: the server saves the current outfit as "My outfit before <name>" in My Outfits and copies the look's folder
// into Clothing; the viewer loads that folder and wears it with LLAppearanceMgr (Replace Outfit: body parts the look does
// not bring are kept, llappearancemgr.cpp updateCOF). The look's own shape carries its Hover, so its feet meet the ground
// in every viewer; a per-account Hover Height left from before would add to it, so it is set back to 0 (and said so).
// Wolf Territories logins only: the requests carry this login's agent and session ids.
class WolfFloaterChooseAvatar : public LLFloater
{
public:
    WolfFloaterChooseAvatar(const LLSD& key);
    ~WolfFloaterChooseAvatar() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    void drawBackdrop();   // WolfChooseAvatarBackdrop::draw

    struct Look
    {
        S32 mId = 0;
        std::string mName;
        std::string mKind;      // people | animal | scifi (starteravatarslib.php SA_KINDS)
        std::string mBlurb;
        LLPointer<LLViewerTexture> mTexture;
        S32 mPictureState = 0;  // 0 not asked, 1 loading, 2 shown, 3 failed
        std::string mPictureWhy;
    };

private:
    enum class EStatus { INFO, OK, ERR };
    static constexpr S32 CARDS = 6;   // 3 x 2 a group: the most looks one group has (People)

    void requestList();
    void onList(const LLSD& reply, const std::string& error);
    void requestPicture(S32 index);
    void onPicture(S32 id, const LLSD& reply, const std::string& error);
    void showKind(const std::string& kind);
    void refreshCards();
    void select(S32 look_index);
    void refreshChosen();
    void onWear();
    void setStatus(const std::string& text, EStatus kind);
    void setBusy(bool busy);
    void onChosen(const LLSD& reply, const std::string& error);
    Look* lookById(S32 id);

    static void apiCoro(LLHandle<LLFloater> handle, LLSD body, std::function<void(WolfFloaterChooseAvatar*, const LLSD&, const std::string&)> done);
    static void wearCoro(LLHandle<LLFloater> handle, LLUUID folder, LLUUID outfit, std::string name);

    std::vector<Look> mLooks;
    std::vector<S32> mShown;               // indices into mLooks of the cards on show
    std::string mKind = "people";
    S32 mSelected = -1;                    // index into mLooks
    bool mBusy = false;
    bool mListAsked = false;

    LLPanel* mCardsPanel = nullptr;
    WolfStarterCard* mCards[CARDS] = {};
    LLButton* mKindBtn[3] = {};
    LLTextBox* mEmpty = nullptr;
    LLTextBox* mChosenName = nullptr;
    LLTextBox* mChosenBlurb = nullptr;
    LLTextBox* mReplaceNote = nullptr;
    LLTextEditor* mStatus = nullptr;
    LLButton* mWear = nullptr;
    WolfChooseAvatarBackdrop* mBackdrop = nullptr;
};

#endif // WOLF_FLOATERCHOOSEAVATAR_H
