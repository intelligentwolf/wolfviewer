/**
 * @file wolffloaterdjplaylist.h
 * @brief WolfViewer: Wolf DJ playlist floater (files and folders on the left, the playlist on
 *        the right). The player itself is wolfdjplayer.cpp.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 * $/LicenseInfo$
 */

#ifndef WOLF_FLOATERDJPLAYLIST_H
#define WOLF_FLOATERDJPLAYLIST_H

#include "llfloater.h"

#include <string>
#include <vector>

class LLButton;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLTextBox;

class WolfFloaterDJPlaylist : public LLFloater
{
public:
    WolfFloaterDJPlaylist(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // [WOLF DJ 2026-10-19] public for chat control ("playlist <name>", wolfdjchatcontrol.cpp)
    void selectList(const std::string& name);

private:
    struct Entry
    {
        std::string mPath;      // UTF-8
        std::string mName;
        bool mDir = false;
    };

    void showFolder(const std::string& folder);
    void onBrowserDoubleClick();
    void addSelected();
    void setJinglePad(int pad);         // [WOLF DJ 2026-10-05]
    void addFolder();
    void addFiles(const std::vector<std::string>& paths);
    void rebuildPlaylist();
    void savePlaylist(const std::vector<std::string>& files);
    void moveSelected(int dir);
    void removeSelected();

    // [WOLF DJ 2026-10-19] Paul: "in the dj mixer allow the user to add multiple playlists". Named
    // playlists (setting WolfDJPlaylists, name -> files); the one picked (WolfDJPlaylistCurrent) is the
    // one the player plays, mirrored in WolfDJPlaylist as before. Picking another while a song plays
    // lets that song finish, then the new playlist plays from its first song (WolfDJPlayer::setFiles).
    void loadLists();
    void refreshListCombo();
    void newList();
    void renameList();
    void deleteList();
    void showError(const std::string& text);    // stays until the next action that works
    void refreshChatControlNote();              // [WOLF DJ 2026-10-19] chat control's one-line help

    LLComboBox* mLists = nullptr;
    LLLineEditor* mListName = nullptr;
    LLSD mPlaylists;                // name -> array of files
    std::string mCurrentList;
    std::string mUiError;

    LLScrollListCtrl* mBrowser = nullptr;
    LLScrollListCtrl* mPlaylist = nullptr;
    LLTextBox* mPath = nullptr;
    LLTextBox* mNowPlaying = nullptr;
    LLTextBox* mTime = nullptr;
    LLTextBox* mError = nullptr;
    LLTextBox* mJingleNote = nullptr;   // [WOLF DJ 2026-10-05] what was just put on a pad
    LLButton* mPlay = nullptr;
    std::string mFolder;
    std::vector<Entry> mEntries;
    int mShownCurrent = -2;
    int mShownSerial = -1;
    int mShownLengths = -1;     // WolfDJPlayer::lengthsVersion the list was made with
};

#endif // WOLF_FLOATERDJPLAYLIST_H
