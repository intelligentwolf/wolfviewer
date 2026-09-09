/**
 * @file wolffloatermeshupload.h
 * @brief WolfViewer: the Wolf Territories model uploader's floater.
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

#ifndef WOLF_FLOATER_MESH_UPLOAD_H
#define WOLF_FLOATER_MESH_UPLOAD_H

#include "llfloater.h"
#include "wolfmeshupload.h"

class LLButton;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLTextBox;

/**
 * Source: wolfstorm/js/ui/floaters/floater_mesh_upload.js FloaterMeshUpload — the same fields in
 * the same order, doing the same thing.
 *
 * WOLF TERRITORIES ONLY. The uploader writes assets and inventory through Wolf Territories'
 * own ROBUST services (rust_proxy/src/main.rs:1638 GRID_ROBUST_BASE is that grid, hard-coded), so
 * it is meaningless anywhere else. The gate is in three places, because one is not enough:
 *   - LLFloaterModelPreview::showModelPreview only routes here on Wolf Territories; every other
 *     grid gets Firestorm's uploader, unchanged.
 *   - "wolf_mesh_upload" is on both SLapp openfloater blacklists (llviewerfloaterreg.cpp:294,
 *     :347) alongside "upload_model", so an in-world link cannot pop it either.
 *   - onUpload() re-checks WolfMeshUpload::isAvailable() before sending anything, because a
 *     session can change grid under a floater that was left open.
 *
 * The "Old Upload" button is the way to Firestorm's uploader from here, for COLLADA, for rigged
 * meshes, for hand-authored LODs and for physics shapes — none of which this uploader builds.
 */
class WolfFloaterMeshUpload : public LLFloater
{
public:
    WolfFloaterMeshUpload(const LLSD& key);
    ~WolfFloaterMeshUpload();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    void onBrowse();
    void onFilePicked(const std::vector<std::string>& filenames);
    void onUpload();
    void onOldUpload();
    void onIncludeTexturesChanged();

    /** Parse `path` off the main thread and show what it found. */
    void beginParse(const std::string& path);
    void onParsed(const std::string& path, WolfMeshUpload::model_ptr_t model, const std::string& error);

    void setStatus(const std::string& text, bool is_error);
    void refreshInfo();
    void setBusy(bool busy);

    LLButton*       mBrowseBtn = nullptr;
    LLButton*       mUploadBtn = nullptr;
    LLButton*       mOldUploadBtn = nullptr;
    LLButton*       mCancelBtn = nullptr;
    LLCheckBoxCtrl* mIncludeTextures = nullptr;
    LLLineEditor*   mNameEdit = nullptr;
    LLLineEditor*   mDescEdit = nullptr;
    LLTextBox*      mFileNameText = nullptr;
    LLTextBox*      mInfoText = nullptr;
    LLTextBox*      mStatusText = nullptr;

    /** Destination folder from the open key, null for the default Objects folder. */
    LLUUID                       mDestFolderId;
    std::string                  mModelPath;
    WolfMeshUpload::model_ptr_t  mModel;
    /** A parse or an upload is in flight; the buttons are disabled and a second one is refused. */
    bool                         mBusy = false;
};

#endif // WOLF_FLOATER_MESH_UPLOAD_H
