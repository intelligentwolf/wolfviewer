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

#include "llrect.h"

class LLButton;
class LLCheckBoxCtrl;
class LLLineEditor;
class LLTextBox;
class WolfMeshUploadSheet;

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
/*
 * [BLUEPRINT LOOK 2026-10-20] Paul: "upgrade the mesh upload floater make it look really nice as well ... as nice as the
 * new avatar and clothing generator but not the same". "The Drawing Office": a blueprint sheet painted in code behind the
 * controls by WolfMeshUploadSheet (the technique of wolffloaterradio.cpp WolfRadioBackdrop and llfloaterwolfavatars.cpp
 * WolfAvatarsBackdrop; the primitives in wolfpaint.h): cobalt paper with a drafting grid, a wireframe box with dimension
 * lines, the title block, four stamps (Choose - Check - Upload - Done), three cards - the model file; the spec sheet, the
 * model's numbers in cells; what it is called in your inventory - the status on its own plate and every button's face.
 * Every control is still the XUI widget it was (laid out by layoutWidgets() on each resize), so the keyboard, focus and
 * tooltips work as before; the ids, callbacks, limits and the upload itself are unchanged.
 * Who and why: a builder with a model from Blender or Meshy wants it in their inventory as a rezzable object in a minute,
 * and to know before sending it what they will get (size, triangles, simplified or not, linkset, textures and their cost).
 */
class WolfFloaterMeshUpload : public LLFloater
{
    friend class WolfMeshUploadSheet;   // the view behind the controls that paints the sheet
public:
    WolfFloaterMeshUpload(const LLSD& key);
    ~WolfFloaterMeshUpload();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void reshape(S32 width, S32 height, bool called_from_parent = true) override;

private:
    // ---- the drawn sheet ----
    /** Where everything sits, floater-local; worked out by layoutWidgets() on every resize. */
    struct Layout
    {
        LLRect client;          // below the title bar
        LLRect drawing;         // the wireframe box with its dimension lines
        LLRect title_block;     // "UPLOAD MODEL", the blurb, the site line
        LLRect stamps;          // Choose - Check - Upload - Done
        LLRect card[3];         // the model file, the spec sheet, in your inventory
        LLRect cells;           // the spec sheet's cells (two rows of three)
        LLRect status_plate;    // the status line's plate
    };
    enum EStage { STAGE_CHOOSE = 0, STAGE_CHECK, STAGE_UPLOAD, STAGE_DONE };
    void layoutWidgets();
    void drawSheet();
    void drawPaper() const;
    void drawHero(F32 t) const;
    void drawStamps() const;
    void drawCards() const;
    void drawCells() const;
    void drawStatus(F32 t) const;
    void drawButtons() const;
    /** The status shows an error (setStatus is_error), drawn on the red plate. */
    bool mStatusIsError = false;
    EStage mStage = STAGE_CHOOSE;
    Layout mLayout;
    WolfMeshUploadSheet* mSheet = nullptr;
    LLTextBox* mBlurb = nullptr;
    LLTextBox* mFormats = nullptr;
    LLTextBox* mNameLabel = nullptr;
    LLTextBox* mDescLabel = nullptr;
    LLTextBox* mTexNote = nullptr;

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
