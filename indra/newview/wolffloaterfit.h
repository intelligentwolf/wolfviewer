/**
 * @file wolffloaterfit.h
 * @brief WolfViewer: keep a tall Wolf window inside the screen (its content scrolls instead).
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

#ifndef WOLF_FLOATERFIT_H
#define WOLF_FLOATERFIT_H

#include "llfloater.h"

/**
 * [TICKET 529 PATTERN 2026-11-05] Paul "fix 2": the Wolf Animator (560 x 716) and the Building Generator (720 x 706) ran
 * past the bottom of a 1360 x 768 screen at UI scale 1.35. The rule is the Region tab's (commit 953fecd63d,
 * wolfpassingplanes.cpp layout): a window may be only as tall as the floater view's snap rect (LLFloaterView::getSnapRect:
 * the space between the menu bar and the bottom toolbars, where floaters are kept) and its content scrolls; when there is
 * room again it goes back to its own height. Its top stays where it is unless the bottom would leave the snap rect, then it
 * moves up just enough. A minimised window, or one not in the floater view (torn off, docked), is left alone.
 * Called every frame from the window's draw(): it only reshapes when something changed.
 */
namespace WolfFloaterFit
{
    inline void fit(LLFloater* f, S32 natural_height)
    {
        if (!f || !gFloaterView || f->getParent() != gFloaterView || f->isMinimized() || natural_height <= 0) return;
        const LLRect snap = gFloaterView->getSnapRect();
        if (snap.getHeight() <= 0) return;
        LLRect r = f->getRect();
        const S32 want = llmin(natural_height, snap.getHeight());
        S32 top = llmin(r.mTop, snap.mTop);
        if (top - want < snap.mBottom) top = snap.mBottom + want;
        if (r.getHeight() == want && r.mTop == top) return;
        r.setLeftTopAndSize(r.mLeft, top, r.getWidth(), want);
        f->setShape(r);
    }
}

#endif // WOLF_FLOATERFIT_H
