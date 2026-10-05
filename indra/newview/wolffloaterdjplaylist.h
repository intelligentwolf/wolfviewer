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
class LLScrollListCtrl;
class LLTextBox;

class WolfFloaterDJPlaylist : public LLFloater
{
public:
    WolfFloaterDJPlaylist(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

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
};

#endif // WOLF_FLOATERDJPLAYLIST_H
