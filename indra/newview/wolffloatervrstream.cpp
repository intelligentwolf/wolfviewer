/**
 * @file wolffloatervrstream.cpp
 * @brief WolfViewer: VR in Your Headset Browser floater. See wolffloatervrstream.h.
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

#include "wolffloatervrstream.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llrender2dutils.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"
#include "qrcodegen/qrcodegen.hpp"
#include "wolfvrstream.h"

namespace
{
    std::string fmtMs(double v, const char* unit = " ms")
    {
        if (v < 0) return "-";
        return llformat("%.1f%s", v, unit);
    }
}

WolfFloaterVRStream::WolfFloaterVRStream(const LLSD& key) : LLFloater(key)
{
}

bool WolfFloaterVRStream::postBuild()
{
    WolfVRStream::init();
    mStatus = getChild<LLTextEditor>("status");
    mAddress = getChild<LLTextEditor>("address");
    mUsbUrl = getChild<LLTextEditor>("usb_url");
    mNumbers = getChild<LLTextEditor>("numbers");
    // [VR 3D] VR 3D Mode is offered only once it is finished (WolfVR3DMode)
    if (!gSavedSettings.getBOOL("WolfVR3DMode")) getChild<LLComboBox>("mode")->remove("VR 3D Mode");
    mQrArea = getChild<LLView>("qr_area");
    getChild<LLButton>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    refresh();
    return true;
}

void WolfFloaterVRStream::setQr(const std::string& url)
{
    if (url == mQrText) return;
    mQrText = url;
    mQr.clear();
    mQrSize = 0;
    if (url.empty()) return;
    try
    {
        const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        mQrSize = qr.getSize();
        mQr.resize((size_t)mQrSize * mQrSize);
        for (int y = 0; y < mQrSize; ++y)
            for (int x = 0; x < mQrSize; ++x)
                mQr[(size_t)y * mQrSize + x] = qr.getModule(x, y);
    }
    catch (const std::exception& e)
    {
        LL_WARNS("WolfVR") << "QR code: " << e.what() << LL_ENDL;
        mQrSize = 0;
    }
}

void WolfFloaterVRStream::refresh()
{
    const bool on = WolfVRStream::running();
    const wolfvr::Stats st = WolfVRStream::stats();
    std::string status;
    if (!on)
    {
        const std::string err = WolfVRStream::lastError();
        if (gSavedSettings.getBOOL("WolfVRStreamEnabled") && !err.empty())
        {
            LLStringUtil::format_map_t args;
            args["[ERROR]"] = err;
            status = getString("status_failed", args);
        }
        else
        {
            status = getString("status_off");
        }
    }
    else if (st.client_connected)
    {
        status = getString(gSavedSettings.getString("WolfVRStreamMode") == "world" ? "status_connected_world" : "status_connected");
    }
    else
    {
        status = getString("status_waiting");
    }
    if (on && !st.last_error.empty()) status += "\n" + st.last_error;
    if (mStatus->getText() != status) mStatus->setText(status);

    const std::string lan = WolfVRStream::lanUrl(), usb = WolfVRStream::usbUrl();
    const std::string addr = on ? WolfVRStream::shortAddress() : getString("address_off");
    if (mAddress->getText() != addr) mAddress->setText(addr);
    if (mUsbUrl->getText() != usb) mUsbUrl->setText(usb);
    setQr(lan);
    const U32 port = WolfVRStream::port() ? WolfVRStream::port() : gSavedSettings.getU32("WolfVRStreamPort");
    if (port != mShownPort)
    {
        mShownPort = port;
        getChild<LLUICtrl>("usb_note")->setTextArg("[PORT]", llformat("%u", port));
    }

    std::string n;
    if (on)
    {
        n += llformat("Picture   %s (%s)  sent %s fps  %s Mbit/s  skipped %llu\n", st.encoder.empty() ? "-" : st.encoder.c_str(),
                      st.codec.empty() ? "-" : st.codec.c_str(), fmtMs(st.send_fps, "").c_str(),
                      st.kbps < 0 ? "-" : llformat("%.1f", st.kbps / 1000.0).c_str(), (unsigned long long)st.frames_skipped);
        n += llformat("PC        render %s  encode %s (p95 %s)\n", fmtMs(st.render_ms).c_str(), fmtMs(st.encode_ms).c_str(),
                      fmtMs(st.encode_ms_p95).c_str());
        n += llformat("Network   pose to PC %s  round trip %s  poses %s/s\n", fmtMs(st.pose_latency_ms).c_str(),
                      fmtMs(st.rtt_ms).c_str(), fmtMs(st.pose_hz, "").c_str());
        n += llformat("Headset   %s fps  new pictures %s fps  decode %s\n", fmtMs(st.hs_display_fps, "").c_str(),
                      fmtMs(st.hs_new_frame_fps, "").c_str(), fmtMs(st.hs_decode_ms).c_str());
        n += llformat("Delay     head move to picture %s (p95 %s)  PC to headset %s", fmtMs(st.hs_pose_to_photon_ms).c_str(),
                      fmtMs(st.hs_pose_to_photon_p95).c_str(), fmtMs(st.hs_viewer_to_photon_ms).c_str());
    }
    if (mNumbers->getText() != n) mNumbers->setText(n);
}

void WolfFloaterVRStream::draw()
{
    if (mRefresh.getElapsedTimeF32() > 0.5f)
    {
        mRefresh.reset();
        refresh();
    }
    LLFloater::draw();

    if (mQrSize <= 0 || !mQrArea) return;
    // Dark modules on white with a 4-module quiet zone (ISO/IEC 18004), whole pixels per module so it stays sharp.
    const LLRect r = mQrArea->getRect();
    const int total = mQrSize + 8;
    const int px = llmax(1, llmin(r.getWidth(), r.getHeight()) / total);
    const int side = px * total;
    const int left = r.mLeft + (r.getWidth() - side) / 2;
    const int top = r.mTop - (r.getHeight() - side) / 2;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(left, top, left + side, top - side, LLColor4::white, true);
    for (int y = 0; y < mQrSize; ++y)
    {
        for (int x = 0; x < mQrSize; ++x)
        {
            if (!mQr[(size_t)y * mQrSize + x]) continue;
            const int l = left + (x + 4) * px, t = top - (y + 4) * px;
            gl_rect_2d(l, t, l + px, t - px, LLColor4::black, true);
        }
    }
}
