/**
 * @file wolffloatercompass.h
 * @brief WolfViewer: World > Compass — a Victorian mariner's compass that shows North and the
 *        direction you are actually moving.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATERCOMPASS_H
#define WOLF_FLOATERCOMPASS_H

#include "llfloater.h"
#include "llframetimer.h"

#include <string>
#include <vector>

class LLFontGL;

// [WOLF COMPASS 2026-10-16] Paul: "Create a new compass floating window that shows NORTH with a
// classical compass arrow and the direction the user is moving make it clear, but victorian."
//
// Drawn entirely in code (no textures): a brass bowl, an aged-parchment card with a 16-point rose,
// a red-and-black needle whose North tip is a fleur-de-lis, and a blued-steel course pointer on the rim.
//
// FRAME (read this before changing the maths). It behaves like a real hand compass held in front
// of the screen:
//   - The fixed black LUBBER MARK at the top of the brass bowl is where the view is looking (the
//     main camera's horizontal heading, the same axis the mini-map turns with: llnetmap.cpp draw(),
//     atan2(camera at.x, camera at.y)). The card turns under it, so the degree read at the lubber
//     mark is the heading.
//   - The card, its rose and the needle always point to NORTH. The needle is the only arrow on the
//     card and is always drawn at full colour (Paul 10-16: "the compass arrow should always turn north").
//   - The blued-steel pointer on the brass rim shows the way the avatar (or the vehicle it sits on)
//     is MOVING over the ground, drawn in the same frame, so "pointer at the top" means moving the
//     way you look. Below AT_REST_SPEED it fades out completely and the readout says "at rest".
// World axes: +X is east, +Y is north (lldefs.h gDirAxes: east {1,0}, north {0,1}), so a
// horizontal vector (x, y) has the compass bearing atan2(x, y), clockwise from north.
class WolfFloaterCompass : public LLFloater
{
public:
    WolfFloaterCompass(const LLSD& key);
    ~WolfFloaterCompass() override = default;

    bool postBuild() override;
    void draw() override;
    void onOpen(const LLSD& key) override;

private:
    // Where the view looks (bearing, degrees clockwise from north) and how the avatar moves.
    F32  sampleHeading() const;
    bool sampleCourse(F32& course_deg) const;   // false when at rest
    void updateSmoothing();

    void drawCompass(F32 cx, F32 cy, F32 R) const;
    void drawReadout(const LLRect& plaque, const LLFontGL* font) const;

    const std::string& pointName(F32 bearing_deg) const;

    // Smoothed state: what is on screen eases towards the sampled values every frame.
    F32  mHeading = 0.f;       // degrees, clockwise from north
    F32  mCourse = 0.f;        // degrees, clockwise from north (the last course while at rest)
    F32  mMoving = 0.f;        // 0 at rest .. 1 moving, eased so the rim pointer fades rather than blinks
    bool mIsMoving = false;    // this frame's sample: moving faster than AT_REST_SPEED
    bool mHaveCourse = false;  // a course has been seen since the window opened
    bool mPrimed = false;      // the next frame snaps instead of easing (first frame, after a gap)
    LLFrameTimer mFrameTimer;

    // Text from the XUI (floater_wolf_compass.xml) so it can be translated.
    std::vector<std::string> mPointNames;   // 16 names clockwise from North
    std::string mHeadingLabel;
    std::string mCourseLabel;
    std::string mAtRestLabel;
};

#endif // WOLF_FLOATERCOMPASS_H
