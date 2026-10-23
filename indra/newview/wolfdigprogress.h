/**
 * @file wolfdigprogress.h
 * @brief WolfViewer: the painted "working" plate of the Tunnel tool and the Cave Builder - a digging mole, a word that fits
 *        the step, the real step and its progress, and failures that stay and can be copied.
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

#ifndef WOLF_DIG_PROGRESS_H
#define WOLF_DIG_PROGRESS_H

#include <string>

#include "llview.h"

class LLButton;
class LLFloater;
class LLTextEditor;

// [WOLF TUNNELS 2026-10-23] Paul: "make the interface look much more fancy like our uploaders liking when it's making the stuff
// do words like Tunnelling, Digging, Moling (like a mole digging)". Who and why: a region owner who pressed Build, Light up or
// Delete wants to see at a glance that it is working, which step it is on and how far along, and - when it stops - to read
// the reason and copy it. Flow: work (mole + a word for the step + the step's own line + a bar from real counts) -> done (a
// tick and the line, a few seconds) or failed (a red plate with the reason in a read-only, selectable box and an OK).
//
// Painted in code the way the uploaders are (wolffloatermeshupload.cpp WolfMeshUploadSheet, llfloaterwolfavatars.cpp
// WolfAvatarsBackdrop: wolfpaint.h primitives, a fixed palette with every text pair's contrast worked out, text on its own
// plate, a turning arc while busy, a warning mark with every error, never colour alone). Not an XUI widget (made in code, not
// registered), so it adds no widget Params to the factory. It lies over the window's own status lines while it shows.
class WolfDigProgress : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };

    /** Make one and put it in `owner` at `rect` (floater-local), in front of the controls it covers. Hidden until shown. */
    static WolfDigProgress* add(LLFloater* owner, const LLRect& rect);

    /** Working on `line` (the job's own words, e.g. "Step 2 of 5: making the rock..."). `fraction` 0..1 from real counts, or
     *  below 0 when the step cannot be counted (the bar then sweeps, and no percent is shown). */
    void showWork(const std::string& line, F32 fraction);
    /** Finished: a tick and `line` for a few seconds, then hidden (the window's status line keeps the words). */
    void showDone(const std::string& line);
    /** Stopped: `text` on the red plate until OK is pressed or the next job starts; the text can be selected and copied. */
    void showFailed(const std::string& text);
    void hide();
    bool failedShown() const { return mMode == MODE_FAILED; }

    void draw() override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    WolfDigProgress(const Params& p);
    enum EMode { MODE_HIDDEN, MODE_WORK, MODE_DONE, MODE_FAILED };
    void layoutChildren();
    void drawMole(const LLRect& pit, F32 t, bool lit) const;

    EMode mMode = MODE_HIDDEN;
    std::string mLine;
    F32 mFraction = -1.f;
    F64 mDoneAt = 0.0;
    LLTextEditor* mError = nullptr;
    LLButton* mOk = nullptr;
};

#endif // WOLF_DIG_PROGRESS_H
