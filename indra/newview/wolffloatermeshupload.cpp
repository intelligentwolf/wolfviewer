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
#include "lldraghandle.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "lltextbox.h"
#include "llviewermenufile.h"
#include "workqueue.h"

#include "wolfgrid.h"
#include "wolfpaint.h"

using namespace WolfPaint;

namespace
{
    // ---- palette: the blueprint sheet ([BLUEPRINT LOOK 2026-10-20]) ----
    // Fixed colours, not the skin's: a painted sheet (as the radio and WolfAvatars), every line of text on fixed paper.
    // Contrast (WCAG 2.x relative luminance), against the lightest ground each text colour sits on:
    //   INK #f0f7ff on the paper's top #12396e 10.6:1, on a card's top #0b2347 14.5:1, on INPUT #061a36 16.1:1
    //   DIM #bcd2ec on the paper 7.4:1, on a card 10.1:1;  CYAN #8fdcff on the paper 7.5:1, on a card 10.3:1
    //   NAVY_INK #04203f on the primary button (#eaf7ff 15.0:1 .. #a3dcff 11.1:1), on the cyan stamp 10.8:1, on a done
    //     stamp #d6ecff 13.5:1;  INK on a quiet button #143d70 10.1:1;  OFF_INK #a9bdd6 on a disabled button #0d2a50 7.5:1
    //   ERR_INK #ffd0c8 on ERR_PLATE #3d1222 11.6:1;  OK #a8f5c8 on INPUT 13.7:1
    const LLColor4 INK(0.941f, 0.969f, 1.f, 1.f);
    const LLColor4 DIM(0.737f, 0.824f, 0.925f, 1.f);
    const LLColor4 CYAN(0.561f, 0.863f, 1.f, 1.f);
    const LLColor4 NAVY_INK(0.016f, 0.125f, 0.247f, 1.f);
    const LLColor4 OFF_INK(0.663f, 0.741f, 0.839f, 1.f);
    const LLColor4 ERR_INK(1.f, 0.816f, 0.784f, 1.f);
    const LLColor4 ERR_PLATE(0.239f, 0.071f, 0.133f, 1.f);
    const LLColor4 ERR_EDGE(1.f, 0.541f, 0.471f, 1.f);
    const LLColor4 OK(0.659f, 0.961f, 0.784f, 1.f);

    const LLColor4 PAPER_TOP(0.071f, 0.224f, 0.431f, 1.f);
    const LLColor4 PAPER_BOTTOM(0.039f, 0.145f, 0.278f, 1.f);
    const LLColor4 GRID(0.588f, 0.784f, 1.f, 1.f);
    const LLColor4 CARD_TOP(0.043f, 0.137f, 0.278f, 1.f);
    const LLColor4 CARD_BOTTOM(0.031f, 0.106f, 0.220f, 1.f);
    const LLColor4 EDGE(0.667f, 0.839f, 1.f, 0.42f);
    const LLColor4 INPUT(0.024f, 0.102f, 0.212f, 1.f);
    const LLColor4 BTN(0.078f, 0.239f, 0.439f, 1.f);
    const LLColor4 BTN_OFF(0.051f, 0.165f, 0.314f, 1.f);
    const LLColor4 PRIMARY_TOP(0.918f, 0.969f, 1.f, 1.f);
    const LLColor4 PRIMARY_BOTTOM(0.639f, 0.863f, 1.f, 1.f);
    const LLColor4 DONE_STAMP(0.839f, 0.925f, 1.f, 1.f);
    const LLColor4 SHADOW(0.f, 0.f, 0.f, 0.35f);
    const LLColor4 CLEAR(0.f, 0.f, 0.f, 0.f);

    constexpr S32 STAGES = 4;               // Choose, Check, Upload, Done (the floater string "stages")
    constexpr S32 CELLS = 5;                // Faces, Vertices, Triangles, Size, Rezzes as (the floater string "cells")

    /** 1234567 -> "1,234,567" (the spec sheet's numbers; wolfmeshupload.cpp withCommas does the same for its words). */
    std::string grouped(size_t n)
    {
        const std::string digits = llformat("%llu", (unsigned long long)n);
        std::string out;
        const size_t len = digits.size();
        for (size_t i = 0; i < len; ++i)
        {
            if (i && (len - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return out;
    }

    // Source: llfloaterwolfavatars.cpp:491 splitList - a comma separated floater string
    std::vector<std::string> splitList(const std::string& s)
    {
        std::vector<std::string> out;
        std::string::size_type start = 0;
        while (start <= s.size())
        {
            const std::string::size_type comma = s.find(',', start);
            const std::string::size_type end = comma == std::string::npos ? s.size() : comma;
            std::string item = s.substr(start, end - start);
            LLStringUtil::trim(item);
            out.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    /** The largest of the given fonts, biggest first, whose spaced lettering fits max_w and whose line fits max_h. */
    const LLFontGL* fitSpaced(const std::vector<const LLFontGL*>& fonts, const std::string& s, F32 spacing, F32 max_w, F32 max_h)
    {
        for (const LLFontGL* f : fonts)
        {
            if (f && spacedWidth(f, s, spacing) <= max_w && (F32)f->getLineHeight() <= max_h) return f;
        }
        return fonts.empty() ? LLFontGL::getFontSansSerifSmall() : fonts.back();
    }

    /** A card on the sheet: a dark panel with a light rule round it and a soft shadow. */
    void card(const LLRect& r)
    {
        LLRect shadow = r;
        shadow.translate(2, -4);
        fillRounded(shadow, allCorners(4.f), SHADOW, SHADOW);
        fillRounded(r, allCorners(4.f), CARD_TOP, CARD_BOTTOM);
        strokeRounded(r, allCorners(4.f), 0.f, 1.f, EDGE, withAlpha(EDGE, 0.7f));
    }

    /** A rectangle drawn in dashes (a stamp not reached yet). */
    void dashedRect(const LLRect& r, const LLColor4& c)
    {
        const F32 l = (F32)r.mLeft, t = (F32)r.mTop, rt = (F32)r.mRight, b = (F32)r.mBottom;
        dashed(l, t, rt, t, 1.f, 4.f, 3.f, c);
        dashed(l, b, rt, b, 1.f, 4.f, 3.f, c);
        dashed(l, b, l, t, 1.f, 4.f, 3.f, c);
        dashed(rt, b, rt, t, 1.f, 4.f, 3.f, c);
    }

    /** The warning mark an error carries (a word and a shape, not only a colour). */
    void warningMark(F32 x, F32 y, F32 r)
    {
        disc(x, y, r, ERR_EDGE, ERR_EDGE);
        line(x, y + r * 0.55f, x, y - r * 0.15f, llmax(1.5f, r * 0.28f), ERR_PLATE);
        disc(x, y - r * 0.50f, llmax(1.f, r * 0.15f), ERR_PLATE, ERR_PLATE);
    }
}

/**
 * The view behind every control that paints the blueprint sheet. It covers the whole floater (so its coordinates are the
 * floater's), sits in front of the drag handle and behind everything else, and is not mouse-opaque, so the window still
 * drags by its paper. Source: llfloaterwolfavatars.cpp:845 WolfAvatarsBackdrop (wolffloaterradio.cpp:499 WolfRadioBackdrop).
 */
class WolfMeshUploadSheet : public LLView
{
public:
    struct Params : public LLInitParam::Block<Params, LLView::Params>
    {
        Params() {}
    };

    WolfMeshUploadSheet(const Params& p, WolfFloaterMeshUpload* owner) : LLView(p), mOwner(owner) {}

    void draw() override
    {
        mOwner->drawSheet();
    }

private:
    WolfFloaterMeshUpload* mOwner;
};

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

    mBlurb           = getChild<LLTextBox>("blurb");
    mFormats         = getChild<LLTextBox>("formats");
    mNameLabel       = getChild<LLTextBox>("name_label");
    mDescLabel       = getChild<LLTextBox>("desc_label");
    mTexNote         = getChild<LLTextBox>("tex_note");

    // Say what textures will actually cost, the way WolfStorm's uploader does
    // (floater_mesh_upload.js:99, "L$N each" from the economy data). A checkbox that spends
    // money should name the price before it is ticked, not after.
    // [BLUEPRINT LOOK] the checkbox says "Include textures" and the line under it says what that costs (it was one long label)
    const S32 tex_cost = LLAgentBenefitsMgr::current().getTextureUploadCost();
    mTexNote->setText(tex_cost > 0
        ? llformat("Each distinct texture is uploaded as its own inventory item, L$%d each.", tex_cost)
        : std::string("Each distinct texture is uploaded as its own inventory item."));

    mUploadBtn->setEnabled(false);

    // The painted sheet goes behind every control, then the drag handle behind it, so empty paper drags the window.
    // Source: llfloaterwolfavatars.cpp:1014-1024 (wolffloaterradio.cpp:630-643).
    WolfMeshUploadSheet::Params sp;
    sp.name("wolf_mesh_upload_sheet");
    sp.rect(getLocalRect());
    sp.follows.flags(FOLLOWS_ALL);
    sp.mouse_opaque(false);
    mSheet = new WolfMeshUploadSheet(sp, this);
    addChild(mSheet);
    sendChildToBack(mSheet);
    if (getDragHandle()) sendChildToBack(getDragHandle());
    layoutWidgets();
    return true;
}

void WolfFloaterMeshUpload::reshape(S32 width, S32 height, bool called_from_parent)
{
    // Source: llfloaterwolfavatars.cpp:1031 reshape - the drawing and every control follow the window's size
    LLFloater::reshape(width, height, called_from_parent);
    layoutWidgets();
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
    mStage = STAGE_CHOOSE;
    layoutWidgets();
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
    // [BLUEPRINT LOOK] fixed colours on the drawn plate (the skin's would not hold their contrast on the sheet); an error
    // also gets the red plate and the warning mark (drawStatus), and stays until the next status replaces it
    mStatusIsError = is_error && !text.empty();
    mStatusText->setColor(is_error ? ERR_INK : mStage == STAGE_DONE ? OK : INK);
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
        setStatus("COLLADA (.dae) files are uploaded by the classic model uploader. "
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
    mStage = STAGE_CHOOSE;
    layoutWidgets();   // the spec sheet's notes move up while there are no cells

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
        mInfoText->setColor(ERR_INK);   // [BLUEPRINT LOOK] on the red plate drawCards puts under it
        setStatus("Could not read that model.", true);
        mUploadBtn->setEnabled(false);
        mStage = STAGE_CHOOSE;
        layoutWidgets();
        return;
    }
    mModel = model;
    mInfoText->setColor(INK);
    setStatus(LLStringUtil::null, false);
    refreshInfo();
    mUploadBtn->setEnabled(true);
    mStage = STAGE_CHECK;
    layoutWidgets();   // the cells take the top of the spec sheet
}

void WolfFloaterMeshUpload::refreshInfo()
{
    if (!mModel)
    {
        mInfoText->setText(LLStringUtil::null);
        return;
    }
    // [BLUEPRINT LOOK] summary()'s first line is the numbers ("N face(s), N verts, N tris - X x Y x Z m - rezzes as a
    // linkset of N prims", wolfmeshupload.cpp Model::summary, ended by "\n"); they are drawn as the spec sheet's cells
    // (drawCells, from the same model), so the box shows the lines after it: simplified or not, and the textures.
    const std::string summary = mModel->summary(mIncludeTextures->getValue().asBoolean());
    const size_t nl = summary.find('\n');
    mInfoText->setText(nl == std::string::npos ? summary : summary.substr(nl + 1));
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
                  "the classic uploader.", true);
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
    mStage = STAGE_UPLOAD;

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
            self->mStage = ok ? STAGE_DONE : STAGE_CHECK;
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
    // showModelPreview, which now routes here on Wolf Territories. The destination folder the
    // gallery handed us (onOpen) goes with it, as showModelPreview(dest_id) would have passed it.
    //
    // This floater must NOT share the "upload" registry group with "upload_model"
    // (llviewerfloaterreg.cpp): LLFloaterReg::getInstance resolves by group, and would hand back
    // this floater under the wrong type while it is still open.
    LLFloaterModelPreview::showClassicModelPreview(mDestFolderId);
    closeFloater();
}

// ─────────────────────────── the drawn sheet ([BLUEPRINT LOOK 2026-10-20]) ───────────────────────────

void WolfFloaterMeshUpload::layoutWidgets()
{
    if (!mSheet || !mTexNote || !mUploadBtn || !mStatusText || !mInfoText)
    {
        return;   // reshape() during construction, before postBuild() is done
    }
    if (isMinimized())
    {
        // Source: llfloaterwolfavatars.cpp:1987 - a minimized floater is the bare title bar: nothing to lay out
        return;
    }
    Layout& L = mLayout;
    const S32 W = getRect().getWidth();
    const S32 H = getRect().getHeight();
    mSheet->setShape(getLocalRect());
    // Source: llfloaterwolfavatars.cpp:2003 - the client area below the title bar
    L.client = LLRect(LLPANEL_BORDER_WIDTH, H - getHeaderHeight(), W - LLPANEL_BORDER_WIDTH, LLPANEL_BORDER_WIDTH);
    constexpr S32 M = 14;                   // side margin
    const S32 left = L.client.mLeft + M, right = L.client.mRight - M;

    // ---- the hero: the drawing on the left, the title block on the right ----
    constexpr S32 HERO_H = 92;
    const S32 top = L.client.mTop - 10;
    L.drawing = LLRect(left, top, left + 118, top - HERO_H);
    L.title_block = LLRect(L.drawing.mRight + 12, top, right, top - HERO_H);
    mBlurb->setShape(LLRect(L.title_block.mLeft + 12, L.title_block.mTop - 36, L.title_block.mRight - 10, L.title_block.mTop - 68));

    // ---- the stamps ----
    L.stamps = LLRect(left, L.title_block.mBottom - 10, right, L.title_block.mBottom - 40);

    // ---- bottom up: the buttons (Cancel and Old Upload bottom left, Upload bottom right), the status plate ----
    const S32 fb = L.client.mBottom + 10;
    mCancelBtn->setShape(LLRect(left, fb + 42, left + 110, fb + 2));
    mOldUploadBtn->setShape(LLRect(left + 118, fb + 42, left + 248, fb + 2));
    mUploadBtn->setShape(LLRect(right - 170, fb + 44, right, fb));
    L.status_plate = LLRect(left, fb + 44 + 10 + 56, right, fb + 44 + 10);   // three lines of words: a server's refusal can be long
    mStatusText->setShape(LLRect(L.status_plate.mLeft + 32, L.status_plate.mTop - 5, L.status_plate.mRight - 10, L.status_plate.mBottom + 7));

    // ---- the three cards: the file (fixed), the inventory (fixed, at the bottom), the spec sheet between them ----
    const S32 cards_top = L.stamps.mBottom - 10, cards_bottom = L.status_plate.mTop + 10;
    L.card[0] = LLRect(left, cards_top, right, cards_top - 104);
    L.card[2] = LLRect(left, cards_bottom + 128, right, cards_bottom);
    L.card[1] = LLRect(left, L.card[0].mBottom - 8, right, L.card[2].mTop + 8);

    const LLRect& A = L.card[0];
    mBrowseBtn->setShape(LLRect(A.mLeft + 12, A.mTop - 30, A.mLeft + 142, A.mTop - 60));
    mFileNameText->setShape(LLRect(A.mLeft + 152, A.mTop - 36, A.mRight - 12, A.mTop - 54));
    mFormats->setShape(LLRect(A.mLeft + 12, A.mTop - 66, A.mRight - 12, A.mBottom + 6));

    const LLRect& B = L.card[1];
    constexpr S32 CELL_H = 42, CELL_GAP = 6;
    if (mModel)
    {
        L.cells = LLRect(B.mLeft + 12, B.mTop - 30, B.mRight - 12, B.mTop - 30 - 2 * CELL_H - CELL_GAP);
        mInfoText->setShape(LLRect(B.mLeft + 12, L.cells.mBottom - 8, B.mRight - 12, B.mBottom + 8));
    }
    else
    {
        // no numbers yet: the box (a parse failure's reason) and the placeholder line take the top of the card
        L.cells = LLRect();
        mInfoText->setShape(LLRect(B.mLeft + 40, B.mTop - 34, B.mRight - 16, B.mBottom + 10));
    }

    const LLRect& C = L.card[2];
    const S32 mid = (C.mLeft + C.mRight) / 2;
    mNameLabel->setShape(LLRect(C.mLeft + 12, C.mTop - 30, mid - 6, C.mTop - 46));
    mNameEdit->setShape(LLRect(C.mLeft + 12, C.mTop - 48, mid - 6, C.mTop - 70));
    mDescLabel->setShape(LLRect(mid + 6, C.mTop - 30, C.mRight - 12, C.mTop - 46));
    mDescEdit->setShape(LLRect(mid + 6, C.mTop - 48, C.mRight - 12, C.mTop - 70));
    mIncludeTextures->setShape(LLRect(C.mLeft + 12, C.mTop - 80, C.mRight - 12, C.mTop - 96));
    mTexNote->setShape(LLRect(C.mLeft + 32, C.mTop - 98, C.mRight - 12, C.mTop - 114));
}

void WolfFloaterMeshUpload::drawSheet()
{
    const Layout& L = mLayout;
    if (isMinimized() || L.client.getWidth() < 120 || L.client.getHeight() < 120)
    {
        return;
    }
    const F32 t = animSeconds();
    LLGLSUIDefault ui;
    untextured();
    drawPaper();
    drawHero(t);
    drawStamps();
    drawCards();
    drawCells();
    drawStatus(t);
    drawButtons();
    untextured();
}

void WolfFloaterMeshUpload::drawPaper() const
{
    const LLRect& c = mLayout.client;
    // the cobalt sheet, a lighter wash where the lamp falls (top left)
    box((F32)c.mLeft, (F32)c.mTop, (F32)c.mRight, (F32)c.mBottom, PAPER_TOP, PAPER_BOTTOM);
    ellipse((F32)c.mLeft + (F32)c.getWidth() * 0.30f, (F32)c.mTop, (F32)c.getWidth() * 0.6f, (F32)c.getHeight() * 0.45f,
            LLColor4(0.09f, 0.27f, 0.50f, 0.55f), CLEAR);
    // the drafting grid: a fine 12 px one, a heavier 60 px one (from the sheet's top left, as the web window's)
    for (S32 x = c.mLeft; x <= c.mRight; x += 12)
    {
        const F32 a = (x - c.mLeft) % 60 == 0 ? 0.16f : 0.07f;
        box((F32)x, (F32)c.mTop, (F32)x + 1.f, (F32)c.mBottom, withAlpha(GRID, a), withAlpha(GRID, a));
    }
    for (S32 y = c.mTop; y >= c.mBottom; y -= 12)
    {
        const F32 a = (c.mTop - y) % 60 == 0 ? 0.16f : 0.07f;
        box((F32)c.mLeft, (F32)y, (F32)c.mRight, (F32)y - 1.f, withAlpha(GRID, a), withAlpha(GRID, a));
    }
    // the printed frame round the sheet
    const LLRect frame(c.mLeft + 5, c.mTop - 5, c.mRight - 5, c.mBottom + 5);
    strokeRounded(frame, allCorners(0.f), 0.f, 1.f, withAlpha(EDGE, 0.85f), withAlpha(EDGE, 0.85f));
    strokeRounded(grown(frame, -4), allCorners(0.f), 0.f, 1.f, withAlpha(EDGE, 0.45f), withAlpha(EDGE, 0.45f));
}

void WolfFloaterMeshUpload::drawHero(F32 t) const
{
    const Layout& L = mLayout;
    // ---- the drawing: a box in isometric projection, hidden edges dashed, width / depth / height dimension lines.
    // Points in a 132 x 100 drawing space (the web window's SVG viewBox), y down, scaled into L.drawing.
    const LLRect& d = L.drawing;
    const F32 u = llmin((F32)d.getWidth() / 132.f, (F32)d.getHeight() / 100.f);
    const F32 ox = (F32)d.getCenterX() - 66.f * u, oy = (F32)d.getCenterY() + 50.f * u;
    auto X = [&](F32 x) { return ox + x * u; };
    auto Y = [&](F32 y) { return oy - y * u; };
    const F32 w = llmax(1.f, 1.6f * u);
    // the top face, tinted
    quad(X(30), Y(34), X(66), Y(16), X(102), Y(34), X(66), Y(52), withAlpha(CYAN, 0.14f));
    // hidden edges (the back corner of the bottom face), dashed
    dashed(X(30), Y(68), X(66), Y(50), 1.f, 3.f * u, 3.f * u, withAlpha(CYAN, 0.75f));
    dashed(X(102), Y(68), X(66), Y(50), 1.f, 3.f * u, 3.f * u, withAlpha(CYAN, 0.75f));
    dashed(X(66), Y(50), X(66), Y(16), 1.f, 3.f * u, 3.f * u, withAlpha(CYAN, 0.75f));
    // the visible edges
    const F32 E[9][4] = { { 30, 34, 66, 16 }, { 66, 16, 102, 34 }, { 102, 34, 66, 52 }, { 66, 52, 30, 34 },
                          { 30, 34, 30, 68 }, { 30, 68, 66, 86 }, { 66, 86, 66, 52 }, { 102, 34, 102, 68 }, { 102, 68, 66, 86 } };
    for (const auto& e : E) limb(X(e[0]), Y(e[1]), X(e[2]), Y(e[3]), w, INK);
    // the dimension lines with their end ticks
    line(X(24), Y(72), X(60), Y(90), 1.f, CYAN);
    line(X(21), Y(70.5f), X(27), Y(73.5f), 1.f, CYAN);
    line(X(57), Y(88.5f), X(63), Y(91.5f), 1.f, CYAN);
    line(X(108), Y(72), X(72), Y(90), 1.f, CYAN);
    line(X(111), Y(70.5f), X(105), Y(73.5f), 1.f, CYAN);
    line(X(75), Y(88.5f), X(69), Y(91.5f), 1.f, CYAN);
    line(X(114), Y(34), X(114), Y(68), 1.f, CYAN);
    line(X(111), Y(34), X(117), Y(34), 1.f, CYAN);
    line(X(111), Y(68), X(117), Y(68), 1.f, CYAN);
    // the corners pinned
    const F32 P[4][2] = { { 66, 16 }, { 30, 34 }, { 102, 34 }, { 66, 86 } };
    for (const auto& p : P)
    {
        disc(X(p[0]), Y(p[1]), 2.2f * u, CYAN, CYAN);
        disc(X(p[0]), Y(p[1]), 1.2f * u, PAPER_BOTTOM, PAPER_BOTTOM);
    }
    // while it uploads, a scan line sweeps the box (the words below say what is happening)
    if (mStage == STAGE_UPLOAD)
    {
        const F32 k = 0.5f + 0.5f * sinf(t * 2.f);
        const F32 sy = Y(20.f + 64.f * k);
        box(X(26), sy + 3.f, X(106), sy - 3.f, withAlpha(CYAN, 0.f), withAlpha(CYAN, 0.25f));
        line(X(26), sy, X(106), sy, 1.5f, withAlpha(CYAN, 0.9f));
    }

    // ---- the title block: a double rule, the title spaced like a draughtsman's, the site line under a rule ----
    const LLRect& tb = L.title_block;
    fillRounded(tb, allCorners(0.f), withAlpha(CARD_TOP, 0.75f), withAlpha(CARD_BOTTOM, 0.75f));
    strokeRounded(tb, allCorners(0.f), 0.f, 1.5f, EDGE, EDGE);
    strokeRounded(tb, allCorners(0.f), 4.f, 5.f, withAlpha(EDGE, 0.5f), withAlpha(EDGE, 0.5f));
    const std::string title = getString("title");
    constexpr F32 SPACING = 4.f;
    const LLFontGL* tf = fitSpaced({ LLFontGL::getFontSansSerifHuge(), LLFontGL::getFontSansSerifBig(), LLFontGL::getFontSansSerifBold() },
                                   title, SPACING, (F32)tb.getWidth() - 24.f, 30.f);
    const F32 ty = (F32)tb.mTop - 19.f;
    // a faint cyan glow under the letters, then the letters (INK on the block 14.5:1)
    spacedText(tf, title, (F32)tb.mLeft + 12.f, ty - 1.f, withAlpha(CYAN, 0.35f), SPACING);
    spacedText(tf, title, (F32)tb.mLeft + 12.f, ty, INK, SPACING);
    untextured();
    const F32 rule_y = (F32)tb.mBottom + 21.f;
    line((F32)tb.mLeft + 10.f, rule_y, (F32)tb.mRight - 10.f, rule_y, 1.f, withAlpha(EDGE, 0.8f));
    text(LLFontGL::getFontSansSerifSmall(), getString("site"), (F32)tb.mLeft + 12.f, (F32)tb.mBottom + 11.f, DIM, LLFontGL::LEFT,
         (F32)tb.getWidth() - 24.f);
}

void WolfFloaterMeshUpload::drawStamps() const
{
    const Layout& L = mLayout;
    const std::vector<std::string> words = splitList(getString("stages"));
    if ((S32)words.size() < STAGES) return;
    constexpr S32 GAP = 6;
    const S32 sw = (L.stamps.getWidth() - (STAGES - 1) * GAP) / STAGES;
    const LLFontGL* f = LLFontGL::getFontSansSerifBold();
    const LLFontGL* nf = LLFontGL::getFontMonospace();
    for (S32 i = 0; i < STAGES; ++i)
    {
        const S32 l = L.stamps.mLeft + i * (sw + GAP);
        const LLRect r(l, L.stamps.mTop, i == STAGES - 1 ? L.stamps.mRight : l + sw, L.stamps.mBottom);
        const bool now = i == (S32)mStage, done = i < (S32)mStage;
        untextured();
        if (now)
        {
            halo(r, 3.f, 8, withAlpha(CYAN, 0.6f));
            fillRounded(r, allCorners(3.f), withAlpha(CYAN, 0.16f), withAlpha(CYAN, 0.08f));
            strokeRounded(r, allCorners(3.f), 0.f, 1.5f, CYAN, CYAN);
        }
        else if (done)
        {
            fillRounded(r, allCorners(3.f), withAlpha(CARD_TOP, 0.8f), withAlpha(CARD_BOTTOM, 0.8f));
            strokeRounded(r, allCorners(3.f), 0.f, 1.f, INK, INK);
        }
        else
        {
            fillRounded(r, allCorners(3.f), withAlpha(CARD_TOP, 0.5f), withAlpha(CARD_BOTTOM, 0.5f));
            dashedRect(r, withAlpha(EDGE, 1.f));
        }
        // the stamp: a little square with the number (now: cyan; done: a tick; later: an outline), then the word
        const F32 word_w = f->getWidthF32(words[i]);
        const F32 group = 20.f + 6.f + word_w;
        const bool room = group <= (F32)r.getWidth() - 8.f;
        const F32 sx = room ? (F32)r.getCenterX() - group * 0.5f : (F32)r.mLeft + 6.f;
        const F32 cy = (F32)r.getCenterY();
        const LLRect sq((S32)sx, (S32)(cy + 10.f), (S32)sx + 20, (S32)(cy - 10.f));
        if (now)
        {
            fillRounded(sq, allCorners(3.f), CYAN, CYAN);
            text(nf, llformat("%d", i + 1), (F32)sq.getCenterX(), cy, NAVY_INK);
        }
        else if (done)
        {
            fillRounded(sq, allCorners(3.f), DONE_STAMP, DONE_STAMP);
            untextured();
            checkMark((F32)sq.getCenterX(), cy, 11.f, 2.2f, NAVY_INK);
        }
        else
        {
            strokeRounded(sq, allCorners(3.f), 0.f, 1.5f, withAlpha(DIM, 0.8f), withAlpha(DIM, 0.8f));
            text(nf, llformat("%d", i + 1), (F32)sq.getCenterX(), cy, DIM);
        }
        // the word (CYAN on the lit stamp 7.5:1+, INK done, DIM later); underlined on the current one, as well as cyan
        const LLColor4 ink = now ? CYAN : done ? INK : DIM;
        const F32 wx = sx + 26.f, ww = (F32)r.mRight - 4.f - wx;
        text(f, words[i], wx, cy, ink, LLFontGL::LEFT, llmax(10.f, ww));
        if (now)
        {
            untextured();
            const F32 uw = llmin(word_w, ww);
            line(wx, cy - 8.f, wx + uw, cy - 8.f, 1.f, CYAN);
        }
    }
}

void WolfFloaterMeshUpload::drawCards() const
{
    const Layout& L = mLayout;
    const std::string heads[3] = { getString("card_file"), getString("card_spec"), getString("card_item") };
    const char* letters[3] = { "A", "B", "C" };
    const LLFontGL* hf = LLFontGL::getFontSansSerifBold();
    for (S32 k = 0; k < 3; ++k)
    {
        const LLRect& r = L.card[k];
        if (r.getHeight() < 30) continue;
        untextured();
        card(r);
        // the heading: a lettered ring, then the words spaced, in cyan
        const F32 hx = (F32)r.mLeft + 21.f, hy = (F32)r.mTop - 16.f;
        ring(hx, hy, 8.f, 9.f, CYAN);
        text(LLFontGL::getFontSansSerifSmall(), letters[k], hx, hy, CYAN);
        spacedText(hf, heads[k], hx + 16.f, hy, CYAN, 2.f);
    }
    untextured();
    // the file picker's dashed drop line round the button and the name
    {
        const LLRect& A = L.card[0];
        dashedRect(LLRect(A.mLeft + 6, A.mTop - 24, A.mRight - 6, A.mTop - 66), withAlpha(CYAN, 0.55f));
    }
    // the spec sheet with no numbers yet: the placeholder line, or the reason the file could not be read on a red plate
    if (!mModel)
    {
        const LLRect& B = L.card[1];
        if (mInfoText->getText().empty())
        {
            text(LLFontGL::getFontSansSerif(), getString("spec_empty"), (F32)B.mLeft + 14.f, (F32)B.mTop - 44.f, DIM, LLFontGL::LEFT,
                 (F32)B.getWidth() - 28.f);
        }
        else
        {
            const LLRect plate(B.mLeft + 10, B.mTop - 28, B.mRight - 10, B.mBottom + 8);
            fillRounded(plate, allCorners(3.f), ERR_PLATE, ERR_PLATE);
            strokeRounded(plate, allCorners(3.f), 0.f, 1.f, ERR_EDGE, ERR_EDGE);
            warningMark((F32)plate.mLeft + 15.f, (F32)plate.mTop - 14.f, 8.f);
        }
    }
}

void WolfFloaterMeshUpload::drawCells() const
{
    const Layout& L = mLayout;
    if (!mModel || L.cells.getHeight() <= 0) return;
    // the same numbers summary()'s first line gives (wolfmeshupload.cpp Model::summary), from the same model
    size_t faces = 0, verts = 0, tris = 0;
    for (const WolfMeshUpload::Prim& p : mModel->prims())
    {
        faces += p.mFaces.size();
        for (const WolfMeshUpload::Submesh& f : p.mFaces)
        {
            verts += f.mPositions.size();
            tris += f.mIndices.size() / 3;
        }
    }
    const LLVector3& sc = mModel->overallScale();
    const size_t prims = mModel->prims().size();
    std::vector<std::string> labels = splitList(getString("cells"));
    if ((S32)labels.size() < CELLS) return;
    if (mModel->simplifiedFromTris()) labels[2] += " - SIMPLIFIED";
    const std::string values[CELLS] = { grouped(faces), grouped(verts), grouped(tris),
                                        llformat("%.2f x %.2f x %.2f", sc.mV[VX], sc.mV[VY], sc.mV[VZ]),
                                        prims > 1 ? llformat("a linkset of %d prims", (S32)prims) : std::string("one prim") };
    // two rows of three: Faces, Vertices, Triangles; Size (two columns wide), Rezzes as
    constexpr S32 GAP = 6;
    const S32 cw = (L.cells.getWidth() - 2 * GAP) / 3, ch = (L.cells.getHeight() - GAP) / 2;
    const LLFontGL* lf = LLFontGL::getFontSansSerifSmall();
    const LLFontGL* nf = LLFontGL::getFontMonospace();
    const LLFontGL* wf = LLFontGL::getFontSansSerifBold();
    for (S32 i = 0; i < CELLS; ++i)
    {
        const S32 row = i < 3 ? 0 : 1;
        const S32 col = i < 3 ? i : (i == 3 ? 0 : 2);
        const S32 span = i == 3 ? 2 : 1;
        const S32 l = L.cells.mLeft + col * (cw + GAP);
        const S32 tp = L.cells.mTop - row * (ch + GAP);
        const LLRect r(l, tp, col + span == 3 ? L.cells.mRight : l + span * cw + (span - 1) * GAP, tp - ch);
        untextured();
        fillRounded(r, allCorners(2.f), INPUT, INPUT);
        strokeRounded(r, allCorners(2.f), 0.f, 1.f, withAlpha(EDGE, 0.85f), withAlpha(EDGE, 0.85f));
        box((F32)r.mLeft, (F32)r.mTop, (F32)r.mRight, (F32)r.mTop - 2.f, CYAN, CYAN);
        const F32 maxw = (F32)r.getWidth() - 16.f;
        text(lf, labels[i], (F32)r.mLeft + 8.f, (F32)r.mTop - 12.f, DIM, LLFontGL::LEFT, maxw);
        text(i == 4 ? wf : nf, values[i], (F32)r.mLeft + 8.f, (F32)r.mBottom + 12.f, INK, LLFontGL::LEFT, maxw);
    }
    // the notes under the cells sit on a cyan rule (the simplified line, the textures)
    if (!mInfoText->getText().empty())
    {
        untextured();
        const LLRect ir = mInfoText->getRect();
        box((F32)ir.mLeft - 8.f, (F32)ir.mTop, (F32)ir.mLeft - 6.f, (F32)llmax(ir.mBottom, ir.mTop - 40), CYAN, CYAN);
    }
}

void WolfFloaterMeshUpload::drawStatus(F32 t) const
{
    const Layout& L = mLayout;
    const bool busy = mBusy;
    if (mStatusText->getText().empty() && !busy) return;
    const LLRect& p = L.status_plate;
    untextured();
    if (mStatusIsError)
    {
        fillRounded(p, allCorners(3.f), ERR_PLATE, ERR_PLATE);
        strokeRounded(p, allCorners(3.f), 0.f, 1.f, ERR_EDGE, ERR_EDGE);
        warningMark((F32)p.mLeft + 16.f, (F32)p.mTop - 15.f, 8.f);
        return;
    }
    const bool done = mStage == STAGE_DONE;
    fillRounded(p, allCorners(3.f), INPUT, INPUT);
    strokeRounded(p, allCorners(3.f), 0.f, 1.f, done ? withAlpha(OK, 0.7f) : EDGE, done ? withAlpha(OK, 0.7f) : EDGE);
    if (done)
    {
        disc((F32)p.mLeft + 16.f, (F32)p.mTop - 15.f, 8.f, OK, OK);
        checkMark((F32)p.mLeft + 16.f, (F32)p.mTop - 15.f, 9.f, 2.f, NAVY_INK);
    }
    else
    {
        // a small turning arc while the model is read or sent
        const F32 a = fmodf(t * 4.f, F_TWO_PI);
        ring((F32)p.mLeft + 16.f, (F32)p.mTop - 15.f, 6.f, 8.f, withAlpha(EDGE, 0.6f));
        arcBand((F32)p.mLeft + 16.f, (F32)p.mTop - 15.f, 6.f, 8.f, a, a + 1.8f, withAlpha(CYAN, 0.f), CYAN);
    }
    if (busy)
    {
        // the indeterminate drafting stroke along the plate's foot (the words say what it is doing)
        const F32 l = (F32)p.mLeft + 2.f, r = (F32)p.mRight - 2.f, len = (r - l) * 0.35f;
        const F32 k = fmodf(t * 0.7f, 1.f);
        const F32 x0 = l - len + (r - l + len) * k;
        const F32 a0 = llmax(l, x0), a1 = llmin(r, x0 + len);
        if (a1 > a0) box(a0, (F32)p.mBottom + 4.f, a1, (F32)p.mBottom + 2.f, CYAN, CYAN);
    }
}

void WolfFloaterMeshUpload::drawButtons() const
{
    untextured();
    // the quiet buttons: a dark blue face with a light rule; the main one (Upload): white to cyan with a glow
    auto face = [](LLButton* b, bool primary)
    {
        if (!b || !b->isInVisibleChain()) return;
        const LLRect r = b->getRect();
        const bool enabled = b->isInEnabledChain();
        const bool pressed = b->hasMouseCapture() && mouseOver(b);
        const bool hover = enabled && !pressed && mouseOver(b);
        LLRect shadow = r;
        shadow.translate(0, -2);
        fillRounded(shadow, allCorners(4.f), LLColor4(0.01f, 0.06f, 0.13f, 1.f), LLColor4(0.01f, 0.06f, 0.13f, 1.f));
        if (!enabled)
        {
            fillRounded(r, allCorners(4.f), BTN_OFF, BTN_OFF);
            strokeRounded(r, allCorners(4.f), 0.f, 1.f, withAlpha(EDGE, 0.6f), withAlpha(EDGE, 0.6f));
            return;
        }
        if (primary)
        {
            halo(r, 4.f, 8, withAlpha(CYAN, 0.7f));
            LLColor4 top = PRIMARY_TOP, bottom = PRIMARY_BOTTOM;
            if (hover) bottom = lighten(bottom, 0.06f);
            if (pressed) { top = lighten(top, -0.06f); bottom = lighten(bottom, -0.06f); }
            fillRounded(r, allCorners(4.f), top, bottom);
            strokeRounded(r, allCorners(4.f), 0.f, 1.f, LLColor4::white, LLColor4::white);
            return;
        }
        LLColor4 c = BTN;
        if (hover) c = lighten(c, 0.04f);
        if (pressed) c = lighten(c, -0.03f);
        fillRounded(r, allCorners(4.f), lighten(c, 0.02f), c);
        strokeRounded(r, allCorners(4.f), 0.f, 1.f, hover ? CYAN : EDGE, hover ? CYAN : EDGE);
    };
    face(mBrowseBtn, false);
    face(mCancelBtn, false);
    face(mOldUploadBtn, false);
    face(mUploadBtn, true);
}
