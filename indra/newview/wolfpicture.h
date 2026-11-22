/**
 * @file wolfpicture.h
 * @brief WolfViewer: a resident's own reference picture for the Building Generator and Create with AI - read from a file,
 *        made smaller as a JPEG to send, and shown as a thumbnail.
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

#ifndef WOLF_PICTURE_H
#define WOLF_PICTURE_H

#include <string>

#include "llpointer.h"
#include "llview.h"

class LLViewerTexture;

/**
 * [PICTURE 2026-11-22] Paul: "In the Building Generator, allow users to upload their own picture. Also add a button to the
 * object builder." The picture a resident picks (File > Upload > Image's picker, FFLOAD_IMAGE: PNG, JPEG, BMP, TGA) is decoded
 * here, drawn on white (no transparency), made at most SEND_SIDE px a side and encoded as a JPEG, so it travels small; the
 * server checks it and draws it again whatever arrives (wolfstorm php/buildinggen_lib.php bg_pic_normalise, rust_proxy
 * build_http.rs reference_png). Never shown to anyone else, never a texture in-world.
 */
namespace WolfPicture
{
    // Source: wolfstorm php/buildinggen_lib.php BG_PIC_IN_MAX (8 MB) - the same limit for the file a resident picks.
    constexpr S64 MAX_FILE_BYTES = 8 * 1024 * 1024;
    // Source: wolfstorm js/ui/floaters/floater_building_gen.js PIC_SEND_SIDE (the WolfStorm window sends the same size).
    constexpr S32 SEND_SIDE = 1600;
    constexpr S32 THUMB_SIDE = 256;
    // Source: buildinggen_lib.php BG_PIC_SIDE_MIN (64). MAX_PIXELS is this viewer's own decode limit (6000 x 6000 x 4 bytes =
    // 144 MB once, here on the resident's machine); the yard's BG_PIC_PIXELS_MAX (20 MP) never applies, since what is sent
    // is the SEND_SIDE JPEG.
    constexpr S32 MIN_SIDE = 64;
    constexpr S64 MAX_PIXELS = 36000000;

    struct Picture
    {
        std::string name;                       // the file's name without its folder, for the window's words
        std::string jpegBase64;                 // what is sent: {"image": jpegBase64}
        LLPointer<LLViewerTexture> thumb;       // at most THUMB_SIDE px a side
    };

    /**
     * Reads and prepares the picture at path. On failure returns false and sets problem to one of "type" (not a picture
     * this viewer can read), "unreadable", "too_big" (over MAX_FILE_BYTES), "too_small", "too_many_pixels": each window has a
     * string "picture_<problem>" with [NAME] for the file's name. Runs on the main thread (one decode of one file, at the
     * resident's press).
     */
    bool load(const std::string& path, Picture& out, std::string& problem);
}

/** A thumbnail of the picture, fitted inside its rectangle on a dark ground; empty (just the ground) when there is none. */
class WolfPictureView : public LLView
{
public:
    // its own Params (never another widget's: lluictrlfactory.cpp registerWidget, memory viewer-install-must-pass-startup-check)
    struct Params : LLInitParam::Block<Params, LLView::Params> { Params() {} };
    explicit WolfPictureView(const Params& p);
    ~WolfPictureView() override;

    void setTexture(const LLPointer<LLViewerTexture>& texture);
    void setDim(bool dim) { mDim = dim; }       // dimmed while the picture is being checked
    void draw() override;

private:
    LLPointer<LLViewerTexture> mTexture;
    bool mDim = false;
};

#endif // WOLF_PICTURE_H
