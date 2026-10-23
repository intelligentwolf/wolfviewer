/**
 * @file llviewermedia_streamingaudio.h
 * @author Tofu Linden, Sam Kolb
 * @brief LLStreamingAudio_MediaPlugins implementation - an implementation of the streaming audio interface which is implemented as a client of the media plugin API.
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
#include "linden_common.h"
#include "llpluginclassmedia.h"
#include "llpluginclassmediaowner.h"
#include "llviewermedia.h"

#include "llviewermedia_streamingaudio.h"

#include "llmimetypes.h"
#include "lldir.h"

#if LL_LINUX
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include "llclipboard.h"
#include "llnotificationsutil.h"

// <WolfViewer 2026-10-23> Tell Linux users when parcel music cannot play because GStreamer, or
// one of its plugins, is missing, and give them the exact command for their distribution.
//
// Parcel music goes through media_plugin_gstreamer (mime_types_linux.xml audio/*), which
// dlopen()s GStreamer at run time (media_plugin_gstreamer10.cpp startup()). Two ways it fails:
//  1. GStreamer itself cannot be loaded: init_media_plugin() returns -1 before the plugin is
//     running, the child exits and LLPluginProcessParent reports pluginLaunchFailed(). The
//     plugin cannot say why (its socket is closed with it), so the viewer loads the same two
//     libraries the plugin needs to find out (wolf_missing_gst_library()).
//  2. GStreamer runs but a stream needs a plugin that is not installed: the plugin sends
//     "wolf_gst_missing_plugin" (MediaPluginGStreamer10::reportMissingPlugin()). It filters out
//     bad parcel URLs (web pages, playlists), so a dead stream never shows this.
namespace
{
    bool sWolfGstNoticeDone = false;    // shown, or decided against, once per session

    // os-release(5): /etc/os-release, falling back to /usr/lib/os-release
    std::map<std::string, std::string> wolf_read_os_release()
    {
        std::map<std::string, std::string> fields;
        std::ifstream in("/etc/os-release");
        if (!in.is_open())
        {
            in.open("/usr/lib/os-release");
        }
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
            {
                continue;
            }
            std::string value = line.substr(eq + 1);
            if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            {
                value = value.substr(1, value.size() - 2);
            }
            fields[line.substr(0, eq)] = value;
        }
        return fields;
    }

    // The install line for this distribution, from ID and ID_LIKE; empty = not one we know.
    // Package names checked 2026-10-23: Debian 12 apt-cache, Fedora packages.fedoraproject.org,
    // Arch pacman -Si, openSUSE Tumbleweed / Leap 15.6 / Leap 16.0 OSS repositories.
    std::string wolf_gst_install_command(std::string &distro_out)
    {
        std::map<std::string, std::string> os = wolf_read_os_release();
        std::set<std::string> ids;
        std::string id = os["ID"];
        LLStringUtil::toLower(id);
        if (!id.empty())
        {
            ids.insert(id);
        }
        std::string like = os["ID_LIKE"];
        LLStringUtil::toLower(like);
        std::istringstream like_words(like);
        std::string word;
        while (like_words >> word)
        {
            ids.insert(word);
        }
        distro_out = id.empty() ? "unknown" : id;
        auto is = [&ids](const char *name) { return ids.count(name) > 0; };

        if (is("debian") || is("ubuntu"))
        {
            return "sudo apt install gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav";
        }
        // RHEL and its rebuilds name Fedora in ID_LIKE but do not carry these packages
        if (is("fedora") && !is("rhel"))
        {
            return "sudo dnf install gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free gstreamer1-plugin-libav";
        }
        if (is("arch"))
        {
            return "sudo pacman -S gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav";
        }
        if (id.compare(0, 8, "opensuse") == 0 || is("opensuse"))
        {
            return "sudo zypper install gstreamer gstreamer-plugins-base gstreamer-plugins-good gstreamer-plugins-bad gstreamer-plugins-libav";
        }
        return std::string();
    }

    // The first GStreamer library the plugin needs that cannot be loaded, with the loader's
    // reason; empty = both load. Same names as media_plugin_gstreamer10.cpp startup(), whose
    // required symbols live in these two (libglib/libgobject are their own dependencies).
    // GLib/GObject are already in the viewer (GIO), and all of them are NODELETE, so this
    // leaves nothing behind that matters.
    std::string wolf_missing_gst_library()
    {
        static const char *const libs[] = { "libgstreamer-1.0.so.0", "libgstapp-1.0.so.0" };
        for (const char *lib : libs)
        {
            void *handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
            if (!handle)
            {
                const char *why = dlerror();
                return std::string(lib) + " (" + (why ? why : "not found") + ")";
            }
            dlclose(handle);
        }
        return std::string();
    }

    void wolf_show_gst_notice()
    {
        std::string distro;
        const std::string command = wolf_gst_install_command(distro);
        LL_INFOS("StreamingAudio") << "GStreamer install help for distribution '" << distro << "': "
                                   << (command.empty() ? std::string("generic advice") : command) << LL_ENDL;
        if (command.empty())
        {
            LLNotificationsUtil::add("WolfGStreamerMissingGeneric");
            return;
        }
        LLSD args;
        args["COMMAND"] = command;
        LLNotificationsUtil::add("WolfGStreamerMissing", args, LLSD(), [command](const LLSD& n, const LLSD& r)
        {
            // option 0 = "Copy command"; the default (and what an ignored notice answers) is OK
            if (LLNotificationsUtil::getSelectedOption(n, r) == 0)
            {
                const LLWString text = utf8str_to_wstring(command);
                LLClipboard::instance().copyToClipboard(text, 0, static_cast<S32>(text.size()));
            }
        });
    }

    void wolf_check_music_needs_gstreamer(const LLPluginClassMedia *plugin)
    {
        if (sWolfGstNoticeDone || !plugin)
        {
            return;
        }
        if (!plugin->getWolfMissingGstPlugin().empty())
        {
            sWolfGstNoticeDone = true;
            LL_WARNS("StreamingAudio") << "Parcel music can't play: GStreamer is installed but missing a plugin ("
                                       << plugin->getWolfMissingGstPlugin() << ")" << LL_ENDL;
            wolf_show_gst_notice();
        }
        else if (plugin->getWolfPluginLaunchFailed())
        {
            sWolfGstNoticeDone = true;
            if (getenv("LL_DISABLE_GSTREAMER"))
            {
                LL_INFOS("StreamingAudio") << "Parcel music is off: LL_DISABLE_GSTREAMER is set" << LL_ENDL;
                return;
            }
            const std::string missing = wolf_missing_gst_library();
            if (missing.empty())
            {
                LL_WARNS("StreamingAudio") << "The parcel music plugin failed to start, but the GStreamer libraries load; "
                                           << "not a missing-GStreamer problem" << LL_ENDL;
                return;
            }
            LL_WARNS("StreamingAudio") << "Parcel music can't play: GStreamer libraries are missing: " << missing << LL_ENDL;
            wolf_show_gst_notice();
        }
    }
}
// </WolfViewer>
#endif

LLStreamingAudio_MediaPlugins::LLStreamingAudio_MediaPlugins() :
    mMediaPlugin(NULL),
    mGain(1.0)
{
    // nothing interesting to do?
    // we will lazily create a media plugin at play-time, if none exists.
}

LLStreamingAudio_MediaPlugins::~LLStreamingAudio_MediaPlugins()
{
    delete mMediaPlugin;
    mMediaPlugin = NULL;
}

void LLStreamingAudio_MediaPlugins::start(const std::string& url)
{
    if (!mMediaPlugin) // lazy-init the underlying media plugin
    {
        mMediaPlugin = initializeMedia("audio/mpeg"); // assumes that whatever media implementation supports mp3 also supports vorbis.
        LL_INFOS() << "streaming audio mMediaPlugin is now " << mMediaPlugin << LL_ENDL;
    }

    if(!mMediaPlugin)
        return;

    if (!url.empty())
    {
        LL_INFOS() << "Starting internet stream: " << url << LL_ENDL;

        mURL = url; // keep original url here for comparison purposes
        std::string snt_url = url;
        LLStringUtil::trim(snt_url);
        size_t pos = snt_url.find(' ');
        if (pos != std::string::npos)
        {
            // fmod permited having names after the url and people were using it.
            // People label their streams this way, ignore the 'label'.
            snt_url = snt_url.substr(0, pos);
        }
        mMediaPlugin->loadURI(snt_url);
        mMediaPlugin->start();
        LL_INFOS() << "Playing stream..." << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "setting stream to NULL"<< LL_ENDL;
        mURL.clear();
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }
}

void LLStreamingAudio_MediaPlugins::stop()
{
    LL_INFOS() << "Stopping internet stream." << LL_ENDL;
    if(mMediaPlugin)
    {
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }

    mURL.clear();

    // <FS:Ansariel> Stream meta data display
    updateMetadata();
}

void LLStreamingAudio_MediaPlugins::pause(int pause)
{
    if(!mMediaPlugin)
        return;

    if(pause)
    {
        LL_INFOS() << "Pausing internet stream." << LL_ENDL;
        mMediaPlugin->pause();
    }
    else
    {
        LL_INFOS() << "Unpausing internet stream." << LL_ENDL;
        mMediaPlugin->start();
    }
}

void LLStreamingAudio_MediaPlugins::update()
{
    if (mMediaPlugin)
        mMediaPlugin->idle();

#if LL_LINUX
    wolf_check_music_needs_gstreamer(mMediaPlugin);     // <WolfViewer 2026-10-23>
#endif

    // <FS:Ansariel> Stream meta data display
    updateMetadata();
}

int LLStreamingAudio_MediaPlugins::isPlaying()
{
    if (!mMediaPlugin)
        return 0; // stopped

    LLPluginClassMediaOwner::EMediaStatus status =
        mMediaPlugin->getStatus();

    switch (status)
    {
    case LLPluginClassMediaOwner::MEDIA_LOADING: // but not MEDIA_LOADED
    case LLPluginClassMediaOwner::MEDIA_PLAYING:
        return 1; // Active and playing
    case LLPluginClassMediaOwner::MEDIA_PAUSED:
        return 2; // paused
    default:
        return 0; // stopped
    }
}

void LLStreamingAudio_MediaPlugins::setGain(F32 vol)
{
    mGain = vol;

    if(!mMediaPlugin)
        return;

    vol = llclamp(vol, 0.f, 1.f);
    mMediaPlugin->setVolume(vol);
}

F32 LLStreamingAudio_MediaPlugins::getGain()
{
    return mGain;
}

std::string LLStreamingAudio_MediaPlugins::getURL()
{
    return mURL;
}

LLPluginClassMedia* LLStreamingAudio_MediaPlugins::initializeMedia(const std::string& media_type)
{
    LLPluginClassMediaOwner* owner = NULL;
    S32 default_size = 1; // audio-only - be minimal, doesn't matter
    F64 default_zoom = 1.0;
    LLPluginClassMedia* media_source = LLViewerMediaImpl::newSourceFromMediaType(media_type, owner, default_size, default_size, default_zoom);

    if (media_source)
    {
        media_source->setLoop(false); // audio streams are not expected to loop
    }

    return media_source;
}

// <FS:ND> stream metadata from plugin
void LLStreamingAudio_MediaPlugins::updateMetadata() noexcept
{
    if (!mMediaPlugin)
    {
        return;
    }

    if (mTitle != mMediaPlugin->getTitle() || mArtist != mMediaPlugin->getArtist())
    {
        mArtist = mMediaPlugin->getArtist();
        mTitle = mMediaPlugin->getTitle();
        mMetadata.clear();
        mMetadata["ARTIST"] = mArtist;
        mMetadata["TITLE"] = mTitle;

        mMetadataUpdateSignal(mMetadata);
    }
}
// </FS:ND>
