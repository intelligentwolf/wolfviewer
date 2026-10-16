/**
 * @file llviewerdisplay.h
 * @brief LLViewerDisplay class header file
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLVIEWERDISPLAY_H
#define LL_LLVIEWERDISPLAY_H

#include "stdtypes.h"   // U64 gPresentedFrameCount (WolfViewer 2026-10-16)

class LLPostProcess;

void display_startup();
void display_cleanup();

void display(bool rebuild = true, F32 zoom_factor = 1.f, int subfield = 0, bool for_snapshot = false);

extern bool gDisplaySwapBuffers;
// [WolfViewer 2026-10-16] frames actually presented (every swapBuffers in llviewerdisplay.cpp)
extern U64 gPresentedFrameCount;
// [WolfViewer 2026-10-16] the REAL frame rate: frames presented over the wall-clock time they took, the last ~1 s,
// updated twice a second (0 until the first half second). The status bar shows it always; the performance floaters
// show it under the Wolf environment (Paul: "make the performance floaters show the correct figures as well").
F32 wolf_real_fps();
extern bool gDepthDirty;
extern bool gTeleportDisplay;
extern LLFrameTimer gTeleportDisplayTimer;
extern bool         gForceRenderLandFence;
extern bool gResizeScreenTexture;
extern bool gResizeShadowTexture;
extern bool gWindowResized;

// <FS:Ansariel> Draw Distance stepping; originally based on SpeedRez by Henri Beauchamp, licensed under LGPL
extern F32 gSavedDrawDistance;
extern F32 gLastDrawDistanceStep;

// <FS:Ansariel> FIRE-12004: Attachments getting lost on TP
extern LLFrameTimer gPostTeleportFinishKillObjectDelayTimer;

#endif // LL_LLVIEWERDISPLAY_H
