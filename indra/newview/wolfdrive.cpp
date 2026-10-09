/**
 * @file wolfdrive.cpp
 * @brief WolfViewer driving: controls, the analogue link to WolfSim, the dashboard's data. See wolfdrive.h.
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

#include "llviewerprecompiledheaders.h"

#include "wolfdrive.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "fsnearbychathub.h"
#include "llagent.h"
#include "llappviewer.h"
#include "llcommandmanager.h"
#include "lldispatcher.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llmoveview.h"
#include "llstartup.h"
#include "lltoolbarview.h"
#include "llviewercontrol.h"
#include "llviewergenericmessage.h"
#include "llviewerjoystick.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "wolfflight.h"
#include "wolfgrid.h"
#include "wolfobjectprops.h"

// ════════════════════════════════════════════════════════════════════════════════════════
// The three vehicles. Every published figure below cites where it was read (researched
// 2026-10-09); the rest are marked MODEL - the viewer's own choices for a believable gearbox
// when no script sends its own values, not figures from any manufacturer.
// ════════════════════════════════════════════════════════════════════════════════════════
namespace
{
    // CAR - Porsche 911 Carrera (992).
    //   Ratios, reverse, axle 3.39 x transaxle 0.92, rear tyre 295/35 ZR 20, maximum engine speed
    //   7500, 16.6 gal tank and 21 mpg combined: Porsche newsroom "992.2 911 Carrera and Carrera
    //   Cabriolet Technical Specifications" (PDF).
    //   The analogue rev counter, 8000 rpm full scale with the red line at 7400: Motor1, "The New
    //   Porsche 911 Loses Its Analog Tachometer" (the 992.1 cluster).
    //   Speedometer 330 km/h full scale: the BMW M3 (F80) cluster (vehiclesizes.com), the other
    //   analogue sports cluster researched; 205 mph = 330 km/h, shown as 200 mph.
    const F32 CAR_RATIOS[8] = { 4.89f, 3.17f, 2.15f, 1.56f, 1.18f, 0.94f, 0.76f, 0.61f };

    // TRUCK - Scania R 450 with the GRS905 12-speed range-splitter (Opticruise).
    //   Ratios 11.320 .. 1.000 (12 forward, crawlers left out) and reverse low 14.766: "905 Gearbox
    //   Student Material" (pdfcoffee.com) and the Scribd "GEAR-RATIO" GRS905 summary.
    //   Rear axle 2.59: one of the Scania R756 ratios listed in Scania's "Superior axle efficiency"
    //   brochure (IAA 2024). Tyres 315/70 R22.5: Scania R 450 4x2 listings (truckscout24).
    //   Red line 2400 rpm, maximum power at 1900: TruckersReport forum, "SCANIA 124 / 470 max RPM".
    //   Green band 1100-1500 rpm: a Scania owner, same search (Scania's own figure not published).
    //   90 km/h limiter: EU Directive 92/6/EEC (N3 vehicles, EUR-Lex).
    //   700 l tank (R 450 4x2 listings) at 25.19 l/100 km (Scania R 450, 40 t long-haul test).
    //   The 3000 rpm and 120 km/h full scales are this design's (MODEL), chosen to put the limiter
    //   three quarters round the dial as truck clusters do.
    const F32 TRUCK_RATIOS[12] = { 11.320f, 9.164f, 7.194f, 5.823f, 4.632f, 3.750f,
                                   3.019f, 2.444f, 1.918f, 1.553f, 1.235f, 1.000f };

    // BIKE - BMW S 1000 RR.
    //   Primary 1.652 and gears 2.647 .. 1.261: fastcarcheck.uk (S 1000RR 2019-). Final drive 2.65
    //   (17/45) and rear tyre 190/55 ZR17: motorcycle.com 2019 S 1000 RR specifications.
    //   Rev limiter 14,600 rpm: Rider Magazine, 2023 S 1000 RR first ride. 299 km/h: BMW's claim.
    //   16.5 l tank, 6.4 l/100 km WMTC: BMW Motorrad technical data.
    //   The bar's 15,000 rpm full scale is this design's (MODEL).
    const F32 BIKE_PRIMARY = 1.652f;
    const F32 BIKE_RATIOS[6] = { 2.647f * BIKE_PRIMARY, 2.091f * BIKE_PRIMARY, 1.727f * BIKE_PRIMARY,
                                 1.500f * BIKE_PRIMARY, 1.360f * BIKE_PRIMARY, 1.261f * BIKE_PRIMARY };

    // Rolling circumference from the tyre code: pi x (rim + 2 x section x aspect).
    constexpr F32 tyreCirc(F32 section_mm, F32 aspect, F32 rim_in)
    {
        return 3.14159265f * (rim_in * 25.4f + 2.f * section_mm * aspect) / 1000.f;
    }

    const WolfDrive::Spec SPECS[WolfDrive::TYPE_COUNT] =
    {
        // name   gears ratios       reverse  final            tyre
        { "CAR",   8, CAR_RATIOS,    3.99f,   3.39f * 0.92f,   tyreCirc(295.f, 0.35f, 20.f),
        //  idle   launch  upL     upF     downL   downF   (MODEL; up / down: the AUTO box only)
            800.f, 2200.f, 2300.f, 6900.f, 1300.f, 3800.f,
        //  limiter dial    redline  green       km/h    mph     governor range                         shift
            7500.f, 8000.f, 7400.f,  0.f, 0.f,   330.f,  200.f,  0.f,     16.6f * 21.f * 1.609344f,     0.25f },
        { "TRUCK", 12, TRUCK_RATIOS, 14.766f, 2.59f,           tyreCirc(315.f, 0.70f, 22.5f),
            600.f, 1000.f, 1250.f, 1850.f, 950.f,  1250.f,
            2400.f, 3000.f, 2400.f,  1100.f, 1500.f, 120.f, 80.f, 90.f,    700.f / 25.19f * 100.f,        0.8f },
        { "BIKE",  6, BIKE_RATIOS,   0.f,     2.65f,           tyreCirc(190.f, 0.55f, 17.f),
            1300.f, 4000.f, 0.f,   0.f,    0.f,    0.f,
            14600.f, 15000.f, 14600.f, 0.f, 0.f,  0.f,    0.f,   0.f,     16.5f / 6.4f * 100.f,          0.f },
    };

    // Source: wolfvehiclecontrols.cpp (the Move floater's Vehicle tab, removed 2026-10-09) and
    // touch_controls.js: DEAD_ZONE 0.25 for the on-screen wheel, SHIFT_TAP 150 ms, NUDGE_TIME 0.25 s
    // (llviewerinput.cpp NUDGE_TIME), KEY_STALE 0.25 s.
    const F32 SCREEN_DEAD_ZONE = 0.25f;
    const F64 SHIFT_TAP_SECONDS = 0.15;
    const F32 NUDGE_TIME = 0.25f;
    const F64 KEY_STALE_SECONDS = 0.25;
    // The controller is read every frame while the window has focus; a frame hitch must not drop
    // the pedals, only a real loss of focus (scanJoystick stops before our hook).
    const F64 HW_STALE_SECONDS = 1.0;
    // A script's engine speed and gear older than this are a script that stopped: the model again.
    const F64 SCRIPT_LIVE_SECONDS = 5.0;
    // WolfDriveModule.cs: on change at most 10 a second, once a second unchanged.
    const F64 SEND_MIN_GAP = 0.1;
    const F64 SEND_REFRESH = 1.0;
    const S32 MAX_REMEMBERED_VEHICLES = 200;

    const std::string METHOD_TO_SIM("wolfdrive");          // WolfDriveModule.cs METHOD_IN
    const std::string MESSAGE_FROM_SIM("WolfDashboard");   // WolfDriveModule.cs MESSAGE_OUT

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    LLViewerObject* seatRoot()
    {
        if (!isAgentAvatarValid() || !gAgentAvatarp->isSitting())
        {
            return nullptr;
        }
        LLViewerObject* p = (LLViewerObject*)gAgentAvatarp->getParent();
        while (p && p->getParent())
        {
            p = (LLViewerObject*)p->getParent();
        }
        return p;
    }

    // Source: wolfflight.cpp worldRotation - own rotation, then every parent's.
    LLQuaternion worldRotation(const LLViewerObject* objectp)
    {
        LLQuaternion q = objectp->getRotation();
        const LLViewerObject* p = (const LLViewerObject*)objectp->getParent();
        while (p)
        {
            q = q * p->getRotation();
            p = (const LLViewerObject*)p->getParent();
        }
        return q;
    }

    F32 deadZone(F32 v, F32 dz)
    {
        if (dz >= 0.99f) return 0.f;
        const F32 a = fabsf(v);
        if (a <= dz) return 0.f;
        return (v < 0.f ? -1.f : 1.f) * llmin(1.f, (a - dz) / (1.f - dz));
    }

    bool parseNumber(const std::string& s, F64& out)
    {
        if (s.empty() || s.size() > 24) return false;
        for (char c : s)
        {
            if (!((c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        }
        errno = 0;
        char* end = nullptr;
        const double v = strtod(s.c_str(), &end);
        if (errno != 0 || !end || *end != '\0' || !std::isfinite(v)) return false;
        out = v;
        return true;
    }

    const char* const ACTION_SETTINGS[WolfDrive::ACTION_COUNT] =
    {
        "WolfDriveButtonGearUp", "WolfDriveButtonGearDown", "WolfDriveButtonHorn", "WolfDriveButtonLights",
        "WolfDriveButtonIndicatorLeft", "WolfDriveButtonIndicatorRight", "WolfDriveButtonHandbrake",
        "WolfDriveButtonHazards", "WolfDriveButtonCruise",
    };
    const char* const ACTION_LABELS[WolfDrive::ACTION_COUNT] =
    {
        "Gear up", "Gear down", "Horn", "Lights", "Left indicator", "Right indicator", "Handbrake",
        "Hazard lights", "Cruise control",
    };
    const char* const ACTION_IDS[WolfDrive::ACTION_COUNT] =
    {
        "gear_up", "gear_down", "horn", "lights", "ind_left", "ind_right", "handbrake", "hazards", "cruise",
    };
    // The keys an action has until the driver changes it: letters no viewer binding uses unmodified
    // (app_settings/key_bindings.xml binds W A S D E C F R T) - the viewer's own choice. Gear up /
    // down have none: Page Up / Page Down already reach the vehicle as CONTROL_UP / CONTROL_DOWN.
    const char* const ACTION_DEFAULT_KEYS[WolfDrive::ACTION_COUNT] =
    {
        "", "", "H", "L", "Z", "X", "B", "V", "K",
    };
    const char* const OUT_LABELS[WolfDrive::OUT_COUNT] =
    {
        "Nothing (dashboard only)", "Page Up (CONTROL_UP)", "Page Down (CONTROL_DOWN)", "A chat command",
    };
    const size_t MAX_COMMAND = 254;
    const char* const AXIS_SETTINGS[WolfDrive::AXIS_ROLE_COUNT] =
    {
        "WolfDriveSteerAxis", "WolfDriveThrottleAxis", "WolfDriveBrakeAxis", "WolfDriveClutchAxis",
    };
    const char* const AXIS_INVERT[WolfDrive::AXIS_ROLE_COUNT] =
    {
        "WolfDriveSteerInvert", "WolfDriveThrottleInvert", "WolfDriveBrakeInvert", "WolfDriveClutchInvert",
    };
    const char* const AXIS_FULL[WolfDrive::AXIS_ROLE_COUNT] =
    {
        "", "WolfDriveThrottleFullRange", "WolfDriveBrakeFullRange", "WolfDriveClutchFullRange",
    };

    const char* const WARNING_NAMES[] =
    {
        "engine", "oil", "battery", "brake", "abs", "temp", "tyre", "airbag", "seatbelt", "fuel",
        "adblue", "air", "door", "esp",
    };
}

/**
 * Source: wolfgame.cpp WolfGamePushHandler (itself after wolfregionweather.cpp) - registered with
 * gGenericDispatcher; WolfDrive checks the grid, the region and the seat.
 */
class WolfDashboardPushHandler : public LLDispatchHandler
{
public:
    bool operator()(const LLDispatcher*, const std::string&, const LLUUID& invoice,
                    const sparam_t& strings) override
    {
        WolfDrive::instance().receiveDashboard(invoice, strings);
        return true;
    }
};

namespace
{
    WolfDashboardPushHandler sDashboardHandler;
}

// static
const WolfDrive::Spec& WolfDrive::spec(EType t)
{
    return SPECS[llclamp((S32)t, 0, (S32)TYPE_COUNT - 1)];
}

// static
const char* WolfDrive::actionSetting(S32 action)
{
    return ACTION_SETTINGS[llclamp(action, 0, ACTION_COUNT - 1)];
}

// static
const char* WolfDrive::actionLabel(S32 action)
{
    return ACTION_LABELS[llclamp(action, 0, ACTION_COUNT - 1)];
}

// static
const char* WolfDrive::actionId(S32 action)
{
    return ACTION_IDS[llclamp(action, 0, ACTION_COUNT - 1)];
}

// static
const char* WolfDrive::outLabel(S32 out)
{
    return OUT_LABELS[llclamp(out, 0, OUT_COUNT - 1)];
}

// static
WolfDrive::ActionCfg WolfDrive::actionDefault(S32 action)
{
    ActionCfg c;
    action = llclamp(action, 0, ACTION_COUNT - 1);
    c.mKey = ACTION_DEFAULT_KEYS[action];
    // gear up / down: the Page Up / Page Down a gearbox script listens for (the old lever's taps)
    c.mOut = action == ACT_GEAR_UP ? OUT_PAGE_UP : action == ACT_GEAR_DOWN ? OUT_PAGE_DOWN : OUT_NONE;
    return c;
}

/** The action as the driver set it. Read from WolfDriveActions once and kept (the dashboard asks
 *  for every lamp's key hint each frame); setActionCfg / resetActions read it again. */
WolfDrive::ActionCfg WolfDrive::actionCfg(S32 action) const
{
    action = llclamp(action, 0, ACTION_COUNT - 1);
    if (!mActionCacheValid)
    {
        const LLSD all = gSavedSettings.getLLSD("WolfDriveActions");
        for (S32 i = 0; i < ACTION_COUNT; ++i)
        {
            ActionCfg c = actionDefault(i);
            if (all.isMap() && all.has(actionId(i)) && all[actionId(i)].isMap())
            {
                const LLSD& a = all[actionId(i)];
                if (a.has("key")) c.mKey = a["key"].asString();
                if (a.has("out")) c.mOut = llclamp((S32)a["out"].asInteger(), 0, (S32)OUT_COUNT - 1);
                if (a.has("cmd")) c.mCmd = a["cmd"].asString().substr(0, MAX_COMMAND);
                if (a.has("ch")) c.mChannel = (S32)a["ch"].asInteger();
            }
            mActionCache[i] = c;
        }
        mActionCacheValid = true;
    }
    return mActionCache[action];
}

void WolfDrive::setActionCfg(S32 action, const ActionCfg& cfg)
{
    if (action < 0 || action >= ACTION_COUNT)
    {
        return;
    }
    LLSD all = gSavedSettings.getLLSD("WolfDriveActions");
    if (!all.isMap()) all = LLSD::emptyMap();
    LLSD a = LLSD::emptyMap();
    a["key"] = cfg.mKey;
    a["out"] = (LLSD::Integer)llclamp(cfg.mOut, 0, (S32)OUT_COUNT - 1);
    a["cmd"] = cfg.mCmd.substr(0, MAX_COMMAND);
    a["ch"] = (LLSD::Integer)cfg.mChannel;
    all[actionId(action)] = a;
    gSavedSettings.setLLSD("WolfDriveActions", all);
    mActionCacheValid = false;
    mActionKeysDirty = true;   // read the keys again
}

void WolfDrive::resetActions()
{
    gSavedSettings.setLLSD("WolfDriveActions", LLSD::emptyMap());
    for (S32 a = 0; a < ACTION_COUNT; ++a)
    {
        gSavedSettings.setS32(ACTION_SETTINGS[a], -1);   // and no controller buttons
    }
    mActionCacheValid = false;
    mActionKeysDirty = true;
}

void WolfDrive::pressAction(S32 action)
{
    if (onWolf() && action >= 0 && action < ACTION_COUNT)   // [WOLF ONLY] nothing latches off Wolf
    {
        mScreenLatched |= (1u << action);   // through the same edge as every other source
    }
}

// static
const char* WolfDrive::axisSetting(S32 role)
{
    return AXIS_SETTINGS[llclamp(role, 0, AXIS_ROLE_COUNT - 1)];
}

// static
const char* WolfDrive::axisInvertSetting(S32 role)
{
    return AXIS_INVERT[llclamp(role, 0, AXIS_ROLE_COUNT - 1)];
}

// static
const char* WolfDrive::axisFullRangeSetting(S32 role)
{
    return AXIS_FULL[llclamp(role, 0, AXIS_ROLE_COUNT - 1)];
}

// static
bool WolfDrive::onWolf()
{
    return WolfGrid::isOnWolfTerritories();
}

// static
// Sending to the region (hello, "in", horn, the vehicle-name lookup): only once the region the
// agent is in has itself said it is Wolf (wolfgrid.h isOnWolfTerritoriesConfirmed). onWolf() is
// the lenient one for showing UI, which keeps the previous region's answer across a crossing.
bool WolfDrive::canSend()
{
    return WolfGrid::isOnWolfTerritoriesConfirmed();
}

WolfDrive::WolfDrive()
{
    if (!gGenericDispatcher.isHandlerPresent(MESSAGE_FROM_SIM))
    {
        gGenericDispatcher.addHandler(MESSAGE_FROM_SIM, &sDashboardHandler);
    }
    // World > Car Dashboard and the toolbar button flip this; the dashboard's close button too.
    if (LLControlVariable* c = gSavedSettings.getControl("WolfDashboardMode"))
    {
        c->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&)
        {
            WolfDrive::instance().requestDashboard(v.asBoolean());
        });
    }
    mType = (EType)llclamp(gSavedSettings.getS32("WolfDashboardType"), 0, (S32)TYPE_COUNT - 1);
    mOdoKm = llmax(0.0, (F64)gSavedSettings.getF32("WolfDashboardOdometerKm"));
}

WolfDrive::~WolfDrive()
{
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the controller
// ════════════════════════════════════════════════════════════════════════════════════════

bool WolfDrive::controllerReady() const
{
    static LLCachedControl<bool> enabled(gSavedSettings, "JoystickEnabled", false);
    return enabled && LLViewerJoystick::instanceExists() && LLViewerJoystick::getInstance()->isJoystickInitialized();
}

F32 WolfDrive::mapAxis(S32 role, F32 raw) const
{
    static LLCachedControl<F32> dz_setting(gSavedSettings, "WolfDriveDeadZone", 0.1f);
    const F32 dz = llclamp((F32)dz_setting, 0.f, 0.9f);
    const bool invert = gSavedSettings.getBOOL(AXIS_INVERT[role]);
    F32 v = invert ? -raw : raw;
    if (role == AXIS_STEER)
    {
        return deadZone(llclamp(v, -1.f, 1.f), dz);
    }
    const bool full = gSavedSettings.getBOOL(AXIS_FULL[role]);
    F32 p = full ? (v + 1.f) * 0.5f : llmax(0.f, v);
    return llmax(0.f, deadZone(llclamp(p, 0.f, 1.f), dz));
}

F32 WolfDrive::hardwareValue(S32 role) const
{
    if (role < 0 || role >= AXIS_ROLE_COUNT || !controllerReady())
    {
        return 0.f;
    }
    LLViewerJoystick* js = LLViewerJoystick::getInstance();
    const U32 count = js->getNumOfJoystickAxes();
    // Combined pedals: one axis, the throttle one way from its centre and the brake the other.
    static LLCachedControl<bool> combined(gSavedSettings, "WolfDriveCombinedPedals", false);
    if (combined && (role == AXIS_THROTTLE || role == AXIS_BRAKE))
    {
        const S32 axis = gSavedSettings.getS32(AXIS_SETTINGS[AXIS_THROTTLE]);
        if (axis < 0 || (U32)axis >= count) return 0.f;
        static LLCachedControl<F32> dz_setting(gSavedSettings, "WolfDriveDeadZone", 0.1f);
        const F32 dz = llclamp((F32)dz_setting, 0.f, 0.9f);
        F32 v = js->getJoystickAxis(axis);
        if (gSavedSettings.getBOOL(AXIS_INVERT[AXIS_THROTTLE])) v = -v;
        v = llclamp(v, -1.f, 1.f);
        return llmax(0.f, deadZone(role == AXIS_THROTTLE ? llmax(0.f, v) : llmax(0.f, -v), dz));
    }
    const S32 axis = gSavedSettings.getS32(AXIS_SETTINGS[role]);
    if (axis < 0 || (U32)axis >= count)
    {
        return 0.f;
    }
    return mapAxis(role, js->getJoystickAxis(axis));
}

bool WolfDrive::hardwareButton(S32 action) const
{
    if (action < 0 || action >= ACTION_COUNT || !controllerReady())
    {
        return false;
    }
    LLViewerJoystick* js = LLViewerJoystick::getInstance();
    const S32 b = gSavedSettings.getS32(ACTION_SETTINGS[action]);
    if (b < 0 || (U32)b >= js->getNumOfJoystickButtons())
    {
        return false;
    }
    return js->getJoystickButton(b) != 0;
}

bool WolfDrive::joystickStep(LLViewerJoystick* js)
{
    static LLCachedControl<bool> use_for_driving(gSavedSettings, "WolfDriveHardware", false);
    // [WOLF ONLY] Paul 2026-10-09: "other grids can't use these interfaces they are only for wolf".
    // Off Wolf the controller is stock: false here and LLViewerJoystick::scanJoystick goes on to its
    // own avatar / flycam movement as if WolfDrive did not exist.
    const bool engaged = onWolf() && use_for_driving && js && js->isJoystickInitialized() && seatRoot() != nullptr;
    if (!engaged)
    {
        if (mHardwareDriving)
        {
            mHardwareDriving = false;
            for (F32& v : mHw) v = 0.f;
            mHwButtons = 0;
        }
        return false;
    }
    mHardwareDriving = true;
    mHwSeen = now();
    for (S32 r = 0; r < AXIS_ROLE_COUNT; ++r)
    {
        mHw[r] = hardwareValue(r);
    }
    U32 held = 0;
    for (S32 a = 0; a < ACTION_COUNT; ++a)
    {
        if (hardwareButton(a)) held |= (1u << a);
    }
    mHwButtons = held;

    // Driving owns the controller: the flycam comes off (it would fight the vehicle's camera)
    // and LLViewerJoystick::scanJoystick skips moveAvatar for this frame.
    if (js->getOverrideCamera())
    {
        js->toggleFlycam();
    }

    // The keyboard's flags, from thresholds: past the dead zone the wheel turns (YAW_POS / NEG,
    // as Left / Right), a pedal pressed is Up (AT_POS) or Down (AT_NEG); the brake wins.
    if (!gAgent.isMovementLocked())
    {
        if (mHw[AXIS_STEER] != 0.f)
        {
            // + is a turn to the right, i.e. negative yaw (the old wheel, LLJoystickAgentTurn).
            gAgent.moveYaw(-mHw[AXIS_STEER]);
        }
        if (mHw[AXIS_BRAKE] > 0.f)
        {
            gAgent.moveAt(-1, false);
        }
        else if (mHw[AXIS_THROTTLE] > 0.f)
        {
            gAgent.moveAt(1, false);
        }
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the keyboard mirror and the on-screen controls
// ════════════════════════════════════════════════════════════════════════════════════════

// Source: wolfvehiclecontrols.cpp WolfVehicle::noteScanKey (removed with the Vehicle tab):
// watches only - every key goes on to its own binding unchanged. Unmodified keys only.
void WolfDrive::noteScanKey(KEY key, MASK mask, bool key_down, bool key_up, bool key_level, bool repeat)
{
    // [WOLF ONLY] off Wolf no key is watched at all (H, L, Z... are stock Firestorm's there)
    if (mask != MASK_NONE || !mSeated || !onWolf())
    {
        return;
    }
    // Down and up in one slow frame is a tap that has already ended.
    const bool held = key_level || (key_down && !key_up);
    const F64 t = now();
    // The driver's action keys (Driving Controls), parsed again only after a change through
    // setActionCfg / resetActions (the Driving Controls floater), so an unknown stored key warns once.
    if (mActionKeysDirty)
    {
        mActionKeysDirty = false;
        for (S32 a = 0; a < ACTION_COUNT; ++a)
        {
            const ActionCfg c = actionCfg(a);
            KEY k = 0;
            mActionKey[a] = (!c.mKey.empty() && LLKeyboard::keyFromString(c.mKey, &k)) ? k : 0;
        }
    }
    for (S32 a = 0; a < ACTION_COUNT; ++a)
    {
        if (mActionKey[a] != 0 && mActionKey[a] == key)
        {
            if (held) mKeyButtons |= (1u << a);
            else mKeyButtons &= ~(1u << a);
            // a tap shorter than a frame (down and up in one scan) still counts once
            if (key_down && !repeat) mScreenLatched |= (1u << a);
            mKeySeen[a] = t;
        }
    }
    if (key == KEY_UP || key == 'W')
    {
        mKbdAccel = held;
        mKbdAccelSeen = t;
    }
    else if (key == KEY_DOWN || key == 'S')
    {
        mKbdBrake = held;
        mKbdBrakeSeen = t;
    }
    else if (key == KEY_LEFT || key == 'A')
    {
        mKbdLeft = held;
        mKbdSteerSeen = t;
    }
    else if (key == KEY_RIGHT || key == 'D')
    {
        mKbdRight = held;
        mKbdSteerSeen = t;
    }
    else if ((key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && key_down && !repeat)
    {
        // The key itself already goes to the vehicle (CONTROL_UP / CONTROL_DOWN): only the
        // picture follows it here, no tap of our own.
        shiftPicture(key == KEY_PAGE_UP ? 1 : -1);
    }
}

void WolfDrive::setScreenSteer(F32 v, bool held)
{
    held = held && onWolf();   // [WOLF ONLY] nothing is held off Wolf
    mScreenSteerHeld = held;
    mScreenSteer = held ? llclamp(v, -1.f, 1.f) : 0.f;
}

void WolfDrive::setScreenPedal(bool brake, bool held)
{
    (brake ? mScreenBrake : mScreenAccel) = held && onWolf();   // [WOLF ONLY]
}

void WolfDrive::setScreenButton(U32 button, bool held)
{
    if (held && !onWolf())
    {
        return;   // [WOLF ONLY] nothing latches off Wolf
    }
    if (held)
    {
        mScreenButtons |= button;
        mScreenLatched |= button;   // a click shorter than a frame still counts once
    }
    else
    {
        mScreenButtons &= ~button;
    }
}

// static
std::vector<S32> WolfDrive::selectorPositions(EType t, bool auto_box)
{
    if (t == TYPE_BIKE) return gearSequence(t, false);
    // AUTO: the automatic's own lever (car P R N D with paddles for M; truck Opticruise R N A).
    if (auto_box) return gearSequence(t, true);
    // MANUAL: R, N and one slot for the forward gears (GEAR_D stands for it; the dashboard writes
    // the gear in it) - 8 and 12 gears would not fit a lever.
    return { GEAR_R, GEAR_N, GEAR_D };
}

// static
/** Every lever position in the order Page Up / Page Down step through them. MANUAL (the default
 *  for car and truck, always for the bike) - Paul 2026-10-09: "the car needs
 *  proper gears not automatic ... page up it goes up a gear page down down a gear (if its more
 *  page ups than they have gears we just sit in top gear)". Car and truck R, N, 1..mGears (down
 *  from 1 is N, then R); the bike's sequential box as it is, 1-N-2-3-4-5-6. */
std::vector<S32> WolfDrive::gearSequence(EType t, bool auto_box)
{
    if (t == TYPE_BIKE) return { 1, GEAR_N, 2, 3, 4, 5, 6 };
    if (auto_box)
    {
        // Paul 2026-10-09: "on the lorry and car make it so you can select automatic or manual".
        return t == TYPE_TRUCK ? std::vector<S32>{ GEAR_R, GEAR_N, GEAR_D }               // Opticruise: R N A
                               : std::vector<S32>{ GEAR_P, GEAR_R, GEAR_N, GEAR_D };       // P R N D
    }
    std::vector<S32> seq = { GEAR_R, GEAR_N };
    for (S32 g = 1; g <= spec(t).mGears; ++g) seq.push_back(g);
    return seq;
}

// static
std::string WolfDrive::gearLabel(S32 selector)
{
    switch (selector)
    {
    case GEAR_P: return "P";
    case GEAR_R: return "R";
    case GEAR_N: return "N";
    case GEAR_D: return "D";
    default:     return llformat("%d", selector);
    }
}

void WolfDrive::queueTaps(S32 steps)
{
    if (steps == 0 || !mSeated)
    {
        return;
    }
    const S32 dir = steps > 0 ? 1 : -1;
    for (S32 n = 0; n < llabs(steps); ++n)
    {
        mTapQueue.push_back(dir);
    }
}

// Source: wolfvehiclecontrols.cpp tapIdle - one PageUp / PageDown press per queued step, held
// SHIFT_TAP_SECONDS then a gap as long, so the script sees each as its own CONTROL_UP / DOWN.
void WolfDrive::tapStep()
{
    if (!mSeated)
    {
        mTapQueue.clear();
        mTapDir = 0;
        return;
    }
    const F64 t = now();
    if (mTapDir != 0)
    {
        if (t - mTapPhaseStart < SHIFT_TAP_SECONDS)
        {
            gAgent.moveUp(mTapDir);   // level-held, like agent_jump / agent_push_down
            return;
        }
        mTapDir = 0;
        mTapPhaseStart = t;
        return;
    }
    if (mTapQueue.empty() || t - mTapPhaseStart < SHIFT_TAP_SECONDS)
    {
        return;
    }
    mTapDir = mTapQueue.front();
    mTapQueue.erase(mTapQueue.begin());
    mTapPhaseStart = t;
    gAgent.moveUp(mTapDir);
}

/** A click on the lever: the gear clicked (the car / truck forward slot is first gear, or stays
 *  in the gear already held), reached by one Page Up / Page Down tap per step of gearSequence -
 *  the same steps the keys take, so the picture and the vehicle's script stay together. */
void WolfDrive::setSelector(S32 g)
{
    const bool ab = autoBox();
    if (g == GEAR_D && mType != TYPE_BIKE && !ab)
    {
        g = mSelector >= 1 ? mSelector : 1;
    }
    if (g == GEAR_D && ab)
    {
        mManualGear = 0;   // the lever back in D / A: the automatic chooses again
    }
    const std::vector<S32> seq = gearSequence(mType, ab);
    auto to = std::find(seq.begin(), seq.end(), g);
    auto from = std::find(seq.begin(), seq.end(), mSelector);
    if (to == seq.end() || from == seq.end() || to == from)
    {
        return;
    }
    mSelector = g;
    queueTaps((S32)(to - from));
}

/** The picture one gear up (+1) or down (-1) along gearSequence, as each Page Up / Page Down (the
 *  key itself, the gear keys, a controller button, the paddles) does. Past the top it stays in top
 *  gear, past the bottom in R (the bike's 1). What reaches the vehicle is the press's own. */
void WolfDrive::shiftPicture(S32 dir)
{
    dir = dir > 0 ? 1 : -1;
    if (autoBox())
    {
        // AUTO: in D / A a press takes a gear yourself (M), one up or down from the automatic's;
        // in P, R or N the picture stays (the press still goes to the vehicle).
        if (mSelector != GEAR_D)
        {
            return;
        }
        const S32 cur = mManualGear > 0 ? mManualGear : llmax(1, mModelGear);
        mManualGear = llclamp(cur + dir, 1, spec(mType).mGears);
        return;
    }
    const std::vector<S32> seq = gearSequence(mType, false);
    auto it = std::find(seq.begin(), seq.end(), mSelector);
    const S32 n = (S32)seq.size();
    const S32 i = it == seq.end() ? (S32)(std::find(seq.begin(), seq.end(), GEAR_N) - seq.begin()) : (S32)(it - seq.begin());
    mSelector = seq[llclamp(i + dir, 0, n - 1)];
}

/** One press of an action, from any source: the dashboard's lamp / picture, then what the driver
 *  set it to send the vehicle (a Page Up / Page Down tap, or a chat command on a channel - the
 *  flight deck's GEAR / ENGINE commands, wolfflight.cpp sendGearCommand). */
void WolfDrive::doAction(S32 action)
{
    if (!onWolf())
    {
        return;   // [WOLF ONLY] nothing at all off Wolf: no lamp, no command, no message
    }
    switch (action)
    {
    case ACT_GEAR_UP:   shiftPicture(1); break;
    case ACT_GEAR_DOWN: shiftPicture(-1); break;
    case ACT_LIGHTS:    mOwnLights = (mOwnLights + 1) % 3; break;
    case ACT_IND_LEFT:  mOwnIndicators = mOwnIndicators == 1 ? 0 : 1; break;
    case ACT_IND_RIGHT: mOwnIndicators = mOwnIndicators == 2 ? 0 : 2; break;
    case ACT_HAZARDS:   mOwnIndicators = mOwnIndicators == 3 ? 0 : 3; break;
    case ACT_HANDBRAKE: mOwnHandbrake = !mOwnHandbrake; break;
    case ACT_CRUISE:    mOwnCruise = !mOwnCruise; break;
    default: break;   // the horn: the script makes the sound
    }
    if (!mSeated)
    {
        return;   // nothing to send: the commands are for the vehicle sat on
    }
    const ActionCfg c = actionCfg(action);
    if (action == ACT_HORN && c.mOut == OUT_NONE)
    {
        // No horn of their own set: the built-in horn for this design, played by the region from
        // the vehicle so everyone near hears it (WolfDriveModule.cs Horn). The region allows one
        // every 2 s or so.
        if (canSend() && gAgent.getRegion())
        {
            static const char* const KIND[TYPE_COUNT] = { "car", "truck", "bike" };
            send_generic_message(METHOD_TO_SIM, { "horn", KIND[llclamp((S32)mType, 0, (S32)TYPE_COUNT - 1)] });
        }
        return;
    }
    switch (c.mOut)
    {
    case OUT_PAGE_UP:   queueTaps(1); break;
    case OUT_PAGE_DOWN: queueTaps(-1); break;
    case OUT_CHAT:
        if (!c.mCmd.empty())
        {
            // The command as chat to the vehicle's script on its channel (0 = said aloud).
            FSNearbyChat::sendChatFromViewerFinal(c.mCmd, c.mCmd, CHAT_TYPE_NORMAL, false, c.mChannel);
        }
        break;
    default: break;
    }
}

void WolfDrive::edgeButtons(U32 held)
{
    const U32 pressed = held & ~mPrevButtons;
    mPrevButtons = held;
    for (S32 a = 0; a < ACTION_COUNT; ++a)
    {
        if (pressed & (1u << a)) doAction(a);
    }
}

void WolfDrive::combine(F32 dt)
{
    const F64 t = now();
    if (mHardwareDriving && t - mHwSeen > HW_STALE_SECONDS)
    {
        // Focus went away (scanJoystick stops before our hook): nothing stays pressed.
        mHardwareDriving = false;
        for (F32& v : mHw) v = 0.f;
        mHwButtons = 0;
    }
    const bool kbd_accel = mKbdAccel && t - mKbdAccelSeen < KEY_STALE_SECONDS;
    const bool kbd_brake = mKbdBrake && t - mKbdBrakeSeen < KEY_STALE_SECONDS;
    S32 kbd_steer = 0;
    if (t - mKbdSteerSeen < KEY_STALE_SECONDS)
    {
        kbd_steer = (mKbdRight ? 1 : 0) - (mKbdLeft ? 1 : 0);
    }

    if (mScreenSteerHeld)       mSteer = mScreenSteer;
    else if (mHw[AXIS_STEER] != 0.f) mSteer = mHw[AXIS_STEER];
    else if (kbd_steer != 0)    mSteer = (F32)kbd_steer;
    else                        mSteer = 0.f;

    mThrottle = llmax(mHw[AXIS_THROTTLE], (mScreenAccel || kbd_accel) ? 1.f : 0.f);
    mBrake = llmax(mHw[AXIS_BRAKE], (mScreenBrake || kbd_brake) ? 1.f : 0.f);
    mClutch = mHw[AXIS_CLUTCH];
    U32 keys = 0;
    for (S32 a = 0; a < ACTION_COUNT; ++a)
    {
        // a held key is scanned every frame; a lost key-up clears after KEY_STALE_SECONDS
        if ((mKeyButtons & (1u << a)) && t - mKeySeen[a] < KEY_STALE_SECONDS) keys |= (1u << a);
    }
    mButtons = (mHwButtons | mScreenButtons | mScreenLatched | keys) & BUTTON_MASK;
    mScreenLatched = 0;
    edgeButtons(mButtons);
}

// The on-screen pedals and wheel drive through the same LLAgent calls the old Vehicle tab used
// (wolfvehiclecontrols.cpp driveIdle / WolfSteeringWheel::onHeldDown).
void WolfDrive::applyControlFlags()
{
    if (!mSeated || gAgent.isMovementLocked())
    {
        mScreenPedalDir = 0;
        return;
    }
    const F64 t = now();
    const S32 dir = mScreenBrake ? -1 : mScreenAccel ? 1 : 0;
    if (dir != mScreenPedalDir)
    {
        mScreenPedalDir = dir;
        mScreenPedalSince = t;
    }
    if (dir != 0)
    {
        // Source: llviewerinput.cpp agent_push_forwardbackward - a nudge first, then full.
        if (t - mScreenPedalSince < NUDGE_TIME) gAgent.moveAtNudge(dir);
        else gAgent.moveAt(dir);
    }
    if (mScreenSteerHeld)
    {
        const F32 axis = deadZone(mScreenSteer, SCREEN_DEAD_ZONE);
        if (axis != 0.f)
        {
            gAgent.moveYaw(-axis);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// to WolfSim
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::sendAnalogue()
{
    const bool active = mSeated && gAgent.getRegion() && canSend() && (mHardwareDriving || mDashOn);
    if (!active)
    {
        if (mWasSending && mSeated && gAgent.getRegion() && canSend())
        {
            // Let go: one last message with nothing pressed, so a script stops at once.
            send_generic_message(METHOD_TO_SIM, { "in", "0.000", "0.000", "0.000", "0.000",
                                                  llformat("%d", mSelector), "0" });
        }
        mWasSending = false;
        mLastSentKey.clear();
        return;
    }
    S32 gear = mSelector;   // R, N or the gear held (gearSequence)
    if (mSelector == GEAR_D && mManualGear > 0) gear = llmin(mManualGear, spec(mType).mGears);   // AUTO's M: the gear held
    const std::vector<std::string> params =
    {
        "in",
        llformat("%.3f", llclamp(mSteer, -1.f, 1.f)),
        llformat("%.3f", llclamp(mThrottle, 0.f, 1.f)),
        llformat("%.3f", llclamp(mBrake, 0.f, 1.f)),
        llformat("%.3f", llclamp(mClutch, 0.f, 1.f)),
        llformat("%d", gear),
        llformat("%u", mButtons & BUTTON_MASK),
    };
    std::string key;
    for (const std::string& p : params) key += p + "|";
    const F64 t = now();
    const bool changed = key != mLastSentKey;
    if ((changed && t - mLastSent >= SEND_MIN_GAP) || t - mLastSent >= SEND_REFRESH)
    {
        // Source: llviewergenericmessage.cpp send_generic_message - each parameter goes out with
        // its NUL, which WolfDriveModule.ParseNumber strips.
        send_generic_message(METHOD_TO_SIM, params);
        mLastSent = t;
        mLastSentKey = key;
        mWasSending = true;
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// from WolfSim: wolfDashboard
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::receiveDashboard(const LLUUID& invoice, const std::vector<std::string>& strings)
{
    // Wolf Territories only, from the region the agent is in, while sitting on something.
    if (!onWolf()) return;
    LLViewerRegion* rgn = gAgent.getRegion();
    if (!rgn || rgn->getRegionID() != invoice) return;
    LLViewerObject* root = seatRoot();
    if (!root) return;
    if (mScript.mVehicle != root->getID())
    {
        mScript = Script();
        mScript.mVehicle = root->getID();
    }
    if (strings.size() > 64) return;

    // The same keys and ranges as WolfDriveModule.cs DASH_KEYS; never trust the wire.
    for (const std::string& s : strings)
    {
        if (s.size() > 254) continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        F64 n = 0.0;
        const bool num = parseNumber(v, n);
        if (k == "type")
        {
            if (v == "car") { mScript.mHasType = true; mScript.mType = TYPE_CAR; }
            else if (v == "truck") { mScript.mHasType = true; mScript.mType = TYPE_TRUCK; }
            else if (v == "bike") { mScript.mHasType = true; mScript.mType = TYPE_BIKE; }
        }
        else if (k == "units")
        {
            if (v == "kmh" || v == "mph") { mScript.mHasUnits = true; mScript.mMph = v == "mph"; }
        }
        else if (k == "gear")
        {
            static const char* ok[] = { "P", "R", "N", "D", "S", "C", "R1", "R2", "R3", "R4" };
            bool good = false;
            for (const char* o : ok) good = good || v == o;
            if (!good && num && n >= 1 && n <= 18 && v.find('.') == std::string::npos && v[0] != '-') good = true;
            if (good) { mScript.mHasGear = true; mScript.mGear = v; }
        }
        else if (!num) continue;
        else if (k == "rpm")        { mScript.mHasRpm = true; mScript.mRpm = (F32)llclamp(n, 0.0, 30000.0); }
        else if (k == "max_rpm")    { mScript.mHasMaxRpm = true; mScript.mMaxRpm = (F32)llclamp(n, 1000.0, 30000.0); }
        else if (k == "redline")    { mScript.mHasRedline = true; mScript.mRedline = (F32)llclamp(n, 500.0, 30000.0); }
        else if (k == "max_speed")  { mScript.mHasMaxSpeed = true; mScript.mMaxSpeed = (F32)llclamp(n, 10.0, 1000.0); }
        else if (k == "fuel")       { mScript.mHasFuel = true; mScript.mFuel = (F32)llclamp(n, 0.0, 100.0); }
        else if (k == "adblue")     { mScript.mHasAdBlue = true; mScript.mAdBlue = (F32)llclamp(n, 0.0, 100.0); }
        else if (k == "temp")       { mScript.mHasTemp = true; mScript.mTemp = (F32)llclamp(n, -40.0, 150.0); }
        else if (k == "air1")       { mScript.mHasAir1 = true; mScript.mAir1 = (F32)llclamp(n, 0.0, 12.0); }
        else if (k == "air2")       { mScript.mHasAir2 = true; mScript.mAir2 = (F32)llclamp(n, 0.0, 12.0); }
        else if (k == "indicators") { mScript.mHasInd = true; mScript.mInd = (S32)llclamp(n, 0.0, 3.0); }
        else if (k == "lights")     { mScript.mHasLights = true; mScript.mLights = (S32)llclamp(n, 0.0, 2.0); }
        else if (k == "handbrake")  { mScript.mHasHandbrake = true; mScript.mHandbrake = n >= 0.5; }
        else if (k == "retarder")   { mScript.mHasRetarder = true; mScript.mRetarder = (S32)llclamp(n, 0.0, 5.0); }
        else if (k == "cruise")     { mScript.mHasCruise = true; mScript.mCruise = n >= 0.5; }
        else if (k == "drive_time") { mScript.mHasDriveTime = true; mScript.mDriveTime = (F32)llclamp(n, 0.0, 86400.0); }
        else if (k == "odo")        { mScript.mHasOdo = true; mScript.mOdo = llclamp(n, 0.0, 9999999.0); }
        else if (k == "clear" && n >= 0.5)
        {
            const LLUUID keep = mScript.mVehicle;
            mScript = Script();
            mScript.mVehicle = keep;
        }
    }
    for (const std::string& s : strings)
    {
        if (s.compare(0, 9, "warnings=") != 0 || s.size() > 254) continue;
        mScript.mHasWarnings = true;
        mScript.mWarnings.clear();
        const std::string list = s.substr(9);
        size_t a = 0;
        while (a <= list.size() && mScript.mWarnings.size() < 14)
        {
            size_t b = list.find(',', a);
            if (b == std::string::npos) b = list.size();
            const std::string w = list.substr(a, b - a);
            for (const char* name : WARNING_NAMES)
            {
                if (w == name)
                {
                    mScript.mWarnings.push_back(w);
                    break;
                }
            }
            a = b + 1;
        }
    }
    mScript.mAt = now();

    // A script driving the dashboard is the strongest sign this is a vehicle: open it once per
    // seat unless the driver closed it (World > Car Dashboard brings it back).
    if (!mDashOn && !mAutoDone && gSavedSettings.getBOOL("WolfDashboardAuto") && !WolfFlight::instance().active())
    {
        mAutoDone = true;
        mAutoOpened = true;
        requestDashboard(true);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the dashboard: on / off, the design, the Wolf gate
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::requestDashboard(bool on)
{
    if (mSyncing)
    {
        return;
    }
    if (on && !onWolf())
    {
        on = false;   // [WOLF ONLY] no dashboard and nothing said about one off Wolf
    }
    if (on && WolfFlight::instance().active())
    {
        // One panel across the bottom at a time: Flight / Sailing Mode gives way.
        gSavedSettings.setBOOL(WolfFlight::instance().sailing() ? "WolfSailMode" : "WolfFlightMode", false);
    }
    if (!on)
    {
        mAutoOpened = false;
        mAutoDone = true;             // closed by hand (or by the gate): not reopened for this seat
        mScreenAccel = mScreenBrake = false;
        mScreenSteerHeld = false;
        mScreenSteer = 0.f;
        mScreenButtons = 0;
    }
    mDashOn = on;
    mSyncing = true;
    if (gSavedSettings.getBOOL("WolfDashboardMode") != on)
    {
        gSavedSettings.setBOOL("WolfDashboardMode", on);
    }
    mSyncing = false;
}

void WolfDrive::chooseType(EType t)
{
    t = (EType)llclamp((S32)t, 0, (S32)TYPE_COUNT - 1);
    mType = t;
    gSavedSettings.setS32("WolfDashboardType", (S32)t);
    if (mNameKnown && !mVehicleName.empty())
    {
        LLSD map = gSavedPerAccountSettings.getLLSD("WolfDashboardVehicleTypes");
        if (!map.isMap()) map = LLSD::emptyMap();
        if (map.has(mVehicleName) || map.size() < (size_t)MAX_REMEMBERED_VEHICLES)
        {
            map[mVehicleName] = (LLSD::Integer)t;
            gSavedPerAccountSettings.setLLSD("WolfDashboardVehicleTypes", map);
        }
    }
    // The gears are the new design's: a gear it does not have is neutral.
    const std::vector<S32> seq = gearSequence(mType, autoBox());
    if (std::find(seq.begin(), seq.end(), mSelector) == seq.end())
    {
        mSelector = GEAR_N;
    }
    mManualGear = 0;
}

// static
const char* WolfDrive::autoBoxSetting(EType t)
{
    return t == TYPE_TRUCK ? "WolfDashboardAutoTruck" : "WolfDashboardAutoCar";
}

bool WolfDrive::autoBox() const
{
    return mType != TYPE_BIKE && gSavedSettings.getBOOL(autoBoxSetting(mType));
}

/** The dashboard's AUTO / MANUAL switch (car and truck; the bike is always manual). Remembered for
 *  the design. The picture starts again in N; nothing is sent - the vehicle's script keeps its own
 *  gear, and Page Up / Page Down go to it in both modes. */
void WolfDrive::setAutoBox(bool on)
{
    if (mType == TYPE_BIKE || on == autoBox())
    {
        return;
    }
    gSavedSettings.setBOOL(autoBoxSetting(mType), on);
    mSelector = GEAR_N;
    mManualGear = 0;
    mModelGear = 1;
}

void WolfDrive::loadTypeFor(const std::string& name)
{
    LLSD map = gSavedPerAccountSettings.getLLSD("WolfDashboardVehicleTypes");
    if (map.isMap() && map.has(name))
    {
        mType = (EType)llclamp((S32)map[name].asInteger(), 0, (S32)TYPE_COUNT - 1);
    }
    else
    {
        mType = (EType)llclamp(gSavedSettings.getS32("WolfDashboardType"), 0, (S32)TYPE_COUNT - 1);
    }
    mSelector = GEAR_N;   // a new vehicle starts in neutral
    mManualGear = 0;
}

void WolfDrive::resetTrip()
{
    mTripKm = 0.0;
    mMaxLeanL = mMaxLeanR = 0.f;
}

/** [WOLF ONLY] Off Wolf nothing is held, sat on or remembered for the region: keys, controller,
 *  screen controls, queued taps and the script's values are dropped, and the seat is forgotten so
 *  that back on Wolf watchSeat finds it again and says "hello" to that region afresh. The saved
 *  settings (keys, axes, design, odometer) are not touched. */
void WolfDrive::goInert()
{
    if (mSeatRoot.notNull())
    {
        gSavedSettings.setF32("WolfDashboardOdometerKm", (F32)mOdoKm);
        if (!mNameKnown)
        {
            WolfObjectProps::instance().unwant(mSeatRoot);   // a name request not yet sent stays unsent
        }
    }
    mSeatRoot.setNull();
    mSeated = false;
    mScript = Script();
    mAutoOpened = false;
    mAutoDone = false;
    mHelloSeat.setNull();
    mHelloRegion.setNull();
    mWasSending = false;
    mKbdAccel = mKbdBrake = mKbdLeft = mKbdRight = false;
    mKeyButtons = 0;
    mScreenAccel = mScreenBrake = false;
    mScreenSteerHeld = false;
    mScreenSteer = 0.f;
    mScreenButtons = 0;
    mScreenLatched = 0;
    mHardwareDriving = false;
    for (F32& v : mHw) v = 0.f;
    mHwButtons = 0;
    mButtons = 0;
    mPrevButtons = 0;
    mTapQueue.clear();
    mTapDir = 0;
}

/**
 * [WOLF GRID GATE] Source: wolfgame.cpp WolfGame::gateToolbar - a toolbar lays out hidden buttons
 * too, so off Wolf the wolf_dashboard button is REMOVED (where it was is kept in a per-account
 * setting) and put back in the same place on the way back.
 */
void WolfDrive::refreshDashGate()
{
    const bool wolf = onWolf();
    if ((S32)wolf != mOnWolf)
    {
        const bool was_known = mOnWolf >= 0;
        mOnWolf = wolf ? 1 : 0;
        if (wolf && was_known && mReopenOnWolf)
        {
            // back home: the dashboard that was up when Wolf was left comes back
            mReopenOnWolf = false;
            if (!WolfFlight::instance().active()) requestDashboard(true);   // the flight deck keeps the bottom
        }
        if (!wolf)
        {
            // [WOLF ONLY] left Wolf (hypergrid, or a login elsewhere): everything shuts. The saved
            // settings are left as they are, so it all comes back on the way home.
            mReopenOnWolf = mReopenOnWolf || mDashOn;
            if (mDashOn)
            {
                mAutoOpened = false;
                requestDashboard(false);
            }
            LLFloaterReg::hideInstance("wolf_drive_controls");
            goInert();
        }
    }
    if (LLStartUp::getStartupState() < STATE_STARTED || !gToolBarView) return;
    const char* name = "wolf_dashboard";
    if (!wolf)
    {
        // every frame (one lookup): a saved button never shows off Wolf, even for a moment
        if (gToolBarView->hasCommand(LLCommandId(name)) == LLToolBarEnums::TOOLBAR_NONE) return;
        LLSD hidden = gSavedPerAccountSettings.getLLSD("WolfDashboardToolbarHidden");
        if (!hidden.isMap()) hidden = LLSD::emptyMap();
        int rank = LLToolBar::RANK_NONE;
        const S32 where = gToolBarView->removeCommand(LLCommandId(name), rank);
        if (where != LLToolBarEnums::TOOLBAR_NONE)
        {
            hidden["toolbar"] = (LLSD::Integer)where;
            hidden["rank"] = (LLSD::Integer)rank;
            gSavedPerAccountSettings.setLLSD("WolfDashboardToolbarHidden", hidden);
        }
        return;
    }
    if (now() - mLastGate < 1.0 || !gToolBarView->getVisible()) return;
    mLastGate = now();
    LLSD hidden = gSavedPerAccountSettings.getLLSD("WolfDashboardToolbarHidden");
    if (!hidden.isMap()) hidden = LLSD::emptyMap();
    if (!hidden.has("toolbar")) return;
    const S32 where = hidden["toolbar"].asInteger();
    if (where >= LLToolBarEnums::TOOLBAR_FIRST && where <= LLToolBarEnums::TOOLBAR_LAST
        && gToolBarView->hasCommand(LLCommandId(name)) == LLToolBarEnums::TOOLBAR_NONE)
    {
        gToolBarView->addCommand(LLCommandId(name), (LLToolBarEnums::EToolBarLocation)where, hidden["rank"].asInteger());
    }
    gSavedPerAccountSettings.setLLSD("WolfDashboardToolbarHidden", LLSD::emptyMap());
}

// ════════════════════════════════════════════════════════════════════════════════════════
// the seat
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::watchSeat()
{
    LLViewerObject* root = seatRoot();
    const LLUUID id = root ? root->getID() : LLUUID::null;
    if (id != mSeatRoot)
    {
        // Stood up, or sat on something else.
        if (mSeatRoot.notNull())
        {
            gSavedSettings.setF32("WolfDashboardOdometerKm", (F32)mOdoKm);
            if (mAutoOpened && mDashOn)
            {
                requestDashboard(false);
            }
        }
        mSeatRoot = id;
        mSeated = root != nullptr;
        // The vehicle's first wolfDashboard can arrive before this frame notices the new seat
        // (receiveDashboard keys it by the same root): keep it, and the auto-open it caused.
        if (id.isNull() || mScript.mVehicle != id)
        {
            mScript = Script();
            mAutoOpened = false;
            mAutoDone = false;
        }
        mVehicleName.clear();
        mNameKnown = false;
        mTapQueue.clear();
        mTapDir = 0;
        mModelRpm = 0.f;
        mFuelUsedKm = 0.f;
        mDriving = 0.f;
        mOwnIndicators = 0;
        mOwnLights = 0;
        mOwnHandbrake = false;
        mOwnCruise = false;
        mPrevButtons = 0;
        resetTrip();
        if (!mSeated)
        {
            mKbdAccel = mKbdBrake = mKbdLeft = mKbdRight = false;
        }
        mSelector = GEAR_N;   // sat on a vehicle: in neutral, as Paul asked
        mManualGear = 0;
        mModelGear = 1;
    }
    if (!root)
    {
        return;
    }
    // Wolf Territories: tell the region this viewer shows dashboards, once per seat and region
    // (WolfDriveModule.cs sends "WolfDashboard" only to viewers that said so, so stock viewers never
    // get a method they have no handler for).
    // Waits (not dropped) until this region has confirmed it is Wolf: mHelloSeat / mHelloRegion only
    // change when it goes, so the next frames try again.
    if (canSend() && gAgent.getRegion())
    {
        const LLUUID region = gAgent.getRegion()->getRegionID();
        if (mHelloSeat != id || mHelloRegion != region)
        {
            mHelloSeat = id;
            mHelloRegion = region;
            send_generic_message(METHOD_TO_SIM, { "hello" });
        }
    }
    // The name (for remembering the design per vehicle) is asked of the region only on Wolf
    // Territories: a stock viewer sends no RequestObjectPropertiesFamily on sitting, so neither
    // does this one elsewhere.
    if (!mNameKnown && canSend())
    {
        if (const WolfObjectProps::Props* props = WolfObjectProps::instance().get(root->getID()))
        {
            mVehicleName = props->mName;
            mNameKnown = true;
            loadTypeFor(mVehicleName);
        }
        else
        {
            WolfObjectProps::instance().want(root, false);
        }
    }
    // Sitting on something physical is sitting in a vehicle that is being driven: open the
    // dashboard once per seat (WolfDashboardAuto), unless Flight / Sailing Mode is up or the
    // driver closed it. Scripts usually go physical after the sit, so this keeps looking.
    if (!mDashOn && !mAutoDone && onWolf() && root->flagUsePhysics()
        && gSavedSettings.getBOOL("WolfDashboardAuto") && !WolfFlight::instance().active())
    {
        mAutoDone = true;
        mAutoOpened = true;
        requestDashboard(true);
    }
}

// ════════════════════════════════════════════════════════════════════════════════════════
// THE VIEWER'S OWN MODEL - what the instruments show when the vehicle's script sends nothing.
// Speed is always measured (the root prim's velocity). From it and the published gear ratios,
// final drive and tyre size (top of this file) come the engine speed. MANUAL (the default, and the
// bike always): the gear the driver stepped to. AUTO (car / truck, the dashboard's switch): an automatic (car:
// 8-speed like the 992's PDK, truck: 12-speed automated like Opticruise) shifts on rpm thresholds
// that rise with the throttle; the bike is sequential, the rider picks the gear. The thresholds,
// idle and pull-away speeds are the MODEL's choices, not published figures.
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::model(F32 dt)
{
    LLViewerObject* root = seatRoot();
    Shown s;
    if (!root || !isAgentAvatarValid())
    {
        mShown = s;
        return;
    }
    s.mValid = true;
    s.mScripted = mScript.mVehicle == root->getID() && mScript.mAt > 0.0;
    const Script& sc = mScript;
    const bool use = s.mScripted;

    if (use && sc.mHasType && sc.mType != mType)
    {
        mType = sc.mType;
        const std::vector<S32> seq = gearSequence(mType, autoBox());
        if (std::find(seq.begin(), seq.end(), mSelector) == seq.end())
        {
            mSelector = GEAR_N;
        }
        mManualGear = 0;   // as chooseType: the other design's M gear is not this one's
        mModelGear = 1;
    }
    const Spec& sp = spec(mType);

    const LLVector3 vel = root->getVelocity();
    const F32 speed = sqrtf(vel.mV[VX] * vel.mV[VX] + vel.mV[VY] * vel.mV[VY]);
    s.mSpeed = speed;

    // distance: the trip, the odometer, the model's fuel; driving time while moving
    const F64 km = (F64)speed * dt / 1000.0;
    mTripKm += km;
    mOdoKm += km;
    mFuelUsedKm += (F32)km;
    if (speed > 0.5f) mDriving += dt;

    // engine. MANUAL (the default; the bike always): the gear is the one the driver stepped to with
    // Page Up / Page Down (gearSequence), and the revs follow the wheels through that gear's cited
    // ratio - they climb in a gear, drop on an upshift and reach the limiter if a low gear is
    // over-revved. AUTO (car / truck, chosen on the dashboard): the automatic as before.
    const F32 wheel_rps = speed / llmax(0.1f, sp.mTyreCirc);
    F32 target = sp.mIdle;
    S32 gear_n = 0;
    std::string label;
    bool manual = false;
    if (!autoBox())
    {
        manual = true;
        gear_n = mSelector >= 1 ? llmin(mSelector, sp.mGears) : 0;
        label = gear_n > 0 ? llformat("%d", gear_n) : gearLabel(mSelector);
    }
    else if (mSelector == GEAR_D)
    {
        const F64 t = now();
        if (mManualGear > 0)
        {
            manual = true;
            gear_n = llclamp(mManualGear, 1, sp.mGears);
        }
        else
        {
            // the automatic: shift on engine speed, the thresholds rising with the throttle
            mModelGear = llclamp(mModelGear, 1, sp.mGears);
            const F32 rpm_now = wheel_rps * 60.f * sp.mFinal * sp.mRatios[mModelGear - 1];
            const F32 up = lerp(sp.mUpLight, sp.mUpFull, mThrottle);
            const F32 down = lerp(sp.mDownLight, sp.mDownFull, mThrottle);
            if (t - mShiftAt > sp.mShiftSecs)
            {
                if (rpm_now > up && mModelGear < sp.mGears) { ++mModelGear; mShiftAt = t; }
                else if (rpm_now < down && mModelGear > 1) { --mModelGear; mShiftAt = t; }
            }
            if (mBrake > 0.f && speed < 1.f) mModelGear = 1;
            gear_n = mModelGear;
        }
        // truck: the number (the dashboard draws the GRS905 range and split beside it)
        label = mType == TYPE_TRUCK ? llformat("%d", gear_n) : (manual ? llformat("M%d", gear_n) : std::string("D"));
    }
    else
    {
        label = gearLabel(mSelector);
    }
    s.mAutoBox = autoBox();

    if (gear_n > 0)
    {
        const F32 wheel_rpm = wheel_rps * 60.f * sp.mFinal * sp.mRatios[gear_n - 1];
        if (mClutch > 0.5f)
        {
            // clutch pedal down: the engine revs freely
            target = sp.mIdle + mThrottle * (sp.mRedline * 0.9f - sp.mIdle);
        }
        else
        {
            // pulling away the clutch slips, so the engine sits above the wheels until they catch up
            target = llmax(wheel_rpm, sp.mIdle + mThrottle * (sp.mLaunch - sp.mIdle));
        }
    }
    else if (mSelector == GEAR_R && sp.mReverse > 0.f)
    {
        const F32 wheel_rpm = wheel_rps * 60.f * sp.mFinal * sp.mReverse;
        target = llmax(wheel_rpm, sp.mIdle + mThrottle * (sp.mLaunch - sp.mIdle));
    }
    else
    {
        // P / N: the engine revs freely with the throttle
        target = sp.mIdle + mThrottle * (sp.mRedline * 0.9f - sp.mIdle);
    }
    target = llclamp(target, 0.f, sp.mLimiter);
    mModelRpm += (target - mModelRpm) * llmin(1.f, dt * 8.f);
    if (mModelRpm < 1.f) mModelRpm = sp.mIdle;

    const bool live = use && now() - sc.mAt < SCRIPT_LIVE_SECONDS;
    s.mRpm = live && sc.mHasRpm ? sc.mRpm : mModelRpm;
    s.mMaxRpm = use && sc.mHasMaxRpm ? sc.mMaxRpm : sp.mDialRpm;
    s.mRedline = use && sc.mHasRedline ? llmin(sc.mRedline, s.mMaxRpm) : llmin(sp.mRedline, s.mMaxRpm);
    s.mMph = use && sc.mHasUnits ? sc.mMph : gSavedSettings.getBOOL("WolfDashboardMph");
    s.mMaxSpeed = use && sc.mHasMaxSpeed ? sc.mMaxSpeed : (s.mMph ? sp.mDialMph : sp.mDialKmh);
    s.mGear = live && sc.mHasGear ? sc.mGear : label;
    s.mGearNumber = gear_n;
    if (live && sc.mHasGear)
    {
        const S32 n = atoi(sc.mGear.c_str());
        s.mGearNumber = n > 0 ? n : 0;
    }
    s.mManual = manual;
    const F32 fuel_model = llclamp(100.f - mFuelUsedKm / llmax(1.f, sp.mRangeKm) * 100.f, 0.f, 100.f);
    s.mFuel = use && sc.mHasFuel ? sc.mFuel : fuel_model;
    s.mAdBlue = use && sc.mHasAdBlue ? sc.mAdBlue : 100.f;   // no published tank size: full unless told
    // coolant: the model warms to 90 C over the first minute driven (MODEL)
    s.mTemp = use && sc.mHasTemp ? sc.mTemp : llmin(90.f, 40.f + mDriving * (50.f / 60.f));
    // Source: "the braking system normally works at 8 bar" (truck air brake research).
    s.mAir1 = use && sc.mHasAir1 ? sc.mAir1 : 8.f;
    s.mAir2 = use && sc.mHasAir2 ? sc.mAir2 : 8.f;
    s.mIndicators = use && sc.mHasInd ? sc.mInd : mOwnIndicators;
    s.mLights = use && sc.mHasLights ? sc.mLights : mOwnLights;
    s.mHandbrake = use && sc.mHasHandbrake ? sc.mHandbrake : mOwnHandbrake;
    s.mRetarder = use && sc.mHasRetarder ? sc.mRetarder : 0;
    s.mCruise = use && sc.mHasCruise ? sc.mCruise : mOwnCruise;
    s.mDriveTime = use && sc.mHasDriveTime ? sc.mDriveTime : mDriving;
    s.mOdoKm = use && sc.mHasOdo ? sc.mOdo : mOdoKm;
    s.mTripKm = mTripKm;
    if (use && sc.mHasWarnings)
    {
        s.mWarnings = sc.mWarnings;
    }
    else if (s.mFuel < 10.f)
    {
        s.mWarnings.push_back("fuel");
    }

    // lean: the roll of the seat's frame (what a rider leans with), + = right side down
    const LLQuaternion q = worldRotation(gAgentAvatarp);
    const LLVector3 left = LLVector3::y_axis * q;
    const F32 lean = asinf(llclamp(left.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    s.mLean = lean;
    if (lean > mMaxLeanR) mMaxLeanR = lean;
    if (-lean > mMaxLeanL) mMaxLeanL = -lean;
    s.mMaxLeanL = mMaxLeanL;
    s.mMaxLeanR = mMaxLeanR;
    mShown = s;
}

// ════════════════════════════════════════════════════════════════════════════════════════
// per frame (llappviewer.cpp, after WolfFlight::idle: this frame's AgentUpdate has gone, so the
// flags set here go out with the next one)
// ════════════════════════════════════════════════════════════════════════════════════════

void WolfDrive::idle()
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        return;
    }
    const F32 dt = llclamp((F32)gFrameIntervalSeconds, 0.001f, 0.25f);
    refreshDashGate();
    if (!onWolf())
    {
        return;   // [WOLF ONLY] no seat watching, no flags, no taps, no messages off Wolf
    }
    watchSeat();
    // Flight / Sailing Mode came on: it has the bottom of the screen.
    if (mDashOn && WolfFlight::instance().active())
    {
        mAutoOpened = false;
        requestDashboard(false);
    }
    combine(dt);
    applyControlFlags();
    tapStep();
    sendAnalogue();
    model(dt);
}
