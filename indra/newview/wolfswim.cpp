/**
 * @file wolfswim.cpp
 * @brief WolfViewer: swimming. See wolfswim.h.
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

#include "wolfswim.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "aoengine.h"
#include "llagent.h"
#include "llanimationstates.h"
#include "llassetstorage.h"
#include "llmotion.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "wolfgrid.h"
#include "wolfdryvolumes.h"   // [WOLF SEA TUNNEL 2026-10-28/] no swimming in a sea tunnel
#include "wolfboatrock.h"     // [SWIM HEAD 2026-11-13/] drawnWaveHeight: the surface as drawn
#include "llstartup.h"        // [SWIM HEAD 2026-11-13/] no swim decisions before STATE_STARTED

namespace
{
    // Source: region-terrain-work/wolf-swim/make_swim_anims.py TREAD_ID / STROKE_ID - stored on the Wolf grid's asset
    // service (Robust /assets, type 20) on 2026-10-20 and read back byte for byte.
    const LLUUID TREAD_ANIM("ff007f5d-053f-4433-b0b5-d2e357bd9b2b");
    const LLUUID STROKE_ANIM("e9ca399a-f4a8-403a-814b-767439ff4de9");

    // The surface band (design values, metres of the avatar's top against the water height). Under ENTER_BELOW the
    // swim starts; it ends only past EXIT_ABOVE, so waves of the top round the surface never flick it on and off.
    // Upward control goes at TOP_STOP_BELOW (just under the surface); above PUSH_DOWN_ABOVE, while not touching
    // anything, the avatar is nudged down (a pitched mouselook swimmer can still climb with forward alone: WolfSim
    // AddNewMovement keeps the body's pitch while Flying).
    const F32 ENTER_BELOW = 0.10f, EXIT_ABOVE = 0.35f, TOP_STOP_BELOW = 0.05f, PUSH_DOWN_ABOVE = 0.12f;
    // [SWIM HEAD 2026-11-13] A swimmer touching nothing ends the swim only this far up (design value). Live test run 2
    // (09:48:38Z, Sandbox 2): up pressed while moving across at the surface carried the top 0.2-0.4 m past the point
    // where steer takes up away (the region's move arrives 0.2-0.35 s late, at up to 1.63 m/s), past EXIT_ABOVE: the
    // swim ended in mid-air, flying went off and the avatar fell ~3 m into the water. In the air steer's push-down
    // brings it back instead; past this the avatar is really out (thrown up by something), and the swim ends.
    const F32 EXIT_ABOVE_IN_AIR = 1.0f;
    // [SWIM HEAD 2026-11-13] MEASURED, live test run 4 (10:32:53Z, Sandbox 2): swimming off from a held head, steer's
    // push-down (pressed until the top is back under PUSH_DOWN_ABOVE) sank the top to 1.16 m under that line at
    // 1.64 m/s: ~0.6 s of the move still arriving after the release. The push is judged that far ahead.
    const F32 PUSH_LEAD_S = 0.6f;
    // [SWIM HEAD 2026-11-13] Flying out of the water on purpose (review W2): Up held this long at the surface (design
    // value) lets the swimmer rise out and fly on, as plain flying does (steer otherwise takes Up away at the surface,
    // and the hold swallows it); a short press only surfaces. Up from the keyboard is AGENT_CONTROL_UP_POS every frame
    // while the key is down (llviewerinput.cpp agent_jump -> LLAgent::moveUp, llagent.cpp:902).
    const F64 RISE_OUT_HOLD_S = 1.0;

    // From the head joint to the top of the head. Source: wolfstorm/python/charbuilder std_male.npz (the standard
    // shape's body mesh) highest vertex z 1.946 against sl_skeleton.json (avatar_skeleton.xml) mSkull z 1.762.
    const F32 SKULL_TO_TOP = 0.184f;

    // [SWIM HEAD 2026-11-13] Ticket 802 (IndigoQueen King): treading water at the surface, the head is held out of it
    // with the waterline at the chin, on the waves this viewer draws (holdHead). Design values, all untuned until the
    // live test. (WolfSim line numbers here are the live tree, airebuilder wtu-invthumbs - swim-floor-t802 README; in the
    // airebuilder/opensim checkout the same lines are ScenePresence.cs:1124, 3902-3950 AddNewMovement and 6127
    // SendControlsToScripts.)
    //   SURFACE_REACH - the hold starts once the top of the head is within this of the drawn surface (a swimmer who
    //     comes up, or dips under for a moment, pops back up; one who stops deeper down stays there);
    //   The move a press asks for is AgentControlStopSlowVel x 4 = 0.2 x 4.096 x 4 = 3.28 m/s (WolfSim
    //     ScenePresence.cs:1124, and AddNewMovement's flying `direc *= 4.0f`, :3993). MEASURED (live test 2026-11-13
    //     09:09Z, Sandbox 2, BulletSim, ~/.cache/w106/swimhead-rig): the viewer saw at most 1.63 m/s, and a press of
    //     ~0.4 s moved the chin ~1.15 m in all, its last ~0.3 m arriving 0.2-0.35 s AFTER the release (the region's
    //     update coming back). A press-until-there loop (the first version, 0.15 s look-ahead) swung the chin +/-0.35 m
    //     at ~0.8 Hz. So the hold moves in measured PULSES instead, and looks again only once each pulse has arrived:
    //   HOLD_PER_M_S - seconds of press per metre to go: 0.4 s / 1.15 m (the measurement above);
    //   HOLD_MIN_PRESS_S / HOLD_MAX_PRESS_S - each pulse at least / at most this long (a 0.1 s pulse moves ~0.29 m);
    //   HOLD_SETTLE_S - after a pulse, no new one until its move has come back (the 0.35 s above, rounded up);
    //   HOLD_START_BAND - a pulse only for a chin further than this from the surface: more than half the smallest pulse's
    //     move, so a pulse never ends further away than it started (no hunting about the surface).
    //   Each change of the control flags is an AgentUpdate straight away (llviewermessage.cpp:4378) plus its follow-up:
    //     at most 2 changes per (0.1 + 0.4) s = 4 a second.
    //   HOLD_MAX_PITCH - the hold only works with a level body: flying, the region turns the up / down move by the body
    //     rotation the viewer sends (AddNewMovement `direc = vec * rot`, :3961, not levelled while Flying, :3953-3959),
    //     which is the agent frame (llviewermessage.cpp:4390) and pitches in mouselook (LLAgent::pitch); a pitched body
    //     would drift across the water on every press. The sine of 5 degrees: up to 0.29 m/s of drift during a press.
    const F32 SURFACE_REACH = 0.30f, HOLD_START_BAND = 0.15f;
    const F32 HOLD_PER_M_S = 0.4f / 1.15f;
    const F64 HOLD_MIN_PRESS_S = 0.10, HOLD_MAX_PRESS_S = 0.40, HOLD_SETTLE_S = 0.40;
    const F32 HOLD_MAX_PITCH = 0.0872f;

    // Stroke when moving faster than this (m/s), back to the tread under STILL_SPEED for STILL_FOR_S (design values;
    // the swim itself is 3.28 m/s - wolfswim.h).
    const F32 MOVING_SPEED = 0.35f, STILL_SPEED = 0.2f;
    const F64 STILL_FOR_S = 0.5;

    // Re-ask for an animation the region has not echoed back after this long (an agent request the region refused
    // because the same id was its default movement animation - OpenSim AnimationSet.Add/HasAnimation - or one a
    // scripted AO stopped by id).
    const F64 REQUEST_AGAIN_S = 1.5;
    // The own-walk paddle restarts (stop, then start this much later) so it is again the NEWEST animation at its
    // priority: llpose.cpp LLJointStateBlender::addJointState keeps newer motions of the same priority first and
    // blendJointStates lets the first full-weight one win. Two frames apart at least, so every viewer sees the stop.
    const F64 PADDLE_RESTART_GAP_S = 0.3;

    // On land: what the avatar is (see sampleOnLand). Every SAMPLE_EVERY_S, the last SAMPLES kept.
    const F64 SAMPLE_EVERY_S = 0.5;
    const size_t SAMPLES = 16, POSE_SAMPLES_NEEDED = 8;
    // Learning the walk: moving on land at least this fast (m/s) for LEARN_AFTER_S; the walk is the animation that
    // started within LEARN_WINDOW_BEFORE_S before to LEARN_WINDOW_AFTER_S after the move began (design values).
    const F32 LEARN_SPEED = 0.5f;
    const F64 LEARN_AFTER_S = 0.6, LEARN_WINDOW_BEFORE_S = 0.3, LEARN_WINDOW_AFTER_S = 1.5;

    enum class EMode
    {
        NONE,       // nothing of ours plays (the AO's own swim states, or a quadruped whose walk is not known yet)
        WOLF_ANIMS, // biped: wolf_swim_tread / wolf_swim_stroke
        AO_PADDLE,  // quadruped: the Firestorm AO's Walking state stands in for its fly / hover states
        OWN_PADDLE  // quadruped: the walk it was seen using on land
    };

    struct State
    {
        bool mSwimming = false;
        bool mWeTurnedFlyOn = false;   // we switched flying on; switched off again at the end
        bool mSuppressed = false;      // flying turned off by the person (or refused) in this dip: walk the bottom
        bool mStroke = false;
        F64 mStillSince = 0.0;
        EMode mMode = EMode::NONE;
        LLUUID mPlaying;               // the animation we asked the region for, null when none
        F64 mRequestedAt = 0.0;
        F64 mPaddleStartAt = 0.0;      // OWN_PADDLE: start (again) at this time, 0 = no start pending
        F64 mPaddleStartedAt = 0.0;
        // [SWIM HEAD 2026-11-13] mHolding: treading with the head held out (holdHead); mHeadUp: the head was raised and the
        // top has not been back under the surface band since, so the out-of-the-water test goes by the chin meanwhile
        bool mHolding = false;
        bool mHeadUp = false;
        F64 mUpAtSurfaceSince = 0.0;   // Up held at the surface since, 0 = not held there
        bool mRisingOut = false;       // held long enough: rising out of the water (RISE_OUT_HOLD_S)
        S32 mPress = 0;                // holdHead: the press now, +1 up, -1 down, 0 none
        F64 mPressChangedAt = 0.0;
        F64 mPressFor = 0.0;           // the length of the pulse now pressed
        U32 mPressEdges = 0;           // press changes, counted for the debug line
        LLVOAvatarSelf* mChinOf = nullptr;   // chinJoint's cache: the avatar and head joint it was found for
        LLJoint* mChinHeadOf = nullptr;
        LLJoint* mChin = nullptr;

        // on land
        F64 mNextSample = 0.0;
        std::vector<F32> mTopSamples;  // the top above the root, live
        std::vector<bool> mQuadVotes;  // live pose says quadruped
        LLVector3 mRestSignature;      // rest pelvis -> head, to notice another body
        std::map<LLUUID, F64> mFirstSeen;
        bool mWasMoving = false;
        bool mLearnedThisMove = false;
        F64 mMoveStart = 0.0;
        LLUUID mLearnedWalk, mLearnedRun;

        // once per session
        bool mToldStart = false, mToldNoFly = false, mToldNoWalk = false, mToldMissing = false, mCheckedAssets = false;
    };

    State& st()
    {
        static State s;
        return s;
    }

    F64 now() { return LLFrameTimer::getTotalSeconds(); }

    void tip(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("SystemMessageTip", args);
    }

    // Failures: a tip that also goes into nearby chat (notifications.xml ChatSystemMessageTip, log_to_chat), so it can
    // still be read after the tip fades.
    void problem(const std::string& text)
    {
        LLSD args;
        args["MESSAGE"] = text;
        LLNotificationsUtil::add("ChatSystemMessageTip", args);
    }

    bool isFlyMotion(const LLUUID& id)
    {
        // The movement animations the region plays while flying - WolfSim ScenePresenceAnimator.cs
        // DetermineMovementAnimation, region "Flying": FLY / FLYSLOW, HOVER_UP, HOVER_DOWN, HOVER.
        return id == ANIM_AGENT_FLY || id == ANIM_AGENT_FLYSLOW || id == ANIM_AGENT_HOVER
            || id == ANIM_AGENT_HOVER_UP || id == ANIM_AGENT_HOVER_DOWN;
    }

    bool isBuiltIn(const LLUUID& id)
    {
        return gAnimLibrary.animStateToString(id) != nullptr;   // llanimationstates.cpp:354 - null for any other id
    }

    // A joint's position in the avatar's own rest frame (X forward, Z up): the local positions summed from the joint up to
    // the pelvis. LLJoint::getPosition is the live local position, which for a joint carrying an attachment's position
    // override IS the override (llvoavatar.cpp:7740 addAttachmentOverridesForObject -> LLJoint::addAttachmentPosOverride
    // -> LLJoint::updatePos, lljoint.cpp:570-590 setPosition(the active override)). The pelvis' own position is left out:
    // Firestorm zeroes it (llavatarappearance.cpp:894 buildCharacter), the avatar's root sits there.
    LLVector3 restAbovePelvis(LLJoint* joint, LLJoint* pelvis)
    {
        LLVector3 sum;
        for (LLJoint* j = joint; j && j != pelvis; j = j->getParent())
        {
            sum += j->getPosition();
        }
        return sum;
    }

    // The quadruped rule (see the report / wolfswim.h): the head is further forward of the pelvis than it is above it,
    // and at least 10 cm forward. A biped's head is ~0.62 m above its pelvis and 0.03 m behind it (avatar_skeleton.xml:
    // mPelvis 1.067, mHead 1.683, x -0.025); a quadruped's spine runs forward, its head about level with its hips.
    bool quadPose(const LLVector3& pelvis_to_head)
    {
        const F32 forward = std::sqrt(pelvis_to_head.mV[VX] * pelvis_to_head.mV[VX] + pelvis_to_head.mV[VY] * pelvis_to_head.mV[VY]);
        return forward > 0.10f && forward > pelvis_to_head.mV[VZ];
    }

    bool restQuad(LLVOAvatarSelf* av)
    {
        return av->mHeadp && av->mPelvisp && quadPose(restAbovePelvis(av->mHeadp, av->mPelvisp));
    }

    // The highest point of the avatar now, in agent space z: the spine and head joints (live, so animations and overrides
    // are in it) and the top of the head above the skull joint, along the head's own up.
    F32 liveTopZ(LLVOAvatarSelf* av)
    {
        F32 top = -F32_MAX;
        LLJoint* joints[] = { av->mPelvisp, av->mTorsop, av->mChestp, av->mNeckp, av->mHeadp, av->mSkullp };
        for (LLJoint* j : joints)
        {
            if (j)
            {
                top = llmax(top, j->getWorldPosition().mV[VZ]);
            }
        }
        if (av->mSkullp)
        {
            const LLVector3 head_top = av->mSkullp->getWorldPosition() + LLVector3(0.f, 0.f, SKULL_TO_TOP) * av->mSkullp->getWorldRotation();
            top = llmax(top, head_top.mV[VZ]);
        }
        return top;
    }

    // How far the top of this avatar is above its root when it stands on land: the middle of the samples taken on land,
    // else the rest skeleton's (skull up to the pelvis, plus the top of the head). Measured against the ROOT, which the
    // physics moves and no animation does, so a swim animation lying the body down never moves the swim's own trigger.
    F32 topAboveRoot(LLVOAvatarSelf* av)
    {
        const State& s = st();
        F32 v = 0.f;
        if (!s.mTopSamples.empty())
        {
            std::vector<F32> sorted(s.mTopSamples);
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            v = sorted[sorted.size() / 2];
        }
        else if (av->mSkullp && av->mPelvisp)
        {
            v = restAbovePelvis(av->mSkullp, av->mPelvisp).mV[VZ] + SKULL_TO_TOP;
        }
        return llclamp(v, 0.2f, 3.f);   // a sane band whatever the rig
    }

    bool isQuadruped(LLVOAvatarSelf* av)
    {
        if (restQuad(av))
        {
            return true;
        }
        // An animation-posed quadruped (biped skeleton, its AO bends it onto four legs): the live pose on land says so.
        const State& s = st();
        if (s.mQuadVotes.size() < POSE_SAMPLES_NEEDED)
        {
            return false;
        }
        const size_t votes = (size_t)std::count(s.mQuadVotes.begin(), s.mQuadVotes.end(), true);
        return votes * 4 >= s.mQuadVotes.size() * 3;
    }

    template <typename T> void keepLast(std::vector<T>& v, const T& x)
    {
        v.push_back(x);
        if (v.size() > SAMPLES)
        {
            v.erase(v.begin());
        }
    }

    // On land, standing or walking (not sat, not flying, not in the air, not crouching): what the avatar really looks
    // like, and which animation is its walk.
    void sampleOnLand(LLVOAvatarSelf* av)
    {
        State& s = st();
        const F64 t = now();

        // another body (its rest skeleton changed): forget what was learned about the old one
        if (av->mHeadp && av->mPelvisp)
        {
            const LLVector3 sig = restAbovePelvis(av->mHeadp, av->mPelvisp);
            if (dist_vec(sig, s.mRestSignature) > 0.05f)
            {
                s.mRestSignature = sig;
                s.mTopSamples.clear();
                s.mQuadVotes.clear();
                s.mLearnedWalk.setNull();
                s.mLearnedRun.setNull();
            }
        }

        // when each playing animation first appeared
        for (auto it = s.mFirstSeen.begin(); it != s.mFirstSeen.end();)
        {
            it = av->mSignaledAnimations.count(it->first) ? std::next(it) : s.mFirstSeen.erase(it);
        }
        for (const auto& sig : av->mSignaledAnimations)
        {
            s.mFirstSeen.emplace(sig.first, t);
        }

        const bool on_land = !av->isSitting() && !gAgent.getFlying() && !av->mInAir
            && !(gAgent.getControlFlags() & (AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_UP_NEG));
        if (!on_land || !av->mRoot)
        {
            s.mWasMoving = false;
            return;
        }

        if (t >= s.mNextSample && av->mPelvisp && av->mHeadp)
        {
            s.mNextSample = t + SAMPLE_EVERY_S;
            keepLast(s.mTopSamples, liveTopZ(av) - av->mRoot->getWorldPosition().mV[VZ]);
            // the live pelvis -> head in the avatar's frame (the root's rotation taken off)
            const LLVector3 d = (av->mHeadp->getWorldPosition() - av->mPelvisp->getWorldPosition()) * ~av->mRoot->getWorldRotation();
            keepLast(s.mQuadVotes, quadPose(d));
        }

        LLVector3 vel = av->getVelocity();
        vel.mV[VZ] = 0.f;
        const bool moving = vel.length() > LEARN_SPEED;
        if (moving && !s.mWasMoving)
        {
            s.mMoveStart = t;
            s.mLearnedThisMove = false;
        }
        s.mWasMoving = moving;
        if (!moving || s.mLearnedThisMove || t - s.mMoveStart < LEARN_AFTER_S)
        {
            return;
        }
        s.mLearnedThisMove = true;

        // The walk: an animation that is not one of the built-in ones and started when the move did. A scripted AO with
        // llSetAnimationOverride makes the region play it as the movement animation (OpenSim ScenePresenceAnimator
        // TrySetMovementAnimation), a classic one starts it; either way it appears in the AvatarAnimation list now.
        LLUUID best;
        F64 best_gap = F64_MAX;
        for (const auto& seen : s.mFirstSeen)
        {
            if (isBuiltIn(seen.first) || seen.first == TREAD_ANIM || seen.first == STROKE_ANIM)
            {
                continue;
            }
            if (seen.second < s.mMoveStart - LEARN_WINDOW_BEFORE_S || seen.second > s.mMoveStart + LEARN_WINDOW_AFTER_S)
            {
                continue;
            }
            const F64 gap = std::fabs(seen.second - s.mMoveStart);
            if (gap < best_gap)
            {
                best_gap = gap;
                best = seen.first;
            }
        }
        if (best.isNull())
        {
            // no AO: the built-in walk is this avatar's own walk
            const LLUUID builtin[] = { ANIM_AGENT_WALK, ANIM_AGENT_FEMALE_WALK, ANIM_AGENT_WALK_NEW, ANIM_AGENT_RUN, ANIM_AGENT_RUN_NEW,
                                       ANIM_AGENT_FEMALE_RUN_NEW };
            for (const LLUUID& id : builtin)
            {
                if (av->mSignaledAnimations.count(id))
                {
                    best = id;
                    break;
                }
            }
        }
        if (best.notNull())
        {
            (gAgent.getRunning() ? s.mLearnedRun : s.mLearnedWalk) = best;
        }
    }

    // The water surface over the avatar: the water height of the region it is in. Off a region's edge the agent is
    // still owned by the region it came from (WolfSim ScenePresence.CheckForBorderCrossing pushes a walking / swimming
    // avatar back; only a seated one goes on - virtual-sea plan), so that region's water is the sea there.
    bool waterHeight(const LLVector3d& at, F32& water)
    {
        LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(at);
        if (!region)
        {
            region = gAgent.getRegion();
        }
        if (!region)
        {
            return false;
        }
        water = region->getWaterHeight();
        return true;
    }

    // [SWIM HEAD 2026-11-13] The chin, from the skeleton: the bento mFaceChin joint (avatar_skeleton.xml:55, under
    // mFaceJaw under mHead - 0.069 m below the head joint and 0.33 m below the top of the head on the standard shape),
    // which every avatar has and which follows the head's live pose and any rigged mesh's joint offsets. The head joint
    // if a skeleton should lack it.
    // Looked up by name once per avatar and skeleton (a new self avatar or a rebuilt skeleton has a new head joint).
    LLJoint* chinJoint(LLVOAvatarSelf* av)
    {
        State& s = st();
        if (s.mChinOf != av || s.mChinHeadOf != av->mHeadp)
        {
            s.mChinOf = av;
            s.mChinHeadOf = av->mHeadp;
            LLJoint* chin = av->getJoint("mFaceChin");
            s.mChin = chin ? chin : av->mHeadp;
        }
        return s.mChin;
    }

    // [SWIM HEAD 2026-11-13] The water surface as this viewer DRAWS it over an agent-space point: the region's water
    // height plus the Wolf sea's wave there now - the same reading the swim bubbles and the floating birds use
    // (wolfswimbubbles.cpp surfaceOf, wolfbirds.cpp waterSurfaceZ: WolfBoatRock::drawnWaveHeight, the shader's wave model).
    bool drawnSurface(const LLVector3& p, F32& surface)
    {
        LLViewerRegion* r = LLWorld::getInstance()->getRegionFromPosAgent(p);
        if (!r)
        {
            r = gAgent.getRegion();
        }
        if (!r)
        {
            return false;
        }
        const F32 wave = WolfBoatRock::instanceExists() ? WolfBoatRock::instance().drawnWaveHeight(r, p.mV[VX], p.mV[VY]) : 0.f;
        surface = r->getWaterHeight() + wave;
        return std::isfinite(surface);
    }

    // [SWIM HEAD 2026-11-13] Treading at the surface: up or down (the region's own swim speed, steer) so the chin rides
    // the drawn surface. The position is the region's, so every viewer sees the head out of the water. Upward is capped
    // at the chin line: up is pressed only while the chin is under the surface, so the hold never lifts the body out of
    // the water. It presses in measured pulses, each followed by a pause for its move to arrive (HOLD_* above). The
    // person's own Up is swallowed while held (a tap only surfaces); held for RISE_OUT_HOLD_S it ends the hold and the
    // swimmer rises out.
    void holdHead(LLVOAvatarSelf* av, F32 chin_z, F32 surface)
    {
        State& s = st();
        gAgent.setControlFlags(AGENT_CONTROL_STOP);   // the slow swim speed, as steer
        gAgent.clearControlFlags(AGENT_CONTROL_UP_POS | AGENT_CONTROL_NUDGE_UP_POS | AGENT_CONTROL_FAST_UP
                                 | AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_UP_NEG);
        const F32 err = surface - chin_z;   // > 0: the chin is under the surface
        const F64 t = now();
        const F64 held = t - s.mPressChangedAt;
        S32 press = s.mPress;
        if (press == 0)
        {
            if (held >= HOLD_SETTLE_S && std::fabs(err) > HOLD_START_BAND && (err > 0.f || av->mInAir))
            {
                press = err > 0.f ? 1 : -1;
                s.mPressFor = llclamp((F64)(std::fabs(err) * HOLD_PER_M_S), HOLD_MIN_PRESS_S, HOLD_MAX_PRESS_S);
            }
        }
        else if (held >= s.mPressFor)
        {
            press = 0;   // the pulse is long enough for the distance
        }
        if (press < 0 && !av->mInAir)
        {
            press = 0;   // never down while touching (flying + down + touching lands: steer)
        }
        if (press != s.mPress)
        {
            s.mPress = press;
            s.mPressChangedAt = t;
            ++s.mPressEdges;
        }
        if (press > 0)
        {
            gAgent.setControlFlags(AGENT_CONTROL_NUDGE_UP_POS);
        }
        else if (press < 0)
        {
            gAgent.setControlFlags(AGENT_CONTROL_NUDGE_UP_NEG);
        }
    }

    void endHold()
    {
        State& s = st();
        s.mHolding = false;
        if (s.mPress != 0)
        {
            s.mPress = 0;
            s.mPressChangedAt = now();
            ++s.mPressEdges;
        }
    }

    void request(const LLUUID& id, bool start)
    {
        if (id.notNull())
        {
            gAgent.sendAnimationRequest(id, start ? ANIM_REQUEST_START : ANIM_REQUEST_STOP);
        }
    }

    void stopPlaying()
    {
        State& s = st();
        if (s.mPlaying.notNull() && s.mPaddleStartAt == 0.0)
        {
            request(s.mPlaying, false);
        }
        s.mPlaying.setNull();
        s.mPaddleStartAt = 0.0;
    }

    void play(const LLUUID& id)
    {
        State& s = st();
        if (s.mPlaying == id)
        {
            return;
        }
        stopPlaying();
        s.mPlaying = id;
        s.mRequestedAt = now();
        s.mPaddleStartedAt = now();
        request(id, true);
    }

    void setMode(EMode mode)
    {
        State& s = st();
        if (s.mMode == mode)
        {
            return;
        }
        if (s.mMode == EMode::AO_PADDLE)
        {
            AOEngine::instance().setWolfSwimPaddle(false);
        }
        if (mode != EMode::WOLF_ANIMS && mode != EMode::OWN_PADDLE)
        {
            stopPlaying();
        }
        s.mMode = mode;
        if (mode == EMode::AO_PADDLE)
        {
            AOEngine::instance().setWolfSwimPaddle(true);
        }
    }

    // Once a session: are the two biped animations on this grid? A missing one is said, not only logged.
    void checkAssets()
    {
        State& s = st();
        if (s.mCheckedAssets || !gAssetStorage)
        {
            return;
        }
        s.mCheckedAssets = true;
        for (const LLUUID& id : { TREAD_ANIM, STROKE_ANIM })
        {
            if (gAssetStorage->hasLocalAsset(id, LLAssetType::AT_ANIMATION))
            {
                continue;
            }
            gAssetStorage->getAssetData(id, LLAssetType::AT_ANIMATION,
                [](const LLUUID& asset_id, LLAssetType::EType, void*, S32 status, LLExtStat)
                {
                    if (status != 0 && !st().mToldMissing)
                    {
                        st().mToldMissing = true;
                        LL_WARNS("WolfSwim") << "swim animation " << asset_id << " could not be fetched, status " << status << LL_ENDL;
                        problem("Swimming: the swim animations could not be loaded from this region (" + asset_id.asString()
                                + "), so you swim with the flying ones.");
                    }
                },
                nullptr, true);
        }
    }

    // Which animations go with the swim now (wolfswim.h).
    EMode chooseMode(LLVOAvatarSelf* av)
    {
        State& s = st();
        AOEngine& ao = AOEngine::instance();
        if (ao.hasWolfSwimStates())
        {
            return EMode::NONE;          // the AO swims already (AOEngine::getStateForMotion, mUnderWater)
        }
        if (!isQuadruped(av))
        {
            return EMode::WOLF_ANIMS;
        }
        if (ao.hasWalkingAnimations())
        {
            return EMode::AO_PADDLE;
        }
        if (s.mLearnedWalk.notNull() || s.mLearnedRun.notNull())
        {
            return EMode::OWN_PADDLE;
        }
        if (!s.mToldNoWalk)
        {
            s.mToldNoWalk = true;
            problem("Swimming: walk a few steps on land first, so your four-legged avatar can paddle with its own walk.");
        }
        return EMode::NONE;
    }

    void startSwimming()
    {
        State& s = st();
        LL_DEBUGS("WolfSwim") << "swim start" << LL_ENDL;   // [SWIM HEAD 2026-11-13/] for the live test's timeline
        s.mSwimming = true;
        s.mStroke = false;
        endHold();
        s.mHeadUp = false;   // [SWIM HEAD 2026-11-13/]
        s.mUpAtSurfaceSince = 0.0;
        s.mRisingOut = false;
        s.mStillSince = now();
        s.mWeTurnedFlyOn = false;
        if (!gAgent.getFlying())
        {
            gAgent.setFlying(true);   // refused on a no-fly parcel (llagent.cpp setFlying canFly) and by RLV @fly
            s.mWeTurnedFlyOn = gAgent.getFlying();
        }
        if (!gAgent.getFlying())
        {
            s.mSwimming = false;
            s.mSuppressed = true;
            if (!s.mToldNoFly)
            {
                s.mToldNoFly = true;
                problem("Swimming: flying is not allowed here, so you walk on the bottom instead.");
            }
            return;
        }
        if (!s.mToldStart)
        {
            s.mToldStart = true;
            tip("Swimming. The movement keys swim you in any direction. To leave the water, swim to the shore, or hold Up "
                "for a moment at the surface to fly out. Wolf World > Swimming switches it off.");
        }
    }

    // end_fly: switch flying off again if we switched it on (not when the person sat down: sitting did that already).
    // why: for the WolfSwim debug line.
    void stopSwimming(bool end_fly, const char* why)
    {
        State& s = st();
        if (!s.mSwimming)
        {
            return;
        }
        LL_DEBUGS("WolfSwim") << "swim stop: " << why << LL_ENDL;   // [SWIM HEAD 2026-11-13/] for the live test's timeline
        setMode(EMode::NONE);
        stopPlaying();
        s.mSwimming = false;
        endHold();
        s.mHeadUp = false;   // [SWIM HEAD 2026-11-13/]
        s.mUpAtSurfaceSince = 0.0;
        s.mRisingOut = false;
        if (end_fly && s.mWeTurnedFlyOn && gAgent.getFlying())
        {
            gAgent.setFlying(false);
        }
        s.mWeTurnedFlyOn = false;
    }

    // rising: the swimmer is flying out on purpose (RISE_OUT_HOLD_S) - their Up is left alone and nothing pushes down.
    void steer(LLVOAvatarSelf* av, F32 top, F32 water, bool rising)
    {
        // Slow: STOP held makes the region move us at AgentControlStopSlowVel (WolfSim ScenePresence.cs:1124, 2968-2977),
        // x4 while flying (AddNewMovement, :3993).
        gAgent.setControlFlags(AGENT_CONTROL_STOP);

        // Never up out of the water (unless rising out on purpose).
        if (!rising && top > water - TOP_STOP_BELOW)
        {
            gAgent.clearControlFlags(AGENT_CONTROL_UP_POS | AGENT_CONTROL_NUDGE_UP_POS | AGENT_CONTROL_FAST_UP);
        }
        // [SWIM HEAD 2026-11-13] judged PUSH_LEAD_S ahead at the present vertical speed, so the push ends before it has
        // overshot (the region's move arrives late - see PUSH_LEAD_S). The look-ahead only from the line where Up is taken
        // away (above), so it never works against a swimmer's own Up on the way up from below (review W1: the region adds
        // the direction flags together, WolfSim ScenePresence.cs:2875-2879).
        const bool over = top > water + PUSH_DOWN_ABOVE
            || (top > water - TOP_STOP_BELOW && top + av->getVelocity().mV[VZ] * PUSH_LEAD_S > water + PUSH_DOWN_ABOVE);
        if (!rising && over && av->mInAir)
        {
            // climbing with forward alone (a pitched body): forward goes too until it is back under
            gAgent.clearControlFlags(AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG);
            gAgent.setControlFlags(AGENT_CONTROL_NUDGE_UP_NEG);
        }
        // Never landing on the bottom: flying + down + touching makes the region land (ScenePresenceAnimator.cs "heldDown
        // && isColliding" -> actor.Flying = false) and the viewer too (LLAgent::propagate auto-land).
        if (!av->mInAir)
        {
            gAgent.clearControlFlags(AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_UP_NEG | AGENT_CONTROL_FAST_UP);
        }
    }

    void animate(LLVOAvatarSelf* av)
    {
        State& s = st();
        const F64 t = now();
        setMode(chooseMode(av));

        if (s.mMode == EMode::WOLF_ANIMS)
        {
            checkAssets();
            // [SWIM HEAD 2026-11-13] while the head is up its own up / down moves (holdHead, the waves) are no stroke:
            // only the speed across the water counts then. (holdHead's NUDGE_UP_POS / NUDGE_UP_NEG are not in `input`.)
            LLVector3 vel = av->getVelocity();
            if (s.mHeadUp)
            {
                vel.mV[VZ] = 0.f;
            }
            const F32 speed = vel.length();
            const bool input = (gAgent.getControlFlags() & (AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS
                                | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG | AGENT_CONTROL_NUDGE_AT_POS
                                | AGENT_CONTROL_NUDGE_AT_NEG | AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG)) != 0;
            if (input || speed > MOVING_SPEED)
            {
                s.mStroke = true;
                s.mStillSince = t;
            }
            else if (speed > STILL_SPEED)
            {
                s.mStillSince = t;
            }
            else if (t - s.mStillSince > STILL_FOR_S)
            {
                s.mStroke = false;
            }
            play(s.mStroke ? STROKE_ANIM : TREAD_ANIM);
        }
        else if (s.mMode == EMode::OWN_PADDLE)
        {
            const LLUUID paddle = s.mLearnedWalk.notNull() ? s.mLearnedWalk : s.mLearnedRun;
            if (s.mPlaying != paddle)
            {
                play(paddle);
            }
            else if (s.mPaddleStartAt != 0.0)
            {
                if (t >= s.mPaddleStartAt)
                {
                    s.mPaddleStartAt = 0.0;
                    s.mRequestedAt = s.mPaddleStartedAt = t;
                    request(paddle, true);
                }
            }
            else
            {
                // Something newer at the same or a higher priority (the region's fly / hover, or its override from a
                // scripted AO) started after the paddle: start the paddle again so it is the newest.
                LLMotion* mine = av->findMotion(paddle);
                for (const auto& seen : s.mFirstSeen)
                {
                    if (seen.first == paddle || seen.second <= s.mPaddleStartedAt
                        || !(isFlyMotion(seen.first) || !isBuiltIn(seen.first)))
                    {
                        continue;
                    }
                    LLMotion* other = av->findMotion(seen.first);
                    if (!mine || !other || other->getPriority() >= mine->getPriority())
                    {
                        request(paddle, false);
                        s.mPaddleStartAt = t + PADDLE_RESTART_GAP_S;
                        s.mPaddleStartedAt = s.mPaddleStartAt;   // anything that starts before the restart is older than it
                        break;
                    }
                }
            }
        }

        // ask again for one the region has not echoed back
        if (s.mPlaying.notNull() && s.mPaddleStartAt == 0.0 && !av->mSignaledAnimations.count(s.mPlaying)
            && t - s.mRequestedAt > REQUEST_AGAIN_S)
        {
            s.mRequestedAt = t;
            request(s.mPlaying, true);
        }
    }
}

namespace WolfSwim
{
    bool isSwimming()
    {
        return st().mSwimming;
    }

    void beforeAgentUpdate()
    {
        State& s = st();
        // [SWIM HEAD 2026-11-13] before STATE_STARTED the region's water height and the avatar's position are not real yet
        // (the region handshake sets the water: llviewerregion.cpp:3365; before it LLSurface::getWaterHeight gives
        // DEFAULT_WATER_HEIGHT) - live test logs showed a swim start / stop pair at every login
        if (!isAgentAvatarValid() || !gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
        {
            s.mSwimming = false;   // nothing to send to: the region forgets our animations with us
            endHold();
            s.mHeadUp = false;   // [SWIM HEAD 2026-11-13/]
            s.mChinOf = nullptr;   // [SWIM HEAD 2026-11-13/] the next self avatar looks its chin up again
            if (s.mMode == EMode::AO_PADDLE && AOEngine::instanceExists())
            {
                AOEngine::instance().setWolfSwimPaddle(false);
            }
            s.mMode = EMode::NONE;
            s.mPlaying.setNull();
            s.mPaddleStartAt = 0.0;
            return;
        }
        LLVOAvatarSelf* av = gAgentAvatarp;

        // keeps the first-seen times and what the avatar is, also while swimming (the times feed the paddle restart)
        sampleOnLand(av);

        static LLCachedControl<bool> enabled(gSavedSettings, "WolfSwimming", true);
        const bool wolf = WolfGrid::showsWolfRegion();
        const bool teleporting = gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        if (!enabled || !wolf || teleporting || av->isSitting() || !av->mRoot)
        {
            stopSwimming(!av->isSitting(), "off, not a Wolf region, teleport or sat");
            s.mSuppressed = false;
            return;
        }

        const LLVector3 root = av->mRoot->getWorldPosition();
        const F32 top = root.mV[VZ] + topAboveRoot(av);
        F32 water = 0.f;
        if (!waterHeight(gAgent.getPosGlobalFromAgent(LLVector3(root.mV[VX], root.mV[VY], top)), water))
        {
            stopSwimming(true, "no water height");
            return;
        }

        // [SWIM HEAD 2026-11-13] The chin and the drawn surface over it (ticket 802: treading holds the chin there). Read
        // only while swimming or about to start (the same test as the start, below), so walking on land costs nothing.
        F32 chin_z = 0.f, surface = 0.f;
        bool have_surface = false;
        if (s.mSwimming || top < water - ENTER_BELOW)
        {
            if (LLJoint* chin = chinJoint(av))
            {
                const LLVector3 c = chin->getWorldPosition();
                chin_z = c.mV[VZ];
                have_surface = drawnSurface(c, surface);
            }
        }
        // The raised head no longer decides once the hold has let go and the avatar stands on the bottom (wading ashore:
        // the top's own rule again) or its top is back inside the band, where the top's rule would not end the swim.
        if (s.mHeadUp && (!have_surface || (!s.mHolding && (!av->mInAir || top <= water + EXIT_ABOVE))))
        {
            s.mHeadUp = false;
        }
        // Out of the water: the top past the band (EXIT_ABOVE_IN_AIR for a swimmer touching nothing) - or, while the head
        // is held up (its top is then ~0.33 m over the water on the standard shape, by design), the chin past the band
        // over the drawn surface.
        // (Run 4, 10:30:40Z: a hold pulse that overshot to the chin 0.33 m over the surface ended the swim in mid-air
        // with the plain band - the in-air band for the chin too.) Review W3: the wider band changes nothing for any
        // other swimmer, because nothing else of the swim's own can put a swimmer touching nothing that high: Up is taken
        // away at TOP_STOP_BELOW and the push-down starts at PUSH_DOWN_ABOVE, so a top 0.35-1.0 m up in the air is
        // always momentum, a hold pulse or a wave, and the push-down / the hold bring it back. Rising out on purpose
        // (RISE_OUT_HOLD_S) uses the plain band on the top and keeps flying, as plain flying out of the water does.
        const F32 exit_above = (s.mSwimming && av->mInAir && !s.mRisingOut) ? EXIT_ABOVE_IN_AIR : EXIT_ABOVE;
        const bool out = (s.mHeadUp && !s.mRisingOut) ? chin_z > surface + exit_above : top > water + exit_above;

        // [WOLF SEA TUNNEL 2026-10-28] a head inside an air-filled volume (a sea tunnel, wolfdryvolumes.h) is out of the
        // water: walk, as on land
        const bool dry = !out && WolfDryVolumes::instance().contains(LLVector3(root.mV[VX], root.mV[VY], top));
        if (out || dry)
        {
            const bool flying_out = s.mRisingOut && !dry;
            stopSwimming(!flying_out, dry ? "in a dry volume" : (flying_out ? "flew out" : "out of the water"));
            s.mSuppressed = false;     // out of the water: the next dip swims again
            return;
        }

        if (s.mSwimming && !gAgent.getFlying())
        {
            // The person switched flying off (Fly button, the Home key ...): walk on the bottom until out of the water
            // or flying again.
            stopSwimming(false, "flying switched off");
            s.mSuppressed = true;
            return;
        }
        if (s.mSuppressed)
        {
            if (!gAgent.getFlying() || top >= water - ENTER_BELOW)
            {
                return;
            }
            s.mSuppressed = false;     // flying again while under: swim again (flying is the person's: left on at the end)
            startSwimming();
        }
        else if (!s.mSwimming)
        {
            if (top >= water - ENTER_BELOW)
            {
                return;
            }
            startSwimming();
        }
        if (!s.mSwimming)
        {
            return;
        }

        // [SWIM HEAD 2026-11-13] Treading: no key across the water or down (up is fine: that is surfacing) and hardly
        // moving across it. The hold starts near the surface only (SURFACE_REACH), then lasts until a stroke or a dive.
        // Never while a script has taken the up or down control (it would be sent our presses: WolfSim
        // ScenePresence.SendControlsToScripts folds the nudge flags into CONTROL_UP / CONTROL_DOWN, :6553-6554; the
        // viewer counts taken controls from ScriptControlChange,
        // LLAgent::processScriptControlChange, llagent.cpp:4599), nor with a pitched body (HOLD_MAX_PITCH): today's
        // swim then.
        const U32 keys = gAgent.getControlFlags();   // the person's own this frame (resetControlFlags after each send)
        const bool across_or_down = (keys & (AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS
                                     | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG
                                     | AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG | AGENT_CONTROL_UP_NEG
                                     | AGENT_CONTROL_NUDGE_UP_NEG)) != 0;
        LLVector3 across = av->getVelocity();
        across.mV[VZ] = 0.f;
        const bool scripted = gAgent.isControlTaken(CONTROL_UP_POS_INDEX) || gAgent.isControlTaken(CONTROL_UP_NEG_INDEX);
        // [SWIM HEAD 2026-11-13] Up held at the surface (where steer takes it away, or the head held up) for
        // RISE_OUT_HOLD_S: rise out. Let go before the top is out and the swim goes on as before.
        const bool up_key = (keys & AGENT_CONTROL_UP_POS) != 0;
        const bool at_surface = s.mHeadUp || top > water - TOP_STOP_BELOW;
        if (!up_key || !at_surface)
        {
            s.mUpAtSurfaceSince = 0.0;
            s.mRisingOut = s.mRisingOut && up_key;   // keeps rising while Up stays down, once started
        }
        else if (s.mUpAtSurfaceSince == 0.0)
        {
            s.mUpAtSurfaceSince = now();
        }
        else if (now() - s.mUpAtSurfaceSince >= RISE_OUT_HOLD_S)
        {
            s.mRisingOut = true;
        }
        const bool level = std::fabs(gAgent.getFrameAgent().getAtAxis().mV[VZ]) <= HOLD_MAX_PITCH;
        if (!have_surface || scripted || !level || across_or_down || across.length() > MOVING_SPEED || s.mRisingOut)
        {
            endHold();
        }
        else if (!s.mHolding && top >= surface - SURFACE_REACH)
        {
            s.mHolding = true;
        }

        if (s.mHolding)
        {
            s.mHeadUp = true;
            holdHead(av, chin_z, surface);
        }
        else
        {
            steer(av, top, water, s.mRisingOut);
        }
        animate(av);

        // [SWIM HEAD 2026-11-13] debug only (logcontrol tag WolfSwim), four times a second: what the hold works from
        static F64 next_debug = 0.0;
        if (now() >= next_debug)
        {
            next_debug = now() + 0.25;
            static U32 last_edges = 0;
            LL_DEBUGS("WolfSwim") << "swim hold=" << s.mHolding << " headup=" << s.mHeadUp << " stroke=" << s.mStroke
                                  << " rising=" << s.mRisingOut
                                  << " press=" << s.mPress << " edges=" << (s.mPressEdges - last_edges) << "/0.25s"
                                  << " edges_total=" << s.mPressEdges << " scripted=" << scripted << " level=" << level
                                  << " chin=" << chin_z << " head=" << (av->mHeadp ? av->mHeadp->getWorldPosition().mV[VZ] : 0.f)
                                  << " surface=" << surface << " water=" << water << " top=" << top
                                  << " vz=" << av->getVelocity().mV[VZ] << " inair=" << av->mInAir
                                  << " flags=" << std::hex << gAgent.getControlFlags() << std::dec << LL_ENDL;
            last_edges = s.mPressEdges;
        }
    }
}
