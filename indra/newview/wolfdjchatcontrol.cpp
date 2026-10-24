/**
 * @file wolfdjchatcontrol.cpp
 * @brief WolfViewer Wolf DJ chat control (see wolfdjchatcontrol.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "wolfdjchatcontrol.h"
#include "wolfdjplayer.h"
#include "wolffloaterdjplaylist.h"

#include "fscommon.h"
#include "llagent.h"
#include "llchat.h"
#include "llfloaterreg.h"
#include "llsdutil.h"
#include "llviewercontrol.h"

#include <cstdlib>

namespace
{
    // "/89 play" -> "play" when 89 is the chat-control channel; false for anything else.
    bool strip_channel(const std::string& text, std::string& command)
    {
        const S32 channel = gSavedSettings.getS32("WolfDJChatChannel");
        const std::string prefix = llformat("/%d", channel);
        if (text.compare(0, prefix.size(), prefix) != 0)
            return false;
        if (text.size() > prefix.size() && text[prefix.size()] != ' ')
            return false;                       // "/890 ..." is another channel
        command = text.substr(prefix.size());
        LLStringUtil::trim(command);
        return true;
    }

    void say(const std::string& line)
    {
        FSCommon::report_to_nearby_chat("Wolf DJ: " + line);
    }

    std::string track_label(int index)
    {
        WolfDJPlayer& player = WolfDJPlayer::instance();
        std::string artist, title;
        player.nowPlaying(artist, title);
        std::string name = title.empty() ? std::string() : (artist.empty() ? title : artist + " - " + title);
        const int count = (int)player.files().size();
        return name.empty() ? llformat("song %d of %d", index + 1, count)
                            : llformat("song %d of %d, %s", index + 1, count, name.c_str());
    }

}

bool WolfDJChatControl::sTyped = false;

void WolfDJChatControl::onOwnChat(const std::string& text, S32 channel)
{
    // only what the DJ typed - never a clicked app/chat link, a gesture, RLV or a script (wolfdjchatcontrol.h)
    if (!sTyped || !gSavedSettings.getBOOL("WolfDJChatControl") || channel != gSavedSettings.getS32("WolfDJChatChannel"))
        return;
    std::string command = text;
    LLStringUtil::trim(command);
    run(command, false);
}

bool WolfDJChatControl::onObjectChat(const std::string& text, U8 chat_type, const LLUUID& owner_id)
{
    if (!gSavedSettings.getBOOL("WolfDJChatControl"))
        return false;
    // llOwnerSay (CHAT_TYPE_OWNER) or llRegionSayTo(owner, 0, ...) (CHAT_TYPE_DIRECT), llchat.h:55-56
    if (chat_type != CHAT_TYPE_OWNER && chat_type != CHAT_TYPE_DIRECT)
        return false;
    // the region fills OwnerID with the object's real owner for both (OpenSim ChatModule.cs:253, :357). Only
    // the DJ's own objects: not group-owned ones, not anything on someone else's land, not a null OwnerID.
    if (owner_id.isNull() || owner_id != gAgentID)
        return false;
    std::string command;
    if (!strip_channel(text, command))
        return false;
    run(command, true);
    return true;
}

void WolfDJChatControl::run(const std::string& command_in, bool quiet)
{
    WolfDJPlayer& player = WolfDJPlayer::instance();
    player.loadSaved();                         // the saved playlist, as the desk's play button (once per session)

    std::string command = command_in, arg;
    const size_t space = command.find(' ');
    if (space != std::string::npos)
    {
        arg = command.substr(space + 1);
        LLStringUtil::trim(arg);
        command.resize(space);
    }
    LLStringUtil::toLower(command);
    const int count = (int)player.files().size();

    // Playing needs the playlist on a mixer channel; the player routes it itself (WolfDJPlayer::play and
    // togglePause call routeToMixer, which does nothing when it is already routed). Asking first gives the
    // reason now - the player's lastError() keeps an old message until a song starts.
    auto routed = [&player]() -> bool
    {
        std::string why;
        if (player.routeToMixer(why)) return true;
        say(why);
        return false;
    };

    if (command == "play")
    {
        if (count == 0)
        {
            say("the playlist is empty.");
            return;
        }
        if (!arg.empty())
        {
            const int n = atoi(arg.c_str());
            if (n < 1 || n > count)
            {
                say(llformat("there is no song %s (the playlist has %d).", arg.c_str(), count));
                return;
            }
            if (!routed()) return;
            player.play(n - 1);
        }
        else if (player.isPlaying() && !player.isPaused())
        {
            if (!quiet) say("already playing.");
            return;
        }
        else
        {
            if (!routed()) return;
            player.togglePause();               // the play button: resumes, or starts where it left off
        }
        if (!quiet) say("playing.");
    }
    else if (command == "pause")
    {
        if (player.isPlaying() && !player.isPaused())
            player.togglePause();
        if (!quiet) say("paused.");
    }
    else if (command == "resume")
    {
        if (!player.isPlaying() || !player.isPaused())
        {
            if (!quiet) say(player.isPlaying() ? "already playing." : "nothing is paused (use play).");
            return;
        }
        if (!routed()) return;
        player.togglePause();
        if (!quiet) say("playing.");
    }
    else if (command == "stop")
    {
        player.stop();
        if (!quiet) say("stopped.");
    }
    else if (command == "next")
    {
        player.next();
        if (!quiet) say("next song.");
    }
    else if (command == "prev" || command == "previous")
    {
        player.previous();
        if (!quiet) say("previous song.");
    }
    else if (command == "jingle")
    {
        const int n = atoi(arg.c_str());
        if (n < 1 || n > WolfDJJingles::PAD_COUNT)
        {
            say(llformat("jingle pads are 1 to %d.", WolfDJJingles::PAD_COUNT));
            return;
        }
        if (WolfDJJingles::instance().pad(n - 1).empty())
        {
            say(llformat("jingle pad %d is empty.", n));
            return;
        }
        WolfDJJingles::instance().trigger(n - 1);
        if (!quiet) say(llformat("jingle %d.", n));
    }
    else if (command == "playlist")
    {
        std::string chosen;
        if (arg.empty() || !WolfFloaterDJPlaylist::choosePlaylist(arg, chosen))
        {
            say(arg.empty() ? std::string("which playlist? (playlist <name>)")
                            : "there is no playlist called \"" + arg + "\".");
            return;
        }
        if (!quiet) say("playlist \"" + chosen + "\" (the song playing now finishes first).");
    }
    else if (command == "status")
    {
        if (!player.isPlaying()) say(count ? llformat("stopped, %d songs in the playlist.", count) : std::string("stopped, the playlist is empty."));
        else say(std::string(player.isPaused() ? "paused at " : "playing ") + track_label(player.current()) + ".");
    }
    else
    {
        say("unknown command \"" + command_in + "\". Commands: play, play <n>, pause, resume, stop, next, prev, jingle <1-8>, playlist <name>, status.");
    }
}
