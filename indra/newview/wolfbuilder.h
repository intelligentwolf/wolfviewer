/**
 * @file wolfbuilder.h
 * @brief WolfViewer: the builder's helper - Builders Mode (a Midday sky while the Build floater is
 *        open), a grid drawn on the ground while building, snapping to the builder's own grid origin,
 *        and floating names over nearby objects. The Wolf Builder floater (wolffloaterbuilder.cpp) is
 *        the switchboard; this is the machinery.
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

#ifndef WOLF_BUILDER_H
#define WOLF_BUILDER_H

#include <map>
#include <string>

#include "llframetimer.h"
#include "llpointer.h"
#include "llsettingsdaycycle.h"
#include "llsettingssky.h"
#include "llsettingswater.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v3math.h"
#include "v4color.h"
#include "llquaternion.h"

class LLHUDText;
class LLViewerObject;

// [WOLF BUILDER 2026-10-13] Paul: "on the build floater add a Builders Mode that will set the EEP to
// midday so it's easy to see, and add another Wolf Builder floater with controls like a grid, easier
// snapping to defined grids, floating object names (all as options), floating number of prims and
// location like a card - imagine if you will like a builders helper".
//
// Settings (app_settings/settings.xml): WolfBuildersMode, WolfBuilderGrid, WolfBuilderSnapOrigin,
// WolfBuilderGridOrigin, WolfBuilderObjectGrid, WolfBuilderObjectGridAllFaces, WolfBuilderNames,
// WolfBuilderNamesRadius. The grid's spacing and size are the
// snap grid's own GridResolution and GridDrawSize, so the Build floater's Options... window and the
// Wolf Builder window always agree.
class WolfBuilder : public LLSingleton<WolfBuilder>
{
    LLSINGLETON(WolfBuilder);
    ~WolfBuilder();

public:
    // ---- Builders Mode (the Build floater's check box) ----
    // Put the Midday sky on the local environment exactly as World > Environment > Midday does, after
    // remembering what the local environment held. Idempotent. False (with the reason in why_not) when
    // it could not: RLV forbids environment changes, or the legacy Midday preset failed to load.
    bool applyBuildersMode(std::string& why_not);
    // Put back what the local environment held when applyBuildersMode ran, unless the user has since
    // chosen another sky themselves (then their choice stands). No-op when nothing was applied.
    void restoreEnvironment();
    bool buildersModeApplied() const { return mApplied; }

    // ---- the grid ----
    // WolfBuilderSnapOrigin on: snapping and the drawn grid are relative to WolfBuilderGridOrigin
    // (metres from the region's south-west corner) instead of the region corner itself.
    static bool      snapOriginEnabled();
    static LLVector3 snapOriginAgent();   // the origin in agent coordinates (the region's corner when no region)
    // Draw the grids, from render_ui_3d (llviewerdisplay.cpp) under gUIProgram like the manipulators,
    // while Builders Mode (WolfBuildersMode) is on and the Build floater or the Wolf Builder floater
    // is open:
    //  - WolfBuilderGrid: on the ground (the land height under its centre) around the selection or you;
    //  - WolfBuilderObjectGrid: on the selection's own bounding box, in the Build floater's ruler frame
    //    (World / Local / Reference), on its bottom face or, with WolfBuilderObjectGridAllFaces, all six.
    void renderGrid();
    // Whether the grids may draw now: a floater to build with is open.
    static bool gridWindowsOpen();

    // ---- floating object names ----
    // Every frame from LLAppViewer::idle: keep a name tag over each root prim within
    // WolfBuilderNamesRadius of the avatar while WolfBuilderNames is on; drop them all when it is off.
    void idle();
    void clearNameTags();
    S32  nameTagCount() const { return (S32)mTags.size(); }

private:
    struct Tag
    {
        LLPointer<LLViewerObject> mObject;
        LLPointer<LLHUDText>      mText;
        std::string               mName;
    };
    // The plane a grid lies in, in its own frame: llmaniptranslate.cpp renderGridVert's three cases.
    enum EPlane { PLANE_XY, PLANE_XZ, PLANE_YZ };
    void drawGridTiles(EPlane plane, F32 size, F32 r, F32 g, F32 b, F32 a);
    // One grid: centred at center_agent, lying in `plane` of the frame `rotation`, a line every `scale`
    // (per axis, grid frame) counted from grid_origin_agent.
    void drawGridPlane(EPlane plane, const LLVector3& center_agent, const LLQuaternion& rotation,
                       const LLVector3& grid_origin_agent, const LLVector3& scale, F32 size,
                       const LLColor4& line_color, F32 alpha, U32 tex);
    void renderGroundGrid(U32 tex, F32 alpha);
    void renderObjectGrid(U32 tex, F32 alpha);
    void logGridDrawing(bool drawing, const char* why);

    bool                  mGridWasDrawing = false;   // what logGridDrawing last logged
    std::string           mGridLogReason;

    std::map<LLUUID, Tag> mTags;
    LLFrameTimer          mScanTimer;
    bool                  mNamesWereOn = false;   // last frame's WolfBuilderNames, to catch it being switched on

    // What the local environment held before Builders Mode, and what Builders Mode put there.
    bool                    mApplied = false;
    bool                    mHadLocal = false;
    bool                    mSavedSkyFixed = false;   // the sky was a fixed one, not the day cycle's (NO_ANIMATE_SKY)
    LLSettingsDay::ptr_t    mSavedDay;
    LLSettingsDay::Seconds  mSavedDayLength = LLSettingsDay::Seconds(0);
    LLSettingsDay::Seconds  mSavedDayOffset = LLSettingsDay::Seconds(0);
    LLSettingsSky::ptr_t    mSavedSky;
    LLSettingsWater::ptr_t  mSavedWater;
    LLSettingsSky::ptr_t    mLegacyMidday;   // the OpenSim legacy-windlight Midday, when that path was taken
};

#endif // WOLF_BUILDER_H
