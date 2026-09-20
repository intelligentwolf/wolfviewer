/**
 * @file wolffloaterphotoeffects.cpp
 * @brief WolfViewer: World > Photo Effects… — a tile per style and a strength slider.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */
#include "llviewerprecompiledheaders.h"

#include "wolffloaterphotoeffects.h"

#include "llbutton.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

namespace
{
    // Same order and names as wolfstorm photo_filter.js FILTERS; index = WolfViewerPhotoFilter.
    const char* const MODE_LABELS[WolfFloaterPhotoEffects::MODE_COUNT] =
    {
        "Off", "Sepia", "Black & White", "Noir", "Vintage", "Warm", "Cool", "Faded", "Vivid",
        "Picasso", "Van Gogh", "Monet", "Seurat"
    };
}

WolfFloaterPhotoEffects::WolfFloaterPhotoEffects(const LLSD& key)
:   LLFloater(key)
{
}

WolfFloaterPhotoEffects::~WolfFloaterPhotoEffects()
{
    mModeConn.disconnect();
    mStrengthConn.disconnect();
}

bool WolfFloaterPhotoEffects::postBuild()
{
    for (S32 i = 0; i < MODE_COUNT; ++i)
    {
        mTiles[i] = getChild<LLButton>(llformat("tile_%d", i));
        mTiles[i]->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { onTile(i); });
    }
    mStrength = getChild<LLSliderCtrl>("strength");
    mStrength->setCommitCallback([this](LLUICtrl*, const LLSD&) { onStrength(); });
    mStatus = getChild<LLTextBox>("status");
    // The menu's WolfPhoto.* listeners and a Preferences backup restore can change the
    // settings while this is open: follow them rather than showing a stale tile.
    if (LLControlVariable* c = gSavedSettings.getControl("WolfViewerPhotoFilter"))
    {
        mModeConn = c->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { refresh(); });
    }
    if (LLControlVariable* c = gSavedSettings.getControl("WolfViewerPhotoFilterStrength"))
    {
        mStrengthConn = c->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { refresh(); });
    }
    refresh();
    return true;
}

void WolfFloaterPhotoEffects::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    refresh();
}

void WolfFloaterPhotoEffects::onTile(S32 mode)
{
    // Source: llviewermenu.cpp WolfPhotoSetFilter — the same setting, the same clamp.
    gSavedSettings.setS32("WolfViewerPhotoFilter", llclamp(mode, 0, MODE_COUNT - 1));
    refresh();
}

void WolfFloaterPhotoEffects::onStrength()
{
    // Source: llviewermenu.cpp WolfPhotoSetStrength — percent in, 0..1 stored.
    gSavedSettings.setF32("WolfViewerPhotoFilterStrength", llclamp(mStrength->getValueF32() / 100.f, 0.f, 1.f));
    refresh();
}

void WolfFloaterPhotoEffects::refresh()
{
    const S32 mode = llclamp(gSavedSettings.getS32("WolfViewerPhotoFilter"), 0, MODE_COUNT - 1);
    const S32 pct = ll_round(llclamp(gSavedSettings.getF32("WolfViewerPhotoFilterStrength"), 0.f, 1.f) * 100.f);
    for (S32 i = 0; i < MODE_COUNT; ++i)
    {
        if (mTiles[i]) mTiles[i]->setToggleState(i == mode);   // is_toggle buttons: the _sel image is the outline
    }
    if (mStrength && ll_round(mStrength->getValueF32()) != pct) mStrength->setValue((F32)pct);
    if (mStatus)
    {
        mStatus->setText(mode == 0
            ? std::string("Off. Applies to your own view only. Snapshots include it.")
            : llformat("%s at %d%%. Applies to your own view only. Snapshots include it.", MODE_LABELS[mode], pct));
    }
}
