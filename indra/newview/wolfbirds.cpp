/**
 * @file wolfbirds.cpp
 * @brief WolfViewer: wild birds over Wolf Territories regions (ticket 445, Maximus Lear's idea). See wolfbirds.h.
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
#include "wolfbirds.h"
#include "wolfsoundeffects.h"   // [SOUND EFFECTS 2026-11-06/]

#include <cmath>

#include "llagent.h"
#include "llagentcamera.h"   // [BIRDS MID/] gAgentCamera.mDrawDistance
#include "llaudioengine.h"
#include "lldate.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llrand.h"
#include "llrender.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llvolume.h"
#include "llvovolume.h"   // [BIRDS LANDING/] sWolfRoofProbe
#include "llworld.h"
#include "pipeline.h"
#include "wolfaltitudesky.h"    // [BIRDS MID/] farClipExtra
#include "wolfboatrock.h"       // [BIRDS LANDING/] drawnWaveHeight, isRocking
#include "wolfframegovernor.h"
#include "wolfgrid.h"
#include "wolfhurricane.h"
#include "wolfregionweather.h"
#include "wolfweather.h"
#include "wolfweathersound.h"

// Source for every bird figure below (sizes, speeds, wingbeats): region-terrain-work/birds-2026-11-02/README.md "The
// birds" - RSPB species pages for size, Pennycuick (1990, J. Exp. Biol. 150:171) and Pennycuick (2008, Modelling the
// Flying Bird) for flight speed and wingbeat frequency, Ballerini et al. (2008, PNAS 105:1232) for the starlings'
// seven nearest neighbours.
namespace
{
    struct Species
    {
        F32 mSpan;          // wingspan, m
        F32 mLength;        // body length, m
        F32 mSpeed;         // cruising airspeed, m/s
        F32 mBeatHz;        // wingbeat frequency
        F32 mBeatDeg;       // inner wing amplitude, degrees
        F32 mAltMin, mAltMax;   // metres above the surface below
        F32 mTop[3], mTip[3], mUnder[3], mBody[3], mHead[3];   // colours (sRGB 0..1)
    };
    // GULL a herring gull: 1.37 m span, 0.60 m long, 10.6 m/s, 2.8 Hz; grey back, black wingtips, white below.
    // TERN a common tern: 0.82 m span, 0.33 m long, 8.9 m/s, 3.4 Hz; pale grey back, black cap, white below.
    // SMALL a sparrow or finch: 0.23 m span, 0.15 m long, 10 m/s, 14 Hz (bounding flight); brown above, buff below.
    // STARLING 0.39 m span, 0.21 m long, 12 m/s, 12 Hz; glossy black-brown all over.
    const Species SPECIES[WolfBirds::KIND_COUNT] = {
        { 1.37f, 0.60f, 10.6f,  2.8f, 34.f, 10.f, 45.f,
          {0.60f, 0.64f, 0.70f}, {0.06f, 0.06f, 0.07f}, {0.93f, 0.93f, 0.92f}, {0.94f, 0.94f, 0.93f}, {0.95f, 0.95f, 0.94f} },
        { 0.82f, 0.33f,  8.9f,  3.4f, 40.f,  5.f, 14.f,
          {0.78f, 0.80f, 0.84f}, {0.50f, 0.52f, 0.56f}, {0.95f, 0.95f, 0.95f}, {0.95f, 0.95f, 0.95f}, {0.04f, 0.04f, 0.05f} },
        { 0.23f, 0.15f, 10.0f, 14.0f, 55.f,  4.f, 18.f,
          {0.45f, 0.33f, 0.22f}, {0.28f, 0.21f, 0.14f}, {0.80f, 0.72f, 0.60f}, {0.50f, 0.38f, 0.26f}, {0.40f, 0.30f, 0.21f} },
        { 0.39f, 0.21f, 12.0f, 12.0f, 45.f, 35.f, 90.f,
          {0.08f, 0.08f, 0.10f}, {0.05f, 0.05f, 0.06f}, {0.13f, 0.12f, 0.13f}, {0.09f, 0.09f, 0.11f}, {0.07f, 0.07f, 0.09f} },
    };

    // The director: how often it decides, and how far round the camera birds live.
    constexpr F64 DIRECT_SECS = 3.0;
    constexpr F32 NEAR_M = 60.f, FAR_M = 220.f;      // where a flock's centre may be
    // [BIRDS MID 2026-11-02] ticket 463: the draw distance (RenderFarClip, the camera's far clip) also bounds the flocks.
    // Nothing past the far clip is drawn, and the Mid graphics level sets 128 m (featuretable_linux.txt "list Mid"
    // RenderFarClip 128; measured in a test viewer: far clip 133 m at Mid), so with 60..220 m most birds were out of
    // sight at Mid. A flock's birds roam up to VIEW_MARGIN_M round its centre (move(): the target is the centre +- 40 m),
    // so the centre keeps that much inside the draw distance; never nearer than MIN_REACH_M.
    constexpr F32 VIEW_MARGIN_M = 40.f;
    constexpr F32 MIN_REACH_M = 30.f;
    constexpr F32 ENTRY_M = 170.f;                   // a new flock flies in from this far beyond its centre
    constexpr F32 GONE_M = 320.f;                    // a leaving bird is dropped beyond this
    constexpr F32 GONE_HIDDEN_M = 140.f;             // ... or beyond this when out of view
    constexpr F32 TELEPORT_M = 300.f;                // the camera jumped: start again
    // [BIRDS 2026-11-02] A camera this high over the ground or sea is in a skybox or on a high build: no birds there.
    // Birds keep to their own heights over the surface (the tallest, a murmuration, to 90 m), never up in the sky.
    constexpr F32 CAMERA_HIGH_M = 150.f;
    // Flock sizes before the daylight / weather / budget scale.
    constexpr S32 GULLS_MIN = 4, GULLS_MAX = 9;
    constexpr S32 TERNS_MIN = 3, TERNS_MAX = 6;
    constexpr S32 SMALL_MIN = 8, SMALL_MAX = 18;
    constexpr S32 STARLINGS = 140;
    constexpr S32 MAX_FLOCKS = 5;
    constexpr S32 HARD_MAX_BIRDS = 300;
    // Built cells: CELL_M squares; a cell with this much build in it is "built" and birds keep out of it.
    constexpr F32 CELL_M = 16.f;
    constexpr S32 MAX_CELLS = 512;                   // a 8192 m region
    constexpr F32 BUILT_WEIGHT = 14.f;
    constexpr F32 SKYBOX_ABOVE_M = 60.f;             // objects higher than this over the ground are not the town below
    constexpr S32 SCAN_PER_FRAME = 3000;
    constexpr F64 SCAN_SECS = 30.0;
    // Calls: how far each kind is heard, how often a flock calls.
    constexpr F32 CALL_EAR_M = 4.f;                  // the call is placed this far along the bird's direction (see playCall)
    constexpr F32 BIRD_G_MS2 = 9.81f;
    // Beyond this many wingspans a bird's body is a few pixels across: only its wings are drawn (12 of its 28 triangles saved).
    constexpr F32 LOD_SPANS = 120.f;

    // ── [BIRDS LANDING 2026-11-02] ─────────────────────────────────────────────────────────────────────────────────
    // README "Landing" lists every figure with its source; the studies are cited where each is used.
    //
    // FLUSHING: how close something may come before a bird down on the ground, water or a perch takes off in alarm.
    // People: herring gulls in Liverpool flushed at 5.6 m (Pavlova & Wronski 2020, Advances in Environmental Research,
    // Nova, pp. 33-62: "Mean (+-SE) FID of Herring gulls was 5.63 +- 0.63 m"), western gulls at 17.4 m (Blumstein 2006,
    // Animal Behaviour 71:389, Table A1); 10 m lies between the town gull and the wild one, and is the figure Paul asked
    // for. House sparrows: "Average FID per settlement was 10.0 m" (Garcia-Arroyo et al. 2023, Scientific Reports
    // 13:2545; Blumstein 2006 Table A1 has 13.2 m).
    constexpr F32 FLUSH_PERSON_GULL_M = 10.f;
    constexpr F32 FLUSH_PERSON_SMALL_M = 10.f;
    // Vehicles and boats: laughing gulls flushed at 27.8 m from an outboard boat (Rodgers & Schwikert 2002, Conservation
    // Biology 16:216, Table 1), ring-billed gulls at 22.0 m from a car (Rodgers & Smith 1997, Wildlife Society Bulletin
    // 25:139); 25 m is between the two. No figure for sparrows and cars was found, so the gulls' is used for both.
    constexpr F32 FLUSH_VEHICLE_M = 25.f;
    // Low aircraft: no study of planes was found; the only aerial one is drones, where herring and lesser black-backed
    // gulls flushed at a mean of 25 m on a diagonal approach (Velilla et al. 2025, PLoS ONE 20:e0332619, Table 3). The
    // distance is measured in 3D, so only a plane that comes low near them counts.
    constexpr F32 FLUSH_AIRCRAFT_M = 25.f;
    // The camera is not a creature the birds can see. Chosen: only when it is practically on top of a bird (inside 2 m),
    // so a resident can zoom in to watch a gull from a few metres without scaring it, but cannot fly through it.
    constexpr F32 FLUSH_CAMERA_M = 2.f;
    // A root object moving faster than this is a vehicle or boat under way (chosen: a walking pace, 1 m/s, is the speed of
    // the walking approaches in the studies above, "1-2 m/s" in Pavlova & Wronski 2020); faster than AIRCRAFT_MIN_MS it is
    // flying (chosen: 30 m/s, about 58 knots, under any plane's stall speed and over any boat a region carries).
    constexpr F32 VEHICLE_MIN_MS = 1.f;
    constexpr F32 AIRCRAFT_MIN_MS = 30.f;
    // How often the flush check runs. Chosen: 4 times a second, so a bird reacts within a quarter of a second, while the
    // check (avatars x down birds) stays off the per-frame path.
    constexpr F64 FLUSH_CHECK_SECS = 0.25;
    // One bird's alarm call lifts its flock-mates down within this distance (gulls; a small flock always lifts as one).
    // Chosen: the gulls' own flush distance, so a raft lifts together from the side the danger comes.
    constexpr F32 ALARM_SPREAD_M = FLUSH_PERSON_GULL_M;
    // The alarm climb: chosen as a quarter of the cruising speed upwards (a climb of about 14 degrees) for 3 s.
    constexpr F32 ALARM_CLIMB_FRAC = 0.25f;
    constexpr F32 ALARM_CLIMB_S = 3.f;
    // After a flush a flock does not land again for this long (chosen: long enough to be clearly somewhere else).
    constexpr F32 WARY_MIN_S = 12.f, WARY_MAX_S = 25.f;
    // A flushed flock settles again at least this many times its flush distance from what flushed it (chosen: clearly
    // "further away"; town herring gulls themselves only moved 4.6 m, Pavlova & Wronski 2020, which reads as no move).
    constexpr F32 RESETTLE_FID_MULT = 2.f;
    // About half of flushed shorebirds "flew elsewhere" (Burger 1981, as quoted in Rodgers & Schwikert 2002): a small flock
    // that lifts leaves for good this often, and otherwise re-lands nearby.
    constexpr F32 LEAVE_AFTER_LIFT = 0.5f;
    //
    // THE RAY BUDGET: the downward rays that find perches and check a landing spot is open to the sky. Chosen: 4 a
    // second (the weather's roof grid casts many more), never more than 4 at once, never on a frame by habit.
    constexpr F32 RAYS_PER_SEC = 4.f;
    constexpr F32 RAY_BURST = 4.f;
    // A landing spot is open to the sky when a ray from this high above it meets nothing before the surface itself (chosen:
    // above any house or harbour roof round it, below the 60 m skybox line of the built-cell scan).
    constexpr F32 CLEAR_ABOVE_M = 40.f;
    // PERCHES: a few gulls on roof ridges, wall tops and harbour railings (herring gulls nest "between chimney pots, or in
    // complex roof structures", Rock 2005, British Birds 98:338; RSPB herring gull page, "perched in harbour").
    constexpr S32 MAX_PERCHED = 6;          // chosen: "a few", whatever the size of the town
    constexpr S32 MAX_PERCHES_KNOWN = 6;    // free perches remembered at once
    constexpr F64 PERCH_KEEP_SECS = 90.0;   // a found perch is forgotten after this (it may have been built over)
    constexpr F32 PERCH_MIN_M = 2.5f;       // over the ground: a wall top or a roof, not a step or a bench (chosen)
    constexpr F32 PERCH_MAX_M = 25.f;       // ... and no higher than the gulls' own low flight (chosen, SPECIES alt 10-45 m)
    constexpr F32 PERCH_NORMAL_Z = 0.94f;   // near-horizontal: the surface tilts at most 20 degrees (cos 20 = 0.94; chosen)
    constexpr F32 PERCH_SEARCH_M = 90.f;    // prims this near a gull flock's centre are tried
    constexpr F32 PERCH_GAP_M = 0.9f;       // two gulls perch no closer (chosen: more than a gull's 0.6 m length apart)
    constexpr F32 PERCH_MOVED_M = 0.05f;    // the perch prim moved more than this: the gull flies off
    constexpr S32 MAX_PERCH_CANDIDATES = 4096;
    constexpr F64 PERCH_RAY_SECS = 1.0;     // at most one perch ray a second, so the landing checks keep their rays
    // SHORE: sand or shingle at the water's edge. The drawn water stands no higher than 0.39 x its depth (wolfboatrock.cpp
    // waveHeight, McCowan's limit), so land above the water plane is dry; SHORE_MIN_M keeps the feet clear of the ripple.
    // SHORE_MAX_M: the strand line, not the dunes behind it (chosen). The slope: a beach face up to 15 degrees (chosen: as
    // steep as a shingle bank a gull would stand on; steeper is a bank or a wall).
    constexpr F32 SHORE_MIN_M = 0.15f, SHORE_MAX_M = 2.5f;
    constexpr F32 SHORE_MAX_SLOPE = 0.27f;  // tan 15 degrees
    constexpr F32 SHORE_SEARCH_M = 70.f;
    // GRASS: open, flat, unbuilt land, above the shore (chosen: 1 m over the water plane, and at most 10 degrees).
    constexpr F32 GRASS_ABOVE_WATER_M = 1.f;
    constexpr F32 GRASS_MAX_SLOPE = 0.18f;  // tan 10 degrees
    constexpr F32 PATCH_R_M = 5.f;          // a feeding flock spreads over this radius (chosen: a few metres, as seen in town)
    //
    // DOWN: what they do there. House sparrows feeding: "head-up and pecking events was 26.40 +- 2.95 and 54.20 +- 5.35 ...
    // over the 3-min period", head up 0.66 of the time, "scan bouts (0.16 +- 0.06 min)" (Tisdale & Fernandez-Juricic
    // 2009, Behavioral Ecology 20:936). So a head-up bout of 9.6 s, a head-down bout of 9.6 x 0.34 / 0.66 = 4.9 s, and
    // 54.2 pecks in 3 min x 0.34 of it head down = 0.89 pecks a second while feeding.
    constexpr F32 SCAN_BOUT_S = 9.6f;
    constexpr F32 PECK_BOUT_S = 4.9f;
    constexpr F32 PECK_HZ = 0.89f;
    constexpr F32 PECK_PITCH_DEG = 50.f;    // how far the head goes down to peck (chosen, the look of it)
    // Sparrows hop ("You'll often see them hopping around on the ground", RSPB house sparrow). No hop rate was found:
    // chosen, a hop of 0.08 m (half its length) every 0.35 s, 4 cm high.
    constexpr F32 HOP_M = 0.08f, HOP_S = 0.35f, HOP_H = 0.04f;
    // Gulls walk; no walking speed was found. Chosen 0.5 m/s, half the 1.01 m/s escape speed measured for town herring
    // gulls (Pavlova & Wronski 2020). Paddling: chosen 0.3 m/s, an easy drift, turning at most 0.3 rad/s.
    constexpr F32 WALK_MS = 0.5f, PADDLE_MS = 0.3f, PADDLE_TURN = 0.3f;
    // How long a gull stays down by itself. No loafing-bout figure was found: chosen 25-90 s, so someone watching sees
    // gulls both land and leave inside a minute or two; a gull starts looking for somewhere to settle after 20-60 s aloft.
    constexpr F32 GULL_DOWN_MIN_S = 25.f, GULL_DOWN_MAX_S = 90.f;
    constexpr F32 GULL_LAND_MIN_S = 20.f, GULL_LAND_MAX_S = 60.f;
    // A small flock wheels 15-40 s before dropping to feed, feeds 20-60 s, then lifts (chosen, same reasoning).
    constexpr F32 SMALL_SETTLE_MIN_S = 15.f, SMALL_SETTLE_MAX_S = 40.f;
    constexpr F32 SMALL_FEED_MIN_S = 20.f, SMALL_FEED_MAX_S = 60.f;
    // Take-off: gulls "are able to take off quickly with little space" (Burger & Gochfeld 1996, Handbook of the Birds of
    // the World, as cited by Wikipedia "Gull"). No run length was found: from the water, chosen a short patter of 0.6 s
    // reaching 70 % of cruising speed; from land or a perch a spring straight up into flapping flight.
    constexpr F32 TAKEOFF_RUN_S = 0.6f;
    constexpr F32 TAKEOFF_RUN_SPEED = 0.7f;
    constexpr F32 SPRING_MS = 2.5f;         // the spring's upward speed (chosen)
    constexpr F32 RISE_MS = 1.5f;           // the least climb just after take-off, until it is 1.5 m up (chosen: under 1 s)
    // The approach: a glide slope of 1 in 3 down to the spot (chosen), slowing as it comes in; given up after 40 s.
    constexpr F32 GLIDE_SLOPE = 0.33f;
    constexpr F32 APPROACH_TIMEOUT_S = 40.f;
    // Heavy rain keeps them down: rainfall "increased time spent perching" in frigatebirds (De Pascalis et al. 2022,
    // Behavioral Ecology 33:343). A downpour (level 3+) already sends all birds away (weather_factor), so "heavy rain"
    // here is the heaviest rain that still has birds: level 2.
    constexpr S32 SIT_TIGHT_LEVEL = 2;
    constexpr F32 SIT_TIGHT_LAND_S = 4.f;   // airborne birds look for somewhere to sit within this (chosen)
    //
    // THE BODY ON THE SURFACE. Standing, the body's underside is the bare leg (the tarsus) above the feet: herring gull
    // 58.9 mm (male) / 54.7 mm (female) (Threlfall & Jewer 1978, Auk 95:749), so 57 mm; house sparrow 19.8 mm (Griggio &
    // Hoi 2010, BMC Evol. Biol. 10:261). The body's underside is 0.081 of its length below its axis (buildMesh: h x 0.9,
    // h = 0.09 len). Floating, the waterline is at the body's axis less a third of its half-depth (chosen: a gull rides
    // high, most of its body over the water).
    constexpr F32 TARSUS_GULL_M = 0.057f, TARSUS_SMALL_M = 0.0198f;
    constexpr F32 BODY_UNDER = 0.081f;      // x length
    // The drawn water under a floating bird is sampled (WolfBoatRock::drawnWaveHeight, the whole wave model) at most every
    // WAVE_SAMPLE_S and carried on at the rate between the last two samples in between. Measured in world on 2 Nov: the
    // per-frame sample was most of the landing's cost (move() 29-58 us for 4-7 gulls on the water). The error of a linear
    // carry over 0.05 s is under a millimetre on the swell and about 1.5 cm on the fastest shore breaker (amplitude x
    // omega^2 x 0.05^2); under 20 frames a second every frame is a fresh sample.
    constexpr F64 WAVE_SAMPLE_S = 0.05;
    constexpr F32 FLOAT_ABOVE = 0.03f;      // x length: the axis over the waterline
    // Herring gull: "Their legs are pink with webbed feet" (RSPB herring gull). No wording was found for the sparrow's: its
    // legs are drawn in its own darker back colour (SPECIES mTip).
    const F32 LEG_GULL[3] = { 0.86f, 0.62f, 0.60f };

    // The calls, app_settings/wolfbirds (SOURCES.md there; region-terrain-work/birds-2026-11-02/tools/build_sounds.py).
    const WolfWeatherSound::Recording CALL_GULL[3] = {
        { "wolf_bird_gull_1.ogg", "c75a55b5-6904-4389-8d66-830368e62d2f", nullptr, false, "wolfbirds" },
        { "wolf_bird_gull_2.ogg", "8178967f-63ec-423a-a9ca-2edf64dd43bf", nullptr, false, "wolfbirds" },
        { "wolf_bird_gull_3.ogg", "22e8f39f-50e8-445d-b966-1beea24af28b", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_TERN[3] = {
        { "wolf_bird_tern_1.ogg", "a1d20907-e572-491f-a508-c05b73b74ebc", nullptr, false, "wolfbirds" },
        { "wolf_bird_tern_2.ogg", "38f4bd21-41d8-4a3e-aa64-b79c569ff3bb", nullptr, false, "wolfbirds" },
        { "wolf_bird_tern_3.ogg", "462eae77-e9f2-4163-bc74-678b095ef76b", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_SMALL[3] = {
        { "wolf_bird_small_1.ogg", "289b6347-4726-4eff-8ad7-a86ce199920b", nullptr, false, "wolfbirds" },
        { "wolf_bird_small_2.ogg", "c0e58dbc-8ceb-44d4-ad10-33a9370d9877", nullptr, false, "wolfbirds" },
        { "wolf_bird_small_3.ogg", "a194a129-3cfc-45ec-969c-415cd4cb3600", nullptr, false, "wolfbirds" },
    };
    const WolfWeatherSound::Recording CALL_FLOCK =
        { "wolf_bird_flock_1.ogg", "5ccc9fba-3b59-4c33-8b31-5b6255759964", nullptr, false, "wolfbirds" };

    F32 frand_range(F32 lo, F32 hi) { return lo + ll_frand(hi - lo); }
    S32 rand_range(S32 lo, S32 hi) { return lo + ll_rand(hi - lo + 1); }

    F32 smoothstep(F32 a, F32 b, F32 x)
    {
        const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    LLVector3 flat(const LLVector3& v) { return LLVector3(v.mV[VX], v.mV[VY], 0.f); }

    /** [BIRDS MID 2026-11-02] ticket 463: how far out a flock's centre may be (at most FAR_M), drawn in to stay inside the
     *  draw distance. The main camera's far clip is the draw distance plus the altitude extra (llviewerdisplay.cpp
     *  final_far = gAgentCamera.mDrawDistance ... += WolfAltitudeSky::farClipExtra()); read from those, not from
     *  LLViewerCamera::getFar(), which a reflection-probe pass (gCubeSnapshot: RenderReflectionProbeDrawDistance) can
     *  leave at its own value until the next main-camera update. */
    F32 flock_reach_m()
    {
        const F32 view_m = gAgentCamera.mDrawDistance + WolfAltitudeSky::farClipExtra();
        return llclamp(view_m - VIEW_MARGIN_M, MIN_REACH_M, FAR_M);
    }

    /** The sun's height over the horizon, degrees, in the sky on screen (llenvironment.h getCurrentSky; as
     *  wolfautoenvironment.cpp:313). +90 when no sky is known yet (birds are not held back by a missing sky). */
    F32 sun_elevation_deg(bool& west)
    {
        west = false;
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return 90.f;
        const LLVector3 sun = sky->getSunDirection();
        west = sun.mV[VX] < 0.f;   // +X is east (region coordinates): the evening sun is in the west
        return (F32)(std::asin(llclamp((F64)sun.mV[VZ], -1.0, 1.0)) * 180.0 / F_PI);
    }

    /** [BIRDS 2026-11-02] Paul: "not everyone will want bird flocks on their region - for example a space region". An
     *  Earth sky has an atmosphere: the sky on screen (llsettingssky.h getBlueDensity / getBlueHorizon / getHazeDensity,
     *  the EEP's own sky colour and haze) is not all black. A sky with no blue and no haze is space (or a black void), and
     *  no birds fly in it. The thresholds are "black to the eye" (2 % of full scale), not any preset's value. */
    bool earth_sky()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        if (!sky) return true;
        const LLColor3 bd = sky->getBlueDensity(), bh = sky->getBlueHorizon();
        const F32 blue = llmax(llmax(bd.mV[0], bd.mV[1]), llmax(bd.mV[2], llmax(bh.mV[0], llmax(bh.mV[1], bh.mV[2]))));
        return !(blue < 0.02f && sky->getHazeDensity() < 0.02f);
    }

    /** The month now (1..12, UTC), and whether the region is south of the equator: its real place when it follows one
     *  (the automatic environment), else the north (the grid's own clock is the UK's). */
    S32 month_now()
    {
        S32 month = 0;
        LLDate::now().split(nullptr, &month);
        return month;
    }
    bool southern()
    {
        if (!WolfRegionWeather::instanceExists()) return false;
        const WolfRegionWeather& rw = WolfRegionWeather::instance();
        return (rw.autoEnvSky() || rw.autoEnvWeather()) && rw.autoEnvLat() < 0.0;
    }

    /** Starlings gather in winter: October to March north of the equator, April to September south of it. */
    bool murmuration_season()
    {
        const S32 month = month_now();
        const bool winter_north = (month >= 10 || month <= 3);
        return southern() ? !winter_north : winter_north;
    }

    /** Terns breed here and fish the coasts from April to September (north); otherwise gulls only. */
    bool tern_season()
    {
        const S32 month = month_now();
        const bool summer_north = (month >= 4 && month <= 9);
        return southern() ? !summer_north : summer_north;
    }

    /** 0..1: how much the weather on screen keeps birds down. Source: WolfWeatherProfile (wolfweatherprofile.h): kind,
     *  level 1..4 (light .. torrential / blizzard), fog 0..100. */
    F32 weather_factor()
    {
        if (!WolfWeather::instanceExists() || !WolfWeather::enabled()) return 1.f;
        const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
        F32 f = 1.f;
        if (p.mKind != WolfWeatherProfile::CLEAR)
        {
            if (p.mLevel >= 3) return 0.f;          // a downpour or a blizzard: they shelter
            f = (p.mLevel <= 1) ? 0.7f : 0.35f;
        }
        if (p.mFog > 60) f *= 0.5f;
        return f;
    }

    /** [BIRDS LANDING 2026-11-02] Heavy rain on screen that still has birds (SIT_TIGHT_LEVEL; a downpour has none): the
     *  birds down stay down and those aloft settle (wolfweatherprofile.h kind RAIN, level 1..4). */
    bool sit_tight_now()
    {
        if (!WolfWeather::instanceExists() || !WolfWeather::enabled()) return false;
        const WolfWeatherProfile& p = WolfWeather::instance().activeProfile();
        return p.mKind == WolfWeatherProfile::RAIN && p.mLevel >= SIT_TIGHT_LEVEL;
    }

    /** True while a hurricane is over the region (wolfhurricane.h timeAt: stage NONE before it forms and after it is gone). */
    bool hurricane_now()
    {
        if (!WolfRegionWeather::instanceExists()) return false;
        const WolfHurricane* h = WolfRegionWeather::instance().hurricane();
        return h && h->timeAt(LLDate::now().secondsSinceEpoch()).active();
    }
}

WolfBirds::WolfBirds() {}
WolfBirds::~WolfBirds() {}

void WolfBirds::rebase(const LLVector3& delta)
{
    for (Bird& b : mBirds)
    {
        b.mPos += delta;
        b.mWaypoint += delta;   // mOrbitOffset is relative to the flock's centre
        b.mSpotPos += delta;    // [BIRDS LANDING/]
        b.mPerchAt += delta;
    }
    for (Flock& f : mFlocks)
    {
        f.mCentre += delta;
        f.mTarget += delta;
        f.mExit += delta;
        f.mPatch += delta;      // [BIRDS LANDING/]
        f.mFlushedFrom += delta;
    }
    for (Perch& p : mPerches)
    {
        p.mAt += delta;
        p.mObjectAt += delta;
    }
    for (Threat& t : mThreats) t.mPos += delta;
    mLastCamera += delta;
}

void WolfBirds::reset()
{
    mBirds.clear();
    mFlocks.clear();
    mPerches.clear();   // [BIRDS LANDING/]
    mThreats.clear();
    mNextDirect = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the place
// ═══════════════════════════════════════════════════════════════════════════════════════

/** The region under a point: its own water height for the sea level (a neighbour's can differ). Null over ground no
 *  region covers; then a point is not water, and the surface is the land the world resolves (review 2026-11-02). */
F32 WolfBirds::surfaceZ(const LLVector3& agent) const
{
    const F32 land = LLWorld::getInstance()->resolveLandHeightAgent(agent);
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(agent);
    return r ? llmax(land, r->getWaterHeight()) : land;
}

bool WolfBirds::overWater(const LLVector3& agent) const
{
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(agent);
    if (!r) return false;
    // half a metre under: the wet sand at the edge is not sea
    return LLWorld::getInstance()->resolveLandHeightAgent(agent) < r->getWaterHeight() - 0.5f;
}

bool WolfBirds::inRegion(LLViewerRegion* region, const LLVector3& agent, F32 margin) const
{
    if (!region) return false;
    const LLVector3 o = region->getOriginAgent();
    const F32 w = region->getWidth();
    const F32 x = agent.mV[VX] - o.mV[VX], y = agent.mV[VY] - o.mV[VY];
    return x >= -margin && y >= -margin && x <= w + margin && y <= w + margin;
}

bool WolfBirds::built(const LLVector3& agent) const
{
    LLViewerRegion* r = gAgent.getRegion();
    if (!r || !mHaveBuilt || mCells <= 0) return false;
    const LLVector3 o = r->getOriginAgent();
    const S32 cx = (S32)floorf((agent.mV[VX] - o.mV[VX]) / CELL_M);
    const S32 cy = (S32)floorf((agent.mV[VY] - o.mV[VY]) / CELL_M);
    if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) return false;
    return (F32)mBuilt[(size_t)(cy * mCells + cx)] >= BUILT_WEIGHT;
}

/**
 * Which cells are crowded with builds, from the objects the viewer already holds (gObjectList), spread over frames
 * (SCAN_PER_FRAME objects each) and repeated every SCAN_SECS. Only prims count (LL_PCODE_VOLUME, llvolume.h): not trees
 * or grass (birds like trees), not avatars or their attachments, not skyboxes high over the ground. Each prim adds
 * 1 + its largest side / 8 m to the cell its centre is in, so a few big walls count as much as many small parts.
 */
void WolfBirds::scanBuilds(LLViewerRegion* region, F64 now)
{
    if (mScanIndex < 0)
    {
        if (now < mNextScan) return;
        mCells = llclamp((S32)ceilf(region->getWidth() / CELL_M), 1, MAX_CELLS);
        mBuiltScan.assign((size_t)(mCells * mCells), 0);
        mPerchCandsScan.clear();   // [BIRDS LANDING/]
        mScanIndex = 0;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 perch_reach = flock_reach_m() + PERCH_SEARCH_M;   // [BIRDS MID 2026-11-02] ticket 463: the flocks' reach
    const LLVector3 o = region->getOriginAgent();
    const S32 n = gObjectList.getNumObjects();
    const S32 end = llmin(n, mScanIndex + SCAN_PER_FRAME);
    for (S32 i = mScanIndex; i < end; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isDead() || obj->getRegion() != region) continue;
        if (obj->getPCode() != LL_PCODE_VOLUME || obj->isAttachment() || obj->isAvatar()) continue;
        const LLVector3 p = obj->getPositionAgent();
        const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(p);
        if (p.mV[VZ] > ground + SKYBOX_ABOVE_M) continue;
        const S32 cx = (S32)floorf((p.mV[VX] - o.mV[VX]) / CELL_M);
        const S32 cy = (S32)floorf((p.mV[VY] - o.mV[VY]) / CELL_M);
        if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) continue;
        const LLVector3 s = obj->getScale();
        const F32 w = 1.f + llmax(s.mV[VX], llmax(s.mV[VY], s.mV[VZ])) / 8.f;
        U16& c = mBuiltScan[(size_t)(cy * mCells + cx)];
        c = (U16)llmin(60000.f, (F32)c + w);
        // [BIRDS LANDING 2026-11-02] a prim whose top could be a perch, near enough to the camera for a gull to use: one
        // to aim a perch ray at (seekPerch). Its top from its largest side, as the rotation is not known here; the ray finds
        // the real surface.
        const F32 top = p.mV[VZ] + 0.5f * llmax(s.mV[VX], llmax(s.mV[VY], s.mV[VZ]));
        if ((S32)mPerchCandsScan.size() < MAX_PERCH_CANDIDATES && top - ground >= PERCH_MIN_M && top - ground <= PERCH_MAX_M
            && flat(p - cam).length() <= perch_reach)
        {
            PerchCandidate pc;
            pc.mRegionPos = p - o;
            pc.mHalfX = 0.5f * s.mV[VX];
            pc.mHalfY = 0.5f * s.mV[VY];
            pc.mTop = top - o.mV[VZ];
            mPerchCandsScan.push_back(pc);
        }
    }
    mScanIndex = end;
    if (mScanIndex >= n)
    {
        mPerchCands.swap(mPerchCandsScan);   // [BIRDS LANDING/]
        mBuilt.swap(mBuiltScan);
        mHaveBuilt = true;
        mScanIndex = -1;
        mNextScan = now + SCAN_SECS;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the director
// ═══════════════════════════════════════════════════════════════════════════════════════

bool WolfBirds::wanted(LLViewerRegion*& region) const
{
    region = gAgent.getRegion();
    if (!region) return false;
    static LLCachedControl<bool> on(gSavedSettings, "WolfViewerBirds", true);
    if (!on) return false;
    if (!WolfGrid::showsWolfRegionWeather()) return false;   // Wolf regions only, visitors included (wolfgrid.h)
    if (!WolfRegionWeather::instanceExists() || !WolfRegionWeather::instance().birdsAllowed()) return false;
    return true;
}

bool WolfBirds::pickPlace(LLViewerRegion* region, Kind kind, F32 min_m, F32 max_m, LLVector3& out) const
{
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (S32 attempt = 0; attempt < 16; ++attempt)
    {
        const F32 a = ll_frand(F_TWO_PI), d = frand_range(min_m, max_m);
        LLVector3 p(cam.mV[VX] + cosf(a) * d, cam.mV[VY] + sinf(a) * d, 0.f);
        if (!inRegion(region, p, -8.f)) continue;
        const bool sea = overWater(p);
        if ((kind == GULL || kind == TERN) != sea) continue;
        if (built(p)) continue;
        p.mV[VZ] = surfaceZ(p);
        out = p;
        return true;
    }
    return false;
}

void WolfBirds::direct(LLViewerRegion* region, F64 now)
{
    // What the place offers round the camera: a 9 x 9 sample of points within the flocks' reach (flock_reach_m, ticket
    // 463: where flocks can be, and the terrain the viewer has been sent), inside the region.
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    const F32 far_m = flock_reach_m();
    S32 sea = 0, land = 0, total = 0;
    for (S32 iy = -4; iy <= 4; ++iy)
    {
        for (S32 ix = -4; ix <= 4; ++ix)
        {
            const LLVector3 p(cam.mV[VX] + (F32)ix * (far_m / 4.f), cam.mV[VY] + (F32)iy * (far_m / 4.f), 0.f);
            if (!inRegion(region, p, 0.f)) continue;
            ++total;
            if (overWater(p)) ++sea;
            else if (!built(p)) ++land;
        }
    }
    const F32 sea_f = total ? (F32)sea / (F32)total : 0.f;
    const F32 land_f = total ? (F32)land / (F32)total : 0.f;

    bool west = false;
    const F32 sun = sun_elevation_deg(west);
    // Day: full from 4 degrees up; nothing below -4 (they roost by civil dusk).
    const F32 day = smoothstep(-4.f, 4.f, sun);
    const F32 weather = weather_factor();
    mSitTight = sit_tight_now();   // [BIRDS LANDING/]
    const bool storm = hurricane_now();
    const F32 budget = WolfFrameGovernor::budget();
    static LLCachedControl<S32> max_setting(gSavedSettings, "WolfViewerBirdsMax", 160);
    const S32 cap = llclamp((S32)((F32)llclamp((S32)max_setting, 10, HARD_MAX_BIRDS) * budget), 0, HARD_MAX_BIRDS);
    // [BIRDS 2026-11-02] not in a space sky, not up in a skybox; sea birds only where the region's default is sea and coast
    const bool earth = earth_sky();
    const bool high = cam.mV[VZ] - surfaceZ(cam) > CAMERA_HIGH_M;
    const bool sea_only = WolfRegionWeather::instance().birdsSeaOnly();
    const F32 scale = (storm || !earth || high) ? 0.f : day * weather;
    // Murmuration: an evening in the season, the sun from 5 degrees up to 4 down, over open land.
    const bool dusk = west && sun < 5.f && sun > -4.f;
    const bool murmur = !storm && earth && !high && !sea_only && weather > 0.5f && dusk && murmuration_season() && land_f >= 0.25f;

    // Wanted flocks of each kind now.
    S32 want[KIND_COUNT] = { 0, 0, 0, 0 };
    if (scale > 0.05f)
    {
        if (sea_f >= 0.08f) want[GULL] = (sea_f > 0.4f) ? 2 : 1;
        if (sea_f >= 0.08f && tern_season() && day > 0.9f) want[TERN] = 1;
        if (land_f >= 0.15f && !sea_only) want[SMALL] = (land_f > 0.5f) ? 2 : 1;
    }
    if (murmur) want[STARLING] = 1;

    S32 have[KIND_COUNT] = { 0, 0, 0, 0 };
    for (Flock& f : mFlocks)
    {
        if (f.mLeaving || f.mRoosting) continue;
        if (have[f.mKind] >= want[f.mKind])
        {
            // more than wanted: the starlings go down to roost where they are; the others fly off
            if (f.mKind == STARLING && !dusk) f.mRoosting = true;
            else leave(f, cam);   // [BIRDS LANDING/] birds down take off first (move())
            continue;
        }
        ++have[f.mKind];
    }
    S32 flying = (S32)mBirds.size();
    S32 flocks = (S32)mFlocks.size();   // move() drops a flock as soon as its last bird has gone
    for (S32 k = 0; k < KIND_COUNT && flocks < MAX_FLOCKS; ++k)
    {
        if (have[k] >= want[k]) continue;
        S32 n = 0;
        switch (k)
        {
        case GULL:     n = rand_range(GULLS_MIN, GULLS_MAX); break;
        case TERN:     n = rand_range(TERNS_MIN, TERNS_MAX); break;
        case SMALL:    n = rand_range(SMALL_MIN, SMALL_MAX); break;
        default:       n = STARLINGS; break;
        }
        n = (S32)llround((F32)n * (k == STARLING ? weather : scale));
        n = llmin(n, cap - flying);
        if (n < 2) continue;
        if (!spawnFlock(region, (Kind)k, n, now)) continue;   // no place for this kind here: try the next kind
        flying += n;
        ++flocks;
        break;   // one new flock per decision: they arrive one after another, not all at once
    }

    if (now - mLastLog > 60.0 && (!mBirds.empty() || flocks > 0))
    {
        mLastLog = now;
        LL_DEBUGS("WolfBirds") << mBirds.size() << " birds in " << flocks << " flocks; sea " << sea_f << " land " << land_f
                               << " sun " << sun << (west ? " (evening)" : "") << " weather " << weather << " budget "
                               << budget << " cap " << cap << " reach " << far_m << " m; " << landedCount() << " down, "
                               << mPerches.size()
                               << " free perches of " << mPerchCands.size() << " candidates"
                               << (mSitTight ? ", sitting tight" : "") << "; cost "
                               << (mCostFrames ? mCostIdleUs / mCostFrames : 0.0) << " us idle + "
                               << (mCostFrames ? mCostMeshUs / mCostFrames : 0.0) << " us mesh a frame (idle: flush "
                               << (mCostFrames ? mCostFlushUs / mCostFrames : 0.0) << ", perch "
                               << (mCostFrames ? mCostPerchUs / mCostFrames : 0.0) << ", move "
                               << (mCostFrames ? mCostMoveUs / mCostFrames : 0.0) << ")" << LL_ENDL;
        mCostIdleUs = mCostMeshUs = mCostFlushUs = mCostPerchUs = mCostMoveUs = 0.0;
        mCostFrames = 0;
    }
}

bool WolfBirds::spawnFlock(LLViewerRegion* region, Kind kind, S32 count, F64 now)
{
    Flock f;
    f.mKind = kind;
    const F32 far_m = flock_reach_m();   // [BIRDS MID 2026-11-02] ticket 463
    const F32 near_m = llmin(NEAR_M, far_m * 0.5f);
    if (!pickPlace(region, kind, near_m, far_m, f.mCentre)) return false;
    f.mTarget = f.mCentre;
    f.mRetarget = now + frand_range(8.f, 20.f);
    f.mNextCall = now + frand_range(3.f, 10.f);
    f.mLoop = ll_frand(F_TWO_PI);
    f.mSettleIn = frand_range(SMALL_SETTLE_MIN_S, SMALL_SETTLE_MAX_S);   // [BIRDS LANDING/]
    // Where they fly in from: ENTRY_M beyond the centre, out of view if that can be found, else simply far away.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    LLVector3 entry = f.mCentre;
    for (S32 i = 0; i < 8; ++i)
    {
        const F32 a = ll_frand(F_TWO_PI);
        LLVector3 e = f.mCentre + LLVector3(cosf(a), sinf(a), 0.f) * ENTRY_M;
        e.mV[VZ] = surfaceZ(e) + SPECIES[kind].mAltMax;
        entry = e;
        if (!camera->sphereInFrustum(e, 2.f) || (e - cam).length() > far_m + 40.f) break;
    }
    const S32 index = (S32)mFlocks.size();
    mFlocks.push_back(f);
    const Species& sp = SPECIES[kind];
    const F32 spread = (kind == STARLING) ? 18.f : 8.f;
    for (S32 i = 0; i < count; ++i)
    {
        Bird b;
        b.mFlock = index;
        b.mPos = entry + LLVector3(frand_range(-spread, spread), frand_range(-spread, spread), frand_range(-spread, spread) * 0.4f);
        b.mSpeed = sp.mSpeed * frand_range(0.9f, 1.1f);
        b.mSize = frand_range(0.9f, 1.1f);
        LLVector3 to = f.mCentre - b.mPos;
        to.mV[VZ] = 0.f;
        to.normVec();
        b.mVel = to * b.mSpeed;
        b.mPhase = ll_frand(F_TWO_PI);
        b.mAlt = frand_range(sp.mAltMin, sp.mAltMax);
        b.mOrbitOffset = LLVector3(frand_range(-30.f, 30.f), frand_range(-30.f, 30.f), 0.f);
        b.mOrbitR = frand_range(12.f, 30.f);
        b.mOrbitDir = (ll_frand() < 0.5f) ? -1.f : 1.f;
        b.mWaypoint = f.mCentre;
        b.mFlapFor = frand_range(1.f, 3.f);
        b.mNextLand = frand_range(GULL_LAND_MIN_S, GULL_LAND_MAX_S);   // [BIRDS LANDING/]
        mBirds.push_back(b);
    }
    LL_DEBUGS("WolfBirds") << "flock " << (S32)kind << " of " << count << " coming in" << LL_ENDL;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// [BIRDS LANDING 2026-11-02] landing, the ground, flushing (README "Landing")
// ═══════════════════════════════════════════════════════════════════════════════════════

S32 WolfBirds::landedCount() const
{
    S32 n = 0;
    for (const Bird& b : mBirds)
    {
        if (b.mLand == DOWN) ++n;
    }
    return n;
}

bool WolfBirds::takeRay()
{
    if (mRayTokens < 1.f) return false;
    mRayTokens -= 1.f;
    return true;
}

/** One budgeted ray down from CLEAR_ABOVE_M over a surface point: open to the sky when it meets the terrain (or the
 *  grass, or nothing: over water it stops a metre under the surface) and no prim, tree or attachment on the way down.
 *  sWolfRoofProbe as the weather's roof grid (wolfweather.cpp rayCell): prims set to click-action "Ignore" and roofs the
 *  camera does not draw this frame count too (llvovolume.cpp, llspatialpartition.cpp LLOctreeIntersect::check). */
S32 WolfBirds::spotClear(const LLVector3& surface)
{
    if (!takeRay()) return -1;
    LLVector4a start, end, hit;
    const LLVector3 s3(surface.mV[VX], surface.mV[VY], surface.mV[VZ] + CLEAR_ABOVE_M);
    const LLVector3 e3(surface.mV[VX], surface.mV[VY], surface.mV[VZ] - 1.f);
    start.load3(s3.mV);
    end.load3(e3.mV);
    LLVOVolume::sWolfRoofProbe = true;
    LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, nullptr, nullptr, nullptr, &hit);
    LLVOVolume::sWolfRoofProbe = false;
    if (!obj) return 1;
    const LLPCode pc = obj->getPCode();
    return (pc == LLViewerObject::LL_VO_SURFACE_PATCH || pc == LL_PCODE_LEGACY_GRASS) ? 1 : 0;
}

F32 WolfBirds::slopeAt(const LLVector3& p) const
{
    LLWorld* w = LLWorld::getInstance();
    const F32 dx = w->resolveLandHeightAgent(p + LLVector3(1.f, 0.f, 0.f)) - w->resolveLandHeightAgent(p - LLVector3(1.f, 0.f, 0.f));
    const F32 dy = w->resolveLandHeightAgent(p + LLVector3(0.f, 1.f, 0.f)) - w->resolveLandHeightAgent(p - LLVector3(0.f, 1.f, 0.f));
    return sqrtf(dx * dx + dy * dy) * 0.5f;
}

bool WolfBirds::shoreAt(const LLVector3& p) const
{
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
    if (!r || !inRegion(gAgent.getRegion(), p, -4.f) || built(p)) return false;
    const F32 above = LLWorld::getInstance()->resolveLandHeightAgent(p) - r->getWaterHeight();
    return above >= SHORE_MIN_M && above <= SHORE_MAX_M && slopeAt(p) <= SHORE_MAX_SLOPE;
}

bool WolfBirds::grassAt(const LLVector3& p) const
{
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
    if (!r || !inRegion(gAgent.getRegion(), p, -8.f) || built(p)) return false;
    const F32 above = LLWorld::getInstance()->resolveLandHeightAgent(p) - r->getWaterHeight();
    return above >= GRASS_ABOVE_WATER_M && slopeAt(p) <= GRASS_MAX_SLOPE;
}

F32 WolfBirds::flushRadius(U8 threat, Kind kind)
{
    switch (threat)
    {
    case THREAT_PERSON:   return kind == SMALL ? FLUSH_PERSON_SMALL_M : FLUSH_PERSON_GULL_M;
    case THREAT_VEHICLE:  return FLUSH_VEHICLE_M;
    case THREAT_AIRCRAFT: return FLUSH_AIRCRAFT_M;
    default:              return FLUSH_CAMERA_M;
    }
}

bool WolfBirds::threatNear(const LLVector3& p, Kind kind, F32 mult) const
{
    for (const Threat& t : mThreats)
    {
        const F32 r = flushRadius(t.mKind, kind) * mult;
        if ((t.mPos - p).lengthSquared() < r * r) return true;
    }
    return false;
}

/** The body's centre over the surface it is down on: floating, the axis just over the waterline; standing, the body's
 *  underside on the bare leg (TARSUS_*), both scaled with the bird. */
F32 WolfBirds::standHeight(const Bird& b, Kind kind) const
{
    const F32 len = SPECIES[kind].mLength * b.mSize;
    if (b.mSpot == SPOT_WATER) return len * FLOAT_ABOVE;
    return len * BODY_UNDER + (kind == SMALL ? TARSUS_SMALL_M : TARSUS_GULL_M) * b.mSize;
}

/** The drawn water at a point: the region's water height plus the wave the shader draws there now. */
F32 WolfBirds::waterSurfaceZ(const LLVector3& p) const
{
    LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
    if (!r) return surfaceZ(p);
    const F32 wave = WolfBoatRock::instanceExists() ? WolfBoatRock::instance().drawnWaveHeight(r, p.mV[VX], p.mV[VY]) : 0.f;
    return r->getWaterHeight() + wave;
}

F32 WolfBirds::floatZ(Bird& b, const LLVector3& p) const
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F64 since = now - b.mWaveT;
    if (b.mWaveT > 0.0 && since < WAVE_SAMPLE_S) return b.mWaveZ + b.mWaveRate * (F32)since;
    const F32 z = waterSurfaceZ(p);
    b.mWaveRate = (b.mWaveT > 0.0 && since < 0.5) ? (z - b.mWaveZ) / (F32)since : 0.f;
    b.mWaveZ = z;
    b.mWaveT = now;
    return z;
}

bool WolfBirds::perchStill(const Bird& b) const
{
    const LLViewerObject* o = b.mPerch.get();
    if (!o || o->isDead()) return false;
    if ((o->getPositionAgent() - b.mPerchAt).length() > PERCH_MOVED_M) return false;
    // the quaternions' dot is cos(half the angle between them): 0.99999 is about half a degree (chosen: any real turn)
    const LLQuaternion q = o->getRotationRegion();
    return fabsf(q.mQ[VX] * b.mPerchRot.mQ[VX] + q.mQ[VY] * b.mPerchRot.mQ[VY] + q.mQ[VZ] * b.mPerchRot.mQ[VZ]
                 + q.mQ[VW] * b.mPerchRot.mQ[VW]) > 0.99999f;
}

void WolfBirds::leave(Flock& f, const LLVector3& cam)
{
    LLVector3 dir = flat(f.mCentre - cam);
    if (dir.isExactlyZero()) dir.setVec(1.f, 0.f, 0.f);
    dir.normVec();
    f.mLeaving = true;
    f.mRoosting = false;
    f.mDown = false;
    f.mExit = cam + dir * 500.f;
    f.mExit.mV[VZ] = f.mCentre.mV[VZ] + 40.f;
}

/**
 * The perches: now and then (within the ray budget) one ray straight down from CLEAR_ABOVE_M over a prim near a gull flock
 * (mPerchCands, gathered by scanBuilds). The first surface the ray meets is the topmost there, so a perch is never under a
 * roof or inside a room; it is kept when it is a prim (not the terrain, a tree, an avatar or an attachment), its surface
 * tilts no more than 20 degrees, it stands PERCH_MIN_M..PERCH_MAX_M over the ground, its linkset is still (not under way,
 * not a boat the rocker floats) and nothing that would flush a gull is near.
 */
void WolfBirds::seekPerch(F64 now)
{
    for (auto it = mPerches.begin(); it != mPerches.end();)
    {
        const LLViewerObject* o = it->mObject.get();
        if (now - it->mFound > PERCH_KEEP_SECS || !o || o->isDead() || (o->getPositionAgent() - it->mObjectAt).length() > PERCH_MOVED_M)
        {
            it = mPerches.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // the landings' own rays come first: a perch ray only when one is left over for them, and at most one a second
    // (review 2026-11-02: perch rays had taken every token in a town full of candidates)
    if ((S32)mPerches.size() >= MAX_PERCHES_KNOWN || mPerchCands.empty() || mRayTokens < 2.f || now < mNextPerchRay) return;
    mNextPerchRay = now + PERCH_RAY_SECS;
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    std::vector<const Flock*> gulls;
    for (const Flock& f : mFlocks)
    {
        if (f.mKind == GULL && !f.mLeaving) gulls.push_back(&f);
    }
    if (gulls.empty()) return;
    const Flock& gf = *gulls[(size_t)ll_rand((S32)gulls.size())];
    const LLVector3 o = region->getOriginAgent();
    const PerchCandidate* c = nullptr;
    for (S32 k = 0; k < 8 && !c; ++k)
    {
        const PerchCandidate& pc = mPerchCands[(size_t)ll_rand((S32)mPerchCands.size())];
        if (flat(o + pc.mRegionPos - gf.mCentre).length() <= PERCH_SEARCH_M) c = &pc;
    }
    if (!c || !takeRay()) return;
    const LLVector3 aim = o + LLVector3(c->mRegionPos.mV[VX] + frand_range(-c->mHalfX, c->mHalfX) * 0.9f,
                                        c->mRegionPos.mV[VY] + frand_range(-c->mHalfY, c->mHalfY) * 0.9f, c->mTop);
    const F32 ground = LLWorld::getInstance()->resolveLandHeightAgent(aim);
    LLVector4a start, end, hit, normal;
    const LLVector3 s3(aim.mV[VX], aim.mV[VY], aim.mV[VZ] + CLEAR_ABOVE_M), e3(aim.mV[VX], aim.mV[VY], ground - 0.5f);
    start.load3(s3.mV);
    end.load3(e3.mV);
    LLVOVolume::sWolfRoofProbe = true;
    LLViewerObject* obj = gPipeline.lineSegmentIntersectInWorld(start, end, true, false, true, false, nullptr, nullptr, nullptr,
                                                                &hit, nullptr, &normal);
    LLVOVolume::sWolfRoofProbe = false;
    if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME || obj->isAttachment() || obj->getAvatar() || obj->isAvatar()) return;
    LLVector3 n(normal.getF32ptr());
    if (n.normVec() < 1e-3f || n.mV[VZ] < PERCH_NORMAL_Z) return;
    const LLVector3 at(hit.getF32ptr());
    const F32 over = at.mV[VZ] - ground;
    if (over < PERCH_MIN_M || over > PERCH_MAX_M) return;
    LLViewerObject* root = obj->getRootEdit();
    if (!root || root->isAvatar() || root->getVelocity().lengthSquared() > 0.0025f || !root->getAngularVelocity().isExactlyZero()) return;
    if (WolfBoatRock::instanceExists() && WolfBoatRock::instance().isRocking(root)) return;
    if (threatNear(at, GULL, 1.f)) return;
    for (const Perch& p : mPerches)
    {
        if ((p.mAt - at).length() < PERCH_GAP_M) return;
    }
    for (const Bird& b : mBirds)
    {
        if (b.mSpot == SPOT_PERCH && (b.mSpotPos - at).length() < PERCH_GAP_M) return;
    }
    Perch p;
    p.mObject = obj;
    p.mAt = at;
    p.mObjectAt = obj->getPositionAgent();
    p.mObjectRot = obj->getRotationRegion();
    p.mFound = now;
    mPerches.push_back(p);
}

/** A gull aloft picks where to settle: a free perch (if fewer than MAX_PERCHED are taken), the shore near its flock, or the
 *  sea beside a flock-mate already on the water (a raft) or near the flock's centre. Chosen weights: 30 % perch, 30 % shore,
 *  the rest the sea - most gulls seen at a coast are on the water. */
void WolfBirds::chooseLanding(Bird& b, Flock& f, S32 index, F64 now)
{
    (void)now;
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    const F32 r = ll_frand();
    LLVector3 spot;
    U8 kind = SPOT_NONE;
    S32 perched = 0;
    for (const Bird& o : mBirds)
    {
        if (o.mSpot == SPOT_PERCH && (o.mLand == APPROACH || o.mLand == DOWN)) ++perched;
    }
    Perch perch;
    if (r < 0.3f)
    {
        if (perched < MAX_PERCHED && !mPerches.empty())
        {
            size_t best = 0;
            F32 best_d = 1e9f;
            for (size_t k = 0; k < mPerches.size(); ++k)
            {
                const F32 d = (mPerches[k].mAt - b.mPos).length();
                if (d < best_d) { best_d = d; best = k; }
            }
            perch = mPerches[best];
            mPerches.erase(mPerches.begin() + (std::ptrdiff_t)best);
            const LLViewerObject* o = perch.mObject.get();
            if (o && !o->isDead() && (o->getPositionAgent() - perch.mObjectAt).length() <= PERCH_MOVED_M
                && !threatNear(perch.mAt, GULL, RESETTLE_FID_MULT))
            {
                spot = perch.mAt;
                kind = SPOT_PERCH;
            }
        }   // no perch free: the sea (below), not the shore, so the shore keeps its own 30 % (review 2026-11-02)
    }
    else if (r < 0.6f)
    {
        for (S32 k = 0; k < 10; ++k)
        {
            const F32 a = ll_frand(F_TWO_PI), d = frand_range(10.f, SHORE_SEARCH_M);
            LLVector3 p = flat(f.mCentre) + LLVector3(cosf(a) * d, sinf(a) * d, 0.f);
            if (!shoreAt(p) || threatNear(p, GULL, RESETTLE_FID_MULT)) continue;
            p.mV[VZ] = LLWorld::getInstance()->resolveLandHeightAgent(p);
            spot = p;
            kind = SPOT_SHORE;
            break;
        }
    }
    if (kind == SPOT_NONE)
    {
        // a raft: beside a flock-mate already on the water (chosen 2-6 m off), else near the flock's centre
        LLVector3 near_to = f.mCentre;
        for (S32 j = 0; j < (S32)mBirds.size(); ++j)
        {
            const Bird& o = mBirds[(size_t)j];
            if (j != index && o.mFlock == b.mFlock && o.mLand == DOWN && o.mSpot == SPOT_WATER) { near_to = o.mPos; break; }
        }
        const bool raft = (near_to != f.mCentre);
        for (S32 k = 0; k < 6; ++k)
        {
            const F32 a = ll_frand(F_TWO_PI), d = raft ? frand_range(2.f, 6.f) : frand_range(0.f, 25.f);
            LLVector3 p = flat(near_to) + LLVector3(cosf(a) * d, sinf(a) * d, 0.f);
            if (!inRegion(region, p, -8.f) || !overWater(p) || built(p) || threatNear(p, GULL, RESETTLE_FID_MULT)) continue;
            LLViewerRegion* pr = LLWorld::getInstance()->getRegionFromPosAgent(p);
            p.mV[VZ] = pr ? pr->getWaterHeight() : region->getWaterHeight();
            spot = p;
            kind = SPOT_WATER;
            break;
        }
    }
    if (kind == SPOT_NONE)
    {
        b.mNextLand = frand_range(3.f, 8.f);   // nowhere this time: look again soon
        return;
    }
    if (kind != SPOT_PERCH)
    {
        const S32 clear = spotClear(spot);
        if (clear < 0) { b.mNextLand = 0.3f; return; }                    // no ray left this moment
        if (clear == 0) { b.mNextLand = frand_range(3.f, 8.f); return; }  // something over it: not there
    }
    b.mLand = APPROACH;
    b.mSpot = kind;
    b.mSpotPos = spot;
    b.mActFor = APPROACH_TIMEOUT_S;
    if (kind == SPOT_PERCH)
    {
        b.mPerch = perch.mObject;
        b.mPerchAt = perch.mObjectAt;
        b.mPerchRot = perch.mObjectRot;
    }
}

/** A small flock drops to feed: a patch of open flat land near it, away from anything that flushed it, open to the sky;
 *  every bird takes its own spot within PATCH_R_M of it. */
void WolfBirds::settleFlock(Flock& f, S32 flock_index)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region) return;
    LLVector3 c;
    S32 n = 0;
    for (const Bird& b : mBirds)
    {
        if (b.mFlock == flock_index && b.mLand == AIR) { c += b.mPos; ++n; }
    }
    if (n == 0) return;
    c /= (F32)n;
    const F32 keep_off = FLUSH_PERSON_SMALL_M * RESETTLE_FID_MULT;
    for (S32 k = 0; k < 10; ++k)
    {
        // chosen: within 40 m of where the flock is, "nearby"
        const F32 a = ll_frand(F_TWO_PI), d = frand_range(0.f, 40.f);
        LLVector3 p = flat(c) + LLVector3(cosf(a) * d, sinf(a) * d, 0.f);
        if (f.mFlushed && flat(p - f.mFlushedFrom).length() < keep_off) continue;
        if (!grassAt(p) || threatNear(p, SMALL, RESETTLE_FID_MULT)) continue;
        p.mV[VZ] = LLWorld::getInstance()->resolveLandHeightAgent(p);
        const S32 clear = spotClear(p);
        if (clear < 0) { f.mSettleIn = 0.3f; return; }
        if (clear == 0) continue;
        f.mDown = true;
        f.mPatch = p;
        f.mFeedFor = frand_range(SMALL_FEED_MIN_S, SMALL_FEED_MAX_S);
        f.mFlushed = false;
        for (Bird& b : mBirds)
        {
            if (b.mFlock != flock_index || b.mLand != AIR) continue;
            const F32 ba = ll_frand(F_TWO_PI), bd = PATCH_R_M * sqrtf(ll_frand());
            LLVector3 s = p + LLVector3(cosf(ba) * bd, sinf(ba) * bd, 0.f);
            if (!grassAt(s)) s = p;
            s.mV[VZ] = LLWorld::getInstance()->resolveLandHeightAgent(s);
            b.mLand = APPROACH;
            b.mSpot = SPOT_GRASS;
            b.mSpotPos = s;
            b.mActFor = APPROACH_TIMEOUT_S;
        }
        return;
    }
    f.mSettleIn = frand_range(5.f, 10.f);   // nowhere suitable here: look again later
}

/** The whole small flock lifts together; then about half the time it leaves for good (LEAVE_AFTER_LIFT), else it re-lands
 *  nearby after a short circuit (chosen 4-10 s; after a flush, once it is no longer wary). */
void WolfBirds::liftFlock(Flock& f, S32 flock_index, bool alarm, const LLVector3& from)
{
    for (Bird& b : mBirds)
    {
        if (b.mFlock == flock_index) takeOff(b, alarm, from);
    }
    f.mDown = false;
    if (ll_frand() < LEAVE_AFTER_LIFT)
    {
        leave(f, LLViewerCamera::getInstance()->getOrigin());
        return;
    }
    f.mSettleIn = frand_range(4.f, 10.f);
}

void WolfBirds::takeOff(Bird& b, bool alarm, const LLVector3& from)
{
    if (b.mLand == AIR) return;
    if (b.mLand == TAKEOFF)
    {
        if (alarm && !b.mAlarm) { b.mAlarm = true; b.mAlarmFor = ALARM_CLIMB_S; }
        return;
    }
    const bool was_down = (b.mLand == DOWN);
    const bool on_water = was_down && b.mSpot == SPOT_WATER;
    if (alarm)
    {
        const LLVector3 away = flat(b.mPos - from);
        if (!away.isExactlyZero()) b.mYaw = atan2f(away.mV[VY], away.mV[VX]);
    }
    const LLVector3 fwd(cosf(b.mYaw), sinf(b.mYaw), 0.f);
    b.mPerch = nullptr;
    b.mSpot = SPOT_NONE;
    b.mAct = ACT_REST;
    b.mPitch = 0.f;
    b.mLift = 0.f;
    b.mAlarm = alarm;
    b.mAlarmFor = alarm ? ALARM_CLIMB_S : 0.f;
    b.mRising = true;
    b.mRiseT = 0.f;
    b.mFlapFor = 2.f;
    b.mGlideFor = 0.f;
    b.mNextLand = frand_range(GULL_LAND_MIN_S, GULL_LAND_MAX_S) + (alarm ? frand_range(WARY_MIN_S, WARY_MAX_S) : 0.f);
    if (on_water)
    {
        b.mLand = TAKEOFF;
        b.mRun = TAKEOFF_RUN_S;
        b.mVel = fwd * 0.5f;
    }
    else if (was_down)
    {
        b.mLand = AIR;
        b.mVel = fwd * (b.mSpeed * 0.4f) + LLVector3(0.f, 0.f, SPRING_MS);
    }
    else
    {
        b.mLand = AIR;   // an approach given up: it is flying already
    }
}

/** Something came too near bird `index`: it takes off in alarm with a call; a small flock lifts as one, a gull lifts the
 *  flock-mates down within ALARM_SPREAD_M of it, and the gull flock's centre moves out over the sea, away. */
void WolfBirds::flush(S32 index, const LLVector3& from)
{
    Bird& b = mBirds[(size_t)index];
    const S32 fi = b.mFlock;
    Flock& f = mFlocks[(size_t)fi];
    f.mNextCall = 0.0;   // the alarm call, at once (calls())
    f.mFlushed = true;
    f.mFlushedFrom = from;
    f.mWary = frand_range(WARY_MIN_S, WARY_MAX_S);
    if (f.mKind == SMALL)
    {
        liftFlock(f, fi, true, from);
        return;
    }
    const LLVector3 at = b.mPos;
    for (Bird& o : mBirds)
    {
        if (o.mFlock == fi && o.mLand != AIR && (o.mPos - at).length() <= ALARM_SPREAD_M) takeOff(o, true, from);
    }
    LLVector3 away = flat(f.mCentre - from);
    if (away.isExactlyZero()) away.setVec(1.f, 0.f, 0.f);
    away.normVec();
    LLVector3 c = flat(from) + away * (FLUSH_PERSON_GULL_M * RESETTLE_FID_MULT + frand_range(20.f, 50.f));
    if (inRegion(gAgent.getRegion(), c, -8.f) && overWater(c) && !built(c))
    {
        c.mV[VZ] = surfaceZ(c);
        f.mCentre = c;
    }
}

/**
 * Four times a second (FLUSH_CHECK_SECS): what could flush a bird - every avatar the viewer knows (LLWorld::getAvatars,
 * the agent's own included), every moving root object (gObjectList's active objects: a vehicle or boat over
 * VEHICLE_MIN_MS, a plane over AIRCRAFT_MIN_MS; the speed is the larger of the object's velocity and its own movement
 * since the last check, so a keyframed or scripted mover counts too) and the camera - and every bird down or coming down
 * within that thing's flush distance takes off.
 */
void WolfBirds::flushCheck(F64 now)
{
    if (now < mNextFlush) return;
    mNextFlush = now + FLUSH_CHECK_SECS;
    const F64 since = mMoverLastT > 0.0 ? now - mMoverLastT : 0.0;
    mMoverLastT = now;
    mThreats.clear();
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    // birds live within GONE_M x 1.6 of the camera (move()); anything further cannot reach one
    const F32 reach = GONE_M * 1.6f + FLUSH_VEHICLE_M;
    LLWorld::getInstance()->getAvatars(nullptr, &mAvatarScratch, gAgent.getPosGlobalFromAgent(cam), reach);
    for (const LLVector3d& g : mAvatarScratch)
    {
        Threat t;
        t.mPos = gAgent.getPosAgentFromGlobal(g);
        t.mKind = THREAT_PERSON;
        mThreats.push_back(t);
    }
    std::map<LLUUID, LLVector3d> seen;
    for (const LLPointer<LLViewerObject>& op : gObjectList.getActiveObjects())
    {
        LLViewerObject* obj = op.get();
        if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || obj->getParent()) continue;
        const LLVector3 pos = obj->getPositionAgent();
        if ((pos - cam).length() > reach) continue;
        const LLVector3d g = obj->getPositionGlobal();
        F32 speed = obj->getVelocity().length();
        auto it = mMoverLast.find(obj->getID());
        if (it != mMoverLast.end() && since > 0.05) speed = llmax(speed, (F32)((g - it->second).length() / since));
        seen[obj->getID()] = g;
        if (speed < VEHICLE_MIN_MS) continue;
        Threat t;
        t.mPos = pos;
        t.mKind = speed >= AIRCRAFT_MIN_MS ? THREAT_AIRCRAFT : THREAT_VEHICLE;
        mThreats.push_back(t);
    }
    mMoverLast.swap(seen);
    Threat ct;
    ct.mPos = cam;
    ct.mKind = THREAT_CAMERA;
    mThreats.push_back(ct);

    for (S32 i = 0; i < (S32)mBirds.size(); ++i)
    {
        const Bird& b = mBirds[(size_t)i];
        if (b.mLand != DOWN && b.mLand != APPROACH) continue;
        const Kind kind = mFlocks[(size_t)b.mFlock].mKind;
        for (const Threat& t : mThreats)
        {
            const F32 r = flushRadius(t.mKind, kind);
            if ((t.mPos - b.mPos).lengthSquared() < r * r)
            {
                flush(i, t.mPos);
                break;
            }
        }
    }
}

void WolfBirds::moveApproach(Bird& b, Flock& f, F32 dt)
{
    const Kind kind = f.mKind;
    b.mActFor -= dt;
    if (b.mActFor <= 0.f || (b.mSpot == SPOT_PERCH && !perchStill(b)))
    {
        takeOff(b, false, b.mPos);   // it could not get down there: fly on
        return;
    }
    LLVector3 goal = b.mSpotPos;
    goal.mV[VZ] = (b.mSpot == SPOT_WATER ? floatZ(b, goal) : b.mSpotPos.mV[VZ]) + standHeight(b, kind);
    const LLVector3 to = goal - b.mPos;
    const F32 dh = flat(to).length();
    if (dh < 0.3f && fabsf(to.mV[VZ]) < 0.3f)
    {
        // touch down
        const LLVector3 v = flat(b.mVel);
        if (!v.isExactlyZero()) b.mYaw = atan2f(v.mV[VY], v.mV[VX]);
        b.mLand = DOWN;
        b.mPos = goal;
        b.mVel.clear();
        b.mBank = 0.f;
        b.mFlapFor = 0.f;
        b.mGlideFor = 0.f;
        b.mAct = ACT_REST;
        b.mActFor = frand_range(1.f, 3.f);
        b.mDownFor = frand_range(GULL_DOWN_MIN_S, GULL_DOWN_MAX_S);
        b.mStep = 0.f;
        b.mAlarm = false;
        b.mAlarmFor = 0.f;
        return;
    }
    if (dh < 2.f)
    {
        // the last two metres: straight in, slowing to a stop (no orbiting round the spot)
        LLVector3 v = to * 2.5f;
        const F32 m = v.length();
        if (m > 3.f) v *= 3.f / m;
        b.mVel = v;
    }
    else
    {
        // a glide slope of 1 in 3 down to the spot, slowing as it comes in
        LLVector3 aim = goal;
        aim.mV[VZ] += llmin(dh * GLIDE_SLOPE, 40.f);
        LLVector3 want = aim - b.mPos;
        const F32 len = llmax(want.length(), 1e-3f);
        const F32 speed = llclamp(dh * 0.6f + 0.6f, 1.f, b.mSpeed);
        want *= speed / len;
        steer(b, want, kind == SMALL ? 14.f : 8.f, dt);
    }
    // a gull glides in and beats in the last few metres (the flare); a small bird flaps
    b.mFlapFor = (kind != GULL || dh < 6.f) ? 0.2f : 0.f;
    b.mGlideFor = 0.f;
}

void WolfBirds::moveDown(Bird& b, Flock& f, F32 dt)
{
    const Kind kind = f.mKind;
    if (b.mSpot == SPOT_PERCH && !perchStill(b))
    {
        // the perch moved or went under it: off in alarm, with a call
        f.mNextCall = 0.0;
        takeOff(b, true, b.mPos);
        return;
    }
    if (kind == GULL)
    {
        if (!mSitTight) b.mDownFor -= dt;
        if (b.mDownFor <= 0.f)
        {
            takeOff(b, false, b.mPos);
            return;
        }
    }
    b.mActFor -= dt;
    if (b.mActFor <= 0.f)
    {
        const F32 r = ll_frand();
        switch (b.mSpot)
        {
        case SPOT_GRASS:
            // head up (scanning, or hopping on) in SCAN_BOUT_S bouts, head down pecking in PECK_BOUT_S bouts
            if (b.mAct == ACT_PECK)
            {
                b.mAct = (r < 0.5f) ? ACT_MOVE : ACT_REST;
                b.mActFor = SCAN_BOUT_S * frand_range(0.5f, 1.5f);
            }
            else
            {
                b.mAct = ACT_PECK;
                b.mActFor = PECK_BOUT_S * frand_range(0.5f, 1.5f);
            }
            break;
        case SPOT_WATER:   // chosen: mostly floating, some paddling, a little preening; 3-10 s each
            b.mAct = (r < 0.6f) ? ACT_REST : (r < 0.9f ? ACT_MOVE : ACT_PREEN);
            b.mActFor = frand_range(3.f, 10.f);
            b.mTurn = frand_range(-PADDLE_TURN, PADDLE_TURN);
            break;
        case SPOT_SHORE:   // chosen: standing, a few steps, preening
            b.mAct = (r < 0.5f) ? ACT_REST : (r < 0.8f ? ACT_MOVE : ACT_PREEN);
            b.mActFor = (b.mAct == ACT_MOVE) ? frand_range(1.5f, 4.f) : frand_range(3.f, 10.f);
            if (b.mAct == ACT_MOVE) b.mYaw += frand_range(-1.2f, 1.2f);
            break;
        default:           // a perch: standing (turning now and then), preening
            b.mAct = (r < 0.75f) ? ACT_REST : ACT_PREEN;
            b.mActFor = frand_range(4.f, 12.f);
            if (b.mAct == ACT_REST && ll_frand() < 0.5f) b.mYaw += frand_range(-0.7f, 0.7f);
            break;
        }
        b.mStep = 0.f;
    }

    LLVector3 p = b.mPos;
    const LLVector3 fwd(cosf(b.mYaw), sinf(b.mYaw), 0.f);
    F32 pitch = 0.f;
    b.mLift = 0.f;
    switch (b.mAct)
    {
    case ACT_MOVE:
        if (b.mSpot == SPOT_WATER)
        {
            const LLVector3 next = p + fwd * (PADDLE_MS * dt);
            b.mYaw += b.mTurn * dt;
            if (!overWater(next) || built(next) || !inRegion(gAgent.getRegion(), next, -4.f)) b.mYaw += F_PI;
            else p = next;
        }
        else if (b.mSpot == SPOT_SHORE)
        {
            const LLVector3 next = p + fwd * (WALK_MS * dt);
            if (!shoreAt(next) || threatNear(next, kind, 1.f)) { b.mAct = ACT_REST; b.mYaw += frand_range(2.f, 4.f); }
            else { p = next; b.mStep += dt * WALK_MS / (TARSUS_GULL_M * 2.f * b.mSize); }   // a stride of two leg lengths (chosen)
        }
        else if (b.mSpot == SPOT_GRASS)
        {
            const F32 before = b.mStep;
            b.mStep += dt / HOP_S;
            if (floorf(b.mStep) != floorf(before))
            {
                // a new hop: now and then a turn (chosen up to 35 degrees)
                b.mYaw += frand_range(-0.6f, 0.6f);
            }
            const LLVector3 next = p + fwd * (HOP_M / HOP_S * dt);
            if (!grassAt(next)) b.mYaw += F_PI;
            else p = next;
            b.mLift = HOP_H * sinf(F_PI * (b.mStep - floorf(b.mStep)));
        }
        break;
    case ACT_PECK:
        // the head goes down and back up at PECK_HZ, never quite up while the bout lasts
        b.mStep += dt * PECK_HZ;
        pitch = PECK_PITCH_DEG * DEG_TO_RAD * (0.55f + 0.45f * sinf(F_TWO_PI * b.mStep));
        break;
    case ACT_PREEN:
        b.mStep += dt;
        pitch = (25.f + 10.f * sinf(b.mStep * 3.f)) * DEG_TO_RAD;   // head down into the feathers (chosen, the look of it)
        break;
    default:
        break;
    }
    // the bill reaches the surface and no further: a body of half-length L pitched by a, standing h over the surface,
    // brings its tip down by L sin a, so a is held to asin(h / L) (the legs do not bend here)
    const F32 half_len = 0.5f * SPECIES[kind].mLength * b.mSize;
    pitch = llmin(pitch, asinf(llclamp(standHeight(b, kind) / half_len, 0.f, 1.f)));
    b.mPitch += (pitch - b.mPitch) * llmin(1.f, dt * 10.f);
    // exactly on the surface under it
    F32 z = 0.f;
    switch (b.mSpot)
    {
    case SPOT_WATER: z = floatZ(b, p); break;
    case SPOT_PERCH: z = b.mSpotPos.mV[VZ]; break;
    default:         z = LLWorld::getInstance()->resolveLandHeightAgent(p) + b.mLift; break;
    }
    p.mV[VZ] = z + standHeight(b, kind);
    b.mPos = p;
    b.mVel.clear();
}

/** A gull off the water: a short run along the surface, beating, to TAKEOFF_RUN_SPEED of its cruise, then into the air. */
void WolfBirds::moveTakeOff(Bird& b, Flock& f, F32 dt)
{
    b.mRun -= dt;
    const LLVector3 fwd(cosf(b.mYaw), sinf(b.mYaw), 0.f);
    const F32 top = b.mSpeed * TAKEOFF_RUN_SPEED;
    const F32 v = llmin(top, flat(b.mVel).length() + top / TAKEOFF_RUN_S * dt);
    b.mVel = fwd * v;
    b.mFlapFor = 0.5f;
    LLVector3 p = b.mPos + b.mVel * dt;
    const F32 frac = 1.f - llmax(b.mRun, 0.f) / TAKEOFF_RUN_S;
    // the feet patter on the water while the body rises a little (chosen: 0.3 m over the run)
    p.mV[VZ] = floatZ(b, p) + SPECIES[f.mKind].mLength * b.mSize * FLOAT_ABOVE + 0.3f * frac;
    b.mPos = p;
    if (b.mRun <= 0.f)
    {
        b.mLand = AIR;
        b.mVel.mV[VZ] = b.mAlarm ? b.mSpeed * ALARM_CLIMB_FRAC : 1.5f;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// flying
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::steer(Bird& b, const LLVector3& desired_vel, F32 max_accel, F32 dt)
{
    LLVector3 dv = desired_vel - b.mVel;
    const F32 m = dv.length();
    if (m > max_accel * dt) dv *= (max_accel * dt) / m;
    const LLVector3 old = b.mVel;
    b.mVel += dv;
    // Bank into the turn: tan(bank) = v * turn rate / g (a coordinated turn).
    const F32 v = llmax(1.f, flat(b.mVel).length());
    const F32 turn = (flat(old) % flat(b.mVel)).mV[VZ] / (llmax(1.f, flat(old).length()) * v * llmax(dt, 1e-3f));
    const F32 bank = llclamp(atanf(v * turn / BIRD_G_MS2), -1.0f, 1.0f);
    b.mBank += (bank - b.mBank) * llmin(1.f, dt * 4.f);
}

/** Every bird: keep above the surface below, and out of built cells: pushed away from every built cell of the 5 x 5 round
 *  the point 15 m ahead of it, and climbing while over one, unless it is already 50 m above it. */
void WolfBirds::keepClear(Bird& b, F32 dt)
{
    const F32 ground = surfaceZ(b.mPos);
    if (!b.mRising)
    {
        if (b.mPos.mV[VZ] < ground + 1.5f)
        {
            b.mPos.mV[VZ] = ground + 1.5f;
            if (b.mVel.mV[VZ] < 0.f) b.mVel.mV[VZ] = 0.f;
        }
    }
    else
    {
        // [BIRDS LANDING 2026-11-02] just off the ground or water: the floor over the surface under it rises from 0.1 m (about
        // the height a bird stands at, chosen) at RISE_MS to the usual 1.5 m, so it climbs out instead of jumping up in one
        // frame, and follows rising ground; over water the floor is the drawn wave, not the rest level.
        b.mRiseT += dt;
        const F32 floor = llmin(1.5f, 0.1f + RISE_MS * b.mRiseT);
        const F32 base = overWater(b.mPos) ? llmax(ground, floatZ(b, b.mPos)) : ground;
        if (b.mPos.mV[VZ] < base + floor)
        {
            b.mPos.mV[VZ] = base + floor;
            if (b.mVel.mV[VZ] < RISE_MS) b.mVel.mV[VZ] = RISE_MS;
        }
        if (floor >= 1.5f) b.mRising = false;
    }
    LLViewerRegion* r = gAgent.getRegion();
    if (!r || !mHaveBuilt || mCells <= 0 || b.mPos.mV[VZ] > ground + 50.f) return;
    LLVector3 dir = flat(b.mVel);
    if (dir.isExactlyZero()) dir.setVec(1.f, 0.f, 0.f);
    dir.normVec();
    const LLVector3 o = r->getOriginAgent();
    const LLVector3 ahead = b.mPos + dir * 15.f;
    const S32 cx0 = (S32)floorf((ahead.mV[VX] - o.mV[VX]) / CELL_M), cy0 = (S32)floorf((ahead.mV[VY] - o.mV[VY]) / CELL_M);
    F32 rx = 0.f, ry = 0.f;
    bool hit = false;
    for (S32 cy = cy0 - 2; cy <= cy0 + 2; ++cy)
    {
        for (S32 cx = cx0 - 2; cx <= cx0 + 2; ++cx)
        {
            if (cx < 0 || cy < 0 || cx >= mCells || cy >= mCells) continue;
            if ((F32)mBuilt[(size_t)(cy * mCells + cx)] < BUILT_WEIGHT) continue;
            const F32 dx = b.mPos.mV[VX] - (o.mV[VX] + ((F32)cx + 0.5f) * CELL_M);
            const F32 dy = b.mPos.mV[VY] - (o.mV[VY] + ((F32)cy + 0.5f) * CELL_M);
            const F32 d2 = llmax(16.f, dx * dx + dy * dy);
            rx += dx / d2;
            ry += dy / d2;
            hit = true;
        }
    }
    if (!hit) return;
    const F32 k = 600.f * dt;   // the same push as wolf_birds.js _keepClear (tested there: none low over a town)
    b.mVel.mV[VX] += rx * k;
    b.mVel.mV[VY] += ry * k;
    if (built(b.mPos)) b.mVel.mV[VZ] += 6.f * dt;
}

void WolfBirds::moveGull(Bird& b, Flock& f, F32 dt)
{
    // Soaring circles round its own centre near the flock's, which drifts over the sea.
    const LLVector3 c = f.mCentre + b.mOrbitOffset;
    LLVector3 r = flat(b.mPos - c);
    if (r.isExactlyZero()) r.setVec(1.f, 0.f, 0.f);
    const F32 ang = atan2f(r.mV[VY], r.mV[VX]) + b.mOrbitDir * 0.6f;   // aim a little round the circle
    LLVector3 aim(c.mV[VX] + cosf(ang) * b.mOrbitR, c.mV[VY] + sinf(ang) * b.mOrbitR, 0.f);
    aim.mV[VZ] = surfaceZ(aim) + b.mAlt;
    LLVector3 want = aim - b.mPos;
    want.normVec();
    want *= b.mSpeed;
    want.mV[VZ] = llclamp(want.mV[VZ], -1.5f, 1.5f);
    steer(b, want, 4.f, dt);
    // Glide most of the time; a few beats now and then, and whenever it has to climb.
    if (b.mFlapFor > 0.f) b.mFlapFor -= dt;
    else if (b.mGlideFor > 0.f) b.mGlideFor -= dt;
    else if (ll_frand() < 0.5f) b.mFlapFor = frand_range(1.2f, 2.8f);
    else b.mGlideFor = frand_range(5.f, 14.f);
    if (b.mVel.mV[VZ] > 0.6f && b.mFlapFor <= 0.f) b.mFlapFor = 1.f;
    b.mAlt += frand_range(-1.f, 1.f) * dt;
    b.mAlt = llclamp(b.mAlt, SPECIES[GULL].mAltMin, SPECIES[GULL].mAltMax);
}

void WolfBirds::moveTern(Bird& b, Flock& f, F32 dt)
{
    // Patrol low over the water, flapping; now and then hover, then plunge to the surface and climb away.
    const F32 surface = surfaceZ(b.mPos);
    b.mStateFor -= dt;
    switch (b.mState)
    {
    case 0:   // patrol
    {
        LLVector3 to = b.mWaypoint - b.mPos;
        if (flat(to).length() < 8.f || b.mStateFor < -30.f)
        {
            b.mWaypoint = f.mCentre + LLVector3(frand_range(-50.f, 50.f), frand_range(-50.f, 50.f), 0.f);
            if (!overWater(b.mWaypoint)) b.mWaypoint = f.mCentre;
            b.mStateFor = 0.f;
        }
        LLVector3 aim = b.mWaypoint;
        aim.mV[VZ] = surfaceZ(aim) + b.mAlt + 1.2f * sinf(b.mPhase * 0.08f);
        LLVector3 want = aim - b.mPos;
        want.normVec();
        want *= b.mSpeed;
        steer(b, want, 6.f, dt);
        if (ll_frand() < 0.04f * dt && overWater(b.mPos)) { b.mState = 1; b.mStateFor = frand_range(1.2f, 2.2f); }
        break;
    }
    case 1:   // hover
        steer(b, LLVector3(0.f, 0.f, 0.f) + flat(b.mVel) * 0.15f, 8.f, dt);
        if (b.mStateFor <= 0.f) b.mState = (ll_frand() < 0.6f) ? 2 : 0;
        break;
    case 2:   // plunge
        steer(b, LLVector3(flat(b.mVel).mV[VX] * 0.3f, flat(b.mVel).mV[VY] * 0.3f, -7.f), 20.f, dt);
        if (b.mPos.mV[VZ] <= surface + 1.6f) { b.mState = 3; b.mStateFor = 2.f; }
        break;
    default:  // climb away
        steer(b, LLVector3(b.mVel.mV[VX], b.mVel.mV[VY], 0.f) * 0.5f + LLVector3(0.f, 0.f, 3.f) + flat(f.mCentre - b.mPos) * 0.05f, 8.f, dt);
        if (b.mStateFor <= 0.f) b.mState = 0;
        break;
    }
    b.mFlapFor = 1.f;   // terns flap all the time
}

/** Small flocks and starlings: boids (separation, alignment, cohesion) after a moving target. Starlings take their seven
 *  nearest of a sample of the flock (Ballerini et al. 2008: each starling follows about seven neighbours). */
void WolfBirds::moveFlocking(Bird& b, Flock& f, S32 index, F32 dt)
{
    const bool starling = (f.mKind == STARLING);
    LLVector3 sep, ali, coh;
    S32 n = 0;
    const S32 count = (S32)mBirds.size();
    if (starling)
    {
        // seven nearest of up to 24 random flock-mates
        F32 best_d[7];
        S32 best_i[7];
        for (S32 k = 0; k < 7; ++k) { best_d[k] = 1e9f; best_i[k] = -1; }
        for (S32 s = 0; s < 24; ++s)
        {
            const S32 j = ll_rand(count);
            if (j == index || mBirds[j].mFlock != b.mFlock) continue;
            const F32 d = (mBirds[j].mPos - b.mPos).lengthSquared();
            for (S32 k = 0; k < 7; ++k)
            {
                if (best_i[k] == j) break;
                if (d < best_d[k])
                {
                    for (S32 m = 6; m > k; --m) { best_d[m] = best_d[m - 1]; best_i[m] = best_i[m - 1]; }
                    best_d[k] = d; best_i[k] = j;
                    break;
                }
            }
        }
        for (S32 k = 0; k < 7; ++k)
        {
            if (best_i[k] < 0) continue;
            const Bird& o = mBirds[best_i[k]];
            const LLVector3 d = b.mPos - o.mPos;
            const F32 len = d.length();
            if (len < 1.2f && len > 1e-3f) sep += d / (len * len);
            ali += o.mVel;
            coh += o.mPos;
            ++n;
        }
    }
    else
    {
        for (S32 j = 0; j < count; ++j)
        {
            const Bird& o = mBirds[j];
            if (j == index || o.mFlock != b.mFlock || o.mLand != AIR) continue;   // [BIRDS LANDING/] not the ones down
            const LLVector3 d = b.mPos - o.mPos;
            const F32 len = d.length();
            if (len > 12.f) continue;
            if (len < 1.0f && len > 1e-3f) sep += d / (len * len);
            ali += o.mVel;
            coh += o.mPos;
            ++n;
        }
    }
    LLVector3 want = b.mVel;
    if (n > 0)
    {
        ali /= (F32)n;
        coh /= (F32)n;
        want += (ali - b.mVel) * (starling ? 0.9f : 0.5f);
        want += (coh - b.mPos) * (starling ? 0.25f : 0.4f);
        want += sep * 2.5f;
    }
    // the flock's target, with its height over the ground
    LLVector3 aim = f.mTarget;
    if (!f.mRoosting) aim.mV[VZ] = surfaceZ(aim) + b.mAlt;
    LLVector3 to = aim - b.mPos;
    const F32 dist = to.length();
    const F32 pull = starling ? llclamp((dist - 25.f) / 40.f, 0.05f, 1.f) : 0.6f;
    if (dist > 1e-3f) want += to / dist * b.mSpeed * pull;
    // cruise at its own speed
    const F32 sp = want.length();
    if (sp > 1e-3f) want *= b.mSpeed / sp;
    if (!f.mRoosting) want.mV[VZ] = llclamp(want.mV[VZ], -3.f, 3.f);
    steer(b, want, starling ? 14.f : 10.f, dt);

    // wings: small birds bound (a burst of beats, then a closed-wing glide); starlings beat with short glides
    if (b.mFlapFor > 0.f) { b.mFlapFor -= dt; if (b.mFlapFor <= 0.f) b.mGlideFor = starling ? frand_range(0.3f, 0.9f) : frand_range(0.18f, 0.32f); }
    else if (b.mGlideFor > 0.f) { b.mGlideFor -= dt; b.mVel.mV[VZ] -= (starling ? 0.5f : 1.5f) * dt; }
    else b.mFlapFor = starling ? frand_range(0.8f, 2.0f) : frand_range(0.28f, 0.45f);
}

void WolfBirds::move(F32 dt, F64 now)
{
    // the flocks' own movement
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLViewerRegion* region = gAgent.getRegion();
    // [BIRDS MID 2026-11-02] ticket 463: a drifting centre keeps within the reach + 30 m (FAR_M + 30 m as before when the
    // draw distance is long; inside the draw distance less 10 m when it is short, see flock_reach_m)
    const F32 drift_max = flock_reach_m() + 30.f;
    // [BIRDS LANDING 2026-11-02] the flocks' landing: a leaving flock's birds take off first; a small flock drops to feed
    // and lifts as one; gulls (below, one by one) look for somewhere to settle after a while aloft
    for (S32 fi = 0; fi < (S32)mFlocks.size(); ++fi)
    {
        Flock& f = mFlocks[(size_t)fi];
        f.mWary = llmax(0.f, f.mWary - dt);
        if (f.mLeaving || f.mRoosting)
        {
            for (Bird& b : mBirds)
            {
                if (b.mFlock == fi && (b.mLand == DOWN || b.mLand == APPROACH)) takeOff(b, false, b.mPos);
            }
            f.mDown = false;
            continue;
        }
        if (f.mKind != SMALL) continue;
        if (!f.mDown)
        {
            if (mSitTight) f.mSettleIn = llmin(f.mSettleIn, SIT_TIGHT_LAND_S);
            f.mSettleIn -= dt;
            if (f.mSettleIn <= 0.f && f.mWary <= 0.f) settleFlock(f, fi);
            continue;
        }
        bool approaching = false, any = false;
        for (const Bird& b : mBirds)
        {
            if (b.mFlock != fi) continue;
            any = true;
            if (b.mLand == APPROACH) approaching = true;
        }
        if (!any) continue;
        if (!approaching && !mSitTight) f.mFeedFor -= dt;
        if (f.mFeedFor <= 0.f) liftFlock(f, fi, false, f.mPatch);
    }
    for (Flock& f : mFlocks)
    {
        if (f.mLeaving) { f.mTarget = f.mExit; continue; }
        if (f.mKind == STARLING)
        {
            // they wheel round a slow loop over the roost; going to roost, the target sinks to the ground
            f.mLoop += dt * 0.12f;
            if (f.mRoosting)
            {
                f.mTarget = f.mCentre;
                f.mTarget.mV[VZ] = surfaceZ(f.mCentre) + 0.5f;
            }
            else
            {
                f.mTarget = f.mCentre + LLVector3(cosf(f.mLoop) * 55.f, sinf(f.mLoop * 1.7f) * 40.f, 0.f);
            }
            continue;
        }
        if (now >= f.mRetarget)
        {
            f.mRetarget = now + frand_range(8.f, 20.f);
            // drift the centre a little over the same kind of place, and pick the next target near it
            LLVector3 c = f.mCentre + LLVector3(frand_range(-25.f, 25.f), frand_range(-25.f, 25.f), 0.f);
            const bool sea_kind = (f.mKind == GULL || f.mKind == TERN);
            if (inRegion(region, c, -8.f) && overWater(c) == sea_kind && !built(c) && (flat(c - cam).length() < drift_max))
            {
                c.mV[VZ] = surfaceZ(c);
                f.mCentre = c;
            }
            f.mTarget = f.mCentre + LLVector3(frand_range(-40.f, 40.f), frand_range(-40.f, 40.f), 0.f);
            if (built(f.mTarget) || overWater(f.mTarget) != sea_kind) f.mTarget = f.mCentre;
        }
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    for (S32 i = 0; i < (S32)mBirds.size(); ++i)
    {
        Bird& b = mBirds[i];
        Flock& f = mFlocks[b.mFlock];
        // [BIRDS LANDING 2026-11-02] coming down, down, or taking off
        if (b.mLand == DOWN) { moveDown(b, f, dt); continue; }
        if (b.mLand == TAKEOFF)
        {
            moveTakeOff(b, f, dt);
        }
        else if (b.mLand == APPROACH)
        {
            moveApproach(b, f, dt);
            if (b.mLand == DOWN) continue;
            b.mPos += b.mVel * dt;
            // never through the ground or water on the way in (over water the drawn wave, as its goal: review 2026-11-02)
            const F32 base = (b.mSpot == SPOT_WATER && overWater(b.mPos)) ? llmax(surfaceZ(b.mPos), floatZ(b, b.mPos))
                                                                       : surfaceZ(b.mPos);
            const F32 floor_z = base + standHeight(b, f.mKind);
            if (b.mPos.mV[VZ] < floor_z) b.mPos.mV[VZ] = floor_z;
        }
        else
        {
            if (f.mKind == GULL && !f.mLeaving && f.mWary <= 0.f)
            {
                if (mSitTight) b.mNextLand = llmin(b.mNextLand, SIT_TIGHT_LAND_S);
                b.mNextLand -= dt;
                if (b.mNextLand <= 0.f) chooseLanding(b, f, i, now);
            }
            if (f.mLeaving)
            {
                moveFlocking(b, f, i, dt);
            }
            else
            {
                switch (f.mKind)
                {
                case GULL: moveGull(b, f, dt); break;
                case TERN: moveTern(b, f, dt); break;
                default:   moveFlocking(b, f, i, dt); break;
                }
            }
            if (b.mAlarmFor > 0.f)
            {
                // the alarm: a quick climb away, beating
                b.mAlarmFor -= dt;
                const F32 climb = b.mSpeed * ALARM_CLIMB_FRAC;
                b.mVel.mV[VZ] += (climb - b.mVel.mV[VZ]) * llmin(1.f, dt * 4.f);
                b.mFlapFor = llmax(b.mFlapFor, 0.3f);
                if (b.mAlarmFor <= 0.f) b.mAlarm = false;
            }
            b.mPos += b.mVel * dt;
            if (!(f.mRoosting)) keepClear(b, dt);
        }
        const Species& sp = SPECIES[f.mKind];
        const F32 beat = (b.mFlapFor > 0.f) ? sp.mBeatHz : 0.f;
        b.mPhase += F_TWO_PI * beat * dt;   // a glide holds the phase; buildMesh sets the gliding wings itself
        if (b.mPhase > F_TWO_PI * 64.f) b.mPhase -= F_TWO_PI * 64.f;
    }

    // drop: leaving birds far off, roosting starlings that reached the ground, flocks with no birds
    std::vector<S32> live(mFlocks.size(), 0);
    size_t w = 0;
    for (size_t i = 0; i < mBirds.size(); ++i)
    {
        Bird& b = mBirds[i];
        const Flock& f = mFlocks[b.mFlock];
        const F32 d = (b.mPos - cam).length();
        bool drop = false;
        if (f.mLeaving && (d > GONE_M || (d > GONE_HIDDEN_M && !camera->sphereInFrustum(b.mPos, 1.f)))) drop = true;
        if (f.mRoosting && b.mPos.mV[VZ] < surfaceZ(b.mPos) + 3.f) drop = true;
        if (d > GONE_M * 1.6f) drop = true;   // anything that has strayed this far
        if (drop) continue;
        ++live[(size_t)b.mFlock];
        mBirds[w++] = b;
    }
    mBirds.resize(w);
    // a flock with no birds left is gone: compact the list and renumber the birds' flocks (the list stays at most
    // MAX_FLOCKS long however long the session runs)
    std::vector<S32> remap(mFlocks.size(), -1);
    std::vector<Flock> kept;
    kept.reserve(mFlocks.size());
    for (size_t k = 0; k < mFlocks.size(); ++k)
    {
        if (live[k] == 0) continue;
        remap[k] = (S32)kept.size();
        kept.push_back(mFlocks[k]);
    }
    if (kept.size() != mFlocks.size())
    {
        for (Bird& b : mBirds) b.mFlock = remap[(size_t)b.mFlock];
        mFlocks.swap(kept);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// calls
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::calls(F64 now)
{
    static LLCachedControl<bool> sound_on(gSavedSettings, "WolfViewerBirdsSound", true);
    if (!sound_on || !gAudiop) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    for (size_t k = 0; k < mFlocks.size(); ++k)
    {
        Flock& f = mFlocks[k];
        if (now < f.mNextCall) continue;
        switch (f.mKind)
        {
        case GULL:     f.mNextCall = now + frand_range(6.f, 18.f); break;
        case TERN:     f.mNextCall = now + frand_range(4.f, 11.f); break;
        case SMALL:    f.mNextCall = now + frand_range(3.f, 9.f); break;
        default:       f.mNextCall = now + frand_range(2.5f, 4.f); break;
        }
        // the nearest bird of the flock calls
        F32 best = 1e9f;
        LLVector3 at;
        for (const Bird& b : mBirds)
        {
            if (b.mFlock != (S32)k) continue;
            const F32 d = (b.mPos - cam).length();
            if (d < best) { best = d; at = b.mPos; }
        }
        if (best < 1e8f) playCall(f.mKind, at, best);
    }
}

/**
 * One call. OpenAL's inverse-distance model at a 1 m reference (llaudioengine_openal.cpp sets only the rolloff factor)
 * would make a gull 60 m off 36 dB down, inaudible; the real birds carry much further. So the call is placed CALL_EAR_M
 * along the bird's direction (it still comes from where the bird is) and its own gain falls more gently with the real
 * distance. Played as AMBIENT, so the Ambient volume slider governs it, like the rain (wolfweathersound.cpp).
 */
void WolfBirds::playCall(Kind kind, const LLVector3& at_agent, F32 dist)
{
    const WolfWeatherSound::Recording* rec = nullptr;
    F32 base = 0.f, full_m = 0.f, max_m = 0.f;
    switch (kind)
    {
    case GULL:  rec = &CALL_GULL[ll_rand(3)];  base = 0.60f; full_m = 30.f; max_m = 150.f; break;
    case TERN:  rec = &CALL_TERN[ll_rand(3)];  base = 0.45f; full_m = 20.f; max_m = 110.f; break;
    case SMALL: rec = &CALL_SMALL[ll_rand(3)]; base = 0.35f; full_m = 12.f; max_m = 45.f;  break;
    default:    rec = &CALL_FLOCK;             base = 0.50f; full_m = 40.f; max_m = 130.f; break;
    }
    if (dist > max_m) return;
    WolfWeatherSound& ws = WolfWeatherSound::instance();
    if (!ws.recordingReady(*rec)) return;   // decoding: the next call will play
    const F32 indoors = (WolfWeather::instanceExists() && WolfWeather::instance().cameraUnderRoof()) ? 0.35f : 1.f;
    const F32 gain = llclamp(base * (full_m / llmax(full_m, dist)) * indoors * WolfSoundEffects::level(WolfSoundEffects::BIRDS), 0.f, 1.f);   // [SOUND EFFECTS 2026-11-06/]
    if (gain <= 0.f) return;
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    LLVector3 dir = at_agent - cam;
    if (dir.isExactlyZero()) dir.setVec(0.f, 0.f, 1.f);
    dir.normVec();
    const LLVector3 pos = cam + dir * CALL_EAR_M;
    gAudiop->triggerSound(LLUUID(rec->mUuid), gAgentID, gain, LLAudioEngine::AUDIO_TYPE_AMBIENT, gAgent.getPosGlobalFromAgent(pos));
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the frame
// ═══════════════════════════════════════════════════════════════════════════════════════

void WolfBirds::idle()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = llclamp((F32)(now - mLastIdle), 0.f, 0.1f);
    mLastIdle = now;
    LLTimer cost;   // [BIRDS LANDING/] the frame cost, for the log line (direct())
    mRayTokens = llmin(RAY_BURST, mRayTokens + RAYS_PER_SEC * dt);
    // The agent frame moves when the agent crosses into another region (LLAgent::setRegion) or rebases on a huge one
    // (LLAgent::rebaseOrigin): every stored agent-metre position moves with it, so the birds stay where they were in the
    // world (review 2026-11-02). Only then is the camera's jump a teleport or not.
    const LLVector3d origin = gAgent.getAgentOriginGlobal();
    if (origin != mLastOrigin)
    {
        const LLVector3 delta(mLastOrigin - origin);
        if (!mLastOrigin.isExactlyZero()) rebase(delta);
        mLastOrigin = origin;
    }
    LLViewerRegion* region = nullptr;
    if (!wanted(region))
    {
        if (!mBirds.empty() || !mFlocks.empty())
        {
            // switched off here (setting, owner, left Wolf): the ones in the air fly away
            bool any_left = false;
            for (Flock& f : mFlocks)
            {
                if (!f.mLeaving) leave(f, LLViewerCamera::getInstance()->getOrigin());   // birds down take off first (move())
                any_left = true;
            }
            static LLCachedControl<bool> on(gSavedSettings, "WolfViewerBirds", true);
            if (!any_left || !on || !region) reset();   // the resident's own switch is instant
            else move(dt, now);
        }
        return;
    }
    const LLVector3 cam = LLViewerCamera::getInstance()->getOrigin();
    if (region->getRegionID() != mRegionId)
    {
        mRegionId = region->getRegionID();
        mHaveBuilt = false;
        mPerchCands.clear();   // [BIRDS LANDING/] region metres of the old region
        mPerchCandsScan.clear();
        mPerches.clear();
        mScanIndex = -1;
        mNextScan = 0.0;
        // a teleport starts again; a walk or flight over the border keeps the birds in the air
        if ((cam - mLastCamera).length() > TELEPORT_M) reset();
    }
    else if ((cam - mLastCamera).length() > TELEPORT_M)
    {
        reset();   // a teleport inside the region
    }
    mLastCamera = cam;
    scanBuilds(region, now);
    // no new flock before the first scan of this region is done: never over a town
    if (mHaveBuilt && now >= mNextDirect)
    {
        mNextDirect = now + DIRECT_SECS;
        direct(region, now);
    }
    if (!mBirds.empty())
    {
        LLTimer part;
        flushCheck(now);   // [BIRDS LANDING/]
        mCostFlushUs += part.getElapsedTimeF64() * 1e6;
        part.reset();
        seekPerch(now);
        mCostPerchUs += part.getElapsedTimeF64() * 1e6;
    }
    if (dt > 0.f && !mBirds.empty())
    {
        LLTimer part;
        move(dt, now);
        mCostMoveUs += part.getElapsedTimeF64() * 1e6;
    }
    calls(now);
    if (!mBirds.empty())
    {
        mCostIdleUs += cost.getElapsedTimeF64() * 1e6;
        ++mCostFrames;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════
// the look
// ═══════════════════════════════════════════════════════════════════════════════════════

namespace
{
    // Triangles per bird: body 8, tail 2 panels x 2 sides = 4, each wing 4 panels x 2 sides = 8, [BIRDS LANDING/] each leg
    // 1 panel x 2 sides = 2 (drawn only while it stands).
    constexpr S32 TRIS_PER_BIRD = 8 + 4 + 16 + 4;
    constexpr S32 VERTS_PER_BIRD = TRIS_PER_BIRD * 3;
    constexpr S32 MAX_VERTS = HARD_MAX_BIRDS * VERTS_PER_BIRD;   // 28 800: inside the U16 indices
    static_assert(MAX_VERTS <= 65536, "the birds' indices are U16");

    struct Writer
    {
        LLStrider<LLVector4a> pos;
        LLStrider<LLVector3> norm;
        LLStrider<LLVector2> uv;
        LLStrider<LLColor4U> col;
        S32 n = 0;

        void vert(const LLVector3& p, const LLVector3& nn, const F32* c)
        {
            (*pos++).set(p.mV[VX], p.mV[VY], p.mV[VZ], 0.f);   // w = texture index 0 (wolffarground.cpp)
            *norm++ = nn;
            *uv++ = LLVector2(0.5f, 0.5f);
            *col++ = LLColor4U((U8)(c[0] * 255.f), (U8)(c[1] * 255.f), (U8)(c[2] * 255.f), 255);
            ++n;
        }
        /** One triangle facing `out` (its winding set so the face that is drawn is the one `out` points from). */
        void tri(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& out,
                 const F32* ca, const F32* cb, const F32* cc)
        {
            LLVector3 nn = (b - a) % (c - a);
            if (nn.isExactlyZero()) nn = out;
            nn.normVec();
            if (nn * out < 0.f)
            {
                vert(a, -nn, ca); vert(c, -nn, cc); vert(b, -nn, cb);
            }
            else
            {
                vert(a, nn, ca); vert(b, nn, cb); vert(c, nn, cc);
            }
        }
        /** Both faces of a thin panel: the upper face coloured `top_*`, the lower `under_*`. */
        void panel(const LLVector3& a, const LLVector3& b, const LLVector3& c, const LLVector3& up,
                   const F32* ta, const F32* tb, const F32* tc, const F32* ua, const F32* ub, const F32* uc)
        {
            tri(a, b, c, up, ta, tb, tc);
            tri(a, b, c, -up, ua, ub, uc);
        }
    };
}

bool WolfBirds::ensureBuffer()
{
    if (mVB.notNull()) return true;
    LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL
                                                      | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR
                                                      | LLVertexBuffer::MAP_TEXTURE_INDEX);
    if (!vb->allocateBuffer(MAX_VERTS, MAX_VERTS))
    {
        LL_WARNS("WolfBirds") << "could not allocate the birds' vertex buffer" << LL_ENDL;
        return false;
    }
    // The indices never change: every triangle has its own three vertices (flat shading, two colours a panel).
    LLStrider<U16> idx;
    if (!vb->getIndexStrider(idx)) return false;
    for (S32 i = 0; i < MAX_VERTS; ++i) *idx++ = (U16)i;
    vb->unmapBuffer();
    mVB = vb;
    return true;
}

void WolfBirds::buildMesh(S32& verts)
{
    verts = 0;
    LLTimer cost;   // [BIRDS LANDING/] the frame cost, for the log line
    if (mBirds.empty() || !ensureBuffer()) return;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLVector3 cam = camera->getOrigin();
    // the birds in view (and big enough to be seen: beyond about 900 spans a bird is under a pixel on a 1080p frame),
    // so only their vertices are written and uploaded
    mVisible.clear();
    for (size_t i = 0; i < mBirds.size() && (S32)(mVisible.size() + 1) * VERTS_PER_BIRD <= MAX_VERTS; ++i)
    {
        const Bird& b = mBirds[i];
        const F32 span = SPECIES[mFlocks[b.mFlock].mKind].mSpan * b.mSize;
        if (!camera->sphereInFrustum(b.mPos, span)) continue;
        if ((b.mPos - cam).length() > span * 900.f) continue;
        mVisible.push_back((S32)i);
    }
    if (mVisible.empty()) return;
    const S32 count = (S32)mVisible.size() * VERTS_PER_BIRD;
    Writer w;
    if (!mVB->getVertexStrider(w.pos, 0, count) || !mVB->getNormalStrider(w.norm, 0, count)
        || !mVB->getTexCoord0Strider(w.uv, 0, count) || !mVB->getColorStrider(w.col, 0, count))
    {
        mVB->unmapBuffer();   // nothing drawn this frame, and nothing left mapped (review 2026-11-02)
        return;
    }
    for (S32 vi : mVisible)
    {
        const Bird& b = mBirds[(size_t)vi];
        const Species& sp = SPECIES[mFlocks[b.mFlock].mKind];
        const F32 span = sp.mSpan * b.mSize, len = sp.mLength * b.mSize;
        const bool distant = (b.mPos - cam).length() > span * LOD_SPANS;   // not "far": <windef.h> defines far (and near) away

        // its frame: forward along its flight, banked into its turn; [BIRDS LANDING/] down, along its heading, the head
        // pitched down to peck or preen (mBank is 0 there)
        const bool down = (b.mLand == DOWN);
        LLVector3 fwd = b.mVel;
        if (down)
        {
            const F32 cp = cosf(b.mPitch);
            fwd.setVec(cosf(b.mYaw) * cp, sinf(b.mYaw) * cp, -sinf(b.mPitch));
        }
        if (fwd.isExactlyZero()) fwd.setVec(1.f, 0.f, 0.f);
        fwd.normVec();
        LLVector3 left = LLVector3(0.f, 0.f, 1.f) % fwd;
        if (left.isExactlyZero()) left.setVec(0.f, 1.f, 0.f);
        left.normVec();
        LLVector3 up = fwd % left;
        const F32 cb = cosf(b.mBank), sb = sinf(b.mBank);
        const LLVector3 left_b = left * cb + up * sb;
        const LLVector3 up_b = up * cb - left * sb;
        auto P = [&](F32 x, F32 y, F32 z) { return b.mPos + fwd * x + left_b * y + up_b * z; };

        // the wings' angles: a beat about the shoulder, the hand lagging behind; a glide holds them nearly level, a
        // gull's in its bent "M", a small bird's bound folds them to its sides
        const F32 s = sinf(b.mPhase);
        F32 a1 = sp.mBeatDeg * DEG_TO_RAD * s;
        F32 a2 = sp.mBeatDeg * 0.6f * DEG_TO_RAD * sinf(b.mPhase - 0.7f);
        F32 reach = 1.f;
        const bool gliding = (b.mFlapFor <= 0.f);
        const Kind kind = mFlocks[b.mFlock].mKind;
        // [BIRDS LANDING 2026-11-02] sweep: the wing swung back about the shoulder (0 = spread); lift: raised onto the back
        F32 sweep = 0.f, lift = 0.f;
        if (down)
        {
            // folded along the back, the hand over the arm so the tips reach just past the tail; preening, half raised
            const bool preen = (b.mAct == ACT_PREEN);
            a1 = preen ? 25.f * DEG_TO_RAD : 0.f;
            a2 = preen ? -10.f * DEG_TO_RAD : 0.f;
            reach = preen ? 0.8f : 0.55f;
            sweep = (preen ? 35.f : 80.f) * DEG_TO_RAD;
            lift = len * 0.045f;
        }
        else if (gliding)
        {
            if (kind == GULL)      { a1 = 10.f * DEG_TO_RAD; a2 = -16.f * DEG_TO_RAD; }
            else if (kind == SMALL){ a1 = 0.f; a2 = 0.f; reach = 0.18f; }
            else                   { a1 = 4.f * DEG_TO_RAD; a2 = -2.f * DEG_TO_RAD; }
        }
        const F32 ssw = sinf(sweep), csw = cosf(sweep);

        // body: nose, a ring of four at the shoulders, the tail root
        const F32 h = len * 0.09f, wb = len * 0.08f;
        const LLVector3 nose = P(len * 0.5f, 0.f, 0.f);
        const LLVector3 tailr = P(-len * 0.3f, 0.f, 0.f);
        const LLVector3 rt = P(len * 0.08f, 0.f, h), rb = P(len * 0.08f, 0.f, -h * 0.9f);
        const LLVector3 rl = P(len * 0.08f, wb, 0.f), rr = P(len * 0.08f, -wb, 0.f);
        const LLVector3 mid = P(len * 0.08f, 0.f, 0.f);
        const LLVector3 ring[4] = { rt, rl, rb, rr };
        const F32* ring_c[4] = { sp.mBody, sp.mBody, sp.mUnder, sp.mBody };
        for (S32 k = 0; k < 4 && !distant; ++k)
        {
            const LLVector3& r0 = ring[k];
            const LLVector3& r1 = ring[(k + 1) % 4];
            const LLVector3 outward = ((r0 + r1) * 0.5f) - mid;
            w.tri(nose, r0, r1, outward + fwd * 0.2f, sp.mHead, ring_c[k], ring_c[(k + 1) % 4]);
            w.tri(tailr, r0, r1, outward - fwd * 0.2f, sp.mBody, ring_c[k], ring_c[(k + 1) % 4]);
        }
        // tail: a fan, forked for the tern
        const F32 tw = len * ((kind == TERN) ? 0.16f : 0.11f);
        const LLVector3 tl = P(-len * 0.55f, tw, 0.f), tr = P(-len * 0.55f, -tw, 0.f);
        const LLVector3 tc = (kind == TERN) ? P(-len * 0.38f, 0.f, 0.f) : P(-len * 0.52f, 0.f, 0.f);
        if (!distant)
        {
            w.panel(tailr, tl, tc, up_b, sp.mTop, sp.mTip, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            w.panel(tailr, tc, tr, up_b, sp.mTop, sp.mTop, sp.mTip, sp.mUnder, sp.mUnder, sp.mUnder);
        }

        // wings: shoulder - wrist (45 % of the half-span) - tip, swept back towards the tip
        const F32 half = span * 0.5f * reach;
        for (S32 side = 0; side < 2; ++side)
        {
            const F32 sg = side == 0 ? 1.f : -1.f;
            const F32 yw = half * 0.45f, yt = half;
            const F32 c1 = cosf(a1), s1 = sinf(a1), c2 = cosf(a1 + a2), s2 = sinf(a1 + a2);
            // y and z of a point along the wing at span y (from the shoulder at wb)
            // [BIRDS LANDING/] the sweep shears each point back by its distance out from the shoulder (chord kept)
            auto swept = [&](F32 x, F32 y, F32 z) { const F32 d = y - wb; return P(x - d * ssw, sg * (wb + d * csw), z + lift); };
            auto inner = [&](F32 x, F32 y) { const F32 d = y - wb; return swept(x, wb + d * c1, d * s1); };
            const F32 wy = wb + (yw - wb) * c1, wz = (yw - wb) * s1;
            auto outer = [&](F32 x, F32 y) { const F32 d = y - yw; return swept(x, wy + d * c2, wz + d * s2); };
            const LLVector3 root_l = inner(len * 0.14f, wb), root_t = inner(-len * 0.10f, wb);
            const LLVector3 wrist_l = inner(len * 0.10f, yw), wrist_t = inner(-len * 0.16f, yw);
            const LLVector3 tip_l = outer(-len * 0.04f, yt), tip_t = outer(-len * 0.18f, yt * 0.94f);
            const LLVector3 wing_up = up_b;
            w.panel(root_l, root_t, wrist_t, wing_up, sp.mTop, sp.mTop, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            w.panel(root_l, wrist_t, wrist_l, wing_up, sp.mTop, sp.mTop, sp.mTop, sp.mUnder, sp.mUnder, sp.mUnder);
            const F32* under_tip = (kind == GULL) ? sp.mTip : sp.mUnder;
            w.panel(wrist_l, wrist_t, tip_t, wing_up, sp.mTop, sp.mTop, sp.mTip, sp.mUnder, sp.mUnder, under_tip);
            w.panel(wrist_l, tip_t, tip_l, wing_up, sp.mTop, sp.mTip, sp.mTip, sp.mUnder, under_tip, under_tip);
        }

        // [BIRDS LANDING 2026-11-02] legs, while it stands (not floating): from the hip under the body to the foot on the
        // surface, the foot placed in the bird's level frame so pecking pitches the body and not the feet; walking swings
        // them in turn, hopping keeps them together
        if (down && b.mSpot != SPOT_WATER && !distant)
        {
            const F32 stand = standHeight(b, kind);
            const LLVector3 lfwd(cosf(b.mYaw), sinf(b.mYaw), 0.f), lleft(-sinf(b.mYaw), cosf(b.mYaw), 0.f);
            const LLVector3 feet(b.mPos.mV[VX], b.mPos.mV[VY], b.mPos.mV[VZ] - stand);
            const F32* leg_c = (kind == GULL) ? LEG_GULL : sp.mTip;
            const F32 tarsus = (kind == SMALL ? TARSUS_SMALL_M : TARSUS_GULL_M) * b.mSize;
            const bool walking = (b.mAct == ACT_MOVE && b.mSpot == SPOT_SHORE);
            for (S32 side = 0; side < 2; ++side)
            {
                const F32 sg = side == 0 ? 1.f : -1.f;
                const F32 swing = walking ? tarsus * 0.5f * sinf(F_TWO_PI * b.mStep + (side ? F_PI : 0.f)) : 0.f;
                const LLVector3 hip_f = P(len * 0.05f, sg * wb * 0.5f, -h * 0.8f), hip_b = P(-len * 0.05f, sg * wb * 0.5f, -h * 0.8f);
                const LLVector3 foot = feet + lfwd * swing + lleft * (sg * wb * 0.5f);
                w.panel(hip_f, hip_b, foot, left_b * sg, leg_c, leg_c, leg_c, leg_c, leg_c, leg_c);
            }
        }
    }
    mVB->unmapBuffer();
    verts = w.n;
    mCostMeshUs += cost.getElapsedTimeF64() * 1e6;
}

void WolfBirds::renderDeferred()
{
    if (mBirds.empty()) return;
    S32 verts = 0;
    buildMesh(verts);
    if (verts == 0) return;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    // Source: WolfFarGround::renderDeferred (wolffarground.cpp) - the same program, matrices and attributes, so the birds
    // are lit, fogged and hazed as the scene round them is; a white texture leaves the vertex colours as they are.
    if (LLViewerFetchedTexture::sWhiteImagep.isNull()) return;   // as pipeline.cpp checks it
    LLGLDisable blend(GL_BLEND);
    LLGLEnable cull(GL_CULL_FACE);
    LLGLSLShader& shader = gDeferredNonIndexedDiffuseAlphaMaskProgram;
    shader.bind();
    shader.setMinimumAlpha(0.5f);
    gGLLastMatrix = nullptr;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);
    gGL.getTexUnit(0)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
    mVB->setBuffer();
    mVB->drawRange(LLRender::TRIANGLES, 0, (U32)(verts - 1), (U32)verts, 0);
    shader.unbind();
}
