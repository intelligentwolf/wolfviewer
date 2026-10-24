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
#include "wolfdjaudio.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llspinctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lluicolortable.h"
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
    mJingleNote = getChild<LLTextBox>("jingle_note");
    mLists = getChild<LLComboBox>("playlist_select");
    mListName = getChild<LLLineEditor>("playlist_name");
    mLists->setCommitCallback([this](LLUICtrl*, const LLSD&) { selectList(mLists->getValue().asString()); });
    getChild<LLButton>("playlist_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) { newList(); });
    getChild<LLButton>("playlist_rename")->setCommitCallback([this](LLUICtrl*, const LLSD&) { renameList(); });
    getChild<LLButton>("playlist_delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { deleteList(); });

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
    // [WOLF DJ 2026-10-05] Paul: "a jingle set of buttons in different colours you can apply files
    // to in the playlist folder": pick a song (left, or in the playlist), press a pad to put it there.
    for (int i = 0; i < WolfDJJingles::PAD_COUNT; ++i)
    {
        const float* rgb = WolfDJJingles::PAD_RGB[i];
        LLButton* pad = getChild<LLButton>(llformat("set_jingle_%d", i));
        pad->setImageColor(LLUIColor(LLColor4(rgb[0], rgb[1], rgb[2], 1.f)));
        pad->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { setJinglePad(i); });
    }
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
    // [WOLF DJ 2026-10-19] chat control (wolfdjchatcontrol.h)
    LLCheckBoxCtrl* chat = getChild<LLCheckBoxCtrl>("chat_control");
    chat->setValue(gSavedSettings.getBOOL("WolfDJChatControl"));
    chat->setCommitCallback([this](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setBOOL("WolfDJChatControl", c->getValue().asBoolean());
        refreshChatControlNote();
    });
    LLSpinCtrl* channel = getChild<LLSpinCtrl>("chat_channel");
    channel->setValue(gSavedSettings.getS32("WolfDJChatChannel"));
    channel->setCommitCallback([this](LLUICtrl* c, const LLSD&)
    {
        gSavedSettings.setS32("WolfDJChatChannel", llclamp(c->getValue().asInteger(), 1, 999999));
        refreshChatControlNote();
    });
    refreshChatControlNote();
    return true;
}

void WolfFloaterDJPlaylist::refreshChatControlNote()
{
    const S32 channel = gSavedSettings.getS32("WolfDJChatChannel");
    getChild<LLUICtrl>("chat_channel")->setEnabled(gSavedSettings.getBOOL("WolfDJChatControl"));
    getChild<LLTextBox>("chat_control_note")->setText(gSavedSettings.getBOOL("WolfDJChatControl")
        ? llformat("Listening on /%d to you and your own objects (llOwnerSay \"/%d play\"): play, play <n>, pause, resume, stop, next, prev, jingle <1-8>, playlist <name>, status.", channel, channel)
        : std::string("Chat Control is off. The playlist and the broadcast work as usual."));
}

void WolfFloaterDJPlaylist::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    getChild<LLCheckBoxCtrl>("send_titles")->set(gSavedSettings.getBOOL("WolfDJPlaylistTitles"));
    WolfDJPlayer::instance().loadSaved();      // the playlist saved last time
    loadLists();
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

// [WOLF DJ 2026-10-05] The selected song (the folder on the left first, else the playlist) onto a jingle pad.
void WolfFloaterDJPlaylist::setJinglePad(int pad)
{
    std::string path;
    if (LLScrollListItem* item = mBrowser->getFirstSelected())
    {
        const S32 i = item->getValue().asInteger();
        if (i >= 0 && i < (S32)mEntries.size() && !mEntries[i].mDir) path = mEntries[i].mPath;
    }
    if (path.empty())
    {
        const S32 idx = mPlaylist->getFirstSelectedIndex();
        const std::vector<std::string> files = WolfDJPlayer::instance().files();
        if (idx >= 0 && idx < (S32)files.size()) path = files[idx];
    }
    if (path.empty())
    {
        showError(llformat("Pick a song first (on the left, or in the playlist), then press pad %d.", pad + 1));
        return;
    }
    WolfDJJingles::instance().setPad(pad, path);
    showError(std::string());
    mJingleNote->setText(llformat("Pad %d: %s", pad + 1, WolfDJJingles::padLabel(path).c_str()));
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

// [WOLF DJ 2026-10-24] Paul: "there is add a folder which doesn't do anything". It took only the songs lying
// directly in the folder shown, so a Music folder of artist/album folders added nothing, and said nothing.
// Now it takes the folder picked on the left (else the one shown) with every folder inside it, and says
// what it did. Hidden folders are skipped and the walk is capped, so pressing it on Home cannot hang the viewer.
void WolfFloaterDJPlaylist::addFolder()
{
    constexpr size_t MAX_SONGS = 2000;
    constexpr size_t MAX_LOOKED_AT = 50000;
    std::string folder = mFolder;
    if (LLScrollListItem* item = mBrowser->getFirstSelected())
    {
        const S32 i = item->getValue().asInteger();
        if (i >= 0 && i < (S32)mEntries.size() && mEntries[i].mDir) folder = mEntries[i].mPath;
    }
    const std::string folder_name = display_name(folder).empty() ? folder : display_name(folder);
    std::vector<std::string> paths;
    bool too_many_songs = false, too_many_files = false;
    size_t looked_at = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(to_path(folder), fs::directory_options::skip_permission_denied, ec), end;
    if (ec)
    {
        showError(llformat("Cannot open the folder \"%s\".", folder_name.c_str()));
        return;
    }
    for (; !ec && it != end; it.increment(ec))
    {
        if (++looked_at > MAX_LOOKED_AT)
        {
            too_many_files = true;
            break;
        }
        const std::string name = from_path(it->path().filename());
        std::error_code ec2;
        const bool dir = it->is_directory(ec2);
        if (name.empty() || name[0] == '.')
        {
            if (dir) it.disable_recursion_pending();
            continue;
        }
        if (dir || !WolfDJPlayer::isAudioFile(name)) continue;
        if (paths.size() >= MAX_SONGS)
        {
            too_many_songs = true;
            break;
        }
        paths.push_back(from_path(it->path()));
    }
    if (paths.empty())
    {
        showError(too_many_files
            ? llformat("No songs found in the first %d files of \"%s\" - pick a smaller folder.", (int)MAX_LOOKED_AT, folder_name.c_str())
            : llformat("No songs in \"%s\" or the folders inside it.", folder_name.c_str()));
        return;
    }
    std::sort(paths.begin(), paths.end(), less_nocase);    // folder by folder, each in name order
    addFiles(paths);
    std::string note = llformat("Added %d songs from \"%s\".", (int)paths.size(), folder_name.c_str());
    if (too_many_songs) note += llformat(" Stopped at %d - pick a smaller folder for the rest.", (int)MAX_SONGS);
    else if (too_many_files) note += llformat(" Stopped looking after %d files - pick a smaller folder for the rest.", (int)MAX_LOOKED_AT);
    showNote(note);
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
    gSavedSettings.setLLSD("WolfDJPlaylist", arr);     // the playing list, read by WolfDJPlayer::loadSaved
    if (!mCurrentList.empty())
    {
        mPlaylists[mCurrentList] = arr;
        gSavedSettings.setLLSD("WolfDJPlaylists", mPlaylists);
    }
    showError(std::string());
    mShownCurrent = -2;
    rebuildPlaylist();
}

// [WOLF DJ 2026-10-19] Named playlists. The first time, the one playlist there was (WolfDJPlaylist)
// becomes "Playlist 1", so nobody loses their list.
void WolfFloaterDJPlaylist::loadLists()
{
    mPlaylists = gSavedSettings.getLLSD("WolfDJPlaylists");
    if (!mPlaylists.isMap() || mPlaylists.size() == 0)
    {
        mPlaylists = LLSD::emptyMap();
        const LLSD old = gSavedSettings.getLLSD("WolfDJPlaylist");
        mPlaylists["Playlist 1"] = old.isArray() ? old : LLSD::emptyArray();
        gSavedSettings.setLLSD("WolfDJPlaylists", mPlaylists);
    }
    mCurrentList = gSavedSettings.getString("WolfDJPlaylistCurrent");
    if (!mPlaylists.has(mCurrentList))
    {
        mCurrentList = mPlaylists.beginMap()->first;
        gSavedSettings.setString("WolfDJPlaylistCurrent", mCurrentList);
    }
    refreshListCombo();
}

void WolfFloaterDJPlaylist::refreshListCombo()
{
    mLists->removeall();
    for (LLSD::map_const_iterator it = mPlaylists.beginMap(); it != mPlaylists.endMap(); ++it)
    {
        const S32 n = it->second.isArray() ? (S32)it->second.size() : 0;
        mLists->add(llformat("%s  (%d)", it->first.c_str(), n), LLSD(it->first));
    }
    mLists->selectByValue(LLSD(mCurrentList));
    getChild<LLButton>("playlist_delete")->setEnabled(mPlaylists.size() > 1);
}

void WolfFloaterDJPlaylist::selectList(const std::string& name)
{
    if (name.empty() || !mPlaylists.has(name)) return;
    mCurrentList = name;
    gSavedSettings.setString("WolfDJPlaylistCurrent", name);
    std::vector<std::string> files;
    for (const LLSD& f : llsd::inArray(mPlaylists[name]))
    {
        if (f.isString()) files.push_back(f.asString());
    }
    savePlaylist(files);        // the player takes it; a song playing now finishes first
    refreshListCombo();
}

void WolfFloaterDJPlaylist::newList()
{
    std::string name = mListName->getText();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        for (int i = (int)mPlaylists.size() + 1; ; ++i)
        {
            name = llformat("Playlist %d", i);
            if (!mPlaylists.has(name)) break;
        }
    }
    if (mPlaylists.has(name))
    {
        showError(llformat("There is already a playlist called \"%s\".", name.c_str()));
        return;
    }
    mPlaylists[name] = LLSD::emptyArray();
    gSavedSettings.setLLSD("WolfDJPlaylists", mPlaylists);
    mListName->clear();
    selectList(name);
}

void WolfFloaterDJPlaylist::renameList()
{
    std::string name = mListName->getText();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        showError(std::string("Type the new name in the box first, then press Rename."));
        return;
    }
    if (name == mCurrentList) return;
    if (mPlaylists.has(name))
    {
        showError(llformat("There is already a playlist called \"%s\".", name.c_str()));
        return;
    }
    mPlaylists[name] = mPlaylists[mCurrentList];
    mPlaylists.erase(mCurrentList);
    mCurrentList = name;
    gSavedSettings.setLLSD("WolfDJPlaylists", mPlaylists);
    gSavedSettings.setString("WolfDJPlaylistCurrent", name);
    mListName->clear();
    showError(std::string());
    refreshListCombo();
}

void WolfFloaterDJPlaylist::deleteList()
{
    if (mPlaylists.size() <= 1)
    {
        showError(std::string("This is your only playlist - use Clear to empty it."));
        return;
    }
    const std::string doomed = mCurrentList;
    LLSD args;
    args["MESSAGE"] = llformat("Delete the playlist \"%s\"? Only the list goes - your music files are not touched.", doomed.c_str());
    // The window may be closed before the question is answered: reach it through its handle (as wolffloaterdj.cpp does).
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("GenericAlertYesCancel", args, LLSD(), [h, doomed](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return false;
        WolfFloaterDJPlaylist* f = static_cast<WolfFloaterDJPlaylist*>(h.get());
        if (!f || !f->mPlaylists.has(doomed) || f->mPlaylists.size() <= 1) return false;
        f->mPlaylists.erase(doomed);
        gSavedSettings.setLLSD("WolfDJPlaylists", f->mPlaylists);
        if (f->mCurrentList == doomed) f->selectList(f->mPlaylists.beginMap()->first);
        else f->refreshListCombo();
        return false;
    });
}

void WolfFloaterDJPlaylist::showError(const std::string& text)
{
    mUiError = text;
    mUiIsNote = false;
}

void WolfFloaterDJPlaylist::showNote(const std::string& text)
{
    mUiError = text;
    mUiIsNote = true;
}

// [WOLF DJ 2026-10-24] Moved here from wolfdjchatcontrol.cpp (choose_playlist) so the mixer's picker shares it.
// static
bool WolfFloaterDJPlaylist::choosePlaylist(const std::string& wanted, std::string& chosen)
{
    const LLSD lists = gSavedSettings.getLLSD("WolfDJPlaylists");
    if (!lists.isMap())
        return false;
    for (LLSD::map_const_iterator it = lists.beginMap(); it != lists.endMap(); ++it)
    {
        std::string a = it->first, b = wanted;
        LLStringUtil::toLower(a);
        LLStringUtil::toLower(b);
        if (a != b)
            continue;
        chosen = it->first;
        if (WolfFloaterDJPlaylist* floater = LLFloaterReg::findTypedInstance<WolfFloaterDJPlaylist>("wolf_dj_playlist"))
        {
            floater->selectList(chosen);
            return true;
        }
        std::vector<std::string> files;
        LLSD arr = LLSD::emptyArray();
        for (const LLSD& f : llsd::inArray(it->second))
        {
            if (f.isString())
            {
                files.push_back(f.asString());
                arr.append(f);
            }
        }
        gSavedSettings.setString("WolfDJPlaylistCurrent", chosen);
        gSavedSettings.setLLSD("WolfDJPlaylist", arr);
        WolfDJPlayer::instance().setFiles(files);   // a song playing now finishes first
        return true;
    }
    return false;
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
        // [WOLF DJ 2026-10-10] Each song's length, as the background scan learns it.
        const double len = player.knownDuration(files[i]);
        const S32 secs = (S32)len;
        row["columns"][2]["column"] = "length";
        row["columns"][2]["value"] = len < 0.0 ? std::string("...") : (len <= 0.0 ? std::string("?") : llformat("%d:%02d", secs / 60, secs % 60));
        mPlaylist->addElement(row);
    }
    if (keep >= 0 && keep < (S32)files.size()) mPlaylist->selectNthItem(keep);
    mShownCurrent = player.isPlaying() ? cur : -1;
    mShownLengths = player.lengthsVersion();
}

void WolfFloaterDJPlaylist::draw()
{
    WolfDJPlayer& player = WolfDJPlayer::instance();
    const int shown = player.isPlaying() ? player.current() : -1;
    if (shown != mShownCurrent || player.trackSerial() != mShownSerial || player.lengthsVersion() != mShownLengths)
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
        if (len > 0)
        {
            const S32 left = std::max(0, len - pos);
            t += llformat(" / %d:%02d    %d:%02d left", len / 60, len % 60, left / 60, left % 60);
        }
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
    // [WOLF DJ 2026-10-19] This window's own messages first (they used to be overwritten here on the next frame).
    const bool note = !mUiError.empty() && mUiIsNote;
    if (note != mShownNote)
    {
        mError->setColor(LLUIColorTable::instance().getColor(note ? "LabelTextColor" : "Red"));
        mShownNote = note;
    }
    mError->setText(mUiError.empty() ? player.lastError() : mUiError);
    LLFloater::draw();
}
