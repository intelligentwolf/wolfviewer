/**
 * @file wolffloaterdjplaylist.cpp
 * @brief WolfViewer: Wolf DJ playlist floater. See wolffloaterdjplaylist.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "wolffloaterdjplaylist.h"

#include "wolfdjplayer.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace
{
    fs::path to_path(const std::string& utf8)
    {
#if LL_WINDOWS
        return fs::path(ll_convert_string_to_wide(utf8));
#else
        return fs::path(utf8);
#endif
    }

    std::string from_path(const fs::path& p)
    {
#if LL_WINDOWS
        return ll_convert_wide_to_string(p.wstring());
#else
        return p.string();
#endif
    }

    std::string home_dir()
    {
#if LL_WINDOWS
        const wchar_t* h = _wgetenv(L"USERPROFILE");
        return h ? ll_convert_wide_to_string(std::wstring(h)) : std::string("C:\\");
#else
        const char* h = getenv("HOME");
        return h ? std::string(h) : std::string("/");
#endif
    }

    std::string display_name(const std::string& path)
    {
        return from_path(to_path(path).filename());
    }

    bool less_nocase(const std::string& a, const std::string& b)
    {
        return LLStringUtil::compareInsensitive(a, b) < 0;
    }
}

WolfFloaterDJPlaylist::WolfFloaterDJPlaylist(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterDJPlaylist::postBuild()
{
    mBrowser = getChild<LLScrollListCtrl>("browser");
    mPlaylist = getChild<LLScrollListCtrl>("playlist");
    mPath = getChild<LLTextBox>("path_text");
    mNowPlaying = getChild<LLTextBox>("now_playing");
    mTime = getChild<LLTextBox>("time_text");
    mError = getChild<LLTextBox>("error_text");
    mPlay = getChild<LLButton>("play");

    mBrowser->setDoubleClickCallback([this]() { onBrowserDoubleClick(); });
    mPlaylist->setDoubleClickCallback([this]()
    {
        const S32 idx = mPlaylist->getFirstSelectedIndex();
        if (idx >= 0) WolfDJPlayer::instance().play(idx);
    });
    getChild<LLButton>("music_dir")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const fs::path music = to_path(home_dir()) / "Music";
        std::error_code ec;
        showFolder(fs::is_directory(music, ec) ? from_path(music) : home_dir());
    });
    getChild<LLButton>("home_dir")->setCommitCallback([this](LLUICtrl*, const LLSD&) { showFolder(home_dir()); });
    getChild<LLButton>("up_dir")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const fs::path p = to_path(mFolder);
        if (p.has_parent_path() && p.parent_path() != p) showFolder(from_path(p.parent_path()));
    });
    getChild<LLButton>("add_files")->setCommitCallback([this](LLUICtrl*, const LLSD&) { addSelected(); });
    getChild<LLButton>("add_folder")->setCommitCallback([this](LLUICtrl*, const LLSD&) { addFolder(); });

    WolfDJPlayer& player = WolfDJPlayer::instance();
    getChild<LLButton>("prev")->setCommitCallback([&player](LLUICtrl*, const LLSD&) { player.previous(); });
    getChild<LLButton>("next")->setCommitCallback([&player](LLUICtrl*, const LLSD&) { player.next(); });
    getChild<LLButton>("stop")->setCommitCallback([&player](LLUICtrl*, const LLSD&) { player.stop(); });
    mPlay->setCommitCallback([this, &player](LLUICtrl*, const LLSD&)
    {
        const S32 sel = mPlaylist->getFirstSelectedIndex();
        if (!player.isPlaying() && sel >= 0) player.play(sel);
        else player.togglePause();
    });
    getChild<LLButton>("move_up")->setCommitCallback([this](LLUICtrl*, const LLSD&) { moveSelected(-1); });
    getChild<LLButton>("move_down")->setCommitCallback([this](LLUICtrl*, const LLSD&) { moveSelected(1); });
    getChild<LLButton>("remove")->setCommitCallback([this](LLUICtrl*, const LLSD&) { removeSelected(); });
    getChild<LLButton>("clear")->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        WolfDJPlayer::instance().stop();
        savePlaylist({});
    });
    getChild<LLCheckBoxCtrl>("send_titles")->setCommitCallback([](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL("WolfDJPlaylistTitles", c->getValue().asBoolean());
    });
    return true;
}

void WolfFloaterDJPlaylist::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    getChild<LLCheckBoxCtrl>("send_titles")->set(gSavedSettings.getBOOL("WolfDJPlaylistTitles"));
    WolfDJPlayer& player = WolfDJPlayer::instance();
    if (player.files().empty())
    {
        // The playlist saved last time.
        std::vector<std::string> files;
        const LLSD saved = gSavedSettings.getLLSD("WolfDJPlaylist");
        if (saved.isArray())
        {
            for (const LLSD& f : llsd::inArray(saved))
            {
                if (f.isString()) files.push_back(f.asString());
            }
        }
        player.setFiles(files);
    }
    std::string folder = gSavedSettings.getString("WolfDJPlaylistFolder");
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(to_path(folder), ec))
    {
        const fs::path music = to_path(home_dir()) / "Music";
        folder = fs::is_directory(music, ec) ? from_path(music) : home_dir();
    }
    showFolder(folder);
    mShownCurrent = -2;
    rebuildPlaylist();
}

void WolfFloaterDJPlaylist::showFolder(const std::string& folder)
{
    mFolder = folder;
    gSavedSettings.setString("WolfDJPlaylistFolder", folder);
    mPath->setText(folder);
    mPath->setToolTip(folder);
    mEntries.clear();
    std::vector<Entry> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(to_path(folder), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = from_path(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        if (it->is_directory(ec2))
        {
            dirs.push_back({ from_path(it->path()), name, true });
        }
        else if (WolfDJPlayer::isAudioFile(name))
        {
            files.push_back({ from_path(it->path()), name, false });
        }
    }
    auto by_name = [](const Entry& a, const Entry& b) { return less_nocase(a.mName, b.mName); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    mEntries = dirs;
    mEntries.insert(mEntries.end(), files.begin(), files.end());

    mBrowser->deleteAllItems();
    for (size_t i = 0; i < mEntries.size(); ++i)
    {
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = mEntries[i].mDir ? mEntries[i].mName + "/" : mEntries[i].mName;
        if (mEntries[i].mDir) row["columns"][0]["font"]["style"] = "BOLD";
        mBrowser->addElement(row);
    }
    if (ec)
    {
        mError->setText(std::string("Cannot open that folder."));
    }
}

void WolfFloaterDJPlaylist::onBrowserDoubleClick()
{
    LLScrollListItem* item = mBrowser->getFirstSelected();
    if (!item) return;
    const S32 i = item->getValue().asInteger();
    if (i < 0 || i >= (S32)mEntries.size()) return;
    if (mEntries[i].mDir)
    {
        showFolder(mEntries[i].mPath);
    }
    else
    {
        addFiles({ mEntries[i].mPath });
    }
}

void WolfFloaterDJPlaylist::addSelected()
{
    std::vector<std::string> paths;
    for (LLScrollListItem* item : mBrowser->getAllSelected())
    {
        const S32 i = item->getValue().asInteger();
        if (i >= 0 && i < (S32)mEntries.size() && !mEntries[i].mDir) paths.push_back(mEntries[i].mPath);
    }
    addFiles(paths);
}

void WolfFloaterDJPlaylist::addFolder()
{
    std::vector<std::string> paths;
    for (const Entry& e : mEntries)
    {
        if (!e.mDir) paths.push_back(e.mPath);
    }
    addFiles(paths);
}

void WolfFloaterDJPlaylist::addFiles(const std::vector<std::string>& paths)
{
    if (paths.empty()) return;
    std::vector<std::string> files = WolfDJPlayer::instance().files();
    files.insert(files.end(), paths.begin(), paths.end());
    savePlaylist(files);
}

void WolfFloaterDJPlaylist::savePlaylist(const std::vector<std::string>& files)
{
    WolfDJPlayer::instance().setFiles(files);
    LLSD arr = LLSD::emptyArray();
    for (const std::string& f : files) arr.append(f);
    gSavedSettings.setLLSD("WolfDJPlaylist", arr);
    mShownCurrent = -2;
    rebuildPlaylist();
}

void WolfFloaterDJPlaylist::moveSelected(int dir)
{
    const S32 idx = mPlaylist->getFirstSelectedIndex();
    std::vector<std::string> files = WolfDJPlayer::instance().files();
    const S32 to = idx + dir;
    if (idx < 0 || to < 0 || to >= (S32)files.size()) return;
    std::swap(files[idx], files[to]);
    savePlaylist(files);
    mPlaylist->selectNthItem(to);
}

void WolfFloaterDJPlaylist::removeSelected()
{
    std::vector<S32> gone;
    for (LLScrollListItem* item : mPlaylist->getAllSelected())
    {
        gone.push_back(item->getValue().asInteger());
    }
    if (gone.empty()) return;
    WolfDJPlayer& player = WolfDJPlayer::instance();
    if (std::find(gone.begin(), gone.end(), player.current()) != gone.end())
    {
        player.stop();
    }
    std::vector<std::string> files = player.files(), kept;
    for (S32 i = 0; i < (S32)files.size(); ++i)
    {
        if (std::find(gone.begin(), gone.end(), i) == gone.end()) kept.push_back(files[i]);
    }
    savePlaylist(kept);
}

void WolfFloaterDJPlaylist::rebuildPlaylist()
{
    WolfDJPlayer& player = WolfDJPlayer::instance();
    const std::vector<std::string> files = player.files();
    const int cur = player.current();
    const S32 keep = mPlaylist->getFirstSelectedIndex();
    mPlaylist->deleteAllItems();
    for (size_t i = 0; i < files.size(); ++i)
    {
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "mark";
        row["columns"][0]["value"] = (int)i == cur && player.isPlaying() ? ">" : "";
        row["columns"][1]["column"] = "name";
        row["columns"][1]["value"] = display_name(files[i]);
        if ((int)i == cur && player.isPlaying()) row["columns"][1]["font"]["style"] = "BOLD";
        mPlaylist->addElement(row);
    }
    if (keep >= 0 && keep < (S32)files.size()) mPlaylist->selectNthItem(keep);
    mShownCurrent = player.isPlaying() ? cur : -1;
}

void WolfFloaterDJPlaylist::draw()
{
    WolfDJPlayer& player = WolfDJPlayer::instance();
    const int shown = player.isPlaying() ? player.current() : -1;
    if (shown != mShownCurrent || player.trackSerial() != mShownSerial)
    {
        mShownSerial = player.trackSerial();
        rebuildPlaylist();
    }
    std::string artist, title;
    player.nowPlaying(artist, title);
    if (player.isPlaying())
    {
        mNowPlaying->setText(artist.empty() ? title : artist + " - " + title);
        const S32 pos = (S32)player.position();
        const S32 len = (S32)player.duration();
        std::string t = llformat("%d:%02d", pos / 60, pos % 60);
        if (len > 0) t += llformat(" / %d:%02d", len / 60, len % 60);
        if (player.isPaused()) t += "  (paused)";
        if (!player.output()) t += "  - not on a mixer channel";
        mTime->setText(t);
    }
    else
    {
        mNowPlaying->setText(std::string("Nothing playing"));
        mTime->setText(std::string());
    }
    mPlay->setLabel(player.isPlaying() && !player.isPaused() ? std::string("Pause") : std::string("Play"));
    mError->setText(player.lastError());
    LLFloater::draw();
}
