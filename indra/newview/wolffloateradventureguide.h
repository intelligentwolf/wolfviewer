/**
 * @file wolffloateradventureguide.h
 * @brief WolfViewer: the Adventure Guide - help for the Studio page or step on screen, docked beside the Studio.
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

#ifndef WOLF_FLOATER_ADVENTURE_GUIDE_H
#define WOLF_FLOATER_ADVENTURE_GUIDE_H

#include "llfloater.h"
#include "lluictrl.h"
#include "wolffloaterroleplay.h"   // WolfGameHits

class WolfAdventureStudioView;

/**
 * [WOLF ADVENTURE AUDIT 2026-10-17] Paul 2026-10-17: "maybe a full tutorial in a floater next to it".
 * [SIMPLE STUDIO 2026-10-17] SIMPLE_STUDIO_SPEC.md: it never opens on its own; opened by hand (the Studio's Guide button) it
 * shows the three pages as three short steps - Your story, Your characters, Your story map - the one on screen marked, each
 * a way to that page. On an expert page (Advanced editing) or a Start here step it adds that page's help in plain words with
 * an example and a link to the matching page of the website guide (https://www.wolf-grid.com/story-guide?p=<slug>, the slugs
 * of gridmanager/wolfstoryguide_lib.php wsg_pages). Drawn in the wolfGame family like the Studio.
 */
class WolfAdventureGuideView : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params() {}
    };
    WolfAdventureGuideView(const Params& p);

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;
    bool handleUnicodeCharHere(llwchar uni_char) override;
    void onMouseCaptureLost() override;

private:
    enum EHit { GH_NONE = 0, GH_CLOSE, GH_WEB, GH_STEP };
    void press(S32 id, S32 arg);
    WolfAdventureStudioView* studio() const;

    WolfGameHits mHits;
    F32 mScroll = 0.f;
    F32 mContentH = 0.f;
    bool mKeyboardNav = false;
};

class WolfFloaterAdventureGuide : public LLFloater
{
public:
    WolfFloaterAdventureGuide(const LLSD& key);
    bool postBuild() override;
    void draw() override;
};

#endif // WOLF_FLOATER_ADVENTURE_GUIDE_H
