/**
 * @file lljoystickbutton.h
 * @brief LLJoystick class definition
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLJOYSTICKBUTTON_H
#define LL_LLJOYSTICKBUTTON_H

#include "llbutton.h"
#include "llcoord.h"
#include "llviewertexture.h"
#include "llquaternion.h"

typedef enum e_joystick_quadrant
{
    JQ_ORIGIN,
    JQ_UP,
    JQ_DOWN,
    JQ_LEFT,
    JQ_RIGHT
} EJoystickQuadrant;

struct QuadrantNames : public LLInitParam::TypeValuesHelper<EJoystickQuadrant, QuadrantNames>
{
    static void declareValues();
};

class LLJoystick
:   public LLButton
{
public:
    struct Params
    :   public LLInitParam::Block<Params, LLButton::Params>
    {
        Optional<EJoystickQuadrant, QuadrantNames> quadrant;
        // <WolfViewer 2026-09-06> ANALOGUE thumb stick (WolfStorm's js/ui/camera_controls.js
        // and js/input/touch_controls.js): a knob that follows the pointer inside a ring,
        // a 0.25 dead zone, and a rate that scales with how far the stick is pushed. Off
        // by default so every stock XUI joystick is untouched.
        Optional<bool> wolf_analog;

        Params()
        :   quadrant("quadrant", JQ_ORIGIN),
            wolf_analog("wolf_analog", false)
        {
            changeDefault(label, "");
        }
    };
    LLJoystick(const Params&);

    virtual bool    handleMouseDown(S32 x, S32 y, MASK mask);
    virtual bool    handleMouseUp(S32 x, S32 y, MASK mask);
    virtual bool    handleHover(S32 x, S32 y, MASK mask);

    virtual void    onMouseUp() {}
    virtual void    onHeldDown() = 0;
    F32             getElapsedHeldDownTime();

    static void     onBtnHeldDown(void *userdata);      // called by llbutton callback handler
    void            setInitialQuadrant(EJoystickQuadrant initial) { mInitialQuadrant = initial; };

    /**
     * Checks if click location is inside joystick circle.
     *
     * Image containing circle is square and this square has adherent points with joystick
     * circle. Make sure to change method according to shape other than square.
     */
    bool    pointInCircle(S32 x, S32 y) const;
    // <FS:Beq> FIRE-30414 Camera control arrows not clickable
    // bool pointInCenterDot(S32 x, S32 y, S32 radius) const;
    bool    pointInCenterDot(S32 x, S32 y) const;
    // </FS:Beq>
    static std::string nameFromQuadrant(const EJoystickQuadrant quadrant);
    static EJoystickQuadrant quadrantFromName(const std::string& name);
    static EJoystickQuadrant selectQuadrant(LLXMLNodePtr node);


    // <WolfViewer 2026-09-06> analogue stick state and drawing (see the .cpp header note)
    bool            isWolfAnalog() const { return mWolfAnalog; }
    /** Deflection past the dead zone, remapped to -1..1 per axis (camera_controls.js _axis). */
    static F32      wolfAxis(F32 v);
    /** A press that never left the centre dot was released (camera_controls.js onUp). */
    virtual void    onWolfCentreTap() {}
    void            wolfDrawAnalog();
    // </WolfViewer>

protected:
    virtual void    updateSlop();                   // recompute slop margins
    // <WolfViewer 2026-09-06>
    F32             wolfRadius() const;             // travel of the knob's centre, pixels
    F32             wolfKnobRadius() const;
    void            wolfUpdateStick(S32 x, S32 y);
    // </WolfViewer>

protected:
    // <WolfViewer 2026-09-06>
    bool                mWolfAnalog;
    bool                mWolfActive;                // pointer captured on the stick
    bool                mWolfCentred;               // never left the dead zone since the press
    F32                 mWolfNX;                    // knob offset / travel, +right
    F32                 mWolfNY;                    // knob offset / travel, +UP (LLView y grows up)
    // </WolfViewer>
    EJoystickQuadrant   mInitialQuadrant;           // mousedown = click in this quadrant
    LLCoordGL           mInitialOffset;             // pretend mouse started here
    LLCoordGL           mLastMouse;                 // where was mouse on last hover event
    LLCoordGL           mFirstMouse;                // when mouse clicked, where was it
    S32                 mVertSlopNear;              // where the slop regions end
    S32                 mVertSlopFar;               // where the slop regions end
    S32                 mHorizSlopNear;             // where the slop regions end
    S32                 mHorizSlopFar;              // where the slop regions end
    bool                mHeldDown;
    LLFrameTimer        mHeldDownTimer;
};


// Turn agent left and right, move forward and back
class LLJoystickAgentTurn
:   public LLJoystick
{
public:
    struct Params : public LLJoystick::Params {};
    LLJoystickAgentTurn(const Params& p) : LLJoystick(p) {}
    virtual void    onHeldDown();
    virtual void    draw();   // <WolfViewer 2026-09-06> analogue stick drawing
};


// Slide left and right, move forward and back
class LLJoystickAgentSlide
:   public LLJoystick
{
public:
    struct Params : public LLJoystick::Params {};
    LLJoystickAgentSlide(const Params& p) : LLJoystick(p) {}

    virtual void    onHeldDown();
    virtual void    onMouseUp();
};


// Rotate camera around the focus point
class LLJoystickCameraRotate
:   public LLJoystick
{
public:
    struct Params
    :   public LLInitParam::Block<Params, LLJoystick::Params>
    {
        Params()
        {
            changeDefault(held_down_delay.seconds, 0.0);
        }
    };

    LLJoystickCameraRotate(const LLJoystickCameraRotate::Params&);

    virtual void    setToggleState( bool left, bool top, bool right, bool bottom );

    virtual bool    handleMouseDown(S32 x, S32 y, MASK mask);
    virtual bool    handleMouseUp(S32 x, S32 y, MASK mask);
    virtual bool    handleHover(S32 x, S32 y, MASK mask);
    virtual void    onHeldDown();
    virtual void    resetJoystickCamera();
    virtual void    draw();
    virtual void    onWolfCentreTap() { resetJoystickCamera(); }   // <WolfViewer 2026-09-06>

protected:
    F32             getOrbitRate();
    virtual void    updateSlop();
    void            drawRotatedImage( LLPointer<LLUIImage> image, S32 rotations );

protected:
    bool            mInLeft;
    bool            mInTop;
    bool            mInRight;
    bool            mInBottom;
    bool            mInCenter;

    std::string     mCenterImageName;
};


// Track the camera focus point forward/backward and side to side
class LLJoystickCameraTrack
:   public LLJoystickCameraRotate
{
public:
    struct Params
    :   public LLInitParam::Block<Params, LLJoystickCameraRotate::Params>
    {
        Params();
    };

    LLJoystickCameraTrack(const LLJoystickCameraTrack::Params&);
    virtual void    onHeldDown();
    virtual void    resetJoystickCamera();
};

//
class LLJoystickQuaternion :
    public LLJoystick
{
public:
    struct Params :
        public LLInitParam::Block<Params, LLJoystick::Params>
    {
        Params();
    };

    LLJoystickQuaternion(const LLJoystickQuaternion::Params &);

    virtual void    setToggleState(bool left, bool top, bool right, bool bottom);

    virtual bool    handleMouseDown(S32 x, S32 y, MASK mask);
    virtual bool    handleMouseUp(S32 x, S32 y, MASK mask);
    virtual void    onHeldDown();
    virtual void    draw();

    void            setRotation(const LLQuaternion &value);
    LLQuaternion    getRotation() const;

protected:
    F32             getOrbitRate();
    virtual void    updateSlop();
    void            drawRotatedImage(LLPointer<LLUIImage> image, S32 rotations);

    bool            mInLeft;
    bool            mInTop;
    bool            mInRight;
    bool            mInBottom;

    S32             mXAxisIndex;
    S32             mYAxisIndex;
    S32             mZAxisIndex;

    LLVector3       mVectorZero;
    LLQuaternion    mRotation;
    LLVector3       mUpDnAxis;
    LLVector3       mLfRtAxis;
};

#endif  // LL_LLJOYSTICKBUTTON_H
