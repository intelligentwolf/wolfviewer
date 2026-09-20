/**
 * @file wolffloaterphotoeffects.h
 * @brief WolfViewer: World > Photo Effects… — a tile per style and a strength slider.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#ifndef WOLF_FLOATERPHOTOEFFECTS_H
#define WOLF_FLOATERPHOTOEFFECTS_H

#include "llfloater.h"

class LLButton;
class LLSliderCtrl;
class LLTextBox;

// [PAINTERS 2026-09-20] Source: wolfstorm/js/ui/floaters/floater_photo_effects.js — same
// layout, same tiles (textures/wolf_photo/<id>.png, the real filter shader run over a harbour
// scene by wolfstorm/tests/photo_tiles_gen.html). Reads and writes the two settings the post
// pass uses, WolfViewerPhotoFilter (0..12) and WolfViewerPhotoFilterStrength (0..1), so the
// world changes as you click and drag. Paul: "its nice to have it open rather than having to
// click the menu each time", "a little image showing each style".
class WolfFloaterPhotoEffects : public LLFloater
{
public:
    WolfFloaterPhotoEffects(const LLSD& key);
    ~WolfFloaterPhotoEffects() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    static constexpr S32 MODE_COUNT = 13;   // 0 Off .. 12 Seurat (pipeline.cpp wolfPhotoFilter)

private:
    void onTile(S32 mode);
    void onStrength();
    void refresh();

    LLButton*     mTiles[MODE_COUNT] = { nullptr };
    LLSliderCtrl* mStrength = nullptr;
    LLTextBox*    mStatus = nullptr;
    boost::signals2::connection mModeConn, mStrengthConn;
};

#endif // WOLF_FLOATERPHOTOEFFECTS_H
