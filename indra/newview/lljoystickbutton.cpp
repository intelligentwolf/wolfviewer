/**
 * @file lljoystickbutton.cpp
 * @brief LLJoystick class implementation
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

#include "llviewerprecompiledheaders.h"

#include "lljoystickbutton.h"

// Library includes
#include "llcoord.h"
#include "indra_constants.h"
#include "llrender.h"

// Project includes
#include "llui.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewercontrol.h" // <FS:PP> gSavedSettings
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llmoveview.h"

#include "llglheaders.h"

static LLDefaultChildRegistry::Register<LLJoystickAgentSlide> r1("joystick_slide");
static LLDefaultChildRegistry::Register<LLJoystickAgentTurn> r2("joystick_turn");
static LLDefaultChildRegistry::Register<LLJoystickCameraRotate> r3("joystick_rotate");
static LLDefaultChildRegistry::Register<LLJoystickCameraTrack> r5("joystick_track");
static LLDefaultChildRegistry::Register<LLJoystickQuaternion> r6("joystick_quat");


const F32 NUDGE_TIME = 0.25f;       // in seconds
const F32 ORBIT_NUDGE_RATE = 0.05f; // fraction of normal speed

// <WolfViewer 2026-09-06> ANALOGUE THUMB STICKS.
//
// Paul, 2026-09-06: "the move and camera controls in wolfstorm are wonderful ... wolfstorm
// has lovely joysticks". Ported from WolfStorm's js/ui/camera_controls.js (orbit / track
// sticks + zoom) and js/input/touch_controls.js (movement stick), which in turn cite this
// file: the stock widget is a quadrant hit-test that fires a fixed rate, with no analogue
// value to scale by. Here, with wolf_analog="true":
//   - the knob follows the pointer, clamped to a ring (camera_controls.js _updateStick);
//   - deflection inside a 0.25 dead zone does nothing (a thumb resting a few pixels off
//     centre must not drift the camera forever — camera_controls.js DEAD_ZONE), and past
//     it is remapped to 0..1 per axis (_axis);
//   - the rate is that axis value times Firestorm's own nudge ramp (getOrbitRate /
//     LLFloaterMove::getYawRate) — at full deflection exactly the stock rate, below it
//     proportionally gentler. LLAgentCamera's key setters already take a magnitude
//     (llagentcamera.h:420 setOrbitLeftKey(F32 mag)), so the camera code is untouched;
//   - a press that never leaves the dead zone is a centre tap: the camera sticks reset
//     (camera_controls.js onUp -> _resetStick; stock: pointInCenterDot on the press);
//   - the widget draws itself: glass base disc, border, dead-zone ring, knob with a
//     centre dot (css/camera_controls.css / css/touch_controls.css colours, in colors.xml
//     as WolfJoystick*).
static const F32 WOLF_DEAD_ZONE = 0.25f;     // camera_controls.js:66, touch_controls.js:49
static const F32 WOLF_KNOB_FRACTION = 0.40f; // knob diameter / stick diameter (54 / 132 px)
// </WolfViewer>

//const S32 CENTER_DOT_RADIUS = 7;  // <FS:Beq/> FIRE-30414 Camera control arrows not clickable

//
// Public Methods
//
void QuadrantNames::declareValues()
{
    declare("origin", JQ_ORIGIN);
    declare("up", JQ_UP);
    declare("down", JQ_DOWN);
    declare("left", JQ_LEFT);
    declare("right", JQ_RIGHT);
}


LLJoystick::LLJoystick(const LLJoystick::Params& p)
:   LLButton(p),
    mWolfAnalog(p.wolf_analog),   // <WolfViewer 2026-09-06>
    mWolfActive(false),
    mWolfCentred(false),
    mWolfNX(0.f),
    mWolfNY(0.f),
    mInitialOffset(0, 0),
    mLastMouse(0, 0),
    mFirstMouse(0, 0),
    mVertSlopNear(0),
    mVertSlopFar(0),
    mHorizSlopNear(0),
    mHorizSlopFar(0),
    mHeldDown(false),
    mHeldDownTimer(),
    mInitialQuadrant(p.quadrant)
{
    setHeldDownCallback(&LLJoystick::onBtnHeldDown, this);
}


void LLJoystick::updateSlop()
{
    mVertSlopNear = getRect().getHeight();
    mVertSlopFar = getRect().getHeight() * 2;

    mHorizSlopNear = getRect().getWidth();
    mHorizSlopFar = getRect().getWidth() * 2;

    // Compute initial mouse offset based on initial quadrant.
    // Place the mouse evenly between the near and far zones.
    switch (mInitialQuadrant)
    {
    case JQ_ORIGIN:
        mInitialOffset.set(0, 0);
        break;

    case JQ_UP:
        mInitialOffset.mX = 0;
        mInitialOffset.mY = (mVertSlopNear + mVertSlopFar) / 2;
        break;

    case JQ_DOWN:
        mInitialOffset.mX = 0;
        mInitialOffset.mY = - (mVertSlopNear + mVertSlopFar) / 2;
        break;

    case JQ_LEFT:
        mInitialOffset.mX = - (mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        break;

    case JQ_RIGHT:
        mInitialOffset.mX = (mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        break;

    default:
        LL_ERRS() << "LLJoystick::LLJoystick() - bad switch case" << LL_ENDL;
        break;
    }

    return;
}

bool LLJoystick::pointInCircle(S32 x, S32 y) const
{
    // <FS:Chanayane> Fix joystick accepting clicks outside its circular shape when non-square
    // Original code assumed a square widget; if not square it accepted all clicks as a fallback,
    // causing camera rotation to trigger anywhere in the bounding rect.
    // Replaced with an ellipse test so non-square widgets are handled correctly.
    //if(this->getLocalRect().getHeight() != this->getLocalRect().getWidth())
    //{
    //    LL_DEBUGS() << "Joystick shape is not square"<<LL_ENDL;
    //    return true;
    //}
    ////center is x and y coordinates of center of joystick circle, and also its radius
    //int center = this->getLocalRect().getHeight()/2;
    //bool in_circle = (x - center) * (x - center) + (y - center) * (y - center) <= center * center;
    //return in_circle;

    // Point-in-ellipse test: (dx/a)^2 + (dy/b)^2 <= 1
    // where a and b are the horizontal and vertical semi-axes (half width/height).
    F32 a = this->getLocalRect().getWidth() / 2.f;
    F32 b = this->getLocalRect().getHeight() / 2.f;
    if (a == 0.f || b == 0.f)
    {
        return false;
    }
    F32 dx = x - a;
    F32 dy = y - b;
    return (dx * dx) / (a * a) + (dy * dy) / (b * b) <= 1.f;
    // </FS:Chanayane>
}

// <FS:Beq> FIRE-30414 Camera control arrows not clickable
// bool LLJoystick::pointInCenterDot(S32 x, S32 y, S32 radius) const
// {
//  if (this->getLocalRect().getHeight() != this->getLocalRect().getWidth())
//  {
//      LL_WARNS() << "Joystick shape is not square" << LL_ENDL;
//      return true;
//  }

//  S32 center = this->getLocalRect().getHeight() / 2;

//  bool in_center_circle = (x - center) * (x - center) + (y - center) * (y - center) <= radius * radius;

//  return in_center_circle;
// }
bool LLJoystick::pointInCenterDot(S32 x, S32 y) const
{
    constexpr auto center_dot_scale{0.15};// based on current images.
    S32 center_dot_x_rad = (S32)(this->getLocalRect().getWidth()/2*center_dot_scale);
    S32 center_dot_y_rad = (S32)(this->getLocalRect().getHeight()/2*center_dot_scale);
    auto a{this->getLocalRect().getCenterX()};
    auto b{this->getLocalRect().getCenterY()};
    // point inside ellipse if result 1 or less.
    auto result = ((((x - a)*(x - a)) / (center_dot_x_rad*center_dot_x_rad))
            +(((y - b)*(y - b)) / (center_dot_y_rad*center_dot_y_rad)));

    return result<=1?true:false;
}
// </FS:Beq>

bool LLJoystick::handleMouseDown(S32 x, S32 y, MASK mask)
{
    //LL_INFOS() << "joystick mouse down " << x << ", " << y << LL_ENDL;
    bool handles = false;

    if(pointInCircle(x, y))
    {
        mLastMouse.set(x, y);
        mFirstMouse.set(x, y);
        mMouseDownTimer.reset();
        // <WolfViewer 2026-09-06> Source: camera_controls.js onDown — the knob jumps to the
        // pointer at once and the press starts out "centred" until it leaves the dead zone.
        if (mWolfAnalog)
        {
            mWolfActive = true;
            mWolfCentred = true;
            wolfUpdateStick(x, y);
        }
        // </WolfViewer>
        handles = LLButton::handleMouseDown(x, y, mask);
    }

    return handles;
}


bool LLJoystick::handleMouseUp(S32 x, S32 y, MASK mask)
{
    // LL_INFOS() << "joystick mouse up " << x << ", " << y << LL_ENDL;

    if( hasMouseCapture() )
    {
        mLastMouse.set(x, y);
        mHeldDown = false;
        onMouseUp();
        // <WolfViewer 2026-09-06> Source: camera_controls.js onUp — knob snaps back; a press
        // that never left the centre is a reset, not a drag.
        if (mWolfAnalog)
        {
            const bool was_centred = mWolfCentred;
            mWolfActive = false;
            mWolfNX = 0.f;
            mWolfNY = 0.f;
            if (was_centred)
            {
                onWolfCentreTap();
            }
        }
        // </WolfViewer>
    }

    return LLButton::handleMouseUp(x, y, mask);
}


bool LLJoystick::handleHover(S32 x, S32 y, MASK mask)
{
    if( hasMouseCapture() )
    {
        mLastMouse.set(x, y);
        if (mWolfAnalog)   // <WolfViewer 2026-09-06>
        {
            wolfUpdateStick(x, y);
        }
    }

    return LLButton::handleHover(x, y, mask);
}

// <WolfViewer 2026-09-06> ---------------------------------------------------------------
// Source: camera_controls.js _stickRadius — travel from the LIVE sizes, never hard-coded.
F32 LLJoystick::wolfKnobRadius() const
{
    const F32 d = (F32)llmin(getRect().getWidth(), getRect().getHeight());
    return d * WOLF_KNOB_FRACTION * 0.5f;
}

F32 LLJoystick::wolfRadius() const
{
    const F32 d = (F32)llmin(getRect().getWidth(), getRect().getHeight());
    return llmax(12.f, (d - d * WOLF_KNOB_FRACTION) * 0.5f);
}

// Source: camera_controls.js _updateStick — clamp the knob to the ring so it reads as a
// stick, normalise to -1..1. LLView's y grows UPWARD, so +ny is screen-up here (the JS
// negates its screen-down y at the point of use; the axis mapping is the same).
void LLJoystick::wolfUpdateStick(S32 x, S32 y)
{
    const F32 cx = getRect().getWidth() * 0.5f;
    const F32 cy = getRect().getHeight() * 0.5f;
    const F32 radius = wolfRadius();
    F32 dx = (F32)x - cx;
    F32 dy = (F32)y - cy;
    const F32 len = sqrtf(dx * dx + dy * dy);
    if (len > radius)
    {
        const F32 k = radius / len;
        dx *= k;
        dy *= k;
    }
    mWolfNX = dx / radius;
    mWolfNY = dy / radius;
    if (sqrtf(mWolfNX * mWolfNX + mWolfNY * mWolfNY) > WOLF_DEAD_ZONE)
    {
        mWolfCentred = false;
    }
}

// Source: camera_controls.js _axis
F32 LLJoystick::wolfAxis(F32 v)
{
    const F32 dead = WOLF_DEAD_ZONE;
    if (v > dead) return llmin(1.f, (v - dead) / (1.f - dead));
    if (v < -dead) return llmax(-1.f, (v + dead) / (1.f - dead));
    return 0.f;
}

// Source: css/camera_controls.css .cam-stick / .cam-stick-ring / .cam-stick-dot /
// .cam-stick-knob and css/touch_controls.css — the same glass look, drawn with primitives
// (XUI has no blur; colour, border and the knob gradient's two tones are what carry it).
void LLJoystick::wolfDrawAnalog()
{
    static LLUIColor base        = LLUIColorTable::instance().getColor("WolfJoystickBase",         LLColor4(0.078f, 0.102f, 0.149f, 0.42f));
    static LLUIColor base_active = LLUIColorTable::instance().getColor("WolfJoystickBaseActive",   LLColor4(0.118f, 0.173f, 0.267f, 0.58f));
    static LLUIColor border      = LLUIColorTable::instance().getColor("WolfJoystickBorder",       LLColor4(0.627f, 0.784f, 1.f,    0.45f));
    static LLUIColor border_act  = LLUIColorTable::instance().getColor("WolfJoystickBorderActive", LLColor4(0.745f, 0.882f, 1.f,    0.75f));
    static LLUIColor ring        = LLUIColorTable::instance().getColor("WolfJoystickRing",         LLColor4(0.784f, 0.882f, 1.f,    0.35f));
    static LLUIColor knob        = LLUIColorTable::instance().getColor("WolfJoystickKnob",         LLColor4(0.922f, 0.961f, 1.f,    0.98f));
    static LLUIColor knob_shade  = LLUIColorTable::instance().getColor("WolfJoystickKnobShade",    LLColor4(0.471f, 0.647f, 0.863f, 0.95f));
    static LLUIColor dot         = LLUIColorTable::instance().getColor("WolfJoystickDot",          LLColor4(0.078f, 0.102f, 0.149f, 0.70f));

    LLGLSUIDefault gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    const F32 cx = getRect().getWidth() * 0.5f;
    const F32 cy = getRect().getHeight() * 0.5f;
    const F32 R = llmin(getRect().getWidth(), getRect().getHeight()) * 0.5f;
    const F32 travel = wolfRadius();
    const F32 kr = wolfKnobRadius();

    // base disc + border
    gGL.color4fv((mWolfActive ? base_active : base).get().mV);
    gl_circle_2d(cx, cy, R - 1.f, 64, true);
    gGL.color4fv((mWolfActive ? border_act : border).get().mV);
    gl_circle_2d(cx, cy, R - 1.f, 64, false);
    // dead-zone ring (decorative; the real threshold is WOLF_DEAD_ZONE of the travel)
    gGL.color4fv(ring.get().mV);
    gl_circle_2d(cx, cy, travel * WOLF_DEAD_ZONE, 32, false);
    // knob: highlight disc with a shaded inner disc offset toward the lower right, the
    // cheapest reading of the CSS radial gradient "circle at 35% 30%"
    const F32 kx = cx + mWolfNX * travel;
    const F32 ky = cy + mWolfNY * travel;
    gGL.color4fv(knob.get().mV);
    gl_circle_2d(kx, ky, kr, 48, true);
    gGL.color4fv(knob_shade.get().mV);
    gl_circle_2d(kx + kr * 0.12f, ky - kr * 0.14f, kr * 0.78f, 48, true);
    gGL.color4fv(dot.get().mV);
    gl_circle_2d(kx, ky, llmax(2.f, kr * 0.16f), 16, true);
}
// </WolfViewer> ---------------------------------------------------------------------------


F32 LLJoystick::getElapsedHeldDownTime()
{
    if( mHeldDown )
    {
        return getHeldDownTime();
    }
    else
    {
        return 0.f;
    }
}

// static
void LLJoystick::onBtnHeldDown(void *userdata)
{
    LLJoystick *self = (LLJoystick *)userdata;
    if (self)
    {
        self->mHeldDown = true;
        self->onHeldDown();
    }
}

EJoystickQuadrant LLJoystick::selectQuadrant(LLXMLNodePtr node)
{

    EJoystickQuadrant quadrant = JQ_RIGHT;

    if (node->hasAttribute("quadrant"))
    {
        std::string quadrant_name;
        node->getAttributeString("quadrant", quadrant_name);

        quadrant = quadrantFromName(quadrant_name);
    }
    return quadrant;
}


std::string LLJoystick::nameFromQuadrant(EJoystickQuadrant  quadrant)
{
    if (quadrant == JQ_ORIGIN)      return std::string("origin");
    else if (quadrant == JQ_UP)     return std::string("up");
    else if (quadrant == JQ_DOWN)   return std::string("down");
    else if (quadrant == JQ_LEFT)   return std::string("left");
    else if (quadrant == JQ_RIGHT)  return std::string("right");
    else return std::string();
}


EJoystickQuadrant LLJoystick::quadrantFromName(const std::string& sQuadrant)
{
    EJoystickQuadrant quadrant = JQ_RIGHT;

    if (sQuadrant == "origin")
    {
        quadrant = JQ_ORIGIN;
    }
    else if (sQuadrant == "up")
    {
        quadrant = JQ_UP;
    }
    else if (sQuadrant == "down")
    {
        quadrant = JQ_DOWN;
    }
    else if (sQuadrant == "left")
    {
        quadrant = JQ_LEFT;
    }
    else if (sQuadrant == "right")
    {
        quadrant = JQ_RIGHT;
    }

    return quadrant;
}


//-------------------------------------------------------------------------------
// LLJoystickAgentTurn
//-------------------------------------------------------------------------------

void LLJoystickAgentTurn::onHeldDown()
{
    F32 time = getElapsedHeldDownTime();
    updateSlop();

    // <WolfViewer 2026-09-06> Source: touch_controls.js _updateStick — up/down walk, left/right
    // turn — with the yaw analogue like the stock stick's m = dx/|dy| below, but from the
    // knob's own deflection, and the stock nudge for a tap.
    if (mWolfAnalog)
    {
        const F32 ax = wolfAxis(mWolfNX);
        const F32 ay = wolfAxis(mWolfNY);
        if (ax != 0.f)
        {
            gAgent.moveYaw(-LLFloaterMove::getYawRate(time) * ax);
        }
        if (ay > 0.f)
        {
            if (time < NUDGE_TIME) gAgent.moveAtNudge(1); else gAgent.moveAt(1);
        }
        else if (ay < 0.f)
        {
            if (time < NUDGE_TIME) gAgent.moveAtNudge(-1); else gAgent.moveAt(-1);
        }
        return;
    }
    // </WolfViewer>

    //LL_INFOS() << "move forward/backward (and/or turn)" << LL_ENDL;

    S32 dx = mLastMouse.mX - mFirstMouse.mX + mInitialOffset.mX;
    S32 dy = mLastMouse.mY - mFirstMouse.mY + mInitialOffset.mY;

    float m = (float) (dx)/abs(dy);

    if (m > 1) {
        m = 1;
    }
    else if (m < -1) {
        m = -1;
    }
    gAgent.moveYaw(-LLFloaterMove::getYawRate(time)*m);


    // handle forward/back movement
    if (dy > mVertSlopFar)
    {
        // ...if mouse is forward of run region run forward
        gAgent.moveAt(1);
    }
    else if (dy > mVertSlopNear)
    {
        if( time < NUDGE_TIME )
        {
            gAgent.moveAtNudge(1);
        }
        else
        {
            // ...else if mouse is forward of walk region walk forward
            // JC 9/5/2002 - Always run / move quickly.
            gAgent.moveAt(1);
        }
    }
    else if (dy < -mVertSlopFar)
    {
        // ...else if mouse is behind run region run backward
        gAgent.moveAt(-1);
    }
    else if (dy < -mVertSlopNear)
    {
        if( time < NUDGE_TIME )
        {
            gAgent.moveAtNudge(-1);
        }
        else
        {
            // ...else if mouse is behind walk region walk backward
            // JC 9/5/2002 - Always run / move quickly.
            gAgent.moveAt(-1);
        }
    }
}

// <WolfViewer 2026-09-06>
void LLJoystickAgentTurn::draw()
{
    if (mWolfAnalog)
    {
        wolfDrawAnalog();
        return;
    }
    LLJoystick::draw();
}
// </WolfViewer>

//-------------------------------------------------------------------------------
// LLJoystickAgentSlide
//-------------------------------------------------------------------------------

void LLJoystickAgentSlide::onMouseUp()
{
    F32 time = getElapsedHeldDownTime();
    if( time < NUDGE_TIME )
    {
        switch (mInitialQuadrant)
        {
        case JQ_LEFT:
            gAgent.moveLeftNudge(1);
            break;

        case JQ_RIGHT:
            gAgent.moveLeftNudge(-1);
            break;

        default:
            break;
        }
    }
}

void LLJoystickAgentSlide::onHeldDown()
{
    //LL_INFOS() << "slide left/right (and/or move forward/backward)" << LL_ENDL;

    updateSlop();

    S32 dx = mLastMouse.mX - mFirstMouse.mX + mInitialOffset.mX;
    S32 dy = mLastMouse.mY - mFirstMouse.mY + mInitialOffset.mY;

    // handle left-right sliding
    if (dx > mHorizSlopNear)
    {
        gAgent.moveLeft(-1);
    }
    else if (dx < -mHorizSlopNear)
    {
        gAgent.moveLeft(1);
    }

    // handle forward/back movement
    if (dy > mVertSlopFar)
    {
        // ...if mouse is forward of run region run forward
        gAgent.moveAt(1);
    }
    else if (dy > mVertSlopNear)
    {
        // ...else if mouse is forward of walk region walk forward
        gAgent.moveAtNudge(1);
    }
    else if (dy < -mVertSlopFar)
    {
        // ...else if mouse is behind run region run backward
        gAgent.moveAt(-1);
    }
    else if (dy < -mVertSlopNear)
    {
        // ...else if mouse is behind walk region walk backward
        gAgent.moveAtNudge(-1);
    }
}


//-------------------------------------------------------------------------------
// LLJoystickCameraRotate
//-------------------------------------------------------------------------------

LLJoystickCameraRotate::LLJoystickCameraRotate(const LLJoystickCameraRotate::Params& p)
:   LLJoystick(p),
    mInLeft( false ),
    mInTop( false ),
    mInRight( false ),
    mInBottom( false ),
    mInCenter( false )
{
    mCenterImageName = "Cam_Rotate_Center";
}


void LLJoystickCameraRotate::updateSlop()
{
    // do the initial offset calculation based on mousedown location

    // small fixed slop region
    mVertSlopNear = 16;
    mVertSlopFar = 32;

    mHorizSlopNear = 16;
    mHorizSlopFar = 32;

    return;
}


bool LLJoystickCameraRotate::handleMouseDown(S32 x, S32 y, MASK mask)
{
    gAgent.setMovementLocked(true);
    updateSlop();

    // <WolfViewer 2026-09-06> analogue: no quadrant, no initial offset — the knob is the input.
    if (mWolfAnalog)
    {
        mInitialOffset.mX = 0;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_ORIGIN;
        mInCenter = false;
        return LLJoystick::handleMouseDown(x, y, mask);
    }
    // </WolfViewer>

    // Set initial offset based on initial click location
    S32 horiz_center = getRect().getWidth() / 2;
    S32 vert_center = getRect().getHeight() / 2;

    S32 dx = x - horiz_center;
    S32 dy = y - vert_center;
    // <FS:Beq> FIRE-30414
    // if (pointInCenterDot(x, y, CENTER_DOT_RADIUS))
    if (pointInCenterDot(x, y))
    // </FS:Beq>
    {
        mInitialOffset.mX = 0;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_ORIGIN;
        mInCenter = true;

        resetJoystickCamera();
    }
    else if (dy > dx && dy > -dx)
    {
        // top
        mInitialOffset.mX = 0;
        mInitialOffset.mY = (mVertSlopNear + mVertSlopFar) / 2;
        mInitialQuadrant = JQ_UP;
    }
    else if (dy > dx && dy <= -dx)
    {
        // left
        mInitialOffset.mX = - (mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_LEFT;
    }
    else if (dy <= dx && dy <= -dx)
    {
        // bottom
        mInitialOffset.mX = 0;
        mInitialOffset.mY = - (mVertSlopNear + mVertSlopFar) / 2;
        mInitialQuadrant = JQ_DOWN;
    }
    else
    {
        // right
        mInitialOffset.mX = (mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_RIGHT;
    }

    return LLJoystick::handleMouseDown(x, y, mask);
}

bool LLJoystickCameraRotate::handleMouseUp(S32 x, S32 y, MASK mask)
{
    gAgent.setMovementLocked(false);
    mInCenter = false;
    return LLJoystick::handleMouseUp(x, y, mask);
}

bool LLJoystickCameraRotate::handleHover(S32 x, S32 y, MASK mask)
{
    // <FS:Beq> FIRE-30414
    // if (!pointInCenterDot(x, y, CENTER_DOT_RADIUS))
    if (!pointInCenterDot(x, y))
    // </FS:Beq>
    {
        mInCenter = false;
    }

    return LLJoystick::handleHover(x, y, mask);
}

void LLJoystickCameraRotate::onHeldDown()
{
    updateSlop();

    // <WolfViewer 2026-09-06> Source: camera_controls.js _tick (orbit): push RIGHT ->
    // setOrbitLeftKey, push UP -> setOrbitUpKey, scaled by the deflection and the nudge ramp.
    if (mWolfAnalog)
    {
        const F32 rate = getOrbitRate();
        const F32 ax = wolfAxis(mWolfNX);
        const F32 ay = wolfAxis(mWolfNY);
        if (ax > 0.f)      { gAgentCamera.unlockView(); gAgentCamera.setOrbitLeftKey(ax * rate); }
        else if (ax < 0.f) { gAgentCamera.unlockView(); gAgentCamera.setOrbitRightKey(-ax * rate); }
        if (ay > 0.f)      { gAgentCamera.unlockView(); gAgentCamera.setOrbitUpKey(ay * rate); }
        else if (ay < 0.f) { gAgentCamera.unlockView(); gAgentCamera.setOrbitDownKey(-ay * rate); }
        return;
    }
    // </WolfViewer>

    S32 dx = mLastMouse.mX - mFirstMouse.mX + mInitialOffset.mX;
    S32 dy = mLastMouse.mY - mFirstMouse.mY + mInitialOffset.mY;

    // left-right rotation
    if (dx > mHorizSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setOrbitLeftKey(getOrbitRate());
    }
    else if (dx < -mHorizSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setOrbitRightKey(getOrbitRate());
    }

    // over/under rotation
    if (dy > mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setOrbitUpKey(getOrbitRate());
    }
    else if (dy < -mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setOrbitDownKey(getOrbitRate());
    }
}

void LLJoystickCameraRotate::resetJoystickCamera()
{
    // <FS:PP> If user opted to disable center reset buttons, do not reset
    if (gSavedSettings.getBOOL("DisableCameraJoystickCenterReset"))
    {
        return;
    }
    // </FS:PP>
    gAgentCamera.resetCameraOrbit();
}

F32 LLJoystickCameraRotate::getOrbitRate()
{
    F32 time = getElapsedHeldDownTime();
    if( time < NUDGE_TIME )
    {
        F32 rate = ORBIT_NUDGE_RATE + time * (1 - ORBIT_NUDGE_RATE)/ NUDGE_TIME;
        //LL_INFOS() << rate << LL_ENDL;
        return rate;
    }
    else
    {
        return 1;
    }
}


// Only used for drawing
void LLJoystickCameraRotate::setToggleState( bool left, bool top, bool right, bool bottom )
{
    mInLeft = left;
    mInTop = top;
    mInRight = right;
    mInBottom = bottom;
}

void LLJoystickCameraRotate::draw()
{
    // <WolfViewer 2026-09-06>
    if (mWolfAnalog)
    {
        wolfDrawAnalog();
        return;
    }
    // </WolfViewer>
    LLGLSUIDefault gls_ui;

  getImageUnselected()->draw( getLocalRect() );
    LLPointer<LLUIImage> image = getImageSelected();

    if (mInCenter)
    {
        drawRotatedImage(LLUI::getUIImage(mCenterImageName), 0);
    }
    else
    {
        if (mInTop)
        {
            drawRotatedImage(getImageSelected(), 0);
        }

        if (mInRight)
        {
            drawRotatedImage(getImageSelected(), 1);
        }

        if (mInBottom)
        {
            drawRotatedImage(getImageSelected(), 2);
        }

        if (mInLeft)
        {
            drawRotatedImage(getImageSelected(), 3);
        }
    }
}

// Draws image rotated by multiples of 90 degrees
void LLJoystickCameraRotate::drawRotatedImage( LLPointer<LLUIImage> image, S32 rotations )
{
    S32 width = image->getWidth();
    S32 height = image->getHeight();
    LLTexture* texture = image->getImage();

    /*
     * Scale  texture coordinate system
     * to handle the different between image size and size of texture.
     * If we will use default matrix,
     * it may break texture mapping after rotation.
     * see EXT-2023 Camera floater: arrows became shifted when pressed.
     */
    F32 uv[][2] =
    {
        { (F32)width/texture->getWidth(), (F32)height/texture->getHeight() },
        { 0.f, (F32)height/texture->getHeight() },
        { 0.f, 0.f },
        { (F32)width/texture->getWidth(), 0.f }
    };

    gGL.getTexUnit(0)->bind(texture);

    gGL.color4fv(UI_VERTEX_COLOR.mV);

    gGL.begin(LLRender::TRIANGLES);
    {
        S32 scaledWidth = getLocalRect().getWidth();
        S32 scaledHeight = getLocalRect().getHeight();

        gGL.texCoord2fv(uv[(rotations + 0) % 4]);
        gGL.vertex2i(scaledWidth, scaledHeight );

        gGL.texCoord2fv(uv[(rotations + 1) % 4]);
        gGL.vertex2i(0, scaledHeight );

        gGL.texCoord2fv(uv[(rotations + 2) % 4]);
        gGL.vertex2i(0, 0);


        gGL.texCoord2fv(uv[(rotations + 0) % 4]);
        gGL.vertex2i(scaledWidth, scaledHeight );

        gGL.texCoord2fv(uv[(rotations + 2) % 4]);
        gGL.vertex2i(0, 0);

        gGL.texCoord2fv( uv[ (rotations + 3) % 4]);
        gGL.vertex2i(scaledWidth, 0);
    }
    gGL.end();
}



//-------------------------------------------------------------------------------
// LLJoystickCameraTrack
//-------------------------------------------------------------------------------

LLJoystickCameraTrack::Params::Params()
{
    held_down_delay.seconds(0.0);
}

LLJoystickCameraTrack::LLJoystickCameraTrack(const LLJoystickCameraTrack::Params& p)
:   LLJoystickCameraRotate(p)
{
    mCenterImageName = "Cam_Tracking_Center";
}


void LLJoystickCameraTrack::onHeldDown()
{
    updateSlop();

    // <WolfViewer 2026-09-06> Source: camera_controls.js _tick (track): push RIGHT ->
    // setPanRightKey, push UP -> setPanUpKey, scaled by the deflection and the nudge ramp.
    if (mWolfAnalog)
    {
        const F32 rate = getOrbitRate();
        const F32 ax = wolfAxis(mWolfNX);
        const F32 ay = wolfAxis(mWolfNY);
        if (ax > 0.f)      { gAgentCamera.unlockView(); gAgentCamera.setPanRightKey(ax * rate); }
        else if (ax < 0.f) { gAgentCamera.unlockView(); gAgentCamera.setPanLeftKey(-ax * rate); }
        if (ay > 0.f)      { gAgentCamera.unlockView(); gAgentCamera.setPanUpKey(ay * rate); }
        else if (ay < 0.f) { gAgentCamera.unlockView(); gAgentCamera.setPanDownKey(-ay * rate); }
        return;
    }
    // </WolfViewer>

    S32 dx = mLastMouse.mX - mFirstMouse.mX + mInitialOffset.mX;
    S32 dy = mLastMouse.mY - mFirstMouse.mY + mInitialOffset.mY;

    if (dx > mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setPanRightKey(getOrbitRate());
    }
    else if (dx < -mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setPanLeftKey(getOrbitRate());
    }

    // over/under rotation
    if (dy > mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setPanUpKey(getOrbitRate());
    }
    else if (dy < -mVertSlopNear)
    {
        gAgentCamera.unlockView();
        gAgentCamera.setPanDownKey(getOrbitRate());
    }
}

void LLJoystickCameraTrack::resetJoystickCamera()
{
    // <FS:PP> If user opted to disable center reset buttons, do not reset
    if (gSavedSettings.getBOOL("DisableCameraJoystickCenterReset"))
    {
        return;
    }
    // </FS:PP>
    gAgentCamera.resetCameraPan();
}

//-------------------------------------------------------------------------------
// LLJoystickQuaternion
//-------------------------------------------------------------------------------

LLJoystickQuaternion::Params::Params()
{
}

LLJoystickQuaternion::LLJoystickQuaternion(const LLJoystickQuaternion::Params &p):
    LLJoystick(p),
    mInLeft(false),
    mInTop(false),
    mInRight(false),
    mInBottom(false),
    mVectorZero(0.0f, 0.0f, 1.0f),
    mRotation(),
    mUpDnAxis(1.0f, 0.0f, 0.0f),
    mLfRtAxis(0.0f, 0.0f, 1.0f),
    mXAxisIndex(2), // left & right across the control
    mYAxisIndex(0), // up & down across the  control
    mZAxisIndex(1)  // tested for above and below
{
    for (int i = 0; i < 3; ++i)
    {
        mLfRtAxis.mV[i] = (mXAxisIndex == i) ? 1.0f : 0.0f;
        mUpDnAxis.mV[i] = (mYAxisIndex == i) ? 1.0f : 0.0f;
    }
}

void LLJoystickQuaternion::setToggleState(bool left, bool top, bool right, bool bottom)
{
    mInLeft = left;
    mInTop = top;
    mInRight = right;
    mInBottom = bottom;
}

bool LLJoystickQuaternion::handleMouseDown(S32 x, S32 y, MASK mask)
{
    updateSlop();

    // Set initial offset based on initial click location
    S32 horiz_center = getRect().getWidth() / 2;
    S32 vert_center = getRect().getHeight() / 2;

    S32 dx = x - horiz_center;
    S32 dy = y - vert_center;

    if (dy > dx && dy > -dx)
    {
        // top
        mInitialOffset.mX = 0;
        mInitialOffset.mY = (mVertSlopNear + mVertSlopFar) / 2;
        mInitialQuadrant = JQ_UP;
    }
    else if (dy > dx && dy <= -dx)
    {
        // left
        mInitialOffset.mX = -(mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_LEFT;
    }
    else if (dy <= dx && dy <= -dx)
    {
        // bottom
        mInitialOffset.mX = 0;
        mInitialOffset.mY = -(mVertSlopNear + mVertSlopFar) / 2;
        mInitialQuadrant = JQ_DOWN;
    }
    else
    {
        // right
        mInitialOffset.mX = (mHorizSlopNear + mHorizSlopFar) / 2;
        mInitialOffset.mY = 0;
        mInitialQuadrant = JQ_RIGHT;
    }

    return LLJoystick::handleMouseDown(x, y, mask);
}

bool LLJoystickQuaternion::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return LLJoystick::handleMouseUp(x, y, mask);
}

void LLJoystickQuaternion::onHeldDown()
{
    LLVector3 axis;
    updateSlop();

    S32 dx = mLastMouse.mX - mFirstMouse.mX + mInitialOffset.mX;
    S32 dy = mLastMouse.mY - mFirstMouse.mY + mInitialOffset.mY;

    // left-right rotation
    if (dx > mHorizSlopNear)
    {
        axis += mUpDnAxis;
    }
    else if (dx < -mHorizSlopNear)
    {
        axis -= mUpDnAxis;
    }

    // over/under rotation
    if (dy > mVertSlopNear)
    {
        axis += mLfRtAxis;
    }
    else if (dy < -mVertSlopNear)
    {
        axis -= mLfRtAxis;
    }

    if (axis.isNull())
        return;

    axis.normalize();

    LLQuaternion delta;
    delta.setAngleAxis(0.0523599f, axis);   // about 3deg

    mRotation *= delta;
    setValue(mRotation.getValue());
    onCommit();
}

void LLJoystickQuaternion::draw()
{
    LLGLSUIDefault gls_ui;

    getImageUnselected()->draw(0, 0);
    LLPointer<LLUIImage> image = getImageSelected();

    if (mInTop)
    {
        drawRotatedImage(getImageSelected(), 0);
    }

    if (mInRight)
    {
        drawRotatedImage(getImageSelected(), 1);
    }

    if (mInBottom)
    {
        drawRotatedImage(getImageSelected(), 2);
    }

    if (mInLeft)
    {
        drawRotatedImage(getImageSelected(), 3);
    }

    LLVector3 draw_point = mVectorZero * mRotation;
    S32 halfwidth = getRect().getWidth() / 2;
    S32 halfheight = getRect().getHeight() / 2;
    draw_point.mV[mXAxisIndex] = (draw_point.mV[mXAxisIndex] + 1.0f) * halfwidth;
    draw_point.mV[mYAxisIndex] = (draw_point.mV[mYAxisIndex] + 1.0f) * halfheight;

    gl_circle_2d(draw_point.mV[mXAxisIndex], draw_point.mV[mYAxisIndex], 4, 8,
        draw_point.mV[mZAxisIndex] >= 0.f);

}

F32 LLJoystickQuaternion::getOrbitRate()
{
    return 1;
}

void LLJoystickQuaternion::updateSlop()
{
    // small fixed slop region
    mVertSlopNear = 16;
    mVertSlopFar = 32;

    mHorizSlopNear = 16;
    mHorizSlopFar = 32;
}

void LLJoystickQuaternion::drawRotatedImage(LLPointer<LLUIImage> image, S32 rotations)
{
    S32 width = image->getWidth();
    S32 height = image->getHeight();
    LLTexture* texture = image->getImage();

    /*
    * Scale  texture coordinate system
    * to handle the different between image size and size of texture.
    */
    F32 uv[][2] =
    {
        { (F32)width / texture->getWidth(), (F32)height / texture->getHeight() },
        { 0.f, (F32)height / texture->getHeight() },
        { 0.f, 0.f },
        { (F32)width / texture->getWidth(), 0.f }
    };

    gGL.getTexUnit(0)->bind(texture);

    gGL.color4fv(UI_VERTEX_COLOR.mV);

    gGL.begin(LLRender::TRIANGLES);
    {
        gGL.texCoord2fv(uv[(rotations + 0) % 4]);
        gGL.vertex2i(width, height);

        gGL.texCoord2fv(uv[(rotations + 1) % 4]);
        gGL.vertex2i(0, height);

        gGL.texCoord2fv(uv[(rotations + 2) % 4]);
        gGL.vertex2i(0, 0);

        gGL.texCoord2fv(uv[(rotations + 0) % 4]);
        gGL.vertex2i(width, height);

        gGL.texCoord2fv(uv[(rotations + 1) % 4]);
        gGL.vertex2i(0, height);

        gGL.texCoord2fv(uv[(rotations + 3) % 4]);
        gGL.vertex2i(width, 0);
    }
    gGL.end();
}

void LLJoystickQuaternion::setRotation(const LLQuaternion &value)
{
    if (value != mRotation)
    {
        mRotation = value;
        mRotation.normalize();
        LLJoystick::setValue(mRotation.getValue());
    }
}

LLQuaternion LLJoystickQuaternion::getRotation() const
{
    return mRotation;
}


