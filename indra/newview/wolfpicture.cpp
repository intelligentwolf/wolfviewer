/**
 * @file wolfpicture.cpp
 * @brief WolfViewer: a resident's own reference picture. See wolfpicture.h.
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

#include "wolfpicture.h"

#include <cstring>
#include <png.h>

#include "llbase64.h"
#include "lldir.h"
#include "llfile.h"
#include "llgl.h"
#include "llimage.h"
#include "llimagejpeg.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewertexture.h"

namespace
{
    // Source: llimage.h LLImageRaw(U16 width, U16 height, ...) - a raw image's sides are U16.
    constexpr S32 RAW_SIDE_MAX = 65535;

    // A grey, grey+alpha, RGB or RGBA image as RGB on white (the JPEG encoder takes 1 or 3 components, llimagejpeg.cpp
    // encode; see-through parts of a PNG would otherwise come out black).
    LLPointer<LLImageRaw> rgb_on_white(LLImageRaw* src)
    {
        const S32 w = src->getWidth(), h = src->getHeight(), c = src->getComponents();
        if (w <= 0 || h <= 0 || c < 1 || c > 4) return nullptr;
        LLPointer<LLImageRaw> out = new LLImageRaw((U16)w, (U16)h, 3);
        LLImageDataSharedLock in_lock(src);
        LLImageDataLock out_lock(out);
        const U8* in = src->getData();
        U8* dst = out->getData();
        if (!in || !dst) return nullptr;
        for (S64 i = 0, n = (S64)w * h; i < n; ++i)
        {
            const U8* p = in + i * c;
            U8 r, g, b, a = 255;
            if (c <= 2) { r = g = b = p[0]; if (c == 2) a = p[1]; }
            else { r = p[0]; g = p[1]; b = p[2]; if (c == 4) a = p[3]; }
            // over white: out = colour * a + 255 * (1 - a)
            dst[i * 3]     = (U8)((r * a + 255 * (255 - a) + 127) / 255);
            dst[i * 3 + 1] = (U8)((g * a + 255 * (255 - a) + 127) / 255);
            dst[i * 3 + 2] = (U8)((b * a + 255 * (255 - a) + 127) / 255);
        }
        return out;
    }

    // A copy of raw at most side px on its longer side (LLImageRaw::scale works in place; LLImageRaw::duplicate copies only
    // when the image is shared, llimage.h, so the copying constructor LLImageRaw(const U8*, w, h, c) is used).
    LLPointer<LLImageRaw> fitted(LLImageRaw* raw, S32 side)
    {
        LLPointer<LLImageRaw> copy;
        {
            LLImageDataSharedLock lock(raw);
            if (!raw->getData()) return nullptr;
            copy = new LLImageRaw(raw->getData(), raw->getWidth(), raw->getHeight(), raw->getComponents());
        }
        if (copy.isNull() || !copy->getData()) return nullptr;
        const S32 w = copy->getWidth(), h = copy->getHeight();
        const F32 k = llmin(1.f, (F32)side / (F32)llmax(w, h));
        if (k < 1.f && !copy->scale(llmax(1, ll_round((F32)w * k)), llmax(1, ll_round((F32)h * k)))) return nullptr;
        return copy;
    }
}

bool WolfPicture::load(const std::string& path, Picture& out, std::string& problem)
{
    out = Picture();
    out.name = gDirUtilp->getBaseFileName(path, false);
    // Source: llimage.cpp getCodecFromExtension (the lower-case extension, lldir.cpp getExtension); the picker offers tga,
    // bmp, jpg, jpeg and png (llfilepicker.cpp FFLOAD_IMAGE), and every one of them is drawn again as a JPEG below.
    const EImageCodec codec = LLImageBase::getCodecFromExtension(gDirUtilp->getExtension(path));
    if (codec != IMG_CODEC_PNG && codec != IMG_CODEC_JPEG && codec != IMG_CODEC_BMP && codec != IMG_CODEC_TGA)
    {
        problem = "type";
        return false;
    }
    llstat st;
    if (LLFile::stat(path, &st) != 0 || st.st_size <= 0) { problem = "unreadable"; return false; }
    if ((S64)st.st_size > MAX_FILE_BYTES) { problem = "too_big"; return false; }
    llifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || (!in.good() && !in.eof())) { problem = "unreadable"; return false; }

    // A PNG's real size from libpng first, as wolfai.cpp png_texture does: LLPngWrapper keeps sides as U16, so a wider PNG
    // must be refused before the decode, never wrapped into a small one.
    if (codec == IMG_CODEC_PNG)
    {
        png_image header{};
        header.version = PNG_IMAGE_VERSION;
        bool ok = png_image_begin_read_from_memory(&header, bytes.data(), bytes.size()) != 0;
        const U32 pw = header.width, ph = header.height;
        png_image_free(&header);
        if (!ok) { problem = "unreadable"; return false; }
        if (pw > (U32)RAW_SIDE_MAX || ph > (U32)RAW_SIDE_MAX || (U64)pw * ph > (U64)MAX_PIXELS) { problem = "too_many_pixels"; return false; }
    }

    // Source: llimage.cpp LLImageFormatted::loadFromMemory (createFromType, allocateData, copy, updateData).
    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(codec);
    if (image.isNull()) { problem = "type"; return false; }
    {
        LLImageDataLock lock(image);
        U8* data = image->allocateData((S32)bytes.size());
        if (!data) { problem = "unreadable"; return false; }
        memcpy(data, bytes.data(), bytes.size());
    }
    if (!image->updateData()) { problem = "unreadable"; return false; }
    const S32 w = image->getWidth(), h = image->getHeight();
    if (w <= 0 || h <= 0) { problem = "unreadable"; return false; }
    if ((S64)w * h > MAX_PIXELS) { problem = "too_many_pixels"; return false; }
    if (llmin(w, h) < MIN_SIDE) { problem = "too_small"; return false; }
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!image->decode(raw, 0.f) || raw->getWidth() <= 0 || raw->getHeight() <= 0) { problem = "unreadable"; return false; }
    image = nullptr;
    bytes.clear();

    LLPointer<LLImageRaw> rgb = rgb_on_white(raw);
    raw = nullptr;
    if (rgb.isNull()) { problem = "unreadable"; return false; }
    LLPointer<LLImageRaw> send = fitted(rgb, SEND_SIDE);
    LLPointer<LLImageRaw> thumb = fitted(rgb, THUMB_SIDE);
    if (send.isNull() || thumb.isNull()) { problem = "unreadable"; return false; }
    // Source: llfloater360capture.cpp encodeAndSave: LLImageJPEG(quality)->encode(raw, 0), then getData / getDataSize.
    LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(90);
    {
        LLImageDataSharedLock lock(send);
        if (!jpeg->encode(send, 0.f)) { problem = "unreadable"; return false; }
    }
    {
        LLImageDataSharedLock lock(jpeg);
        if (!jpeg->getData() || jpeg->getDataSize() <= 0) { problem = "unreadable"; return false; }
        out.jpegBase64 = LLBase64::encode(jpeg->getData(), (size_t)jpeg->getDataSize());
    }
    out.thumb = LLViewerTextureManager::getLocalTexture(thumb.get(), false);
    if (out.jpegBase64.empty() || out.thumb.isNull()) { problem = "unreadable"; return false; }
    return true;
}

WolfPictureView::WolfPictureView(const Params& p)
    : LLView(p)
{
}

WolfPictureView::~WolfPictureView() = default;

void WolfPictureView::setTexture(const LLPointer<LLViewerTexture>& texture)
{
    mTexture = texture;
}

void WolfPictureView::draw()
{
    // Source: wolfai.cpp WolfAIPreview::draw (fitted, centred, gl_draw_scaled_image) on the read-only boxes' dark ground.
    const S32 W = getRect().getWidth(), H = getRect().getHeight();
    gl_rect_2d(0, H, W, 0, LLColor4(0.08f, 0.09f, 0.11f, 1.f), true);
    if (mTexture.notNull() && mTexture->getWidth() > 0 && mTexture->getHeight() > 0)
    {
        LLGLSUIDefault ui;
        const F32 scale = llmin(F32(W - 2) / (F32)mTexture->getWidth(), F32(H - 2) / (F32)mTexture->getHeight());
        const S32 width = ll_round((F32)mTexture->getWidth() * scale), height = ll_round((F32)mTexture->getHeight() * scale);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gl_draw_scaled_image((W - width) / 2, (H - height) / 2, width, height, mTexture, LLColor4(1.f, 1.f, 1.f, mDim ? 0.45f : 1.f));
    }
    gl_rect_2d(0, H, W, 0, LLColor4(0.36f, 0.38f, 0.43f, 1.f), false);
    LLView::draw();
}
