/**
 * @file   llagentlistener.cpp
 * @author Brad Kittenbrink
 * @date   2009-07-10
 * @brief  Implementation for llagentlistener.
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
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

#include "llagentlistener.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llvoavatar.h"
#include "llcommandhandler.h"
#include "llinventorymodel.h"
#include "llslurl.h"
#include "llurldispatcher.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "lltoolgrab.h"
#include "llhudeffectlookat.h"
#include "llviewercamera.h"
#include "llcallbacklist.h"     // <WolfViewer> doAfterInterval (WolfLeap teleports)
#include "llregionhandle.h"     // <WolfViewer> from_region_handle
#include "llworldmapmessage.h"  // <WolfViewer> sendNamedRegionRequest
#include <cmath>                // <WolfViewer> std::isfinite
#include <memory>               // <WolfViewer> std::shared_ptr
#include <set>                  // <WolfViewer> pending lookups
// [RLVa:KB] - Checked: 2011-05-22 (RLVa-1.3.1a)
#include "rlvactions.h"
#include "rlvhandler.h"
#include "llvoavatarself.h"
// [/RLVa:KB]

LLAgentListener::LLAgentListener(LLAgent &agent)
  : LLEventAPI("LLAgent",
               "LLAgent listener to (e.g.) teleport, sit, stand, etc."),
    mAgent(agent)
{
    // <WolfViewer 2026-10-31> WolfLeap teleports (Paul: they "never work"). See requestTeleport().
    add("requestTeleport",
        "Teleport: [\"regionname\"], [\"x\"], [\"y\"], [\"z\"] (region-local metres, any size of region;\n"
        "default 128, 128, 0).\n"
        "If [\"skip_confirmation\"] is true: look the region up by name and teleport straight there, then\n"
        "reply once with [\"success\"] and either [\"region\"], [\"position\"], [\"global\"] or [\"error\"]\n"
        "(region not found, teleport failed: <reason>, no answer in time).\n"
        "Otherwise dispatch a SLURL as a click would (place details or teleport, by SLURLTeleportDirectly)\n"
        "and reply at once.",
        &LLAgentListener::requestTeleport);
    // </WolfViewer>
    add("requestSit",
        "[\"obj_uuid\"]: id of object to sit on, use this or [\"position\"] to indicate the sit target"
        "[\"position\"]: region position {x, y, z} where to find closest object to sit on",
        &LLAgentListener::requestSit);
    add("requestStand",
        "Ask to stand up",
        &LLAgentListener::requestStand);
    add("requestTouch",
        "[\"obj_uuid\"]: id of object to touch, use this or [\"position\"] to indicate the object to touch"
        "[\"position\"]: region position {x, y, z} where to find closest object to touch"
        "[\"face\"]: optional object face number to touch[Default: 0]",
        &LLAgentListener::requestTouch);
    add("resetAxes",
        "Set the agent to a fixed orientation (optionally specify [\"lookat\"] = array of [x, y, z])",
        &LLAgentListener::resetAxes);
    add("getPosition",
        "Send information about the agent's position and orientation on [\"reply\"]:\n"
        "[\"region\"]: array of region {x, y, z} position\n"
        "[\"global\"]: array of global {x, y, z} position\n"
        "[\"euler\"]: map of {roll, pitch, yaw}\n"
        "[\"quat\"]:  array of [x, y, z, w] quaternion values",
        &LLAgentListener::getPosition,
        LLSDMap("reply", LLSD()));
    add("startAutoPilot",
        "Start the autopilot system using the following parameters:\n"
        "[\"target_global\"]: array of target global {x, y, z} position\n"
        "[\"stop_distance\"]: maximum stop distance from target [default: autopilot guess]\n"
        "[\"target_rotation\"]: array of [x, y, z, w] quaternion values [default: no target]\n"
        "[\"rotation_threshold\"]: target maximum angle from target facing rotation [default: 0.03 radians]\n"
        "[\"behavior_name\"]: name of the autopilot behavior [default: \"\"]\n"
        "[\"allow_flying\"]: allow flying during autopilot [default: True]\n"
        "event with [\"success\"] flag is sent to 'LLAutopilot' event pump, when auto pilot is terminated",
        &LLAgentListener::startAutoPilot,
        llsd::map("target_global", LLSD()));
    add("getAutoPilot",
        "Send information about current state of the autopilot system to [\"reply\"]:\n"
        "[\"enabled\"]: boolean indicating whether or not autopilot is enabled\n"
        "[\"target_global\"]: array of target global {x, y, z} position\n"
        "[\"leader_id\"]: uuid of target autopilot is following\n"
        "[\"stop_distance\"]: maximum stop distance from target\n"
        "[\"target_distance\"]: last known distance from target\n"
        "[\"use_rotation\"]: boolean indicating if autopilot has a target facing rotation\n"
        "[\"target_facing\"]: array of {x, y} target direction to face\n"
        "[\"rotation_threshold\"]: target maximum angle from target facing rotation\n"
        "[\"behavior_name\"]: name of the autopilot behavior",
        &LLAgentListener::getAutoPilot,
        llsd::map("reply", LLSD()));
    add("startFollowPilot",
        "[\"leader_id\"]: uuid of target to follow using the autopilot system (optional with avatar_name)\n"
        "[\"avatar_name\"]: avatar name to follow using the autopilot system (optional with leader_id)\n"
        "[\"allow_flying\"]: allow flying during autopilot [default: True]\n"
        "[\"stop_distance\"]: maximum stop distance from target [default: autopilot guess]",
        &LLAgentListener::startFollowPilot,
        llsd::map("reply", LLSD()));
    add("setAutoPilotTarget",
        "Update target for currently running autopilot:\n"
        "[\"target_global\"]: array of target global {x, y, z} position",
        &LLAgentListener::setAutoPilotTarget);
    add("stopAutoPilot",
        "Stop the autopilot system:\n"
        "[\"user_cancel\"] indicates whether or not to act as though user canceled autopilot [default: false]",
        &LLAgentListener::stopAutoPilot);
    add("lookAt",
        "[\"type\"]: number to indicate the lookAt type, 0 to clear\n"
        "[\"obj_uuid\"]: id of object to look at, use this or [\"position\"] to indicate the target\n"
        "[\"position\"]: region position {x, y, z} where to find closest object or avatar to look at",
        &LLAgentListener::lookAt);
    add("getGroups",
        "Send information about the agent's groups on [\"reply\"]:\n"
        "[\"groups\"]: array of group information\n"
        "[\"id\"]: group id\n"
        "[\"name\"]: group name\n"
        "[\"insignia\"]: group insignia texture id\n"
        "[\"notices\"]: boolean indicating if this user accepts notices from this group\n"
        "[\"display\"]: boolean indicating if this group is listed in the user's profile\n"
        "[\"contrib\"]: user's land contribution to this group\n",
        &LLAgentListener::getGroups,
        LLSDMap("reply", LLSD()));
    //camera params are similar to LSL, see https://wiki.secondlife.com/wiki/LlSetCameraParams
    add("setCameraParams",
        "Set Follow camera params, and then activate it:\n"
        "[\"camera_pos\"]: vector3, camera position in region coordinates\n"
        "[\"focus_pos\"]: vector3, what the camera is aimed at (in region coordinates)\n"
        "[\"focus_offset\"]: vector3, adjusts the camera focus position relative to the target, default is (1, 0, 0)\n"
        "[\"distance\"]: float (meters), distance the camera wants to be from its target, default is 3\n"
        "[\"focus_threshold\"]: float (meters), sets the radius of a sphere around the camera's target position within which its focus is not affected by target motion, default is 1\n"
        "[\"camera_threshold\"]: float (meters), sets the radius of a sphere around the camera's ideal position within which it is not affected by target motion, default is 1\n"
        "[\"focus_lag\"]: float (seconds), how much the camera lags as it tries to aim towards the target, default is 0.1\n"
        "[\"camera_lag\"]: float (seconds), how much the camera lags as it tries to move towards its 'ideal' position, default is 0.1\n"
        "[\"camera_pitch\"]: float (degrees), adjusts the angular amount that the camera aims straight ahead vs. straight down, maintaining the same distance, default is 0\n"
        "[\"behindness_angle\"]: float (degrees), sets the angle in degrees within which the camera is not constrained by changes in target rotation, default is 10\n"
        "[\"behindness_lag\"]: float (seconds), sets how strongly the camera is forced to stay behind the target if outside of behindness angle, default is 0\n"
        "[\"camera_locked\"]: bool, locks the camera position so it will not move\n"
        "[\"focus_locked\"]: bool, locks the camera focus so it will not move",
        &LLAgentListener::setFollowCamParams);
    add("setFollowCamActive",
        "Turns on or off scripted control of the camera using boolean [\"active\"]",
        &LLAgentListener::setFollowCamActive,
        llsd::map("active", LLSD()));
    add("removeCameraParams",
        "Reset Follow camera params",
        &LLAgentListener::removeFollowCamParams);

    add("playAnimation",
        "Play [\"item_id\"] animation locally (by default) or [\"inworld\"] (when set to true)",
        &LLAgentListener::playAnimation,
        llsd::map("item_id", LLSD(), "reply", LLSD()));
    add("stopAnimation",
        "Stop playing [\"item_id\"] animation",
        &LLAgentListener::stopAnimation,
        llsd::map("item_id", LLSD(), "reply", LLSD()));
    add("getAnimationInfo",
        "Return information about [\"item_id\"] animation",
        &LLAgentListener::getAnimationInfo,
        llsd::map("item_id", LLSD(), "reply", LLSD()));

    add("getID",
        "Return your own avatar ID",
        &LLAgentListener::getID,
        llsd::map("reply", LLSD()));

    add("getNearbyAvatarsList",
        "Return result set key [\"result\"] for nearby avatars in a range of [\"dist\"]\n"
        "if [\"dist\"] is not specified, 'RenderFarClip' setting is used\n"
        "reply contains \"result\" table with \"id\", \"name\", \"global_pos\", \"region_pos\", \"region_id\" fields",
        &LLAgentListener::getNearbyAvatarsList,
        llsd::map("reply", LLSD()));

    add("getNearbyObjectsList",
        "Return result set key [\"result\"] for nearby objects in a range of [\"dist\"]\n"
        "if [\"dist\"] is not specified, 'RenderFarClip' setting is used\n"
        "reply contains \"result\" table with \"id\", \"global_pos\", \"region_pos\", \"region_id\" fields",
        &LLAgentListener::getNearbyObjectsList,
        llsd::map("reply", LLSD()));

    add("getAgentScreenPos",
        "Return screen position of the [\"avatar_id\"] avatar or own avatar if not specified\n"
        "reply contains \"x\", \"y\" coordinates and \"onscreen\" flag to indicate if it's actually in within the current window\n"
        "avatar render position is used as the point",
        &LLAgentListener::getAgentScreenPos,
        llsd::map("reply", LLSD()));
}

// <WolfViewer 2026-10-31> WolfLeap teleports. Paul: teleports requested through WolfLeap "never work".
// The stock skip_confirmation path handed [region, x, y, z] to the "teleport" command handler, but in an OPENSIM
// build that handler (llurldispatcher.cpp LLTeleportHandler::handle) reads the tokens as [grid, region, x, y, z]
// (dispatchApp puts the grid first) and builds "hop://<tokens>/" from them (fsslurl.cpp LLSLURL(const LLSD&, bool)),
// so the x coordinate became the region name, y became x, z became y - TROMSO/924/1152/47 went to a region
// called "924" at 1152,47 (WolfViewer.log 31 Oct 06:00:19: "pos_global { 1152, 47, 0 }") - and it still asked for
// a click (TeleportViaSLAPP). It never replied either, so the caller only ever saw a time-out.
// Now: look the region up by name (the same map-name search the world map uses), teleport to its origin plus the
// region-local position (var regions: the simulator maps the 256 m tile back to the region,
// EntityTransferModule.GetTeleportDestinationRegion), and reply once with the outcome.
namespace
{
    // A region lookup that gets no answer in this time is reported as failed.
    constexpr F32 LEAP_TP_LOOKUP_TIMEOUT = 20.f;
    // A started teleport that has neither finished nor failed in this time is reported as failed. The viewer's own
    // give-up (llviewerdisplay.cpp update_tp_display, TELEPORT_EXPIRY) sends no event at all.
    constexpr F32 LEAP_TP_RESULT_TIMEOUT = 60.f;
    // Bigger region-local values are refused (they would not survive the cast to F32 and no region is that big).
    constexpr F64 LEAP_TP_MAX_METRES = 1000000.0;

    // Names (lower case) with a WolfLeap lookup waiting for its answer. The exact-name search keeps one callback per
    // name and silently drops a second one (fsworldmapmessage.cpp sendExactNamedRegionRequest: try_emplace), so a
    // second request for the same name is refused instead of left to time out.
    std::set<std::string> sLeapTpLookups;

    struct LeapTeleport
    {
        LLSD request;               // the LEAP request: its ["reply"] pump and ["reqid"] address the one reply
        std::string region_name;    // as asked for
        LLVector3 local_pos;        // region-local metres
        std::string lookup_key;     // its entry in sLeapTpLookups while the lookup is waiting, else empty
        bool looked_up{ false };    // the region search answered (the lookup time-out no longer applies)
        bool done{ false };
        LLTempBoundListener finished_conn;  // "LLTeleport" {success: true} (llviewerparcelmgr.cpp postTeleportFinished)
        LLTempBoundListener failed_conn;    // "WolfTeleportFailed" {reason, message_id} (llviewermessage.cpp)
    };
    using LeapTeleportPtr = std::shared_ptr<LeapTeleport>;
    using LeapTeleportWeak = std::weak_ptr<LeapTeleport>;

    // The region name as the simulator spells a plain one: a hypergrid "grid:port:Region" or ".../Region" ends in it.
    std::string leap_tp_plain_name(const std::string& name)
    {
        const std::string::size_type cut = name.find_last_of(":/");
        std::string plain = (cut == std::string::npos) ? name : name.substr(cut + 1);
        LLStringUtil::trim(plain);
        return plain;
    }

    void leap_tp_end_lookup(const LeapTeleportPtr& tp)
    {
        if (!tp->lookup_key.empty())
        {
            sLeapTpLookups.erase(tp->lookup_key);
            tp->lookup_key.clear();
        }
    }

    void leap_tp_finish(const LeapTeleportPtr& tp, LLSD reply)
    {
        if (tp->done)
        {
            return;
        }
        tp->done = true;
        leap_tp_end_lookup(tp);
        tp->finished_conn.disconnect();
        tp->failed_conn.disconnect();
        if (reply.has("error"))
        {
            reply["success"] = false;
            LL_WARNS("WolfLeap") << "requestTeleport " << tp->region_name << " " << tp->local_pos << ": "
                                 << reply["error"].asString() << LL_ENDL;
        }
        else
        {
            reply["success"] = true;
            LL_INFOS("WolfLeap") << "requestTeleport " << tp->region_name << " " << tp->local_pos << ": "
                                 << reply["message"].asString() << LL_ENDL;
        }
        sendReply(reply, tp->request);
    }

    LLSD leap_tp_error(const std::string& text)
    {
        LLSD reply(LLSD::emptyMap());
        reply["error"] = text;
        return reply;
    }

    // Where the agent is now, for every reply after a teleport was started.
    void leap_tp_add_location(LLSD& reply)
    {
        LLViewerRegion* regionp = gAgent.getRegion();
        reply["region"] = regionp ? regionp->getName() : std::string();
        reply["position"] = ll_sd_from_vector3(gAgent.getPositionRegion());
        reply["global"] = ll_sd_from_vector3d(gAgent.getPositionGlobal());
    }

    void leap_tp_start(const LeapTeleportPtr& tp, U64 region_handle)
    {
        if (tp->done)
        {
            return;     // the lookup already timed out and was reported
        }
        tp->looked_up = true;
        leap_tp_end_lookup(tp);
        // The simulator ends a name search with a block named as the search at grid 0,0
        // (MapSearchModule.cs OnMapNameRequestHandler / AddFinalBlock), and the exact-name matcher
        // (fsworldmapmessage.cpp processExactNamedRegionResponse) hands that block on when no region matched:
        // handle 0 means "no such region", not "the region at 0,0".
        if (region_handle == 0)
        {
            leap_tp_finish(tp, leap_tp_error("region not found: " + tp->region_name));
            return;
        }
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        {
            leap_tp_finish(tp, leap_tp_error("teleport failed: another teleport is already in progress"));
            return;
        }

        LeapTeleportWeak weak(tp);
        tp->finished_conn = LLEventPumps::instance().obtain("LLTeleport").listen(
            LLEventPump::inventName("WolfLeapTeleport"),
            [weak](const LLSD& event)
            {
                LeapTeleportPtr self = weak.lock();
                // {success: false} comes first on every TeleportFailed, even one that is retried
                // (llviewermessage.cpp process_teleport_failed), so failures are taken from WolfTeleportFailed.
                if (self && event["success"].asBoolean())
                {
                    LLSD reply(LLSD::emptyMap());
                    leap_tp_add_location(reply);
                    const std::string arrived = reply["region"].asString();
                    const std::string wanted = leap_tp_plain_name(self->region_name);
                    if (!wanted.empty() && LLStringUtil::compareInsensitive(arrived, wanted) != 0)
                    {
                        // Past the far edge of the region the simulator picks whatever region holds that spot.
                        reply["error"] = "teleport failed: arrived in " + arrived + ", not " + self->region_name
                                         + " - is the position inside that region?";
                    }
                    else
                    {
                        reply["message"] = "Teleported to " + arrived;
                    }
                    leap_tp_finish(self, reply);
                }
                return false;
            });
        tp->failed_conn = LLEventPumps::instance().obtain("WolfTeleportFailed").listen(
            LLEventPump::inventName("WolfLeapTeleport"),
            [weak](const LLSD& event)
            {
                if (LeapTeleportPtr self = weak.lock())
                {
                    LLSD reply = leap_tp_error("teleport failed: " + event["reason"].asString());
                    reply["message_id"] = event["message_id"];
                    leap_tp_add_location(reply);
                    leap_tp_finish(self, reply);
                }
                return false;
            });

        const LLVector3d global_pos = from_region_handle(region_handle) + LLVector3d(tp->local_pos);
        gAgent.teleportViaLocation(global_pos);
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
        {
            // teleportViaLocation / doTeleportViaLocation return without a word when RLVa forbids it or the agent
            // has no region.
            LLSD reply = leap_tp_error("teleport failed: the viewer did not start it (RLV restrictions, or no region)");
            leap_tp_add_location(reply);
            leap_tp_finish(tp, reply);
            return;
        }

        doAfterInterval([tp]()
            {
                // Not necessarily a failure: the viewer's own give-up and a cancelled teleport post nothing, and a
                // slow one may still arrive. "region"/"position" say where the agent is now.
                LLSD reply = leap_tp_error(llformat("teleport result unknown: nothing after %.0f seconds"
                                                    " (it may still finish - check region/position)",
                                                    LEAP_TP_RESULT_TIMEOUT));
                reply["timeout"] = true;
                leap_tp_add_location(reply);
                leap_tp_finish(tp, reply);
            }, LEAP_TP_RESULT_TIMEOUT);
    }
}

void LLAgentListener::requestTeleport(LLSD const & event_data) const
{
    if(event_data["skip_confirmation"].asBoolean())
    {
        LeapTeleportPtr tp = std::make_shared<LeapTeleport>();
        tp->request = event_data;
        tp->region_name = event_data["regionname"].asString();
        LLStringUtil::trim(tp->region_name);
        if (tp->region_name.empty())
        {
            leap_tp_finish(tp, leap_tp_error("requestTeleport needs [\"regionname\"]"));
            return;
        }
        // Region-local metres; the stock defaults (llurldispatcher.cpp LLTeleportHandler::from_event).
        static const char* const AXES[3] = { "x", "y", "z" };
        static const F32 DEFAULTS[3] = { 128.f, 128.f, 0.f };
        for (S32 i = 0; i < 3; ++i)
        {
            const LLSD& value = event_data[AXES[i]];
            if (value.isUndefined())
            {
                tp->local_pos.mV[i] = DEFAULTS[i];
                continue;
            }
            const F64 metres = value.asReal();
            if (!(value.isReal() || value.isInteger()) || !std::isfinite(metres) || metres < 0.0
                || metres > LEAP_TP_MAX_METRES)
            {
                leap_tp_finish(tp, leap_tp_error(std::string("requestTeleport: [\"") + AXES[i]
                                                 + "\"] must be a number of metres from 0 to 1000000"));
                return;
            }
            tp->local_pos.mV[i] = (F32)metres;
        }
        if (!gAgent.getRegion())
        {
            leap_tp_finish(tp, leap_tp_error("teleport failed: not logged in to a region"));
            return;
        }

        std::string key = tp->region_name;
        LLStringUtil::toLower(key);
        if (!sLeapTpLookups.insert(key).second)
        {
            leap_tp_finish(tp, leap_tp_error("a WolfLeap lookup of " + tp->region_name
                                             + " is still waiting for its answer - try again when it ends"));
            return;
        }
        tp->lookup_key = key;

        LeapTeleportWeak weak(tp);
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(tp->region_name,
            [weak](U64 region_handle, const std::string& /*url*/, const LLUUID& /*snapshot_id*/, bool /*teleport*/)
            {
                if (LeapTeleportPtr self = weak.lock())
                {
                    leap_tp_start(self, region_handle);
                }
            },
            std::string(),  // no SLURL: the callback above does the teleport
            false);
        doAfterInterval([tp]()
            {
                if (tp->looked_up)
                {
                    return;     // the teleport has its own time-out
                }
                leap_tp_finish(tp, leap_tp_error(llformat("region lookup got no answer in %.0f seconds: ",
                                                          LEAP_TP_LOOKUP_TIMEOUT) + tp->region_name));
            }, LEAP_TP_LOOKUP_TIMEOUT);
    }
    else
    {
        Response response(LLSD(), event_data);
        std::string url = LLSLURL(event_data["regionname"],
                                  LLVector3((F32)event_data["x"].asReal(),
                                            (F32)event_data["y"].asReal(),
                                            (F32)event_data["z"].asReal())).getSLURLString();
        LLURLDispatcher::dispatch(url, LLCommandHandler::NAV_TYPE_CLICKED, NULL, false);
        response["message"] = "Dispatched " + url + " as a click: SLURLTeleportDirectly decides whether it teleports"
                              " or shows the place details, and no result follows - use skip_confirmation for one";
    }
}
// </WolfViewer>

void LLAgentListener::requestSit(LLSD const & event_data) const
{
    //mAgent.getAvatarObject()->sitOnObject();
    // shamelessly ripped from llviewermenu.cpp:handle_sit_or_stand()
    // *TODO - find a permanent place to share this code properly.
    Response response(LLSD(), event_data);
    LLViewerObject *object = NULL;
    if (event_data.has("obj_uuid"))
    {
        object = gObjectList.findObject(event_data["obj_uuid"]);
    }
    else if (event_data.has("position"))
    {
        LLVector3 target_position = ll_vector3_from_sd(event_data["position"]);
        object = findObjectClosestTo(target_position, true);
    }
    else
    {
        //just sit on the ground
        mAgent.setControlFlags(AGENT_CONTROL_SIT_ON_GROUND);
        return;
    }

// [RLVa:KB] - Checked: 2010-03-06 (RLVa-1.2.0c) | Modified: RLVa-1.1.0j
    // TODO-RLVa: [RLVa-1.2.1] Figure out how to call this?
    if ( (rlv_handler_t::isEnabled()) && (!RlvActions::canSit(object)) )
    {
        return;
    }
// [/RLVa:KB]

    if (object && object->getPCode() == LL_PCODE_VOLUME)
    {
// [RLVa:KB] - Checked: 2010-08-29 (RLVa-1.2.1c) | Added: RLVa-1.2.1c
        if ( (gRlvHandler.hasBehaviour(RLV_BHVR_STANDTP)) && (isAgentAvatarValid()) )
        {
            if (gAgentAvatarp->isSitting())
            {
                gAgent.standUp();
                return;
            }
            gRlvHandler.setSitSource(gAgent.getPositionGlobal());
        }
// [/RLVa:KB]

        gMessageSystem->newMessageFast(_PREHASH_AgentRequestSit);
        gMessageSystem->nextBlockFast(_PREHASH_AgentData);
        gMessageSystem->addUUIDFast(_PREHASH_AgentID, mAgent.getID());
        gMessageSystem->addUUIDFast(_PREHASH_SessionID, mAgent.getSessionID());
        gMessageSystem->nextBlockFast(_PREHASH_TargetObject);
        gMessageSystem->addUUIDFast(_PREHASH_TargetID, object->mID);
        gMessageSystem->addVector3Fast(_PREHASH_Offset, LLVector3(0,0,0));

        object->getRegion()->sendReliableMessage();
    }
    else
    {
        response.error("requestSit could not find the sit target");
    }
}

void LLAgentListener::requestStand(LLSD const & event_data) const
{
// [RLVa:KB] - Checked: 2010-03-07 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
    // TODO-RLVa: [RLVa-1.2.1] Figure out how to call this?
    if ( (rlv_handler_t::isEnabled()) && (!RlvActions::canStand()) )
    {
        return;
    }
// [/RLVa:KB]

    mAgent.setControlFlags(AGENT_CONTROL_STAND_UP);
}


LLViewerObject * LLAgentListener::findObjectClosestTo(const LLVector3 & position, bool sit_target) const
{
    LLViewerObject *object = NULL;

    // Find the object closest to that position
    F32 min_distance = 10000.0f;        // Start big
    S32 num_objects = gObjectList.getNumObjects();
    S32 cur_index = 0;
    while (cur_index < num_objects)
    {
        LLViewerObject * cur_object = gObjectList.getObject(cur_index++);
        if (cur_object && !cur_object->isAttachment())
        {
            if(sit_target && (cur_object->getPCode() != LL_PCODE_VOLUME))
            {
                continue;
            }
            // Calculate distance from the target position
            LLVector3 target_diff = cur_object->getPositionRegion() - position;
            F32 distance_to_target = target_diff.length();
            if (distance_to_target < min_distance)
            {   // Found an object closer
                min_distance = distance_to_target;
                object = cur_object;
            }
        }
    }

    return object;
}


void LLAgentListener::requestTouch(LLSD const & event_data) const
{
    LLViewerObject *object = NULL;

    if (event_data.has("obj_uuid"))
    {
        object = gObjectList.findObject(event_data["obj_uuid"]);
    }
    else if (event_data.has("position"))
    {
        LLVector3 target_position = ll_vector3_from_sd(event_data["position"]);
        object = findObjectClosestTo(target_position);
    }

    S32 face = 0;
    if (event_data.has("face"))
    {
        face = event_data["face"].asInteger();
    }

    if (object && object->getPCode() == LL_PCODE_VOLUME)
    {
        // Fake enough pick info to get it to (hopefully) work
        LLPickInfo pick;
        pick.mObjectFace = face;

        /*
        These values are sent to the simulator, but face seems to be easiest to use

        pick.mUVCoords   "UVCoord"
        pick.mSTCoords  "STCoord"
        pick.mObjectFace    "FaceIndex"
        pick.mIntersection  "Position"
        pick.mNormal    "Normal"
        pick.mBinormal  "Binormal"
        */

        // A touch is a sketchy message sequence ... send a grab, immediately
        // followed by un-grabbing, crossing fingers and hoping packets arrive in
        // the correct order
        send_ObjectGrab_message(object, pick, LLVector3::zero);
        send_ObjectDeGrab_message(object, pick);
    }
    else
    {
        LL_WARNS() << "LLAgent requestTouch could not find the touch target "
            << event_data["obj_uuid"].asUUID() << LL_ENDL;
    }
}


void LLAgentListener::resetAxes(const LLSD& event_data) const
{
    if (event_data.has("lookat"))
    {
        mAgent.resetAxes(ll_vector3_from_sd(event_data["lookat"]));
    }
    else
    {
        // no "lookat", default call
        mAgent.resetAxes();
    }
}

void LLAgentListener::getPosition(const LLSD& event_data) const
{
    F32 roll, pitch, yaw;
    LLQuaternion quat(mAgent.getQuat());
    quat.getEulerAngles(&roll, &pitch, &yaw);

    LLSD reply = LLSD::emptyMap();
    reply["quat"] = llsd_copy_array(boost::begin(quat.mQ), boost::end(quat.mQ));
    reply["euler"] = LLSD::emptyMap();
    reply["euler"]["roll"] = roll;
    reply["euler"]["pitch"] = pitch;
    reply["euler"]["yaw"] = yaw;
    reply["region"] = ll_sd_from_vector3(mAgent.getPositionRegion());   // <WolfViewer> region frame; agent frame may be rebased
    reply["global"] = ll_sd_from_vector3d(mAgent.getPositionGlobal());

    sendReply(reply, event_data);
}


void LLAgentListener::startAutoPilot(LLSD const & event_data)
{
    LLQuaternion* target_rotation = NULL;
    if (event_data.has("target_rotation"))
    {
        LLQuaternion target_rotation_value = ll_quaternion_from_sd(event_data["target_rotation"]);
        target_rotation = &target_rotation_value;
    }

    F32 rotation_threshold = 0.03f;
    if (event_data.has("rotation_threshold"))
    {
        rotation_threshold = (F32)event_data["rotation_threshold"].asReal();
    }

    bool allow_flying = true;
    if (event_data.has("allow_flying"))
    {
        allow_flying = (bool) event_data["allow_flying"].asBoolean();
        mAgent.setFlying(allow_flying);
    }

    F32 stop_distance = 0.f;
    if (event_data.has("stop_distance"))
    {
        stop_distance = (F32)event_data["stop_distance"].asReal();
    }

    std::string behavior_name = LLCoros::getName();
    if (event_data.has("behavior_name"))
    {
        behavior_name = event_data["behavior_name"].asString();
    }

    // Clear follow target, this is doing a path
    mFollowTarget.setNull();

    auto finish_cb = [](bool success, void*)
    {
        LLEventPumps::instance().obtain("LLAutopilot").post(llsd::map("success", success));
    };

    mAgent.startAutoPilotGlobal(ll_vector3d_from_sd(event_data["target_global"]),
                                behavior_name,
                                target_rotation,
                                finish_cb, NULL,
                                stop_distance,
                                rotation_threshold,
                                allow_flying);
}

void LLAgentListener::getAutoPilot(const LLSD& event_data) const
{
    Response reply(LLSD(), event_data);

    LLSD::Boolean enabled = mAgent.getAutoPilot();
    reply["enabled"] = enabled;

    reply["target_global"] = ll_sd_from_vector3d(mAgent.getAutoPilotTargetGlobal());

    reply["leader_id"] = mAgent.getAutoPilotLeaderID();

    reply["stop_distance"] = mAgent.getAutoPilotStopDistance();

    reply["target_distance"] = mAgent.getAutoPilotTargetDist();
    if (!enabled &&
        mFollowTarget.notNull())
    {   // Get an actual distance from the target object we were following
        LLViewerObject * target = gObjectList.findObject(mFollowTarget);
        if (target)
        {   // Found the target AV, return the actual distance to them as well as their ID
            LLVector3 difference = target->getPositionRegion() - mAgent.getPositionRegion();   // <WolfViewer> both region frame; agent frame may be rebased
            reply["target_distance"] = difference.length();
            reply["leader_id"] = mFollowTarget;
        }
    }

    reply["use_rotation"] = (LLSD::Boolean) mAgent.getAutoPilotUseRotation();
    reply["target_facing"] = ll_sd_from_vector3(mAgent.getAutoPilotTargetFacing());
    reply["rotation_threshold"] = mAgent.getAutoPilotRotationThreshold();
    reply["behavior_name"] = mAgent.getAutoPilotBehaviorName();
    reply["fly"] = (LLSD::Boolean) mAgent.getFlying();
}

void LLAgentListener::startFollowPilot(LLSD const & event_data)
{
    Response response(LLSD(), event_data);
    LLUUID target_id;

    bool allow_flying = true;
    if (event_data.has("allow_flying"))
    {
        allow_flying = (bool) event_data["allow_flying"].asBoolean();
    }

    if (event_data.has("leader_id"))
    {
        target_id = event_data["leader_id"];
    }
    else if (event_data.has("avatar_name"))
    {   // Find the avatar with matching name
        std::string target_name = event_data["avatar_name"].asString();

        if (target_name.length() > 0)
        {
            S32 num_objects = gObjectList.getNumObjects();
            S32 cur_index = 0;
            while (cur_index < num_objects)
            {
                LLViewerObject * cur_object = gObjectList.getObject(cur_index++);
                if (cur_object &&
                    cur_object->asAvatar() &&
                    cur_object->asAvatar()->getFullname() == target_name)
                {   // Found avatar with matching name, extract id and break out of loop
                    target_id = cur_object->getID();
                    break;
                }
            }
        }
    }
    else
    {
        return response.error("'leader_id' or 'avatar_name' should be specified");
    }

    F32 stop_distance = 0.f;
    if (event_data.has("stop_distance"))
    {
        stop_distance = (F32)event_data["stop_distance"].asReal();
    }

    if (!gObjectList.findObject(target_id))
    {
        std::string target_info = event_data.has("leader_id") ? event_data["leader_id"] : event_data["avatar_name"];
        return response.error(stringize("Target ", std::quoted(target_info), " was not found"));
    }

    mAgent.setFlying(allow_flying);
    mFollowTarget = target_id;  // Save follow target so we can report distance later

    mAgent.startFollowPilot(target_id, allow_flying, stop_distance);
}

void LLAgentListener::setAutoPilotTarget(LLSD const & event_data) const
{
    if (event_data.has("target_global"))
    {
        LLVector3d target_global(ll_vector3d_from_sd(event_data["target_global"]));
        mAgent.setAutoPilotTargetGlobal(target_global);
    }
}

void LLAgentListener::stopAutoPilot(LLSD const & event_data) const
{
    bool user_cancel = false;
    if (event_data.has("user_cancel"))
    {
        user_cancel = event_data["user_cancel"].asBoolean();
    }
    mAgent.stopAutoPilot(user_cancel);
}

void LLAgentListener::lookAt(LLSD const & event_data) const
{
    LLViewerObject *object = NULL;
    if (event_data.has("obj_uuid"))
    {
        object = gObjectList.findObject(event_data["obj_uuid"]);
    }
    else if (event_data.has("position"))
    {
        LLVector3 target_position = ll_vector3_from_sd(event_data["position"]);
        object = findObjectClosestTo(target_position);
    }

    S32 look_at_type = (S32) LOOKAT_TARGET_NONE;
    if (event_data.has("type"))
    {
        look_at_type = event_data["type"].asInteger();
    }
    if (look_at_type >= (S32) LOOKAT_TARGET_NONE &&
        look_at_type < (S32) LOOKAT_NUM_TARGETS)
    {
        gAgentCamera.setLookAt((ELookAtType) look_at_type, object);
    }
}

void LLAgentListener::getGroups(const LLSD& event) const
{
    LLSD reply(LLSD::emptyArray());
    for (std::vector<LLGroupData>::const_iterator
             gi(mAgent.mGroups.begin()), gend(mAgent.mGroups.end());
         gi != gend; ++gi)
    {
        reply.append(LLSDMap
                     ("id", gi->mID)
                     ("name", gi->mName)
                     ("insignia", gi->mInsigniaID)
                     ("notices", bool(gi->mAcceptNotices))
                     ("display", bool(gi->mListInProfile))
                     ("contrib", gi->mContribution));
    }
    sendReply(LLSDMap("groups", reply), event);
}

/*----------------------------- camera control -----------------------------*/
// specialize LLSDParam to support (const LLVector3&) arguments -- this
// wouldn't even be necessary except that the relevant LLVector3 constructor
// is explicitly explicit
template <>
class LLSDParam<const LLVector3&>: public LLSDParamBase
{
public:
    LLSDParam(const LLSD& value): value(LLVector3(value)) {}

    operator const LLVector3&() const { return value; }

private:
    LLVector3 value;
};

// accept any of a number of similar LLFollowCamMgr methods with different
// argument types, and return a wrapper lambda that accepts LLSD and converts
// to the target argument type
template <typename T>
auto wrap(void (LLFollowCamMgr::*method)(const LLUUID& source, T arg))
{
    return [method](LLFollowCamMgr& followcam, const LLUUID& source, const LLSD& arg)
    { (followcam.*method)(source, LLSDParam<T>(arg)); };
}

// table of supported LLFollowCamMgr methods,
// with the corresponding setFollowCamParams() argument keys
static std::pair<std::string, std::function<void(LLFollowCamMgr&, const LLUUID&, const LLSD&)>>
cam_params[] =
{
    { "camera_pos",       wrap(&LLFollowCamMgr::setPosition) },
    { "focus_pos",        wrap(&LLFollowCamMgr::setFocus) },
    { "focus_offset",     wrap(&LLFollowCamMgr::setFocusOffset) },
    { "camera_locked",    wrap(&LLFollowCamMgr::setPositionLocked) },
    { "focus_locked",     wrap(&LLFollowCamMgr::setFocusLocked) },
    { "distance",         wrap(&LLFollowCamMgr::setDistance) },
    { "focus_threshold",  wrap(&LLFollowCamMgr::setFocusThreshold) },
    { "camera_threshold", wrap(&LLFollowCamMgr::setPositionThreshold) },
    { "focus_lag",        wrap(&LLFollowCamMgr::setFocusLag) },
    { "camera_lag",       wrap(&LLFollowCamMgr::setPositionLag) },
    { "camera_pitch",     wrap(&LLFollowCamMgr::setPitch) },
    { "behindness_lag",   wrap(&LLFollowCamMgr::setBehindnessLag) },
    { "behindness_angle", wrap(&LLFollowCamMgr::setBehindnessAngle) },
};

void LLAgentListener::setFollowCamParams(const LLSD& event) const
{
    auto& followcam{ LLFollowCamMgr::instance() };
    for (const auto& pair : cam_params)
    {
        if (event.has(pair.first))
        {
            pair.second(followcam, gAgentID, event[pair.first]);
        }
    }
    followcam.setCameraActive(gAgentID, true);
}

void LLAgentListener::setFollowCamActive(LLSD const & event) const
{
    LLFollowCamMgr::getInstance()->setCameraActive(gAgentID, event["active"]);
}

void LLAgentListener::removeFollowCamParams(LLSD const & event) const
{
    LLFollowCamMgr::getInstance()->removeFollowCamParams(gAgentID);
}

LLViewerInventoryItem* get_anim_item(LLEventAPI::Response &response, const LLSD &event_data)
{
    LLViewerInventoryItem* item = gInventory.getItem(event_data["item_id"].asUUID());
    if (!item || (item->getInventoryType() != LLInventoryType::IT_ANIMATION))
    {
        response.error(stringize("Animation item ", std::quoted(event_data["item_id"].asString()), " was not found"));
        return NULL;
    }
    return item;
}

void LLAgentListener::playAnimation(LLSD const &event_data)
{
    Response response(LLSD(), event_data);
    if (LLViewerInventoryItem* item = get_anim_item(response, event_data))
    {
        if (event_data["inworld"].asBoolean())
        {
            mAgent.sendAnimationRequest(item->getAssetUUID(), ANIM_REQUEST_START);
        }
        else
        {
            gAgentAvatarp->startMotion(item->getAssetUUID());
        }
    }
}

void LLAgentListener::stopAnimation(LLSD const &event_data)
{
    Response response(LLSD(), event_data);
    if (LLViewerInventoryItem* item = get_anim_item(response, event_data))
    {
        gAgentAvatarp->stopMotion(item->getAssetUUID());
        mAgent.sendAnimationRequest(item->getAssetUUID(), ANIM_REQUEST_STOP);
    }
}

void LLAgentListener::getAnimationInfo(LLSD const &event_data)
{
    Response response(LLSD(), event_data);
    if (LLViewerInventoryItem* item = get_anim_item(response, event_data))
    {
        // if motion exists, will return existing one
        LLMotion* motion = gAgentAvatarp->createMotion(item->getAssetUUID());
        response["anim_info"] = llsd::map("duration", motion->getDuration(),
                                               "is_loop", motion->getLoop(),
                                               "num_joints", motion->getNumJointMotions(),
                                               "asset_id", item->getAssetUUID(),
                                               "priority", motion->getPriority());
    }
}

void LLAgentListener::getID(LLSD const& event_data)
{
    Response response(llsd::map("id", gAgentID), event_data);
}

F32 get_search_radius(LLSD const& event_data)
{
    static LLCachedControl<F32> render_far_clip(gSavedSettings, "RenderFarClip", 64);
    F32 dist = render_far_clip;
    if (event_data.has("dist"))
    {
        dist = llclamp((F32)event_data["dist"].asReal(), 1, 512);
    }
   return dist * dist;
}

void LLAgentListener::getNearbyAvatarsList(LLSD const& event_data)
{
    Response response(LLSD(), event_data);
    F32 radius = get_search_radius(event_data);
    LLVector3d agent_pos = gAgent.getPositionGlobal();
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (avatar && !avatar->isDead() && !avatar->isControlAvatar() && !avatar->isSelf())
        {
            if ((dist_vec_squared(avatar->getPositionGlobal(), agent_pos) <= radius))
            {
                LLAvatarName av_name;
                LLAvatarNameCache::get(avatar->getID(), &av_name);
                LLVector3 region_pos = avatar->getCharacterPosition();   // agent space
                response["result"].append(llsd::map("id", avatar->getID(), "global_pos", ll_sd_from_vector3d(avatar->getPosGlobalFromAgent(region_pos)),
                                                    "region_pos", ll_sd_from_vector3(avatar->getRegion()->getPosRegionFromAgent(region_pos)),   // <WolfViewer> agent frame may be rebased
                                                    "name", av_name.getUserName(), "region_id", avatar->getRegion()->getRegionID()));
            }
        }
    }
}

void LLAgentListener::getNearbyObjectsList(LLSD const& event_data)
{
    Response response(LLSD(), event_data);
    F32 radius = get_search_radius(event_data);
    S32 num_objects = gObjectList.getNumObjects();
    LLVector3d agent_pos = gAgent.getPositionGlobal();
    for (S32 i = 0; i < num_objects; ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (object && object->getVolume() && !object->isAttachment())
        {
            if ((dist_vec_squared(object->getPositionGlobal(), agent_pos) <= radius))
            {
                response["result"].append(llsd::map("id", object->getID(), "global_pos", ll_sd_from_vector3d(object->getPositionGlobal()), "region_pos",
                          ll_sd_from_vector3(object->getPositionRegion()), "region_id", object->getRegion()->getRegionID()));
            }
        }
    }
}

void LLAgentListener::getAgentScreenPos(LLSD const& event_data)
{
    Response response(LLSD(), event_data);
    LLVector3 render_pos;
    if (event_data.has("avatar_id") && (event_data["avatar_id"].asUUID() != gAgentID))
    {
        LLUUID avatar_id(event_data["avatar_id"]);
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = (LLVOAvatar*)character;
            if (!avatar->isDead() && (avatar->getID() == avatar_id))
            {
                render_pos = avatar->getRenderPosition();
                break;
            }
        }
    }
    else if (gAgentAvatarp.notNull() && gAgentAvatarp->isValid())
    {
        render_pos = gAgentAvatarp->getRenderPosition();
    }
    LLCoordGL screen_pos;
    response["onscreen"] = LLViewerCamera::getInstance()->projectPosAgentToScreen(render_pos, screen_pos, false);
    response["x"] = screen_pos.mX;
    response["y"] = screen_pos.mY;
}
