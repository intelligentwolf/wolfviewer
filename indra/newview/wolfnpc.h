/**
 * @file wolfnpc.h
 * @brief WolfViewer: WolfNPC - the link to the region's WolfNPCModule (GenericMessage "wolfnpc" out, "WolfNPC" in).
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

#ifndef WOLF_NPC_H
#define WOLF_NPC_H

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

/**
 * [WOLFNPC 2026-10-24] Paul: "people can manage an NPC on their region through the viewer".
 * The contract: firestorm/region-terrain-work/wolfnpc-release/PROTOCOL.md (section 2 is this file).
 *
 * Source: PROTOCOL.md section 2 - the WolfGame transport (WolfGameModule.cs:42-52, Send/Chunk at :3945-4015): the JSON is
 * cut into UTF-8 pieces of at most 240 bytes that never split a character, and sent as packets
 * ["<id>/<part>/<total>", piece, piece, piece] (at most 3 pieces a packet, part counted from 0). Both ways.
 *   viewer -> region: GenericMessage "wolfnpc"; the region allows 16 KiB a message and 4 messages a second (bucket 8)
 *   region -> viewer: GenericMessage "WolfNPC", Invoice = the region (as WolfGame: only the agent's own region counts)
 * Every request carries "op" and "req" (this counter, echoed back); every reply "op", "req", "ok" and, when not ok,
 * "error" (shown as is) and maybe "code". Pushes (no req): "changed" {id} and "credits" {credits}.
 * Sent only while the region underfoot has itself said it is Wolf (WolfGrid::isOnWolfTerritoriesConfirmed).
 */
class WolfNPC : public LLSingleton<WolfNPC>
{
    LLSINGLETON(WolfNPC);
    ~WolfNPC() override;

public:
    /** The reply (a map with "ok"; on a failure that never reached the region, ok false and "error" set here). */
    typedef std::function<void(const LLSD& reply)> reply_fn;
    typedef std::function<void(const std::string& npc_id)> changed_fn;

    /** Send {op, ...fields}; `done` gets the reply, or a failure (not Wolf, too big, no answer in time, region left). */
    void request(const std::string& op, const LLSD& fields, reply_fn done);
    /** The window listens for "changed" pushes (one listener: the WolfNPC window). */
    void setChangedListener(changed_fn fn) { mChanged = fn; }
    /** [ROUND 3] PROTOCOL.md §19 push gm_ended {id, why}: the game master's control of an NPC ended. */
    typedef std::function<void(const std::string& npc_id, const std::string& why)> gm_ended_fn;
    void setGmEndedListener(gm_ended_fn fn) { mGmEnded = fn; }

    /** Once a frame (llappviewer.cpp idle): the send queue, half-built messages and requests that got no answer. */
    void idle();

    /** From the GenericMessage dispatcher: one packet of a "WolfNPC" message. */
    void receivePacket(const LLUUID& invoice, const std::vector<std::string>& strings);

private:
    struct Pending
    {
        S32 mTotal = 0;
        F64 mFirst = 0.0;
        std::map<S32, std::string> mParts;
    };
    struct Waiting
    {
        reply_fn mDone;
        F64 mDeadline = 0.0;
        std::string mOp;
    };

    void onRegionChanged();
    void failAll(const std::string& why);
    void dispatch(const LLSD& m);
    bool takeToken();

    std::map<S64, Pending> mPartial;              // region -> viewer messages still arriving, by their id
    std::map<S32, Waiting> mWaiting;              // requests sent, by req
    struct Outgoing
    {
        S32 mReq = 0;
        std::vector<std::vector<std::string>> mPackets;   // each packet's parameters: header, then up to 3 pieces
    };
    std::deque<Outgoing> mQueue;                  // whole messages waiting for a token (the region's rate)
    S32 mNextReq = 1;
    S32 mNextMessageId = 1;
    F64 mTokens = 8.0;
    F64 mTokenAt = 0.0;
    changed_fn mChanged;
    gm_ended_fn mGmEnded;
    boost::signals2::connection mRegionChangedConnection;
};

#endif // WOLF_NPC_H
