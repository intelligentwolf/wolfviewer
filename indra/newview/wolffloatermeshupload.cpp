/**
 * @file wolffloatermeshupload.cpp
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

#include "llviewerprecompiledheaders.h"

#include "wolffloatermeshupload.h"

#include "llbutton.h"
#include "llagentbenefits.h"
#include "llcheckboxctrl.h"
#include "lldir.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "lltextbox.h"
#include "llviewermenufile.h"
#include "workqueue.h"

#include "wolfgrid.h"

WolfFloaterMeshUpload::WolfFloaterMeshUpload(const LLSD& key)
    : LLFloater(key)
{
}

WolfFloaterMeshUpload::~WolfFloaterMeshUpload()
{
}

bool WolfFloaterMeshUpload::postBuild()
{
    mBrowseBtn       = getChild<LLButton>("browse_btn");
    mUploadBtn       = getChild<LLButton>("upload_btn");
    mOldUploadBtn    = getChild<LLButton>("old_upload_btn");
    mCancelBtn       = getChild<LLButton>("cancel_btn");
    mIncludeTextures = getChild<LLCheckBoxCtrl>("include_textures");
    mNameEdit        = getChild<LLLineEditor>("name_edit");
    mDescEdit        = getChild<LLLineEditor>("desc_edit");
    mFileNameText    = getChild<LLTextBox>("file_name");
    mInfoText        = getChild<LLTextBox>("info");
    mStatusText      = getChild<LLTextBox>("status");

    mBrowseBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBrowse(); });
    mUploadBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpload(); });
    mOldUploadBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOldUpload(); });
    mCancelBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    mIncludeTextures->setCommitCallback([this](LLUICtrl*, const LLSD&) { onIncludeTexturesChanged(); });

    // Say what textures will actually cost, the way WolfStorm's uploader does
    // (floater_mesh_upload.js:99, "L$N each" from the economy data). A checkbox that spends
    // money should name the price before it is ticked, not after.
    const S32 tex_cost = LLAgentBenefitsMgr::current().getTextureUploadCost();
    mIncludeTextures->setLabel(tex_cost > 0
        ? llformat("Include textures (each distinct texture is uploaded as its own inventory "
                   "item, L$%d each)", tex_cost)
        : std::string("Include textures (each distinct texture is uploaded as its own inventory item)"));

    mUploadBtn->setEnabled(false);
    return true;
}

void WolfFloaterMeshUpload::onOpen(const LLSD& key)
{
    // The inventory gallery opens the uploader against a specific folder
    // (llinventoryfunctions.cpp:4228 -> showModelPreview(dest_id)); the menu passes nothing.
    mDestFolderId = key.has("dest_folder") ? key["dest_folder"].asUUID() : LLUUID::null;
    // Reopening starts clean, the way the web viewer's open() does
    // (floater_mesh_upload.js:80-95).
    mModel.reset();
    mModelPath.clear();
    mBusy = false;
    mFileNameText->setText(std::string("No file chosen"));
    mNameEdit->setText(LLStringUtil::null);
    mDescEdit->setText(LLStringUtil::null);
    mInfoText->setText(LLStringUtil::null);
    setStatus(LLStringUtil::null, false);
    setBusy(false);
}

void WolfFloaterMeshUpload::setBusy(bool busy)
{
    mBusy = busy;
    mBrowseBtn->setEnabled(!busy);
    mUploadBtn->setEnabled(!busy && mModel && !mModel->prims().empty());
    // "Old Upload" stays live: it is the way out if this uploader cannot read the file.
    mIncludeTextures->setEnabled(!busy);
}

void WolfFloaterMeshUpload::setStatus(const std::string& text, bool is_error)
{
    mStatusText->setText(text);
    // The one visible difference between "working" and "went wrong". A colour is not enough on
    // its own, so the message itself always says which.
    mStatusText->setColor(is_error ? LLUIColorTable::instance().getColor("EmphasisColor")
                                   : LLUIColorTable::instance().getColor("LabelTextColor"));
}

// ─────────────────────────── choosing a file ───────────────────────────

void WolfFloaterMeshUpload::onBrowse()
{
    if (mBusy)
    {
        return;
    }
    LLFilePickerReplyThread::startPicker(
        [handle = getDerivedHandle<WolfFloaterMeshUpload>()]
        (const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (WolfFloaterMeshUpload* self = handle.get())
            {
                self->onFilePicked(filenames);
            }
        },
        LLFilePicker::FFLOAD_WOLF_MODEL, false);
}

void WolfFloaterMeshUpload::onFilePicked(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty())
    {
        return;
    }
    const std::string& path = filenames[0];
    const std::string ext = gDirUtilp->getExtension(path);

    if (ext == "dae")
    {
        // COLLADA belongs to Firestorm's uploader, which carries the rigging, LOD and physics
        // machinery this one has no equivalent for. Say so and offer the button rather than
        // failing with "unsupported format".
        setStatus("COLLADA (.dae) files are uploaded by Firestorm's own uploader. "
                  "Press \"Old Upload\" to open it.", true);
        return;
    }
    if (!WolfMeshUpload::isSupportedExtension(ext))
    {
        setStatus("This uploader reads OBJ, glTF and GLB. Press \"Old Upload\" for other formats.", true);
        return;
    }

    mModelPath = path;
    mFileNameText->setText(gDirUtilp->getBaseFileName(path));
    if (mNameEdit->getText().empty())
    {
        mNameEdit->setText(gDirUtilp->getBaseFileName(path, true));
    }
    beginParse(path);
}

void WolfFloaterMeshUpload::beginParse(const std::string& path)
{
    mModel.reset();
    mInfoText->setText(LLStringUtil::null);
    setStatus("Reading model...", false);
    setBusy(true);

    // Parsing a large GLB is seconds of tight loop, so it runs on the general queue rather than
    // freezing the frame. WolfMeshUpload::Model::load touches only the filesystem and tinygltf,
    // which is what makes that safe. Source: the same postTo pattern as wolfnaturalwater.cpp:154-172.
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");

    auto handle = getDerivedHandle<WolfFloaterMeshUpload>();

    // The parse result: the model when it worked, the reason when it did not.
    struct Parsed
    {
        WolfMeshUpload::model_ptr_t mModel;
        std::string                 mError;
    };

    auto run = [path]() -> Parsed
    {
        Parsed out;
        // A model file is untrusted input. The parser bounds-checks what it reads, but a
        // corrupt or hostile file can still ask for an allocation that fails, and an exception
        // escaping this lambda is rethrown on the main loop by the work queue — i.e. it takes
        // the whole viewer down for someone who merely opened the wrong .glb. Turn any throw
        // into the error the floater already knows how to display.
        try
        {
            auto model = std::make_shared<WolfMeshUpload::Model>();
            if (!model->load(path, out.mError) || !model->finalise(out.mError))
            {
                if (out.mError.empty())
                {
                    out.mError = "That model could not be read.";
                }
                return out;
            }
            out.mModel = model;
        }
        catch (const std::exception& e)
        {
            out.mModel.reset();
            out.mError = std::string("That model could not be read: ") + e.what()
                       + ". The file is probably corrupt.";
        }
        catch (...)
        {
            out.mModel.reset();
            out.mError = "That model could not be read - the file is probably corrupt.";
        }
        return out;
    };

    auto finish = [handle, path](Parsed result)
    {
        if (WolfFloaterMeshUpload* self = handle.get())
        {
            self->onParsed(path, result.mModel, result.mError);
        }
    };

    if (!main_queue || !general_queue)
    {
        // No worker queues (shutting down, or a build without them): do it here rather than
        // silently doing nothing.
        finish(run());
        return;
    }
    main_queue->postTo(general_queue, run, finish);
}

void WolfFloaterMeshUpload::onParsed(const std::string& path, WolfMeshUpload::model_ptr_t model,
                                     const std::string& error)
{
    if (path != mModelPath)
    {
        return;   // a second file was chosen while this one was parsing
    }
    setBusy(false);
    if (!model)
    {
        mModel.reset();
        mInfoText->setText(error);
        setStatus("Could not read that model.", true);
        mUploadBtn->setEnabled(false);
        return;
    }
    mModel = model;
    setStatus(LLStringUtil::null, false);
    refreshInfo();
    mUploadBtn->setEnabled(true);
}

void WolfFloaterMeshUpload::refreshInfo()
{
    if (!mModel)
    {
        mInfoText->setText(LLStringUtil::null);
        return;
    }
    mInfoText->setText(mModel->summary(mIncludeTextures->getValue().asBoolean()));
}

void WolfFloaterMeshUpload::onIncludeTexturesChanged()
{
    refreshInfo();
}

// ─────────────────────────── uploading ───────────────────────────

void WolfFloaterMeshUpload::onUpload()
{
    if (mBusy || !mModel || mModel->prims().empty())
    {
        return;
    }
    // Belt and braces: the floater is only registered on Wolf Territories, but a session can
    // change grid under a floater that was left open.
    if (!WolfMeshUpload::isAvailable())
    {
        setStatus("This uploader works on Wolf Territories only. Press \"Old Upload\" to use "
                  "Firestorm's uploader.", true);
        return;
    }

    WolfMeshUpload::Options options;
    options.mName = mNameEdit->getText();
    LLStringUtil::trim(options.mName);
    if (options.mName.empty())
    {
        options.mName = gDirUtilp->getBaseFileName(mModelPath, true);
    }
    if (options.mName.empty())
    {
        options.mName = "model";
    }
    options.mDescription = mDescEdit->getText();
    LLStringUtil::trim(options.mDescription);
    options.mIncludeTextures = mIncludeTextures->getValue().asBoolean();
    options.mFolderId = mDestFolderId;

    setBusy(true);
    setStatus("Uploading...", false);

    auto handle = getDerivedHandle<WolfFloaterMeshUpload>();
    WolfMeshUpload::upload(
        mModel, options,
        [handle](const std::string& text, bool is_error)
        {
            if (WolfFloaterMeshUpload* self = handle.get())
            {
                self->setStatus(text, is_error);
            }
        },
        [handle](bool ok, const std::string& message, const LLUUID& item_id)
        {
            WolfFloaterMeshUpload* self = handle.get();
            if (!self)
            {
                // The floater went away mid-upload. The upload itself still finished, so say so
                // where it can still be read.
                LL_INFOS("WolfMeshUpload") << "upload finished after the floater closed: "
                    << (ok ? "ok " : "failed ") << message << LL_ENDL;
                return;
            }
            self->setBusy(false);
            if (ok)
            {
                self->setStatus("Uploaded. The new object is in your Objects folder"
                                + (item_id.notNull()
                                   ? std::string(" (item ") + item_id.asString().substr(0, 8) + "...)."
                                   : std::string(".")),
                                false);
            }
            else
            {
                self->setStatus(message, true);
            }
        });
}

void WolfFloaterMeshUpload::onOldUpload()
{
    // Firestorm's own uploader, unchanged. showClassicModelPreview is the original body of
    // showModelPreview, which now routes here on Wolf Territories.
    LLFloaterModelPreview::showClassicModelPreview();
    closeFloater();
}
