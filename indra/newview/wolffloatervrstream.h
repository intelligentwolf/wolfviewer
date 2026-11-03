/**
 * @file wolffloatervrstream.h
 * @brief WolfViewer: Wolf World > Display & Effects > VR in Your Headset Browser... (stage 1: the LAN pipe).
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

#ifndef WOLF_FLOATERVRSTREAM_H
#define WOLF_FLOATERVRSTREAM_H

#include "llfloater.h"
#include "llframetimer.h"

#include <vector>

class LLTextEditor;

// [VR PIPE 2026-11-03] The switch (WolfVRStreamEnabled), the address and its QR code, the first-time certificate
// step, the USB alternative and the live numbers. [VR SIMPLE 2026-11-03] The address is the short one to type
// ("192.168.1.9:4333", no code), shown large. No pairing and nothing to manage. The QR code is drawn as filled squares in draw() (qrcodegen);
// it is rebuilt only when the address changes.
class WolfFloaterVRStream : public LLFloater
{
public:
    WolfFloaterVRStream(const LLSD& key);

    bool postBuild() override;
    void draw() override;

private:
    void refresh();
    void setQr(const std::string& url);

    LLTextEditor* mStatus = nullptr;
    LLTextEditor* mAddress = nullptr;
    LLTextEditor* mUsbUrl = nullptr;
    LLTextEditor* mNumbers = nullptr;
    LLView* mQrArea = nullptr;
    LLFrameTimer mRefresh;
    std::string mQrText;
    std::vector<bool> mQr;   // modules, row-major
    int mQrSize = 0;
    U32 mShownPort = 0;
};

#endif // WOLF_FLOATERVRSTREAM_H
