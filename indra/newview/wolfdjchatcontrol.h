/**
 * @file wolfdjchatcontrol.h
 * @brief WolfViewer Wolf DJ chat control: drive the DJ playlist from chat commands and from the
 *        DJ's own scripts, so an automated show can start, pause and stop the music.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_DJCHATCONTROL_H
#define WOLF_DJCHATCONTROL_H

#include "lluuid.h"
#include <string>

// [WOLF DJ 2026-10-19] Elisa Porter: "adding Chat Control directly to your Wolf DJ playlist, allowing music
// to be started, paused and stopped through inworld chat commands or LSL scripts ... without touching Wolf
// DJ's existing audio, mixer or streaming functionality"; Paul: "lets get this built into the viewer".
//
// Off unless "Enable Chat Control" is ticked in the playlist window (WolfDJChatControl); the channel is
// WolfDJChatChannel (default 89). Two ways in, both only from the DJ:
//   1. the DJ TYPES "/89 play" in the nearby chat bar or the nearby chat window (FSNearbyChat::sendChat,
//      FSFloaterNearbyChat::sendChat open a TypedChat scope around their send; send_chat_from_viewer then
//      calls onOwnChat). Chat the viewer sends for anyone else's reasons never counts: a clicked
//      secondlife:///app/chat/89/stop link (LLChatCommandHandler, fsnearbychathub.cpp - anyone can post one),
//      a gesture's chat step (LLGestureMgr; gestures can be given by others), RLV, scripts. The line still
//      goes to the region as normal, so the DJ's own scripts listening on 89 hear it too.
//   2. an object the DJ OWNS says "/89 play" to them with llOwnerSay or llRegionSayTo(owner, 0, ...)
//      (process_chat_from_simulator, llviewermessage.cpp). The region never passes chat on any other
//      channel to a viewer - llRegionSayTo to an avatar on a non-zero channel only reaches its attachments
//      (OpenSim WorldCommModule.cs DeliverMessageTo) - so channel 0 owner/direct chat is how a script talks
//      to the viewer. The command line is not shown in chat. Only OwnerID == the DJ counts - the region
//      stamps it from the object's real owner (OpenSim ChatModule.cs:253, :357), so another resident's
//      objects (on their own land or anywhere), group-owned objects and other people's chat are never
//      obeyed: nobody can stop or change someone else's show.
//
// Commands (any case): play, play <n>, pause, resume, stop, next, prev, jingle <1-8>,
// playlist <name>, status. They do exactly what the playlist window's buttons do (WolfDJPlayer,
// WolfDJJingles); the audio, mixer and stream are not touched.
class WolfDJChatControl
{
public:
    // While one exists, outgoing chat is chat the DJ typed (see 1. above). Main thread only.
    struct TypedChat
    {
        TypedChat() { sTyped = true; }
        ~TypedChat() { sTyped = false; }
    };

    // From send_chat_from_viewer: the DJ's own chat on a channel. Never stops the chat being sent.
    static void onOwnChat(const std::string& text, S32 channel);

    // From process_chat_from_simulator: owner-say / region-say-to chat. True = it was a command for the
    // DJ from one of their own objects; the caller then does not show it.
    static bool onObjectChat(const std::string& text, U8 chat_type, const LLUUID& owner_id);

private:
    static bool sTyped;

    // Runs one command (the text after "/<channel> "). quiet = only report problems.
    static void run(const std::string& command, bool quiet);
};

#endif // WOLF_DJCHATCONTROL_H
